// Process-lifetime target ownership and chain head tracking.
//
// This is deliberately internal. The public API describes original
// predecessor semantics; this ledger is the small coordination seam that
// lets separately committed same-target hooks pass the current chain head to
// a compatible engine without turning process-wide ownership into an
// exclusivity error.

#ifndef HK_CORE_OWNERSHIP_H
#define HK_CORE_OWNERSHIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/HookKit/HookKitPlan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HK_OWNERSHIP_NO_RECORD = 0,
    HK_OWNERSHIP_FOUND,
    HK_OWNERSHIP_OUT_OF_MEMORY,
} hk_ownership_status_t;

typedef struct {
    bool present;
    void *head_replacement;
    void *predecessor;
    const char *engine_id;  // borrowed from the process-lifetime record
} hk_ownership_state_t;

// The core holds this one process-wide guard across ownership lookup,
// engine mutation, and record update. It is a commit-time lock, not a hook
// dispatch lock: unrelated analysis and preparation remain concurrent.
void hk_ownership_lock(void);
void hk_ownership_unlock(void);

hk_ownership_status_t hk_ownership_lookup_locked(
    const hk_hook_spec_t *spec,
    hk_ownership_state_t *out_state);

bool hk_ownership_record_locked(
    const hk_hook_spec_t *spec,
    const char *engine_id,
    void *replacement,
    void *predecessor);

// Allocates the canonical target identity used by the ownership ledger. The
// caller owns *out_key and frees it with free(). False means allocation or
// input-key construction failed.
bool hk_ownership_target_key_copy(
    const hk_hook_spec_t *spec,
    uint8_t **out_key,
    size_t *out_size);

// Host-test cleanup only. Production ownership intentionally lasts until
// process exit, just like installed original slots.
void hk_ownership_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif // HK_CORE_OWNERSHIP_H
