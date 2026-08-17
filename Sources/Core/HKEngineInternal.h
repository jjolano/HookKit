// Internal engine contract -- Milestone 4's "fake engines" work, and a
// deliberately minimal subset of spec section 8's full hk_engine_vtable_t.
//
// What's here: enough for hk_plan_analyze to ask "does any registered
// engine claim it can serve this hook" and get a real answer instead of
// always HK_OUTCOME_NO_ROUTE. What's NOT here, on purpose: prepare_group/
// commit_group/revalidate_group/verify_group/compensate_group/
// inspect_continuation (section 8.1) -- those matter once
// hk_plan_prepare/commit exist to call them, which they don't yet. This
// header grows into the real contract incrementally, the same way
// HookKitResults.h grew from "just enough for hk_plan_analyze" to the
// full result struct across several commits -- not redesigned from
// scratch each time.
//
// Not public API: no engine outside this codebase registers against this
// contract. The production engines (Milestone 6+) are a fixed, compiled-in
// set, not something arbitrary callers extend -- this header exists for
// Sources/Core/*.c and Tests/Host/*.c (fake engines for testing) only.

#ifndef HK_CORE_ENGINE_INTERNAL_H
#define HK_CORE_ENGINE_INTERNAL_H

#include <stdint.h>

#include "../../Headers/HookKit/HookKitPlan.h"
#include "../../Headers/HookKit/HookKitResults.h"
#include "../../Headers/HookKit/HookKitTargets.h"

// One bit per hk_target_kind_t value -- lets a descriptor say "I handle
// symbol and address targets" without a variable-length list.
typedef uint32_t hk_target_kind_mask_t;
#define HK_TARGET_KIND_BIT(kind) (1u << (uint32_t)(kind))

typedef struct {
    const char *engine_id;
    hk_target_kind_mask_t target_kinds;
    // Upper bound of what this engine can ever achieve -- not a per-hook
    // fact, a fixed property of the engine (spec section 8.2's
    // "achieved reachability vector" as a static capability, not a result
    // field). Real engines' actual per-operation achieved_reach can be a
    // subset of this depending on the specific target; a fake engine used
    // for router testing doesn't need that nuance.
    hk_reachability_t achievable_reach;
} hk_engine_capabilities_t;

typedef struct hk_engine_vtable {
    hk_engine_capabilities_t (*describe)(void);
} hk_engine_vtable_t;

// Eligibility per spec section 9, minimal subset: target kind supported,
// and every required_reach bit is within the engine's achievable_reach.
// Explicitly NOT checked yet, and stated rather than silently assumed
// passing: image scope exactness, original requirement / continuation
// policy compatibility, forbidden preparation/commit effects, install
// context, architecture/OS, ownership conflicts, engine certification.
// Each of those needs a concept this rewrite hasn't built yet (the image
// catalog is Milestone 5; ownership ledger and engine certification are
// later Milestone 4/8/10 work) -- this function will grow more criteria
// as those land, the same incremental way the header above says the
// vtable will.
static inline bool hk_engine_eligible_minimal(
    const hk_engine_capabilities_t *caps,
    hk_target_kind_t target_kind,
    hk_reachability_t required_reach)
{
    if (!(caps->target_kinds & HK_TARGET_KIND_BIT(target_kind))) {
        return false;
    }
    return (required_reach & ~caps->achievable_reach) == 0;
}

// Defined in HKRuntime.c. `vtable` is not owned or copied -- the caller
// must keep it alive for the runtime's lifetime.
bool hk_runtime_register_engine_for_testing(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable);

#endif // HK_CORE_ENGINE_INTERNAL_H
