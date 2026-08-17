// hk_report_t construction/release. Real hk_report_release now -- it used
// to be a no-op in HKRuntime.c because no concrete hk_report_t existed
// yet (every out_report was NULL); that placeholder is replaced below.

#include "HKReportInternal.h"

#include <stdlib.h>
#include <string.h>

#include "HKIDs.h"
#include "HKArtifactLedger.h"

// Builds an owned report from `count` result snapshots. Copies by value
// (see HKReportInternal.h on why that's sufficient today). Returns NULL on
// OOM -- callers propagate HK_STATUS_OUT_OF_MEMORY.
hk_report_t *hk_report_create(const hk_hook_result_t *results, size_t count) {
    hk_report_t *report = (hk_report_t *)calloc(1, sizeof(hk_report_t));
    if (!report) {
        return NULL;
    }
    report->report_id = hk_id_generate();
    report->artifacts = hk_artifact_ledger_create();
    if (!report->artifacts) {
        free(report);
        return NULL;
    }
    if (count > 0) {
        report->results = (hk_hook_result_t *)malloc(count * sizeof(hk_hook_result_t));
        if (!report->results) {
            hk_artifact_ledger_destroy(report->artifacts);
            free(report);
            return NULL;
        }
        memcpy(report->results, results, count * sizeof(hk_hook_result_t));
    }
    report->result_count = count;
    return report;
}

void hk_report_release(hk_report_t *report) {
    if (!report) {
        return;
    }
    hk_artifact_ledger_destroy(report->artifacts);
    free(report->results);
    free(report);
}

// Public (HookKitArtifacts.h). Deep-copies the report's artifact ledger
// into an immutable snapshot -- empty until the commit path populates the
// ledger (see HKReportInternal.h). Independent of the report: releasing the
// report afterwards does not invalidate a snapshot already taken.
hk_status_t hk_report_copy_artifacts(const hk_report_t *report,
                                     hk_artifact_snapshot_t **out_snapshot) {
    if (out_snapshot) {
        *out_snapshot = NULL;
    }
    if (!report || !out_snapshot) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    return hk_artifact_snapshot_from_ledger(report->artifacts, out_snapshot);
}
