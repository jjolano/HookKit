#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <pthread.h>

#import "native/hk_native.h"
#import "native/hk_swift.h"

#pragma mark - Backend registry

// One table drives selection, availability, type reporting and the info dicts;
// adding a backend is one entry here plus its picker rows in
// hk_category_priorities (the single source of truth for category membership).
// Order is priority order.

// fishhook is compiled in, so it is the floor that is always present.
static BOOL fishhook_available(void) {
    return YES;
}

// litehook is compiled in and available on all archs.
static BOOL litehook_available(void) {
    return YES;
}

static BOOL native_available(void) {
    return hk_native_supported() ? YES : NO;
}

// Swift vtables: available when the arch supports the engine (arm64/arm64e),
// the Swift 5 ABI runtime is present (iOS 12.2+), and swift_demangle
// resolves. libswiftCore is a plain system dylib — never a jailbreak path,
// so no path rewrite. The probe result is cached unconditionally, success or
// failure: a Swift runtime that appears after the first probe is not retried.
static BOOL swift_available(void) {
    // swift_available() can be queried concurrently (registry selection and
    // availability introspection), so the probe's cached statics and the
    // published hk_swift_demangle global must be serialized; the publish
    // happens-before any reader that got YES through the same mutex.
    // ponytail: pthread mutex instead of dispatch_once — the probe's cached
    // statics and the published hk_swift_demangle global must be serialized
    // against concurrent registry queries; os_unfair_lock needs iOS 10+,
    // above the 9.0 deployment floor (Makefile TARGET). There is no retry
    // contract to preserve: unlike the jailbreak-provider probes, this probe
    // is cached unconditionally (a Swift runtime appearing later is not
    // retried — see the top-of-function comment), so once-semantics would
    // behave identically.
    static pthread_mutex_t probeMutex = PTHREAD_MUTEX_INITIALIZER;
    static BOOL cached = NO;
    static BOOL available = NO;

    pthread_mutex_lock(&probeMutex);

    if(!cached) {
        if(hk_swift_supported()) {
            NSOperatingSystemVersion swiftFloor = { 12, 2, 0 };
            if([[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:swiftFloor]) {
                hk_swift_demangle = (hk_swift_demangle_fn)dlsym(RTLD_DEFAULT, "swift_demangle");

                if(!hk_swift_demangle) {
                    void *core = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_LAZY);

                    if(core) {
                        hk_swift_demangle = (hk_swift_demangle_fn)dlsym(core, "swift_demangle");
                    }
                }

                available = hk_swift_demangle != NULL;
            }
        }

        // Swift-specific contract: cached unconditionally (unlike the
        // jailbreak-provider probes, which retry a failed dlopen).
        cached = YES;
    }

    BOOL result = available;
    pthread_mutex_unlock(&probeMutex);

    return result;
}

// Dobby is compiled in on modern arm64/arm64e only; the table entry stays on
// every arch/lane so the public type values and backend count remain stable.
static BOOL dobby_available(void) {
#if (defined(__arm64__) || defined(__arm64e__)) && !defined(HK_NO_DOBBY)
    return YES;
#else
    return NO;
#endif
}

const HKBackendDescriptor *hk_backends(size_t *outCount) {
    static HKBackendDescriptor table[9];
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        // kinds is the adapter hook-kind mask encoded in category bits:
        // HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE for the function
        // kind (the technique fields disambiguate which), HK_CAT_MEMORY for
        // memory patching. publicationPolicy is indexed by
        // HKFunctionTechnique: None / Rebind / Inline.
        // Unavailable means the backend has no such technique at all (fail
        // closed: no original can ever be published under it).
        table[0] = (HKBackendDescriptor){
            .type = HK_LIB_ELLEKIT,
            .backendClass = [HKElleKitBackend class],
            .identifier = @"ellekit",
            .name = @"ElleKit",
            .available = libhooker_available,
            .discoverable = libhooker_discoverable,
            .automatic = YES,
            .selectable = YES,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE | HK_CAT_MEMORY,
            // H8: the Inline policy depends on the DETECTED provider ABI —
            // real libhooker's LHHookFunctions writes the original BEFORE the
            // patch activates (verified upstream), while ElleKit's patch
            // writer runs first and the original lands in the out cell
            // afterwards.
            // HKOriginalPublicationRuntime defers to
            // hk_resolved_publication_policy, which reads the runtime ABI
            // classification (hk_ellekit_current_function_policy); an
            // unclassified provider fails closed to Unavailable.
            .defaultTechnique = HKFunctionTechniqueInline,
            .publicationPolicy = { HKOriginalPublicationBeforeActivation, HKOriginalPublicationUnavailable, HKOriginalPublicationRuntime },
        };
        // Substrate/Substitute memory: MSHookMemory / SubHookMemory are
        // resolved at probe time but NOT required for the probe (a build
        // without them still passes), so the memory bit is capability-if-
        // present — hookMemory: truthfully refuses HK_ERR_NOT_SUPPORTED at
        // dispatch when the symbol is absent.
        table[1] = (HKBackendDescriptor){
            .type = HK_LIB_SUBSTRATE,
            .backendClass = [HKSubstrateBackend class],
            .identifier = @"substrate",
            .name = @"Cydia Substrate",
            .available = substrate_available,
            .discoverable = substrate_discoverable,
            .automatic = YES,
            .selectable = NO,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE | HK_CAT_MEMORY,
            .defaultTechnique = HKFunctionTechniqueInline,
            .publicationPolicy = { HKOriginalPublicationBeforeActivation, HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation },
        };
        table[2] = (HKBackendDescriptor){
            .type = HK_LIB_SUBSTITUTE,
            .backendClass = [HKSubstituteBackend class],
            .identifier = @"substitute",
            .name = @"Substitute",
            .available = substitute_available,
            .discoverable = substitute_discoverable,
            .automatic = YES,
            .selectable = NO,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE | HK_CAT_MEMORY,
            .defaultTechnique = HKFunctionTechniqueInline,
            .publicationPolicy = { HKOriginalPublicationBeforeActivation, HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation },
        };
        // Not eligible for the legacy pinned default; capability/operation
        // routing may still reach it.
        table[3] = (HKBackendDescriptor){
            .type = HK_LIB_NATIVE,
            .backendClass = [HKNativeBackend class],
            .identifier = @"native",
            .name = @"HookKit",
            .available = native_available,
            .discoverable = native_available,
            .automatic = NO,
            .selectable = YES,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE | HK_CAT_MEMORY,
            .defaultTechnique = HKFunctionTechniqueInline,
            // Trampoline is sealed (relocated + written) before hk_write
            // activates the patch.
            .publicationPolicy = { HKOriginalPublicationBeforeActivation, HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation },
        };
        // Not eligible for the legacy pinned default. The implicit function
        // router may reach Dobby through FUNCTION_INLINE.
        table[4] = (HKBackendDescriptor){
            .type = HK_LIB_DOBBY,
            .backendClass = [HKDobbyBackend class],
            .identifier = @"dobby",
            .name = @"Dobby",
            .available = dobby_available,
            .discoverable = dobby_available,
            .automatic = NO,
            .selectable = YES,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE | HK_CAT_MEMORY,
            .defaultTechnique = HKFunctionTechniqueInline,
            // The vendored static lib is rebuilt from upstream 5dfc854 with a
            // reorder patch: DobbyHook publishes *out_origin_func BEFORE
            // routing->Active() commits the trampoline (instruction-verified
            // in both slices, 2026-08-11) — safe for a requested original.
            .publicationPolicy = { HKOriginalPublicationUnavailable, HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation },
        };
        // Not eligible for the legacy pinned default; category routing reaches
        // it only after the lighter inline candidates decline.
        table[5] = (HKBackendDescriptor){
            .type = HK_LIB_FRIDA,
            .backendClass = [HKFridaBackend class],
            .identifier = @"frida",
            .name = @"Frida",
            .available = frida_available,
            .discoverable = frida_discoverable,
            .automatic = NO,
            .selectable = YES,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE,
            .defaultTechnique = HKFunctionTechniqueInline,
            // Originals are staged inside the gum transaction and published
            // before end_transaction activates the batch.
            .publicationPolicy = { HKOriginalPublicationUnavailable, HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation },
        };
        table[6] = (HKBackendDescriptor){
            .type = HK_LIB_FISHHOOK,
            .backendClass = [HKFishhookBackend class],
            .identifier = @"fishhook",
            .name = @"fishhook",
            .available = fishhook_available,
            .discoverable = fishhook_available,
            .automatic = YES,
            .selectable = YES,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE,
            .defaultTechnique = HKFunctionTechniqueRebind,
            // Single-op: rebind_symbols_hook's publish callback fires before the
            // first slot write. Batch: rebind_symbols_hook_batch's per-rebinding
            // publish cells do the same for each op. No inline path exists.
            .publicationPolicy = { HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation, HKOriginalPublicationUnavailable },
        };
        // Never automatic: Swift vtable hooks are a separate API (no
        // message/function overlap) and only make sense when the caller has a
        // Swift class in hand, so this backend is opt-in.
        table[7] = (HKBackendDescriptor){
            .type = HK_LIB_SWIFT,
            .backendClass = [HKSwiftBackend class],
            .identifier = @"swift",
            .name = @"Swift vtables",
            .available = swift_available,
            .discoverable = swift_available,
            .automatic = NO,
            .selectable = NO,
            .kinds = HK_CAT_NONE,   // vtable metadata only; none of the three hook kinds
            .defaultTechnique = HKFunctionTechniqueNone,
            .publicationPolicy = { HKOriginalPublicationUnavailable, HKOriginalPublicationUnavailable, HKOriginalPublicationUnavailable },
        };
        // Not eligible for the legacy pinned default. Operation routing uses
        // it as the compiled-in fallback for rebind and memory coverage.
        // Judgment call: the memory bit is set — litehook_hook_memory is a
        // real, KERN_SUCCESS-checked patch path, so the descriptor must say
        // so truthfully.
        table[8] = (HKBackendDescriptor){
            .type = HK_LIB_LITEHOOK,
            .backendClass = [HKLitehookBackend class],
            .identifier = @"litehook",
            .name = @"litehook",
            .available = litehook_available,
            .discoverable = litehook_available,
            .automatic = NO,
            .selectable = YES,
            .kinds = HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE | HK_CAT_MEMORY,
            // Vendor default is the rebind path (strategy zero-inits to
            // HKStrategyDefault); inline is opt-in via setStrategy:.
            .defaultTechnique = HKFunctionTechniqueRebind,
            // Rebind: the original IS the untouched function body. Inline:
            // no original trampoline — the backend itself refuses old_ptr.
            .publicationPolicy = { HKOriginalPublicationUnavailable, HKOriginalPublicationBeforeActivation, HKOriginalPublicationUnavailable },
        };
    });

    *outCount = sizeof(table) / sizeof(table[0]);
    return table;
}

const HKBackendDescriptor *hk_backend_descriptor_for_type(hookkit_lib_t type) {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if(table[i].type == type) {
            return &table[i];
        }
    }

    return NULL;
}

const HKBackendDescriptor *hk_backend_descriptor_for_backend(id<HKSubstitutorBackend> backend) {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; backend && i < count; i++) {
        if([backend isKindOfClass:table[i].backendClass]) {
            return &table[i];
        }
    }

    return NULL;
}

#pragma mark - Original-publication policy resolution

// H8: the combined ElleKit/libhooker backend's Inline policy depends on the
// DETECTED provider ABI (real libhooker's LHHookFunctions writes the original
// before the patch activates; ElleKit's patch writer runs first), so the
// descriptor marks that entry HKOriginalPublicationRuntime and the policy is
// resolved here, at call time, from the backend's runtime classification
// (hk_ellekit_current_function_policy, in HKElleKitBackend.m where the ABI
// detection lives). Every other entry resolves to its descriptor value as-is.
// Unknown backends fail closed to Unavailable (nothing can ever be published).
HKOriginalPublicationPolicy hk_resolved_publication_policy(hookkit_lib_t backendType, HKFunctionTechnique technique) {
    const HKBackendDescriptor *descriptor = hk_backend_descriptor_for_type(backendType);

    if(descriptor) {
        HKOriginalPublicationPolicy policy = descriptor->publicationPolicy[(int)technique];
        return policy == HKOriginalPublicationRuntime
            ? hk_ellekit_current_function_policy()
            : policy;
    }

    return HKOriginalPublicationUnavailable;
}

#pragma mark - Category priorities

// Per-category priority order, as (backend, technique) pairs: each entry
// lists the pickers that satisfy the category, in the order they are tried
// (first available wins). The strategy is passed to the backend when it
// implements setStrategy:; backends without it use their vendor default
// technique. The order matches the main hk_backends table priority within
// each category.
// Message hooking is facade-native and therefore has no picker row. This table
// is the single source of truth for backend-selected categories.
const HKCategoryPriority hk_category_priorities[] = {
    { HK_CAT_FUNCTION_REBIND, { {HK_LIB_FISHHOOK, HKStrategyRebind, HKFunctionTechniqueRebind},
                                {HK_LIB_LITEHOOK, HKStrategyRebind, HKFunctionTechniqueRebind} }, 2 },
    // Prologue inline trampolines are AArch64-only: litehook, Dobby and Frida
    // emit AArch64 instructions unconditionally, so litehook's picker is
    // arm64/arm64e-only. On 32-bit archs ElleKit, Dobby and Frida still cover
    // the category, and Dobby/Frida report unavailable at runtime, so no
    // resolution can select HKStrategyInline there.
    { HK_CAT_FUNCTION_INLINE, { {HK_LIB_ELLEKIT, HKStrategyInline, HKFunctionTechniqueInline},
                                {HK_LIB_DOBBY, HKStrategyInline, HKFunctionTechniqueInline},
                                {HK_LIB_FRIDA, HKStrategyInline, HKFunctionTechniqueInline},
#if defined(__arm64__) || defined(__arm64e__)
                                // litehook inline: no original trampoline, so
                                // the Inline policy is Unavailable — a caller
                                // that requested an original must be refused.
                                {HK_LIB_LITEHOOK, HKStrategyInline, HKFunctionTechniqueInline} },
                               4 },
#else
                                // litehook's inline trampoline emits AArch64
                                // opcodes only (see HKLitehookBackend), so its
                                // inline picker is arm64/arm64e-only; on 32-bit
                                // archs ElleKit, Dobby and Frida still cover
                                // the category. litehook rebind and memory use
                                // stay available on 32-bit.
                                },
                               3 },
#endif
#if defined(__arm64__) || defined(__arm64e__)
    // Private-symbol rows resolve to the backend's own function-hook routine
    // after the DSC lookup: the providers hook the found address inline
    // (HKFunctionTechniqueInline), litehook falls through to its rebind path.
    { HK_CAT_PRIVATE_SYMBOL,  { {HK_LIB_ELLEKIT, HKStrategyPrivateSymbol, HKFunctionTechniqueInline},
                                {HK_LIB_SUBSTRATE, HKStrategyPrivateSymbol, HKFunctionTechniqueInline},
                                {HK_LIB_SUBSTITUTE, HKStrategyPrivateSymbol, HKFunctionTechniqueInline},
                                {HK_LIB_LITEHOOK, HKStrategyPrivateSymbol, HKFunctionTechniqueRebind} }, 4 },
#else
    // litehook's DSC private-symbol lookup hardcodes 64-bit structures
    // (mach_header_64 / LC_SEGMENT_64 / section_64 / nlist_64), so on 32-bit
    // archs the private-symbol category drops the litehook picker — ElleKit,
    // Substrate and Substitute still cover it. Explicit litehook rebind and
    // memory use stays available on 32-bit; only this category picker is out.
    { HK_CAT_PRIVATE_SYMBOL,  { {HK_LIB_ELLEKIT, HKStrategyPrivateSymbol, HKFunctionTechniqueInline},
                                {HK_LIB_SUBSTRATE, HKStrategyPrivateSymbol, HKFunctionTechniqueInline},
                                {HK_LIB_SUBSTITUTE, HKStrategyPrivateSymbol, HKFunctionTechniqueInline} }, 3 },
#endif
};
const size_t hk_category_priority_count = sizeof(hk_category_priorities) / sizeof(hk_category_priorities[0]);
