// Internal layout of hk_plan_t/hk_domain_t (public: opaque,
// HookKitPlan.h). Shared across Sources/Core/*.c.

#ifndef HK_CORE_PLAN_INTERNAL_H
#define HK_CORE_PLAN_INTERNAL_H

#include <stddef.h>

#include "../../Headers/HookKit/HookKitPlan.h"
#include "../../Headers/HookKit/HookKitRuntime.h"
#include "HKEngineInternal.h"
#include "HKInstalled.h"

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

// Same individually-heap-allocated reasoning as hk_domain above:
// hk_hook_t* is returned from hk_plan_add_hook and reused later (e.g. as
// an element of another hook's commit_after array), so it must survive
// the hooks array reallocating as more hooks are added.
//
// Deep-copy ownership is tracked with explicit named fields rather than
// one packed buffer -- verbose, but each field's lifetime is obvious at
// the free site, and hk_hook_t is one heap object per hook, not something
// stored inline in a hot array where the extra fields would cost anything
// that matters. Which fields are populated depends on spec.target_kind;
// unused ones stay NULL/zero (calloc'd).
struct hk_hook {
    hk_id_t hook_id;
    char *stable_hook_id_owned;

    hk_hook_spec_t spec;  // deep-copied; target's string/bytes pointers repointed at the owned_* fields below

    char *owned_symbol_name;
    char *owned_symbol_defining_image_path;
    char *owned_symbol_caller_image_scope_path;

    char *owned_address_expected_image_path;
    uint8_t *owned_address_expected_initial_bytes;

    char *owned_objc_class_name;
    char *owned_objc_selector_name;

    char *owned_memory_base_image_path;
    uint8_t *owned_memory_replacement_bytes;
    uint8_t *owned_memory_expected_bytes;
    uint8_t *owned_memory_expected_mask;

    // commit_after in the public spec is an array of hk_hook_t* the
    // caller's buffer need not outlive the call -- copied into an owned
    // array here. The hk_hook_t* elements themselves are references to
    // other hooks this plan already owns, not deep-copied targets.
    const hk_hook_t **owned_commit_after;

    // Set by hk_hook_analyze_one when a route is found (NULL otherwise).
    // Not owned: points at a caller/test-registered vtable that outlives
    // the hook, same non-ownership convention as hk_runtime_t.engines[].
    // hk_plan_prepare reads this to call the SAME engine that analysis
    // found eligible, rather than re-searching (and potentially finding a
    // different one if the registry changed between analyze and prepare).
    const hk_engine_vtable_t *matched_engine;

    // Set by hk_plan_commit when this hook goes ACTIVE and its engine
    // published an original. NOT owned: points into the process-global
    // installed registry (HKInstalled.h), which outlives this hook -- that
    // survival is the whole point (a live replacement loads through the
    // slot long after the plan/hook are released).
    hk_installed_hook_t *installed;

    hk_hook_result_t result;
};

struct hk_plan {
    hk_id_t plan_id;
    hk_runtime_t *runtime;   // not owned; caller keeps the runtime alive
    hk_plan_config_t config;
    hk_plan_state_t state;

    struct hk_domain **domains;
    size_t domain_count;
    size_t domain_capacity;

    struct hk_hook **hooks;
    size_t hook_count;
    size_t hook_capacity;
};

#endif // HK_CORE_PLAN_INTERNAL_H
