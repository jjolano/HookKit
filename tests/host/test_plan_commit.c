// Host test for hk_plan_commit (src/core/HKPlan.c). The property
// under test is the mutation-state -> outcome mapping (spec section
// 4.4/6.27) -- one of the spec's core invariants -- exercised for real
// via 4 distinct fake engines (tests/host/fake_engines.h) rather than
// trusted from reading the switch statement.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/core/HKOwnership.h"
#include "fake_engines.h"

static bool ids_equal(hk_id_t a, hk_id_t b) {
    return a.high == b.high && a.low == b.low;
}

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
    assert(report->results[0].verified);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  complete-mutation-becomes-active: PASS\n");
}

static void test_undeclared_commit_effect_becomes_unknown(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_undeclared_effect_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_UNKNOWN);
    assert(report->results[0].mutation == HK_MUTATION_UNKNOWN);
    assert(report->results[0].observed_commit_effects == HK_EFFECT_MEMORY_MUTATION);
    assert(report->results[0].error_code == HK_STATUS_INTERNAL_ERROR);
    assert(strcmp(report->results[0].error_message.data,
                  "engine produced an undeclared commit effect") == 0);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  undeclared-commit-effect-becomes-unknown: PASS\n");
}

static void test_failed_verification_becomes_unknown(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_verification_failure_engine, "hook.verify");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_UNKNOWN);
    assert(report->results[0].mutation == HK_MUTATION_UNKNOWN);
    assert(!report->results[0].verified);
    assert(strcmp(report->results[0].error_message.data,
                  "fake readback mismatch") == 0);

    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    hk_artifact_t artifact;
    assert(hk_artifact_snapshot_count(snapshot) == 1);
    assert(hk_artifact_snapshot_copy_at(snapshot, 0, &artifact) == HK_STATUS_OK);
    assert(artifact.state == HK_ARTIFACT_COMMITTED);
    assert(!artifact.verified);
    hk_artifact_snapshot_release(snapshot);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  failed-verification-becomes-unknown: PASS\n");
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

static void test_commit_records_artifact_with_stamped_ids(void) {
    // The real end-to-end proof of the artifact ledger: a committed hook's
    // engine records an artifact, and it reaches the report's snapshot with
    // the engine's mechanism facts intact AND the four contextual IDs
    // stamped by the sink (which the engine could not have forged -- it
    // only ever saw an hk_hook_spec_t).
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_t *hook = prepared_hook(rt, plan, &fake_rebind_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);

    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    // Mechanism facts the engine filled.
    assert(a.kind == HK_ARTIFACT_IMPORT_SLOT);
    assert(a.state == HK_ARTIFACT_VERIFIED);
    assert(a.verified);
    assert(a.effects == HK_EFFECT_IMPORT_MUTATION);
    assert(a.import_slot_address == 0xF00D1000);
    assert(a.mechanically_reversible);
    assert(a.engine_id.length == strlen("fake-rebind"));
    assert(memcmp(a.engine_id.data, "fake-rebind", a.engine_id.length) == 0);
    // Contextual IDs the sink stamped -- the actual point of the test.
    assert(a.artifact_id.high != 0 || a.artifact_id.low != 0);  // a real generated id
    assert(ids_equal(a.plan_id, plan->plan_id));
    assert(ids_equal(a.request_id, hook->hook_id));
    assert(ids_equal(a.runtime_owner_id, hk_runtime_owner_id(rt)));

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  commit-records-artifact-with-stamped-ids: PASS\n");
}

static void test_failed_artifact_record_becomes_unknown(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_failed_artifact_record_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_FAILED_UNKNOWN);
    assert(report->results[0].mutation == HK_MUTATION_UNKNOWN);
    assert(report->results[0].observed_commit_effects == HK_EFFECT_IMPORT_MUTATION);
    assert(report->results[0].artifact_count == 0);

    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 0);
    hk_artifact_snapshot_release(snapshot);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  failed-artifact-record-becomes-unknown: PASS\n");
}

static void test_refused_commit_records_no_artifact(void) {
    // HK_MUTATION_NONE means the engine touched nothing -- so it must have
    // recorded nothing. No phantom artifact for a refused commit.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    prepared_hook(rt, plan, &fake_commit_none_engine, "hook.a");

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 0);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  refused-commit-records-no-artifact: PASS\n");
}

static void test_each_hook_stamps_its_own_request_id(void) {
    // Two hooks, same engine. Each artifact's request_id must be ITS OWN
    // hook's id -- catches a request_id set once outside the commit loop
    // (all artifacts would share the last hook's id).
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec0 = symbol_spec("hook.0", "getpid");
    hk_hook_t *hook0 = NULL;
    assert(hk_plan_add_hook(plan, &spec0, &hook0) == HK_STATUS_OK);
    hk_hook_spec_t spec1 = symbol_spec("hook.1", "getppid");
    hk_hook_t *hook1 = NULL;
    assert(hk_plan_add_hook(plan, &spec1, &hook1) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 2);

    // Ledger append order follows commit-loop (hook) order.
    hk_artifact_t a0, a1;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a0) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(snap, 1, &a1) == HK_STATUS_OK);
    assert(ids_equal(a0.request_id, hook0->hook_id));
    assert(ids_equal(a1.request_id, hook1->hook_id));
    assert(!ids_equal(a0.request_id, a1.request_id));       // distinct, not a shared stale id
    assert(!ids_equal(a0.artifact_id, a1.artifact_id));     // each got its own fresh id

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  each-hook-stamps-its-own-request-id: PASS\n");
}

static void test_grouped_commit_revalidates_and_verifies_each_member(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(
        rt, &fake_group_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t first_spec = symbol_spec("group.commit.first", "getpid");
    hk_hook_t *first = NULL;
    assert(hk_plan_add_hook(plan, &first_spec, &first) == HK_STATUS_OK);
    hk_hook_spec_t second_spec = symbol_spec("group.commit.second", "getppid");
    hk_hook_t *second = NULL;
    assert(hk_plan_add_hook(plan, &second_spec, &second) == HK_STATUS_OK);

    fake_group_prepare_reset();
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(fake_group_prepare_calls == 1);
    assert(fake_group_prepare_members == 2);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake_group_revalidate_calls == 1);
    assert(fake_group_commit_calls == 1);
    assert(fake_group_verify_calls == 1);
    assert(first->result.outcome == HK_OUTCOME_ACTIVE);
    assert(second->result.outcome == HK_OUTCOME_ACTIVE);
    assert(first->result.verified);
    assert(second->result.verified);

    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 2);
    hk_artifact_t first_artifact;
    hk_artifact_t second_artifact;
    assert(hk_artifact_snapshot_copy_at(snapshot, 0, &first_artifact) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(snapshot, 1, &second_artifact) == HK_STATUS_OK);
    assert(ids_equal(first_artifact.request_id, first->hook_id));
    assert(ids_equal(second_artifact.request_id, second->hook_id));
    assert(first_artifact.state == HK_ARTIFACT_VERIFIED);
    assert(second_artifact.state == HK_ARTIFACT_VERIFIED);

    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  grouped-commit-revalidates-and-verifies-each-member: PASS\n");
}

static void test_grouped_commit_compensates_verified_siblings(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(
        rt, &fake_group_compensating_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t first_spec = symbol_spec("group.compensate.first", "getpid");
    hk_hook_t *first = NULL;
    assert(hk_plan_add_hook(plan, &first_spec, &first) == HK_STATUS_OK);
    hk_hook_spec_t second_spec = symbol_spec("group.compensate.second", "getppid");
    hk_hook_t *second = NULL;
    assert(hk_plan_add_hook(plan, &second_spec, &second) == HK_STATUS_OK);

    fake_group_prepare_reset();
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake_group_verify_calls == 1);
    assert(fake_group_compensate_calls == 1);
    assert(first->result.outcome == HK_OUTCOME_COMPENSATED);
    assert(second->result.outcome == HK_OUTCOME_COMPENSATED);
    assert(first->result.mutation == HK_MUTATION_COMPLETE);
    assert(second->result.mutation == HK_MUTATION_UNKNOWN);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 2);
    hk_artifact_t first_artifact;
    hk_artifact_t second_artifact;
    assert(hk_artifact_snapshot_copy_at(snapshot, 0, &first_artifact) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(snapshot, 1, &second_artifact) == HK_STATUS_OK);
    assert(first_artifact.state == HK_ARTIFACT_COMPENSATED);
    assert(second_artifact.state == HK_ARTIFACT_COMPENSATED);
    assert(!first_artifact.verified);
    assert(!second_artifact.verified);

    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  grouped-commit-compensates-verified-siblings: PASS\n");
}

static void test_commit_honors_order_and_dependencies(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_order_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t low_domain_spec;
    memset(&low_domain_spec, 0, sizeof(low_domain_spec));
    low_domain_spec.struct_size = sizeof(low_domain_spec);
    low_domain_spec.struct_version = HK_ABI_VERSION_3_0;
    low_domain_spec.stable_domain_id = "order.low";
    low_domain_spec.domain_order = 0;
    hk_domain_t *low_domain = NULL;
    assert(hk_plan_define_domain(plan, &low_domain_spec, &low_domain) == HK_STATUS_OK);

    hk_domain_spec_t high_domain_spec = low_domain_spec;
    high_domain_spec.stable_domain_id = "order.high";
    high_domain_spec.domain_order = 1;
    hk_domain_t *high_domain = NULL;
    assert(hk_plan_define_domain(plan, &high_domain_spec, &high_domain) == HK_STATUS_OK);

    hk_hook_spec_t first_spec = symbol_spec("order.first", "getpid");
    first_spec.commit_order = 30;
    first_spec.domain = high_domain;
    hk_hook_t *first = NULL;
    assert(hk_plan_add_hook(plan, &first_spec, &first) == HK_STATUS_OK);

    hk_hook_spec_t dependent_spec = symbol_spec("order.dependent", "getppid");
    dependent_spec.commit_order = 10;
    dependent_spec.domain = high_domain;
    const hk_hook_t *dependencies[] = {first};
    dependent_spec.commit_after = dependencies;
    dependent_spec.commit_after_count = 1;
    hk_hook_t *dependent = NULL;
    assert(hk_plan_add_hook(plan, &dependent_spec, &dependent) == HK_STATUS_OK);

    hk_hook_spec_t middle_spec = symbol_spec("order.middle", "getuid");
    middle_spec.commit_order = 100;
    middle_spec.domain = low_domain;
    hk_hook_t *middle = NULL;
    assert(hk_plan_add_hook(plan, &middle_spec, &middle) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    fake_commit_order_log_reset();
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake_commit_order_log_count == 3);
    // `dependent` has the smallest numeric order but cannot precede `first`;
    // the independent middle hook can.
    assert(strcmp(fake_commit_order_log[0], "order.middle") == 0);
    assert(strcmp(fake_commit_order_log[1], "order.first") == 0);
    assert(strcmp(fake_commit_order_log[2], "order.dependent") == 0);
    assert(first->result.outcome == HK_OUTCOME_ACTIVE);
    assert(middle->result.outcome == HK_OUTCOME_ACTIVE);
    assert(dependent->result.outcome == HK_OUTCOME_ACTIVE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  commit-honors-order-and-dependencies: PASS\n");
}

static void test_failed_dependency_blocks_dependents(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_order_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t first_spec = symbol_spec("blocked.first", "getpid");
    hk_hook_t *first = NULL;
    assert(hk_plan_add_hook(plan, &first_spec, &first) == HK_STATUS_OK);
    hk_hook_spec_t dependent_spec = symbol_spec("blocked.dependent", "getppid");
    const hk_hook_t *dependencies[] = {first};
    dependent_spec.commit_after = dependencies;
    dependent_spec.commit_after_count = 1;
    hk_hook_t *dependent = NULL;
    assert(hk_plan_add_hook(plan, &dependent_spec, &dependent) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    // Simulate a clean refusal by the prerequisite after preparation. The
    // dependent must not be dispatched after a dependency is inactive.
    first->matched_engine = &fake_commit_none_engine;
    fake_commit_order_log_reset();
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake_commit_order_log_count == 0);
    assert(first->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(dependent->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(dependent->result.error_code == HK_STATUS_INVALID_STATE);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  failed-dependency-blocks-dependents: PASS\n");
}

static void test_domain_preflight_blocks_stale_mandatory(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_order_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t domain_spec;
    memset(&domain_spec, 0, sizeof(domain_spec));
    domain_spec.struct_size = sizeof(domain_spec);
    domain_spec.struct_version = HK_ABI_VERSION_3_0;
    domain_spec.stable_domain_id = "commit.stale";
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &domain_spec, &domain) == HK_STATUS_OK);

    hk_hook_spec_t mandatory_spec = symbol_spec("commit.stale.mandatory", "getpid");
    mandatory_spec.domain = domain;
    hk_hook_t *mandatory = NULL;
    assert(hk_plan_add_hook(plan, &mandatory_spec, &mandatory) == HK_STATUS_OK);

    hk_hook_spec_t sibling_spec = symbol_spec("commit.stale.sibling", "getppid");
    sibling_spec.domain = domain;
    hk_hook_t *sibling = NULL;
    assert(hk_plan_add_hook(plan, &sibling_spec, &sibling) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    // Host catalog generation is zero. Make the mandatory member stale after
    // preparation; the sibling must not mutate before this is discovered.
    mandatory->result.image_generation++;

    fake_commit_order_log_reset();
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake_commit_order_log_count == 0);
    assert(mandatory->result.outcome == HK_OUTCOME_STALE_PLAN);
    assert(sibling->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(report->results[0].outcome == HK_OUTCOME_STALE_PLAN);
    assert(report->results[1].outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  domain-preflight-blocks-stale-mandatory: PASS\n");
}

static void test_domain_preflight_blocks_unprepared_mandatory(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_order_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t domain_spec;
    memset(&domain_spec, 0, sizeof(domain_spec));
    domain_spec.struct_size = sizeof(domain_spec);
    domain_spec.struct_version = HK_ABI_VERSION_3_0;
    domain_spec.stable_domain_id = "commit.unprepared";
    domain_spec.require_all_mandatory_prepared = false;
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &domain_spec, &domain) == HK_STATUS_OK);

    hk_hook_spec_t no_route_spec;
    memset(&no_route_spec, 0, sizeof(no_route_spec));
    no_route_spec.struct_size = sizeof(no_route_spec);
    no_route_spec.struct_version = HK_ABI_VERSION_3_0;
    no_route_spec.stable_hook_id = "commit.unprepared.mandatory";
    no_route_spec.target_kind = HK_TARGET_OBJC_METHOD;
    no_route_spec.target.objc.class_name = "NSObject";
    no_route_spec.target.objc.selector_name = "description";
    no_route_spec.required_reach = HK_REACH_OBJC_DISPATCH;
    no_route_spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    no_route_spec.role = HK_OPERATION_MANDATORY;
    no_route_spec.domain = domain;
    hk_hook_t *no_route = NULL;
    assert(hk_plan_add_hook(plan, &no_route_spec, &no_route) == HK_STATUS_OK);

    hk_hook_spec_t sibling_spec = symbol_spec("commit.unprepared.sibling", "getpid");
    sibling_spec.domain = domain;
    hk_hook_t *sibling = NULL;
    assert(hk_plan_add_hook(plan, &sibling_spec, &sibling) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(no_route->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(sibling->result.outcome == HK_OUTCOME_PREPARED);

    fake_commit_order_log_reset();
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake_commit_order_log_count == 0);
    assert(no_route->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(sibling->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  domain-preflight-blocks-unprepared-mandatory: PASS\n");
}

int main(void) {
    #define RUN_TEST(test) do { hk_ownership_reset_for_testing(); test(); } while (0)
    RUN_TEST(test_commit_requires_prepared_or_partial_state);
    RUN_TEST(test_complete_mutation_becomes_active);
    RUN_TEST(test_undeclared_commit_effect_becomes_unknown);
    RUN_TEST(test_failed_verification_becomes_unknown);
    RUN_TEST(test_none_mutation_becomes_failed_safe);
    RUN_TEST(test_partial_mutation_becomes_failed_partial);
    RUN_TEST(test_unknown_mutation_becomes_failed_unknown);
    RUN_TEST(test_missing_commit_one_treated_as_unknown_not_none);
    RUN_TEST(test_not_prepared_hook_left_untouched);
    RUN_TEST(test_partial_when_some_hooks_fail_commit);
    RUN_TEST(test_commit_records_artifact_with_stamped_ids);
    RUN_TEST(test_failed_artifact_record_becomes_unknown);
    RUN_TEST(test_refused_commit_records_no_artifact);
    RUN_TEST(test_each_hook_stamps_its_own_request_id);
    RUN_TEST(test_grouped_commit_revalidates_and_verifies_each_member);
    RUN_TEST(test_grouped_commit_compensates_verified_siblings);
    RUN_TEST(test_commit_honors_order_and_dependencies);
    RUN_TEST(test_failed_dependency_blocks_dependents);
    RUN_TEST(test_domain_preflight_blocks_stale_mandatory);
    RUN_TEST(test_domain_preflight_blocks_unprepared_mandatory);
    #undef RUN_TEST
    hk_ownership_reset_for_testing();
    printf("all plan commit tests passed\n");
    return 0;
}
