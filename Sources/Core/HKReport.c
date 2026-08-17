// hk_report_t construction/release. Real hk_report_release now -- it used
// to be a no-op in HKRuntime.c because no concrete hk_report_t existed
// yet (every out_report was NULL); that placeholder is replaced below.

#include "HKReportInternal.h"

#include <stdlib.h>
#include <string.h>

#include "HKIDs.h"

// Builds an owned report from `count` result snapshots. Copies by value
// (see HKReportInternal.h on why that's sufficient today). Returns NULL on
// OOM -- callers propagate HK_STATUS_OUT_OF_MEMORY.
hk_report_t *hk_report_create(const hk_hook_result_t *results, size_t count) {
    hk_report_t *report = (hk_report_t *)calloc(1, sizeof(hk_report_t));
    if (!report) {
        return NULL;
    }
    report->report_id = hk_id_generate();
    if (count > 0) {
        report->results = (hk_hook_result_t *)malloc(count * sizeof(hk_hook_result_t));
        if (!report->results) {
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
    free(report->results);
    free(report);
}
