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
# Mach-O dylib versions: must match HookKit.tbd (current/compatibility 2.4.0)
# so consumers linking via the tbd record a satisfiable requirement. Theos
# sets no versions itself, so they come from here.
HookKit_LDFLAGS += -current_version 2.4.0 -compatibility_version 2.4.0
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
# libc++ comes from Dobby on the slices that link it; the arm64e slice below
# drops Dobby but still needs it for the ObjC function-local-static guards.
HookKit_LDFLAGS += -lc++
# STOPGAP: the vendored archive's arm64e slice is old-ABI arm64e -- modern
# Xcode's ld reports "found architecture 'arm64e.old', required architecture
# 'arm64e'", discards every member, and the link dies on undefined _DobbyHook.
# Until vendor/dobby/libdobby.a is rebuilt with a new-ABI arm64e slice, drop
# Dobby from that slice only (theos re-reads this makefile per arch with
# THEOS_CURRENT_ARCH set). HKDobbyBackend keeps its class and table entry and
# reports unavailable there via dobby_available(), exactly as on armv7 and the
# legacy lane. Revert by deleting this conditional and restoring the plain
# -Lvendor/dobby -ldobby.
ifeq ($(THEOS_CURRENT_ARCH),arm64e)
HookKit_CFLAGS += -DHK_NO_DOBBY
else
HookKit_LDFLAGS += -Lvendor/dobby -ldobby
endif
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
	$(ECHO_NOTHING)$(MAKE) test-reloc test-swift-abi test-substitute-classifier test-inline-guard test-original-publication$(ECHO_END)

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
