// Internal engine contract -- Milestone 4's "fake engines" work, and a
// deliberately minimal subset of spec section 8's full hk_engine_vtable_t.
//
// What's here: enough for hk_plan_analyze to ask "does any registered
// engine claim it can serve this hook" (describe()) and enough for
// hk_plan_prepare to actually attempt preparation (prepare_one()). What's
// NOT here, on purpose: prepare_GROUP/commit_group/revalidate_group/
// verify_group/compensate_group/inspect_continuation (section 8.1) use
// *grouped* operations for batching (one image walk covering many hooks at
// once) -- real engines will need that (Milestone 6+), but no fake engine
// in this codebase's tests has anything to batch, so prepare_one takes one
// hook at a time. This header grows into the real contract incrementally,
// the same way HookKitResults.h grew from "just enough for hk_plan_analyze"
// to the full result struct across several commits -- not redesigned from
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
#include "HKArtifactLedger.h"  // hk_artifact_sink_t

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

// What a preparation attempt actually concluded. See prepare_one_ctx_status.
typedef enum {
    HK_PREPARE_OK = 0,
    // Could not prepare. Nothing was reserved and nothing was touched -- the
    // bool contract's `false`, and what becomes HK_OUTCOME_FAILED_SAFE.
    HK_PREPARE_FAILED,
    // Correctly nothing to do: the request was conditional
    // (HK_AVAILABILITY_OPTIONAL_IF_PRESENT) and its target is absent, so it
    // is SATISFIED. Must not count toward a plan's failures.
    HK_PREPARE_NOT_APPLICABLE,
} hk_prepare_result_t;

// Optional detail an engine may attach to a preparation result.
// `error_message` must have static lifetime -- it is carried, not copied.
typedef struct {
    int64_t error_code;         // engine-defined; 0 when none
    const char *error_message;  // NULL when none
} hk_prepare_diag_t;

typedef struct hk_engine_vtable {
    hk_engine_capabilities_t (*describe)(void);

    // Attempts preparation for one hook (spec section 4.2: request-
    // permitted non-target effects only, never a target mutation -- a
    // fake engine honors this by construction since it has no target to
    // touch in the first place). Returns true on success. False means
    // preparation failed cleanly before anything was reserved -- there is
    // no partial-preparation concept at this minimal a contract, so every
    // failure here is the equivalent of HK_OUTCOME_FAILED_SAFE, never
    // FAILED_PARTIAL/FAILED_UNKNOWN.
    bool (*prepare_one)(const hk_hook_spec_t *spec);

    // Attempts commit for one already-prepared hook. Returns the real
    // mutation state (spec section 6.27/4.4) -- HK_MUTATION_NONE means
    // commit was refused before touching anything (a clean failure, the
    // commit-time analogue of prepare_one returning false).
    // HK_MUTATION_COMPLETE/PARTIAL/UNKNOWN are the engine's honest report
    // of what actually happened; a fake engine used for router/plan
    // testing only ever needs to return NONE or COMPLETE (it has no real
    // target to partially mutate), but the type is hk_mutation_state_t,
    // not bool, so a future real engine's honest PARTIAL/UNKNOWN reports
    // fit this same contract without a signature change.
    //
    // On any mutation other than NONE, the engine records what it produced
    // into `sink` (hk_artifact_sink_record) -- one call per artifact. The
    // engine fills only the mechanism facts; the sink stamps the contextual
    // IDs (see HKArtifactLedger.h). `sink` is never NULL when commit_one is
    // called. An engine that refuses (returns NONE) records nothing.
    hk_mutation_state_t (*commit_one)(const hk_hook_spec_t *spec,
                                      hk_artifact_sink_t *sink);

    // ---- context-carrying variants (optional, additive) ----------------
    //
    // The pair above is context-free, which forces any engine with real
    // prepared state into two workarounds: a file-scoped environment
    // (so only ONE can be configured at a time) and a side stash keyed by
    // stable_hook_id (so prepare can hand something to commit). All three
    // Milestone 6 adapters did exactly that and each said so in its header.
    // These variants retire both: the engine gets the context it was
    // registered with, and whatever prepare produced comes straight back at
    // commit.
    //
    // Purely additive. Existing engines use designated initializers, which
    // zero-fill omitted members, so every engine written before these
    // existed keeps working unchanged with these left NULL -- and the core
    // already NULL-checks before calling. An engine implements EITHER the
    // context-free pair OR this one; when both are present the core prefers
    // these, since an engine that filled them meant them.
    //
    // Register with hk_runtime_register_engine_with_context to supply the
    // context; registering without one passes NULL, which is fine for an
    // engine whose context is genuinely nothing.

    // `out_prepared` receives whatever the engine wants handed back at
    // commit -- ownership passes to the caller, which guarantees exactly one
    // release_prepared call for every true return, whether or not commit
    // ever runs. Returning false must leave *out_prepared untouched.
    bool (*prepare_one_ctx)(void *engine_ctx, const hk_hook_spec_t *spec,
                            void **out_prepared);

    // `prepared` is exactly what prepare_one_ctx produced for THIS hook.
    // Does not release it; the core does that.
    hk_mutation_state_t (*commit_one_ctx)(void *engine_ctx,
                                          const hk_hook_spec_t *spec,
                                          void *prepared,
                                          hk_artifact_sink_t *sink);

    // Releases prepared state. Called exactly once per successful
    // prepare_one_ctx -- on plan release, on re-preparation, and after a
    // commit alike. May be NULL when the state needs no cleanup (an engine
    // that hands back a small integer cast to void*, say).
    void (*release_prepared)(void *engine_ctx, void *prepared);

    // ---- richer preparation result (optional, additive) ----------------
    //
    // Both preparation entry points above return bool, which conflates two
    // genuinely different things:
    //
    //   "I could not prepare this"        -- a failure
    //   "there is correctly nothing here" -- a SATISFIED request
    //
    // The second is what HK_AVAILABILITY_OPTIONAL_IF_PRESENT means: hook it
    // if it exists, and if it does not, that is the requested behavior. Under
    // the bool contract it lands in the same `false` as a real failure, which
    // is not merely a labelling problem -- hk_plan_prepare counts every false
    // toward `failed`, and any failure puts the whole plan in HK_PLAN_FAILED.
    // So one absent optional target currently fails a plan that did exactly
    // what it was asked.
    //
    // It also flattens every distinct refusal an engine can make (the inline
    // engine alone has four) into one undifferentiated failure with no reason
    // attached.
    //
    // Purely additive, same as the context entry points: an engine that does
    // not implement this keeps working unchanged, and the core prefers this
    // one when present because an engine that filled it meant it.

    // Prepared state is handed back the same way prepare_one_ctx does, with
    // the same ownership rule: exactly one release_prepared per HK_PREPARE_OK.
    // Anything other than HK_PREPARE_OK must leave *out_prepared untouched.
    //
    // `out_diag` is never NULL and starts zeroed. An engine may fill
    // error_code and error_message to say WHY; error_message must be a string
    // with static lifetime (a literal), since it is carried in the result
    // without being copied. The core fills error_domain from the engine's own
    // engine_id, so engines do not repeat it.
    hk_prepare_result_t (*prepare_one_ctx_status)(void *engine_ctx,
                                                  const hk_hook_spec_t *spec,
                                                  void **out_prepared,
                                                  hk_prepare_diag_t *out_diag);
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

// Same, plus the context handed to the vtable's *_ctx entry points. Neither
// `vtable` nor `engine_ctx` is owned or copied; both must outlive the
// runtime. Registering without a context is the same as passing NULL here.
bool hk_runtime_register_engine_with_context(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable,
    void *engine_ctx);

#endif // HK_CORE_ENGINE_INTERNAL_H
