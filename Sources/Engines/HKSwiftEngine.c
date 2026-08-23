// Public Swift request surface over the two-phase native metadata primitive.
// The class-name form is intentionally limited to ObjC-visible Swift classes;
// callers with a metadata pointer do not need a runtime lookup.

#include "../../Headers/HookKit/HookKitSwift.h"
#include "../../native/hk_swift.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <objc/runtime.h>
#endif

// Exact mangled names work before this is resolved; substring lookup fails
// closed until a caller resolves the demangler.
hk_swift_demangle_fn hk_swift_demangle = NULL;

struct hk_swift_plan {
    Class cls;
    hk_swift_slot_plan_t slot;
};

static Class target_class(const hk_swift_target_t *target) {
    if (target->metadata) {
        return (Class)target->metadata;
    }
#if defined(__APPLE__)
    if (target->class_name) {
        return (Class)objc_getClass(target->class_name);
    }
#else
    (void)target;
#endif
    return NULL;
}

static bool target_index(Class cls, const hk_swift_target_t *target,
                         uint32_t *out_index) {
    if (target->name_kind == HK_SWIFT_NAME_SLOT_INDEX) {
        *out_index = target->slot_index;
        return true;
    }
    if (!target->method_name || !target->method_name[0]) {
        return false;
    }
    return hk_swift_find_slot(cls, target->method_name, out_index);
}

hk_status_t hk_swift_prepare(const hk_swift_target_t *target,
                             hk_swift_plan_t **out_plan) {
    if (out_plan) {
        *out_plan = NULL;
    }
    if (!target || !out_plan || target->struct_size < sizeof(*target)) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (!hk_swift_supported()) {
        return HK_STATUS_UNAVAILABLE;
    }

    Class cls = target_class(target);
    if (!cls) {
        return HK_STATUS_UNAVAILABLE;
    }

    uint32_t index = 0;
    if (!target_index(cls, target, &index)) {
        return HK_STATUS_UNAVAILABLE;
    }

    hk_swift_plan_t *plan = calloc(1, sizeof(*plan));
    if (!plan) {
        return HK_STATUS_OUT_OF_MEMORY;
    }
    plan->cls = cls;
    if (!hk_swift_prepare_slot(cls, index, &plan->slot)) {
        free(plan);
        return HK_STATUS_INTERNAL_ERROR;
    }

    *out_plan = plan;
    return HK_STATUS_OK;
}

hk_status_t hk_swift_commit(hk_swift_plan_t *plan, void *replacement,
                            void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!plan || !replacement) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    return hk_swift_commit_slot(&plan->slot, replacement, out_original)
        ? HK_STATUS_OK
        : HK_STATUS_INTERNAL_ERROR;
}

void hk_swift_plan_release(hk_swift_plan_t *plan) {
    free(plan);
}

hk_status_t hk_swift_hook(const hk_swift_target_t *target, void *replacement,
                          void **out_original) {
    if (out_original) {
        *out_original = NULL;
    }
    if (!target || !replacement) {
        return HK_STATUS_INVALID_ARGUMENT;
    }

    hk_swift_plan_t *plan = NULL;
    hk_status_t status = hk_swift_prepare(target, &plan);
    if (status != HK_STATUS_OK) {
        return status;
    }
    status = hk_swift_commit(plan, replacement, out_original);
    hk_swift_plan_release(plan);
    return status;
}

int hk_swift_last_error_code(void) {
    return hk_swift_last_error();
}
