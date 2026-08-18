// End-to-end: the plan lifecycle driving the real memory-patch engine through
// its runtime adapter (HKMemoryVtable.h). Proves the vtable-adapter pattern
// generalizes to a second engine and a second target kind
// (HK_TARGET_MEMORY_PATCH), and exercises the plan's memory-target path.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "../../Sources/Engines/HKMemoryVtable.h"

#define REGION 4u
static const uint8_t ORIGINAL[REGION]    = {0x11, 0x22, 0x33, 0x44};
static const uint8_t REPLACEMENT[REGION] = {0xAA, 0xBB, 0xCC, 0xDD};

static bool buffer_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    (void)ctx;
    memcpy((void *)address, data, size);
    return true;
}

static hk_bytes_view_t view(const uint8_t *d, size_t n) {
    hk_bytes_view_t v; v.data = d; v.size = n; return v;
}

// A memory-patch hook. `relative` chooses absolute vs image-relative
// addressing so both paths through the adapter are covered.
static hk_hook_spec_t memory_spec(const char *id, uintptr_t address, bool relative,
                                  const uint8_t *expected, const uint8_t *replacement) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_MEMORY_PATCH;
    spec.target.memory.struct_size = sizeof(spec.target.memory);
    spec.target.memory.struct_version = HK_ABI_VERSION_3_0;
    spec.target.memory.address = address;
    spec.target.memory.address_is_image_relative = relative;
    spec.target.memory.size = REGION;
    spec.target.memory.kind = HK_MEMORY_KIND_CODE;
    if (expected)    spec.target.memory.expected_bytes = view(expected, REGION);
    if (replacement) spec.target.memory.replacement_bytes = view(replacement, REGION);
    spec.required_reach = HK_REACH_EXACT_MEMORY;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void set_env(uintptr_t image_base) {
    hk_memory_binding_env_t env;
    env.image_base = image_base;
    env.write = buffer_write;
    env.write_ctx = NULL;
    hk_memory_vtable_set_environment_for_testing(&env);
}

static void test_absolute_patch_full_lifecycle(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_memory_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    set_env(0);  // absolute addressing needs no image base

    hk_hook_spec_t spec = memory_spec("patch.abs", (uintptr_t)region, false,
                                      ORIGINAL, REPLACEMENT);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_memory_vtable());  // routed on memory-target kind
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(memcmp(region, ORIGINAL, REGION) == 0);  // prepare mutated nothing

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(memcmp(region, REPLACEMENT, REGION) == 0);  // patched

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_MEMORY_PATCH);
    assert(a.effects == HK_EFFECT_MEMORY_MUTATION);
    assert(a.address == (uintptr_t)region && a.size == REGION);
    assert(memcmp(a.original_bytes.inline_bytes.data, ORIGINAL, REGION) == 0);
    assert(a.request_id.high == hook->hook_id.high && a.request_id.low == hook->hook_id.low);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_memory_vtable_reset_for_testing();
    printf("  absolute-patch-full-lifecycle: PASS\n");
}

static void test_image_relative_patch(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    const uintptr_t offset = 0x40;

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_memory_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    // image_base chosen so base + offset resolves to the region.
    set_env((uintptr_t)region - offset);

    hk_hook_spec_t spec = memory_spec("patch.rel", offset, true, ORIGINAL, REPLACEMENT);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(memcmp(region, REPLACEMENT, REGION) == 0);  // relative address resolved correctly

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_memory_vtable_reset_for_testing();
    printf("  image-relative-patch: PASS\n");
}

static void test_precondition_failure_surfaces_at_prepare(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    uint8_t wrong_expected[REGION] = {0x11, 0x22, 0x33, 0x99};

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_memory_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    set_env(0);

    hk_hook_spec_t spec = memory_spec("patch.bad", (uintptr_t)region, false,
                                      wrong_expected, REPLACEMENT);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_memory_vtable());  // routed...
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);  // ...refused at prepare
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);
    assert(memcmp(region, ORIGINAL, REGION) == 0);  // untouched

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_memory_vtable_reset_for_testing();
    printf("  precondition-failure-surfaces-at-prepare: PASS\n");
}

int main(void) {
    test_absolute_patch_full_lifecycle();
    test_image_relative_patch();
    test_precondition_failure_surfaces_at_prepare();
    printf("all memory wired tests passed\n");
    return 0;
}
