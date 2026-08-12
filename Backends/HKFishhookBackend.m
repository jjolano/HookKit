#import "Internal/HKBackendInternal.h"

#import <dlfcn.h>
#import <errno.h>
#import <stdlib.h>
#import <string.h>

#if __has_include(<ptrauth.h>)
#import <ptrauth.h>
#endif

#import "vendor/fishhook/fishhook.h"

#pragma mark - HKFishhookRebinding

// fishhook's rebind_symbols retains the name and replaced pointers of each
// struct rebinding for ALL future dlopen events, so they must outlive
// hookFunction:. Each hook is therefore kept in a process-lifetime store
// forever — per-hook, bounded. Deliberate: fishhook writes the original
// through these cells on every future image load. Guarded because
// hookFunction: may be called from multiple threads (fishhook's own list is
// locked internally; the ObjC store is not). Hooks that matched no loaded
// reference are unregistered (rebind_symbols_unbind) and freed instead, so a
// refused (HK_ERR_NOT_SUPPORTED) hook retains nothing for future image loads.
@interface HKFishhookRebinding : NSObject {
@public
    char *name;
    void **origCell;
}
@end

@implementation HKFishhookRebinding
@end

static NSMutableArray<HKFishhookRebinding *> *fishhookRebindingStore(void) {
    static NSMutableArray *store = nil;
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        store = [NSMutableArray new];
    });

    return store;
}

#pragma mark - C1 original publication

// rebind_symbols_hook invokes its publish callback DURING the scan — before
// the first matching slot write (vendor guarantee) — so the original must
// land in the operation's OUTPUT cell (the batch-owned cell from
// hk_original_output_cell on the drained path, the facade's local cell on
// the immediate path). The context therefore captures the cell pointer, not
// the caller's pointer: by the time the callback fires, the facade may have
// moved the publication state.
typedef struct {
    void **cell;
} HKFishhookPublishContext;

static void hk_fishhook_publish_original(void *context, void *original) {
    HKFishhookPublishContext *ctx = (HKFishhookPublishContext *)context;

    // NULL cell = no original requested: hk_original_output_cell returns
    // NULL (the vendor's NULL-oldptr mode) instead of staging storage, so
    // the callback skips the write — the vendor still publishes to the
    // caller only when requested, which is the contract. fishhook's own
    // rebinding->replaced cell (owned->origCell) keeps capturing the
    // original for future image loads either way.
    if(ctx->cell) {
        *ctx->cell = original;
    }
}

#pragma mark - HKFishhookBackend

@implementation HKFishhookBackend {
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
    _lastErrno = 0;

    Dl_info info;

#if __has_feature(ptrauth_calls)
    // The caller may pass a signed function pointer (arm64e function
    // pointers are signed with asia/div-0 by the ABI). dladdr and the
    // dli_saddr comparison below work on the raw address, so strip first.
    function = ptrauth_strip(function, ptrauth_key_asia);
#endif

    if(!(dladdr(function, &info) && info.dli_sname && info.dli_saddr == function)) {
        // fishhook rebinds by exported symbol name; private/interior
        // addresses are not rebindable
        return HK_ERR_NOT_SUPPORTED;
    }

    HKFishhookRebinding *owned = [HKFishhookRebinding new];
    const char *name = info.dli_sname;
    if (name && name[0] == '_') name++;
    owned->name = strdup(name);
    owned->origCell = calloc(1, sizeof(void *));

    if(!owned->name || !owned->origCell) {
        // OOM: fishhook retains both pointers for every future dlopen and
        // dereferences origCell when an import matches, so a NULL must never
        // reach it. Nothing was registered or retained here — report the
        // failure (strdup/calloc set errno to ENOMEM).
        _lastErrno = ENOMEM;
        free(owned->name);
        free(owned->origCell);
        return HK_ERR;
    }

    struct rebinding rebinding = {
        owned->name, replacement, owned->origCell
    };

    // C1: publish the original through the operation's output cell during
    // the scan — the vendor fires the callback before the first slot write,
    // so the original is observable no later than the first replacement.
    HKFishhookPublishContext context = { .cell = old_ptr };
    struct rebind_result result = { 0, 0 };

    errno = 0;
    int rc = rebind_symbols_hook(&rebinding, 1, &result, hk_fishhook_publish_original, &context);

    if(rc != 0) {
        // prepend failed (OOM): nothing was registered and nothing is
        // retained by the library — the cells are still ours to free.
        _lastErrno = errno;
        free(owned->name);
        free(owned->origCell);
        return HK_ERR;
    }

    if(result.matched == 0) {
        // The symbol is exported (dladdr found it) but no loaded image
        // references it through an indirect symbol pointer, so the rebinding
        // is a silent no-op. Unregister it so nothing applies on a future
        // image load — and only free the retained cells once the library
        // CONFIRMED the entry is gone (a -1 unbind means the entry is still
        // registered and still dereferences them; leaking beats a
        // use-after-free on the next dlopen). Side-effect-free, so callers
        // may switch technique.
        NSLog(@"[HookKit] fishhook: symbol '%s' is not referenced by any loaded image; hook is a no-op", owned->name);

        if(rebind_symbols_unbind(&rebinding, 1) == 0) {
            free(owned->name);
            free(owned->origCell);
        }

        if(old_ptr) {
            *old_ptr = NULL;   // no original exists on the NOT_SUPPORTED path
        }

        _lastErrno = ENOENT;
        return HK_ERR_NOT_SUPPORTED;
    }

    // At least one slot was rewritten: the rebinding is live for this and
    // every future image load, so the store keeps the name/origCell alive
    // (fishhook dereferences origCell on each future match).
    @synchronized(fishhookRebindingStore()) {
        [fishhookRebindingStore() addObject:owned];
    }

    if(result.matched > 0 && (result.failed > 0 || result.restore_failed)) {
        // Partial rebind: matched slots are live, the rest could not be made
        // writable — or a rewritten section's original page protection could
        // not be restored (those pages may remain writable), which is also a
        // partial, not a clean, success. The entry stays registered (future
        // image loads may write the skipped slots); the detail comes from
        // errno when the platform set one (the vendor's vm_protect does not
        // touch errno, so fall back to EIO — a write that did not land).
        // matched == 0 never reaches here (it returns NOT_SUPPORTED above),
        // so restore_failed alone keeps the NOT_SUPPORTED outcome.
        _lastErrno = errno ? errno : EIO;
        return HK_ERR_PARTIAL;
    }

    return HK_OK;
}

- (hookkit_status_t)hookMemory:(void *)target withData:(const void *)data size:(size_t)size {
    _lastErrno = 0;
    return HK_ERR_NOT_SUPPORTED;
}

// Per-op fallback (OOM of the batch scratch arrays): apply each op through the
// single-op path. The facade's native-batch drain owns begin/publish/finish,
// so this must NOT call them — it only rebinds and sets hook->status, writing
// the original into the op's output cell (hookFunction: does that via its
// publish callback).
- (void)executeHooksSequential:(NSArray<HKHookOperation *> *)hooks {
    for(HKHookOperation *hook in hooks) {
        void **cell = hk_original_output_cell(&hook->original);
        hookkit_status_t result = [self hookFunction:hook->function withReplacement:hook->replacement outOldPtr:cell];
        hook->status = result;
        if(result != HK_OK) {
            hook->backendErrno = _lastErrno;
        }
    }
}

- (void)executeHooks:(NSArray<HKHookOperation *> *)hooks {
    // Native batch (descriptor nativeBatch=YES): apply every rebindable op in
    // ONE image walk via rebind_symbols_hook_batch, instead of one ~O(images)
    // walk per op. The facade has already run hk_original_begin for each op
    // (NULLing its output cell) and will run hk_original_publish/finish after
    // this returns, so here we only rebind and set hook->status; each op's
    // original lands in its output cell mid-walk through the per-rebinding
    // publish cell.
    NSUInteger n = hooks.count;
    if(n == 0) {
        return;
    }

    struct rebinding *rebindings = calloc(n, sizeof(struct rebinding));
    void ***publish_cells = calloc(n, sizeof(void **));
    // ObjC objects held in retained arrays (ARC): a C pointer array would not
    // retain them, and the newly-created rebindings must survive from here
    // through the post-walk distribution below.
    NSMutableArray<HKFishhookRebinding *> *ownedList = [NSMutableArray arrayWithCapacity:n];
    NSMutableArray<HKHookOperation *> *mapList = [NSMutableArray arrayWithCapacity:n];

    if(!rebindings || !publish_cells) {
        // OOM building the batch: fall back to the per-op path (still correct,
        // just O(images) per op).
        free(rebindings); free(publish_cells);
        [self executeHooksSequential:hooks];
        return;
    }

    size_t k = 0;
    for(HKHookOperation *hook in hooks) {
        void *function = hook->function;
#if __has_feature(ptrauth_calls)
        function = ptrauth_strip(function, ptrauth_key_asia);
#endif
        Dl_info info;
        if(!(dladdr(function, &info) && info.dli_sname && info.dli_saddr == function)) {
            // fishhook rebinds by exported symbol name; private/interior
            // addresses are not rebindable.
            hook->status = HK_ERR_NOT_SUPPORTED;
            hook->backendErrno = 0;
            continue;
        }

        HKFishhookRebinding *ob = [HKFishhookRebinding new];
        const char *name = info.dli_sname;
        if(name && name[0] == '_') name++;
        ob->name = strdup(name);
        ob->origCell = calloc(1, sizeof(void *));
        if(!ob->name || !ob->origCell) {
            free(ob->name);
            free(ob->origCell);
            ob->name = NULL;
            ob->origCell = NULL;
            hook->status = HK_ERR;
            hook->backendErrno = ENOMEM;
            continue;
        }

        rebindings[k].name = ob->name;
        rebindings[k].replacement = hook->replacement;
        rebindings[k].replaced = ob->origCell;              // retained cell (future image loads)
        publish_cells[k] = hk_original_output_cell(&hook->original);  // borrowed caller cell (this scan)
        [ownedList addObject:ob];
        [mapList addObject:hook];
        k++;
    }

    if(k > 0) {
        struct rebind_result result = { 0, 0, 0 };
        int rc = rebind_symbols_hook_batch(rebindings, publish_cells, k, &result);

        if(rc != 0) {
            // prepend failed (OOM): nothing was registered or retained.
            for(size_t j = 0; j < k; j++) {
                HKFishhookRebinding *ob = ownedList[j];
                free(ob->name);
                free(ob->origCell);
                ob->name = NULL;
                ob->origCell = NULL;
                mapList[j]->status = HK_ERR;
                mapList[j]->backendErrno = errno;
            }
        } else {
            // The whole batch is one retained fishhook entry (all k rebindings,
            // matched and no-op alike), so every origCell must stay alive for
            // process life — keep every owned in the store. Per-op outcome is
            // read from its origCell: written ⇒ at least one slot was rewritten
            // (live); still NULL ⇒ the symbol is referenced by no loaded image
            // (a silent no-op, reported NOT_SUPPORTED). No-op rebindings sit in
            // the shared entry harmlessly (they never match a future image).
            @synchronized(fishhookRebindingStore()) {
                [fishhookRebindingStore() addObjectsFromArray:ownedList];
            }
            for(size_t j = 0; j < k; j++) {
                HKHookOperation *hook = mapList[j];
                if(*(ownedList[j]->origCell) != NULL) {
                    hook->status = HK_OK;
                } else {
                    hook->status = HK_ERR_NOT_SUPPORTED;
                    hook->backendErrno = ENOENT;
                    if(publish_cells[j]) {
                        *(publish_cells[j]) = NULL;
                    }
                }
            }
        }
    }

    free(rebindings);
    free(publish_cells);
}
@end