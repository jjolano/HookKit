#import <HookKit/Compat.h>
#import "Internal/HKBackendInternal.h"
#import "Internal/HKInlineGuard.h"
#import "Internal/HKInlinePreflight.h"

#import <objc/runtime.h>

#import "native/hk_swift.h"

// Owned here (resolved by swift_available() via dlsym); consumed by the
// engine's name-based lookup (declared in native/hk_swift.h).
hk_swift_demangle_fn hk_swift_demangle = NULL;

// Inline-ownership guard scope: a backend is an inline writer when its hook
// overwrites the target's prologue bytes. native/Dobby/Frida/ElleKit/
// Substrate/Substitute always do for function hooks; litehook only in
// HKStrategyInline mode — its default rebind path is GOT/import-scoped and
// its memory path is a byte blob, neither of which touches the prologue, so
// both stay unguarded. fishhook (rebind) and the Swift backend (vtable
// metadata) are never inline writers either.
static BOOL hk_backend_is_inline_writer(id<HKSubstitutorBackend> backend) {
    if(!backend) {
        return NO;
    }

    // The registry table is the single source of truth for the fixed backends.
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(size_t i = 0; i < count; i++) {
        if(![backend isKindOfClass:table[i].backendClass]) {
            continue;
        }

        switch(table[i].type) {
            case HK_LIB_NATIVE:
            case HK_LIB_DOBBY:
            case HK_LIB_FRIDA:
            case HK_LIB_ELLEKIT:
            case HK_LIB_SUBSTRATE:
            case HK_LIB_SUBSTITUTE:
                return YES;

            case HK_LIB_LITEHOOK:
                // Inline only when the active technique says so: rebind and
                // memory never write the prologue.
                return [backend respondsToSelector:@selector(strategy)] && [(id)backend strategy] == HKStrategyInline;

            default:
                return NO;
        }
    }

    return NO;
}

// Side-effect-free discovery for one backend, used by the availability-
// introspection entry points: the dlopen-based backends report through their
// preflight-only *_discoverable() variants (dlopen_preflight never maps the
// image and never runs its constructors, gum_init_embedded included).
// Backends WITHOUT a discoverable variant are reported unavailable here —
// their available() probes are never consulted on the safe path (the Swift
// probe dlopens libswiftCore, the MS/ElleKit/Frida probes dlopen the
// provider). The real dlopen happens only on the actual hook path
// (initLibraries / defaultBackend / auto-cover), which keeps using the full
// available() probes.
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

        default:
            return NO;
    }
}

// Conservative shared prologue check for inline-capable backends that bring
// no preflightFunction: of their own (the MS-compatible providers: ElleKit,
// Substrate, Substitute). Runs the shared fixed-window validator with the
// LARGEST overwrite window any inline backend uses (litehook's 20 bytes), so
// a target that passes here is safe for every smaller (16-byte) window too.
// Returns YES when the backend may be dispatched, NO to refuse the hook.
// Backends that implement preflightFunction: guarantee their own checks
// (their hook paths run the same validator), so they are never double-checked
// here; non-inline backends have no prologue to validate.
static BOOL hk_shared_inline_preflight_ok(id<HKSubstitutorBackend> backend, void *function, void *replacement) {
    if(!hk_backend_is_inline_writer(backend)) {
        return YES;
    }

    if([backend respondsToSelector:@selector(preflightFunction:withReplacement:)]) {
        return YES;
    }

    return hk_inline_preflight(function, replacement, HK_INLINE_PREFLIGHT_LITEHOOK_WINDOW, NULL) == HK_OK;
}

// Deferred idempotent-rehook settlement record (see hookFunction:): a dup of
// a queued-but-not-executed inline hook has no original yet, so the caller's
// out-pointer cannot be published synchronously. The pointer is borrowed
// under the batch contract (the caller's storage is valid until executeHooks
// returns) and is written exactly once, inside executeHooks, never retained
// past it.
@interface HKGuardWaiter : NSObject {
@public
    uintptr_t address;
    void *replacement;
    int backendType;
    void **callerOrig;
}
@end

@implementation HKGuardWaiter
@end

#pragma mark - HKSubstitutor

@interface HKSubstitutor ()
- (void)noteHookResult:(hookkit_status_t)status fromBackend:(id<HKSubstitutorBackend>)resultBackend;
- (hookkit_lib_t)backendType;
- (BOOL)enqueueKind:(HKHookKind)kind status:(hookkit_status_t *)outStatus build:(void (^)(HKHookOperation *hook))build;
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
    // Deferred idempotent-rehook settlements (HKGuardWaiter): a dup of a
    // queued-but-not-executed inline hook has no original until the owning
    // hook drains, so the caller's out-pointer is settled in executeHooks.
    NSMutableArray<HKGuardWaiter *> *dupWaiters;
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
        dupWaiters = [NSMutableArray new];
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
        // category.
        size_t count = 0;
        const HKBackendDescriptor *table = hk_backends(&count);

        types = HK_LIB_NONE;

        for(NSNumber *num in orderedCategories) {
            // Trust boundary: the list is caller-supplied. Skip malformed
            // elements (non-NSNumber) instead of raising at
            // unsignedIntegerValue.
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
                            if(table[i].type == picker.type && table[i].available()) {
                                backend = [table[i].backendClass new];

                                if([backend respondsToSelector:@selector(setStrategy:)]) {
                                    [backend setStrategy:picker.strategy];
                                }

                                resolvedStrategy = picker.strategy;
                                types |= table[i].type;
                                goto category_done;
                            }
                        }
                    }
                }
            }
        }

category_done: ;
    } else if(autoCoverCategories) {
        // Auto-cover mode: no single backend is authoritative. Pin the first
        // available category backend as the fallback used by the batched path
        // and the image APIs; non-batched function hooks route per-hook via
        // preflight instead (see hk_backendForAutoCoverFunction:).
        size_t count = 0;
        const HKBackendDescriptor *table = hk_backends(&count);

        types = HK_LIB_NONE;
        resolvedStrategy = HKStrategyDefault;

        for(NSNumber *num in autoCoverCategories) {
            hookkit_cat_t want = (hookkit_cat_t)num.unsignedIntegerValue;

            if(!want) {
                continue;
            }

            for(size_t c = 0; c < hk_category_priority_count; c++) {
                if(hk_category_priorities[c].category & want) {
                    for(size_t o = 0; o < hk_category_priorities[c].count; o++) {
                        HKCategoryPicker picker = hk_category_priorities[c].order[o];

                        for(size_t i = 0; i < count; i++) {
                            if(table[i].type == picker.type && table[i].available()) {
                                backend = [table[i].backendClass new];

                                if([backend respondsToSelector:@selector(setStrategy:)]) {
                                    [backend setStrategy:picker.strategy];
                                }

                                resolvedStrategy = picker.strategy;
                                types |= table[i].type;
                                goto auto_cover_done;
                            }
                        }
                    }
                }
            }
        }

auto_cover_done: ;
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
    // nil is still an explicit ordered request: treat it as empty, never as unset
    substitutor->orderedTypes = [types copy] ?: @[];
    [substitutor initLibraries];
    return substitutor;
}

+ (instancetype)substitutorWithCategory:(hookkit_cat_t)category {
    // single-element fallback list: shares the ordered resolution loop
    return [self substitutorWithOrderedCategories:@[@(category)]];
}

+ (instancetype)substitutorWithOrderedCategories:(NSArray<NSNumber *> *)categories {
    HKSubstitutor *substitutor = [self new];
    // nil is still an explicit ordered request: treat it as empty, never as unset
    substitutor->orderedCategories = [categories copy] ?: @[];
    [substitutor initLibraries];
    return substitutor;
}

+ (instancetype)substitutorWithAutoCoverCategories:(NSArray<NSNumber *> *)categories {
    HKSubstitutor *substitutor = [self new];
    // nil is still an explicit request: treat it as empty, never as unset
    substitutor->autoCoverCategories = [categories copy] ?: @[];
    // Auto-cover resolves per-hook, so no backend is pinned at init.
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
// when every picker declined (or no category matched).
//
// Deliberately preflight-driven only: it never inspects a hook RESULT to
// decide routing (a failed invocation may have mutated the target). A backend
// without preflightFunction: is presumed to accept only when it is not an
// inline writer — inline-capable backends without a vendor preflight (the MS
// providers) are gated by the shared conservative prologue check
// (hk_shared_inline_preflight_ok), so no inline-capable dispatch ever
// proceeds unguarded.
- (id<HKSubstitutorBackend>)hk_backendForAutoCoverFunction:(void *)function replacement:(void *)replacement {
    size_t count = 0;
    const HKBackendDescriptor *table = hk_backends(&count);

    for(NSNumber *num in autoCoverCategories) {
        // Trust boundary: the list is caller-supplied. Skip malformed
        // elements (non-NSNumber) instead of raising at
        // unsignedIntegerValue.
        if(![num isKindOfClass:[NSNumber class]]) {
            NSLog(@"[HookKit] warning: %s in auto-cover category list is not an NSNumber; skipped", class_getName([num class]));
            continue;
        }

        hookkit_cat_t want = (hookkit_cat_t)num.unsignedIntegerValue;

        if(!want) {
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

                        if(![candidate respondsToSelector:@selector(preflightFunction:withReplacement:)]) {
                            // No vendor veto channel: enforce the shared
                            // conservative prologue check for inline-capable
                            // backends (the MS providers: ElleKit, Substrate,
                            // Substitute), so a target no inline backend could
                            // safely overwrite is DECLINED here — the walk
                            // continues to the next picker instead of
                            // dispatching unguarded. Non-inline backends
                            // without a preflight (rebind paths) accept.
                            if(!hk_shared_inline_preflight_ok(candidate, function, replacement)) {
                                continue;
                            }

                            return candidate;
                        }

                        hookkit_status_t verdict = [candidate preflightFunction:function withReplacement:replacement];

                        if(verdict == HK_OK) {
                            return candidate;
                        }
                    }
                }
            }
        }
    }

    return nil;
}

// Shared tail of the three batching-capable hook entry points: backend
// presence, the batching decision, the kind check and the enqueue. Returns YES
// when the call is fully handled (result in *outStatus), NO when the caller
// should hook immediately. `build` fills in the kind-specific fields and only
// runs on the enqueue path, so a non-batched hook allocates nothing extra.
//
// The caller's argument guard deliberately stays at the call site, ahead of
// this: hookMemory: copies the patch bytes in `build`, and dataWithBytes:NULL
// would crash before a guard in here could reject it.
- (BOOL)enqueueKind:(HKHookKind)kind status:(hookkit_status_t *)outStatus build:(void (^)(HKHookOperation *hook))build {
    if(!backend) {
        *outStatus = HK_ERR_NOT_SUPPORTED;
    } else if(!batching || ![backend batchingSupported]) {
        return NO;
    } else if(![backend supportsHookKind:kind]) {
        *outStatus = HK_ERR_NOT_SUPPORTED;
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
    // backends use class_getInstanceMethod(), which walks the metaclass's
    // inheritance tree exactly like the class's own. Instance methods pass the
    // class through unchanged. (A selector that exists on neither the class
    // nor the metaclass is left to the backend's own NOT_SUPPORTED check.)
    Class dispatchClass = class_getInstanceMethod(objcClass, selector) ? objcClass : object_getClass(objcClass);

    hookkit_status_t status;

    if([self enqueueKind:HKHookKindMessage status:&status build:^(HKHookOperation *hook) {
        hook->objcClass = dispatchClass;
        hook->selector = selector;
        hook->replacement = replacement;
        hook->callerOrig = old_ptr;
    }]) {
        return status;
    }

    // owned cell: the backend never touches the caller's pointer directly
    void *cell = NULL;
    hookkit_status_t result = [backend hookMessageInClass:dispatchClass withSelector:selector withReplacement:replacement outOldPtr:&cell];

    // Publish-only-non-NULL-original invariant: check the status FIRST, then
    // the cell. A backend that reports success without writing a real
    // original has broken its contract (the MS APIs are void, so the cell is
    // the only observable success signal) — treat as a hard failure and
    // publish nothing.
    if(result == HK_OK && !cell) {
        result = HK_ERR;
    }

    if(result == HK_OK && old_ptr) {
        *old_ptr = cell;
    }

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
    // routing is not enabled, `backend` is the pinned resolution.
    id<HKSubstitutorBackend> routeBackend = backend;

    if(autoCoverCategories && !batching) {
        routeBackend = [self hk_backendForAutoCoverFunction:function replacement:replacement];

        if(!routeBackend) {
            // Every picker declined the target: no backend can hook it safely.
            // No backend-specific detail to report — clear any stale state.
            [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:nil];
            return HK_ERR_NOT_SUPPORTED;
        }
    }

    // Shared conservative prologue check: an inline-capable backend without a
    // preflightFunction: of its own (ElleKit/Substrate/Substitute) is only
    // dispatched when the shared fixed-window validator accepts the target —
    // the same checks the Dobby/litehook/native backends run themselves. This
    // covers every inline-capable dispatch path — auto-cover (already declined
    // in the router, re-checked here), explicit inline, and the batched
    // enqueue — and refuses BEFORE any guard reservation or write, so a
    // rejected target never reaches a backend unguarded.
    if(!hk_shared_inline_preflight_ok(routeBackend, function, replacement)) {
        [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
        return HK_ERR_NOT_SUPPORTED;
    }

    // Process-wide inline-ownership guard: prevents HookKit-vs-HookKit
    // contention — two substitutors (or one hooking twice) installing
    // DIFFERENT inline hooks on the same address through DIFFERENT inline
    // backends would double-patch one prologue. Only inline writers are
    // guarded: rebind paths (fishhook/litehook-rebind) are GOT-scoped and
    // memory patches are byte blobs — neither overwrites the prologue, so
    // both stay unguarded (ponytail: rebind-vs-inline on one address is a
    // same-slot double-write only if the prologue is the GOT slot, which
    // never happens for function pointers).
    uintptr_t guardAddr = 0;

    if(hk_backend_is_inline_writer(routeBackend)) {
        // Normalize the key exactly as the backend will write: strip PAC on
        // arm64e, mask the thumb bit on 32-bit ARM. The same key is stored
        // on the op so executeHooks can update the guard without re-deriving
        // it (ptrauth_strip needs the raw pointer, which the backend may
        // have consumed by then).
#if __has_feature(ptrauth_calls)
        function = ptrauth_strip(function, ptrauth_key_asia);
#endif
#if defined(__arm__)
        uintptr_t addr = (uintptr_t)function & ~(uintptr_t)1;
#else
        uintptr_t addr = (uintptr_t)function;
#endif
        guardAddr = addr;

        void *guardOrig = NULL;
        hk_guard_result_t guard = hk_inline_guard_reserve(addr, replacement, [self typeForBackend:routeBackend], &guardOrig);

        if(guard == HK_GUARD_BLOCKED) {
            // Already inline-hooked by another HookKit backend with a
            // DIFFERENT replacement — invoking this backend would double-
            // patch the prologue. Nothing was written.
            [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
            return HK_ERR_NOT_SUPPORTED;
        }

        if(guard == HK_GUARD_FULL) {
            // The ownership table is full and no entry matched: nothing was
            // reserved, so proceeding would install an UNGUARDED hook — the
            // one outcome worse than declining it. Refuse; nothing was
            // written.
            [self noteHookResult:HK_ERR_NOT_SUPPORTED fromBackend:routeBackend];
            return HK_ERR_NOT_SUPPORTED;
        }

        if(guard == HK_GUARD_DUP) {
            // Idempotent same-replacement re-hook: the hook is already
            // installed, so the saved original is the answer. A NULL saved
            // original means the owning entry is still PENDING (its hook is
            // queued in a batch and has not executed yet — the original does
            // not exist) or tainted (original unknowable). While this call
            // will itself be batched, defer the publish: executeHooks settles
            // the caller's cell from the guard after the drain, when the
            // owner's original exists.
            if(!guardOrig && old_ptr && batching && backend && [backend batchingSupported] && [backend supportsHookKind:HKHookKindFunction]) {
                HKGuardWaiter *waiter = [HKGuardWaiter new];
                waiter->address = addr;
                waiter->replacement = replacement;
                waiter->backendType = [self typeForBackend:routeBackend];
                waiter->callerOrig = old_ptr;

                @synchronized(self) {
                    [dupWaiters addObject:waiter];
                }

                [self noteHookResult:HK_OK fromBackend:routeBackend];
                return HK_OK;
            }

            if(old_ptr) {
                *old_ptr = guardOrig;
            }

            [self noteHookResult:HK_OK fromBackend:routeBackend];
            return HK_OK;
        }

        // HK_GUARD_OK: entry reserved; the hook proceeds. The entry is
        // settled below (immediate path) or in executeHooks (batched path).
    }

    hookkit_status_t status;

    if([self enqueueKind:HKHookKindFunction status:&status build:^(HKHookOperation *hook) {
        hook->function = function;
        hook->replacement = replacement;
        hook->callerOrig = old_ptr;
        hook->guardAddr = guardAddr;    // 0 = not inline-guarded
    }]) {
        return status;
    }

    // owned cell: the backend never touches the caller's pointer directly
    void *cell = NULL;
    hookkit_status_t result = [routeBackend hookFunction:function withReplacement:replacement outOldPtr:&cell];

    // Publish-only-non-NULL-original invariant: check the status FIRST, then
    // the cell. A backend that reports success without writing a real
    // original has broken its contract and the outcome is unknowable (the
    // prologue may be written) — treat as a hard failure so the guard taints
    // and nothing is published.
    if(result == HK_OK && !cell) {
        result = HK_ERR;
    }

    if(guardAddr) {
        // Settle the guard with the actual outcome: OK stores the saved
        // original, HK_ERR taints (the prologue may be half-written), and
        // NOT_SUPPORTED releases the entry (the backend wrote nothing).
        hk_inline_guard_update(guardAddr, result, cell);
    }

    if(result == HK_OK && old_ptr) {
        *old_ptr = cell;
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
    if(!objcClass || !name || ![name length] || !replacement) {
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

    // owned cell: the backend never touches the caller's pointer directly
    void *cell = NULL;
    hookkit_status_t result = [backend hookSwiftMethodInClass:objcClass withName:name withReplacement:replacement outOldPtr:&cell];

    if(result == HK_OK && old_ptr) {
        *old_ptr = cell;
    }

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

    // owned cell: the backend never touches the caller's pointer directly
    void *cell = NULL;
    hookkit_status_t result = [backend hookSwiftVtableSlotInClass:objcClass withIndex:index withReplacement:replacement outOldPtr:&cell];

    if(result == HK_OK && old_ptr) {
        *old_ptr = cell;
    }

    [self noteHookResult:result];
    return result;
}

- (HKImageRef)openImage:(NSString *)path {
    if(!path) {
        return NULL;
    }

    if(!backend) {
        return NULL;
    }

    return [backend openImage:path];
}

- (void)closeImage:(HKImageRef)image {
    if(backend && image) {
        [backend closeImage:image];
    }
}

- (hookkit_status_t)findSymbolsInImage:(HKImageRef)image symbolNames:(NSArray<NSString *> *)symbolNames outSymbols:(NSArray<NSValue *> **)outSymbols {
    if(!symbolNames || ![symbolNames count] || !outSymbols) {
        return HK_ERR_INVALID_ARGUMENT;
    }

    NSMutableArray *outSyms = [NSMutableArray new];
    NSUInteger found = 0;

    for(NSString *symbolName in symbolNames) {
        // Trust boundary: symbolNames is caller-supplied. A malformed element
        // (non-NSString) would raise an uncaught NSInvalidArgumentException at
        // length/UTF8String inside findSymbolInImage: — reject with the
        // invalid-argument convention instead of raising.
        if(![symbolName isKindOfClass:[NSString class]]) {
            NSLog(@"[HookKit] warning: %s in symbolNames is not an NSString; rejected", class_getName([symbolName class]));
            return HK_ERR_INVALID_ARGUMENT;
        }

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
    if(!symbolName || ![symbolName length]) {
        return NULL;
    }

    if(!backend) {
        return NULL;
    }

    return [backend findSymbolInImage:image symbolName:symbolName];
}

- (hookkit_status_t)executeHooks {
    NSArray<HKHookOperation *> *hooks;

    @synchronized(self) {
        if(![batchHooks count] && ![dupWaiters count]) {
            [self noteHookResult:HK_OK];
            return HK_OK;
        }

        hooks = [batchHooks copy];
        [batchHooks removeAllObjects];
    }

    // An empty drain (waiters only: a deferred dup whose owner hooks in
    // another substitutor) has nothing for the backend to execute.
    hookkit_status_t result = HK_OK;

    if([hooks count]) {
        result = backend ? [backend executeHooks:hooks] : HK_ERR_NOT_SUPPORTED;
    }

    // copy per-op results back to the callers and drop all borrowed references
    for(HKHookOperation *hook in hooks) {
        // Batch form of the publish-only-non-NULL-original invariant: an op
        // the backend marked succeeded MUST have written a real original into
        // the batch-owned cell (origValue — storage that lives for the full
        // drain duration; the drained ops are retained until this loop
        // finishes). A succeeded op with a NULL cell has an unknowable
        // outcome — demote it to failure so the guard taints and the caller's
        // cell is never written a NULL "original". Memory ops have no
        // original by design.
        if(hook->succeeded && !hook->origValue && hook->kind != HKHookKindMemory) {
            hook->succeeded = NO;
        }

        // Settle the inline guard for guarded function ops with the batch's
        // per-op outcome. NOT_SUPPORTED never comes out of executeHooks (only
        // the succeeded flag), so taint-on-failure is the honest contract:
        // ponytail: a failed op taints (HK_ERR) rather than releasing — the
        // batch API cannot distinguish "backend wrote nothing" from "backend
        // wrote part of the prologue", so blocking later different hooks is
        // the safe approximation.
        if(hook->guardAddr && hook->kind == HKHookKindFunction) {
            hk_inline_guard_update(hook->guardAddr, hook->succeeded ? 0 : 1, hook->origValue);
        }

        // Synchronous publish from batch-owned storage, inside executeHooks,
        // while the drained ops are retained and the caller's storage is
        // valid (batch contract: alive until executeHooks returns). Never
        // retained past this point.
        if(hook->callerOrig) {
            if(hook->succeeded) {
                *hook->callerOrig = hook->origValue;
            }

            hook->callerOrig = NULL;
        }
    }

    // Settle deferred idempotent-rehook originals (see hookFunction:): the
    // drain just executed or released the owning hook, so the guard now holds
    // the definitive answer. Synchronous, inside executeHooks — the waiters'
    // caller storage is valid until executeHooks returns (the batch contract
    // that also governs the ops' callerOrig) — and the waiters are dropped
    // here, never retained past the drain.
    @synchronized(self) {
        for(HKGuardWaiter *waiter in dupWaiters) {
            void *settled = NULL;
            hk_guard_result_t guard = hk_inline_guard_reserve(waiter->address, waiter->replacement, waiter->backendType, &settled);

            if(guard == HK_GUARD_DUP) {
                // The owning hook is installed: the stored original is the
                // answer. NULL when the entry is tainted (original
                // unknowable) or still pending in ANOTHER substitutor's
                // un-drained batch (ponytail: cross-substitutor drain
                // ordering race — the same-substitutor case is fully settled
                // by this drain; nothing honest exists to publish yet).
                *waiter->callerOrig = settled;
            } else if(guard == HK_GUARD_OK) {
                // The owner's hook was released (wrote nothing): nothing is
                // installed, so the function itself is the original. Return
                // the entry we just re-claimed — nobody owns it.
                hk_inline_guard_update(waiter->address, 2, NULL);
                *waiter->callerOrig = (void *)waiter->address;
            } else {
                // BLOCKED (a different hook now owns the address) or FULL:
                // the duplicate was never installed and there is no original
                // to publish.
                *waiter->callerOrig = NULL;
            }
        }

        [dupWaiters removeAllObjects];
    }

    [self noteHookResult:result];
    return result;
}

- (int)getLibErrno:(hookkit_lib_t *)outType {
    if(outType) {
        *outType = lastLibErrnoType;
    }

    return lastLibErrno;
}
@end