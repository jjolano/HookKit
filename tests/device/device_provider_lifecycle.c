// Packaged-framework arm64 gate for the HK provider adapters. It selects
// each already-registered provider binding without adding a production test
// SPI, then drives only the public plan lifecycle.

#include <HookKit/HookKit.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/engines/HKStaticPool.h"

__attribute__((noinline)) static int dobby_target(int value) {
    volatile int result = value;
    result += 7;
    return result;
}

__attribute__((noinline)) static int dobby_replacement(int value) {
    return value + 42;
}

__attribute__((noinline)) static int gum_target(int value) {
    volatile int result = value;
    result += 11;
    return result;
}

__attribute__((noinline)) static int gum_replacement(int value) {
    return value + 55;
}

__attribute__((noinline)) static int ellekit_target(int value) {
    volatile int result = value;
    result += 13;
    return result;
}

__attribute__((noinline)) static int ellekit_replacement(int value) {
    return value + 70;
}

typedef int (*int_function_t)(int);

static int fail(const char *provider, const char *stage) {
    fprintf(stderr, "HookKit HK provider %s: FAIL: %s\n", provider, stage);
    return 1;
}

#define CHECK(provider, stage, condition) \
    do { if (!(condition)) return fail((provider), (stage)); } while (0)

static bool text_is(hk_string_view_t text, const char *expected) {
    size_t length = strlen(expected);
    return text.data && text.length == length && memcmp(text.data, expected, length) == 0;
}

static bool select_registered_provider(hk_runtime_t *runtime, const char *engine_id) {
    // The canonical package registers the providers. Before a plan exists,
    // narrowing its private registry is a test-only way to exercise each
    // shipping platform binding without exposing a public preference knob.
    for (size_t i = 0; i < runtime->engine_count; i++) {
        const hk_engine_vtable_t *engine = runtime->engines[i];
        if (!engine || !engine->describe) {
            continue;
        }
        hk_engine_capabilities_t capabilities = engine->describe();
        if (capabilities.engine_id && strcmp(capabilities.engine_id, engine_id) == 0) {
            runtime->engines[0] = engine;
            runtime->engine_ctxs[0] = runtime->engine_ctxs[i];
            runtime->engine_testing[0] = runtime->engine_testing[i];
            runtime->engine_count = 1;
            return true;
        }
    }
    return false;
}

static bool snapshot_has(const hk_artifact_snapshot_t *snapshot,
                         hk_artifact_kind_t kind, hk_effects_t effects,
                         const char *engine_id) {
    for (size_t i = 0; i < hk_artifact_snapshot_count(snapshot); i++) {
        hk_artifact_t artifact;
        memset(&artifact, 0, sizeof(artifact));
        if (hk_artifact_snapshot_copy_at(snapshot, i, &artifact) == HK_STATUS_OK &&
            artifact.kind == kind && artifact.effects == effects &&
            text_is(artifact.engine_id, engine_id)) {
            return true;
        }
    }
    return false;
}

static int test_provider(const char *engine_id, int_function_t target,
                         int_function_t replacement, int baseline, int hooked,
                         hk_effects_t prepare_effects, hk_effects_t commit_effects,
                          size_t artifact_count, bool hookkit_continuation,
                          bool force_dynamic) {
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    hk_report_t *report = NULL;
    hk_artifact_snapshot_t *artifacts = NULL;
    hk_hook_result_t result;
    memset(&result, 0, sizeof(result));
    CHECK(engine_id, "baseline", target(1) == baseline);
    CHECK(engine_id, "runtime create", hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    CHECK(engine_id, "registered binding", select_registered_provider(runtime, engine_id));
    uintptr_t held[HK_STATIC_POOL_MAX_SLOTS];
    size_t held_count = 0;
    if (force_dynamic) {
        hk_reloc_engine_ctx_t *pool = &runtime->static_engine;
        CHECK(engine_id, "pool callbacks", pool->alloc && pool->seal && pool->free_page);
        // Fresh-process mode: reserve and seal unused slots, without changing
        // provider bindings or writing the pool bitmap directly.
        for (;;) {
            hk_artifact_mapping_t mapping = {0};
            uintptr_t page = pool->alloc(pool->seam_ctx, HK_RELOC_PAGE_BYTES,
                                         (uintptr_t)target, &mapping);
            if (!page) break;
            CHECK(engine_id, "pool bound", held_count < HK_STATIC_POOL_MAX_SLOTS);
            CHECK(engine_id, "pool backing", mapping.kind == HK_MAPPING_STATIC_HOOKKIT_SECTION);
            CHECK(engine_id, "seal reserved slot",
                  pool->seal(pool->seam_ctx, page, HK_RELOC_PAGE_BYTES));
            held[held_count++] = page;
        }
        CHECK(engine_id, "reserved static pool", held_count != 0);
    }
    CHECK(engine_id, "plan create", hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = engine_id;
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = (uintptr_t)target;
    spec.replacement = (void *)replacement;
    spec.required_reach = HK_REACH_ENTRYPOINT;
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;

    CHECK(engine_id, "add hook", hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    CHECK(engine_id, "analyze", hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    CHECK(engine_id, "analyze result", hk_hook_copy_result(hook, &result) == HK_STATUS_OK &&
          result.outcome == HK_OUTCOME_ANALYZED && text_is(result.diagnostic_engine_id, engine_id));
    CHECK(engine_id, "analyze is non-mutating", target(1) == baseline);

    CHECK(engine_id, "prepare", hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    CHECK(engine_id, "prepare result", hk_hook_copy_result(hook, &result) == HK_STATUS_OK &&
          result.outcome == HK_OUTCOME_PREPARED);
    CHECK(engine_id, "prepare is non-mutating", target(1) == baseline);
    hk_continuation_info_t prepared = result.continuation;
    bool static_backing = hookkit_continuation &&
        prepared.mapping_kind == HK_MAPPING_STATIC_HOOKKIT_SECTION;
    if (force_dynamic) {
        CHECK(engine_id, "dynamic fallback backing",
              prepared.mapping_kind == HK_MAPPING_ANONYMOUS &&
              prepared.kind == HK_CONTINUATION_KIND_DYNAMIC);
        for (size_t i = 0; i < held_count; i++) {
            CHECK(engine_id, "fallback is not reserved slot", prepared.mapping_base != held[i]);
            runtime->static_engine.free_page(runtime->static_engine.seam_ctx,
                                             held[i], HK_RELOC_PAGE_BYTES);
        }
        printf("HookKit HK provider %s: dynamic-fallback reserved-slots=%zu\n",
               engine_id, held_count);
    }
    hk_effects_t backing_effect = static_backing
        ? HK_EFFECT_STATIC_CONTINUATION_USE : HK_EFFECT_EXECUTABLE_ALLOCATION;
    if (hookkit_continuation) {
        CHECK(engine_id, "prepared continuation",
              result.continuation.kind == (static_backing
                  ? HK_CONTINUATION_KIND_STATIC : HK_CONTINUATION_KIND_DYNAMIC) &&
              result.continuation.mapping_kind == (static_backing
                  ? HK_MAPPING_STATIC_HOOKKIT_SECTION : HK_MAPPING_ANONYMOUS) &&
              result.continuation.executable_memory_allocated == !static_backing &&
              result.continuation.mapping_base && result.continuation.mapping_size >= 128 &&
              result.continuation.address &&
              (result.continuation.mapping_id.high || result.continuation.mapping_id.low) &&
              result.continuation.mapping_protection == 5u &&
              result.continuation.relocated_instruction_count == 4 &&
              result.continuation.fully_inspected);
        prepare_effects |= backing_effect;
        commit_effects |= backing_effect;
    }
    CHECK(engine_id, "prepare effects", result.observed_prepare_effects == prepare_effects);

    CHECK(engine_id, "commit", hk_plan_commit(plan, &report) == HK_STATUS_OK && report);
    CHECK(engine_id, "commit result", hk_hook_copy_result(hook, &result) == HK_STATUS_OK &&
          result.outcome == HK_OUTCOME_ACTIVE && result.mutation == HK_MUTATION_COMPLETE &&
          result.observed_commit_effects == commit_effects && !result.verified &&
          result.original_available && result.continuation.kind == (static_backing
              ? HK_CONTINUATION_KIND_STATIC : HK_CONTINUATION_KIND_DYNAMIC) &&
          result.continuation.mapping_kind == (hookkit_continuation
              ? prepared.mapping_kind : HK_MAPPING_PROVIDER_OWNED) &&
          result.continuation.executable_memory_allocated == !static_backing &&
          !result.continuation.mechanically_reversible &&
          !result.continuation.safe_to_reverse_after_activation &&
          result.continuation.fully_inspected == hookkit_continuation);
    if (hookkit_continuation) {
        CHECK(engine_id, "continuation preserved across commit",
              memcmp(&prepared, &result.continuation, sizeof(prepared)) == 0);
    }
    CHECK(engine_id, "replacement", target(1) == hooked);
    const hk_original_slot_t *slot = hk_hook_original_slot(hook);
    void *original = hk_original_slot_load(slot);
    CHECK(engine_id, "original", original && ((int_function_t)original)(1) == baseline);

    CHECK(engine_id, "artifact snapshot", hk_report_copy_artifacts(report, &artifacts) == HK_STATUS_OK &&
          hk_artifact_snapshot_count(artifacts) == artifact_count);
    CHECK(engine_id, "target artifact", snapshot_has(artifacts,
          HK_ARTIFACT_TARGET_TEXT_PATCH, HK_EFFECT_TARGET_TEXT_MUTATION, engine_id));
    CHECK(engine_id, "original artifact", snapshot_has(artifacts,
          HK_ARTIFACT_ORIGINAL_POINTER, backing_effect, engine_id));
    if (hookkit_continuation) {
        CHECK(engine_id, "HookKit trampoline artifact", snapshot_has(artifacts,
              static_backing ? HK_ARTIFACT_STATIC_CONTINUATION : HK_ARTIFACT_TRAMPOLINE,
              backing_effect, engine_id));
        for (size_t i = 0; i < hk_artifact_snapshot_count(artifacts); i++) {
            hk_artifact_t a;
            CHECK(engine_id, "artifact read", hk_artifact_snapshot_copy_at(artifacts, i, &a) == HK_STATUS_OK);
            if (a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH) {
                CHECK(engine_id, "target replacement", a.replacement_pointer == spec.replacement);
                continue;
            }
            CHECK(engine_id, "artifact backing matches prepared continuation",
                  a.effects == backing_effect && a.mapping.kind == prepared.mapping_kind &&
                  a.mapping.base == prepared.mapping_base && a.mapping.size == prepared.mapping_size &&
                  a.mapping.mapping_id.high == prepared.mapping_id.high &&
                  a.mapping.mapping_id.low == prepared.mapping_id.low &&
                  a.mapping.protection.read && a.mapping.protection.execute && !a.mapping.protection.write &&
                  a.continuation_address == prepared.address);
        }
        printf("HookKit HK provider %s: backing=%d mapping=%d size=%zu executable_memory_allocated=%d\n",
               engine_id, prepared.kind, prepared.mapping_kind, prepared.mapping_size,
               prepared.executable_memory_allocated);
    }
    if (strcmp(engine_id, "provider-dobby") == 0) {
        CHECK(engine_id, "activation artifact", snapshot_has(artifacts,
              HK_ARTIFACT_PROVIDER_ACTIVATION, HK_EFFECT_PROVIDER_ACTIVATION, engine_id));
        CHECK(engine_id, "unknown artifact", snapshot_has(artifacts,
              HK_ARTIFACT_UNKNOWN_PROCESS_MUTATION,
              HK_EFFECT_UNKNOWN_PROCESS_MUTATION, engine_id));
    }

    hk_artifact_snapshot_release(artifacts);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    CHECK(engine_id, "replacement after release", target(1) == hooked);
    CHECK(engine_id, "original after release",
          hk_original_slot_load(slot) == original && ((int_function_t)original)(1) == baseline);
    printf("HookKit HK provider %s: PASS\n", engine_id);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "dynamic-fallback") == 0) {
        return test_provider("provider-ellekit", ellekit_target,
                             ellekit_replacement, 14, 71,
                             HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                 HK_EFFECT_PROVIDER_ACTIVATION |
                                 HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
                             HK_EFFECT_TARGET_TEXT_MUTATION, 3, true, true);
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [dynamic-fallback]\n", argv[0]);
        return 1;
    }
    if (test_provider("provider-dobby", dobby_target, dobby_replacement, 8, 43,
                      0, HK_EFFECT_TARGET_TEXT_MUTATION |
                             HK_EFFECT_EXECUTABLE_ALLOCATION |
                             HK_EFFECT_PROVIDER_ACTIVATION |
                             HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
                      4, false, false) != 0) {
        return 1;
    }
    if (test_provider("provider-gum", gum_target, gum_replacement, 12, 56,
                      HK_EFFECT_PROVIDER_IMAGE_LOAD |
                          HK_EFFECT_PROVIDER_ACTIVATION |
                          HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
                      HK_EFFECT_TARGET_TEXT_MUTATION |
                          HK_EFFECT_EXECUTABLE_ALLOCATION,
                      2, false, false) != 0) {
        return 1;
    }
    return test_provider("provider-ellekit", ellekit_target,
                         ellekit_replacement, 14, 71,
                         HK_EFFECT_PROVIDER_IMAGE_LOAD |
                             HK_EFFECT_PROVIDER_ACTIVATION |
                             HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
                         HK_EFFECT_TARGET_TEXT_MUTATION,
                         3, true, false);
}
