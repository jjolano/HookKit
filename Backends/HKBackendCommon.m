#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <mach-o/dyld.h>

// The @implementation lives here (not the shared header): the ivars of
// HKHookOperation are @public and referenced from every backend TU through
// the _OBJC_IVAR_$_ symbols, which only the implementing TU emits.
@implementation HKHookOperation
@end

// Jailbreak-root path seam — compile-time per scheme. Each package is built
// for one jailbreak type, so the branch is baked in: rootful = identity (no
// prefix), rootless = libroot's jbrootpath (auto-linked -lroot by theos;
// resolves /var/jb or the jailbreak's own prefix), roothide = libroothide's
// jbroot() (random-named jbroot, no /var/jb).
#ifdef HK_ROOTHIDE
#import <roothide.h>
NSString* HKJBPath(NSString* path) { return jbroot(path); }
#elif defined(HK_ROOTLESS)
#import <rootless.h>
NSString* HKJBPath(NSString* path) { return ROOT_PATH_NS(path); }
#else
NSString* HKJBPath(NSString* path) { return path; }
#endif

// Shared by every backend that scans for a symbol with no image specified.
void *hk_search_loaded_images(void *(^probe)(const char *imageName)) {
    int count = _dyld_image_count();

    for(int i = 0; i < count; i++) {
        const char *image_name = _dyld_get_image_name(i);

        if(!image_name) {
            continue;
        }

        void *found = probe(image_name);

        if(found) {
            return found;
        }
    }

    return NULL;
}

hookkit_status_t hk_batch_status(int okCount, int total) {
    if(okCount < total) {
        NSLog(@"[HookKit] warning: successfully hooked less than expected (%d/%d)", okCount, total);
    }

    if(okCount == total) {
        return HK_OK;
    }

    return okCount > 0 ? HK_ERR_PARTIAL : HK_ERR;
}

// Shared dlfcn image lookup for the backends whose engines bring no image API
// of their own (fishhook, Dobby, Frida) — the three had byte-identical copies.
// Deliberately not <HKSubstitutorBackend>-conforming: that would warn on the
// six hooking methods it has no business implementing. Subclasses declare the
// protocol themselves.
// ponytail: the native backend has this same shape over hk_native_open_image/
// _find_symbol/_close_image; parameterising open/find/close as ivars to absorb
// it costs more lines than the copy does. Revisit if a fifth copy appears.
// Declared here, not just defined below: -Wprotocol resolves a subclass's
// conformance against declared methods, so the inherited trio must be visible
// at the subclass @interface.
@implementation HKDlfcnBackend

- (HKImageRef)openImage:(NSString *)path {
    // RTLD_NOLOAD: inspect-only, never loads the dylib — matches the MS/ElleKit
    // contract that openImage does not load images
    return (HKImageRef)dlopen([path fileSystemRepresentation], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
}

- (void)closeImage:(HKImageRef)image {
    if(image) {
        dlclose((void *)image);
    }
}

- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName {
    const char *symbol = [symbolName UTF8String];

    // dlsym takes the C name; callers may pass the Mach-O symbol-table name
    // (leading underscore, e.g. "_signal"). Strip one leading underscore so a
    // mangled name resolves on dlsym's fast path — otherwise every "_name"
    // lookup misses dlsym AND the O(images) walk below (same wrong name),
    // silently returning NULL: ~160ms wasted per miss and the hook never
    // installs. Unmangled names ("signal") are unaffected. Same convention as
    // HKFishhookBackend's dli_sname handling.
    if(symbol && symbol[0] == '_') {
        symbol++;
    }

    if(image) {
        return dlsym((void *)image, symbol);
    }

    // image == NULL: the default scope already covers every globally-visible
    // image, so one dlsym answers for any exported symbol. The old fallback
    // walked ALL ~600 loaded images doing dlopen(RTLD_NOLOAD)+dlsym+dlclose
    // each — but that only re-searches images RTLD_DEFAULT already covered and
    // STILL cannot find a non-exported (private) symbol, so on a miss it burned
    // ~160ms per call to fail anyway (Shadow's __signal_nobind/__sigaction
    // ctor lookups: ~320ms of pure loss at launch, on top of the underscore
    // miss fixed above). Its one unique case is a symbol exported solely by an
    // RTLD_LOCAL image; a caller needing that resolves the handle and uses
    // findSymbolInImage:<handle>.
    // ponytail: dropped the O(images) fallback; add findSymbolInImage:<handle>
    // at the call site if an RTLD_LOCAL-only symbol ever needs resolving.
    return dlsym(RTLD_DEFAULT, symbol);
}
@end
