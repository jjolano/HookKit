// Canonical facade bridge. Keep this deliberately boring: one legacy call is
// one 3.0 runtime/plan lifecycle, so there is no second state machine to
// drift from the public ABI.

#include "HKLegacyFacade.h"

#include "../../Headers/HookKit/HookKit.h"
#include "../Core/HKRuntimeInternal.h"
#include "../../native/hk_swift.h"
#include "../../native/hk_native.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

static int map_status(hk_status_t status) {
    switch (status) {
        case HK_STATUS_INVALID_ARGUMENT:
            return HK_LEGACY_ERR_INVALID_ARGUMENT;
        case HK_STATUS_UNAVAILABLE:
            return HK_LEGACY_ERR_NOT_SUPPORTED;
        case HK_STATUS_OK:
            return HK_LEGACY_OK;
        default:
            return HK_LEGACY_ERR;
    }
}

static int map_result(const hk_hook_t *hook, void **out_original,
                      void *prepared_original) {
    hk_hook_result_t result;
    if (out_original) {
        *out_original = prepared_original;
    }
    if (hk_hook_copy_result(hook, &result) != HK_STATUS_OK) {
        return HK_LEGACY_ERR;
    }
    switch (result.outcome) {
        case HK_OUTCOME_ACTIVE:
            if (!out_original) {
                return HK_LEGACY_OK;
            }
            if (!result.original_available) {
                return HK_LEGACY_ERR;
            }
            {
                void *installed =
                    hk_original_slot_load(hk_hook_original_slot(hook));
                if (installed) {
                    *out_original = installed;
                }
            }
            return *out_original ? HK_LEGACY_OK : HK_LEGACY_ERR;
        case HK_OUTCOME_NO_ROUTE:
        case HK_OUTCOME_FAILED_SAFE:
            if (out_original) {
                *out_original = NULL;
            }
            return HK_LEGACY_ERR_NOT_SUPPORTED;
        case HK_OUTCOME_FAILED_PARTIAL:
            return HK_LEGACY_ERR_PARTIAL;
        case HK_OUTCOME_FAILED_UNKNOWN:
            return HK_LEGACY_ERR;
        default:
            if (out_original) {
                *out_original = NULL;
            }
            return HK_LEGACY_ERR;
    }
}

static bool hk_legacy_backend_ids_are_only(const char *backend_ids,
                                           const char *wanted) {
    if (!backend_ids || !wanted || !wanted[0]) {
        return false;
    }
    bool found = false;
    const char *cursor = backend_ids;
    while (*cursor) {
        while (*cursor == ',' || *cursor == ' ' || *cursor == '\t' ||
               *cursor == '\n' || *cursor == '\r') {
            cursor++;
        }
        const char *token = cursor;
        while (*cursor && *cursor != ',' && *cursor != ' ' &&
               *cursor != '\t' && *cursor != '\n' && *cursor != '\r') {
            cursor++;
        }
        size_t length = (size_t)(cursor - token);
        if (length == 0) {
            continue;
        }
        if (found || strlen(wanted) != length ||
            strncmp(token, wanted, length) != 0) {
            return false;
        }
        found = true;
    }
    return found;
}

// HookKit v1's fishhook module accepted a function address, used dladdr to
// recover its exported name, then rebound that name. Preserve exactly that
// adapter behavior when the v1 picker selects current rebind.
static int hk_legacy_route_function_spec(const hk_hook_spec_t *spec,
                                         const char *backend_ids,
                                         hk_hook_spec_t *out_spec) {
    if (!spec || !out_spec) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    *out_spec = *spec;
    if (!hk_legacy_backend_ids_are_only(backend_ids, "rebind") ||
        spec->target_kind != HK_TARGET_FUNCTION_ADDRESS) {
        return HK_LEGACY_OK;
    }
#if defined(__APPLE__)
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr((const void *)spec->target.address.address, &info) ||
        !info.dli_sname || !info.dli_sname[0]) {
        return HK_LEGACY_ERR_NOT_SUPPORTED;
    }

    out_spec->target_kind = HK_TARGET_FUNCTION_SYMBOL;
    out_spec->required_reach = HK_REACH_EXISTING_IMPORTS;
    out_spec->preferred_reach = HK_REACH_EXISTING_IMPORTS;
    if (out_spec->original_requirement == HK_ORIGINAL_CALLABLE_CONTINUATION) {
        out_spec->original_requirement = HK_ORIGINAL_DIRECT_PREDECESSOR;
    }
    hk_symbol_target_t *symbol = &out_spec->target.symbol;
    memset(symbol, 0, sizeof(*symbol));
    symbol->struct_size = sizeof(*symbol);
    symbol->struct_version = HK_ABI_VERSION_3_0;
    symbol->name = info.dli_sname;
    symbol->name_convention = HK_SYMBOL_NAME_C;
    symbol->visibility = HK_SYMBOL_VISIBILITY_EXPORTED_ONLY;
    symbol->defining_image.struct_size = sizeof(symbol->defining_image);
    symbol->defining_image.struct_version = HK_ABI_VERSION_3_0;
    symbol->defining_image.kind = HK_IMAGE_ANY_LOADED;
    symbol->caller_image_scope.struct_size = sizeof(symbol->caller_image_scope);
    symbol->caller_image_scope.struct_version = HK_ABI_VERSION_3_0;
    symbol->caller_image_scope.kind = HK_IMAGE_ANY_LOADED;
    symbol->alias_policy = HK_SYMBOL_ALIAS_EXACT_ONLY;
    return HK_LEGACY_OK;
#else
    return HK_LEGACY_ERR_NOT_SUPPORTED;
#endif
}

static int apply_plan(const hk_hook_spec_t *spec, void **out_original,
                      const char *backend_ids) {
    if (out_original) {
        *out_original = NULL;
    }

    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    hk_report_t *report = NULL;
    void *prepared_original = NULL;
    int result = HK_LEGACY_ERR;

    hk_runtime_config_t runtime_config;
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.struct_size = sizeof(runtime_config);
    runtime_config.struct_version = HK_ABI_VERSION_3_0;
    runtime_config.install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS;
    if (hk_runtime_create_with_backend_override(&runtime_config, backend_ids,
                                                &runtime) != HK_STATUS_OK ||
        !runtime) {
        return HK_LEGACY_ERR_NOT_SUPPORTED;
    }
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK || !plan) {
        goto done;
    }
    hk_hook_spec_t routed_spec;
    int route = hk_legacy_route_function_spec(spec, backend_ids, &routed_spec);
    if (route != HK_LEGACY_OK) {
        result = route;
        goto done;
    }
    hk_status_t status = hk_plan_add_hook(plan, &routed_spec, &hook);
    if (status != HK_STATUS_OK || !hook) {
        result = map_status(status);
        goto done;
    }
    if (hk_plan_analyze(plan, &report) != HK_STATUS_OK) {
        goto done;
    }
    hk_report_release(report);
    report = NULL;
    if (hk_plan_prepare(plan, &report) != HK_STATUS_OK) {
        goto done;
    }
    hk_report_release(report);
    report = NULL;
    if (out_original) {
        hk_hook_result_t prepared_result;
        if (hk_hook_copy_result(hook, &prepared_result) != HK_STATUS_OK ||
            prepared_result.outcome != HK_OUTCOME_PREPARED) {
            result = HK_LEGACY_ERR;
            goto done;
        }
        // Engines that can describe a continuation during preparation (ObjC
        // and relocating inline) retain the historical early publication.
        // A provider-owned continuation is published by the HookKit commit path;
        // do not reject an otherwise valid provider just because its pointer
        // becomes known there.
        prepared_original = (void *)prepared_result.continuation.address;
        if (prepared_original) {
            *out_original = prepared_original;
        }
    }
    if (hk_plan_commit(plan, &report) != HK_STATUS_OK) {
        if (out_original) {
            *out_original = NULL;
        }
        goto done;
    }
    result = map_result(hook, out_original, prepared_original);

done:
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    return result;
}

// ---- spec builders (shared by the single and batched paths) ----------------

static uint64_t hk_legacy_spec_counter = 0;

// Plans validate stable_hook_id uniqueness, so batched specs need distinct
// ids; the buffer is caller-owned and must outlive hk_plan_add_hook.
static void init_spec_id(hk_hook_spec_t *spec, char *id_buf, size_t id_cap,
                         const char *prefix, hk_target_kind_t kind,
                         void *replacement, hk_reachability_t reach,
                         hk_original_requirement_t original) {
    snprintf(id_buf, id_cap, "%s-%llu", prefix,
             (unsigned long long)(++hk_legacy_spec_counter));
    memset(spec, 0, sizeof(*spec));
    spec->struct_size = sizeof(*spec);
    spec->struct_version = HK_ABI_VERSION_3_0;
    spec->stable_hook_id = id_buf;
    spec->target_kind = kind;
    spec->replacement = replacement;
    spec->required_reach = reach;
    spec->preferred_reach = reach;
    spec->original_requirement = original;
    spec->continuation_policy = HK_CONTINUATION_ANY;
    spec->availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec->role = HK_OPERATION_MANDATORY;
}

int hk_legacy_build_objc_spec(void *dispatch_class, void *selector,
                               void *replacement, void **out_original,
                               char *id_buf, size_t id_cap,
                               hk_hook_spec_t *out_spec) {
    if (!dispatch_class || !selector || !replacement) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    init_spec_id(out_spec, id_buf, id_cap, "legacy-objc",
                 HK_TARGET_OBJC_METHOD, replacement, HK_REACH_OBJC_DISPATCH,
                 out_original ? HK_ORIGINAL_DIRECT_PREDECESSOR
                              : HK_ORIGINAL_NONE);
    out_spec->target.objc.struct_size = sizeof(out_spec->target.objc);
    out_spec->target.objc.struct_version = HK_ABI_VERSION_3_0;
    out_spec->target.objc.cls = dispatch_class;
    out_spec->target.objc.sel = selector;
    out_spec->target.objc.method_kind = HK_OBJC_INSTANCE_METHOD;
    out_spec->target.objc.inheritance_policy = HK_OBJC_ALLOW_INHERITED_OVERRIDE;
    out_spec->target.objc.availability = HK_AVAILABILITY_REQUIRED_NOW;
    return HK_LEGACY_OK;
}

int hk_legacy_build_function_spec(void *function, void *replacement,
                                   void **out_original,
                                   char *id_buf, size_t id_cap,
                                   hk_hook_spec_t *out_spec) {
    if (!function || !replacement) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    init_spec_id(out_spec, id_buf, id_cap, "legacy-function",
                 HK_TARGET_FUNCTION_ADDRESS, replacement, HK_REACH_ENTRYPOINT,
                 out_original ? HK_ORIGINAL_CALLABLE_CONTINUATION
                              : HK_ORIGINAL_NONE);
    out_spec->target.address.struct_size = sizeof(out_spec->target.address);
    out_spec->target.address.struct_version = HK_ABI_VERSION_3_0;
    out_spec->target.address.address = (uintptr_t)function;
    return HK_LEGACY_OK;
}

int hk_legacy_apply_specs_with_backend_ids(const hk_hook_spec_t *specs,
                                            void **const *originals,
                                            size_t count,
                                            int *out_results,
                                            const char *backend_ids) {
    if (count == 0) {
        return HK_LEGACY_OK;
    }
    if (!specs || !out_results) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }

    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_report_t *report = NULL;
    hk_hook_t **hooks = calloc(count, sizeof(*hooks));
    if (!hooks) {
        for (size_t i = 0; i < count; i++) {
            out_results[i] = HK_LEGACY_ERR;
        }
        return HK_LEGACY_ERR;
    }

    hk_runtime_config_t runtime_config;
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.struct_size = sizeof(runtime_config);
    runtime_config.struct_version = HK_ABI_VERSION_3_0;
    runtime_config.install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS;
    if (hk_runtime_create_with_backend_override(&runtime_config, backend_ids,
                                                &runtime) != HK_STATUS_OK ||
        !runtime) {
        free(hooks);
        for (size_t i = 0; i < count; i++) {
            out_results[i] = HK_LEGACY_ERR_NOT_SUPPORTED;
        }
        return HK_LEGACY_ERR_NOT_SUPPORTED;
    }
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK || !plan) {
        free(hooks);
        for (size_t i = 0; i < count; i++) {
            out_results[i] = HK_LEGACY_ERR;
        }
        return HK_LEGACY_ERR;
    }

    // Per-op add validation matches the single path: one rejected spec does
    // not block its siblings.
    for (size_t i = 0; i < count; i++) {
        hooks[i] = NULL;
        hk_hook_spec_t routed_spec;
        int route = hk_legacy_route_function_spec(&specs[i], backend_ids,
                                                   &routed_spec);
        if (route != HK_LEGACY_OK) {
            out_results[i] = route;
            continue;
        }
        hk_status_t status = hk_plan_add_hook(plan, &routed_spec, &hooks[i]);
        out_results[i] = map_status(status);
    }

    if (hk_plan_analyze(plan, &report) == HK_STATUS_OK) {
        hk_report_release(report);
        report = NULL;
        if (hk_plan_prepare(plan, &report) == HK_STATUS_OK) {
            hk_report_release(report);
            report = NULL;
            // Early publication for engines that describe a continuation at
            // prepare time (ObjC, relocating inline) -- same as apply_plan.
            for (size_t i = 0; i < count; i++) {
                if (!hooks[i] || !originals || !originals[i]) {
                    continue;
                }
                hk_hook_result_t prepared_result;
                if (hk_hook_copy_result(hooks[i], &prepared_result) ==
                        HK_STATUS_OK &&
                    prepared_result.outcome == HK_OUTCOME_PREPARED &&
                    prepared_result.continuation.address) {
                    *originals[i] =
                        (void *)prepared_result.continuation.address;
                }
            }
            hk_plan_commit(plan, &report);
        }
    }
    hk_report_release(report);
    report = NULL;

    size_t succeeded = 0;
    for (size_t i = 0; i < count; i++) {
        if (!hooks[i]) {
            continue;  // add_hook already wrote the per-op status
        }
        out_results[i] = map_result(hooks[i], originals ? originals[i] : NULL,
                                    NULL);
        succeeded += out_results[i] == HK_LEGACY_OK;
    }

    hk_plan_release(plan);
    hk_runtime_release(runtime);
    free(hooks);

    if (succeeded == count) {
        return HK_LEGACY_OK;
    }
    return succeeded ? HK_LEGACY_ERR_PARTIAL : HK_LEGACY_ERR;
}

int hk_legacy_apply_specs(const hk_hook_spec_t *specs,
                           void **const *originals,
                           size_t count,
                           int *out_results) {
    return hk_legacy_apply_specs_with_backend_ids(specs, originals, count,
                                                   out_results, NULL);
}

static void init_spec(hk_hook_spec_t *spec, const char *stable_id,
                      hk_target_kind_t kind, void *replacement,
                      hk_reachability_t reach,
                      hk_original_requirement_t original) {
    memset(spec, 0, sizeof(*spec));
    spec->struct_size = sizeof(*spec);
    spec->struct_version = HK_ABI_VERSION_3_0;
    spec->stable_hook_id = stable_id;
    spec->target_kind = kind;
    spec->replacement = replacement;
    spec->required_reach = reach;
    spec->preferred_reach = reach;
    spec->original_requirement = original;
    spec->continuation_policy = HK_CONTINUATION_ANY;
    spec->availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec->role = HK_OPERATION_MANDATORY;
}

int hk_legacy_hook_objc(void *dispatch_class, void *selector,
                         void *replacement, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    hk_hook_spec_t spec;
    char id_buf[48];
    int build = hk_legacy_build_objc_spec(dispatch_class, selector,
                                           replacement, out_original,
                                           id_buf, sizeof(id_buf), &spec);
    if (build != HK_LEGACY_OK) {
        return build;
    }
    return apply_plan(&spec, out_original, NULL);
}

int hk_legacy_hook_function(void *function, void *replacement,
                             void **out_original) {
    return hk_legacy_hook_function_with_backend_ids(function, replacement,
                                                     out_original, NULL);
}

int hk_legacy_hook_function_with_backend_ids(void *function, void *replacement,
                                              void **out_original,
                                              const char *backend_ids) {
    if (out_original) {
        *out_original = NULL;
    }
    hk_hook_spec_t spec;
    char id_buf[48];
    int build = hk_legacy_build_function_spec(function, replacement,
                                               out_original,
                                               id_buf, sizeof(id_buf), &spec);
    if (build != HK_LEGACY_OK) {
        return build;
    }
    return apply_plan(&spec, out_original, backend_ids);
}

int hk_legacy_hook_memory(void *target, const void *data, size_t size) {
    return hk_legacy_hook_memory_with_backend_ids(target, data, size, NULL);
}

int hk_legacy_hook_memory_with_backend_ids(void *target, const void *data,
                                            size_t size,
                                            const char *backend_ids) {
    if (!target || !data || size == 0) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    // The canonical byte-patch engine is certified only where HookKit owns a
    // safe writer. Do not turn a valid 32-bit legacy request into an unsafe
    // raw store merely because the retired facade exposed this call.
    if (!hk_native_supported()) {
        return HK_LEGACY_ERR_NOT_SUPPORTED;
    }
    // The old spelling has no expected-byte parameter.  Capture the current
    // bytes now and use the normal 3.0 precondition path; preparation and
    // commit still revalidate instead of retaining a permissive exception.
    uint8_t *expected = malloc(size);
    if (!expected) {
        return HK_LEGACY_ERR;
    }
    if (!hk_native_range_readable(target, size)) {
        free(expected);
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    memcpy(expected, target, size);

    hk_hook_spec_t spec;
    init_spec(&spec, "legacy-memory", HK_TARGET_MEMORY_PATCH, NULL,
              HK_REACH_EXACT_MEMORY, HK_ORIGINAL_NONE);
    spec.target.memory.struct_size = sizeof(spec.target.memory);
    spec.target.memory.struct_version = HK_ABI_VERSION_3_0;
    spec.target.memory.address = (uintptr_t)target;
    spec.target.memory.size = size;
    spec.target.memory.replacement_bytes.data = data;
    spec.target.memory.replacement_bytes.size = size;
    spec.target.memory.expected_bytes.data = expected;
    spec.target.memory.expected_bytes.size = size;
    spec.target.memory.kind = HK_MEMORY_KIND_DATA;
    int result = apply_plan(&spec, NULL, backend_ids);
    free(expected);
    return result;
}

static int ensure_swift_demangler(void) {
#if defined(__APPLE__)
    if (!hk_swift_demangle) {
        hk_swift_demangle = (hk_swift_demangle_fn)dlsym(RTLD_DEFAULT,
                                                        "swift_demangle");
    }
#endif
    return hk_swift_demangle ? HK_LEGACY_OK : HK_LEGACY_ERR_NOT_SUPPORTED;
}

int hk_legacy_hook_swift_method(void *metadata, const char *name,
                                 void *replacement, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!metadata || !name || !name[0] || !replacement) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    hk_swift_target_t target = hk_swift_target_init();
    target.metadata = metadata;
    target.method_name = name;
    target.name_kind = (strncmp(name, "$s", 2) == 0 ||
                        strncmp(name, "_$s", 3) == 0)
        ? HK_SWIFT_NAME_MANGLED_EXACT
        : HK_SWIFT_NAME_DEMANGLED_SUBSTRING;
    if (target.name_kind == HK_SWIFT_NAME_DEMANGLED_SUBSTRING &&
        ensure_swift_demangler() != HK_LEGACY_OK) {
        return HK_LEGACY_ERR_NOT_SUPPORTED;
    }
    return map_status(hk_swift_hook(&target, replacement, out_original));
}

int hk_legacy_hook_swift_slot(void *metadata, uint32_t index,
                               void *replacement, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!metadata || !replacement) {
        return HK_LEGACY_ERR_INVALID_ARGUMENT;
    }
    hk_swift_target_t target = hk_swift_target_init();
    target.metadata = metadata;
    target.name_kind = HK_SWIFT_NAME_SLOT_INDEX;
    target.slot_index = index;
    return map_status(hk_swift_hook(&target, replacement, out_original));
}
