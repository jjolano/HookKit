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
    caps.engine_id = "memory";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_MEMORY_PATCH);
    caps.achievable_reach = HK_REACH_EXACT_MEMORY;
    return caps;
}

static bool memory_prepare_one_ctx(void *engine_ctx, const hk_hook_spec_t *spec,
                                   void **out_prepared) {
    const hk_memory_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_MEMORY_PATCH) {
        return false;
    }
    const hk_memory_target_t *mem = &spec->target.memory;

    prepared_patch_t *p = malloc(sizeof(*p));
    if (!p) {
        return false;
    }
    p->address = resolve_address(ctx, mem);
    hk_mempatch_status_t st = hk_mempatch_prepare(p->address, mem->size,
                                                  mem->expected_bytes, mem->expected_mask,
                                                  &p->plan);
    if (st != HK_MEMPATCH_OK) {
        free(p);  // precondition failed / too large / invalid: clean fail
        return false;
    }
    *out_prepared = p;
    return true;
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

static void memory_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_memory_vtable = {
    .describe = memory_describe,
    .prepare_one_ctx = memory_prepare_one_ctx,
    .commit_one_ctx = memory_commit_one_ctx,
    .release_prepared = memory_release_prepared,
};

const hk_engine_vtable_t *hk_memory_vtable(void) {
    return &g_memory_vtable;
}
