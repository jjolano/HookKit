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
else ifeq ($(HOOKKIT_LANE),roothide)
override ARCHS := arm64 arm64e
override TARGET := iphone:clang:latest:15.0
override THEOS_PACKAGE_SCHEME := roothide
else ifneq ($(HOOKKIT_LANE),)
$(error unknown HOOKKIT_LANE '$(HOOKKIT_LANE)')
endif

include $(THEOS)/makefiles/common.mk

FRAMEWORK_NAME = HookKit

# HookKit is the canonical 3.0 framework in every package lane.
HookKit_FILES = \
	Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c Sources/Core/HKOwnership.c \
	Sources/Core/HKImageCatalog.c Sources/Core/HKImageScope.c \
	Sources/Core/HKInstalled.c Sources/Core/HKPlan.c Sources/Core/HKReport.c \
	Sources/Core/HKRuntime.c Sources/Core/HKRuntimeSymbol.c \
	Sources/Engines/HKInlineEngine.c Sources/Engines/HKInlineVtable.c \
	Sources/Engines/HKMemoryEngine.c Sources/Engines/HKMemoryVtable.c \
	Sources/Engines/HKObjCEngine.c Sources/Engines/HKObjCVtable.c \
	Sources/Engines/HKProviderVtable.c \
	Sources/Engines/HKRebindEngine.c Sources/Engines/HKRebindVtable.c \
	Sources/Engines/HKRelocInlineEngine.c Sources/Engines/HKRelocInlineVtable.c \
	Sources/Engines/HKSwiftEngine.c Sources/Engines/HKStaticPool.c \
	Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKExportTrie.c \
	Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKMachO.c \
	Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c \
	Internal/HKInlinePreflight.m \
	native/hk_native.c native/hk_arm64.c native/hk_symbols.c native/hk_swift.c
HookKit_FRAMEWORKS = Foundation
HookKit_INSTALL_PATH = /Library/Frameworks
HookKit_PUBLIC_HEADERS = Headers/HookKit.h
HookKit_CFLAGS = -fobjc-arc -I. -IHeaders -Ivendor -Ivendor/litehook
HookKit_LDFLAGS =
# Jailbreak-root seam is compile-time per scheme:
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
HookKit_CFLAGS += -DHOOKKIT_CANONICAL_3=1
HookKit_LDFLAGS += -lobjc -current_version 3.0.0 -compatibility_version 3.0.0
HookKit_LDFLAGS += -exported_symbols_list $(CURDIR)/scripts/export-HookKit.list

include $(THEOS_MAKE_PATH)/framework.mk

# Theos flattens a directory supplied through *_PUBLIC_HEADERS. Keep the
# canonical headers beneath the framework's HookKit/ namespace too.
after-HookKit-all::
	$(ECHO_NOTHING)rm -rf "$(THEOS_OBJ_DIR)/HookKit.framework/Headers/HookKit"$(ECHO_END)
	$(ECHO_NOTHING)mkdir -p "$(THEOS_OBJ_DIR)/HookKit.framework/Headers/HookKit"$(ECHO_END)
	$(ECHO_NOTHING)rsync -a Headers/HookKit/ "$(THEOS_OBJ_DIR)/HookKit.framework/Headers/HookKit/"$(ECHO_END)

# Dobby's current archive hard-imports post-iOS-9 private symbols. Keep it out
# of the iOS-9 lane; its provider engine is unavailable there.
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
# runtime through the provider adapter, keeping LGPL code
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

# The 76 MB Gum devkit is intentionally not tracked. Fetch its pinned,
# checksummed release assets only for a lane that builds HKGum.
before-HKGum-all::
	$(ECHO_NOTHING)bash scripts/fetch-gum.sh$(ECHO_END)
endif

# Release export check: verifies every built binary exports exactly its
# allowlist (scripts/export-*.list), per arch slice. Discovers the freshly
# built products under .theos (fat + per-arch thin copies + staged copies),
# so it works after both `make` and `make package`. Fails with a clear
# diff-style message on any discrepancy.
.PHONY: check-exports
check-exports:
	$(ECHO_NOTHING)bash scripts/check_exports.sh$(ECHO_END)

# Release package check. build.sh supplies the package profile.
.PHONY: check-compat
check-compat:
	$(ECHO_NOTHING)bash scripts/check_compat.sh $(COMPAT_PROFILE) $(COMPAT_ARTIFACT) $(COMPAT_GUM_ARTIFACT)$(ECHO_END)

# HookKit 3.0 rebind engine (Milestone 6). The first engine: rewrites import
# slots (both LC_DYSYMTAB and chained-fixup mechanisms) to redirect an
# imported symbol. Two-phase (prepare mutates nothing, commit revalidates and
# writes); the write is behind a seam a host test drives into a buffer.
.PHONY: test-rebind-engine
test-rebind-engine:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_rebind_engine Tests/Host/test_rebind_engine.c Sources/Engines/HKRebindEngine.c Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c native/hk_symbols.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c -lpthread && $(THEOS_OBJ_DIR)/test_rebind_engine$(ECHO_END)

.PHONY: test-rebind-pac
test-rebind-pac:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -DHK_PTRAUTH_TEST=1 -o $(THEOS_OBJ_DIR)/test_rebind_pac Tests/Host/test_rebind_pac.c Sources/Engines/HKRebindEngine.c Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c native/hk_symbols.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c && $(THEOS_OBJ_DIR)/test_rebind_pac$(ECHO_END)

# HookKit 3.0 end-to-end: the plan lifecycle driving the REAL memory-patch
# engine through its runtime adapter (Milestone 6). Real analyze/prepare/commit,
# buffer-backed writes, and real artifacts. Covers absolute and image-relative
# targets; no fake engine.
.PHONY: test-memory-wired
test-memory-wired:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_memory_wired Tests/Host/test_memory_wired.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_memory_wired$(ECHO_END)

.PHONY: test-memory-engine
test-memory-engine:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_memory_engine Tests/Host/test_memory_engine.c Sources/Engines/HKMemoryEngine.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c -lpthread && $(THEOS_OBJ_DIR)/test_memory_engine$(ECHO_END)

# HookKit 3.0 Objective-C method engine (Milestone 6). There is no ObjC
# runtime on this host, so the whole runtime is a function-pointer seam and
# the suite drives it with an in-memory class table -- which is what makes
# the metaclass hop, the local-vs-inherited test, class_replaceMethod's
# authoritative-only-when-local return, revalidation, and publish-before-
# replace all host-observable. Plain C11: no .m, no -lobjc.
.PHONY: test-objc-engine
test-objc-engine:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_objc_engine Tests/Host/test_objc_engine.c Sources/Engines/HKObjCEngine.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c -lpthread && $(THEOS_OBJ_DIR)/test_objc_engine$(ECHO_END)

# HookKit 3.0 native RELOCATING inline engine (Milestone 8). Preserves the
# displaced prologue in a trampoline so the original stays callable -- the one
# thing terminal inline refuses to do. Reuses native/hk_arm64.c's relocator
# as-is: it already writes into a caller-provided buffer, so allocation was
# always outside it. Two device seams (obtain an executable page, seal R-W to
# R-X); everything else is buffer arithmetic.
.PHONY: test-reloc-inline-engine
test-reloc-inline-engine:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_reloc_inline_engine Tests/Host/test_reloc_inline_engine.c Sources/Engines/HKRelocInlineEngine.c native/hk_arm64.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c -lpthread && $(THEOS_OBJ_DIR)/test_reloc_inline_engine$(ECHO_END)

# HookKit 3.0 end-to-end: the relocating inline engine through its adapter,
# AND alongside the terminal one (Milestone 8). The two describe themselves
# identically except for which originals they serve, so this is where the
# original-requirement routing criterion earns its keep.
.PHONY: test-reloc-inline-wired
test-reloc-inline-wired:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_reloc_inline_wired Tests/Host/test_reloc_inline_wired.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_reloc_inline_wired$(ECHO_END)

# HookKit 3.0 static continuation (Milestone 9): a fixed pool of trampoline
# slots that were executable at load, driven by the SAME relocating engine
# through a second vtable that declares no executable allocation. The survey
# found the engine needs no change -- its seams already describe what a pool
# does.
.PHONY: test-static-continuation
test-static-continuation:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_static_continuation Tests/Host/test_static_continuation.c Sources/Engines/HKStaticPool.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_static_continuation$(ECHO_END)

# HookKit 3.0 image-scope check (Sources/Core/HKImageScope.c): does an address
# actually lie in the image a request named, and is that image the expected
# build. Assembled from the catalog matcher, hk_macho_peek_header and
# hk_macho_image_span_for_loaded_image -- all already host-tested. A NULL or
# empty catalog means NOT CHECKED, not failed (a host or custom context may
# omit the production runtime's dyld-populated catalog).
.PHONY: test-image-scope
test-image-scope:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_image_scope Tests/Host/test_image_scope.c Sources/Core/HKImageScope.c Sources/Core/HKImageCatalog.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c -lpthread && $(THEOS_OBJ_DIR)/test_image_scope$(ECHO_END)

# HookKit 3.0 native TERMINAL inline engine (Milestone 7). Overwrites a
# function entry with a branch and stops there: no relocation, no trampoline,
# no executable allocation (spec 13.4). Reuses native/hk_arm64.c as-is -- that
# file is Darwin-free by design and test-reloc already runs it here. Only the
# store is device-only, and it is behind a seam.
.PHONY: test-inline-engine
test-inline-engine:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_inline_engine Tests/Host/test_inline_engine.c Sources/Engines/HKInlineEngine.c native/hk_arm64.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c -lpthread && $(THEOS_OBJ_DIR)/test_inline_engine$(ECHO_END)

# HookKit 3.0 end-to-end: the plan lifecycle driving the REAL terminal inline
# engine through its runtime adapter (Milestone 7). Fourth engine wired in and
# the first to reach HK_TARGET_FUNCTION_ADDRESS, so it exercises the plan's
# address-target path. No fake engine; the only fake is the write seam.
.PHONY: test-inline-wired
test-inline-wired:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_inline_wired Tests/Host/test_inline_wired.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_inline_wired$(ECHO_END)

# HookKit 3.0 end-to-end: the plan lifecycle driving the REAL ObjC engine
# through its runtime adapter (Milestone 6). Third engine wired in, and the
# first to reach HK_TARGET_OBJC_METHOD -- so this is what exercises the plan's
# ObjC-target path. No fake engine; the only fake is the runtime seam.
.PHONY: test-objc-wired
test-objc-wired:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_objc_wired Tests/Host/test_objc_wired.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_objc_wired$(ECHO_END)

# HookKit 3.0 end-to-end: the plan lifecycle driving the REAL rebind engine
# through its runtime adapter (Milestone 6). Real analyze/prepare/commit, real
# resolvers finding real slots in a synthetic image, real writes via a
# buffer-backed seam, real artifacts in the report. No fake engine.
.PHONY: test-rebind-wired
test-rebind-wired:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_rebind_wired Tests/Host/test_rebind_wired.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_rebind_wired$(ECHO_END)

# Milestone 5 conformance run against a REAL Mach-O image. Deliberately NOT
# part of `make test`: it needs a specimen pulled off a device, and specimens
# are third-party binaries that are not committed. See the header of
# Tools/conformance/macho_conformance.c for how to obtain one.
#
#   make conformance IMAGE=/path/to/libfoo.dylib
#   make conformance IMAGE=/path/to/libfoo.dylib SYMBOLS="malloc free"
.PHONY: conformance
conformance:
	$(ECHO_NOTHING)test -n "$(IMAGE)" || { echo "usage: make conformance IMAGE=<mach-o> [SYMBOLS=\"a b\"]"; exit 2; }; \
	mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O1 -o $(THEOS_OBJ_DIR)/macho_conformance Tools/conformance/macho_conformance.c Sources/Resolvers/*.c && $(THEOS_OBJ_DIR)/macho_conformance "$(IMAGE)" $(SYMBOLS)$(ECHO_END)

# Host-side test aggregate: builds and runs each suite in sequence, stopping
# at the first failure (no -k).
.PHONY: test
test:
	$(ECHO_NOTHING)$(MAKE) test-reloc test-swift-abi test-swift-engine test-header-compile test-abi test-shadow-manifest test-provider-evidence test-runtime-lifecycle test-plan-lifecycle test-hook-add test-plan-analyze test-engine-registry test-backend-policy test-backend-enumeration test-plan-prepare test-plan-commit test-ownership test-domain-gate test-artifact-ledger test-installed-original test-plan-model test-fault-injection test-image-catalog test-symbol-table test-macho test-export-trie test-symbol-resolve test-import-slots test-chained-fixups test-pointer-auth test-cache-patches test-rebind-engine test-rebind-pac test-rebind-wired test-memory-engine test-memory-wired test-objc-engine test-objc-wired test-inline-engine test-inline-wired test-image-scope test-reloc-inline-engine test-reloc-inline-wired test-static-continuation test-provider-vtable$(ECHO_END)

.PHONY: test-pointer-auth
test-pointer-auth:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -DHK_PTRAUTH_TEST=1 -o $(THEOS_OBJ_DIR)/test_pointer_auth Tests/Host/test_pointer_auth.c Sources/Core/HKOwnership.c -lpthread && $(THEOS_OBJ_DIR)/test_pointer_auth$(ECHO_END)

.PHONY: test-cache-patches
test-cache-patches:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_cache_patches Tests/Host/test_cache_patches.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c Sources/Resolvers/HKExportTrie.c native/hk_symbols.c && $(THEOS_OBJ_DIR)/test_cache_patches$(ECHO_END)

.PHONY: test-abi
test-abi:
	$(ECHO_NOTHING)python3 Tools/abi/test_extract_abi.py && python3 Tools/abi/test_compare_abi.py$(ECHO_END)

.PHONY: test-shadow-manifest
test-shadow-manifest:
	$(ECHO_NOTHING)python3 Tools/shadow-manifest-extract/test_extract.py && python3 Tools/shadow-manifest-extract/test_logos_extract.py$(ECHO_END)

.PHONY: test-provider-evidence
test-provider-evidence:
	$(ECHO_NOTHING)python3 Tools/provider-evidence/test_validate.py && python3 Tools/provider-evidence/validate.py$(ECHO_END)

.PHONY: test-provider-vtable
test-provider-vtable:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_provider_vtable Tests/Host/test_provider_vtable.c Sources/Engines/HKProviderVtable.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_provider_vtable$(ECHO_END)

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

.PHONY: test-swift-engine
test-swift-engine:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_swift_engine Tests/Host/test_swift_engine.c Sources/Engines/HKSwiftEngine.c native/hk_swift.c native/hk_native.c native/hk_arm64.c -ldl $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_swift_engine$(ECHO_END)

# HookKit 3.0 new-ABI header compile tests (spec section 21, Milestone 3):
# the same Headers/HookKit/*.h compiled and run under all 4 language modes.
# ObjC/ObjC++ reuse the existing fake Foundation/objc-runtime stubs
# (tests/fake_headers) rather than a new copy -- these new headers are
# Foundation-free by design, so the stub is only there to give the ObjC
# compiler front end a minimal NSObject/Class/SEL vocabulary to check
# against.
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
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_runtime_lifecycle Tests/Host/test_runtime_lifecycle.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKInstalled.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_runtime_lifecycle$(ECHO_END)

# HookKit 3.0 plan lifecycle + domain registration test (Milestone 4):
# real Sources/Core/HKPlan.c. The critical property this exercises is
# hk_domain_t* pointer stability across the internal array's realloc
# growth (37 domains, several times past the initial capacity of 4) --
# see HKPlanInternal.h for why domains are individually heap-allocated
# rather than stored inline in that array.
.PHONY: test-plan-lifecycle
test-plan-lifecycle:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_lifecycle Tests/Host/test_plan_lifecycle.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_plan_lifecycle$(ECHO_END)

# HookKit 3.0 hook registration test (Milestone 4): real hk_plan_add_hook,
# the deep copy of the full target union (symbol/address/objc/memory).
# Covers per-kind deep-copy verification, the foreign-domain and
# forward-commit_after rejections, hk_hook_t* pointer stability across growth,
# and recursive HK_IMAGE_EXPLICIT_SET ownership.
.PHONY: test-hook-add
test-hook-add:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_hook_add Tests/Host/test_hook_add.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_hook_add$(ECHO_END)

# HookKit 3.0 plan analysis test (Milestone 4): real hk_plan_analyze +
# hk_report_t (Sources/Core/HKReport.c). No engine registry exists yet, so
# every hook honestly gets HK_OUTCOME_NO_ROUTE -- this test is about the
# plumbing (state transitions, report/hook independence, result content)
# being correct given that starting point, not routing logic that doesn't
# exist yet.
.PHONY: test-plan-analyze
test-plan-analyze:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_analyze Tests/Host/test_plan_analyze.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_plan_analyze$(ECHO_END)

# HookKit 3.0 engine registry test (Milestone 4's "fake engines"): proves
# hk_plan_analyze actually consults registered engines now (see
# Sources/Core/HKEngineInternal.h for the minimal internal contract this
# is built on) -- an eligible engine upgrades a hook from NO_ROUTE to
# ANALYZED, an engine matching the target kind but not the required reach
# correctly stays NO_ROUTE, and first-eligible-wins correctly skips past
# an ineligible engine registered earlier.
.PHONY: test-engine-registry
test-engine-registry:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_engine_registry Tests/Host/test_engine_registry.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_engine_registry$(ECHO_END)

# HookKit 3.0 runtime ordering plus strict backend-ID selection.
.PHONY: test-backend-policy
test-backend-policy:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_backend_policy Tests/Host/test_backend_policy.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_backend_policy$(ECHO_END)

.PHONY: test-backend-enumeration
test-backend-enumeration:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_backend_enumeration Tests/Host/test_backend_enumeration.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_backend_enumeration$(ECHO_END)

# HookKit 3.0 plan preparation test (Milestone 4): real hk_plan_prepare.
# Covers the per-hook outcome transitions (ANALYZED -> PREPARED/
# FAILED_SAFE, NO_ROUTE left untouched), the plan-level PREPARED/PARTIAL/
# FAILED rollup, and that prepare calls the same engine analyze matched
# (hook->matched_engine) rather than re-searching the registry. Shares
# Tests/Host/fake_engines.h with test-engine-registry.
.PHONY: test-plan-prepare
test-plan-prepare:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_prepare Tests/Host/test_plan_prepare.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_plan_prepare$(ECHO_END)

# HookKit 3.0 plan commit test (Milestone 4): real hk_plan_commit. The
# property under test is the mutation-state -> outcome mapping (spec
# section 4.4/6.27) -- one of the spec's core invariants -- exercised via
# 4 distinct fake engines in Tests/Host/fake_engines.h (COMPLETE/NONE/
# PARTIAL/UNKNOWN, plus a commit_one == NULL case) rather than trusted
# from reading the switch statement.
.PHONY: test-plan-commit
test-plan-commit:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_commit Tests/Host/test_plan_commit.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKOwnership.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_plan_commit$(ECHO_END)

.PHONY: test-ownership
test-ownership:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_ownership Tests/Host/test_ownership.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKOwnership.c Sources/Core/HKRuntime.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_ownership$(ECHO_END)

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
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_domain_gate Tests/Host/test_domain_gate.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_domain_gate$(ECHO_END)

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
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_installed_original Tests/Host/test_installed_original.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_installed_original$(ECHO_END)

# HookKit 3.0 model-based test of the plan lifecycle state machine (Milestone
# 4). An independent reference model predicts accept/reject + resulting state
# for every (state, op); random operation sequences cross-check it against a
# real plan, and a coverage assertion proves the whole (state x op) table was
# exercised. Success path only -- FAILED/PARTIAL rollups live in
# test-plan-prepare / test-plan-commit.
.PHONY: test-plan-model
test-plan-model:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_plan_model Tests/Host/test_plan_model.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_plan_model$(ECHO_END)

# HookKit 3.0 fault-injection (OOM) sweep (Milestone 4). Wraps
# malloc/calloc/realloc via the linker so it can fail the Nth allocation, then
# runs the full plan lifecycle once per N until no failure fires -- so every
# allocation site is the failure point exactly once. Enforces that an
# OUT_OF_MEMORY return never advances plan state. Run under ASan separately to
# also catch OOM-path leaks; here it catches crashes and the state invariant.
.PHONY: test-fault-injection
test-fault-injection:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 $(if $(filter Linux,$(HOST_OS)),-Wl$(comma)--wrap=malloc$(comma)--wrap=calloc$(comma)--wrap=realloc) -o $(THEOS_OBJ_DIR)/test_fault_injection Tests/Host/test_fault_injection.c Sources/Core/HKImageCatalog.c Sources/Core/HKIDs.c Sources/Core/HKRuntime.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKArtifactLedger.c Sources/Core/HKInstalled.c $(HK_PLATFORM_ENGINE_SOURCES) -lpthread $(HK_PLATFORM_ENGINE_LDFLAGS) && $(THEOS_OBJ_DIR)/test_fault_injection$(ECHO_END)

# HookKit 3.0 image catalog test (Milestone 5). The platform-agnostic half:
# selector matching (all 6 hk_image_selector_kind_t cases + EXPLICIT_SET
# union/dedup) against synthetic entries. Real dyld population is device-only
# (see HKImageCatalog.h) and not exercised here. Self-contained.
.PHONY: test-image-catalog
test-image-catalog:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_image_catalog Tests/Host/test_image_catalog.c Sources/Core/HKImageCatalog.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c && $(THEOS_OBJ_DIR)/test_image_catalog$(ECHO_END)

# HookKit 3.0 Mach-O symbol table search (Milestone 5, private-symbol
# resolver). Pure logic over a caller-supplied table view, so it is fully
# host-testable against synthetic tables: name conventions, visibility
# filtering, STAB rejection, and bounds safety against a malformed
# (unterminated) string table. Self-contained.
.PHONY: test-symbol-table
test-symbol-table:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_symbol_table Tests/Host/test_symbol_table.c Sources/Resolvers/HKSymbolTable.c && $(THEOS_OBJ_DIR)/test_symbol_table$(ECHO_END)

# HookKit 3.0 chained fixups, metadata half (Milestone 5). The MODERN iOS 15+
# import mechanism, which the LC_DYSYMTAB path does not cover. Cross-checks
# the parser against Apple's own vendored definitions in
# vendor/litehook/fixup-chains.h, including the bit layouts.
.PHONY: test-chained-fixups
test-chained-fixups:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_chained_fixups Tests/Host/test_chained_fixups.c Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c native/hk_symbols.c && $(THEOS_OBJ_DIR)/test_chained_fixups$(ECHO_END)

# HookKit 3.0 import slot resolution (Milestone 5). Maps each symbol-pointer
# slot to the symbol it binds to, via LC_DYSYMTAB's indirect symbol table --
# the question a rebind engine must answer. Bounded where fishhook's
# equivalent walk trusts dyld's prior validation.
.PHONY: test-import-slots
test-import-slots:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_import_slots Tests/Host/test_import_slots.c Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c native/hk_symbols.c && $(THEOS_OBJ_DIR)/test_import_slots$(ECHO_END)

# HookKit 3.0 resolver-selection layer (Milestone 5). The single place that
# decides HOW a symbol is looked up: name normalization in one place, and the
# source preference order that finally makes hk_symbol_visibility_t mean
# something. Pure logic over caller-supplied sources.
.PHONY: test-symbol-resolve
test-symbol-resolve:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_symbol_resolve Tests/Host/test_symbol_resolve.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c native/hk_symbols.c && $(THEOS_OBJ_DIR)/test_symbol_resolve$(ECHO_END)

# HookKit 3.0 export trie resolver (Milestone 5). ULEB128 decoding + trie
# walking against synthetic tries: the proper path for EXPORTED symbols
# (the symbol table is the private-symbol path). Pure buffer logic. Includes
# the cycle guard, whose absence hangs rather than crashing -- a failure mode
# no sanitizer detects.
.PHONY: test-export-trie
test-export-trie:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_export_trie Tests/Host/test_export_trie.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c && $(THEOS_OBJ_DIR)/test_export_trie$(ECHO_END)

# HookKit 3.0 Mach-O container parsing (Milestone 5). Pure buffer logic:
# header validation, bounded load-command iteration, and building the
# LC_SYMTAB view that the symbol search consumes -- including an end-to-end
# test composing both resolvers. Only obtaining a real image is device-only.
.PHONY: test-macho
test-macho:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && clang -Wall -Wextra -Werror -std=c11 -O2 -o $(THEOS_OBJ_DIR)/test_macho Tests/Host/test_macho.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolTable.c && $(THEOS_OBJ_DIR)/test_macho$(ECHO_END)

# Canonical HookKit lifecycle smoke. This intentionally exercises only public C ABI
# loading and immutable artifact reads; engine installation remains a
# separate device gate until production registration is enabled.
DEVICE_CANONICAL_SDK ?= $(THEOS)/sdks/iPhoneOS16.5.sdk
DEVICE_CANONICAL_ARCH ?= arm64
DEVICE_CANONICAL_MIN ?= 15.0
DEVICE_CANONICAL_LDID ?= ldid
# The Swift driver otherwise selects the host runtime resources and host ld.
# Keep the device probe on the iPhoneOS Swift runtime and target linker.
SWIFT_DEVICE_RESOURCE_DIR ?= $(THEOS)/toolchain/linux/iphone/lib/swift

# Modern arm64e uses a versioned ptrauth Mach-O ABI. The canonical package
# already selects this toolchain in build.sh; standalone device smokes must do
# the same or they can link an arm64e.old executable that no modern device can
# execute. The bundled Linux Swift 5.8 driver also needs its arm64e assembly
# reassembled below; C/ObjC companion objects use the same modern wrapper.
HOST_OS ?= $(shell uname -s)

# A literal comma, for embedding one inside a $(if ...)/$(filter ...) call's
# own comma-delimited arguments (e.g. an -Wl,opt1,opt2 flag list below).
comma := ,

# hk_runtime_register_platform_engines (Sources/Core/HKRuntime.c, __APPLE__-only)
# unconditionally wires up every native engine plus the ObjC runtime shims,
# regardless of which single engine a given host test exercises. Any host
# test linking HKRuntime.c needs this whole closure on a Darwin host or the
# linker fails on macOS-only lanes while Linux (where the block does not
# compile) stays green. HOOKKIT_CANONICAL_3-gated providers (Dobby/Gum/
# ElleKit/Substitute) are deliberately excluded: host tests never define
# HOOKKIT_CANONICAL_3, so those stay unreferenced either way.
HK_PLATFORM_ENGINE_SOURCES = Sources/Core/HKImageScope.c Sources/Engines/HKInlineEngine.c Sources/Engines/HKInlineVtable.c Sources/Engines/HKMemoryEngine.c Sources/Engines/HKMemoryVtable.c Sources/Engines/HKObjCEngine.c Sources/Engines/HKObjCVtable.c Sources/Engines/HKRebindEngine.c Sources/Engines/HKRebindVtable.c Sources/Engines/HKRelocInlineEngine.c Sources/Engines/HKRelocInlineVtable.c Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c native/hk_arm64.c native/hk_native.c native/hk_symbols.c
HK_PLATFORM_ENGINE_LDFLAGS = $(if $(filter Darwin,$(HOST_OS)),-lobjc)
MODERN_TOOLCHAIN ?= $(THEOS)/toolchain/modern/linux/iphone
DEVICE_CANONICAL_CLANG ?= $(SDKBINPATH)/clang
DEVICE_CANONICAL_SWIFTC ?= $(SDKBINPATH)/swiftc
DEVICE_CANONICAL_LD ?= $(SDKBINPATH)/ld
ifeq ($(HOST_OS),Linux)
ifeq ($(DEVICE_CANONICAL_ARCH),arm64e)
DEVICE_CANONICAL_CLANG := $(MODERN_TOOLCHAIN)/bin/clang
DEVICE_CANONICAL_LD := $(MODERN_TOOLCHAIN)/bin/ld
endif
endif

DEVICE_CANONICAL_SWIFT_FLAGS = -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) \
	-sdk $(DEVICE_CANONICAL_SDK) -resource-dir $(SWIFT_DEVICE_RESOURCE_DIR) \
	-parse-as-library -module-name HKSwiftProbe
DEVICE_CANONICAL_SWIFT_OBJECT = $(DEVICE_CANONICAL_SWIFTC) $(DEVICE_CANONICAL_SWIFT_FLAGS) \
	-emit-object -o $(1) $(2)
DEVICE_CANONICAL_SWIFT_ABI_GUARD = :
ifeq ($(DEVICE_CANONICAL_ARCH),arm64e)
# Swift 5.8's Linux driver writes arm64e.old objects even though its assembly
# contains modern ptrauth instructions. Reassemble that assembly with the
# verified clang wrapper, then prove the resulting object has the ptrauth ABI
# marker instead of allowing the final Swift link to mask the mismatch.
DEVICE_CANONICAL_SWIFT_ABI_GUARD = test "$$(od -An -tx1 -j11 -N1 $(1) | tr -d ' ')" = 80 || { echo "error: $(1) is arm64e.old; configure a Swift compiler that emits the versioned ptrauth ABI" >&2; exit 1; }
ifeq ($(HOST_OS),Linux)
DEVICE_CANONICAL_SWIFT_OBJECT = $(DEVICE_CANONICAL_SWIFTC) $(DEVICE_CANONICAL_SWIFT_FLAGS) \
	-emit-assembly -o $(1).s $(2) && $(DEVICE_CANONICAL_CLANG) \
	-target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) \
	-isysroot $(DEVICE_CANONICAL_SDK) -c -o $(1) $(1).s && rm -f $(1).s
endif
endif

DEVICE_CANONICAL_TARGETS := device-lifecycle-smoke device-objc-smoke device-swift-smoke \
	device-swift-real-smoke \
	device-catalog-smoke device-resolver-smoke device-rebind-smoke \
	device-rebind-adapter-smoke device-static-smoke device-provider-smoke \
	device-provider-lifecycle-smoke device-provider-alias-smoke
.PHONY: check-device-canonical-toolchain

check-device-canonical-toolchain:
ifeq ($(HOST_OS),Linux)
ifeq ($(DEVICE_CANONICAL_ARCH),arm64e)
	$(ECHO_NOTHING)bash $(CURDIR)/scripts/setup-modern-toolchain.sh --verify $(MODERN_TOOLCHAIN)$(ECHO_END)
endif
endif

$(DEVICE_CANONICAL_TARGETS): check-device-canonical-toolchain

.PHONY: device-lifecycle-smoke
device-lifecycle-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -F$(CURDIR)/.theos/obj -framework HookKit -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_lifecycle_smoke tests/device_lifecycle_smoke.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_lifecycle_smoke$(ECHO_END)

.PHONY: device-objc-smoke
device-objc-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -fobjc-arc -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -F$(CURDIR)/.theos/obj -framework HookKit -lobjc -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_objc_smoke tests/device_objc_smoke.m && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_objc_smoke$(ECHO_END)

.PHONY: device-swift-smoke
device-swift-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -F$(CURDIR)/.theos/obj -framework HookKit -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_swift_smoke tests/device_swift_smoke.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_swift_smoke$(ECHO_END)

# Real Swift metadata/vtable smoke. The probe class is compiled into the
# executable, so it never patches a system class and restores its own slot
# before exiting. Build canonical HookKit first; this target links that ABI.
.PHONY: device-swift-real-smoke
device-swift-real-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(call DEVICE_CANONICAL_SWIFT_OBJECT,$(THEOS_OBJ_DIR)/device_swift_real_probe.o,tests/device_swift_real_probe.swift) && $(call DEVICE_CANONICAL_SWIFT_ABI_GUARD,$(THEOS_OBJ_DIR)/device_swift_real_probe.o) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -c -o $(THEOS_OBJ_DIR)/device_swift_real_smoke.o tests/device_swift_real_smoke.c && $(DEVICE_CANONICAL_SWIFTC) -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -sdk $(DEVICE_CANONICAL_SDK) -resource-dir $(SWIFT_DEVICE_RESOURCE_DIR) -use-ld=$(DEVICE_CANONICAL_LD) -F$(CURDIR)/.theos/obj -framework HookKit -Xlinker -rpath -Xlinker /Library/Frameworks -Xlinker -rpath -Xlinker /var/jb/Library/Frameworks -Xlinker -rpath -Xlinker /usr/lib/swift -o $(THEOS_OBJ_DIR)/device_swift_real_smoke $(THEOS_OBJ_DIR)/device_swift_real_smoke.o $(THEOS_OBJ_DIR)/device_swift_real_probe.o && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_swift_real_smoke$(ECHO_END)

.PHONY: device-catalog-smoke
device-catalog-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -I$(CURDIR)/Sources/Core -I$(CURDIR)/Sources/Resolvers -o $(THEOS_OBJ_DIR)/device_catalog_smoke tests/device_image_catalog.c Sources/Core/HKImageCatalog.c Sources/Resolvers/HKMachO.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_catalog_smoke$(ECHO_END)

.PHONY: device-resolver-smoke
device-resolver-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -F$(CURDIR)/.theos/obj -framework HookKit -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_resolver_smoke tests/device_resolver.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_resolver_smoke$(ECHO_END)

.PHONY: device-rebind-smoke
device-rebind-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -fobjc-arc -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Sources/Engines -I$(CURDIR)/Sources/Resolvers -I$(CURDIR)/Sources/Core -lobjc -o $(THEOS_OBJ_DIR)/device_rebind_smoke tests/device_rebind.m Sources/Engines/HKRebindEngine.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKMachO.c native/hk_native.c native/hk_arm64.c native/hk_symbols.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_rebind_smoke$(ECHO_END)

.PHONY: device-rebind-adapter-smoke
device-rebind-adapter-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -fobjc-arc -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -F$(CURDIR)/.theos/obj -framework HookKit -lobjc -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_rebind_adapter_smoke tests/device_rebind_adapter.m && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_rebind_adapter_smoke$(ECHO_END)

.PHONY: device-static-smoke
device-static-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -I$(CURDIR)/Sources/Core -I$(CURDIR)/Sources/Engines -I$(CURDIR)/Sources/Resolvers -I$(CURDIR)/native -lobjc -framework CoreFoundation -o $(THEOS_OBJ_DIR)/device_static_smoke tests/device_static_continuation.c Sources/Core/HKArtifactLedger.c Sources/Core/HKIDs.c Sources/Core/HKImageCatalog.c Sources/Core/HKImageScope.c Sources/Core/HKInstalled.c Sources/Core/HKOwnership.c Sources/Core/HKPlan.c Sources/Core/HKReport.c Sources/Core/HKRuntime.c Sources/Engines/HKInlineEngine.c Sources/Engines/HKInlineVtable.c Sources/Engines/HKMemoryEngine.c Sources/Engines/HKMemoryVtable.c Sources/Engines/HKObjCEngine.c Sources/Engines/HKObjCVtable.c Sources/Engines/HKRebindEngine.c Sources/Engines/HKRebindVtable.c Sources/Engines/HKRelocInlineEngine.c Sources/Engines/HKRelocInlineVtable.c Sources/Engines/HKStaticPool.c Sources/Engines/HKSwiftEngine.c Sources/Resolvers/HKChainedFixups.c Sources/Resolvers/HKDyldCachePatches.c Sources/Resolvers/HKExportTrie.c Sources/Resolvers/HKImportSlots.c Sources/Resolvers/HKMachO.c Sources/Resolvers/HKSymbolResolve.c Sources/Resolvers/HKSymbolTable.c native/hk_arm64.c native/hk_native.c native/hk_symbols.c native/hk_swift.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_static_smoke$(ECHO_END)

.PHONY: device-provider-smoke
device-provider-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/vendor/dobby -L$(CURDIR)/vendor/dobby -o $(THEOS_OBJ_DIR)/device_provider tests/device_provider.c -ldobby -lc++ && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_provider$(ECHO_END)

.PHONY: device-provider-lifecycle-smoke
device-provider-lifecycle-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/Headers -I$(CURDIR)/Sources/Core -I$(CURDIR)/Sources/Engines -F$(CURDIR)/.theos/obj -framework HookKit -lobjc -rpath /Library/Frameworks -rpath /var/jb/Library/Frameworks -o $(THEOS_OBJ_DIR)/device_provider_lifecycle tests/device_provider_lifecycle.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_provider_lifecycle$(ECHO_END)

.PHONY: device-provider-alias-smoke
device-provider-alias-smoke:
	$(ECHO_NOTHING)mkdir -p $(THEOS_OBJ_DIR) && $(DEVICE_CANONICAL_CLANG) -Wall -Wextra -Werror -O0 -fno-inline -target $(DEVICE_CANONICAL_ARCH)-apple-ios$(DEVICE_CANONICAL_MIN) -isysroot $(DEVICE_CANONICAL_SDK) -I$(CURDIR)/vendor/libhooker -o $(THEOS_OBJ_DIR)/device_provider_alias tests/device_provider_alias.c && $(DEVICE_CANONICAL_LDID) -S$(CURDIR)/tests/device_smoke.entitlements $(THEOS_OBJ_DIR)/device_provider_alias$(ECHO_END)
