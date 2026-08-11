#import "Internal/HKBackendInternal.h"
#import "Internal/HKInlinePreflight.h"

#import <dlfcn.h>
#import <errno.h>
#import <pthread.h>

#include <stdint.h>

#import "native/hk_arm64.h"

// Dobby: vendored static lib with arm64/arm64e slices only; the header is
// plain C and safe to include, but the backend class below is arch-gated too
// so armv7 builds never reference DobbyHook/DobbyCodePatch at link time.
#if defined(__arm64__) || defined(__arm64e__)
#include "dobby/dobby.h"
#endif

#pragma mark - HKDobbyBackend

#if defined(__arm64__) || defined(__arm64e__)
@implementation HKDobbyBackend
- (int)lastErrno {
    return _lastErrno;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

- (hookkit_status_t)hk_dobby_inline_preflight:(void *)function replacement:(void *)replacement {
    // Fail closed before DobbyHook: Dobby's relocator neither rejects short
    // functions (it reads its 12-16 byte overwrite window without recognizing
    // early exits, smashing whatever follows) nor handles literal loads (it
    // UNIMPLEMENTED()s on some LDR-literal encodings and mishandles SIMD
    // literal loads). The checks read only the overwrite window and never
    // write, so a reject leaves the target untouched. Shared with the
    // litehook backend and with Dobby's own hook path (see
    // Internal/HKInlinePreflight.h), so preflight agrees exactly with
    // execution.
    return hk_inline_preflight(function, replacement, HK_INLINE_PREFLIGHT_DOBBY_WINDOW, &_lastErrno);
}

- (hookkit_status_t)preflightFunction:(void *)function withReplacement:(void *)replacement {
    return [self hk_dobby_inline_preflight:function replacement:replacement];
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    _lastErrno = 0;

    hookkit_status_t preflight = [self hk_dobby_inline_preflight:function replacement:replacement];

    if(preflight != HK_OK) {
        // Refused before any write: the target's prologue cannot be
        // overwritten safely (see hk_dobby_inline_preflight).
        return preflight;
    }

    // DobbyHook returns 0 on success; -1 on failure (null address, already
    // hooked, or routing error). Hooks by address — no exported-symbol check.
    // A vendor -1 stays HK_ERR: mutation may already have occurred.
    void *orig = NULL;
    int result = DobbyHook(function, replacement, &orig);
    _lastErrno = result;

    if(result != 0) {
        return HK_ERR;
    }

    if(old_ptr) {
        *old_ptr = orig;
    }

    return HK_OK;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    // DobbyCodePatch takes a uint32_t size: a patch over the cap would be
    // silently truncated — refuse BEFORE calling Dobby (side-effect-free).
    if(size > UINT32_MAX) {
        _lastErrno = EOVERFLOW;
        return HK_ERR_NOT_SUPPORTED;
    }

    // DobbyCodePatch returns 0 on success; -1 on failure (invalid arguments
    // or a mach vm_protect error).
    int result = DobbyCodePatch(target, (uint8_t *)data, (uint32_t)size);
    _lastErrno = result;
    return result == 0 ? HK_OK : HK_ERR;
}

- (void)executeHooks:(NSArray<HKHookOperation *> *)hooks {
    // Unreachable from the facade (nativeBatch = NO: the drain runs ops
    // per-op through executeOperation:onBackend:). Defensive honesty: an op
    // reaching here was never installed, so it must not report success — the
    // facade's finish restores any begun cell for NOT_SUPPORTED.
    for(HKHookOperation *hook in hooks) {
        hook->status = HK_ERR_NOT_SUPPORTED;
    }
}
@end
#else   // !arm64: stub — the class symbol must exist for the registry entry,
        // but dobby_available() is NO on armv7 so this is never instantiated.
@implementation HKDobbyBackend
- (int)lastErrno {
    return _lastErrno;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

- (void)executeHooks:(NSArray<HKHookOperation *> *)hooks {
    // Unreachable from the facade (nativeBatch = NO, and dobby_available()
    // is NO on armv7 so this class is never instantiated). Defensive
    // honesty: an op reaching here was never installed.
    for(HKHookOperation *hook in hooks) {
        hook->status = HK_ERR_NOT_SUPPORTED;
    }
}
// image methods inherited from HKDlfcnBackend: they were NULL stubs here, but
// dobby_available() is NO on armv7 so this class is never instantiated — the
// stub existed only so the class symbol resolves for the registry entry.
@end
#endif

#pragma mark - Frida (HKGum) runtime resolution

// Frida hooks through the HKGum.dylib wrapper, dlopen'd at runtime (path
// resolved via HKJBPath, same pattern as libhooker/libsubstitute): the framework never
// links frida-gum directly. No arch guard — everything is runtime dlopen, and
// on armv7 dlopen simply fails (the wrapper product is arch-gated in the
// Makefile). Theos forces the arm64e slice minos to 14.0, but the arm64 slice
// keeps the deployment floor (9.0/12.0), so HKGum.dylib loads on iOS 12/13 on
// arm64 devices; only on arm64e does dyld refuse below iOS 14. dlopen failure
// is the whole gate (verified: built HKGum arm64 slice carries minos 12.0).
static void *hkgum_handle = NULL;
static int (*fn_hkgum_hook_function)(void *, void *, void **) = NULL;
static void (*fn_hkgum_begin_transaction)(void) = NULL;
static void (*fn_hkgum_end_transaction)(void) = NULL;

// Frida is available when the HKGum.dylib wrapper dlopens AND the full
// required symbol set resolves (see the resolution block above). Only
// successful probes are cached: on dlopen failure or an ABI-incomplete dylib
// the handle is closed and nothing is cached, so a later probe retries.
// The function-pointer globals are published only after the complete symbol
// set is validated, so a partial probe never leaves half-initialized state
// for hook paths to trip over. Single probe attempt, no caching and not
// thread-safe on its own — frida_available serializes callers.
static BOOL frida_probe(void) {
    NSString *jbPath = HKJBPath(@"/usr/lib/HKGum.dylib");

    if(!jbPath) {
        return NO;
    }

    void *handle = dlopen([jbPath fileSystemRepresentation], RTLD_LAZY);

    if(!handle) {
        return NO;
    }

    int (*hookFunction)(void *, void *, void **) = (int (*)(void *, void *, void **))dlsym(handle, "hkgum_hook_function");
    void (*beginTransaction)(void) = (void (*)(void))dlsym(handle, "hkgum_begin_transaction");
    void (*endTransaction)(void) = (void (*)(void))dlsym(handle, "hkgum_end_transaction");

    if(!hookFunction || !beginTransaction || !endTransaction) {
        // ABI-incomplete: not the wrapper we expect. Leave state uncached so
        // a later probe can retry, and don't leave the handle lying around.
        dlclose(handle);
        return NO;
    }

    // Full set resolved: publish the function pointers and cache the success.
    hkgum_handle = handle;
    fn_hkgum_hook_function = hookFunction;
    fn_hkgum_begin_transaction = beginTransaction;
    fn_hkgum_end_transaction = endTransaction;

    return YES;
}

BOOL frida_available(void) {
    // frida_available() can be queried concurrently (registry selection and
    // availability introspection), so the probe's cached statics and the
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

    if(!cached && frida_probe()) {
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
// constructors, so introspection cannot initialize a hooking provider
// (HKGum's constructor calls gum_init_embedded). Deliberately uncached: the
// check is a single stat-family syscall on the preflight path, and an uncached
// probe retries if the engine appears after HookKit loads (mirroring the
// activation probe's retry contract).
BOOL frida_discoverable(void) {
    NSString *jbPath = HKJBPath(@"/usr/lib/HKGum.dylib");

    if(!jbPath) {
        return NO;
    }

    return dlopen_preflight([jbPath fileSystemRepresentation]);
}

#pragma mark - HKFridaBackend

@implementation HKFridaBackend {
    int _lastErrno;
}
- (int)lastErrno {
    return _lastErrno;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    // C1: gum_interceptor_replace publishes the original through the
    // out-param as it stages the replacement, so hand the caller's cell
    // straight to it — no local staging, no copy-after-activation. On
    // failure gum reports the error without publishing an original, and the
    // facade's publish-only-non-NULL invariant catches any vendor that
    // claims success without producing one.
    int result = fn_hkgum_hook_function(function, replacement, old_ptr);
    _lastErrno = result;

    return result == 0 ? HK_OK : HK_ERR;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

// One gum transaction around the whole batch: replacements inside a
// transaction are staged and only published at end_transaction, so the batch
// is applied atomically. frida-gum 17.17 reports no failure at the
// transaction boundary — begin and end are void (frida-gum.h:52094-52095,
// wrappers in hkgum.c) — so there is no begin/commit status to check. The
// only failure channel is the per-hook GumReplaceReturn: a failed replace is
// never staged, so end_transaction never publishes it, and the op stays
// HK_ERR with backendErrno carrying the gum detail — which settles the
// inline guard as tainted at the substitutor layer (HKSubstitutor.m: failed
// op -> HK_ERR -> taint), so no failure is ever claimed successful.
// Individual hook failures fail the batch without a rollback, exactly as
// documented. Message/memory hooks are not supported (ops get
// HK_ERR_NOT_SUPPORTED; the facade's finish restores any begun cell).
- (void)executeHooks:(NSArray<HKHookOperation *> *)hooks {
    int failureErrno = 0;

    fn_hkgum_begin_transaction();

    for(HKHookOperation *hook in hooks) {
        switch(hook->kind) {
            case HKHookKindFunction: {
                void *orig = NULL;
                int result = fn_hkgum_hook_function(hook->function, hook->replacement, &orig);

                if(result == 0) {
                    // C1: publish the original BEFORE end_transaction — the
                    // replacement goes live the moment the transaction ends,
                    // and a reentrant replacement must never observe a NULL
                    // original. The facade already began the publication
                    // (caller cell saved and NULLed), so this write reaches
                    // the caller immediately; the drain's re-publish is
                    // idempotent.
                    hk_original_publish(&hook->original, orig);
                    hook->status = HK_OK;
                } else {
                    hook->status = HK_ERR;
                    hook->backendErrno = result;

                    if(!failureErrno) {
                        failureErrno = result;
                    }
                }

                break;
            }

            case HKHookKindMessage:
            case HKHookKindMemory:
                // not supported
                hook->status = HK_ERR_NOT_SUPPORTED;
                break;
        }
    }

    fn_hkgum_end_transaction();

    _lastErrno = failureErrno;
}
@end