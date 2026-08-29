// hk_plan_t lifecycle, domain registration, hook registration, and
// analysis and commit. See HKPlanInternal.h for why domains and hooks are
// individually heap-allocated rather than stored inline in a growable array.
//
// hk_plan_analyze consults the runtime's registered engine vtables and
// records an honest route (or NO_ROUTE) without mutating a target.
//
// hk_plan_add_hook only supports HK_TARGET_FUNCTION_SYMBOL/_ADDRESS/
// _OBJC_METHOD/_MEMORY_PATCH -- HK_TARGET_SWIFT_VTABLE is rejected with
// HK_STATUS_UNAVAILABLE, matching HookKitTargets.h's design note that
// Swift hooks go through the dedicated HookKitSwift.h entry point, never
// this union. Image selectors, including recursively-owned explicit sets,
// are copied at add time so their caller-owned trees do not outlive the call.

#include "HKIDs.h"
#include "HKArtifactLedger.h"
#include "HKInstalled.h"
#include "HKOwnership.h"
#include "HKPlanInternal.h"
#include "HKReportInternal.h"
#include "HKRuntimeInternal.h"

#include <stdlib.h>
#include <string.h>

static bool hk_engine_ids_equal(const char *left, const char *right);
static void hk_plan_set_ownership_error(hk_hook_result_t *out,
                                        hk_outcome_t outcome,
                                        const char *message);

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

// Image selectors are the one recursive public value in a hook spec. Copy
// the tree once at add time so deferred hooks and released plans never retain
// caller-owned paths or child arrays. The depth cap turns a malicious
// self-referential selector into INVALID_ARGUMENT instead of recursion.
#define HK_MAX_IMAGE_SELECTOR_DEPTH 32u

static void hk_image_selector_owned_destroy(hk_owned_image_selector_t *owned) {
    if (!owned) {
        return;
    }
    for (size_t i = 0; i < owned->child_count; i++) {
        hk_image_selector_owned_destroy(owned->children[i]);
    }
    free(owned->children);
    free((void *)owned->child_views);
    free(owned->path);
    free(owned);
}

static hk_status_t hk_image_selector_copy_owned(
    const hk_image_selector_t *src,
    hk_owned_image_selector_t **out_owned,
    unsigned depth)
{
    if (!src || !out_owned || depth > HK_MAX_IMAGE_SELECTOR_DEPTH) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_owned = NULL;

    hk_owned_image_selector_t *owned = calloc(1, sizeof(*owned));
    if (!owned) {
        return HK_STATUS_OUT_OF_MEMORY;
    }
    owned->value.struct_size = sizeof(owned->value);
    owned->value.struct_version = HK_ABI_VERSION_3_0;
    owned->value.kind = src->kind;
    owned->value.header = src->header;
    memcpy(owned->value.uuid, src->uuid, sizeof(owned->value.uuid));

    if (src->path) {
        if (!hk_strdup_checked(src->path, &owned->path)) {
            hk_image_selector_owned_destroy(owned);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        owned->value.path = owned->path;
    }

    if (src->kind == HK_IMAGE_EXPLICIT_SET) {
        if (src->explicit_set_count > 0 && !src->explicit_set) {
            hk_image_selector_owned_destroy(owned);
            return HK_STATUS_INVALID_ARGUMENT;
        }
        if (src->explicit_set_count > SIZE_MAX / sizeof(*owned->children) ||
            src->explicit_set_count > SIZE_MAX / sizeof(*owned->child_views)) {
            hk_image_selector_owned_destroy(owned);
            return HK_STATUS_INVALID_ARGUMENT;
        }
        owned->child_count = src->explicit_set_count;
        if (owned->child_count > 0) {
            owned->children = calloc(owned->child_count,
                                     sizeof(*owned->children));
            owned->child_views = calloc(owned->child_count,
                                        sizeof(*owned->child_views));
            if (!owned->children || !owned->child_views) {
                hk_image_selector_owned_destroy(owned);
                return HK_STATUS_OUT_OF_MEMORY;
            }
            for (size_t i = 0; i < owned->child_count; i++) {
                hk_status_t status = hk_image_selector_copy_owned(
                    src->explicit_set[i], &owned->children[i], depth + 1);
                if (status != HK_STATUS_OK) {
                    hk_image_selector_owned_destroy(owned);
                    return status;
                }
                owned->child_views[i] = &owned->children[i]->value;
            }
            owned->value.explicit_set = owned->child_views;
            owned->value.explicit_set_count = owned->child_count;
        }
    }

    *out_owned = owned;
    return HK_STATUS_OK;
}

static hk_status_t hk_image_selector_copy(
    const hk_image_selector_t *src,
    hk_image_selector_t *dst,
    hk_owned_image_selector_t **out_owned)
{
    hk_status_t status = hk_image_selector_copy_owned(src, out_owned, 0);
    if (status == HK_STATUS_OK) {
        *dst = (*out_owned)->value;
    }
    return status;
}

// Frees every owned_* allocation plus the hook struct itself. Used by both
// hk_plan_release (every hook) and hk_plan_add_hook's error-unwind path
// (the one partially-built hook when a later deep-copy step fails) --
// same cleanup either way, written once.
// Releases whatever prepare_one_ctx produced for this hook, if anything.
// Called both on re-preparation and at free, so every successful preparation
// is balanced by exactly one release even when commit never runs.
static void hk_hook_release_prepared(struct hk_hook *hook) {
    if (!hook || !hook->prepared_state) {
        return;
    }
    if (hook->matched_engine && hook->matched_engine->release_prepared) {
        hook->matched_engine->release_prepared(hook->matched_engine_ctx,
                                               hook->prepared_state);
    }
    hook->prepared_state = NULL;
    hook->has_prepared_continuation = false;
    memset(&hook->prepared_continuation, 0,
           sizeof(hook->prepared_continuation));
}

static void hk_hook_free(struct hk_hook *hook) {
    if (!hook) {
        return;
    }
    hk_hook_release_prepared(hook);
    free(hook->stable_hook_id_owned);
    free(hook->owned_symbol_name);
    hk_image_selector_owned_destroy(hook->owned_symbol_defining_image);
    hk_image_selector_owned_destroy(hook->owned_symbol_caller_image_scope);
    hk_image_selector_owned_destroy(hook->owned_address_expected_image);
    free(hook->owned_address_expected_initial_bytes);
    free(hook->owned_objc_class_name);
    free(hook->owned_objc_selector_name);
    hk_image_selector_owned_destroy(hook->owned_memory_base_image);
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
        if (!hk_strdup_checked(config->debug_label, &plan->owned_debug_label)) {
            free(plan);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        plan->config.debug_label = plan->owned_debug_label;
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
        struct hk_hook *hook = plan->hooks[i];
        // A hook still waiting for its target outlives the plan: ownership
        // moves to the runtime, which retries it on drain. If the queue cannot
        // grow, free it rather than leak -- a dropped retry is a reported
        // absence, a leak is neither.
        if (hook->result.outcome == HK_OUTCOME_PENDING && plan->runtime &&
            hk_runtime_adopt_pending_hook(plan->runtime, hook)) {
            continue;
        }
        hk_hook_free(hook);
    }
    free(plan->hooks);
    for (size_t i = 0; i < plan->domain_count; i++) {
        free(plan->domains[i]->stable_domain_id_owned);
        free(plan->domains[i]);
    }
    free(plan->domains);
    free(plan->owned_debug_label);
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
                                                &hook->owned_symbol_defining_image);
        if (st != HK_STATUS_OK) {
            return st;
        }
        st = hk_image_selector_copy(&src->caller_image_scope, &dst->caller_image_scope,
                                    &hook->owned_symbol_caller_image_scope);
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
                                                &hook->owned_address_expected_image);
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
                                                    &hook->owned_memory_base_image);
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

// An engine may describe a prepared continuation before commit. The callback
// is optional: engines that publish the description only as part of commit
// keep the old path, while engines with a prepared continuation can expose it
// without resolving the target twice.
static hk_prepare_result_t hk_hook_inspect_prepared_continuation(
    struct hk_hook *hook,
    hk_prepare_diag_t *diag) {
    if (!hook || !hook->matched_engine ||
        !hk_engine_vtable_has_field(
            hook->matched_engine,
            offsetof(hk_engine_vtable_t, inspect_continuation),
            sizeof(hook->matched_engine->inspect_continuation)) ||
        !hook->matched_engine->inspect_continuation) {
        return HK_PREPARE_OK;
    }

    hk_continuation_info_t info;
    memset(&info, 0, sizeof(info));
    hk_verify_diag_t inspect_diag;
    memset(&inspect_diag, 0, sizeof(inspect_diag));
    hk_verify_result_t result = hook->matched_engine->inspect_continuation(
        hook->matched_engine_ctx, &hook->spec, hook->prepared_state, &info,
        &inspect_diag);
    if (result == HK_VERIFY_UNAVAILABLE) {
        return HK_PREPARE_OK;
    }
    if (result == HK_VERIFY_OK) {
        if (info.struct_size < sizeof(info)) {
            inspect_diag.error_code = HK_STATUS_INTERNAL_ERROR;
            inspect_diag.error_message =
                "engine returned a truncated continuation description";
            result = HK_VERIFY_FAILED;
        } else {
            hook->prepared_continuation = info;
            hook->has_prepared_continuation = true;
            return HK_PREPARE_OK;
        }
    }

    hk_hook_release_prepared(hook);
    diag->error_code = inspect_diag.error_code != 0
        ? inspect_diag.error_code
        : HK_STATUS_INTERNAL_ERROR;
    diag->error_message = inspect_diag.error_message
        ? inspect_diag.error_message
        : "engine could not inspect its prepared continuation";
    return HK_PREPARE_FAILED;
}

// Per-hook engine dispatch, extracted so hk_plan_prepare/commit and
// hk_runtime_drain_pending all reach engines the SAME way. A deferred hook
// retried later must go through exactly the path it would have taken at
// prepare time, or "retry" would quietly mean something different from "try".

// Preference order, richest first. An engine implementing a later entry point
// meant it; the earlier ones are what an engine written before that entry
// point existed still uses.
static hk_prepare_result_t hk_hook_dispatch_prepare(struct hk_hook *hook,
                                                    hk_prepare_diag_t *out_diag) {
    memset(out_diag, 0, sizeof(*out_diag));
    hk_prepare_result_t result;
    if (hook->matched_engine && hook->matched_engine->prepare_one_ctx_status) {
        void *prepared = NULL;
        result =
            hook->matched_engine->prepare_one_ctx_status(hook->matched_engine_ctx,
                                                         &hook->spec, &prepared, out_diag);
        hook->prepared_state = (result == HK_PREPARE_OK) ? prepared : NULL;
        return result == HK_PREPARE_OK
            ? hk_hook_inspect_prepared_continuation(hook, out_diag)
            : result;
    }
    if (hook->matched_engine && hook->matched_engine->prepare_one_ctx) {
        void *prepared = NULL;
        bool ok = hook->matched_engine->prepare_one_ctx(hook->matched_engine_ctx,
                                                        &hook->spec, &prepared);
        // Only a successful preparation may hand state forward; a false
        // return means nothing was reserved, so nothing is retained.
        hook->prepared_state = ok ? prepared : NULL;
        result = ok ? HK_PREPARE_OK : HK_PREPARE_FAILED;
        return result == HK_PREPARE_OK
            ? hk_hook_inspect_prepared_continuation(hook, out_diag)
            : result;
    }
    bool ok = hook->matched_engine && hook->matched_engine->prepare_one
            && hook->matched_engine->prepare_one(&hook->spec);
    result = ok ? HK_PREPARE_OK : HK_PREPARE_FAILED;
    return result == HK_PREPARE_OK
        ? hk_hook_inspect_prepared_continuation(hook, out_diag)
        : result;
}

// Preparation effects are an engine contract, not just a report field. A
// continuation allocator that does something outside its descriptor must not
// quietly turn a constrained request into a different request. On violation,
// discard any prepared state and make the operation fail safely.
static void hk_hook_validate_prepare_effects(struct hk_hook *hook,
                                              hk_prepare_result_t *result,
                                              hk_prepare_diag_t *diag) {
    if (!hook || !result || !diag || !hook->matched_engine ||
        diag->observed_effects == 0) {
        return;
    }
    hk_engine_capabilities_t caps = hook->matched_engine->describe();
    if ((diag->observed_effects & ~caps.prepare_effects) == 0) {
        return;
    }
    if (*result == HK_PREPARE_OK) {
        hk_hook_release_prepared(hook);
    }
    *result = HK_PREPARE_FAILED;
    diag->error_code = HK_STATUS_INTERNAL_ERROR;
    diag->error_message = "engine produced an undeclared preparation effect";
}

// Commit effects are checked at the same boundary as preparation effects.
// An engine that reports an effect outside its static declaration is no longer
// trustworthy enough for COMPLETE or NONE: the core must surface UNKNOWN and
// must not make a fallback decision from the lie.
static bool hk_hook_validate_commit_effects(const struct hk_hook *hook,
                                            const hk_artifact_sink_t *sink,
                                            hk_hook_result_t *out) {
    if (!hook || !sink || !out || !hook->matched_engine ||
        sink->observed_effects == 0) {
        return true;
    }
    hk_engine_capabilities_t caps = hook->matched_engine->describe();
    if ((sink->observed_effects & ~caps.commit_effects) == 0) {
        return true;
    }
    out->error_code = HK_STATUS_INTERNAL_ERROR;
    out->error_domain.data = "core";
    out->error_domain.length = 4;
    out->error_message.data = "engine produced an undeclared commit effect";
    out->error_message.length = strlen(out->error_message.data);
    return false;
}

// Same preference, and it has to be the same: an engine whose prepare produced
// state must be the one handed it back.
static hk_mutation_state_t hk_hook_dispatch_commit(struct hk_hook *hook,
                                                   hk_artifact_sink_t *sink) {
    if (hook->matched_engine && hook->matched_engine->commit_one_ctx) {
        return hook->matched_engine->commit_one_ctx(hook->matched_engine_ctx,
                                                    &hook->spec,
                                                    hook->prepared_state, sink);
    }
    if (hook->matched_engine && hook->matched_engine->commit_one) {
        return hook->matched_engine->commit_one(&hook->spec, sink);
    }
    // matched_engine gone, or never implemented commit_one -- an engine this
    // inconsistent cannot be trusted to have done nothing, so this is UNKNOWN,
    // not the more optimistic NONE.
    return HK_MUTATION_UNKNOWN;
}

static hk_verify_result_t hk_hook_dispatch_verify(struct hk_hook *hook,
                                                  hk_verify_diag_t *out_diag) {
    memset(out_diag, 0, sizeof(*out_diag));
    if (hook->matched_engine && hook->matched_engine->verify_one_ctx) {
        return hook->matched_engine->verify_one_ctx(hook->matched_engine_ctx,
                                                    &hook->spec,
                                                    hook->prepared_state,
                                                    out_diag);
    }
    if (hook->matched_engine && hook->matched_engine->verify_one) {
        return hook->matched_engine->verify_one(&hook->spec, out_diag);
    }
    return HK_VERIFY_UNAVAILABLE;
}

// A COMPLETE mutation is only VERIFIED after the selected engine performs a
// post-write readback. A failed readback makes the target state UNKNOWN; an
// omitted verifier leaves the mutation active but honestly unverified.
static void hk_hook_apply_verification(struct hk_hook *hook,
                                       hk_artifact_ledger_t *ledger,
                                       size_t artifact_start,
                                       hk_mutation_state_t *mutation,
                                       hk_hook_result_t *out,
                                       const hk_engine_operation_t *group_operation) {
    out->verified = false;
    if (*mutation != HK_MUTATION_COMPLETE || !hook->matched_engine) {
        return;
    }

    hk_verify_diag_t diag;
    hk_verify_result_t result;
    if (group_operation) {
        diag = group_operation->verify_diag;
        result = group_operation->verify_result;
    } else {
        result = hk_hook_dispatch_verify(hook, &diag);
    }
    if (result == HK_VERIFY_UNAVAILABLE) {
        return;
    }
    if (result == HK_VERIFY_OK) {
        size_t artifact_count = hk_artifact_ledger_count(ledger) - artifact_start;
        if (hk_artifact_ledger_mark_verified(ledger, artifact_start,
                                             artifact_count)) {
            out->verified = true;
            return;
        }
        result = HK_VERIFY_FAILED;
        diag.error_code = HK_STATUS_INTERNAL_ERROR;
        diag.error_message = "could not mark verified artifacts";
    }

    *mutation = HK_MUTATION_UNKNOWN;
    out->error_code = diag.error_code != 0
        ? diag.error_code
        : HK_STATUS_INTERNAL_ERROR;
    out->error_domain.data = "engine";
    out->error_domain.length = 6;
    if (hook->matched_engine->describe) {
        hk_engine_capabilities_t caps = hook->matched_engine->describe();
        if (caps.engine_id) {
            out->error_domain.data = caps.engine_id;
            out->error_domain.length = strlen(caps.engine_id);
        }
    }
    out->error_message.data = diag.error_message
        ? diag.error_message
        : "post-commit verification failed";
    out->error_message.length = strlen(out->error_message.data);
}

// Completes the common result/artifact/verification rollup after either a
// one-hook or grouped commit callback has supplied the mutation state.
static void hk_plan_finish_commit_attempt(
    const hk_plan_t *plan,
    struct hk_hook *hook,
    hk_hook_result_t *out,
    hk_artifact_ledger_t *ledger,
    size_t artifact_start,
    hk_mutation_state_t mutation,
    hk_artifact_sink_t *sink,
    const hk_engine_operation_t *group_operation,
    const hk_ownership_state_t *ownership,
    const hk_engine_capabilities_t *ownership_caps,
    bool compensated,
    size_t *active,
    size_t *failed)
{
    if (sink->record_failed && mutation == HK_MUTATION_COMPLETE) {
        mutation = HK_MUTATION_UNKNOWN;
    }
    if (!hk_hook_validate_commit_effects(hook, sink, out)) {
        mutation = HK_MUTATION_UNKNOWN;
    }
    out->mutation = mutation;
    if (sink->has_continuation) {
        out->continuation = sink->continuation;
    }
    out->observed_commit_effects = sink->observed_effects;
    out->artifact_count = hk_artifact_ledger_count(ledger) - artifact_start;
    if (out->artifact_count > 0) {
        out->matched_locations = (uint32_t)out->artifact_count;
        out->modified_locations = (mutation == HK_MUTATION_NONE) ? 0u
                                                                  : (uint32_t)out->artifact_count;
    }
    out->installed_generation = hk_image_catalog_generation(plan->runtime->catalog);
    hk_hook_apply_verification(hook, ledger, artifact_start, &mutation, out,
                               group_operation);
    out->mutation = mutation;

    if (ownership && ownership_caps && mutation == HK_MUTATION_COMPLETE &&
        !hk_ownership_record_locked(
            &hook->spec, ownership_caps->engine_id,
            hook->spec.replacement, sink->published_original)) {
        mutation = HK_MUTATION_UNKNOWN;
        out->mutation = mutation;
        out->error_domain.data = "core";
        out->error_domain.length = 4;
        out->error_code = HK_STATUS_OUT_OF_MEMORY;
        out->error_message.data = "could not record target ownership";
        out->error_message.length = strlen(out->error_message.data);
    }

    if (compensated) {
        out->outcome = HK_OUTCOME_COMPENSATED;
        (*failed)++;
        hook->result = *out;
        return;
    }

    switch (mutation) {
    case HK_MUTATION_COMPLETE:
        out->outcome = HK_OUTCOME_ACTIVE;
        (*active)++;
        if (sink->published_original) {
            hk_id_t installed_id = hk_id_generate();
            out->installed_id = installed_id;
            out->original_available = true;
            hk_installed_hook_t *rec =
                hk_installed_record_create(installed_id,
                                           sink->published_original, out);
            if (rec) {
                hook->installed = rec;
            } else {
                // The mutation happened, but the process-lifetime handle
                // could not be retained; do not advertise a false slot.
                out->installed_id = (hk_id_t){0};
                out->original_available = false;
            }
        }
        break;
    case HK_MUTATION_NONE:
        out->outcome = HK_OUTCOME_FAILED_SAFE;
        (*failed)++;
        break;
    case HK_MUTATION_PARTIAL:
        out->outcome = HK_OUTCOME_FAILED_PARTIAL;
        (*failed)++;
        break;
    case HK_MUTATION_UNKNOWN:
    default:
        out->outcome = HK_OUTCOME_FAILED_UNKNOWN;
        (*failed)++;
        break;
    }
    hook->result = *out;
}

static hk_status_t hk_plan_add_hook_impl(
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
    // Spec 6.19: a memory patch must say what it expects to find. Without it
    // the region cannot be revalidated, so committing means writing over
    // whatever is there -- a different build, an already-patched region, or
    // simply the wrong address. Rejected here rather than at prepare, because
    // it is a malformed REQUEST and not a runtime condition.
    if (spec->target_kind == HK_TARGET_MEMORY_PATCH &&
        spec->target.memory.expected_bytes.data == NULL) {
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

// Fills `out` with the NO_ROUTE defaults every hook starts from, then chooses
// the eligible engine with the most preferred reach, then the explicit
// request-specific route rank. Registration order remains the stable final
// tie-breaker for equal capability.
//
// Side-effect-free by construction: this touches no target, calls no
// provider, allocates only the ordinary heap memory the result/report
// need. describe() is required to be side-effect-free too (spec section
// 8.3) -- not mechanically enforced yet (that needs the interposition-
// based analysis side-effect tests in spec section 21.2, not written),
// but every fake engine this codebase defines honors it by construction.
static unsigned hk_bit_count64(uint64_t value) {
    return __builtin_popcountll(value);
}

// A static reachability bit is only an honest per-operation result when the
// engine's context can prove it. Exact image scope is the one capability that
// depends on runtime state: the adapters check against a live catalog, and an
// empty catalog intentionally means "not checked". Keep the general engine
// descriptor static, but remove this bit for analysis when that proof is not
// available. The same effective mask is used for required and preferred
// reach, so a preferred bit is never reported as achieved by accident.
static hk_reachability_t hk_engine_effective_reach(
    const hk_plan_t *plan,
    const struct hk_hook *hook,
    const hk_engine_capabilities_t *caps)
{
    hk_reachability_t reach = caps->achievable_reach;
    if (!(reach & HK_REACH_EXACT_IMAGE_SCOPE)) {
        return reach;
    }
    bool supported = caps->exact_image_scope_targets &
                     HK_TARGET_KIND_BIT(hook->spec.target_kind);
    if (!plan->runtime->catalog ||
        hk_image_catalog_count(plan->runtime->catalog) == 0) {
        supported = false;
    }
    if (hook->spec.target_kind == HK_TARGET_MEMORY_PATCH &&
        !hook->spec.target.memory.address_is_image_relative) {
        supported = false;
    }
    // The built-in rebind adapter does not claim this bit: caller/importer
    // scope alone is not defining-image proof. A future symbol engine may
    // claim it explicitly, in which case retain the achieved bit here and
    // let the engine's preparation path enforce the selector.
    if (hook->spec.target_kind != HK_TARGET_FUNCTION_ADDRESS &&
        hook->spec.target_kind != HK_TARGET_MEMORY_PATCH &&
        !(hook->spec.target_kind == HK_TARGET_FUNCTION_SYMBOL &&
          (caps->exact_image_scope_targets &
           HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL)) != 0)) {
        supported = false;
    }
    if (!supported) {
        reach &= ~HK_REACH_EXACT_IMAGE_SCOPE;
    }
    return reach;
}

// A defining-image selector is a promise about WHERE a symbol comes from, not
// merely which importer gets rewritten. The current rebind adapter can prove
// caller/importer scope, but it cannot resolve a bind back to one defining
// image, so do not route a restricted defining-image request to it and then
// silently ignore that field. A future engine that declares exact scope for
// symbols is allowed through this gate.
static bool hk_engine_honors_symbol_defining_scope(
    const struct hk_hook *hook,
    const hk_engine_capabilities_t *caps)
{
    if (!hook || hook->spec.target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        return true;
    }
    if (hook->spec.target.symbol.defining_image.kind == HK_IMAGE_ANY_LOADED) {
        return true;
    }
    return (caps->exact_image_scope_targets &
            HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL)) != 0;
}

static void hk_hook_analyze_one(const hk_plan_t *plan, struct hk_hook *hook, hk_hook_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->struct_version = HK_ABI_VERSION_3_0;
    out->request_id = hook->hook_id;
    out->plan_id = plan->plan_id;
    out->runtime_owner_id = hk_runtime_owner_id(plan->runtime);
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
    out->image_generation = hk_image_catalog_generation(plan->runtime->catalog);
    out->matched_locations = 0;
    out->modified_locations = 0;
    out->error_domain.data = NULL;
    out->error_domain.length = 0;
    out->error_code = 0;
    out->error_message.data = NULL;
    out->error_message.length = 0;
    out->artifact_count = 0;

    size_t best = plan->runtime->engine_count;
    unsigned best_preferred = 0;
    uint32_t best_route_rank = 0;
    hk_engine_capabilities_t best_caps;
    memset(&best_caps, 0, sizeof(best_caps));
    hk_reachability_t best_reach = 0;

    for (size_t i = 0; i < plan->runtime->engine_count; i++) {
        const hk_engine_vtable_t *candidate = plan->runtime->engines[i];
        void *candidate_ctx = plan->runtime->engine_ctxs[i];
        hk_engine_capabilities_t caps = candidate->describe();
        hk_reachability_t effective_reach =
            hk_engine_effective_reach(plan, hook, &caps);
        uint32_t route_rank = 0;

        if (hk_engine_vtable_has_field(
                candidate, offsetof(hk_engine_vtable_t, discover),
                sizeof(candidate->discover)) && candidate->discover) {
            hk_engine_discovery_t discovery;
            memset(&discovery, 0, sizeof(discovery));
            if (!candidate->discover(candidate_ctx, &discovery) ||
                !discovery.available) {
                continue;
            }
        }
        if (hk_engine_vtable_has_field(
                candidate, offsetof(hk_engine_vtable_t, analyze_operation),
                sizeof(candidate->analyze_operation)) &&
            candidate->analyze_operation) {
            hk_engine_analysis_t analysis;
            memset(&analysis, 0, sizeof(analysis));
            if (!candidate->analyze_operation(candidate_ctx, &hook->spec,
                                               &analysis) ||
                !analysis.eligible) {
                continue;
            }
            if (analysis.achieved_reach != 0) {
                effective_reach &= analysis.achieved_reach;
            }
            route_rank = analysis.route_rank;
            if ((analysis.required_prepare_effects & ~caps.prepare_effects) != 0 ||
                (analysis.required_commit_effects & ~caps.commit_effects) != 0) {
                continue;
            }
        }
        if (!hk_engine_eligible_minimal_full(&caps, hook->spec.target_kind,
                                             hook->spec.required_reach,
                                             hook->spec.original_requirement,
                                             hk_effective_constraints(
                                                 hook->spec.constraints,
                                                 hook->spec.continuation_policy)) ||
            (hook->spec.required_reach & ~effective_reach) != 0 ||
            !hk_engine_honors_symbol_defining_scope(hook, &caps) ||
            !hk_engine_supports_install_context(
                &caps, plan->runtime->config.install_context) ||
            !hk_engine_supports_platform(
                &caps, plan->runtime->platform_architecture,
                plan->runtime->platform_ios_version,
                plan->runtime->engine_testing[i])) {
            continue;
        }
        unsigned preferred = hk_bit_count64(effective_reach &
                                            hook->spec.preferred_reach);
        if (best == plan->runtime->engine_count || preferred > best_preferred ||
            (preferred == best_preferred && route_rank > best_route_rank)) {
            best = i;
            best_preferred = preferred;
            best_route_rank = route_rank;
            best_caps = caps;
            best_reach = effective_reach;
        }
    }

    if (best != plan->runtime->engine_count) {
        out->outcome = HK_OUTCOME_ANALYZED;
        out->achieved_reach = best_reach;
        out->unmet_preferred_reach = hook->spec.preferred_reach &
                                     ~best_reach;
        out->retryable = false;  // a real route exists now; nothing to retry
        out->diagnostic_engine_id.data = best_caps.engine_id;
        out->diagnostic_engine_id.length = best_caps.engine_id
            ? strlen(best_caps.engine_id) : 0;
        out->declared_prepare_effects = best_caps.prepare_effects;
        out->declared_commit_effects = best_caps.commit_effects;
        out->image_generation = hk_image_catalog_generation(plan->runtime->catalog);
        // Remembered so hk_plan_prepare calls the SAME engine analysis
        // found, rather than re-searching (and risking a different result
        // if the registry changed between analyze and prepare).
        hook->matched_engine = plan->runtime->engines[best];
        hook->matched_engine_ctx = plan->runtime->engine_ctxs[best];
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
    hook->matched_engine_ctx = NULL;
}

hk_status_t hk_plan_add_hook(
    hk_plan_t *plan,
    const hk_hook_spec_t *spec,
    hk_hook_t **out_hook)
{
    return hk_plan_add_hook_impl(plan, spec, out_hook);
}

// Milestone 12: hand a deferred hook to the runtime instead of freeing it.
// See HKRuntimeInternal.h for why this transfers rather than copies.
bool hk_runtime_adopt_pending_hook(hk_runtime_t *runtime, struct hk_hook *hook) {
    if (!runtime || !hook) {
        return false;
    }
    if (runtime->pending_count == runtime->pending_capacity) {
        size_t cap = runtime->pending_capacity ? runtime->pending_capacity * 2 : 4;
        struct hk_hook **grown =
            (struct hk_hook **)realloc(runtime->pending, cap * sizeof(*grown));
        if (!grown) {
            return false;  // caller keeps ownership and frees it
        }
        runtime->pending = grown;
        runtime->pending_capacity = cap;
    }
    runtime->pending[runtime->pending_count++] = hook;
    return true;
}

// Frees every queued hook. Called from hk_runtime_release, which is where the
// runtime's ownership of them ends.
void hk_runtime_free_pending_hooks(hk_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    for (size_t i = 0; i < runtime->pending_count; i++) {
        hk_hook_free(runtime->pending[i]);
    }
    free(runtime->pending);
    runtime->pending = NULL;
    runtime->pending_count = 0;
    runtime->pending_capacity = 0;
}

// Retries every queued hook, in queue order.
//
// A retry goes through hk_hook_dispatch_prepare/commit -- the SAME path a
// first attempt takes -- so "retried later" means exactly "tried again", not
// something subtly different. A hook that now installs leaves the queue; one
// whose target is still absent stays PENDING and stays queued; one that fails
// for any other reason leaves the queue with its failure reported, because a
// target that is present and unhookable will not become hookable by waiting.
hk_status_t hk_runtime_drain_pending(hk_runtime_t *runtime, hk_report_t **out_report) {
    if (out_report) {
        *out_report = NULL;
    }
    if (!runtime) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (runtime->pending_count == 0) {
        return HK_STATUS_OK;  // nothing queued is a steady state, not a failure
    }

    const size_t n = runtime->pending_count;
    hk_hook_result_t *results = (hk_hook_result_t *)calloc(n, sizeof(*results));
    if (!results) {
        return HK_STATUS_OUT_OF_MEMORY;
    }
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    if (!ledger) {
        free(results);
        return HK_STATUS_OUT_OF_MEMORY;
    }

    size_t keep = 0;
    for (size_t i = 0; i < n; i++) {
        struct hk_hook *hook = runtime->pending[i];
        hk_hook_result_t *out = &results[i];
        *out = hook->result;

        hk_prepare_diag_t diag;
        hk_prepare_result_t pr = hk_hook_dispatch_prepare(hook, &diag);
        hk_hook_validate_prepare_effects(hook, &pr, &diag);
        out->observed_prepare_effects = diag.observed_effects;
        if (diag.error_message || diag.error_code != 0) {
            out->error_code = diag.error_code;
            if (diag.error_message) {
                out->error_message.data = diag.error_message;
                out->error_message.length = strlen(diag.error_message);
            }
        }

        if (pr == HK_PREPARE_NOT_APPLICABLE) {
            // Still not here. Stays queued, stays PENDING -- the request said
            // to wait, and waiting is what it is doing.
            out->outcome = HK_OUTCOME_PENDING;
            out->retryable = true;
            out->currently_valid = false;
            hook->result = *out;
            runtime->pending[keep++] = hook;
            continue;
        }
        if (pr != HK_PREPARE_OK) {
            // Present but unhookable. Leaves the queue: waiting longer cannot
            // change a target that is already there and still refused.
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            out->retryable = false;
            hook->result = *out;
            hk_hook_free(hook);
            continue;
        }

        hk_ownership_lock();
        hk_ownership_state_t ownership;
        hk_ownership_status_t ownership_status =
            hk_ownership_lookup_locked(&hook->spec, &ownership);
        if (ownership_status == HK_OWNERSHIP_OUT_OF_MEMORY) {
            out->outcome = HK_OUTCOME_FAILED_UNKNOWN;
            out->mutation = HK_MUTATION_UNKNOWN;
            out->retryable = false;
            out->currently_valid = true;
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_OUT_OF_MEMORY;
            out->error_message.data = "could not inspect target ownership";
            out->error_message.length = strlen(out->error_message.data);
            hook->result = *out;
            hk_ownership_unlock();
            hk_hook_free(hook);
            continue;
        }
        hk_engine_capabilities_t caps = hook->matched_engine->describe();
        if (ownership.present &&
            (!(caps.chainable_target_kinds &
               HK_TARGET_KIND_BIT(hook->spec.target_kind)) ||
             !hk_engine_ids_equal(caps.engine_id, ownership.engine_id))) {
            hk_plan_set_ownership_error(
                out, HK_OUTCOME_CONFLICT,
                "existing target ownership is not chainable by this engine");
            hook->result = *out;
            hk_ownership_unlock();
            hk_hook_free(hook);
            continue;
        }

        hk_artifact_sink_t sink;
        memset(&sink, 0, sizeof(sink));
        sink.ledger = ledger;
        sink.plan_id = hook->result.plan_id;
        sink.runtime_owner_id = runtime->owner_id;
        sink.request_id = hook->hook_id;
        sink.require_predecessor_match = ownership.present;
        sink.required_predecessor = ownership.head_replacement;
        sink.published_original = NULL;
        sink.has_continuation = false;
        memset(&sink.continuation, 0, sizeof(sink.continuation));
        sink.static_continuation = false;
        sink.record_failed = false;
        sink.observed_effects = 0;
        size_t artifact_start = hk_artifact_ledger_count(ledger);

        hk_mutation_state_t mutation = hk_hook_dispatch_commit(hook, &sink);
        if (sink.record_failed && mutation == HK_MUTATION_COMPLETE) {
            mutation = HK_MUTATION_UNKNOWN;
        }
        if (mutation == HK_MUTATION_COMPLETE &&
            !hk_hook_validate_commit_effects(hook, &sink, out)) {
            mutation = HK_MUTATION_UNKNOWN;
        }
        if (mutation == HK_MUTATION_COMPLETE && ownership.present &&
            sink.published_original != ownership.head_replacement) {
            mutation = HK_MUTATION_UNKNOWN;
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_INVALID_STATE;
            out->error_message.data =
                "engine did not publish the owned predecessor";
            out->error_message.length = strlen(out->error_message.data);
        }
        if (sink.record_failed && mutation == HK_MUTATION_COMPLETE) {
            mutation = HK_MUTATION_UNKNOWN;
        }
        if (!hk_hook_validate_commit_effects(hook, &sink, out)) {
            mutation = HK_MUTATION_UNKNOWN;
        }
        out->mutation = mutation;
        if (sink.has_continuation) {
            out->continuation = sink.continuation;
        }
        out->observed_commit_effects = sink.observed_effects;
        out->artifact_count = hk_artifact_ledger_count(ledger) - artifact_start;
        if (out->artifact_count > 0) {
            out->matched_locations = (uint32_t)out->artifact_count;
            out->modified_locations = (mutation == HK_MUTATION_NONE) ? 0u
                                                                      : (uint32_t)out->artifact_count;
        }
        out->installed_generation = hk_image_catalog_generation(runtime->catalog);
        hk_hook_apply_verification(hook, ledger, artifact_start, &mutation, out,
                                   NULL);
        out->mutation = mutation;
        if (mutation == HK_MUTATION_COMPLETE &&
            !hk_ownership_record_locked(&hook->spec, caps.engine_id,
                                        hook->spec.replacement,
                                        sink.published_original)) {
            mutation = HK_MUTATION_UNKNOWN;
            out->mutation = mutation;
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_OUT_OF_MEMORY;
            out->error_message.data = "could not record target ownership";
            out->error_message.length = strlen(out->error_message.data);
        }
        hk_ownership_unlock();
        if (mutation == HK_MUTATION_COMPLETE) {
            out->outcome = HK_OUTCOME_ACTIVE;
            if (sink.published_original) {
                hk_id_t installed_id = hk_id_generate();
                out->installed_id = installed_id;
                out->original_available = true;
                hk_installed_hook_t *rec =
                    hk_installed_record_create(installed_id, sink.published_original, out);
                if (rec) {
                    hook->installed = rec;
                } else {
                    out->installed_id = (hk_id_t){0};
                    out->original_available = false;
                }
            }
        } else if (mutation == HK_MUTATION_PARTIAL) {
            out->outcome = HK_OUTCOME_FAILED_PARTIAL;
        } else if (mutation == HK_MUTATION_UNKNOWN) {
            out->outcome = HK_OUTCOME_FAILED_UNKNOWN;
        } else {
            out->outcome = HK_OUTCOME_FAILED_SAFE;
        }
        hook->result = *out;
        // Installed or failed, it is no longer waiting on anything.
        hk_hook_free(hook);
    }
    runtime->pending_count = keep;

    hk_report_t *report = hk_report_create(results, n);
    free(results);
    if (!report) {
        hk_artifact_ledger_destroy(ledger);
        return HK_STATUS_OUT_OF_MEMORY;
    }
    (void)hk_runtime_append_artifacts(runtime, ledger);
    (void)hk_artifact_process_append_ledger(ledger);
    hk_report_adopt_artifact_ledger(report, ledger);
    if (out_report) {
        *out_report = report;
    } else {
        hk_report_release(report);
    }
    return HK_STATUS_OK;
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

static size_t hk_plan_domain_index(const hk_plan_t *plan,
                                   const struct hk_domain *domain) {
    if (!plan || !domain) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < plan->domain_count; i++) {
        if (plan->domains[i] == domain) {
            return i;
        }
    }
    return SIZE_MAX;
}

// Preparation of a mandatory member is a domain gate, not an isolated hook
// result. If an earlier member already prepared and a later mandatory member
// then fails, release the earlier prepared state and refuse it too. No target
// has been mutated yet, so release_prepared is the existing safe cleanup seam.
static size_t hk_release_prepared_domain_members(
    const hk_plan_t *plan,
    const struct hk_domain *domain,
    hk_hook_result_t *results) {
    size_t released = 0;
    for (size_t i = 0; i < plan->hook_count; i++) {
        struct hk_hook *hook = plan->hooks[i];
        if (hook->spec.domain != domain ||
            hook->result.outcome != HK_OUTCOME_PREPARED) {
            continue;
        }

        hk_hook_release_prepared(hook);
        hk_hook_result_t *out = &results[i];
        *out = hook->result;
        out->outcome = HK_OUTCOME_FAILED_SAFE;
        out->mutation = HK_MUTATION_NONE;
        out->retryable = false;
        out->currently_valid = true;
        out->error_domain.data = "core";
        out->error_domain.length = 4;
        out->error_code = HK_STATUS_INVALID_STATE;
        out->error_message.data = "mandatory domain preparation failed";
        out->error_message.length = strlen(out->error_message.data);
        hook->result = *out;
        released++;
    }
    return released;
}

// Carries one engine preparation result into the public per-hook state. The
// grouped and one-hook paths must share this rollup: otherwise a new engine
// callback could quietly disagree with the lifecycle's pending/optional/
// failure accounting.
static void hk_plan_record_prepare_attempt(
    const hk_plan_t *plan,
    size_t hook_index,
    hk_prepare_result_t result,
    hk_prepare_diag_t *diag,
    hk_hook_result_t *results,
    const bool *domain_gate_ok,
    bool *domain_prepare_failed,
    size_t *prepared,
    size_t *failed)
{
    struct hk_hook *hook = plan->hooks[hook_index];
    hk_hook_result_t *out = &results[hook_index];

    hk_hook_validate_prepare_effects(hook, &result, diag);
    if (result != HK_PREPARE_OK) {
        // A failed grouped callback is not allowed to strand a value it
        // happened to allocate before returning false. The core owns every
        // successful preparation, regardless of which callback produced it.
        hk_hook_release_prepared(hook);
    }
    out->observed_prepare_effects = diag->observed_effects;

    if (diag->error_message || diag->error_code != 0) {
        out->error_code = diag->error_code;
        if (diag->error_message) {
            out->error_message.data = diag->error_message;
            out->error_message.length = strlen(diag->error_message);
        }
        hk_engine_capabilities_t caps = hook->matched_engine->describe();
        if (caps.engine_id) {
            out->error_domain.data = caps.engine_id;
            out->error_domain.length = strlen(caps.engine_id);
        }
    }

    if (result == HK_PREPARE_OK) {
        out->outcome = HK_OUTCOME_PREPARED;
        if (hook->has_prepared_continuation) {
            out->continuation = hook->prepared_continuation;
        }
        (*prepared)++;
    } else if (result == HK_PREPARE_NOT_APPLICABLE &&
               (hook->spec.target_kind == HK_TARGET_OBJC_METHOD
                    ? hook->spec.target.objc.availability
                    : hook->spec.availability) == HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE) {
        out->outcome = HK_OUTCOME_PENDING;
        out->retryable = true;
        out->currently_valid = false;
    } else if (result == HK_PREPARE_NOT_APPLICABLE) {
        // Optional absence is a satisfied request, represented by the only
        // non-failure outcome that says there is nothing to install.
        out->outcome = HK_OUTCOME_NO_ROUTE;
        out->retryable = true;
        out->currently_valid = false;
    } else {
        out->outcome = HK_OUTCOME_FAILED_SAFE;
        (*failed)++;
    }
    hook->result = *out;

    size_t domain_index = hk_plan_domain_index(plan, hook->spec.domain);
    if (result == HK_PREPARE_FAILED && domain_index != SIZE_MAX &&
        domain_gate_ok[domain_index] &&
        hook->spec.domain->spec.require_all_mandatory_prepared &&
        hook->spec.role == HK_OPERATION_MANDATORY) {
        domain_prepare_failed[domain_index] = true;
        size_t released = hk_release_prepared_domain_members(
            plan, hook->spec.domain, results);
        *prepared -= released;
        *failed += released;
    }
}

// Grouping is deliberately limited to ungated contiguous waves for now. A
// domain gate is a cross-hook atomic boundary; letting an optional callback
// bypass that boundary would be a semantic change, not an optimization. The
// fallback still handles those hooks one at a time.
static size_t hk_plan_prepare_group_count(const hk_plan_t *plan, size_t start) {
    if (!plan || start >= plan->hook_count) {
        return 0;
    }
    const struct hk_hook *first = plan->hooks[start];
    if (first->result.outcome != HK_OUTCOME_ANALYZED ||
        first->spec.domain || !first->matched_engine ||
        !first->matched_engine->prepare_group_ctx) {
        return 0;
    }
    hk_engine_capabilities_t caps = first->matched_engine->describe();
    if (!caps.native_grouping) {
        return 0;
    }
    const uint64_t generation =
        hk_image_catalog_generation(plan->runtime->catalog);
    size_t count = 1;
    for (size_t i = start + 1; i < plan->hook_count; i++) {
        const struct hk_hook *hook = plan->hooks[i];
        if (hook->result.outcome != HK_OUTCOME_ANALYZED ||
            hook->spec.domain ||
            hook->matched_engine != first->matched_engine ||
            hook->matched_engine_ctx != first->matched_engine_ctx ||
            hook->result.image_generation != generation) {
            break;
        }
        count++;
    }
    return count > 1 ? count : 0;
}

static hk_status_t hk_plan_prepare_group(
    const hk_plan_t *plan,
    size_t start,
    size_t count,
    hk_hook_result_t *results,
    size_t *attempted,
    size_t *prepared,
    size_t *failed)
{
    const struct hk_hook *first = plan->hooks[start];
    hk_engine_operation_t *operations = calloc(count, sizeof(*operations));
    if (!operations) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    for (size_t offset = 0; offset < count; offset++) {
        struct hk_hook *hook = plan->hooks[start + offset];
        results[start + offset] = hook->result;
        (*attempted)++;
        hk_hook_release_prepared(hook);
        operations[offset].spec = &hook->spec;
        operations[offset].prepare_result = HK_PREPARE_FAILED;
        operations[offset].prepared = NULL;
        memset(&operations[offset].prepare_diag, 0,
               sizeof(operations[offset].prepare_diag));
    }

    bool accepted = first->matched_engine->prepare_group_ctx(
        first->matched_engine_ctx, operations, count);
    for (size_t offset = 0; offset < count; offset++) {
        struct hk_hook *hook = plan->hooks[start + offset];
        if (!accepted) {
            operations[offset].prepare_result = HK_PREPARE_FAILED;
            operations[offset].prepared = NULL;
        }
        hook->prepared_state = operations[offset].prepared;
        if (operations[offset].prepare_result == HK_PREPARE_OK) {
            hk_prepare_result_t inspected =
                hk_hook_inspect_prepared_continuation(
                    hook, &operations[offset].prepare_diag);
            if (inspected != HK_PREPARE_OK) {
                operations[offset].prepare_result = inspected;
            }
        }
        operations[offset].prepared = hook->prepared_state;
        hk_plan_record_prepare_attempt(
            plan, start + offset, operations[offset].prepare_result,
            &operations[offset].prepare_diag, results, NULL, NULL,
            prepared, failed);
    }
    free(operations);
    return HK_STATUS_OK;
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
    bool *domain_prepare_failed = NULL;
    if (plan->domain_count > 0) {
        domain_gate_ok = (bool *)malloc(plan->domain_count * sizeof(bool));
        if (!domain_gate_ok) {
            free(results);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        domain_prepare_failed = (bool *)calloc(plan->domain_count, sizeof(bool));
        if (!domain_prepare_failed) {
            free(domain_gate_ok);
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

        size_t group_count = hk_plan_prepare_group_count(plan, i);
        if (group_count > 1) {
            hk_status_t group_status = hk_plan_prepare_group(
                plan, i, group_count, results, &attempted, &prepared, &failed);
            if (group_status != HK_STATUS_OK) {
                free(domain_gate_ok);
                free(domain_prepare_failed);
                free(results);
                return group_status;
            }
            i += group_count - 1;
            continue;
        }

        if (hook->result.image_generation !=
            hk_image_catalog_generation(plan->runtime->catalog)) {
            out->outcome = HK_OUTCOME_STALE_PLAN;
            out->retryable = true;
            out->currently_valid = false;
            failed++;
            hook->result = *out;
            size_t domain_index = hk_plan_domain_index(plan, hook->spec.domain);
            if (domain_index != SIZE_MAX && domain_gate_ok[domain_index] &&
                hook->spec.domain->spec.require_all_mandatory_prepared &&
                hook->spec.role == HK_OPERATION_MANDATORY) {
                domain_prepare_failed[domain_index] = true;
                size_t released = hk_release_prepared_domain_members(
                    plan, hook->spec.domain, results);
                prepared -= released;
                failed += released;
            }
            continue;
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

        size_t domain_index = hk_plan_domain_index(plan, hook->spec.domain);
        bool gate_blocked = domain_index != SIZE_MAX &&
            (!domain_gate_ok[domain_index] || domain_prepare_failed[domain_index]);
        if (gate_blocked) {
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            failed++;
            hook->result = *out;
            continue;
        }

        // Releases whatever a previous attempt produced, so the assignment
        // below cannot strand an earlier allocation. UNREACHABLE TODAY, and
        // said plainly rather than left to look load-bearing: the state
        // machine is one-way (analyze requires DRAFT, prepare requires
        // ANALYZED), so no hook can reach this loop twice. It is here because
        // it makes the assignment safe by construction rather than safe only
        // because a distant state check happens to forbid the second visit --
        // and a retry path would make it live.
        hk_hook_release_prepared(hook);

        hk_prepare_diag_t diag;
        hk_prepare_result_t result = hk_hook_dispatch_prepare(hook, &diag);
        hk_plan_record_prepare_attempt(
            plan, i, result, &diag, results, domain_gate_ok,
            domain_prepare_failed, &prepared, &failed);
    }
    free(domain_gate_ok);
    free(domain_prepare_failed);

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
static size_t hk_plan_hook_index(const hk_plan_t *plan,
                                 const struct hk_hook *needle) {
    for (size_t i = 0; i < plan->hook_count; i++) {
        if (plan->hooks[i] == needle) return i;
    }
    return plan->hook_count;
}

static bool hk_commit_dependencies_processed(const hk_plan_t *plan,
                                             const struct hk_hook *hook,
                                             const bool *processed) {
    for (size_t i = 0; i < hook->spec.commit_after_count; i++) {
        size_t dependency = hk_plan_hook_index(
            plan, (const struct hk_hook *)hook->spec.commit_after[i]);
        if (dependency == plan->hook_count || !processed[dependency]) {
            return false;
        }
    }
    return true;
}

static uint32_t hk_hook_domain_order(const struct hk_hook *hook) {
    return hook->spec.domain ? hook->spec.domain->spec.domain_order : 0;
}

static bool hk_commit_order_before(const struct hk_hook *left,
                                   size_t left_index,
                                   const struct hk_hook *right,
                                   size_t right_index) {
    uint32_t left_domain = hk_hook_domain_order(left);
    uint32_t right_domain = hk_hook_domain_order(right);
    if (left_domain != right_domain) return left_domain < right_domain;
    if (left->spec.commit_order != right->spec.commit_order) {
        return left->spec.commit_order < right->spec.commit_order;
    }
    return left_index < right_index;
}

// Produces a stable topological order for hooks that prepared. Dependencies
// outrank numeric domain/hook order; equal keys retain add order. A cycle is
// impossible through the public add path, but returning false keeps internal
// misuse safe.
static bool hk_build_commit_order(const hk_plan_t *plan,
                                  size_t **out_order,
                                  size_t *out_count) {
    *out_order = NULL;
    *out_count = 0;
    if (plan->hook_count == 0) return true;

    size_t *order = (size_t *)malloc(plan->hook_count * sizeof(*order));
    if (!order) {
        free(order);
        return false;
    }

    size_t prepared_count = 0;
    bool simple_order = true;
    bool have_order_key = false;
    uint32_t simple_domain_order = 0;
    uint32_t simple_commit_order = 0;
    for (size_t i = 0; i < plan->hook_count; i++) {
        struct hk_hook *hook = plan->hooks[i];
        if (hook->result.outcome == HK_OUTCOME_PREPARED) {
            prepared_count++;
            if (hook->spec.commit_after_count != 0) {
                simple_order = false;
            }
            uint32_t domain_order = hk_hook_domain_order(hook);
            if (!have_order_key) {
                simple_domain_order = domain_order;
                simple_commit_order = hook->spec.commit_order;
                have_order_key = true;
            } else if (domain_order != simple_domain_order ||
                       hook->spec.commit_order != simple_commit_order) {
                simple_order = false;
            }
        } else {
            // Non-prepared hooks are settled and never enter commit order.
        }
    }

    // The common case has no dependency edges and one ordering key. Add order
    // is already the required stable order, so avoid the general O(n^2)
    // topological selection below.
    if (simple_order) {
        size_t output = 0;
        for (size_t i = 0; i < plan->hook_count; i++) {
            if (plan->hooks[i]->result.outcome == HK_OUTCOME_PREPARED) {
                order[output++] = i;
            }
        }
        *out_order = order;
        *out_count = prepared_count;
        return true;
    }

    bool *processed = (bool *)calloc(plan->hook_count, sizeof(*processed));
    if (!processed) {
        free(order);
        return false;
    }
    for (size_t i = 0; i < plan->hook_count; i++) {
        if (plan->hooks[i]->result.outcome != HK_OUTCOME_PREPARED) {
            // A dependency that never prepared is settled; its dependent is
            // refused before dispatch rather than silently installed.
            processed[i] = true;
        }
    }

    for (size_t step = 0; step < prepared_count; step++) {
        size_t best = plan->hook_count;
        for (size_t i = 0; i < plan->hook_count; i++) {
            struct hk_hook *candidate = plan->hooks[i];
            if (processed[i] || candidate->result.outcome != HK_OUTCOME_PREPARED ||
                !hk_commit_dependencies_processed(plan, candidate, processed)) {
                continue;
            }
            if (best == plan->hook_count ||
                hk_commit_order_before(candidate, i, plan->hooks[best], best)) {
                best = i;
            }
        }
        if (best == plan->hook_count) {
            free(order);
            free(processed);
            return false;
        }
        processed[best] = true;
        order[step] = best;
    }

    free(processed);
    *out_order = order;
    *out_count = prepared_count;
    return true;
}

// A mandatory domain is an atomic preparation/revalidation boundary: before
// its first mutating primitive, every mandatory member must still be
// prepared, current, and dependency-ready. This is intentionally a small
// O(n^2) scan over a plan rather than another persistent index -- plans are
// short-lived and the index would buy complexity without buying correctness.
static bool hk_commit_dependency_ready_for_domain(
    const struct hk_hook *hook,
    const struct hk_domain *domain)
{
    for (size_t i = 0; i < hook->spec.commit_after_count; i++) {
        const struct hk_hook *dependency =
            (const struct hk_hook *)hook->spec.commit_after[i];
        if (dependency->result.outcome == HK_OUTCOME_ACTIVE) {
            continue;
        }
        // A prepared dependency in this same domain is valid at the domain
        // boundary; the topological commit order guarantees it is dispatched
        // before this dependent. A prepared dependency in another domain
        // must already be ACTIVE by the time this domain begins.
        if (dependency->spec.domain == domain &&
            dependency->result.outcome == HK_OUTCOME_PREPARED) {
            continue;
        }
        return false;
    }
    return true;
}

static bool hk_engine_ids_equal(const char *left, const char *right) {
    if (!left || !right) {
        return left == right;
    }
    return strcmp(left, right) == 0;
}

static void hk_plan_set_ownership_error(hk_hook_result_t *out,
                                        hk_outcome_t outcome,
                                        const char *message) {
    out->outcome = outcome;
    out->mutation = HK_MUTATION_NONE;
    out->retryable = false;
    out->currently_valid = true;
    out->error_domain.data = "core";
    out->error_domain.length = 4;
    out->error_code = HK_STATUS_INVALID_STATE;
    out->error_message.data = message;
    out->error_message.length = strlen(message);
}

static bool hk_domain_commit_preflight(const hk_plan_t *plan,
                                       const struct hk_domain *domain)
{
    const uint64_t generation = hk_image_catalog_generation(
        plan->runtime->catalog);
    for (size_t i = 0; i < plan->hook_count; i++) {
        const struct hk_hook *hook = plan->hooks[i];
        if (hook->spec.domain != domain ||
            hook->spec.role != HK_OPERATION_MANDATORY) {
            continue;
        }
        if (hook->result.outcome != HK_OUTCOME_PREPARED ||
            hook->result.image_generation != generation ||
            !hk_commit_dependency_ready_for_domain(hook, domain)) {
            return false;
        }
    }
    return true;
}

// Mark every prepared member of a domain before any member is dispatched.
// The mandatory member that caused the refusal keeps a specific stale result
// when possible; its prepared siblings receive a safe refusal instead of
// being allowed to mutate after the domain's invariant has failed.
static size_t hk_mark_domain_commit_preflight_failed(
    const hk_plan_t *plan,
    const struct hk_domain *domain,
    hk_hook_result_t *results)
{
    const uint64_t generation = hk_image_catalog_generation(
        plan->runtime->catalog);
    size_t marked = 0;
    for (size_t i = 0; i < plan->hook_count; i++) {
        struct hk_hook *hook = plan->hooks[i];
        if (hook->spec.domain != domain ||
            hook->result.outcome != HK_OUTCOME_PREPARED) {
            continue;
        }

        hk_hook_result_t *out = &results[i];
        *out = hook->result;
        out->mutation = HK_MUTATION_NONE;
        out->error_domain.data = "core";
        out->error_domain.length = 4;
        out->error_code = HK_STATUS_INVALID_STATE;
        if (hook->result.image_generation != generation) {
            out->outcome = HK_OUTCOME_STALE_PLAN;
            out->retryable = true;
            out->currently_valid = false;
            out->error_message.data = "mandatory domain member became stale";
        } else {
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            out->retryable = false;
            out->currently_valid = true;
            out->error_message.data =
                "mandatory domain preflight did not pass";
        }
        out->error_message.length = strlen(out->error_message.data);
        hook->result = *out;
        marked++;
    }
    return marked;
}

// Like preparation grouping, commit grouping is intentionally limited to a
// dependency-free, ungated wave. A dependency or domain is an ordering and
// atomicity boundary, so silently batching across either would change the
// lifecycle rather than merely reduce engine calls.
static size_t hk_plan_commit_group_count(const hk_plan_t *plan,
                                         const size_t *commit_order,
                                         size_t start,
                                         size_t commit_count) {
    if (!plan || !commit_order || start >= commit_count) {
        return 0;
    }
    const struct hk_hook *first = plan->hooks[commit_order[start]];
    if (first->result.outcome != HK_OUTCOME_PREPARED ||
        first->spec.domain || first->spec.commit_after_count != 0 ||
        !first->matched_engine || !first->matched_engine->commit_group_ctx) {
        return 0;
    }
    hk_engine_capabilities_t caps = first->matched_engine->describe();
    if (!caps.native_grouping) {
        return 0;
    }
    hk_ownership_lock();
    hk_ownership_state_t first_ownership;
    hk_ownership_status_t first_ownership_status =
        hk_ownership_lookup_locked(&first->spec, &first_ownership);
    hk_ownership_unlock();
    if (first_ownership_status != HK_OWNERSHIP_NO_RECORD) {
        return 0;
    }
    const uint64_t generation =
        hk_image_catalog_generation(plan->runtime->catalog);
    size_t count = 1;
    for (size_t offset = 1; start + offset < commit_count; offset++) {
        const struct hk_hook *hook =
            plan->hooks[commit_order[start + offset]];
        if (hook->result.outcome != HK_OUTCOME_PREPARED ||
            hook->spec.domain || hook->spec.commit_after_count != 0 ||
            hook->matched_engine != first->matched_engine ||
            hook->matched_engine_ctx != first->matched_engine_ctx ||
            hook->result.image_generation != generation) {
            break;
        }
        for (size_t previous = start; previous < start + offset; previous++) {
            bool same_target = false;
            if (hk_ownership_targets_equal(
                    &plan->hooks[commit_order[previous]]->spec,
                    &hook->spec, &same_target) != HK_OWNERSHIP_NO_RECORD ||
                same_target) {
                return count > 1 ? count : 0;
            }
        }
        hk_ownership_lock();
        hk_ownership_state_t ownership;
        hk_ownership_status_t ownership_status =
            hk_ownership_lookup_locked(&hook->spec, &ownership);
        hk_ownership_unlock();
        if (ownership_status == HK_OWNERSHIP_OUT_OF_MEMORY) {
            return 0;
        }
        if (ownership_status != HK_OWNERSHIP_NO_RECORD) {
            break;
        }
        count++;
    }
    return count > 1 ? count : 0;
}

static hk_status_t hk_plan_commit_group(
    const hk_plan_t *plan,
    const size_t *commit_order,
    size_t start,
    size_t count,
    hk_hook_result_t *results,
    hk_artifact_ledger_t *ledger,
    size_t *attempted,
    size_t *active,
    size_t *failed)
{
    const struct hk_hook *first = plan->hooks[commit_order[start]];
    hk_engine_operation_t *operations = calloc(count, sizeof(*operations));
    hk_artifact_sink_t *sinks = calloc(count, sizeof(*sinks));
    size_t *artifact_starts = calloc(count, sizeof(*artifact_starts));
    hk_artifact_ledger_t **operation_ledgers =
        calloc(count, sizeof(*operation_ledgers));
    if (!operations || !sinks || !artifact_starts || !operation_ledgers) {
        free(operation_ledgers);
        free(artifact_starts);
        free(sinks);
        free(operations);
        return HK_STATUS_OUT_OF_MEMORY;
    }

    for (size_t offset = 0; offset < count; offset++) {
        size_t index = commit_order[start + offset];
        struct hk_hook *hook = plan->hooks[index];
        hk_engine_operation_t *operation = &operations[offset];
        hk_artifact_sink_t *sink = &sinks[offset];
        results[index] = hook->result;
        (*attempted)++;

        operation_ledgers[offset] = hk_artifact_ledger_create();
        if (!operation_ledgers[offset]) {
            for (size_t cleanup = 0; cleanup < offset; cleanup++) {
                hk_artifact_ledger_destroy(operation_ledgers[cleanup]);
            }
            free(operation_ledgers);
            free(artifact_starts);
            free(sinks);
            free(operations);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        sink->ledger = operation_ledgers[offset];
        sink->plan_id = plan->plan_id;
        sink->runtime_owner_id = hk_runtime_owner_id(plan->runtime);
        sink->request_id = hook->hook_id;
        sink->published_original = NULL;
        sink->has_continuation = hook->has_prepared_continuation;
        sink->continuation = hook->prepared_continuation;
        sink->static_continuation = false;
        sink->record_failed = false;
        sink->observed_effects = 0;

        operation->spec = &hook->spec;
        operation->prepared = hook->prepared_state;
        operation->sink = sink;
        operation->mutation = HK_MUTATION_UNKNOWN;
        operation->verify_result = HK_VERIFY_FAILED;
        operation->revalidated =
            first->matched_engine->revalidate_group_ctx == NULL;
        operation->compensated = false;
    }

    // Keep the target lookup, grouped mutation, and successful head updates
    // under one process-wide guard. The selector above excludes known-owned
    // and duplicate targets; this lock closes the normal concurrent-commit
    // window between that check and the callback.
    hk_ownership_lock();
    bool revalidation_ok = true;
    if (first->matched_engine->revalidate_group_ctx) {
        revalidation_ok = first->matched_engine->revalidate_group_ctx(
            first->matched_engine_ctx, operations, count);
        for (size_t offset = 0; offset < count; offset++) {
            if (!operations[offset].revalidated) {
                revalidation_ok = false;
                break;
            }
        }
    }

    bool commit_ok = false;
    if (revalidation_ok) {
        commit_ok = first->matched_engine->commit_group_ctx(
            first->matched_engine_ctx, operations, count);
    }
    if (!revalidation_ok) {
        for (size_t offset = 0; offset < count; offset++) {
            operations[offset].mutation = HK_MUTATION_NONE;
            hk_hook_result_t *out =
                &results[commit_order[start + offset]];
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_INVALID_STATE;
            out->error_message.data = "group revalidation failed";
            out->error_message.length = strlen(out->error_message.data);
        }
    } else if (!commit_ok) {
        for (size_t offset = 0; offset < count; offset++) {
            operations[offset].mutation = HK_MUTATION_UNKNOWN;
            hk_hook_result_t *out =
                &results[commit_order[start + offset]];
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_INTERNAL_ERROR;
            out->error_message.data = "group commit callback failed";
            out->error_message.length = strlen(out->error_message.data);
        }
    }

    bool grouped_verification = false;
    if (commit_ok && first->matched_engine->verify_group_ctx) {
        grouped_verification = true;
        bool verify_ok = first->matched_engine->verify_group_ctx(
            first->matched_engine_ctx, operations, count);
        if (!verify_ok) {
            for (size_t offset = 0; offset < count; offset++) {
                operations[offset].verify_result = HK_VERIFY_FAILED;
                memset(&operations[offset].verify_diag, 0,
                       sizeof(operations[offset].verify_diag));
            }
        }
    }

    hk_engine_capabilities_t caps = first->matched_engine->describe();
    bool needs_compensation = false;
    if (commit_ok) {
        for (size_t offset = 0; offset < count; offset++) {
            hk_engine_operation_t *operation = &operations[offset];
            if (operation->mutation == HK_MUTATION_PARTIAL ||
                operation->mutation == HK_MUTATION_UNKNOWN ||
                (grouped_verification &&
                 operation->mutation == HK_MUTATION_COMPLETE &&
                 operation->verify_result != HK_VERIFY_OK)) {
                needs_compensation = true;
                break;
            }
        }
    } else if (revalidation_ok) {
        // A false grouped commit callback may have mutated an unknown subset;
        // give a certified reversible engine its one chance to restore it.
        needs_compensation = true;
    }
    if (needs_compensation && caps.supports_compensation &&
        first->matched_engine->compensate_group_ctx) {
        bool compensation_ok = first->matched_engine->compensate_group_ctx(
            first->matched_engine_ctx, operations, count);
        if (!compensation_ok) {
            for (size_t offset = 0; offset < count; offset++) {
                operations[offset].compensated = false;
            }
        }
    }

    for (size_t offset = 0; offset < count; offset++) {
        hk_engine_operation_t *operation = &operations[offset];
        hk_hook_result_t *out = &results[commit_order[start + offset]];
        if (operation->sink->record_failed &&
            operation->mutation == HK_MUTATION_COMPLETE) {
            operation->mutation = HK_MUTATION_UNKNOWN;
        }
        if (operation->mutation == HK_MUTATION_COMPLETE &&
            !hk_hook_validate_commit_effects(
                plan->hooks[commit_order[start + offset]],
                operation->sink, out)) {
            operation->mutation = HK_MUTATION_UNKNOWN;
        }
        if (operation->mutation == HK_MUTATION_COMPLETE &&
            !operation->compensated &&
            !hk_ownership_record_locked(
                &plan->hooks[commit_order[start + offset]]->spec,
                caps.engine_id,
                plan->hooks[commit_order[start + offset]]->spec.replacement,
                operation->sink->published_original)) {
            operation->mutation = HK_MUTATION_UNKNOWN;
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_OUT_OF_MEMORY;
            out->error_message.data = "could not record target ownership";
            out->error_message.length = strlen(out->error_message.data);
        }
    }
    hk_ownership_unlock();

    // Group callbacks receive one sink per operation. Keep those ranges
    // separate until the callback has finished so one hook's verification
    // cannot accidentally promote a sibling's artifacts.
    for (size_t offset = 0; offset < count; offset++) {
        artifact_starts[offset] = hk_artifact_ledger_count(ledger);
        if (!hk_artifact_ledger_append_ledger(
                ledger, operation_ledgers[offset])) {
            operations[offset].sink->record_failed = true;
        }
        if (operations[offset].sink->record_failed) {
            operations[offset].compensated = false;
        }
    }

    for (size_t offset = 0; offset < count; offset++) {
        size_t index = commit_order[start + offset];
        hk_plan_finish_commit_attempt(
            plan, plan->hooks[index], &results[index], ledger,
            artifact_starts[offset], operations[offset].mutation,
            operations[offset].sink,
            grouped_verification ? &operations[offset] : NULL,
            NULL, NULL,
            operations[offset].compensated,
            active, failed);
    }

    for (size_t offset = 0; offset < count; offset++) {
        if (operations[offset].compensated &&
            operations[offset].mutation != HK_MUTATION_NONE &&
            !operations[offset].sink->record_failed &&
            !hk_artifact_ledger_mark_compensated(
                ledger, artifact_starts[offset],
                hk_artifact_ledger_count(operation_ledgers[offset]))) {
            // The range was produced by this core-owned ledger, so this is
            // only reachable if the bookkeeping contract is violated.
            operations[offset].compensated = false;
        }
    }

    for (size_t offset = 0; offset < count; offset++) {
        hk_artifact_ledger_destroy(operation_ledgers[offset]);
    }
    free(operation_ledgers);
    free(artifact_starts);
    free(sinks);
    free(operations);
    return HK_STATUS_OK;
}

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
        // commit_order below only visits hooks that reached PREPARED --
        // anything else (NO_ROUTE, FAILED_SAFE from an earlier stage, ...)
        // is settled and never touched by that loop. Seed every slot from
        // the hook's current result up front so those stay their honest
        // pre-commit outcome instead of the malloc'd memory's leftovers.
        for (size_t i = 0; i < plan->hook_count; i++) {
            results[i] = plan->hooks[i]->result;
        }
    }

    // The artifact ledger engines record into during this commit. Built now
    // (before the report exists) and adopted into the report at the end.
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    if (!ledger) {
        free(results);
        return HK_STATUS_OUT_OF_MEMORY;
    }
    size_t *commit_order = NULL;
    size_t commit_count = 0;
    if (!hk_build_commit_order(plan, &commit_order, &commit_count)) {
        hk_artifact_ledger_destroy(ledger);
        free(results);
        return HK_STATUS_OUT_OF_MEMORY;
    }
    hk_artifact_sink_t sink;
    sink.ledger = ledger;
    sink.plan_id = plan->plan_id;
    sink.runtime_owner_id = hk_runtime_owner_id(plan->runtime);
    sink.request_id = (hk_id_t){0};    // set per hook below
    sink.published_original = NULL;    // reset per hook below
    sink.has_continuation = false;
    memset(&sink.continuation, 0, sizeof(sink.continuation));
    sink.static_continuation = false;
    sink.record_failed = false;
    sink.observed_effects = 0;

    size_t attempted = 0, active = 0, failed = 0;

    for (size_t order_index = 0; order_index < commit_count; order_index++) {
        size_t i = commit_order[order_index];
        struct hk_hook *hook = plan->hooks[i];
        hk_hook_result_t *out = &results[i];
        *out = hook->result;  // carry forward PREPARE-time outcome unless this hook is actually attempted below

        if (hook->result.outcome != HK_OUTCOME_PREPARED) {
            continue;  // never prepared -- nothing to commit
        }

        size_t group_count = hk_plan_commit_group_count(
            plan, commit_order, order_index, commit_count);
        if (group_count > 1) {
            hk_status_t group_status = hk_plan_commit_group(
                plan, commit_order, order_index, group_count, results,
                ledger, &attempted, &active, &failed);
            if (group_status != HK_STATUS_OK) {
                free(commit_order);
                hk_artifact_ledger_destroy(ledger);
                free(results);
                return group_status;
            }
            order_index += group_count - 1;
            continue;
        }

        if (hook->spec.domain) {
            bool domain_seen = false;
            for (size_t previous = 0; previous < order_index; previous++) {
                if (plan->hooks[commit_order[previous]]->spec.domain ==
                    hook->spec.domain) {
                    domain_seen = true;
                    break;
                }
            }
            if (!domain_seen && !hk_domain_commit_preflight(
                    plan, hook->spec.domain)) {
                size_t blocked = hk_mark_domain_commit_preflight_failed(
                    plan, hook->spec.domain, results);
                attempted += blocked;
                failed += blocked;
                continue;
            }
        }

        if (hook->result.image_generation !=
            hk_image_catalog_generation(plan->runtime->catalog)) {
            out->outcome = HK_OUTCOME_STALE_PLAN;
            out->retryable = true;
            out->currently_valid = false;
            failed++;
            hook->result = *out;
            continue;
        }
        attempted++;

        bool dependencies_active = true;
        for (size_t d = 0; d < hook->spec.commit_after_count; d++) {
            const struct hk_hook *dependency =
                (const struct hk_hook *)hook->spec.commit_after[d];
            if (dependency->result.outcome != HK_OUTCOME_ACTIVE) {
                dependencies_active = false;
                break;
            }
        }
        if (!dependencies_active) {
            out->outcome = HK_OUTCOME_FAILED_SAFE;
            out->mutation = HK_MUTATION_NONE;
            out->retryable = false;
            out->currently_valid = true;
            out->error_code = HK_STATUS_INVALID_STATE;
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_message.data = "commit dependency did not become active";
            out->error_message.length = strlen(out->error_message.data);
            failed++;
            hook->result = *out;
            continue;
        }

        hk_ownership_lock();
        hk_ownership_state_t ownership;
        hk_ownership_status_t ownership_status =
            hk_ownership_lookup_locked(&hook->spec, &ownership);
        if (ownership_status == HK_OWNERSHIP_OUT_OF_MEMORY) {
            hk_ownership_unlock();
            free(commit_order);
            hk_artifact_ledger_destroy(ledger);
            free(results);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        hk_engine_capabilities_t caps = hook->matched_engine->describe();
        const hk_target_kind_mask_t target_bit =
            HK_TARGET_KIND_BIT(hook->spec.target_kind);
        if (ownership.present &&
            (!(caps.chainable_target_kinds & target_bit) ||
             !hk_engine_ids_equal(caps.engine_id, ownership.engine_id))) {
            hk_plan_set_ownership_error(
                out, HK_OUTCOME_CONFLICT,
                "existing target ownership is not chainable by this engine");
            failed++;
            hook->result = *out;
            hk_ownership_unlock();
            continue;
        }

        sink.request_id = hook->hook_id;  // this hook is the artifact's originating request
        sink.published_original = NULL;   // each hook's engine publishes its own, or none
        sink.has_continuation = hook->has_prepared_continuation;
        sink.continuation = hook->prepared_continuation;
        sink.require_predecessor_match = ownership.present;
        sink.required_predecessor = ownership.head_replacement;
        sink.static_continuation = false;
        sink.record_failed = false;
        sink.observed_effects = 0;
        size_t artifact_start = hk_artifact_ledger_count(ledger);
        // Same preference as at prepare, and it has to be the same: an engine
        // whose prepare produced state must be the one handed it back.
        hk_mutation_state_t mutation = hk_hook_dispatch_commit(hook, &sink);
        if (sink.record_failed && mutation == HK_MUTATION_COMPLETE) {
            mutation = HK_MUTATION_UNKNOWN;
        }
        if (mutation == HK_MUTATION_COMPLETE &&
            !hk_hook_validate_commit_effects(hook, &sink, out)) {
            mutation = HK_MUTATION_UNKNOWN;
        }
        if (mutation == HK_MUTATION_COMPLETE && ownership.present &&
            sink.published_original != ownership.head_replacement) {
            mutation = HK_MUTATION_UNKNOWN;
            out->error_domain.data = "core";
            out->error_domain.length = 4;
            out->error_code = HK_STATUS_INVALID_STATE;
            out->error_message.data =
                "engine did not publish the owned predecessor";
            out->error_message.length = strlen(out->error_message.data);
        }
        hk_plan_finish_commit_attempt(plan, hook, out, ledger,
                                      artifact_start, mutation, &sink, NULL,
                                      &ownership, &caps,
                                      false,
                                      &active, &failed);
        hk_ownership_unlock();
    }

    free(commit_order);
    hk_report_t *report = hk_report_create(results, plan->hook_count);
    free(results);
    if (!report) {
        hk_artifact_ledger_destroy(ledger);  // report didn't take it; don't leak
        return HK_STATUS_OUT_OF_MEMORY;
    }
    (void)hk_runtime_append_artifacts(plan->runtime, ledger);
    (void)hk_artifact_process_append_ledger(ledger);
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
