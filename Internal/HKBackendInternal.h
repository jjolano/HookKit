// HookKit private header: shared declarations for the facade/registry split.
// Lives under Internal/ at the project root and is #imported only by the
// framework's own .m files — it is NOT under Headers/, so it is never
// installed into the public Headers/ tree (the public list is the fixed
// HookKit_PUBLIC_HEADERS set).
#ifndef hookkit_backend_internal_h
#define hookkit_backend_internal_h

#import <HookKit.h>
#import "HKOriginalPublication.h"

#pragma mark - Hook operations

typedef NS_ENUM(int, HKHookKind) {
    HKHookKindMessage,
    HKHookKindFunction,
    HKHookKindMemory
};

// How a function hook is applied. Set by the facade when an operation is
// resolved/enqueued; the backend executes accordingly and the facade uses it
// for the inline-guard and original-publication decisions.
typedef NS_ENUM(uint8_t, HKFunctionTechnique) {
    HKFunctionTechniqueNone,
    HKFunctionTechniqueRebind,
    HKFunctionTechniqueInline
};

// When the original implementation becomes available relative to activation,
// per function technique. BeforeActivation backends can publish a real
// original into a caller's cell; AfterActivation/Unavailable backends must be
// refused (or used without an original) when the caller requested one.
// Runtime is resolved per call from the backend provider (see
// hk_resolved_publication_policy) — used when the policy depends on a
// detected provider ABI (the combined ElleKit/libhooker Inline entry).
typedef NS_ENUM(uint8_t, HKOriginalPublicationPolicy) {
    HKOriginalPublicationBeforeActivation,
    HKOriginalPublicationAfterActivation,
    HKOriginalPublicationUnavailable,
    HKOriginalPublicationRuntime
};

// The only operation shape that crosses the backend seam. Native batching is
// a function-hook capability (fishhook and Frida); routing, message, memory,
// and guard state stay private to the facade.
@interface HKFunctionBatchItem : NSObject {
@public
    void *function;
    void *replacement;
    hookkit_status_t status;
    HKOriginalPublication original;
    int backendErrno;
}
@end

#pragma mark - Backend protocol

// HKStrategy is public API now (HookKit.h, next to hookkit_cat_t) so that
// activeStrategy is observable; the protocol's setStrategy: below is the
// backend-facing channel that consumes it.
@protocol HKSubstitutorBackend <NSObject>
@property (nonatomic, readonly) int lastErrno;

@optional
- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;
- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size;
// Native function-batch adapters update each item's status/backendErrno.
- (void)executeFunctionHooks:(NSArray<HKFunctionBatchItem *> *)hooks;

- (HKImageRef)openImage:(NSString *)path;
- (void)closeImage:(HKImageRef)image;
- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName;

// Swift vtable hooking. Optional: only the Swift backend implements these;
// the facade rejects the capability for every other backend.
- (hookkit_status_t)hookSwiftMethodInClass:(Class)objcClass withName:(NSString *)name withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;
- (hookkit_status_t)hookSwiftVtableSlotInClass:(Class)objcClass withIndex:(NSUInteger)index withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;
// Technique hint for strategy-aware backends (HKLitehookBackend). Optional:
// backends without it keep their vendor default technique.
- (void)setStrategy:(HKStrategy)strategy;
// Side-effect-free capability preflight (auto-cover routing). Optional: a
// backend that does not implement it is presumed able to attempt any target
// in its descriptor kinds (a preflight that only declines, never verifies —
// HK_OK means "no known veto", not "guaranteed safe"). Backends with real
// prologue preflights (litehook/Dobby/native inline) implement it so the
// auto-cover router can pick the first backend that accepts a target without
// ever invoking a hook that would be rejected mid-flight.
- (hookkit_status_t)preflightFunction:(void *)function withReplacement:(void *)replacement;
@end

#pragma mark - Backend classes

// ElleKit backend: libhooker API, resolved at runtime via dlopen/dlsym.
@interface HKElleKitBackend : NSObject <HKSubstitutorBackend> {
    int _lastErrno;
}
@end

// Shared implementation for backends exposing the Cydia Substrate C API.
// The facade drains batches sequentially because this adapter has no native
// batch primitive.
@interface HKMSBackend : NSObject <HKSubstitutorBackend> {
@protected
    void (*msHookFunction)(void *, void *, void **);
    void *(*msGetImageByName)(const char *);
    void *(*msFindSymbol)(void *, const char *);
    int _lastErrno;
}

- (instancetype)initWithHookFunction:(void (*)(void *, void *, void **))hookFunction
                    getImageByName:(void *(*)(const char *))getImageByName
                       findSymbol:(void *(*)(void *, const char *))findSymbol;
@end

// Cydia Substrate backend: unc0ver and 32-bit jailbreaks.
@interface HKSubstrateBackend : HKMSBackend
@end

// Substitute backend: checkra1n-classic (Substitute-based jailbreaks).
// Uses libsubstitute's native API when available, otherwise the MS-compatible
// path (which also fixes the leak of Substitute image handles).
@interface HKSubstituteBackend : HKMSBackend
@end

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
@interface HKDlfcnBackend : NSObject
- (HKImageRef)openImage:(NSString *)path;
- (void)closeImage:(HKImageRef)image;
- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName;
@end

// fishhook backend: rebind_symbols for C functions, including one native
// function-batch walk; dlsym/dyld iteration for symbol lookup.
@interface HKFishhookBackend : HKDlfcnBackend <HKSubstitutorBackend>
@end

// litehook backend: strategy-aware — GOT/import rebinding via
// litehook_rebind_symbol (address-based, no symbol-name requirement) by
// default, plus memory patching via litehook_hook_memory. With setStrategy:
// the same backend also serves prologue inline trampolines (litehook_hook_function)
// and DSC private-symbol lookups (litehook_find_dsc_symbol), so one vendor
// covers several categories. Compiled in on all archs; batches drain through
// the facade one operation at a time.
// ponytail: litehook_rebind_symbol's kern_return_t carries the failure
// detail (KERN_MEMORY_FAILURE leaves the live rebind list untouched), and the
// zero-match honesty signal comes from its out-param match count, captured
// under the same lock as the apply. litehook commits the global rebind only
// after a first match, so a zero-match hookFunction: registers NOTHING and
// reports HK_ERR_NOT_SUPPORTED (side-effect-free, retryable).
@interface HKLitehookBackend : HKDlfcnBackend <HKSubstitutorBackend> {
    int _lastErrno;
    // zero-init (HKStrategyDefault): a bare [[self class] new] keeps the
    // vendor default until setStrategy: is called
    HKStrategy _strategy;
}
// Read the active technique: lets the facade's inline-ownership guard decide
// whether a litehook hook is an inline writer (strategy == HKStrategyInline)
// or a rebind/memory path (GOT-scoped, never touches the prologue).
- (HKStrategy)strategy;
@end

// Native backend: HookKit's own engine, requiring no hooking library on the
// device. Never selected automatically — callers opt in with HK_LIB_NATIVE.
// See native/hk_native.h for the constraints.
@interface HKNativeBackend : NSObject <HKSubstitutorBackend>
@end

// Swift backend: rewrites Swift class metadata vtable slots via HookKit's own
// engine (native/hk_swift.c). No arch gate — availability is runtime
// (hk_swift_supported() + the Swift 5 runtime check in swift_available()).
// Hooks the class's own methods only, by name or by declaration-order index;
// see native/hk_swift.h for the ABI contract and v1 scope.
@interface HKSwiftBackend : NSObject <HKSubstitutorBackend>
@end

// Dobby backend: inline hooking via the vendored Dobby static library
// (vendor/dobby). Hooks by address, so interior/private C functions work
// (unlike fishhook). Batches drain through the facade one operation at a time.
// arm64/arm64e only — the static lib has no armv7 slice, so the @interface
// stays visible for the registry but the @implementation is arch-gated and
// dobby_available() reports NO on armv7.
@interface HKDobbyBackend : HKDlfcnBackend <HKSubstitutorBackend> {
    int _lastErrno;
}
@end

// Frida backend: inline hooking via frida-gum, loaded at runtime through the
// HKGum.dylib wrapper (vendor/gum/hkgum.c). Function batching is supported via
// gum interceptor transactions; memory patching is absent.
@interface HKFridaBackend : HKDlfcnBackend <HKSubstitutorBackend>
@end

#pragma mark - Shared helpers

// Owned wrapper around one backend image handle. closeImage: closes the raw
// handle and frees this wrapper; use-after-close is outside the interface.
struct HKImage {
    uint32_t magic;
    hookkit_lib_t ownerType;
    void *rawHandle;
};

// Jailbreak-root path (identity on rootful; libroot's jbrootpath on
// rootless; libroothide's jbroot on roothide). Defined in
// Backends/HKBackendCommon.m.
NSString *HKJBPath(NSString *path);

// Iterates the loaded dyld images, calling probe with each image's name until
// it returns non-NULL; NULL when no image matched. Defined in
// Backends/HKBackendCommon.m.
void *hk_search_loaded_images(void *(^probe)(const char *imageName));

// Batch honesty helper: HK_OK when every op succeeded, HK_ERR_PARTIAL when
// some did, HK_ERR when none did. Defined in Backends/HKBackendCommon.m.
hookkit_status_t hk_batch_status(int succeeded, int total);

#pragma mark - Backend availability

// The registry's table entries reference these; each is defined next to the
// dlopen/dlsym resolution it drives, in the backend file for that library:
//   libhooker_available   -> Backends/HKElleKitBackend.m
//   substrate_available   -> Backends/HKMSBackends.m
//   substitute_available  -> Backends/HKMSBackends.m
//   frida_available       -> Backends/HKInlineBackends.m
// (fishhook/litehook/native/dobby/swift predicates are compile-time or
// engine checks with no resolver of their own — they stay in the registry).
//
// The *_available() probes are ACTIVATION: they dlopen the engine and run
// its constructors. Each dlopen-based backend therefore also exposes a
// *_discoverable() sibling — dlopen_preflight on the same jb-root path,
// side-effect-free (never loads, never initializes) — for the availability-
// introspection entry points, which must not activate any provider.
BOOL libhooker_available(void);
BOOL substrate_available(void);
BOOL substitute_available(void);
BOOL frida_available(void);
BOOL libhooker_discoverable(void);
BOOL substrate_discoverable(void);
BOOL substitute_discoverable(void);
BOOL frida_discoverable(void);

// Detected-ABI original-publication policy for the combined ElleKit/libhooker
// backend. Implemented in Backends/HKElleKitBackend.m, where the provider
// classification lives: BeforeActivation for the real-libhooker ABI
// (LHHookFunctions writes the original before the patch activates), 
// AfterActivation for the ElleKit ABI (its patch writer runs first), and the
// safest policy — Unavailable, fail closed — for an unclassified provider.
// Consumed by hk_resolved_publication_policy.
HKOriginalPublicationPolicy hk_ellekit_current_function_policy(void);

#pragma mark - Registry interface

// Backend table + category pickers, owned by HKBackendRegistry.m; the facade
// consumes them for selection, availability reporting and the info dicts.
typedef BOOL (*HKBackendAvailability)(void);

typedef struct {
    hookkit_lib_t type;
    __unsafe_unretained Class backendClass;
    __unsafe_unretained NSString *identifier;
    __unsafe_unretained NSString *name;
    HKBackendAvailability available;     // activation probe
    HKBackendAvailability discoverable;  // side-effect-free presence probe
    BOOL automatic;     // eligible for +defaultBackend
    BOOL selectable;    // appears in consumer settings-style pickers
    // no categories field: membership lives in hk_category_priorities only,
    // so the two can never diverge (see getAvailableCategories)
    hookkit_cat_t kinds;                  // adapter hook-kind mask: function technique bits and HK_CAT_MEMORY
    HKFunctionTechnique defaultTechnique; // technique applied when no picker names one
    // Original-publication policy per function technique, indexed by
    // HKFunctionTechnique (None/Rebind/Inline).
    HKOriginalPublicationPolicy publicationPolicy[3];
} HKBackendDescriptor;

typedef struct {
    hookkit_lib_t type;
    HKStrategy strategy;
    HKFunctionTechnique technique;        // resolved technique for this picker row
} HKCategoryPicker;

typedef struct {
    hookkit_cat_t category;
    HKCategoryPicker order[8];
    size_t count;
} HKCategoryPriority;

extern const HKCategoryPriority hk_category_priorities[];
extern const size_t hk_category_priority_count;

const HKBackendDescriptor *hk_backends(size_t *outCount);
const HKBackendDescriptor *hk_backend_descriptor_for_type(hookkit_lib_t type);
const HKBackendDescriptor *hk_backend_descriptor_for_backend(id<HKSubstitutorBackend> backend);

// Resolves the effective original-publication policy for a backend +
// technique: the descriptor's per-technique policy, except that
// HKOriginalPublicationRuntime — the combined ElleKit/libhooker backend's
// Inline entry, whose policy depends on the detected provider ABI — is
// resolved at call time via hk_ellekit_current_function_policy. Unknown
// backends fail closed to Unavailable. Implemented in HKBackendRegistry.m;
// callable from the facade (HKSubstitutor.m).
HKOriginalPublicationPolicy hk_resolved_publication_policy(hookkit_lib_t backendType, HKFunctionTechnique technique);

#endif
