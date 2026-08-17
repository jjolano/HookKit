// Shared fake engines for Sources/Core host tests (HKEngineInternal.h's
// contract). Extracted here once test_plan_prepare.c needed the same
// fakes test_engine_registry.c already had, rather than duplicating them
// a second time.

#ifndef HK_TEST_FAKE_ENGINES_H
#define HK_TEST_FAKE_ENGINES_H

#include "../../Sources/Core/HKEngineInternal.h"

// Handles function-symbol targets needing only existing-imports reach,
// and always prepares successfully. Mirrors the real rebind engine's
// eventual shape (Milestone 6) closely enough to be a meaningful
// stand-in, without pretending to implement anything beyond that.
static inline hk_engine_capabilities_t fake_rebind_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-rebind";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline bool fake_rebind_prepare_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return true;
}
static inline hk_mutation_state_t fake_rebind_commit_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return HK_MUTATION_COMPLETE;
}
static const hk_engine_vtable_t fake_rebind_engine = {
    .describe = fake_rebind_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_rebind_commit_one,
};

// Same eligibility shape as fake_rebind_engine, but always fails to
// prepare -- for testing hk_plan_prepare's failure path without needing
// mutable global test state (a second always-failing engine is simpler
// and less order-dependent than a toggle flag on a shared one).
static inline hk_engine_capabilities_t fake_always_fails_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-always-fails";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline bool fake_always_fails_prepare_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return false;
}
static const hk_engine_vtable_t fake_always_fails_engine = {
    .describe = fake_always_fails_describe,
    .prepare_one = fake_always_fails_prepare_one,
};

// Handles objc-method targets. No prepare_one (NULL) -- deliberately, to
// exercise hk_plan_prepare's handling of an engine that was eligible for
// describe() purposes but never implemented preparation (a real
// inconsistency the router should catch, not silently skip).
static inline hk_engine_capabilities_t fake_objc_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-objc";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    caps.achievable_reach = HK_REACH_OBJC_DISPATCH;
    return caps;
}
static const hk_engine_vtable_t fake_objc_engine = {
    .describe = fake_objc_describe,
    .prepare_one = NULL,
    .commit_one = NULL,
};

// The four fakes below all handle function-symbol/existing-imports (same
// shape as fake_rebind_engine) and always prepare successfully -- they
// exist purely to exercise hk_plan_commit's mutation-state-to-outcome
// mapping, one real code path per fake, rather than trusting the switch
// statement's cases from reading it. Mutation-state semantics are one of
// the spec's core invariants (section 4.4/6.27), which is why this gets
// more fakes than prepare's testing needed.
static inline bool fake_commit_helper_prepare_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return true;
}

static inline hk_engine_capabilities_t fake_commit_none_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-commit-none";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline hk_mutation_state_t fake_commit_none_commit_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return HK_MUTATION_NONE;  // refused cleanly, nothing touched
}
static const hk_engine_vtable_t fake_commit_none_engine = {
    .describe = fake_commit_none_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = fake_commit_none_commit_one,
};

static inline hk_engine_capabilities_t fake_commit_partial_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-commit-partial";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline hk_mutation_state_t fake_commit_partial_commit_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return HK_MUTATION_PARTIAL;
}
static const hk_engine_vtable_t fake_commit_partial_engine = {
    .describe = fake_commit_partial_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = fake_commit_partial_commit_one,
};

static inline hk_engine_capabilities_t fake_commit_unknown_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-commit-unknown";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline hk_mutation_state_t fake_commit_unknown_commit_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return HK_MUTATION_UNKNOWN;
}
static const hk_engine_vtable_t fake_commit_unknown_engine = {
    .describe = fake_commit_unknown_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = fake_commit_unknown_commit_one,
};

// prepares successfully but has no commit_one at all -- the commit-time
// analogue of fake_objc_engine's missing prepare_one.
static inline hk_engine_capabilities_t fake_no_commit_one_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "fake-no-commit-one";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static const hk_engine_vtable_t fake_no_commit_one_engine = {
    .describe = fake_no_commit_one_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = NULL,
};

#endif // HK_TEST_FAKE_ENGINES_H
