// Memory-patch engine <-> runtime adapter. See HKMemoryVtable.h.
//
// No file-scoped state: the writer and image base come from the registered
// engine context, and the prepared plan is handed back by the core.

#include "HKMemoryVtable.h"

#include <stdlib.h>
#include <string.h>

// A prepared memory patch needs the resolved address at commit as well as the
// engine's plan -- resolving it twice would let an image-relative target land
// somewhere else if the context moved in between.
typedef struct {
    hk_mempatch_plan_t plan;
    uintptr_t address;
} prepared_patch_t;

// The spec carries the target's offset/absolute address; the context carries
// where an image-relative target's image is mapped. An absolute target
// resolves to its own address.
static uintptr_t resolve_address(const hk_memory_engine_ctx_t *ctx,
                                 const hk_memory_target_t *mem) {
    if (mem->address_is_image_relative) {
        return ctx->image_base + (uintptr_t)mem->address;
    }
    return (uintptr_t)mem->address;
}

static hk_engine_capabilities_t memory_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "memory";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_MEMORY_PATCH);
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                         HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                                   HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    caps.achievable_reach = HK_REACH_EXACT_MEMORY |
                            HK_REACH_EXACT_IMAGE_SCOPE;
    caps.exact_image_scope_targets = HK_TARGET_KIND_BIT(HK_TARGET_MEMORY_PATCH);
    // A later plan may re-patch the same target only when its expected bytes
    // match the owned head, which the engine's precondition check enforces.
    caps.chainable_target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_MEMORY_PATCH);
    // A controlled byte patch. NOTE: the spec has no HK_FORBID_* bit for a
    // plain memory mutation, so a caller cannot forbid this one -- see
    // hk_effect_forbid_bit.
    caps.commit_effects = HK_EFFECT_MEMORY_MUTATION;
    return caps;
}

// The engine's refusals are distinct diagnoses. Messages are literals, per the
// diag contract.
static hk_prepare_result_t memory_classify(hk_mempatch_status_t st, hk_prepare_diag_t *diag) {
    if (st == HK_MEMPATCH_OK) {
        return HK_PREPARE_OK;
    }
    diag->error_code = (int64_t)st;
    switch (st) {
        case HK_MEMPATCH_PRECONDITION_FAILED:
            diag->error_message = "the region does not hold the bytes the request expected"; break;
        case HK_MEMPATCH_TOO_LARGE:
            diag->error_message = "the region is larger than the engine can capture"; break;
        case HK_MEMPATCH_INVALID_ARGUMENT:
        case HK_MEMPATCH_OK:
            diag->error_message = "invalid memory-patch target"; break;
    }
    return HK_PREPARE_FAILED;
}

static hk_prepare_result_t memory_prepare_one_ctx_status(void *engine_ctx,
                                                         const hk_hook_spec_t *spec,
                                                         void **out_prepared,
                                                         hk_prepare_diag_t *out_diag) {
    const hk_memory_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_MEMORY_PATCH) {
        out_diag->error_message = "memory engine invoked without a writer or with a non-memory target";
        return HK_PREPARE_FAILED;
    }
    const hk_memory_target_t *mem = &spec->target.memory;
    const uintptr_t address = resolve_address(ctx, mem);

    // base_image is meaningful only for an image-relative target -- an
    // absolute address is not claimed to live anywhere in particular, so
    // checking it against a selector the request never filled would invent a
    // requirement. Checked BEFORE the region is read: if the address is not in
    // the image the request named, reading it is already reading the wrong
    // memory.
    if (mem->address_is_image_relative) {
        hk_image_scope_status_t scope =
            hk_image_scope_check(ctx->catalog, &mem->base_image, false, NULL, address);
        if (scope != HK_IMAGE_SCOPE_OK && scope != HK_IMAGE_SCOPE_NO_CATALOG) {
            out_diag->error_code = HK_MEMORY_DIAG_IMAGE_SCOPE_BASE + (int64_t)scope;
            out_diag->error_message = hk_image_scope_describe(scope);
            return HK_PREPARE_FAILED;
        }
    }

    prepared_patch_t *p = malloc(sizeof(*p));
    if (!p) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
    }
    p->address = address;
    hk_mempatch_status_t st = hk_mempatch_prepare(p->address, mem->size,
                                                  mem->expected_bytes, mem->expected_mask,
                                                  &p->plan);
    hk_prepare_result_t result = memory_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(p);  // nothing reserved on any non-OK status
        return result;
    }
    *out_prepared = p;
    return HK_PREPARE_OK;
}

static hk_mutation_state_t memory_commit_one_ctx(void *engine_ctx,
                                                 const hk_hook_spec_t *spec,
                                                 void *prepared,
                                                 hk_artifact_sink_t *sink) {
    const hk_memory_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !prepared) {
        return HK_MUTATION_NONE;
    }
    prepared_patch_t *p = prepared;
    return hk_mempatch_commit(p->address, &p->plan,
                              spec->target.memory.replacement_bytes,
                              ctx->write, ctx->write_ctx, sink);
}

static hk_verify_result_t memory_verify_one_ctx(void *engine_ctx,
                                                const hk_hook_spec_t *spec,
                                                void *prepared,
                                                hk_verify_diag_t *out_diag) {
    (void)engine_ctx;
    if (!spec || !prepared ||
        spec->target_kind != HK_TARGET_MEMORY_PATCH ||
        !spec->target.memory.replacement_bytes.data) {
        out_diag->error_message = "memory verification received an invalid prepared patch";
        return HK_VERIFY_FAILED;
    }
    const prepared_patch_t *p = prepared;
    if (memcmp((const void *)p->address,
               spec->target.memory.replacement_bytes.data,
               p->plan.size) != 0) {
        out_diag->error_message = "memory patch readback does not match the replacement";
        return HK_VERIFY_FAILED;
    }
    return HK_VERIFY_OK;
}

static void memory_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_memory_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = memory_describe,
    .prepare_one_ctx_status = memory_prepare_one_ctx_status,
    .commit_one_ctx = memory_commit_one_ctx,
    .verify_one_ctx = memory_verify_one_ctx,
    .release_prepared = memory_release_prepared,
};

const hk_engine_vtable_t *hk_memory_vtable(void) {
    return &g_memory_vtable;
}
