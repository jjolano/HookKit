// Artifact ledger + the immutable snapshot it produces. See
// HKArtifactLedger.h for the write side and the ownership-honesty note; the
// snapshot read side (count/copy_at/release) is declared publicly in
// HookKitArtifacts.h and defined here against the snapshot type below.

#include "HKArtifactLedger.h"

#include <stdlib.h>
#include <string.h>

// Mutable, append-only. Grows geometrically -- the same array-doubling the
// plan's hook array uses (HKPlan.c). Stores hk_artifact_t by value, not a
// pointer array, because unlike hooks/domains these records are never
// individually handed back as stable handles -- callers only ever see deep
// copies inside a snapshot, so nothing points into this storage across a
// realloc.
struct hk_artifact_ledger {
    hk_artifact_t *items;
    size_t count;
    size_t capacity;
};

// Immutable: fixed-size array captured at snapshot time. No capacity field
// because it never grows.
struct hk_artifact_snapshot {
    hk_artifact_t *items;
    size_t count;
};

hk_artifact_ledger_t *hk_artifact_ledger_create(void) {
    return (hk_artifact_ledger_t *)calloc(1, sizeof(hk_artifact_ledger_t));
}

void hk_artifact_ledger_destroy(hk_artifact_ledger_t *ledger) {
    if (!ledger) {
        return;
    }
    free(ledger->items);
    free(ledger);
}

bool hk_artifact_ledger_append(hk_artifact_ledger_t *ledger,
                               const hk_artifact_t *artifact) {
    if (!ledger || !artifact) {
        return false;
    }
    if (ledger->count == ledger->capacity) {
        size_t new_capacity = ledger->capacity == 0 ? 4 : ledger->capacity * 2;
        hk_artifact_t *grown = (hk_artifact_t *)realloc(
            ledger->items, new_capacity * sizeof(hk_artifact_t));
        if (!grown) {
            return false;
        }
        ledger->items = grown;
        ledger->capacity = new_capacity;
    }
    ledger->items[ledger->count] = *artifact;  // value copy (see header note)
    ledger->count++;
    return true;
}

size_t hk_artifact_ledger_count(const hk_artifact_ledger_t *ledger) {
    return ledger ? ledger->count : 0;
}

hk_status_t hk_artifact_snapshot_from_ledger(const hk_artifact_ledger_t *ledger,
                                             hk_artifact_snapshot_t **out_snapshot) {
    if (!out_snapshot) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_snapshot = NULL;

    hk_artifact_snapshot_t *snapshot =
        (hk_artifact_snapshot_t *)calloc(1, sizeof(hk_artifact_snapshot_t));
    if (!snapshot) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    size_t count = ledger ? ledger->count : 0;
    if (count > 0) {
        snapshot->items = (hk_artifact_t *)malloc(count * sizeof(hk_artifact_t));
        if (!snapshot->items) {
            free(snapshot);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        // Independent array of value copies -- the ledger may grow or be
        // destroyed after this without touching the snapshot.
        memcpy(snapshot->items, ledger->items, count * sizeof(hk_artifact_t));
    }
    snapshot->count = count;
    *out_snapshot = snapshot;
    return HK_STATUS_OK;
}

// --- public read side (declared in HookKitArtifacts.h) ---

size_t hk_artifact_snapshot_count(const hk_artifact_snapshot_t *snapshot) {
    return snapshot ? snapshot->count : 0;
}

hk_status_t hk_artifact_snapshot_copy_at(const hk_artifact_snapshot_t *snapshot,
                                         size_t index,
                                         hk_artifact_t *out_artifact) {
    if (!snapshot || !out_artifact) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    if (index >= snapshot->count) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    // Straight value copy. There is exactly one ABI version (3.0) today, so
    // no struct_size-aware partial copy is needed; when a later ABI adds
    // fields, this is where a size-clamped copy would go (the snapshot item
    // already carries its own struct_size/struct_version from append time).
    *out_artifact = snapshot->items[index];
    return HK_STATUS_OK;
}

void hk_artifact_snapshot_release(hk_artifact_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    free(snapshot->items);
    free(snapshot);
}
