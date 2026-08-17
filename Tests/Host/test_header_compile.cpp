// C++ compile test for the new HookKit 3.0 headers -- see
// test_header_compile.c for what and why. Proves the extern "C" guards in
// every header actually work: a C++ translation unit including them and
// linking against C-compiled definitions must not suffer name mangling
// mismatches (not exercised by linking here, since nothing is linked, but
// the extern "C" block only compiles cleanly if paired correctly).

#include <cstddef>
#include <cstdint>

#include "../../Headers/HookKit/HookKit.h"

typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");
static_assert(sizeof(hk_id_t) == 16, "hk_id_t is two uint64_t");

static_assert(HK_STATUS_OK == 0, "hk_status_t numeric values are part of the ABI");
static_assert(HK_PLAN_DISCARDED == 9, "hk_plan_state_t numeric values are part of the ABI");
static_assert(HK_COMPENSATION_REQUIRE_REVERSIBLE_PREFIX == 2, "hk_compensation_policy_t numeric values are part of the ABI");

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

int main() {
    hk_domain_spec_t domain = make_sample_domain();
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
    return 0;
}
