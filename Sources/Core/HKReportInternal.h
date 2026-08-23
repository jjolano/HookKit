// Internal layout of hk_report_t (public: opaque, HookKitRuntime.h).
// Shared across Sources/Core/*.c that produce reports (HKPlan.c today;
// HKRuntime.c's hk_runtime_drain_pending once Milestone 12 gives it
// something to report).

#ifndef HK_CORE_REPORT_INTERNAL_H
#define HK_CORE_REPORT_INTERNAL_H

#include <stddef.h>

#include "../../Headers/HookKit/HookKitRuntime.h"
#include "../../Headers/HookKit/HookKitResults.h"
#include "HKArtifactLedger.h"

// A flat snapshot of hk_hook_result_t values -- not pointers back into live
// hk_hook_t objects. hk_hook_result_t owns no heap allocations: its string
// views point at static engine diagnostics, and continuation data is carried
// by value. A plain array of value copies is therefore sufficient today.
//
// `artifacts` is the report's owned artifact ledger (spec section 7). It is
// created empty with the report and is replaced by the populated commit
// ledger. Analyze/prepare reports legitimately carry an empty ledger, since
// those phases make no mutations to produce artifacts from.
struct hk_report {
    hk_id_t report_id;
    hk_hook_result_t *results;
    size_t result_count;
    hk_artifact_ledger_t *artifacts;
};

// Internal constructor (not public API -- callers get reports only via
// hk_plan_analyze/prepare/commit/hk_runtime_drain_pending). NULL on OOM.
// The report starts with an empty artifact ledger; the commit path swaps a
// populated one in with hk_report_adopt_artifact_ledger below.
hk_report_t *hk_report_create(const hk_hook_result_t *results, size_t count);

// Replaces the report's (empty) ledger with `ledger`, taking ownership of
// it and destroying the one the report was created with. For the commit
// path, which builds and populates a ledger during its commit loop (before
// the report exists) and hands it over here. `report` must be non-NULL.
void hk_report_adopt_artifact_ledger(hk_report_t *report, hk_artifact_ledger_t *ledger);

#endif // HK_CORE_REPORT_INTERNAL_H
