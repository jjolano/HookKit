#import <HookKit.h>
#import "Internal/HKBackendInternal.h"
#import "Internal/HKInlineGuard.h"
#import "Internal/HKInlinePreflight.h"

#import <objc/runtime.h>
#include <stdlib.h>

#import "native/hk_swift.h"

// Owned here (resolved by swift_available() via dlsym); consumed by the
// engine's name-based lookup (declared in native/hk_swift.h).
hk_swift_demangle_fn hk_swift_demangle = NULL;

// Facade-private deferred state. Only the five function-batch fields inherited
// from HKFunctionBatchItem cross the backend seam.
@interface HKHookOperation : HKFunctionBatchItem {
@public
    HKHookKind kind;
    Class objcClass;
    SEL selector;
    void *target;
    NSData *data;
    size_t size;
    HKFunctionTechnique technique;
    uint64_t guardToken;
    id<HKSubstitutorBackend> routedBackend;
    BOOL routeAtDrain;
    NSUInteger routeCursor;
    BOOL automaticRoute;
}
@end

@implementation HKHookOperation
@end

// Shared prologue check for inline-capable backends that bring no
// preflightFunction: of their own. Every other inline adapter gets the common
// PAC/alignment/self-hook/readable/executable checks before dispatch. Adapters
// with a fixed overwrite window run their stronger validator in their own
// preflight/hook method, so they are not double-checked here.
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
static id<HKSubstitutorBackend> hk_walk_categories(NSArray<NSNumber *> *categories, BOOL (^visit)(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker, NSUInteger cursor)) {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);
    NSUInteger cursor = 0;

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
                    NSUInteger pickerCursor = cursor++;

                    for(size_t i = 0; i < count; i++) {
                        if(table[i].type != picker.type || !table[i].available()) {
                            continue;
                        }

                        id<HKSubstitutorBackend> candidate = [table[i].backendClass new];

                        if([candidate respondsToSelector:@selector(setStrategy:)]) {
                            [candidate setStrategy:picker.strategy];
                        }

                        if(visit(candidate, picker, pickerCursor)) {
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
- (void)drainGroup:(NSArray<HKHookOperation *> *)hooks onBackend:(id<HKSubstitutorBackend>)groupBackend;
- (void)drainRoutedHooks:(NSArray<HKHookOperation *> *)hooks;
- (HKFunctionTechnique)resolvedTechniqueForBackend:(id<HKSubstitutorBackend>)candidate;
- (BOOL)routeFunctionOperation:(HKHookOperation *)hook;
- (BOOL)routeMemoryOperation:(HKHookOperation *)hook;
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
    // single-element list). Tried in order; HK_CAT_MESSAGE needs no backend,
    // while the other categories stop at the first available picker.
    // Non-nil-but-empty means no backend.
    NSArray<NSNumber *> *orderedCategories;
    // Auto-cover routing mode: same shape as orderedCategories, but the
    // backend is chosen PER-HOOK by preflight instead of once at init. The
    // category's pickers are walked in priority order; the first available
    // backend whose side-effect-free preflight accepts the target is invoked
    // exactly once. Set via substitutorWithAutoCoverCategories:.
    NSArray<NSNumber *> *autoCoverCategories;
    // The implicit/default substitutor keeps its legacy pinned backend for
    // image APIs and activeType, but routes function and memory hooks by
    // operation. Explicit type/category requests never set this flag.
    BOOL automaticSelection;
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
        // Category fallback list: messages resolve in the facade; otherwise
        // try each category's (backend, strategy) pickers in order and stop at
        // the first available one (its own picker priority decides which).
        // Availability is checked
        // directly (not the automatic flag), so opt-in backends like Native
        // and Frida are still reachable when they are the only option in a
        // category. Shared iterator: same NSNumber validation as every other
        // category-driven path.
        types = HK_LIB_NONE;

        for(NSNumber *num in orderedCategories) {
            if(![num isKindOfClass:[NSNumber class]]) {
                NSLog(@"[HookKit] warning: %s in category list is not an NSNumber; skipped", class_getName([num class]));
                continue;
            }

            hookkit_cat_t want = (hookkit_cat_t)num.unsignedIntegerValue;
            if(want & HK_CAT_MESSAGE) {
                break;
            }

            backend = hk_walk_categories(@[num], ^BOOL(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker, NSUInteger cursor) {
                (void) cursor;
                resolvedStrategy = picker.strategy;
                types |= [self typeForBackend:candidate];
                return YES;
            });

            if(backend) {
                break;
            }
        }
    } else if(autoCoverCategories) {
        // Auto-cover mode: no single function backend is authoritative. Pin
        // the first available category backend for image/symbol APIs and
        // activeType; function hooks route per operation.
        types = HK_LIB_NONE;
        resolvedStrategy = HKStrategyDefault;

        backend = hk_walk_categories(autoCoverCategories, ^BOOL(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker, NSUInteger cursor) {
            (void) cursor;
            resolvedStrategy = picker.strategy;
            types |= [self typeForBackend:candidate];
            return YES;
        });
    } else if(types == HK_LIB_NONE) {
        backend = [[self class] defaultBackend];
        resolvedStrategy = HKStrategyDefault;
        automaticSelection = YES;
        autoCoverCategories = @[@(HK_CAT_FUNCTION_INLINE), @(HK_CAT_FUNCTION_REBIND)];
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
    const HKBackendDescriptor *descriptor = hk_backend_descriptor_for_backend(candidate);
    return descriptor ? descriptor->type : HK_LIB_NONE;
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
            const HKBackendDescriptor *descriptor = hk_backend_descriptor_for_backend(candidate);
            return descriptor ? descriptor->defaultTechnique : HKFunctionTechniqueNone;
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
        // discovery is consulted here (the descriptor's discoverable probe) — never
        // the available() probes, which dlopen providers and run their
        // constructors (gum_init_embedded can execute inside the host
        // process). Backends without a discoverable variant are reported
        // unavailable on the safe path; the real dlopen happens only on the
        // actual hook path (initLibraries, defaultBackend, auto-cover),
        // which keeps using the full available() probes.
        if(table[i].discoverable()) {
            types |= table[i].type;
        }
    }

    return types;
}

+ (hookkit_cat_t)getAvailableCategories {
    // Objective-C runtime message hooking is facade-native and always
    // available; the registry table below covers adapter-selected categories.
    hookkit_cat_t cats = HK_CAT_MESSAGE;
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    // A backend-selected category is available when any picker maps to an
    // available backend — the same lookup initLibraries performs. Same
    // discovery-vs-activation rule as getAvailableSubstitutorTypes: only
    // side-effect-free descriptor discovery is consulted,
    // never the available() probes that dlopen and initialize providers.
    for(size_t c = 0; c < hk_category_priority_count; c++) {
        for(size_t o = 0; o < hk_category_priorities[c].count; o++) {
            for(size_t i = 0; i < count; i++) {
                if(table[i].type != hk_category_priorities[c].order[o].type) {
                    continue;
                }

                if(table[i].discoverable()) {
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
        if((types & table[i].type) && table[i].discoverable()) {
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
// The caller advances the returned cursor only after NOT_SUPPORTED, whose
// public contract guarantees nothing was written. Every other hook result is
// terminal. A backend without preflightFunction: is presumed to accept only
// when it is not an
// inline-capable prologue writer — inline-capable backends without a vendor
// preflight (the MS providers) are gated by the shared conservative prologue
// check (hk_shared_inline_preflight_ok), so no inline-capable dispatch ever
// proceeds unguarded.
- (id<HKSubstitutorBackend>)hk_backendForAutoCoverFunction:(void *)function replacement:(void *)replacement requestOriginal:(BOOL)requestOriginal startingAtCursor:(NSUInteger)startCursor nextCursor:(NSUInteger *)outNextCursor technique:(HKFunctionTechnique *)outTechnique {
    return hk_walk_categories(autoCoverCategories, ^BOOL(id<HKSubstitutorBackend> candidate, HKCategoryPicker picker, NSUInteger cursor) {
        if(cursor < startCursor) {
            return NO;
        }

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
            if(outNextCursor) {
                *outNextCursor = cursor + 1;
            }

            return YES;
        }

        if([candidate preflightFunction:function withReplacement:replacement] != HK_OK) {
            return NO;
        }

        if(outTechnique) {
            *outTechnique = picker.technique;
        }
        if(outNextCursor) {
            *outNextCursor = cursor + 1;
        }

        return YES;
    });
}

// Descriptor capability gate (H2/M7): whether the backend's registry kinds
// mask serves `kind`. Messages bypass this gate because they are facade-owned;
// the descriptor is the single source of truth for function/memory support.
// Unknown backends fail closed.
static BOOL hk_descriptor_kind_supported(id<HKSubstitutorBackend> backend, HKHookKind kind) {
    const HKBackendDescriptor *descriptor = hk_backend_descriptor_for_backend(backend);

    if(descriptor) {
        switch(kind) {
            case HKHookKindMessage:
                return YES;

            case HKHookKindFunction:
                return (descriptor->kinds & (HK_CAT_FUNCTION_REBIND | HK_CAT_FUNCTION_INLINE)) != 0;

            case HKHookKindMemory:
                return (descriptor->kinds & HK_CAT_MEMORY) != 0;
        }
    }

    return NO;
}

// Selects and reserves the next safe function route. Returning YES means the
// caller should dispatch the operation. Returning NO means routing settled it
// (idempotent success, hard guard error, or exhausted capability routes).
- (BOOL)routeFunctionOperation:(HKHookOperation *)hook {
    for(;;) {
        HKFunctionTechnique technique = HKFunctionTechniqueNone;
        NSUInteger nextCursor = hook->routeCursor;
        id<HKSubstitutorBackend> candidate = [self hk_backendForAutoCoverFunction:hook->function
                                                                      replacement:hook->replacement
                                                                  requestOriginal:(hook->original.callerCell != NULL)
                                                                startingAtCursor:hook->routeCursor
                                                                       nextCursor:&nextCursor
                                                                        technique:&technique];
        if(!candidate) {
            hook->routedBackend = nil;
            hook->backendErrno = 0;
            hook->status = HK_ERR_NOT_SUPPORTED;
            return NO;
        }

        hook->routedBackend = candidate;
        hook->routeCursor = nextCursor;
        hook->technique = technique;
        hook->backendErrno = 0;

        if(!hk_descriptor_kind_supported(candidate, HKHookKindFunction) ||
           !hk_shared_inline_preflight_ok(candidate, technique, hook->function, hook->replacement)) {
            continue;
        }

        if(technique != HKFunctionTechniqueInline) {
            hook->guardToken = 0;
            return YES;
        }

#if __has_feature(ptrauth_calls)
        hook->function = ptrauth_strip(hook->function, ptrauth_key_asia);
#endif
        void *guardOrig = NULL;
        uint64_t generation = 0;
        hk_guard_result_t guard = hk_inline_guard_reserve(hk_inline_guard_key(hook->function),
                                                          hook->replacement,
                                                          [self typeForBackend:candidate],
                                                          &generation,
                                                          &guardOrig);
        switch(guard) {
            case HK_GUARD_BLOCKED:
            case HK_GUARD_FULL:
                // This inline writer cannot own the prologue. Continue to a
                // later route (normally an import-slot rebind).
                continue;

            case HK_GUARD_DUP_PENDING:
            case HK_GUARD_DUP_TAINTED:
                hook->status = HK_ERR;
                return NO;

            case HK_GUARD_DUP_INSTALLED:
                if(!hook->original.callerCell) {
                    hook->status = HK_OK;
                } else if(guardOrig) {
                    *hook->original.callerCell = guardOrig;
                    hook->original.value = guardOrig;
                    hook->status = HK_OK;
                } else {
                    hook->status = HK_ERR;
                }
                return NO;

            case HK_GUARD_RESERVED:
                hook->guardToken = generation;
                return YES;
        }
    }
}

// Default memory routing follows the descriptor priority and retries only a
// backend that truthfully reports NOT_SUPPORTED (the public nothing-written
// contract). Explicit backend instances never call this helper.
- (BOOL)routeMemoryOperation:(HKHookOperation *)hook {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(NSUInteger i = hook->routeCursor; i < count; i++) {
        if(!(table[i].kinds & HK_CAT_MEMORY) || !table[i].available()) {
            continue;
        }

        hook->routedBackend = [table[i].backendClass new];
        hook->routeCursor = i + 1;
        hook->backendErrno = 0;
        hook->technique = HKFunctionTechniqueNone;
        return YES;
    }

    hook->routedBackend = nil;
    hook->backendErrno = 0;
    hook->status = HK_ERR_NOT_SUPPORTED;
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
    BOOL facadeMessage = kind == HKHookKindMessage;
    BOOL automaticMemory = kind == HKHookKindMemory && automaticSelection;

    if(!backend && !facadeMessage) {
        *outStatus = HK_ERR_NOT_SUPPORTED;
    } else if(!facadeMessage && !automaticMemory && !hk_descriptor_kind_supported(backend, kind)) {
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
    }]) {
        return status;
    }

    // Immediate path: the same publication cycle a drained message op runs —
    // the original is published into the caller's cell BEFORE the
    // class_replaceMethod swap lands (C1), so the caller observes it from the
    // moment activation happens.
    HKOriginalPublication publication = { .callerCell = old_ptr };
    hk_original_begin(&publication);
    hookkit_status_t result = hk_apply_message_hook(dispatchClass, selector, replacement, &publication);
    result = hk_original_finish(&publication, result);

    // Message hooks are implemented by the Objective-C runtime path above;
    // there is no backend errno/type to attribute.
    [self noteHookResult:result fromBackend:nil];
    return result;
}

- (hookkit_status_t)hookFunction:(void *)function withReplacement:(void *)replacement outOldPtr:(void **)old_ptr {
    if(!function || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }

    if(autoCoverCategories) {
        HKHookOperation *hook = [HKHookOperation new];
        hook->kind = HKHookKindFunction;
        hook->function = function;
        hook->replacement = replacement;
        hook->original.callerCell = old_ptr;
        hook->automaticRoute = YES;
        hook->routeAtDrain = batching && autoCoverCategories.count == 1 &&
            [autoCoverCategories.firstObject isKindOfClass:[NSNumber class]] &&
            autoCoverCategories.firstObject.unsignedIntegerValue == HK_CAT_FUNCTION_REBIND;

        if(!hook->routeAtDrain && ![self routeFunctionOperation:hook]) {
            [self noteHookResult:hook->status fromBackend:hook->routedBackend];
            return hook->status;
        }

        if(batching) {
            @synchronized(self) {
                [batchHooks addObject:hook];
            }
            [self noteHookResult:HK_OK];
            return HK_OK;
        }

        for(;;) {
            id<HKSubstitutorBackend> routeBackend = hook->routedBackend;
            hookkit_status_t result = [self executeOperation:hook onBackend:routeBackend];

            if(hook->guardToken) {
                hk_inline_guard_update(hk_inline_guard_key(hook->function), hook->guardToken,
                                       result == HK_OK ? 0 : (result == HK_ERR_NOT_SUPPORTED ? 2 : 1),
                                       hook->original.value);
                hook->guardToken = 0;
            }

            if(result != HK_ERR_NOT_SUPPORTED || ![self routeFunctionOperation:hook]) {
                [self noteHookResult:hook->status fromBackend:hook->routedBackend ?: routeBackend];
                return hook->status;
            }
        }
    }

    id<HKSubstitutorBackend> routeBackend = backend;
    HKFunctionTechnique technique = [self resolvedTechniqueForBackend:routeBackend];

    if(!hk_descriptor_kind_supported(routeBackend, HKHookKindFunction) ||
       ![routeBackend respondsToSelector:@selector(hookFunction:withReplacement:outOldPtr:)]) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
        return HK_ERR_NOT_SUPPORTED;
    }

    if(old_ptr) {
        HKOriginalPublicationPolicy policy = hk_resolved_publication_policy([self typeForBackend:routeBackend], technique);
        if(policy == HKOriginalPublicationAfterActivation || policy == HKOriginalPublicationUnavailable) {
            [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
            return HK_ERR_NOT_SUPPORTED;
        }
    }

    if(!hk_shared_inline_preflight_ok(routeBackend, technique, function, replacement)) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
        return HK_ERR_NOT_SUPPORTED;
    }

    uint64_t guardGeneration = 0;
    if(technique == HKFunctionTechniqueInline) {
#if __has_feature(ptrauth_calls)
        function = ptrauth_strip(function, ptrauth_key_asia);
#endif
        void *guardOrig = NULL;
        hk_guard_result_t guard = hk_inline_guard_reserve(hk_inline_guard_key(function), replacement,
                                                          [self typeForBackend:routeBackend],
                                                          &guardGeneration, &guardOrig);
        switch(guard) {
            case HK_GUARD_BLOCKED:
            case HK_GUARD_FULL:
                [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
                return HK_ERR_NOT_SUPPORTED;
            case HK_GUARD_DUP_PENDING:
            case HK_GUARD_DUP_TAINTED:
                [self noteHookResult:HK_ERR fromBackend:nil];
                return HK_ERR;
            case HK_GUARD_DUP_INSTALLED:
                if(!old_ptr) {
                    [self noteHookResult:HK_OK fromBackend:routeBackend];
                    return HK_OK;
                }
                if(guardOrig) {
                    *old_ptr = guardOrig;
                    [self noteHookResult:HK_OK fromBackend:routeBackend];
                    return HK_OK;
                }
                [self noteHookResult:HK_ERR fromBackend:nil];
                return HK_ERR;
            case HK_GUARD_RESERVED:
                break;
        }
    }

    hookkit_status_t status;

    if([self enqueueKind:HKHookKindFunction status:&status build:^(HKHookOperation *hook) {
        hook->function = function;
        hook->replacement = replacement;
        hook->original.callerCell = old_ptr;
        hook->guardToken = guardGeneration;   // 0 = not inline-guarded
        hook->technique = technique;   // ACTUAL resolved technique (see above)
    }]) {
        return status;
    }

    // Immediate path: the same publication cycle a drained function op runs
    // (see executeOperation:) — hk_original_begin saves and NULLs the
    // caller's cell, the backend writes the original through
    // hk_original_output_cell (the caller's ACTUAL cell when requested, NULL
    // when not — the vendor's NULL-oldptr mode, which may permit smaller
    // hooks), publish records it, and finish applies the only-real-original
    // invariant. The caller's cell therefore holds the original BEFORE the
    // replacement becomes reachable — there is no copy-after-activation in
    // the facade. The raw backend status goes straight through finish, which
    // converts an OK-with-no-original to HK_ERR only when the caller
    // REQUESTED one (a litehook-inline install with no original is success
    // when nothing was requested).
    HKOriginalPublication publication = { .callerCell = old_ptr };
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

    if(automaticSelection) {
        HKHookOperation *hook = [HKHookOperation new];
        hook->kind = HKHookKindMemory;
        hook->target = target;
        hook->data = [NSData dataWithBytes:data length:size];
        hook->size = size;
        hook->automaticRoute = YES;

        if(![self routeMemoryOperation:hook]) {
            [self noteHookResult:hook->status fromBackend:nil];
            return hook->status;
        }

        if(batching) {
            @synchronized(self) {
                [batchHooks addObject:hook];
            }
            [self noteHookResult:HK_OK];
            return HK_OK;
        }

        for(;;) {
            id<HKSubstitutorBackend> routeBackend = hook->routedBackend;
            hookkit_status_t result = [self executeOperation:hook onBackend:routeBackend];

            if(result != HK_ERR_NOT_SUPPORTED || ![self routeMemoryOperation:hook]) {
                [self noteHookResult:hook->status fromBackend:hook->routedBackend ?: routeBackend];
                return hook->status;
            }
        }
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

    hookkit_status_t result = [backend respondsToSelector:@selector(hookMemory:withData:size:)]
        ? [backend hookMemory:target withData:data size:size]
        : HK_ERR_NOT_SUPPORTED;
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
    HKOriginalPublication publication = { .callerCell = old_ptr };
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
    HKOriginalPublication publication = { .callerCell = old_ptr };
    hk_original_begin(&publication);
    void **cell = hk_original_output_cell(&publication);
    hookkit_status_t result = [backend hookSwiftVtableSlotInClass:objcClass withIndex:index withReplacement:replacement outOldPtr:cell];
    hk_original_publish(&publication, cell ? *cell : NULL);
    result = hk_original_finish(&publication, result);

    [self noteHookResult:result];
    return result;
}

static const uint32_t HKImageWrapperMagic = 0x484B494D;   // 'HKIM'

- (HKImageRef)openImage:(NSString *)path {
    if(![path isKindOfClass:[NSString class]] || !backend ||
       ![backend respondsToSelector:@selector(openImage:)] ||
       ![backend respondsToSelector:@selector(closeImage:)] ||
       ![backend respondsToSelector:@selector(findSymbolInImage:symbolName:)]) {
        return NULL;
    }

    void *rawHandle = (void *)[backend openImage:path];

    if(!rawHandle) {
        return NULL;
    }

    struct HKImage *wrapper = calloc(1, sizeof(struct HKImage));

    if(!wrapper) {
        if([backend respondsToSelector:@selector(closeImage:)]) {
            [backend closeImage:rawHandle];
        }
        return NULL;
    }

    wrapper->magic = HKImageWrapperMagic;
    wrapper->ownerType = [self typeForBackend:backend];
    wrapper->rawHandle = rawHandle;
    return (HKImageRef)wrapper;
}

- (void)closeImage:(HKImageRef)image {
    if(!backend || !image || ![backend respondsToSelector:@selector(closeImage:)]) {
        return;
    }

    struct HKImage *wrapper = (struct HKImage *)image;

    if(wrapper->magic != HKImageWrapperMagic || wrapper->ownerType != [self typeForBackend:backend]) {
        return;
    }

    [backend closeImage:wrapper->rawHandle];
    free(wrapper);
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

    if(backend && image) {
        struct HKImage *wrapper = (struct HKImage *)image;
        if(wrapper->magic != HKImageWrapperMagic || wrapper->ownerType != [self typeForBackend:backend]) {
            return HK_ERR_INVALID_ARGUMENT;
        }
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

    if(!backend || ![backend respondsToSelector:@selector(findSymbolInImage:symbolName:)]) {
        return NULL;
    }

    if(!image) {
        return [backend findSymbolInImage:NULL symbolName:symbolName];
    }

    struct HKImage *wrapper = (struct HKImage *)image;
    if(wrapper->magic != HKImageWrapperMagic || wrapper->ownerType != [self typeForBackend:backend]) {
        return NULL;
    }

    return [backend findSymbolInImage:wrapper->rawHandle symbolName:symbolName];
}

// Single-op sequential execution for backends without a native batch
// primitive: run one operation's full publication cycle — hk_original_begin
// (saves and NULLs the caller's cell) → the backend call (function/memory)
// or the central runtime message hook, both through the publication's output
// cell → publish what was produced → hk_original_finish (applies the
// publish-only-real-original invariant). The op's status and backendErrno
// carry the final outcome.
- (hookkit_status_t)executeOperation:(HKHookOperation *)hook onBackend:(id<HKSubstitutorBackend>)resultBackend {
    hook->backendErrno = 0;
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
            result = [resultBackend respondsToSelector:@selector(hookFunction:withReplacement:outOldPtr:)]
                ? [resultBackend hookFunction:hook->function withReplacement:hook->replacement outOldPtr:cell]
                : HK_ERR_NOT_SUPPORTED;
            break;

        case HKHookKindMemory:
            result = [resultBackend respondsToSelector:@selector(hookMemory:withData:size:)]
                ? [resultBackend hookMemory:hook->target withData:[hook->data bytes] size:hook->size]
                : HK_ERR_NOT_SUPPORTED;
            break;
    }

    // Record what the backend produced (idempotent when it wrote straight
    // into the caller's cell; marks published so finish's invariant passes).
    hk_original_publish(&hook->original, cell ? *cell : NULL);
    result = hk_original_finish(&hook->original, result);
    hook->status = result;

    if(result != HK_OK && hook->kind != HKHookKindMessage) {
        hook->backendErrno = [resultBackend lastErrno];
    }

    return result;
}

// Drain one group of enqueued ops through a single backend (the pinned backend
// for a normal substitutor, or one router-picked backend for an auto-cover one).
// Native-batch when the backend supports it — begin every publication up front,
// hand the group to the backend, publish/finalize per op — else run each op
// through the single-op helper. Extracted from executeHooks so the grouped
// auto-cover path and the single-backend fast path share one implementation.
- (void)drainGroup:(NSArray<HKHookOperation *> *)hooks onBackend:(id<HKSubstitutorBackend>)groupBackend {
    if(hooks.count && ((HKHookOperation *)hooks.firstObject)->kind == HKHookKindFunction &&
       [groupBackend respondsToSelector:@selector(executeFunctionHooks:)]) {
        for(HKHookOperation *hook in hooks) {
            hook->backendErrno = 0;
            hk_original_begin(&hook->original);
        }

        [groupBackend executeFunctionHooks:(NSArray<HKFunctionBatchItem *> *)hooks];

        for(HKHookOperation *hook in hooks) {
            void **cell = hk_original_output_cell(&hook->original);
            hk_original_publish(&hook->original, cell ? *cell : NULL);
            hook->status = hk_original_finish(&hook->original, hook->status);
        }
    } else {
        for(HKHookOperation *hook in hooks) {
            [self executeOperation:hook onBackend:groupBackend];
        }
    }
}

// Automatic batches may contain several backends and techniques. Coalesce
// only identical (backend class, hook kind, technique) routes: litehook
// inline and litehook rebind are the same class but different engines.
- (void)drainRoutedHooks:(NSArray<HKHookOperation *> *)hooks {
    NSMutableArray<id<HKSubstitutorBackend>> *reps = [NSMutableArray new];
    NSMutableArray<HKHookOperation *> *heads = [NSMutableArray new];
    NSMutableArray<NSMutableArray<HKHookOperation *> *> *groups = [NSMutableArray new];

    for(HKHookOperation *hook in hooks) {
        id<HKSubstitutorBackend> groupBackend = hook->routedBackend ?: backend;
        NSUInteger idx = NSNotFound;

        for(NSUInteger i = 0; i < reps.count; i++) {
            HKHookOperation *head = heads[i];
            if([reps[i] class] == [groupBackend class] &&
               head->kind == hook->kind && head->technique == hook->technique) {
                idx = i;
                break;
            }
        }

        if(idx == NSNotFound) {
            [reps addObject:groupBackend];
            [heads addObject:hook];
            [groups addObject:[NSMutableArray new]];
            idx = reps.count - 1;
        }

        [groups[idx] addObject:hook];
    }

    for(NSUInteger i = 0; i < reps.count; i++) {
        [self drainGroup:groups[i] onBackend:reps[i]];
    }
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

    // Rebind-only routes resolve against the image set at drain rather than
    // enqueue. Other automatic routes already carry their first candidate.
    NSMutableArray<HKHookOperation *> *dispatchHooks = [NSMutableArray arrayWithCapacity:hooks.count];
    for(HKHookOperation *hook in hooks) {
        if(hook->routeAtDrain) {
            hook->routeAtDrain = NO;
            if(![self routeFunctionOperation:hook]) {
                continue;
            }
        }
        [dispatchHooks addObject:hook];
    }

    if((!automaticSelection && !autoCoverCategories) || !backend) {
        [self drainGroup:dispatchHooks onBackend:backend];
    } else {
        [self drainRoutedHooks:dispatchHooks];
    }

    // Retry automatic operations in grouped waves. Only NOT_SUPPORTED enters
    // another wave; OK, PARTIAL and ERR are terminal because they may have
    // changed the target.
    NSArray<HKHookOperation *> *wave = dispatchHooks;
    for(;;) {
        NSMutableArray<HKHookOperation *> *retry = [NSMutableArray new];

        for(HKHookOperation *hook in wave) {
            if(!hook->automaticRoute) {
                continue;
            }

            if(hook->guardToken && hook->kind == HKHookKindFunction) {
                hk_inline_guard_update(hk_inline_guard_key(hook->function), hook->guardToken,
                                       hook->status == HK_OK ? 0 : (hook->status == HK_ERR_NOT_SUPPORTED ? 2 : 1),
                                       hook->original.value);
                hook->guardToken = 0;
            }

            if(hook->status != HK_ERR_NOT_SUPPORTED) {
                continue;
            }

            BOOL shouldDispatch = hook->kind == HKHookKindFunction
                ? [self routeFunctionOperation:hook]
                : [self routeMemoryOperation:hook];
            if(shouldDispatch) {
                [retry addObject:hook];
            }
        }

        if(!retry.count) {
            break;
        }

        [self drainRoutedHooks:retry];
        wave = retry;
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
                lastLibErrnoType = [self typeForBackend:hook->routedBackend ?: backend];
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
