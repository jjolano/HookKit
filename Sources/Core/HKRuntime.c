// hk_runtime_t lifecycle. Milestone 4, core-only slice: no plan/domain/
// engine tracking yet (Sources/Core/HKPlan.c, not written), so
// hk_runtime_shutdown has nothing to quiesce and hk_runtime_drain_pending
// has nothing to apply -- both real, both honestly minimal for what
// exists today, not stubs pretending otherwise.
//
// Loading this translation unit does no implicit work (docs/3.0/
// ARCHITECTURE.md invariant #7) -- everything here runs only from an
// explicit hk_runtime_create call. hk_runtime_create itself does no
// provider activation, image traversal, callback registration, or thread
// creation: calloc, one ID generation, one struct copy.

#include "HKIDs.h"
#include "HKRuntimeInternal.h"

#include <stdlib.h>
#include <string.h>

// NULL config is a deliberate design choice beyond what the master spec's
// text specifies (it never says whether config is required) -- treated as
// "use every default": no executor (caller must drain_pending()), no
// diagnostics, HK_INSTALL_CONTEXT_EARLY_PROCESS. Makes the common case
// (a plain hk_runtime_create(NULL, &rt)) not require constructing a
// struct just to zero it.
hk_status_t hk_runtime_create(
    const hk_runtime_config_t *config,
    hk_runtime_t **out_runtime)
{
    if (!out_runtime) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_runtime = NULL;

    if (config && config->struct_size < sizeof(hk_runtime_config_t)) {
        // Smaller than this implementation's understanding of
        // hk_runtime_config_t's 3.0.0 shape is malformed, not a partial
        // read to tolerate -- "unknown trailing fields are ignored" only
        // ever means LARGER, never smaller (docs/3.0/PUBLIC_C_ABI.md).
        return HK_STATUS_INVALID_ARGUMENT;
    }

    hk_runtime_t *runtime = (hk_runtime_t *)calloc(1, sizeof(hk_runtime_t));
    if (!runtime) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    runtime->owner_id = hk_id_generate();

    if (config) {
        // struct_size may exceed sizeof(hk_runtime_config_t) (a newer
        // caller with fields this build predates) -- copy only what this
        // implementation knows about, never past its own struct size.
        size_t copy_size = config->struct_size < sizeof(hk_runtime_config_t)
                          ? config->struct_size
                          : sizeof(hk_runtime_config_t);
        memcpy(&runtime->config, config, copy_size);
    }
    runtime->config.struct_size = sizeof(hk_runtime_config_t);
    runtime->config.struct_version = HK_ABI_VERSION_3_0;

    atomic_init(&runtime->shutdown_called, false);

    *out_runtime = runtime;
    return HK_STATUS_OK;
}

void hk_runtime_shutdown(hk_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    atomic_store_explicit(&runtime->shutdown_called, true, memory_order_release);
}

// Does not generically unhook active targets -- there are none to unhook
// yet at this slice of Milestone 4 (no engines exist), and the eventual
// real implementation still won't, per docs/3.0/PUBLIC_C_ABI.md's explicit
// statement that installed hooks outlive runtime wrapper release.
void hk_runtime_release(hk_runtime_t *runtime) {
    free(runtime);
}

hk_id_t hk_runtime_owner_id(const hk_runtime_t *runtime) {
    if (!runtime) {
        hk_id_t zero;
        zero.high = 0;
        zero.low = 0;
        return zero;
    }
    return runtime->owner_id;
}

// Not public API -- see HKEngineInternal.h/HKRuntimeInternal.h. `vtable`
// is not owned or copied: the caller (test code today; a real engine
// module eventually) must keep it alive for the runtime's lifetime, the
// same non-ownership convention hk_hook_spec_t.replacement already uses
// for a caller-owned function pointer.
bool hk_runtime_register_engine_with_context(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable,
    void *engine_ctx)
{
    if (!runtime || !vtable) {
        return false;
    }
    if (runtime->engine_count >= HK_RUNTIME_MAX_ENGINES) {
        return false;
    }
    runtime->engines[runtime->engine_count] = vtable;
    runtime->engine_ctxs[runtime->engine_count] = engine_ctx;
    runtime->engine_count++;
    return true;
}

bool hk_runtime_register_engine_for_testing(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable)
{
    return hk_runtime_register_engine_with_context(runtime, vtable, NULL);
}

// Nothing can be pending yet: the late-image delta queue this drains
// (Milestone 12) doesn't exist. Always succeeds with an empty report
// rather than erroring, since "nothing to drain" is a legitimate steady
// state, not a failure.
hk_status_t hk_runtime_drain_pending(
    hk_runtime_t *runtime,
    hk_report_t **out_report)
{
    if (!runtime) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (out_report) {
        *out_report = NULL;
    }
    return HK_STATUS_OK;
}

// hk_report_release now lives in Sources/Core/HKReport.c, where
// hk_report_t's concrete definition does. It used to be a permanent-
// looking no-op here (every report was NULL because nothing produced one
// yet); hk_plan_analyze is the first real producer.
