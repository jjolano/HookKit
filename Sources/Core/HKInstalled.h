// Process-lifetime installed records -- Milestone 4's "Original slots"
// (and the installed-hook handle that carries them).
//
// The hard requirement these exist to satisfy (docs/3.0/PUBLIC_C_ABI.md):
// "Active installation data and original slots that live replacements still
// use must survive runtime wrapper release." A replacement installed by a
// hook keeps calling through its original slot long after the hk_plan_t /
// hk_runtime_t / hk_hook_t wrappers that created it are gone. So these
// records CANNOT be owned by the plan/hook -- they are allocated with
// process lifetime and intentionally retained in a process-global registry,
// never freed in production. hk_installed_reset_for_testing frees them so
// host tests stay leak-clean under ASan; production has no such call.
//
// The public types (hk_original_slot_t, hk_installed_hook_t) are opaque in
// HookKitPlan.h; this internal header completes them for Sources/Core.

#ifndef HK_CORE_INSTALLED_H
#define HK_CORE_INSTALLED_H

#include <stdatomic.h>
#include <stdbool.h>

#include "../../Headers/HookKit/HookKitPlan.h"
#include "../../Headers/HookKit/HookKitResults.h"

#ifdef __cplusplus
extern "C" {
#endif

// The stable location a live replacement loads the current original pointer
// from. Atomic because a future re-hook/update can republish the original
// under a concurrent reader (no such update path exists yet, but the load
// is on the replacement's hot path, so the type is right from the start).
struct hk_original_slot {
    _Atomic(void *) original;
};

// One active installation. Process-lifetime; linked into the global
// registry at creation. `result` is a value snapshot taken at commit.
struct hk_installed_hook {
    hk_id_t installed_id;
    bool has_original;                 // false => no original slot exposed
    struct hk_original_slot slot;      // meaningful only when has_original
    hk_hook_result_t result;
    struct hk_installed_hook *next;    // registry chain (owned by the registry)
};

// Allocates a process-lifetime installed record, publishes `original_or_null`
// into its slot (has_original = original_or_null != NULL), copies `result`,
// and registers it. Returns the record (never freed in production) or NULL
// on OOM. `result` must be non-NULL.
hk_installed_hook_t *hk_installed_record_create(hk_id_t installed_id,
                                                void *original_or_null,
                                                const hk_hook_result_t *result);

// Test-only: frees every registered record. Not for production use -- these
// records are meant to outlive everything. Lets host tests assert
// leak-freedom under -fsanitize=address.
void hk_installed_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif // HK_CORE_INSTALLED_H
