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

// One bit per hk_original_requirement_t value, so an engine can say which
// kinds of "original" it is able to hand back.
typedef uint32_t hk_original_requirement_mask_t;
#define HK_ORIGINAL_REQ_BIT(req) (1u << (uint32_t)(req))
#define HK_ORIGINAL_REQ_ALL (HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE) | \
                             HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_DIRECT_PREDECESSOR) | \
                             HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_CALLABLE_CONTINUATION))

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

    // Which hk_original_requirement_t values this engine can satisfy, as a
    // bitmask (HK_ORIGINAL_REQ_BIT). Zero means "declares nothing", which is
    // treated as "any" -- see hk_engine_eligible_minimal for why that default
    // is the safe one rather than the lazy one.
    //
    // This exists because two engines now differ ONLY on this axis: terminal
    // inline destroys the prologue and can serve HK_ORIGINAL_NONE alone, while
    // relocating inline preserves it in a trampoline and can serve all three.
    // They otherwise describe themselves identically (function-address target,
    // entry-point reach), so without this the first registered wins and the
    // other's capability is unreachable.
    hk_original_requirement_mask_t original_requirements;

    // What this engine may produce when it commits (hk_effects_t bits). Zero
    // means "declares nothing", read as "declares no forbidden effect" -- the
    // same safe default as original_requirements, so an engine written before
    // this field existed stays eligible exactly as it was.
    //
    // Checked against the request's hk_constraints_t. Without this, a caller
    // saying HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY got an executable page
    // allocated anyway: nothing in Sources/ read `constraints` at all.
    hk_effects_t commit_effects;
} hk_engine_capabilities_t;

// Which HK_FORBID_* bit bans a given effect.
//
// This CANNOT be a mask-and-compare, and that is the whole reason it is a
// function. The two enums agree on bits 0-3 and then diverge: effect bit 4 is
// HK_EFFECT_MEMORY_MUTATION while forbid bit 4 is
// HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY. ANDing them would silently cross-wire
// those two -- a memory-patch engine rejected by a caller who forbade
// executable allocation, and vice versa. The mapping is written out instead.
//
// Effects with NO corresponding forbid bit -- MEMORY_MUTATION, IMAGE_LOAD,
// FILE_MAPPING, MEMORY_PROTECTION_CHANGE -- return 0 and are therefore never
// forbidden. That is a gap in the spec's constraint vocabulary, not an
// oversight here: there is no way for a caller to express "do not patch
// memory", so this cannot honour one.
static inline hk_constraints_t hk_effect_forbid_bit(hk_effects_t effect) {
    switch (effect) {
        case HK_EFFECT_TARGET_TEXT_MUTATION:    return HK_FORBID_TARGET_TEXT_PATCH;
        case HK_EFFECT_IMPORT_MUTATION:         return HK_FORBID_IMPORT_SLOT_PATCH;
        case HK_EFFECT_OBJC_METADATA_MUTATION:  return HK_FORBID_OBJC_METADATA_CHANGE;
        case HK_EFFECT_SWIFT_VTABLE_MUTATION:   return HK_FORBID_SWIFT_VTABLE_CHANGE;
        case HK_EFFECT_EXECUTABLE_ALLOCATION:   return HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY;
        case HK_EFFECT_STATIC_CONTINUATION_USE: return HK_FORBID_STATIC_CONTINUATION;
        case HK_EFFECT_PRIVATE_SYMBOL_SCAN:     return HK_FORBID_PRIVATE_SYMBOL_SCAN;
        case HK_EFFECT_PROVIDER_ACTIVATION:     return HK_FORBID_PROVIDER_ACTIVATION;
        case HK_EFFECT_PROVIDER_IMAGE_LOAD:     return HK_FORBID_PROVIDER_IMAGE_LOAD;
        case HK_EFFECT_CALLBACK_REGISTRATION:   return HK_FORBID_CALLBACK_REGISTRATION;
        case HK_EFFECT_THREAD_CREATION:         return HK_FORBID_THREAD_CREATION;
        case HK_EFFECT_UNKNOWN_PROCESS_MUTATION:return HK_FORBID_UNKNOWN_COMMIT_EFFECTS;
        default:                                return 0;
    }
}

// The effects a request forbids, from its explicit constraints AND from its
// continuation policy. The policy is not redundant with the constraints:
// HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY says the same thing as
// HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY, and a caller who set only the policy
// meant it just as much.
static inline hk_constraints_t hk_effective_constraints(
    hk_constraints_t constraints,
    hk_continuation_policy_t continuation_policy)
{
    if (continuation_policy == HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY) {
        constraints |= HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY;
    }
    return constraints;
}

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
static inline bool hk_engine_eligible_minimal_full(
    const hk_engine_capabilities_t *caps,
    hk_target_kind_t target_kind,
    hk_reachability_t required_reach,
    hk_original_requirement_t original_requirement,
    hk_constraints_t effective_constraints)
{
    if (!(caps->target_kinds & HK_TARGET_KIND_BIT(target_kind))) {
        return false;
    }
    if ((required_reach & ~caps->achievable_reach) != 0) {
        return false;
    }
    // Zero means the engine declared nothing, which is read as "any" rather
    // than "none". That is deliberate and it is the SAFE default, not the lazy
    // one: an engine that says nothing about originals behaves exactly as it
    // did before this field existed, so adding the field cannot silently make
    // a previously-eligible engine ineligible. An engine that genuinely cannot
    // serve a requirement says so, and is then correctly skipped.
    if (caps->original_requirements != 0 &&
        !(caps->original_requirements & HK_ORIGINAL_REQ_BIT(original_requirement))) {
        return false;
    }
    // An engine that would produce a forbidden effect is not eligible. Checked
    // bit by bit through the mapping above rather than by masking, since the
    // two enums do not share a layout.
    if (effective_constraints != 0 && caps->commit_effects != 0) {
        for (unsigned bit = 0; bit < 64; bit++) {
            hk_effects_t effect = 1ull << bit;
            if (!(caps->commit_effects & effect)) {
                continue;
            }
            hk_constraints_t forbid = hk_effect_forbid_bit(effect);
            if (forbid != 0 && (effective_constraints & forbid) != 0) {
                return false;
            }
        }
    }
    return true;
}

// Kept as the two-criterion form for callers that have no requirement in hand.
static inline bool hk_engine_eligible_minimal(
    const hk_engine_capabilities_t *caps,
    hk_target_kind_t target_kind,
    hk_reachability_t required_reach)
{
    return hk_engine_eligible_minimal_full(caps, target_kind, required_reach,
                                           HK_ORIGINAL_NONE, 0);
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
