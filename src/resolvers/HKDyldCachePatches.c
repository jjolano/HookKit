#include "HKDyldCachePatches.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

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
    const uint8_t *patch;
    size_t patch_size;
} cache_view_t;

// The full identity of one cache view. Two views that differ here may hold
// unrelated bytes, so nothing derived from one may be reused for the other.
static bool cache_view_equal(const cache_view_t *a, const cache_view_t *b) {
    return a->base == b->base && a->size == b->size &&
           a->unslid_base == b->unslid_base && a->patch == b->patch &&
           a->patch_size == b->patch_size;
}

typedef struct {
    uint64_t defining_image;
    uint32_t export_index;
} cache_export_match_t;

struct hk_cache_patch_lookup {
    cache_view_t view;
    uint32_t version;
    hk_symbol_name_convention_t convention;
    cache_export_match_t *matches;
    size_t count, capacity;
    bool complete, allocation_failed;
#ifdef HK_CACHE_PATCH_TEST
    size_t scans;
#endif
    char symbol[];
};

#ifdef HK_CACHE_PATCH_TEST
static bool lookup_fail_allocations;
void hk_cache_patch_lookup_fail_allocations_for_testing(bool fail) {
    lookup_fail_allocations = fail;
}
size_t hk_cache_patch_lookup_scan_count(const hk_cache_patch_lookup_t *lookup) {
    return lookup ? lookup->scans : 0;
}
#endif

hk_cache_patch_lookup_t *hk_cache_patch_lookup_create(
    const char *symbol_name, hk_symbol_name_convention_t convention) {
    if (!symbol_name) return NULL;
    size_t length = strlen(symbol_name);
    if (length >= SIZE_MAX - sizeof(hk_cache_patch_lookup_t)) return NULL;
#ifdef HK_CACHE_PATCH_TEST
    if (lookup_fail_allocations) return NULL;
#endif
    hk_cache_patch_lookup_t *lookup = calloc(1, sizeof(*lookup) + length + 1);
    if (lookup) {
        memcpy(lookup->symbol, symbol_name, length + 1);
        lookup->convention = convention;
    }
    return lookup;
}

void hk_cache_patch_lookup_destroy(hk_cache_patch_lookup_t *lookup) {
    if (!lookup) return;
    free(lookup->matches);
    free(lookup);
}

static void lookup_begin(hk_cache_patch_lookup_t *lookup,
                         const cache_view_t *view, uint32_t version) {
    if (!cache_view_equal(&lookup->view, view) || lookup->version != version) {
        lookup->complete = false;
    }
    if (!lookup->complete) {
        lookup->count = 0;
        lookup->view = *view;
        lookup->version = version;
#ifdef HK_CACHE_PATCH_TEST
        lookup->scans++;
#endif
    }
}

static void lookup_append(hk_cache_patch_lookup_t *lookup, uint64_t image,
                          uint32_t export_index) {
    if (!lookup || lookup->allocation_failed) return;
    if (lookup->count == lookup->capacity) {
        size_t capacity = lookup->capacity ? lookup->capacity * 2 : 4;
        cache_export_match_t *matches = NULL;
        if (capacity > lookup->capacity && capacity <= SIZE_MAX / sizeof(*matches)) {
#ifdef HK_CACHE_PATCH_TEST
            if (!lookup_fail_allocations)
#endif
                matches = realloc(lookup->matches, capacity * sizeof(*matches));
        }
        if (!matches) {
            lookup->allocation_failed = true;
            return;
        }
        lookup->matches = matches;
        lookup->capacity = capacity;
    }
    lookup->matches[lookup->count++] = (cache_export_match_t){image, export_index};
}

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
        segment->vmsize > UINTPTR_MAX) {
        return true;
    }
    // A dyld slide is signed conceptually, but HookKit's existing ABI stores
    // it modulo uintptr_t. Unsigned addition performs the required translation
    // for both positive and negative slides.
    uintptr_t start = (uintptr_t)segment->vmaddr + ctx->slide;
    if ((uintptr_t)segment->vmsize > UINTPTR_MAX - start) {
        return true;
    }
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

// ---- process-scoped immutable export index ------------------------------
//
// A v2-v4 patch table lists every image's exports in one flat table whose
// names live in a bounded string pool. Immutable live cache metadata therefore
// supports an index that outlives one preparation AND one symbol: an opted-in
// target's export names are recorded once, sorted for binary name lookup, and
// later queries for any symbol replay their matches from the index instead of
// rescanning every export. Entries are borrowed (names point into the cache's
// pool) and the index holds no slot values, PAC data, or prepared plans.

// Entries sort by (name, defining_image, export_index). That pair is exactly
// the order the direct parser visits -- definitions ascending, each
// definition's exports ascending -- so equal names replay in traversal order.
typedef struct {
    const char *name;       // borrowed from the cache's immutable name pool
    uint32_t defining_image;
    uint32_t export_index;  // global index into the image-export table
} cache_index_entry_t;

typedef struct {
    size_t count;
    cache_index_entry_t *entries;
} cache_export_index_t;

// One slot per distinct cache identity, filled once. Published entries are
// never freed for the process lifetime: a query drops the lock and keeps
// reading them, so replacing a live index in place would be a use-after-free.
// An identity with no slot left simply keeps the direct per-symbol scan.
// ponytail: 4 pinned slots, no eviction. Sized for the handful of cache views
// one process maps; anything else falls back instead of churning memory.
#define HK_CACHE_INDEX_SLOTS 4u
static pthread_mutex_t g_cache_index_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    cache_view_t view;
    uint32_t version;
    cache_export_index_t index;
} g_cache_index_slots[HK_CACHE_INDEX_SLOTS];
static size_t g_cache_index_used;

#ifdef HK_CACHE_PATCH_TEST
// Number of built (therefore pinned) indices. There is one build per slot, so
// this doubles as the build count the tests assert on.
size_t hk_cache_patch_index_build_count(void) {
    return g_cache_index_used;
}

void hk_cache_patch_index_reset_for_testing(void) {
    pthread_mutex_lock(&g_cache_index_lock);
    for (size_t i = 0; i < g_cache_index_used; i++) {
        free(g_cache_index_slots[i].index.entries);
        g_cache_index_slots[i].index.entries = NULL;
        g_cache_index_slots[i].index.count = 0;
    }
    g_cache_index_used = 0;
    pthread_mutex_unlock(&g_cache_index_lock);
}
#endif

// Direct-parser traversal order for two entries of the same name.
static bool cache_index_entry_less(const cache_index_entry_t *a,
                                   const cache_index_entry_t *b) {
    if (a->defining_image != b->defining_image) {
        return a->defining_image < b->defining_image;
    }
    return a->export_index < b->export_index;
}

static int cache_index_entry_compare(const void *left, const void *right) {
    const cache_index_entry_t *a = left;
    const cache_index_entry_t *b = right;
    int order = strcmp(a->name, b->name);
    if (order != 0) {
        return order;
    }
    if (a->defining_image != b->defining_image) {
        return a->defining_image < b->defining_image ? -1 : 1;
    }
    if (a->export_index == b->export_index) {
        return 0;
    }
    return a->export_index < b->export_index ? -1 : 1;
}

// Records every export of one immutable v2-v4 patch table into `out`, or
// returns false leaving `out` untouched. Everything the direct parser
// validates while scanning a table is validated here too, and a failure
// abandons the index before any caller can use it, so the direct parser still
// reports the same status at the same point.
//
// A definition row may overlap another's export range, so the entry count is
// the sum over rows, not the size of the flat export table: the traversal is
// per row.
static bool cache_index_build(const cache_view_t *view, uint32_t version,
                              cache_export_index_t *out) {
    table_v2_t table;
    if (!load_v2(view, version, &table) || table.image_count == 0 ||
        table.image_count > UINT32_MAX ||
        (version >= 3 && table.got_client_count != table.image_count) ||
        table.image_export_count > UINT32_MAX) {
        return false;
    }
    size_t capacity = 0;
    for (uint64_t def = 0; def < table.image_count; def++) {
        const uint8_t *image = table.images + def * CACHE_IMAGE_PATCH_V2_SIZE;
        uint32_t client_first = read_u32(image);
        uint32_t client_count = read_u32(image + 4);
        uint32_t export_first = read_u32(image + 8);
        uint32_t export_count = read_u32(image + 12);
        if ((uint64_t)client_first + client_count > table.client_count ||
            (uint64_t)export_first + export_count > table.image_export_count ||
            capacity > SIZE_MAX - export_count) {
            return false;
        }
        capacity += export_count;
    }
    cache_index_entry_t *entries = NULL;
    if (capacity) {
        if (capacity > SIZE_MAX / sizeof(*entries)) {
            return false;
        }
#ifdef HK_CACHE_PATCH_TEST
        if (!lookup_fail_allocations)
#endif
            entries = malloc(capacity * sizeof(*entries));
        if (!entries) {
            return false;
        }
    }
    size_t used = 0;
    for (uint64_t def = 0; def < table.image_count; def++) {
        const uint8_t *image = table.images + def * CACHE_IMAGE_PATCH_V2_SIZE;
        uint32_t export_first = read_u32(image + 8);
        uint32_t export_count = read_u32(image + 12);
        for (uint32_t e = 0; e < export_count; e++) {
            uint32_t global_export = export_first + e;
            const uint8_t *export = table.image_exports +
                (uint64_t)global_export * CACHE_IMAGE_EXPORT_V2_SIZE;
            const char *name = bounded_string(
                table.names, (size_t)table.names_size,
                read_u32(export + 4) & 0x0FFFFFFFu);
            if (!name) {
                free(entries);
                return false;
            }
            entries[used] = (cache_index_entry_t){
                .name = name,
                .defining_image = (uint32_t)def,
                .export_index = global_export,
            };
            used++;
        }
    }
    if (used) {
        qsort(entries, used, sizeof(*entries), cache_index_entry_compare);
    }
    out->count = used;
    out->entries = entries;
    return true;
}

// This view's index, built on first use. NULL -- malformed table, allocation
// failure, or a different view -- sends the caller to the direct scan.
static const cache_export_index_t *cache_index_acquire(const cache_view_t *view,
                                                       uint32_t version) {
    pthread_mutex_lock(&g_cache_index_lock);
    const cache_export_index_t *found = NULL;
    for (size_t i = 0; i < g_cache_index_used; i++) {
        if (g_cache_index_slots[i].version == version &&
            cache_view_equal(&g_cache_index_slots[i].view, view)) {
            found = &g_cache_index_slots[i].index;
            break;
        }
    }
    if (!found && g_cache_index_used < HK_CACHE_INDEX_SLOTS) {
        // Publish only a fully built index: a failure here leaves the slot
        // table exactly as it was.
        cache_export_index_t built;
        if (cache_index_build(view, version, &built)) {
            g_cache_index_slots[g_cache_index_used].view = *view;
            g_cache_index_slots[g_cache_index_used].version = version;
            g_cache_index_slots[g_cache_index_used].index = built;
            g_cache_index_used++;
            found = &g_cache_index_slots[g_cache_index_used - 1].index;
        }
    }
    pthread_mutex_unlock(&g_cache_index_lock);
    return found;
}

// First entry whose name is not less than `name`. Entries sharing a name are
// ordered by defining image then export index, so one name's run replays
// exactly the order the direct parser would emit.
static size_t cache_index_lower_bound(const cache_export_index_t *index,
                                      const char *name) {
    size_t low = 0, high = index->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(index->entries[mid].name, name) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
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

// Emits the uses of one already-matched export: every client of that defining
// image whose image index is the importer, then -- v3 and later -- the defining
// image's shared-GOT uses. The direct parser and the index replay both emit
// through here, so a replayed site list cannot differ from a scanned one. The
// direct loop validates this image row before calling; the index replay relies
// on the bounds checks below.
static hk_cache_patch_status_t emit_export_uses(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    const table_v2_t *table, uint32_t version, uint32_t importer_index,
    uint64_t def, uint32_t global_export, hk_cache_patch_visit_fn visit,
    void *ctx, bool *found) {
    const uint8_t *image = table->images + def * CACHE_IMAGE_PATCH_V2_SIZE;
    uint32_t client_first = read_u32(image);
    uint32_t client_count = read_u32(image + 4);
    if ((uint64_t)client_first + client_count > table->client_count) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    for (uint32_t c = 0; c < client_count; c++) {
        const uint8_t *client = table->clients +
            (uint64_t)(client_first + c) * CACHE_CLIENT_V2_SIZE;
        uint32_t ce_first = read_u32(client + 4);
        uint32_t ce_count = read_u32(client + 8);
        if ((uint64_t)ce_first + ce_count > table->client_export_count) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        if (read_u32(client) != importer_index) {
            continue;
        }
        for (uint32_t ce = 0; ce < ce_count; ce++) {
            hk_cache_patch_status_t status = emit_client_export(
                target, view, table, version, global_export,
                table->client_exports +
                    (uint64_t)(ce_first + ce) * CACHE_CLIENT_EXPORT_V2_SIZE,
                visit, ctx, found);
            if (status != HK_CACHE_PATCH_OK) {
                return status;
            }
        }
    }
    if (version < 3) {
        return HK_CACHE_PATCH_OK;
    }
    const uint8_t *got_client = table->got_clients +
        def * CACHE_GOT_CLIENT_V3_SIZE;
    uint32_t ge_first = read_u32(got_client);
    uint32_t ge_count = read_u32(got_client + 4);
    if ((uint64_t)ge_first + ge_count > table->got_export_count) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    for (uint32_t ge = 0; ge < ge_count; ge++) {
        hk_cache_patch_status_t status = emit_got_export(
            target, view, table, version, global_export,
            table->got_exports +
                (uint64_t)(ge_first + ge) * CACHE_GOT_EXPORT_V3_SIZE,
            visit, ctx, found);
        if (status != HK_CACHE_PATCH_OK) {
            return status;
        }
    }
    return HK_CACHE_PATCH_OK;
}

static hk_cache_patch_status_t parse_v2_v4(
    const hk_cache_patch_target_t *target, const cache_view_t *view,
    uint32_t version, uint32_t importer_index,
    const hk_symbol_candidates_t *candidates, hk_cache_patch_visit_fn visit,
    void *ctx, bool *found, hk_cache_patch_lookup_t *lookup,
    const cache_export_index_t *index) {
    table_v2_t table;
    if (!load_v2(view, version, &table) || table.image_count == 0 ||
        importer_index >= table.image_count ||
        (version >= 3 && table.got_client_count != table.image_count)) {
        return HK_CACHE_PATCH_MALFORMED;
    }
    if (lookup && lookup->allocation_failed) lookup = NULL;
    if (lookup) lookup_begin(lookup, view, version);
    bool reuse = lookup && lookup->complete;
    size_t match = 0;
    if (!reuse && index) {
        // First pass for this symbol over immutable metadata: take the
        // candidate names' runs straight from the index. Each run is in
        // traversal order, so merging the runs by that same order replays the
        // exact def-major, export-ascending sequence the direct parser emits,
        // including duplicate exports of one name and overlapping ranges.
        size_t next[2] = {0, 0}, end[2] = {0, 0};
        for (unsigned c = 0; c < candidates->count; c++) {
            const char *name = candidates->names[c];
            next[c] = cache_index_lower_bound(index, name);
            end[c] = next[c];
            while (end[c] < index->count &&
                   strcmp(index->entries[end[c]].name, name) == 0) {
                end[c]++;
            }
        }
        for (;;) {
            size_t pick = SIZE_MAX;
            for (unsigned c = 0; c < candidates->count; c++) {
                if (next[c] < end[c] &&
                    (pick == SIZE_MAX ||
                     cache_index_entry_less(&index->entries[next[c]],
                                            &index->entries[next[pick]]))) {
                    pick = c;
                }
            }
            if (pick == SIZE_MAX) {
                break;
            }
            const cache_index_entry_t *entry = &index->entries[next[pick]++];
            const uint8_t *export = table.image_exports +
                (uint64_t)entry->export_index * CACHE_IMAGE_EXPORT_V2_SIZE;
            if ((read_u32(export + 4) >> 28) != 0) {
                return HK_CACHE_PATCH_UNSUPPORTED;
            }
            lookup_append(lookup, entry->defining_image, entry->export_index);
            hk_cache_patch_status_t status = emit_export_uses(
                target, view, &table, version, importer_index,
                entry->defining_image, entry->export_index, visit, ctx, found);
            if (status != HK_CACHE_PATCH_OK) {
                return status;
            }
        }
        if (lookup && !lookup->allocation_failed) lookup->complete = true;
        return HK_CACHE_PATCH_OK;
    }
    for (uint64_t def = 0; def < table.image_count; def++) {
        // A completed scan validated every row of this immutable metadata.
        // Revisit only definitions with matches, including none for a miss.
        if (reuse) {
            if (match == lookup->count) break;
            def = lookup->matches[match].defining_image;
        }
        const uint8_t *image = table.images + def * CACHE_IMAGE_PATCH_V2_SIZE;
        uint32_t export_first = read_u32(image + 8);
        uint32_t export_count = read_u32(image + 12);
        if ((uint64_t)read_u32(image) + read_u32(image + 4) > table.client_count ||
            (uint64_t)export_first + export_count > table.image_export_count) {
            return HK_CACHE_PATCH_MALFORMED;
        }
        for (uint32_t e = 0; e < export_count; e++) {
            if (reuse) {
                if (match == lookup->count || lookup->matches[match].defining_image != def)
                    break;
                e = lookup->matches[match++].export_index - export_first;
            }
            uint32_t global_export = export_first + e;
            const uint8_t *export = table.image_exports +
                (uint64_t)global_export * CACHE_IMAGE_EXPORT_V2_SIZE;
            uint32_t name_and_kind = read_u32(export + 4);
            if (!reuse) {
                const char *name = bounded_string(table.names, (size_t)table.names_size,
                                                  name_and_kind & 0x0FFFFFFFu);
                if (!name) {
                    return HK_CACHE_PATCH_MALFORMED;
                }
                if (!candidate_matches(candidates, name)) {
                    continue;
                }
                lookup_append(lookup, def, global_export);
            }
            if ((name_and_kind >> 28) != 0) {
                return HK_CACHE_PATCH_UNSUPPORTED;
            }
            hk_cache_patch_status_t status = emit_export_uses(
                target, view, &table, version, importer_index, def,
                global_export, visit, ctx, found);
            if (status != HK_CACHE_PATCH_OK) {
                return status;
            }
        }
    }
    if (lookup && !lookup->allocation_failed) lookup->complete = true;
    return HK_CACHE_PATCH_OK;
}

hk_cache_patch_status_t hk_dyld_cache_iterate_symbol_uses_with_lookup(
    const hk_cache_patch_target_t *target, const char *symbol_name,
    hk_symbol_name_convention_t convention, hk_cache_patch_visit_fn visit,
    void *ctx, hk_cache_patch_lookup_t *lookup) {
    if (lookup && (lookup->convention != convention || !symbol_name ||
                   strcmp(lookup->symbol, symbol_name) != 0)) lookup = NULL;
    if (!target || !target->cache_base || !target->image_header ||
        !target->image_path ||
        !symbol_name || !visit || target->cache_size < 168u) {
        return HK_CACHE_PATCH_INVALID_ARGUMENT;
    }
    const uint8_t *base = target->cache_base;
    uintptr_t cache_start = (uintptr_t)base;
    if (target->cache_size > UINTPTR_MAX - cache_start ||
        (uintptr_t)target->image_header < cache_start ||
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
        if (address < unslid_base ||
            address - unslid_base >= target->cache_size ||
            cache_start + (uintptr_t)(address - unslid_base) !=
                (uintptr_t)target->image_header) {
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
        // One index for every process-lifetime preparation off the same live
        // cache. A lookup that already completed for this exact view and
        // version replays its own matches, so it needs no index and the lock
        // stays out of that path. Every other case leaves `index` NULL when no
        // index can be had, which is the direct per-symbol scan.
        bool lookup_complete = lookup && lookup->complete &&
            lookup->version == version && cache_view_equal(&lookup->view, &view);
        const cache_export_index_t *index =
            target->immutable_metadata && !lookup_complete
                ? cache_index_acquire(&view, version) : NULL;
        status = parse_v2_v4(target, &view, version, importer_index,
                             &candidates, visit, ctx, &found, lookup, index);
    } else {
        return HK_CACHE_PATCH_UNSUPPORTED;
    }
    if (status != HK_CACHE_PATCH_OK) {
        if (lookup) lookup->complete = false;
        return status;
    }
    return found ? HK_CACHE_PATCH_OK : HK_CACHE_PATCH_NOT_FOUND;
}

hk_cache_patch_status_t hk_dyld_cache_iterate_symbol_uses(
    const hk_cache_patch_target_t *target, const char *symbol_name,
    hk_symbol_name_convention_t convention, hk_cache_patch_visit_fn visit,
    void *ctx) {
    return hk_dyld_cache_iterate_symbol_uses_with_lookup(
        target, symbol_name, convention, visit, ctx, NULL);
}
