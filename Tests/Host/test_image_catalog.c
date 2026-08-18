// Host test for Sources/Core/HKImageCatalog.c -- the platform-agnostic
// half (structure + selector matching), exercised against synthetic image
// entries. Real dyld population is device-only and not tested here (see
// HKImageCatalog.h).

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Core/HKImageCatalog.h"

// ---- helpers ------------------------------------------------------------

static void add(hk_image_catalog_t *cat, const char *path, bool is_main,
                const void *header, bool uuid_present, uint8_t uuid_fill) {
    hk_image_entry_t e;
    memset(&e, 0, sizeof(e));
    e.path = path;
    e.is_main_executable = is_main;
    e.header = header;
    e.slide = 0x4000;
    e.uuid_present = uuid_present;
    if (uuid_present) {
        memset(e.uuid, uuid_fill, sizeof(e.uuid));
    }
    assert(hk_image_catalog_add_entry(cat, &e));
}

static hk_image_selector_t sel_kind(hk_image_selector_kind_t k) {
    hk_image_selector_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = sizeof(s);
    s.struct_version = HK_ABI_VERSION_3_0;
    s.kind = k;
    return s;
}

typedef struct {
    size_t indices[64];
    size_t n;
    size_t stop_after;  // 0 = never stop
} visit_ctx_t;

static bool record_visit(void *c, size_t index, const hk_image_entry_t *entry) {
    (void)entry;
    visit_ctx_t *v = (visit_ctx_t *)c;
    if (v->n < 64) {
        v->indices[v->n] = index;
    }
    v->n++;
    if (v->stop_after && v->n >= v->stop_after) {
        return false;
    }
    return true;
}

// Builds a 3-image catalog: libA, the main executable, libB (in that order).
static hk_image_catalog_t *sample_catalog(void) {
    hk_image_catalog_t *cat = hk_image_catalog_create();
    assert(cat != NULL);
    add(cat, "/usr/lib/libA.dylib", false, (const void *)0x1000, true, 0xAA);
    add(cat, "/var/app/Main",       true,  (const void *)0x2000, true, 0xBB);
    add(cat, "/usr/lib/libB.dylib", false, (const void *)0x3000, false, 0x00);
    assert(hk_image_catalog_count(cat) == 3);
    return cat;
}

// ---- tests --------------------------------------------------------------

static void test_any_loaded_matches_all_in_order(void) {
    hk_image_catalog_t *cat = sample_catalog();
    hk_image_selector_t s = sel_kind(HK_IMAGE_ANY_LOADED);
    visit_ctx_t v = {{0}, 0, 0};
    size_t n = hk_image_catalog_match(cat, &s, record_visit, &v);
    assert(n == 3 && v.n == 3);
    assert(v.indices[0] == 0 && v.indices[1] == 1 && v.indices[2] == 2);
    hk_image_catalog_destroy(cat);
    printf("  any-loaded-matches-all-in-order: PASS\n");
}

static void test_main_executable(void) {
    hk_image_catalog_t *cat = sample_catalog();
    hk_image_selector_t s = sel_kind(HK_IMAGE_MAIN_EXECUTABLE);
    visit_ctx_t v = {{0}, 0, 0};
    size_t n = hk_image_catalog_match(cat, &s, record_visit, &v);
    assert(n == 1 && v.indices[0] == 1);  // only the main executable
    hk_image_catalog_destroy(cat);
    printf("  main-executable: PASS\n");
}

static void test_exact_path(void) {
    hk_image_catalog_t *cat = sample_catalog();
    hk_image_selector_t s = sel_kind(HK_IMAGE_EXACT_PATH);
    s.path = "/usr/lib/libB.dylib";
    visit_ctx_t v = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &s, record_visit, &v) == 1);
    assert(v.indices[0] == 2);

    // A path not in the catalog matches nothing.
    hk_image_selector_t miss = sel_kind(HK_IMAGE_EXACT_PATH);
    miss.path = "/nope";
    visit_ctx_t v2 = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &miss, record_visit, &v2) == 0);

    hk_image_catalog_destroy(cat);
    printf("  exact-path: PASS\n");
}

static void test_exact_uuid_requires_uuid_present(void) {
    hk_image_catalog_t *cat = sample_catalog();
    hk_image_selector_t s = sel_kind(HK_IMAGE_EXACT_UUID);
    memset(s.uuid, 0xAA, sizeof(s.uuid));
    visit_ctx_t v = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &s, record_visit, &v) == 1);
    assert(v.indices[0] == 0);  // libA has uuid 0xAA...

    // libB has uuid bytes all-zero but uuid_present=false: an all-zero uuid
    // selector must NOT match it (absence of a uuid is not a zero uuid).
    hk_image_selector_t zero = sel_kind(HK_IMAGE_EXACT_UUID);  // uuid already 0
    visit_ctx_t v2 = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &zero, record_visit, &v2) == 0);

    hk_image_catalog_destroy(cat);
    printf("  exact-uuid-requires-uuid-present: PASS\n");
}

static void test_exact_header(void) {
    hk_image_catalog_t *cat = sample_catalog();
    hk_image_selector_t s = sel_kind(HK_IMAGE_EXACT_HEADER);
    s.header = (const void *)0x2000;  // the main executable's header
    visit_ctx_t v = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &s, record_visit, &v) == 1);
    assert(v.indices[0] == 1);

    // A NULL header selector matches nothing (never treat "no header" as a
    // wildcard against entries that happen to have NULL headers).
    hk_image_selector_t null_hdr = sel_kind(HK_IMAGE_EXACT_HEADER);  // header = NULL
    visit_ctx_t v2 = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &null_hdr, record_visit, &v2) == 0);

    hk_image_catalog_destroy(cat);
    printf("  exact-header: PASS\n");
}

static void test_explicit_set_union_and_dedup(void) {
    hk_image_catalog_t *cat = sample_catalog();

    // Union of two disjoint sub-selectors -> both matched.
    hk_image_selector_t a = sel_kind(HK_IMAGE_EXACT_PATH); a.path = "/usr/lib/libA.dylib";
    hk_image_selector_t b = sel_kind(HK_IMAGE_EXACT_PATH); b.path = "/usr/lib/libB.dylib";
    const hk_image_selector_t *subs[] = {&a, &b};
    hk_image_selector_t set = sel_kind(HK_IMAGE_EXPLICIT_SET);
    set.explicit_set = subs;
    set.explicit_set_count = 2;
    visit_ctx_t v = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &set, record_visit, &v) == 2);
    assert(v.indices[0] == 0 && v.indices[1] == 2);  // libA (0), libB (2), in order

    // Overlapping sub-selectors: EXACT_PATH(Main) and MAIN_EXECUTABLE both
    // match the same entry -- it must be visited exactly ONCE.
    hk_image_selector_t p = sel_kind(HK_IMAGE_EXACT_PATH); p.path = "/var/app/Main";
    hk_image_selector_t m = sel_kind(HK_IMAGE_MAIN_EXECUTABLE);
    const hk_image_selector_t *subs2[] = {&p, &m};
    hk_image_selector_t set2 = sel_kind(HK_IMAGE_EXPLICIT_SET);
    set2.explicit_set = subs2;
    set2.explicit_set_count = 2;
    visit_ctx_t v2 = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &set2, record_visit, &v2) == 1);
    assert(v2.indices[0] == 1);

    hk_image_catalog_destroy(cat);
    printf("  explicit-set-union-and-dedup: PASS\n");
}

static void test_early_stop(void) {
    hk_image_catalog_t *cat = sample_catalog();
    hk_image_selector_t s = sel_kind(HK_IMAGE_ANY_LOADED);
    visit_ctx_t v = {{0}, 0, 1};  // stop after the first visit
    size_t n = hk_image_catalog_match(cat, &s, record_visit, &v);
    assert(n == 1 && v.n == 1 && v.indices[0] == 0);
    hk_image_catalog_destroy(cat);
    printf("  early-stop: PASS\n");
}

static void test_generation_bumps_on_add(void) {
    hk_image_catalog_t *cat = hk_image_catalog_create();
    assert(hk_image_catalog_generation(cat) == 0);
    add(cat, "/a", false, (const void *)0x1, false, 0);
    uint64_t g1 = hk_image_catalog_generation(cat);
    add(cat, "/b", false, (const void *)0x2, false, 0);
    uint64_t g2 = hk_image_catalog_generation(cat);
    assert(g1 == 1 && g2 == 2);
    hk_image_catalog_destroy(cat);
    printf("  generation-bumps-on-add: PASS\n");
}

static void test_path_is_deep_copied(void) {
    // Prove the catalog owns its path copy: mutate the source buffer after
    // adding, then match against the original text. If the catalog had kept
    // the caller's pointer, the mutation would make the match miss.
    hk_image_catalog_t *cat = hk_image_catalog_create();
    char src[32];
    memcpy(src, "/tmp/ephemeral.dylib", sizeof("/tmp/ephemeral.dylib"));
    add(cat, src, false, (const void *)0x9, false, 0);
    memset(src, 'X', sizeof(src));  // clobber the source; catalog copy must be intact

    hk_image_selector_t s = sel_kind(HK_IMAGE_EXACT_PATH);
    s.path = "/tmp/ephemeral.dylib";
    visit_ctx_t v = {{0}, 0, 0};
    assert(hk_image_catalog_match(cat, &s, record_visit, &v) == 1);
    hk_image_catalog_destroy(cat);
    printf("  path-is-deep-copied: PASS\n");
}

static void test_null_tolerance(void) {
    hk_image_selector_t s = sel_kind(HK_IMAGE_ANY_LOADED);
    visit_ctx_t v = {{0}, 0, 0};
    assert(hk_image_catalog_match(NULL, &s, record_visit, &v) == 0);

    hk_image_catalog_t *cat = sample_catalog();
    assert(hk_image_catalog_match(cat, NULL, record_visit, &v) == 0);
    assert(hk_image_catalog_match(cat, &s, NULL, &v) == 0);

    hk_image_entry_t e;
    memset(&e, 0, sizeof(e));
    e.path = "/x";
    assert(!hk_image_catalog_add_entry(NULL, &e));
    assert(!hk_image_catalog_add_entry(cat, NULL));

    assert(hk_image_catalog_count(NULL) == 0);
    assert(hk_image_catalog_generation(NULL) == 0);
    hk_image_catalog_destroy(NULL);  // must not crash

    hk_image_catalog_destroy(cat);
    printf("  null-tolerance: PASS\n");
}

int main(void) {
    test_any_loaded_matches_all_in_order();
    test_main_executable();
    test_exact_path();
    test_exact_uuid_requires_uuid_present();
    test_exact_header();
    test_explicit_set_union_and_dedup();
    test_early_stop();
    test_generation_bumps_on_add();
    test_path_is_deep_copied();
    test_null_tolerance();
    printf("all image catalog tests passed\n");
    return 0;
}
