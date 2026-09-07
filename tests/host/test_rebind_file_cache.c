// Host test for the rebind file-cache lifetime rule.
//
// prepare_file_chains serves file-backed chained-fixup images from a 4-entry
// cache whose fixups/slice borrow the entry's file mapping. Two threads must
// be able to prepare different images concurrently: a parse in progress pins
// its entry (the file-cache lock is held through the parse), and a miss
// parses its own mapping before publishing it, so no eviction can unmap a
// blob another thread is still reading.
//
// What this catches: if the lock were released before the parse (or the miss
// published before parsing), a concurrent prepare on enough other images
// evicts the entry and munmaps the blob mid-parse, crashing or misreporting
// sites. Nine images thrash both the 4-entry file cache and the 8-entry
// symbol cache, so every prepare below does real file parsing under constant
// eviction pressure. Fixtures mirror test_rebind_pac.c (same chained-fixup
// layout), but go through image_path + real temp files instead of the
// file_image seam, which bypasses the cache.

// _GNU_SOURCE for mkstemp and pthread_barrier_* under -std=c11.
#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/engines/HKRebindEngine.h"
#include "../../src/resolvers/HKChainedFixups.h"

#define CPU_ARM64 UINT32_C(0x0100000c)
#define CPU_SUBTYPE_ARM64E 2u
#define VM_BASE UINT64_C(0x100000000)
#define IMAGE_SIZE 0x700u
#define FILE_SIZE  0x600u
#define DATA_FILE_OFFSET 0x200u
#define SLOT_IMAGE_OFFSET 0x410u
#define FIXUPS_FILE_OFFSET 0x320u
#define FIXUPS_SIZE 85u
#define BASE_ORIGINAL UINT64_C(0x12345000)

#define N_IMAGES 9   // thrashes the 4-entry file cache and 8-entry symbol cache
#define N_THREADS 4
#define ROUNDS 60

static void put_u16(uint8_t *b, size_t o, uint16_t v) { memcpy(b + o, &v, 2); }
static void put_u32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static void put_u64(uint8_t *b, size_t o, uint64_t v) { memcpy(b + o, &v, 8); }

static void put_segment(uint8_t *b, size_t off, const char *name,
                        uint64_t vmaddr, uint64_t vmsize,
                        uint64_t fileoff, uint64_t filesize, uint32_t initprot) {
    put_u32(b, off, HK_LC_SEGMENT_64);
    put_u32(b, off + 4, HK_SEGMENT_COMMAND_64_SIZE);
    memcpy(b + off + 8, name, strlen(name));
    put_u64(b, off + 24, vmaddr);
    put_u64(b, off + 32, vmsize);
    put_u64(b, off + 40, fileoff);
    put_u64(b, off + 48, filesize);
    put_u32(b, off + 56, initprot);
    put_u32(b, off + 60, initprot);
}

static uint64_t auth_bind(uint32_t ordinal, uint16_t diversity,
                          bool addr_div, uint8_t key, uint32_t next) {
    return ordinal | ((uint64_t)diversity << 32) |
           ((uint64_t)addr_div << 48) | ((uint64_t)key << 49) |
           ((uint64_t)next << 51) | (UINT64_C(1) << 62) |
           (UINT64_C(1) << 63);
}

static void build_header(uint8_t *b, const uint8_t uuid[16]) {
    put_u32(b, 0, HK_MH_MAGIC_64);
    put_u32(b, 4, CPU_ARM64);
    put_u32(b, 8, CPU_SUBTYPE_ARM64E);
    put_u32(b, 12, 6);
    put_u32(b, 16, 6);
    put_u32(b, 20, 328);
    put_segment(b, 32, "__PAGEZERO", 0, VM_BASE, 0, 0, 0);
    put_segment(b, 104, "__TEXT", VM_BASE, 0x400, 0, 0x200, 5);
    put_segment(b, 176, "__DATA", VM_BASE + 0x400, 0x100,
                DATA_FILE_OFFSET, 0x100, 3);
    put_segment(b, 248, "__LINKEDIT", VM_BASE + 0x500, 0x200,
                0x300, 0x200, 1);
    put_u32(b, 320, HK_LC_UUID);
    put_u32(b, 324, 24);
    memcpy(b + 328, uuid, 16);
    put_u32(b, 344, HK_LC_DYLD_CHAINED_FIXUPS);
    put_u32(b, 348, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(b, 352, FIXUPS_FILE_OFFSET);
    put_u32(b, 356, FIXUPS_SIZE);
}

static void build_fixups(uint8_t *file, const char *symbol) {
    uint8_t *b = file + FIXUPS_FILE_OFFSET;
    memset(b, 0, FIXUPS_SIZE);
    put_u32(b, 0, 0);
    put_u32(b, 4, 28);
    put_u32(b, 8, 72);
    put_u32(b, 12, 80);
    put_u32(b, 16, 1);
    put_u32(b, 20, HK_CHAINED_IMPORT_ADDEND);
    put_u32(b, 24, 0);
    put_u32(b, 28, 4);
    put_u32(b, 32, 0);
    put_u32(b, 36, 0);
    put_u32(b, 40, 20);
    put_u32(b, 44, 0);
    put_u32(b, 48, 24);
    put_u16(b, 52, 0x100);
    put_u16(b, 54, HK_CHAINED_PTR_ARM64E_USERLAND24);
    put_u64(b, 56, 0x400);
    put_u16(b, 68, 1);
    put_u16(b, 70, 0x10);
    put_u32(b, 72, 1);
    put_u32(b, 76, 5);
    // Distinct symbol per image: a parse that reads another image's blob
    // (use-after-unmap landing on reused pages) finds no match and reports
    // NOT_FOUND instead of OK, so corruption is observable, not benign.
    size_t len = strlen(symbol) + 1;
    assert(80 + len <= FIXUPS_SIZE);
    memcpy(b + 80, symbol, len);

    put_u64(file, DATA_FILE_OFFSET + 0x10,
            auth_bind(0, 0x1111, true, HK_PAC_KEY_IA, 1));
    put_u64(file, DATA_FILE_OFFSET + 0x18,
            auth_bind(0, 0x2222, false, HK_PAC_KEY_DB, 0));
}

typedef struct {
    uint8_t *live;
    char path[64];
    char symbol[8];   // distinct per image, so cross-image reads misreport
    hk_rebind_target_t target;
} image_t;

static image_t g_images[N_IMAGES];
static pthread_barrier_t g_round_barrier;

static void make_image(image_t *img, unsigned index) {
    uint8_t uuid[16];
    memset(uuid, 0, sizeof(uuid));
    uuid[0] = (uint8_t)(0xA0 + index);   // distinct identity per image
    uuid[15] = (uint8_t)index;

    img->live = aligned_alloc(64, IMAGE_SIZE);
    assert(img->live);
    memset(img->live, 0, IMAGE_SIZE);
    build_header(img->live, uuid);

    // Pool stores the underscore form; HK_SYMBOL_NAME_C "sN" matches "_sN".
    snprintf(img->symbol, sizeof(img->symbol), "_s%u", index);
    uint8_t *file = aligned_alloc(64, FILE_SIZE);
    assert(file);
    memset(file, 0, FILE_SIZE);
    build_header(file, uuid);
    build_fixups(file, img->symbol);

    // The live slots hold signed values, exactly as dyld would leave them.
    hk_pac_schema_t first = {
        .authenticated = true, .key = HK_PAC_KEY_IA,
        .diversity = 0x1111, .address_diversity = true,
    };
    hk_pac_schema_t second = {
        .authenticated = true, .key = HK_PAC_KEY_DB,
        .diversity = 0x2222, .address_diversity = false,
    };
    put_u64(img->live, SLOT_IMAGE_OFFSET,
            hk_pac_sign_slot(BASE_ORIGINAL + 5, &first,
                             (uintptr_t)img->live + SLOT_IMAGE_OFFSET));
    put_u64(img->live, SLOT_IMAGE_OFFSET + 8,
            hk_pac_sign_slot(BASE_ORIGINAL + 5, &second,
                             (uintptr_t)img->live + SLOT_IMAGE_OFFSET + 8));

    snprintf(img->path, sizeof(img->path), "/tmp/hk-rebind-cache-%d-XXXXXX",
             (int)index);
    int fd = mkstemp(img->path);
    assert(fd >= 0);
    assert(write(fd, file, FILE_SIZE) == (ssize_t)FILE_SIZE);
    close(fd);
    free(file);

    // NOTE: no file_image seam -- image_path forces the cached file path.
    memset(&img->target, 0, sizeof(img->target));
    img->target.image_base = img->live;
    img->target.image_size = IMAGE_SIZE;
    img->target.slide = (uintptr_t)img->live - (uintptr_t)VM_BASE;
    img->target.image_path = img->path;
}

static void check_prepare(image_t *img) {
    hk_rebind_plan_t plan;
    // Strip the underscore: HK_SYMBOL_NAME_C expands "sN" to "_sN".
    hk_rebind_status_t st =
        hk_rebind_prepare(&img->target, img->symbol + 1, HK_SYMBOL_NAME_C, &plan);
    assert(st == HK_REBIND_OK);
    assert(plan.count == 2 && plan.originals_agree);
    assert(plan.sites[0].address == (uintptr_t)img->live + SLOT_IMAGE_OFFSET);
    assert(plan.sites[1].address == (uintptr_t)img->live + SLOT_IMAGE_OFFSET + 8);
    assert(hk_pac_strip_code(plan.original) == BASE_ORIGINAL);
    assert(plan.sites[0].addend == 5 && plan.sites[1].addend == 5);
}

static void *worker(void *opaque) {
    unsigned seed = (unsigned)(uintptr_t)opaque;
    for (unsigned round = 0; round < ROUNDS; round++) {
        // Line up the threads so parses and evictions overlap as much as a
        // test can arrange: while one thread parses image i, the others are
        // filling the 4-entry cache with the remaining images.
        pthread_barrier_wait(&g_round_barrier);
        for (unsigned k = 0; k < N_IMAGES; k++) {
            check_prepare(&g_images[(seed + k) % N_IMAGES]);
        }
    }
    return NULL;
}

#ifdef HK_REBIND_TEST
// Test-only seam into the engine (compiled in only with HK_REBIND_TEST).
extern void (*hk_rebind_parse_hook_for_testing)(bool hit, const char *path,
                                                const void *base);
extern bool hk_rebind_file_cache_locked_for_testing(void);
extern bool hk_rebind_file_cache_contains_for_testing(const char *path,
                                                      const void *base);
extern void hk_rebind_symbol_cache_clear_for_testing(void);

static struct {
    bool hit_seen, hit_locked, miss_seen, miss_cached;
} g_probe;

static void probe_hook(bool hit, const char *path, const void *base) {
    if (hit) {
        g_probe.hit_seen = true;
        g_probe.hit_locked = hk_rebind_file_cache_locked_for_testing();
    } else {
        g_probe.miss_seen = true;
        g_probe.miss_cached =
            hk_rebind_file_cache_contains_for_testing(path, base);
    }
}

// A miss parse must run against the thread's own mapping, before the entry
// is published: otherwise a concurrent prepare can evict the entry and unmap
// the blob mid-parse.
static void test_miss_parses_before_publish(void) {
    memset(&g_probe, 0, sizeof(g_probe));
    hk_rebind_parse_hook_for_testing = probe_hook;
    check_prepare(&g_images[0]);   // first touch: miss path
    hk_rebind_parse_hook_for_testing = NULL;
    assert(g_probe.miss_seen && !g_probe.hit_seen);
    assert(!g_probe.miss_cached);
    printf("  miss-parses-before-publish: PASS\n");
}

// A file-cache hit parse borrows the entry's mapping, so the file-cache lock
// must be held through it: otherwise a concurrent miss evicts and unmaps the
// entry mid-parse.
static void test_hit_parses_under_lock(void) {
    // Symbol cache is larger (8) than the file cache (4), so clear it to
    // arrange a file hit with a symbol miss -- the path that parses.
    hk_rebind_symbol_cache_clear_for_testing();
    memset(&g_probe, 0, sizeof(g_probe));
    hk_rebind_parse_hook_for_testing = probe_hook;
    check_prepare(&g_images[0]);   // file-cache hit path
    hk_rebind_parse_hook_for_testing = NULL;
    assert(g_probe.hit_seen && !g_probe.miss_seen);
    assert(g_probe.hit_locked);
    printf("  hit-parses-under-lock: PASS\n");
}
#endif

int main(void) {
    for (unsigned i = 0; i < N_IMAGES; i++) {
        make_image(&g_images[i], i);
    }
#ifdef HK_REBIND_TEST
    // Mechanism checks first, while the caches hold only image 0's entries.
    test_miss_parses_before_publish();
    test_hit_parses_under_lock();
#endif
    // Warm up once single-threaded so failures below are concurrency bugs,
    // not fixture bugs.
    for (unsigned i = 0; i < N_IMAGES; i++) {
        check_prepare(&g_images[i]);
    }
    printf("  single-threaded-warmup: PASS\n");

    assert(pthread_barrier_init(&g_round_barrier, NULL, N_THREADS) == 0);
    pthread_t threads[N_THREADS];
    for (unsigned i = 0; i < N_THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, worker,
                              (void *)(uintptr_t)(i * 3 + 1)) == 0);
    }
    for (unsigned i = 0; i < N_THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    pthread_barrier_destroy(&g_round_barrier);

    for (unsigned i = 0; i < N_IMAGES; i++) {
        unlink(g_images[i].path);
        free(g_images[i].live);
    }
    printf("  concurrent-prepare-under-eviction: PASS (%u threads x %u rounds x %u images)\n",
           N_THREADS, ROUNDS, N_IMAGES);
    printf("all rebind file-cache tests passed\n");
    return 0;
}
