// HK3 provider adapter conformance through the real plan lifecycle. Vendor
// calls are seams here; the device smoke supplies Dobby/Gum themselves.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKInstalled.h"
#include "../../Sources/Core/HKOwnership.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "../../Sources/Engines/HKProviderVtable.h"

typedef struct {
    _Alignas(4) uint8_t entry[16];
    _Alignas(4) uint8_t page[HK_RELOC_PAGE_BYTES];
    bool available;
    bool valid;
    bool activate;
    bool hook_success;
    bool allow_null_original;
    bool original_visible_during_hook;
    void *original;
    void *visible_original;
    void *installed;
    unsigned discover_calls;
    unsigned validate_calls;
    unsigned activate_calls;
    unsigned hook_calls;
    unsigned verify_calls;
    unsigned read_calls;
    unsigned alloc_calls;
    unsigned seal_calls;
    unsigned free_calls;
} fake_provider_t;

static void fake_original(void) {}
static void fake_replacement(void) {}

static bool fake_discover(void *opaque) {
    fake_provider_t *fake = opaque;
    fake->discover_calls++;
    return fake->available;
}

static bool fake_validate(void *opaque, const hk_hook_spec_t *spec,
                          hk_prepare_diag_t *out_diag) {
    fake_provider_t *fake = opaque;
    fake->validate_calls++;
    if (!fake->valid || spec->target.address.address != (uintptr_t)fake->entry) {
        out_diag->error_message = "fake provider rejected target";
        return false;
    }
    return true;
}

static bool fake_activate_provider(void *opaque, hk_prepare_diag_t *out_diag) {
    fake_provider_t *fake = opaque;
    fake->activate_calls++;
    if (!fake->activate) {
        out_diag->error_message = "fake provider activation failed";
        return false;
    }
    out_diag->observed_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                HK_EFFECT_PROVIDER_ACTIVATION |
                                HK_EFFECT_UNKNOWN_PROCESS_MUTATION;
    return true;
}

static int fake_hook(void *opaque, void *target, void *replacement,
                     void **out_original) {
    fake_provider_t *fake = opaque;
    fake->hook_calls++;
    fake->original_visible_during_hook = fake->visible_original != NULL;
    if (!fake->hook_success || target != fake->entry ||
        replacement != (void *)fake_replacement ||
        (!out_original && !fake->allow_null_original)) {
        return -1;
    }
    fake->installed = replacement;
    if (out_original) {
        *out_original = fake->original;
    }
    return 0;
}

static uintptr_t fake_alloc(void *opaque, size_t size, uintptr_t near) {
    fake_provider_t *fake = opaque;
    (void)near;
    fake->alloc_calls++;
    return size <= sizeof(fake->page) ? (uintptr_t)fake->page : 0;
}

static bool fake_seal(void *opaque, uintptr_t page, size_t size) {
    fake_provider_t *fake = opaque;
    fake->seal_calls++;
    return page == (uintptr_t)fake->page && size == sizeof(fake->page);
}

static void fake_free(void *opaque, uintptr_t page, size_t size) {
    fake_provider_t *fake = opaque;
    assert(page == (uintptr_t)fake->page && size == sizeof(fake->page));
    fake->free_calls++;
}

static bool fake_verify(void *opaque, void *target, void *replacement) {
    fake_provider_t *fake = opaque;
    fake->verify_calls++;
    return target == fake->entry && fake->installed == replacement;
}

static bool fake_read(void *opaque, const void *target, uint8_t *out,
                      size_t size) {
    fake_provider_t *fake = opaque;
    fake->read_calls++;
    if (target != fake->entry || size > sizeof(fake->entry)) {
        return false;
    }
    memcpy(out, fake->entry, size);
    return true;
}

static fake_provider_t fake_provider(void) {
    fake_provider_t fake;
    memset(&fake, 0, sizeof(fake));
    const uint32_t nop = 0xD503201Fu;
    for (size_t i = 0; i < sizeof(fake.entry); i += sizeof(nop)) {
        memcpy(fake.entry + i, &nop, sizeof(nop));
    }
    fake.available = true;
    fake.valid = true;
    fake.activate = true;
    fake.hook_success = true;
    fake.original = (void *)fake_original;
    return fake;
}

static hk_provider_engine_ctx_t provider_context(hk_provider_kind_t kind,
                                                  fake_provider_t *fake,
                                                  bool include_activation,
                                                  bool include_verify) {
    hk_provider_engine_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.kind = kind;
    ctx.provider_ctx = fake;
    ctx.discover = fake_discover;
    ctx.prepare = include_activation ? fake_activate_provider : NULL;
    ctx.validate = fake_validate;
    ctx.hook = fake_hook;
    ctx.verify = include_verify ? fake_verify : NULL;
    if (kind == HK_PROVIDER_SUBSTITUTE) {
        ctx.read = fake_read;
    }
    if (kind == HK_PROVIDER_ELLEKIT) {
        fake->allow_null_original = true;
        ctx.max_overwrite_size = 16;
        ctx.alloc = fake_alloc;
        ctx.seal = fake_seal;
        ctx.free_page = fake_free;
        ctx.seam_ctx = fake;
    }
    return ctx;
}

static hk_hook_spec_t address_spec(const char *id, fake_provider_t *fake) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = (uintptr_t)fake->entry;
    spec.replacement = (void *)fake_replacement;
    spec.required_reach = HK_REACH_ENTRYPOINT;
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static bool snapshot_has(const hk_artifact_snapshot_t *snapshot,
                         hk_artifact_kind_t kind, hk_effects_t effects) {
    for (size_t i = 0; i < hk_artifact_snapshot_count(snapshot); i++) {
        hk_artifact_t artifact;
        if (hk_artifact_snapshot_copy_at(snapshot, i, &artifact) == HK_STATUS_OK &&
            artifact.kind == kind && artifact.effects == effects) {
            return true;
        }
    }
    return false;
}

static void reset_test_state(void) {
    hk_ownership_reset_for_testing();
    hk_installed_reset_for_testing();
}

static void test_dobby_full_lifecycle(void) {
    fake_provider_t fake = fake_provider();
    hk_provider_engine_ctx_t ctx =
        provider_context(HK_PROVIDER_DOBBY, &fake, false, true);
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_dobby_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = address_spec("provider.dobby.lifecycle", &fake);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_dobby_provider_vtable());
    assert(fake.discover_calls == 1 && fake.validate_calls == 1);

    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(fake.validate_calls == 2 && fake.activate_calls == 0);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hook->result.observed_prepare_effects == 0);
    assert(memcmp(fake.entry, (uint8_t[]){0x1f, 0x20, 0x03, 0xd5}, 4) == 0);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake.hook_calls == 1 && fake.verify_calls == 1);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hook->result.mutation == HK_MUTATION_COMPLETE);
    assert(hook->result.verified);
    assert(hook->result.original_available);
    assert(hk_original_slot_load(hk_hook_original_slot(hook)) == fake.original);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_DYNAMIC);
    assert(hook->result.continuation.address == (uintptr_t)fake.original);
    assert(hook->result.continuation.mapping_kind == HK_MAPPING_PROVIDER_OWNED);
    assert(!hook->result.continuation.fully_inspected);
    assert(hook->result.observed_commit_effects ==
           (HK_EFFECT_TARGET_TEXT_MUTATION | HK_EFFECT_EXECUTABLE_ALLOCATION |
            HK_EFFECT_PROVIDER_ACTIVATION | HK_EFFECT_UNKNOWN_PROCESS_MUTATION));

    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 4);
    assert(snapshot_has(snapshot, HK_ARTIFACT_PROVIDER_ACTIVATION,
                        HK_EFFECT_PROVIDER_ACTIVATION));
    assert(snapshot_has(snapshot, HK_ARTIFACT_UNKNOWN_PROCESS_MUTATION,
                        HK_EFFECT_UNKNOWN_PROCESS_MUTATION));
    assert(snapshot_has(snapshot, HK_ARTIFACT_TARGET_TEXT_PATCH,
                        HK_EFFECT_TARGET_TEXT_MUTATION));
    assert(snapshot_has(snapshot, HK_ARTIFACT_ORIGINAL_POINTER,
                        HK_EFFECT_EXECUTABLE_ALLOCATION));

    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();
    puts("  dobby-full-lifecycle: PASS");
}

static void test_gum_activation_and_constraints(void) {
    fake_provider_t fake = fake_provider();
    hk_provider_engine_ctx_t ctx =
        provider_context(HK_PROVIDER_GUM, &fake, true, false);
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_gum_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = address_spec("provider.gum.lifecycle", &fake);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_gum_provider_vtable());
    assert(fake.discover_calls == 1 && fake.activate_calls == 0);

    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(fake.activate_calls == 1);
    assert(hook->result.observed_prepare_effects ==
           (HK_EFFECT_PROVIDER_IMAGE_LOAD | HK_EFFECT_PROVIDER_ACTIVATION |
            HK_EFFECT_UNKNOWN_PROCESS_MUTATION));
    assert(memcmp(fake.entry, (uint8_t[]){0x1f, 0x20, 0x03, 0xd5}, 4) == 0);

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(!hook->result.verified);  // provider return code is not readback
    assert(hook->result.observed_commit_effects ==
           (HK_EFFECT_TARGET_TEXT_MUTATION | HK_EFFECT_EXECUTABLE_ALLOCATION));
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 2);
    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();

    runtime = NULL;
    plan = NULL;
    hook = NULL;
    fake = fake_provider();
    ctx = provider_context(HK_PROVIDER_GUM, &fake, true, false);
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_gum_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    spec = address_spec("provider.gum.forbidden", &fake);
    spec.constraints = HK_FORBID_PROVIDER_IMAGE_LOAD;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(fake.activate_calls == 0);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();
    puts("  gum-activation-and-constraints: PASS");
}

static void test_ellekit_uses_hookkit_hybrid_continuation(void) {
    fake_provider_t fake = fake_provider();
    hk_provider_engine_ctx_t ctx =
        provider_context(HK_PROVIDER_ELLEKIT, &fake, true, false);
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_ellekit_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = address_spec("provider.ellekit.none", &fake);
    spec.original_requirement = HK_ORIGINAL_NONE;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_ellekit_provider_vtable());
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(fake.activate_calls == 1);
    assert(hook->result.observed_prepare_effects ==
           (HK_EFFECT_PROVIDER_IMAGE_LOAD | HK_EFFECT_PROVIDER_ACTIVATION |
            HK_EFFECT_UNKNOWN_PROCESS_MUTATION));

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake.hook_calls == 1);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(!hook->result.original_available);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_NONE);
    assert(hook->result.observed_commit_effects == HK_EFFECT_TARGET_TEXT_MUTATION);
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 1);
    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();

    fake = fake_provider();
    ctx = provider_context(HK_PROVIDER_ELLEKIT, &fake, true, false);
    runtime = NULL;
    plan = NULL;
    hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_ellekit_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    spec = address_spec("provider.ellekit.original", &fake);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_ellekit_provider_vtable());
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(fake.alloc_calls == 1 && fake.seal_calls == 1);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_DYNAMIC);
    assert(hook->result.continuation.address != 0);
    assert(hook->result.continuation.jump_back_destination ==
           (uintptr_t)fake.entry + 16);
    assert(hook->result.continuation.mapping_kind == HK_MAPPING_ANONYMOUS);
    assert(hook->result.continuation.relocated_instruction_count == 4);
    assert(hook->result.continuation.fully_inspected);

    // This is what the canonical facade publishes after prepare. The fake
    // provider observes it while applying the patch, before it returns.
    fake.visible_original = (void *)hook->result.continuation.address;
    report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(fake.hook_calls == 1 && fake.original_visible_during_hook);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hook->result.original_available);
    assert(hk_original_slot_load(hk_hook_original_slot(hook)) ==
           fake.visible_original);
    assert(hook->result.continuation.address ==
           (uintptr_t)fake.visible_original);
    assert(hk_report_copy_artifacts(report, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 3);
    assert(snapshot_has(snapshot, HK_ARTIFACT_TRAMPOLINE,
                        HK_EFFECT_EXECUTABLE_ALLOCATION));
    assert(snapshot_has(snapshot, HK_ARTIFACT_ORIGINAL_POINTER,
                        HK_EFFECT_EXECUTABLE_ALLOCATION));
    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    assert(fake.free_calls == 0);
    hk_runtime_release(runtime);
    reset_test_state();
    puts("  ellekit-hookkit-hybrid-continuation: PASS");
}

static void test_stale_and_provider_failure_are_not_retried(void) {
    fake_provider_t fake = fake_provider();
    hk_provider_engine_ctx_t ctx =
        provider_context(HK_PROVIDER_DOBBY, &fake, false, false);
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_dobby_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = address_spec("provider.dobby.stale", &fake);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    fake.entry[0] ^= 0x1u;
    assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hook->result.mutation == HK_MUTATION_NONE);
    assert(fake.hook_calls == 0);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();

    fake = fake_provider();
    fake.hook_success = false;
    ctx = provider_context(HK_PROVIDER_DOBBY, &fake, false, false);
    runtime = NULL;
    plan = NULL;
    hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_dobby_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    spec = address_spec("provider.dobby.failure", &fake);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_UNKNOWN);
    assert(hook->result.mutation == HK_MUTATION_UNKNOWN);
    assert(fake.hook_calls == 1);
    assert(hook->result.observed_commit_effects == HK_EFFECT_UNKNOWN_PROCESS_MUTATION);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();
    puts("  stale-and-provider-failure-are-not-retried: PASS");
}

static void test_dynamic_continuation_policy_refuses_route(void) {
    fake_provider_t fake = fake_provider();
    hk_provider_engine_ctx_t ctx =
        provider_context(HK_PROVIDER_DOBBY, &fake, false, false);
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime,
                                                    hk_dobby_provider_vtable(),
                                                    &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = address_spec("provider.dobby.no-dynamic", &fake);
    spec.continuation_policy = HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(fake.activate_calls == 0 && fake.hook_calls == 0);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();
    puts("  dynamic-continuation-policy-refuses-route: PASS");
}

static void test_substitute_old_lane_lifecycle(void) {
    fake_provider_t fake = fake_provider();
    hk_provider_engine_ctx_t ctx =
        provider_context(HK_PROVIDER_SUBSTITUTE, &fake, true, true);
    const hk_engine_vtable_t *vtable = hk_substitute_provider_vtable();
    hk_engine_capabilities_t caps = vtable->describe();
    assert((caps.architectures & (HK_ENGINE_ARCHITECTURE_ARMV7 |
                                  HK_ENGINE_ARCHITECTURE_ARMV7S)) ==
           (HK_ENGINE_ARCHITECTURE_ARMV7 | HK_ENGINE_ARCHITECTURE_ARMV7S));
    assert(caps.minimum_ios_version == HK_ENGINE_IOS_VERSION(9, 0, 0));

    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(runtime, vtable, &ctx));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = address_spec("provider.substitute.old-lane", &fake);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == vtable && fake.discover_calls == 1);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(fake.read_calls == 1 && fake.activate_calls == 1);

    assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
    assert(fake.read_calls == 2 && fake.hook_calls == 1 &&
           fake.verify_calls == 1);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE &&
           hk_original_slot_load(hk_hook_original_slot(hook)) == fake.original);

    hk_plan_release(plan);
    hk_runtime_release(runtime);
    reset_test_state();
    puts("  substitute-old-lane-lifecycle: PASS");
}

int main(void) {
    test_dobby_full_lifecycle();
    test_gum_activation_and_constraints();
    test_ellekit_uses_hookkit_hybrid_continuation();
    test_stale_and_provider_failure_are_not_retried();
    test_dynamic_continuation_policy_refuses_route();
    test_substitute_old_lane_lifecycle();
    puts("provider vtable tests: PASS");
    return 0;
}
