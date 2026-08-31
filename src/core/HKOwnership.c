// Process-lifetime target ownership ledger. Keys are canonical byte strings
// built from target identity fields, not stable_hook_id or replacement
// pointers, so two consumers can intentionally chain the same target.

#include "HKOwnership.h"

#include "../internal/HKPointerAuth.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} hk_key_builder_t;

typedef struct hk_ownership_record {
    uint8_t *key;
    size_t key_size;
    char *engine_id;
    void *head_replacement;
    void *predecessor;
    struct hk_ownership_record *next;
} hk_ownership_record_t;

static pthread_mutex_t g_ownership_lock = PTHREAD_MUTEX_INITIALIZER;
static hk_ownership_record_t *g_ownership_records;

static bool key_append(hk_key_builder_t *builder,
                       const void *data,
                       size_t size) {
    if (size > SIZE_MAX - builder->size) {
        return false;
    }
    size_t needed = builder->size + size;
    if (needed > builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity : 64;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        uint8_t *grown = realloc(builder->data, capacity);
        if (!grown) {
            return false;
        }
        builder->data = grown;
        builder->capacity = capacity;
    }
    if (size > 0) {
        memcpy(builder->data + builder->size, data, size);
    }
    builder->size = needed;
    return true;
}

static bool key_u8(hk_key_builder_t *builder, uint8_t value) {
    return key_append(builder, &value, sizeof(value));
}

static bool key_u32(hk_key_builder_t *builder, uint32_t value) {
    return key_append(builder, &value, sizeof(value));
}

static bool key_u64(hk_key_builder_t *builder, uint64_t value) {
    return key_append(builder, &value, sizeof(value));
}

static bool key_string(hk_key_builder_t *builder, const char *value) {
    uint64_t length = value ? (uint64_t)strlen(value) : UINT64_MAX;
    if (!key_u64(builder, length)) {
        return false;
    }
    return length == UINT64_MAX ||
           key_append(builder, value, (size_t)length);
}

#define HK_OWNERSHIP_MAX_IMAGE_SELECTOR_DEPTH 32u

typedef struct {
    uint8_t *data;
    size_t size;
} hk_key_blob_t;

static void key_blobs_destroy(hk_key_blob_t *blobs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(blobs[i].data);
    }
    free(blobs);
}

static int key_blob_compare(const void *left, const void *right) {
    const hk_key_blob_t *a = left;
    const hk_key_blob_t *b = right;
    size_t common = a->size < b->size ? a->size : b->size;
    int result = memcmp(a->data, b->data, common);
    if (result != 0) {
        return result;
    }
    return a->size > b->size ? 1 : a->size < b->size ? -1 : 0;
}

static bool key_selector(hk_key_builder_t *builder,
                         const hk_image_selector_t *selector,
                         unsigned depth);

static bool key_blob_append(hk_key_blob_t **blobs,
                            size_t *count,
                            size_t *capacity,
                            hk_key_builder_t *key) {
    if (*count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 4;
        if (grown < *capacity || grown > SIZE_MAX / sizeof(**blobs)) {
            return false;
        }
        hk_key_blob_t *resized = realloc(*blobs, grown * sizeof(**blobs));
        if (!resized) {
            return false;
        }
        *blobs = resized;
        *capacity = grown;
    }
    (*blobs)[*count].data = key->data;
    (*blobs)[*count].size = key->size;
    key->data = NULL;
    key->size = 0;
    (*count)++;
    return true;
}

static bool key_selector_collect(hk_key_blob_t **blobs,
                                 size_t *count,
                                 size_t *capacity,
                                 const hk_image_selector_t *selector,
                                 unsigned depth) {
    if (!selector || depth > HK_OWNERSHIP_MAX_IMAGE_SELECTOR_DEPTH) {
        return false;
    }
    if (selector->kind == HK_IMAGE_EXPLICIT_SET) {
        if (selector->explicit_set_count > 0 && !selector->explicit_set) {
            return false;
        }
        for (size_t i = 0; i < selector->explicit_set_count; i++) {
            if (!key_selector_collect(blobs, count, capacity,
                                      selector->explicit_set[i], depth + 1)) {
                return false;
            }
        }
        return true;
    }

    hk_key_builder_t key = {0};
    if (!key_selector(&key, selector, depth) ||
        !key_blob_append(blobs, count, capacity, &key)) {
        free(key.data);
        return false;
    }
    return true;
}

static bool key_selector(hk_key_builder_t *builder,
                          const hk_image_selector_t *selector,
                          unsigned depth) {
    if (!selector || depth > HK_OWNERSHIP_MAX_IMAGE_SELECTOR_DEPTH) {
        return false;
    }

    switch (selector->kind) {
    case HK_IMAGE_ANY_LOADED:
    case HK_IMAGE_MAIN_EXECUTABLE:
        return key_u32(builder, (uint32_t)selector->kind);
    case HK_IMAGE_EXACT_PATH:
        return key_u32(builder, (uint32_t)selector->kind) &&
               key_string(builder, selector->path);
    case HK_IMAGE_EXACT_UUID:
        return key_u32(builder, (uint32_t)selector->kind) &&
               key_append(builder, selector->uuid, sizeof(selector->uuid));
    case HK_IMAGE_EXACT_HEADER:
        return key_u32(builder, (uint32_t)selector->kind) &&
               key_u64(builder, (uint64_t)(uintptr_t)selector->header);
    case HK_IMAGE_EXPLICIT_SET: {
        hk_key_blob_t *children = NULL;
        size_t count = 0;
        size_t capacity = 0;
        if (!key_selector_collect(&children, &count, &capacity, selector, depth)) {
            key_blobs_destroy(children, count);
            return false;
        }
        if (count > 1) {
            qsort(children, count, sizeof(*children), key_blob_compare);
        }

        size_t unique_count = 0;
        for (size_t i = 0; i < count; i++) {
            if (unique_count > 0 &&
                key_blob_compare(&children[unique_count - 1], &children[i]) == 0) {
                free(children[i].data);
                children[i].data = NULL;
                continue;
            }
            if (unique_count != i) {
                children[unique_count] = children[i];
                children[i].data = NULL;
            }
            unique_count++;
        }

        bool ok = key_u32(builder, HK_IMAGE_EXPLICIT_SET) &&
                  key_u64(builder, (uint64_t)unique_count);
        for (size_t i = 0; ok && i < unique_count; i++) {
            ok = key_u64(builder, (uint64_t)children[i].size) &&
                 key_append(builder, children[i].data, children[i].size);
        }
        key_blobs_destroy(children, count);
        return ok;
    }
    default:
        return key_u32(builder, (uint32_t)selector->kind);
    }
}

static bool key_target(hk_key_builder_t *builder,
                        const hk_hook_spec_t *spec) {
    if (!spec || !key_u32(builder, (uint32_t)spec->target_kind)) {
        return false;
    }
    switch (spec->target_kind) {
    case HK_TARGET_FUNCTION_SYMBOL: {
        return key_string(builder, spec->target.symbol.name) &&
               key_u32(builder, (uint32_t)spec->target.symbol.name_convention) &&
               key_u32(builder, (uint32_t)spec->target.symbol.alias_policy) &&
               key_u8(builder, spec->target.symbol.interior_address_permitted) &&
               key_selector(builder, &spec->target.symbol.defining_image, 0) &&
               key_selector(builder, &spec->target.symbol.caller_image_scope, 0);
    }
    case HK_TARGET_FUNCTION_ADDRESS:
        return key_u64(builder, (uint64_t)(spec->target.address.may_strip_pac_or_thumb_state
                            ? hk_pac_strip_code(spec->target.address.address)
                            : spec->target.address.address));
    case HK_TARGET_OBJC_METHOD: {
        const hk_objc_target_t *objc = &spec->target.objc;
        return key_u8(builder, objc->cls != NULL) &&
               (objc->cls ? key_u64(builder, (uint64_t)(uintptr_t)objc->cls)
                          : key_string(builder, objc->class_name)) &&
               key_u8(builder, objc->sel != NULL) &&
               (objc->sel ? key_u64(builder, (uint64_t)(uintptr_t)objc->sel)
                          : key_string(builder, objc->selector_name)) &&
               key_u32(builder, (uint32_t)spec->target.objc.method_kind);
    }
    case HK_TARGET_MEMORY_PATCH:
        return key_u64(builder, (uint64_t)spec->target.memory.address) &&
               key_u8(builder, spec->target.memory.address_is_image_relative) &&
               key_u64(builder, (uint64_t)spec->target.memory.size) &&
               (!spec->target.memory.address_is_image_relative ||
                key_selector(builder, &spec->target.memory.base_image, 0));
    case HK_TARGET_SWIFT_VTABLE:
    default:
        return true;
    }
}

static bool key_build(const hk_hook_spec_t *spec,
                       uint8_t **out_key,
                       size_t *out_size) {
    *out_key = NULL;
    *out_size = 0;
    hk_key_builder_t builder = {0};
    if (!key_target(&builder, spec)) {
        free(builder.data);
        return false;
    }
    *out_key = builder.data;
    *out_size = builder.size;
    return true;
}

bool hk_ownership_target_key_copy(const hk_hook_spec_t *spec,
                                  uint8_t **out_key,
                                  size_t *out_size) {
    if (!out_key || !out_size) {
        return false;
    }
    *out_key = NULL;
    *out_size = 0;
    return key_build(spec, out_key, out_size);
}

static hk_ownership_record_t *find_record(const uint8_t *key,
                                          size_t key_size) {
    for (hk_ownership_record_t *record = g_ownership_records;
         record;
         record = record->next) {
        if (record->key_size == key_size &&
            memcmp(record->key, key, key_size) == 0) {
            return record;
        }
    }
    return NULL;
}

void hk_ownership_lock(void) {
    pthread_mutex_lock(&g_ownership_lock);
}

void hk_ownership_unlock(void) {
    pthread_mutex_unlock(&g_ownership_lock);
}

hk_ownership_status_t hk_ownership_lookup_locked(
    const hk_hook_spec_t *spec,
    hk_ownership_state_t *out_state) {
    if (!out_state) {
        return HK_OWNERSHIP_OUT_OF_MEMORY;
    }
    memset(out_state, 0, sizeof(*out_state));
    uint8_t *key = NULL;
    size_t key_size = 0;
    if (!key_build(spec, &key, &key_size)) {
        return HK_OWNERSHIP_OUT_OF_MEMORY;
    }
    hk_ownership_record_t *record = find_record(key, key_size);
    if (record) {
        out_state->present = true;
        out_state->head_replacement = record->head_replacement;
        out_state->predecessor = record->predecessor;
        out_state->engine_id = record->engine_id;
    }
    free(key);
    return record ? HK_OWNERSHIP_FOUND : HK_OWNERSHIP_NO_RECORD;
}

bool hk_ownership_record_locked(const hk_hook_spec_t *spec,
                                const char *engine_id,
                                void *replacement,
                                void *predecessor) {
    uint8_t *key = NULL;
    size_t key_size = 0;
    if (!key_build(spec, &key, &key_size)) {
        return false;
    }

    hk_ownership_record_t *record = find_record(key, key_size);
    if (record) {
        free(key);
        record->head_replacement = replacement;
        record->predecessor = predecessor;
        return true;
    }

    record = calloc(1, sizeof(*record));
    if (!record) {
        free(key);
        return false;
    }
    if (engine_id) {
        size_t length = strlen(engine_id) + 1;
        record->engine_id = malloc(length);
        if (!record->engine_id) {
            free(record);
            free(key);
            return false;
        }
        memcpy(record->engine_id, engine_id, length);
    }
    record->key = key;
    record->key_size = key_size;
    record->head_replacement = replacement;
    record->predecessor = predecessor;
    record->next = g_ownership_records;
    g_ownership_records = record;
    return true;
}

void hk_ownership_reset_for_testing(void) {
    hk_ownership_lock();
    hk_ownership_record_t *record = g_ownership_records;
    g_ownership_records = NULL;
    hk_ownership_unlock();
    while (record) {
        hk_ownership_record_t *next = record->next;
        free(record->key);
        free(record->engine_id);
        free(record);
        record = next;
    }
}
