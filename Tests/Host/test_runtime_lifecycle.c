// Host test for Sources/Core/HKRuntime.c + HKIDs.c -- real implementation,
// not a header compile check. Includes the internal headers directly (same
// pattern as tests/test_swift_abi.c including native/hk_swift.c) so this
// can verify the config was actually copied, not just that create()
// returned OK.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKIDs.h"
#include "../../Sources/Core/HKRuntimeInternal.h"

static void test_create_with_null_config(void) {
    hk_runtime_t *rt = NULL;
    hk_status_t status = hk_runtime_create(NULL, &rt);
    assert(status == HK_STATUS_OK);
    assert(rt != NULL);
    assert(rt->config.struct_size == sizeof(hk_runtime_config_t));
    assert(rt->config.struct_version == HK_ABI_VERSION_3_0);
    assert(rt->config.submit == NULL);
    hk_runtime_release(rt);
    printf("  create-with-null-config: PASS\n");
}

static void test_create_with_real_config(void) {
    bool submit_called_flag = false;
    hk_runtime_config_t config;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.struct_version = HK_ABI_VERSION_3_0;
    config.submit = NULL;
    config.executor_context = &submit_called_flag;  // arbitrary non-NULL pointer to round-trip
    config.diagnostic_callback = NULL;
    config.diagnostic_context = NULL;
    config.install_context = HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED;

    hk_runtime_t *rt = NULL;
    hk_status_t status = hk_runtime_create(&config, &rt);
    assert(status == HK_STATUS_OK);
    assert(rt != NULL);
    // Real verification the config was actually deep-copied by value, not
    // just that the call succeeded.
    assert(rt->config.executor_context == &submit_called_flag);
    assert(rt->config.install_context == HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED);
    hk_runtime_release(rt);
    printf("  create-with-real-config: PASS\n");
}

static void test_undersized_config_rejected(void) {
    hk_runtime_config_t config;
    memset(&config, 0, sizeof(config));
    config.struct_size = 4;  // smaller than this build's hk_runtime_config_t
    config.struct_version = HK_ABI_VERSION_3_0;

    hk_runtime_t *rt = (hk_runtime_t *)(void *)0x1;  // sentinel: must be set to NULL on rejection
    hk_status_t status = hk_runtime_create(&config, &rt);
    assert(status == HK_STATUS_INVALID_ARGUMENT);
    assert(rt == NULL);
    printf("  undersized-config-rejected: PASS\n");
}

static void test_null_out_runtime_rejected(void) {
    hk_status_t status = hk_runtime_create(NULL, NULL);
    assert(status == HK_STATUS_INVALID_ARGUMENT);
    printf("  null-out-runtime-rejected: PASS\n");
}

static void test_owner_ids_distinct_but_same_process_nonce(void) {
    hk_runtime_t *a = NULL, *b = NULL;
    assert(hk_runtime_create(NULL, &a) == HK_STATUS_OK);
    assert(hk_runtime_create(NULL, &b) == HK_STATUS_OK);

    hk_id_t id_a = hk_runtime_owner_id(a);
    hk_id_t id_b = hk_runtime_owner_id(b);

    assert(id_a.high == id_b.high);  // same process nonce
    assert(id_a.low != id_b.low);    // distinct monotonic counter values
    assert(id_a.low != 0 && id_b.low != 0);  // 0 is the "no ID" sentinel, never issued

    hk_runtime_release(a);
    hk_runtime_release(b);
    printf("  owner-ids-distinct-but-same-process-nonce: PASS\n");
}

static void test_owner_id_of_null_is_zero(void) {
    hk_id_t id = hk_runtime_owner_id(NULL);
    assert(id.high == 0 && id.low == 0);
    printf("  owner-id-of-null-is-zero: PASS\n");
}

static void test_shutdown_and_release_tolerate_null(void) {
    hk_runtime_shutdown(NULL);   // must not crash
    hk_runtime_release(NULL);    // free(NULL) is legal, must not crash
    hk_report_release(NULL);     // must not crash
    printf("  shutdown-and-release-tolerate-null: PASS\n");
}

static void test_drain_pending_empty_steady_state(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);

    hk_report_t *report = (hk_report_t *)(void *)0x1;  // sentinel
    hk_status_t status = hk_runtime_drain_pending(rt, &report);
    assert(status == HK_STATUS_OK);
    assert(report == NULL);  // nothing pending is success, not an error

    assert(hk_runtime_drain_pending(NULL, NULL) == HK_STATUS_INVALID_ARGUMENT);

    hk_runtime_release(rt);
    printf("  drain-pending-empty-steady-state: PASS\n");
}

static void test_id_generate_monotonic(void) {
    hk_id_t a = hk_id_generate();
    hk_id_t b = hk_id_generate();
    assert(a.high == b.high);
    assert(b.low == a.low + 1);
    printf("  id-generate-monotonic: PASS\n");
}

int main(void) {
    test_create_with_null_config();
    test_create_with_real_config();
    test_undersized_config_rejected();
    test_null_out_runtime_rejected();
    test_owner_ids_distinct_but_same_process_nonce();
    test_owner_id_of_null_is_zero();
    test_shutdown_and_release_tolerate_null();
    test_drain_pending_empty_steady_state();
    test_id_generate_monotonic();
    printf("all runtime lifecycle tests passed\n");
    return 0;
}
