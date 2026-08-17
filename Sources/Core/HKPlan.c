// hk_plan_t lifecycle, domain registration, and hook registration.
// Milestone 4 continues: hk_plan_analyze/prepare/commit and everything
// past DRAFT state are not implemented yet. See HKPlanInternal.h for why
// domains and hooks are individually heap-allocated rather than stored
// inline in a growable array.
//
// hk_plan_add_hook only supports HK_TARGET_FUNCTION_SYMBOL/_ADDRESS/
// _OBJC_METHOD/_MEMORY_PATCH -- HK_TARGET_SWIFT_VTABLE is rejected with
// HK_STATUS_UNAVAILABLE, matching HookKitTargets.h's design note that
// Swift hooks are meant to go through a dedicated HookKitSwift.h entry
// point (not written yet), never this union. Image selectors with
// kind == HK_IMAGE_EXPLICIT_SET are also rejected (HK_STATUS_UNAVAILABLE):
// deep-copying that case is genuinely recursive (an array of pointers to
// more hk_image_selector_t, each of which could itself carry another
// explicit_set), and nothing in the current Shadow manifest exercises it
// -- real scope decision, not an oversight, and stated here rather than
// silently handled wrong.

#include "HKIDs.h"
#include "HKPlanInternal.h"
#include "HKRuntimeInternal.h"

#include <stdlib.h>
#include <string.h>

// Returns false only on real OOM. A NULL `s` is not an error -- *out is
// set to NULL and the function returns true -- so callers can tell "input
// was NULL" apart from "malloc failed" without a second check.
static bool hk_strdup_checked(const char *s, char **out) {
    if (!s) {
        *out = NULL;
        return true;
    }
    size_t len = strlen(s) + 1;
    *out = (char *)malloc(len);
    if (!*out) {
        return false;
    }
    memcpy(*out, s, len);
    return true;
}

static bool hk_bytes_dup_checked(hk_bytes_view_t src, hk_bytes_view_t *out) {
    if (!src.data || src.size == 0) {
        out->data = NULL;
        out->size = 0;
        return true;
    }
    uint8_t *copy = (uint8_t *)malloc(src.size);
    if (!copy) {
        return false;
    }
    memcpy(copy, src.data, src.size);
    out->data = copy;
    out->size = src.size;
    return true;
}

// Copies the scalar/fixed-size fields of an image selector (kind, uuid,
// the foreign non-owned header pointer) and deep-copies `path` into
// *owned_path. Rejects HK_IMAGE_EXPLICIT_SET before touching anything --
// see this file's header comment for why that case isn't implemented.
// header/explicit_set/explicit_set_count are left zeroed in *dst even for
// non-EXPLICIT_SET kinds: header is a foreign pointer this function never
// owns or copies deeply, so it is intentionally not round-tripped here.
static hk_status_t hk_image_selector_copy(
    const hk_image_selector_t *src,
    hk_image_selector_t *dst,
    char **owned_path)
{
    if (src->kind == HK_IMAGE_EXPLICIT_SET) {
        return HK_STATUS_UNAVAILABLE;
    }
    memset(dst, 0, sizeof(*dst));
    dst->struct_size = sizeof(*dst);
    dst->struct_version = HK_ABI_VERSION_3_0;
    dst->kind = src->kind;
    memcpy(dst->uuid, src->uuid, sizeof(dst->uuid));
    if (!hk_strdup_checked(src->path, owned_path)) {
        return HK_STATUS_OUT_OF_MEMORY;
    }
    dst->path = *owned_path;
    return HK_STATUS_OK;
}

// Frees every owned_* allocation plus the hook struct itself. Used by both
// hk_plan_release (every hook) and hk_plan_add_hook's error-unwind path
// (the one partially-built hook when a later deep-copy step fails) --
// same cleanup either way, written once.
static void hk_hook_free(struct hk_hook *hook) {
    if (!hook) {
        return;
    }
    free(hook->stable_hook_id_owned);
    free(hook->owned_symbol_name);
    free(hook->owned_symbol_defining_image_path);
    free(hook->owned_symbol_caller_image_scope_path);
    free(hook->owned_address_expected_image_path);
    free(hook->owned_address_expected_initial_bytes);
    free(hook->owned_objc_class_name);
    free(hook->owned_objc_selector_name);
    free(hook->owned_memory_base_image_path);
    free(hook->owned_memory_replacement_bytes);
    free(hook->owned_memory_expected_bytes);
    free(hook->owned_memory_expected_mask);
    free((void *)hook->owned_commit_after);
    free(hook);
}

hk_status_t hk_plan_create(
    hk_runtime_t *runtime,
    const hk_plan_config_t *config,
    hk_plan_t **out_plan)
{
    if (!runtime || !out_plan) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_plan = NULL;

    if (config && config->struct_size < sizeof(hk_plan_config_t)) {
        return HK_STATUS_INVALID_ARGUMENT;
    }

    hk_plan_t *plan = (hk_plan_t *)calloc(1, sizeof(hk_plan_t));
    if (!plan) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    plan->plan_id = hk_id_generate();
    plan->runtime = runtime;
    plan->state = HK_PLAN_DRAFT;

    if (config) {
        size_t copy_size = config->struct_size < sizeof(hk_plan_config_t)
                          ? config->struct_size
                          : sizeof(hk_plan_config_t);
        memcpy(&plan->config, config, copy_size);
        // debug_label is a caller-owned pointer in the spec passed in --
        // NOT deep-copied here (unlike stable_domain_id/stable_hook_id
        // below). Real gap, stated: HKPlanInternal.h's hk_plan_config_t
        // has no owned-string slot yet. Fine for now since nothing reads
        // it back through the public API today (no hk_plan_config
        // getter exists), but flagged so the day one is added, this
        // doesn't silently hold a dangling pointer.
    }
    plan->config.struct_size = sizeof(hk_plan_config_t);
    plan->config.struct_version = HK_ABI_VERSION_3_0;

    *out_plan = plan;
    return HK_STATUS_OK;
}

void hk_plan_release(hk_plan_t *plan) {
    if (!plan) {
        return;
    }
    for (size_t i = 0; i < plan->hook_count; i++) {
        hk_hook_free(plan->hooks[i]);
    }
    free(plan->hooks);
    for (size_t i = 0; i < plan->domain_count; i++) {
        free(plan->domains[i]->stable_domain_id_owned);
        free(plan->domains[i]);
    }
    free(plan->domains);
    free(plan);
}

hk_plan_state_t hk_plan_state(const hk_plan_t *plan) {
    if (!plan) {
        return HK_PLAN_DISCARDED;  // closest honest answer for a NULL handle
    }
    return plan->state;
}

hk_status_t hk_plan_define_domain(
    hk_plan_t *plan,
    const hk_domain_spec_t *spec,
    hk_domain_t **out_domain)
{
    if (out_domain) {
        *out_domain = NULL;
    }
    if (!plan || !spec || !out_domain) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (plan->state != HK_PLAN_DRAFT) {
        // "Only HK_PLAN_DRAFT accepts new domains or hooks" -- spec section
        // 6.25, verbatim.
        return HK_STATUS_INVALID_STATE;
    }
    if (spec->struct_size < sizeof(hk_domain_spec_t)) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (!spec->stable_domain_id) {
        return HK_STATUS_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < plan->domain_count; i++) {
        if (strcmp(plan->domains[i]->stable_domain_id_owned, spec->stable_domain_id) == 0) {
            // "Stable IDs are validated unique within the plan at add
            // time" -- spec section 6.24 (written for hooks, applies the
            // same way to domains' stable_domain_id).
            return HK_STATUS_INVALID_ARGUMENT;
        }
    }

    struct hk_domain *domain = (struct hk_domain *)calloc(1, sizeof(struct hk_domain));
    if (!domain) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    size_t id_len = strlen(spec->stable_domain_id);
    domain->stable_domain_id_owned = (char *)malloc(id_len + 1);
    if (!domain->stable_domain_id_owned) {
        free(domain);
        return HK_STATUS_OUT_OF_MEMORY;
    }
    memcpy(domain->stable_domain_id_owned, spec->stable_domain_id, id_len + 1);

    size_t copy_size = spec->struct_size < sizeof(hk_domain_spec_t)
                      ? spec->struct_size
                      : sizeof(hk_domain_spec_t);
    memcpy(&domain->spec, spec, copy_size);
    domain->spec.struct_size = sizeof(hk_domain_spec_t);
    domain->spec.struct_version = HK_ABI_VERSION_3_0;
    domain->spec.stable_domain_id = domain->stable_domain_id_owned;  // repoint at our owned copy
    domain->domain_id = hk_id_generate();

    if (plan->domain_count == plan->domain_capacity) {
        size_t new_capacity = plan->domain_capacity == 0 ? 4 : plan->domain_capacity * 2;
        struct hk_domain **grown = (struct hk_domain **)realloc(
            plan->domains, new_capacity * sizeof(struct hk_domain *));
        if (!grown) {
            free(domain->stable_domain_id_owned);
            free(domain);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        plan->domains = grown;
        plan->domain_capacity = new_capacity;
    }
    plan->domains[plan->domain_count++] = domain;

    *out_domain = domain;
    return HK_STATUS_OK;
}

// Deep-copies spec->target into hook->spec.target and hook's owned_*
// fields, dispatching on target_kind. Each branch repoints the copied
// target struct's pointer fields at the hook's own owned_* allocations,
// exactly like hk_plan_define_domain does for stable_domain_id -- the
// caller's spec is never referenced again after this returns.
static hk_status_t hk_hook_copy_target(struct hk_hook *hook, const hk_hook_spec_t *spec) {
    switch (spec->target_kind) {
    case HK_TARGET_FUNCTION_SYMBOL: {
        const hk_symbol_target_t *src = &spec->target.symbol;
        hk_symbol_target_t *dst = &hook->spec.target.symbol;
        *dst = *src;
        dst->struct_size = sizeof(*dst);
        dst->struct_version = HK_ABI_VERSION_3_0;
        if (!hk_strdup_checked(src->name, &hook->owned_symbol_name)) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
        dst->name = hook->owned_symbol_name;
        hk_status_t st = hk_image_selector_copy(&src->defining_image, &dst->defining_image,
                                                 &hook->owned_symbol_defining_image_path);
        if (st != HK_STATUS_OK) {
            return st;
        }
        st = hk_image_selector_copy(&src->caller_image_scope, &dst->caller_image_scope,
                                     &hook->owned_symbol_caller_image_scope_path);
        if (st != HK_STATUS_OK) {
            return st;
        }
        return HK_STATUS_OK;
    }
    case HK_TARGET_FUNCTION_ADDRESS: {
        const hk_address_target_t *src = &spec->target.address;
        hk_address_target_t *dst = &hook->spec.target.address;
        *dst = *src;
        dst->struct_size = sizeof(*dst);
        dst->struct_version = HK_ABI_VERSION_3_0;
        hk_status_t st = hk_image_selector_copy(&src->expected_image, &dst->expected_image,
                                                 &hook->owned_address_expected_image_path);
        if (st != HK_STATUS_OK) {
            return st;
        }
        if (src->expected_initial_bytes && src->expected_initial_bytes_size > 0) {
            hook->owned_address_expected_initial_bytes =
                (uint8_t *)malloc(src->expected_initial_bytes_size);
            if (!hook->owned_address_expected_initial_bytes) {
                return HK_STATUS_OUT_OF_MEMORY;
            }
            memcpy(hook->owned_address_expected_initial_bytes, src->expected_initial_bytes,
                   src->expected_initial_bytes_size);
            dst->expected_initial_bytes = hook->owned_address_expected_initial_bytes;
        } else {
            dst->expected_initial_bytes = NULL;
            dst->expected_initial_bytes_size = 0;
        }
        return HK_STATUS_OK;
    }
    case HK_TARGET_OBJC_METHOD: {
        const hk_objc_target_t *src = &spec->target.objc;
        hk_objc_target_t *dst = &hook->spec.target.objc;
        *dst = *src;  // includes the non-owned cls/sel opaque pointers, copied as-is
        dst->struct_size = sizeof(*dst);
        dst->struct_version = HK_ABI_VERSION_3_0;
        if (!hk_strdup_checked(src->class_name, &hook->owned_objc_class_name)) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
        dst->class_name = hook->owned_objc_class_name;
        if (!hk_strdup_checked(src->selector_name, &hook->owned_objc_selector_name)) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
        dst->selector_name = hook->owned_objc_selector_name;
        return HK_STATUS_OK;
    }
    case HK_TARGET_MEMORY_PATCH: {
        const hk_memory_target_t *src = &spec->target.memory;
        hk_memory_target_t *dst = &hook->spec.target.memory;
        *dst = *src;
        dst->struct_size = sizeof(*dst);
        dst->struct_version = HK_ABI_VERSION_3_0;
        if (src->address_is_image_relative) {
            hk_status_t st = hk_image_selector_copy(&src->base_image, &dst->base_image,
                                                     &hook->owned_memory_base_image_path);
            if (st != HK_STATUS_OK) {
                return st;
            }
        } else {
            // base_image is documented as "meaningful only when
            // address_is_image_relative" (HookKitTargets.h), but the
            // initial `*dst = *src` above still copied whatever raw,
            // non-owned pointer the caller happened to leave in it. Left
            // alone, that's a dangling-pointer trap the moment the
            // caller's spec goes out of scope -- zero it instead of
            // trusting a field the contract says not to read.
            memset(&dst->base_image, 0, sizeof(dst->base_image));
        }
        hk_bytes_view_t copied;
        if (!hk_bytes_dup_checked(src->replacement_bytes, &copied)) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
        hook->owned_memory_replacement_bytes = (uint8_t *)copied.data;
        dst->replacement_bytes = copied;

        if (!hk_bytes_dup_checked(src->expected_bytes, &copied)) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
        hook->owned_memory_expected_bytes = (uint8_t *)copied.data;
        dst->expected_bytes = copied;

        if (!hk_bytes_dup_checked(src->expected_mask, &copied)) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
        hook->owned_memory_expected_mask = (uint8_t *)copied.data;
        dst->expected_mask = copied;
        return HK_STATUS_OK;
    }
    case HK_TARGET_SWIFT_VTABLE:
    default:
        // Swift: HookKitSwift.h's own entry point, not this union -- see
        // this file's header comment. Anything else is a target_kind this
        // build doesn't know, which is exactly what HK_STATUS_UNAVAILABLE
        // means: a real request this implementation cannot serve, not a
        // malformed one.
        return HK_STATUS_UNAVAILABLE;
    }
}

hk_status_t hk_plan_add_hook(
    hk_plan_t *plan,
    const hk_hook_spec_t *spec,
    hk_hook_t **out_hook)
{
    if (out_hook) {
        *out_hook = NULL;
    }
    if (!plan || !spec || !out_hook) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (plan->state != HK_PLAN_DRAFT) {
        return HK_STATUS_INVALID_STATE;
    }
    if (spec->struct_size < sizeof(hk_hook_spec_t)) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (!spec->stable_hook_id) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (spec->original_requirement == HK_ORIGINAL_CALLABLE_CONTINUATION
        && spec->continuation_policy == HK_CONTINUATION_FORBIDDEN) {
        // "Reject ... as contradictory before route analysis" -- spec
        // section 6.7, verbatim. add_hook is the earliest point that can
        // catch it, so it does, rather than deferring to hk_plan_analyze.
        return HK_STATUS_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < plan->hook_count; i++) {
        if (strcmp(plan->hooks[i]->stable_hook_id_owned, spec->stable_hook_id) == 0) {
            return HK_STATUS_INVALID_ARGUMENT;
        }
    }
    if (spec->domain) {
        bool found = false;
        for (size_t i = 0; i < plan->domain_count; i++) {
            if (plan->domains[i] == spec->domain) {
                found = true;
                break;
            }
        }
        if (!found) {
            // A domain pointer this plan never issued -- another plan's
            // domain, or garbage. Never silently accepted.
            return HK_STATUS_INVALID_ARGUMENT;
        }
    }
    if (spec->commit_after_count > 0 && !spec->commit_after) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < spec->commit_after_count; i++) {
        if (!spec->commit_after[i]) {
            return HK_STATUS_INVALID_ARGUMENT;
        }
        bool found = false;
        for (size_t j = 0; j < plan->hook_count; j++) {
            if (plan->hooks[j] == spec->commit_after[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            // Must already be a member of this plan: hk_hook_spec_t is
            // deep-copied once at add time with no update API, so a
            // forward reference to a not-yet-added hook could never
            // resolve to anything real later.
            return HK_STATUS_INVALID_ARGUMENT;
        }
    }

    struct hk_hook *hook = (struct hk_hook *)calloc(1, sizeof(struct hk_hook));
    if (!hook) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    if (!hk_strdup_checked(spec->stable_hook_id, &hook->stable_hook_id_owned)) {
        hk_hook_free(hook);
        return HK_STATUS_OUT_OF_MEMORY;
    }

    size_t copy_size = spec->struct_size < sizeof(hk_hook_spec_t)
                      ? spec->struct_size
                      : sizeof(hk_hook_spec_t);
    memcpy(&hook->spec, spec, copy_size);
    hook->spec.struct_size = sizeof(hk_hook_spec_t);
    hook->spec.struct_version = HK_ABI_VERSION_3_0;
    hook->spec.stable_hook_id = hook->stable_hook_id_owned;
    hook->spec.target_kind = spec->target_kind;

    hk_status_t target_status = hk_hook_copy_target(hook, spec);
    if (target_status != HK_STATUS_OK) {
        hk_hook_free(hook);
        return target_status;
    }

    if (spec->commit_after_count > 0) {
        hook->owned_commit_after = (const hk_hook_t **)malloc(
            spec->commit_after_count * sizeof(hk_hook_t *));
        if (!hook->owned_commit_after) {
            hk_hook_free(hook);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        memcpy((void *)hook->owned_commit_after, spec->commit_after,
               spec->commit_after_count * sizeof(hk_hook_t *));
    }
    hook->spec.commit_after = hook->owned_commit_after;

    hook->hook_id = hk_id_generate();

    memset(&hook->result, 0, sizeof(hook->result));
    hook->result.struct_size = sizeof(hook->result);
    hook->result.struct_version = HK_ABI_VERSION_3_0;
    hook->result.request_id = hook->hook_id;
    hook->result.outcome = HK_OUTCOME_UNANALYZED;
    hook->result.mutation = HK_MUTATION_NONE;

    if (plan->hook_count == plan->hook_capacity) {
        size_t new_capacity = plan->hook_capacity == 0 ? 4 : plan->hook_capacity * 2;
        struct hk_hook **grown = (struct hk_hook **)realloc(
            plan->hooks, new_capacity * sizeof(struct hk_hook *));
        if (!grown) {
            hk_hook_free(hook);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        plan->hooks = grown;
        plan->hook_capacity = new_capacity;
    }
    plan->hooks[plan->hook_count++] = hook;

    *out_hook = hook;
    return HK_STATUS_OK;
}
