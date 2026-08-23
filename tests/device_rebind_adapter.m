#include <HookKit/HookKit.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile unsigned g_hits;

static int replacement_puts(const char *message) {
    (void)message;
    g_hits++;
    return 0;
}

static int fail(const char *message) {
    const char prefix[] = "HookKit rebind adapter: FAIL (";
    const char suffix[] = ")\n";
    write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    write(STDERR_FILENO, message, strlen(message));
    write(STDERR_FILENO, suffix, sizeof(suffix) - 1);
    return 1;
}

int main(void) {
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    hk_report_t *report = NULL;
    hk_hook_spec_t spec;

    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK || !runtime) {
        return fail("runtime");
    }
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK || !plan) {
        hk_runtime_release(runtime);
        return fail("plan");
    }

    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = "device.rebind.adapter.puts";
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.replacement = (void *)replacement_puts;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.preferred_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = HK_ORIGINAL_DIRECT_PREDECESSOR;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;

    spec.target.symbol.struct_size = sizeof(spec.target.symbol);
    spec.target.symbol.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.name = "puts";
    spec.target.symbol.name_convention = HK_SYMBOL_NAME_C;
    spec.target.symbol.defining_image.struct_size = sizeof(hk_image_selector_t);
    spec.target.symbol.defining_image.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.defining_image.kind = HK_IMAGE_ANY_LOADED;
    spec.target.symbol.caller_image_scope.struct_size = sizeof(hk_image_selector_t);
    spec.target.symbol.caller_image_scope.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.caller_image_scope.kind = HK_IMAGE_MAIN_EXECUTABLE;

    if (hk_plan_add_hook(plan, &spec, &hook) != HK_STATUS_OK || !hook ||
        hk_plan_analyze(plan, &report) != HK_STATUS_OK) {
        hk_report_release(report);
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("analyze");
    }
    hk_report_release(report);
    report = NULL;
    if (hk_plan_prepare(plan, &report) != HK_STATUS_OK) {
        hk_report_release(report);
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("prepare");
    }
    hk_report_release(report);
    report = NULL;
    if (hk_plan_commit(plan, &report) != HK_STATUS_OK) {
        hk_report_release(report);
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("commit");
    }

    hk_hook_result_t result;
    if (hk_hook_copy_result(hook, &result) != HK_STATUS_OK ||
        result.outcome != HK_OUTCOME_ACTIVE || !result.original_available ||
        !hk_original_slot_load(hk_hook_original_slot(hook)) ||
        result.matched_locations == 0 ||
        result.modified_locations == 0 || result.artifact_count == 0 ||
        !(result.observed_commit_effects & HK_EFFECT_IMPORT_MUTATION)) {
        hk_report_release(report);
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("result");
    }

    puts("HookKit rebind adapter trigger");
    const char pass[] = "HookKit rebind adapter: PASS\n";
    int status = (g_hits == 1) ? 0 : fail("replacement");
    if (status == 0) {
        write(STDOUT_FILENO, pass, sizeof(pass) - 1);
    }
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    return status;
}
