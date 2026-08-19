// Terminal inline engine <-> runtime adapter. See HKInlineVtable.h.

#include "HKInlineVtable.h"

#include <stdlib.h>
#include <string.h>

static hk_engine_capabilities_t inline_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "inline-terminal";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.achievable_reach = HK_REACH_ENTRYPOINT;
    return caps;
}

static bool inline_prepare_one_ctx(void *engine_ctx, const hk_hook_spec_t *spec,
                                   void **out_prepared) {
    const hk_inline_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_FUNCTION_ADDRESS) {
        return false;
    }
    const hk_address_target_t *addr = &spec->target.address;

    hk_inline_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        return false;
    }
    // original_requirement comes from the SPEC, not the target: it is the
    // caller's demand, and this engine refusing it is the whole reason
    // terminal inline can skip relocation. Passing it through rather than
    // hardcoding NONE is what keeps that refusal honest.
    hk_inline_status_t st = hk_inline_prepare((uintptr_t)addr->address,
                                              (uintptr_t)spec->replacement,
                                              spec->original_requirement,
                                              addr->expected_initial_bytes,
                                              addr->expected_initial_bytes_size,
                                              plan);
    if (st != HK_INLINE_OK) {
        // Every non-OK status is a clean prepare failure: nothing was written.
        // The distinctions the engine makes (too short, trap stub, needs a
        // continuation, precondition failed) are all collapsed into `false`
        // here -- the same bool ceiling the ObjC adapter records, and the same
        // fix applies.
        free(plan);
        return false;
    }
    *out_prepared = plan;
    return true;
}

static hk_mutation_state_t inline_commit_one_ctx(void *engine_ctx,
                                                 const hk_hook_spec_t *spec,
                                                 void *prepared,
                                                 hk_artifact_sink_t *sink) {
    const hk_inline_engine_ctx_t *ctx = engine_ctx;
    (void)spec;
    // The !prepared half is live (prepare failed or never ran). The !ctx half
    // is UNREACHABLE and said so plainly: commit only runs after prepare
    // returned true, prepare refuses a NULL context, and matched_engine_ctx is
    // fixed at analyze so both phases see the same pointer. It stays because
    // ctx is dereferenced two lines down, and a guard makes that safe by
    // construction rather than safe because of an argument about a different
    // function.
    if (!ctx || !prepared) {
        return HK_MUTATION_NONE;
    }
    return hk_inline_commit((const hk_inline_plan_t *)prepared,
                            ctx->write, ctx->write_ctx, sink);
}

static void inline_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_inline_vtable = {
    .describe = inline_describe,
    .prepare_one_ctx = inline_prepare_one_ctx,
    .commit_one_ctx = inline_commit_one_ctx,
    .release_prepared = inline_release_prepared,
};

const hk_engine_vtable_t *hk_inline_vtable(void) {
    return &g_inline_vtable;
}
