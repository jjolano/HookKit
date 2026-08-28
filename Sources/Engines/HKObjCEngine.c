// Objective-C method engine. See HKObjCEngine.h for the phase split and why
// the runtime is a seam.

#include "HKObjCEngine.h"

#include <string.h>

// Every seam entry is required: a partially-filled runtime would fail at an
// arbitrary point mid-resolution rather than up front.
static bool runtime_complete(const hk_objc_runtime_t *rt) {
    return rt && rt->get_class && rt->get_metaclass && rt->get_superclass &&
           rt->register_selector && rt->get_instance_method &&
           rt->method_get_imp && rt->method_get_types && rt->replace_method;
}

// A method is local to `cls` when resolving it from `cls` and resolving it
// from `cls`'s superclass give different answers. class_getInstanceMethod
// searches ancestors, so equal answers mean both found the same inherited
// method. A root class has no superclass, and the seam contract says a NULL
// class resolves to NULL, so anything found there is local -- which is
// correct, and falls out rather than needing a special case.
static bool method_is_local(const hk_objc_runtime_t *rt, void *cls, void *sel,
                            void *method) {
    void *super = rt->get_superclass(rt->ctx, cls);
    return rt->get_instance_method(rt->ctx, super, sel) != method;
}

hk_objc_status_t hk_objc_prepare(const hk_objc_runtime_t *rt,
                                 const hk_objc_target_t *target,
                                 hk_objc_plan_t *out_plan) {
    if (!runtime_complete(rt) || !target || !out_plan) {
        return HK_OBJC_INVALID_ARGUMENT;
    }
    memset(out_plan, 0, sizeof(*out_plan));

    // DEFER_UNTIL_AVAILABLE used to be refused here, because deferral needed
    // somewhere to retry from and there was nowhere. Milestone 12 built that:
    // the plan marks an absent deferred target PENDING and the runtime retries
    // it on drain. So this engine's job is the same for both conditional
    // forms -- report that the target is not here -- and the difference
    // between "skip it" and "wait for it" is the plan's to make, not the
    // engine's.
    const bool optional = (target->availability == HK_AVAILABILITY_OPTIONAL_IF_PRESENT ||
                           target->availability == HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE);

    // A pointer, when given, wins over the name -- the caller already did the
    // lookup and may have a class the name would not reach.
    void *cls = target->cls;
    if (!cls) {
        if (!target->class_name) {
            return HK_OBJC_INVALID_ARGUMENT;
        }
        cls = rt->get_class(rt->ctx, target->class_name);
        if (!cls) {
            return optional ? HK_OBJC_NOT_APPLICABLE : HK_OBJC_CLASS_NOT_FOUND;
        }
    }

    // Class methods live on the metaclass; resolving and replacing there is
    // the whole of the difference between the two method kinds.
    if (target->method_kind == HK_OBJC_CLASS_METHOD) {
        cls = rt->get_metaclass(rt->ctx, cls);
        if (!cls) {
            return optional ? HK_OBJC_NOT_APPLICABLE : HK_OBJC_CLASS_NOT_FOUND;
        }
    }

    void *sel = target->sel;
    if (!sel) {
        if (!target->selector_name) {
            return HK_OBJC_INVALID_ARGUMENT;
        }
        sel = rt->register_selector(rt->ctx, target->selector_name);
        if (!sel) {
            return HK_OBJC_INVALID_ARGUMENT;
        }
    }

    void *method = rt->get_instance_method(rt->ctx, cls, sel);
    if (!method) {
        return optional ? HK_OBJC_NOT_APPLICABLE : HK_OBJC_METHOD_NOT_FOUND;
    }

    const bool is_local = method_is_local(rt, cls, sel, method);
    if (!is_local && target->inheritance_policy == HK_OBJC_LOCAL_METHOD_ONLY) {
        // Honoring this would mean adding an override, which changes dispatch
        // for every sibling subclass of the ancestor too -- a wider blast
        // radius than was asked for.
        return HK_OBJC_INHERITED_REFUSED;
    }

    void *imp = rt->method_get_imp(rt->ctx, method);
    if (!imp) {
        return HK_OBJC_NO_IMPLEMENTATION;
    }

    out_plan->cls = cls;
    out_plan->sel = sel;
    out_plan->method = method;
    out_plan->original_imp = imp;
    // The encoding is metadata for forwarding. A method with none gets a
    // minimal non-NULL placeholder, which some runtime paths require.
    out_plan->types = rt->method_get_types(rt->ctx, method);
    if (!out_plan->types) {
        out_plan->types = "@:@";
    }
    out_plan->is_local = is_local;
    out_plan->captured = true;
    return HK_OBJC_OK;
}

hk_mutation_state_t hk_objc_commit(const hk_objc_runtime_t *rt,
                                   const hk_objc_plan_t *plan,
                                   void *replacement,
                                   void **out_original,
                                   hk_artifact_sink_t *sink) {
    if (!runtime_complete(rt) || !plan || !plan->captured || !replacement) {
        return HK_MUTATION_NONE;
    }

    if (sink && sink->require_predecessor_match &&
        plan->original_imp != sink->required_predecessor) {
        return HK_MUTATION_NONE;
    }

    // Invariant #3: the method must still resolve to what prepare captured.
    // If someone else replaced it in between, their implementation is the
    // live original -- overwriting it while reporting ours would publish a
    // predecessor that is no longer real.
    void *method = rt->get_instance_method(rt->ctx, plan->cls, plan->sel);
    if (!method || rt->method_get_imp(rt->ctx, method) != plan->original_imp) {
        return HK_MUTATION_NONE;
    }

    // Invariant #5: the original is published before the replacement can be
    // reached, never after.
    if (out_original) {
        *out_original = plan->original_imp;
    }

    void *replaced = rt->replace_method(rt->ctx, plan->cls, plan->sel,
                                        replacement, plan->types);

    // The return value is authoritative only when it is non-NULL, which the
    // runtime does exactly when the method lived on this class. NULL means it
    // ADDED an override over an inherited method and replaced nothing, so the
    // inherited IMP read at prepare stands as the original.
    void *original = replaced ? replaced : plan->original_imp;
    if (replaced && out_original) {
        *out_original = replaced;
    }

    if (sink) {
        hk_artifact_t a;
        memset(&a, 0, sizeof(a));
        a.struct_size = sizeof(a);
        a.struct_version = HK_ABI_VERSION_3_0;
        a.kind = HK_ARTIFACT_OBJC_METHOD_CHANGE;
        a.state = HK_ARTIFACT_COMMITTED;
        a.effects = HK_EFFECT_OBJC_METADATA_MUTATION;
        a.engine_id.data = "objc";
        a.engine_id.length = 4;
        a.original_pointer = original;
        a.replacement_pointer = replacement;
        // Reversibility is a per-artifact fact here, not a per-engine one, and
        // the two cases genuinely differ. Replacing a LOCAL method back is a
        // clean restore. Undoing an ADDED override is not: putting the
        // inherited IMP back leaves the class holding a local method it never
        // had, which changes what a later hook of the ancestor reaches.
        a.mechanically_reversible = plan->is_local;
        a.safe_to_reverse_after_activation = plan->is_local;
        (void)hk_artifact_sink_record(sink, &a);
    }
    return HK_MUTATION_COMPLETE;
}
