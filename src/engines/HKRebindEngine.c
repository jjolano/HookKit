// Rebind engine. See HKRebindEngine.h for why prepare and commit are separate
// phases and why the write is the only part behind a device seam.

#include "HKRebindEngine.h"

#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#include "../resolvers/HKChainedFixups.h"
#include "../resolvers/HKDyldCachePatches.h"
#include "../resolvers/HKMachO.h"
#include "../resolvers/HKSymbolResolve.h"

uint64_t hk_rebind_read_slot(uintptr_t address) {
    uint64_t v;
    memcpy(&v, (const void *)address, sizeof(v));
    return v;
}

// ---- phase 1: prepare (mutates nothing) ---------------------------------

typedef enum {
    ADD_SITE_OK,
    ADD_SITE_OVERFLOW,
    ADD_SITE_MALFORMED,
    ADD_SITE_PAC_MISMATCH,
} add_site_status_t;

typedef struct {
    const hk_symbol_candidates_t *candidates;
    uintptr_t slide;
    hk_rebind_plan_t *plan;
    add_site_status_t error;
} collect_ctx_t;

static bool checked_adjust(uintptr_t value, int64_t addend, bool add,
                           uintptr_t *out) {
    uint64_t magnitude = addend < 0
        ? (uint64_t)(-(addend + 1)) + 1
        : (uint64_t)addend;
    bool increase = (addend >= 0) == add;
    if (increase) {
        if (magnitude > (uint64_t)UINTPTR_MAX - value) return false;
        *out = value + (uintptr_t)magnitude;
    } else {
        if (magnitude > value) return false;
        *out = value - (uintptr_t)magnitude;
    }
    return true;
}

static add_site_status_t add_site(hk_rebind_plan_t *plan, uintptr_t address,
                                  const hk_pac_schema_t *schema, int64_t addend,
                                  bool weak_import, bool from_chained,
                                  bool from_cache) {
    for (uint32_t i = 0; i < plan->count; i++) {
        hk_rebind_site_t *existing = &plan->sites[i];
        if (existing->address != address) {
            continue;
        }
        return existing->schema.authenticated == schema->authenticated &&
                       existing->schema.key == schema->key &&
                       existing->schema.diversity == schema->diversity &&
                       existing->schema.address_diversity == schema->address_diversity &&
                       existing->addend == addend
            ? ADD_SITE_OK : ADD_SITE_MALFORMED;
    }
    if (plan->count >= HK_REBIND_MAX_SITES) {
        return ADD_SITE_OVERFLOW;
    }
    uint64_t old = hk_rebind_read_slot(address);
    if (old == 0) {
        if (!weak_import || addend != 0) {
            return ADD_SITE_MALFORMED;
        }
    } else if (!hk_pac_slot_matches((uintptr_t)old, schema, address)) {
        return ADD_SITE_PAC_MISMATCH;
    }
    uintptr_t base = 0;
    if (!checked_adjust(old == 0 ? 0 : hk_pac_strip_slot((uintptr_t)old, schema), addend,
                        false, &base)) {
        return ADD_SITE_MALFORMED;
    }
    hk_rebind_site_t *s = &plan->sites[plan->count++];
    memset(s, 0, sizeof(*s));
    s->address = address;
    s->original = old;
    s->callable_original = hk_pac_make_callable(base);
    s->addend = addend;
    s->schema = *schema;
    s->weak_import = weak_import;
    s->from_chained = from_chained;
    s->from_cache = from_cache;
    return ADD_SITE_OK;
}

// LC_DYSYMTAB path: slot_vmaddr is an UNSLID VM address, so the slide applies.
static bool dysymtab_slot_cb(void *ctx, const hk_import_slot_t *slot) {
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    for (unsigned i = 0; i < c->candidates->count; i++) {
        if (strcmp(slot->symbol_name, c->candidates->names[i]) == 0) {
            hk_pac_schema_t schema;
            memset(&schema, 0, sizeof(schema));
            if (strcmp(slot->sectname, "__auth_got") == 0) {
                schema.authenticated = true;
                schema.key = HK_PAC_KEY_IA;
                schema.address_diversity = true;
            }
            add_site_status_t status = add_site(
                c->plan, (uintptr_t)slot->slot_vmaddr + c->slide,
                &schema, 0, false, false, false);
            if (status != ADD_SITE_OK) {
                c->error = status;
                return false;
            }
            break;
        }
    }
    return true;
}

typedef struct {
    const hk_chained_fixups_t *fixups;
    const hk_symbol_candidates_t *candidates;
    uintptr_t image_base;
    size_t image_size;
    hk_rebind_plan_t *plan;
    bool overflow;
    bool malformed;
    bool pac_mismatch;
} chained_ctx_t;

// Chained-fixups path: slot_image_offset is an offset FROM THE IMAGE BASE, a
// different coordinate system from the LC_DYSYMTAB path's unslid vmaddr.
// Conflating them would put every write at the wrong address.
static bool chained_bind_cb(void *ctx, const hk_chained_bind_t *bind) {
    chained_ctx_t *c = (chained_ctx_t *)ctx;
    hk_chained_import_t import;
    if (hk_chained_import_at(c->fixups, bind->import_ordinal, &import) != HK_CHAINED_OK) {
        c->malformed = true;
        return false;
    }
    for (unsigned i = 0; i < c->candidates->count; i++) {
        if (strcmp(import.symbol_name, c->candidates->names[i]) == 0) {
            if (bind->slot_image_offset > c->image_size ||
                sizeof(uint64_t) > c->image_size - bind->slot_image_offset) {
                c->malformed = true;
                return false;
            }
            if ((bind->addend > 0 && import.addend > INT64_MAX - bind->addend) ||
                (bind->addend < 0 && import.addend < INT64_MIN - bind->addend)) {
                c->malformed = true;
                return false;
            }
            int64_t combined = import.addend + bind->addend;
            hk_pac_schema_t schema = {
                .authenticated = bind->is_auth,
                .key = (hk_pac_key_t)bind->key,
                .diversity = bind->diversity,
                .address_diversity = bind->address_diversity,
            };
            add_site_status_t status = add_site(
                c->plan, c->image_base + (uintptr_t)bind->slot_image_offset,
                &schema, combined, import.weak_import, true, false);
            if (status == ADD_SITE_OVERFLOW) c->overflow = true;
            if (status == ADD_SITE_MALFORMED) c->malformed = true;
            if (status == ADD_SITE_PAC_MISMATCH) c->pac_mismatch = true;
            if (status != ADD_SITE_OK) return false;
            break;
        }
    }
    return true;
}

typedef struct {
    hk_rebind_plan_t *plan;
    bool overflow;
    bool malformed;
    bool pac_mismatch;
} cache_collect_ctx_t;

static bool cache_patch_cb(void *opaque, const hk_cache_patch_site_t *site) {
    cache_collect_ctx_t *ctx = opaque;
    add_site_status_t status = add_site(
        ctx->plan, site->address, &site->schema, site->addend,
        site->weak_import, false, true);
    if (status == ADD_SITE_OVERFLOW) ctx->overflow = true;
    if (status == ADD_SITE_MALFORMED) ctx->malformed = true;
    if (status == ADD_SITE_PAC_MISMATCH) ctx->pac_mismatch = true;
    return status == ADD_SITE_OK;
}

typedef struct {
    const uint8_t *base;
    size_t size;
    bool mapped;
} file_view_t;

static bool open_file_view(const hk_rebind_target_t *target, file_view_t *out) {
    memset(out, 0, sizeof(*out));
    if (target->file_image && target->file_image_size) {
        out->base = target->file_image;
        out->size = target->file_image_size;
        return true;
    }
    if (!target->image_path) {
        return false;
    }
    int fd = open(target->image_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > SIZE_MAX) {
        close(fd);
        return false;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return false;
    }
    out->base = map;
    out->size = (size_t)st.st_size;
    out->mapped = true;
    return true;
}

static void close_file_view(file_view_t *view) {
    if (view->mapped) {
        munmap((void *)view->base, view->size);
    }
    view->base = NULL;
    view->size = 0;
    view->mapped = false;
}

#define HK_REBIND_MAX_SEGMENTS 64u
typedef struct {
    hk_macho_segment_t segments[HK_REBIND_MAX_SEGMENTS];
    uint32_t count;
    bool overflow;
} segment_collect_t;

// ponytail: 4-entry file cache for chained-fixup images. Per-image map if throughput matters.
#define HK_REBIND_FILE_CACHE_SIZE 4
static pthread_mutex_t g_rebind_file_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool valid;
    char *path;
    const void *image_base;
    uint32_t cputype;
    uint32_t cpusubtype;
    file_view_t file;
    const void *slice;
    size_t slice_size;
    hk_chained_fixups_t fixups;
    segment_collect_t collected;
    hk_chained_segment_mapping_t mappings[HK_REBIND_MAX_SEGMENTS];
    uint64_t preferred;
} g_rebind_file_cache[HK_REBIND_FILE_CACHE_SIZE] = {0};
static uint32_t g_rebind_file_cache_next = 0;

static void __attribute__((unused)) rebind_file_cache_clear_locked(void) {
    for (int i = 0; i < HK_REBIND_FILE_CACHE_SIZE; i++) {
        if (!g_rebind_file_cache[i].valid) continue;
        free(g_rebind_file_cache[i].path);
        close_file_view(&g_rebind_file_cache[i].file);
        memset(&g_rebind_file_cache[i], 0, sizeof(g_rebind_file_cache[i]));
    }
    g_rebind_file_cache_next = 0;
}

// Second level: per-(image,symbol) site list cache. 8-entry, per-image.
#define HK_REBIND_SYMBOL_CACHE_SIZE 8
static pthread_mutex_t g_rebind_symbol_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool valid;
    char *path;
    const void *image_base;
    char *symbol;
    hk_symbol_name_convention_t convention;
    uint32_t count;
    struct {
        uintptr_t address;
        int64_t addend;
        hk_pac_schema_t schema;
        bool weak;
    } sites[HK_REBIND_MAX_SITES];
} g_rebind_symbol_cache[HK_REBIND_SYMBOL_CACHE_SIZE] = {0};
static uint32_t g_rebind_symbol_cache_next = 0;

static void __attribute__((unused)) rebind_symbol_cache_clear_locked(void) {
    for (int i = 0; i < HK_REBIND_SYMBOL_CACHE_SIZE; i++) {
        if (!g_rebind_symbol_cache[i].valid) continue;
        free(g_rebind_symbol_cache[i].path);
        free(g_rebind_symbol_cache[i].symbol);
        memset(&g_rebind_symbol_cache[i], 0, sizeof(g_rebind_symbol_cache[i]));
    }
    g_rebind_symbol_cache_next = 0;
}

// Third level: dyld cache patch per-symbol cache. The shared cache's patch table
// for a given symbol is the same for all hooks in the process, so caching it
// avoids re-scanning the cache's patch table (thousands of entries) per hook.
// Keyed per (cache, importer, symbol): the site list is per-importer, so a
// symbol-only key would serve one image's sites to every other image.
// ponytail: fixed-size array sized to the loaded-image working set; a hash
// table keyed by (cache, importer, symbol) if memory pressure ever matters.
#define HK_CACHE_PATCH_CACHE_SIZE 256
static pthread_mutex_t g_cache_patch_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool valid;
    bool found;
    const void *cache_base;
    const void *image_header;
    char *symbol;
    hk_symbol_name_convention_t convention;
    bool include_shared_got;
    hk_rebind_plan_t plan;
} g_cache_patch_cache[HK_CACHE_PATCH_CACHE_SIZE] = {0};
static uint32_t g_cache_patch_cache_next = 0;

static bool collect_segment(void *opaque, uint32_t index,
                            const hk_macho_segment_t *segment) {
    segment_collect_t *ctx = opaque;
    if (index >= HK_REBIND_MAX_SEGMENTS) {
        ctx->overflow = true;
        return false;
    }
    ctx->segments[index] = *segment;
    ctx->count = index + 1;
    return true;
}

static hk_rebind_status_t prepare_file_chains(
    const hk_rebind_target_t *target, const char *symbol_name,
    hk_symbol_name_convention_t convention,
    const hk_symbol_candidates_t *candidates,
    const hk_macho_header_t *loaded_header, hk_rebind_plan_t *plan) {
    // Fast path: file_image seam (host tests) bypasses cache — no file on disk.
    if (target->file_image && target->file_image_size) {
        file_view_t file;
        if (!open_file_view(target, &file)) {
            return HK_REBIND_METADATA_UNAVAILABLE;
        }
        const void *slice = NULL;
        size_t slice_size = 0;
        hk_macho_status_t macho = hk_macho_select_slice(
            file.base, file.size, loaded_header->cputype, loaded_header->cpusubtype,
            &slice, &slice_size);
        if (macho != HK_MACHO_OK) {
            close_file_view(&file);
            return macho == HK_MACHO_NOT_FOUND ? HK_REBIND_UNSUPPORTED_FORMAT
                                                : HK_REBIND_MALFORMED_IMAGE;
        }
        uint8_t live_uuid[16], file_uuid[16];
        size_t live_size = HK_MACHO_HEADER_64_SIZE + loaded_header->sizeofcmds;
        if (hk_macho_copy_uuid(target->image_base, live_size, live_uuid) != HK_MACHO_OK ||
            hk_macho_copy_uuid(slice, slice_size, file_uuid) != HK_MACHO_OK ||
            memcmp(live_uuid, file_uuid, sizeof(live_uuid)) != 0) {
            close_file_view(&file);
            return HK_REBIND_MALFORMED_IMAGE;
        }
        size_t fixup_offset = 0, fixup_size = 0;
        if (hk_macho_find_chained_fixups(slice, slice_size, &fixup_offset,
                                         &fixup_size) != HK_MACHO_OK) {
            close_file_view(&file);
            return HK_REBIND_MALFORMED_IMAGE;
        }
        hk_chained_fixups_t fixups;
        hk_chained_status_t chained = hk_chained_fixups_parse(
            (const uint8_t *)slice + fixup_offset, fixup_size, &fixups);
        if (chained != HK_CHAINED_OK) {
            close_file_view(&file);
            return chained == HK_CHAINED_UNSUPPORTED_FORMAT ||
                           chained == HK_CHAINED_UNSUPPORTED_VERSION
                ? HK_REBIND_UNSUPPORTED_FORMAT : HK_REBIND_MALFORMED_IMAGE;
        }
        segment_collect_t collected;
        memset(&collected, 0, sizeof(collected));
        if (hk_macho_iterate_segments(slice, slice_size, collect_segment,
                                       &collected) != HK_MACHO_OK ||
            collected.overflow || collected.count == 0) {
            close_file_view(&file);
            return collected.overflow ? HK_REBIND_UNSUPPORTED_FORMAT
                                      : HK_REBIND_MALFORMED_IMAGE;
        }
        uint64_t preferred = UINT64_MAX;
        for (uint32_t i = 0; i < collected.count; i++) {
            hk_macho_segment_t *segment = &collected.segments[i];
            if (strcmp(segment->segname, "__PAGEZERO") != 0 &&
                segment->vmaddr < preferred) {
                preferred = segment->vmaddr;
            }
        }
        if (preferred == UINT64_MAX) {
            close_file_view(&file);
            return HK_REBIND_MALFORMED_IMAGE;
        }
        hk_chained_segment_mapping_t mappings[HK_REBIND_MAX_SEGMENTS];
        for (uint32_t i = 0; i < collected.count; i++) {
            hk_macho_segment_t *segment = &collected.segments[i];
            if (strcmp(segment->segname, "__PAGEZERO") == 0) {
                memset(&mappings[i], 0, sizeof(mappings[i]));
                continue;
            }
            if (segment->vmaddr < preferred || segment->fileoff > slice_size ||
                segment->filesize > slice_size - segment->fileoff) {
                close_file_view(&file);
                return HK_REBIND_MALFORMED_IMAGE;
            }
            mappings[i].image_offset = segment->vmaddr - preferred;
            mappings[i].file_offset = segment->fileoff;
            mappings[i].file_size = segment->filesize;
        }
        chained_ctx_t ctx = {
            .fixups = &fixups,
            .candidates = candidates,
            .image_base = (uintptr_t)target->image_base,
            .image_size = target->image_size,
            .plan = plan,
        };
        chained = hk_chained_fixups_iterate_file_binds(
            &fixups, slice, slice_size, mappings, collected.count,
            chained_bind_cb, &ctx);
        close_file_view(&file);
        if (ctx.overflow) return HK_REBIND_TOO_MANY_SITES;
        if (ctx.pac_mismatch) return HK_REBIND_PAC_MISMATCH;
        if (ctx.malformed || chained == HK_CHAINED_MALFORMED)
            return HK_REBIND_MALFORMED_IMAGE;
        if (chained != HK_CHAINED_OK) return HK_REBIND_UNSUPPORTED_FORMAT;
        return plan->count ? HK_REBIND_OK : HK_REBIND_NOT_FOUND;
    }

    // Cached path for file-backed images (device): 4-entry, per-image.
    // ponytail: 4-entry, per-image map if throughput matters (was single-entry, thrash on 2 images).
    bool hit = false;
    int hit_idx = -1;
    hk_chained_fixups_t cached_fixups;
    segment_collect_t cached_collected;
    hk_chained_segment_mapping_t cached_mappings[HK_REBIND_MAX_SEGMENTS];
    const void *cached_slice = NULL;
    size_t cached_slice_size = 0;
    pthread_mutex_lock(&g_rebind_file_cache_lock);
    for (int i = 0; i < HK_REBIND_FILE_CACHE_SIZE; i++) {
        if (g_rebind_file_cache[i].valid
            && target->image_path && g_rebind_file_cache[i].path
            && strcmp(target->image_path, g_rebind_file_cache[i].path) == 0
            && target->image_base == g_rebind_file_cache[i].image_base
            && loaded_header->cputype == g_rebind_file_cache[i].cputype
            && loaded_header->cpusubtype == g_rebind_file_cache[i].cpusubtype) {
            hit = true;
            hit_idx = i;
            break;
        }
    }
    if (hit) {
        cached_fixups = g_rebind_file_cache[hit_idx].fixups;
        cached_collected = g_rebind_file_cache[hit_idx].collected;
        memcpy(cached_mappings, g_rebind_file_cache[hit_idx].mappings, sizeof(cached_mappings));
        cached_slice = g_rebind_file_cache[hit_idx].slice;
        cached_slice_size = g_rebind_file_cache[hit_idx].slice_size;
    }
    pthread_mutex_unlock(&g_rebind_file_cache_lock);
    if (hit) {
        // Second level: per-(image,symbol) site list — 8-entry.
        int sym_idx = -1;
        pthread_mutex_lock(&g_rebind_symbol_cache_lock);
        for (int i = 0; i < HK_REBIND_SYMBOL_CACHE_SIZE; i++) {
            if (!g_rebind_symbol_cache[i].valid) continue;
            if (g_rebind_symbol_cache[i].image_base != target->image_base) continue;
            if (g_rebind_symbol_cache[i].convention != convention) continue;
            if (!g_rebind_symbol_cache[i].path || !g_rebind_symbol_cache[i].symbol) continue;
            if (strcmp(g_rebind_symbol_cache[i].path, target->image_path) != 0) continue;
            if (strcmp(g_rebind_symbol_cache[i].symbol, symbol_name) != 0) continue;
            sym_idx = i;
            break;
        }
        if (sym_idx != -1) {
            uint32_t cnt = g_rebind_symbol_cache[sym_idx].count;
            struct { uintptr_t address; int64_t addend; hk_pac_schema_t schema; bool weak; } tmp[HK_REBIND_MAX_SITES];
            for (uint32_t i = 0; i < cnt; i++) {
                tmp[i].address = g_rebind_symbol_cache[sym_idx].sites[i].address;
                tmp[i].addend = g_rebind_symbol_cache[sym_idx].sites[i].addend;
                tmp[i].schema = g_rebind_symbol_cache[sym_idx].sites[i].schema;
                tmp[i].weak = g_rebind_symbol_cache[sym_idx].sites[i].weak;
            }
            pthread_mutex_unlock(&g_rebind_symbol_cache_lock);
            for (uint32_t i = 0; i < cnt; i++) {
                add_site_status_t st = add_site(plan, tmp[i].address, &tmp[i].schema, tmp[i].addend, tmp[i].weak, true, false);
                if (st == ADD_SITE_OVERFLOW) return HK_REBIND_TOO_MANY_SITES;
                if (st == ADD_SITE_MALFORMED) return HK_REBIND_MALFORMED_IMAGE;
                if (st == ADD_SITE_PAC_MISMATCH) return HK_REBIND_PAC_MISMATCH;
                if (st != ADD_SITE_OK) return HK_REBIND_MALFORMED_IMAGE;
            }
            return plan->count ? HK_REBIND_OK : HK_REBIND_NOT_FOUND;
        }
        pthread_mutex_unlock(&g_rebind_symbol_cache_lock);
        chained_ctx_t ctx = {
            .fixups = &cached_fixups,
            .candidates = candidates,
            .image_base = (uintptr_t)target->image_base,
            .image_size = target->image_size,
            .plan = plan,
        };
        hk_chained_status_t chained = hk_chained_fixups_iterate_file_binds(
            &cached_fixups, cached_slice, cached_slice_size, cached_mappings, cached_collected.count,
            chained_bind_cb, &ctx);
        if (ctx.overflow) return HK_REBIND_TOO_MANY_SITES;
        if (ctx.pac_mismatch) return HK_REBIND_PAC_MISMATCH;
        if (ctx.malformed || chained == HK_CHAINED_MALFORMED)
            return HK_REBIND_MALFORMED_IMAGE;
        if (chained != HK_CHAINED_OK) return HK_REBIND_UNSUPPORTED_FORMAT;
        // Always cache, even for not-found (count 0) — so next hook for same
        // image+symbol can return NOT_FOUND without re-scanning thousands of binds.
        {
            pthread_mutex_lock(&g_rebind_symbol_cache_lock);
            int victim = -1;
            for (int i = 0; i < HK_REBIND_SYMBOL_CACHE_SIZE; i++) {
                if (!g_rebind_symbol_cache[i].valid) { victim = i; break; }
            }
            if (victim == -1) {
                victim = g_rebind_symbol_cache_next % HK_REBIND_SYMBOL_CACHE_SIZE;
                free(g_rebind_symbol_cache[victim].path);
                free(g_rebind_symbol_cache[victim].symbol);
                memset(&g_rebind_symbol_cache[victim], 0, sizeof(g_rebind_symbol_cache[victim]));
                g_rebind_symbol_cache_next = (victim + 1) % HK_REBIND_SYMBOL_CACHE_SIZE;
            }
            size_t plen = strlen(target->image_path) + 1;
            size_t slen = strlen(symbol_name) + 1;
            char *p = (char *)malloc(plen);
            char *s = (char *)malloc(slen);
            if (p && s) {
                memcpy(p, target->image_path, plen);
                memcpy(s, symbol_name, slen);
                g_rebind_symbol_cache[victim].path = p;
                g_rebind_symbol_cache[victim].symbol = s;
                g_rebind_symbol_cache[victim].image_base = target->image_base;
                g_rebind_symbol_cache[victim].convention = convention;
                g_rebind_symbol_cache[victim].count = plan->count;
                for (uint32_t i = 0; i < plan->count; i++) {
                    g_rebind_symbol_cache[victim].sites[i].address = plan->sites[i].address;
                    g_rebind_symbol_cache[victim].sites[i].addend = plan->sites[i].addend;
                    g_rebind_symbol_cache[victim].sites[i].schema = plan->sites[i].schema;
                    g_rebind_symbol_cache[victim].sites[i].weak = plan->sites[i].weak_import;
                }
                g_rebind_symbol_cache[victim].valid = true;
            } else {
                free(p); free(s);
            }
            pthread_mutex_unlock(&g_rebind_symbol_cache_lock);
        }
        return plan->count ? HK_REBIND_OK : HK_REBIND_NOT_FOUND;
    }

    // Miss: full load then populate cache
    file_view_t file;
    if (!open_file_view(target, &file)) {
        return HK_REBIND_METADATA_UNAVAILABLE;
    }
    const void *slice = NULL;
    size_t slice_size = 0;
    hk_macho_status_t macho = hk_macho_select_slice(
        file.base, file.size, loaded_header->cputype, loaded_header->cpusubtype,
        &slice, &slice_size);
    if (macho != HK_MACHO_OK) {
        close_file_view(&file);
        return macho == HK_MACHO_NOT_FOUND ? HK_REBIND_UNSUPPORTED_FORMAT
                                            : HK_REBIND_MALFORMED_IMAGE;
    }
    uint8_t live_uuid[16], file_uuid[16];
    size_t live_size = HK_MACHO_HEADER_64_SIZE + loaded_header->sizeofcmds;
    if (hk_macho_copy_uuid(target->image_base, live_size, live_uuid) != HK_MACHO_OK ||
        hk_macho_copy_uuid(slice, slice_size, file_uuid) != HK_MACHO_OK ||
        memcmp(live_uuid, file_uuid, sizeof(live_uuid)) != 0) {
        close_file_view(&file);
        return HK_REBIND_MALFORMED_IMAGE;
    }
    size_t fixup_offset = 0, fixup_size = 0;
    if (hk_macho_find_chained_fixups(slice, slice_size, &fixup_offset,
                                     &fixup_size) != HK_MACHO_OK) {
        close_file_view(&file);
        return HK_REBIND_MALFORMED_IMAGE;
    }
    hk_chained_fixups_t fixups;
    hk_chained_status_t chained = hk_chained_fixups_parse(
        (const uint8_t *)slice + fixup_offset, fixup_size, &fixups);
    if (chained != HK_CHAINED_OK) {
        close_file_view(&file);
        return chained == HK_CHAINED_UNSUPPORTED_FORMAT ||
                       chained == HK_CHAINED_UNSUPPORTED_VERSION
            ? HK_REBIND_UNSUPPORTED_FORMAT : HK_REBIND_MALFORMED_IMAGE;
    }
    segment_collect_t collected;
    memset(&collected, 0, sizeof(collected));
    if (hk_macho_iterate_segments(slice, slice_size, collect_segment,
                                   &collected) != HK_MACHO_OK ||
        collected.overflow || collected.count == 0) {
        close_file_view(&file);
        return collected.overflow ? HK_REBIND_UNSUPPORTED_FORMAT
                                  : HK_REBIND_MALFORMED_IMAGE;
    }
    uint64_t preferred = UINT64_MAX;
    for (uint32_t i = 0; i < collected.count; i++) {
        hk_macho_segment_t *segment = &collected.segments[i];
        if (strcmp(segment->segname, "__PAGEZERO") != 0 &&
            segment->vmaddr < preferred) {
            preferred = segment->vmaddr;
        }
    }
    if (preferred == UINT64_MAX) {
        close_file_view(&file);
        return HK_REBIND_MALFORMED_IMAGE;
    }
    hk_chained_segment_mapping_t mappings[HK_REBIND_MAX_SEGMENTS];
    for (uint32_t i = 0; i < collected.count; i++) {
        hk_macho_segment_t *segment = &collected.segments[i];
        if (strcmp(segment->segname, "__PAGEZERO") == 0) {
            memset(&mappings[i], 0, sizeof(mappings[i]));
            continue;
        }
        if (segment->vmaddr < preferred || segment->fileoff > slice_size ||
            segment->filesize > slice_size - segment->fileoff) {
            close_file_view(&file);
            return HK_REBIND_MALFORMED_IMAGE;
        }
        mappings[i].image_offset = segment->vmaddr - preferred;
        mappings[i].file_offset = segment->fileoff;
        mappings[i].file_size = segment->filesize;
    }
    // Populate cache on success path (keep file mapped) — 4-entry with eviction
    if (target->image_path) {
        pthread_mutex_lock(&g_rebind_file_cache_lock);
        int victim = -1;
        for (int i = 0; i < HK_REBIND_FILE_CACHE_SIZE; i++) {
            if (!g_rebind_file_cache[i].valid) { victim = i; break; }
        }
        if (victim == -1) {
            victim = g_rebind_file_cache_next % HK_REBIND_FILE_CACHE_SIZE;
            if (g_rebind_file_cache[victim].valid) {
                free(g_rebind_file_cache[victim].path);
                close_file_view(&g_rebind_file_cache[victim].file);
                memset(&g_rebind_file_cache[victim], 0, sizeof(g_rebind_file_cache[victim]));
            }
            g_rebind_file_cache_next = (victim + 1) % HK_REBIND_FILE_CACHE_SIZE;
        }
        size_t _path_len = strlen(target->image_path) + 1;
        g_rebind_file_cache[victim].path = (char *)malloc(_path_len);
        if (g_rebind_file_cache[victim].path) memcpy(g_rebind_file_cache[victim].path, target->image_path, _path_len);
        g_rebind_file_cache[victim].image_base = target->image_base;
        g_rebind_file_cache[victim].cputype = loaded_header->cputype;
        g_rebind_file_cache[victim].cpusubtype = loaded_header->cpusubtype;
        g_rebind_file_cache[victim].file = file;
        g_rebind_file_cache[victim].slice = slice;
        g_rebind_file_cache[victim].slice_size = slice_size;
        g_rebind_file_cache[victim].fixups = fixups;
        g_rebind_file_cache[victim].collected = collected;
        memcpy(g_rebind_file_cache[victim].mappings, mappings, sizeof(mappings));
        g_rebind_file_cache[victim].preferred = preferred;
        g_rebind_file_cache[victim].valid = (g_rebind_file_cache[victim].path != NULL);
        if (!g_rebind_file_cache[victim].valid) {
            close_file_view(&file);
            memset(&g_rebind_file_cache[victim], 0, sizeof(g_rebind_file_cache[victim]));
        } else {
            file.base = NULL;
            file.size = 0;
            file.mapped = false;
        }
        pthread_mutex_unlock(&g_rebind_file_cache_lock);
    }
    chained_ctx_t ctx = {
        .fixups = &fixups,
        .candidates = candidates,
        .image_base = (uintptr_t)target->image_base,
        .image_size = target->image_size,
        .plan = plan,
    };
    chained = hk_chained_fixups_iterate_file_binds(
        &fixups, slice, slice_size, mappings, collected.count,
        chained_bind_cb, &ctx);
    // Keep file mapped until after iterate; for cached case file was transferred (empty) so no-op
    if (file.mapped) {
        close_file_view(&file);
    }
    if (ctx.overflow) return HK_REBIND_TOO_MANY_SITES;
    if (ctx.pac_mismatch) return HK_REBIND_PAC_MISMATCH;
    if (ctx.malformed || chained == HK_CHAINED_MALFORMED)
        return HK_REBIND_MALFORMED_IMAGE;
    if (chained != HK_CHAINED_OK) return HK_REBIND_UNSUPPORTED_FORMAT;
    if (1) {
        pthread_mutex_lock(&g_rebind_symbol_cache_lock);
        int existing = -1;
        for (int i = 0; i < HK_REBIND_SYMBOL_CACHE_SIZE; i++) {
            if (!g_rebind_symbol_cache[i].valid) continue;
            if (g_rebind_symbol_cache[i].image_base != target->image_base) continue;
            if (g_rebind_symbol_cache[i].convention != convention) continue;
            if (!g_rebind_symbol_cache[i].path || !g_rebind_symbol_cache[i].symbol) continue;
            if (strcmp(g_rebind_symbol_cache[i].path, target->image_path) != 0) continue;
            if (strcmp(g_rebind_symbol_cache[i].symbol, symbol_name) != 0) continue;
            existing = i;
            break;
        }
        if (existing == -1) {
            int victim = -1;
            for (int i = 0; i < HK_REBIND_SYMBOL_CACHE_SIZE; i++) {
                if (!g_rebind_symbol_cache[i].valid) { victim = i; break; }
            }
            if (victim == -1) {
                victim = g_rebind_symbol_cache_next % HK_REBIND_SYMBOL_CACHE_SIZE;
                free(g_rebind_symbol_cache[victim].path);
                free(g_rebind_symbol_cache[victim].symbol);
                memset(&g_rebind_symbol_cache[victim], 0, sizeof(g_rebind_symbol_cache[victim]));
                g_rebind_symbol_cache_next = (victim + 1) % HK_REBIND_SYMBOL_CACHE_SIZE;
            }
            size_t plen = strlen(target->image_path) + 1;
            size_t slen = strlen(symbol_name) + 1;
            char *p = (char *)malloc(plen);
            char *s = (char *)malloc(slen);
            if (p && s) {
                memcpy(p, target->image_path, plen);
                memcpy(s, symbol_name, slen);
                g_rebind_symbol_cache[victim].path = p;
                g_rebind_symbol_cache[victim].symbol = s;
                g_rebind_symbol_cache[victim].image_base = target->image_base;
                g_rebind_symbol_cache[victim].convention = convention;
                g_rebind_symbol_cache[victim].count = plan->count;
                for (uint32_t i = 0; i < plan->count; i++) {
                    g_rebind_symbol_cache[victim].sites[i].address = plan->sites[i].address;
                    g_rebind_symbol_cache[victim].sites[i].addend = plan->sites[i].addend;
                    g_rebind_symbol_cache[victim].sites[i].schema = plan->sites[i].schema;
                    g_rebind_symbol_cache[victim].sites[i].weak = plan->sites[i].weak_import;
                }
                g_rebind_symbol_cache[victim].valid = true;
            } else {
                free(p); free(s);
            }
        } else {
        }
        pthread_mutex_unlock(&g_rebind_symbol_cache_lock);
    } else {
    }
    return plan->count ? HK_REBIND_OK : HK_REBIND_NOT_FOUND;
}

static hk_rebind_status_t finalize_plan(hk_rebind_plan_t *plan) {
    if (plan->count == 0) {
        return HK_REBIND_NOT_FOUND;
    }
    plan->original = plan->sites[0].callable_original;
    plan->originals_agree = true;
    for (uint32_t i = 1; i < plan->count; i++) {
        if (plan->sites[i].callable_original != plan->original) {
            plan->originals_agree = false;
            break;
        }
    }
    return HK_REBIND_OK;
}

#if defined(__APPLE__)
extern const void *_dyld_get_shared_cache_range(size_t *length)
    __attribute__((weak_import));
#endif

static void live_cache_range(const hk_rebind_target_t *target,
                             const void **out_base, size_t *out_size) {
    *out_base = target->cache_base;
    *out_size = target->cache_size;
#if defined(__APPLE__)
    if (!*out_base && _dyld_get_shared_cache_range) {
        *out_base = _dyld_get_shared_cache_range(out_size);
    }
#endif
}

hk_rebind_status_t hk_rebind_prepare(const hk_rebind_target_t *target,
                                     const char *symbol_name,
                                     hk_symbol_name_convention_t convention,
                                     hk_rebind_plan_t *out_plan) {
    if (!target || !target->image_base || !symbol_name || !out_plan) {
        return HK_REBIND_INVALID_ARGUMENT;
    }
    memset(out_plan, 0, sizeof(*out_plan));

    // The same linker-form expansion every other resolver uses, from the one
    // place it lives.
    hk_symbol_candidates_t candidates;
    if (hk_symbol_build_candidates(symbol_name, convention, &candidates) != HK_RESOLVE_OK) {
        return HK_REBIND_INVALID_ARGUMENT;
    }

    const void *image = target->image_base;
    size_t size = target->image_size;

    hk_macho_header_t loaded_header;
    if (hk_macho_peek_header(image, HK_MACHO_HEADER_64_SIZE,
                             &loaded_header) != HK_MACHO_OK ||
        (size_t)loaded_header.sizeofcmds > SIZE_MAX - HK_MACHO_HEADER_64_SIZE) {
        return HK_REBIND_MALFORMED_IMAGE;
    }
    const size_t header_size = HK_MACHO_HEADER_64_SIZE + loaded_header.sizeofcmds;
    if (size < header_size) {
        return HK_REBIND_MALFORMED_IMAGE;
    }

    // Cached images no longer carry their input bind chains. The live cache
    // patch table is dyld's authoritative symbol-to-use map.
    const void *cache_base = NULL;
    size_t cache_size = 0;
    live_cache_range(target, &cache_base, &cache_size);
    // Check cache patch per-(importer,symbol) cache
    if (cache_base && symbol_name) {
        pthread_mutex_lock(&g_cache_patch_cache_lock);
        for (int i=0;i<HK_CACHE_PATCH_CACHE_SIZE;i++) if (g_cache_patch_cache[i].valid && g_cache_patch_cache[i].cache_base==cache_base && g_cache_patch_cache[i].image_header==image && g_cache_patch_cache[i].include_shared_got==target->include_shared_cache_got && g_cache_patch_cache[i].symbol && strcmp(g_cache_patch_cache[i].symbol,symbol_name)==0 && g_cache_patch_cache[i].convention==convention) {
            *out_plan = g_cache_patch_cache[i].plan;
            pthread_mutex_unlock(&g_cache_patch_cache_lock);
            return g_cache_patch_cache[i].found ? finalize_plan(out_plan) : HK_REBIND_NOT_FOUND;
        }
        pthread_mutex_unlock(&g_cache_patch_cache_lock);
    }
if (cache_base && (uintptr_t)image >= (uintptr_t)cache_base &&
        (uintptr_t)image - (uintptr_t)cache_base < cache_size) {
        hk_cache_patch_target_t cache_target;
        memset(&cache_target, 0, sizeof(cache_target));
        cache_target.cache_base = cache_base;
        cache_target.cache_size = cache_size;
        cache_target.image_header = image;
        cache_target.image_header_size = header_size;
        cache_target.image_slide = target->slide;
        cache_target.image_path = target->image_path;
        cache_target.uuid_present = target->uuid_present;
        memcpy(cache_target.uuid, target->uuid, sizeof(cache_target.uuid));
        cache_target.include_shared_got = target->include_shared_cache_got;
        cache_collect_ctx_t collect = { .plan = out_plan };
        hk_cache_patch_status_t status = hk_dyld_cache_iterate_symbol_uses(
            &cache_target, symbol_name, convention, cache_patch_cb, &collect);
        if (collect.overflow) return HK_REBIND_TOO_MANY_SITES;
        if (collect.pac_mismatch) return HK_REBIND_PAC_MISMATCH;
        if (collect.malformed) return HK_REBIND_MALFORMED_IMAGE;
        switch (status) {
        case HK_CACHE_PATCH_OK:
        case HK_CACHE_PATCH_NOT_FOUND: {
            pthread_mutex_lock(&g_cache_patch_cache_lock);
            int victim=-1; for(int i=0;i<HK_CACHE_PATCH_CACHE_SIZE;i++) if(!g_cache_patch_cache[i].valid){victim=i;break;} if(victim==-1){victim=g_cache_patch_cache_next%HK_CACHE_PATCH_CACHE_SIZE; free(g_cache_patch_cache[victim].symbol); memset(&g_cache_patch_cache[victim],0,sizeof(g_cache_patch_cache[victim])); g_cache_patch_cache_next=(victim+1)%HK_CACHE_PATCH_CACHE_SIZE;}
            size_t slen=strlen(symbol_name)+1; char *s=(char*)malloc(slen); if(s){memcpy(s,symbol_name,slen); g_cache_patch_cache[victim].cache_base=cache_base; g_cache_patch_cache[victim].image_header=image; g_cache_patch_cache[victim].include_shared_got=target->include_shared_cache_got; g_cache_patch_cache[victim].symbol=s; g_cache_patch_cache[victim].convention=convention; g_cache_patch_cache[victim].plan=*out_plan; g_cache_patch_cache[victim].found=(status==HK_CACHE_PATCH_OK); g_cache_patch_cache[victim].valid=true; }
            pthread_mutex_unlock(&g_cache_patch_cache_lock);
            return (status==HK_CACHE_PATCH_OK) ? finalize_plan(out_plan) : HK_REBIND_NOT_FOUND; }
        case HK_CACHE_PATCH_SCOPE_UNREPRESENTABLE:
            return HK_REBIND_SCOPE_UNREPRESENTABLE;
        case HK_CACHE_PATCH_UNSUPPORTED:
            return HK_REBIND_UNSUPPORTED_FORMAT;
        case HK_CACHE_PATCH_NO_METADATA:
            break; // genuine legacy cache: try LC_DYSYMTAB below
        case HK_CACHE_PATCH_NOT_CACHE:
        case HK_CACHE_PATCH_INVALID_ARGUMENT:
        case HK_CACHE_PATCH_MALFORMED:
        default:
            return HK_REBIND_MALFORMED_IMAGE;
        }
    }

    // A chained-fixup command is authoritative. Walk the original file words,
    // never the live slots dyld has already resolved and PAC-signed.
    size_t chained_command = 0;
    uint32_t chained_command_size = 0;
    hk_macho_status_t chain_command_status = hk_macho_find_load_command(
        image, header_size, HK_LC_DYLD_CHAINED_FIXUPS,
        &chained_command, &chained_command_size);
    if (chain_command_status == HK_MACHO_OK) {
        (void)chained_command;
        if (chained_command_size < HK_LINKEDIT_DATA_CMD_SIZE) {
            return HK_REBIND_MALFORMED_IMAGE;
        }
        hk_rebind_status_t status = prepare_file_chains(
            target, symbol_name, convention, &candidates, &loaded_header, out_plan);
        return status == HK_REBIND_OK ? finalize_plan(out_plan) : status;
    }
    if (chain_command_status != HK_MACHO_NOT_FOUND) {
        return HK_REBIND_MALFORMED_IMAGE;
    }

    // Legacy image: LC_DYSYMTAB is authoritative.
    hk_import_tables_t tables;
    hk_import_status_t import_status = hk_import_tables_from_loaded_image(
        image, header_size, target->slide, &tables);
    if (import_status != HK_IMPORT_OK) {
        return HK_REBIND_MALFORMED_IMAGE;
    }
    if (!tables.indirect_symbols) {
        return HK_REBIND_NOT_FOUND;
    }
    collect_ctx_t collect = {
        .candidates = &candidates,
        .slide = target->slide,
        .plan = out_plan,
        .error = ADD_SITE_OK,
    };
    import_status = hk_import_slots_iterate(
        image, header_size, &tables, dysymtab_slot_cb, &collect);
    if (collect.error == ADD_SITE_OVERFLOW) return HK_REBIND_TOO_MANY_SITES;
    if (collect.error == ADD_SITE_PAC_MISMATCH) return HK_REBIND_PAC_MISMATCH;
    if (collect.error == ADD_SITE_MALFORMED || import_status != HK_IMPORT_OK) {
        return HK_REBIND_MALFORMED_IMAGE;
    }
    return finalize_plan(out_plan);
}

// ---- phase 2: commit ----------------------------------------------------

bool hk_rebind_replacement_for_site(const hk_rebind_site_t *site,
                                    uint64_t replacement,
                                    uint64_t *out_value) {
    if (!site || !out_value) {
        return false;
    }
    uintptr_t adjusted = 0;
    if (!checked_adjust(hk_pac_strip_code((uintptr_t)replacement),
                        site->addend, true, &adjusted)) {
        return false;
    }
    *out_value = hk_pac_sign_slot(adjusted, &site->schema, site->address);
    return true;
}

static void record_artifact(hk_artifact_sink_t *sink, const hk_rebind_site_t *site,
                            uint64_t replacement) {
    if (!sink) {
        return;
    }
    hk_artifact_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(a);
    a.struct_version = HK_ABI_VERSION_3_0;
    a.kind = HK_ARTIFACT_IMPORT_SLOT;
    a.state = HK_ARTIFACT_COMMITTED;
    a.effects = HK_EFFECT_IMPORT_MUTATION;
    a.engine_id.data = "fishhook";
    a.engine_id.length = 8;
    a.import_slot_address = site->address;
    a.address = site->address;
    a.original_pointer = (void *)(uintptr_t)site->original;
    a.replacement_pointer = (void *)(uintptr_t)replacement;
    // Restoring a slot is a plain store of the value we already hold, so this
    // is genuinely reversible -- unlike a relocated inline patch.
    a.mechanically_reversible = true;
    a.safe_to_reverse_after_activation = true;
    (void)hk_artifact_sink_record(sink, &a);
}

hk_mutation_state_t hk_rebind_commit(const hk_rebind_target_t *target,
                                     const hk_rebind_plan_t *plan,
                                     uint64_t replacement,
                                     hk_artifact_sink_t *sink,
                                     uint32_t *out_written) {
    if (out_written) {
        *out_written = 0;
    }
    if (!target || !plan || !target->write || plan->count == 0) {
        return HK_MUTATION_NONE;  // nothing attempted, nothing touched
    }

    uint64_t replacements[HK_REBIND_MAX_SITES];
    for (uint32_t i = 0; i < plan->count; i++) {
        if (!hk_rebind_replacement_for_site(&plan->sites[i], replacement,
                                            &replacements[i])) {
            return HK_MUTATION_NONE;
        }
    }

    if (sink && sink->require_predecessor_match) {
        const uint64_t predecessor =
            (uint64_t)(uintptr_t)sink->required_predecessor;
        for (uint32_t i = 0; i < plan->count; i++) {
            if (plan->sites[i].callable_original != predecessor) {
                return HK_MUTATION_NONE;
            }
        }
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < plan->count; i++) {
        const hk_rebind_site_t *site = &plan->sites[i];
        uint64_t site_replacement = replacements[i];

        // Invariant #3: revalidate immediately before the write. If the slot
        // no longer holds what prepare saw, something else changed it since --
        // possibly another hooking consumer -- and writing would silently
        // destroy their work.
        if (hk_rebind_read_slot(site->address) != site->original) {
            // Refusing on the FIRST site means nothing was touched; refusing
            // later means the image is already mixed.
            if (out_written) { *out_written = written; }
            return (written == 0) ? HK_MUTATION_NONE : HK_MUTATION_PARTIAL;
        }

        if (!target->write(target->write_ctx, site->address, site_replacement)) {
            if (out_written) { *out_written = written; }
            // Invariant #4: after a partial mutation no fallback may be
            // attempted, so this must be reported as PARTIAL, never as a
            // clean failure the router could retry elsewhere.
            return (written == 0) ? HK_MUTATION_NONE : HK_MUTATION_PARTIAL;
        }
        record_artifact(sink, site, site_replacement);
        written++;
    }

    if (out_written) {
        *out_written = written;
    }
    return HK_MUTATION_COMPLETE;
}
