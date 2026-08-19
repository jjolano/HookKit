// Rebind engine <-> runtime adapter. See HKRebindVtable.h.
//
// No file-scoped state: the image and writer come from the registered engine
// context, and the prepared plan is handed back by the core.

#include "HKRebindVtable.h"

#include <stdlib.h>
#include <string.h>

// prepare must not be handed a writer -- it mutates nothing, and the type
// system is the cheapest place to say so.
static hk_rebind_target_t target_from(const hk_rebind_engine_ctx_t *ctx,
                                      bool with_writer) {
    hk_rebind_target_t t;
    t.image_base = ctx->image_base;
    t.image_size = ctx->image_size;
    t.slide = ctx->slide;
    t.write = with_writer ? ctx->write : NULL;
    t.write_ctx = ctx->write_ctx;
    return t;
}

static hk_engine_capabilities_t rebind_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "rebind";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}

static bool rebind_prepare_one_ctx(void *engine_ctx, const hk_hook_spec_t *spec,
                                   void **out_prepared) {
    const hk_rebind_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        return false;
    }

    hk_rebind_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        return false;
    }
    hk_rebind_target_t target = target_from(ctx, false);  // prepare never writes
    hk_rebind_status_t st = hk_rebind_prepare(&target, spec->target.symbol.name,
                                              spec->target.symbol.name_convention, plan);
    if (st != HK_REBIND_OK) {
        free(plan);  // NOT_FOUND / malformed / overflow -> clean prepare failure
        return false;
    }
    *out_prepared = plan;
    return true;
}

static hk_mutation_state_t rebind_commit_one_ctx(void *engine_ctx,
                                                 const hk_hook_spec_t *spec,
                                                 void *prepared,
                                                 hk_artifact_sink_t *sink) {
    const hk_rebind_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !prepared) {
        // No captured plan -- prepare did not run or did not succeed for this
        // hook. Nothing was ever reserved, so nothing is touched.
        return HK_MUTATION_NONE;
    }
    hk_rebind_target_t target = target_from(ctx, true);
    uint32_t written = 0;
    return hk_rebind_commit(&target, (const hk_rebind_plan_t *)prepared,
                            (uint64_t)(uintptr_t)spec->replacement, sink, &written);
}

static void rebind_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_rebind_vtable = {
    .describe = rebind_describe,
    .prepare_one_ctx = rebind_prepare_one_ctx,
    .commit_one_ctx = rebind_commit_one_ctx,
    .release_prepared = rebind_release_prepared,
};

const hk_engine_vtable_t *hk_rebind_vtable(void) {
    return &g_rebind_vtable;
}
