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

    hk_image_catalog_destroy(cat100);
    hk_image_catalog_destroy(cat1000);
    return 0;
}
