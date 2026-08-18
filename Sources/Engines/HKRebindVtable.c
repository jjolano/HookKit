// Rebind engine <-> runtime adapter. See HKRebindVtable.h for what the
// file-scoped environment papers over and why.

#include "HKRebindVtable.h"

#include <string.h>

// The one active environment (see the header on why this is file-scoped and
// what that costs). Not owned; the caller keeps it alive across the plan.
static hk_rebind_binding_env_t g_env;
static bool g_env_set;

// Prepared-state stash: the plan hk_rebind_prepare produced, kept between
// prepare_one and commit_one because the vtable threads no state itself.
// Keyed by stable_hook_id -- fixed size, since a host test drives a handful.
#define HK_REBIND_STASH_MAX 16u
typedef struct {
    char hook_id[64];
    hk_rebind_plan_t plan;
    bool used;
} stash_entry_t;
static stash_entry_t g_stash[HK_REBIND_STASH_MAX];

static stash_entry_t *stash_find(const char *hook_id) {
    if (!hook_id) {
        return NULL;
    }
    for (unsigned i = 0; i < HK_REBIND_STASH_MAX; i++) {
        if (g_stash[i].used && strcmp(g_stash[i].hook_id, hook_id) == 0) {
            return &g_stash[i];
        }
    }
    return NULL;
}

static stash_entry_t *stash_put(const char *hook_id) {
    if (!hook_id || strlen(hook_id) >= sizeof(g_stash[0].hook_id)) {
        return NULL;
    }
    stash_entry_t *e = stash_find(hook_id);  // overwrite a re-prepared hook
    if (!e) {
        for (unsigned i = 0; i < HK_REBIND_STASH_MAX; i++) {
            if (!g_stash[i].used) { e = &g_stash[i]; break; }
        }
    }
    if (!e) {
        return NULL;  // stash full
    }
    memset(e, 0, sizeof(*e));
    strcpy(e->hook_id, hook_id);
    e->used = true;
    return e;
}

void hk_rebind_vtable_set_environment_for_testing(const hk_rebind_binding_env_t *env) {
    memset(g_stash, 0, sizeof(g_stash));  // a new environment invalidates old plans
    if (env) {
        g_env = *env;
        g_env_set = true;
    } else {
        memset(&g_env, 0, sizeof(g_env));
        g_env_set = false;
    }
}

void hk_rebind_vtable_reset_for_testing(void) {
    hk_rebind_vtable_set_environment_for_testing(NULL);
}

static hk_rebind_target_t target_from_env(bool with_writer) {
    hk_rebind_target_t t;
    t.image_base = g_env.image_base;
    t.image_size = g_env.image_size;
    t.slide = g_env.slide;
    t.write = with_writer ? g_env.write : NULL;
    t.write_ctx = g_env.write_ctx;
    return t;
}

static hk_engine_capabilities_t rebind_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "rebind";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}

static bool rebind_prepare_one(const hk_hook_spec_t *spec) {
    if (!g_env_set || !spec || spec->target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        return false;
    }
    stash_entry_t *e = stash_put(spec->stable_hook_id);
    if (!e) {
        return false;  // unstashable (id too long, or stash full): fail cleanly
    }

    hk_rebind_target_t target = target_from_env(false);  // prepare never writes
    hk_rebind_status_t st = hk_rebind_prepare(&target, spec->target.symbol.name,
                                              spec->target.symbol.name_convention, &e->plan);
    if (st != HK_REBIND_OK) {
        e->used = false;  // NOT_FOUND / malformed / overflow -> clean prepare failure
        return false;
    }
    return true;
}

static hk_mutation_state_t rebind_commit_one(const hk_hook_spec_t *spec,
                                             hk_artifact_sink_t *sink) {
    if (!g_env_set || !spec) {
        return HK_MUTATION_NONE;
    }
    stash_entry_t *e = stash_find(spec->stable_hook_id);
    if (!e) {
        // No captured plan -- prepare did not run or did not succeed for this
        // hook. Nothing was ever reserved, so nothing is touched.
        return HK_MUTATION_NONE;
    }

    hk_rebind_target_t target = target_from_env(true);
    uint32_t written = 0;
    hk_mutation_state_t mutation =
        hk_rebind_commit(&target, &e->plan, (uint64_t)(uintptr_t)spec->replacement,
                         sink, &written);
    return mutation;
}

static const hk_engine_vtable_t g_rebind_vtable = {
    .describe = rebind_describe,
    .prepare_one = rebind_prepare_one,
    .commit_one = rebind_commit_one,
};

const hk_engine_vtable_t *hk_rebind_vtable(void) {
    return &g_rebind_vtable;
}
