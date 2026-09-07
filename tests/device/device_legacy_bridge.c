// Source-linked simulation of the real bridge/plan/adapter on arm64. Fixture
// targets and continuations are heap buffers, never executable or called.
#include "../../src/core/HKRuntimeInternal.h"

static hk_status_t device_fixture_runtime(const hk_runtime_config_t *config,
                                           hk_runtime_t **runtime) {
    hk_status_t status = hk_runtime_create(config, runtime);
    // The host fixture supplies its own writer. Do not route heap targets to
    // the platform engines automatically registered on Apple builds.
    if (status == HK_STATUS_OK) (*runtime)->engine_count = 0;
    return status;
}

#define hk_runtime_create device_fixture_runtime
#define main legacy_bridge_fixture_main
#include "../host/test_legacy_bridge.c"
#undef main
#undef hk_runtime_create

int main(void) {
    setbuf(stdout, NULL);
    puts("SIMULATION legacy bridge: heap writes, PARTIAL/UNKNOWN/verification/API-error injection; kernel-failure-proven=no; uncertain-original-callability=not-tested");
    return legacy_bridge_fixture_main();
}
