// Host test for hk_plan_analyze + hk_report_t (Sources/Core/HKPlan.c,
// HKReport.c). No engine registry exists yet, so every hook is expected
// to get HK_OUTCOME_NO_ROUTE -- honestly, not as a placeholder. This test
// is about proving the plumbing (state transitions, report/hook
// independence, result content) is correct given that honest starting
// point, not about routing logic that doesn't exist yet.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"

static void make_plan(hk_runtime_t **rt, hk_plan_t **plan) {
    assert(hk_runtime_create(NULL, rt) == HK_STATUS_OK);
    assert(hk_plan_create(*rt, NULL, plan) == HK_STATUS_OK);
}

static hk_hook_t *add_symbol_hook(hk_plan_t *plan, const char *id, const char *symbol,
                                   hk_reachability_t preferred_reach) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.struct_size = sizeof(spec.target.symbol);
    spec.target.symbol.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.name = symbol;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.preferred_reach = preferred_reach;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    return hook;
}

static void test_analyze_empty_plan(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report != NULL);
    assert(report->result_count == 0);
    assert(hk_plan_state(plan) == HK_PLAN_ANALYZED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  analyze-empty-plan: PASS\n");
}

static void test_analyze_reports_no_route_honestly(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_t *h1 = add_symbol_hook(plan, "hook.a", "getpid", HK_REACH_FUTURE_IMPORTS);
    hk_hook_t *h2 = add_symbol_hook(plan, "hook.b", "getppid", 0);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->result_count == 2);

    for (size_t i = 0; i < report->result_count; i++) {
        assert(report->results[i].outcome == HK_OUTCOME_NO_ROUTE);
        assert(report->results[i].mutation == HK_MUTATION_NONE);
        assert(report->results[i].achieved_reach == 0);
        assert(report->results[i].verified == false);
        assert(report->results[i].artifact_count == 0);
    }

    // request_id in each result must match the hook it came from, and
    // unmet_preferred_reach must echo back what was actually requested.
    assert(memcmp(&report->results[0].request_id, &h1->hook_id, sizeof(hk_id_t)) == 0);
    assert(report->results[0].unmet_preferred_reach == HK_REACH_FUTURE_IMPORTS);
    assert(memcmp(&report->results[1].request_id, &h2->hook_id, sizeof(hk_id_t)) == 0);
    assert(report->results[1].unmet_preferred_reach == 0);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  analyze-reports-no-route-honestly: PASS\n");
}

static void test_hook_result_transitions_unanalyzed_to_no_route(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_t *hook = add_symbol_hook(plan, "hook.a", "getpid", 0);

    hk_hook_result_t before;
    assert(hk_hook_copy_result(hook, &before) == HK_STATUS_OK);
    assert(before.outcome == HK_OUTCOME_UNANALYZED);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);

    hk_hook_result_t after;
    assert(hk_hook_copy_result(hook, &after) == HK_STATUS_OK);
    assert(after.outcome == HK_OUTCOME_NO_ROUTE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  hook-result-transitions-unanalyzed-to-no-route: PASS\n");
}

static void test_reanalyze_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);
    add_symbol_hook(plan, "hook.a", "getpid", 0);

    hk_report_t *report1 = NULL;
    assert(hk_plan_analyze(plan, &report1) == HK_STATUS_OK);

    hk_report_t *report2 = NULL;
    assert(hk_plan_analyze(plan, &report2) == HK_STATUS_INVALID_STATE);
    assert(report2 == NULL);

    hk_report_release(report1);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  reanalyze-rejected: PASS\n");
}

static void test_analyze_null_plan_rejected(void) {
    hk_report_t *report = NULL;
    assert(hk_plan_analyze(NULL, &report) == HK_STATUS_INVALID_ARGUMENT);
    assert(report == NULL);
    printf("  analyze-null-plan-rejected: PASS\n");
}

static void test_analyze_with_null_out_report_still_transitions_state(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);
    hk_hook_t *hook = add_symbol_hook(plan, "hook.a", "getpid", 0);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_state(plan) == HK_PLAN_ANALYZED);

    hk_hook_result_t result;
    assert(hk_hook_copy_result(hook, &result) == HK_STATUS_OK);
    assert(result.outcome == HK_OUTCOME_NO_ROUTE);  // hook results still updated even with no out_report

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  analyze-with-null-out-report-still-transitions-state: PASS\n");
}

static void test_report_independent_of_plan_after_release(void) {
    // Releasing the report must not affect the plan or its hooks -- the
    // report holds its own copies, not shared/aliased storage.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);
    hk_hook_t *hook = add_symbol_hook(plan, "hook.a", "getpid", 0);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    hk_report_release(report);

    hk_hook_result_t result;
    assert(hk_hook_copy_result(hook, &result) == HK_STATUS_OK);
    assert(result.outcome == HK_OUTCOME_NO_ROUTE);  // still readable after the report is gone

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  report-independent-of-plan-after-release: PASS\n");
}

static void test_report_release_tolerates_null(void) {
    hk_report_release(NULL);
    printf("  report-release-tolerates-null: PASS\n");
}

int main(void) {
    test_analyze_empty_plan();
    test_analyze_reports_no_route_honestly();
    test_hook_result_transitions_unanalyzed_to_no_route();
    test_reanalyze_rejected();
    test_analyze_null_plan_rejected();
    test_analyze_with_null_out_report_still_transitions_state();
    test_report_independent_of_plan_after_release();
    test_report_release_tolerates_null();
    printf("all plan analyze tests passed\n");
    return 0;
}
