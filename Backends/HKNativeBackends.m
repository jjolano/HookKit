#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <errno.h>
#import <objc/runtime.h>
#import <pthread.h>

#import "native/hk_arm64.h"
#import "native/hk_native.h"
#import "native/hk_swift.h"

#pragma mark - HKNativeBackend

@implementation HKNativeBackend {
    int _lastErrno;
}

- (int)lastErrno {
    return _lastErrno;
}

// The native engine patches executable memory without suspending peer
// threads, so code patching must only happen at load time, on the main
// thread, before the target can run elsewhere (same contract as Substitute).
static BOOL hk_native_ensure_main_thread(void) {
#if TARGET_OS_IPHONE
    if(!pthread_main_np()) {
        return NO;
    }
#endif

    return YES;
}

// Engine failures split into capability misses (the target's shape is outside
// what the engine can safely patch — callers may switch technique) and hard
// errors (the patch was attempted and failed).
static hookkit_status_t hk_native_map_engine_failure(int errnoVal) {
    switch(errnoVal) {
        case HK_NATIVE_ERR_UNSUPPORTED:
        case HK_NATIVE_ERR_SHORT_FUNCTION:
        case HK_NATIVE_ERR_RELOCATE:
        case HK_NATIVE_ERR_UNREADABLE:
            return HK_ERR_NOT_SUPPORTED;

        default:
            return HK_ERR;
    }
}

// Side-effect-free capability preflight for auto-cover routing: mirrors the
// no-write rejections the engine would produce (alignment, self-hook,
// short-function over the actual 4- or 16-byte branch window) plus the
// main-thread gate, so a router can pick this backend without ever invoking a
// hook that would be refused. The engine's own hook path validates through
// hk_native_preflight_function, so a preflight accept and the hook can never
// disagree on the checks they share. All checks read only; a reject leaves
// the target untouched.
- (hookkit_status_t)preflightFunction:(void *)function withReplacement:(void *)replacement {
    if(!hk_native_ensure_main_thread()) {
        _lastErrno = HK_NATIVE_ERR_UNSUPPORTED;
        return HK_ERR_NOT_SUPPORTED;
    }

    int engineErrno = hk_native_preflight_function(function, replacement);

    if(engineErrno != 0) {
        _lastErrno = engineErrno;
        return hk_native_map_engine_failure(engineErrno);
    }

    return HK_OK;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!hk_native_ensure_main_thread()) {
        _lastErrno = HK_NATIVE_ERR_UNSUPPORTED;
        return HK_ERR_NOT_SUPPORTED;
    }

    // C1: the engine publishes the sealed trampoline into *out_orig BEFORE
    // the target is patched (hk_native.c hk_native_hook_function), so hand
    // the caller's cell straight to it — no local staging, no
    // copy-after-activation. Every failure path returns before the publish;
    // a post-publish write failure keeps the (real, executable) trampoline
    // in the cell, which a mid-write crash must not lose.
    if(!hk_native_hook_function(function, replacement, old_ptr)) {
        _lastErrno = hk_native_last_error();
        return hk_native_map_engine_failure(_lastErrno);
    }

    _lastErrno = 0;
    return HK_OK;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    if(!hk_native_ensure_main_thread()) {
        _lastErrno = HK_NATIVE_ERR_UNSUPPORTED;
        return HK_ERR_NOT_SUPPORTED;
    }

    if(!hk_native_patch_memory(target, data, size)) {
        _lastErrno = hk_native_last_error();
        return hk_native_map_engine_failure(_lastErrno);
    }

    _lastErrno = 0;
    return HK_OK;
}

- (HKImageRef)openImage:(NSString *)path {
    return (HKImageRef)hk_native_open_image([path fileSystemRepresentation]);
}

- (void)closeImage:(HKImageRef)image {
    if(image) {
        hk_native_close_image((hk_image *)image);
    }
}

- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName {
    const char *symbol = [symbolName UTF8String];

    if(image) {
        return hk_native_find_symbol((hk_image *)image, symbol);
    }

    // image == NULL: search the default scope, then all loaded dyld images
    void *found = dlsym(RTLD_DEFAULT, symbol);

    if(found) {
        return found;
    }

    return hk_search_loaded_images(^void *(const char *image_name) {
        // Scan variant: no dlopen/dlclose per image — dlsym(RTLD_DEFAULT)
        // already missed above, so the handle's dlsym fallback has nothing
        // unique to add and the per-image dlopen was the scan's dominant cost.
        hk_image *handle = hk_native_open_image_scan(image_name);

        if(!handle) {
            return NULL;
        }

        void *result = hk_native_find_symbol(handle, symbol);
        hk_native_close_image(handle);
        return result;
    });
}
@end

#pragma mark - HKSwiftBackend

@implementation HKSwiftBackend {
    int _lastErrno;
}

- (int)lastErrno {
    return _lastErrno;
}

// Engine errors fall into two buckets: class-shape problems mean the target
// is outside v1 scope (NOT_SUPPORTED); lookup/signing/write problems are
// hard errors (HK_ERR).
- (hookkit_status_t)mapEngineError:(int)code {
    switch(code) {
        case HK_SWIFT_ERR_UNSUPPORTED:
        case HK_SWIFT_ERR_NOT_SWIFT:
        case HK_SWIFT_ERR_NOT_CLASS_DESCRIPTOR:
        case HK_SWIFT_ERR_NO_VTABLE:
        case HK_SWIFT_ERR_UNSUPPORTED_LAYOUT:
            return HK_ERR_NOT_SUPPORTED;

        case HK_SWIFT_ERR_NOT_FOUND:
        case HK_SWIFT_ERR_AMBIGUOUS:
        case HK_SWIFT_ERR_PAC_MISMATCH:
        case HK_SWIFT_ERR_INVALID_INDEX:
        case HK_SWIFT_ERR_ARG:
        case HK_SWIFT_ERR_WRITE:
            return HK_ERR;
    }

    return HK_ERR;
}

- (hookkit_status_t)hookSwiftMethodInClass:(Class)objcClass withName:(NSString *)name withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    // C1: the engine publishes the stripped original into *out_orig BEFORE
    // the slot mutates (hk_swift.c hk_swift_hook_slot), so hand the caller's
    // cell straight to it — no local staging, no copy-after-activation.
    if(!hk_swift_hook_method(objcClass, [name UTF8String], replacement, old_ptr)) {
        _lastErrno = hk_swift_last_error();
        return [self mapEngineError:_lastErrno];
    }

    _lastErrno = 0;
    return HK_OK;
}

- (hookkit_status_t)hookSwiftVtableSlotInClass:(Class)objcClass withIndex:(NSUInteger)index withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    // C1: same contract as hookSwiftMethodInClass: — the original is
    // published before the slot write, so pass the caller's cell directly.
    if(!hk_swift_hook_vtable_slot(objcClass, (uint32_t)index, replacement, old_ptr)) {
        _lastErrno = hk_swift_last_error();
        return [self mapEngineError:_lastErrno];
    }

    _lastErrno = 0;
    return HK_OK;
}
@end
