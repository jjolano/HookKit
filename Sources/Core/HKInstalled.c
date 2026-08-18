// Process-lifetime installed records + the public original-slot/installed-
// hook accessors. See HKInstalled.h for why these records are never freed
// in production.

#include "HKInstalled.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// The process-global registry of installed records. A plain mutex, not a
// lock-free push: creation happens once per active hook at commit, never on
// a hot path, so simple-and-correct wins.
// ponytail: global registry mutex; commit is not hot, per-record locking would buy nothing.
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static hk_installed_hook_t *g_registry_head;

hk_installed_hook_t *hk_installed_record_create(hk_id_t installed_id,
                                                void *original_or_null,
                                                const hk_hook_result_t *result) {
    if (!result) {
        return NULL;
    }
    hk_installed_hook_t *record =
        (hk_installed_hook_t *)calloc(1, sizeof(hk_installed_hook_t));
    if (!record) {
        return NULL;
    }
    record->installed_id = installed_id;
    record->has_original = (original_or_null != NULL);
    atomic_init(&record->slot.original, original_or_null);
    record->result = *result;  // value snapshot

    pthread_mutex_lock(&g_registry_lock);
    record->next = g_registry_head;
    g_registry_head = record;
    pthread_mutex_unlock(&g_registry_lock);
    return record;
}

void hk_installed_reset_for_testing(void) {
    pthread_mutex_lock(&g_registry_lock);
    hk_installed_hook_t *cur = g_registry_head;
    g_registry_head = NULL;
    pthread_mutex_unlock(&g_registry_lock);

    while (cur) {
        hk_installed_hook_t *next = cur->next;
        free(cur);
        cur = next;
    }
}

// --- public accessors (declared in HookKitPlan.h) ---

void *hk_original_slot_load(const hk_original_slot_t *slot) {
    if (!slot) {
        return NULL;
    }
    // Cast away const only to satisfy the atomic-load signature; the load
    // itself does not modify the slot.
    return atomic_load((_Atomic(void *) *)&slot->original);
}

hk_status_t hk_installed_hook_copy_result(const hk_installed_hook_t *installed,
                                          hk_hook_result_t *out_result) {
    if (!installed || !out_result) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_result = installed->result;
    return HK_STATUS_OK;
}
