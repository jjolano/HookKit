#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <errno.h>
#import <mach-o/dyld.h>
#import <mach/mach.h>
#import <objc/runtime.h>
#import <pthread.h>
#import <string.h>

#import "vendor/substrate/substrate.h"
#import "vendor/substitute/substitute.h"
#import "Internal/HKSubstituteErrors.h"

#pragma mark - Cydia Substrate / Substitute (MS-compatible API) runtime resolution

// Both libraries expose the classic Cydia Substrate C API. Cydia Substrate
// exports the MS* symbols directly; libsubstitute exports them under MS*
// (older versions) or Sub* (newer versions) names, so try both.
static void *substrate_handle = NULL;
static void (*substrate_hookFunction)(void *, void *, void **) = NULL;
static void (*substrate_hookMessageEx)(Class, SEL, void *, void **) = NULL;
static void *(*substrate_getImageByName)(const char *) = NULL;
static void *(*substrate_findSymbol)(void *, const char *) = NULL;
static void (*substrate_hookMemory)(void *, const void *, size_t) = NULL;

// Only successful probes are cached: if dlopen fails, a later call retries
// (the engine may appear after HookKit loads).
static BOOL probe_substrate(void) {
    NSString *jbPath = HKJBPath(@"/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate");

    if(!jbPath) {
        return NO;
    }

    void *handle = dlopen([jbPath fileSystemRepresentation], RTLD_LAZY);

    if(!handle) {
        return NO;
    }

    // Resolve into locals first: the globals are published only after the
    // ENTIRE required symbol set is present, so an incomplete library can
    // never leave half-populated function pointers behind.
    void (*hookFunction)(void *, void *, void **) = (void (*)(void *, void *, void **))dlsym(handle, "MSHookFunction");
    void (*hookMessageEx)(Class, SEL, void *, void **) = (void (*)(Class, SEL, void *, void **))dlsym(handle, "MSHookMessageEx");
    void *(*getImageByName)(const char *) = (void *(*)(const char *))dlsym(handle, "MSGetImageByName");
    void *(*findSymbol)(void *, const char *) = (void *(*)(void *, const char *))dlsym(handle, "MSFindSymbol");
    void (*hookMemory)(void *, const void *, size_t) = (void (*)(void *, const void *, size_t))dlsym(handle, "MSHookMemory");

    // ABI-incomplete: drop the handle and stay uncached so a later probe
    // genuinely retries (the engine may gain the full ABI after HookKit
    // loads). Nothing was published.
    if(!(hookFunction && hookMessageEx && getImageByName && findSymbol)) {
        dlclose(handle);
        return NO;
    }

    substrate_handle = handle;
    substrate_hookFunction = hookFunction;
    substrate_hookMessageEx = hookMessageEx;
    substrate_getImageByName = getImageByName;
    substrate_findSymbol = findSymbol;
    substrate_hookMemory = hookMemory;

    return YES;
}

BOOL substrate_available(void) {
    // substrate_available() can be queried concurrently (registry selection
    // and availability introspection), so the probe's cached statics and the
    // published function-pointer globals must be serialized; the publish
    // happens-before any reader that got YES through the same mutex.
    // ponytail: pthread mutex instead of dispatch_once — once-semantics
    // would cache a failed probe forever and break the documented retry
    // contract (engine may appear after HookKit loads); os_unfair_lock
    // needs iOS 10+, above the 9.0 deployment floor (Makefile TARGET).
    static pthread_mutex_t probeMutex = PTHREAD_MUTEX_INITIALIZER;
    static BOOL cached = NO;
    static BOOL available = NO;

    pthread_mutex_lock(&probeMutex);

    if(!cached && probe_substrate()) {
        available = YES;
        cached = YES;
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
BOOL substrate_discoverable(void) {
    NSString *jbPath = HKJBPath(@"/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate");

    if(!jbPath) {
        return NO;
    }

    return dlopen_preflight([jbPath fileSystemRepresentation]);
}

static void *libsubstitute_handle = NULL;
static void (*substitute_hookFunction)(void *, void *, void **) = NULL;
static void (*substitute_hookMessageEx)(Class, SEL, void *, void **) = NULL;
static void *(*substitute_getImageByName)(const char *) = NULL;
static void *(*substitute_findSymbol)(void *, const char *) = NULL;
static void (*substitute_hookMemory)(void *, const void *, size_t) = NULL;

// Native libsubstitute API, preferred over the MS-compatible shims when present.
static int (*fn_substitute_hook_functions)(const struct substitute_function_hook *, size_t, struct substitute_function_hook_record **, int) = NULL;
static int (*fn_substitute_hook_objc_message)(Class, SEL, void *, void *, bool *) = NULL;
static struct substitute_image *(*fn_substitute_open_image)(const char *) = NULL;
static void (*fn_substitute_close_image)(struct substitute_image *) = NULL;
static int (*fn_substitute_find_private_syms)(struct substitute_image *, const char **, void **, size_t) = NULL;
static void *(*fn_substitute_sym_to_ptr)(struct substitute_image *, substitute_sym *) = NULL;
static int (*fn_substitute_interpose_imports)(const struct substitute_image *, const struct substitute_import_hook *, size_t, struct substitute_import_hook_record **, int) = NULL;
static BOOL substitute_native_available = NO;

static void *resolve_ms_symbol(void *handle, const char *name, const char *fallback) {
    void *symbol = dlsym(handle, name);

    if(!symbol && fallback) {
        symbol = dlsym(handle, fallback);
    }

    return symbol;
}

// Only successful probes are cached: if dlopen fails, a later call retries
// (the engine may appear after HookKit loads).
static BOOL probe_substitute(void) {
    NSString *jbPath = HKJBPath(@"/usr/lib/libsubstitute.0.dylib");

    if(!jbPath) {
        return NO;
    }

    void *handle = dlopen([jbPath fileSystemRepresentation], RTLD_LAZY);

    if(!handle) {
        return NO;
    }

    // Resolve into locals first: the globals are published only after the
    // ENTIRE required symbol set is present, so an incomplete library can
    // never leave half-populated function pointers behind.
    void (*hookFunction)(void *, void *, void **) = (void (*)(void *, void *, void **))resolve_ms_symbol(handle, "MSHookFunction", "SubHookFunction");
    void (*hookMessageEx)(Class, SEL, void *, void **) = (void (*)(Class, SEL, void *, void **))resolve_ms_symbol(handle, "MSHookMessageEx", "SubHookMessageEx");
    void *(*getImageByName)(const char *) = (void *(*)(const char *))resolve_ms_symbol(handle, "MSGetImageByName", "SubGetImageByName");
    void *(*findSymbol)(void *, const char *) = (void *(*)(void *, const char *))resolve_ms_symbol(handle, "MSFindSymbol", "SubFindSymbol");
    void (*hookMemory)(void *, const void *, size_t) = (void (*)(void *, const void *, size_t))resolve_ms_symbol(handle, "MSHookMemory", "SubHookMemory");

    int (*hookFunctions)(const struct substitute_function_hook *, size_t, struct substitute_function_hook_record **, int) = (int (*)(const struct substitute_function_hook *, size_t, struct substitute_function_hook_record **, int))dlsym(handle, "substitute_hook_functions");
    int (*hookObjcMessage)(Class, SEL, void *, void *, bool *) = (int (*)(Class, SEL, void *, void *, bool *))dlsym(handle, "substitute_hook_objc_message");
    struct substitute_image *(*openImage)(const char *) = (struct substitute_image *(*)(const char *))dlsym(handle, "substitute_open_image");
    void (*closeImage)(struct substitute_image *) = (void (*)(struct substitute_image *))dlsym(handle, "substitute_close_image");
    int (*findPrivateSyms)(struct substitute_image *, const char **, void **, size_t) = (int (*)(struct substitute_image *, const char **, void **, size_t))dlsym(handle, "substitute_find_private_syms");
    void *(*symToPtr)(struct substitute_image *, substitute_sym *) = (void *(*)(struct substitute_image *, substitute_sym *))dlsym(handle, "substitute_sym_to_ptr");
    int (*interposeImports)(const struct substitute_image *, const struct substitute_import_hook *, size_t, struct substitute_import_hook_record **, int) = (int (*)(const struct substitute_image *, const struct substitute_import_hook *, size_t, struct substitute_import_hook_record **, int))dlsym(handle, "substitute_interpose_imports");

    BOOL nativeAvailable = hookFunctions && hookObjcMessage
        && openImage && closeImage
        && findPrivateSyms && symToPtr;

    // ABI-incomplete (neither the MS-compatible shim set nor the native set
    // is fully present): drop the handle and stay uncached so a later probe
    // genuinely retries. Nothing was published.
    if(!((hookFunction && hookMessageEx && getImageByName && findSymbol)
            || nativeAvailable)) {
        dlclose(handle);
        return NO;
    }

    libsubstitute_handle = handle;
    substitute_hookFunction = hookFunction;
    substitute_hookMessageEx = hookMessageEx;
    substitute_getImageByName = getImageByName;
    substitute_findSymbol = findSymbol;
    substitute_hookMemory = hookMemory;

    fn_substitute_hook_functions = hookFunctions;
    fn_substitute_hook_objc_message = hookObjcMessage;
    fn_substitute_open_image = openImage;
    fn_substitute_close_image = closeImage;
    fn_substitute_find_private_syms = findPrivateSyms;
    fn_substitute_sym_to_ptr = symToPtr;
    fn_substitute_interpose_imports = interposeImports;
    substitute_native_available = nativeAvailable;

    return YES;
}

BOOL substitute_available(void) {
    // substitute_available() can be queried concurrently (registry selection
    // and availability introspection), so the probe's cached statics and the
    // published function-pointer globals must be serialized; the publish
    // happens-before any reader that got YES through the same mutex.
    // ponytail: pthread mutex instead of dispatch_once — once-semantics
    // would cache a failed probe forever and break the documented retry
    // contract (engine may appear after HookKit loads); os_unfair_lock
    // needs iOS 10+, above the 9.0 deployment floor (Makefile TARGET).
    static pthread_mutex_t probeMutex = PTHREAD_MUTEX_INITIALIZER;
    static BOOL cached = NO;
    static BOOL available = NO;

    pthread_mutex_lock(&probeMutex);

    if(!cached && probe_substitute()) {
        available = YES;
        cached = YES;
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
BOOL substitute_discoverable(void) {
    NSString *jbPath = HKJBPath(@"/usr/lib/libsubstitute.0.dylib");

    if(!jbPath) {
        return NO;
    }

    return dlopen_preflight([jbPath fileSystemRepresentation]);
}

#pragma mark - HKMSBackend

@implementation HKMSBackend
- (instancetype)initWithHookFunction:(void (*)(void *, void *, void **))hookFunction
                      hookMessageEx:(void (*)(Class, SEL, void *, void **))hookMessageEx
                    getImageByName:(void *(*)(const char *))getImageByName
                       findSymbol:(void *(*)(void *, const char *))findSymbol {
    if((self = [super init])) {
        msHookFunction = hookFunction;
        msHookMessageEx = hookMessageEx;
        msGetImageByName = getImageByName;
        msFindSymbol = findSymbol;
        _lastErrno = 0;
    }

    return self;
}

- (BOOL)batchingSupported {
    return NO;
}

- (BOOL)supportsHookKind:(HKHookKind)kind {
    return kind == HKHookKindMessage || kind == HKHookKindFunction;
}

- (int)lastErrno {
    return _lastErrno;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!class_getInstanceMethod(objcClass, selector) && !class_getClassMethod(objcClass, selector)) {
        _lastErrno = 0;
        return HK_ERR_NOT_SUPPORTED;
    }

    // MSHookMessageEx is void; the only observable success signal is the
    // original IMP being written into the out cell. Always pass a slot WE
    // own (never the caller's directly): a call that produced no original
    // must not publish a NULL cell as success (HKSubstitutor invariant:
    // publish only non-NULL originals). Some Substrate builds also signal
    // failure through errno.
    IMP original = NULL;
    errno = 0;
    msHookMessageEx(objcClass, selector, replacement, (void **)&original);
    _lastErrno = errno;

    if(errno) {
        // Failure signalled through errno: the hook may already be applied.
        return HK_ERR;
    }

    if(!original) {
        // No errno but no original either: nothing observable happened, so
        // the hook was not applied — a capability-style miss, side-effect-
        // free, so callers may switch technique.
        _lastErrno = ENOENT;
        return HK_ERR_NOT_SUPPORTED;
    }

    if(old_ptr) {
        *old_ptr = (void *)original;
    }

    return HK_OK;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    // MSHookFunction is void — success is only observable through the
    // original being written into the out cell (some builds also signal
    // failure through errno). Same contract as hookMessageInClass: pass a
    // slot we own, and never publish a NULL original as success.
    void *original = NULL;
    errno = 0;
    msHookFunction(function, replacement, &original);
    _lastErrno = errno;

    if(errno) {
        // Failure signalled through errno: the hook may already be applied.
        return HK_ERR;
    }

    if(!original) {
        // No errno but no original either: the function was not hooked (too
        // short / bad shape / no writable prologue) — nothing was written,
        // so callers may switch technique.
        _lastErrno = ENOENT;
        return HK_ERR_NOT_SUPPORTED;
    }

    if(old_ptr) {
        *old_ptr = original;
    }

    return HK_OK;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

- (hookkit_status_t)executeHooks:(NSArray<HKHookOperation *> *)hooks {
    // nothing pending: hooks are applied at hook time
    return HK_OK;
}

- (HKImageRef)openImage:(NSString *)path {
    return (HKImageRef)msGetImageByName([path fileSystemRepresentation]);
}

- (void)closeImage:(HKImageRef)image {
    // MSCloseImage is not actually exported by either library at runtime
}

- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName {
    const char *symbol = [symbolName UTF8String];

    if(image) {
        return msFindSymbol((void *)image, symbol);
    }

    // image == NULL: iterate all loaded dyld images
    return hk_search_loaded_images(^void *(const char *image_name) {
        void *imageHandle = msGetImageByName(image_name);
        return imageHandle ? msFindSymbol(imageHandle, symbol) : NULL;
    });
}
@end

#pragma mark - Verified void memory patching

// MSHookMemory / SubHookMemory are void: no status, no errno contract. The
// only honest success signal is the patch being present afterwards, so verify
// by reading the target back and comparing to what was written. The read goes
// through mach_vm_read (not memcmp) so an unreadable target reports
// "unverifiable" instead of faulting the process.
// ponytail: no read-before — the decision-relevant compare is read-after vs
// the expected bytes; a pre-existing-equal region is already the desired
// state, and any mismatch is a failure either way.
static hookkit_status_t hk_verified_memory_patch(void *target, const void *data, size_t size, void (*hookMemory)(void *, const void *, size_t), int *outErrno) {
    if(!target || !data) {
        *outErrno = EINVAL;
        return HK_ERR_INVALID_ARGUMENT;
    }

    if(size == 0) {
        // Nothing to write, nothing to verify: a zero-byte patch has no
        // observable effect, so success cannot be a lie.
        *outErrno = 0;
        return HK_OK;
    }

    hookMemory(target, data, size);

    vm_address_t readback = 0;
    mach_msg_type_number_t readSize = 0;
    kern_return_t kr = vm_read(mach_task_self(), (vm_address_t)target, size, &readback, &readSize);

    if(kr != KERN_SUCCESS || readSize != size || !readback) {
        // Unverifiable: the void API gave no status and the outcome cannot
        // be confirmed. Treat as suspect (HK_ERR, never retry), not success.
        *outErrno = ENODATA;
        return HK_ERR;
    }

    BOOL landed = (memcmp((const void *)readback, data, size) == 0);
    vm_deallocate(mach_task_self(), readback, (vm_size_t)readSize);

    if(!landed) {
        // The patch did not land (or was reverted): the region may be in a
        // partially-patched state, so this is terminal, not retryable.
        *outErrno = EIO;
        return HK_ERR;
    }

    *outErrno = 0;
    return HK_OK;
}

#pragma mark - HKSubstrateBackend

@implementation HKSubstrateBackend
- (instancetype)init {
    return [super initWithHookFunction:substrate_hookFunction hookMessageEx:substrate_hookMessageEx getImageByName:substrate_getImageByName findSymbol:substrate_findSymbol];
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    if(substrate_hookMemory) {
        return hk_verified_memory_patch(target, data, size, substrate_hookMemory, &_lastErrno);
    }

    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}
@end

#pragma mark - Substitute error classification

// Maps a native libsubstitute error code to the hookkit status. Pure: no
// state, just the code table. Shared by hookFunction: and hookMessageInClass:.
//
// Capability misses mean the hook was NOT applied — the function's shape or
// the selector's absence make this technique unusable, which is what
// HK_ERR_NOT_SUPPORTED reports so callers can switch hooking techniques.
//
// Everything else (OOM, VM, NOT_ON_MAIN_THREAD, UNEXPECTED_PC_ON_OTHER_THREAD
// [the hooks were otherwise completed], ADJUSTING_THREADS, or any unknown or
// future code from a newer installed libsubstitute than the vendored header)
// is terminal: the hook may already be applied, so it must never be retried.
// The default fails closed to HK_ERR.
//
// The taxonomy itself lives in Internal/HKSubstituteErrors.c (pure C, shared
// with the host-side unit test) — the mapping here is just its status form.
static hookkit_status_t substitute_error_to_status(int err) {
    switch(hk_substitute_err_classify(err)) {
        case HKSubErrOK:
            return HK_OK;

        case HKSubErrCapabilityMiss:
            return HK_ERR_NOT_SUPPORTED;

        default:
            return HK_ERR;
    }
}

#pragma mark - HKSubstituteBackend

@implementation HKSubstituteBackend
- (instancetype)init {
    return [super initWithHookFunction:substitute_hookFunction hookMessageEx:substitute_hookMessageEx getImageByName:substitute_getImageByName findSymbol:substitute_findSymbol];
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!substitute_native_available) {
        return [super hookMessageInClass:objcClass withSelector:selector withReplacement:replacement outOldPtr:old_ptr];
    }

    if(!class_getInstanceMethod(objcClass, selector) && !class_getClassMethod(objcClass, selector)) {
        _lastErrno = 0;
        return HK_ERR_NOT_SUPPORTED;
    }

    int result = fn_substitute_hook_objc_message(objcClass, selector, replacement, old_ptr, NULL);
    _lastErrno = result;
    return substitute_error_to_status(result);
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!substitute_native_available) {
        return [super hookFunction:function withReplacement:replacement outOldPtr:old_ptr];
    }

    struct substitute_function_hook hook = {
        function, replacement, old_ptr, 0
    };

    int result = fn_substitute_hook_functions(&hook, 1, NULL, 0);
    _lastErrno = result;

    if(result == SUBSTITUTE_OK) {
        return HK_OK;
    }

    // GOT/PLT interposition fallback: on iOS 16.2+ the AMFI/APT policy
    // changes broke executable-code patching, so substitute_hook_functions
    // fails on functions it could otherwise hook. The fallback fires ONLY
    // for the five capability-miss codes — substitute reported it could not
    // patch the function, so nothing was written and interposing is safe.
    // Any other code (OOM, VM, NOT_ON_MAIN_THREAD, UNEXPECTED_PC_ON_OTHER_
    // THREAD, ADJUSTING_THREADS, or unknown/future) means the hook may
    // already be applied — retrying with interposition could double-hook.
    // Same taxonomy as the status mapping above (Internal/HKSubstituteErrors.c).
    if(fn_substitute_interpose_imports && hk_substitute_err_is_retryable(result)) {
        Dl_info info;

#if __has_feature(ptrauth_calls)
        function = ptrauth_strip(function, ptrauth_key_asia);
#endif

        if(dladdr(function, &info) && info.dli_sname) {
            // Fresh zeroed out-cell: substitute may have written old_ptr
            // while preparing the failed inline hook — never reuse a
            // possibly-written cell.
            void *interposedOld = NULL;

            struct substitute_import_hook ih = {
                .name = info.dli_sname,
                .replacement = replacement,
                .old_ptr = &interposedOld,
                .options = 0
            };

            // substitute_interpose_imports requires the handle of the image
            // that IMPORTS the symbol (vendored substitute.h: "@handle
            // handle of the importing library") and dereferences it, so NULL
            // is a latent crash. The function's DEFINING image (dladdr's
            // dli_fname) is the wrong handle: an image does not import its
            // own exports through the GOT, so interposing on it reports
            // SUBSTITUTE_OK while hooking nothing. Iterate the loaded
            // images and interpose on each until one actually imports the
            // symbol — evidenced by interposedOld being written non-NULL.
            // Success is never claimed without that write.
            // ponytail: stops at the first importer (matching the pre-fix
            // single-image scope); iterate every importer if a partial-hook
            // report ever matters.
            int lastInterposeErr = SUBSTITUTE_OK;
            BOOL interposedAny = NO;

            for(uint32_t i = 0; i < _dyld_image_count() && !interposedAny; i++) {
                struct substitute_image *importingImage = fn_substitute_open_image(_dyld_get_image_name(i));

                if(!importingImage) {
                    continue;
                }

                int interposeResult = fn_substitute_interpose_imports(importingImage, &ih, 1, NULL, 0);
                fn_substitute_close_image(importingImage);
                lastInterposeErr = interposeResult;

                if(interposeResult == SUBSTITUTE_OK && interposedOld) {
                    interposedAny = YES;
                }
            }

            _lastErrno = lastInterposeErr;

            if(interposedAny) {
                if(old_ptr) {
                    *old_ptr = interposedOld;
                }

                return HK_OK;
            }

            if(lastInterposeErr != SUBSTITUTE_OK) {
                // An importer was found but its interpose failed: its GOT
                // may be partially rewritten, so this is terminal — never
                // retry with a second interposition.
                return HK_ERR;
            }

            // Every image accepted with nothing rewritten: no loaded image
            // imports the symbol through the GOT. Side-effect-free miss, so
            // callers may switch technique.
            _lastErrno = ENOENT;
            return HK_ERR_NOT_SUPPORTED;
        }
    }

    return substitute_error_to_status(result);
}

- (HKImageRef)openImage:(NSString *)path {
    if(!substitute_native_available) {
        return [super openImage:path];
    }

    return (HKImageRef)fn_substitute_open_image([path fileSystemRepresentation]);
}

- (void)closeImage:(HKImageRef)image {
    if(substitute_native_available && image) {
        fn_substitute_close_image((struct substitute_image *)image);
        return;
    }

    [super closeImage:image];
}

- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName {
    if(!substitute_native_available) {
        return [super findSymbolInImage:image symbolName:symbolName];
    }

    const char *symbol = [symbolName UTF8String];

    if(image) {
        void *sym = NULL;

        if(fn_substitute_find_private_syms((struct substitute_image *)image, &symbol, &sym, 1) == SUBSTITUTE_OK && sym) {
            return fn_substitute_sym_to_ptr((struct substitute_image *)image, (substitute_sym *)sym);
        }

        return NULL;
    }

    // image == NULL: iterate all loaded dyld images
    return hk_search_loaded_images(^void *(const char *image_name) {
        struct substitute_image *subImage = fn_substitute_open_image(image_name);

        if(!subImage) {
            return NULL;
        }

        // the block captures `symbol` as const, so pass a mutable copy
        const char *probe = symbol;
        void *sym = NULL;
        void *result = NULL;

        if(fn_substitute_find_private_syms(subImage, &probe, &sym, 1) == SUBSTITUTE_OK && sym) {
            result = fn_substitute_sym_to_ptr(subImage, (substitute_sym *)sym);
        }

        fn_substitute_close_image(subImage);
        return result;
    });
}

// Substitute backend memory hooking: the MS-compatible shim path resolves
// SubHookMemory on Substitute (the native API has no separate memory hook).
- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    if(substitute_hookMemory) {
        return hk_verified_memory_patch(target, data, size, substitute_hookMemory, &_lastErrno);
    }

    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}
@end