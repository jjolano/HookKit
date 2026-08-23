// HookKit 3.0 -- original/continuation policy, reachability, effects,
// constraints, outcome/mutation state, and the per-hook result struct.
// See docs/3.0/PUBLIC_C_ABI.md and docs/3.0/REACHABILITY_VECTOR.md
// (pending) for the honesty rules governing achieved_reach/effects, not
// just their bit layout.

#ifndef HOOKKIT_RESULTS_H
#define HOOKKIT_RESULTS_H

#include <stdbool.h>
#include <stdint.h>

#include "HookKitBase.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Original requirement and continuation policy (request-side) ------

typedef enum {
    HK_ORIGINAL_NONE = 0,
    HK_ORIGINAL_DIRECT_PREDECESSOR,
    HK_ORIGINAL_CALLABLE_CONTINUATION,
} hk_original_requirement_t;

typedef enum {
    HK_CONTINUATION_ANY = 0,
    HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY,
    HK_CONTINUATION_FORBIDDEN,
} hk_continuation_policy_t;

// HK_ORIGINAL_CALLABLE_CONTINUATION + HK_CONTINUATION_FORBIDDEN is
// contradictory and must be rejected by hk_plan_add_hook before analysis
// runs -- see docs/3.0/ORIGINAL_AND_CONTINUATION_MODEL.md (pending).

// ---- Continuation kind and mapping kind (result-side) ------------------

typedef enum {
    HK_CONTINUATION_KIND_NONE = 0,
    HK_CONTINUATION_KIND_DIRECT_PREDECESSOR,
    HK_CONTINUATION_KIND_STATIC,
    HK_CONTINUATION_KIND_DYNAMIC,
    HK_CONTINUATION_KIND_PROVIDER_INTERNAL,
    HK_CONTINUATION_KIND_UNKNOWN,
} hk_continuation_kind_t;

typedef enum {
    HK_MAPPING_NONE = 0,
    HK_MAPPING_IMAGE_TEXT,
    HK_MAPPING_IMAGE_DATA,
    HK_MAPPING_SHARED_CACHE,
    HK_MAPPING_STATIC_HOOKKIT_SECTION,
    HK_MAPPING_ANONYMOUS,
    HK_MAPPING_PROVIDER_OWNED,
    HK_MAPPING_UNKNOWN,
} hk_mapping_kind_t;

// Every prepared or active function-entry hook returns one of these. A
// strict terminal-inline success (docs/3.0/TERMINAL_INLINE.md, pending
// Milestone 7) must report kind=NONE, address=0, mapping_id={0,0},
// executable_memory_allocated=false, relocated_instruction_count=0,
// jump_back_destination=0 -- enforced by the terminal-inline engine
// contract, not by this struct's shape.
typedef struct {
    HK_STRUCT_HEADER;

    hk_continuation_kind_t kind;

    uintptr_t address;
    uintptr_t jump_back_destination;

    hk_id_t mapping_id;
    hk_mapping_kind_t mapping_kind;
    uintptr_t mapping_base;
    size_t mapping_size;
    uint32_t mapping_protection;

    bool executable_memory_allocated;
    uint32_t relocated_instruction_count;

    bool readable;
    bool mechanically_reversible;
    bool safe_to_reverse_after_activation;
    bool fully_inspected;
} hk_continuation_info_t;

// ---- Reachability capability vector -------------------------------------
//
// required_reach/preferred_reach on a request; achieved_reach/
// unmet_preferred_reach on a result. All required bits must be achieved,
// no exceptions. What may never be claimed (never entrypoint/saved-pointer
// reach from import rebinding, never future-image reach without an active
// future-image mechanism, never exact-image-scope when the engine mutates
// a broader set) is enforced by the engine contract and router, not by
// this typedef -- see docs/3.0/REACHABILITY_VECTOR.md (pending).

typedef uint64_t hk_reachability_t;

enum {
    HK_REACH_EXISTING_IMPORTS      = 1ull << 0,
    HK_REACH_FUTURE_IMPORTS        = 1ull << 1,
    HK_REACH_ENTRYPOINT            = 1ull << 2,
    HK_REACH_DLSYM_POINTERS        = 1ull << 3,
    HK_REACH_SAVED_POINTERS        = 1ull << 4,
    HK_REACH_EXACT_IMAGE_SCOPE     = 1ull << 5,
    HK_REACH_PRIVATE_ADDRESS       = 1ull << 6,
    HK_REACH_CURRENT_IMAGES        = 1ull << 7,
    HK_REACH_FUTURE_IMAGES         = 1ull << 8,
    HK_REACH_OBJC_DISPATCH         = 1ull << 9,
    HK_REACH_SWIFT_VTABLE_DISPATCH = 1ull << 10,
    HK_REACH_EXACT_MEMORY          = 1ull << 11,
};

// ---- Process effects and request constraints ----------------------------
//
// Every operation reports 4 masks: declared_prepare_effects,
// observed_prepare_effects, declared_commit_effects,
// observed_commit_effects (all on hk_hook_result_t below). An observed
// effect outside the declared upper bound is an engine contract violation
// and fails the operation -- this is what makes the analysis/preparation
// purity rules (docs/3.0/ARCHITECTURE.md, invariants #1-#2) mechanically
// enforceable rather than just documented.

typedef uint64_t hk_effects_t;

enum {
    HK_EFFECT_TARGET_TEXT_MUTATION       = 1ull << 0,
    HK_EFFECT_IMPORT_MUTATION            = 1ull << 1,
    HK_EFFECT_OBJC_METADATA_MUTATION     = 1ull << 2,
    HK_EFFECT_SWIFT_VTABLE_MUTATION      = 1ull << 3,
    HK_EFFECT_MEMORY_MUTATION            = 1ull << 4,

    HK_EFFECT_EXECUTABLE_ALLOCATION      = 1ull << 5,
    HK_EFFECT_STATIC_CONTINUATION_USE    = 1ull << 6,
    HK_EFFECT_PROVIDER_ACTIVATION        = 1ull << 7,
    HK_EFFECT_PROVIDER_IMAGE_LOAD        = 1ull << 8,
    HK_EFFECT_IMAGE_LOAD                 = 1ull << 9,
    HK_EFFECT_CALLBACK_REGISTRATION      = 1ull << 10,
    HK_EFFECT_THREAD_CREATION            = 1ull << 11,
    HK_EFFECT_PRIVATE_SYMBOL_SCAN        = 1ull << 12,
    HK_EFFECT_FILE_MAPPING               = 1ull << 13,
    HK_EFFECT_MEMORY_PROTECTION_CHANGE   = 1ull << 14,

    HK_EFFECT_UNKNOWN_PROCESS_MUTATION   = 1ull << 63,
};

typedef uint64_t hk_constraints_t;

enum {
    HK_FORBID_TARGET_TEXT_PATCH           = 1ull << 0,
    HK_FORBID_IMPORT_SLOT_PATCH           = 1ull << 1,
    HK_FORBID_OBJC_METADATA_CHANGE        = 1ull << 2,
    HK_FORBID_SWIFT_VTABLE_CHANGE         = 1ull << 3,

    HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY   = 1ull << 4,
    HK_FORBID_STATIC_CONTINUATION         = 1ull << 5,
    HK_FORBID_PRIVATE_SYMBOL_SCAN         = 1ull << 6,

    HK_FORBID_PROVIDER_ACTIVATION         = 1ull << 7,
    HK_FORBID_PROVIDER_IMAGE_LOAD         = 1ull << 8,
    HK_FORBID_CALLBACK_REGISTRATION       = 1ull << 9,
    HK_FORBID_LATE_IMAGE_CALLBACK         = 1ull << 10,
    HK_FORBID_THREAD_CREATION             = 1ull << 11,

    HK_FORBID_UNKNOWN_PREPARATION_EFFECTS = 1ull << 12,
    HK_FORBID_UNKNOWN_COMMIT_EFFECTS      = 1ull << 13,
};

// ---- Outcome and mutation state ------------------------------------------

typedef enum {
    HK_OUTCOME_UNANALYZED = 0,
    HK_OUTCOME_NO_ROUTE,
    HK_OUTCOME_ANALYZED,
    HK_OUTCOME_PREPARED,
    HK_OUTCOME_PENDING,
    HK_OUTCOME_ACTIVE,
    HK_OUTCOME_ALREADY_ACTIVE,
    HK_OUTCOME_STALE_PLAN,
    HK_OUTCOME_CONFLICT,
    HK_OUTCOME_FAILED_SAFE,
    HK_OUTCOME_FAILED_PARTIAL,
    HK_OUTCOME_FAILED_UNKNOWN,
    HK_OUTCOME_COMPENSATED,
    HK_OUTCOME_INVALIDATED,
} hk_outcome_t;

// A route may fall back to another candidate only after HK_MUTATION_NONE.
// Never after PARTIAL or UNKNOWN -- docs/3.0/ARCHITECTURE.md invariant #4.
typedef enum {
    HK_MUTATION_NONE = 0,
    HK_MUTATION_COMPLETE,
    HK_MUTATION_PARTIAL,
    HK_MUTATION_UNKNOWN,
} hk_mutation_state_t;

// ---- Hook result ----------------------------------------------------------
//
// Never an aggregate-only result: every hook in a plan gets its own one of
// these, even inside a batch (docs/3.0/ARCHITECTURE.md's non-negotiables).

typedef struct {
    HK_STRUCT_HEADER;

    hk_id_t runtime_owner_id;
    hk_id_t plan_id;
    hk_id_t request_id;
    hk_id_t installed_id;

    uint64_t image_generation;
    uint64_t installed_generation;

    hk_outcome_t outcome;
    hk_mutation_state_t mutation;

    hk_reachability_t achieved_reach;
    hk_reachability_t unmet_preferred_reach;

    hk_effects_t declared_prepare_effects;
    hk_effects_t observed_prepare_effects;
    hk_effects_t declared_commit_effects;
    hk_effects_t observed_commit_effects;

    hk_continuation_info_t continuation;

    bool original_available;
    bool verified;
    bool retryable;
    bool currently_valid;

    uint32_t matched_locations;
    uint32_t modified_locations;

    hk_string_view_t diagnostic_engine_id;
    hk_string_view_t error_domain;
    int64_t error_code;
    hk_string_view_t error_message;

    size_t artifact_count;
} hk_hook_result_t;

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_RESULTS_H
