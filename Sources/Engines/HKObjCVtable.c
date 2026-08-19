// ObjC engine <-> runtime adapter. See HKObjCVtable.h.
//
// No file-scoped state at all: the runtime comes from the registered engine
// context and the prepared plan is handed back by the core. That is the whole
// difference from the other two adapters, and it is why this file has no
// stash, no environment, and no _for_testing entry points.

#include "HKObjCVtable.h"

#include <stdlib.h>
#include <string.h>

static hk_engine_capabilities_t objc_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "objc";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    caps.achievable_reach = HK_REACH_OBJC_DISPATCH;
    return caps;
}

static bool objc_prepare_one_ctx(void *engine_ctx, const hk_hook_spec_t *spec,
                                 void **out_prepared) {
    hk_objc_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_OBJC_METHOD) {
        return false;
    }

    hk_objc_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        return false;
    }
    hk_objc_status_t st = hk_objc_prepare(&ctx->runtime, &spec->target.objc, plan);
    if (st != HK_OBJC_OK) {
        // Every non-OK status is a clean prepare failure at this contract:
        // nothing was reserved and nothing was touched. That includes
        // NOT_APPLICABLE -- an absent optional target is a satisfied request,
        // but both preparation entry points return bool and cannot say
        // "correctly nothing to do" distinctly from "could not". Stated
        // rather than glossed: the distinction exists in the engine and is
        // lost here. Retiring it needs a richer prepare result, which is a
        // separate change from the context/prepared-state one this adapter
        // is the first to use.
        free(plan);
        return false;
    }
    *out_prepared = plan;
    return true;
}

static hk_mutation_state_t objc_commit_one_ctx(void *engine_ctx,
                                               const hk_hook_spec_t *spec,
                                               void *prepared,
                                               hk_artifact_sink_t *sink) {
    hk_objc_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !prepared) {
        return HK_MUTATION_NONE;  // never prepared, or prepared by someone else
    }
    // out_original is NULL: the original travels in the artifact instead --
    // see the header.
    return hk_objc_commit(&ctx->runtime, (const hk_objc_plan_t *)prepared,
                          spec->replacement, NULL, sink);
}

static void objc_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_objc_vtable = {
    .describe = objc_describe,
    .prepare_one_ctx = objc_prepare_one_ctx,
    .commit_one_ctx = objc_commit_one_ctx,
    .release_prepared = objc_release_prepared,
};

const hk_engine_vtable_t *hk_objc_vtable(void) {
    return &g_objc_vtable;
}
