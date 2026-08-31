// Host test for hk_plan_prepare (src/core/HKPlan.c). Covers the
// per-hook outcome transitions (ANALYZED -> PREPARED/FAILED_SAFE,
// NO_ROUTE untouched), the plan-level PREPARED/PARTIAL/FAILED rollup, and
// that prepare calls the SAME engine analyze found (via hook->matched_engine)
// rather than re-searching the registry.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
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

static hk_hook_spec_t objc_spec(const char *id) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_OBJC_METHOD;
    spec.target.objc.class_name = "NSObject";
    spec.target.objc.selector_name = "description";
    spec.target.objc.method_kind = HK_OBJC_INSTANCE_METHOD;
    spec.required_reach = HK_REACH_OBJC_DISPATCH;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void test_prepare_requires_analyzed_state(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_INVALID_STATE);  // still DRAFT
    assert(report == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  prepare-requires-analyzed-state: PASS\n");
}

static void test_analyzed_hook_prepares_successfully(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", "getpid");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_PREPARED);
    assert(hk_plan_state(plan) == HK_PLAN_PREPARED);

    hk_hook_result_t result;
    assert(hk_hook_copy_result(hook, &result) == HK_STATUS_OK);
    assert(result.outcome == HK_OUTCOME_PREPARED);  // hook's own stored result also updated

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  analyzed-hook-prepares-successfully: PASS\n");
}

static void test_no_route_hook_left_untouched(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // No engines registered -- the hook will be NO_ROUTE after analyze.
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", "getpid");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_NO_ROUTE);  // unchanged, not silently failed
    // Zero hooks attempted, zero failed -- a plan whose only hooks are
    // NO_ROUTE still "prepares successfully" (nothing to fail).
    assert(hk_plan_state(plan) == HK_PLAN_PREPARED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  no-route-hook-left-untouched: PASS\n");
}

static void test_prepare_failure_reports_failed_safe(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_always_fails_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", "getpid");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);  // route exists, matched_engine set

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_SAFE);
    // Only hook, only attempt, and it failed -- whole plan FAILED, not PARTIAL.
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  prepare-failure-reports-failed-safe: PASS\n");
}

static void test_engine_without_prepare_one_treated_as_failure(void) {
    // fake_objc_engine deliberately has prepare_one == NULL -- an engine
    // eligible for describe() but never implementing preparation must not
    // be silently skipped; it's a real inconsistency, reported as failure.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_objc_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.objc");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_SAFE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  engine-without-prepare-one-treated-as-failure: PASS\n");
}

static void test_partial_when_some_hooks_fail(void) {
    // Both hooks match the same registered engine via first-eligible-wins,
    // so both would naturally succeed through it -- to construct a
    // genuine mixed PREPARED+FAILED_SAFE outcome deterministically, force
    // the second hook's matched_engine to the failing one directly after
    // analyze (internal test access, same pattern as
    // test_plan_lifecycle.c's wrong-state test).
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
    assert(ok_hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert(fail_hook->result.outcome == HK_OUTCOME_ANALYZED);
    fail_hook->matched_engine = &fake_always_fails_engine;  // override after analyze

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(report->result_count == 2);
    assert(ok_hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(fail_hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    // 2 attempted, 1 prepared, 1 failed -- neither a clean success nor a
    // total failure.
    assert(hk_plan_state(plan) == HK_PLAN_PARTIAL);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  partial-when-some-hooks-fail: PASS\n");
}

static void test_group_prepare_batches_adjacent_hooks(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    fake_group_prepare_reset();
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt,
                                                  &fake_group_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t first_spec = symbol_spec("group.first", "getpid");
    hk_hook_spec_t second_spec = symbol_spec("group.second", "getppid");
    hk_hook_t *first = NULL;
    hk_hook_t *second = NULL;
    assert(hk_plan_add_hook(plan, &first_spec, &first) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &second_spec, &second) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(first->result.outcome == HK_OUTCOME_ANALYZED);
    assert(second->result.outcome == HK_OUTCOME_ANALYZED);

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(fake_group_prepare_calls == 1);
    assert(fake_group_prepare_members == 2);
    assert(first->result.outcome == HK_OUTCOME_PREPARED);
    assert(second->result.outcome == HK_OUTCOME_PREPARED);
    assert(report->results[0].outcome == HK_OUTCOME_PREPARED);
    assert(report->results[1].outcome == HK_OUTCOME_PREPARED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  group-prepare-batches-adjacent-hooks: PASS\n");
}

int main(void) {
    test_prepare_requires_analyzed_state();
    test_analyzed_hook_prepares_successfully();
    test_no_route_hook_left_untouched();
    test_prepare_failure_reports_failed_safe();
    test_engine_without_prepare_one_treated_as_failure();
    test_partial_when_some_hooks_fail();
    test_group_prepare_batches_adjacent_hooks();
    printf("all plan prepare tests passed\n");
    return 0;
}
