#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/resolvers/HKDyldCachePatches.h"
#include "../../src/resolvers/HKMachO.h"

#define CACHE_SIZE 0x3000u
// Deliberately above ordinary host heap addresses: dyld slides are signed,
// so this forces the negative-slide translation path on every CI host.
#define UNSLID UINT64_C(0x0000700000000000)
#define IMPORTER_OFF 0x800u
#define SLOT0_OFF 0xA00u
#define SLOT1_OFF 0xA08u
#define GOT_OFF 0xC00u
#define PATCH_OFF 0x1000u

static void put_u32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static void put_u64(uint8_t *b, size_t o, uint64_t v) { memcpy(b + o, &v, 8); }

static uint32_t old_bits(bool auth, bool addr_div, uint8_t key,
                         uint16_t diversity, uint8_t addend) {
    return ((uint32_t)addend << 7) | ((uint32_t)auth << 12) |
           ((uint32_t)addr_div << 13) | ((uint32_t)key << 14) |
           ((uint32_t)diversity << 16);
}

static uint32_t v4_bits(bool auth, bool weak, bool addr_div, bool data_key,
                        uint16_t diversity, uint32_t addend) {
    uint32_t bits = (uint32_t)auth | ((uint32_t)weak << 8) |
                    (addend << 9);
    if (auth) {
        bits |= ((uint32_t)addr_div << 14) |
                ((uint32_t)data_key << 15) |
                ((uint32_t)diversity << 16);
    }
    return bits;
}

static void build_importer(uint8_t *cache) {
    uint8_t *header = cache + IMPORTER_OFF;
    put_u32(header, 0, HK_MH_MAGIC_64);
    put_u32(header, 4, 0x0100000c);
    put_u32(header, 8, 2);
    put_u32(header, 12, 6);
    put_u32(header, 16, 1);
    put_u32(header, 20, HK_SEGMENT_COMMAND_64_SIZE);
    put_u32(header, 32, HK_LC_SEGMENT_64);
    put_u32(header, 36, HK_SEGMENT_COMMAND_64_SIZE);
    memcpy(header + 40, "__DATA", 6);
    put_u64(header, 56, UNSLID + IMPORTER_OFF);
    put_u64(header, 64, 0x400);
    put_u64(header, 72, IMPORTER_OFF);
    put_u64(header, 80, 0x400);
    put_u32(header, 88, 3);
    put_u32(header, 92, 3);
}

static void build_cache_header(uint8_t *cache, uint32_t version) {
    uint32_t mapping_offset = version == 1 ? 0x180u : 0x200u;
    put_u32(cache, 16, mapping_offset);
    put_u32(cache, 20, 1);
    put_u64(cache, 152, UNSLID + PATCH_OFF);
    put_u64(cache, 160, 0x400);
    put_u32(cache, 220, 1u << 11);
    put_u64(cache, mapping_offset, UNSLID);
    put_u64(cache, mapping_offset + 8, CACHE_SIZE);
    put_u64(cache, mapping_offset + 16, 0);
    if (version == 1) {
        put_u32(cache, 24, 0x240);
        put_u32(cache, 28, 2);
    } else {
        put_u32(cache, 448, 0x240);
        put_u32(cache, 452, 2);
    }
    put_u64(cache, 0x240, UNSLID + 0x600);
    put_u32(cache, 0x258, 0x300);
    put_u64(cache, 0x260, UNSLID + IMPORTER_OFF);
    put_u32(cache, 0x278, 0x320);
    memcpy(cache + 0x300, "/usr/lib/def.dylib", 19);
    memcpy(cache + 0x320, "/usr/lib/importer.dylib", 24);
    build_importer(cache);
}

static void build_v1(uint8_t *cache) {
    uint8_t *p = cache + PATCH_OFF;
    put_u64(p, 0, UNSLID + 0x1080);
    put_u64(p, 8, 2);
    put_u64(p, 16, UNSLID + 0x1090);
    put_u64(p, 24, 1);
    put_u64(p, 32, UNSLID + 0x10A0);
    put_u64(p, 40, 2);
    put_u64(p, 48, UNSLID + 0x10C0);
    put_u64(p, 56, 5);
    put_u32(cache, 0x1080, 0);
    put_u32(cache, 0x1084, 1);
    put_u32(cache, 0x1090, 0x100);
    put_u32(cache, 0x1094, 0);
    put_u32(cache, 0x1098, 2);
    put_u32(cache, 0x109C, 0);
    put_u32(cache, 0x10A0, SLOT0_OFF);
    put_u32(cache, 0x10A8, old_bits(true, true, HK_PAC_KEY_IA, 0x1111, 5));
    put_u32(cache, 0x10B0, SLOT1_OFF);
    put_u32(cache, 0x10B8, old_bits(false, false, 0, 0, 7));
    memcpy(cache + 0x10C0, "_foo", 5);
}

static void pair(uint8_t *p, size_t off, uint64_t address, uint64_t count) {
    put_u64(p, off, address);
    put_u64(p, off + 8, count);
}

static void build_v2_v4(uint8_t *cache, uint32_t version) {
    uint8_t *p = cache + PATCH_OFF;
    put_u32(p, 0, version);
    put_u32(p, 4, 0);
    pair(p, 8, UNSLID + 0x1100, 2);
    pair(p, 24, UNSLID + 0x1120, 1);
    pair(p, 40, UNSLID + 0x1128, 1);
    pair(p, 56, UNSLID + 0x1138, 1);
    pair(p, 72, UNSLID + 0x1148, 2);
    pair(p, 88, UNSLID + 0x1160, 5);
    if (version >= 3) {
        pair(p, 104, UNSLID + 0x1170, 2);
        pair(p, 120, UNSLID + 0x1180, 1);
        pair(p, 136, UNSLID + 0x1190, 1);
    }
    put_u32(cache, 0x1100, 0);
    put_u32(cache, 0x1104, 1);
    put_u32(cache, 0x1108, 0);
    put_u32(cache, 0x110C, 1);
    put_u32(cache, 0x1120, 0x100);
    put_u32(cache, 0x1124, 0);
    put_u32(cache, 0x1128, 1);
    put_u32(cache, 0x112C, 0);
    put_u32(cache, 0x1130, 1);
    put_u32(cache, 0x1138, 0);
    put_u32(cache, 0x113C, 0);
    put_u32(cache, 0x1140, 2);
    put_u32(cache, 0x1148, 0x200);
    put_u32(cache, 0x114C, version == 4
        ? v4_bits(true, true, true, false, 0x1234, 5)
        : old_bits(true, true, HK_PAC_KEY_IB, 0x1234, 5));
    put_u32(cache, 0x1150, 0x208);
    put_u32(cache, 0x1154, version == 4
        ? v4_bits(false, false, false, false, 0, 0x12345)
        : old_bits(false, false, 0, 0, 7));
    memcpy(cache + 0x1160, "_foo", 5);
    if (version >= 3) {
        put_u32(cache, 0x1170, 0);
        put_u32(cache, 0x1174, 1);
        put_u32(cache, 0x1180, 0);
        put_u32(cache, 0x1184, 0);
        put_u32(cache, 0x1188, 1);
        put_u64(cache, 0x1190, GOT_OFF);
        put_u32(cache, 0x1198, version == 4
            ? v4_bits(true, false, false, true, 0x4321, 3)
            : old_bits(true, false, HK_PAC_KEY_DA, 0x4321, 3));
    }
}

static void build_cache(uint8_t *cache, uint32_t version) {
    memset(cache, 0, CACHE_SIZE);
    build_cache_header(cache, version);
    if (version == 1) build_v1(cache);
    else build_v2_v4(cache, version);
}

typedef struct {
    hk_cache_patch_site_t sites[4];
    size_t count;
} collected_t;

static bool collect(void *opaque, const hk_cache_patch_site_t *site) {
    collected_t *result = opaque;
    assert(result->count < 4);
    result->sites[result->count++] = *site;
    return true;
}

static hk_cache_patch_target_t target_for(uint8_t *cache, bool include_got) {
    hk_cache_patch_target_t target;
    memset(&target, 0, sizeof(target));
    target.cache_base = cache;
    target.cache_size = CACHE_SIZE;
    target.image_header = cache + IMPORTER_OFF;
    target.image_header_size = HK_MACHO_HEADER_64_SIZE + HK_SEGMENT_COMMAND_64_SIZE;
    target.image_slide = (uintptr_t)cache - (uintptr_t)UNSLID;
    target.image_path = "/usr/lib/importer.dylib";
    target.include_shared_got = include_got;
    return target;
}


// Both images import duplicate exports of the same name. Keep each client's
// locations relative to its own header, so accidental importer reuse fails.
static void build_multi_importer(uint8_t *cache, uint32_t version) {
    build_cache(cache, version);
    memcpy(cache + 0x600, cache + IMPORTER_OFF, 104);
    put_u64(cache, 0x600 + 56, UNSLID + 0x600);
    put_u64(cache, 0x600 + 72, 0x600);
    pair(cache + PATCH_OFF, 24, UNSLID + 0x1230, 2);
    pair(cache + PATCH_OFF, 40, UNSLID + 0x1200, 2);
    pair(cache + PATCH_OFF, 56, UNSLID + 0x1250, 2);
    put_u32(cache, 0x1104, 2);
    put_u32(cache, 0x110C, 2);
    for (uint32_t i = 0; i < 2; i++) {
        put_u32(cache, 0x1200 + i * 12, i);
        put_u32(cache, 0x1208 + i * 12, 2);
        put_u32(cache, 0x1230 + i * 8, 0x100 + i * 8);
        put_u32(cache, 0x1250 + i * 12, i);
        put_u32(cache, 0x1254 + i * 12, i);
        put_u32(cache, 0x1258 + i * 12, 1);
    }
}

static void compare_lookup(hk_cache_patch_target_t *target, const char *name,
                           hk_symbol_name_convention_t convention,
                           hk_cache_patch_lookup_t *lookup,
                           hk_cache_patch_status_t expected, size_t count) {
    collected_t plain = {0}, cached = {0};
    assert(hk_dyld_cache_iterate_symbol_uses(target, name, convention,
               collect, &plain) == expected);
    assert(hk_dyld_cache_iterate_symbol_uses_with_lookup(target, name, convention,
               collect, &cached, lookup) == expected);
    assert(plain.count == count && cached.count == count);
    assert(memcmp(plain.sites, cached.sites,
                  count * sizeof(plain.sites[0])) == 0);
}

static void test_lookup_reuse(uint8_t *cache) {
    for (uint32_t version = 2; version <= 4; version++) {
        build_multi_importer(cache, version);
        hk_cache_patch_target_t target = target_for(cache, true);
        hk_cache_patch_lookup_t *lookup = hk_cache_patch_lookup_create(
            "foo", HK_SYMBOL_NAME_C);
        assert(lookup);
        size_t count = version >= 3 ? 3 : 2;
        compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                       HK_CACHE_PATCH_OK, count);
        target.image_header = cache + 0x600;
        target.image_path = "/usr/lib/def.dylib";
        compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                       HK_CACHE_PATCH_OK, count);
        assert(hk_cache_patch_lookup_scan_count(lookup) == 1);
        // A context for a different convention/name must not return foo sites.
        compare_lookup(&target, "foo", HK_SYMBOL_NAME_MACHO_EXACT, lookup,
                       HK_CACHE_PATCH_NOT_FOUND, 0);
        compare_lookup(&target, "missing", HK_SYMBOL_NAME_C, lookup,
                       HK_CACHE_PATCH_NOT_FOUND, 0);
        target.image_path = "/wrong/path";
        compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                       HK_CACHE_PATCH_MALFORMED, 0);
        target.image_path = "/usr/lib/def.dylib";
        if (version >= 3) {
            target.include_shared_got = false;
            compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                           HK_CACHE_PATCH_SCOPE_UNREPRESENTABLE, 1);
        }
        hk_cache_patch_lookup_destroy(lookup);

        lookup = hk_cache_patch_lookup_create("missing", HK_SYMBOL_NAME_C);
        assert(lookup);
        for (unsigned i = 0; i < 3; i++)
            compare_lookup(&target, "missing", HK_SYMBOL_NAME_C, lookup,
                           HK_CACHE_PATCH_NOT_FOUND, 0);
        assert(hk_cache_patch_lookup_scan_count(lookup) == 1);
        hk_cache_patch_lookup_destroy(lookup);
    }
}


static void test_lookup_nonadjacent_definitions(uint8_t *cache) {
    for (uint32_t version = 2; version <= 4; version++) {
        build_multi_importer(cache, version);
        // Definitions 0 and 3 match; 0 has two matching exports. Definitions
        // 1 and 2 have none. A replay must exhaust 0 before jumping to 3.
        put_u32(cache, 452, 4);
        for (unsigned i = 2; i < 4; i++) {
            put_u64(cache, 0x240 + 32 * i, UNSLID + 0x400 + 0x100 * (i - 2));
            put_u32(cache, 0x258 + 32 * i, 0x300);
        }
        memcpy(cache + 0x1280, cache + 0x1100, 16);
        pair(cache + PATCH_OFF, 8, UNSLID + 0x1280, 4);
        put_u32(cache, 0x12B4, 2);
        put_u32(cache, 0x12B8, 2);
        put_u32(cache, 0x12BC, 1);
        pair(cache + PATCH_OFF, 24, UNSLID + 0x1230, 3);
        put_u32(cache, 0x1240, 0x110);
        pair(cache + PATCH_OFF, 56, UNSLID + 0x1250, 3);
        put_u32(cache, 0x1268, 2);
        put_u32(cache, 0x1270, 1);
        put_u32(cache, 0x1208, 3);
        put_u32(cache, 0x1214, 3);
        if (version >= 3) {
            pair(cache + PATCH_OFF, 104, UNSLID + 0x12C0, 4);
            put_u32(cache, 0x12C4, 1);
        }
        hk_cache_patch_target_t target = target_for(cache, true);
        hk_cache_patch_lookup_t *lookup = hk_cache_patch_lookup_create(
            "foo", HK_SYMBOL_NAME_C);
        assert(lookup);
        size_t count = version >= 3 ? 4 : 3;
        for (unsigned i = 0; i < 3; i++)
            compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                           HK_CACHE_PATCH_OK, count);
        assert(hk_cache_patch_lookup_scan_count(lookup) == 1);
        hk_cache_patch_lookup_destroy(lookup);

        lookup = hk_cache_patch_lookup_create("missing", HK_SYMBOL_NAME_C);
        assert(lookup);
        for (unsigned i = 0; i < 3; i++)
            compare_lookup(&target, "missing", HK_SYMBOL_NAME_C, lookup,
                           HK_CACHE_PATCH_NOT_FOUND, 0);
        assert(hk_cache_patch_lookup_scan_count(lookup) == 1);
        hk_cache_patch_lookup_destroy(lookup);

        // The first traversal still validates an unmatched definition row.
        put_u32(cache, 0x12A0, UINT32_MAX);
        lookup = hk_cache_patch_lookup_create("foo", HK_SYMBOL_NAME_C);
        assert(lookup);
        compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                       HK_CACHE_PATCH_MALFORMED, count - 1);
        hk_cache_patch_lookup_destroy(lookup);
        lookup = hk_cache_patch_lookup_create("missing", HK_SYMBOL_NAME_C);
        assert(lookup);
        compare_lookup(&target, "missing", HK_SYMBOL_NAME_C, lookup,
                       HK_CACHE_PATCH_MALFORMED, 0);
        hk_cache_patch_lookup_destroy(lookup);
    }
}

static void test_lookup_identity_and_failures(uint8_t *cache) {
    build_multi_importer(cache, 4);
    hk_cache_patch_target_t target = target_for(cache, true);
    hk_cache_patch_lookup_t *lookup = hk_cache_patch_lookup_create(
        "foo", HK_SYMBOL_NAME_C);
    assert(lookup);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup, HK_CACHE_PATCH_OK, 3);
    uint8_t *other = aligned_alloc(64, CACHE_SIZE);
    assert(other);
    build_multi_importer(other, 4);
    target = target_for(other, true);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup, HK_CACHE_PATCH_OK, 3);
    assert(hk_cache_patch_lookup_scan_count(lookup) == 2);
    hk_cache_patch_lookup_destroy(lookup);
    free(other);

    // New preparation must see changed names even at the same cache address.
    memcpy(cache + 0x1160, "_bar", 5);
    target = target_for(cache, true);
    lookup = hk_cache_patch_lookup_create("foo", HK_SYMBOL_NAME_C);
    assert(lookup);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                   HK_CACHE_PATCH_NOT_FOUND, 0);
    hk_cache_patch_lookup_destroy(lookup);

    // An error after a valid export must preserve already-emitted sites and
    // must not publish the partial match list for the next call.
    build_multi_importer(cache, 4);
    put_u32(cache, 0x123C, 0x10000000);
    lookup = hk_cache_patch_lookup_create("foo", HK_SYMBOL_NAME_C);
    assert(lookup);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                   HK_CACHE_PATCH_UNSUPPORTED, 2);
    put_u32(cache, 0x123C, 0x0FFFFFFF);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup,
                   HK_CACHE_PATCH_MALFORMED, 2);
    put_u32(cache, 0x123C, 0);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup, HK_CACHE_PATCH_OK, 3);
    hk_cache_patch_lookup_destroy(lookup);

    hk_cache_patch_lookup_fail_allocations_for_testing(true);
    assert(!hk_cache_patch_lookup_create("foo", HK_SYMBOL_NAME_C));
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, NULL, HK_CACHE_PATCH_OK, 3);
    hk_cache_patch_lookup_fail_allocations_for_testing(false);
    lookup = hk_cache_patch_lookup_create("foo", HK_SYMBOL_NAME_C);
    assert(lookup);
    hk_cache_patch_lookup_fail_allocations_for_testing(true);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup, HK_CACHE_PATCH_OK, 3);
    hk_cache_patch_lookup_fail_allocations_for_testing(false);
    compare_lookup(&target, "foo", HK_SYMBOL_NAME_C, lookup, HK_CACHE_PATCH_OK, 3);
    hk_cache_patch_lookup_destroy(lookup);
}

int main(void) {
    uint8_t *cache = aligned_alloc(64, CACHE_SIZE);
    assert(cache);
    for (uint32_t version = 1; version <= 4; version++) {
        build_cache(cache, version);
        hk_cache_patch_target_t target = target_for(cache, true);
        collected_t result = {0};
        hk_cache_patch_status_t status = hk_dyld_cache_iterate_symbol_uses(
            &target, "foo", HK_SYMBOL_NAME_C, collect, &result);
        if (status != HK_CACHE_PATCH_OK) {
            fprintf(stderr, "cache patch v%u failed with status %d\n",
                    version, status);
        }
        assert(status == HK_CACHE_PATCH_OK);
        size_t expected = version >= 3 ? 3 : 2;
        assert(result.count == expected);
        assert(result.sites[0].address == (uintptr_t)cache + SLOT0_OFF);
        assert(result.sites[0].schema.authenticated);
        assert(result.sites[0].addend == 5);
        assert(result.sites[1].address == (uintptr_t)cache + SLOT1_OFF);
        if (version == 4) {
            assert(result.sites[0].weak_import);
            assert(result.sites[1].addend == 0x12345);
        }
        if (version >= 3) {
            assert(result.sites[2].address == (uintptr_t)cache + GOT_OFF);
            assert(result.sites[2].shared_got);
            target.include_shared_got = false;
            result.count = 0;
            assert(hk_dyld_cache_iterate_symbol_uses(
                       &target, "foo", HK_SYMBOL_NAME_C, collect, &result) ==
                   HK_CACHE_PATCH_SCOPE_UNREPRESENTABLE);
        }
    }

    build_cache(cache, 4);
    put_u32(cache, PATCH_OFF, 99);
    hk_cache_patch_target_t target = target_for(cache, true);
    collected_t result = {0};
    assert(hk_dyld_cache_iterate_symbol_uses(
               &target, "foo", HK_SYMBOL_NAME_C, collect, &result) ==
           HK_CACHE_PATCH_UNSUPPORTED);
    build_cache(cache, 4);
    put_u32(cache, PATCH_OFF + 4, 1);
    assert(hk_dyld_cache_iterate_symbol_uses(
               &target, "foo", HK_SYMBOL_NAME_C, collect, &result) ==
           HK_CACHE_PATCH_MALFORMED);
    build_cache(cache, 4);
    put_u64(cache, 160, 32);
    assert(hk_dyld_cache_iterate_symbol_uses(
               &target, "foo", HK_SYMBOL_NAME_C, collect, &result) ==
           HK_CACHE_PATCH_MALFORMED);

    test_lookup_reuse(cache);
    test_lookup_nonadjacent_definitions(cache);
    test_lookup_identity_and_failures(cache);
    free(cache);
    printf("dyld shared-cache patch v1-v4 tests passed\n");
    return 0;
}
