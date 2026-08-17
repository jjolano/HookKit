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
static const hk_engine_vtable_t fake_rebind_engine = {
    .describe = fake_rebind_describe,
    .prepare_one = fake_rebind_prepare_one,
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
};

#endif // HK_TEST_FAKE_ENGINES_H
