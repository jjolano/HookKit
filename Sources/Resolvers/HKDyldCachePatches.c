#include "HKDyldCachePatches.h"

#include <limits.h>
#include <string.h>

#if defined(HK_CACHE_PATCH_DIAGNOSTICS)
#include <stdio.h>
static hk_cache_patch_status_t malformed_at(unsigned line) {
    fprintf(stderr, "cache patch malformed at line %u\n", line);
    return (hk_cache_patch_status_t)5;
}
#define HK_CACHE_PATCH_MALFORMED malformed_at(__LINE__)
#endif

#include "HKMachO.h"
#include "HKSymbolResolve.h"

// Stable dyld_cache_header offsets. The header is append-only; mappingOffset
// is also the version boundary used by dyld before reading newer fields.
#define DCH_MAPPING_OFFSET       16u
#define DCH_MAPPING_COUNT        20u
#define DCH_IMAGES_OFFSET_OLD    24u
#define DCH_IMAGES_COUNT_OLD     28u
#define DCH_PATCH_INFO_ADDR     152u
#define DCH_PATCH_INFO_SIZE     160u
#define DCH_FORMAT_FLAGS        220u
#define DCH_SWIFT_OPTS_SIZE     384u
#define DCH_IMAGES_OFFSET       448u
#define DCH_IMAGES_COUNT        452u
#define DCH_IMAGE_INFO_SIZE      32u
#define DCH_MAPPING_INFO_SIZE    32u
#define DCH_TEXT_INFO_SIZE       32u
#define DCH_IMAGES_TEXT_OFFSET  136u
#define DCH_IMAGES_TEXT_COUNT   144u

#define CACHE_IMAGE_PATCH_V1_SIZE     8u
#define CACHE_EXPORT_V1_SIZE         16u
#define CACHE_LOCATION_V1_SIZE       16u
#define CACHE_IMAGE_PATCH_V2_SIZE    16u
#define CACHE_IMAGE_EXPORT_V2_SIZE    8u
#define CACHE_CLIENT_V2_SIZE         12u
#define CACHE_CLIENT_EXPORT_V2_SIZE  12u
#define CACHE_LOCATION_V2_SIZE        8u
#define CACHE_GOT_CLIENT_V3_SIZE      8u
#define CACHE_GOT_EXPORT_V3_SIZE     12u
#define CACHE_GOT_LOCATION_V3_SIZE   16u
#define CACHE_GOT_LOCATION_V4_SIZE   16u

static uint32_t read_u32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static bool range_at(const uint8_t *base, size_t size, uint64_t offset,
                     uint64_t count, size_t stride, const uint8_t **out) {
    if (offset > size || count > (UINT64_MAX / (stride ? stride : 1u))) {
        return false;
    }
    uint64_t bytes = count * stride;
    if (bytes > size - offset) {
        return false;
    }
    *out = base + (size_t)offset;
    return true;
}

typedef struct {
    const uint8_t *base;
    size_t size;
    uint64_t unslid_base;
    uintptr_t slide;
    const uint8_t *patch;
    size_t patch_size;
} cache_view_t;

static bool patch_array(const cache_view_t *view, uint64_t address,
                        uint64_t count, size_t stride, const uint8_t **out) {
    if (address < view->unslid_base || address - view->unslid_base > view->size) {
        return false;
    }
    uint64_t offset = address - view->unslid_base;
    if (!range_at(view->base, view->size, offset, count, stride, out)) {
        return false;
    }
    uintptr_t start = (uintptr_t)*out;
    uintptr_t patch_start = (uintptr_t)view->patch;
    uint64_t bytes = count * stride;
    return start >= patch_start && start - patch_start <= view->patch_size &&
           bytes <= view->patch_size - (start - patch_start);
}

static const char *bounded_string(const uint8_t *pool, size_t size,
                                  uint32_t offset) {
    if (offset >= size) {
        return NULL;
    }
    const uint8_t *start = pool + offset;
    return memchr(start, 0, size - offset) ? (const char *)start : NULL;
}

static bool candidate_matches(const hk_symbol_candidates_t *candidates,
                              const char *name) {
    for (unsigned i = 0; i < candidates->count; i++) {
        if (strcmp(candidates->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static hk_pac_schema_t schema_v1_v3(uint32_t bits) {
    hk_pac_schema_t schema;
    memset(&schema, 0, sizeof(schema));
    schema.authenticated = ((bits >> 12) & 1u) != 0;
    schema.address_diversity = ((bits >> 13) & 1u) != 0;
    schema.key = (hk_pac_key_t)((bits >> 14) & 3u);
    schema.diversity = (uint16_t)(bits >> 16);
    return schema;
}

static uint64_t addend_v1_v3(uint32_t bits) {
    return (bits >> 7) & 0x1Fu;
}

static hk_pac_schema_t schema_v4(uint32_t bits) {
    hk_pac_schema_t schema;
    memset(&schema, 0, sizeof(schema));
    schema.authenticated = (bits & 1u) != 0;
    if (schema.authenticated) {
        schema.address_diversity = ((bits >> 14) & 1u) != 0;
        schema.key = ((bits >> 15) & 1u) ? HK_PAC_KEY_DA : HK_PAC_KEY_IA;
        schema.diversity = (uint16_t)(bits >> 16);
    }
    return schema;
}

static uint64_t addend_v4(uint32_t bits) {
    return (bits & 1u) ? ((bits >> 9) & 0x1Fu) : (bits >> 9);
}

static bool weak_v4(uint32_t bits) {
    return ((bits >> 8) & 1u) != 0;
}

typedef struct {
    uintptr_t address;
    uintptr_t slide;
    bool found;
} segment_contains_ctx_t;

static bool writable_segment_contains(void *opaque, uint32_t index,
                                      const hk_macho_segment_t *segment) {
    (void)index;
    segment_contains_ctx_t *ctx = opaque;
    if (!(segment->initprot & 2u) || segment->vmsize == 0 ||
        segment->vmaddr > UINTPTR_MAX - ctx->slide ||
        segment->vmsize > UINTPTR_MAX - (segment->vmaddr + ctx->slide)) {
        return true;
    }
    uintptr_t start = (uintptr_t)segment->vmaddr + ctx->slide;
    uintptr_t end = start + (uintptr_t)segment->vmsize;
    if (ctx->address >= start && ctx->address < end) {
        ctx->found = true;
        return false;
    }
    return true;
}

static bool belongs_to_importer(const hk_cache_patch_target_t *target,
                                uintptr_t address) {
    segment_contains_ctx_t ctx = {
        .address = address,
        .slide = target->image_slide,
    };
    return hk_macho_iterate_segments(target->image_header,
                                     target->image_header_size,
                                     writable_segment_contains, &ctx) == HK_MACHO_OK &&
           ctx.found;
}

static hk_cache_patch_status_t emit_location(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    uint64_t offset, uint32_t bits, bool v4, bool shared_got,
    hk_cache_patch_visit_fn visit, void *ctx) {
    if (offset > view->size - sizeof(uintptr_t)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    hk_cache_patch_site_t site;
    memset(&site, 0, sizeof(site));
    site.address = (uintptr_t)view->base + (uintptr_t)offset;
    site.schema = v4 ? schema_v4(bits) : schema_v1_v3(bits);
    site.addend = (int64_t)(v4 ? addend_v4(bits) : addend_v1_v3(bits));
    site.weak_import = v4 && weak_v4(bits);
    site.shared_got = shared_got;
    if (!shared_got && !belongs_to_importer(target, site.address)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    (void)visit(ctx, &site);
    return HK_CACHE_PATCH_OK;
}

static hk_cache_patch_status_t parse_v1(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    uint32_t importer_index, const hk_symbol_candidates_t *candidates,
    hk_cache_patch_visit_fn visit, void *ctx, bool *found) {
    (void)importer_index;
    if (view->patch_size < 64u) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    const uint8_t *p = view->patch;
    const uint8_t *images, *exports, *locations, *names;
    uint64_t image_count = read_u64(p + 8);
    uint64_t export_count = read_u64(p + 24);
    uint64_t location_count = read_u64(p + 40);
    uint64_t names_size = read_u64(p + 56);
    if (!patch_array(view, read_u64(p), image_count, CACHE_IMAGE_PATCH_V1_SIZE, &images) ||
        !patch_array(view, read_u64(p + 16), export_count, CACHE_EXPORT_V1_SIZE, &exports) ||
        !patch_array(view, read_u64(p + 32), location_count, CACHE_LOCATION_V1_SIZE, &locations) ||
        !patch_array(view, read_u64(p + 48), names_size, 1u, &names)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    for (uint64_t i = 0; i < image_count; i++) {
        const uint8_t *image = images + i * CACHE_IMAGE_PATCH_V1_SIZE;
        uint32_t first = read_u32(image);
        uint32_t count = read_u32(image + 4);
        if ((uint64_t)first + count > export_count) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        for (uint32_t j = 0; j < count; j++) {
            const uint8_t *export = exports + (uint64_t)(first + j) * CACHE_EXPORT_V1_SIZE;
            const char *name = bounded_string(names, (size_t)names_size,
                                              read_u32(export + 12));
            if (!name) {
                return HK_CACHE_PATCH_MALFORMED;
            }
            if (!candidate_matches(candidates, name)) {
                continue;
            }
            uint32_t loc_first = read_u32(export + 4);
            uint32_t loc_count = read_u32(export + 8);
            if ((uint64_t)loc_first + loc_count > location_count) {
                return HK_CACHE_PATCH_MALFORMED;
            }
            for (uint32_t k = 0; k < loc_count; k++) {
                const uint8_t *loc = locations +
                    (uint64_t)(loc_first + k) * CACHE_LOCATION_V1_SIZE;
                uint64_t offset = read_u32(loc);
                uintptr_t address = (uintptr_t)view->base + (uintptr_t)offset;
                if (!belongs_to_importer(target, address)) {
                    continue;
                }
                hk_cache_patch_status_t status = emit_location(
                    target, view, offset, read_u32(loc + 8), false, false,
                    visit, ctx);
                if (status != HK_CACHE_PATCH_OK) {
                    return status;
                }
                *found = true;
            }
        }
    }
    return HK_CACHE_PATCH_OK;
}

typedef struct {
    const uint8_t *images;
    uint64_t image_count;
    const uint8_t *image_exports;
    uint64_t image_export_count;
    const uint8_t *clients;
    uint64_t client_count;
    const uint8_t *client_exports;
    uint64_t client_export_count;
    const uint8_t *locations;
    uint64_t location_count;
    const uint8_t *names;
    uint64_t names_size;
    const uint8_t *got_clients;
    uint64_t got_client_count;
    const uint8_t *got_exports;
    uint64_t got_export_count;
    const uint8_t *got_locations;
    uint64_t got_location_count;
} table_v2_t;

static bool load_v2(const cache_view_t *view, uint32_t version,
                    table_v2_t *out) {
    const uint8_t *p = view->patch;
    if (view->patch_size < (version >= 3 ? 152u : 104u) || read_u32(p + 4) != 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
#define LOAD_PAIR(field, count_field, off, stride) \
    do { \
        out->count_field = read_u64(p + (off) + 8u); \
        if (!patch_array(view, read_u64(p + (off)), out->count_field, \
                         (stride), &out->field)) return false; \
    } while (0)
    LOAD_PAIR(images, image_count, 8u, CACHE_IMAGE_PATCH_V2_SIZE);
    LOAD_PAIR(image_exports, image_export_count, 24u, CACHE_IMAGE_EXPORT_V2_SIZE);
    LOAD_PAIR(clients, client_count, 40u, CACHE_CLIENT_V2_SIZE);
    LOAD_PAIR(client_exports, client_export_count, 56u, CACHE_CLIENT_EXPORT_V2_SIZE);
    LOAD_PAIR(locations, location_count, 72u, CACHE_LOCATION_V2_SIZE);
    LOAD_PAIR(names, names_size, 88u, 1u);
    if (version >= 3) {
        LOAD_PAIR(got_clients, got_client_count, 104u, CACHE_GOT_CLIENT_V3_SIZE);
        LOAD_PAIR(got_exports, got_export_count, 120u, CACHE_GOT_EXPORT_V3_SIZE);
        LOAD_PAIR(got_locations, got_location_count, 136u,
                  version == 4 ? CACHE_GOT_LOCATION_V4_SIZE
                               : CACHE_GOT_LOCATION_V3_SIZE);
    }
#undef LOAD_PAIR
    return true;
}

static hk_cache_patch_status_t emit_client_export(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    const table_v2_t *table, uint32_t version, uint32_t global_export,
    const uint8_t *client_export, hk_cache_patch_visit_fn visit, void *ctx,
    bool *found) {
    if (read_u32(client_export) != global_export) {
        return HK_CACHE_PATCH_OK;
    }
    uint32_t first = read_u32(client_export + 4);
    uint32_t count = read_u32(client_export + 8);
    if ((uint64_t)first + count > table->location_count) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *loc = table->locations +
            (uint64_t)(first + i) * CACHE_LOCATION_V2_SIZE;
        uint32_t offset = read_u32(loc);
        uint64_t image_offset = (uintptr_t)target->image_header -
                                (uintptr_t)view->base;
        if (image_offset > view->size || offset > view->size - image_offset) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        hk_cache_patch_status_t status = emit_location(
            target, view, image_offset + offset,
            read_u32(loc + 4), version == 4, false, visit, ctx);
        if (status != HK_CACHE_PATCH_OK) {
            return status;
        }
        *found = true;
    }
    return HK_CACHE_PATCH_OK;
}

static hk_cache_patch_status_t emit_got_export(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    const table_v2_t *table, uint32_t version, uint32_t global_export,
    const uint8_t *got_export, hk_cache_patch_visit_fn visit, void *ctx,
    bool *found) {
    if (read_u32(got_export) != global_export) {
        return HK_CACHE_PATCH_OK;
    }
    uint32_t first = read_u32(got_export + 4);
    uint32_t count = read_u32(got_export + 8);
    if ((uint64_t)first + count > table->got_location_count) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    if (count && !target->include_shared_got) {
        return HK_CACHE_PATCH_SCOPE_UNREPRESENTABLE;
    }
    size_t stride = version == 4 ? CACHE_GOT_LOCATION_V4_SIZE
                                 : CACHE_GOT_LOCATION_V3_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *loc = table->got_locations + (uint64_t)(first + i) * stride;
        hk_cache_patch_status_t status = emit_location(
            target, view, read_u64(loc), read_u32(loc + 8), version == 4,
            true, visit, ctx);
        if (status != HK_CACHE_PATCH_OK) {
            return status;
        }
        *found = true;
    }
    return HK_CACHE_PATCH_OK;
}

static hk_cache_patch_status_t parse_v2_v4(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    uint32_t version, uint32_t importer_index,
    const hk_symbol_candidates_t *candidates, hk_cache_patch_visit_fn visit,
    void *ctx, bool *found) {
    table_v2_t table;
    if (!load_v2(view, version, &table) || table.image_count == 0 ||
        importer_index >= table.image_count ||
        (version >= 3 && table.got_client_count != table.image_count)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    for (uint64_t def = 0; def < table.image_count; def++) {
        const uint8_t *image = table.images + def * CACHE_IMAGE_PATCH_V2_SIZE;
        uint32_t client_first = read_u32(image);
        uint32_t client_count = read_u32(image + 4);
        uint32_t export_first = read_u32(image + 8);
        uint32_t export_count = read_u32(image + 12);
        if ((uint64_t)client_first + client_count > table.client_count ||
            (uint64_t)export_first + export_count > table.image_export_count) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        for (uint32_t e = 0; e < export_count; e++) {
            uint32_t global_export = export_first + e;
            const uint8_t *export = table.image_exports +
                (uint64_t)global_export * CACHE_IMAGE_EXPORT_V2_SIZE;
            uint32_t name_and_kind = read_u32(export + 4);
            const char *name = bounded_string(table.names, (size_t)table.names_size,
                                              name_and_kind & 0x0FFFFFFFu);
            if (!name) {
                return HK_CACHE_PATCH_MALFORMED;
            }
            if (!candidate_matches(candidates, name)) {
                continue;
            }
            if ((name_and_kind >> 28) != 0) {
                return HK_CACHE_PATCH_UNSUPPORTED;
            }
            for (uint32_t c = 0; c < client_count; c++) {
                const uint8_t *client = table.clients +
                    (uint64_t)(client_first + c) * CACHE_CLIENT_V2_SIZE;
                uint32_t ce_first = read_u32(client + 4);
                uint32_t ce_count = read_u32(client + 8);
                if ((uint64_t)ce_first + ce_count > table.client_export_count) {
                    return HK_CACHE_PATCH_MALFORMED;
                }
                if (read_u32(client) != importer_index) {
                    continue;
                }
                for (uint32_t ce = 0; ce < ce_count; ce++) {
                    hk_cache_patch_status_t status = emit_client_export(
                        target, view, &table, version, global_export,
                        table.client_exports +
                            (uint64_t)(ce_first + ce) * CACHE_CLIENT_EXPORT_V2_SIZE,
                        visit, ctx, found);
                    if (status != HK_CACHE_PATCH_OK) {
                        return status;
                    }
                }
            }
            if (version >= 3) {
                const uint8_t *got_client = table.got_clients +
                    def * CACHE_GOT_CLIENT_V3_SIZE;
                uint32_t ge_first = read_u32(got_client);
                uint32_t ge_count = read_u32(got_client + 4);
                if ((uint64_t)ge_first + ge_count > table.got_export_count) {
                    return HK_CACHE_PATCH_MALFORMED;
                }
                for (uint32_t ge = 0; ge < ge_count; ge++) {
                    hk_cache_patch_status_t status = emit_got_export(
                        target, view, &table, version, global_export,
                        table.got_exports +
                            (uint64_t)(ge_first + ge) * CACHE_GOT_EXPORT_V3_SIZE,
                        visit, ctx, found);
                    if (status != HK_CACHE_PATCH_OK) {
                        return status;
                    }
                }
            }
        }
    }
    return HK_CACHE_PATCH_OK;
}

hk_cache_patch_status_t hk_dyld_cache_iterate_symbol_uses(
    const hk_cache_patch_target_t *target, const char *symbol_name,
    hk_symbol_name_convention_t convention, hk_cache_patch_visit_fn visit,
    void *ctx) {
    if (!target || !target->cache_base || !target->image_header ||
        !target->image_path ||
        !symbol_name || !visit || target->cache_size < 168u) {
        return HK_CACHE_PATCH_INVALID_ARGUMENT;
    }
    const uint8_t *base = target->cache_base;
    uintptr_t cache_start = (uintptr_t)base;
    if ((uintptr_t)target->image_header < cache_start ||
        (uintptr_t)target->image_header - cache_start >= target->cache_size) {
        return HK_CACHE_PATCH_NOT_CACHE;
    }
    uint32_t mapping_offset = read_u32(base + DCH_MAPPING_OFFSET);
    uint32_t mapping_count = read_u32(base + DCH_MAPPING_COUNT);
    const uint8_t *mappings;
    if (mapping_count == 0 ||
        !range_at(base, target->cache_size, mapping_offset, mapping_count,
                  DCH_MAPPING_INFO_SIZE, &mappings) || read_u64(mappings + 16) != 0) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    uint64_t unslid_base = read_u64(mappings);
    if (cache_start < unslid_base) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    uintptr_t slide = cache_start - (uintptr_t)unslid_base;
    if (target->image_slide != slide) {
        return HK_CACHE_PATCH_MALFORMED;
    }

    uint32_t images_offset = read_u32(base + DCH_IMAGES_OFFSET_OLD);
    uint32_t images_count = read_u32(base + DCH_IMAGES_COUNT_OLD);
    if (mapping_offset >= DCH_IMAGES_COUNT + sizeof(uint32_t)) {
        if (target->cache_size < DCH_IMAGES_COUNT + sizeof(uint32_t)) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        images_offset = read_u32(base + DCH_IMAGES_OFFSET);
        images_count = read_u32(base + DCH_IMAGES_COUNT);
    }
    const uint8_t *images;
    if (!range_at(base, target->cache_size, images_offset, images_count,
                  DCH_IMAGE_INFO_SIZE, &images)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    uint32_t importer_index = UINT32_MAX;
    for (uint32_t i = 0; i < images_count; i++) {
        const uint8_t *image = images + (size_t)i * DCH_IMAGE_INFO_SIZE;
        uint64_t address = read_u64(image);
        if (address > UINTPTR_MAX - slide ||
            (uintptr_t)address + slide != (uintptr_t)target->image_header) {
            continue;
        }
        const char *path = bounded_string(base, target->cache_size,
                                          read_u32(image + 24));
        if (!path || strcmp(path, target->image_path) != 0) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        importer_index = i;
        break;
    }
    if (importer_index == UINT32_MAX) {
        return HK_CACHE_PATCH_NOT_CACHE;
    }

    if (target->uuid_present) {
        if (mapping_offset < DCH_IMAGES_TEXT_COUNT + 8u) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        uint64_t text_offset = read_u64(base + DCH_IMAGES_TEXT_OFFSET);
        uint64_t text_count = read_u64(base + DCH_IMAGES_TEXT_COUNT);
        const uint8_t *texts;
        if (!range_at(base, target->cache_size, text_offset, text_count,
                      DCH_TEXT_INFO_SIZE, &texts)) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        bool uuid_match = false;
        uint64_t image_unslid = read_u64(images +
            (size_t)importer_index * DCH_IMAGE_INFO_SIZE);
        for (uint64_t i = 0; i < text_count; i++) {
            const uint8_t *text = texts + i * DCH_TEXT_INFO_SIZE;
            if (read_u64(text + 16) == image_unslid) {
                uuid_match = memcmp(text, target->uuid, 16) == 0;
                break;
            }
        }
        if (!uuid_match) {
            return HK_CACHE_PATCH_MALFORMED;
        }
    }

    uint64_t patch_addr = read_u64(base + DCH_PATCH_INFO_ADDR);
    uint64_t patch_size = read_u64(base + DCH_PATCH_INFO_SIZE);
    if (patch_addr == 0 || patch_size == 0) {
        if (mapping_offset >= DCH_FORMAT_FLAGS + sizeof(uint32_t) &&
            target->cache_size < DCH_FORMAT_FLAGS + sizeof(uint32_t)) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        bool built_from_chains =
            mapping_offset >= DCH_FORMAT_FLAGS + sizeof(uint32_t) &&
            (read_u32(base + DCH_FORMAT_FLAGS) & (1u << 11)) != 0;
        return built_from_chains ? HK_CACHE_PATCH_MALFORMED
                                 : HK_CACHE_PATCH_NO_METADATA;
    }
    if (patch_addr < unslid_base || patch_addr - unslid_base > target->cache_size ||
        patch_size > target->cache_size - (patch_addr - unslid_base)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    cache_view_t view = {
        .base = base,
        .size = target->cache_size,
        .unslid_base = unslid_base,
        .slide = slide,
        .patch = base + (size_t)(patch_addr - unslid_base),
        .patch_size = (size_t)patch_size,
    };
    hk_symbol_candidates_t candidates;
    if (hk_symbol_build_candidates(symbol_name, convention, &candidates) !=
        HK_RESOLVE_OK) {
        return HK_CACHE_PATCH_INVALID_ARGUMENT;
    }
    uint32_t version = mapping_offset <= DCH_SWIFT_OPTS_SIZE
        ? 1u : read_u32(view.patch);
    if (view.patch_size < 24u) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    uint64_t patch_image_count = version == 1
        ? read_u64(view.patch + 8) : read_u64(view.patch + 16);
    if (patch_image_count != images_count) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    bool found = false;
    hk_cache_patch_status_t status;
    if (version == 1) {
        status = parse_v1(target, &view, importer_index, &candidates,
                          visit, ctx, &found);
    } else if (version >= 2 && version <= 4) {
        status = parse_v2_v4(target, &view, version, importer_index,
                             &candidates, visit, ctx, &found);
    } else {
        return HK_CACHE_PATCH_UNSUPPORTED;
    }
    if (status != HK_CACHE_PATCH_OK) {
        return status;
    }
    return found ? HK_CACHE_PATCH_OK : HK_CACHE_PATCH_NOT_FOUND;
}
