// Host test for the domain preparation gate (spec section 15.1) in
// hk_plan_prepare (src/core/HKPlan.c). The property under test: a
// domain with require_all_mandatory_prepared set, containing a mandatory
// hook with no route, blocks EVERY hook in that domain -- even ones that
// individually would have prepared successfully.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "fake_engines.h"

static hk_domain_spec_t gated_domain_spec(const char *id) {
    hk_domain_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_domain_id = id;
    spec.require_all_mandatory_prepared = true;
    spec.compensation_policy = HK_COMPENSATION_NONE;
    return spec;
}

static hk_hook_spec_t symbol_spec_in_domain(const char *id, const char *symbol,
                                             hk_domain_t *domain, hk_operation_role_t role) {
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
    spec.role = role;
    spec.domain = domain;
    return spec;
}

static void test_no_domain_is_never_gated(void) {
    // Regression check: a hook with spec.domain == NULL must behave
    // exactly as it did before this iteration -- the gate only applies to
    // hooks that opted into a domain.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec_in_domain("hook.a", "getpid", NULL, HK_OPERATION_MANDATORY);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  no-domain-is-never-gated: PASS\n");
}

static void test_gate_off_ignores_failed_mandatory_sibling(void) {
    // require_all_mandatory_prepared == false: a failed mandatory hook in
    // the domain must NOT block its sibling.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t dspec = gated_domain_spec("domain.ungated");
    dspec.require_all_mandatory_prepared = false;
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &dspec, &domain) == HK_STATUS_OK);

    // No engine matches objc targets -> this mandatory hook gets NO_ROUTE.
    hk_hook_spec_t no_route_spec;
    memset(&no_route_spec, 0, sizeof(no_route_spec));
    no_route_spec.struct_size = sizeof(no_route_spec);
    no_route_spec.struct_version = HK_ABI_VERSION_3_0;
    no_route_spec.stable_hook_id = "hook.no-route";
    no_route_spec.target_kind = HK_TARGET_OBJC_METHOD;
    no_route_spec.target.objc.class_name = "NSObject";
    no_route_spec.target.objc.selector_name = "description";
    no_route_spec.required_reach = HK_REACH_OBJC_DISPATCH;
    no_route_spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    no_route_spec.role = HK_OPERATION_MANDATORY;
    no_route_spec.domain = domain;
    hk_hook_t *no_route_hook = NULL;
    assert(hk_plan_add_hook(plan, &no_route_spec, &no_route_hook) == HK_STATUS_OK);

    hk_hook_spec_t ok_spec = symbol_spec_in_domain("hook.ok", "getpid", domain, HK_OPERATION_MANDATORY);
    hk_hook_t *ok_hook = NULL;
    assert(hk_plan_add_hook(plan, &ok_spec, &ok_hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(no_route_hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(ok_hook->result.outcome == HK_OUTCOME_PREPARED);  // NOT blocked -- gate is off

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  gate-off-ignores-failed-mandatory-sibling: PASS\n");
}

static void test_gate_on_blocks_whole_domain(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t dspec = gated_domain_spec("domain.gated");
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &dspec, &domain) == HK_STATUS_OK);

    hk_hook_spec_t no_route_spec;
    memset(&no_route_spec, 0, sizeof(no_route_spec));
    no_route_spec.struct_size = sizeof(no_route_spec);
    no_route_spec.struct_version = HK_ABI_VERSION_3_0;
    no_route_spec.stable_hook_id = "hook.no-route";
    no_route_spec.target_kind = HK_TARGET_OBJC_METHOD;  // no engine registered for this kind
    no_route_spec.target.objc.class_name = "NSObject";
    no_route_spec.target.objc.selector_name = "description";
    no_route_spec.required_reach = HK_REACH_OBJC_DISPATCH;
    no_route_spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    no_route_spec.role = HK_OPERATION_MANDATORY;
    no_route_spec.domain = domain;
    hk_hook_t *no_route_hook = NULL;
    assert(hk_plan_add_hook(plan, &no_route_spec, &no_route_hook) == HK_STATUS_OK);

    // This hook WOULD prepare successfully on its own (fake_rebind_engine
    // matches it) -- the whole point of the test is that the domain gate
    // blocks it anyway, because its mandatory sibling above has no route.
    hk_hook_spec_t would_succeed_spec = symbol_spec_in_domain(
        "hook.would-succeed", "getpid", domain, HK_OPERATION_MANDATORY);
    hk_hook_t *would_succeed_hook = NULL;
    assert(hk_plan_add_hook(plan, &would_succeed_spec, &would_succeed_hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(no_route_hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(would_succeed_hook->result.outcome == HK_OUTCOME_ANALYZED);  // has a route at analyze time

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(would_succeed_hook->result.outcome == HK_OUTCOME_FAILED_SAFE);  // blocked by the gate, not PREPARED
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);  // 1 attempted (the gated one doesn't count as attempted), 0 prepared

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  gate-on-blocks-whole-domain: PASS\n");
}

static void test_gate_on_blocks_after_mandatory_prepare_failure(void) {
    // The first mandatory member prepares successfully, then a later
    // mandatory member fails. The domain gate must release the first member's
    // prepared state and refuse the domain as a whole; leaving the first
    // member PREPARED would make the later commit path able to mutate part of
    // a domain whose preparation contract already failed.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t dspec = gated_domain_spec("domain.prepare-failure");
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &dspec, &domain) == HK_STATUS_OK);

    hk_hook_spec_t ok_spec = symbol_spec_in_domain(
        "hook.prepare-ok", "getpid", domain, HK_OPERATION_MANDATORY);
    hk_hook_t *ok_hook = NULL;
    assert(hk_plan_add_hook(plan, &ok_spec, &ok_hook) == HK_STATUS_OK);

    hk_hook_spec_t fail_spec = symbol_spec_in_domain(
        "hook.prepare-fail", "getppid", domain, HK_OPERATION_MANDATORY);
    hk_hook_t *fail_hook = NULL;
    assert(hk_plan_add_hook(plan, &fail_spec, &fail_hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(ok_hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert(fail_hook->result.outcome == HK_OUTCOME_ANALYZED);
    fail_hook->matched_engine = &fake_always_fails_engine;

    hk_report_t *report = NULL;
    assert(hk_plan_prepare(plan, &report) == HK_STATUS_OK);
    assert(ok_hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(fail_hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(ok_hook->prepared_state == NULL);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  gate-on-blocks-after-mandatory-prepare-failure: PASS\n");
}

static void test_optional_hook_does_not_trigger_gate(void) {
    // An OPTIONAL hook with no route must not block its domain -- only
    // MANDATORY members count toward the gate (spec section 15.1).
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t dspec = gated_domain_spec("domain.optional-sibling");
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &dspec, &domain) == HK_STATUS_OK);

    hk_hook_spec_t optional_no_route;
    memset(&optional_no_route, 0, sizeof(optional_no_route));
    optional_no_route.struct_size = sizeof(optional_no_route);
    optional_no_route.struct_version = HK_ABI_VERSION_3_0;
    optional_no_route.stable_hook_id = "hook.optional-no-route";
    optional_no_route.target_kind = HK_TARGET_OBJC_METHOD;
    optional_no_route.target.objc.class_name = "NSObject";
    optional_no_route.target.objc.selector_name = "description";
    optional_no_route.required_reach = HK_REACH_OBJC_DISPATCH;
    optional_no_route.availability = HK_AVAILABILITY_REQUIRED_NOW;
    optional_no_route.role = HK_OPERATION_OPTIONAL;  // not mandatory
    optional_no_route.domain = domain;
    hk_hook_t *optional_hook = NULL;
    assert(hk_plan_add_hook(plan, &optional_no_route, &optional_hook) == HK_STATUS_OK);

    hk_hook_spec_t mandatory_ok = symbol_spec_in_domain(
        "hook.mandatory-ok", "getpid", domain, HK_OPERATION_MANDATORY);
    hk_hook_t *mandatory_hook = NULL;
    assert(hk_plan_add_hook(plan, &mandatory_ok, &mandatory_hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(optional_hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(mandatory_hook->result.outcome == HK_OUTCOME_PREPARED);  // not blocked

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  optional-hook-does-not-trigger-gate: PASS\n");
}

int main(void) {
    test_no_domain_is_never_gated();
    test_gate_off_ignores_failed_mandatory_sibling();
    test_gate_on_blocks_whole_domain();
    test_gate_on_blocks_after_mandatory_prepare_failure();
    test_optional_hook_does_not_trigger_gate();
    printf("all domain gate tests passed\n");
    return 0;
}
