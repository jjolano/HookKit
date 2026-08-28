// Internal layout of hk_runtime_t (public: opaque, HookKitRuntime.h).
// Shared across Sources/Core/*.c that need runtime internals -- nothing
// outside Sources/Core/ should include this.

#ifndef HK_CORE_RUNTIME_INTERNAL_H
#define HK_CORE_RUNTIME_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>

#include "../../Headers/HookKit/HookKitRuntime.h"
#include "HKEngineInternal.h"
#include "HKArtifactLedger.h"
#include "HKImageCatalog.h"
#include "../Engines/HKInlineVtable.h"
#include "../Engines/HKMemoryVtable.h"
#include "../Engines/HKObjCVtable.h"
#include "../Engines/HKProviderVtable.h"
#include "../Engines/HKRebindVtable.h"
#include "../Engines/HKRelocInlineVtable.h"

// Fixed-size, not grown dynamically: the real production engine set
// (Milestone 6+) is small and compiled-in, and test code registering fake
// engines never needs more than a handful at once. A growable array would
// be solving a problem this doesn't have yet -- see HKPlanInternal.h for
// where that complexity (pointer-stable growth) actually is needed.
#define HK_RUNTIME_MAX_ENGINES 16

// Defined in HKPlanInternal.h; only ever held by pointer here.
struct hk_hook;

struct hk_runtime {
    hk_id_t owner_id;

    // Copied by value at hk_runtime_create -- every field is a function
    // pointer, a void* context, or an enum, so a plain struct copy already
    // satisfies the "caller's buffer need not outlive the call" rule with
    // no separate allocation needed (unlike string/bytes-bearing specs
    // elsewhere in the ABI, which deep-copy into owned storage instead).
    hk_runtime_config_t config;

    atomic_bool shutdown_called;

    // Host runs leave these zero. Apple builds initialize them once from the
    // slice and deployment target so production routing can fail closed for
    // an uncertified architecture or unsupported minimum iOS version.
    hk_engine_architecture_mask_t platform_architecture;
    uint32_t platform_ios_version;

    // Not public API -- see HKEngineInternal.h. Production fills this with
    // the fixed compiled-in engine set; tests can add private fake engines.
    const hk_engine_vtable_t *engines[HK_RUNTIME_MAX_ENGINES];
    // Parallel to engines[] -- the context each was registered with, passed
    // to that engine's *_ctx entry points. NULL for an engine registered
    // without one. Kept as a parallel array rather than folded into a struct
    // so the existing engines[] indexing and its non-ownership convention
    // stay exactly as they were.
    void *engine_ctxs[HK_RUNTIME_MAX_ENGINES];
    // Parallel to engines[]: only hk_runtime_register_engine_for_testing
    // sets this. It permits synthetic tests of modes not device-certified.
    bool engine_testing[HK_RUNTIME_MAX_ENGINES];
    size_t engine_count;

    // Darwin's libobjc binding for the built-in ObjC engine. The context is
    // embedded so the registered vtable never points at temporary stack
    // state. Host builds leave it zeroed and tests register their seam.
    hk_objc_engine_ctx_t objc_engine;
    hk_rebind_engine_ctx_t rebind_engine;
    hk_memory_engine_ctx_t memory_engine;
    hk_inline_engine_ctx_t inline_engine;
    hk_reloc_engine_ctx_t reloc_engine;
    hk_provider_engine_ctx_t dobby_provider;
    hk_provider_engine_ctx_t gum_provider;
    hk_provider_engine_ctx_t ellekit_provider;
    hk_provider_engine_ctx_t substitute_provider;
    hk_image_catalog_t *catalog;

    // Hooks whose target was not available yet and whose request said to wait
    // (HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE). TRANSFERRED here from the plan
    // at hk_plan_release rather than copied -- the deep-copy machinery writes
    // into hk_hook_t's own owned_* fields, so moving the hook reuses it
    // untouched, and it keeps any hk_hook_t* the caller still holds valid.
    // The runtime owns these and frees them at hk_runtime_release.
    struct hk_hook **pending;
    size_t pending_count;
    size_t pending_capacity;

    // Runtime-level accumulation of every artifact produced by this owner.
    hk_artifact_ledger_t *artifacts;
};

// Compatibility-only construction seam. A non-NULL list is a strict,
// per-runtime override: only matching engines (plus the facade-native ObjC
// engine) remain eligible. An empty list leaves no function/memory route.
// Normal public creation passes NULL.
hk_status_t hk_runtime_create_with_backend_override(
    const hk_runtime_config_t *config,
    const char *backend_ids,
    hk_runtime_t **out_runtime);

// Deferred-hook queue (Milestone 12). Both are implemented in HKPlan.c, which
// is where hk_hook_t's guts and hk_hook_free live.

// Moves a deferred hook's ownership from its plan to the runtime. Returns
// false on OOM, in which case the caller keeps ownership and must free it --
// a hook that cannot be queued is better freed than leaked.
bool hk_runtime_adopt_pending_hook(hk_runtime_t *runtime, struct hk_hook *hook);

// Frees every queued hook. Called from hk_runtime_release, which is where the
// runtime's ownership of them ends.
void hk_runtime_free_pending_hooks(hk_runtime_t *runtime);

bool hk_runtime_append_artifacts(hk_runtime_t *runtime,
                                 const hk_artifact_ledger_t *source);

#endif // HK_CORE_RUNTIME_INTERNAL_H
