#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <errno.h>
#import <pthread.h>
#import <stdlib.h>
#import <string.h>

#import "vendor/libhooker/libhooker.h"

#pragma mark - libhooker (ElleKit) runtime resolution

// libhooker is dlopen'd at runtime so that HookKit loads cleanly without ElleKit installed.
// fishhook is compiled in and always available.
static void *libhooker_handle = NULL;
// The libhooker ABI is NOT uniform across providers. The vendored header
// (libhooker.h, coolstar's real libhooker for unc0ver/Taurine) declares
// LHHookFunctions/LHPatchMemory returning an applied count. ElleKit exposes
// the same symbols with zero-on-success semantics.
//
// The provider is classified by an observable EXPORT, never by image
// filename (H8): ElleKit exposes its own EKHookFunction entry point
// alongside the libhooker-compat surface, and real libhooker never does —
// marker present means the ElleKit zero-on-success ABI; marker absent with
// the complete official libhooker symbol set
// means the count-returning libhooker ABI. Anything else fails CLOSED: no
// hook is ever attempted, because a misclassification could apply a hook
// and then suppress its original.
typedef NS_ENUM(uint8_t, HKEKABI) {
    HKEKABIUnknown = 0,   // provider loaded but unclassifiable: refuse every operation
    HKEKABIElleKit,       // zero-on-success ABI (EKHookFunction marker present)
    HKEKABILibhooker      // applied-count ABI (complete official symbol set, no marker)
};

static HKEKABI libhooker_abi = HKEKABIUnknown;
static int (*fn_LHHookFunctions)(const struct LHFunctionHook *, int) = NULL;
static int (*fn_LHPatchMemory)(const struct LHMemoryPatch *, int) = NULL;
static struct libhooker_image *(*fn_LHOpenImage)(const char *) = NULL;
static void (*fn_LHCloseImage)(struct libhooker_image *) = NULL;
static bool (*fn_LHFindSymbols)(struct libhooker_image *, const char **, void **, size_t) = NULL;

// Defined below probe_libhooker, which calls it during ABI detection.
static BOOL provider_exports_official_set(void *hookerHandle);

// Only successful probes are cached: if dlopen fails, a later call retries
// (the engine may appear after HookKit loads).
static BOOL probe_libhooker(void) {
    NSString *hookerPath = HKJBPath(@"/usr/lib/libhooker.dylib");

    if(!hookerPath) {
        return NO;
    }

    void *hookerHandle = dlopen([hookerPath fileSystemRepresentation], RTLD_LAZY);

    if(!hookerHandle) {
        return NO;
    }

    // Resolve into locals first: the globals are published only after the
    // ENTIRE required symbol set is present, so an incomplete library can
    // never leave half-populated function pointers behind.
    int (*LHHookFunctions)(const struct LHFunctionHook *, int) = (int (*)(const struct LHFunctionHook *, int))dlsym(hookerHandle, "LHHookFunctions");
    int (*LHPatchMemory)(const struct LHMemoryPatch *, int) = (int (*)(const struct LHMemoryPatch *, int))dlsym(hookerHandle, "LHPatchMemory");
    struct libhooker_image *(*LHOpenImage)(const char *) = (struct libhooker_image *(*)(const char *))dlsym(hookerHandle, "LHOpenImage");
    void (*LHCloseImage)(struct libhooker_image *) = (void (*)(struct libhooker_image *))dlsym(hookerHandle, "LHCloseImage");
    bool (*LHFindSymbols)(struct libhooker_image *, const char **, void **, size_t) = (bool (*)(struct libhooker_image *, const char **, void **, size_t))dlsym(hookerHandle, "LHFindSymbols");

    // ABI-incomplete: drop the handle and stay uncached so a later probe
    // genuinely retries (the engine may gain the full ABI after HookKit
    // loads). Nothing was published.
    if(!(LHHookFunctions && LHPatchMemory
            && LHOpenImage && LHCloseImage && LHFindSymbols)) {
        dlclose(hookerHandle);
        return NO;
    }

    // Provider ABI detection (H8): classify by observable exports, never by
    // image filename. Probe only this provider's handles: RTLD_DEFAULT could
    // find an unrelated ElleKit image when real libhooker is also installed.
    if(dlsym(hookerHandle, "EKHookFunction")) {
        // ElleKit: 0-on-success LHHookFunctions/LHPatchMemory.
        libhooker_abi = HKEKABIElleKit;
    } else if(provider_exports_official_set(hookerHandle)) {
        // Real libhooker (unc0ver/Taurine): applied-count returns + errno.
        libhooker_abi = HKEKABILibhooker;
    } else {
        dlclose(hookerHandle);
        return NO;
    }

    libhooker_handle = hookerHandle;
    fn_LHHookFunctions = LHHookFunctions;
    fn_LHPatchMemory = LHPatchMemory;
    fn_LHOpenImage = LHOpenImage;
    fn_LHCloseImage = LHCloseImage;
    fn_LHFindSymbols = LHFindSymbols;

    return YES;
}

// Official libhooker exports used to distinguish its count-returning ABI from
// an unknown compatibility provider when the ElleKit marker is absent.
static BOOL provider_exports_official_set(void *hookerHandle) {
    static const char *const officialSymbols[] = {
        "LHHookFunctions", "LHPatchMemory",
        "LHOpenImage", "LHCloseImage", "LHFindSymbols",
        "LHStrError", "LHExecMemory"
    };

    for(size_t i = 0; i < sizeof(officialSymbols) / sizeof(officialSymbols[0]); i++) {
        if(!dlsym(hookerHandle, officialSymbols[i])) {
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
// Deliberately uncached: the two cheap preflights retry if the engine appears
// after HookKit loads (mirroring the activation probe's retry contract).
BOOL libhooker_discoverable(void) {
    NSString *hookerPath = HKJBPath(@"/usr/lib/libhooker.dylib");

    if(!hookerPath) {
        return NO;
    }

    return dlopen_preflight([hookerPath fileSystemRepresentation]);
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
