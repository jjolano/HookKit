// Process-lifetime target ownership ledger. Keys are canonical byte strings
// built from target identity fields, not stable_hook_id or replacement
// pointers, so two consumers can intentionally chain the same target.

#include "HKOwnership.h"

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

static bool key_selector(hk_key_builder_t *builder,
                         const hk_image_selector_t *selector,
                         unsigned depth) {
    if (!selector || depth > 32u) {
        return key_u8(builder, 0);
    }
    if (!key_u8(builder, 1) ||
        !key_u32(builder, (uint32_t)selector->kind) ||
        !key_u64(builder, (uint64_t)(uintptr_t)selector->header) ||
        !key_append(builder, selector->uuid, sizeof(selector->uuid)) ||
        !key_string(builder, selector->path)) {
        return false;
    }
    if (selector->kind != HK_IMAGE_EXPLICIT_SET) {
        return key_u64(builder, 0);
    }
    if (selector->explicit_set_count > 0 && !selector->explicit_set) {
        return false;
    }
    if (!key_u64(builder, (uint64_t)selector->explicit_set_count)) {
        return false;
    }
    for (size_t i = 0; i < selector->explicit_set_count; i++) {
        if (!key_selector(builder, selector->explicit_set[i], depth + 1)) {
            return false;
        }
    }
    return true;
}

static const char *normalized_symbol_name(const hk_symbol_target_t *symbol,
                                          char *storage,
                                          size_t storage_size) {
    const char *name = symbol->name ? symbol->name : "";
    if (symbol->name_convention == HK_SYMBOL_NAME_C && name[0] == '_') {
        name++;
    }
    size_t length = strlen(name);
    if (length + 1 > storage_size) {
        return name;  // the caller only uses this for a length-prefixed copy
    }
    memcpy(storage, name, length + 1);
    return storage;
}

static bool key_target(hk_key_builder_t *builder,
                       const hk_hook_spec_t *spec) {
    if (!spec || !key_u32(builder, (uint32_t)spec->target_kind)) {
        return false;
    }
    switch (spec->target_kind) {
    case HK_TARGET_FUNCTION_SYMBOL: {
        char normalized[1];
        const char *name = normalized_symbol_name(
            &spec->target.symbol, normalized, sizeof(normalized));
        return key_string(builder, name) &&
               key_u32(builder, (uint32_t)spec->target.symbol.name_convention) &&
               key_u32(builder, (uint32_t)spec->target.symbol.alias_policy) &&
               key_u8(builder, spec->target.symbol.interior_address_permitted) &&
               key_selector(builder, &spec->target.symbol.defining_image, 0) &&
               key_selector(builder, &spec->target.symbol.caller_image_scope, 0);
    }
    case HK_TARGET_FUNCTION_ADDRESS:
        return key_u64(builder, (uint64_t)spec->target.address.address) &&
               key_u8(builder, spec->target.address.expected_uuid_present) &&
               key_append(builder, spec->target.address.expected_uuid,
                          sizeof(spec->target.address.expected_uuid)) &&
               key_u8(builder, spec->target.address.may_strip_pac_or_thumb_state) &&
               key_selector(builder, &spec->target.address.expected_image, 0);
    case HK_TARGET_OBJC_METHOD:
        return key_u64(builder, (uint64_t)(uintptr_t)spec->target.objc.cls) &&
               key_string(builder, spec->target.objc.class_name) &&
               key_u64(builder, (uint64_t)(uintptr_t)spec->target.objc.sel) &&
               key_string(builder, spec->target.objc.selector_name) &&
               key_u32(builder, (uint32_t)spec->target.objc.method_kind);
    case HK_TARGET_MEMORY_PATCH:
        return key_u64(builder, (uint64_t)spec->target.memory.address) &&
               key_u8(builder, spec->target.memory.address_is_image_relative) &&
               key_u64(builder, (uint64_t)spec->target.memory.size) &&
               key_u32(builder, (uint32_t)spec->target.memory.kind) &&
               key_selector(builder, &spec->target.memory.base_image, 0);
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

hk_ownership_status_t hk_ownership_targets_equal(
    const hk_hook_spec_t *left,
    const hk_hook_spec_t *right,
    bool *out_equal) {
    if (!out_equal) {
        return HK_OWNERSHIP_OUT_OF_MEMORY;
    }
    *out_equal = false;
    uint8_t *left_key = NULL;
    uint8_t *right_key = NULL;
    size_t left_size = 0;
    size_t right_size = 0;
    if (!key_build(left, &left_key, &left_size) ||
        !key_build(right, &right_key, &right_size)) {
        free(left_key);
        free(right_key);
        return HK_OWNERSHIP_OUT_OF_MEMORY;
    }
    *out_equal = left_size == right_size &&
                 memcmp(left_key, right_key, left_size) == 0;
    free(left_key);
    free(right_key);
    return HK_OWNERSHIP_NO_RECORD;
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
