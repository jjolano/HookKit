#import <HookKit/Compat.h>
#import "Internal/HKBackendInternal.h"
#import "Internal/HKInlineGuard.h"
#import "Internal/HKInlinePreflight.h"

#import <objc/runtime.h>
#include <stdlib.h>
#include <pthread.h>

#import "native/hk_swift.h"

// Owned here (resolved by swift_available() via dlsym); consumed by the
// engine's name-based lookup (declared in native/hk_swift.h).
hk_swift_demangle_fn hk_swift_demangle = NULL;

// Side-effect-free discovery for one backend, used by the availability-
// introspection entry points: the dlopen-based jailbreak providers (ElleKit,
// Substrate, Substitute, Frida) report through their preflight-only
// *_discoverable() variants (dlopen_preflight never maps the image and never
// runs its constructors, gum_init_embedded included). The remaining backends
// (fishhook, litehook, native, dobby — compile-time/arch checks; swift — a
// plain system dylib dlsym, never a jailbreak provider) have side-effect-free
// available() probes, so they are reported through those. The real dlopen
// happens only on the actual hook path (initLibraries / defaultBackend /
// auto-cover), which keeps using the full available() probes.
static BOOL hk_backend_discoverable(hookkit_lib_t type) {
    switch(type) {
        case HK_LIB_ELLEKIT:
            return libhooker_discoverable();

        case HK_LIB_SUBSTRATE:
            return substrate_discoverable();

        case HK_LIB_SUBSTITUTE:
            return substitute_discoverable();

        case HK_LIB_FRIDA:
            return frida_discoverable();

        default: {
            size_t count = 0;
            const HKBackendDescriptor *table = hk_backends(&count);

            for(size_t i = 0; i < count; i++) {
                if(table[i].type == type) {
                    return table[i].available();
                }
            }

            return NO;
        }
    }
}

// Shared prologue check for inline-capable backends that bring no
// preflightFunction: of their own (the MS-compatible providers: ElleKit,
// Substrate, Substitute). Two layers, matching Internal/HKInlinePreflight.h:
// the backend-INDEPENDENT basic checks (PAC strip, alignment, self-hook,
// readable+executable replacement and target entry) run on EVERY inline
// dispatch — a misaligned, self-hooked, unmapped or non-executable target
// must fail before any engine is reached, whatever its relocator (M1) — and
// only for a backend whose descriptor sets sharedArm64Preflight (the
// fixed-window relocators: Dobby, litehook) does the full fixed-window
// validator additionally run, with the LARGEST overwrite window any inline
// backend uses (litehook's 20 bytes), so a target that passes here is safe
// for every smaller (16-byte) window too. The strong engines (ElleKit,
// Substrate, Substitute, Frida) bring their own production relocators, which
// decide instruction eligibility themselves: they are basic-checked only.
// Returns YES when the backend may be dispatched, NO to refuse the hook.
// Descriptor-gated (C2/M7): the check only runs on arm64/arm64e — on ARMv7
// Substrate/Substitute validate their own prologues — and only for the
// inline technique of a backend whose descriptor sets sharedArm64Preflight
// (rebind never touches the prologue; backends without the flag skip).
// Backends that implement preflightFunction: guarantee their own checks
// (their hook paths run the same validator), so they are never double-checked
// here.
static BOOL hk_shared_inline_preflight_ok(id<HKSubstitutorBackend> backend, HKFunctionTechnique technique, void *function, void *replacement) {
#if !defined(__arm64__) && !defined(__arm64e__)
    return YES;
#else
    if(technique != HKFunctionTechniqueInline) {
        return YES;
    }

    if([backend respondsToSelector:@selector(preflightFunction:withReplacement:)]) {
        return YES;
    }

    // M1: the generic checks gate EVERY inline-capable dispatch, flagged or
    // not — an engine's own relocator is only ever handed a mapped,
    // executable, aligned, non-self target. Side-effect-free.
    if(hk_inline_preflight_basic(function, replacement, NULL) != HK_OK) {
        return NO;
    }

    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if([backend isKindOfClass:table[i].backendClass]) {
            // Flagged backends are the fixed-window relocators (Dobby,
            // litehook): they additionally get the full window scan. The
            // strong engines are basic-checked only — their own relocators
            // decide instruction eligibility.
            return table[i].sharedArm64Preflight
                ? (hk_inline_preflight(function, replacement, HK_INLINE_PREFLIGHT_LITEHOOK_WINDOW, NULL) == HK_OK)
                : YES;
        }
    }

    // Unknown backend: no shared preflight to enforce (matches the old
    // default — only known inline writers were ever checked).
    return YES;
#endif
}

// Original-publication policy for a backend + resolved technique: resolved at
// runtime via hk_resolved_publication_policy (HKBackendRegistry.m) — the
// descriptor's per-technique policy, with HKOriginalPublicationRuntime
// (the combined ElleKit/libhooker Inline entry) resolved from the detected
// provider ABI. Unknown backends fail closed to Unavailable.

// Shared category walk for the three category-driven paths (ordered-category
// resolution, auto-cover pinning, per-hook auto-cover routing): validates
// every list element is an NSNumber (the lists are caller-supplied — a
// malformed element would raise an uncaught NSInvalidArgumentException at
// unsignedIntegerValue), skips HK_CAT_NONE entries, walks category priorities
// and picker order, and instantiates each available picker backend with its
// strategy before asking `visit` whether it wins. Stops at the first YES and
// returns that backend; nil when every picker declined or no category matched
// (an empty list therefore resolves no backend).
static id<HKSubstitutorBackend> hk_walk_categories(NSArray<NSNumber *> *categories, BOOL (^visit)(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker)) {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(NSNumber *num in categories) {
        if(![num isKindOfClass:[NSNumber class]]) {
            NSLog(@"[HookKit] warning: %s in category list is not an NSNumber; skipped", class_getName([num class]));
            continue;
        }

        hookkit_cat_t want = (hookkit_cat_t)num.unsignedIntegerValue;

        if(!want) {
            // HK_CAT_NONE entry: nothing to resolve, keep looking
            continue;
        }

        for(size_t c = 0; c < hk_category_priority_count; c++) {
            if(hk_category_priorities[c].category & want) {
                for(size_t o = 0; o < hk_category_priorities[c].count; o++) {
                    HKCategoryPicker picker = hk_category_priorities[c].order[o];

                    for(size_t i = 0; i < count; i++) {
                        if(table[i].type != picker.type || !table[i].available()) {
                            continue;
                        }

                        id<HKSubstitutorBackend> candidate = [table[i].backendClass new];

                        if([candidate respondsToSelector:@selector(setStrategy:)]) {
                            [candidate setStrategy:picker.strategy];
                        }

                        if(visit(candidate, picker)) {
                            return candidate;
                        }
                    }
                }
            }
        }
    }

    return nil;
}

// Normalized inline-guard key for a function pointer: strip PAC on arm64e,
// mask the thumb bit on 32-bit ARM. The same key the reservation was made
// under, so a drained op can re-derive its address for the settle without
// re-deriving the pointer the backend consumed (hookFunction stores the
// already-stripped pointer on the op; stripping again is a no-op).
static uintptr_t hk_inline_guard_key(void *function) {
#if __has_feature(ptrauth_calls)
    function = ptrauth_strip(function, ptrauth_key_asia);
#endif
#if defined(__arm__)
    return (uintptr_t)function & ~(uintptr_t)1;
#else
    return (uintptr_t)function;
#endif
}

#pragma mark - Original publication

// Original-publication contract (declared in Internal/HKBackendInternal.h):
// begin/output_cell/publish/capture/finish drive one operation's
// HKOriginalPublication through a drain cycle. The backend never touches the
// struct directly — it writes through the cell hk_original_output_cell
// returns, and the facade drives the rest.
void hk_original_begin(HKOriginalPublication *publication) {
    // requested is NOT reset here: it is fixed at operation construction —
    // the op's original.requested is initialized before the backend call
    // (enqueue build blocks / the immediate-path initializers) and must
    // survive begin so hk_original_output_cell routes the backend's write
    // into the caller's cell for the whole drain cycle.
    if(!publication->callerCell) {
        // No caller cell: nothing to save or NULL-out, just clear the state.
        publication->savedCallerValue = NULL;
        publication->value = NULL;
        publication->capture = NO;
        publication->published = NO;
        return;
    }

    // Save the caller's ACTUAL cell value, then NULL it so the caller never
    // observes a stale original mid-drain. The cell is borrowed (facade
    // contract: valid until executeHooks returns).
    publication->savedCallerValue = *publication->callerCell;
    *publication->callerCell = NULL;
    publication->value = NULL;
    publication->capture = NO;
    publication->published = NO;
}

void **hk_original_output_cell(HKOriginalPublication *publication) {
    // Requested + caller cell: the vendor API writes straight into the
    // caller's cell. Otherwise: internal staging storage never published.
    return (publication->requested && publication->callerCell) ? publication->callerCell : &publication->value;
}

void hk_original_publish(HKOriginalPublication *publication, void *original) {
    publication->value = original;

    if(publication->requested && publication->callerCell) {
        *publication->callerCell = original;
    }

    // published means an ACTUAL original exists; a NULL write is not one
    // (finish's publish-original-before-success invariant relies on this).
    if(original) {
        publication->published = YES;
    }
}

void hk_original_capture(HKOriginalPublication *publication) {
    // A live original is recorded, but publication to the caller is deferred
    // (guard/backup path). Satisfies finish's invariant like published does.
    publication->capture = YES;
}

hookkit_status_t hk_original_finish(HKOriginalPublication *publication, hookkit_status_t status) {
    if(status == HK_ERR_NOT_SUPPORTED) {
        // Backend wrote nothing: undo the begin-NULL so the caller's cell
        // keeps its pre-hook value. Return the status unchanged.
        if(publication->callerCell) {
            *publication->callerCell = publication->savedCallerValue;
        }

        return status;
    }

    if(status == HK_OK && publication->requested && !publication->published && !publication->capture) {
        // The backend claimed success but never produced an original:
        // publish-original-before-success invariant violated.
        return HK_ERR;
    }

    // HK_OK with a real original, or a hard error — unchanged (any
    // already-published original stays: activation may have happened).
    return status;
}

// Central message-hook implementation (C1/L1): resolves the current —
// possibly inherited — IMP for a selector and replaces it via the ObjC
// runtime directly, with the original PUBLISHED before the replace lands.
// Used by both the immediate path and the drained path, so message hooks
// never dispatch to per-backend adapters and every backend gets
// publish-before-activation. Returns HK_OK once the implementation is
// swapped; the publication carries the original.
static hookkit_status_t hk_apply_message_hook(Class objcClass, SEL selector, void *replacement, HKOriginalPublication *publication) {
    Method method = class_getInstanceMethod(objcClass, selector);

    if(!method) {
        // No such method (neither the class nor its ancestors): nothing to
        // hook, nothing written — a capability-style miss.
        return HK_ERR_NOT_SUPPORTED;
    }

    IMP inheritedIMP = method_getImplementation(method);

    if(!inheritedIMP) {
        // A resolved method with no implementation has no original to
        // publish: refuse before writing anything.
        return HK_ERR_NOT_SUPPORTED;
    }

    // C1: publish the pre-read IMP BEFORE the replace lands — the caller's
    // cell (when requested) holds the original from the moment activation
    // happens.
    hk_original_publish(publication, (void *)inheritedIMP);

    // L1: class_replaceMethod returns the IMP it actually replaced when the
    // method lived on the class (authoritative); NULL when a NEW override was
    // added over an inherited method, in which case the pre-read inherited
    // IMP stands. The type encoding is metadata for forwarding only; a
    // method with no encoding gets a minimal placeholder rather than NULL.
    IMP replaced = class_replaceMethod(objcClass, selector, (IMP)replacement, method_getTypeEncoding(method) ?: "@:@");

    if(replaced) {
        hk_original_publish(publication, (void *)replaced);
    }

    return HK_OK;
}

// Deferred idempotent-rehook settlements are gone with the new guard API: a
// duplicate of a PENDING hook is refused at hook time (HK_GUARD_DUP_PENDING),
// and a duplicate of an INSTALLED hook publishes the guard's saved original
// synchronously — no waiter is ever needed.

#pragma mark - HKSubstitutor

@interface HKSubstitutor ()
- (void)noteHookResult:(hookkit_status_t)status fromBackend:(id<HKSubstitutorBackend>)resultBackend;
- (hookkit_lib_t)backendType;
- (BOOL)enqueueKind:(HKHookKind)kind status:(hookkit_status_t *)outStatus build:(void (^)(HKHookOperation *hook))build;
- (hookkit_status_t)executeOperation:(HKHookOperation *)hook onBackend:(id<HKSubstitutorBackend>)resultBackend;
- (BOOL)backendHasNativeBatch:(id<HKSubstitutorBackend>)candidate;
- (HKFunctionTechnique)resolvedTechniqueForBackend:(id<HKSubstitutorBackend>)candidate;
@end

@implementation HKSubstitutor {
    id<HKSubstitutorBackend> backend;
    NSMutableArray<HKHookOperation *> *batchHooks;
    int lastLibErrno;
    hookkit_lib_t lastLibErrnoType;
    // Priority-ordered list of hookkit_lib_t (NSNumber), from substitutorWithOrderedTypes:.
    // Overrides the fixed table priority when non-nil; non-nil-but-empty means no backend.
    NSArray<NSNumber *> *orderedTypes;
    // Priority-ordered list of hookkit_cat_t (NSNumber), from
    // substitutorWithOrderedCategories: (substitutorWithCategory: feeds a
    // single-element list). Tried in order; the first category that resolves
    // to an available backend wins. Non-nil-but-empty means no backend.
    NSArray<NSNumber *> *orderedCategories;
    // Auto-cover routing mode: same shape as orderedCategories, but the
    // backend is chosen PER-HOOK by preflight instead of once at init. The
    // category's pickers are walked in priority order; the first available
    // backend whose side-effect-free preflight accepts the target is invoked
    // exactly once. Set via substitutorWithAutoCoverCategories:.
    NSArray<NSNumber *> *autoCoverCategories;
    // Technique the active backend applies: the winning picker's strategy, or
    // HKStrategyDefault when resolution didn't name one. Zero-init default.
    HKStrategy resolvedStrategy;
}

// activeStrategy is resolvedStrategy: set by the resolution branches in
// initLibraries, readonly on the public surface. `types` is backed by its own
// ivar with an explicit setter that freezes configuration after resolution.
@synthesize types, batching, activeType, activeStrategy = resolvedStrategy;

- (void)setTypes:(hookkit_lib_t)value {
    // Configuration is frozen once a backend has been resolved: mutating the
    // request mask afterwards would leave `types` and `activeType` describing
    // different configurations (initLibraries keeps the original backend).
    // Pre-resolution writes still work, so a failed resolution can be retried
    // with a different request before any backend exists.
    if(!backend) {
        types = value;
    }
}

+ (id<HKSubstitutorBackend>)defaultBackend {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if(table[i].automatic && table[i].available()) {
            return [table[i].backendClass new];
        }
    }

    return nil;
}

- (instancetype)init {
    if((self = [super init])) {
        batchHooks = [NSMutableArray new];
        backend = nil;
        types = HK_LIB_NONE;
        activeType = HK_LIB_NONE;
        lastLibErrno = 0;
        lastLibErrnoType = HK_LIB_NONE;
    }

    return self;
}

- (void)initLibraries {
    if(backend) {
        // idempotent: never re-resolve mid-flight (e.g. engine switching)
        return;
    }

    if(orderedTypes) {
        // an explicitly-supplied list is honoured as given: empty yields no
        // backend rather than falling through to the automatic pick
        size_t count = 0;
        const HKBackendDescriptor *table = hk_backends(&count);

        types = HK_LIB_NONE;
        resolvedStrategy = HKStrategyDefault;

        for(NSNumber *num in orderedTypes) {
            // Trust boundary: the list is caller-supplied. A malformed
            // element (non-NSNumber) would raise an uncaught
            // NSInvalidArgumentException at unsignedIntegerValue — skip it
            // like any other unknown entry.
            if(![num isKindOfClass:[NSNumber class]]) {
                NSLog(@"[HookKit] warning: %s in type list is not an NSNumber; skipped", class_getName([num class]));
                continue;
            }

            for(size_t i = 0; i < count; i++) {
                if(table[i].type == (hookkit_lib_t)num.unsignedIntegerValue && table[i].available()) {
                    backend = [table[i].backendClass new];
                    types |= table[i].type;
                    break;
                }
            }

            if(backend) {
                break;
            }
        }
    } else if(orderedCategories) {
        // Category fallback list: try each category's (backend, strategy)
        // pickers in order; the first category with an available backend wins
        // (its own picker priority decides which). Availability is checked
        // directly (not the automatic flag), so opt-in backends like Native
        // and Frida are still reachable when they are the only option in a
        // category. Shared iterator: same NSNumber validation as every other
        // category-driven path.
        types = HK_LIB_NONE;

        backend = hk_walk_categories(orderedCategories, ^BOOL(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker) {
            resolvedStrategy = picker.strategy;
            types |= [self typeForBackend:candidate];
            return YES;
        });
    } else if(autoCoverCategories) {
        // Auto-cover mode: no single backend is authoritative. Pin the first
        // available category backend as the fallback used by the drained
        // (batched) path and the image APIs; non-batched function hooks route
        // per-hook via preflight instead (see hk_backendForAutoCoverFunction:).
        // The pin records its picker's strategy so batched function ops
        // resolve the ACTUAL technique (e.g. litehook inline), not the
        // vendor default.
        types = HK_LIB_NONE;
        resolvedStrategy = HKStrategyDefault;

        backend = hk_walk_categories(autoCoverCategories, ^BOOL(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker) {
            resolvedStrategy = picker.strategy;
            types |= [self typeForBackend:candidate];
            return YES;
        });
    } else if(types == HK_LIB_NONE) {
        backend = [[self class] defaultBackend];
        resolvedStrategy = HKStrategyDefault;
    } else {
        size_t count = 0;
        const HKBackendDescriptor *table = hk_backends(&count);

        resolvedStrategy = HKStrategyDefault;

        for(size_t i = 0; i < count; i++) {
            if((types & table[i].type) && table[i].available()) {
                backend = [table[i].backendClass new];
                break;
            }
        }
    }
    // explicit types with none available: backend stays nil — the request is
    // honest; the consumer guards with getAvailableSubstitutorTypes

    if(backend) {
        activeType = [self backendType];
    } else {
        activeType = HK_LIB_NONE;
    }
}

- (hookkit_lib_t)typeForBackend:(id<HKSubstitutorBackend>)candidate {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if([candidate isKindOfClass:table[i].backendClass]) {
            return table[i].type;
        }
    }

    return HK_LIB_NONE;
}

// Whether the backend applies a drained batch natively (registry descriptor).
// Backends without a native batch primitive are run per-op, sequentially, via
// executeOperation:onBackend:.
- (BOOL)backendHasNativeBatch:(id<HKSubstitutorBackend>)candidate {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if([candidate isKindOfClass:table[i].backendClass]) {
            return table[i].nativeBatch;
        }
    }

    return NO;
}

// The technique a function op resolves to: the active strategy when it names
// one (rebind/inline), otherwise the backend's default technique. Mirrors the
// category-picker resolution (HKStrategyPrivateSymbol resolves to each
// backend's own default: inline for the providers, rebind for litehook).
- (HKFunctionTechnique)resolvedTechniqueForBackend:(id<HKSubstitutorBackend>)candidate {
    switch(resolvedStrategy) {
        case HKStrategyRebind:
            return HKFunctionTechniqueRebind;

        case HKStrategyInline:
            return HKFunctionTechniqueInline;

        default: {
            size_t count = 0;
            const HKBackendDescriptor *table = hk_backends(&count);

            for(size_t i = 0; i < count; i++) {
                if([candidate isKindOfClass:table[i].backendClass]) {
                    return table[i].defaultTechnique;
                }
            }

            return HKFunctionTechniqueNone;
        }
    }
}

- (hookkit_lib_t)backendType {
    return [self typeForBackend:backend];
}

- (void)noteHookResult:(hookkit_status_t)status {
    // Default attribution: the pinned backend. The auto-cover path passes the
    // routed backend explicitly (see hookFunction:), since routing can pick a
    // different backend than the one pinned at init.
    [self noteHookResult:status fromBackend:backend];
}

- (void)noteHookResult:(hookkit_status_t)status fromBackend:(id<HKSubstitutorBackend>)resultBackend {
    if(status == HK_OK || status == HK_ERR_INVALID_ARGUMENT) {
        // success, or a caller error with no backend-specific detail
        lastLibErrno = 0;
        lastLibErrnoType = HK_LIB_NONE;
    } else if(resultBackend) {
        int backendErrno = [resultBackend lastErrno];

        if(status == HK_ERR_NOT_SUPPORTED && !([resultBackend isKindOfClass:[HKSwiftBackend class]] && backendErrno != 0)) {
            // Generic capability gate: the backend set no meaningful detail,
            // so clear any stale value from an earlier, unrelated call. The
            // Swift backend is the carve-out — it deliberately maps real
            // engine failures (not-a-Swift-class, no vtable, unsupported
            // layout, ...; see HKNativeBackends.m mapEngineError:) onto
            // NOT_SUPPORTED WITH a meaningful errno, and that detail is
            // preserved like the plain error branch.
            lastLibErrno = 0;
            lastLibErrnoType = HK_LIB_NONE;
        } else {
            lastLibErrno = backendErrno;
            lastLibErrnoType = [self typeForBackend:resultBackend];
        }
    } else {
        lastLibErrno = 0;
        lastLibErrnoType = HK_LIB_NONE;
    }
}

+ (hookkit_lib_t)getAvailableSubstitutorTypes {
    hookkit_lib_t types = HK_LIB_NONE;
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        // Introspection must not ACTIVATE engines: only side-effect-free
        // discovery is consulted here (see hk_backend_discoverable) — never
        // the available() probes, which dlopen providers and run their
        // constructors (gum_init_embedded can execute inside the host
        // process). Backends without a discoverable variant are reported
        // unavailable on the safe path; the real dlopen happens only on the
        // actual hook path (initLibraries, defaultBackend, auto-cover),
        // which keeps using the full available() probes.
        if(hk_backend_discoverable(table[i].type)) {
            types |= table[i].type;
        }
    }

    return types;
}

+ (hookkit_cat_t)getAvailableCategories {
    hookkit_cat_t cats = HK_CAT_NONE;
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    // hk_category_priorities is the single source of truth for category
    // membership: a category is available when any of its pickers maps to an
    // available backend — the same lookup initLibraries performs. Same
    // discovery-vs-activation rule as getAvailableSubstitutorTypes: only
    // side-effect-free discovery is consulted (see hk_backend_discoverable),
    // never the available() probes that dlopen and initialize providers.
    for(size_t c = 0; c < hk_category_priority_count; c++) {
        for(size_t o = 0; o < hk_category_priorities[c].count; o++) {
            for(size_t i = 0; i < count; i++) {
                if(table[i].type != hk_category_priorities[c].order[o].type) {
                    continue;
                }

                if(hk_backend_discoverable(table[i].type)) {
                    cats |= hk_category_priorities[c].category;
                    break;
                }
            }
        }
    }

    return cats;
}

+ (NSArray<NSDictionary *> *)getSubstitutorTypeInfo:(hookkit_lib_t)types {
    NSMutableArray *result = [NSMutableArray new];
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    // Same discovery-vs-activation rule as the other introspection entry
    // points: side-effect-free discovery only — the available() probes dlopen
    // providers and must not run from a "get info" query.
    for(size_t i = 0; i < count; i++) {
        if((types & table[i].type) && hk_backend_discoverable(table[i].type)) {
            [result addObject:@{
                @"id" : table[i].identifier,
                @"name" : table[i].name,
                @"type" : @(table[i].type),
                @"selectable" : @(table[i].selectable)
            }];
        }
    }

    return [result copy];
}

+ (instancetype)substitutorWithTypes:(hookkit_lib_t)types {
    HKSubstitutor *substitutor = [self new];
    [substitutor setTypes:types];
    [substitutor initLibraries];
    return substitutor;
}

+ (instancetype)substitutorWithOrderedTypes:(NSArray<NSNumber *> *)types {
    HKSubstitutor *substitutor = [self new];
    // nil is still an explicit ordered request: treat it as empty, never as
    // unset; a non-array (trust boundary: caller-supplied) is treated the
    // same instead of being fast-enumerated as garbage.
    substitutor->orderedTypes = [types isKindOfClass:[NSArray class]] ? [types copy] : @[];
    [substitutor initLibraries];
    return substitutor;
}

+ (instancetype)substitutorWithCategory:(hookkit_cat_t)category {
    // single-element fallback list: shares the ordered resolution loop
    return [self substitutorWithOrderedCategories:@[@(category)]];
}

+ (instancetype)substitutorWithOrderedCategories:(NSArray<NSNumber *> *)categories {
    HKSubstitutor *substitutor = [self new];
    // nil is still an explicit ordered request: treat it as empty, never as
    // unset; a non-array is treated the same (trust boundary).
    substitutor->orderedCategories = [categories isKindOfClass:[NSArray class]] ? [categories copy] : @[];
    [substitutor initLibraries];
    return substitutor;
}

+ (instancetype)substitutorWithAutoCoverCategories:(NSArray<NSNumber *> *)categories {
    HKSubstitutor *substitutor = [self new];
    // nil is still an explicit request: treat it as empty, never as unset;
    // a non-array is treated the same (trust boundary).
    substitutor->autoCoverCategories = [categories isKindOfClass:[NSArray class]] ? [categories copy] : @[];
    // Non-batched function hooks resolve per-hook, but initLibraries still
    // pins the first available category backend as the batched/image
    // fallback (see the auto-cover branch there).
    [substitutor initLibraries];
    return substitutor;
}

+ (instancetype)defaultSubstitutor {
    static HKSubstitutor *defaultSubstitutor = nil;
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        defaultSubstitutor = [self new];
        [defaultSubstitutor initLibraries];
    });

    return defaultSubstitutor;
}

// Auto-cover routing core: walks the autoCoverCategories list in priority
// order, and within each category walks its picker order. For each
// (category, picker) pair, instantiates the picker's backend and consults its
// side-effect-free preflightFunction: — the first available backend whose
// preflight accepts the target wins. Returns the configured backend, or nil
// when every picker declined (or no category matched); *outTechnique receives
// the WINNING picker's resolved technique (never written when nil is
// returned), which is the ACTUAL technique the backend will apply.
//
// Policy-aware (H8): when requestOriginal is YES (the caller asked for an
// original), a candidate whose resolved publication policy for the picker's
// technique is AfterActivation or Unavailable is SKIPPED in favor of the next
// picker — the router never picks a backend that could only refuse the hook
// at dispatch. No-original hooks keep every candidate eligible (the per-hook
// dispatch check in hookFunction: still enforces the policy before any guard
// reserve, so the router is a preference, not the only gate).
//
// Deliberately preflight-driven only: it never inspects a hook RESULT to
// decide routing (a failed invocation may have mutated the target). A backend
// without preflightFunction: is presumed to accept only when it is not an
// inline-capable prologue writer — inline-capable backends without a vendor
// preflight (the MS providers) are gated by the shared conservative prologue
// check (hk_shared_inline_preflight_ok), so no inline-capable dispatch ever
// proceeds unguarded.
- (id<HKSubstitutorBackend>)hk_backendForAutoCoverFunction:(void *)function replacement:(void *)replacement requestOriginal:(BOOL)requestOriginal technique:(HKFunctionTechnique *)outTechnique {
    return hk_walk_categories(autoCoverCategories, ^BOOL(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker) {
        if(requestOriginal) {
            // H8: resolved policy (Runtime entries are resolved from the
            // provider ABI here, exactly as the dispatch check does) — a
            // candidate that cannot publish a real original before activation
            // is declined so the walk continues to the next picker.
            HKOriginalPublicationPolicy policy = hk_resolved_publication_policy([self typeForBackend:candidate], picker.technique);

            if(policy == HKOriginalPublicationAfterActivation || policy == HKOriginalPublicationUnavailable) {
                return NO;
            }
        }

        if(![candidate respondsToSelector:@selector(preflightFunction:withReplacement:)]) {
            // No vendor veto channel: enforce the shared prologue check for
            // inline-capable backends (the MS providers: ElleKit, Substrate,
            // Substitute) — the generic checks (misaligned, self-hook,
            // unmapped, non-executable) plus, for the flagged fixed-window
            // backends, the window scan. A target that fails them is DECLINED
            // here — the walk continues to the next picker instead of
            // dispatching unguarded. The MS providers' own relocators decide
            // instruction eligibility. Non-inline backends without a
            // preflight (rebind paths) accept.
            if(!hk_shared_inline_preflight_ok(candidate, picker.technique, function, replacement)) {
                return NO;
            }

            if(outTechnique) {
                *outTechnique = picker.technique;
            }

            return YES;
        }

        if([candidate preflightFunction:function withReplacement:replacement] != HK_OK) {
            return NO;
        }

        if(outTechnique) {
            *outTechnique = picker.technique;
        }

        return YES;
    });
}

// Descriptor capability gate (H2/M7): whether the backend's registry kinds
// mask serves `kind`. The descriptor is the single source of truth for the
// message/function/memory kinds. Unknown backends fail closed.
static BOOL hk_descriptor_kind_supported(id<HKSubstitutorBackend> backend, HKHookKind kind) {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if([backend isKindOfClass:table[i].backendClass]) {
            switch(kind) {
                case HKHookKindMessage:
                    return (table[i].kinds & HK_CAT_MESSAGE) != 0;

                case HKHookKindFunction:
                    return (table[i].kinds & (HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE)) != 0;

                case HKHookKindMemory:
                    return (table[i].kinds & HK_CAT_MEMORY) != 0;
            }
        }
    }

    return NO;
}

// Shared tail of the three batching-capable hook entry points: backend
// presence, the batching decision, the kind check and the enqueue. Returns YES
// when the call is fully handled (result in *outStatus), NO when the caller
// should hook immediately. `build` fills in the kind-specific fields and only
// runs on the enqueue path, so a non-batched hook allocates nothing extra.
//
// Queue-always batching (H2): while batching is on, EVERY supported kind is
// queued, whether or not the backend has a native batch primitive — the drain
// runs non-native-batch backends per-op (executeOperation:onBackend:).
//
// Capability dispatch (H2/M7): the descriptor kinds mask gates BOTH paths,
// ahead of the batching branch — a hook of a kind the backend does not serve
// is refused with HK_ERR_NOT_SUPPORTED whether or not batching is on, so the
// immediate path never reaches a backend call it cannot honour.
//
// The caller's argument guard deliberately stays at the call site, ahead of
// this: hookMemory: copies the patch bytes in `build`, and dataWithBytes:NULL
// would crash before a guard in here could reject it.
- (BOOL)enqueueKind:(HKHookKind)kind status:(hookkit_status_t *)outStatus build:(void (^)(HKHookOperation *hook))build {
    if(!backend) {
        *outStatus = HK_ERR_NOT_SUPPORTED;
    } else if(!hk_descriptor_kind_supported(backend, kind)) {
        *outStatus = HK_ERR_NOT_SUPPORTED;
    } else if(!batching) {
        return NO;
    } else {
        HKHookOperation *hook = [HKHookOperation new];
        hook->kind = kind;
        build(hook);

        @synchronized(self) {
            [batchHooks addObject:hook];
        }

        *outStatus = HK_OK;
    }

    [self noteHookResult:*outStatus];
    return YES;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass withSelector:(SEL)selector withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!objcClass || !selector || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }

    // Normalize the dispatch class: class methods live on the metaclass, so a
    // class-method-only selector must be hooked through object_getClass() —
    // the central runtime hook resolves the IMP via class_getInstanceMethod,
    // which walks the metaclass's inheritance tree exactly like the class's
    // own. Instance methods pass the class through unchanged. (A selector
    // that exists on neither the class nor the metaclass is refused with
    // HK_ERR_NOT_SUPPORTED by the central hook — nothing is written.)
    Class dispatchClass = class_getInstanceMethod(objcClass, selector) ? objcClass : object_getClass(objcClass);

    hookkit_status_t status;

    if([self enqueueKind:HKHookKindMessage status:&status build:^(HKHookOperation *hook) {
        hook->objcClass = dispatchClass;
        hook->selector = selector;
        hook->replacement = replacement;
        hook->original.callerCell = old_ptr;
        hook->original.requested = (old_ptr != NULL);
    }]) {
        return status;
    }

    // Immediate path: the same publication cycle a drained message op runs —
    // the original is published into the caller's cell BEFORE the
    // class_replaceMethod swap lands (C1), so the caller observes it from the
    // moment activation happens.
    HKOriginalPublication publication = { .callerCell = old_ptr, .requested = (old_ptr != NULL) };
    hk_original_begin(&publication);
    hookkit_status_t result = hk_apply_message_hook(dispatchClass, selector, replacement, &publication);
    result = hk_original_finish(&publication, result);

    [self noteHookResult:result];
    return result;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!function || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }

    // Auto-cover routing: pick the first backend whose preflight accepts this
    // target (see hk_backendForAutoCoverFunction:), then invoke it once. When
    // routing is not enabled, `backend` is the pinned resolution. The
    // operation's ACTUAL technique is resolved here — the winning picker's
    // technique in auto-cover mode, the strategy/default-technique resolution
    // otherwise — and recorded on the op BEFORE the guard reserve, so the
    // guard, the preflight and the drain all see the same technique the
    // backend will apply.
    id<HKSubstitutorBackend> routeBackend = backend;
    HKFunctionTechnique technique = HKFunctionTechniqueNone;

    if(autoCoverCategories && !batching) {
        // H8: original-requesting hooks route policy-aware — the router skips
        // candidates whose resolved policy cannot publish an original before
        // activation, so a backend that could only refuse is never picked.
        routeBackend = [self hk_backendForAutoCoverFunction:function replacement:replacement requestOriginal:(old_ptr != NULL) technique:&technique];

        if(!routeBackend) {
            // Every picker declined the target: no backend can hook it safely.
            // No backend-specific detail to report — clear any stale state.
            [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:nil];
            return HK_ERR_NOT_SUPPORTED;
        }
    } else {
        technique = [self resolvedTechniqueForBackend:routeBackend];
    }

    // Capability gate (H2/M7): the routed backend's descriptor kinds mask is
    // the source of truth for what it serves. A function hook on a backend
    // whose mask lacks the function kinds is refused BEFORE any guard
    // reservation or backend call — both here (immediate path) and in
    // enqueueKind (queued path) — so no PENDING guard entry is ever left
    // behind by a capability refusal.
    if(!hk_descriptor_kind_supported(routeBackend, HKHookKindFunction)) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
        return HK_ERR_NOT_SUPPORTED;
    }

    // Descriptor-driven original-publication policy (M7): a caller that
    // REQUESTED an original is refused BEFORE any dispatch when the resolved
    // technique's policy cannot publish one before activation. Backends whose
    // policy is BeforeActivation/Runtime write the original into the caller's
    // cell before the patch lands; AfterActivation/Unavailable backends get a
    // side-effect-free HK_ERR_NOT_SUPPORTED so the caller can retry without
    // requesting an original.
    if(old_ptr) {
        HKOriginalPublicationPolicy policy = hk_resolved_publication_policy([self typeForBackend:routeBackend], technique);

        if(policy == HKOriginalPublicationAfterActivation || policy == HKOriginalPublicationUnavailable) {
            [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
            return HK_ERR_NOT_SUPPORTED;
        }
    }

    // Shared prologue check: an inline-capable backend without a
    // preflightFunction: of its own (ElleKit/Substrate/Substitute) is only
    // dispatched when the shared validator accepts the target — the generic
    // checks on every inline dispatch, plus the fixed-window scan for the
    // flagged relocators (Dobby, litehook), the same checks those backends
    // run themselves. The check is descriptor-gated (see
    // hk_shared_inline_preflight_ok: arch + technique + sharedArm64Preflight),
    // so rebind paths skip it and the strong engines are basic-checked only.
    // This covers every inline-capable dispatch path — auto-cover (already
    // declined in the router, re-checked here), explicit inline, and the
    // batched enqueue — and refuses BEFORE any guard reservation or write,
    // so a rejected target never reaches a backend unguarded.
    if(!hk_shared_inline_preflight_ok(routeBackend, technique, function, replacement)) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
        return HK_ERR_NOT_SUPPORTED;
    }

    // Process-wide inline-ownership guard: prevents HookKit-vs-HookKit
    // contention — two substitutors (or one hooking twice) installing
    // DIFFERENT inline hooks on the same address through DIFFERENT inline
    // backends would double-patch one prologue. Only the inline technique is
    // guarded (descriptor-resolved): rebind paths (fishhook/litehook-rebind)
    // are GOT-scoped and memory patches are byte blobs — neither overwrites
    // the prologue, so both stay unguarded (ponytail: rebind-vs-inline on one
    // address is a same-slot double-write only if the prologue is the GOT
    // slot, which never happens for function pointers).
    uint64_t guardGeneration = 0;   // 0 = not inline-guarded

    if(technique == HKFunctionTechniqueInline) {
        // Normalize the key exactly as the backend will write: strip PAC on
        // arm64e, mask the thumb bit on 32-bit ARM. The op stores the
        // stripped pointer, so executeHooks can re-derive the same key for
        // the settle (hk_inline_guard_key) without the raw pointer the
        // backend consumed.
#if __has_feature(ptrauth_calls)
        function = ptrauth_strip(function, ptrauth_key_asia);
#endif
        uintptr_t guardAddr = hk_inline_guard_key(function);

        void *guardOrig = NULL;
        hk_guard_result_t guard = hk_inline_guard_reserve(guardAddr, replacement, [self typeForBackend:routeBackend], &guardGeneration, &guardOrig);

        switch(guard) {
            case HK_GUARD_BLOCKED:
                // Already inline-hooked by another HookKit backend with a
                // DIFFERENT replacement — invoking this backend would double-
                // patch the prologue. Nothing was written.
                [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
                return HK_ERR_NOT_SUPPORTED;

            case HK_GUARD_FULL:
                // The ownership table is full and no entry matched: nothing
                // was reserved, so proceeding would install an UNGUARDED hook
                // — the one outcome worse than declining it. Refuse; nothing
                // was written.
                [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
                return HK_ERR_NOT_SUPPORTED;

            case HK_GUARD_DUP_PENDING:
                // Same address + same replacement already reserved but not
                // installed (its hook is queued or in flight): nothing was
                // written and no original exists yet — refuse. The old
                // waiter-based deferral is gone: the caller can retry after
                // the owning drain settles the entry.
                [self noteHookResult:HK_ERR fromBackend:nil];
                return HK_ERR;

            case HK_GUARD_DUP_TAINTED:
                // Same address + same replacement, but a previous hook failed
                // mid-flight: the original is unknowable — hard error.
                [self noteHookResult:HK_ERR fromBackend:nil];
                return HK_ERR;

            case HK_GUARD_DUP_INSTALLED:
                // Idempotent same-replacement re-hook: the hook is already
                // installed, so the saved original is the answer — but only
                // when the caller asked for one AND one exists.
                if(!old_ptr) {
                    [self noteHookResult:HK_OK fromBackend:routeBackend];
                    return HK_OK;
                }

                if(guardOrig) {
                    *old_ptr = guardOrig;
                    [self noteHookResult:HK_OK fromBackend:routeBackend];
                    return HK_OK;
                }

                // The caller requested an original the installed entry cannot
                // provide: hard error, nothing more to publish.
                [self noteHookResult:HK_ERR fromBackend:nil];
                return HK_ERR;

            case HK_GUARD_RESERVED:
                // Entry reserved (PENDING); the hook proceeds. The entry is
                // settled below (immediate path) or in executeHooks (batched
                // path) with the generation token.
                break;
        }
    }

    hookkit_status_t status;

    if([self enqueueKind:HKHookKindFunction status:&status build:^(HKHookOperation *hook) {
        hook->function = function;
        hook->replacement = replacement;
        hook->original.callerCell = old_ptr;
        hook->original.requested = (old_ptr != NULL);
        hook->guardToken = guardGeneration;   // 0 = not inline-guarded
        hook->technique = technique;   // ACTUAL resolved technique (see above)
    }]) {
        return status;
    }

    // Immediate path: the same publication cycle a drained function op runs
    // (see executeOperation:) — hk_original_begin saves and NULLs the
    // caller's cell, the backend writes the original through
    // hk_original_output_cell (the caller's ACTUAL cell when requested, never
    // a facade-owned staging cell copied afterward), publish records it, and
    // finish applies the only-real-original invariant. The caller's cell
    // therefore holds the original BEFORE the replacement becomes reachable —
    // there is no copy-after-activation in the facade. The raw backend status
    // goes straight through finish, which converts an OK-with-no-original to
    // HK_ERR only when the caller REQUESTED one (a litehook-inline install
    // with no original is success when nothing was requested).
    HKOriginalPublication publication = { .callerCell = old_ptr, .requested = (old_ptr != NULL) };
    hk_original_begin(&publication);
    void **cell = hk_original_output_cell(&publication);
    hookkit_status_t result = [routeBackend hookFunction:function withReplacement:replacement outOldPtr:cell];
    hk_original_publish(&publication, cell ? *cell : NULL);
    result = hk_original_finish(&publication, result);

    if(guardGeneration) {
        // Settle the guard with the FINAL outcome: OK stores the saved
        // original, HK_ERR taints (the prologue may be half-written), and
        // NOT_SUPPORTED releases the entry (the backend wrote nothing). The
        // generation token drops stale settles.
        hk_inline_guard_update(hk_inline_guard_key(function), guardGeneration,
                               result == HK_OK ? 0 : (result == HK_ERR_NOT_SUPPORTED ? 2 : 1), publication.value);
    }

    // Attribute to the backend that actually ran (routeBackend == backend
    // except in auto-cover mode, where routing can pick a different one).
    [self noteHookResult:result fromBackend:routeBackend];
    return result;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    if(!target || !data || size == 0) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }

    hookkit_status_t status;

    if([self enqueueKind:HKHookKindMemory status:&status build:^(HKHookOperation *hook) {
        hook->target = target;
        // copy the patch bytes now: the caller's buffer must not outlive the call
        hook->data = [NSData dataWithBytes:data length:size];
        hook->size = size;
    }]) {
        return status;
    }

    hookkit_status_t result = [backend hookMemory:target withData:data size:size];
    [self noteHookResult:result];
    return result;
}

- (hookkit_status_t)hookSwiftMethodInClass:(Class)objcClass withName:(NSString *)name withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    // Validate the name as an NSString BEFORE length/UTF8String (trust
    // boundary: caller-supplied; a malformed element would raise an uncaught
    // NSInvalidArgumentException at [name length]).
    if(!objcClass || ![name isKindOfClass:[NSString class]] || ![name length] || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }

    if(!backend) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED];
        return HK_ERR_NOT_SUPPORTED;
    }

    if(![backend respondsToSelector:@selector(hookSwiftMethodInClass:withName:withReplacement:outOldPtr:)]) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED];
        return HK_ERR_NOT_SUPPORTED;
    }

    // Immediate path: the same publication cycle the drained ops run — the
    // backend writes the original through hk_original_output_cell (the
    // caller's ACTUAL cell when requested), so the caller's cell holds it
    // BEFORE the vtable slot becomes reachable; no copy-after-activation.
    HKOriginalPublication publication = { .callerCell = old_ptr, .requested = (old_ptr != NULL) };
    hk_original_begin(&publication);
    void **cell = hk_original_output_cell(&publication);
    hookkit_status_t result = [backend hookSwiftMethodInClass:objcClass withName:name withReplacement:replacement outOldPtr:cell];
    hk_original_publish(&publication, cell ? *cell : NULL);
    result = hk_original_finish(&publication, result);

    [self noteHookResult:result];
    return result;
}

- (hookkit_status_t)hookSwiftVtableSlotInClass:(Class)objcClass withIndex:(NSUInteger)index withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!objcClass || index > UINT32_MAX || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }

    if(!backend) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED];
        return HK_ERR_NOT_SUPPORTED;
    }

    if(![backend respondsToSelector:@selector(hookSwiftVtableSlotInClass:withIndex:withReplacement:outOldPtr:)]) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED];
        return HK_ERR_NOT_SUPPORTED;
    }

    // Immediate path: the same publication cycle the drained ops run — the
    // backend writes the original through hk_original_output_cell (the
    // caller's ACTUAL cell when requested), so the caller's cell holds it
    // BEFORE the vtable slot becomes reachable; no copy-after-activation.
    HKOriginalPublication publication = { .callerCell = old_ptr, .requested = (old_ptr != NULL) };
    hk_original_begin(&publication);
    void **cell = hk_original_output_cell(&publication);
    hookkit_status_t result = [backend hookSwiftVtableSlotInClass:objcClass withIndex:index withReplacement:replacement outOldPtr:cell];
    hk_original_publish(&publication, cell ? *cell : NULL);
    result = hk_original_finish(&publication, result);

    [self noteHookResult:result];
    return result;
}

// Magic tagging the facade-owned HKImage wrappers (struct HKImage, declared
// in Internal/HKBackendInternal.h): distinguishes a LIVE wrapper from a
// tombstoned (closed) one, and — zeroed at close — makes closeImage:
// idempotent. Foreign or garbage handles never reach this check: they are
// rejected by registry membership first (see below).
static const uint32_t HKImageWrapperMagic = 0x484B494D;   // 'HKIM'

// Live-wrapper registry (M4): every struct HKImage the facade allocates is
// registered by POINTER IDENTITY before it is returned and stays registered
// for the process lifetime. All handle validation is membership-in-registry
// FIRST — the caller-supplied pointer is never dereferenced until it has been
// proven to be one of ours, so a closed, foreign or garbage handle is
// rejected without touching (possibly freed) memory. The magic/ownerType
// fields are a secondary assertion INSIDE the registry validation (safe to
// read: membership already proved the allocation is ours and alive). A mutex
// guards the registry: openImage:/findSymbolInImage:/closeImage: may run on
// different threads.
static pthread_mutex_t hk_image_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableArrayRef hk_live_images = NULL;

static CFMutableArrayRef hk_image_registry(void) {
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        // NULL callbacks: raw pointers, no retain/release.
        hk_live_images = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
    });

    return hk_live_images;
}

static BOOL hk_image_registered(struct HKImage *wrapper) {
    CFArrayRef registry = hk_image_registry();
    pthread_mutex_lock(&hk_image_registry_lock);
    BOOL found = CFArrayContainsValue(registry, CFRangeMake(0, CFArrayGetCount(registry)), wrapper);
    pthread_mutex_unlock(&hk_image_registry_lock);
    return found;
}

static void hk_image_register(struct HKImage *wrapper) {
    CFMutableArrayRef registry = hk_image_registry();
    pthread_mutex_lock(&hk_image_registry_lock);
    CFArrayAppendValue(registry, wrapper);
    pthread_mutex_unlock(&hk_image_registry_lock);
}

// Per-wrapper lock table (M4 concurrency): one pthread_mutex_t per live
// wrapper, serializing each wrapper's validation AND backend raw-handle use.
// Entries are append-only — created at openImage: before the wrapper becomes
// visible, never removed — so a pointer fetched here stays valid for the
// process lifetime (ponytail: leak-not-UAF — one heap mutex per opened image,
// mirroring the tombstoned wrapper that is itself never freed; freeing an
// entry would reopen the close-vs-find race under concurrency).
static pthread_mutex_t hk_image_lock_table_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef hk_image_locks = NULL;

static CFMutableDictionaryRef hk_image_lock_table(void) {
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        // NULL callbacks: raw pointer keys (wrapper addresses) and values
        // (heap mutexes), no retain/release.
        hk_image_locks = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, NULL, NULL);
    });

    return hk_image_locks;
}

static pthread_mutex_t *hk_image_lock_for(struct HKImage *wrapper) {
    pthread_mutex_lock(&hk_image_lock_table_lock);
    pthread_mutex_t *lock = (pthread_mutex_t *)CFDictionaryGetValue(hk_image_lock_table(), wrapper);
    pthread_mutex_unlock(&hk_image_lock_table_lock);
    return lock;
}

// Validate a NON-NULL public image handle and return its per-wrapper mutex,
// HELD (locked); the caller does its backend raw-handle use and unlocks.
// Distinguishes the cases (M4):
//   live facade wrapper     -> non-NULL, locked — the owning backend's raw
//                              handle is stable for the duration (a
//                              concurrent closeImage: blocks on the lock)
//   anything else (foreign, closed/tombstoned, garbage) -> NULL — rejected by
//                              registry membership without dereferencing, and
//                              NEVER falling through to the global lookup.
//                              The magic/ownerType liveness check is re-run
//                              UNDER the wrapper lock, so a close that landed
//                              between membership and lock acquisition is
//                              still caught. (The NULL/global case has no
//                              wrapper; callers handle it before this.)
static pthread_mutex_t *hk_image_acquire(struct HKImage *wrapper, hookkit_lib_t ownerType) {
    if(!hk_image_registered(wrapper)) {
        return NULL;   // not one of ours: closed, foreign or garbage
    }

    pthread_mutex_t *lock = hk_image_lock_for(wrapper);

    pthread_mutex_lock(lock);

    if(wrapper->magic != HKImageWrapperMagic || wrapper->ownerType != ownerType) {
        pthread_mutex_unlock(lock);
        return NULL;   // tombstoned (already closed) or cross-backend wrapper
    }

    return lock;
}

- (HKImageRef)openImage:(NSString *)path {
    if(![path isKindOfClass:[NSString class]] || !backend) {
        return NULL;
    }

    void *rawHandle = (void *)[backend openImage:path];

    if(!rawHandle) {
        return NULL;
    }

    // Facade-owned wrapper: tags the handle with the owning backend so
    // find/close can validate it (cross-backend use fails cleanly instead of
    // handing one engine's handle to another).
    struct HKImage *wrapper = calloc(1, sizeof(struct HKImage));

    if(!wrapper) {
        // M4: the raw backend handle must not leak when the wrapper
        // allocation fails.
        [backend closeImage:rawHandle];
        return NULL;
    }

    // Per-wrapper lock, heap-allocated and NEVER freed (ponytail: leak-not-UAF
    // — a reader that already holds the wrapper pointer must always find a
    // valid mutex for it; the tombstoned wrapper already lives for the
    // process lifetime). Entered in the table BEFORE registration, so any
    // thread that can see the wrapper can acquire its lock.
    pthread_mutex_t *wrapperLock = malloc(sizeof(pthread_mutex_t));

    if(!wrapperLock) {
        [backend closeImage:rawHandle];
        free(wrapper);
        return NULL;
    }

    if(pthread_mutex_init(wrapperLock, NULL) != 0) {
        free(wrapperLock);
        [backend closeImage:rawHandle];
        free(wrapper);
        return NULL;
    }

    pthread_mutex_lock(&hk_image_lock_table_lock);
    CFDictionaryAddValue(hk_image_lock_table(), wrapper, wrapperLock);
    pthread_mutex_unlock(&hk_image_lock_table_lock);

    wrapper->magic = HKImageWrapperMagic;
    wrapper->ownerType = [self typeForBackend:backend];
    wrapper->rawHandle = rawHandle;
    // Live (validatable) from the moment it is handed out: registered before
    // the pointer becomes visible to the caller.
    hk_image_register(wrapper);
    return (HKImageRef)wrapper;
}

- (void)closeImage:(HKImageRef)image {
    if(!backend || !image) {
        return;
    }

    struct HKImage *wrapper = (struct HKImage *)image;

    // M4: registry membership by pointer identity FIRST — a foreign or
    // garbage handle is ignored without ever dereferencing it.
    if(!hk_image_registered(wrapper)) {
        return;
    }

    // Liveness is re-checked UNDER the wrapper lock: concurrent closes
    // serialize here, so only the first observes a live wrapper and closes
    // the raw handle; the second finds the tombstone and no-ops.
    pthread_mutex_t *lock = hk_image_lock_for(wrapper);

    pthread_mutex_lock(lock);

    if(wrapper->magic != HKImageWrapperMagic || wrapper->ownerType != [self typeForBackend:backend]) {
        pthread_mutex_unlock(lock);
        return;   // already closed (tombstoned) or cross-backend: no-op
    }

    void *rawHandle = wrapper->rawHandle;
    // Tombstone while STILL LOCKED, before the raw handle is closed: a
    // concurrent find either locked first (finished its backend use before
    // the close) or locks after and is rejected by the tombstone — a find can
    // never resolve a raw handle that is about to be (or has been) closed.
    wrapper->magic = 0;
    [backend closeImage:rawHandle];
    pthread_mutex_unlock(lock);
    // The wrapper is NEVER freed — it stays registered, so any later use of
    // the stale pointer (close or find) is rejected by membership/magic
    // without a double-close UAF.
    // ponytail: leak-not-UAF — one 24-byte wrapper per opened image for the
    // process lifetime; freeing after registry removal would reopen the
    // close-vs-find race under concurrency.
}

- (hookkit_status_t)findSymbolsInImage:(HKImageRef)image symbolNames:(NSArray<NSString *> *)symbolNames outSymbols:(NSArray<NSValue *> **)outSymbols {
    // Trust boundary: symbolNames is caller-supplied — validate the container
    // is an NSArray BEFORE sending it count (a foreign container class would
    // raise an uncaught exception at [symbolNames count]); nil fails the
    // isKindOfClass check and is rejected the same way.
    if(![symbolNames isKindOfClass:[NSArray class]] || ![symbolNames count] || !outSymbols) {
        return HK_ERR_INVALID_ARGUMENT;
    }

    // Prevalidate EVERY element before the first lookup (M6): a malformed
    // element must reject without running any lookups and without touching
    // the output parameter. Trust boundary: symbolNames is caller-supplied.
    for(NSString *symbolName in symbolNames) {
        if(![symbolName isKindOfClass:[NSString class]]) {
            NSLog(@"[HookKit] warning: %s in symbolNames is not an NSString; rejected", class_getName([symbolName class]));
            return HK_ERR_INVALID_ARGUMENT;
        }
    }

    // M4: the image handle is validated up front — a non-NULL image must be
    // a live facade wrapper owned by the active backend. A foreign, closed
    // or garbage handle rejects the WHOLE call with HK_ERR_INVALID_ARGUMENT
    // before any lookup (a NULL image keeps the global-lookup contract and
    // is resolved per symbol below — each lookup through findSymbolInImage:
    // holds the wrapper lock through its own validation + backend use).
    if(backend && image) {
        pthread_mutex_t *lock = hk_image_acquire((struct HKImage *)image, [self typeForBackend:backend]);

        if(!lock) {
            return HK_ERR_INVALID_ARGUMENT;
        }

        pthread_mutex_unlock(lock);
    }

    NSMutableArray *outSyms = [NSMutableArray new];
    NSUInteger found = 0;

    for(NSString *symbolName in symbolNames) {
        void *symbol = [self findSymbolInImage:image symbolName:symbolName];

        if(symbol) {
            found += 1;
        }

        [outSyms addObject:[NSValue valueWithPointer:symbol]];
    }

    *outSymbols = [outSyms copy];

    if(found == [symbolNames count]) {
        return HK_OK;
    }

    return found > 0 ? HK_ERR_PARTIAL : HK_ERR;
}

- (void *)findSymbolInImage:(HKImageRef)image symbolName:(NSString *)symbolName {
    if(![symbolName isKindOfClass:[NSString class]] || ![symbolName length]) {
        return NULL;
    }

    if(!backend) {
        return NULL;
    }

    // M4: NULL is the documented global (main-image) lookup and is forwarded
    // to the backend as NULL (no wrapper, no lock); ANY other handle must be
    // a live facade wrapper of the active backend — the wrapper lock is held
    // through validation AND the backend raw-handle call, so a concurrent
    // closeImage: cannot close the handle mid-use or double-close it. A
    // handle that fails validation (foreign, closed/tombstoned, garbage) is
    // rejected WITHOUT a lookup — it must never fall through to the global
    // path by unwrapping to NULL.
    if(!image) {
        return [backend findSymbolInImage:NULL symbolName:symbolName];
    }

    struct HKImage *wrapper = (struct HKImage *)image;
    pthread_mutex_t *lock = hk_image_acquire(wrapper, [self typeForBackend:backend]);

    if(!lock) {
        return NULL;
    }

    void *symbol = [backend findSymbolInImage:wrapper->rawHandle symbolName:symbolName];
    pthread_mutex_unlock(lock);
    return symbol;
}

// Single-op sequential execution for backends without a native batch
// primitive: run one operation's full publication cycle — hk_original_begin
// (saves and NULLs the caller's cell) → the backend call (function/memory)
// or the central runtime message hook, both through the publication's output
// cell → publish what was produced → hk_original_finish (applies the
// publish-only-real-original invariant). The op's status and backendErrno
// carry the final outcome.
- (hookkit_status_t)executeOperation:(HKHookOperation *)hook onBackend:(id<HKSubstitutorBackend>)resultBackend {
    hk_original_begin(&hook->original);

    void **cell = hk_original_output_cell(&hook->original);
    hookkit_status_t result;

    switch(hook->kind) {
        case HKHookKindMessage:
            // Central runtime message hook: resolves the current/inherited
            // IMP, publishes it BEFORE the replace lands, then swaps the
            // implementation (see hk_apply_message_hook) — no per-backend
            // message dispatch.
            result = hk_apply_message_hook(hook->objcClass, hook->selector, hook->replacement, &hook->original);
            break;

        case HKHookKindFunction:
            result = [resultBackend hookFunction:hook->function withReplacement:hook->replacement outOldPtr:cell];
            break;

        case HKHookKindMemory:
            result = [resultBackend hookMemory:hook->target withData:[hook->data bytes] size:hook->size];
            break;
    }

    // Record what the backend produced (idempotent when it wrote straight
    // into the caller's cell; marks published so finish's invariant passes).
    hk_original_publish(&hook->original, cell ? *cell : NULL);
    result = hk_original_finish(&hook->original, result);
    hook->status = result;

    if(result != HK_OK) {
        hook->backendErrno = [resultBackend lastErrno];
    }

    return result;
}

- (hookkit_status_t)executeHooks {
    NSArray<HKHookOperation *> *hooks;

    @synchronized(self) {
        if(![batchHooks count]) {
            [self noteHookResult:HK_OK];
            return HK_OK;
        }

        hooks = [batchHooks copy];
        [batchHooks removeAllObjects];
    }

    if(!backend) {
        // Nothing can be queued without a backend (enqueueKind refuses), so
        // this is unreachable in practice; fail closed anyway.
        [self noteHookResult:HK_ERR_NOT_SUPPORTED];
        return HK_ERR_NOT_SUPPORTED;
    }

    if([self backendHasNativeBatch:backend]) {
        // Native batch path: begin every publication up front, hand the whole
        // drained snapshot to the backend (it fills each op's status /
        // backendErrno and writes each original through the output cell), then
        // publish and finalize per op.
        for(HKHookOperation *hook in hooks) {
            hk_original_begin(&hook->original);
        }

        [backend executeHooks:hooks];

        for(HKHookOperation *hook in hooks) {
            void **cell = hk_original_output_cell(&hook->original);
            hk_original_publish(&hook->original, cell ? *cell : NULL);
            hook->status = hk_original_finish(&hook->original, hook->status);
        }
    } else {
        // Sequential path: no native batch primitive — run each op through the
        // single-op helper (begin → backend call → publish → finish).
        for(HKHookOperation *hook in hooks) {
            [self executeOperation:hook onBackend:backend];
        }
    }

    // Settle the inline guards from each op's FINAL status (finish may have
    // changed it), and count successes for the aggregate result. OK stores
    // the saved original, HK_ERR taints (the prologue may be half-written),
    // NOT_SUPPORTED releases (the backend wrote nothing). Stale-generation
    // settles are dropped by the guard itself.
    int okCount = 0;

    for(HKHookOperation *hook in hooks) {
        if(hook->guardToken && hook->kind == HKHookKindFunction) {
            hk_inline_guard_update(hk_inline_guard_key(hook->function), hook->guardToken,
                                   hook->status == HK_OK ? 0 : (hook->status == HK_ERR_NOT_SUPPORTED ? 2 : 1),
                                   hook->original.value);
        }

        if(hook->status == HK_OK) {
            okCount += 1;
        }
    }

    hookkit_status_t result = hk_batch_status(okCount, (int)[hooks count]);

    if(result == HK_OK) {
        [self noteHookResult:HK_OK];
    } else {
        // Preserve the first failing op's per-backend detail. A generic
        // capability miss (NOT_SUPPORTED with no backend detail) clears any
        // stale value, like noteHookResult does for the immediate paths.
        for(HKHookOperation *hook in hooks) {
            if(hook->status == HK_OK) {
                continue;
            }

            if(hook->status == HK_ERR_NOT_SUPPORTED && hook->backendErrno == 0) {
                lastLibErrno = 0;
                lastLibErrnoType = HK_LIB_NONE;
            } else {
                lastLibErrno = hook->backendErrno;
                lastLibErrnoType = [self typeForBackend:backend];
            }

            break;
        }
    }

    return result;
}

- (int)getLibErrno:(hookkit_lib_t *)outType {
    if(outType) {
        *outType = lastLibErrnoType;
    }

    return lastLibErrno;
}
@end