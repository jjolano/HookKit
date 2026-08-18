// hk_plan_t lifecycle, domain registration, hook registration, and
// analysis. Milestone 4 continues: hk_plan_prepare/commit are not
// implemented yet. See HKPlanInternal.h for why domains and hooks are
// individually heap-allocated rather than stored inline in a growable
// array.
//
// hk_plan_analyze reports HK_OUTCOME_NO_ROUTE for every hook right now --
// honestly, not as a stub standing in for something smarter. No engine
// registry exists yet (that's Milestone 6+, and the router that would
// consult it is a separate not-yet-written piece), so there are zero
// candidates for any hook to route to. "The router ran, found nothing to
// route to, and said so truthfully" is what side-effect-free analysis is
// supposed to look like when nothing is registered -- not a placeholder
// to feel embarrassed about.
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
#include "HKArtifactLedger.h"
#include "HKInstalled.h"
#include "HKPlanInternal.h"
#include "HKReportInternal.h"
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

// Fills `out` with the NO_ROUTE defaults every hook starts from, then (if
// the plan's runtime has any registered engines -- see HKEngineInternal.h)
// checks each in registration order and stops at the first eligible one,
// upgrading the result to HK_OUTCOME_ANALYZED. First-eligible-wins, not
// ranked: spec section 9's full ranking algorithm (preferred bits
// satisfied, fewest effects, strongest verification, ...) needs criteria
// this milestone doesn't have real values for yet (no engine has a real
// prepare/commit effect declaration, no ownership ledger exists to check
// conflicts against). Only one fake engine exists in this codebase's own
// tests today, so first-eligible-wins and true ranking aren't
// distinguishable yet either way -- stated so this isn't mistaken for the
// real algorithm once a second engine makes the difference visible.
//
// Side-effect-free by construction: this touches no target, calls no
// provider, allocates only the ordinary heap memory the result/report
// need. describe() is required to be side-effect-free too (spec section
// 8.3) -- not mechanically enforced yet (that needs the interposition-
// based analysis side-effect tests in spec section 21.2, not written),
// but every fake engine this codebase defines honors it by construction.
static void hk_hook_analyze_one(const hk_plan_t *plan, struct hk_hook *hook, hk_hook_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->struct_version = HK_ABI_VERSION_3_0;
    out->request_id = hook->hook_id;
    out->mutation = HK_MUTATION_NONE;
    out->declared_prepare_effects = 0;
    out->observed_prepare_effects = 0;
    out->declared_commit_effects = 0;
    out->observed_commit_effects = 0;
    out->continuation.struct_size = sizeof(out->continuation);
    out->continuation.struct_version = HK_ABI_VERSION_3_0;
    out->continuation.kind = HK_CONTINUATION_KIND_NONE;
    out->original_available = false;
    out->verified = false;
    out->currently_valid = true;
    out->matched_locations = 0;
    out->modified_locations = 0;
    out->error_domain.data = NULL;
    out->error_domain.length = 0;
    out->error_code = 0;
    out->error_message.data = NULL;
    out->error_message.length = 0;
    out->artifact_count = 0;

    for (size_t i = 0; i < plan->runtime->engine_count; i++) {
        hk_engine_capabilities_t caps = plan->runtime->engines[i]->describe();
        if (!hk_engine_eligible_minimal(&caps, hook->spec.target_kind, hook->spec.required_reach)) {
            continue;
        }
        out->outcome = HK_OUTCOME_ANALYZED;
        out->achieved_reach = caps.achievable_reach;
        out->unmet_preferred_reach = hook->spec.preferred_reach & ~caps.achievable_reach;
        out->retryable = false;  // a real route exists now; nothing to retry
        out->diagnostic_engine_id.data = caps.engine_id;
        out->diagnostic_engine_id.length = caps.engine_id ? strlen(caps.engine_id) : 0;
        // Remembered so hk_plan_prepare calls the SAME engine analysis
        // found, rather than re-searching (and risking a different result
        // if the registry changed between analyze and prepare).
        hook->matched_engine = plan->runtime->engines[i];
        return;
    }

    // No engine was eligible (including the "zero engines registered"
    // case this always was before this iteration) -- same honest NO_ROUTE
    // as before, judgment calls on retryable/currently_valid unchanged.
    out->outcome = HK_OUTCOME_NO_ROUTE;
    out->achieved_reach = 0;
    out->unmet_preferred_reach = hook->spec.preferred_reach;
    // Judgment call, stated: NO_ROUTE is retryable because registering an
    // engine later (a runtime/process-lifetime event this plan's analysis
    // has no way to know about in advance) could change the outcome on a
    // fresh hk_plan_analyze call.
    out->retryable = true;
    out->diagnostic_engine_id.data = NULL;
    out->diagnostic_engine_id.length = 0;
    hook->matched_engine = NULL;
}

hk_status_t hk_plan_analyze(hk_plan_t *plan, hk_report_t **out_report) {
    if (out_report) {
        *out_report = NULL;
    }
    if (!plan) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (plan->state != HK_PLAN_DRAFT) {
        // Analysis is meant to snapshot a DRAFT plan's routability.
        // Re-analyzing an already-analyzed (or later-state) plan isn't
        // described anywhere in the spec's plan-state text -- rather than
        // guess at an unspecified re-entry flow, this requires DRAFT,
        // matching the state machine's linear DRAFT -> ANALYZED ->
        // PREPARING -> ... shape (section 6.25).
        return HK_STATUS_INVALID_STATE;
    }

    hk_hook_result_t *results = NULL;
    if (plan->hook_count > 0) {
        results = (hk_hook_result_t *)malloc(plan->hook_count * sizeof(hk_hook_result_t));
        if (!results) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
    }

    for (size_t i = 0; i < plan->hook_count; i++) {
        hk_hook_analyze_one(plan, plan->hooks[i], &results[i]);
        plan->hooks[i]->result = results[i];  // hk_hook_copy_result reads this directly
    }

    hk_report_t *report = hk_report_create(results, plan->hook_count);
    free(results);
    if (!report) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    plan->state = HK_PLAN_ANALYZED;

    if (out_report) {
        *out_report = report;
    } else {
        hk_report_release(report);
    }
    return HK_STATUS_OK;
}

hk_status_t hk_hook_copy_result(const hk_hook_t *hook, hk_hook_result_t *out_result) {
    if (!hook || !out_result) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_result = hook->result;  // flat copy: hk_hook_result_t owns no heap allocations today
    return HK_STATUS_OK;
}

// Spec section 15.1's "domain preparation gate", the specific sub-check
// that's actually checkable today: for a domain with
// require_all_mandatory_prepared set, every HK_OPERATION_MANDATORY hook
// assigned to it must have found a route (HK_OUTCOME_ANALYZED) or the
// WHOLE domain is blocked -- even hooks in it that individually would
// have prepared successfully. NOT checked yet, stated rather than
// silently assumed satisfied: ownership-reservation currency and domain
// dependency cycles (both later Milestone 4 work; there is no ownership
// ledger or dependency graph yet to check against).
static bool hk_domain_mandatory_gate_satisfied(const hk_plan_t *plan, const struct hk_domain *domain) {
    if (!domain->spec.require_all_mandatory_prepared) {
        return true;
    }
    for (size_t i = 0; i < plan->hook_count; i++) {
        const struct hk_hook *hook = plan->hooks[i];
        if (hook->spec.domain != domain) {
            continue;
        }
        if (hook->spec.role == HK_OPERATION_MANDATORY && hook->result.outcome != HK_OUTCOME_ANALYZED) {
            return false;
        }
    }
    return true;
}

// Attempts preparation for every hook that got a route at analyze time
// (HK_OUTCOME_ANALYZED) and whose domain's mandatory gate (above) is
// satisfied. Hooks that stayed NO_ROUTE, or whose domain gate is blocked,
// are left without a prepare_one attempt -- domain-gated hooks are marked
// HK_OUTCOME_FAILED_SAFE (nothing was touched; the gate refused before any
// attempt, not after a failed one) rather than silently carrying forward
// ANALYZED, since ANALYZED would misreport them as still having a live,
// attemptable route.
hk_status_t hk_plan_prepare(hk_plan_t *plan, hk_report_t **out_report) {
    if (out_report) {
        *out_report = NULL;
    }
    if (!plan) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (plan->state != HK_PLAN_ANALYZED) {
        return HK_STATUS_INVALID_STATE;
    }

    hk_hook_result_t *results = NULL;
    if (plan->hook_count > 0) {
        results = (hk_hook_result_t *)malloc(plan->hook_count * sizeof(hk_hook_result_t));
        if (!results) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
    }

    bool *domain_gate_ok = NULL;
    if (plan->domain_count > 0) {
        domain_gate_ok = (bool *)malloc(plan->domain_count * sizeof(bool));
        if (!domain_gate_ok) {
            free(results);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        for (size_t d = 0; d < plan->domain_count; d++) {
            domain_gate_ok[d] = hk_domain_mandatory_gate_satisfied(plan, plan->domains[d]);
        }
    }

    size_t attempted = 0, prepared = 0, failed = 0;

    for (size_t i = 0; i < plan->hook_count; i++) {
        struct hk_hook *hook = plan->hooks[i];
        hk_hook_result_t *out = &results[i];
        *out = hook->result;  // carry forward NO_ROUTE (or whatever analyze left) unchanged by default

        if (hook->result.outcome != HK_OUTCOME_ANALYZED) {
            continue;  // no route -- nothing to prepare
        }

        // Counts as attempted whether or not the domain gate below ends up
        // refusing it: the plan DID process this hook, the gate is just an
        // earlier, cheaper failure point than calling prepare_one -- not
        // "never touched" the way an upstream NO_ROUTE hook is. Getting
        // this wrong (counting gate-blocked hooks as failed but not
        // attempted) breaks the failed<=attempted assumption the
        // PREPARED/PARTIAL/FAILED rollup below depends on -- caught for
        // real by test_domain_gate.c's test_gate_on_blocks_whole_domain,
        // not spotted by inspection.
        attempted++;

        bool gate_blocked = false;
        if (hook->spec.domain) {
            for (size_t d = 0; d < plan->domain_count; d++) {
                if (plan->domains[d] == hook->spec.domain) {
                    gate_blocked = !domain_gate_ok[d];
                    break;
                }
            }
        }
        if (gate_blocked) {
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            failed++;
            hook->result = *out;
            continue;
        }

        // An engine that was eligible for describe() but never implemented
        // prepare_one is a real inconsistency, not a silent skip -- treated
        // as a preparation failure, the same as prepare_one() itself
        // returning false.
        bool ok = hook->matched_engine && hook->matched_engine->prepare_one
                && hook->matched_engine->prepare_one(&hook->spec);

        if (ok) {
            out->outcome = HK_OUTCOME_PREPARED;
            prepared++;
        } else {
            // Preparation failing before anything was reserved is exactly
            // what HK_OUTCOME_FAILED_SAFE means -- this minimal contract
            // has no partial-preparation concept yet, so it's never
            // FAILED_PARTIAL/FAILED_UNKNOWN here.
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            failed++;
        }
        hook->result = *out;
    }
    free(domain_gate_ok);

    hk_report_t *report = hk_report_create(results, plan->hook_count);
    free(results);
    if (!report) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    if (failed == 0) {
        plan->state = HK_PLAN_PREPARED;
    } else if (prepared == 0 && attempted > 0) {
        plan->state = HK_PLAN_FAILED;
    } else {
        plan->state = HK_PLAN_PARTIAL;
    }

    if (out_report) {
        *out_report = report;
    } else {
        hk_report_release(report);
    }
    return HK_STATUS_OK;
}

// Judgment call, stated: accepts HK_PLAN_PREPARED *or* HK_PLAN_PARTIAL --
// the spec's DRAFT -> ANALYZED -> PREPARING -> PREPARED -> COMMITTING ->
// COMMITTED text (section 6.25) only describes the happy path, but a
// PARTIAL plan has real hooks that DID prepare successfully and are worth
// committing; refusing commit outright just because some other hook
// failed to prepare would be less useful than the spec's own domain-gate
// language (section 15.1) implies is intended once it's implemented.
//
// No fallback-after-partial-mutation logic exists here (invariant #4:
// "another route may be attempted only after HK_MUTATION_NONE") because
// there IS no fallback mechanism yet -- each hook has exactly one
// matched_engine, fixed at analyze time, and commit either succeeds or
// fails with that one engine. The invariant is vacuously satisfied (there
// is nothing to violate it), which is different from having correctly
// implemented multi-engine retry -- stated so this isn't mistaken for
// that once a second engine per hook becomes possible.
hk_status_t hk_plan_commit(hk_plan_t *plan, hk_report_t **out_report) {
    if (out_report) {
        *out_report = NULL;
    }
    if (!plan) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (plan->state != HK_PLAN_PREPARED && plan->state != HK_PLAN_PARTIAL) {
        return HK_STATUS_INVALID_STATE;
    }

    hk_hook_result_t *results = NULL;
    if (plan->hook_count > 0) {
        results = (hk_hook_result_t *)malloc(plan->hook_count * sizeof(hk_hook_result_t));
        if (!results) {
            return HK_STATUS_OUT_OF_MEMORY;
        }
    }

    // The artifact ledger engines record into during this commit. Built now
    // (before the report exists) and adopted into the report at the end.
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    if (!ledger) {
        free(results);
        return HK_STATUS_OUT_OF_MEMORY;
    }
    hk_artifact_sink_t sink;
    sink.ledger = ledger;
    sink.plan_id = plan->plan_id;
    sink.runtime_owner_id = hk_runtime_owner_id(plan->runtime);
    sink.request_id = (hk_id_t){0};    // set per hook below
    sink.published_original = NULL;    // reset per hook below

    size_t attempted = 0, active = 0, failed = 0;

    for (size_t i = 0; i < plan->hook_count; i++) {
        struct hk_hook *hook = plan->hooks[i];
        hk_hook_result_t *out = &results[i];
        *out = hook->result;  // carry forward PREPARE-time outcome unless this hook is actually attempted below

        if (hook->result.outcome != HK_OUTCOME_PREPARED) {
            continue;  // never prepared -- nothing to commit
        }
        attempted++;

        sink.request_id = hook->hook_id;  // this hook is the artifact's originating request
        sink.published_original = NULL;   // each hook's engine publishes its own, or none
        hk_mutation_state_t mutation = (hook->matched_engine && hook->matched_engine->commit_one)
            ? hook->matched_engine->commit_one(&hook->spec, &sink)
            : HK_MUTATION_UNKNOWN;  // matched_engine gone, or never implemented commit_one -- an
                                     // engine this inconsistent cannot be trusted to have done
                                     // nothing, so this is UNKNOWN, not the more optimistic NONE.
        out->mutation = mutation;

        switch (mutation) {
        case HK_MUTATION_COMPLETE:
            out->outcome = HK_OUTCOME_ACTIVE;
            active++;
            // If the engine preserved an original, retain it in a
            // process-lifetime installed record so a live replacement can
            // load through it after this plan/hook is released. Set the
            // result's installed_id/original_available BEFORE snapshotting
            // it into the record, so the stored snapshot carries them too.
            if (sink.published_original) {
                hk_id_t installed_id = hk_id_generate();
                out->installed_id = installed_id;
                out->original_available = true;
                hk_installed_hook_t *rec =
                    hk_installed_record_create(installed_id, sink.published_original, out);
                if (rec) {
                    hook->installed = rec;
                } else {
                    // OOM retaining the handle: the mutation still happened,
                    // but we cannot advertise a slot we failed to allocate.
                    out->installed_id = (hk_id_t){0};
                    out->original_available = false;
                }
            }
            break;
        case HK_MUTATION_NONE:
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            failed++;
            break;
        case HK_MUTATION_PARTIAL:
            out->outcome = HK_OUTCOME_FAILED_PARTIAL;
            failed++;
            break;
        case HK_MUTATION_UNKNOWN:
        default:
            out->outcome = HK_OUTCOME_FAILED_UNKNOWN;
            failed++;
            break;
        }
        hook->result = *out;
    }

    hk_report_t *report = hk_report_create(results, plan->hook_count);
    free(results);
    if (!report) {
        hk_artifact_ledger_destroy(ledger);  // report didn't take it; don't leak
        return HK_STATUS_OUT_OF_MEMORY;
    }
    hk_report_adopt_artifact_ledger(report, ledger);  // report now owns the populated ledger

    if (failed == 0) {
        plan->state = HK_PLAN_COMMITTED;
    } else if (active == 0 && attempted > 0) {
        plan->state = HK_PLAN_FAILED;
    } else {
        plan->state = HK_PLAN_PARTIAL;
    }

    if (out_report) {
        *out_report = report;
    } else {
        hk_report_release(report);
    }
    return HK_STATUS_OK;
}

// Original-slot / installed-handle accessors. hk_original_slot_load and
// hk_installed_hook_copy_result live in HKInstalled.c (they need only the
// installed-record internals); the two below need struct hk_hook, so they
// live here. All four were previously undefined -- the state they describe
// (a process-lifetime installed record) did not exist until hk_plan_commit
// began creating one for an active hook whose engine published an original.

// The hook's original slot, or NULL if it has no retained original (never
// went ACTIVE, or its engine published none). The returned slot points into
// the process-global installed registry and stays valid after this hook /
// plan / runtime is released -- the whole reason that registry exists.
const hk_original_slot_t *hk_hook_original_slot(const hk_hook_t *hook) {
    if (!hook || !hook->installed || !hook->installed->has_original) {
        return NULL;
    }
    return &hook->installed->slot;
}

// The hook's installed handle, or NULL if it has no retained installation.
// Same process-lifetime survival as the slot above.
const hk_installed_hook_t *hk_hook_installed_handle(const hk_hook_t *hook) {
    return hook ? hook->installed : NULL;
}
