// Internal layout of hk_runtime_t (public: opaque, HookKitRuntime.h).
// Shared across Sources/Core/*.c that need runtime internals -- nothing
// outside Sources/Core/ should include this.

#ifndef HK_CORE_RUNTIME_INTERNAL_H
#define HK_CORE_RUNTIME_INTERNAL_H

#include <stdatomic.h>

#include "../../Headers/HookKit/HookKitRuntime.h"

struct hk_runtime {
    hk_id_t owner_id;

    // Copied by value at hk_runtime_create -- every field is a function
    // pointer, a void* context, or an enum, so a plain struct copy already
    // satisfies the "caller's buffer need not outlive the call" rule with
    // no separate allocation needed (unlike string/bytes-bearing specs
    // elsewhere in the ABI, which deep-copy into owned storage instead).
    hk_runtime_config_t config;

    atomic_bool shutdown_called;
};

#endif // HK_CORE_RUNTIME_INTERNAL_H
