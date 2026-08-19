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
#include <string.h>

#include "fake_objc_runtime.h"
#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "../../Sources/Engines/HKObjCVtable.h"

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

static void set_env(fake_rt_t *ctx) {
    hk_objc_binding_env_t env;
    env.runtime = make_runtime(ctx);
    hk_objc_vtable_set_environment_for_testing(&env);
}

static void test_local_method_full_lifecycle(void) {
    world_t w; world_init(&w);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_objc_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    set_env(&w.rt);

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
    hk_objc_vtable_reset_for_testing();
    printf("  local-method-full-lifecycle: PASS\n");
}

static void test_class_method_through_the_lifecycle(void) {
    world_t w; world_init(&w);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_objc_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    set_env(&w.rt);

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
    hk_objc_vtable_reset_for_testing();
    printf("  class-method-through-the-lifecycle: PASS\n");
}

static void test_inherited_override_reports_irreversible(void) {
    world_t w; world_init(&w);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_objc_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    set_env(&w.rt);

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
    hk_objc_vtable_reset_for_testing();
    printf("  inherited-override-reports-irreversible: PASS\n");
}

static void test_unresolvable_target_fails_at_prepare(void) {
    world_t w; world_init(&w);

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, hk_objc_vtable()));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    set_env(&w.rt);

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
    hk_objc_vtable_reset_for_testing();
    printf("  unresolvable-target-fails-at-prepare: PASS\n");
}

int main(void) {
    test_local_method_full_lifecycle();
    test_class_method_through_the_lifecycle();
    test_inherited_override_reports_irreversible();
    test_unresolvable_target_fails_at_prepare();
    printf("all objc wired tests passed\n");
    return 0;
}
