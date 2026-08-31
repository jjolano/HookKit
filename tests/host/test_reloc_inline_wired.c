// End-to-end: the plan lifecycle driving the real relocating inline engine
// through its adapter, and -- the point of this suite -- driving it ALONGSIDE
// the terminal engine, which describes itself identically except for which
// originals it serves.
//
// That coexistence is what the routing work exists for. Before
// hk_engine_capabilities_t carried original_requirements, registering both
// meant the first one won every request and the other's capability was
// unreachable. These tests pin the behaviour that replaced it.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/engines/HKInlineVtable.h"
#include "../../src/engines/HKRelocInlineVtable.h"

#define A64_NOP 0xD503201Fu
#define A64_RET 0xD65F03C0u

typedef struct {
    uint8_t *page;
    unsigned alloc_calls;
    unsigned seal_calls;
    unsigned write_calls;
    unsigned free_calls;
} seam_t;

static uintptr_t seam_alloc(void *ctx, size_t size, uintptr_t near) {
    seam_t *s = ctx; s->alloc_calls++; (void)near;
    s->page = aligned_alloc(16, (size + 15) & ~(size_t)15);
    assert(s->page); memset(s->page, 0, size);
    return (uintptr_t)s->page;
}
static bool seam_seal(void *ctx, uintptr_t page, size_t size) {
    seam_t *s = ctx; s->seal_calls++; (void)page; (void)size; return true;
}
static bool seam_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    seam_t *s = ctx; s->write_calls++;
    memcpy((void *)address, data, size);
    return true;
}

static void seam_free_page(void *ctx, uintptr_t page, size_t size) {
    seam_t *s = ctx; (void)size;
    s->free_calls++;
    free((void *)page);
    if ((uintptr_t)s->page == page) s->page = NULL;
}

static uint32_t *make_fn(const uint32_t *insns, size_t count) {
    uint32_t *p = aligned_alloc(16, ((count * 4) + 15) & ~(size_t)15);
    assert(p); memcpy(p, insns, count * 4);
    return p;
}

// memset first: both ctx structs gained fields over their lifetimes, and a
// field-by-field initializer leaves a new one holding stack garbage.
static hk_reloc_engine_ctx_t reloc_ctx(seam_t *s) {
    hk_reloc_engine_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.alloc = seam_alloc; c.seal = seam_seal; c.free_page = seam_free_page; c.seam_ctx = s;
    c.write = seam_write; c.write_ctx = s;
    return c;
}
static hk_inline_engine_ctx_t terminal_ctx(seam_t *s) {
    hk_inline_engine_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.write = seam_write; c.write_ctx = s;
    return c;
}

static hk_hook_spec_t address_spec(const char *id, uintptr_t target, void *replacement,
                                   hk_original_requirement_t original) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = target;
    spec.replacement = replacement;
    spec.required_reach = HK_REACH_ENTRYPOINT;
    spec.original_requirement = original;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void test_full_lifecycle_installs_and_reports(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t ectx = reloc_ctx(&s);
    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("h.cont", target, (void *)(target + 0x1000),
                                       HK_ORIGINAL_CALLABLE_CONTINUATION);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_reloc_inline_vtable());
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hook->result.declared_prepare_effects == HK_EFFECT_EXECUTABLE_ALLOCATION);
    assert(hook->result.observed_prepare_effects == HK_EFFECT_EXECUTABLE_ALLOCATION);
    assert(hook->has_prepared_continuation);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_DYNAMIC);
    assert(hook->result.continuation.address == hook->prepared_continuation.address);
    assert(hook->result.continuation.mapping_id.high ==
               hook->prepared_continuation.mapping_id.high &&
           hook->result.continuation.mapping_id.low ==
               hook->prepared_continuation.mapping_id.low);
    // The trampoline exists BEFORE anything branches anywhere -- invariant #5,
    // and the reason the page is built at prepare rather than at commit.
    assert(s.alloc_calls == 1 && s.seal_calls == 1);
    assert(fn[0] == A64_NOP);      // ...and the target is still untouched
    assert(s.write_calls == 0);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_DYNAMIC);
    assert(hook->result.continuation.address != 0);
    assert(hook->result.continuation.mapping_id.high != 0 ||
           hook->result.continuation.mapping_id.low != 0);
    assert(hook->result.continuation.executable_memory_allocated);
    assert(fn[0] != A64_NOP);      // entry patched
    assert(fn[4] == A64_RET);      // nothing past the window moved

    // Two artifacts: the page, then the text patch, in that order.
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 2);
    hk_artifact_t t, a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &t) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(snap, 1, &a) == HK_STATUS_OK);
    assert(t.kind == HK_ARTIFACT_TRAMPOLINE && !t.mechanically_reversible);
    assert(t.mapping.mapping_id.high == hook->result.continuation.mapping_id.high &&
           t.mapping.mapping_id.low == hook->result.continuation.mapping_id.low);
    assert(a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH && a.mechanically_reversible);
    assert(a.request_id.high == hook->hook_id.high && a.request_id.low == hook->hook_id.low);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(s.page); free(fn);
    printf("  full-lifecycle-installs-and-reports: PASS\n");
}

// The payoff of the routing work: both engines registered, and each request
// reaches the one that can actually serve it.
static void test_both_inline_engines_coexist(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn_none = make_fn(a, 5);
    uint32_t *fn_cont = make_fn(a, 5);

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t rctx = reloc_ctx(&s);
    hk_inline_engine_ctx_t tctx = terminal_ctx(&s);

    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // Terminal FIRST, which is the recommended order: it is the cheaper engine
    // for the requests it can serve, and registering it first is what makes it
    // win those.
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &tctx));
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &rctx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t s_none = address_spec("h.none", (uintptr_t)fn_none,
                                         (void *)((uintptr_t)fn_none + 0x1000),
                                         HK_ORIGINAL_NONE);
    hk_hook_spec_t s_cont = address_spec("h.cont", (uintptr_t)fn_cont,
                                         (void *)((uintptr_t)fn_cont + 0x1000),
                                         HK_ORIGINAL_CALLABLE_CONTINUATION);
    hk_hook_t *hn = NULL, *hc = NULL;
    assert(hk_plan_add_hook(plan, &s_none, &hn) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &s_cont, &hc) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);

    // NONE goes to the cheap engine; CALLABLE_CONTINUATION skips it entirely.
    assert(hn->matched_engine == hk_inline_vtable());
    assert(hc->matched_engine == hk_reloc_inline_vtable());

    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    // Exactly ONE page was allocated -- for the continuation hook. The
    // terminal hook allocated nothing, which is the entire point of preferring
    // it when it can serve the request.
    assert(s.alloc_calls == 1);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hn->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hc->result.outcome == HK_OUTCOME_ACTIVE);
    assert(fn_none[0] != A64_NOP && fn_cont[0] != A64_NOP);

    // Three artifacts total: one text patch from terminal, a trampoline plus a
    // text patch from relocating.
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 3);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(s.page); free(fn_none); free(fn_cont);
    printf("  both-inline-engines-coexist: PASS\n");
}

// Registration order is a cost decision, not a correctness one: relocating
// first serves NONE correctly too, just by allocating a page nobody needed.
static void test_registration_order_is_a_cost_decision(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(a, 5);

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t rctx = reloc_ctx(&s);
    hk_inline_engine_ctx_t tctx = terminal_ctx(&s);

    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &rctx));
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &tctx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("h.none", (uintptr_t)fn,
                                       (void *)((uintptr_t)fn + 0x1000), HK_ORIGINAL_NONE);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    // Relocating won on order, and serves it correctly...
    assert(hook->matched_engine == hk_reloc_inline_vtable());
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    // ...at the cost of a page the terminal engine would not have needed.
    assert(s.alloc_calls == 1);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(s.page); free(fn);
    printf("  registration-order-is-a-cost-decision: PASS\n");
}

// A refusal identifies itself as an image-scope one rather than an engine one.
static void test_refusals_are_distinguishable(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(a, 5);
    // A one-instruction RET: the relocating engine refuses it wherever the
    // terminator sits, since the displaced copy would never reach the jump back.
    const uint32_t tiny[] = {A64_RET, A64_NOP, A64_NOP, A64_NOP};
    uint32_t *tinyfn = make_fn(tiny, 4);

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t ectx = reloc_ctx(&s);
    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t s_short = address_spec("h.short", (uintptr_t)tinyfn,
                                          (void *)((uintptr_t)tinyfn + 0x1000),
                                          HK_ORIGINAL_CALLABLE_CONTINUATION);
    hk_hook_spec_t s_align = address_spec("h.align", (uintptr_t)fn + 1,
                                          (void *)((uintptr_t)fn + 0x1000),
                                          HK_ORIGINAL_CALLABLE_CONTINUATION);
    hk_hook_t *hs = NULL, *ha = NULL;
    assert(hk_plan_add_hook(plan, &s_short, &hs) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &s_align, &ha) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hs->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(ha->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hs->result.error_code == (int64_t)HK_RELOC_TARGET_TOO_SHORT);
    assert(ha->result.error_code == (int64_t)HK_RELOC_MISALIGNED);
    // Both below the image-scope band, which is what the offset guarantees.
    assert(hs->result.error_code < HK_RELOC_DIAG_IMAGE_SCOPE_BASE);
    assert(ha->result.error_domain.data &&
           strcmp(ha->result.error_domain.data, "inline-relocating") == 0);
    // Misalignment is caught before a page is requested.
    assert(tinyfn[0] == A64_RET && fn[0] == A64_NOP);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(s.page); free(fn); free(tinyfn);
    printf("  refusals-are-distinguishable: PASS\n");
}

// Image scope is enforced, and enforced BEFORE a page is requested -- a hook
// that was never going to happen must not leak an executable page on the way
// to being refused.
static void test_image_scope_is_checked_before_allocating(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(a, 5);
    uintptr_t target = (uintptr_t)fn;

    // A catalog that knows some other image, so the request's named image is
    // genuinely not loaded.
    uint8_t hdr[0x200];
    memset(hdr, 0, sizeof(hdr));
    const uint64_t seg_base = (uint64_t)(target & ~(uintptr_t)0xFFF);
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
    e.path = "/usr/lib/libreal.dylib";
    e.header = hdr;
    assert(hk_image_catalog_add_entry(cat, &e));

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t ectx = reloc_ctx(&s);
    ectx.catalog = cat;

    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t wrong = address_spec("h.wrongimage", target,
                                        (void *)(target + 0x1000),
                                        HK_ORIGINAL_CALLABLE_CONTINUATION);
    wrong.target.address.expected_image.struct_size = sizeof(wrong.target.address.expected_image);
    wrong.target.address.expected_image.struct_version = HK_ABI_VERSION_3_0;
    wrong.target.address.expected_image.kind = HK_IMAGE_EXACT_PATH;
    wrong.target.address.expected_image.path = "/usr/lib/libnotloaded.dylib";

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &wrong, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    // Identified as an image-scope refusal, not an engine one.
    assert(hook->result.error_code ==
           HK_RELOC_DIAG_IMAGE_SCOPE_BASE + (int64_t)HK_IMAGE_SCOPE_NO_MATCH);
    // And NO page was requested -- the check ran first, which is the property
    // that keeps a doomed hook from leaking one.
    assert(s.alloc_calls == 0 && s.seal_calls == 0);
    assert(fn[0] == A64_NOP);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_image_catalog_destroy(cat);
    free(fn);
    printf("  image-scope-is-checked-before-allocating: PASS\n");
}

// The pinned prologue reaches the engine. Without it the adapter would happily
// patch a function whose body is not what the caller pinned.
static void test_pinned_prologue_reaches_the_engine(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(a, 5);
    uintptr_t target = (uintptr_t)fn;
    const uint32_t not_what_is_there = A64_RET;
    const uint32_t what_is_there = A64_NOP;

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t ectx = reloc_ctx(&s);
    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t bad = address_spec("h.pinned.bad", target, (void *)(target + 0x1000),
                                      HK_ORIGINAL_CALLABLE_CONTINUATION);
    bad.target.address.expected_initial_bytes = (const uint8_t *)&not_what_is_there;
    bad.target.address.expected_initial_bytes_size = 4;

    hk_hook_spec_t good = address_spec("h.pinned.good", target, (void *)(target + 0x1000),
                                       HK_ORIGINAL_CALLABLE_CONTINUATION);
    good.target.address.expected_initial_bytes = (const uint8_t *)&what_is_there;
    good.target.address.expected_initial_bytes_size = 4;

    hk_hook_t *hb = NULL;
    assert(hk_plan_add_hook(plan, &bad, &hb) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hb->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hb->result.error_code == (int64_t)HK_RELOC_PRECONDITION_FAILED);
    assert(fn[0] == A64_NOP);
    hk_plan_release(plan);

    // ...and a matching pin prepares, so the refusal is about the bytes and
    // not about pinning being rejected wholesale.
    hk_plan_t *p2 = NULL;
    assert(hk_plan_create(rt, NULL, &p2) == HK_STATUS_OK);
    hk_hook_t *hg = NULL;
    assert(hk_plan_add_hook(p2, &good, &hg) == HK_STATUS_OK);
    assert(hk_plan_analyze(p2, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(p2, NULL) == HK_STATUS_OK);
    assert(hg->result.outcome == HK_OUTCOME_PREPARED);

    hk_plan_release(p2);
    hk_runtime_release(rt);
    free(s.page); free(fn);
    printf("  pinned-prologue-reaches-the-engine: PASS\n");
}

// A caller forbidding dynamic executable memory now gets routed AWAY from the
// engine that would allocate it, instead of silently getting a page. Before
// this, nothing in src/ read `constraints` at all.
static void test_forbidding_executable_allocation_routes_to_terminal(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(a, 5);

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t rctx = reloc_ctx(&s);
    hk_inline_engine_ctx_t tctx = terminal_ctx(&s);

    // Relocating registered FIRST, so it would win on order alone -- which is
    // what makes this a real test of the constraint rather than of ordering.
    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &rctx));
    assert(hk_runtime_register_engine_with_context(rt, hk_inline_vtable(), &tctx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("h.noexec", (uintptr_t)fn,
                                       (void *)((uintptr_t)fn + 0x1000),
                                       HK_ORIGINAL_NONE);
    spec.constraints = HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    // Skipped the allocating engine, reached the one that allocates nothing.
    assert(hook->matched_engine == hk_inline_vtable());
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(s.alloc_calls == 0);   // and no page was ever requested

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn);
    printf("  forbidding-executable-allocation-routes-to-terminal: PASS\n");
}

// The continuation POLICY says the same thing as the constraint bit, and a
// caller who set only the policy meant it just as much.
static void test_continuation_policy_forbids_allocation_too(void) {
    const uint32_t a[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(a, 5);

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_engine_ctx_t rctx = reloc_ctx(&s);
    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // ONLY the allocating engine is registered, so there is nowhere else to go.
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &rctx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("h.policy", (uintptr_t)fn,
                                       (void *)((uintptr_t)fn + 0x1000),
                                       HK_ORIGINAL_CALLABLE_CONTINUATION);
    spec.constraints = 0;   // nothing forbidden explicitly...
    spec.continuation_policy = HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    // ...but the policy alone is enough. No engine can serve a callable
    // continuation without allocating, so NO_ROUTE is the honest answer --
    // that is exactly the static-continuation mechanism Milestone 9 is about,
    // and it does not exist yet.
    assert(hook->matched_engine == NULL);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(s.alloc_calls == 0);
    assert(fn[0] == A64_NOP);

    // Sanity: the identical request with HK_CONTINUATION_ANY does route, so
    // the refusal is the policy's doing and not a broken spec.
    hk_plan_t *p2 = NULL;
    assert(hk_plan_create(rt, NULL, &p2) == HK_STATUS_OK);
    hk_hook_spec_t ok = spec;
    ok.stable_hook_id = "h.any";
    ok.continuation_policy = HK_CONTINUATION_ANY;
    hk_hook_t *h2 = NULL;
    assert(hk_plan_add_hook(p2, &ok, &h2) == HK_STATUS_OK);
    assert(hk_plan_analyze(p2, NULL) == HK_STATUS_OK);
    assert(h2->matched_engine == hk_reloc_inline_vtable());

    hk_plan_release(p2);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(fn);
    printf("  continuation-policy-forbids-allocation-too: PASS\n");
}

// The two enums do not share a bit layout past bit 3, so a mask-and-compare
// would cross-wire them. This pins the pair that would collide: an engine
// declaring MEMORY_MUTATION (effect bit 4) must NOT be rejected by a caller
// forbidding DYNAMIC_EXECUTABLE_MEMORY (forbid bit 4).
static void test_effect_and_forbid_layouts_are_not_conflated(void) {
    assert(hk_effect_forbid_bit(HK_EFFECT_EXECUTABLE_ALLOCATION) ==
           HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY);
    // Same bit index, different meanings -- and memory mutation has no forbid
    // bit at all, so it maps to 0 rather than to the one sharing its index.
    assert(hk_effect_forbid_bit(HK_EFFECT_MEMORY_MUTATION) == 0);
    assert(hk_effect_forbid_bit(HK_EFFECT_PRIVATE_SYMBOL_SCAN) ==
           HK_FORBID_PRIVATE_SYMBOL_SCAN);

    // An engine declaring only MEMORY_MUTATION stays eligible under that
    // constraint -- the cross-wiring a naive mask would produce.
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_MEMORY_PATCH);
    caps.achievable_reach = HK_REACH_EXACT_MEMORY;
    caps.commit_effects = HK_EFFECT_MEMORY_MUTATION;
    assert(hk_engine_eligible_minimal_full(&caps, HK_TARGET_MEMORY_PATCH,
                                           HK_REACH_EXACT_MEMORY, HK_ORIGINAL_NONE,
                                           HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY));
    // ...while one that really does allocate is not.
    caps.commit_effects = HK_EFFECT_EXECUTABLE_ALLOCATION;
    assert(!hk_engine_eligible_minimal_full(&caps, HK_TARGET_MEMORY_PATCH,
                                            HK_REACH_EXACT_MEMORY, HK_ORIGINAL_NONE,
                                            HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY));
    printf("  effect-and-forbid-layouts-are-not-conflated: PASS\n");
}

int main(void) {
    #define RUN_TEST(test) do { hk_ownership_reset_for_testing(); test(); } while (0)
    RUN_TEST(test_full_lifecycle_installs_and_reports);
    RUN_TEST(test_both_inline_engines_coexist);
    RUN_TEST(test_registration_order_is_a_cost_decision);
    RUN_TEST(test_refusals_are_distinguishable);
    RUN_TEST(test_image_scope_is_checked_before_allocating);
    RUN_TEST(test_pinned_prologue_reaches_the_engine);
    RUN_TEST(test_forbidding_executable_allocation_routes_to_terminal);
    RUN_TEST(test_continuation_policy_forbids_allocation_too);
    RUN_TEST(test_effect_and_forbid_layouts_are_not_conflated);
    #undef RUN_TEST
    hk_ownership_reset_for_testing();
    printf("all relocating inline wired tests passed\n");
    return 0;
}
