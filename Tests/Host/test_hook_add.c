// Host test for hk_plan_add_hook (Sources/Core/HKPlan.c) -- the deep-copy
// of the full target union. Every test that can prove a real copy
// happened (not just "the call returned OK") mutates the caller's buffer
// after the call and re-checks the hook's own copy, the same style as
// test_plan_lifecycle.c's domain-id test.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"

static void make_plan(hk_runtime_t **rt, hk_plan_t **plan) {
    assert(hk_runtime_create(NULL, rt) == HK_STATUS_OK);
    assert(hk_plan_create(*rt, NULL, plan) == HK_STATUS_OK);
}

static hk_hook_spec_t base_spec(const char *id, hk_target_kind_t kind) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = kind;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void test_symbol_target_deep_copied(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    char name[] = "getpid";
    hk_hook_spec_t spec = base_spec("hook.symbol", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.struct_size = sizeof(spec.target.symbol);
    spec.target.symbol.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.name = name;
    spec.target.symbol.name_convention = HK_SYMBOL_NAME_C;
    spec.target.symbol.defining_image.struct_size = sizeof(hk_image_selector_t);
    spec.target.symbol.defining_image.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.defining_image.kind = HK_IMAGE_ANY_LOADED;
    spec.target.symbol.caller_image_scope.struct_size = sizeof(hk_image_selector_t);
    spec.target.symbol.caller_image_scope.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.caller_image_scope.kind = HK_IMAGE_MAIN_EXECUTABLE;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hook != NULL);

    memset(name, 'X', strlen(name));  // mutate caller's buffer after the call

    assert(strcmp(hook->spec.target.symbol.name, "getpid") == 0);
    assert(hook->spec.target.symbol.name != name);
    assert(hook->spec.target.symbol.caller_image_scope.kind == HK_IMAGE_MAIN_EXECUTABLE);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  symbol-target-deep-copied: PASS\n");
}

static void test_objc_target_deep_copied(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    char class_name[] = "NSFileManager";
    char sel_name[] = "fileExistsAtPath:";
    hk_hook_spec_t spec = base_spec("hook.objc", HK_TARGET_OBJC_METHOD);
    spec.target.objc.struct_size = sizeof(spec.target.objc);
    spec.target.objc.struct_version = HK_ABI_VERSION_3_0;
    spec.target.objc.cls = NULL;
    spec.target.objc.class_name = class_name;
    spec.target.objc.sel = NULL;
    spec.target.objc.selector_name = sel_name;
    spec.target.objc.method_kind = HK_OBJC_INSTANCE_METHOD;
    spec.target.objc.inheritance_policy = HK_OBJC_LOCAL_METHOD_ONLY;
    spec.target.objc.availability = HK_AVAILABILITY_REQUIRED_NOW;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    memset(class_name, 'X', strlen(class_name));
    memset(sel_name, 'X', strlen(sel_name));

    assert(strcmp(hook->spec.target.objc.class_name, "NSFileManager") == 0);
    assert(strcmp(hook->spec.target.objc.selector_name, "fileExistsAtPath:") == 0);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  objc-target-deep-copied: PASS\n");
}

static void test_memory_target_bytes_deep_copied(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    uint8_t replacement[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t expected[] = {0x00, 0x00, 0x00, 0x00};
    uint8_t mask[] = {0xFF, 0xFF, 0xFF, 0xFF};

    hk_hook_spec_t spec = base_spec("hook.memory", HK_TARGET_MEMORY_PATCH);
    spec.target.memory.struct_size = sizeof(spec.target.memory);
    spec.target.memory.struct_version = HK_ABI_VERSION_3_0;
    spec.target.memory.address = 0x1000;
    spec.target.memory.address_is_image_relative = false;
    spec.target.memory.replacement_bytes.data = replacement;
    spec.target.memory.replacement_bytes.size = sizeof(replacement);
    spec.target.memory.expected_bytes.data = expected;
    spec.target.memory.expected_bytes.size = sizeof(expected);
    spec.target.memory.expected_mask.data = mask;
    spec.target.memory.expected_mask.size = sizeof(mask);
    spec.target.memory.size = sizeof(replacement);
    spec.target.memory.kind = HK_MEMORY_KIND_CODE;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    memset(replacement, 0, sizeof(replacement));  // mutate after the call

    const hk_bytes_view_t *copied = &hook->spec.target.memory.replacement_bytes;
    assert(copied->size == 4);
    assert(copied->data != (const uint8_t *)replacement);
    assert(memcmp(copied->data, "\xDE\xAD\xBE\xEF", 4) == 0);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  memory-target-bytes-deep-copied: PASS\n");
}

static void test_memory_target_non_relative_base_image_zeroed(void) {
    // Regression test for a real bug caught while writing this code: if
    // address_is_image_relative is false, base_image must never retain a
    // raw, non-owned pointer copied from the caller's struct -- it would
    // dangle the moment the caller's spec goes out of scope.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    char stray_path[] = "/should/not/be/copied";
    hk_hook_spec_t spec = base_spec("hook.memory.norel", HK_TARGET_MEMORY_PATCH);
    spec.target.memory.struct_size = sizeof(spec.target.memory);
    spec.target.memory.struct_version = HK_ABI_VERSION_3_0;
    spec.target.memory.address = 0x2000;
    spec.target.memory.address_is_image_relative = false;
    spec.target.memory.base_image.kind = HK_IMAGE_EXACT_PATH;
    spec.target.memory.base_image.path = stray_path;  // caller mistake: set despite not being relative
    spec.target.memory.kind = HK_MEMORY_KIND_DATA;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    assert(hook->spec.target.memory.base_image.path == NULL);
    assert(hook->spec.target.memory.base_image.kind == HK_IMAGE_ANY_LOADED);  // zeroed struct

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  memory-target-non-relative-base-image-zeroed: PASS\n");
}

static void test_swift_target_kind_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_spec_t spec = base_spec("hook.swift", HK_TARGET_SWIFT_VTABLE);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_UNAVAILABLE);
    assert(hook == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  swift-target-kind-rejected: PASS\n");
}

static void test_explicit_set_image_selector_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_spec_t spec = base_spec("hook.explicitset", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.struct_size = sizeof(spec.target.symbol);
    spec.target.symbol.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.name = "foo";
    spec.target.symbol.defining_image.kind = HK_IMAGE_EXPLICIT_SET;  // not implemented

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_UNAVAILABLE);
    assert(hook == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  explicit-set-image-selector-rejected: PASS\n");
}

static void test_duplicate_hook_id_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_spec_t spec1 = base_spec("dup", HK_TARGET_FUNCTION_SYMBOL);
    spec1.target.symbol.name = "a";
    hk_hook_spec_t spec2 = base_spec("dup", HK_TARGET_FUNCTION_SYMBOL);
    spec2.target.symbol.name = "b";

    hk_hook_t *h1 = NULL, *h2 = NULL;
    assert(hk_plan_add_hook(plan, &spec1, &h1) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &spec2, &h2) == HK_STATUS_INVALID_ARGUMENT);
    assert(h2 == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  duplicate-hook-id-rejected: PASS\n");
}

static void test_contradictory_original_continuation_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_spec_t spec = base_spec("hook.contradiction", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.name = "a";
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    spec.continuation_policy = HK_CONTINUATION_FORBIDDEN;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_INVALID_ARGUMENT);
    assert(hook == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  contradictory-original-continuation-rejected: PASS\n");
}

static void test_foreign_domain_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan_a = NULL, *plan_b = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan_a) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan_b) == HK_STATUS_OK);

    hk_domain_spec_t domain_spec;
    memset(&domain_spec, 0, sizeof(domain_spec));
    domain_spec.struct_size = sizeof(domain_spec);
    domain_spec.struct_version = HK_ABI_VERSION_3_0;
    domain_spec.stable_domain_id = "domain.a";
    hk_domain_t *domain_a = NULL;
    assert(hk_plan_define_domain(plan_a, &domain_spec, &domain_a) == HK_STATUS_OK);

    hk_hook_spec_t spec = base_spec("hook.foreign-domain", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.name = "a";
    spec.domain = domain_a;  // belongs to plan_a, used against plan_b

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan_b, &spec, &hook) == HK_STATUS_INVALID_ARGUMENT);
    assert(hook == NULL);

    hk_plan_release(plan_a);
    hk_plan_release(plan_b);
    hk_runtime_release(rt);
    printf("  foreign-domain-rejected: PASS\n");
}

static void test_same_plan_domain_accepted(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_domain_spec_t domain_spec;
    memset(&domain_spec, 0, sizeof(domain_spec));
    domain_spec.struct_size = sizeof(domain_spec);
    domain_spec.struct_version = HK_ABI_VERSION_3_0;
    domain_spec.stable_domain_id = "domain.local";
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &domain_spec, &domain) == HK_STATUS_OK);

    hk_hook_spec_t spec = base_spec("hook.local-domain", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.name = "a";
    spec.domain = domain;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hook->spec.domain == domain);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  same-plan-domain-accepted: PASS\n");
}

static void test_commit_after_forward_reference_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_t *not_yet_added = (hk_hook_t *)0x1234;  // never actually added to this plan
    const hk_hook_t *deps[] = { not_yet_added };

    hk_hook_spec_t spec = base_spec("hook.forward-ref", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.name = "a";
    spec.commit_after = deps;
    spec.commit_after_count = 1;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_INVALID_ARGUMENT);
    assert(hook == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  commit-after-forward-reference-rejected: PASS\n");
}

static void test_commit_after_deep_copied(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    hk_hook_spec_t first_spec = base_spec("hook.first", HK_TARGET_FUNCTION_SYMBOL);
    first_spec.target.symbol.name = "a";
    hk_hook_t *first = NULL;
    assert(hk_plan_add_hook(plan, &first_spec, &first) == HK_STATUS_OK);

    const hk_hook_t *deps[1];
    deps[0] = first;
    hk_hook_spec_t second_spec = base_spec("hook.second", HK_TARGET_FUNCTION_SYMBOL);
    second_spec.target.symbol.name = "b";
    second_spec.commit_after = deps;
    second_spec.commit_after_count = 1;

    hk_hook_t *second = NULL;
    assert(hk_plan_add_hook(plan, &second_spec, &second) == HK_STATUS_OK);

    deps[0] = NULL;  // mutate caller's array after the call

    assert(second->spec.commit_after != deps);  // own storage, not aliasing the caller's array
    assert(second->spec.commit_after[0] == first);
    assert(second->spec.commit_after_count == 1);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  commit-after-deep-copied: PASS\n");
}

static void test_hook_pointers_stable_across_growth(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);

    enum { N = 37 };
    hk_hook_t *hooks[N];
    char ids[N][16];
    char names[N][16];

    for (int i = 0; i < N; i++) {
        snprintf(ids[i], sizeof(ids[i]), "hook.%d", i);
        snprintf(names[i], sizeof(names[i]), "sym_%d", i);
        hk_hook_spec_t spec = base_spec(ids[i], HK_TARGET_FUNCTION_SYMBOL);
        spec.target.symbol.name = names[i];
        assert(hk_plan_add_hook(plan, &spec, &hooks[i]) == HK_STATUS_OK);
    }

    for (int i = 0; i < N; i++) {
        char expected_id[16], expected_name[16];
        snprintf(expected_id, sizeof(expected_id), "hook.%d", i);
        snprintf(expected_name, sizeof(expected_name), "sym_%d", i);
        assert(strcmp(hooks[i]->stable_hook_id_owned, expected_id) == 0);
        assert(strcmp(hooks[i]->spec.target.symbol.name, expected_name) == 0);
    }

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  hook-pointers-stable-across-growth: PASS\n");
}

static void test_wrong_state_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    make_plan(&rt, &plan);
    plan->state = HK_PLAN_PREPARED;

    hk_hook_spec_t spec = base_spec("hook.late", HK_TARGET_FUNCTION_SYMBOL);
    spec.target.symbol.name = "a";
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_INVALID_STATE);
    assert(hook == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  wrong-state-rejected: PASS\n");
}

int main(void) {
    test_symbol_target_deep_copied();
    test_objc_target_deep_copied();
    test_memory_target_bytes_deep_copied();
    test_memory_target_non_relative_base_image_zeroed();
    test_swift_target_kind_rejected();
    test_explicit_set_image_selector_rejected();
    test_duplicate_hook_id_rejected();
    test_contradictory_original_continuation_rejected();
    test_foreign_domain_rejected();
    test_same_plan_domain_accepted();
    test_commit_after_forward_reference_rejected();
    test_commit_after_deep_copied();
    test_hook_pointers_stable_across_growth();
    test_wrong_state_rejected();
    printf("all hook add tests passed\n");
    return 0;
}
