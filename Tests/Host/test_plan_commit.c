// Host test for hk_plan_commit (Sources/Core/HKPlan.c). The property
// under test is the mutation-state -> outcome mapping (spec section
// 4.4/6.27) -- one of the spec's core invariants -- exercised for real
// via 4 distinct fake engines (Tests/Host/fake_engines.h) rather than
// trusted from reading the switch statement.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "fake_engines.h"

static hk_hook_spec_t symbol_spec(const char *id, const char *symbol) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = symbol;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

// Drives a single hook through DRAFT -> ANALYZED -> PREPARED against
// `engine`, returning the hook so the caller can commit and inspect it.
static hk_hook_t *prepared_hook(hk_runtime_t *rt, hk_plan_t *plan,
                                 const hk_engine_vtable_t *engine, const char *id) {
    assert(hk_runtime_register_engine_for_testing(rt, engine));
    hk_hook_spec_t spec = symbol_spec(id, "getpid");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    return hook;
}

static void test_commit_requires_prepared_or_partial_state(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_INVALID_STATE);  // still DRAFT
    assert(report == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  commit-requires-prepared-or-partial-state: PASS\n");
}

static void test_complete_mutation_becomes_active(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_t *hook = prepared_hook(rt, plan, &fake_rebind_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_ACTIVE);
    assert(report->results[0].mutation == HK_MUTATION_COMPLETE);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  complete-mutation-becomes-active: PASS\n");
}

static void test_none_mutation_becomes_failed_safe(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_commit_none_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_SAFE);
    assert(report->results[0].mutation == HK_MUTATION_NONE);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  none-mutation-becomes-failed-safe: PASS\n");
}

static void test_partial_mutation_becomes_failed_partial(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_commit_partial_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_PARTIAL);
    assert(report->results[0].mutation == HK_MUTATION_PARTIAL);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);  // only hook, and it failed -- whole plan FAILED

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  partial-mutation-becomes-failed-partial: PASS\n");
}

static void test_unknown_mutation_becomes_failed_unknown(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_commit_unknown_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_UNKNOWN);
    assert(report->results[0].mutation == HK_MUTATION_UNKNOWN);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  unknown-mutation-becomes-failed-unknown: PASS\n");
}

static void test_missing_commit_one_treated_as_unknown_not_none(void) {
    // An engine that prepared successfully but has no commit_one at all
    // is a real inconsistency, and specifically NOT trusted to have done
    // nothing -- UNKNOWN, not the more optimistic NONE, is the honest
    // mutation state for "this engine cannot be asked what it did."
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_no_commit_one_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_UNKNOWN);
    assert(report->results[0].mutation == HK_MUTATION_UNKNOWN);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  missing-commit-one-treated-as-unknown-not-none: PASS\n");
}

static void test_not_prepared_hook_left_untouched(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // No engine registered -- the hook will be NO_ROUTE, never PREPARED.
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", "getpid");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);  // never became PREPARED

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);  // PREPARED is a valid start state (0 attempted)
    assert(report->results[0].outcome == HK_OUTCOME_NO_ROUTE);  // left alone, not silently failed

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  not-prepared-hook-left-untouched: PASS\n");
}

static void test_partial_when_some_hooks_fail_commit(void) {
    // Both hooks match fake_rebind_engine (registered first) via
    // first-eligible-wins, so both would naturally succeed through it --
    // force the second hook's matched_engine to fake_commit_none_engine
    // after analyze (same internal-struct-access pattern
    // test_plan_prepare.c's own PARTIAL test uses) to get a genuine mixed
    // ACTIVE + FAILED_SAFE outcome deterministically.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t ok_spec = symbol_spec("hook.ok", "getpid");
    hk_hook_t *ok_hook = NULL;
    assert(hk_plan_add_hook(plan, &ok_spec, &ok_hook) == HK_STATUS_OK);

    hk_hook_spec_t fail_spec = symbol_spec("hook.fail", "getppid");
    hk_hook_t *fail_hook = NULL;
    assert(hk_plan_add_hook(plan, &fail_spec, &fail_hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    fail_hook->matched_engine = &fake_commit_none_engine;  // override after analyze
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(ok_hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(fail_hook->result.outcome == HK_OUTCOME_PREPARED);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->result_count == 2);
    assert(ok_hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(fail_hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hk_plan_state(plan) == HK_PLAN_PARTIAL);  // 2 attempted, 1 active, 1 failed

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  partial-when-some-hooks-fail-commit: PASS\n");
}

int main(void) {
    test_commit_requires_prepared_or_partial_state();
    test_complete_mutation_becomes_active();
    test_none_mutation_becomes_failed_safe();
    test_partial_mutation_becomes_failed_partial();
    test_unknown_mutation_becomes_failed_unknown();
    test_missing_commit_one_treated_as_unknown_not_none();
    test_not_prepared_hook_left_untouched();
    test_partial_when_some_hooks_fail_commit();
    printf("all plan commit tests passed\n");
    return 0;
}
