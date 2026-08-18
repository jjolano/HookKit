// Chained fixups, metadata half. See HKChainedFixups.h for the split and for
// why decoding uses shifts/masks rather than the vendored bitfield structs.

#include "HKChainedFixups.h"

#include <string.h>

static uint32_t read_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Returns the NUL-terminated string at `offset` within the pool, or NULL if
// the offset is out of range or the string never terminates inside it.
static const char *pool_string(const uint8_t *pool, size_t pool_size, uint64_t offset) {
    if (offset >= (uint64_t)pool_size) {
        return NULL;
    }
    for (size_t i = (size_t)offset; i < pool_size; i++) {
        if (pool[i] == '\0') {
            return (const char *)pool + offset;
        }
    }
    return NULL;
}

static uint32_t import_entry_size(uint32_t format) {
    switch (format) {
    case HK_CHAINED_IMPORT:          return HK_CHAINED_IMPORT_SIZE;
    case HK_CHAINED_IMPORT_ADDEND:   return HK_CHAINED_IMPORT_ADDEND_SIZE;
    case HK_CHAINED_IMPORT_ADDEND64: return HK_CHAINED_IMPORT_ADDEND64_SIZE;
    default:                         return 0;
    }
}

hk_chained_status_t hk_chained_fixups_parse(const void *blob, size_t size,
                                            hk_chained_fixups_t *out_fixups) {
    if (!blob || !out_fixups) {
        return HK_CHAINED_INVALID_ARGUMENT;
    }
    if (size < HK_CHAINED_HEADER_SIZE) {
        return HK_CHAINED_MALFORMED;
    }
    const uint8_t *base = (const uint8_t *)blob;

    memset(out_fixups, 0, sizeof(*out_fixups));
    out_fixups->blob = base;
    out_fixups->size = size;
    out_fixups->fixups_version = read_u32(base + 0);
    out_fixups->starts_offset  = read_u32(base + 4);
    out_fixups->imports_offset = read_u32(base + 8);
    out_fixups->symbols_offset = read_u32(base + 12);
    out_fixups->imports_count  = read_u32(base + 16);
    out_fixups->imports_format = read_u32(base + 20);
    out_fixups->symbols_format = read_u32(base + 24);

    // Only version 0 is defined. A newer payload may lay its fields out
    // differently, so refuse rather than misparse it as if it were version 0.
    if (out_fixups->fixups_version != 0) {
        return HK_CHAINED_UNSUPPORTED_VERSION;
    }
    // zlib-compressed symbol pools would need a decompressor. Refused with a
    // distinct status rather than read as if the bytes were text.
    if (out_fixups->symbols_format != HK_CHAINED_SYMBOLS_UNCOMPRESSED) {
        return HK_CHAINED_UNSUPPORTED_FORMAT;
    }

    uint32_t entry_size = import_entry_size(out_fixups->imports_format);
    if (out_fixups->imports_count > 0 && entry_size == 0) {
        return HK_CHAINED_UNSUPPORTED_FORMAT;  // unknown imports_format
    }
    out_fixups->import_entry_size = entry_size;

    // Every declared region must lie inside the blob.
    if (out_fixups->starts_offset > size ||
        out_fixups->imports_offset > size ||
        out_fixups->symbols_offset > size) {
        return HK_CHAINED_MALFORMED;
    }
    uint64_t imports_bytes = (uint64_t)out_fixups->imports_count * (uint64_t)entry_size;
    if (imports_bytes > (uint64_t)(size - out_fixups->imports_offset)) {
        return HK_CHAINED_MALFORMED;
    }
    // The symbol pool has no declared length: it runs to the end of the blob.
    out_fixups->symbols_size = size - out_fixups->symbols_offset;
    return HK_CHAINED_OK;
}

hk_chained_status_t hk_chained_import_at(const hk_chained_fixups_t *fixups,
                                         uint32_t index,
                                         hk_chained_import_t *out_import) {
    if (!fixups || !fixups->blob || !out_import) {
        return HK_CHAINED_INVALID_ARGUMENT;
    }
    if (index >= fixups->imports_count) {
        return HK_CHAINED_NOT_FOUND;
    }
    const uint8_t *entry = fixups->blob + fixups->imports_offset +
                           (size_t)index * fixups->import_entry_size;

    memset(out_import, 0, sizeof(*out_import));
    out_import->index = index;

    uint64_t name_offset = 0;
    switch (fixups->imports_format) {
    case HK_CHAINED_IMPORT:
    case HK_CHAINED_IMPORT_ADDEND: {
        // lib_ordinal:8, weak_import:1, name_offset:23
        uint32_t word = read_u32(entry);
        out_import->lib_ordinal = (int32_t)(int8_t)(uint8_t)(word & 0xffu);
        out_import->weak_import = ((word >> 8) & 0x1u) != 0;
        name_offset = (word >> 9) & 0x7fffffu;
        if (fixups->imports_format == HK_CHAINED_IMPORT_ADDEND) {
            int32_t addend;
            memcpy(&addend, entry + 4, sizeof(addend));
            out_import->addend = addend;
        }
        break;
    }
    case HK_CHAINED_IMPORT_ADDEND64: {
        // lib_ordinal:16, weak_import:1, reserved:15, name_offset:32
        uint64_t word = read_u64(entry);
        out_import->lib_ordinal = (int32_t)(int16_t)(uint16_t)(word & 0xffffu);
        out_import->weak_import = ((word >> 16) & 0x1u) != 0;
        name_offset = (word >> 32) & 0xffffffffull;
        out_import->addend = (int64_t)read_u64(entry + 8);
        break;
    }
    default:
        return HK_CHAINED_UNSUPPORTED_FORMAT;
    }

    const char *name = pool_string(fixups->blob + fixups->symbols_offset,
                                   fixups->symbols_size, name_offset);
    if (!name) {
        return HK_CHAINED_MALFORMED;
    }
    out_import->symbol_name = name;
    return HK_CHAINED_OK;
}

hk_chained_status_t hk_chained_imports_find(const hk_chained_fixups_t *fixups,
                                            const char *name,
                                            hk_symbol_name_convention_t convention,
                                            hk_chained_import_t *out_import) {
    if (!fixups || !name || !out_import) {
        return HK_CHAINED_INVALID_ARGUMENT;
    }
    // The same normalization rule as every other resolver, from one place.
    hk_symbol_candidates_t candidates;
    hk_resolve_status_t rc = hk_symbol_build_candidates(name, convention, &candidates);
    if (rc == HK_RESOLVE_NAME_TOO_LONG) {
        return HK_CHAINED_NAME_TOO_LONG;
    }
    if (rc != HK_RESOLVE_OK) {
        return HK_CHAINED_INVALID_ARGUMENT;
    }

    for (uint32_t i = 0; i < fixups->imports_count; i++) {
        hk_chained_import_t import;
        hk_chained_status_t status = hk_chained_import_at(fixups, i, &import);
        if (status != HK_CHAINED_OK) {
            return status;  // a malformed entry is not something to skip past
        }
        for (unsigned c = 0; c < candidates.count; c++) {
            if (strcmp(import.symbol_name, candidates.names[c]) == 0) {
                *out_import = import;
                return HK_CHAINED_OK;
            }
        }
    }
    return HK_CHAINED_NOT_FOUND;
}
