// Relocating inline engine <-> runtime adapter. See HKRelocInlineVtable.h for
// why this and the terminal adapter coexist rather than conflict.

#include "HKRelocInlineVtable.h"

#include <stdlib.h>
#include <string.h>

static hk_engine_capabilities_t reloc_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "inline-relocating";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.achievable_reach = HK_REACH_ENTRYPOINT;
    // All three, and that is the whole reason this engine exists alongside the
    // terminal one: it preserves the displaced prologue in a trampoline, so
    // the original stays reachable in every form a request can ask for.
    caps.original_requirements = HK_ORIGINAL_REQ_ALL;
    // The entry patch AND an executable page. Declaring the allocation is what
    // lets a caller who forbids it be routed to the terminal engine instead of
    // silently getting a page anyway.
    caps.commit_effects = HK_EFFECT_TARGET_TEXT_MUTATION | HK_EFFECT_EXECUTABLE_ALLOCATION;
    return caps;
}

// Each refusal is a distinct diagnosis. Messages are literals, per the diag
// contract.
static hk_prepare_result_t reloc_classify(hk_reloc_status_t st, hk_prepare_diag_t *diag) {
    if (st == HK_RELOC_OK) {
        return HK_PREPARE_OK;
    }
    diag->error_code = (int64_t)st;
    switch (st) {
        case HK_RELOC_MISALIGNED:
            diag->error_message = "target is not 4-byte aligned, so it is not an A64 entry point"; break;
        case HK_RELOC_TARGET_TOO_SHORT:
            diag->error_message = "a displaced instruction ends the function; the relocated copy would never reach the jump back"; break;
        case HK_RELOC_TRAP_STUB:
            diag->error_message = "entry is a trap stub (BRK/HLT/UDF); there is no original to preserve"; break;
        case HK_RELOC_PRECONDITION_FAILED:
            diag->error_message = "prologue does not match the bytes the request pinned"; break;
        case HK_RELOC_UNRELOCATABLE:
            diag->error_message = "a displaced instruction cannot be rewritten to run from a new address"; break;
        case HK_RELOC_NO_TRAMPOLINE:
            diag->error_message = "could not obtain or seal an executable page for the trampoline"; break;
        case HK_RELOC_INVALID_ARGUMENT:
        case HK_RELOC_OK:
            diag->error_message = "invalid relocating-inline target"; break;
    }
    return HK_PREPARE_FAILED;
}

static hk_prepare_result_t reloc_prepare_one_ctx_status(void *engine_ctx,
                                                        const hk_hook_spec_t *spec,
                                                        void **out_prepared,
                                                        hk_prepare_diag_t *out_diag) {
    const hk_reloc_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_FUNCTION_ADDRESS) {
        out_diag->error_message = "relocating inline engine invoked without seams or with a non-address target";
        return HK_PREPARE_FAILED;
    }
    const hk_address_target_t *addr = &spec->target.address;

    // Image scope before anything else, and before a page is requested: if the
    // address is not in the image the request named, reading its prologue is
    // already reading the wrong memory, and allocating for it would leak a
    // page for a hook that was never going to happen.
    hk_image_scope_status_t scope =
        hk_image_scope_check(ctx->catalog, &addr->expected_image,
                             addr->expected_uuid_present, addr->expected_uuid,
                             (uintptr_t)addr->address);
    if (scope != HK_IMAGE_SCOPE_OK && scope != HK_IMAGE_SCOPE_NO_CATALOG) {
        out_diag->error_code = HK_RELOC_DIAG_IMAGE_SCOPE_BASE + (int64_t)scope;
        out_diag->error_message = hk_image_scope_describe(scope);
        return HK_PREPARE_FAILED;
    }

    hk_reloc_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
    }
    hk_reloc_status_t st = hk_reloc_prepare((uintptr_t)addr->address,
                                            (uintptr_t)spec->replacement,
                                            addr->expected_initial_bytes,
                                            addr->expected_initial_bytes_size,
                                            ctx->alloc, ctx->seal, ctx->free_page,
                                            ctx->seam_ctx, plan);
    hk_prepare_result_t result = reloc_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(plan);
        return result;
    }

    // The thunk exists so this is normally a 4-byte B. When the page could not
    // be placed within reach it is not, and a thread can enter the function
    // part-patched -- refused unless the caller has said the target is not
    // concurrently executing. The page is reclaimed on the way out: refusing
    // must not leak what preparing allocated.
    if (!plan->atomic_entry_patch && !ctx->allow_non_atomic_entry_patch) {
        out_diag->error_code = HK_RELOC_DIAG_NON_ATOMIC_PATCH;
        out_diag->error_message =
            "entry patch would not be a single aligned store; a thread could enter the function part-patched";
        if (ctx->free_page && plan->trampoline) {
            ctx->free_page(ctx->seam_ctx, plan->trampoline, plan->trampoline_size);
        }
        free(plan);
        return HK_PREPARE_FAILED;
    }

    *out_prepared = plan;
    return HK_PREPARE_OK;
}

static hk_mutation_state_t reloc_commit_one_ctx(void *engine_ctx,
                                                const hk_hook_spec_t *spec,
                                                void *prepared,
                                                hk_artifact_sink_t *sink) {
    const hk_reloc_engine_ctx_t *ctx = engine_ctx;
    (void)spec;
    if (!ctx || !prepared) {
        return HK_MUTATION_NONE;
    }
    return hk_reloc_commit((const hk_reloc_plan_t *)prepared,
                           ctx->write, ctx->write_ctx,
                           ctx->free_page, ctx->seam_ctx, sink);
}

static void reloc_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    // Frees the PLAN, not the trampoline. The page is deliberately not
    // reclaimed here: once sealed it may be executing, and a plan being
    // released says nothing about whether a thread is inside it. That the page
    // outlives the plan is why commit records it as an artifact -- the ledger
    // is how it stays accounted for.
    free(prepared);
}

static const hk_engine_vtable_t g_reloc_vtable = {
    .describe = reloc_describe,
    .prepare_one_ctx_status = reloc_prepare_one_ctx_status,
    .commit_one_ctx = reloc_commit_one_ctx,
    .release_prepared = reloc_release_prepared,
};

const hk_engine_vtable_t *hk_reloc_inline_vtable(void) {
    return &g_reloc_vtable;
}
