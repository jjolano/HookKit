// HookKit 3.0 -- plans, domains, hook requests, and the functions that
// drive a plan through analyze/prepare/commit. See
// docs/3.0/PUBLIC_C_ABI.md and docs/3.0/PLAN_LIFECYCLE.md (pending).

#ifndef HOOKKIT_PLAN_H
#define HOOKKIT_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HookKitBase.h"
#include "HookKitRuntime.h"
#include "HookKitTargets.h"
#include "HookKitResults.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_plan hk_plan_t;
typedef struct hk_domain hk_domain_t;
typedef struct hk_hook hk_hook_t;
typedef struct hk_installed_hook hk_installed_hook_t;
typedef struct hk_original_slot hk_original_slot_t;

// Real gap in the master spec's text (see HookKitRuntime.h's header
// comment): hk_plan_config_t is referenced by hk_plan_create but never
// defined. Minimal, honest fill: nothing a plan strictly needs beyond its
// owning runtime exists in the spec's own requirements, so this carries
// only an optional diagnostic label -- extend when a real requirement
// shows up, not speculatively now.
typedef struct {
    HK_STRUCT_HEADER;

    const char *debug_label;  // optional; appears in diagnostics/reports naming this plan
} hk_plan_config_t;

typedef enum {
    HK_OPERATION_OPTIONAL = 0,
    HK_OPERATION_MANDATORY,
} hk_operation_role_t;

typedef enum {
    HK_COMPENSATION_NONE = 0,
    HK_COMPENSATION_BEST_EFFORT_REVERSIBLE_MEMBERS,
    HK_COMPENSATION_REQUIRE_REVERSIBLE_PREFIX,
} hk_compensation_policy_t;

typedef struct {
    HK_STRUCT_HEADER;

    const char *stable_domain_id;
    uint32_t domain_order;

    bool require_all_mandatory_prepared;
    bool prefer_reversible_before_irreversible;

    hk_compensation_policy_t compensation_policy;
} hk_domain_spec_t;

// All request-owned data (target strings/bytes, stable_hook_id) is
// deep-copied when the hook is added to the plan -- the caller's buffers
// need not outlive hk_plan_add_hook. stable_hook_id and target identity are
// each validated unique within the plan at add time. Chain a target through
// separately committed plans, never sibling hooks in one plan.
typedef struct {
    HK_STRUCT_HEADER;

    const char *stable_hook_id;

    hk_target_kind_t target_kind;
    hk_target_spec_t target;

    void *replacement;

    hk_reachability_t required_reach;
    hk_reachability_t preferred_reach;

    hk_original_requirement_t original_requirement;
    hk_continuation_policy_t continuation_policy;

    hk_constraints_t constraints;
    hk_availability_t availability;

    hk_operation_role_t role;
    hk_domain_t *domain;

    uint32_t commit_order;

    const hk_hook_t *const *commit_after;
    size_t commit_after_count;
} hk_hook_spec_t;

// Only HK_PLAN_DRAFT accepts new domains or hooks. Request objects are
// meant to be treated as immutable once added -- changing one invalidates
// prior analysis/preparation rather than mutating in place.
typedef enum {
    HK_PLAN_DRAFT = 0,
    HK_PLAN_ANALYZED,
    HK_PLAN_PREPARING,
    HK_PLAN_PREPARED,
    HK_PLAN_COMMITTING,
    HK_PLAN_COMMITTED,
    HK_PLAN_PARTIAL,
    HK_PLAN_FAILED,
    HK_PLAN_INVALIDATED,
    HK_PLAN_DISCARDED,
} hk_plan_state_t;

hk_status_t hk_plan_create(
    hk_runtime_t *runtime,
    const hk_plan_config_t *config,
    hk_plan_t **out_plan);

void hk_plan_release(hk_plan_t *plan);

hk_status_t hk_plan_define_domain(
    hk_plan_t *plan,
    const hk_domain_spec_t *spec,
    hk_domain_t **out_domain);

hk_status_t hk_plan_add_hook(
    hk_plan_t *plan,
    const hk_hook_spec_t *spec,
    hk_hook_t **out_hook);

// Side-effect-free (docs/3.0/ARCHITECTURE.md invariant #1) -- ordinary heap
// allocation and read-only inspection only.
hk_status_t hk_plan_analyze(
    hk_plan_t *plan,
    hk_report_t **out_report);

// Never mutates a requested target (invariant #2) -- only request-permitted
// non-target effects (provider activation, continuation allocation, etc.).
hk_status_t hk_plan_prepare(
    hk_plan_t *plan,
    hk_report_t **out_report);

// Revalidates before every write (invariant #3); never falls back to
// another route after HK_MUTATION_PARTIAL or HK_MUTATION_UNKNOWN
// (invariant #4).
hk_status_t hk_plan_commit(
    hk_plan_t *plan,
    hk_report_t **out_report);

hk_plan_state_t hk_plan_state(const hk_plan_t *plan);

hk_status_t hk_hook_copy_result(
    const hk_hook_t *hook,
    hk_hook_result_t *out_result);

const hk_original_slot_t *hk_hook_original_slot(const hk_hook_t *hook);
void *hk_original_slot_load(const hk_original_slot_t *slot);

const hk_installed_hook_t *hk_hook_installed_handle(const hk_hook_t *hook);

hk_status_t hk_installed_hook_copy_result(
    const hk_installed_hook_t *installed,
    hk_hook_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_PLAN_H
