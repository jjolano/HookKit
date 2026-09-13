// bench_resolvers.c — synthetic resolver microbenchmarks (host only)
// ponytail: reuse existing resolver code, no device.
#include "bench_common.h"
#include "../../src/resolvers/HKMachO.h"
#include "../../src/resolvers/HKSymbolTable.h"
#include "../../src/resolvers/HKSymbolResolve.h"
#include "../../src/resolvers/HKExportTrie.h"
#include "../../src/resolvers/HKImportSlots.h"
#include "../../src/resolvers/HKChainedFixups.h"
#include "../../src/core/HKImageCatalog.h"
#include "../../src/resolvers/HKDyldCachePatches.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

// ---- tiny synthetic trie: root -> 'a' -> 'ab' terminal ----
static uint8_t g_trie[] = {
    // node0: terminal 0, 1 child: edge "a" -> offset 7
    0x00, 0x01, 0x61, 0x00, 0x07,
    // node at 7: terminal 0, 1 child: edge "b" (+ "a" prefix already consumed => "ab")
    0x00, 0x01, 0x62, 0x00, 0x0d,
    // node at 0x0d: terminal 1 (flags=0, addr 0x10), child 0
    0x01, 0x00, 0x10, 0x00
};
// Note: the above encodes "ab" as path a->b. Search for "ab" walks a then b.

static void bench_export_hit(void *ctx) {
    (void)ctx;
    hk_export_symbol_t out;
    (void)hk_export_trie_find(g_trie, sizeof(g_trie), "ab", &out);
}
static void bench_export_miss(void *ctx) {
    (void)ctx;
    hk_export_symbol_t out;
    (void)hk_export_trie_find(g_trie, sizeof(g_trie), "zz", &out);
}

static void bench_symbol_candidates(void *ctx) {
    (void)ctx;
    hk_symbol_candidates_t c;
    (void)hk_symbol_build_candidates("mySym", HK_SYMBOL_NAME_C, &c);
}

// Mach-O header peek on tiny buffer
static uint8_t g_fake_macho[HK_MACHO_HEADER_64_SIZE + 32] = {0};
static void init_fake_macho(void) {
    // magic 64 + ncmds 0 etc.
    g_fake_macho[0]=0xcf; g_fake_macho[1]=0xfa; g_fake_macho[2]=0xed; g_fake_macho[3]=0xfe;
    // cputype arm64 0x0100000c
    g_fake_macho[4]=0x0c; g_fake_macho[5]=0x00; g_fake_macho[6]=0x00; g_fake_macho[7]=0x01;
    // filetype 2
    g_fake_macho[12]=0x02;
    // sizeofcmds 0
}
static void bench_macho_peek(void *ctx) {
    (void)ctx;
    hk_macho_header_t h;
    (void)hk_macho_peek_header(g_fake_macho, sizeof(g_fake_macho), &h);
}


static bool visit_noop(void *c, size_t i, const hk_image_entry_t *e){ (void)c;(void)i;(void)e; return true; }
static void bench_catalog_100(void *ctx){
    hk_image_catalog_t *cat = ctx;
    hk_image_selector_t sel; memset(&sel,0,sizeof(sel));
    sel.struct_size=sizeof(sel); sel.struct_version=HK_ABI_VERSION_3_0;
    sel.kind=HK_IMAGE_ANY_LOADED;
    (void)hk_image_catalog_match(cat,&sel,visit_noop,NULL);
}
static hk_image_catalog_t *make_catalog(int n){
    hk_image_catalog_t *c = hk_image_catalog_create();
    for(int i=0;i<n;i++){
        hk_image_entry_t e; memset(&e,0,sizeof(e));
        char path[32]; snprintf(path,sizeof(path),"/a/b/%d.dylib",i);
        e.path = path;
        e.header = (void*)(uintptr_t)(0x1000 + i*0x1000);
        hk_image_catalog_add_entry(c, &e);
    }
    return c;
}


// One sample models one preparation across 64 importers in a 2048-image cache.
// Keep lookup allocation and its first export scan inside the timed sample.
//
// Besides the single _bench_target symbol, the pool carries CACHE_SYMBOLS
// distinct symbols and every importer's client-export list references all of
// them, so one preparation resolves all of them. Symbols live at the tail of
// the export pool (below _bench_target) and each owns one patch location.
enum {
    CACHE_IMPORTERS = 64, CACHE_IMAGE_COUNT = 2048, CACHE_EXPORTS = 16384,
    CACHE_SYMBOLS = 32,
    // Symbol seeds [0, CACHE_PRESENT_SYMBOLS) exist in the cache; higher seeds
    // are queried under names the cache does not carry (missing-heavy case).
    CACHE_PRESENT_SYMBOLS = 8,
    // Client-export entries per importer: CACHE_SYMBOLS plus _bench_target.
    CACHE_CLIENT_ENTRIES = CACHE_SYMBOLS + 1,
    CACHE_SYMBOL_EXPORT_BASE = CACHE_EXPORTS - 1 - CACHE_SYMBOLS,
    // Location of symbol `k` (location 0 belongs to _bench_target at 0x200).
    CACHE_SYMBOL_LOCATION_BASE = 0x208,
    CACHE_SYMBOL_LOCATION_STRIDE = 8,
    CACHE_HEADERS = 0x20000, CACHE_PATCH = 0x40000,
    CACHE_IMAGES = CACHE_PATCH + 0x100,
    CACHE_EXPORT_TABLE = CACHE_IMAGES + CACHE_IMAGE_COUNT * 16,
    CACHE_CLIENTS = CACHE_EXPORT_TABLE + CACHE_EXPORTS * 8,
    CACHE_CLIENT_EXPORTS = CACHE_CLIENTS + CACHE_IMPORTERS * 12,
    CACHE_LOCATIONS = CACHE_CLIENT_EXPORTS + CACHE_IMPORTERS * CACHE_CLIENT_ENTRIES * 12,
    CACHE_GOT_CLIENTS = CACHE_LOCATIONS + CACHE_CLIENT_ENTRIES * 8,
    CACHE_NAMES = CACHE_GOT_CLIENTS + CACHE_IMAGE_COUNT * 8,
    CACHE_BYTES = CACHE_NAMES + CACHE_EXPORTS * 32
};
#define CACHE_UNSLID UINT64_C(0x0000700000000000)

static void cache_u32(uint8_t *b, size_t off, uint32_t value) {
    memcpy(b + off, &value, sizeof(value));
}
static void cache_u64(uint8_t *b, size_t off, uint64_t value) {
    memcpy(b + off, &value, sizeof(value));
}
static void cache_pair(uint8_t *b, size_t off, size_t address, size_t count) {
    cache_u64(b, off, CACHE_UNSLID + address);
    cache_u64(b, off + 8, count);
}

static uint8_t *make_patch_cache(void) {
    uint8_t *b = calloc(1, CACHE_BYTES);
    assert(b);
    cache_u32(b, 16, 0x200);
    cache_u32(b, 20, 1);
    cache_pair(b, 152, CACHE_PATCH, CACHE_BYTES - CACHE_PATCH);
    cache_u32(b, 220, 1u << 11);
    cache_u64(b, 0x200, CACHE_UNSLID);
    cache_u64(b, 0x208, CACHE_BYTES);
    cache_u32(b, 448, 0x240);
    cache_u32(b, 452, CACHE_IMAGE_COUNT);
    for (unsigned i = 0; i < CACHE_IMPORTERS; i++) {
        size_t header = CACHE_HEADERS + i * 0x400;
        size_t path = 0x11000 + i * 64;
        cache_u64(b, 0x240 + i * 32, CACHE_UNSLID + header);
        cache_u32(b, 0x258 + i * 32, (uint32_t)path);
        snprintf((char *)b + path, 64, "/usr/lib/bench%u.dylib", i);
        cache_u32(b, header, HK_MH_MAGIC_64);
        cache_u32(b, header + 4, 0x0100000c);
        cache_u32(b, header + 8, 2);
        cache_u32(b, header + 12, 6);
        cache_u32(b, header + 16, 1);
        cache_u32(b, header + 20, HK_SEGMENT_COMMAND_64_SIZE);
        cache_u32(b, header + 32, HK_LC_SEGMENT_64);
        cache_u32(b, header + 36, HK_SEGMENT_COMMAND_64_SIZE);
        memcpy(b + header + 40, "__DATA", 6);
        cache_u64(b, header + 56, CACHE_UNSLID + header);
        cache_u64(b, header + 64, 0x400);
        cache_u64(b, header + 72, header);
        cache_u64(b, header + 80, 0x400);
        cache_u32(b, header + 88, 3);
        cache_u32(b, header + 92, 3);
        cache_u32(b, CACHE_CLIENTS + i * 12, i);
        cache_u32(b, CACHE_CLIENTS + i * 12 + 4, i * CACHE_CLIENT_ENTRIES);
        cache_u32(b, CACHE_CLIENTS + i * 12 + 8, CACHE_CLIENT_ENTRIES);
        // Entry 0 is _bench_target; entries 1..CACHE_SYMBOLS are the symbols,
        // each pointing at its own location so emitted addresses stay distinct.
        size_t entry = CACHE_CLIENT_EXPORTS + (size_t)i * CACHE_CLIENT_ENTRIES * 12;
        cache_u32(b, entry, CACHE_EXPORTS - 1);
        cache_u32(b, entry + 8, 1);
        for (unsigned k = 0; k < CACHE_SYMBOLS; k++) {
            size_t symbol_entry = entry + (size_t)(k + 1) * 12;
            cache_u32(b, symbol_entry, CACHE_SYMBOL_EXPORT_BASE + k);
            cache_u32(b, symbol_entry + 4, k + 1);
            cache_u32(b, symbol_entry + 8, 1);
        }
    }
    cache_u32(b, CACHE_PATCH, 4);
    cache_pair(b, CACHE_PATCH + 8, CACHE_IMAGES, CACHE_IMAGE_COUNT);
    cache_pair(b, CACHE_PATCH + 24, CACHE_EXPORT_TABLE, CACHE_EXPORTS);
    cache_pair(b, CACHE_PATCH + 40, CACHE_CLIENTS, CACHE_IMPORTERS);
    cache_pair(b, CACHE_PATCH + 56, CACHE_CLIENT_EXPORTS,
               CACHE_IMPORTERS * CACHE_CLIENT_ENTRIES);
    cache_pair(b, CACHE_PATCH + 72, CACHE_LOCATIONS, CACHE_CLIENT_ENTRIES);
    cache_pair(b, CACHE_PATCH + 88, CACHE_NAMES, CACHE_EXPORTS * 32);
    cache_pair(b, CACHE_PATCH + 104, CACHE_GOT_CLIENTS, CACHE_IMAGE_COUNT);
    cache_pair(b, CACHE_PATCH + 120, CACHE_GOT_CLIENTS, 0);
    cache_pair(b, CACHE_PATCH + 136, CACHE_GOT_CLIENTS, 0);
    cache_u32(b, CACHE_IMAGES + (CACHE_IMAGE_COUNT - 1) * 16 + 4, CACHE_IMPORTERS);
    cache_u32(b, CACHE_IMAGES + (CACHE_IMAGE_COUNT - 1) * 16 + 12, CACHE_EXPORTS);
    cache_u32(b, CACHE_LOCATIONS, 0x200);
    for (unsigned i = 0; i < CACHE_EXPORTS; i++) {
        cache_u32(b, CACHE_EXPORT_TABLE + i * 8 + 4, i * 32);
        snprintf((char *)b + CACHE_NAMES + i * 32, 32, "_irrelevant_export_%u", i);
    }
    for (unsigned k = 0; k < CACHE_SYMBOLS; k++) {
        cache_u32(b, CACHE_LOCATIONS + (k + 1) * CACHE_SYMBOL_LOCATION_STRIDE,
                  CACHE_SYMBOL_LOCATION_BASE + k * CACHE_SYMBOL_LOCATION_STRIDE);
        snprintf((char *)b + CACHE_NAMES + (CACHE_SYMBOL_EXPORT_BASE + k) * 32, 32,
                 "_bench_sym_%02u", k);
    }
    strcpy((char *)b + CACHE_NAMES + (CACHE_EXPORTS - 1) * 32, "_bench_target");
    return b;
}

typedef struct { size_t count; uintptr_t address; } cache_result_t;
static bool cache_collect(void *ctx, const hk_cache_patch_site_t *site) {
    cache_result_t *result = ctx;
    result->count++;
    result->address = site->address;
    assert(!site->schema.authenticated && !site->weak_import &&
           !site->shared_got && site->addend == 0);
    return true;
}
static hk_cache_patch_target_t cache_target(uint8_t *b, unsigned importer) {
    size_t header = CACHE_HEADERS + (size_t)importer * 0x400;
    // Synthetic ranges are caller-supplied, so immutable_metadata stays false:
    // only HKRebindEngine opts a live shared-cache range into the metadata index.
    hk_cache_patch_target_t target = {
        .cache_base = b, .cache_size = CACHE_BYTES,
        .image_header = b + header,
        .image_header_size = HK_MACHO_HEADER_64_SIZE + HK_SEGMENT_COMMAND_64_SIZE,
        .image_slide = (uintptr_t)b - (uintptr_t)CACHE_UNSLID,
        .image_path = (char *)b + 0x11000 + importer * 64,
    };
    return target;
}
static void bench_cache_traversal(uint8_t *b, bool cached) {
    hk_cache_patch_lookup_t *lookup = cached
        ? hk_cache_patch_lookup_create("bench_target", HK_SYMBOL_NAME_C) : NULL;
    assert(!cached || lookup);
    for (unsigned i = 0; i < CACHE_IMPORTERS; i++) {
        size_t header = CACHE_HEADERS + i * 0x400;
        hk_cache_patch_target_t target = cache_target(b, i);
        cache_result_t result = {0};
        hk_cache_patch_status_t status = cached
            ? hk_dyld_cache_iterate_symbol_uses_with_lookup(&target, "bench_target",
                HK_SYMBOL_NAME_C, cache_collect, &result, lookup)
            : hk_dyld_cache_iterate_symbol_uses(&target, "bench_target",
                HK_SYMBOL_NAME_C, cache_collect, &result);
        assert(status == HK_CACHE_PATCH_OK);
        assert(result.count == 1 && result.address == (uintptr_t)b + header + 0x200);
    }
    hk_cache_patch_lookup_destroy(lookup);
}
static void bench_cache_uncached(void *ctx) { bench_cache_traversal(ctx, false); }
static void bench_cache_lookup(void *ctx) { bench_cache_traversal(ctx, true); }

// ---- multi-symbol preparation: per-symbol lookup vs immutable metadata index
// A sample resolves CACHE_SYMBOLS symbols across CACHE_IMPORTERS importers, one
// symbol-scoped lookup per symbol (the production shape). The `scan` variant
// leaves immutable_metadata false and pays one full metadata scan per symbol;
// the `index` variant opts in and reuses the process-lifetime index, whose
// build lands in warmup.
typedef struct {
    hk_cache_patch_status_t status[CACHE_SYMBOLS][CACHE_IMPORTERS];
    size_t count[CACHE_SYMBOLS][CACHE_IMPORTERS];
    uintptr_t address[CACHE_SYMBOLS][CACHE_IMPORTERS];
} resolve_matrix_t;

static resolve_matrix_t g_scan_results, g_index_results;

typedef struct {
    uint8_t *cache;
    unsigned present_symbols;
    bool immutable_metadata;
    resolve_matrix_t *results;
} resolve_bench_t;

static void resolve_symbols(uint8_t *b, bool immutable_metadata,
                            unsigned present_symbols, resolve_matrix_t *out) {
    for (unsigned k = 0; k < CACHE_SYMBOLS; k++) {
        char name[32];
        snprintf(name, sizeof(name),
                 k < present_symbols ? "bench_sym_%02u" : "bench_gone_%02u", k);
        hk_cache_patch_lookup_t *lookup =
            hk_cache_patch_lookup_create(name, HK_SYMBOL_NAME_C);
        assert(lookup);
        for (unsigned i = 0; i < CACHE_IMPORTERS; i++) {
            hk_cache_patch_target_t target = cache_target(b, i);
            target.immutable_metadata = immutable_metadata;
            cache_result_t result = {0};
            out->status[k][i] = hk_dyld_cache_iterate_symbol_uses_with_lookup(
                &target, name, HK_SYMBOL_NAME_C, cache_collect, &result, lookup);
            out->count[k][i] = result.count;
            out->address[k][i] = result.address;
        }
        hk_cache_patch_lookup_destroy(lookup);
    }
}

static void bench_resolve_symbols(void *ctx) {
    const resolve_bench_t *bench = ctx;
    resolve_symbols(bench->cache, bench->immutable_metadata,
                    bench->present_symbols, bench->results);
}

static void verify_matrix(uint8_t *b, const resolve_matrix_t *m,
                          unsigned present_symbols) {
    for (unsigned k = 0; k < CACHE_SYMBOLS; k++) {
        for (unsigned i = 0; i < CACHE_IMPORTERS; i++) {
            size_t header = CACHE_HEADERS + (size_t)i * 0x400;
            if (k < present_symbols) {
                assert(m->status[k][i] == HK_CACHE_PATCH_OK);
                assert(m->count[k][i] == 1);
                assert(m->address[k][i] == (uintptr_t)b + header +
                       CACHE_SYMBOL_LOCATION_BASE + k * CACHE_SYMBOL_LOCATION_STRIDE);
            } else {
                assert(m->status[k][i] == HK_CACHE_PATCH_NOT_FOUND);
                assert(m->count[k][i] == 0);
            }
        }
    }
}

// The indexed path must emit exactly what the per-symbol scan emits.
static void assert_matching_results(const resolve_matrix_t *scan,
                                    const resolve_matrix_t *index) {
    for (unsigned k = 0; k < CACHE_SYMBOLS; k++) {
        for (unsigned i = 0; i < CACHE_IMPORTERS; i++) {
            assert(scan->status[k][i] == index->status[k][i]);
            assert(scan->count[k][i] == index->count[k][i]);
            assert(scan->address[k][i] == index->address[k][i]);
        }
    }
}

// Fails on any result/count mismatch between the two paths, or between either
// path and the layout make_patch_cache() wrote.
static void check_scenario(uint8_t *b, unsigned present_symbols) {
    resolve_symbols(b, false, present_symbols, &g_scan_results);
    resolve_symbols(b, true, present_symbols, &g_index_results);
    assert_matching_results(&g_scan_results, &g_index_results);
    verify_matrix(b, &g_scan_results, present_symbols);
}

int main(int argc, char **argv){
    size_t iters=50000;
    size_t warmup=200;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--iters")==0 && i+1<argc) iters=(size_t)atoi(argv[++i]);
        if(strcmp(argv[i],"--warmup")==0 && i+1<argc) warmup=(size_t)atoi(argv[++i]);
    }
    init_fake_macho();
    hk_image_catalog_t *cat100 = make_catalog(100);
    hk_image_catalog_t *cat1000 = make_catalog(1000);

    hk_bench_run("export_trie_hit", bench_export_hit, NULL, warmup, iters, 1);
    hk_bench_run("export_trie_miss", bench_export_miss, NULL, warmup, iters, 1);
    hk_bench_run("symbol_candidates", bench_symbol_candidates, NULL, warmup, iters, 1);
    hk_bench_run("macho_peek", bench_macho_peek, NULL, warmup, iters, 1);
    hk_bench_run("catalog_match_100", bench_catalog_100, cat100, warmup, iters, 1);
    hk_bench_run("catalog_match_1000", bench_catalog_100, cat1000, warmup, iters/10, 1);

    uint8_t *cache = make_patch_cache();
    size_t cache_iters = iters < 30 ? iters : 30;
    size_t cache_warmup = warmup < 2 ? warmup : 2;
    hk_bench_run("cache_64_importers_uncached", bench_cache_uncached, cache,
                 cache_warmup, cache_iters, 1);
    hk_bench_run("cache_64_importers_lookup", bench_cache_lookup, cache,
                 cache_warmup, cache_iters, 1);

    // Two opt-in scenarios over the same cache: fully populated (32 symbols)
    // and missing-heavy (8 of the 32 queried names exist). Labels are stable;
    // medians are ns per (symbol, importer) preparation, so
    // speedup = scan median / index median for the same scenario.
    int multi_ops = CACHE_SYMBOLS * CACHE_IMPORTERS;
    static const struct { const char *label; unsigned present; } multi_scenarios[] = {
        {"cache_32sym_64imp", CACHE_SYMBOLS},
        {"cache_32sym_64imp_missheavy", CACHE_PRESENT_SYMBOLS},
    };
    for (size_t s = 0; s < sizeof(multi_scenarios) / sizeof(multi_scenarios[0]); s++) {
        unsigned present = multi_scenarios[s].present;
        printf("# %s: %d preparations/sample, %u of %d symbols present\n",
               multi_scenarios[s].label, multi_ops, present, (int)CACHE_SYMBOLS);
        check_scenario(cache, present);
        resolve_bench_t bench = {
            .cache = cache, .present_symbols = present, .results = &g_scan_results,
        };
        char label[64];
        snprintf(label, sizeof(label), "%s_scan", multi_scenarios[s].label);
        hk_bench_run(label, bench_resolve_symbols, &bench, cache_warmup, cache_iters,
                     multi_ops);
        bench.immutable_metadata = true;
        bench.results = &g_index_results;
        snprintf(label, sizeof(label), "%s_index", multi_scenarios[s].label);
        hk_bench_run(label, bench_resolve_symbols, &bench, cache_warmup, cache_iters,
                     multi_ops);
        // Timed runs must emit exactly what the verified runs emitted, without
        // re-running resolve_symbols and clobbering the timed matrices.
        assert_matching_results(&g_scan_results, &g_index_results);
        verify_matrix(cache, &g_scan_results, present);
    }
    free(cache);

    hk_image_catalog_destroy(cat100);
    hk_image_catalog_destroy(cat1000);
    return 0;
}
