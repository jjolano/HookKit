// Host test for src/engines/HKObjCEngine.c -- the engine on its own,
// driven directly rather than through the plan lifecycle.
//
// There is no Objective-C runtime on this host at all, so the engine's runtime
// seam is driven by the in-memory class table in fake_objc_runtime.h (shared
// with test_objc_wired.c); see that header for what it models and why. Every
// decision the engine makes is exercised against it. Real libobjc behavior is
// device-only and is not claimed here.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fake_objc_runtime.h"
#include "../../src/core/HKArtifactLedger.h"

static hk_objc_target_t target_for(void *cls, const char *class_name,
                                   const char *sel_name,
                                   hk_objc_method_kind_t kind,
                                   hk_objc_inheritance_policy_t policy,
                                   hk_availability_t availability) {
    hk_objc_target_t t;
    memset(&t, 0, sizeof(t));
    t.struct_size = sizeof(t);
    t.struct_version = HK_ABI_VERSION_3_0;
    t.cls = cls;
    t.class_name = class_name;
    t.selector_name = sel_name;
    t.method_kind = kind;
    t.inheritance_policy = policy;
    t.availability = availability;
    return t;
}

// ---- tests -------------------------------------------------------------

static void test_local_method_replace_is_authoritative(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    // -bar is LOCAL to Child, so class_replaceMethod replaces in place and its
    // return value is the authoritative original.
    hk_objc_target_t t = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY, HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_OK);
    assert(plan.captured && plan.is_local);
    assert(plan.original_imp == (void *)imp_child_bar);
    // The method had no type encoding; the placeholder stands in for it.
    assert(strcmp(plan.types, "@:@") == 0);
    // Prepare mutated nothing.
    assert(w.rt.replace_calls == 0);
    assert(imp_on(&w.child, "bar") == (void *)imp_child_bar);

    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink; memset(&sink, 0, sizeof(sink)); sink.ledger = ledger;

    void *original = NULL;
    w.rt.watch_cell = &original;
    assert(hk_objc_commit(&rt, &plan, (void *)imp_replacement, &original, &sink)
           == HK_MUTATION_COMPLETE);

    assert(imp_on(&w.child, "bar") == (void *)imp_replacement);
    assert(original == (void *)imp_child_bar);
    // Replaced in place -- no second `bar` was added to Child.
    assert(imp_count_on(&w.child, "bar") == 1);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_OBJC_METHOD_CHANGE);
    assert(a.effects == HK_EFFECT_OBJC_METADATA_MUTATION);
    assert(a.original_pointer == (void *)imp_child_bar);
    assert(a.replacement_pointer == (void *)imp_replacement);
    // A local replace is a clean restore.
    assert(a.mechanically_reversible);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  local-method-replace-is-authoritative: PASS\n");
}

static void test_inherited_override_keeps_preread_original(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    // -foo is on Base, INHERITED by Child. Hooking it on Child ADDS an
    // override: class_replaceMethod replaces nothing and returns NULL, so the
    // IMP read at prepare is the only correct original.
    hk_objc_target_t t = target_for(&w.child, NULL, "foo", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                    HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_OK);
    assert(!plan.is_local);                                 // resolved from an ancestor
    assert(plan.original_imp == (void *)imp_base_foo);
    assert(imp_count_on(&w.child, "foo") == 0);             // nothing added yet

    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink; memset(&sink, 0, sizeof(sink)); sink.ledger = ledger;

    void *original = NULL;
    assert(hk_objc_commit(&rt, &plan, (void *)imp_replacement, &original, &sink)
           == HK_MUTATION_COMPLETE);

    // Child gained an override; Base is untouched.
    assert(imp_count_on(&w.child, "foo") == 1);
    assert(imp_on(&w.child, "foo") == (void *)imp_replacement);
    assert(imp_on(&w.base, "foo") == (void *)imp_base_foo);
    // The NULL return did NOT become the published original.
    assert(original == (void *)imp_base_foo);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.original_pointer == (void *)imp_base_foo);
    // Undoing an ADDED override is not a clean restore: Child would be left
    // holding a local method it never had.
    assert(!a.mechanically_reversible);
    assert(!a.safe_to_reverse_after_activation);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  inherited-override-keeps-preread-original: PASS\n");
}

static void test_local_only_policy_refuses_inherited(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    // Same inherited -foo, but the request wants a local method only.
    hk_objc_target_t t = target_for(&w.child, NULL, "foo", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_INHERITED_REFUSED);
    assert(!plan.captured);
    assert(imp_count_on(&w.child, "foo") == 0);
    assert(w.rt.replace_calls == 0);

    // ...and the SAME policy accepts the genuinely local -bar, so the refusal
    // is about inheritance and not about the policy rejecting everything.
    hk_objc_target_t ok = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                     HK_OBJC_LOCAL_METHOD_ONLY,
                                     HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(&rt, &ok, &plan) == HK_OBJC_OK);
    printf("  local-only-policy-refuses-inherited: PASS\n");
}

static void test_class_method_uses_the_metaclass(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    // +ping exists ONLY on the metaclass. Resolving it as an instance method
    // must fail -- that is what makes this test able to catch an engine that
    // forgets the metaclass hop.
    hk_objc_target_t as_instance = target_for(&w.base, NULL, "ping", HK_OBJC_INSTANCE_METHOD,
                                              HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                              HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&rt, &as_instance, &plan) == HK_OBJC_METHOD_NOT_FOUND);

    hk_objc_target_t as_class = target_for(&w.base, NULL, "ping", HK_OBJC_CLASS_METHOD,
                                           HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                           HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(&rt, &as_class, &plan) == HK_OBJC_OK);
    assert(plan.cls == &w.base_meta);   // the replace targets the metaclass
    assert(plan.is_local);
    assert(plan.original_imp == (void *)imp_class_ping);

    void *original = NULL;
    assert(hk_objc_commit(&rt, &plan, (void *)imp_replacement, &original, NULL)
           == HK_MUTATION_COMPLETE);
    assert(imp_on(&w.base_meta, "ping") == (void *)imp_replacement);
    assert(original == (void *)imp_class_ping);
    // The class side was never touched.
    assert(imp_count_on(&w.base, "ping") == 0);
    printf("  class-method-uses-the-metaclass: PASS\n");
}

static void test_revalidation_refuses_a_changed_imp(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    hk_objc_target_t t = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_OK);

    // Someone else hooks -bar between prepare and commit.
    w.child.methods[0].imp = (void *)imp_someone_else;

    void *original = NULL;
    assert(hk_objc_commit(&rt, &plan, (void *)imp_replacement, &original, NULL)
           == HK_MUTATION_NONE);
    // Their implementation is left in place, and nothing was published.
    assert(imp_on(&w.child, "bar") == (void *)imp_someone_else);
    assert(original == NULL);
    assert(w.rt.replace_calls == 0);
    printf("  revalidation-refuses-a-changed-imp: PASS\n");
}

static void test_original_is_published_before_the_replace(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    hk_objc_target_t t = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_OK);

    void *original = NULL;
    w.rt.watch_cell = &original;              // the fake samples this at replace time
    w.rt.cell_was_set_at_replace = false;
    assert(hk_objc_commit(&rt, &plan, (void *)imp_replacement, &original, NULL)
           == HK_MUTATION_COMPLETE);
    // Invariant #5, observed rather than assumed: the cell already held the
    // original at the instant the replacement became reachable.
    assert(w.rt.cell_was_set_at_replace);
    printf("  original-is-published-before-the-replace: PASS\n");
}

static void test_availability_policies(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);
    hk_objc_plan_t plan;

    // A class that does not exist.
    hk_objc_target_t missing_req = target_for(NULL, "Nope", "foo", HK_OBJC_INSTANCE_METHOD,
                                              HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                              HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(&rt, &missing_req, &plan) == HK_OBJC_CLASS_NOT_FOUND);

    hk_objc_target_t missing_opt = target_for(NULL, "Nope", "foo", HK_OBJC_INSTANCE_METHOD,
                                              HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                              HK_AVAILABILITY_OPTIONAL_IF_PRESENT);
    assert(hk_objc_prepare(&rt, &missing_opt, &plan) == HK_OBJC_NOT_APPLICABLE);

    // A real class, absent selector.
    hk_objc_target_t no_sel_req = target_for(&w.child, NULL, "nosuch", HK_OBJC_INSTANCE_METHOD,
                                             HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                             HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(&rt, &no_sel_req, &plan) == HK_OBJC_METHOD_NOT_FOUND);

    hk_objc_target_t no_sel_opt = target_for(&w.child, NULL, "nosuch", HK_OBJC_INSTANCE_METHOD,
                                             HK_OBJC_ALLOW_INHERITED_OVERRIDE,
                                             HK_AVAILABILITY_OPTIONAL_IF_PRESENT);
    assert(hk_objc_prepare(&rt, &no_sel_opt, &plan) == HK_OBJC_NOT_APPLICABLE);

    // An already-present target is prepared immediately even when the caller
    // selected deferred availability; only an absent target is queued by the
    // plan-level lifecycle.
    hk_objc_target_t deferred = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                           HK_OBJC_LOCAL_METHOD_ONLY,
                                           HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE);
    assert(hk_objc_prepare(&rt, &deferred, &plan) == HK_OBJC_OK);
    assert(plan.captured);

    assert(w.rt.replace_calls == 0);
    printf("  availability-policies: PASS\n");
}

static void test_resolution_by_name(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    // Named class + named selector reach the same method as the pointer form.
    hk_objc_target_t by_name = target_for(NULL, "Child", "bar", HK_OBJC_INSTANCE_METHOD,
                                          HK_OBJC_LOCAL_METHOD_ONLY,
                                          HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t a;
    assert(hk_objc_prepare(&rt, &by_name, &a) == HK_OBJC_OK);
    assert(a.cls == &w.child && a.original_imp == (void *)imp_child_bar);

    // A supplied pointer wins over the name, even a contradictory one.
    hk_objc_target_t both = target_for(&w.child, "Base", "bar", HK_OBJC_INSTANCE_METHOD,
                                       HK_OBJC_LOCAL_METHOD_ONLY,
                                       HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t b;
    assert(hk_objc_prepare(&rt, &both, &b) == HK_OBJC_OK);
    assert(b.cls == &w.child);
    printf("  resolution-by-name: PASS\n");
}

static void test_argument_validation(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);
    hk_objc_plan_t plan;

    hk_objc_target_t t = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(NULL, &t, &plan) == HK_OBJC_INVALID_ARGUMENT);
    assert(hk_objc_prepare(&rt, NULL, &plan) == HK_OBJC_INVALID_ARGUMENT);
    assert(hk_objc_prepare(&rt, &t, NULL) == HK_OBJC_INVALID_ARGUMENT);

    // A half-filled runtime is refused up front, not part way through.
    hk_objc_runtime_t partial = rt;
    partial.replace_method = NULL;
    assert(hk_objc_prepare(&partial, &t, &plan) == HK_OBJC_INVALID_ARGUMENT);

    // Neither a class pointer nor a class name; likewise for the selector.
    hk_objc_target_t no_class = target_for(NULL, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                           HK_OBJC_LOCAL_METHOD_ONLY,
                                           HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(&rt, &no_class, &plan) == HK_OBJC_INVALID_ARGUMENT);
    hk_objc_target_t no_sel = target_for(&w.child, NULL, NULL, HK_OBJC_INSTANCE_METHOD,
                                         HK_OBJC_LOCAL_METHOD_ONLY,
                                         HK_AVAILABILITY_REQUIRED_NOW);
    assert(hk_objc_prepare(&rt, &no_sel, &plan) == HK_OBJC_INVALID_ARGUMENT);

    // commit guards.
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_OK);
    assert(hk_objc_commit(NULL, &plan, (void *)imp_replacement, NULL, NULL) == HK_MUTATION_NONE);
    assert(hk_objc_commit(&rt, NULL, (void *)imp_replacement, NULL, NULL) == HK_MUTATION_NONE);
    assert(hk_objc_commit(&rt, &plan, NULL, NULL, NULL) == HK_MUTATION_NONE);
    hk_objc_plan_t uncaptured; memset(&uncaptured, 0, sizeof(uncaptured));
    assert(hk_objc_commit(&rt, &uncaptured, (void *)imp_replacement, NULL, NULL) == HK_MUTATION_NONE);
    assert(w.rt.replace_calls == 0);
    printf("  argument-validation: PASS\n");
}

static void test_method_with_no_implementation(void) {
    world_t w; world_init(&w);
    hk_objc_runtime_t rt = make_runtime(&w.rt);

    w.child.methods[0].imp = NULL;  // resolvable method, no implementation
    hk_objc_target_t t = target_for(&w.child, NULL, "bar", HK_OBJC_INSTANCE_METHOD,
                                    HK_OBJC_LOCAL_METHOD_ONLY,
                                    HK_AVAILABILITY_REQUIRED_NOW);
    hk_objc_plan_t plan;
    // Nothing to publish means nothing safe to replace.
    assert(hk_objc_prepare(&rt, &t, &plan) == HK_OBJC_NO_IMPLEMENTATION);
    assert(!plan.captured);
    assert(w.rt.replace_calls == 0);
    printf("  method-with-no-implementation: PASS\n");
}

int main(void) {
    test_local_method_replace_is_authoritative();
    test_inherited_override_keeps_preread_original();
    test_local_only_policy_refuses_inherited();
    test_class_method_uses_the_metaclass();
    test_revalidation_refuses_a_changed_imp();
    test_original_is_published_before_the_replace();
    test_availability_policies();
    test_resolution_by_name();
    test_argument_validation();
    test_method_with_no_implementation();
    printf("all objc engine tests passed\n");
    return 0;
}
