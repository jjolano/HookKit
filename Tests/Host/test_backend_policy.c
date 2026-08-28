// Host test for strict per-substitutor backend selection.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKEngineInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"

// Minimal engines: only describe() matters to the policy. abi_version/
// struct_size left 0 so hk_runtime_register_engine skips its strict check.
#define FAKE_ENGINE(fn, id_literal)                                     \
    static hk_engine_capabilities_t fn##_describe(void) {              \
        hk_engine_capabilities_t caps;                                \
        memset(&caps, 0, sizeof(caps));                               \
        caps.engine_id = id_literal;                                  \
        return caps;                                                  \
    }                                                                 \
    static const hk_engine_vtable_t fn = { .describe = fn##_describe }

FAKE_ENGINE(eng_alpha, "alpha");
FAKE_ENGINE(eng_beta, "beta");
FAKE_ENGINE(eng_gamma, "gamma");
FAKE_ENGINE(eng_ellekit, "provider-ellekit");
FAKE_ENGINE(eng_objc, "objc");

static const char *id_at(hk_runtime_t *rt, size_t i) {
    return rt->engines[i]->describe().engine_id;
}

// Fresh runtime with the given engines registered in order.
static hk_runtime_t *make_runtime(const hk_engine_vtable_t *const *engines,
                                  size_t count) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK && rt);
    // Host builds register no platform engines; start from a clean slate
    // regardless, so registration order under test is exactly ours.
    rt->engine_count = 0;
    for (size_t i = 0; i < count; i++) {
        assert(hk_runtime_register_engine_for_testing(rt, engines[i]));
    }
    return rt;
}

int main(void) {
    const hk_engine_vtable_t *abc[] = { &eng_alpha, &eng_beta, &eng_gamma };

    // A direct list is strict and ordered. ObjC stays only for its
    // facade-native message route; it is never a function/memory fallback.
    const hk_engine_vtable_t *with_objc[] = {
        &eng_objc, &eng_alpha, &eng_beta, &eng_gamma,
    };
    hk_runtime_t *rt = make_runtime(with_objc, 4);
    hk_runtime_apply_backend_override(rt, "gamma,alpha");
    assert(rt->engine_count == 3);
    assert(strcmp(id_at(rt, 0), "gamma") == 0);
    assert(strcmp(id_at(rt, 1), "alpha") == 0);
    assert(strcmp(id_at(rt, 2), "objc") == 0);
    hk_runtime_release(rt);
    printf("PASS direct selection is strict and ordered\n");

    // Prefix aliases work for the direct API too, while an empty or bad
    // selection deliberately leaves no function/memory engine to fall back to.
    const hk_engine_vtable_t *with_ek[] = {
        &eng_alpha, &eng_ellekit, &eng_beta,
    };
    rt = make_runtime(with_ek, 3);
    hk_runtime_apply_backend_override(rt, "ellekit");
    assert(rt->engine_count == 1);
    assert(strcmp(id_at(rt, 0), "provider-ellekit") == 0);
    hk_runtime_release(rt);

    rt = make_runtime(abc, 3);
    hk_runtime_apply_backend_override(rt, "");
    assert(rt->engine_count == 0);
    hk_runtime_release(rt);

    rt = make_runtime(abc, 3);
    hk_runtime_apply_backend_override(rt, "missing");
    assert(rt->engine_count == 0);
    hk_runtime_release(rt);
    printf("PASS direct empty/invalid selection has no fallback\n");

    printf("ALL backend-policy tests passed\n");
    return 0;
}
