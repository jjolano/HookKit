// Model-based test of the plan lifecycle state machine (Milestone 4).
//
// An independent reference model (model_apply, written from the documented
// lifecycle -- "only DRAFT accepts new domains/hooks", DRAFT->ANALYZED->
// PREPARED->COMMITTED -- NOT copied from HKPlan.c) predicts, for every
// (state, operation), whether the operation is accepted and what state
// results. Random operation sequences are then applied to BOTH the model and
// a real plan, asserting they agree at every step. A coverage check asserts
// every (state, op) cell was actually exercised, so a passing run really did
// visit the whole table rather than a lucky slice of it.
//
// The random walk below keeps the clean success model deliberately small so
// every state/op cell remains easy to audit. A second deterministic model at
// the end covers the failure rollups too; the two models share no state or
// transition code with HKPlan.c.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKOwnership.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "fake_engines.h"

typedef enum { M_DRAFT, M_ANALYZED, M_PREPARED, M_COMMITTED, M_STATE_COUNT } model_state_t;
typedef enum { OP_ADD_HOOK, OP_DEFINE_DOMAIN, OP_ANALYZE, OP_PREPARE, OP_COMMIT, OP_STATE, OP_COUNT } op_t;

// The reference model: returns whether `op` is accepted in *s, advancing *s
// on an accepted state-changing op. Independent statement of intended
// behavior; if the implementation disagrees, one of them is wrong.
static bool model_apply(model_state_t *s, op_t op) {
    switch (op) {
    case OP_ADD_HOOK:
    case OP_DEFINE_DOMAIN:
        return *s == M_DRAFT;  // accepted, no state change
    case OP_ANALYZE:
        if (*s == M_DRAFT) { *s = M_ANALYZED; return true; }
        return false;
    case OP_PREPARE:
        if (*s == M_ANALYZED) { *s = M_PREPARED; return true; }
        return false;
    case OP_COMMIT:
        if (*s == M_PREPARED) { *s = M_COMMITTED; return true; }
        return false;
    case OP_STATE:
        return true;  // read-only, always accepted, no change
    default:
        assert(0 && "unknown op");
        return false;
    }
}

static hk_plan_state_t model_to_impl_state(model_state_t s) {
    switch (s) {
    case M_DRAFT:     return HK_PLAN_DRAFT;
    case M_ANALYZED:  return HK_PLAN_ANALYZED;
    case M_PREPARED:  return HK_PLAN_PREPARED;
    case M_COMMITTED: return HK_PLAN_COMMITTED;
    default:          assert(0); return HK_PLAN_DRAFT;
    }
}

// Applies `op` to the real plan, returning its status. Uses a monotonic
// counter for unique hook/domain stable ids so the ONLY reason an op is ever
// rejected is plan state -- never a duplicate-id INVALID_ARGUMENT, which
// would confound the model comparison.
static hk_status_t impl_apply(hk_plan_t *plan, op_t op, int *counter) {
    char id[32];
    switch (op) {
    case OP_ADD_HOOK: {
        snprintf(id, sizeof(id), "hook.%d", (*counter)++);
        hk_hook_spec_t spec;
        memset(&spec, 0, sizeof(spec));
        spec.struct_size = sizeof(spec);
        spec.struct_version = HK_ABI_VERSION_3_0;
        spec.stable_hook_id = id;
        spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
        spec.target.symbol.name = "getpid";
        spec.required_reach = HK_REACH_EXISTING_IMPORTS;
        spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
        spec.role = HK_OPERATION_MANDATORY;
        hk_hook_t *h = NULL;
        return hk_plan_add_hook(plan, &spec, &h);
    }
    case OP_DEFINE_DOMAIN: {
        snprintf(id, sizeof(id), "dom.%d", (*counter)++);
        hk_domain_spec_t spec;
        memset(&spec, 0, sizeof(spec));
        spec.struct_size = sizeof(spec);
        spec.struct_version = HK_ABI_VERSION_3_0;
        spec.stable_domain_id = id;
        hk_domain_t *d = NULL;
        return hk_plan_define_domain(plan, &spec, &d);
    }
    case OP_ANALYZE: return hk_plan_analyze(plan, NULL);
    case OP_PREPARE: return hk_plan_prepare(plan, NULL);
    case OP_COMMIT:  return hk_plan_commit(plan, NULL);
    case OP_STATE:   (void)hk_plan_state(plan); return HK_STATUS_OK;
    default:         assert(0); return HK_STATUS_INTERNAL_ERROR;
    }
}

// Reproducible LCG -- no dependence on the host's rand() so a failure is
// always replayable from its seed.
static unsigned lcg_next(unsigned *state) {
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fff;
}

static bool g_covered[M_STATE_COUNT][OP_COUNT];

static void run_seed(unsigned seed, int steps) {
    hk_ownership_reset_for_testing();
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    model_state_t model = M_DRAFT;
    int counter = 0;
    unsigned rng = seed;

    for (int step = 0; step < steps; step++) {
        op_t op = (op_t)(lcg_next(&rng) % OP_COUNT);
        model_state_t pre = model;

        bool expect_accepted = model_apply(&model, op);
        hk_status_t status = impl_apply(plan, op, &counter);

        // Accept/reject must agree.
        assert((status == HK_STATUS_OK) == expect_accepted);
        // And a rejection must be specifically INVALID_STATE (state was the
        // only possible reason, given unique ids and valid specs) -- not a
        // stray INVALID_ARGUMENT or OOM masquerading as the same "not OK".
        if (!expect_accepted) {
            assert(status == HK_STATUS_INVALID_STATE);
        }
        // Resulting plan state must match the model's.
        assert(hk_plan_state(plan) == model_to_impl_state(model));

        g_covered[pre][op] = true;
    }

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_ownership_reset_for_testing();
}

// Independent rollup model for the outcomes that the successful random walk
// cannot reach. The counts are observations of the scenario setup, not
// implementation state copied out of HKPlan.c.
typedef enum {
    F_ANALYZED,
    F_PREPARED,
    F_PARTIAL,
    F_FAILED,
    F_COMMITTED,
} failure_model_state_t;

static bool model_prepare_rollup(failure_model_state_t *state,
                                 size_t prepared, size_t failed) {
    if (!state || *state != F_ANALYZED) {
        return false;
    }
    if (failed == 0) {
        *state = F_PREPARED;
    } else if (prepared == 0) {
        *state = F_FAILED;
    } else {
        *state = F_PARTIAL;
    }
    return true;
}

static bool model_commit_rollup(failure_model_state_t *state,
                                size_t active, size_t failed,
                                size_t attempted) {
    if (!state || (*state != F_PREPARED && *state != F_PARTIAL)) {
        return false;
    }
    if (failed == 0) {
        *state = F_COMMITTED;
    } else if (active == 0 && attempted > 0) {
        *state = F_FAILED;
    } else {
        *state = F_PARTIAL;
    }
    return true;
}

static hk_hook_t *add_model_hook(hk_plan_t *plan, const char *id) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = "getpid";
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    return hook;
}

static void test_failure_rollup_model(void) {
    // Prepare: one success plus one failure produces PARTIAL.
    {
        hk_ownership_reset_for_testing();
        hk_runtime_t *rt = NULL;
        hk_plan_t *plan = NULL;
        assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
        assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
        assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
        hk_hook_t *ok = add_model_hook(plan, "model.prepare.ok");
        hk_hook_t *bad = add_model_hook(plan, "model.prepare.bad");
        assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
        bad->matched_engine = &fake_always_fails_engine;

        failure_model_state_t model = F_ANALYZED;
        assert(model_prepare_rollup(&model, 1, 1));
        assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
        assert(ok->result.outcome == HK_OUTCOME_PREPARED);
        assert(bad->result.outcome == HK_OUTCOME_FAILED_SAFE);
        assert(model == F_PARTIAL && hk_plan_state(plan) == HK_PLAN_PARTIAL);

        hk_plan_release(plan);
        hk_runtime_release(rt);
        hk_ownership_reset_for_testing();
    }

    // Prepare: zero successes plus one failure produces FAILED, and commit is
    // rejected from that terminal state.
    {
        hk_ownership_reset_for_testing();
        hk_runtime_t *rt = NULL;
        hk_plan_t *plan = NULL;
        assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
        assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
        assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
        hk_hook_t *bad = add_model_hook(plan, "model.prepare.only-bad");
        assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
        bad->matched_engine = &fake_always_fails_engine;

        failure_model_state_t model = F_ANALYZED;
        assert(model_prepare_rollup(&model, 0, 1));
        assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
        assert(bad->result.outcome == HK_OUTCOME_FAILED_SAFE);
        assert(model == F_FAILED && hk_plan_state(plan) == HK_PLAN_FAILED);
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_INVALID_STATE);

        hk_plan_release(plan);
        hk_runtime_release(rt);
        hk_ownership_reset_for_testing();
    }

    // Commit: one active plus one clean refusal produces PARTIAL.
    {
        hk_ownership_reset_for_testing();
        hk_runtime_t *rt = NULL;
        hk_plan_t *plan = NULL;
        assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
        assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
        assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
        hk_hook_t *ok = add_model_hook(plan, "model.commit.ok");
        hk_hook_t *bad = add_model_hook(plan, "model.commit.bad");
        bad->spec.target.symbol.name = "getppid";
        assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
        bad->matched_engine = &fake_commit_none_engine;
        assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

        failure_model_state_t model = F_ANALYZED;
        assert(model_prepare_rollup(&model, 2, 0));
        assert(model_commit_rollup(&model, 1, 1, 2));
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
        assert(ok->result.outcome == HK_OUTCOME_ACTIVE);
        assert(bad->result.outcome == HK_OUTCOME_FAILED_SAFE);
        assert(model == F_PARTIAL && hk_plan_state(plan) == HK_PLAN_PARTIAL);

        hk_plan_release(plan);
        hk_runtime_release(rt);
        hk_ownership_reset_for_testing();
    }

    // Commit: one clean refusal with no active hook produces FAILED.
    {
        hk_ownership_reset_for_testing();
        hk_runtime_t *rt = NULL;
        hk_plan_t *plan = NULL;
        assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
        assert(hk_runtime_register_engine_for_testing(rt, &fake_commit_none_engine));
        assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
        hk_hook_t *bad = add_model_hook(plan, "model.commit.only-bad");
        assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
        assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

        failure_model_state_t model = F_ANALYZED;
        assert(model_prepare_rollup(&model, 1, 0));
        assert(model_commit_rollup(&model, 0, 1, 1));
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
        assert(bad->result.outcome == HK_OUTCOME_FAILED_SAFE);
        assert(model == F_FAILED && hk_plan_state(plan) == HK_PLAN_FAILED);

        hk_plan_release(plan);
        hk_runtime_release(rt);
        hk_ownership_reset_for_testing();
    }

    puts("all plan failure rollup model scenarios passed (4 scenarios)");
}

int main(void) {
    const unsigned seeds[] = {1u, 7u, 42u, 99u, 1337u, 2026u, 55555u, 8u};
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        run_seed(seeds[i], 40);
    }

    // Prove the random walk actually exercised the whole (state, op) table --
    // a model-based test that only visited a corner would pass vacuously.
    int missing = 0;
    for (int s = 0; s < M_STATE_COUNT; s++) {
        for (int op = 0; op < OP_COUNT; op++) {
            if (!g_covered[s][op]) {
                printf("  UNCOVERED: state=%d op=%d\n", s, op);
                missing++;
            }
        }
    }
    assert(missing == 0);

    printf("all plan model tests passed (%d state x op cells covered)\n",
           M_STATE_COUNT * OP_COUNT);
    test_failure_rollup_model();
    return 0;
}
