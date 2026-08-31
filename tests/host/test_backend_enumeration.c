// Host coverage for the public runtime backend enumerator. It must preserve
// registration order, apply the same side-effect-free discovery rule as the
// router, and let a caller stop without treating that as an error.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/HKEngineInternal.h"
#include "../../src/core/HKRuntimeInternal.h"

static hk_engine_capabilities_t describe_alpha(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "alpha";
    caps.display_name = "Alpha Engine";
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    return caps;
}

static hk_engine_capabilities_t describe_hidden(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "hidden";
    return caps;
}

static hk_engine_capabilities_t describe_legacy(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "unversioned";
    return caps;
}

// Two engines, one group: the enumerator must collapse them to a single
// entry carrying the shared group token and its label.
static hk_engine_capabilities_t describe_group_first(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "grp-first";
    caps.backend_group = "grouped";
    caps.display_name = "Grouped Backend";
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    return caps;
}

static hk_engine_capabilities_t describe_group_second(void) {
    hk_engine_capabilities_t caps = describe_group_first();
    caps.engine_id = "grp-second";
    return caps;
}

static hk_engine_capabilities_t describe_too_new(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "too-new";
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(16, 0, 0);
    return caps;
}

static bool hidden_discover(void *context, hk_engine_discovery_t *out) {
    (void)context;
    out->available = false;
    return true;
}

static const hk_engine_vtable_t alpha_engine = {
    .describe = describe_alpha,
};

static const hk_engine_vtable_t hidden_engine = {
    .describe = describe_hidden,
    .discover = hidden_discover,
};

static const hk_engine_vtable_t legacy_engine = {
    .describe = describe_legacy,
};

static const hk_engine_vtable_t too_new_engine = {
    .describe = describe_too_new,
};

static const hk_engine_vtable_t group_first_engine = {
    .describe = describe_group_first,
};

static const hk_engine_vtable_t group_second_engine = {
    .describe = describe_group_second,
};

typedef struct {
    const char *ids[4];
    size_t count;
    bool stop_after_first;
} collected_ids_t;

static bool collect_id(void *opaque, hk_string_view_t backend_id,
                       hk_string_view_t display_name) {
    collected_ids_t *collected = opaque;
    assert(backend_id.data != NULL);
    assert(display_name.data != NULL);
    // Engines with a label report it; those without fall back to the id.
    if (strcmp(backend_id.data, "alpha") == 0) {
        assert(strcmp(display_name.data, "Alpha Engine") == 0);
    } else if (strcmp(backend_id.data, "grouped") == 0) {
        assert(strcmp(display_name.data, "Grouped Backend") == 0);
    } else {
        assert(strcmp(display_name.data, backend_id.data) == 0);
    }
    assert(collected->count < sizeof(collected->ids) / sizeof(collected->ids[0]));
    collected->ids[collected->count++] = backend_id.data;
    return !collected->stop_after_first;
}

static hk_runtime_t *make_runtime(void) {
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK && runtime);
    assert(hk_runtime_register_engine_for_testing(runtime, &alpha_engine));
    assert(hk_runtime_register_engine_for_testing(runtime, &hidden_engine));
    assert(hk_runtime_register_engine_for_testing(runtime, &legacy_engine));
    return runtime;
}

static void test_skips_undiscoverable_engine(void) {
    hk_runtime_t *runtime = make_runtime();
    collected_ids_t collected = {0};
    assert(hk_runtime_enumerate_backends(runtime, collect_id, &collected) ==
           HK_STATUS_OK);
    assert(collected.count == 2);
    assert(strcmp(collected.ids[0], "alpha") == 0);
    assert(strcmp(collected.ids[1], "unversioned") == 0);
    hk_runtime_release(runtime);
    printf("  skips-undiscoverable-engine: PASS\n");
}

static void test_stopping_is_successful(void) {
    hk_runtime_t *runtime = make_runtime();
    collected_ids_t collected = { .stop_after_first = true };
    assert(hk_runtime_enumerate_backends(runtime, collect_id, &collected) ==
           HK_STATUS_OK);
    assert(collected.count == 1);
    assert(strcmp(collected.ids[0], "alpha") == 0);
    hk_runtime_release(runtime);
    printf("  stopping-is-successful: PASS\n");
}

static void test_invalid_arguments(void) {
    collected_ids_t collected = {0};
    assert(hk_runtime_enumerate_backends(NULL, collect_id, &collected) ==
           HK_STATUS_INVALID_ARGUMENT);
    hk_runtime_t *runtime = make_runtime();
    assert(hk_runtime_enumerate_backends(runtime, NULL, &collected) ==
           HK_STATUS_INVALID_ARGUMENT);
    hk_runtime_release(runtime);
    printf("  invalid-arguments: PASS\n");
}

static void test_skips_platform_ineligible_engine(void) {
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK && runtime);
    assert(hk_runtime_register_engine_for_testing(runtime, &alpha_engine));
    assert(hk_runtime_register_engine_for_testing(runtime, &too_new_engine));
    runtime->platform_architecture = HK_ENGINE_ARCHITECTURE_ARM64;
    runtime->platform_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    runtime->engine_testing[0] = false;
    runtime->engine_testing[1] = false;

    collected_ids_t collected = {0};
    assert(hk_runtime_enumerate_backends(runtime, collect_id, &collected) ==
           HK_STATUS_OK);
    assert(collected.count == 1);
    assert(strcmp(collected.ids[0], "alpha") == 0);
    hk_runtime_release(runtime);
    printf("  skips-platform-ineligible-engine: PASS\n");
}

static void test_collapses_shared_group(void) {
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK && runtime);
    assert(hk_runtime_register_engine_for_testing(runtime, &group_first_engine));
    assert(hk_runtime_register_engine_for_testing(runtime, &group_second_engine));
    collected_ids_t collected = {0};
    assert(hk_runtime_enumerate_backends(runtime, collect_id, &collected) ==
           HK_STATUS_OK);
    assert(collected.count == 1);
    assert(strcmp(collected.ids[0], "grouped") == 0);
    hk_runtime_release(runtime);
    printf("  collapses-shared-group: PASS\n");
}

int main(void) {
    test_skips_undiscoverable_engine();
    test_stopping_is_successful();
    test_invalid_arguments();
    test_skips_platform_ineligible_engine();
    test_collapses_shared_group();
    puts("backend enumeration: PASS");
    return 0;
}
