// End-to-end: the plan lifecycle driving the real terminal inline engine
// through its runtime adapter (HKInlineVtable.h). Fourth engine wired in and
// the first to reach HK_TARGET_FUNCTION_ADDRESS, so this is what exercises
// the plan's address-target path -- routing on target kind,
// prepare-mutates-nothing, commit, and the resulting text-patch artifact in
// the report.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "../../Sources/Engines/HKInlineVtable.h"

#define A64_NOP 0xD503201Fu
#define A64_RET 0xD65F03C0u

static bool buffer_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    (void)ctx;
    memcpy((void *)address, data, size);
    return true;
}

static uint32_t *make_fn(const uint32_t *insns, size_t count) {
    uint32_t *p = aligned_alloc(16, ((count * 4) + 15) & ~(size_t)15);
    assert(p);
    memcpy(p, insns, count * 4);
    return p;
}

static hk_hook_spec_t address_spec(const char *id, uintptr_t target, void *replacement,
                                   hk_original_requirement_t original,
                                   const uint8_t *expected, size_t expected_size) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = target;
    spec.target.address.expected_initial_bytes = expected;
    spec.target.address.expected_initial_bytes_size = expected_size;
    spec.replacement = replacement;
    spec.required_reach = HK_REACH_ENTRYPOINT;
    spec.original_requirement = original;
    // A terminal hook forbids a continuation by construction; saying so in the
    // request keeps the spec self-consistent with what the engine will serve.
    spec.continuation_policy = HK_CONTINUATION_FORBIDDEN;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static hk_inline_engine_ctx_t engine_ctx(void) {
    hk_inline_engine_ctx_t c;
    c.write = buffer_write;
    c.write_ctx = NULL;
    return c;
}

static void test_full_lifecycle_patches_and_reports(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;
    void *replacement = (void *)(target + 0x1000);  // near: a 4-byte B reaches

    hk_inline_engine_ctx_t ectx = engine_ctx();
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("hook.fn", target, replacement,
                                       HK_ORIGINAL_NONE, NULL, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_inline_vtable());  // routed on the address target kind
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(fn[0] == A64_NOP);  // prepare mutated nothing

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert((fn[0] & 0xFC000000u) == 0x14000000u);  // a B was written
    assert(fn[1] == A64_NOP && fn[4] == A64_RET);  // and nothing else moved

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH);
    assert(a.effects == HK_EFFECT_TARGET_TEXT_MUTATION);
    assert(a.address == target && a.size == 4);
    assert(memcmp(a.original_bytes.inline_bytes.data, body, 4) == 0);
    assert(a.mechanically_reversible);
    assert(a.request_id.high == hook->hook_id.high && a.request_id.low == hook->hook_id.low);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn);
    printf("  full-lifecycle-patches-and-reports: PASS\n");
}

// The refusal that defines this engine, surfaced through the whole lifecycle
// rather than just at the engine's own boundary.
static void test_continuation_request_fails_at_prepare(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    hk_inline_engine_ctx_t ectx = engine_ctx();
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // Asking for a callable original. Terminal inline destroys the prologue,
    // so it must refuse rather than allocate a trampoline behind the caller's
    // back. (continuation_policy stays ANY here: the point is that the ENGINE
    // refuses on the requirement, not that the plan pre-rejects the pair.)
    hk_hook_spec_t spec = address_spec("hook.wants.original", target, (void *)(target + 0x1000),
                                       HK_ORIGINAL_CALLABLE_CONTINUATION, NULL, 0);
    spec.continuation_policy = HK_CONTINUATION_ANY;
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_inline_vtable());       // routed on capability...
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);   // ...then refused
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);
    assert(fn[0] == A64_NOP);  // untouched

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn);
    printf("  continuation-request-fails-at-prepare: PASS\n");
}

static void test_pinned_prologue_mismatch_fails_at_prepare(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;
    const uint32_t not_what_is_there = A64_RET;

    hk_inline_engine_ctx_t ectx = engine_ctx();
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("hook.pinned", target, (void *)(target + 0x1000),
                                       HK_ORIGINAL_NONE,
                                       (const uint8_t *)&not_what_is_there, 4);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(fn[0] == A64_NOP);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn);
    printf("  pinned-prologue-mismatch-fails-at-prepare: PASS\n");
}

// Registered without a context, the engine has no way to write and must fail
// cleanly rather than crash -- the router routed on capability alone.
static void test_no_context_fails_cleanly(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), NULL));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("hook.nctx", target, (void *)(target + 0x1000),
                                       HK_ORIGINAL_NONE, NULL, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(fn[0] == A64_NOP);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn);
    printf("  no-context-fails-cleanly: PASS\n");
}

int main(void) {
    test_full_lifecycle_patches_and_reports();
    test_continuation_request_fails_at_prepare();
    test_pinned_prologue_mismatch_fails_at_prepare();
    test_no_context_fails_cleanly();
    printf("all inline wired tests passed\n");
    return 0;
}
