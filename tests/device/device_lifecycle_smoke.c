#include <HookKit/HookKit.h>

#include <stdio.h>

int main(void) {
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_report_t *report = NULL;
    hk_artifact_snapshot_t *snapshot = NULL;

    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK || !runtime) {
        puts("runtime: FAIL");
        return 1;
    }
    hk_id_t owner = hk_runtime_owner_id(runtime);
    if (owner.high == 0 || owner.low == 0) {
        puts("owner-id: FAIL");
        hk_runtime_release(runtime);
        return 1;
    }
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK ||
        hk_plan_analyze(plan, &report) != HK_STATUS_OK || !report ||
        hk_plan_state(plan) != HK_PLAN_ANALYZED) {
        puts("plan: FAIL");
        hk_report_release(report);
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return 1;
    }
    hk_report_release(report);
    report = NULL;

    if (hk_runtime_copy_artifacts(runtime, &snapshot) != HK_STATUS_OK ||
        !snapshot || hk_artifact_snapshot_count(snapshot) != 0) {
        puts("artifacts: FAIL");
        hk_artifact_snapshot_release(snapshot);
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return 1;
    }
    hk_artifact_snapshot_release(snapshot);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    puts("HookKit lifecycle: PASS");
    return 0;
}
