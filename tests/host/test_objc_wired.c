// End-to-end: the plan lifecycle driving the real ObjC method engine through
// its runtime adapter (HKObjCVtable.h). This is the third engine wired in and
// the first to reach HK_TARGET_OBJC_METHOD, so it is what exercises the plan's
// ObjC-target path -- routing on the target kind, prepare-mutates-nothing,
// commit, and the resulting artifact in the report.
//
// The ObjC runtime is the same in-memory fake the engine suite uses (see
// fake_objc_runtime.h); what is new here is that nothing calls the engine
// directly -- analyze/prepare/commit do.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_objc_runtime.h"
#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/engines/HKObjCVtable.h"

static hk_hook_spec_t objc_spec(const char *id, void *cls, const char *sel_name,
                                hk_objc_method_kind_t kind,
                                hk_objc_inheritance_policy_t policy,
                                void *replacement) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_OBJC_METHOD;
    spec.target.objc.struct_size = sizeof(spec.target.objc);
    spec.target.objc.struct_version = HK_ABI_VERSION_3_0;
    spec.target.objc.cls = cls;
    spec.target.objc.selector_name = sel_name;
    spec.target.objc.method_kind = kind;
    spec.target.objc.inheritance_policy = policy;
    spec.target.objc.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.replacement = replacement;
    spec.required_reach = HK_REACH_OBJC_DISPATCH;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

// The engine context is an ordinary caller-owned struct now -- no file-scoped
// environment, so each test owns its own and they cannot collide.
static hk_objc_engine_ctx_t engine_ctx_for(fake_rt_t *ctx) {
    hk_objc_engine_ctx_t e;
    e.runtime = make_runtime(ctx);
    return e;
}

static void test_local_method_full_lifecycle(void) {
    world_t w; world_init(&w);

    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.bar", &w.child, "bar",
                                    HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_objc_vtable());  // routed on the ObjC target kind
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hook->result.continuation.kind ==
           HK_CONTINUATION_KIND_DIRECT_PREDECESSOR);
    assert(hook->result.continuation.address == (uintptr_t)imp_child_bar);
    // Prepare mutated nothing: the method still holds its original IMP and the
    // runtime saw no replace at all.
    assert(imp_on(&w.child, "bar") == (void *)imp_child_bar);
    assert(w.rt.replace_calls == 0);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(imp_on(&w.child, "bar") == (void *)imp_replacement);
    assert(w.rt.replace_calls == 1);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_OBJC_METHOD_CHANGE);
    assert(a.effects == HK_EFFECT_OBJC_METADATA_MUTATION);
    // The original reaches the report through the artifact, which is the only
    // channel commit_one has.
    assert(a.original_pointer == (void *)imp_child_bar);
    assert(a.replacement_pointer == (void *)imp_replacement);
    assert(a.mechanically_reversible);
    assert(a.request_id.high == hook->hook_id.high && a.request_id.low == hook->hook_id.low);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  local-method-full-lifecycle: PASS\n");
}

static void test_class_method_through_the_lifecycle(void) {
    world_t w; world_init(&w);

    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // +ping lives on the metaclass only; the adapter must carry the method
    // kind through for this to resolve at all.
    hk_hook_spec_t spec = objc_spec("hook.ping", &w.base, "ping",
                                    HK_OBJC_CLASS_METHOD,
                                    HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(imp_on(&w.base_meta, "ping") == (void *)imp_replacement);
    assert(imp_count_on(&w.base, "ping") == 0);  // the class side is untouched

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  class-method-through-the-lifecycle: PASS\n");
}

static void test_inherited_override_reports_irreversible(void) {
    world_t w; world_init(&w);

    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // -foo is inherited from Base; hooking it on Child ADDS an override, which
    // is not cleanly reversible -- and that has to survive all the way into
    // the report, not just the engine's return value.
    hk_hook_spec_t spec = objc_spec("hook.foo", &w.child, "foo",
                                    HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.original_pointer == (void *)imp_base_foo);
    assert(!a.mechanically_reversible);
    assert(!a.safe_to_reverse_after_activation);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  inherited-override-reports-irreversible: PASS\n");
}

static void test_unresolvable_target_fails_at_prepare(void) {
    world_t w; world_init(&w);

    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // -foo is inherited, and the request insists on a local method: the engine
    // refuses, and the refusal must surface as a clean prepare failure with
    // nothing touched.
    hk_hook_spec_t spec = objc_spec("hook.strict", &w.child, "foo",
                                    HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_objc_vtable());   // routed...
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);  // ...then refused
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);
    assert(imp_count_on(&w.child, "foo") == 0);
    assert(w.rt.replace_calls == 0);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  unresolvable-target-fails-at-prepare: PASS\n");
}

// ---- the context/prepared-state mechanism itself -----------------------

// The property the file-scoped environment made impossible, and the reason
// the vtable grew context-carrying entry points: two runtimes driving the
// SAME engine against DIFFERENT ObjC runtimes at the same time. Under the old
// adapter the second set_environment call would have clobbered the first, and
// both plans would have patched the same world.
static void test_two_runtimes_with_different_contexts(void) {
    world_t w1; world_init(&w1);
    world_t w2; world_init(&w2);
    hk_objc_engine_ctx_t ectx1 = engine_ctx_for(&w1.rt);
    hk_objc_engine_ctx_t ectx2 = engine_ctx_for(&w2.rt);

    hk_runtime_t *rt1 = NULL; hk_runtime_t *rt2 = NULL;
    hk_plan_t *p1 = NULL; hk_plan_t *p2 = NULL;
    assert(hk_runtime_create(NULL, &rt1) == HK_STATUS_OK);
    assert(hk_runtime_create(NULL, &rt2) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt1, hk_objc_vtable(), &ectx1));
    assert(hk_runtime_register_engine_with_context(rt2, hk_objc_vtable(), &ectx2));
    assert(hk_plan_create(rt1, NULL, &p1) == HK_STATUS_OK);
    assert(hk_plan_create(rt2, NULL, &p2) == HK_STATUS_OK);

    // Same hook id and same selector in both, deliberately: if any state were
    // shared -- an environment or a stash keyed by that id -- these would
    // collide.
    hk_hook_spec_t s1 = objc_spec("same.id", &w1.child, "bar",
                                  HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                  (void *)imp_replacement);
    hk_hook_spec_t s2 = objc_spec("same.id", &w2.child, "bar",
                                  HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                  (void *)imp_someone_else);
    hk_hook_t *h1 = NULL; hk_hook_t *h2 = NULL;
    assert(hk_plan_add_hook(p1, &s1, &h1) == HK_STATUS_OK);
    assert(hk_plan_add_hook(p2, &s2, &h2) == HK_STATUS_OK);

    // Interleaved on purpose: both prepared before either commits, so any
    // single-slot shared state would have been overwritten by the time the
    // first one commits.
    assert(hk_plan_analyze(p1, NULL) == HK_STATUS_OK);
    assert(hk_plan_analyze(p2, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(p1, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(p2, NULL) == HK_STATUS_OK);
    assert(h1->result.outcome == HK_OUTCOME_PREPARED);
    assert(h2->result.outcome == HK_OUTCOME_PREPARED);
    // Each hook carries its own prepared state, and they are distinct objects.
    assert(h1->prepared_state && h2->prepared_state);
    assert(h1->prepared_state != h2->prepared_state);

    hk_report_t *r1 = NULL; hk_report_t *r2 = NULL;
    assert(hk_plan_commit(p1, &r1) == HK_STATUS_OK);
    assert(hk_plan_commit(p2, &r2) == HK_STATUS_OK);

    // Each patched its OWN world with its OWN replacement.
    assert(imp_on(&w1.child, "bar") == (void *)imp_replacement);
    assert(imp_on(&w2.child, "bar") == (void *)imp_someone_else);
    assert(w1.rt.replace_calls == 1 && w2.rt.replace_calls == 1);

    hk_report_release(r1); hk_report_release(r2);
    hk_plan_release(p1); hk_plan_release(p2);
    hk_runtime_release(rt1); hk_runtime_release(rt2);
    printf("  two-runtimes-with-different-contexts: PASS\n");
}

// Prepared state must be released even when commit never runs -- otherwise a
// plan that is analyzed, prepared, and then simply dropped leaks one
// allocation per hook. Under ASan+UBSan (how this suite is also run) a missing
// release is a reported leak, which is what gives this test teeth.
static void test_prepared_state_released_without_commit(void) {
    world_t w; world_init(&w);
    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.dropped", &w.child, "bar",
                                    HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->prepared_state != NULL);

    // Dropped without committing. The method is untouched and the prepared
    // allocation is the plan's to release.
    hk_plan_release(plan);
    assert(imp_on(&w.child, "bar") == (void *)imp_child_bar);
    assert(w.rt.replace_calls == 0);

    hk_runtime_release(rt);
    printf("  prepared-state-released-without-commit: PASS\n");
}

// A second prepare is refused by the state machine rather than producing a
// second prepared state -- which is WHY the release-before-assign guard in
// hk_plan_prepare is unreachable today, and is worth pinning down here so
// that if the lifecycle ever gains a retry path, this test is the thing that
// notices the guard has become live.
static void test_second_prepare_is_refused_by_state(void) {
    world_t w; world_init(&w);
    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.twice", &w.child, "bar",
                                    HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    void *first = hook->prepared_state;
    assert(first != NULL);

    // The lifecycle is one-way: analyze needs DRAFT, prepare needs ANALYZED.
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_INVALID_STATE);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_INVALID_STATE);
    // The refusal changed nothing: same prepared state, nothing written.
    assert(hook->prepared_state == first);
    assert(w.rt.replace_calls == 0);

    // And that state is still the one commit uses.
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(w.rt.replace_calls == 1);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  second-prepare-is-refused-by-state: PASS\n");
}

// A deliberately misbehaving engine: it writes *out_prepared and THEN returns
// false, which the prepare_one_ctx contract forbids ("returning false must
// leave *out_prepared untouched"). Storing that pointer anyway would hand a
// failed preparation's state to release_prepared at plan release -- here, a
// double free of memory the engine already released.
//
// This exists because a teeth-proof found the gap: swapping the core's
// `ok ? prepared : NULL` for a plain `prepared` changed NOTHING against
// well-behaved engines, since prepared starts NULL and a conforming engine
// never writes it on failure. The guard is only load-bearing against an
// engine like this one, so this is the test that makes it so.
static bool g_liar_released;
static void *g_liar_freed_ptr;

static hk_engine_capabilities_t liar_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "liar";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    caps.achievable_reach = HK_REACH_OBJC_DISPATCH;
    return caps;
}
static bool liar_prepare_one_ctx(void *engine_ctx, const hk_hook_spec_t *spec,
                                 void **out_prepared) {
    (void)engine_ctx; (void)spec;
    void *p = malloc(16);
    *out_prepared = p;   // contract violation: written on the failure path
    free(p);             // ...and already released, so retaining it is a UAF
    g_liar_freed_ptr = p;
    return false;
}
static void liar_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx; (void)prepared;
    g_liar_released = true;   // must never happen for a failed preparation
}
static const hk_engine_vtable_t g_liar_engine = {
    .describe = liar_describe,
    .prepare_one_ctx = liar_prepare_one_ctx,
    .release_prepared = liar_release_prepared,
};

static void test_failed_preparation_retains_no_state(void) {
    world_t w; world_init(&w);
    g_liar_released = false;
    g_liar_freed_ptr = NULL;

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, &g_liar_engine, NULL));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.liar", &w.child, "bar",
                                    HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(g_liar_freed_ptr != NULL);     // the engine really did write one
    assert(hook->prepared_state == NULL); // ...and the core kept none of it

    // Releasing the plan must not hand that pointer back to the engine.
    hk_plan_release(plan);
    assert(!g_liar_released);

    hk_runtime_release(rt);
    printf("  failed-preparation-retains-no-state: PASS\n");
}

// The bug the richer prepare result exists to fix. A conditional request
// (OPTIONAL_IF_PRESENT) whose target is absent is SATISFIED -- it must not
// fail the plan. Under the bool contract this hook's `false` counted toward
// `failed`, and any failure puts the plan in HK_PLAN_FAILED, so one absent
// optional target failed a plan that did exactly what it was asked.
static void test_absent_optional_target_does_not_fail_the_plan(void) {
    world_t w; world_init(&w);
    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // A real hook that will prepare, plus a conditional one whose selector
    // does not exist. Both in one plan, so the second cannot be dismissed as
    // "the plan had nothing in it".
    hk_hook_spec_t good = objc_spec("hook.real", &w.child, "bar",
                                    HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    hk_hook_spec_t absent = objc_spec("hook.absent", &w.child, "nosuchselector",
                                      HK_OBJC_INSTANCE_METHOD, HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                      (void *)imp_replacement);
    absent.target.objc.availability = HK_AVAILABILITY_OPTIONAL_IF_PRESENT;

    hk_hook_t *hg = NULL; hk_hook_t *ha = NULL;
    assert(hk_plan_add_hook(plan, &good, &hg) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &absent, &ha) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    // The absent one is not a failure...
    assert(ha->result.outcome != HK_OUTCOME_FAILED_SAFE);
    assert(ha->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(ha->result.retryable);          // the class could gain it later
    assert(!ha->result.currently_valid);
    assert(ha->prepared_state == NULL);    // nothing to commit
    // ...so the plan is still preparable, and the real hook still commits.
    assert(hk_plan_state(plan) == HK_PLAN_PREPARED);
    assert(hg->result.outcome == HK_OUTCOME_PREPARED);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);
    assert(hg->result.outcome == HK_OUTCOME_ACTIVE);
    assert(imp_on(&w.child, "bar") == (void *)imp_replacement);
    // Exactly one artifact: the absent hook produced none.
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  absent-optional-target-does-not-fail-the-plan: PASS\n");
}

// A REQUIRED_NOW target that is absent is still a failure -- the difference is
// the request's availability, not the engine being lenient about missing
// things.
static void test_absent_required_target_still_fails(void) {
    world_t w; world_init(&w);
    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.required", &w.child, "nosuchselector",
                                    HK_OBJC_INSTANCE_METHOD, HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                    (void *)imp_replacement);
    // availability stays REQUIRED_NOW
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  absent-required-target-still-fails: PASS\n");
}

static void test_deferred_target_survives_plan_and_retries(void) {
    world_t w; world_init(&w);
    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = objc_spec("hook.deferred", &w.child, "late",
                                    HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    (void *)imp_replacement);
    // The target-specific field is authoritative; leave the mirrored
    // spec-level field at its helper default to catch routing the decision
    // through the wrong copy.
    spec.target.objc.availability = HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PENDING);
    assert(hk_plan_state(plan) == HK_PLAN_PREPARED);
    hk_id_t plan_id = hook->result.plan_id;
    hk_id_t request_id = hook->hook_id;

    hk_plan_release(plan);
    assert(rt->pending_count == 1);

    hk_report_t *pending_report = NULL;
    assert(hk_runtime_drain_pending(rt, &pending_report) == HK_STATUS_OK);
    assert(pending_report != NULL);
    assert(pending_report->result_count == 1);
    assert(pending_report->results[0].outcome == HK_OUTCOME_PENDING);
    assert(rt->pending_count == 1);
    assert(w.rt.replace_calls == 0);
    hk_report_release(pending_report);

    assert(w.child.count < 8);
    w.child.methods[w.child.count].sel = (const char *)intern_sel("late");
    w.child.methods[w.child.count].imp = (void *)imp_child_bar;
    w.child.methods[w.child.count].types = "v@:";
    w.child.count++;

    hk_report_t *active_report = NULL;
    assert(hk_runtime_drain_pending(rt, &active_report) == HK_STATUS_OK);
    assert(active_report != NULL);
    assert(active_report->result_count == 1);
    assert(active_report->results[0].outcome == HK_OUTCOME_ACTIVE);
    assert(active_report->results[0].mutation == HK_MUTATION_COMPLETE);
    assert(rt->pending_count == 0);
    assert(imp_on(&w.child, "late") == (void *)imp_replacement);
    assert(w.rt.replace_calls == 1);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(active_report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);
    hk_artifact_t artifact;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &artifact) == HK_STATUS_OK);
    assert(artifact.plan_id.high == plan_id.high && artifact.plan_id.low == plan_id.low);
    assert(artifact.request_id.high == request_id.high && artifact.request_id.low == request_id.low);

    hk_artifact_snapshot_release(snap);
    hk_report_release(active_report);
    hk_runtime_release(rt);
    printf("  deferred-target-survives-plan-and-retries: PASS\n");
}

// Distinct refusals now reach the result with distinct reasons, instead of
// every one reading as an undifferentiated "prepare failed".
static void test_refusals_carry_distinct_diagnostics(void) {
    world_t w; world_init(&w);
    hk_objc_engine_ctx_t ectx = engine_ctx_for(&w.rt);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_objc_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // -foo is inherited; asking for a local method only.
    hk_hook_spec_t inherited = objc_spec("h.inherited", &w.child, "foo",
                                         HK_OBJC_INSTANCE_METHOD, HK_OBJC_LOCAL_METHOD_ONLY,
                                         (void *)imp_replacement);
    // A selector that does not exist at all.
    hk_hook_spec_t missing = objc_spec("h.missing", &w.child, "nosuchselector",
                                       HK_OBJC_INSTANCE_METHOD, HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                       (void *)imp_replacement);
    hk_hook_t *hi = NULL; hk_hook_t *hm = NULL;
    assert(hk_plan_add_hook(plan, &inherited, &hi) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &missing, &hm) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hi->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hm->result.outcome == HK_OUTCOME_FAILED_SAFE);
    // Same outcome, different reasons -- which is the whole point.
    assert(hi->result.error_code == (int64_t)HK_OBJC_INHERITED_REFUSED);
    assert(hm->result.error_code == (int64_t)HK_OBJC_METHOD_NOT_FOUND);
    assert(hi->result.error_code != hm->result.error_code);
    assert(hi->result.error_message.data && hm->result.error_message.data);
    assert(hi->result.error_message.length > 0);
    assert(strcmp(hi->result.error_message.data,
                  "method is inherited and the request required a local method") == 0);
    // The domain is the engine that produced the code, filled by the core.
    assert(hi->result.error_domain.data && strcmp(hi->result.error_domain.data, "objc") == 0);
    assert(hi->result.error_domain.length == 4);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  refusals-carry-distinct-diagnostics: PASS\n");
}

int main(void) {
    #define RUN_TEST(test) do { hk_ownership_reset_for_testing(); test(); } while (0)
    RUN_TEST(test_local_method_full_lifecycle);
    RUN_TEST(test_class_method_through_the_lifecycle);
    RUN_TEST(test_inherited_override_reports_irreversible);
    RUN_TEST(test_unresolvable_target_fails_at_prepare);
    RUN_TEST(test_two_runtimes_with_different_contexts);
    RUN_TEST(test_prepared_state_released_without_commit);
    RUN_TEST(test_second_prepare_is_refused_by_state);
    RUN_TEST(test_failed_preparation_retains_no_state);
    RUN_TEST(test_absent_optional_target_does_not_fail_the_plan);
    RUN_TEST(test_absent_required_target_still_fails);
    RUN_TEST(test_deferred_target_survives_plan_and_retries);
    RUN_TEST(test_refusals_carry_distinct_diagnostics);
    #undef RUN_TEST
    hk_ownership_reset_for_testing();
    printf("all objc wired tests passed\n");
    return 0;
}
