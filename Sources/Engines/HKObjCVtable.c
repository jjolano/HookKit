// ObjC engine <-> runtime adapter. See HKObjCVtable.h.

#include "HKObjCVtable.h"

#include <string.h>

static hk_objc_binding_env_t g_env;
static bool g_env_set;

#define HK_OBJC_STASH_MAX 16u
typedef struct {
    char hook_id[64];
    hk_objc_plan_t plan;
    bool used;
} stash_entry_t;
static stash_entry_t g_stash[HK_OBJC_STASH_MAX];

static stash_entry_t *stash_find(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < HK_OBJC_STASH_MAX; i++) {
        if (g_stash[i].used && strcmp(g_stash[i].hook_id, id) == 0) return &g_stash[i];
    }
    return NULL;
}
static stash_entry_t *stash_put(const char *id) {
    if (!id || strlen(id) >= sizeof(g_stash[0].hook_id)) return NULL;
    stash_entry_t *e = stash_find(id);  // overwrite a re-prepared hook
    if (!e) {
        for (unsigned i = 0; i < HK_OBJC_STASH_MAX; i++) {
            if (!g_stash[i].used) { e = &g_stash[i]; break; }
        }
    }
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    strcpy(e->hook_id, id);
    e->used = true;
    return e;
}

void hk_objc_vtable_set_environment_for_testing(const hk_objc_binding_env_t *env) {
    memset(g_stash, 0, sizeof(g_stash));  // a new environment invalidates old plans
    if (env) { g_env = *env; g_env_set = true; }
    else { memset(&g_env, 0, sizeof(g_env)); g_env_set = false; }
}

void hk_objc_vtable_reset_for_testing(void) {
    hk_objc_vtable_set_environment_for_testing(NULL);
}

static hk_engine_capabilities_t objc_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "objc";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    caps.achievable_reach = HK_REACH_OBJC_DISPATCH;
    return caps;
}

static bool objc_prepare_one(const hk_hook_spec_t *spec) {
    if (!g_env_set || !spec || spec->target_kind != HK_TARGET_OBJC_METHOD) {
        return false;
    }
    stash_entry_t *e = stash_put(spec->stable_hook_id);
    if (!e) {
        return false;  // unstashable (id too long, or stash full): fail cleanly
    }
    hk_objc_status_t st = hk_objc_prepare(&g_env.runtime, &spec->target.objc, &e->plan);
    if (st != HK_OBJC_OK) {
        // Every non-OK status is a clean prepare failure at this contract:
        // nothing was reserved and nothing was touched. That includes
        // NOT_APPLICABLE -- an absent optional target is a satisfied request,
        // but hk_engine_vtable_t.prepare_one is a bool and cannot say
        // "correctly nothing to do" distinctly from "could not". Stated rather
        // than glossed: the distinction exists in the engine and is lost here,
        // and recovering it needs the richer prepare result the vtable will
        // grow later.
        e->used = false;
        return false;
    }
    return true;
}

static hk_mutation_state_t objc_commit_one(const hk_hook_spec_t *spec,
                                           hk_artifact_sink_t *sink) {
    if (!g_env_set || !spec) {
        return HK_MUTATION_NONE;
    }
    stash_entry_t *e = stash_find(spec->stable_hook_id);
    if (!e) {
        return HK_MUTATION_NONE;  // prepare never succeeded for this hook
    }
    // out_original is NULL: the original travels in the artifact instead --
    // see the header.
    return hk_objc_commit(&g_env.runtime, &e->plan, spec->replacement, NULL, sink);
}

static const hk_engine_vtable_t g_objc_vtable = {
    .describe = objc_describe,
    .prepare_one = objc_prepare_one,
    .commit_one = objc_commit_one,
};

const hk_engine_vtable_t *hk_objc_vtable(void) {
    return &g_objc_vtable;
}
