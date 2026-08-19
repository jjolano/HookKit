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

// memset first, deliberately: this struct gained a `catalog` field, and a
// field-by-field initializer silently left it holding stack garbage, which
// segfaulted the moment the adapter started consulting it. Zeroing means a
// future field defaults to "not supplied" instead of to whatever was on the
// stack.
static hk_inline_engine_ctx_t engine_ctx(void) {
    hk_inline_engine_ctx_t c;
    memset(&c, 0, sizeof(c));
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

// Four distinct refusals that used to arrive as one undifferentiated
// "prepare failed". They still share HK_OUTCOME_FAILED_SAFE -- none of them is
// a satisfied request -- but each now says which one it was.
static void test_refusals_carry_distinct_diagnostics(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;
    void *near = (void *)(target + 0x1000);
    const uint32_t pinned_wrong = A64_RET;

    hk_inline_engine_ctx_t ectx = engine_ctx();
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t s_align = address_spec("h.align", target + 1, near, HK_ORIGINAL_NONE, NULL, 0);
    hk_hook_spec_t s_cont  = address_spec("h.cont", target, near,
                                          HK_ORIGINAL_CALLABLE_CONTINUATION, NULL, 0);
    s_cont.continuation_policy = HK_CONTINUATION_ANY;
    hk_hook_spec_t s_pin   = address_spec("h.pin", target, near, HK_ORIGINAL_NONE,
                                          (const uint8_t *)&pinned_wrong, 4);
    // A one-instruction function patched with a FAR branch: the 16-byte window
    // runs past its RET.
    const uint32_t tiny[] = {A64_RET, A64_NOP, A64_NOP, A64_NOP};
    uint32_t *tinyfn = make_fn(tiny, 4);
    hk_hook_spec_t s_short = address_spec("h.short", (uintptr_t)tinyfn,
                                          (void *)((uintptr_t)tinyfn + (1ull << 40)),
                                          HK_ORIGINAL_NONE, NULL, 0);

    hk_hook_t *ha = NULL, *hc = NULL, *hp = NULL, *hs = NULL;
    assert(hk_plan_add_hook(plan, &s_align, &ha) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &s_cont, &hc) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &s_pin, &hp) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &s_short, &hs) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    // All four are genuine failures, not satisfied requests.
    assert(ha->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hc->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hp->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hs->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    // ...and each carries its own reason.
    assert(ha->result.error_code == (int64_t)HK_INLINE_MISALIGNED);
    assert(hc->result.error_code == (int64_t)HK_INLINE_NEEDS_CONTINUATION);
    assert(hp->result.error_code == (int64_t)HK_INLINE_PRECONDITION_FAILED);
    assert(hs->result.error_code == (int64_t)HK_INLINE_TARGET_TOO_SHORT);
    assert(ha->result.error_message.data && hc->result.error_message.data);
    assert(hp->result.error_message.data && hs->result.error_message.data);
    assert(strcmp(hc->result.error_message.data,
                  "terminal inline cannot provide an original; use a relocating engine") == 0);
    // The domain is this engine, filled by the core from describe().
    assert(hs->result.error_domain.data &&
           strcmp(hs->result.error_domain.data, "inline-terminal") == 0);

    // Nothing was written by any of them.
    assert(fn[0] == A64_NOP && tinyfn[0] == A64_RET);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn); free(tinyfn);
    printf("  refusals-carry-distinct-diagnostics: PASS\n");
}

// The recorded expected_image gap, closed and exercised through the whole
// lifecycle. A catalog in the context makes the target's expected_image /
// expected_uuid enforceable; without one the check is a documented skip.
static void test_expected_image_is_enforced_when_a_catalog_is_supplied(void) {
    // A synthetic image whose __TEXT covers the function we will hook, so the
    // catalog's span genuinely contains the target address.
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    uint8_t hdr[0x200];
    memset(hdr, 0, sizeof(hdr));
    // MH_MAGIC_64, ncmds 1, sizeofcmds 72, one LC_SEGMENT_64 __TEXT spanning
    // a page around the function.
    const uint64_t seg_base = (uint64_t)(target & ~(uintptr_t)0xFFF);
    memcpy(hdr + 0, (uint32_t[]){0xFEEDFACFu}, 4);
    memcpy(hdr + 16, (uint32_t[]){1u}, 4);
    memcpy(hdr + 20, (uint32_t[]){72u}, 4);
    memcpy(hdr + 32, (uint32_t[]){0x19u}, 4);
    memcpy(hdr + 36, (uint32_t[]){72u}, 4);
    memcpy(hdr + 40, "__TEXT", 6);
    memcpy(hdr + 56, &seg_base, 8);
    memcpy(hdr + 64, (uint64_t[]){0x2000ull}, 8);

    const uint8_t uuid[16] = {0xAA, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    hk_image_catalog_t *cat = hk_image_catalog_create();
    hk_image_entry_t e;
    memset(&e, 0, sizeof(e));
    e.path = "/usr/lib/libtarget.dylib";
    e.header = hdr;
    e.slide = 0;
    e.uuid_present = true;
    memcpy(e.uuid, uuid, 16);
    assert(hk_image_catalog_add_entry(cat, &e));

    hk_inline_engine_ctx_t ectx = engine_ctx();
    ectx.catalog = cat;

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // Names the right image: prepares and commits.
    hk_hook_spec_t good = address_spec("h.right.image", target, (void *)(target + 0x1000),
                                       HK_ORIGINAL_NONE, NULL, 0);
    good.target.address.expected_image.struct_size = sizeof(good.target.address.expected_image);
    good.target.address.expected_image.struct_version = HK_ABI_VERSION_3_0;
    good.target.address.expected_image.kind = HK_IMAGE_EXACT_PATH;
    good.target.address.expected_image.path = "/usr/lib/libtarget.dylib";
    good.target.address.expected_uuid_present = true;
    memcpy(good.target.address.expected_uuid, uuid, 16);

    // Names an image that is not loaded: refused before anything is read.
    hk_hook_spec_t wrong = good;
    wrong.stable_hook_id = "h.wrong.image";
    wrong.target.address.expected_image.path = "/usr/lib/libnotloaded.dylib";

    hk_hook_t *hg = NULL, *hw = NULL;
    assert(hk_plan_add_hook(plan, &good, &hg) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &wrong, &hw) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hg->result.outcome == HK_OUTCOME_PREPARED);
    assert(hw->result.outcome == HK_OUTCOME_FAILED_SAFE);
    // The refusal says it was the image scope, not an inline-engine refusal --
    // that is what the diag-code offset is for.
    assert(hw->result.error_code ==
           HK_INLINE_DIAG_IMAGE_SCOPE_BASE + (int64_t)HK_IMAGE_SCOPE_NO_MATCH);
    assert(hw->result.error_message.data &&
           strcmp(hw->result.error_message.data,
                  hk_image_scope_describe(HK_IMAGE_SCOPE_NO_MATCH)) == 0);
    assert(fn[0] == A64_NOP);  // nothing written by either yet

    hk_plan_release(plan);
    hk_runtime_release(rt);

    // Same wrong-image request, but with NO catalog: the check is SKIPPED, so
    // it prepares. This is the device path today, and it is the reason the
    // policy is a skip -- failing closed here would fail every inline hook.
    hk_inline_engine_ctx_t noctx = engine_ctx();   // catalog stays NULL
    hk_runtime_t *rt2 = NULL;
    hk_plan_t *p2 = NULL;
    assert(hk_runtime_create(NULL, &rt2) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt2, hk_inline_vtable(), &noctx));
    assert(hk_plan_create(rt2, NULL, &p2) == HK_STATUS_OK);
    hk_hook_t *h2 = NULL;
    assert(hk_plan_add_hook(p2, &wrong, &h2) == HK_STATUS_OK);
    assert(hk_plan_analyze(p2, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(p2, NULL) == HK_STATUS_OK);
    assert(h2->result.outcome == HK_OUTCOME_PREPARED);

    hk_plan_release(p2);
    hk_runtime_release(rt2);
    hk_image_catalog_destroy(cat);
    free(fn);
    printf("  expected-image-is-enforced-when-a-catalog-is-supplied: PASS\n");
}

int main(void) {
    test_full_lifecycle_patches_and_reports();
    test_continuation_request_fails_at_prepare();
    test_pinned_prologue_mismatch_fails_at_prepare();
    test_no_context_fails_cleanly();
    test_refusals_carry_distinct_diagnostics();
    test_expected_image_is_enforced_when_a_catalog_is_supplied();
    printf("all inline wired tests passed\n");
    return 0;
}
