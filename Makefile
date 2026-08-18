ARCHS ?= armv7 armv7s arm64 arm64e
TARGET ?= iphone:clang:latest:9.0

# Release lanes override inherited make/environment values so a preceding
# rootless build cannot silently turn a rootful framework into an iOS 15 one.
ifeq ($(HOOKKIT_LANE),rootful-legacy)
override ARCHS := armv7 armv7s arm64 arm64e
override TARGET := iphone:clang:13.7
override TARGET_OS_DEPLOYMENT_VERSION := 9.0
override TARGET_OS_DEPLOYMENT_VERSION_arm64e := 12.0
override THEOS_PACKAGE_SCHEME :=
else ifeq ($(HOOKKIT_LANE),rootful-modern)
override ARCHS := arm64 arm64e
override TARGET := iphone:clang:latest:14.0
override THEOS_PACKAGE_SCHEME :=
else ifeq ($(HOOKKIT_LANE),rootless)
override ARCHS := arm64 arm64e
override TARGET := iphone:clang:latest:15.0
override THEOS_PACKAGE_SCHEME := rootless
else ifneq ($(HOOKKIT_LANE),)
$(error unknown HOOKKIT_LANE '$(HOOKKIT_LANE)')
endif

include $(THEOS)/makefiles/common.mk

FRAMEWORK_NAME = HookKit

HookKit_FILES = HKSubstitutor.m HKBackendRegistry.m Backends/HKBackendCommon.m Backends/HKElleKitBackend.m Backends/HKMSBackends.m Backends/HKFishhookBackend.m Backends/HKLitehookBackend.m Backends/HKInlineBackends.m Backends/HKNativeBackends.m vendor/fishhook/fishhook.c vendor/litehook/litehook.c Internal/HKOriginalPublication.m Internal/HKSubstituteErrors.c Internal/HKInlinePreflight.m Internal/HKInlineGuard.c
# Native backend: arm64/arm64e only, stubbed out by #if on armv7.
HookKit_FILES += native/hk_native.c native/hk_arm64.c native/hk_symbols.c
# Swift vtable backend: arm64/arm64e only (entry points report unsupported on
# armv7 via hk_swift_supported()).
HookKit_FILES += native/hk_swift.c
HookKit_FRAMEWORKS = Foundation
HookKit_INSTALL_PATH = /Library/Frameworks
HookKit_PUBLIC_HEADERS = Headers/HookKit.h
HookKit_CFLAGS = -fobjc-arc -I. -IHeaders -Ivendor -Ivendor/litehook
HookKit_LDFLAGS =
# Jailbreak-root seam is compile-time per scheme (see Backends/HKBackendCommon.m):
# rootful = identity, rootless = libroot (auto-linked -lroot by theos),
# roothide = libroothide's jbroot(). Must append after the base CFLAGS above.
# THEOS_PACKAGE_SCHEME_ROOTHIDE makes <roothide.h> select the real libroothide
# API; without it the header falls back to roothide/stub.h whose jbroot()
# resolves through libroot at runtime — wrong semantics for roothide.
ifeq ($(THEOS_PACKAGE_SCHEME),rootless)
HookKit_CFLAGS += -DHK_ROOTLESS
else ifeq ($(THEOS_PACKAGE_SCHEME),roothide)
HookKit_CFLAGS += -DHK_ROOTHIDE -DTHEOS_PACKAGE_SCHEME_ROOTHIDE
endif
# The roothide scheme module forces -install_name "@loader_path/.jbroot...",
# which would override our @rpath install_name (instance LDFLAGS come after
# internal LDFLAGS); under the roothide scheme drop our explicit install_name
# so the module's .jbroot one wins.
ifneq ($(THEOS_PACKAGE_SCHEME),roothide)
HookKit_LDFLAGS += -install_name @rpath/HookKit.framework/HookKit
else
HookKit_LDFLAGS += -lroothide
endif
HookKit_LDFLAGS += -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -rpath /usr/lib -rpath /var/jb/usr/lib
# Mach-O dylib versions: must match HookKit.tbd (current/compatibility 2.5.0)
# so consumers linking via the tbd record a satisfiable requirement. Theos
# sets no versions itself, so they come from here.
HookKit_LDFLAGS += -current_version 2.5.0 -compatibility_version 2.5.0
# Export boundary: only the public HKSubstitutor ObjC class symbols survive
# the link (see scripts/export-HookKit.list); every backend/litehook/dobby/
# fishhook/native symbol becomes local.
HookKit_LDFLAGS += -exported_symbols_list $(CURDIR)/scripts/export-HookKit.list

include $(THEOS_MAKE_PATH)/framework.mk

# Dobby's current archive hard-imports post-iOS-9 private symbols. Keep it out
# of the old-ABI/iOS-9 lane; the existing stub leaves HK_LIB_DOBBY
# ABI-compatible but unavailable there.
ifeq ($(HOOKKIT_LANE),rootful-legacy)
HookKit_CFLAGS += -DHK_NO_DOBBY
# Dobby supplies libc++ in modern lanes; legacy still needs it for the
# compiler-generated guards around Objective-C function-local statics.
HookKit_LDFLAGS += -lc++
# The pre-Clang-12 Linux bundles do not ship Apple's iOS compiler-rt. Avoid
# accidentally taking the current toolchain's iOS-14 runtime and link the
# platform runtime explicitly. Xcode 11.7 has the matching compiler-rt.
ifeq ($(THEOS_PLATFORM_NAME),linux)
HookKit_LDFLAGS += -nodefaultlibs -lSystem
endif
else ifeq ($(filter arm64,$(ARCHS)),arm64)
HookKit_LDFLAGS += -Lvendor/dobby -ldobby -lc++
endif

# HKGum: thin wrapper dylib statically linking the frida-gum devkit. The
# framework never links gum — the Frida backend dlopens HKGum.dylib at
# runtime via RootBridge (see Backends/HKInlineBackends.m), keeping LGPL code
# out of the framework binary. The devkit ships no armv7 slice, so this
# product is pinned to arm64/arm64e per-product rather than gated on the
# global ARCHS: that lets the framework span all four slices in one pass while
# gum stays 64-bit. Rootless packaging maps /usr/lib -> /var/jb/usr/lib
# automatically.
ifneq ($(HOOKKIT_LANE),rootful-legacy)
LIBRARY_NAME = HKGum
HKGum_FILES = vendor/gum/hkgum.c
HKGum_ARCHS = arm64 arm64e
# Export boundary: only the 3 hkgum_* wrappers (scripts/export-HKGum.list);
# the ~6k frida-gum symbols become local.
HKGum_LDFLAGS = -Lvendor/gum -lfrida-gum -exported_symbols_list $(CURDIR)/scripts/export-HKGum.list
HKGum_INSTALL_PATH = /usr/lib
include $(THEOS_MAKE_PATH)/library.mk
endif

# Release export check: verifies every built binary exports exactly its
# allowlist (scripts/export-*.list), per arch slice. Discovers the freshly
# built products under .theos (fat + per-arch thin copies + staged copies),
# so it works after both `make` and `make package`. Fails with a clear
# diff-style message on any discrepancy.
.PHONY: check-exports
check-exports:
	$(ECHO_NOTHING)bash scripts/check_exports.sh$(ECHO_END)

# Release compatibility check. build.sh supplies the package profile.
.PHONY: check-compat
check-compat:
	$(ECHO_NOTHING)bash scripts/check_compat.sh $(COMPAT_PROFILE) $(COMPAT_ARTIFACT)$(ECHO_END)

# Host-side test aggregate: builds and runs each suite in sequence, stopping
# at the first failure (no -k).
.PHONY: test
test:
	$(ECHO_NOTHING)$(MAKE) test-reloc test-swift-abi test-substitute-classifier test-inline-guard test-original-publication test-header-compile test-runtime-lifecycle test-plan-lifecycle test-hook-add test-plan-analyze test-engine-registry test-plan-prepare test-plan-commit test-domain-gate test-artifact-ledger test-installed-original test-plan-model$(ECHO_END)

# Host-side relocator test. Runs on the build machine, not the device: it only
# exercises instruction decode/re-encode, which is where the crashes come from.
.PHONY: test-reloc
test-reloc:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -O2 -o $(THEOS_OBJ_DIR)/test_arm64_reloc tests/test_arm64_reloc.c native/hk_arm64.c && $(THEOS_OBJ_DIR)/test_arm64_reloc$(ECHO_END)

# Host-side Swift vtable engine test. The test includes native/hk_swift.c
# itself so it can inject a simulated pointer-authentication scheme (the host
# has no PAC hardware) and a fake hk_native_patch_memory, then drives the
# engine's core against a hand-built fake class metadata blob. -rdynamic puts
# the test's fake method symbols into .dynsym so dladdr resolves them, which
# is what lets the name-matching paths run on the host.
.PHONY: test-swift-abi
test-swift-abi:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -O2 -rdynamic -o $(THEOS_OBJ_DIR)/test_swift_abi tests/test_swift_abi.c && $(THEOS_OBJ_DIR)/test_swift_abi$(ECHO_END)

# Host-side substitute error classifier test. Pure code table, runs on the
# build machine. Compiles as ObjC so the test can include the REAL
# Headers/HookKit.h and the real vendored substitute.h (through a fake
# __APPLE__ plus minimal Mach-O/ObjC/Foundation header stubs, so the vendored
# header's Apple-only sections compile on Linux). The classifier under test is
# the REAL Internal/HKSubstituteErrors.c — compiled alongside the test, no
# mirror copy. The -I$(CURDIR)/Internal flag is needed for the helper's
# include of "HKSubstituteErrors.h" from the test's compilation directory.
.PHONY: test-substitute-classifier
test-substitute-classifier:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -O2 -x objective-c -Ivendor -IHeaders -I$(CURDIR)/tests/fake_headers -I$(CURDIR)/Internal -D__APPLE__ -o $(THEOS_OBJ_DIR)/test_substitute_classifier tests/test_substitute_classifier.c Internal/HKSubstituteErrors.c && $(THEOS_OBJ_DIR)/test_substitute_classifier$(ECHO_END)

# Host-side inline-guard test. Pure C, runs on the build machine.
.PHONY: test-inline-guard
test-inline-guard:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -O2 -o $(THEOS_OBJ_DIR)/test_inline_guard tests/test_inline_guard.c Internal/HKInlineGuard.c && $(THEOS_OBJ_DIR)/test_inline_guard$(ECHO_END)

# Host-side original-publication contract test. Uses the real implementation
# with the same minimal Foundation stubs as the substitute classifier.
.PHONY: test-original-publication
test-original-publication:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -O2 -x objective-c -IHeaders -I$(CURDIR)/tests/fake_headers -I$(CURDIR)/Internal -o $(THEOS_OBJ_DIR)/test_original_publication tests/test_original_publication.m Internal/HKOriginalPublication.m && $(THEOS_OBJ_DIR)/test_original_publication$(ECHO_END)

# HookKit 3.0 new-ABI header compile tests (spec section 21, Milestone 3):
# the same Headers/HookKit/*.h compiled and run under all 4 language modes.
# ObjC/ObjC++ reuse the existing fake Foundation/objc-runtime stubs
# (tests/fake_headers) rather than a new copy -- these new headers are
# Foundation-free by design, so the stub is only there to give the ObjC
# compiler front end a minimal NSObject/Class/SEL vocabulary to check
# against, same reason test-substitute-classifier needs it.
.PHONY: test-header-compile
test-header-compile:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && \
	clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_header_compile_c Tests/Host/test_header_compile.c && $(THEOS_OBJ_DIR)/test_header_compile_c && \
	clang -Wall -Wextra -Werror -std=c11 -O2 -x objective-c -D__APPLE__ -I$(CURDIR)/tests/fake_headers -o $(THEOS_OBJ_DIR)/test_header_compile_m Tests/Host/test_header_compile.m && $(THEOS_OBJ_DIR)/test_header_compile_m && \
	clang++ -Wall -Wextra -Werror -std=c++17 -O2 -o $(THEOS_OBJ_DIR)/test_header_compile_cpp Tests/Host/test_header_compile.cpp && $(THEOS_OBJ_DIR)/test_header_compile_cpp && \
	clang++ -Wall -Wextra -Werror -std=c++17 -O2 -x objective-c++ -D__APPLE__ -I$(CURDIR)/tests/fake_headers -o $(THEOS_OBJ_DIR)/test_header_compile_mm Tests/Host/test_header_compile.mm && $(THEOS_OBJ_DIR)/test_header_compile_mm && \
	echo "test-header-compile: C, ObjC, C++, ObjC++ all compiled and passed"$(ECHO_END)

# HookKit 3.0 core runtime lifecycle test (Milestone 4, first slice):
# real Sources/Core/HKRuntime.c + HKIDs.c, linked and run, not just
# compiled -- includes the internal headers directly (same pattern as
# test-swift-abi) to verify config was actually deep-copied, not just that
# calls returned OK. -lpthread for pthread_once (the process-nonce guard).
.PHONY: test-runtime-lifecycle
test-runtime-lifecycle:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_runtime_lifecycle Tests/Host/test_runtime_lifecycle.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c -lpthread && $(THEOS_OBJ_DIR)/test_runtime_lifecycle$(ECHO_END)

# HookKit 3.0 plan lifecycle + domain registration test (Milestone 4):
# real Sources/Core/HKPlan.c. The critical property this exercises is
# hk_domain_t* pointer stability across the internal array's realloc
# growth (37 domains, several times past the initial capacity of 4) --
# see HKPlanInternal.h for why domains are individually heap-allocated
# rather than stored inline in that array.
.PHONY: test-plan-lifecycle
test-plan-lifecycle:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_lifecycle Tests/Host/test_plan_lifecycle.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_plan_lifecycle$(ECHO_END)

# HookKit 3.0 hook registration test (Milestone 4): real hk_plan_add_hook,
# the deep copy of the full target union (symbol/address/objc/memory).
# Covers per-kind deep-copy verification, the foreign-domain and
# forward-commit_after rejections, and hk_hook_t* pointer stability across
# growth -- see Sources/Core/HKPlan.c's file header for what's deliberately
# not supported yet (Swift targets, HK_IMAGE_EXPLICIT_SET).
.PHONY: test-hook-add
test-hook-add:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_hook_add Tests/Host/test_hook_add.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_hook_add$(ECHO_END)

# HookKit 3.0 plan analysis test (Milestone 4): real hk_plan_analyze +
# hk_report_t (Sources/Core/HKReport.c). No engine registry exists yet, so
# every hook honestly gets HK_OUTCOME_NO_ROUTE -- this test is about the
# plumbing (state transitions, report/hook independence, result content)
# being correct given that starting point, not routing logic that doesn't
# exist yet.
.PHONY: test-plan-analyze
test-plan-analyze:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_analyze Tests/Host/test_plan_analyze.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_plan_analyze$(ECHO_END)

# HookKit 3.0 engine registry test (Milestone 4's "fake engines"): proves
# hk_plan_analyze actually consults registered engines now (see
# Sources/Core/HKEngineInternal.h for the minimal internal contract this
# is built on) -- an eligible engine upgrades a hook from NO_ROUTE to
# ANALYZED, an engine matching the target kind but not the required reach
# correctly stays NO_ROUTE, and first-eligible-wins correctly skips past
# an ineligible engine registered earlier.
.PHONY: test-engine-registry
test-engine-registry:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_engine_registry Tests/Host/test_engine_registry.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_engine_registry$(ECHO_END)

# HookKit 3.0 plan preparation test (Milestone 4): real hk_plan_prepare.
# Covers the per-hook outcome transitions (ANALYZED -> PREPARED/
# FAILED_SAFE, NO_ROUTE left untouched), the plan-level PREPARED/PARTIAL/
# FAILED rollup, and that prepare calls the same engine analyze matched
# (hook->matched_engine) rather than re-searching the registry. Shares
# Tests/Host/fake_engines.h with test-engine-registry.
.PHONY: test-plan-prepare
test-plan-prepare:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_prepare Tests/Host/test_plan_prepare.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_plan_prepare$(ECHO_END)

# HookKit 3.0 plan commit test (Milestone 4): real hk_plan_commit. The
# property under test is the mutation-state -> outcome mapping (spec
# section 4.4/6.27) -- one of the spec's core invariants -- exercised via
# 4 distinct fake engines in Tests/Host/fake_engines.h (COMPLETE/NONE/
# PARTIAL/UNKNOWN, plus a commit_one == NULL case) rather than trusted
# from reading the switch statement.
.PHONY: test-plan-commit
test-plan-commit:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_commit Tests/Host/test_plan_commit.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_plan_commit$(ECHO_END)

# HookKit 3.0 domain preparation gate test (spec section 15.1): a domain
# with require_all_mandatory_prepared set, containing a mandatory hook
# with no route, must block EVERY hook in that domain during
# hk_plan_prepare -- even ones that individually would have prepared
# successfully. Caught a real bug in the plan-level PREPARED/PARTIAL/
# FAILED rollup while writing this test (a gate-blocked hook counted as
# failed but not attempted, breaking the failed<=attempted assumption the
# rollup depends on) -- see the fix's comment in HKPlan.c.
.PHONY: test-domain-gate
test-domain-gate:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_domain_gate Tests/Host/test_domain_gate.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_domain_gate$(ECHO_END)

# HookKit 3.0 artifact ledger test (spec section 7 / Milestone 4). Exercises
# the append + immutable-snapshot read path directly (no engine populates
# the ledger yet -- that is the next commit), including snapshot/ledger
# independence and geometric growth. Links only the ledger + report + IDs,
# not the whole plan/runtime, since that is all this path touches.
.PHONY: test-artifact-ledger
test-artifact-ledger:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_artifact_ledger Tests/Host/test_artifact_ledger.c Sources/Core/HKIDs.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c -lpthread && $(THEOS_OBJ_DIR)/test_artifact_ledger$(ECHO_END)

# HookKit 3.0 original-slot / installed-handle test (spec section on
# original slots, Milestone 4). The property that matters: an original slot
# outlives the plan/runtime/hook that created it (process-lifetime installed
# registry), so a live replacement can still load through it. Links the full
# core set since it drives a real commit end to end.
.PHONY: test-installed-original
test-installed-original:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_installed_original Tests/Host/test_installed_original.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_installed_original$(ECHO_END)

# HookKit 3.0 model-based test of the plan lifecycle state machine (Milestone
# 4). An independent reference model predicts accept/reject + resulting state
# for every (state, op); random operation sequences cross-check it against a
# real plan, and a coverage assertion proves the whole (state x op) table was
# exercised. Success path only -- FAILED/PARTIAL rollups live in
# test-plan-prepare / test-plan-commit.
.PHONY: test-plan-model
test-plan-model:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_model Tests/Host/test_plan_model.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c -lpthread && $(THEOS_OBJ_DIR)/test_plan_model$(ECHO_END)

# Device smoke binary. NOT part of `make test`: it links the built framework
# and has to run on a jailbroken device, where the trampoline-page check is
# the only thing that can observe the allocator's real behaviour. Build the
# framework first, then:
#
#   make device-smoke
#   ssh device 'rm -f /var/mobile/device_smoke'
#   scp $(THEOS_OBJ_DIR)/device_smoke device:/var/mobile/
#
# The rm is NOT optional. Overwriting a Mach-O in place that has already been
# executed leaves the kernel's cached code signature stale for that vnode, and
# the next exec is SIGKILLed by AMFI with no output and no crash report — it
# looks exactly like the binary itself being rejected. Delete, then copy.
# Point SDK/MIN at the lane you built, or expect a deployment-version warning
# from the linker (harmless — the smoke binary uses no versioned API).
#
# -O0 -fno-inline is load-bearing, not laziness: at -O2 the compiler inlines or
# devirtualizes the calls to the hook targets, so the call never reaches the
# patched entry and the test measures nothing. The entitlements grant
# dynamic-codesigning, which is what lets the native engine obtain W^X outside
# an injected process.
DEVICE_SMOKE_SDK ?= $(THEOS)/sdks/iPhoneOS13.7.sdk
DEVICE_SMOKE_ARCH ?= arm64
DEVICE_SMOKE_MIN ?= 13.0
DEVICE_SMOKE_LDID ?= ldid

.PHONY: device-smoke
device-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(SDKBINPATH)/clang -Wall -Wextra -O0 -fno-inline -fobjc-arc -target $(DEVICE_SMOKE_ARCH)-apple-ios$(DEVICE_SMOKE_MIN) -isysroot $(DEVICE_SMOKE_SDK) -F$(THEOS_OBJ_DIR) -framework Foundation -framework HookKit -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_smoke tests/device_smoke.m && $(DEVICE_SMOKE_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_smoke$(ECHO_END)
