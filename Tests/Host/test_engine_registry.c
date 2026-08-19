// Host test for the minimal internal engine contract + registry
// (Sources/Core/HKEngineInternal.h, HKRuntime.c's
// hk_runtime_register_engine_for_testing, HKPlan.c's hk_hook_analyze_one).
// The property under test: hk_plan_analyze actually consults registered
// engines now, upgrading eligible hooks from NO_ROUTE to ANALYZED, while
// hooks nothing can serve still honestly get NO_ROUTE.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKEngineInternal.h"
#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "fake_engines.h"

static hk_hook_spec_t symbol_spec(const char *id, hk_reachability_t required, hk_reachability_t preferred) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = "getpid";
    spec.required_reach = required;
    spec.preferred_reach = preferred;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void test_no_engines_registered_still_no_route(void) {
    // Regression check: this was the only behavior before this iteration,
    // and it must not have silently changed.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_NO_ROUTE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  no-engines-registered-still-no-route: PASS\n");
}

static void test_eligible_engine_upgrades_to_analyzed(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS,
                                       HK_REACH_EXISTING_IMPORTS | HK_REACH_FUTURE_IMPORTS);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    const hk_hook_result_t *r = &report->results[0];
    assert(r->outcome == HK_OUTCOME_ANALYZED);
    assert(r->achieved_reach == HK_REACH_EXISTING_IMPORTS);
    // FUTURE_IMPORTS was preferred but the fake engine can't achieve it --
    // must show up as unmet, not silently dropped.
    assert(r->unmet_preferred_reach == HK_REACH_FUTURE_IMPORTS);
    assert(r->retryable == false);
    assert(r->diagnostic_engine_id.length == strlen("fake-rebind"));
    assert(memcmp(r->diagnostic_engine_id.data, "fake-rebind", r->diagnostic_engine_id.length) == 0);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  eligible-engine-upgrades-to-analyzed: PASS\n");
}

static void test_insufficient_reach_stays_no_route(void) {
    // The engine handles the target KIND but can't achieve the required
    // REACH -- must not be treated as eligible just because the kind matched.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // fake_rebind only achieves EXISTING_IMPORTS; require PRIVATE_ADDRESS too.
    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS | HK_REACH_PRIVATE_ADDRESS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_NO_ROUTE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  insufficient-reach-stays-no-route: PASS\n");
}

static void test_first_eligible_wins_skips_ineligible(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // Register the objc engine FIRST -- it's ineligible for a symbol
    // target, so the search must correctly skip past it to the rebind
    // engine registered second, not stop (or crash) on the mismatch.
    assert(hk_runtime_register_engine_for_testing(rt, &fake_objc_engine));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_ANALYZED);
    assert(memcmp(report->results[0].diagnostic_engine_id.data, "fake-rebind",
                  report->results[0].diagnostic_engine_id.length) == 0);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  first-eligible-wins-skips-ineligible: PASS\n");
}

static void test_register_engine_rejects_null(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(NULL, &fake_rebind_engine) == false);
    assert(hk_runtime_register_engine_for_testing(rt, NULL) == false);
    hk_runtime_release(rt);
    printf("  register-engine-rejects-null: PASS\n");
}

// The router must pick on ORIGINAL REQUIREMENT when two engines are otherwise
// identical. This is the terminal/relocating inline pair's exact shape: both
// claim function-address targets with entry-point reach, and differ only in
// which originals they can hand back. Without this criterion the first
// registered would always win and the other's capability would be unreachable.
static void test_routes_on_original_requirement(void) {
    // Registered NONE-only FIRST, deliberately: that is the order that used to
    // break, since the narrower engine would have swallowed every request.
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_original_none_engine));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_original_any_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t none_req = symbol_spec("h.none", HK_REACH_ENTRYPOINT, 0);
    none_req.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    none_req.original_requirement = HK_ORIGINAL_NONE;

    hk_hook_spec_t cont_req = none_req;
    cont_req.stable_hook_id = "h.cont";
    cont_req.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;

    hk_hook_t *hn = NULL, *hc = NULL;
    assert(hk_plan_add_hook(plan, &none_req, &hn) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &cont_req, &hc) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);

    // NONE goes to the first eligible, which is the narrow one.
    assert(hn->matched_engine == &fake_original_none_engine);
    // CALLABLE_CONTINUATION SKIPS it and reaches the one that can serve it.
    assert(hc->matched_engine == &fake_original_any_engine);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  routes-on-original-requirement: PASS\n");
}

// An engine that declares nothing keeps its old behaviour exactly: zero is
// read as "any", so adding the field cannot make a previously-eligible engine
// silently ineligible. That property is why every pre-existing fake engine
// needed no edit.
static void test_undeclared_requirements_mean_any(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // fake_rebind_engine declares no original_requirements at all.
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.any", HK_REACH_EXISTING_IMPORTS, 0);
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == &fake_rebind_engine);   // still eligible

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  undeclared-requirements-mean-any: PASS\n");
}

int main(void) {
    test_routes_on_original_requirement();
    test_undeclared_requirements_mean_any();
    test_no_engines_registered_still_no_route();
    test_eligible_engine_upgrades_to_analyzed();
    test_insufficient_reach_stays_no_route();
    test_first_eligible_wins_skips_ineligible();
    test_register_engine_rejects_null();
    printf("all engine registry tests passed\n");
    return 0;
}
