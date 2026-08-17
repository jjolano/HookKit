// Plain-C compile test for the new HookKit 3.0 headers (spec section 21,
// Milestone 3: "Header compile tests in C, Objective-C, C++, and
// Objective-C++"). Compiling successfully is most of the test; the
// static_asserts catch a struct-layout or enum-value regression a mere
// "did it compile" check would miss. See test_header_compile.m/.cpp/.mm
// for the other 3 language modes -- kept as 4 separate small files on
// purpose, not one shared include, since the point is exercising 4
// distinct compiler front ends against the exact same headers.

#include <stddef.h>
#include <string.h>
#include <assert.h>

#include "../../Headers/HookKit/HookKit.h"

// HK_STRUCT_HEADER must produce struct_size then struct_version, in that
// order, as the first two members -- every extensible struct's
// forward/backward-compat story depends on this exact shape.
typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
_Static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
_Static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");
_Static_assert(offsetof(hk_test_struct_t, x) == 2 * sizeof(uint32_t), "fields after HK_STRUCT_HEADER must follow immediately");

_Static_assert(sizeof(hk_id_t) == 16, "hk_id_t is two uint64_t");

_Static_assert(HK_STATUS_OK == 0, "hk_status_t numeric values are part of the ABI");
_Static_assert(HK_TARGET_SWIFT_VTABLE == 4, "hk_target_kind_t numeric values are part of the ABI");
_Static_assert(HK_ORIGINAL_CALLABLE_CONTINUATION == 2, "hk_original_requirement_t numeric values are part of the ABI");
_Static_assert(HK_CONTINUATION_FORBIDDEN == 2, "hk_continuation_policy_t numeric values are part of the ABI");
_Static_assert(HK_MUTATION_UNKNOWN == 3, "hk_mutation_state_t numeric values are part of the ABI");
_Static_assert(HK_OUTCOME_INVALIDATED == 13, "hk_outcome_t numeric values are part of the ABI");

_Static_assert(HK_REACH_EXACT_MEMORY == (1ull << 11), "hk_reachability_t bit positions are part of the ABI");
_Static_assert(HK_EFFECT_UNKNOWN_PROCESS_MUTATION == (1ull << 63), "hk_effects_t bit positions are part of the ABI");
_Static_assert(HK_FORBID_UNKNOWN_COMMIT_EFFECTS == (1ull << 13), "hk_constraints_t bit positions are part of the ABI");

_Static_assert(HK_ARTIFACT_UNKNOWN_PROCESS_MUTATION == 17, "hk_artifact_kind_t numeric values are part of the ABI");
_Static_assert(HK_ARTIFACT_OBSERVED_EXISTING == 11, "hk_artifact_state_t numeric values are part of the ABI");
_Static_assert(HK_BYTE_STORAGE_INLINE_AND_HASH == 3, "hk_byte_storage_representation_t numeric values are part of the ABI");
_Static_assert(sizeof(((hk_byte_storage_t *)0)->sha256) == 32, "artifact hashes are SHA-256 (spec section 7.4), never a custom checksum");

// An artifact record must be constructible and usable in real (if
// minimal) C code too -- same bar make_sample_spec() above already holds
// hk_hook_spec_t to.
static hk_artifact_t make_sample_artifact(void) {
    hk_artifact_t artifact;
    memset(&artifact, 0, sizeof(artifact));
    artifact.struct_size = sizeof(artifact);
    artifact.struct_version = HK_ABI_VERSION_3_0;
    artifact.kind = HK_ARTIFACT_IMPORT_SLOT;
    artifact.state = HK_ARTIFACT_COMMITTED;
    artifact.effects = HK_EFFECT_IMPORT_MUTATION;
    artifact.original_bytes.representation = HK_BYTE_STORAGE_HASH;
    artifact.original_bytes.length = 8;
    return artifact;
}

// A hook spec must be constructible and usable in real (if minimal) C code,
// not just exist as a type.
static hk_hook_spec_t make_sample_spec(void) {
    hk_hook_spec_t spec;
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = "test.sample";
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.struct_size = sizeof(spec.target.symbol);
    spec.target.symbol.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.name = "getpid";
    spec.target.symbol.name_convention = HK_SYMBOL_NAME_C;
    spec.replacement = NULL;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.preferred_reach = 0;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.constraints = 0;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    spec.domain = NULL;
    spec.commit_order = 0;
    spec.commit_after = NULL;
    spec.commit_after_count = 0;
    return spec;
}

int main(void) {
    hk_hook_spec_t spec = make_sample_spec();
    hk_artifact_t artifact = make_sample_artifact();
    hk_task_fn task_fn_var = NULL;
    hk_executor_submit_fn submit_fn_var = NULL;
    hk_diagnostic_callback_fn diag_fn_var = NULL;
    (void)task_fn_var;
    (void)submit_fn_var;
    (void)diag_fn_var;

    if (spec.target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        return 1;
    }

    if (artifact.kind != HK_ARTIFACT_IMPORT_SLOT || artifact.state != HK_ARTIFACT_COMMITTED) {
        return 1;
    }

    return 0;
}
