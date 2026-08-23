// Provider inline adapter. See HKProviderVtable.h for the intentionally small
// seam between the common HookKit lifecycle and audited vendor call shapes.

#include "HKProviderVtable.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    hk_provider_kind_t kind;
    const char *engine_id;
    const char *mechanism_id;
    size_t inspection_size;
    hk_engine_architecture_mask_t architectures;
    hk_engine_architecture_mask_t certified_architectures;
    uint32_t minimum_ios_version;
    hk_install_context_mask_t install_contexts;
    hk_original_requirement_mask_t original_requirements;
    hk_effects_t prepare_effects;
    hk_effects_t commit_effects;
    bool activation_happens_at_commit;
    bool publishes_original_before_activation;
} provider_profile_t;

static const provider_profile_t g_dobby_profile = {
    .kind = HK_PROVIDER_DOBBY,
    .engine_id = "provider-dobby",
    .mechanism_id = "DobbyHook",
    .inspection_size = 16,
    .architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                     HK_ENGINE_ARCHITECTURE_ARM64E,
    .certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                                 HK_ENGINE_ARCHITECTURE_ARM64E,
    .minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0),
    .install_contexts = HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_EARLY_PROCESS) |
                        HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED),
    .original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE) |
                             HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_CALLABLE_CONTINUATION),
    // Dobby is compiled in, so no image is loaded during preparation. Its
    // first hook can initialize provider-global state, and its audited
    // constructor behavior is intentionally reported as unknown.
    .prepare_effects = 0,
    .commit_effects = HK_EFFECT_TARGET_TEXT_MUTATION |
                      HK_EFFECT_EXECUTABLE_ALLOCATION |
                      HK_EFFECT_PROVIDER_ACTIVATION |
                      HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .activation_happens_at_commit = true,
    .publishes_original_before_activation = true,
};

static const provider_profile_t g_gum_profile = {
    .kind = HK_PROVIDER_GUM,
    .engine_id = "provider-gum",
    .mechanism_id = "hkgum_hook_function",
    .inspection_size = 4,
    .architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                     HK_ENGINE_ARCHITECTURE_ARM64E,
    .certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                                 HK_ENGINE_ARCHITECTURE_ARM64E,
    .minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0),
    .install_contexts = HK_INSTALL_CONTEXT_ALL,
    .original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE) |
                             HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_CALLABLE_CONTINUATION),
    // dlopen may run Gum's provider initialization. The hook itself then
    // changes target text and obtains the provider's dynamic continuation.
    .prepare_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                       HK_EFFECT_PROVIDER_ACTIVATION |
                       HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .commit_effects = HK_EFFECT_TARGET_TEXT_MUTATION |
                      HK_EFFECT_EXECUTABLE_ALLOCATION |
                      HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .activation_happens_at_commit = false,
    .publishes_original_before_activation = true,
};

static const provider_profile_t g_ellekit_profile = {
    .kind = HK_PROVIDER_ELLEKIT,
    .engine_id = "provider-ellekit",
    .mechanism_id = "LHHookFunctions",
    .inspection_size = 4,
    .architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                     HK_ENGINE_ARCHITECTURE_ARM64E,
    .certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                                 HK_ENGINE_ARCHITECTURE_ARM64E,
    .minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0),
    .install_contexts = HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_EARLY_PROCESS) |
                        HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED),
    // ElleKit writes oldptr after making the replacement reachable. For a
    // requested original, HookKit therefore builds its own continuation and
    // invokes ElleKit's documented no-oldptr mode for the target patch.
    .original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE) |
                             HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_CALLABLE_CONTINUATION),
    .prepare_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                       HK_EFFECT_PROVIDER_ACTIVATION |
                       HK_EFFECT_EXECUTABLE_ALLOCATION |
                       HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .commit_effects = HK_EFFECT_TARGET_TEXT_MUTATION |
                      HK_EFFECT_EXECUTABLE_ALLOCATION |
                      HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .activation_happens_at_commit = false,
    .publishes_original_before_activation = false,
};

static const provider_profile_t g_substitute_profile = {
    .kind = HK_PROVIDER_SUBSTITUTE,
    .engine_id = "provider-substitute",
    .mechanism_id = "substitute_hook_functions/MSHookFunction",
    .inspection_size = 4,
    .architectures = HK_ENGINE_ARCHITECTURE_ARMV7 |
                     HK_ENGINE_ARCHITECTURE_ARMV7S |
                     HK_ENGINE_ARCHITECTURE_ARM64 |
                     HK_ENGINE_ARCHITECTURE_ARM64E,
    .certified_architectures = HK_ENGINE_ARCHITECTURE_ARMV7 |
                                 HK_ENGINE_ARCHITECTURE_ARMV7S |
                                 HK_ENGINE_ARCHITECTURE_ARM64 |
                                 HK_ENGINE_ARCHITECTURE_ARM64E,
    .minimum_ios_version = HK_ENGINE_IOS_VERSION(9, 0, 0),
    .install_contexts = HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_EARLY_PROCESS) |
                        HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED),
    .original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE) |
                             HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_CALLABLE_CONTINUATION),
    .prepare_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                       HK_EFFECT_PROVIDER_ACTIVATION |
                       HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .commit_effects = HK_EFFECT_TARGET_TEXT_MUTATION |
                      HK_EFFECT_EXECUTABLE_ALLOCATION |
                      HK_EFFECT_UNKNOWN_PROCESS_MUTATION,
    .activation_happens_at_commit = false,
    .publishes_original_before_activation = true,
};

typedef struct {
    uintptr_t target;
    void *replacement;
    size_t inspection_size;
    uint8_t inspected[HK_RELOC_MAX_PATCH];
    bool hybrid;
    bool provider_called;
    hk_reloc_plan_t continuation;
} provider_prepared_t;

static const hk_provider_engine_ctx_t *provider_context(void *engine_ctx,
                                                         hk_provider_kind_t kind) {
    const hk_provider_engine_ctx_t *ctx = engine_ctx;
    return ctx && ctx->kind == kind ? ctx : NULL;
}

static bool provider_hybrid_available(const hk_provider_engine_ctx_t *ctx) {
    return ctx && ctx->max_overwrite_size != 0 &&
           ctx->max_overwrite_size <= HK_RELOC_MAX_PATCH &&
           (ctx->max_overwrite_size & 3u) == 0 && ctx->alloc && ctx->seal;
}

static bool provider_needs_hybrid(const provider_profile_t *profile,
                                  const hk_hook_spec_t *spec) {
    return profile && spec && !profile->publishes_original_before_activation &&
           spec->original_requirement == HK_ORIGINAL_CALLABLE_CONTINUATION;
}

static hk_engine_capabilities_t provider_describe(const provider_profile_t *profile) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = profile->engine_id;
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    // The provider ABIs are identical across the two AArch64 slices; the
    // late-publishing ElleKit mode is safe only when analysis selects the
    // bounded HookKit continuation path below.
    caps.architectures = profile->architectures;
    caps.certified_architectures = profile->certified_architectures;
    caps.minimum_ios_version = profile->minimum_ios_version;
    caps.install_contexts = profile->install_contexts;
    caps.achievable_reach = HK_REACH_ENTRYPOINT;
    // Vendor out_original values are callable trampolines, never an untouched
    // predecessor. A caller needing a direct predecessor must use another
    // mechanism rather than receive a plausible-but-wrong pointer.
    caps.original_requirements = profile->original_requirements;
    caps.prepare_effects = profile->prepare_effects;
    caps.commit_effects = profile->commit_effects;
    return caps;
}

static hk_engine_capabilities_t dobby_describe(void) {
    return provider_describe(&g_dobby_profile);
}

static hk_engine_capabilities_t gum_describe(void) {
    return provider_describe(&g_gum_profile);
}

static hk_engine_capabilities_t ellekit_describe(void) {
    return provider_describe(&g_ellekit_profile);
}

static hk_engine_capabilities_t substitute_describe(void) {
    return provider_describe(&g_substitute_profile);
}

static bool provider_discover(void *engine_ctx, hk_engine_discovery_t *out,
                              hk_provider_kind_t kind) {
    const hk_provider_engine_ctx_t *ctx = provider_context(engine_ctx, kind);
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!ctx || !out) {
        return false;
    }
    out->available = !ctx->discover || ctx->discover(ctx->provider_ctx);
    if (!out->available) {
        out->error_message = "provider is unavailable without activation";
    }
    return true;
}

static bool dobby_discover(void *engine_ctx, hk_engine_discovery_t *out) {
    return provider_discover(engine_ctx, out, HK_PROVIDER_DOBBY);
}

static bool gum_discover(void *engine_ctx, hk_engine_discovery_t *out) {
    return provider_discover(engine_ctx, out, HK_PROVIDER_GUM);
}

static bool ellekit_discover(void *engine_ctx, hk_engine_discovery_t *out) {
    return provider_discover(engine_ctx, out, HK_PROVIDER_ELLEKIT);
}

static bool substitute_discover(void *engine_ctx, hk_engine_discovery_t *out) {
    return provider_discover(engine_ctx, out, HK_PROVIDER_SUBSTITUTE);
}

static bool provider_analyze(void *engine_ctx, const hk_hook_spec_t *spec,
                             hk_engine_analysis_t *out,
                             const provider_profile_t *profile) {
    const hk_provider_engine_ctx_t *ctx =
        provider_context(engine_ctx, profile->kind);
    if (!ctx || !spec || !out || spec->target_kind != HK_TARGET_FUNCTION_ADDRESS ||
        !spec->replacement || spec->target.address.address == 0 || !ctx->hook) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const bool hybrid = provider_needs_hybrid(profile, spec);
    const size_t inspection_size = hybrid
        ? ctx->max_overwrite_size : profile->inspection_size;
    if (!(profile->original_requirements &
          HK_ORIGINAL_REQ_BIT(spec->original_requirement)) ||
        spec->continuation_policy != HK_CONTINUATION_ANY ||
        (hybrid && !provider_hybrid_available(ctx)) ||
        spec->target.address.expected_initial_bytes_size > inspection_size) {
        return true;
    }

    // A provider-specific preflight is safe during analysis: it only reads
    // the requested addresses. Running it here lets Gum remain a candidate if
    // Dobby's fixed-window relocator honestly refuses a target.
    if (ctx->validate) {
        hk_prepare_diag_t diag;
        memset(&diag, 0, sizeof(diag));
        if (!ctx->validate(ctx->provider_ctx, spec, &diag)) {
            return true;
        }
    }

    out->eligible = true;
    out->achieved_reach = HK_REACH_ENTRYPOINT;
    out->required_prepare_effects = profile->prepare_effects;
    out->required_commit_effects = profile->commit_effects;
    out->continuation_kind = hybrid || profile->publishes_original_before_activation
        ? HK_CONTINUATION_KIND_DYNAMIC : HK_CONTINUATION_KIND_NONE;
    return true;
}

static bool dobby_analyze(void *engine_ctx, const hk_hook_spec_t *spec,
                          hk_engine_analysis_t *out) {
    return provider_analyze(engine_ctx, spec, out, &g_dobby_profile);
}

static bool gum_analyze(void *engine_ctx, const hk_hook_spec_t *spec,
                        hk_engine_analysis_t *out) {
    return provider_analyze(engine_ctx, spec, out, &g_gum_profile);
}

static bool ellekit_analyze(void *engine_ctx, const hk_hook_spec_t *spec,
                            hk_engine_analysis_t *out) {
    return provider_analyze(engine_ctx, spec, out, &g_ellekit_profile);
}

static bool substitute_analyze(void *engine_ctx, const hk_hook_spec_t *spec,
                               hk_engine_analysis_t *out) {
    return provider_analyze(engine_ctx, spec, out, &g_substitute_profile);
}

static hk_prepare_result_t provider_prepare(void *engine_ctx,
                                             const hk_hook_spec_t *spec,
                                             void **out_prepared,
                                             hk_prepare_diag_t *out_diag,
                                             const provider_profile_t *profile) {
    const hk_provider_engine_ctx_t *ctx =
        provider_context(engine_ctx, profile->kind);
    if (!ctx || !spec || !out_prepared || !out_diag ||
        spec->target_kind != HK_TARGET_FUNCTION_ADDRESS ||
        !spec->replacement || spec->target.address.address == 0 || !ctx->hook) {
        out_diag->error_message = "provider adapter received invalid hook state";
        return HK_PREPARE_FAILED;
    }
    if (!(profile->original_requirements &
          HK_ORIGINAL_REQ_BIT(spec->original_requirement))) {
        out_diag->error_message = "provider cannot publish the requested original";
        return HK_PREPARE_FAILED;
    }
    const bool hybrid = provider_needs_hybrid(profile, spec);
    if (hybrid && !provider_hybrid_available(ctx)) {
        out_diag->error_message =
            "provider has no audited overwrite bound for a HookKit continuation";
        return HK_PREPARE_FAILED;
    }
    const size_t inspection_size = hybrid
        ? ctx->max_overwrite_size : profile->inspection_size;
    if (inspection_size > HK_RELOC_MAX_PATCH ||
        spec->target.address.expected_initial_bytes_size > inspection_size) {
        out_diag->error_message =
            "provider cannot revalidate a precondition larger than its inspected entry window";
        return HK_PREPARE_FAILED;
    }
    if (ctx->validate && !ctx->validate(ctx->provider_ctx, spec, out_diag)) {
        if (!out_diag->error_message) {
            out_diag->error_message = "provider preflight refused the target";
        }
        return HK_PREPARE_FAILED;
    }
    if (spec->target.address.expected_initial_bytes_size != 0 &&
        (!spec->target.address.expected_initial_bytes ||
         memcmp((const void *)spec->target.address.address,
                spec->target.address.expected_initial_bytes,
                spec->target.address.expected_initial_bytes_size) != 0)) {
        out_diag->error_message = "target does not match the request's expected entry bytes";
        return HK_PREPARE_FAILED;
    }
    provider_prepared_t *prepared = calloc(1, sizeof(*prepared));
    if (!prepared) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
    }
    prepared->target = spec->target.address.address;
    prepared->replacement = spec->replacement;
    prepared->inspection_size = inspection_size;
    prepared->hybrid = hybrid;
    // Capture before activation and compare immediately before the vendor
    // call.  The old provider adapter reads through Mach VM so a malformed
    // Thumb pointer cannot fault the process; established providers retain
    // their direct, preflight-proven path.
    if (prepared->inspection_size != 0 &&
        (ctx->read
            ? !ctx->read(ctx->provider_ctx, (const void *)prepared->target,
                         prepared->inspected, prepared->inspection_size)
            : (memcpy(prepared->inspected, (const void *)prepared->target,
                      prepared->inspection_size), false))) {
        out_diag->error_message = "provider entry bytes are unreadable";
        free(prepared);
        return HK_PREPARE_FAILED;
    }

    if (hybrid) {
        hk_reloc_status_t reloc = hk_reloc_prepare_continuation(
            prepared->target, ctx->max_overwrite_size,
            spec->target.address.expected_initial_bytes,
            spec->target.address.expected_initial_bytes_size,
            ctx->alloc, ctx->seal, ctx->free_page, ctx->seam_ctx,
            &prepared->continuation);
        if (reloc != HK_RELOC_OK) {
            out_diag->error_code = reloc;
            out_diag->error_message =
                "HookKit cannot relocate the provider's overwrite window";
            free(prepared);
            return HK_PREPARE_FAILED;
        }
    }

    if (ctx->prepare && !ctx->prepare(ctx->provider_ctx, out_diag)) {
        if (!out_diag->error_message) {
            out_diag->error_message = "provider activation failed";
        }
        if (hybrid && ctx->free_page) {
            ctx->free_page(ctx->seam_ctx, prepared->continuation.trampoline,
                           prepared->continuation.trampoline_size);
        }
        free(prepared);
        return HK_PREPARE_FAILED;
    }
    if (hybrid) {
        out_diag->observed_effects |= HK_EFFECT_EXECUTABLE_ALLOCATION;
    }
    *out_prepared = prepared;
    return HK_PREPARE_OK;
}

static hk_prepare_result_t dobby_prepare(void *engine_ctx,
                                         const hk_hook_spec_t *spec,
                                         void **out_prepared,
                                         hk_prepare_diag_t *out_diag) {
    return provider_prepare(engine_ctx, spec, out_prepared, out_diag,
                            &g_dobby_profile);
}

static hk_prepare_result_t gum_prepare(void *engine_ctx,
                                       const hk_hook_spec_t *spec,
                                       void **out_prepared,
                                       hk_prepare_diag_t *out_diag) {
    return provider_prepare(engine_ctx, spec, out_prepared, out_diag,
                            &g_gum_profile);
}

static hk_prepare_result_t ellekit_prepare(void *engine_ctx,
                                           const hk_hook_spec_t *spec,
                                           void **out_prepared,
                                           hk_prepare_diag_t *out_diag) {
    return provider_prepare(engine_ctx, spec, out_prepared, out_diag,
                            &g_ellekit_profile);
}

static hk_prepare_result_t substitute_prepare(void *engine_ctx,
                                              const hk_hook_spec_t *spec,
                                              void **out_prepared,
                                              hk_prepare_diag_t *out_diag) {
    return provider_prepare(engine_ctx, spec, out_prepared, out_diag,
                            &g_substitute_profile);
}

static void provider_record(hk_artifact_sink_t *sink,
                            const provider_profile_t *profile,
                            hk_artifact_kind_t kind,
                            hk_effects_t effects,
                            const provider_prepared_t *prepared,
                            const hk_hook_spec_t *spec,
                            void *original) {
    if (!sink) {
        return;
    }
    hk_artifact_t artifact;
    memset(&artifact, 0, sizeof(artifact));
    artifact.struct_size = sizeof(artifact);
    artifact.struct_version = HK_ABI_VERSION_3_0;
    artifact.kind = kind;
    artifact.state = HK_ARTIFACT_COMMITTED;
    artifact.effects = effects;
    artifact.engine_id.data = profile->engine_id;
    artifact.engine_id.length = strlen(profile->engine_id);
    artifact.mechanism_id.data = profile->mechanism_id;
    artifact.mechanism_id.length = strlen(profile->mechanism_id);
    if (prepared) {
        artifact.address = prepared->target;
    }
    if (spec) {
        artifact.replacement_pointer = spec->replacement;
    }
    artifact.original_pointer = original;
    if (kind == HK_ARTIFACT_TARGET_TEXT_PATCH && prepared && prepared->hybrid) {
        artifact.size = prepared->continuation.patch_size;
        artifact.original_bytes.representation = HK_BYTE_STORAGE_INLINE;
        artifact.original_bytes.inline_bytes.data = prepared->continuation.original;
        artifact.original_bytes.inline_bytes.size = prepared->continuation.patch_size;
        artifact.original_bytes.length = prepared->continuation.patch_size;
    }
    if (kind == HK_ARTIFACT_ORIGINAL_POINTER) {
        artifact.continuation_address = (uintptr_t)original;
        artifact.mapping.struct_size = sizeof(artifact.mapping);
        artifact.mapping.struct_version = HK_ABI_VERSION_3_0;
        if (prepared && prepared->hybrid) {
            artifact.mapping.kind = HK_MAPPING_ANONYMOUS;
            artifact.mapping.mapping_id = prepared->continuation.mapping_id;
            artifact.mapping.base = prepared->continuation.trampoline;
            artifact.mapping.size = prepared->continuation.trampoline_size;
            artifact.fully_inspected = true;
        } else {
            artifact.mapping.kind = HK_MAPPING_PROVIDER_OWNED;
            // The provider returned a callable pointer but not its allocation
            // metadata. Preserve that boundary rather than guessing a mapping.
            artifact.fully_inspected = false;
        }
    }
    (void)hk_artifact_sink_record(sink, &artifact);
}

static void provider_record_hybrid_continuation(
    hk_artifact_sink_t *sink, const provider_profile_t *profile,
    const provider_prepared_t *prepared) {
    if (!sink || !prepared || !prepared->hybrid) {
        return;
    }
    hk_artifact_t artifact;
    memset(&artifact, 0, sizeof(artifact));
    artifact.struct_size = sizeof(artifact);
    artifact.struct_version = HK_ABI_VERSION_3_0;
    artifact.kind = HK_ARTIFACT_TRAMPOLINE;
    artifact.state = HK_ARTIFACT_COMMITTED;
    artifact.effects = HK_EFFECT_EXECUTABLE_ALLOCATION;
    artifact.engine_id.data = profile->engine_id;
    artifact.engine_id.length = strlen(profile->engine_id);
    artifact.mechanism_id.data = "hookkit-provider-hybrid";
    artifact.mechanism_id.length = 23;
    artifact.address = prepared->continuation.trampoline;
    artifact.size = prepared->continuation.trampoline_size;
    artifact.continuation_address = prepared->continuation.original_entry;
    artifact.jump_back_destination = prepared->target +
                                     prepared->continuation.patch_size;
    artifact.mapping.struct_size = sizeof(artifact.mapping);
    artifact.mapping.struct_version = HK_ABI_VERSION_3_0;
    artifact.mapping.kind = HK_MAPPING_ANONYMOUS;
    artifact.mapping.mapping_id = prepared->continuation.mapping_id;
    artifact.mapping.base = prepared->continuation.trampoline;
    artifact.mapping.size = prepared->continuation.trampoline_size;
    artifact.current_protection.read = true;
    artifact.current_protection.execute = true;
    artifact.fully_inspected = true;
    (void)hk_artifact_sink_record(sink, &artifact);
}

static void provider_fill_continuation(hk_artifact_sink_t *sink,
                                       void *original) {
    if (!sink) {
        return;
    }
    memset(&sink->continuation, 0, sizeof(sink->continuation));
    sink->continuation.struct_size = sizeof(sink->continuation);
    sink->continuation.struct_version = HK_ABI_VERSION_3_0;
    sink->continuation.kind = HK_CONTINUATION_KIND_DYNAMIC;
    sink->continuation.address = (uintptr_t)original;
    sink->continuation.mapping_kind = HK_MAPPING_PROVIDER_OWNED;
    sink->continuation.executable_memory_allocated = true;
    // No provider metadata makes the mapping safely reversible or fully
    // inspectable through this interface.
    sink->continuation.mechanically_reversible = false;
    sink->continuation.safe_to_reverse_after_activation = false;
    sink->continuation.fully_inspected = false;
    sink->has_continuation = true;
}

static void provider_fill_hybrid_continuation(
    hk_artifact_sink_t *sink, const provider_prepared_t *prepared) {
    if (!sink || !prepared || !prepared->hybrid) {
        return;
    }
    hk_reloc_describe_continuation(&prepared->continuation, false,
                                   &sink->continuation);
    sink->has_continuation = true;
}

static void provider_record_unknown(hk_artifact_sink_t *sink,
                                    const provider_profile_t *profile,
                                    const provider_prepared_t *prepared,
                                    const hk_hook_spec_t *spec) {
    provider_record(sink, profile, HK_ARTIFACT_UNKNOWN_PROCESS_MUTATION,
                    HK_EFFECT_UNKNOWN_PROCESS_MUTATION, prepared, spec, NULL);
}

static hk_mutation_state_t provider_commit(void *engine_ctx,
                                            const hk_hook_spec_t *spec,
                                            void *prepared_state,
                                            hk_artifact_sink_t *sink,
                                            const provider_profile_t *profile) {
    const hk_provider_engine_ctx_t *ctx =
        provider_context(engine_ctx, profile->kind);
    provider_prepared_t *prepared = prepared_state;
    if (!ctx || !spec || !prepared || !ctx->hook ||
        spec->target_kind != HK_TARGET_FUNCTION_ADDRESS ||
        prepared->target != spec->target.address.address ||
        prepared->replacement != spec->replacement ||
        !(profile->original_requirements &
          HK_ORIGINAL_REQ_BIT(spec->original_requirement)) ||
        (sink && sink->require_predecessor_match)) {
        return HK_MUTATION_NONE;
    }
    if (prepared->inspection_size != 0) {
        uint8_t current[HK_RELOC_MAX_PATCH];
        bool read_ok = ctx->read
            ? ctx->read(ctx->provider_ctx, (const void *)prepared->target,
                        current, prepared->inspection_size)
            : (memcpy(current, (const void *)prepared->target,
                      prepared->inspection_size), true);
        if (!read_ok || memcmp(current, prepared->inspected,
                               prepared->inspection_size) != 0) {
            return HK_MUTATION_NONE;
        }
    }

    // A hybrid original is complete before this call. Publish it to the
    // lifecycle sink first, then ask the provider to perform only its patch.
    // Providers with audited early publication keep their native output cell.
    const bool hybrid = prepared->hybrid;
    bool publish_original =
        spec->original_requirement == HK_ORIGINAL_CALLABLE_CONTINUATION &&
        (profile->publishes_original_before_activation || hybrid);
    void *local_original = NULL;
    if (hybrid && sink) {
        sink->published_original = (void *)prepared->continuation.original_entry;
        provider_fill_hybrid_continuation(sink, prepared);
    }
    void **out_original = hybrid || !profile->publishes_original_before_activation
        ? NULL : (publish_original && sink
            ? &sink->published_original : &local_original);
    prepared->provider_called = true;
    if (ctx->hook(ctx->provider_ctx, (void *)prepared->target,
                  spec->replacement, out_original) != 0 ||
        (out_original && !*out_original)) {
        // The provider's documented failure semantics permit a partial patch.
        // UNKNOWN prevents fallback and records the one fact we can prove.
        if (hybrid) {
            provider_record_hybrid_continuation(sink, profile, prepared);
        }
        provider_record_unknown(sink, profile, prepared, spec);
        return HK_MUTATION_UNKNOWN;
    }
    void *original = hybrid
        ? (void *)prepared->continuation.original_entry
        : (out_original ? *out_original : NULL);

    if (profile->activation_happens_at_commit) {
        provider_record(sink, profile, HK_ARTIFACT_PROVIDER_ACTIVATION,
                        HK_EFFECT_PROVIDER_ACTIVATION, prepared, spec, NULL);
        provider_record_unknown(sink, profile, prepared, spec);
    }
    if (hybrid) {
        provider_record_hybrid_continuation(sink, profile, prepared);
    }
    provider_record(sink, profile, HK_ARTIFACT_TARGET_TEXT_PATCH,
                    HK_EFFECT_TARGET_TEXT_MUTATION, prepared, spec, original);
    if (original) {
        provider_record(sink, profile, HK_ARTIFACT_ORIGINAL_POINTER,
                        HK_EFFECT_EXECUTABLE_ALLOCATION, prepared, spec, original);
    }
    if (sink) {
        sink->published_original = publish_original ? original : NULL;
        if (hybrid) {
            provider_fill_hybrid_continuation(sink, prepared);
        } else if (original) {
            provider_fill_continuation(sink, original);
        }
    }
    return HK_MUTATION_COMPLETE;
}

static hk_mutation_state_t dobby_commit(void *engine_ctx,
                                        const hk_hook_spec_t *spec,
                                        void *prepared,
                                        hk_artifact_sink_t *sink) {
    return provider_commit(engine_ctx, spec, prepared, sink, &g_dobby_profile);
}

static hk_mutation_state_t gum_commit(void *engine_ctx,
                                      const hk_hook_spec_t *spec,
                                      void *prepared,
                                      hk_artifact_sink_t *sink) {
    return provider_commit(engine_ctx, spec, prepared, sink, &g_gum_profile);
}

static hk_mutation_state_t ellekit_commit(void *engine_ctx,
                                          const hk_hook_spec_t *spec,
                                          void *prepared,
                                          hk_artifact_sink_t *sink) {
    return provider_commit(engine_ctx, spec, prepared, sink, &g_ellekit_profile);
}

static hk_mutation_state_t substitute_commit(void *engine_ctx,
                                             const hk_hook_spec_t *spec,
                                             void *prepared,
                                             hk_artifact_sink_t *sink) {
    return provider_commit(engine_ctx, spec, prepared, sink,
                           &g_substitute_profile);
}

static hk_verify_result_t provider_verify(void *engine_ctx,
                                          const hk_hook_spec_t *spec,
                                          void *prepared_state,
                                          hk_verify_diag_t *out_diag,
                                          hk_provider_kind_t kind) {
    const hk_provider_engine_ctx_t *ctx = provider_context(engine_ctx, kind);
    const provider_prepared_t *prepared = prepared_state;
    if (!ctx || !spec || !prepared || prepared->target != spec->target.address.address ||
        prepared->replacement != spec->replacement) {
        out_diag->error_message = "provider verification received invalid prepared state";
        return HK_VERIFY_FAILED;
    }
    if (!ctx->verify) {
        // Provider callbacks may report success, but without this optional
        // readback the lifecycle has no independent target verification.
        return HK_VERIFY_UNAVAILABLE;
    }
    if (!ctx->verify(ctx->provider_ctx, (void *)prepared->target,
                     spec->replacement)) {
        out_diag->error_message = "provider readback does not match replacement";
        return HK_VERIFY_FAILED;
    }
    return HK_VERIFY_OK;
}

static hk_verify_result_t dobby_verify(void *engine_ctx,
                                       const hk_hook_spec_t *spec,
                                       void *prepared,
                                       hk_verify_diag_t *out_diag) {
    return provider_verify(engine_ctx, spec, prepared, out_diag, HK_PROVIDER_DOBBY);
}

static hk_verify_result_t gum_verify(void *engine_ctx,
                                     const hk_hook_spec_t *spec,
                                     void *prepared,
                                     hk_verify_diag_t *out_diag) {
    return provider_verify(engine_ctx, spec, prepared, out_diag, HK_PROVIDER_GUM);
}

static hk_verify_result_t ellekit_verify(void *engine_ctx,
                                         const hk_hook_spec_t *spec,
                                         void *prepared,
                                         hk_verify_diag_t *out_diag) {
    return provider_verify(engine_ctx, spec, prepared, out_diag, HK_PROVIDER_ELLEKIT);
}

static hk_verify_result_t substitute_verify(void *engine_ctx,
                                            const hk_hook_spec_t *spec,
                                            void *prepared,
                                            hk_verify_diag_t *out_diag) {
    return provider_verify(engine_ctx, spec, prepared, out_diag,
                           HK_PROVIDER_SUBSTITUTE);
}

static hk_verify_result_t provider_inspect_continuation(
    void *engine_ctx, const hk_hook_spec_t *spec, void *prepared_state,
    hk_continuation_info_t *out_info, hk_verify_diag_t *out_diag) {
    const hk_provider_engine_ctx_t *ctx = engine_ctx;
    const provider_prepared_t *prepared = prepared_state;
    if (!ctx || !spec || !prepared || !out_info ||
        prepared->target != spec->target.address.address) {
        out_diag->error_message =
            "provider continuation inspection received invalid prepared state";
        return HK_VERIFY_FAILED;
    }
    if (!prepared->hybrid) {
        return HK_VERIFY_UNAVAILABLE;
    }
    hk_reloc_describe_continuation(&prepared->continuation, false, out_info);
    return HK_VERIFY_OK;
}

static void provider_release(void *engine_ctx, void *prepared_state) {
    const hk_provider_engine_ctx_t *ctx = engine_ctx;
    provider_prepared_t *prepared = prepared_state;
    if (ctx && prepared && prepared->hybrid && !prepared->provider_called &&
        ctx->free_page && prepared->continuation.trampoline) {
        ctx->free_page(ctx->seam_ctx, prepared->continuation.trampoline,
                       prepared->continuation.trampoline_size);
    }
    free(prepared);
}

static const hk_engine_vtable_t g_dobby_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = dobby_describe,
    .discover = dobby_discover,
    .analyze_operation = dobby_analyze,
    .prepare_one_ctx_status = dobby_prepare,
    .commit_one_ctx = dobby_commit,
    .verify_one_ctx = dobby_verify,
    .release_prepared = provider_release,
    .inspect_continuation = provider_inspect_continuation,
};

static const hk_engine_vtable_t g_gum_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = gum_describe,
    .discover = gum_discover,
    .analyze_operation = gum_analyze,
    .prepare_one_ctx_status = gum_prepare,
    .commit_one_ctx = gum_commit,
    .verify_one_ctx = gum_verify,
    .release_prepared = provider_release,
    .inspect_continuation = provider_inspect_continuation,
};

static const hk_engine_vtable_t g_ellekit_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = ellekit_describe,
    .discover = ellekit_discover,
    .analyze_operation = ellekit_analyze,
    .prepare_one_ctx_status = ellekit_prepare,
    .commit_one_ctx = ellekit_commit,
    .verify_one_ctx = ellekit_verify,
    .release_prepared = provider_release,
    .inspect_continuation = provider_inspect_continuation,
};

static const hk_engine_vtable_t g_substitute_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = substitute_describe,
    .discover = substitute_discover,
    .analyze_operation = substitute_analyze,
    .prepare_one_ctx_status = substitute_prepare,
    .commit_one_ctx = substitute_commit,
    .verify_one_ctx = substitute_verify,
    .release_prepared = provider_release,
    .inspect_continuation = provider_inspect_continuation,
};

const hk_engine_vtable_t *hk_dobby_provider_vtable(void) {
    return &g_dobby_vtable;
}

const hk_engine_vtable_t *hk_gum_provider_vtable(void) {
    return &g_gum_vtable;
}

const hk_engine_vtable_t *hk_ellekit_provider_vtable(void) {
    return &g_ellekit_vtable;
}

const hk_engine_vtable_t *hk_substitute_provider_vtable(void) {
    return &g_substitute_vtable;
}
