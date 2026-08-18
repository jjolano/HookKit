// Memory-patch engine <-> runtime adapter. See HKMemoryVtable.h.

#include "HKMemoryVtable.h"

#include <string.h>

static hk_memory_binding_env_t g_env;
static bool g_env_set;

#define HK_MEMORY_STASH_MAX 16u
typedef struct {
    char hook_id[64];
    hk_mempatch_plan_t plan;
    uintptr_t address;   // resolved once at prepare, reused at commit
    bool used;
} stash_entry_t;
static stash_entry_t g_stash[HK_MEMORY_STASH_MAX];

static stash_entry_t *stash_find(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < HK_MEMORY_STASH_MAX; i++) {
        if (g_stash[i].used && strcmp(g_stash[i].hook_id, id) == 0) return &g_stash[i];
    }
    return NULL;
}
static stash_entry_t *stash_put(const char *id) {
    if (!id || strlen(id) >= sizeof(g_stash[0].hook_id)) return NULL;
    stash_entry_t *e = stash_find(id);
    if (!e) {
        for (unsigned i = 0; i < HK_MEMORY_STASH_MAX; i++) {
            if (!g_stash[i].used) { e = &g_stash[i]; break; }
        }
    }
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    strcpy(e->hook_id, id);
    e->used = true;
    return e;
}

void hk_memory_vtable_set_environment_for_testing(const hk_memory_binding_env_t *env) {
    memset(g_stash, 0, sizeof(g_stash));
    if (env) { g_env = *env; g_env_set = true; }
    else { memset(&g_env, 0, sizeof(g_env)); g_env_set = false; }
}

void hk_memory_vtable_reset_for_testing(void) {
    hk_memory_vtable_set_environment_for_testing(NULL);
}

// The spec carries the target's offset/absolute address; the environment
// carries where an image-relative target's image is mapped. An absolute target
// resolves to its own address.
static uintptr_t resolve_address(const hk_memory_target_t *mem) {
    if (mem->address_is_image_relative) {
        return g_env.image_base + (uintptr_t)mem->address;
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

static bool memory_prepare_one(const hk_hook_spec_t *spec) {
    if (!g_env_set || !spec || spec->target_kind != HK_TARGET_MEMORY_PATCH) {
        return false;
    }
    const hk_memory_target_t *mem = &spec->target.memory;
    stash_entry_t *e = stash_put(spec->stable_hook_id);
    if (!e) {
        return false;
    }
    e->address = resolve_address(mem);
    hk_mempatch_status_t st = hk_mempatch_prepare(e->address, mem->size,
                                                  mem->expected_bytes, mem->expected_mask,
                                                  &e->plan);
    if (st != HK_MEMPATCH_OK) {
        e->used = false;  // precondition failed / too large / invalid: clean fail
        return false;
    }
    return true;
}

static hk_mutation_state_t memory_commit_one(const hk_hook_spec_t *spec,
                                             hk_artifact_sink_t *sink) {
    if (!g_env_set || !spec) {
        return HK_MUTATION_NONE;
    }
    stash_entry_t *e = stash_find(spec->stable_hook_id);
    if (!e) {
        return HK_MUTATION_NONE;
    }
    return hk_mempatch_commit(e->address, &e->plan, spec->target.memory.replacement_bytes,
                              g_env.write, g_env.write_ctx, sink);
}

static const hk_engine_vtable_t g_memory_vtable = {
    .describe = memory_describe,
    .prepare_one = memory_prepare_one,
    .commit_one = memory_commit_one,
};

const hk_engine_vtable_t *hk_memory_vtable(void) {
    return &g_memory_vtable;
}
