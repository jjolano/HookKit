// Artifact ledger + the immutable snapshot it produces. See
// HKArtifactLedger.h for the write side and the ownership-honesty note; the
// snapshot read side (count/copy_at/release) is declared publicly in
// HookKitArtifacts.h and defined here against the snapshot type below.

#include "HKArtifactLedger.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "HKIDs.h"

typedef struct {
    hk_artifact_t value;
    char *engine_id;
    char *mechanism_id;
    char *image_path;
    uint8_t *original_bytes;
    uint8_t *expected_bytes;
    uint8_t *expected_mask;
    uint8_t *current_bytes;
} hk_owned_artifact_t;

// Mutable, append-only. Grows geometrically -- the same array-doubling the
// plan's hook array uses (HKPlan.c). Every borrowed view is copied beside the
// value so snapshots do not depend on an engine's temporary storage.
struct hk_artifact_ledger {
    hk_owned_artifact_t *items;
    size_t count;
    size_t capacity;
};

// Immutable: fixed-size array captured at snapshot time. No capacity field
// because it never grows.
struct hk_artifact_snapshot {
    hk_owned_artifact_t *items;
    size_t count;
};

static void owned_release(hk_owned_artifact_t *item) {
    if (!item) {
        return;
    }
    free(item->engine_id);
    free(item->mechanism_id);
    free(item->image_path);
    free(item->original_bytes);
    free(item->expected_bytes);
    free(item->expected_mask);
    free(item->current_bytes);
    memset(item, 0, sizeof(*item));
}

static bool copy_string_view(hk_string_view_t *view, char **owned) {
    if (!view->data) {
        return true;
    }
    if (view->length == SIZE_MAX) {
        return false;
    }
    char *copy = (char *)malloc(view->length + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, view->data, view->length);
    copy[view->length] = '\0';
    *owned = copy;
    view->data = copy;
    return true;
}

static bool copy_image_path(hk_image_identity_t *image, char **owned) {
    if (!image->path) {
        return true;
    }
    size_t length = strlen(image->path);
    if (length == SIZE_MAX) {
        return false;
    }
    char *copy = (char *)malloc(length + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, image->path, length + 1);
    *owned = copy;
    image->path = copy;
    return true;
}

static bool copy_inline_bytes(hk_byte_storage_t *storage, uint8_t **owned) {
    if (storage->representation != HK_BYTE_STORAGE_INLINE &&
        storage->representation != HK_BYTE_STORAGE_INLINE_AND_HASH) {
        return true;
    }
    if (storage->inline_bytes.size == 0) {
        return true;
    }
    if (!storage->inline_bytes.data) {
        return false;
    }
    uint8_t *copy = (uint8_t *)malloc(storage->inline_bytes.size);
    if (!copy) {
        return false;
    }
    memcpy(copy, storage->inline_bytes.data, storage->inline_bytes.size);
    *owned = copy;
    storage->inline_bytes.data = copy;
    return true;
}

static bool owned_copy(const hk_artifact_t *source, hk_owned_artifact_t *out) {
    memset(out, 0, sizeof(*out));
    out->value = *source;
    if (!copy_string_view(&out->value.engine_id, &out->engine_id) ||
        !copy_string_view(&out->value.mechanism_id, &out->mechanism_id) ||
        !copy_image_path(&out->value.image, &out->image_path) ||
        !copy_inline_bytes(&out->value.original_bytes, &out->original_bytes) ||
        !copy_inline_bytes(&out->value.expected_bytes, &out->expected_bytes) ||
        !copy_inline_bytes(&out->value.expected_mask, &out->expected_mask) ||
        !copy_inline_bytes(&out->value.current_bytes, &out->current_bytes)) {
        owned_release(out);
        return false;
    }
    return true;
}

static bool ensure_capacity(hk_artifact_ledger_t *ledger, size_t needed) {
    if (needed <= ledger->capacity) {
        return true;
    }
    size_t capacity = ledger->capacity == 0 ? 4 : ledger->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    hk_owned_artifact_t *grown = (hk_owned_artifact_t *)realloc(
        ledger->items, capacity * sizeof(*grown));
    if (!grown) {
        return false;
    }
    ledger->items = grown;
    ledger->capacity = capacity;
    return true;
}

static hk_artifact_ledger_t *g_process_ledger;
static pthread_mutex_t g_process_ledger_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_process_cleanup_registered;

static void hk_artifact_process_cleanup(void) {
    pthread_mutex_lock(&g_process_ledger_lock);
    hk_artifact_ledger_destroy(g_process_ledger);
    g_process_ledger = NULL;
    pthread_mutex_unlock(&g_process_ledger_lock);
}

hk_artifact_ledger_t *hk_artifact_ledger_create(void) {
    return (hk_artifact_ledger_t *)calloc(1, sizeof(hk_artifact_ledger_t));
}

void hk_artifact_ledger_destroy(hk_artifact_ledger_t *ledger) {
    if (!ledger) {
        return;
    }
    for (size_t i = 0; i < ledger->count; i++) {
        owned_release(&ledger->items[i]);
    }
    free(ledger->items);
    free(ledger);
}

bool hk_artifact_ledger_append(hk_artifact_ledger_t *ledger,
                               const hk_artifact_t *artifact) {
    if (!ledger || !artifact) {
        return false;
    }
    hk_owned_artifact_t owned;
    if (!owned_copy(artifact, &owned)) {
        return false;
    }
    if (!ensure_capacity(ledger, ledger->count + 1)) {
        owned_release(&owned);
        return false;
    }
    ledger->items[ledger->count] = owned;
    ledger->count++;
    return true;
}

size_t hk_artifact_ledger_count(const hk_artifact_ledger_t *ledger) {
    return ledger ? ledger->count : 0;
}

bool hk_artifact_ledger_append_ledger(hk_artifact_ledger_t *ledger,
                                      const hk_artifact_ledger_t *source) {
    if (!ledger || !source) {
        return false;
    }
    if (source->count == 0) {
        return true;
    }
    if (source->count > SIZE_MAX - ledger->count) {
        return false;
    }
    size_t needed = ledger->count + source->count;
    if (!ensure_capacity(ledger, needed)) {
        return false;
    }
    size_t start = ledger->count;
    for (size_t i = 0; i < source->count; i++) {
        if (!owned_copy(&source->items[i].value, &ledger->items[start + i])) {
            for (size_t j = start; j < start + i; j++) {
                owned_release(&ledger->items[j]);
            }
            return false;
        }
    }
    ledger->count = needed;
    return true;
}

bool hk_artifact_ledger_mark_verified(hk_artifact_ledger_t *ledger,
                                      size_t start,
                                      size_t count) {
    if (!ledger || start > ledger->count || count > ledger->count - start) {
        return false;
    }
    for (size_t i = start; i < start + count; i++) {
        ledger->items[i].value.state = HK_ARTIFACT_VERIFIED;
        ledger->items[i].value.verified = true;
    }
    return true;
}

bool hk_artifact_ledger_mark_compensated(hk_artifact_ledger_t *ledger,
                                         size_t start,
                                         size_t count) {
    if (!ledger || start > ledger->count || count > ledger->count - start) {
        return false;
    }
    for (size_t i = start; i < start + count; i++) {
        ledger->items[i].value.state = HK_ARTIFACT_COMPENSATED;
        ledger->items[i].value.verified = false;
    }
    return true;
}

bool hk_artifact_process_append_ledger(const hk_artifact_ledger_t *source) {
    if (!source) {
        return false;
    }
    pthread_mutex_lock(&g_process_ledger_lock);
    if (!g_process_ledger) {
        g_process_ledger = hk_artifact_ledger_create();
        if (g_process_ledger && !g_process_cleanup_registered) {
            (void)atexit(hk_artifact_process_cleanup);
            g_process_cleanup_registered = true;
        }
    }
    bool ok = g_process_ledger && hk_artifact_ledger_append_ledger(g_process_ledger, source);
    pthread_mutex_unlock(&g_process_ledger_lock);
    return ok;
}

hk_status_t hk_artifact_process_snapshot(hk_artifact_snapshot_t **out_snapshot) {
    if (!out_snapshot) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&g_process_ledger_lock);
    hk_status_t status = hk_artifact_snapshot_from_ledger(g_process_ledger, out_snapshot);
    pthread_mutex_unlock(&g_process_ledger_lock);
    return status;
}

bool hk_artifact_sink_record(hk_artifact_sink_t *sink, const hk_artifact_t *artifact) {
    if (!sink || !artifact || !sink->ledger) {
        if (sink) {
            sink->record_failed = true;
        }
        return false;
    }
    sink->observed_effects |= artifact->effects;
    // Copy the engine's mechanism facts, then overwrite the four contextual
    // IDs -- the sink is authoritative for those, the engine cannot know
    // them (see the header). artifact_id is freshly generated per record;
    // two artifacts from one commit get distinct ids.
    hk_artifact_t stamped = *artifact;
    stamped.artifact_id = hk_id_generate();
    stamped.plan_id = sink->plan_id;
    stamped.request_id = sink->request_id;
    stamped.runtime_owner_id = sink->runtime_owner_id;
    bool ok = hk_artifact_ledger_append(sink->ledger, &stamped);
    if (!ok) {
        sink->record_failed = true;
    }
    return ok;
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
        snapshot->items = (hk_owned_artifact_t *)calloc(count, sizeof(*snapshot->items));
        if (!snapshot->items) {
            free(snapshot);
            return HK_STATUS_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < count; i++) {
            if (!owned_copy(&ledger->items[i].value, &snapshot->items[i])) {
                for (size_t j = 0; j < i; j++) {
                    owned_release(&snapshot->items[j]);
                }
                free(snapshot->items);
                free(snapshot);
                return HK_STATUS_OUT_OF_MEMORY;
            }
        }
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
    *out_artifact = snapshot->items[index].value;
    return HK_STATUS_OK;
}

void hk_artifact_snapshot_release(hk_artifact_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    for (size_t i = 0; i < snapshot->count; i++) {
        owned_release(&snapshot->items[i]);
    }
    free(snapshot->items);
    free(snapshot);
}

hk_status_t hk_copy_process_artifacts(hk_artifact_snapshot_t **out_snapshot) {
    if (out_snapshot) {
        *out_snapshot = NULL;
    }
    return hk_artifact_process_snapshot(out_snapshot);
}
