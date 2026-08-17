// Internal layout of hk_plan_t/hk_domain_t (public: opaque,
// HookKitPlan.h). Shared across Sources/Core/*.c.

#ifndef HK_CORE_PLAN_INTERNAL_H
#define HK_CORE_PLAN_INTERNAL_H

#include <stddef.h>

#include "../../Headers/HookKit/HookKitPlan.h"
#include "../../Headers/HookKit/HookKitRuntime.h"

// Individually heap-allocated, never stored inline in a growable array.
// hk_plan_t hands out `const hk_domain_t *`/`hk_domain_t *` pointers to
// callers (e.g. via out_domain, then reused across many hk_plan_add_hook
// calls through hk_hook_spec_t.domain) that must stay valid for the rest
// of the plan's life. An inline array would invalidate every previously
// returned pointer the moment realloc moved it to add the next domain --
// a real dangling-pointer trap, not a hypothetical one. hk_plan_t instead
// keeps a realloc-able array of *pointers* to these separately-allocated
// structs: the pointer array can move freely, the structs it points to
// never do.
struct hk_domain {
    hk_id_t domain_id;
    char *stable_domain_id_owned;  // deep copy of spec.stable_domain_id's contents
    hk_domain_spec_t spec;          // .stable_domain_id repointed at stable_domain_id_owned
};

struct hk_plan {
    hk_id_t plan_id;
    hk_runtime_t *runtime;   // not owned; caller keeps the runtime alive
    hk_plan_config_t config;
    hk_plan_state_t state;

    struct hk_domain **domains;
    size_t domain_count;
    size_t domain_capacity;
};

#endif // HK_CORE_PLAN_INTERNAL_H
