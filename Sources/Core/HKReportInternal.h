// Internal layout of hk_report_t (public: opaque, HookKitRuntime.h).
// Shared across Sources/Core/*.c that produce reports (HKPlan.c today;
// HKRuntime.c's hk_runtime_drain_pending once Milestone 12 gives it
// something to report).

#ifndef HK_CORE_REPORT_INTERNAL_H
#define HK_CORE_REPORT_INTERNAL_H

#include <stddef.h>

#include "../../Headers/HookKit/HookKitRuntime.h"
#include "../../Headers/HookKit/HookKitResults.h"

// A flat snapshot of hk_hook_result_t values -- not pointers back into
// live hk_hook_t objects. hk_hook_result_t itself owns no heap allocations
// (its hk_string_view_t fields are views, not owned strings -- none of
// them are populated yet; see HKPlan.c's analyze for the real gap this
// leaves), so a plain array of value copies is sufficient today. The day
// a result carries an owned diagnostic string, this struct is where that
// ownership needs to move to.
struct hk_report {
    hk_id_t report_id;
    hk_hook_result_t *results;
    size_t result_count;
};

// Internal constructor (not public API -- callers get reports only via
// hk_plan_analyze/prepare/commit/hk_runtime_drain_pending). NULL on OOM.
hk_report_t *hk_report_create(const hk_hook_result_t *results, size_t count);

#endif // HK_CORE_REPORT_INTERNAL_H
