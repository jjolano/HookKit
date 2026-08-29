// Terminal inline engine <-> runtime adapter. See HKInlineVtable.h.

#include "HKInlineVtable.h"

#include <stdlib.h>
#include <string.h>

#include "../../Internal/HKPointerAuth.h"

static hk_engine_capabilities_t inline_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "inline-terminal";
    caps.backend_group = "HookKit";
    caps.display_name = "HookKit";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                         HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                                   HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    caps.achievable_reach = HK_REACH_ENTRYPOINT |
                            HK_REACH_EXACT_IMAGE_SCOPE;
    caps.exact_image_scope_targets = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    // NONE only, and this is the engine's defining limit rather than a
    // conservative guess: it overwrites the prologue outright, so there is no
    // predecessor to hand back and no continuation to call. Declaring it here
    // is what lets the router pick the relocating engine for a request this
    // one would only refuse at prepare.
    caps.original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE);
    // The entry patch, and nothing else -- refusing to allocate is what this
    // engine IS, so a caller forbidding executable allocation can still use it.
    caps.commit_effects = HK_EFFECT_TARGET_TEXT_MUTATION;
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
    uintptr_t target_address = addr->may_strip_pac_or_thumb_state
        ? hk_pac_strip_code(addr->address) : addr->address;

    // Image scope first, before anything is read from the target: if the
    // address is not in the image the request named, reading its prologue is
    // already reading the wrong memory. A NULL/empty catalog reports
    // NO_CATALOG, which is a skip and not a refusal -- see HKImageScope.h.
    hk_image_scope_status_t scope =
        hk_image_scope_check(ctx->catalog, &addr->expected_image,
                             addr->expected_uuid_present, addr->expected_uuid,
                             target_address);
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
    hk_inline_status_t st = hk_inline_prepare(target_address,
                                              hk_pac_strip_code((uintptr_t)spec->replacement),
                                              spec->original_requirement,
                                              addr->expected_initial_bytes,
                                              addr->expected_initial_bytes_size,
                                              plan);
    hk_prepare_result_t result = inline_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(plan);  // nothing was written on any non-OK status
        return result;
    }

    // A 4-byte patch is one aligned store and therefore atomic against a
    // thread entering the function mid-patch; anything longer is not. Refused
    // unless the caller has said the target is not concurrently executing --
    // see the header for the observed crash this default exists to prevent.
    if (plan->size != 4 && !ctx->allow_non_atomic_entry_patch) {
        out_diag->error_code = HK_INLINE_DIAG_NON_ATOMIC_PATCH;
        out_diag->error_message =
            "entry patch would not be a single aligned store; a thread could enter the function part-patched";
        free(plan);
        return HK_PREPARE_FAILED;
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

static hk_verify_result_t inline_verify_one_ctx(void *engine_ctx,
                                                const hk_hook_spec_t *spec,
                                                void *prepared,
                                                hk_verify_diag_t *out_diag) {
    (void)engine_ctx;
    (void)spec;
    if (!prepared) {
        out_diag->error_message = "inline verification received no prepared plan";
        return HK_VERIFY_FAILED;
    }
    const hk_inline_plan_t *plan = prepared;
    if (memcmp((const void *)plan->address, plan->patch, plan->size) != 0) {
        out_diag->error_message = "inline patch readback does not match the emitted branch";
        return HK_VERIFY_FAILED;
    }
    return HK_VERIFY_OK;
}

static void inline_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_inline_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = inline_describe,
    .prepare_one_ctx_status = inline_prepare_one_ctx_status,
    .commit_one_ctx = inline_commit_one_ctx,
    .verify_one_ctx = inline_verify_one_ctx,
    .release_prepared = inline_release_prepared,
};

const hk_engine_vtable_t *hk_inline_vtable(void) {
    return &g_inline_vtable;
}
