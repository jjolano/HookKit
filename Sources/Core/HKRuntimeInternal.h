// Internal layout of hk_runtime_t (public: opaque, HookKitRuntime.h).
// Shared across Sources/Core/*.c that need runtime internals -- nothing
// outside Sources/Core/ should include this.

#ifndef HK_CORE_RUNTIME_INTERNAL_H
#define HK_CORE_RUNTIME_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>

#include "../../Headers/HookKit/HookKitRuntime.h"
#include "HKEngineInternal.h"

// Fixed-size, not grown dynamically: the real production engine set
// (Milestone 6+) is small and compiled-in, and test code registering fake
// engines never needs more than a handful at once. A growable array would
// be solving a problem this doesn't have yet -- see HKPlanInternal.h for
// where that complexity (pointer-stable growth) actually is needed.
#define HK_RUNTIME_MAX_ENGINES 16

struct hk_runtime {
    hk_id_t owner_id;

    // Copied by value at hk_runtime_create -- every field is a function
    // pointer, a void* context, or an enum, so a plain struct copy already
    // satisfies the "caller's buffer need not outlive the call" rule with
    // no separate allocation needed (unlike string/bytes-bearing specs
    // elsewhere in the ABI, which deep-copy into owned storage instead).
    hk_runtime_config_t config;

    atomic_bool shutdown_called;

    // Not public API -- see HKEngineInternal.h. Empty in production (no
    // production engine calls hk_runtime_register_engine_for_testing yet;
    // Milestone 6+ engines will eventually populate this for real, at
    // which point "for_testing" in the function name stops being
    // accurate and it should be renamed, not left misleading).
    const hk_engine_vtable_t *engines[HK_RUNTIME_MAX_ENGINES];
    size_t engine_count;
};

#endif // HK_CORE_RUNTIME_INTERNAL_H
