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

// ---- traversal ----------------------------------------------------------

static uint16_t read_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Stride in bytes between consecutive chain entries, per pointer format.
// Returns 0 for a format this parser does not decode -- callers must reject
// that BEFORE walking, since a zero stride is the one way the walk could fail
// to advance.
static uint32_t format_stride(uint16_t pointer_format) {
    switch (pointer_format) {
    case HK_CHAINED_PTR_ARM64E:
    case HK_CHAINED_PTR_ARM64E_USERLAND:
    case HK_CHAINED_PTR_ARM64E_USERLAND24:
        return 8;
    case HK_CHAINED_PTR_64:
    case HK_CHAINED_PTR_64_OFFSET:
        return 4;
    default:
        return 0;
    }
}

static bool format_is_arm64e(uint16_t pointer_format) {
    return pointer_format == HK_CHAINED_PTR_ARM64E ||
           pointer_format == HK_CHAINED_PTR_ARM64E_USERLAND ||
           pointer_format == HK_CHAINED_PTR_ARM64E_USERLAND24;
}

// Decodes the fields shared by bind and rebase entries. `next` sits at the
// same bit position for both within a family, so it is read without first
// knowing which kind this is.
typedef struct {
    bool is_bind;
    bool is_auth;
    uint32_t ordinal;
    uint32_t next;
    uint8_t key;
    uint16_t diversity;
    bool address_diversity;
    int64_t addend;
} chained_ptr_t;

static int64_t sign_extend(uint64_t value, unsigned bits) {
    const uint64_t sign = UINT64_C(1) << (bits - 1u);
    return (int64_t)((value ^ sign) - sign);
}

static void decode_pointer(uint64_t raw, uint16_t pointer_format, chained_ptr_t *out) {
    memset(out, 0, sizeof(*out));
    if (format_is_arm64e(pointer_format)) {
        // ordinal:16|24, ... , next:11 (bits 51-61), bind:62, auth:63
        out->is_auth = ((raw >> 63) & 1u) != 0;
        out->is_bind = ((raw >> 62) & 1u) != 0;
        out->next = (uint32_t)((raw >> 51) & 0x7FFu);
        out->ordinal = (pointer_format == HK_CHAINED_PTR_ARM64E_USERLAND24)
                           ? (uint32_t)(raw & 0xFFFFFFu)   // 24-bit bind
                           : (uint32_t)(raw & 0xFFFFu);
        if (out->is_bind && out->is_auth) {
            out->diversity = (uint16_t)((raw >> 32) & 0xFFFFu);
            out->address_diversity = ((raw >> 48) & 1u) != 0;
            out->key = (uint8_t)((raw >> 49) & 0x3u);
        } else if (out->is_bind) {
            out->addend = sign_extend((raw >> 32) & 0x7FFFFu, 19);
        }
    } else {
        // DYLD_CHAINED_PTR_64 / _64_OFFSET:
        // ordinal:24 (bits 0-23), ..., next:12 (bits 51-62), bind:63
        out->is_auth = false;
        out->is_bind = ((raw >> 63) & 1u) != 0;
        out->next = (uint32_t)((raw >> 51) & 0xFFFu);
        out->ordinal = (uint32_t)(raw & 0xFFFFFFu);
        if (out->is_bind) {
            out->addend = (int64_t)((raw >> 24) & 0xFFu);
        }
    }
}

typedef struct {
    const uint8_t *storage;
    size_t storage_size;
    uint64_t storage_segment_start;
    uint64_t storage_segment_size;
    uint16_t pointer_format;
    uint32_t stride;
    uint64_t segment_start;   // segment_offset
    uint64_t segment_end;     // segment_offset + page_count * page_size
    uint32_t segment_index;
    hk_chained_bind_visit_fn visit;
    void *ctx;
    bool stopped;
} chain_ctx_t;

// Walks one chain from `offset` (an image offset). Returns a status; stops
// early without error if the visitor asks to.
static hk_chained_status_t walk_chain(chain_ctx_t *c, uint64_t offset) {
    for (;;) {
        // Every read is bounded by BOTH the readable image and the segment's
        // own declared span, so a corrupt `next` cannot wander out of either.
        if (offset < c->segment_start || offset > c->segment_end ||
            8u > c->segment_end - offset) {
            return HK_CHAINED_MALFORMED;
        }
        uint64_t within_segment = offset - c->segment_start;
        if (within_segment + 8 > c->storage_segment_size ||
            c->storage_segment_start > c->storage_size ||
            within_segment + 8 > c->storage_size - c->storage_segment_start) {
            return HK_CHAINED_MALFORMED;
        }

        uint64_t raw = read_u64(c->storage + (size_t)c->storage_segment_start +
                                (size_t)within_segment);
        chained_ptr_t ptr;
        decode_pointer(raw, c->pointer_format, &ptr);

        if (ptr.is_bind) {
            hk_chained_bind_t bind;
            memset(&bind, 0, sizeof(bind));
            bind.slot_image_offset = offset;
            bind.import_ordinal = ptr.ordinal;
            bind.segment_index = c->segment_index;
            bind.pointer_format = c->pointer_format;
            bind.is_auth = ptr.is_auth;
            bind.key = ptr.key;
            bind.diversity = ptr.diversity;
            bind.address_diversity = ptr.address_diversity;
            bind.addend = ptr.addend;
            if (!c->visit(c->ctx, &bind)) {
                c->stopped = true;
                return HK_CHAINED_OK;
            }
        }

        if (ptr.next == 0) {
            return HK_CHAINED_OK;  // end of chain
        }
        uint64_t advanced = offset + (uint64_t)ptr.next * (uint64_t)c->stride;
        // Defense in depth: with a nonzero stride this cannot fail, which is
        // exactly why unknown formats are rejected before walking. If a stride
        // of 0 ever reached here, this is what stops the walk spinning.
        if (advanced <= offset) {
            return HK_CHAINED_MALFORMED;
        }
        offset = advanced;
    }
}

static hk_chained_status_t walk_segment(const hk_chained_fixups_t *fixups,
                                        const uint8_t *storage, size_t storage_size,
                                        const hk_chained_segment_mapping_t *mapping,
                                        size_t seg_info_offset, uint32_t segment_index,
                                        hk_chained_bind_visit_fn visit, void *ctx,
                                        bool *stopped) {
    const uint8_t *blob = fixups->blob;
    size_t blob_size = fixups->size;

    if (seg_info_offset > blob_size ||
        blob_size - seg_info_offset < HK_CHAINED_STARTS_IN_SEGMENT_HEADER) {
        return HK_CHAINED_MALFORMED;
    }
    const uint8_t *seg = blob + seg_info_offset;

    uint32_t declared_size = read_u32(seg + 0);
    uint16_t page_size     = read_u16(seg + 4);
    uint16_t ptr_format    = read_u16(seg + 6);
    uint64_t segment_off   = read_u64(seg + 8);
    uint16_t page_count    = read_u16(seg + 20);

    if (page_size == 0) {
        return HK_CHAINED_MALFORMED;  // would make every page start identical
    }
    uint32_t stride = format_stride(ptr_format);
    if (stride == 0) {
        // Rejected BEFORE any walking: decoding with the wrong bit layout
        // would silently produce nonsense, and a zero stride is the only way
        // the chain walk could fail to advance.
        return HK_CHAINED_UNSUPPORTED_FORMAT;
    }

    // Two extents, and the difference matters. page_start[] proper holds one
    // entry per page. The START_MULTI overflow list is indexed into the SAME
    // array but lives BEYOND page_count -- which is exactly why the struct
    // carries its own `size` separate from what page_count implies. Bounding
    // the overflow list by page_count would wrongly reject every multi-start
    // page. (dyld indexes page_start[] directly for the overflow list too.)
    uint64_t available = (uint64_t)(blob_size - seg_info_offset -
                                    HK_CHAINED_STARTS_IN_SEGMENT_HEADER);
    if ((uint64_t)declared_size < (uint64_t)HK_CHAINED_STARTS_IN_SEGMENT_HEADER) {
        return HK_CHAINED_MALFORMED;
    }
    uint64_t starts_extent = (uint64_t)declared_size - HK_CHAINED_STARTS_IN_SEGMENT_HEADER;
    if (starts_extent > available) {
        return HK_CHAINED_MALFORMED;
    }
    uint64_t page_start_bytes = (uint64_t)page_count * sizeof(uint16_t);
    if (page_start_bytes > starts_extent) {
        return HK_CHAINED_MALFORMED;
    }

    chain_ctx_t c;
    c.storage = storage;
    c.storage_size = storage_size;
    c.storage_segment_start = mapping ? mapping->file_offset : segment_off;
    c.storage_segment_size = mapping ? mapping->file_size
                                     : (uint64_t)storage_size -
                                           (segment_off <= storage_size ? segment_off
                                                                        : storage_size);
    if (mapping && mapping->image_offset != segment_off) {
        return HK_CHAINED_MALFORMED;
    }
    c.pointer_format = ptr_format;
    c.stride = stride;
    c.segment_start = segment_off;
    uint64_t segment_span = (uint64_t)page_count * (uint64_t)page_size;
    if (segment_span > UINT64_MAX - segment_off) {
        return HK_CHAINED_MALFORMED;
    }
    c.segment_end = segment_off + segment_span;
    c.segment_index = segment_index;
    c.visit = visit;
    c.ctx = ctx;
    c.stopped = false;

    const uint8_t *page_start = seg + HK_CHAINED_STARTS_IN_SEGMENT_HEADER;

    for (uint16_t page = 0; page < page_count; page++) {
        uint16_t start = read_u16(page_start + (size_t)page * sizeof(uint16_t));
        if (start == HK_CHAINED_PTR_START_NONE) {
            continue;  // no fixups on this page
        }
        uint64_t page_base = segment_off + (uint64_t)page * (uint64_t)page_size;

        if ((start & HK_CHAINED_PTR_START_MULTI) != 0) {
            // Overflow list: the low bits index a run of chain_starts[] that
            // follows page_start[], the last entry flagged with START_LAST.
            uint32_t index = start & ~HK_CHAINED_PTR_START_MULTI;
            for (;;) {
                uint64_t entry_off = (uint64_t)index * sizeof(uint16_t);
                if (entry_off + sizeof(uint16_t) > starts_extent) {
                    return HK_CHAINED_MALFORMED;  // list runs past the starts array
                }
                uint16_t entry = read_u16(page_start + (size_t)entry_off);
                uint16_t offset_in_page = entry & ~HK_CHAINED_PTR_START_LAST;

                hk_chained_status_t status = walk_chain(&c, page_base + offset_in_page);
                if (status != HK_CHAINED_OK) {
                    return status;
                }
                if (c.stopped) {
                    *stopped = true;
                    return HK_CHAINED_OK;
                }
                if ((entry & HK_CHAINED_PTR_START_LAST) != 0) {
                    break;
                }
                index++;
            }
            continue;
        }

        hk_chained_status_t status = walk_chain(&c, page_base + start);
        if (status != HK_CHAINED_OK) {
            return status;
        }
        if (c.stopped) {
            *stopped = true;
            return HK_CHAINED_OK;
        }
    }
    return HK_CHAINED_OK;
}

static hk_chained_status_t iterate_binds(
    const hk_chained_fixups_t *fixups, const void *storage_base,
    size_t storage_size, const hk_chained_segment_mapping_t *segments,
    uint32_t segment_count, hk_chained_bind_visit_fn visit, void *ctx) {
    if (!fixups || !fixups->blob || !storage_base || !visit ||
        (segments && segment_count == 0)) {
        return HK_CHAINED_INVALID_ARGUMENT;
    }
    const uint8_t *blob = fixups->blob;
    size_t blob_size = fixups->size;
    size_t starts = fixups->starts_offset;

    if (starts > blob_size || blob_size - starts < sizeof(uint32_t)) {
        return HK_CHAINED_MALFORMED;
    }
    uint32_t seg_count = read_u32(blob + starts);
    uint64_t table_bytes = (uint64_t)seg_count * sizeof(uint32_t);
    if (table_bytes > (uint64_t)(blob_size - starts - sizeof(uint32_t))) {
        return HK_CHAINED_MALFORMED;
    }

    const uint8_t *storage = (const uint8_t *)storage_base;
    bool stopped = false;

    for (uint32_t i = 0; i < seg_count && !stopped; i++) {
        uint32_t seg_info_offset =
            read_u32(blob + starts + sizeof(uint32_t) + (size_t)i * sizeof(uint32_t));
        if (seg_info_offset == 0) {
            continue;  // this segment has no fixups
        }
        if (segments && i >= segment_count) {
            return HK_CHAINED_MALFORMED;
        }
        if (seg_info_offset > blob_size - starts) {
            return HK_CHAINED_MALFORMED;
        }
        // seg_info_offset is relative to the starts_in_image struct.
        hk_chained_status_t status = walk_segment(fixups, storage, storage_size,
                                                  segments ? &segments[i] : NULL,
                                                  starts + seg_info_offset, i,
                                                  visit, ctx, &stopped);
        if (status != HK_CHAINED_OK) {
            return status;
        }
    }
    return HK_CHAINED_OK;
}

hk_chained_status_t hk_chained_fixups_iterate_binds(const hk_chained_fixups_t *fixups,
                                                    const void *image_base,
                                                    size_t image_size,
                                                    hk_chained_bind_visit_fn visit,
                                                    void *ctx) {
    return iterate_binds(fixups, image_base, image_size, NULL, 0, visit, ctx);
}

hk_chained_status_t hk_chained_fixups_iterate_file_binds(
    const hk_chained_fixups_t *fixups, const void *file_base, size_t file_size,
    const hk_chained_segment_mapping_t *segments, uint32_t segment_count,
    hk_chained_bind_visit_fn visit, void *ctx) {
    if (!segments) {
        return HK_CHAINED_INVALID_ARGUMENT;
    }
    return iterate_binds(fixups, file_base, file_size, segments, segment_count,
                         visit, ctx);
}
