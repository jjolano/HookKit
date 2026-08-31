#ifndef hookkit_h
#define hookkit_h

#import <Foundation/Foundation.h>

// v1 module surface. HookKitCore/HookKitModule/HookKitHook and the three hook
// carriers are 3.0 translators over the same plan lifecycle HKSubstitutor
// drives -- the v1 link and source surface, with no Modulous plugin loading.
#import <HookKit/Hook.h>
#import <HookKit/Module.h>
#import <HookKit/Core.h>

typedef enum {
    HK_OK = 0,
    HK_ERR = (1 << 0),
    HK_ERR_NOT_SUPPORTED = (1 << 1),
    HK_ERR_INVALID_ARGUMENT = (1 << 2),
    HK_ERR_PARTIAL = (1 << 3)
} hookkit_status_t;

typedef enum {
    HK_LIB_NONE = 0,
    HK_LIB_ELLEKIT = (1 << 0),
    HK_LIB_FISHHOOK = (1 << 1),
    HK_LIB_SUBSTRATE = (1 << 2),
    HK_LIB_SUBSTITUTE = (1 << 3),
    HK_LIB_NATIVE = (1 << 4),
    HK_LIB_DOBBY = (1 << 5),
    HK_LIB_FRIDA = (1 << 6),
    HK_LIB_SWIFT = (1 << 7),
    HK_LIB_LITEHOOK = (1 << 8)
} hookkit_lib_t;

typedef const struct HKImage* HKImageRef;

/*
 * Backend category flags. Categories group backends by hooking capability;
 * callers use substitutorWithCategory: to select the first available backend
 * in that category's priority order, without naming a specific library.
 *
 *   MESSAGE          — ObjC message hooking through the Objective-C runtime;
 *                      facade-native and independent of backend selection
 *   FUNCTION_REBIND  — C function rebinding by exported symbol name (fishhook
 *                      / litehook)
 *   FUNCTION_INLINE  — C function inline hooking by address (ElleKit / Dobby
 *                      / Frida / litehook)
 *   PRIVATE_SYMBOL   — Private symbol lookup in a loaded image (ElleKit /
 *                      Substrate / Substitute / litehook)
 *   MEMORY           — Memory patching (hookMemory:). A hook-kind bit, not a
 *                      selectable category: no substitutorWithCategory:
 *                      backend is chosen by it — it reports the memory kind
 *                      in backend descriptors/type info, so callers can tell
 *                      which backends serve hookMemory:.
 *
 * Values are retained for ABI compatibility. The canonical 3.0 facade returns
 * HK_CAT_NONE from getAvailableCategories.
 */
typedef enum {
    HK_CAT_NONE             = 0,
    HK_CAT_MESSAGE          = (1 << 0),
    HK_CAT_FUNCTION_REBIND  = (1 << 1),
    HK_CAT_FUNCTION_INLINE  = (1 << 2),
    HK_CAT_PRIVATE_SYMBOL   = (1 << 3),
    HK_CAT_MEMORY           = (1 << 4)
} hookkit_cat_t;

/*
 * How a backend resolves and applies function hooks. Set per-instance during
 * category resolution (substitutorWithCategory: /
 * substitutorWithOrderedCategories:): the winning category picker decides it,
 * and activeStrategy reflects it. Most callers never touch this — it is the
 * resolution result, not a request. HKStrategyRebind and HKStrategyInline are
 * hooking techniques; HKStrategyPrivateSymbol is a resolution mode, not a
 * technique — the backend locates the private/DSC symbol first, then falls
 * through to address-based rebinding (HKStrategyRebind's routine) to hook it.
 */
typedef NS_ENUM(NSUInteger, HKStrategy) {
    HKStrategyDefault,       // vendor's single/fallback technique
    HKStrategyRebind,        // GOT/import slot rebinding (clean prologue)
    HKStrategyInline,        // prologue inline trampoline (denyFishHook-immune)
    HKStrategyPrivateSymbol  // resolver mode: private/DSC lookup, hook via rebinding
};

/*
 * HookKit 3.0 compatibility behavior (authoritative): this header preserves
 * the 2.x ABI, and HKSubstitutor translates calls into HookKit 3 plans. Its
 * v1-style discovery contract remains for consumers such as Shadow 3.7.6:
 * every discoverable current engine receives an opaque type bit and metadata
 * row, and setTypes: turns that returned bit into a strict per-instance
 * engine-ID selection. Do not assign meaning to the bit names in this header;
 * obtain an id/type pair from getSubstitutorTypeInfo:. getAvailableBackendIDs
 * plus substitutorWithBackendIDs: is the direct current API. Category routing
 * remains retired; activeStrategy is HKStrategyDefault. No backend plist is
 * read. The historical backend-selection narrative below is retained only to
 * document old source spellings; it does not describe canonical execution.
 */

/*
 * Symbol name convention: names passed to findSymbolInImage:/
 * findSymbolsInImage: are Substrate-style — C symbols carry no leading
 * underscore ("malloc"); C++ mangled names keep their leading underscore.
 * The Substrate/MS and fishhook backends pass names through unchanged;
 * ElleKit accepts both forms.
 *
 * Threading: configure first — settle types / initLibraries and the batching
 * mode before any hook call. Backend selection is one-shot: the first
 * successful resolution wins, and later calls are no-ops. Hooks install on
 * exactly one thread, normally the main/load thread. Enqueueing may happen
 * from multiple threads (the batch queue is thread-safe — enqueue may race
 * executeHooks, which drains a snapshot under the same lock, so every queued
 * hook runs exactly once), but only as long as exactly one thread calls
 * executeHooks — concurrent executeHooks calls are not serialized, so do not
 * issue them from more than one thread. Not synchronized: last-error state —
 * getLibErrno: reports the last hook call's per-backend detail; read it
 * immediately on the same thread that made the call, not cross-thread.
 *
 * Main-thread-only operations: the native Substitute API; the native engine's
 * function and memory patching; and the Dobby and litehook inline techniques.
 * None of them suspend peer threads and all patch a prologue window without
 * atomicity, so off the main thread each returns HK_ERR_NOT_SUPPORTED without
 * writing anything — which, on an implicit or auto-cover route, falls through
 * to the next candidate rather than failing the hook. ElleKit, Substrate,
 * Substitute's MS-compatible path and Frida are not gated: they ship their own
 * production relocators, so an off-main-thread inline hook is better served by
 * reaching them. The Swift vtable engine is not gated either — its slot write
 * is a single-copy-atomic pointer store, not a code patch.
 * Image handles are ordinary owned handles: use them on one thread, close
 * each exactly once, and never use one after closeImage: returns.
 *
 * Batch storage lifetime: while batching, enqueue-time HK_OK means "accepted
 * into the queue", not "installed" — only executeHooks installs and writes
 * old_ptr. The caller's old_ptr storage is borrowed: it must stay alive until
 * executeHooks returns, and is never retained past it. Disabling batching
 * while operations are queued leaves them queued — they still run at the next
 * executeHooks — while new hook calls execute immediately. Queue-always
 * batching (H2): while batching is on, EVERY supported hook kind is queued —
 * whether or not the backend has a native batch primitive — and no hook
 * applies before executeHooks. Backends with a native batch primitive apply
 * the drained batch at once (a Frida transaction is an atomic publication of
 * the batch, not a rollback on partial failure); backends without one are
 * applied one op at a time, in submission order, at executeHooks.
 *
 * Availability: getAvailableSubstitutorTypes / getAvailableCategories /
 * getSubstitutorTypeInfo: report DISCOVERY, not activation — the dlopen-based
 * jailbreak providers (ElleKit, Substrate, Substitute, Frida) are probed
 * side-effect-free via dlopen_preflight (never mapped, constructors never
 * run) and the remaining backends through their own side-effect-free checks.
 * A "discoverable" result therefore means the provider is present, not that
 * it is verified activatable; engines are actually dlopen'd and initialized
 * only when a backend is selected and used (initLibraries /
 * defaultSubstitutor / the auto-cover path), which runs the full activation
 * probes. Those activation probes cache per-process results at the first
 * probe — the ElleKit, Substrate, Substitute and Frida probes cache only
 * positive results (a failed dlopen is retried on a later probe), while the
 * Swift probe caches both success and failure; the discovery probes are
 * deliberately uncached.
 *
 * Error reporting: getLibErrno: is an OPAQUE backend-specific code, not a
 * normalized error enum — do not rely on cross-backend meaning. For ElleKit it
 * can be libhooker's errno (from LHHookFunctions / LHPatchMemory), for the
 * Substrate/MS APIs it reflects errno
 * observed at submit time (those entry points are void, so success is
 * unverifiable), for native/Dobby/Frida it can be a mach/driver return, and
 * private negative codes are native/Swift engine errors. A rebinding hook
 * that applies to future image loads can report success while no current
 * symbol exists; backends that refuse this return HK_ERR_NOT_SUPPORTED.
 * HK_ERR_PARTIAL from executeHooks / findSymbolsInImage: means some-but-not-
 * all succeeded: executeHooks writes old_ptr only for succeeded operations,
 * and findSymbolsInImage: leaves NULL entries for misses.
 *
 * HK_ERR_NOT_SUPPORTED contract: the code means "not installed — the target
 * is outside this technique's capability, nothing was written, and the
 * caller's old_ptr is untouched", so callers may retry with another
 * technique. Automatic HookKit instances perform that retry internally.
 * Backends that cannot promise a side-effect-free failure report HK_ERR
 * instead (a failed invocation may have applied). Per backend:
 * fishhook returns it when a symbol is exported but no loaded image
 *  references it, and unregisters the rebinding so nothing applies to future
 *  image loads (rebind_symbols_unbind); substitute maps the capability-miss
 *  codes (FUNC_TOO_SHORT / BAD_INSN_AT_START / CALLS_AT_START /
 *  JUMPS_TO_START / OUT_OF_RANGE / NO_SUCH_SELECTOR) through
 *  substitute_error_to_status, which classifies the native Substitute result
 *  as-is — a capability miss stays HK_ERR_NOT_SUPPORTED with no technique
 *  change behind the descriptor (the old GOT/PLT interpose fallback is gone);
 *  callers that need interposition use auto-cover with an explicit rebind
 *  category; the native engine
 *  maps its UNSUPPORTED / SHORT_FUNCTION / RELOCATE failures the same way;
 * the preflight rejections below use it too. Everything else — OOM, VM,
 * NOT_ON_MAIN_THREAD, UNEXPECTED_PC_ON_OTHER_THREAD, unknown/future codes —
 * is HK_ERR: the hook may already be applied, so it must never be retried.
 *
 * Fail-closed prologue preflight: before any inline code patch, the litehook
 * (20-byte overwrite), Dobby (16-byte) and native backends reject — with
 * HK_ERR_NOT_SUPPORTED and nothing written — a target whose prologue is
 * misaligned, a self-hook, a function that ends inside the overwrite window
 * (an early RET/RETAA/RETAB/BR/BRAAZ/BRABZ or unconditional B), or a literal
 * load / ADR(ADRP) in the window (Dobby/litehook; the native engine's own
 * checks). Too-small functions and literal-load prologues are refused, not
 * smashed. The checks read only the window and never write, so a reject
 * leaves the target intact; they are check-then-act — another thread could
 * mutate the window between scan and patch, an accepted ceiling, as only
 * Substitute suspends threads. Hooks must still be installed at load time,
 * before the target can run elsewhere: a thread already executing inside a
 * prologue when it is patched cannot be rescued by any engine here. (The
 * native engine does make the ENTRY patch a single atomic store where it can,
 * so a thread merely *entering* the target mid-install is safe; see
 * src/native/hk_native.h.)
 *
 * Fail-soft on trap stubs: the dyld shared cache builds dyld's private APIs
 * (dyld_image_get_installname and friends) as stubs whose entry instruction
 * raises SIGTRAP when executed — they exist for symbolication, not for
 * calling. Dispatching one to a hooking backend crashes the process (SIGTRAP)
 * instead of returning an error, so hookFunction: detects the trap entry
 * before any backend runs and returns HK_ERR: the target is permanently
 * unhookable (there is no real "original" to chain to), so retrying is
 * pointless. Both the auto-cover and the direct path are guarded.
 *
 * Arch gates: litehook's inline technique is arm64/arm64e-only — its
 * trampoline emits AArch64 opcodes — so setStrategy: refuses HKStrategyInline
 * on 32-bit, hookFunction:'s inline branch refuses, and the registry's
 * HK_CAT_FUNCTION_INLINE picker drops the litehook row on armv7. litehook
 * rebind and memory-patch remain available on all archs. litehook rebind
 * excludes the image that defines the replacement (calls from that image are
 * not rebound — see README's litehook caveat).
 *
 * Per-backend caveats — fishhook symbol-only rebinding; native/Dobby/Frida
 * codesigning, arch and load-time constraints; Swift vtable scope and calling
 * convention — live in the "Semantics" section of README.md, the canonical
 * copy.
 */

@interface HKSubstitutor : NSObject
@property (assign, nonatomic) hookkit_lib_t types;
@property (assign, nonatomic) BOOL batching;

// The opaque v1 type token selected from getSubstitutorTypeInfo:, or
// HK_LIB_NONE for automatic and raw-ID instances.
@property (readonly, nonatomic) hookkit_lib_t activeType;

// Always HKStrategyDefault in the canonical 3.0 compatibility facade.
@property (readonly, nonatomic) HKStrategy activeStrategy;

// Compatibility no-op. setTypes: resolves the v1 ID/type pair; HookKit runtime
// creation still happens only for an operation.
- (void)initLibraries;

// Dynamically discoverable v1 type tokens: one for every current engine
// returned by getAvailableBackendIDs. The bit values are opaque; obtain the
// matching id/name/type row from getSubstitutorTypeInfo:.
+ (hookkit_lib_t)getAvailableSubstitutorTypes;

// Always HK_CAT_NONE; legacy category routing is retired.
+ (hookkit_cat_t)getAvailableCategories;

// Returns dynamically discovered v1 picker dictionaries. Each has id, name,
// type, and selectable keys. The type is valid only for this enumeration.
+ (NSArray<NSDictionary *> *)getSubstitutorTypeInfo:(hookkit_lib_t)types;

// Returns currently discoverable HookKit 3 backend engine IDs, in the same
// routing order the runtime will use. Pass these exact engine IDs to
// substitutorWithBackendIDs:. The v1 methods above expose the same engines as
// id/type picker rows for unmodified legacy consumers.
+ (NSArray<NSString *> *)getAvailableBackendIDs;

// Creates a substitutor with a strict, per-instance backend override. Pass
// IDs returned by getAvailableBackendIDs in preferred order; only those
// function/memory engines may route this substitutor's hooks. An empty or
// invalid list deliberately leaves no function/memory route. ObjC message
// hooks remain facade-native and are unaffected. No plist configuration is
// involved.
+ (instancetype)substitutorWithBackendIDs:(NSArray<NSString *> *)backendIDs;

// Creates an instance using v1 type tokens returned by
// getSubstitutorTypeInfo:. The named HK_LIB_* constants are retained for ABI,
// not stable current-backend selection; use backend IDs for new code.
+ (instancetype)substitutorWithTypes:(hookkit_lib_t)types;

// Creates an instance of HKSubstitutor with the given substitutor types tried
// in the given priority order — the first available entry wins, regardless of
// the built-in table order. Each element is an NSNumber wrapping a
// hookkit_lib_t. Unknown types are skipped; an empty array yields no backend.
+ (instancetype)substitutorWithOrderedTypes:(NSArray<NSNumber *> *)types;

// Creates an instance of HKSubstitutor for the given backend categories, tried
// in the given priority order — HK_CAT_MESSAGE resolves directly in the
// facade; another category selects its first available built-in picker. Each
// element is an NSNumber wrapping a hookkit_cat_t. HK_CAT_NONE entries are
// skipped; an empty array yields no backend (activeType == HK_LIB_NONE).
+ (instancetype)substitutorWithOrderedCategories:(NSArray<NSNumber *> *)categories;

// Auto-cover routing mode: creates an instance of HKSubstitutor that routes each
// function hook to the first available backend in the given categories whose
// side-effect-free preflight accepts the target. Each element is an NSNumber
// wrapping a hookkit_cat_t; within a category the built-in picker priority
// applies, and categories are tried in the given order. When every picker
// declines the target, the hook returns HK_ERR_NOT_SUPPORTED without invoking
// any backend (fail closed, nothing written). Backends without a preflight are
// presumed to accept. An invoked backend returning HK_ERR_NOT_SUPPORTED is
// skipped because that status guarantees nothing was written; every other
// result is terminal. Requesting old_ptr skips candidates that cannot publish
// the original before activation. Batched routes retry in grouped backend/
// technique waves; rebind-only batches first resolve against the image set at
// drain time.
// Image/symbol APIs use the first available category backend pinned at init;
// activeType reflects that pinned fallback, not a per-hook winner. Coverage
// note: a function too small for an
// inline patch is often still rebindable — fishhook and litehook rebind a
// GOT-referenced export regardless of body size — so
// @[@(HK_CAT_FUNCTION_INLINE), @(HK_CAT_FUNCTION_REBIND)] is the recommended
// composition.
+ (instancetype)substitutorWithAutoCoverCategories:(NSArray<NSNumber *> *)categories;

// Creates an instance of HKSubstitutor for the given backend category. The
// first available backend in that category's built-in priority order is
// selected — callers request a capability, not a specific library. Returns
// an instance with no backend (activeType == HK_LIB_NONE) if no backend in
// the category is available. Convenience for substitutorWithOrderedCategories:
// with a single entry; HK_CAT_NONE is skipped and yields no backend.
// HK_CAT_MESSAGE is facade-native: its instance has activeType == HK_LIB_NONE
// and supports message hooks without loading an adapter.
+ (instancetype)substitutorWithCategory:(hookkit_cat_t)category;

// Creates the process-wide automatic substitutor. Message hooks use the ObjC
// runtime path; function hooks walk FUNCTION_INLINE then FUNCTION_REBIND;
// memory hooks walk capable backend descriptors. Only a side-effect-free
// HK_ERR_NOT_SUPPORTED result advances to the next candidate.
+ (instancetype)defaultSubstitutor;

// For the three batchable methods below: without batching, the return value is
// final. With batching, HK_OK means queued; executeHooks reports installation.
//
// Facade-native Objective-C runtime hook; never dispatches through the selected
// backend. Class-method-only selectors are
// dispatched through the metaclass (object_getClass()), so class methods hook
// correctly on every backend; instance methods pass the class as-is.
- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;

// Hook method for C functions.
- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;

// Hook method for memory patching.
- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size;

// Hook a Swift class method by name (Swift backend only; HK_ERR_NOT_SUPPORTED
// on any other backend). `name` semantics:
//   - "$s..." or "_$s..."  — exact match against the method's mangled symbol
//     name (dladdr dli_sname; the leading underscore is stripped before
//     comparison)
//   - anything else        — case-sensitive substring match against the
//     demangled name (e.g. "viewDidLoad" matches
//     "MyApp.ViewController.viewDidLoad()")
// The match must be unique: zero matches returns HK_ERR (errno
// HK_SWIFT_ERR_NOT_FOUND), more than one returns HK_ERR (HK_SWIFT_ERR_AMBIGUOUS)
// with every candidate logged — never a silent first match. The replacement
// must be a raw function pointer with the Swift calling convention (self in
// x20, heap context in x21 on arm64) — an objc_msgSend-convention IMP will
// misbehave. On success *old_ptr receives the original implementation as an
// unsigned code pointer (same calling convention). v1 scope: the class's own
// methods only, non-generic classes, no resilient superclass, no async
// methods, no class methods, no extensions; @objc dynamic methods are
// hookable (affects Swift callers only).
- (hookkit_status_t)hookSwiftMethodInClass:(Class)objcClass withName:(NSString *)name withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;

// Hook a Swift class method by vtable slot index (Swift backend only;
// HK_ERR_NOT_SUPPORTED on any other backend). The index is the method's
// declaration order within the class (slot i <-> method descriptor i), which
// is stable per build and survives symbol stripping — use this API for
// stripped binaries, where name lookup cannot work. Out-of-range indexes
// return HK_ERR (HK_SWIFT_ERR_INVALID_INDEX). Replacement contract and v1
// scope are identical to hookSwiftMethodInClass:withName:.
- (hookkit_status_t)hookSwiftVtableSlotInClass:(Class)objcClass withIndex:(NSUInteger)index withReplacement:(void *)replacement outOldPtr:(void **)old_ptr;

// Returns an owned opaque image handle, or NULL. Handles are single-threaded,
// must be closed exactly once, and are invalid after closeImage:.
- (HKImageRef)openImage:(NSString *)path;

// Closes the image handle from openImage. The handle is invalid afterwards.
- (void)closeImage:(HKImageRef)image;

// Locates private symbols within a given image, and outputs results to outSymbols (missing symbols are NULL entries). image == NULL is supported if the hooking library implements MSFindSymbol. Returns HK_OK if all symbols were found, HK_ERR_PARTIAL if some, HK_ERR if none.
- (hookkit_status_t)findSymbolsInImage:(HKImageRef)image symbolNames:(NSArray<NSString *> *)symbolNames outSymbols:(NSArray<NSValue *> **)outSymbols;

// Just like findSymbolsInImage, but for one symbol. Returns the symbol address,
// or NULL if not found. image == NULL is fast: exported symbols resolve via
// dlsym (microseconds), then private symbols via the dyld shared cache's
// local-symbols table (one scan over the loaded cache dylibs' already-mapped
// nlist ranges — no per-image walk), and only a symbol no loaded image carries
// falls through to the backend's own lookup.
- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName;

// Installs every hook queued while batching was enabled (queue-always: every
// supported kind is queued while batching is on, whether or not the backend
// has a native batch primitive). Drains the queue. Returns HK_OK if all
// operations succeeded, HK_ERR_PARTIAL if some did, HK_ERR if all failed;
// old_ptr cells are written only for the operations that succeeded.
// Per-operation statuses are not exposed; integrators must verify or disable
// dependent features after HK_ERR_PARTIAL.
// Concurrent calls are not serialized — call from one thread.
- (hookkit_status_t)executeHooks;

// Returns backend-specific error detail from the last hook call that failed
// with a backend error (cleared to 0 on success, on argument errors, and on
// unsupported operations). The
// value is opaque — backend-specific, see the Error reporting paragraph in
// the header comment. If outType is non-NULL it receives the backend the
// code came from (HK_LIB_NONE when cleared). Read it immediately, on the
// same thread that made the call.
- (int)getLibErrno:(hookkit_lib_t *)outType;
@end

// C-style macros for convenience
#ifndef HK_SUBSTITUTOR
#define HK_SUBSTITUTOR [HKSubstitutor defaultSubstitutor]
#endif

#define HKEnableBatching()  [HK_SUBSTITUTOR setBatching:YES]
#define HKDisableBatching() [HK_SUBSTITUTOR setBatching:NO]
#define HKExecuteBatch()    [HK_SUBSTITUTOR executeHooks]

#define HKHookFunction(_symbol, _replace, _result)  [HK_SUBSTITUTOR hookFunction:_symbol withReplacement:_replace outOldPtr:_result]
#define HKHookMemory(_target, _data, _size)         [HK_SUBSTITUTOR hookMemory:_target withData:_data size:_size]
#define HKHookMessage(_class, _sel, _imp, _result)  [HK_SUBSTITUTOR hookMessageInClass:_class withSelector:_sel withReplacement:_imp outOldPtr:(void **)_result]
#define HKHookSwiftMethod(_class, _name, _replace, _result) [HK_SUBSTITUTOR hookSwiftMethodInClass:_class withName:_name withReplacement:_replace outOldPtr:(void **)_result]
#define HKHookSwiftSlot(_class, _index, _replace, _result)  [HK_SUBSTITUTOR hookSwiftVtableSlotInClass:_class withIndex:_index withReplacement:_replace outOldPtr:(void **)_result]

#define HKOpenImage(_path)          (void *)[HK_SUBSTITUTOR openImage:@(_path)]
#define HKCloseImage(_image)        [HK_SUBSTITUTOR closeImage:(HKImageRef)_image]
#define HKFindSymbol(_image, _sym)  [HK_SUBSTITUTOR findSymbolInImage:(HKImageRef)_image symbolName:@(_sym)]
#endif
