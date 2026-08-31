// Host coverage for M4's chain-aware ownership ledger. Same-target hooks are
// allowed only when the selected engine declares chaining and proves the
// prepared predecessor is the current ownership head.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "fake_engines.h"

static hk_hook_spec_t chain_spec(const char *id, void *replacement) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = "chain_target";
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = HK_ORIGINAL_DIRECT_PREDECESSOR;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    spec.replacement = replacement;
    return spec;
}

static hk_hook_t *commit_one(hk_runtime_t *runtime,
                             const hk_engine_vtable_t *engine,
                             const hk_hook_spec_t *spec,
                             hk_plan_t **out_plan,
                             hk_report_t **out_report) {
    hk_plan_t *plan = NULL;
    assert(hk_runtime_register_engine_for_testing(runtime, engine));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_commit(plan, out_report) == HK_STATUS_OK);
    *out_plan = plan;
    return hook;
}

static void test_same_target_chains_after_runtime_release(void) {
    hk_ownership_reset_for_testing();
    fake_chain_reset();

    hk_runtime_t *first_runtime = NULL;
    assert(hk_runtime_create(NULL, &first_runtime) == HK_STATUS_OK);
    hk_hook_spec_t first_spec = chain_spec(
        "ownership.first", (void *)0xC0DE5100);
    hk_plan_t *first_plan = NULL;
    hk_report_t *first_report = NULL;
    hk_hook_t *first = commit_one(first_runtime, &fake_chain_engine,
                                  &first_spec, &first_plan, &first_report);
    assert(first->result.outcome == HK_OUTCOME_ACTIVE);
    assert(first->result.original_available);
    assert(hk_original_slot_load(hk_hook_original_slot(first)) ==
           FAKE_CHAIN_BASE);
    hk_report_release(first_report);
    hk_plan_release(first_plan);
    hk_runtime_release(first_runtime);

    // A new runtime/plan still sees the process-lifetime ownership head.
    hk_runtime_t *second_runtime = NULL;
    assert(hk_runtime_create(NULL, &second_runtime) == HK_STATUS_OK);
    hk_hook_spec_t second_spec = chain_spec(
        "ownership.second", (void *)0xC0DE5200);
    hk_plan_t *second_plan = NULL;
    hk_report_t *second_report = NULL;
    hk_hook_t *second = commit_one(second_runtime, &fake_chain_engine,
                                   &second_spec, &second_plan, &second_report);
    assert(second->result.outcome == HK_OUTCOME_ACTIVE);
    assert(second->result.original_available);
    assert(hk_original_slot_load(hk_hook_original_slot(second)) ==
           (void *)0xC0DE5100);
    assert(fake_chain_head == (void *)0xC0DE5200);

    hk_report_release(second_report);
    hk_plan_release(second_plan);
    hk_runtime_release(second_runtime);
    hk_installed_reset_for_testing();
    hk_ownership_reset_for_testing();
    printf("  same-target-chains-after-runtime-release: PASS\n");
}

static void test_non_chainable_engine_reports_conflict(void) {
    hk_ownership_reset_for_testing();
    fake_chain_reset();

    hk_runtime_t *first_runtime = NULL;
    assert(hk_runtime_create(NULL, &first_runtime) == HK_STATUS_OK);
    hk_hook_spec_t first_spec = chain_spec(
        "ownership.nonchain.first", (void *)0xC0DE5300);
    hk_plan_t *first_plan = NULL;
    hk_report_t *first_report = NULL;
    (void)commit_one(first_runtime, &fake_chain_engine,
                     &first_spec, &first_plan, &first_report);
    hk_report_release(first_report);
    hk_plan_release(first_plan);
    hk_runtime_release(first_runtime);

    hk_runtime_t *second_runtime = NULL;
    assert(hk_runtime_create(NULL, &second_runtime) == HK_STATUS_OK);
    hk_hook_spec_t second_spec = chain_spec(
        "ownership.nonchain.second", (void *)0xC0DE5400);
    hk_plan_t *second_plan = NULL;
    hk_report_t *second_report = NULL;
    hk_hook_t *second = commit_one(second_runtime, &fake_rebind_engine,
                                   &second_spec, &second_plan, &second_report);
    assert(second->result.outcome == HK_OUTCOME_CONFLICT);
    assert(second->result.mutation == HK_MUTATION_NONE);
    assert(fake_chain_head == (void *)0xC0DE5300);

    hk_report_release(second_report);
    hk_plan_release(second_plan);
    hk_runtime_release(second_runtime);
    hk_installed_reset_for_testing();
    hk_ownership_reset_for_testing();
    printf("  non-chainable-engine-reports-conflict: PASS\n");
}

static void test_stale_predecessor_writes_nothing(void) {
    hk_ownership_reset_for_testing();
    fake_chain_reset();

    hk_runtime_t *first_runtime = NULL;
    assert(hk_runtime_create(NULL, &first_runtime) == HK_STATUS_OK);
    hk_hook_spec_t first_spec = chain_spec(
        "ownership.stale.first", (void *)0xC0DE5500);
    hk_plan_t *first_plan = NULL;
    hk_report_t *first_report = NULL;
    (void)commit_one(first_runtime, &fake_chain_engine,
                     &first_spec, &first_plan, &first_report);
    hk_report_release(first_report);
    hk_plan_release(first_plan);
    hk_runtime_release(first_runtime);

    hk_runtime_t *second_runtime = NULL;
    assert(hk_runtime_create(NULL, &second_runtime) == HK_STATUS_OK);
    hk_plan_t *plan = NULL;
    assert(hk_runtime_register_engine_for_testing(
        second_runtime, &fake_chain_engine));
    assert(hk_plan_create(second_runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t second_spec = chain_spec(
        "ownership.stale.second", (void *)0xC0DE5600);
    hk_hook_t *second = NULL;
    assert(hk_plan_add_hook(plan, &second_spec, &second) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    // An external writer changed the slot after prepare. The core's owned
    // head and the engine's captured predecessor now disagree; no store is
    // permitted.
    fake_chain_head = (void *)0xC0DE5BAD;
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(second->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(second->result.mutation == HK_MUTATION_NONE);
    assert(fake_chain_head == (void *)0xC0DE5BAD);
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 0);

    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(second_runtime);
    hk_installed_reset_for_testing();
    hk_ownership_reset_for_testing();
    printf("  stale-predecessor-writes-nothing: PASS\n");
}

static void test_prepared_competing_plans_fail_closed(void) {
    hk_ownership_reset_for_testing();
    fake_chain_reset();

    hk_runtime_t *first_runtime = NULL;
    hk_runtime_t *second_runtime = NULL;
    assert(hk_runtime_create(NULL, &first_runtime) == HK_STATUS_OK);
    assert(hk_runtime_create(NULL, &second_runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(first_runtime,
                                                  &fake_chain_engine));
    assert(hk_runtime_register_engine_for_testing(second_runtime,
                                                  &fake_chain_engine));

    hk_plan_t *first_plan = NULL;
    hk_plan_t *second_plan = NULL;
    assert(hk_plan_create(first_runtime, NULL, &first_plan) == HK_STATUS_OK);
    assert(hk_plan_create(second_runtime, NULL, &second_plan) == HK_STATUS_OK);
    hk_hook_spec_t first_spec = chain_spec(
        "ownership.concurrent.first", (void *)0xC0DE5700);
    hk_hook_spec_t second_spec = chain_spec(
        "ownership.concurrent.second", (void *)0xC0DE5800);
    hk_hook_t *first = NULL;
    hk_hook_t *second = NULL;
    assert(hk_plan_add_hook(first_plan, &first_spec, &first) == HK_STATUS_OK);
    assert(hk_plan_add_hook(second_plan, &second_spec, &second) == HK_STATUS_OK);
    assert(hk_plan_analyze(first_plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_analyze(second_plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(first_plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(second_plan, NULL) == HK_STATUS_OK);

    hk_report_t *first_report = NULL;
    hk_report_t *second_report = NULL;
    assert(hk_plan_commit(first_plan, &first_report) == HK_STATUS_OK);
    assert(first->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hk_plan_commit(second_plan, &second_report) == HK_STATUS_OK);
    assert(second->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(second->result.mutation == HK_MUTATION_NONE);
    assert(fake_chain_head == (void *)0xC0DE5700);

    hk_report_release(first_report);
    hk_report_release(second_report);
    hk_plan_release(first_plan);
    hk_plan_release(second_plan);
    hk_runtime_release(first_runtime);
    hk_runtime_release(second_runtime);
    hk_installed_reset_for_testing();
    hk_ownership_reset_for_testing();
    printf("  prepared-competing-plans-fail-closed: PASS\n");
}

int main(void) {
    test_same_target_chains_after_runtime_release();
    test_non_chainable_engine_reports_conflict();
    test_stale_predecessor_writes_nothing();
    test_prepared_competing_plans_fail_closed();
    printf("all ownership tests passed\n");
    return 0;
}
