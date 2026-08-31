// C++ compile test for the new HookKit 3.0 headers -- see
// test_header_compile.c for what and why. Proves the extern "C" guards in
// every header actually work: a C++ translation unit including them and
// linking against C-compiled definitions must not suffer name mangling
// mismatches (not exercised by linking here, since nothing is linked, but
// the extern "C" block only compiles cleanly if paired correctly).

#include <cassert>
#include <cstring>
#include <cstddef>
#include <cstdint>

#include "../../include/HookKit/HookKit.h"

typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");
static_assert(sizeof(hk_id_t) == 16, "hk_id_t is two uint64_t");

static_assert(HK_STATUS_OK == 0, "hk_status_t numeric values are part of the ABI");
static_assert(HK_PLAN_DISCARDED == 9, "hk_plan_state_t numeric values are part of the ABI");
static_assert(HK_COMPENSATION_REQUIRE_REVERSIBLE_PREFIX == 2, "hk_compensation_policy_t numeric values are part of the ABI");
static_assert(HK_ARTIFACT_LOADED_PROVIDER_IMAGE == 10, "hk_artifact_kind_t numeric values are part of the ABI");

// hk_artifact_mapping_t/hk_vm_protection_t aren't exercised in
// test_header_compile.c -- proves the nested (non-HK_STRUCT_HEADER) plain
// struct aggregate-initializes correctly under a real C++ front end too.
static hk_artifact_mapping_t make_sample_artifact_mapping(void) {
    hk_artifact_mapping_t mapping;
    mapping.struct_size = sizeof(mapping);
    mapping.struct_version = HK_ABI_VERSION_3_0;
    mapping.mapping_id = hk_id_t{0, 0};
    mapping.kind = HK_MAPPING_ANONYMOUS;
    mapping.base = 0x2000;
    mapping.size = 0x1000;
    mapping.protection = hk_vm_protection_t{true, false, true};
    return mapping;
}

static hk_domain_spec_t make_sample_domain(void) {
    hk_domain_spec_t domain;
    domain.struct_size = sizeof(domain);
    domain.struct_version = HK_ABI_VERSION_3_0;
    domain.stable_domain_id = "test.domain";
    domain.domain_order = 0;
    domain.require_all_mandatory_prepared = true;
    domain.prefer_reversible_before_irreversible = true;
    domain.compensation_policy = HK_COMPENSATION_BEST_EFFORT_REVERSIBLE_MEMBERS;
    return domain;
}

// The Swift request types. Included in all four variants deliberately: unlike
// HookKitObjC.h this header needs no ObjC runtime, and the C and C++ passes
// are what prove that claim rather than assert it.
static void exercise_swift_target(void) {
    hk_swift_target_t t = hk_swift_target_init();
    assert(t.struct_size == sizeof(hk_swift_target_t));
    // The documented default is require_unique = TRUE, which is the opposite
    // of what a zeroed struct gives -- so the initializer is load-bearing and
    // not a convenience.
    assert(t.require_unique);
    assert(t.name_kind == HK_SWIFT_NAME_MANGLED_EXACT);
    assert(t.availability == HK_AVAILABILITY_REQUIRED_NOW);

    hk_swift_target_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    assert(!zeroed.require_unique);   // exactly the trap the initializer avoids

    t.name_kind = HK_SWIFT_NAME_DEMANGLED_SUBSTRING;
    t.method_name = "MyClass.doThing";
    t.class_name = "MyModule.MyClass";
    assert(t.method_name && t.class_name);

    t.name_kind = HK_SWIFT_NAME_SLOT_INDEX;
    t.slot_index = 3;
    assert(t.slot_index == 3);
}

int main() {
    exercise_swift_target();
    hk_domain_spec_t domain = make_sample_domain();
    hk_artifact_mapping_t mapping = make_sample_artifact_mapping();
    hk_runtime_config_t config;
    config.struct_size = sizeof(config);
    config.struct_version = HK_ABI_VERSION_3_0;
    config.submit = nullptr;
    config.executor_context = nullptr;
    config.diagnostic_callback = nullptr;
    config.diagnostic_context = nullptr;
    config.install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS;

    if (!domain.require_all_mandatory_prepared || config.submit != nullptr) {
        return 1;
    }
    if (mapping.kind != HK_MAPPING_ANONYMOUS || !mapping.protection.read) {
        return 1;
    }
    return 0;
}
