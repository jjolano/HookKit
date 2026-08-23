// Host test for the process-lifetime installed record: original slots and
// installed handles (Sources/Core/HKInstalled.c, plus the accessors in
// HKPlan.c). The property that matters most -- and is easiest to get wrong
// -- is SURVIVAL: an original slot must stay loadable after the plan,
// runtime, and hook that created it are all released.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKInstalled.h"
#include "../../Sources/Core/HKOwnership.h"
#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "fake_engines.h"

static hk_hook_spec_t symbol_spec(const char *id, hk_original_requirement_t orig) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = "getpid";
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = orig;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

// Drives one hook through DRAFT -> ACTIVE against `engine`, returning it.
static hk_hook_t *committed_hook(hk_runtime_t *rt, hk_plan_t *plan,
                                 const hk_engine_vtable_t *engine,
                                 hk_original_requirement_t orig) {
    assert(hk_runtime_register_engine_for_testing(rt, engine));
    hk_hook_spec_t spec = symbol_spec("hook.a", orig);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    hk_report_release(report);
    return hook;
}

static void test_active_hook_gets_handle_and_slot(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_t *hook = committed_hook(rt, plan, &fake_rebind_original_engine,
                                     HK_ORIGINAL_CALLABLE_CONTINUATION);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);

    const hk_installed_hook_t *handle = hk_hook_installed_handle(hook);
    assert(handle != NULL);
    const hk_original_slot_t *slot = hk_hook_original_slot(hook);
    assert(slot != NULL);
    assert(hk_original_slot_load(slot) == FAKE_ORIGINAL_PTR);

    // Result fields wired from the installation.
    assert(hook->result.original_available);
    assert(hook->result.installed_id.high != 0 || hook->result.installed_id.low != 0);

    // The handle's stored result snapshot matches the hook's final result.
    hk_hook_result_t r;
    assert(hk_installed_hook_copy_result(handle, &r) == HK_STATUS_OK);
    assert(r.outcome == HK_OUTCOME_ACTIVE);
    assert(r.installed_id.high == hook->result.installed_id.high);
    assert(r.installed_id.low == hook->result.installed_id.low);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_installed_reset_for_testing();
    printf("  active-hook-gets-handle-and-slot: PASS\n");
}

static void test_original_slot_survives_plan_and_runtime_release(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_t *hook = committed_hook(rt, plan, &fake_rebind_original_engine,
                                     HK_ORIGINAL_CALLABLE_CONTINUATION);

    // Capture the slot BEFORE releasing anything -- this is exactly what a
    // live replacement does (it holds the slot, not the hook).
    const hk_original_slot_t *slot = hk_hook_original_slot(hook);
    assert(slot != NULL);

    hk_plan_release(plan);      // hook is gone now
    hk_runtime_release(rt);     // runtime wrapper gone too

    // The slot must still resolve -- the whole reason it lives in a
    // process-global registry rather than being owned by the plan/hook.
    assert(hk_original_slot_load(slot) == FAKE_ORIGINAL_PTR);

    hk_installed_reset_for_testing();
    printf("  original-slot-survives-plan-and-runtime-release: PASS\n");
}

static void test_engine_without_original_gives_no_slot(void) {
    // fake_rebind_engine commits ACTIVE but publishes no original -- so no
    // installed record, no slot, and original_available stays false.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_t *hook = committed_hook(rt, plan, &fake_rebind_engine, HK_ORIGINAL_NONE);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);

    assert(hk_hook_installed_handle(hook) == NULL);
    assert(hk_hook_original_slot(hook) == NULL);
    assert(!hook->result.original_available);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_installed_reset_for_testing();  // nothing to free, but keep the pattern
    printf("  engine-without-original-gives-no-slot: PASS\n");
}

static void test_non_active_hook_gives_no_slot(void) {
    // No engine registered -> NO_ROUTE, never ACTIVE -> no installation.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_ORIGINAL_CALLABLE_CONTINUATION);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    hk_report_release(report);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);

    assert(hk_hook_installed_handle(hook) == NULL);
    assert(hk_hook_original_slot(hook) == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  non-active-hook-gives-no-slot: PASS\n");
}

static void test_null_tolerance(void) {
    assert(hk_hook_original_slot(NULL) == NULL);
    assert(hk_original_slot_load(NULL) == NULL);
    assert(hk_hook_installed_handle(NULL) == NULL);

    hk_hook_result_t r;
    assert(hk_installed_hook_copy_result(NULL, &r) == HK_STATUS_INVALID_ARGUMENT);
    // A real handle with a NULL out-param.
    hk_hook_result_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.outcome = HK_OUTCOME_ACTIVE;
    hk_installed_hook_t *rec = hk_installed_record_create((hk_id_t){0}, NULL, &sample);
    assert(rec != NULL);
    assert(hk_installed_hook_copy_result(rec, NULL) == HK_STATUS_INVALID_ARGUMENT);

    hk_installed_reset_for_testing();
    printf("  null-tolerance: PASS\n");
}

int main(void) {
    #define RUN_TEST(test) do { hk_ownership_reset_for_testing(); test(); } while (0)
    RUN_TEST(test_active_hook_gets_handle_and_slot);
    RUN_TEST(test_original_slot_survives_plan_and_runtime_release);
    RUN_TEST(test_engine_without_original_gives_no_slot);
    RUN_TEST(test_non_active_hook_gives_no_slot);
    RUN_TEST(test_null_tolerance);
    #undef RUN_TEST
    hk_ownership_reset_for_testing();
    printf("all installed/original tests passed\n");
    return 0;
}
