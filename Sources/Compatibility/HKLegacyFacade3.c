// Canonical facade bridge. Keep this deliberately boring: one legacy call is
// one 3.0 runtime/plan lifecycle, so there is no second state machine to
// drift from the public ABI.

#include "HKLegacyFacade3.h"

#include "../../Headers/HookKit/HookKit.h"
#include "../../native/hk_swift.h"
#include "../../native/hk_native.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

enum {
    HK3_OK = 0,
    HK3_ERR = 1 << 0,
    HK3_ERR_NOT_SUPPORTED = 1 << 1,
    HK3_ERR_INVALID_ARGUMENT = 1 << 2,
    HK3_ERR_PARTIAL = 1 << 3,
};

static int map_status(hk_status_t status) {
    switch (status) {
        case HK_STATUS_INVALID_ARGUMENT:
            return HK3_ERR_INVALID_ARGUMENT;
        case HK_STATUS_UNAVAILABLE:
            return HK3_ERR_NOT_SUPPORTED;
        case HK_STATUS_OK:
            return HK3_OK;
        default:
            return HK3_ERR;
    }
}

static int map_result(const hk_hook_t *hook, void **out_original,
                      void *prepared_original) {
    hk_hook_result_t result;
    if (out_original) {
        *out_original = prepared_original;
    }
    if (hk_hook_copy_result(hook, &result) != HK_STATUS_OK) {
        return HK3_ERR;
    }
    switch (result.outcome) {
        case HK_OUTCOME_ACTIVE:
            if (!out_original) {
                return HK3_OK;
            }
            if (!result.original_available) {
                return HK3_ERR;
            }
            {
                void *installed =
                    hk_original_slot_load(hk_hook_original_slot(hook));
                if (installed) {
                    *out_original = installed;
                }
            }
            return *out_original ? HK3_OK : HK3_ERR;
        case HK_OUTCOME_NO_ROUTE:
        case HK_OUTCOME_FAILED_SAFE:
            if (out_original) {
                *out_original = NULL;
            }
            return HK3_ERR_NOT_SUPPORTED;
        case HK_OUTCOME_FAILED_PARTIAL:
            return HK3_ERR_PARTIAL;
        case HK_OUTCOME_FAILED_UNKNOWN:
            return HK3_ERR;
        default:
            if (out_original) {
                *out_original = NULL;
            }
            return HK3_ERR;
    }
}

static int apply_plan(const hk_hook_spec_t *spec, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }

    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    hk_report_t *report = NULL;
    void *prepared_original = NULL;
    int result = HK3_ERR;

    hk_runtime_config_t runtime_config;
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.struct_size = sizeof(runtime_config);
    runtime_config.struct_version = HK_ABI_VERSION_3_0;
    runtime_config.install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS;
    if (hk_runtime_create(&runtime_config, &runtime) != HK_STATUS_OK || !runtime) {
        return HK3_ERR_NOT_SUPPORTED;
    }
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK || !plan) {
        goto done;
    }
    hk_status_t status = hk_plan_add_hook(plan, spec, &hook);
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
            result = HK3_ERR;
            goto done;
        }
        // Engines that can describe a continuation during preparation (ObjC
        // and relocating inline) retain the historical early publication.
        // A provider-owned continuation is published by the HK3 commit path;
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

int hk3_legacy_hook_objc(void *dispatch_class, void *selector,
                         void *replacement, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!dispatch_class || !selector || !replacement) {
        return HK3_ERR_INVALID_ARGUMENT;
    }
    hk_hook_spec_t spec;
    init_spec(&spec, "legacy-objc", HK_TARGET_OBJC_METHOD, replacement,
              HK_REACH_OBJC_DISPATCH, out_original
                  ? HK_ORIGINAL_DIRECT_PREDECESSOR : HK_ORIGINAL_NONE);
    spec.target.objc.struct_size = sizeof(spec.target.objc);
    spec.target.objc.struct_version = HK_ABI_VERSION_3_0;
    spec.target.objc.cls = dispatch_class;
    spec.target.objc.sel = selector;
    spec.target.objc.method_kind = HK_OBJC_INSTANCE_METHOD;
    spec.target.objc.inheritance_policy = HK_OBJC_ALLOW_INHERITED_OVERRIDE;
    spec.target.objc.availability = HK_AVAILABILITY_REQUIRED_NOW;
    return apply_plan(&spec, out_original);
}

int hk3_legacy_hook_function(void *function, void *replacement,
                             void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!function || !replacement) {
        return HK3_ERR_INVALID_ARGUMENT;
    }
    hk_hook_spec_t spec;
    init_spec(&spec, "legacy-function", HK_TARGET_FUNCTION_ADDRESS, replacement,
              HK_REACH_ENTRYPOINT, out_original
                  ? HK_ORIGINAL_CALLABLE_CONTINUATION : HK_ORIGINAL_NONE);
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = (uintptr_t)function;
    return apply_plan(&spec, out_original);
}

int hk3_legacy_hook_memory(void *target, const void *data, size_t size) {
    if (!target || !data || size == 0) {
        return HK3_ERR_INVALID_ARGUMENT;
    }
    // The canonical byte-patch engine is certified only where HookKit owns a
    // safe writer. Do not turn a valid 32-bit legacy request into an unsafe
    // raw store merely because the retired facade exposed this call.
    if (!hk_native_supported()) {
        return HK3_ERR_NOT_SUPPORTED;
    }
    // The old spelling has no expected-byte parameter.  Capture the current
    // bytes now and use the normal 3.0 precondition path; preparation and
    // commit still revalidate instead of retaining a permissive exception.
    uint8_t *expected = malloc(size);
    if (!expected) {
        return HK3_ERR;
    }
    if (!hk_native_range_readable(target, size)) {
        free(expected);
        return HK3_ERR_INVALID_ARGUMENT;
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
    int result = apply_plan(&spec, NULL);
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
    return hk_swift_demangle ? HK3_OK : HK3_ERR_NOT_SUPPORTED;
}

int hk3_legacy_hook_swift_method(void *metadata, const char *name,
                                 void *replacement, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!metadata || !name || !name[0] || !replacement) {
        return HK3_ERR_INVALID_ARGUMENT;
    }
    hk_swift_target_t target = hk_swift_target_init();
    target.metadata = metadata;
    target.method_name = name;
    target.name_kind = (strncmp(name, "$s", 2) == 0 ||
                        strncmp(name, "_$s", 3) == 0)
        ? HK_SWIFT_NAME_MANGLED_EXACT
        : HK_SWIFT_NAME_DEMANGLED_SUBSTRING;
    if (target.name_kind == HK_SWIFT_NAME_DEMANGLED_SUBSTRING &&
        ensure_swift_demangler() != HK3_OK) {
        return HK3_ERR_NOT_SUPPORTED;
    }
    return map_status(hk_swift_hook(&target, replacement, out_original));
}

int hk3_legacy_hook_swift_slot(void *metadata, uint32_t index,
                               void *replacement, void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!metadata || !replacement) {
        return HK3_ERR_INVALID_ARGUMENT;
    }
    hk_swift_target_t target = hk_swift_target_init();
    target.metadata = metadata;
    target.name_kind = HK_SWIFT_NAME_SLOT_INDEX;
    target.slot_index = index;
    return map_status(hk_swift_hook(&target, replacement, out_original));
}
