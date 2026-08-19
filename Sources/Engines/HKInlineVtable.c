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

// The four refusals this engine can make are genuinely different diagnoses,
// and flattening them made every one read as "prepare failed". None of them is
// NOT_APPLICABLE: an entry that cannot be patched is a real failure, not a
// satisfied conditional request. Messages are literals, per the diag contract.
static hk_prepare_result_t inline_classify(hk_inline_status_t st, hk_prepare_diag_t *diag) {
    if (st == HK_INLINE_OK) {
        return HK_PREPARE_OK;
    }
    diag->error_code = (int64_t)st;
    switch (st) {
        case HK_INLINE_MISALIGNED:
            diag->error_message = "target is not 4-byte aligned, so it is not an A64 entry point"; break;
        case HK_INLINE_TARGET_TOO_SHORT:
            diag->error_message = "function ends inside the overwrite window; the patch would run past it"; break;
        case HK_INLINE_TRAP_STUB:
            diag->error_message = "entry is a trap stub (BRK/HLT/UDF); there is no original to replace"; break;
        case HK_INLINE_PRECONDITION_FAILED:
            diag->error_message = "prologue does not match the bytes the request pinned"; break;
        case HK_INLINE_NEEDS_CONTINUATION:
            diag->error_message = "terminal inline cannot provide an original; use a relocating engine"; break;
        case HK_INLINE_INVALID_ARGUMENT:
        case HK_INLINE_OK:
            diag->error_message = "invalid inline target"; break;
    }
    return HK_PREPARE_FAILED;
}

static hk_prepare_result_t inline_prepare_one_ctx_status(void *engine_ctx,
                                                         const hk_hook_spec_t *spec,
                                                         void **out_prepared,
                                                         hk_prepare_diag_t *out_diag) {
    const hk_inline_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_FUNCTION_ADDRESS) {
        out_diag->error_message = "inline engine invoked without a writer or with a non-address target";
        return HK_PREPARE_FAILED;
    }
    const hk_address_target_t *addr = &spec->target.address;

    // Image scope first, before anything is read from the target: if the
    // address is not in the image the request named, reading its prologue is
    // already reading the wrong memory. A NULL/empty catalog reports
    // NO_CATALOG, which is a skip and not a refusal -- see HKImageScope.h.
    hk_image_scope_status_t scope =
        hk_image_scope_check(ctx->catalog, &addr->expected_image,
                             addr->expected_uuid_present, addr->expected_uuid,
                             (uintptr_t)addr->address);
    if (scope != HK_IMAGE_SCOPE_OK && scope != HK_IMAGE_SCOPE_NO_CATALOG) {
        out_diag->error_code = HK_INLINE_DIAG_IMAGE_SCOPE_BASE + (int64_t)scope;
        out_diag->error_message = hk_image_scope_describe(scope);
        return HK_PREPARE_FAILED;
    }

    hk_inline_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
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
    hk_prepare_result_t result = inline_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(plan);  // nothing was written on any non-OK status
        return result;
    }
    *out_prepared = plan;
    return HK_PREPARE_OK;
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
    .prepare_one_ctx_status = inline_prepare_one_ctx_status,
    .commit_one_ctx = inline_commit_one_ctx,
    .release_prepared = inline_release_prepared,
};

const hk_engine_vtable_t *hk_inline_vtable(void) {
    return &g_inline_vtable;
}
