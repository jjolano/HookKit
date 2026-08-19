// Host test for Sources/Core/HKImageScope.c.
//
// The catalog is populated with hk_image_catalog_add_entry pointing at
// synthetic Mach-O buffers, which is exactly how it will be populated on
// device once hk_image_catalog_populate_from_dyld exists -- the entries are
// real, only their provenance differs. So everything this file checks is the
// real decision path, not a stand-in for it.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Core/HKImageScope.h"
#include "../../Sources/Resolvers/HKMachO.h"

// ---- synthetic image ----------------------------------------------------
// A minimal Mach-O with one __TEXT segment, which is all a span needs.

#define IMG_SIZE 0x200u
#define HDR      32u
#define SEG_CMD  72u   // sizeof(segment_command_64)

static void put_u32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, sizeof(v)); }
static void put_u64(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, sizeof(v)); }

// Builds an image whose __TEXT is [vmaddr, vmaddr+vmsize).
static uint8_t *build_image(uint64_t vmaddr, uint64_t vmsize) {
    uint8_t *b = calloc(1, IMG_SIZE);
    assert(b);
    put_u32(b, 0, 0xFEEDFACFu);   // MH_MAGIC_64
    put_u32(b, 16, 1);            // ncmds
    put_u32(b, 20, SEG_CMD);      // sizeofcmds

    put_u32(b, HDR + 0, 0x19u);   // LC_SEGMENT_64
    put_u32(b, HDR + 4, SEG_CMD);
    memcpy(b + HDR + 8, "__TEXT", 6);
    put_u64(b, HDR + 24, vmaddr);
    put_u64(b, HDR + 32, vmsize);
    return b;
}

static const uint8_t UUID_A[16] = {0xAA, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t UUID_B[16] = {0xBB, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static hk_image_entry_t entry_for(const char *path, const void *header,
                                  uintptr_t slide, const uint8_t *uuid) {
    hk_image_entry_t e;
    memset(&e, 0, sizeof(e));
    e.path = path;
    e.header = header;
    e.slide = slide;
    if (uuid) {
        e.uuid_present = true;
        memcpy(e.uuid, uuid, 16);
    }
    return e;
}

static hk_image_selector_t selector_path(const char *path) {
    hk_image_selector_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = sizeof(s);
    s.struct_version = HK_ABI_VERSION_3_0;
    s.kind = HK_IMAGE_EXACT_PATH;
    s.path = path;
    return s;
}

// ---- tests --------------------------------------------------------------

static void test_address_inside_and_outside(void) {
    const uint64_t base = 0x100000000ull;
    const uintptr_t slide = 0x4000;
    uint8_t *img = build_image(base, 0x2000);

    hk_image_catalog_t *cat = hk_image_catalog_create();
    hk_image_entry_t e = entry_for("/usr/lib/libfoo.dylib", img, slide, UUID_A);
    assert(hk_image_catalog_add_entry(cat, &e));

    hk_image_selector_t sel = selector_path("/usr/lib/libfoo.dylib");
    const uintptr_t start = (uintptr_t)base + slide;
    const uintptr_t end = (uintptr_t)(base + 0x2000) + slide;

    // Inside: first byte, middle, last byte.
    assert(hk_image_scope_check(cat, &sel, false, NULL, start) == HK_IMAGE_SCOPE_OK);
    assert(hk_image_scope_check(cat, &sel, false, NULL, start + 0x1000) == HK_IMAGE_SCOPE_OK);
    assert(hk_image_scope_check(cat, &sel, false, NULL, end - 1) == HK_IMAGE_SCOPE_OK);

    // Outside: one before the start, and the end itself (half-open range).
    assert(hk_image_scope_check(cat, &sel, false, NULL, start - 1)
           == HK_IMAGE_SCOPE_ADDRESS_OUTSIDE);
    assert(hk_image_scope_check(cat, &sel, false, NULL, end)
           == HK_IMAGE_SCOPE_ADDRESS_OUTSIDE);

    // The unslid address is OUTSIDE -- which is the slide-change case this
    // whole check exists for. An implementation that ignored the slide would
    // accept it.
    assert(hk_image_scope_check(cat, &sel, false, NULL, (uintptr_t)base)
           == HK_IMAGE_SCOPE_ADDRESS_OUTSIDE);

    hk_image_catalog_destroy(cat);
    free(img);
    printf("  address-inside-and-outside: PASS\n");
}

static void test_uuid_distinguishes_builds(void) {
    const uint64_t base = 0x100000000ull;
    uint8_t *img = build_image(base, 0x2000);

    hk_image_catalog_t *cat = hk_image_catalog_create();
    hk_image_entry_t e = entry_for("/usr/lib/libfoo.dylib", img, 0, UUID_A);
    assert(hk_image_catalog_add_entry(cat, &e));
    hk_image_selector_t sel = selector_path("/usr/lib/libfoo.dylib");
    const uintptr_t inside = (uintptr_t)base + 0x100;

    // Right build.
    assert(hk_image_scope_check(cat, &sel, true, UUID_A, inside) == HK_IMAGE_SCOPE_OK);
    // Same path, different build: the image IS loaded, so this is not
    // NO_MATCH -- the status has to say which stage failed.
    assert(hk_image_scope_check(cat, &sel, true, UUID_B, inside)
           == HK_IMAGE_SCOPE_UUID_MISMATCH);
    // No UUID requirement: the same address passes.
    assert(hk_image_scope_check(cat, &sel, false, NULL, inside) == HK_IMAGE_SCOPE_OK);

    // An entry with no UUID recorded cannot satisfy a UUID requirement:
    // "unknown" is not the build that was asked for.
    hk_image_catalog_t *cat2 = hk_image_catalog_create();
    hk_image_entry_t anon = entry_for("/usr/lib/libfoo.dylib", img, 0, NULL);
    assert(hk_image_catalog_add_entry(cat2, &anon));
    assert(hk_image_scope_check(cat2, &sel, true, UUID_A, inside)
           == HK_IMAGE_SCOPE_UUID_MISMATCH);
    assert(hk_image_scope_check(cat2, &sel, false, NULL, inside) == HK_IMAGE_SCOPE_OK);

    hk_image_catalog_destroy(cat2);
    hk_image_catalog_destroy(cat);
    free(img);
    printf("  uuid-distinguishes-builds: PASS\n");
}

// A selector names a SCOPE, not one image, and the same path can be loaded
// more than once. Any matching image accepting the address satisfies it.
static void test_any_matching_image_satisfies(void) {
    const uint64_t base_a = 0x100000000ull;
    const uint64_t base_b = 0x200000000ull;
    uint8_t *img_a = build_image(base_a, 0x1000);
    uint8_t *img_b = build_image(base_b, 0x1000);

    hk_image_catalog_t *cat = hk_image_catalog_create();
    hk_image_entry_t ea = entry_for("/usr/lib/libfoo.dylib", img_a, 0, UUID_A);
    hk_image_entry_t eb = entry_for("/usr/lib/libfoo.dylib", img_b, 0, UUID_B);
    assert(hk_image_catalog_add_entry(cat, &ea));
    assert(hk_image_catalog_add_entry(cat, &eb));
    hk_image_selector_t sel = selector_path("/usr/lib/libfoo.dylib");

    // An address in the SECOND entry is accepted even though the first was
    // visited first -- the walk must not stop at the first non-containing hit.
    assert(hk_image_scope_check(cat, &sel, false, NULL, (uintptr_t)base_b + 0x10)
           == HK_IMAGE_SCOPE_OK);
    assert(hk_image_scope_check(cat, &sel, false, NULL, (uintptr_t)base_a + 0x10)
           == HK_IMAGE_SCOPE_OK);
    // In neither.
    assert(hk_image_scope_check(cat, &sel, false, NULL, 0x900000000ull)
           == HK_IMAGE_SCOPE_ADDRESS_OUTSIDE);
    // UUID narrows it back to one: B's address with A's UUID is a mismatch,
    // since the only entry carrying UUID_A does not contain it.
    assert(hk_image_scope_check(cat, &sel, true, UUID_A, (uintptr_t)base_b + 0x10)
           == HK_IMAGE_SCOPE_ADDRESS_OUTSIDE);
    assert(hk_image_scope_check(cat, &sel, true, UUID_B, (uintptr_t)base_b + 0x10)
           == HK_IMAGE_SCOPE_OK);

    hk_image_catalog_destroy(cat);
    free(img_a);
    free(img_b);
    printf("  any-matching-image-satisfies: PASS\n");
}

static void test_no_catalog_is_a_skip_not_a_failure(void) {
    hk_image_selector_t sel = selector_path("/usr/lib/libfoo.dylib");

    // The policy: no catalog at all, and an empty one, both mean NOT CHECKED.
    // On device the populator is unbuilt, so this is the live path -- failing
    // closed here would make every image-scoped hook fail.
    assert(hk_image_scope_check(NULL, &sel, false, NULL, 0x1000)
           == HK_IMAGE_SCOPE_NO_CATALOG);
    hk_image_catalog_t *empty = hk_image_catalog_create();
    assert(hk_image_scope_check(empty, &sel, false, NULL, 0x1000)
           == HK_IMAGE_SCOPE_NO_CATALOG);
    // ...and it is distinct from "the image is not loaded", which a populated
    // catalog CAN say.
    const uint64_t base = 0x100000000ull;
    uint8_t *img = build_image(base, 0x1000);
    hk_image_entry_t e = entry_for("/usr/lib/libother.dylib", img, 0, UUID_A);
    assert(hk_image_catalog_add_entry(empty, &e));
    assert(hk_image_scope_check(empty, &sel, false, NULL, (uintptr_t)base)
           == HK_IMAGE_SCOPE_NO_MATCH);

    hk_image_catalog_destroy(empty);
    free(img);
    printf("  no-catalog-is-a-skip-not-a-failure: PASS\n");
}

static void test_nothing_asked_is_ok(void) {
    // A NULL selector asks nothing, and so does a UUID requirement with no
    // UUID behind it. Neither may manufacture a refusal.
    assert(hk_image_scope_check(NULL, NULL, false, NULL, 0x1000) == HK_IMAGE_SCOPE_OK);
    hk_image_selector_t sel = selector_path("/usr/lib/libfoo.dylib");
    assert(hk_image_scope_check(NULL, &sel, true, NULL, 0x1000) == HK_IMAGE_SCOPE_OK);
    printf("  nothing-asked-is-ok: PASS\n");
}

static void test_unreadable_image_is_not_outside(void) {
    hk_image_catalog_t *cat = hk_image_catalog_create();

    // A header that is not a Mach-O at all. "Cannot tell" must not be reported
    // as "the address is outside" -- they call for different caller responses.
    uint8_t junk[IMG_SIZE];
    memset(junk, 0xCD, sizeof(junk));
    hk_image_entry_t bad = entry_for("/usr/lib/libjunk.dylib", junk, 0, UUID_A);
    assert(hk_image_catalog_add_entry(cat, &bad));
    hk_image_selector_t sel = selector_path("/usr/lib/libjunk.dylib");
    assert(hk_image_scope_check(cat, &sel, false, NULL, 0x1000)
           == HK_IMAGE_SCOPE_UNREADABLE_IMAGE);

    // A NULL header is the same story.
    hk_image_catalog_t *cat2 = hk_image_catalog_create();
    hk_image_entry_t nohdr = entry_for("/usr/lib/libjunk.dylib", NULL, 0, UUID_A);
    assert(hk_image_catalog_add_entry(cat2, &nohdr));
    assert(hk_image_scope_check(cat2, &sel, false, NULL, 0x1000)
           == HK_IMAGE_SCOPE_UNREADABLE_IMAGE);

    hk_image_catalog_destroy(cat2);
    hk_image_catalog_destroy(cat);
    printf("  unreadable-image-is-not-outside: PASS\n");
}

static void test_every_status_has_a_description(void) {
    const hk_image_scope_status_t all[] = {
        HK_IMAGE_SCOPE_OK, HK_IMAGE_SCOPE_NO_CATALOG, HK_IMAGE_SCOPE_NO_MATCH,
        HK_IMAGE_SCOPE_UUID_MISMATCH, HK_IMAGE_SCOPE_ADDRESS_OUTSIDE,
        HK_IMAGE_SCOPE_UNREADABLE_IMAGE,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *d = hk_image_scope_describe(all[i]);
        assert(d && strlen(d) > 0);
        // Distinct from every other -- a description that repeats tells a
        // caller nothing.
        for (size_t j = 0; j < i; j++) {
            assert(strcmp(d, hk_image_scope_describe(all[j])) != 0);
        }
    }
    assert(hk_image_scope_describe((hk_image_scope_status_t)999) != NULL);
    printf("  every-status-has-a-description: PASS\n");
}

int main(void) {
    test_address_inside_and_outside();
    test_uuid_distinguishes_builds();
    test_any_matching_image_satisfies();
    test_no_catalog_is_a_skip_not_a_failure();
    test_nothing_asked_is_ok();
    test_unreadable_image_is_not_outside();
    test_every_status_has_a_description();
    printf("all image scope tests passed\n");
    return 0;
}
