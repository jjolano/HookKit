// ObjC engine <-> runtime adapter. See HKObjCVtable.h.
//
// No file-scoped state at all: the runtime comes from the registered engine
// context and the prepared plan is handed back by the core. That is the whole
// difference from the other two adapters, and it is why this file has no
// stash, no environment, and no _for_testing entry points.

#include "HKObjCVtable.h"

#include <stdlib.h>
#include <string.h>

static hk_engine_capabilities_t objc_describe(void) {
    hk_engine_capabilities_t caps;
    caps.engine_id = "objc";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    caps.achievable_reach = HK_REACH_OBJC_DISPATCH;
    return caps;
}

// Every refusal the engine can make, carried through instead of flattened.
// The messages are literals, as the diag contract requires.
static hk_prepare_result_t objc_classify(hk_objc_status_t st, hk_prepare_diag_t *diag) {
    diag->error_code = (int64_t)st;
    switch (st) {
        case HK_OBJC_NOT_APPLICABLE:
            // The one status that is NOT a failure: the request said
            // OPTIONAL_IF_PRESENT and the target is absent, so it is
            // satisfied. This is the whole reason for the richer result.
            diag->error_message = NULL;
            diag->error_code = 0;
            return HK_PREPARE_NOT_APPLICABLE;
        case HK_OBJC_CLASS_NOT_FOUND:
            diag->error_message = "class not found"; break;
        case HK_OBJC_METHOD_NOT_FOUND:
            diag->error_message = "selector not found on the class or its ancestors"; break;
        case HK_OBJC_INHERITED_REFUSED:
            diag->error_message = "method is inherited and the request required a local method"; break;
        case HK_OBJC_NO_IMPLEMENTATION:
            diag->error_message = "method has no implementation to publish as the original"; break;
        case HK_OBJC_UNSUPPORTED_POLICY:
            diag->error_message = "DEFER_UNTIL_AVAILABLE needs an image-load callback, which is unbuilt"; break;
        case HK_OBJC_INVALID_ARGUMENT:
            diag->error_message = "invalid objc target"; break;
        case HK_OBJC_OK:
            diag->error_code = 0; diag->error_message = NULL;
            return HK_PREPARE_OK;
    }
    return HK_PREPARE_FAILED;
}

static hk_prepare_result_t objc_prepare_one_ctx_status(void *engine_ctx,
                                                       const hk_hook_spec_t *spec,
                                                       void **out_prepared,
                                                       hk_prepare_diag_t *out_diag) {
    hk_objc_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_OBJC_METHOD) {
        out_diag->error_message = "objc engine invoked without a runtime or with a non-objc target";
        return HK_PREPARE_FAILED;
    }

    hk_objc_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
    }
    hk_objc_status_t st = hk_objc_prepare(&ctx->runtime, &spec->target.objc, plan);
    hk_prepare_result_t result = objc_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(plan);  // nothing reserved on any non-OK result
        return result;
    }
    *out_prepared = plan;
    return HK_PREPARE_OK;
}

static hk_mutation_state_t objc_commit_one_ctx(void *engine_ctx,
                                               const hk_hook_spec_t *spec,
                                               void *prepared,
                                               hk_artifact_sink_t *sink) {
    hk_objc_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !prepared) {
        return HK_MUTATION_NONE;  // never prepared, or prepared by someone else
    }
    // out_original is NULL: the original travels in the artifact instead --
    // see the header.
    return hk_objc_commit(&ctx->runtime, (const hk_objc_plan_t *)prepared,
                          spec->replacement, NULL, sink);
}

static void objc_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_objc_vtable = {
    .describe = objc_describe,
    .prepare_one_ctx_status = objc_prepare_one_ctx_status,
    .commit_one_ctx = objc_commit_one_ctx,
    .release_prepared = objc_release_prepared,
};

const hk_engine_vtable_t *hk_objc_vtable(void) {
    return &g_objc_vtable;
}
