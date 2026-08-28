// Internal engine contract -- Milestone 4's fake-engine seam plus the
// versioned discovery/analysis/continuation extensions from spec section 8.
//
// What's here: a versioned descriptor, optional side-effect-free discovery and
// request analysis, and enough for hk_plan_prepare/commit to drive one hook or
// a native grouped wave through preparation, revalidation, mutation,
// verification, continuation inspection, and certified compensation. The
// legacy zero-header form remains valid for the in-tree fake engines.
//
// Not public API: no engine outside this codebase registers against this
// contract. The production engines (Milestone 6+) are a fixed, compiled-in
// set, not something arbitrary callers extend -- this header exists for
// Sources/Core/*.c and Tests/Host/*.c (fake engines for testing) only.

#ifndef HK_CORE_ENGINE_INTERNAL_H
#define HK_CORE_ENGINE_INTERNAL_H

#include <stddef.h>
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

// One bit per hk_install_context_t value. Zero means the engine did not
// declare a context restriction and remains eligible for every runtime
// context, preserving the additive behavior of older descriptors.
typedef uint32_t hk_install_context_mask_t;
#define HK_INSTALL_CONTEXT_BIT(context) (1u << (uint32_t)(context))
#define HK_INSTALL_CONTEXT_ALL (HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_EARLY_PROCESS) | \
                                HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED) | \
                                HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_RUNTIME_SERIALIZED) | \
                                HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_ARBITRARY_RUNTIME))

// Production routing is architecture-specific. This mask records the accepted
// release lane, not proof of a physical-device run; evidence records carry
// that separate certification status. Zero remains reserved for host fakes.
typedef uint32_t hk_engine_architecture_mask_t;
#define HK_ENGINE_ARCHITECTURE_ARM64  (1u << 0)
#define HK_ENGINE_ARCHITECTURE_ARM64E (1u << 1)
#define HK_ENGINE_ARCHITECTURE_ARMV7  (1u << 2)
#define HK_ENGINE_ARCHITECTURE_ARMV7S (1u << 3)

// Matches Apple's __IPHONE_OS_VERSION_* integer encoding (15.0.0 == 150000).
#define HK_ENGINE_IOS_VERSION(major, minor, patch) \
    ((uint32_t)(major) * 10000u + (uint32_t)(minor) * 100u + (uint32_t)(patch))

typedef struct {
    const char *engine_id;
    hk_target_kind_mask_t target_kinds;
    // Target kinds for which the engine can enforce the request's image
    // selector when a populated catalog is available. This is narrower than
    // target_kinds because a symbol rebind, for example, can scope importer
    // slots without proving the defining image. Zero means no target-kind
    // claim, not "all".
    hk_target_kind_mask_t exact_image_scope_targets;
    hk_install_context_mask_t install_contexts;
    // Architectures this engine was built to handle, and the subset accepted
    // for production routing. Device certification is tracked separately.
    hk_engine_architecture_mask_t architectures;
    hk_engine_architecture_mask_t certified_architectures;
    // Minimum iOS version for this engine mode. A zero value is reserved for
    // host-only fakes; production descriptors state their package floor.
    uint32_t minimum_ios_version;
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

    // What this engine may produce during preparation (hk_effects_t bits).
    // Zero means "declares nothing", read as "declares no forbidden effect"
    // -- the same safe default as original_requirements, so an engine written
    // before this field existed stays eligible exactly as it was.
    hk_effects_t prepare_effects;

    // What this engine may produce when it commits (hk_effects_t bits). Zero
    // means "declares nothing", read as "declares no forbidden effect" -- the
    // same safe default as original_requirements, so an engine written before
    // this field existed stays eligible exactly as it was.
    //
    // Checked against the request's hk_constraints_t. Without this, a caller
    // saying HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY got an executable page
    // allocated anyway: nothing in Sources/ read `constraints` at all.
    hk_effects_t commit_effects;

    // The engine can consume several same-engine operations in one phase.
    // This is a capability declaration, not a promise that every call will
    // batch: the vtable callback may still be absent for a legacy adapter.
    bool native_grouping;
    bool supports_compensation;

    // Target kinds for which an existing replacement can be installed on
    // top of the current head when the engine validates the predecessor
    // witness in hk_artifact_sink_t. Zero means no chaining claim.
    hk_target_kind_mask_t chainable_target_kinds;
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
    hk_effects_t observed_effects; // effects actually produced while preparing
} hk_prepare_diag_t;

// Verification is deliberately separate from mutation state: COMPLETE says
// the engine performed its write, while VERIFIED requires a post-write read
// through the engine's own mechanism. An omitted callback is an honest
// unverified result; a failed callback upgrades COMPLETE to UNKNOWN in the
// plan lifecycle because the installed state can no longer be trusted.
typedef enum {
    HK_VERIFY_UNAVAILABLE = 0,
    HK_VERIFY_OK,
    HK_VERIFY_FAILED,
} hk_verify_result_t;

typedef struct {
    int64_t error_code;         // engine-defined; 0 when none
    const char *error_message;  // NULL when none
} hk_verify_diag_t;

// One operation in an optional grouped engine call. The core owns the
// operation array and every `prepared` value after a successful preparation;
// engines fill only the phase result fields. A callback returning false means
// it could not produce trustworthy per-operation results, so the core keeps
// the conservative defaults (FAILED for prepare, UNKNOWN for commit, and
// FAILED for verification).
typedef struct {
    const hk_hook_spec_t *spec;
    void *prepared;

    hk_prepare_result_t prepare_result;
    hk_prepare_diag_t prepare_diag;

    hk_artifact_sink_t *sink;
    hk_mutation_state_t mutation;

    hk_verify_result_t verify_result;
    hk_verify_diag_t verify_diag;

    bool revalidated;
    bool compensated;
} hk_engine_operation_t;

// Optional versioned discovery/analysis results. The fixed compiled-in
// engines can use the descriptor-only path, while a provider-backed engine
// may add a side-effect-free availability probe and a request-specific route
// refinement without changing the core callback ABI.
typedef struct {
    bool available;
    int64_t error_code;
    const char *error_message; // static lifetime, like engine diagnostics
} hk_engine_discovery_t;

typedef struct {
    bool eligible;
    hk_reachability_t achieved_reach;
    hk_effects_t required_prepare_effects;
    hk_effects_t required_commit_effects;
    hk_continuation_kind_t continuation_kind;
    hk_install_context_t install_context;
    uint32_t route_rank;
} hk_engine_analysis_t;

typedef struct {
    hk_prepare_result_t prepare_result;
    hk_mutation_state_t mutation;
    hk_verify_result_t verify_result;
    bool compensated;
    hk_prepare_diag_t prepare_diag;
    hk_verify_diag_t verify_diag;
} hk_engine_attempt_t;

// The vtable is internal, but it still crosses the core/engine seam. Keep a
// small version header so a newer core can reject an explicitly versioned
// engine it cannot understand while retaining the zero-header form used by
// the original test seam.
#define HK_ENGINE_VTABLE_ABI_VERSION_1 1u

typedef struct hk_engine_vtable {
    uint32_t abi_version; // 0: legacy in-tree shape; otherwise a known version
    uint32_t struct_size; // 0: legacy in-tree shape; otherwise sizeof this vtable

    hk_engine_capabilities_t (*describe)(void);

    // Optional versioned discovery and request analysis. Returning false (or
    // `available`/`eligible` false) removes the candidate without touching a
    // target; the descriptor remains the conservative upper bound.
    bool (*discover)(void *engine_ctx, hk_engine_discovery_t *out);
    bool (*analyze_operation)(void *engine_ctx,
                              const hk_hook_spec_t *request,
                              hk_engine_analysis_t *out);

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

    // Optional post-commit readback for context-free engines. Returning
    // HK_VERIFY_UNAVAILABLE is equivalent to leaving this callback NULL.
    hk_verify_result_t (*verify_one)(const hk_hook_spec_t *spec,
                                     hk_verify_diag_t *out_diag);

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

    // Optional post-commit readback. `prepared` is still owned by the core
    // when verification runs, so engines may use the captured address/plan
    // rather than resolving the target a second time.
    hk_verify_result_t (*verify_one_ctx)(void *engine_ctx,
                                         const hk_hook_spec_t *spec,
                                         void *prepared,
                                         hk_verify_diag_t *out_diag);

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

    // Optional grouped variants. The core uses these only for a contiguous
    // same-engine wave of hooks without a domain gate; all other operations
    // retain the one-hook fallback above. Each callback must fill the
    // per-operation fields in `operations`. Grouping is additive so existing
    // engines remain source-compatible when these members are NULL.
    bool (*prepare_group_ctx)(void *engine_ctx,
                              hk_engine_operation_t *operations,
                              size_t operation_count);
    bool (*revalidate_group_ctx)(void *engine_ctx,
                                 hk_engine_operation_t *operations,
                                 size_t operation_count);
    bool (*commit_group_ctx)(void *engine_ctx,
                             hk_engine_operation_t *operations,
                             size_t operation_count);
    bool (*verify_group_ctx)(void *engine_ctx,
                             hk_engine_operation_t *operations,
                             size_t operation_count);
    // Called only after a group commit/verification leaves a potentially
    // mutated member. The engine sets operation->compensated only when it
    // restored that member mechanically; a false return keeps every member
    // conservative (not compensated).
    bool (*compensate_group_ctx)(void *engine_ctx,
                                 hk_engine_operation_t *operations,
                                 size_t operation_count);

    // Optional continuation inspection after preparation. This is separate
    // from commit: callers may inspect the exact continuation that a prepared
    // operation will publish before any requested target is mutated.
    hk_verify_result_t (*inspect_continuation)(
        void *engine_ctx,
        const hk_hook_spec_t *spec,
        void *prepared,
        hk_continuation_info_t *out_info,
        hk_verify_diag_t *out_diag);
} hk_engine_vtable_t;

// A zero struct_size is the legacy in-tree form. Explicitly versioned
// vtables must contain the requested member before the core reads it.
static inline bool hk_engine_vtable_has_field(const hk_engine_vtable_t *vtable,
                                              size_t offset,
                                              size_t field_size) {
    return vtable &&
        (vtable->struct_size == 0 ||
         (vtable->struct_size >= offset &&
          vtable->struct_size - offset >= field_size));
}

// Eligibility per spec section 9, minimal subset: target kind supported,
// every required_reach bit is within the engine's achievable_reach, original
// requirement compatibility, and declared preparation/commit effects do not
// violate the request. Install-context compatibility is checked by the
// router with hk_engine_supports_install_context. The router additionally
// checks target-specific exact image scope against its live catalog and
// refuses restricted symbol defining-image selectors unless an engine
// advertises exact symbol scope. The router checks architecture, deployment
// floor, and certification through hk_engine_supports_platform; ownership is
// enforced at commit by HKOwnership.c because it
// needs the process-lifetime target ledger and the engine's predecessor
// witness, not just a static capability check.
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
    // two enums do not share a layout. Both phases matter: continuation pages
    // are normally allocated during prepare, before any target is changed.
    if (effective_constraints != 0) {
        const hk_effects_t phases[] = {caps->prepare_effects,
                                       caps->commit_effects};
        for (size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); phase++) {
            for (unsigned bit = 0; bit < 64; bit++) {
                hk_effects_t effect = 1ull << bit;
                if (!(phases[phase] & effect)) {
                    continue;
                }
                hk_constraints_t forbid = hk_effect_forbid_bit(effect);
                if (forbid != 0 && (effective_constraints & forbid) != 0) {
                    return false;
                }
            }
        }
    }
    return true;
}

static inline bool hk_engine_supports_install_context(
    const hk_engine_capabilities_t *caps,
    hk_install_context_t context) {
    if (!caps || caps->install_contexts == 0) {
        return true;
    }
    if ((unsigned)context > (unsigned)HK_INSTALL_CONTEXT_ARBITRARY_RUNTIME) {
        return false;
    }
    return (caps->install_contexts & HK_INSTALL_CONTEXT_BIT(context)) != 0;
}

// The host runtime intentionally has architecture == 0, so existing fake
// engines stay useful without pretending to make a production certification
// claim. On device, only a descriptor that explicitly supports and certifies
// the active architecture may be selected. A private test registration is the
// documented escape hatch for testing an otherwise uncertified mode.
static inline bool hk_engine_supports_platform(
    const hk_engine_capabilities_t *caps,
    hk_engine_architecture_mask_t architecture,
    uint32_t ios_version,
    bool testing_registration)
{
    if (testing_registration || architecture == 0) {
        return true;
    }
    if (!caps || caps->architectures == 0 ||
        !(caps->architectures & architecture) ||
        !(caps->certified_architectures & architecture)) {
        return false;
    }
    return caps->minimum_ios_version == 0 ||
           (ios_version != 0 && ios_version >= caps->minimum_ios_version);
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

// Applies the optional backend-selection override to the already-registered
// engines[], in place: reorders by an ordered preference and/or drops a
// disable set, keyed on each engine's describe().engine_id (matched
// case-insensitively, tolerating an omitted "provider-" prefix so "ellekit"
// matches "provider-ellekit"). The optional environment variables
// HOOKKIT_BACKENDS / HOOKKIT_DISABLE_BACKENDS are comma- or space-separated.
// The choosable set is exactly the engines this build registered -- unknown
// tokens match nothing. Never empties the registry: a disable set covering
// every engine is ignored. Called once by hk_runtime_create after platform
// engines register; host tests may call it after registering their own engines.
// Defined in HKRuntime.c.
void hk_runtime_apply_backend_policy(hk_runtime_t *runtime);

// Strict per-runtime selection used by the HKSubstitutor compatibility
// facade. Matching IDs are kept in list order; nonmatching function/memory
// engines are removed. The ObjC runtime engine remains so message hooks stay
// facade-native. Unknown IDs intentionally leave no function/memory route.
void hk_runtime_apply_backend_override(hk_runtime_t *runtime,
                                       const char *backend_ids);

#endif // HK_CORE_ENGINE_INTERNAL_H
