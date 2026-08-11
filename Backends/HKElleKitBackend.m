#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <errno.h>
#import <pthread.h>
#import <stdlib.h>
#import <string.h>

#import "vendor/libhooker/libhooker.h"
#import "vendor/libhooker/libblackjack.h"

#pragma mark - libhooker (ElleKit) runtime resolution

// libhooker is dlopen'd at runtime so that HookKit loads cleanly without ElleKit installed.
// fishhook is compiled in and always available.
static void* libhooker_handle = NULL;
// The libhooker ABI is NOT uniform across providers. The vendored header
// (libhooker.h, coolstar's real libhooker for unc0ver/Taurine) declares
// LBHookMessage returning enum LIBHOOKER_ERR and LHHookFunctions/
// LHPatchMemory returning an APPLIED COUNT. ElleKit (Dopamine/palera1n)
// exports the same symbols with a VOID LBHookMessage (success = the out
// pointer being written) and 0-on-success for the other two. Reading one
// provider's return under the other's semantics makes every hook look
// failed, the caller's original IMP gets suppressed, and v1-era tweaks
// whose %orig reads that slot crash with a NULL call (Shadow 3.7.6's
// %hook SpringBoard -applicationDidFinishLaunching: sent SpringBoard into
// safe mode exactly this way).
//
// The provider is classified by an observable EXPORT, never by image
// filename (H8): ElleKit exposes its own EKHookFunction entry point
// alongside the libhooker-compat surface, and real libhooker never does —
// marker present means the ElleKit zero-on-success ABI; marker absent with
// the complete official symbol set (vendored libhooker.h + libblackjack.h)
// means the count-returning libhooker ABI. Anything else fails CLOSED: no
// hook is ever attempted, because a misclassification could apply a hook
// and then suppress its original.
typedef NS_ENUM(uint8_t, HKEKABI) {
    HKEKABIUnknown = 0,   // provider loaded but unclassifiable: refuse every operation
    HKEKABIElleKit,       // zero-on-success ABI (EKHookFunction marker present)
    HKEKABILibhooker      // applied-count ABI (complete official symbol set, no marker)
};

static HKEKABI libhooker_abi = HKEKABIUnknown;
static void (*fn_LBHookMessage)(Class, SEL, void *, void *) = NULL;
static int (*fn_LHHookFunctions)(const struct LHFunctionHook *, int) = NULL;
static int (*fn_LHPatchMemory)(const struct LHMemoryPatch *, int) = NULL;
static struct libhooker_image *(*fn_LHOpenImage)(const char *) = NULL;
static void (*fn_LHCloseImage)(struct libhooker_image *) = NULL;
static bool (*fn_LHFindSymbols)(struct libhooker_image *, const char **, void **, size_t) = NULL;

// Defined below probe_libhooker, which calls it during ABI detection.
static BOOL provider_exports_official_set(void *handle);

// Only successful probes are cached: if dlopen fails, a later call retries
// (the engine may appear after HookKit loads).
static BOOL probe_libhooker(void) {
    NSString *jbPath = HKJBPath(@"/usr/lib/libhooker.dylib");

    if(!jbPath) {
        return NO;
    }

    libhooker_handle = dlopen([jbPath fileSystemRepresentation], RTLD_LAZY);

    if(!libhooker_handle) {
        return NO;
    }

    // Resolve into locals first: the globals are published only after the
    // ENTIRE required symbol set is present, so an incomplete library can
    // never leave half-populated function pointers behind.
    void (*LBHookMessage)(Class, SEL, void *, void *) = (void (*)(Class, SEL, void *, void *))dlsym(libhooker_handle, "LBHookMessage");
    int (*LHHookFunctions)(const struct LHFunctionHook *, int) = (int (*)(const struct LHFunctionHook *, int))dlsym(libhooker_handle, "LHHookFunctions");
    int (*LHPatchMemory)(const struct LHMemoryPatch *, int) = (int (*)(const struct LHMemoryPatch *, int))dlsym(libhooker_handle, "LHPatchMemory");
    struct libhooker_image *(*LHOpenImage)(const char *) = (struct libhooker_image *(*)(const char *))dlsym(libhooker_handle, "LHOpenImage");
    void (*LHCloseImage)(struct libhooker_image *) = (void (*)(struct libhooker_image *))dlsym(libhooker_handle, "LHCloseImage");
    bool (*LHFindSymbols)(struct libhooker_image *, const char **, void **, size_t) = (bool (*)(struct libhooker_image *, const char **, void **, size_t))dlsym(libhooker_handle, "LHFindSymbols");

    // ABI-incomplete: drop the handle and stay uncached so a later probe
    // genuinely retries (the engine may gain the full ABI after HookKit
    // loads). Nothing was published.
    if(!(LBHookMessage && LHHookFunctions && LHPatchMemory
            && LHOpenImage && LHCloseImage && LHFindSymbols)) {
        dlclose(libhooker_handle);
        libhooker_handle = NULL;
        return NO;
    }

    fn_LBHookMessage = LBHookMessage;
    fn_LHHookFunctions = LHHookFunctions;
    fn_LHPatchMemory = LHPatchMemory;
    fn_LHOpenImage = LHOpenImage;
    fn_LHCloseImage = LHCloseImage;
    fn_LHFindSymbols = LHFindSymbols;

    // Provider ABI detection (H8): classify by observable exports, never by
    // image filename. EKHookFunction is ElleKit's own entry point; the compat
    // layer may re-export it into this handle or dlopen libellekit lazily, so
    // probe the handle first and the global scope second before concluding the
    // provider is not ElleKit.
    if(dlsym(libhooker_handle, "EKHookFunction") || dlsym(RTLD_DEFAULT, "EKHookFunction")) {
        // ElleKit: void LBHookMessage, 0-on-success LHHookFunctions/LHPatchMemory.
        libhooker_abi = HKEKABIElleKit;
    } else if(provider_exports_official_set(libhooker_handle)) {
        // Real libhooker (unc0ver/Taurine): applied-count returns + errno.
        libhooker_abi = HKEKABILibhooker;
    } else {
        // Unclassifiable provider (partial/shim surface): fail closed — every
        // operation is refused (see refuseIfUnknownABI). A misclassification
        // could apply a hook and then suppress its original.
        libhooker_abi = HKEKABIUnknown;
    }

    return YES;
}

// The complete official libhooker symbol set, per the vendored headers
// (libhooker.h + libblackjack.h): LBHookMessage, LHHookFunctions,
// LHPatchMemory, LHOpenImage, LHCloseImage, LHFindSymbols, LHStrError,
// LHExecMemory. The whole set present — with the EKHookFunction marker
// absent — identifies real libhooker's count-returning ABI; anything less
// is an incomplete shim and fails closed.
static BOOL provider_exports_official_set(void *handle) {
    static const char *const officialSymbols[] = {
        "LBHookMessage", "LHHookFunctions", "LHPatchMemory",
        "LHOpenImage", "LHCloseImage", "LHFindSymbols",
        "LHStrError", "LHExecMemory"
    };

    for(size_t i = 0; i < sizeof(officialSymbols) / sizeof(officialSymbols[0]); i++) {
        if(!dlsym(handle, officialSymbols[i])) {
            return NO;
        }
    }

    return YES;
}

BOOL libhooker_available(void) {
    // libhooker_available() can be queried concurrently (registry selection
    // and availability introspection), so the probe's cached statics, the
    // ABI decision and the published function-pointer globals must be
    // serialized; the publish happens-before any reader that got YES
    // through the same mutex.
    // ponytail: pthread mutex instead of dispatch_once — once-semantics
    // would cache a failed probe forever and break the documented retry
    // contract (engine may appear after HookKit loads); os_unfair_lock
    // needs iOS 10+, above the 9.0 deployment floor (Makefile TARGET).
    static pthread_mutex_t probeMutex = PTHREAD_MUTEX_INITIALIZER;
    static BOOL cached = NO;
    static BOOL available = NO;

    pthread_mutex_lock(&probeMutex);

    if(!cached && probe_libhooker()) {
        if(libhooker_abi == HKEKABIUnknown) {
            // H8: an unclassifiable provider must not pin selection — every
            // operation it receives is refused (refuseIfUnknownABI), so a
            // libhooker_available() YES would starve default selection. Report
            // NOT available so the pickers fall through to the next backend
            // (fishhook, etc.). Deliberately UNCACHED, matching the failed-
            // dlopen retry contract: the provider's export surface is a
            // snapshot that could gain a classifiable symbol set after
            // HookKit loads, and a later probe then picks it up. (dlopen of
            // an already-mapped image is a refcount bump — re-probing is
            // cheap.)
            available = NO;
        } else {
            available = YES;
            cached = YES;
        }
    }

    BOOL result = available;
    pthread_mutex_unlock(&probeMutex);

    return result;
}

// Preflight-only discovery, for the availability-introspection entry points
// (getAvailableSubstitutorTypes / getAvailableCategories): reports loadability
// WITHOUT loading — dlopen_preflight never maps the image and never runs its
// constructors, so introspection cannot initialize a hooking provider.
// Deliberately uncached: the check is a single stat-family syscall on the
// preflight path, and an uncached probe retries if the engine appears after
// HookKit loads (mirroring the activation probe's retry contract).
BOOL libhooker_discoverable(void) {
    NSString *jbPath = HKJBPath(@"/usr/lib/libhooker.dylib");

    if(!jbPath) {
        return NO;
    }

    return dlopen_preflight([jbPath fileSystemRepresentation]);
}

// Runtime original-publication policy for the combined ElleKit/libhooker
// backend (declared in Internal/HKBackendInternal.h; consumed by
// hk_resolved_publication_policy in HKBackendRegistry.m). The policy depends
// on the DETECTED provider ABI, never the library name:
//   real libhooker — LHHookFunctions writes the original BEFORE the patch
//                    activates (verified upstream), so a requested original
//                    is safe.
//   ElleKit        — the patch writer runs first and the original lands in
//                    the out cell afterwards: a requested original must be
//                    refused (or used without one).
//   Unknown        — safest policy, fail closed: the backend refuses every
//                    operation, so no original can ever exist — Unavailable.
HKOriginalPublicationPolicy hk_ellekit_current_function_policy(void) {
    switch(libhooker_abi) {
        case HKEKABILibhooker:
            return HKOriginalPublicationBeforeActivation;

        case HKEKABIElleKit:
            return HKOriginalPublicationAfterActivation;

        default:
            return HKOriginalPublicationUnavailable;
    }
}

#pragma mark - HKElleKitBackend

@implementation HKElleKitBackend
- (int)lastErrno {
    return _lastErrno;
}

// Fail-closed gate for an unclassifiable provider (HKEKABIUnknown): no hook
// is ever attempted — a misclassification could apply a hook and then
// suppress its original — and every operation reports NOT_SUPPORTED with
// ENOTSUP detail so the caller can retry against another backend.
- (hookkit_status_t)refuseIfUnknownABI {
    if(libhooker_abi == HKEKABIUnknown) {
        _lastErrno = ENOTSUP;
        return HK_ERR_NOT_SUPPORTED;
    }

    return HK_OK;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    hookkit_status_t refused = [self refuseIfUnknownABI];

    if(refused != HK_OK) {
        return refused;
    }

    // LBHookMessage's ABI is provider-dependent:
    //  - ElleKit: void — success is the original being written into the out
    //    cell; nothing written means the selector exists on neither the class
    //    nor the metaclass (ElleKit messageHook's guard).
    //  - real libhooker: int — LIBHOOKER_OK (0) on success,
    //    LIBHOOKER_ERR_SELECTOR_NOT_FOUND (1) when the selector is absent.
    // Both write the original on success, so the out cell is the common
    // success signal; check it before any return-value reading. Reading the
    // void return as an error enum was the ABI mismatch: the garbage value
    // (typically the original's low bits) was treated as failure, the
    // caller's original stayed suppressed, and v1-era tweaks calling %orig
    // through the NULL slot crashed (Shadow 3.7.6's %hook SpringBoard was
    // exactly this).
    void *cell = NULL;
    fn_LBHookMessage(objcClass, selector, replacement, (void *)&cell);

    if(!cell) {
        _lastErrno = LIBHOOKER_ERR_SELECTOR_NOT_FOUND;
        return HK_ERR_NOT_SUPPORTED;
    }

    _lastErrno = 0;

    if(old_ptr) {
        *old_ptr = cell;
    }

    return HK_OK;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    hookkit_status_t refused = [self refuseIfUnknownABI];

    if(refused != HK_OK) {
        return refused;
    }

    struct LHFunctionHook hook = {
        function, replacement, (void *)old_ptr, NULL
    };

    // LHHookFunctions semantics by provider:
    //  - ElleKit: LIBHOOKER_OK (0) on success; non-zero is the failure detail.
    //  - real libhooker: applied count (1 for a single hook); 0 on failure.
    // A single-hook call succeeds when the result equals the requested count
    // under the applied-count ABI, or equals LIBHOOKER_OK under ElleKit's.
    // The count ABI's failure detail is the documented errno channel, not the
    // return value: clear errno before the call so a stale thread-local value
    // can never leak, and report it only when the provider actually set it (a
    // zero errno stays zero — a generic capability miss).
    errno = 0;
    int result = fn_LHHookFunctions(&hook, 1);

    if(libhooker_abi == HKEKABILibhooker) {
        _lastErrno = errno;
        return result == 1 ? HK_OK : HK_ERR_NOT_SUPPORTED;
    }

    _lastErrno = result;
    return result == LIBHOOKER_OK ? HK_OK : HK_ERR;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    hookkit_status_t refused = [self refuseIfUnknownABI];

    if(refused != HK_OK) {
        return refused;
    }

    struct LHMemoryPatch patch = {
        target, data, size, 0
    };

    // LHPatchMemory semantics by provider:
    //  - ElleKit: LIBHOOKER_OK (0) on success, 1 when a patch is malformed.
    //  - real libhooker: applied count (1 for a single patch); 0 on failure.
    // Count ABI failures report through errno (see hookFunction:).
    errno = 0;
    int result = fn_LHPatchMemory(&patch, 1);

    if(libhooker_abi == HKEKABILibhooker) {
        _lastErrno = errno;
        return result == 1 ? HK_OK : HK_ERR_NOT_SUPPORTED;
    }

    _lastErrno = result;
    return result == LIBHOOKER_OK ? HK_OK : HK_ERR;
}

// Batch apply contract (protocol): void — each operation carries its own
// final status (and backendErrno) after the drain, and each original is
// written through hk_original_output_cell so it lands in the caller's cell
// (when requested) the moment the vendor API writes it — at-apply-time
// visibility for mid-batch re-entrancy.
- (void)executeHooks:(NSArray<HKHookOperation *> *)hooks {
    // Fail closed: an unclassifiable provider never receives a hook.
    if(libhooker_abi == HKEKABIUnknown) {
        _lastErrno = ENOTSUP;

        for(HKHookOperation *hook in hooks) {
            hook->status = HK_ERR_NOT_SUPPORTED;
            hook->backendErrno = ENOTSUP;
        }

        return;
    }

    // First-failure detail across the whole batch: message ops report the
    // LIBHOOKER_ERR enum, the count-based APIs report errno (cleared just
    // before each call so a short count without an errno write cannot leak a
    // stale thread-local value). Set once, on the FIRST failure; a later
    // success never erases an earlier failure's detail.
    int detail = 0;

    NSMutableData *functionHooks = [NSMutableData new];
    NSMutableData *memoryHooks = [NSMutableData new];
    NSMutableArray<HKHookOperation *> *functionOps = [NSMutableArray new];
    NSMutableArray<HKHookOperation *> *memoryOps = [NSMutableArray new];

    for(HKHookOperation *hook in hooks) {
        switch(hook->kind) {
            case HKHookKindMessage: {
                // LBHookMessage: the out cell is the common success signal on
                // BOTH ABIs (ElleKit's void return has no status to read; the
                // libhooker enum return is redundant with the cell) — nothing
                // written means the selector exists on neither the class nor
                // the metaclass. The output cell doubles as the caller's %orig
                // cell when one was requested, so the original is visible at
                // apply time: a later op in this same batch can re-enter this
                // hook (ElleKit's patch writer calls vm_region_recurse_64,
                // which Shadow's Hook_Memory group hooks) and must not hit a
                // NULL original.
                void **cell = hk_original_output_cell(&hook->original);
                fn_LBHookMessage(hook->objcClass, hook->selector, hook->replacement, (void *)cell);

                if(*cell) {
                    hook->status = HK_OK;
                } else {
                    hook->status = HK_ERR_NOT_SUPPORTED;
                    hook->backendErrno = LIBHOOKER_ERR_SELECTOR_NOT_FOUND;

                    if(!detail) {
                        detail = LIBHOOKER_ERR_SELECTOR_NOT_FOUND;
                    }
                }

                break;
            }

            case HKHookKindFunction: {
                // The output cell is the out-pointer: the vendor writes the
                // original into the caller's cell (when requested) the moment
                // the hook applies, so a mid-batch re-entrant call into this
                // replacement sees a real original.
                struct LHFunctionHook lh = {
                    hook->function, hook->replacement, hk_original_output_cell(&hook->original), NULL
                };

                [functionHooks appendBytes:&lh length:sizeof(struct LHFunctionHook)];
                [functionOps addObject:hook];
                break;
            }

            case HKHookKindMemory: {
                struct LHMemoryPatch lh = {
                    hook->target, [hook->data bytes], hook->size, 0
                };

                [memoryHooks appendBytes:&lh length:sizeof(struct LHMemoryPatch)];
                [memoryOps addObject:hook];
                break;
            }
        }
    }

    if([functionHooks length]) {
        int count = (int)([functionHooks length] / sizeof(struct LHFunctionHook));
        errno = 0;
        int result = fn_LHHookFunctions([functionHooks mutableBytes], count);

        if(libhooker_abi == HKEKABILibhooker) {
            // Real libhooker: result is the number of hooks applied; errno is
            // the failure detail. Clamp the provider's claim before using it
            // as an index bound: an over-count would index past functionOps
            // and a negative claim would corrupt the success tally.
            int applied = result < 0 ? 0 : (result > count ? count : result);

            if(applied < count) {
                NSLog(@"[HKElleKit] warning: batch LHHookFunctions retval less than expected (%d/%d)", result, count);
            }

            for(int i = 0; i < applied; i++) {
                functionOps[i]->status = HK_OK;
            }

            for(int i = applied; i < count; i++) {
                // Not applied: nothing was written for this op. NOT_SUPPORTED
                // (releases the guard; the facade restores the caller's cell)
                // with errno detail when the provider recorded one.
                functionOps[i]->status = HK_ERR_NOT_SUPPORTED;
                functionOps[i]->backendErrno = errno;

                if(!detail) {
                    detail = errno;
                }
            }
        } else {
            // ElleKit: LIBHOOKER_OK (0) on success; non-zero is the failure
            // detail itself, not a partial count. A 0 return means every op
            // succeeded (ElleKit iterates all entries and writes each orig);
            // a non-zero return is a hard error — the batch may be partially
            // applied, so the guards must taint rather than release.
            if(result != LIBHOOKER_OK) {
                NSLog(@"[HKElleKit] warning: batch LHHookFunctions failed (%d)", result);
            }

            for(HKHookOperation *op in functionOps) {
                if(result == LIBHOOKER_OK) {
                    op->status = HK_OK;
                } else {
                    op->status = HK_ERR;
                    op->backendErrno = result;

                    if(!detail) {
                        detail = result;
                    }
                }
            }
        }
    }

    if([memoryHooks length]) {
        int count = (int)([memoryHooks length] / sizeof(struct LHMemoryPatch));
        errno = 0;
        int result = fn_LHPatchMemory([memoryHooks mutableBytes], count);

        if(libhooker_abi == HKEKABILibhooker) {
            // Real libhooker: result is the number of patches applied; clamp
            // the claim before using it as an index bound, as above.
            int applied = result < 0 ? 0 : (result > count ? count : result);

            if(applied < count) {
                NSLog(@"[HKElleKit] warning: batch LHPatchMemory retval less than expected (%d/%d)", result, count);
            }

            for(int i = 0; i < applied; i++) {
                memoryOps[i]->status = HK_OK;
            }

            for(int i = applied; i < count; i++) {
                memoryOps[i]->status = HK_ERR_NOT_SUPPORTED;
                memoryOps[i]->backendErrno = errno;

                if(!detail) {
                    detail = errno;
                }
            }
        } else {
            if(result != LIBHOOKER_OK) {
                NSLog(@"[HKElleKit] warning: batch LHPatchMemory failed (%d)", result);
            }

            for(HKHookOperation *op in memoryOps) {
                if(result == LIBHOOKER_OK) {
                    op->status = HK_OK;
                } else {
                    op->status = HK_ERR;
                    op->backendErrno = result;

                    if(!detail) {
                        detail = result;
                    }
                }
            }
        }
    }

    _lastErrno = detail;
}

- (HKImageRef)openImage:(NSString *)path {
    return (HKImageRef)fn_LHOpenImage([path fileSystemRepresentation]);
}

- (void)closeImage:(HKImageRef)image {
    if(image) {
        fn_LHCloseImage((struct libhooker_image *)image);
    }
}

// ElleKit expects C symbols with a leading underscore; Substrate-style names
// come in without one. Try the name as given, then with '_' prepended.
- (void *)findSymbol:(const char *)name inImage:(struct libhooker_image *)image {
    if(!image || !name || !name[0]) {
        return NULL;
    }

    void *result = NULL;
    const char *probe = name;

    if(fn_LHFindSymbols(image, &probe, &result, 1) && result) {
        return result;
    }

    if(name[0] == '_') {
        return NULL;
    }

    char *prefixed = malloc(strlen(name) + 2);

    if(!prefixed) {
        // OOM: errno is ENOMEM from malloc; nothing was written anywhere.
        _lastErrno = ENOMEM;
        return NULL;
    }

    prefixed[0] = '_';
    strcpy(prefixed + 1, name);

    result = NULL;
    probe = prefixed;
    fn_LHFindSymbols(image, &probe, &result, 1);
    free(prefixed);

    return result;
}

- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName {
    const char *symbol = [symbolName UTF8String];

    if(image) {
        return [self findSymbol:symbol inImage:(struct libhooker_image *)image];
    }

    // image == NULL: iterate all loaded dyld images
    return hk_search_loaded_images(^void *(const char *image_name) {
        struct libhooker_image *libhookerImage = fn_LHOpenImage(image_name);

        if(!libhookerImage) {
            // no handle, no symbol lookup: skip this image
            return NULL;
        }

        void *result = [self findSymbol:symbol inImage:libhookerImage];
        fn_LHCloseImage(libhookerImage);
        return result;
    });
}
@end