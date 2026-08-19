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

// The engine context is an ordinary caller-owned struct now -- no file-scoped
// environment, so each test owns its own.
static hk_memory_engine_ctx_t engine_ctx_for(uintptr_t image_base) {
    hk_memory_engine_ctx_t c;
    memset(&c, 0, sizeof(c));  // a later field must default to "not supplied", not to stack garbage
    c.image_base = image_base;
    c.write = buffer_write;
    c.write_ctx = NULL;
    return c;
}

static void test_absolute_patch_full_lifecycle(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);

    hk_memory_engine_ctx_t ectx = engine_ctx_for(0);  // absolute addressing needs no image base
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_memory_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

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
    printf("  absolute-patch-full-lifecycle: PASS\n");
}

static void test_image_relative_patch(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    const uintptr_t offset = 0x40;

    // image_base chosen so base + offset resolves to the region.
    hk_memory_engine_ctx_t ectx = engine_ctx_for((uintptr_t)region - offset);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_memory_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

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
    printf("  image-relative-patch: PASS\n");
}

static void test_precondition_failure_surfaces_at_prepare(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    uint8_t wrong_expected[REGION] = {0x11, 0x22, 0x33, 0x99};

    hk_memory_engine_ctx_t ectx = engine_ctx_for(0);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_memory_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

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
    printf("  precondition-failure-surfaces-at-prepare: PASS\n");
}

// base_image enforced for an image-relative target. The memory adapter takes
// image_base by hand (standing in for a catalog lookup the dyld populator will
// eventually make), so this check is what catches a hand-passed base that
// disagrees with the image the request actually named.
static void test_base_image_is_enforced_for_relative_targets(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    const uintptr_t offset = 0x40;
    const uintptr_t image_base = (uintptr_t)region - offset;

    // A synthetic image whose __TEXT span covers the region.
    uint8_t hdr[0x200];
    memset(hdr, 0, sizeof(hdr));
    const uint64_t seg_base = (uint64_t)(image_base & ~(uintptr_t)0xFFF);
    memcpy(hdr + 0, (uint32_t[]){0xFEEDFACFu}, 4);
    memcpy(hdr + 16, (uint32_t[]){1u}, 4);
    memcpy(hdr + 20, (uint32_t[]){72u}, 4);
    memcpy(hdr + 32, (uint32_t[]){0x19u}, 4);
    memcpy(hdr + 36, (uint32_t[]){72u}, 4);
    memcpy(hdr + 40, "__TEXT", 6);
    memcpy(hdr + 56, &seg_base, 8);
    memcpy(hdr + 64, (uint64_t[]){0x2000ull}, 8);

    hk_image_catalog_t *cat = hk_image_catalog_create();
    hk_image_entry_t e;
    memset(&e, 0, sizeof(e));
    e.path = "/usr/lib/libtarget.dylib";
    e.header = hdr;
    assert(hk_image_catalog_add_entry(cat, &e));

    hk_memory_engine_ctx_t ectx = engine_ctx_for(image_base);
    ectx.catalog = cat;

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_memory_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t right = memory_spec("p.right", offset, true, ORIGINAL, REPLACEMENT);
    right.target.memory.base_image.struct_size = sizeof(right.target.memory.base_image);
    right.target.memory.base_image.struct_version = HK_ABI_VERSION_3_0;
    right.target.memory.base_image.kind = HK_IMAGE_EXACT_PATH;
    right.target.memory.base_image.path = "/usr/lib/libtarget.dylib";

    hk_hook_spec_t wrong = right;
    wrong.stable_hook_id = "p.wrong";
    wrong.target.memory.base_image.path = "/usr/lib/libnotloaded.dylib";

    hk_hook_t *hr = NULL, *hw = NULL;
    assert(hk_plan_add_hook(plan, &right, &hr) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &wrong, &hw) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hr->result.outcome == HK_OUTCOME_PREPARED);
    assert(hw->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hw->result.error_code ==
           HK_MEMORY_DIAG_IMAGE_SCOPE_BASE + (int64_t)HK_IMAGE_SCOPE_NO_MATCH);
    assert(memcmp(region, ORIGINAL, REGION) == 0);  // neither wrote anything yet

    hk_plan_release(plan);
    hk_runtime_release(rt);

    // An ABSOLUTE target is not claimed to live anywhere in particular, so the
    // same wrong base_image must NOT be checked against it -- checking a
    // selector the request never meant would invent a requirement.
    hk_runtime_t *rt2 = NULL;
    hk_plan_t *p2 = NULL;
    assert(hk_runtime_create(NULL, &rt2) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt2, hk_memory_vtable(), &ectx));
    assert(hk_plan_create(rt2, NULL, &p2) == HK_STATUS_OK);
    hk_hook_spec_t absolute = memory_spec("p.abs", (uintptr_t)region, false, ORIGINAL, REPLACEMENT);
    absolute.target.memory.base_image = wrong.target.memory.base_image;  // deliberately wrong
    hk_hook_t *habs = NULL;
    assert(hk_plan_add_hook(p2, &absolute, &habs) == HK_STATUS_OK);
    assert(hk_plan_analyze(p2, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(p2, NULL) == HK_STATUS_OK);
    assert(habs->result.outcome == HK_OUTCOME_PREPARED);

    hk_plan_release(p2);
    hk_runtime_release(rt2);
    hk_image_catalog_destroy(cat);
    printf("  base-image-is-enforced-for-relative-targets: PASS\n");
}

int main(void) {
    test_absolute_patch_full_lifecycle();
    test_image_relative_patch();
    test_precondition_failure_surfaces_at_prepare();
    test_base_image_is_enforced_for_relative_targets();
    printf("all memory wired tests passed\n");
    return 0;
}
