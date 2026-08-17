// hk_plan_t lifecycle and domain registration. Milestone 4 continues:
// hk_plan_add_hook, hk_plan_analyze/prepare/commit, and everything past
// DRAFT state are not implemented yet -- this slice is create/release/
// state plus hk_plan_define_domain, done correctly rather than everything
// done partially. See HKPlanInternal.h for why domains are individually
// heap-allocated rather than stored inline in a growable array.

#include "HKIDs.h"
#include "HKPlanInternal.h"
#include "HKRuntimeInternal.h"

#include <stdlib.h>
#include <string.h>

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
