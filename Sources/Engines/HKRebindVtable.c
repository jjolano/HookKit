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
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "rebind";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    // One import slot rewritten per site; nothing allocated.
    caps.commit_effects = HK_EFFECT_IMPORT_MUTATION;
    return caps;
}

// The engine's refusals are distinct diagnoses, not one undifferentiated
// failure. Messages are literals, per the diag contract.
static hk_prepare_result_t rebind_classify(hk_rebind_status_t st, hk_prepare_diag_t *diag) {
    if (st == HK_REBIND_OK) {
        return HK_PREPARE_OK;
    }
    diag->error_code = (int64_t)st;
    switch (st) {
        case HK_REBIND_NOT_FOUND:
            diag->error_message = "the image imports no such symbol"; break;
        case HK_REBIND_MALFORMED_IMAGE:
            diag->error_message = "the image's import metadata is structurally invalid"; break;
        case HK_REBIND_TOO_MANY_SITES:
            diag->error_message = "more import slots for this symbol than the engine can record"; break;
        case HK_REBIND_INVALID_ARGUMENT:
        case HK_REBIND_OK:
            diag->error_message = "invalid rebind target"; break;
    }
    return HK_PREPARE_FAILED;
}

static hk_prepare_result_t rebind_prepare_one_ctx_status(void *engine_ctx,
                                                         const hk_hook_spec_t *spec,
                                                         void **out_prepared,
                                                         hk_prepare_diag_t *out_diag) {
    const hk_rebind_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        out_diag->error_message = "rebind engine invoked without an image or with a non-symbol target";
        return HK_PREPARE_FAILED;
    }

    // The rebind engine rewrites import slots in the image this context points
    // at -- the IMPORTER. So caller_image_scope is the selector that applies
    // here; defining_image describes where the symbol is exported from, which
    // this mechanism never resolves and must not pretend to check.
    //
    // Identity, not containment: the question is whether THIS image is in
    // scope, so the header pointer is compared directly. Using the containment
    // form here would assume the header lies inside the image's own segment
    // span -- true of a real Mach-O, but an assumption the check cannot verify,
    // and one a synthetic image can violate.
    hk_image_scope_status_t scope =
        hk_image_scope_check_header(ctx->catalog, &spec->target.symbol.caller_image_scope,
                                    false, NULL, ctx->image_base);
    if (scope != HK_IMAGE_SCOPE_OK && scope != HK_IMAGE_SCOPE_NO_CATALOG) {
        out_diag->error_code = HK_REBIND_DIAG_IMAGE_SCOPE_BASE + (int64_t)scope;
        out_diag->error_message = hk_image_scope_describe(scope);
        return HK_PREPARE_FAILED;
    }

    hk_rebind_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
    }
    hk_rebind_target_t target = target_from(ctx, false);  // prepare never writes
    hk_rebind_status_t st = hk_rebind_prepare(&target, spec->target.symbol.name,
                                              spec->target.symbol.name_convention, plan);
    hk_prepare_result_t result = rebind_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(plan);  // nothing reserved on any non-OK status
        return result;
    }
    *out_prepared = plan;
    return HK_PREPARE_OK;
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
    .prepare_one_ctx_status = rebind_prepare_one_ctx_status,
    .commit_one_ctx = rebind_commit_one_ctx,
    .release_prepared = rebind_release_prepared,
};

const hk_engine_vtable_t *hk_rebind_vtable(void) {
    return &g_rebind_vtable;
}
