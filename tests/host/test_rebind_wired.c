// End-to-end: the Milestone 4 plan lifecycle driving the real Milestone 6
// rebind engine through its runtime adapter (HKRebindVtable.h), against a
// synthetic image with a buffer-backed writer. This is the join the whole
// stack was built toward -- analyze/prepare/commit on a real plan, real
// resolvers finding real slots, real writes, and real artifacts in the report.
// Nothing here is a fake engine.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/engines/HKRebindVtable.h"

// Same loaded-layout image as test_rebind_engine.c: __got with two slots both
// binding "_malloc", tables in __LINKEDIT at translated offsets.
#define V_BASE      0x100000000ull
#define IMG_SIZE    0x600u
#define GOT_OFF     0x180u
#define ORIGINAL    0xAAAA0001ull
#define REPLACEMENT 0xBBBB2222ull

static void put_u32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, sizeof(v)); }
static void put_u64(uint8_t *b, size_t o, uint64_t v) { memcpy(b + o, &v, sizeof(v)); }

static void build_image(uint8_t *img) {
    memset(img, 0, IMG_SIZE);
    const uint32_t seg_data = 32, sect = 104, seg_le = 184, symtab = 256, dysym = 280;
    put_u32(img, 0, HK_MH_MAGIC_64);
    put_u32(img, 16, 4);
    put_u32(img, 20, 152 + 72 + 24 + 80);

    put_u32(img, seg_data, HK_LC_SEGMENT_64);
    put_u32(img, seg_data + 4, 152);
    memcpy(img + seg_data + 8, "__DATA", 6);
    put_u64(img, seg_data + 24, V_BASE + GOT_OFF);
    put_u32(img, seg_data + 64, 1);

    memcpy(img + sect, "__got", 5);
    memcpy(img + sect + 16, "__DATA", 6);
    put_u64(img, sect + 32, V_BASE + GOT_OFF);
    put_u64(img, sect + 40, 16);
    put_u32(img, sect + 64, HK_S_NON_LAZY_SYMBOL_POINTERS);
    put_u32(img, sect + 68, 0);

    put_u32(img, seg_le, HK_LC_SEGMENT_64);
    put_u32(img, seg_le + 4, 72);
    memcpy(img + seg_le + 8, "__LINKEDIT", 10);
    put_u64(img, seg_le + 24, V_BASE + 0x240);
    put_u64(img, seg_le + 32, 0x400);
    put_u64(img, seg_le + 40, 0x40);
    put_u64(img, seg_le + 48, 0x400);

    put_u32(img, symtab, HK_LC_SYMTAB);
    put_u32(img, symtab + 4, HK_SYMTAB_COMMAND_SIZE);
    put_u32(img, symtab + 8, 0x40);
    put_u32(img, symtab + 12, 2);
    put_u32(img, symtab + 16, 0x60);
    put_u32(img, symtab + 20, 32);

    put_u32(img, dysym, HK_LC_DYSYMTAB);
    put_u32(img, dysym + 4, HK_DYSYMTAB_COMMAND_SIZE);
    put_u32(img, dysym + 56, 0x80);
    put_u32(img, dysym + 60, 2);

    put_u32(img, 0x240, 1);      img[0x244] = 0x01;   // "_malloc"
    put_u32(img, 0x250, 9);      img[0x254] = 0x01;   // "_free"
    memcpy(img + 0x260, "\0_malloc\0_free", 15);

    put_u32(img, 0x280, 0);      // both slots -> symbol 0 ("_malloc")
    put_u32(img, 0x284, 0);

    put_u64(img, GOT_OFF, ORIGINAL);
    put_u64(img, GOT_OFF + 8, ORIGINAL);
}

static bool buffer_write(void *ctx, uintptr_t address, uint64_t value) {
    (void)ctx;
    memcpy((void *)address, &value, sizeof(value));
    return true;
}

static uint64_t slot(const uint8_t *img, size_t off) {
    uint64_t v; memcpy(&v, img + off, sizeof(v)); return v;
}

static hk_hook_spec_t symbol_spec(const char *id, const char *symbol, void *replacement) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = symbol;
    spec.target.symbol.name_convention = HK_SYMBOL_NAME_C;
    spec.replacement = replacement;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

// The engine context is an ordinary caller-owned struct now -- no file-scoped
// environment, so each test owns its own.
static hk_rebind_engine_ctx_t engine_ctx_for(uint8_t *img) {
    hk_rebind_engine_ctx_t c;
    memset(&c, 0, sizeof(c));  // a later field must default to "not supplied", not to stack garbage
    c.image_base = img;
    c.image_size = IMG_SIZE;
    c.slide = (uintptr_t)img - (uintptr_t)V_BASE;
    c.write = buffer_write;
    c.write_ctx = NULL;
    return c;
}

static hk_rebind_engine_ctx_t engine_ctx_for_catalog(const hk_image_catalog_t *catalog) {
    hk_rebind_engine_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.write = buffer_write;
    c.catalog = catalog;
    return c;
}

// ---- tests --------------------------------------------------------------

static void test_full_lifecycle_rebinds_and_reports(void) {
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);

    hk_rebind_engine_ctx_t ectx = engine_ctx_for(img);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.malloc", "malloc", (void *)(uintptr_t)REPLACEMENT);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    // The real router matches the rebind engine on target kind + reach.
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert(hook->matched_engine == hk_rebind_vtable());

    // Prepare runs the engine's prepare: captures originals, mutates nothing.
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(slot(img, GOT_OFF) == ORIGINAL && slot(img, GOT_OFF + 8) == ORIGINAL);

    // Commit writes. Both slots bind _malloc, so both are rewritten.
    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hook->result.mutation == HK_MUTATION_COMPLETE);
    assert(slot(img, GOT_OFF) == REPLACEMENT);
    assert(slot(img, GOT_OFF + 8) == REPLACEMENT);

    // The report carries the engine's real artifacts (two slots).
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 2);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_IMPORT_SLOT);
    assert(a.effects == HK_EFFECT_IMPORT_MUTATION);
    assert((uint64_t)(uintptr_t)a.replacement_pointer == REPLACEMENT);
    assert((uint64_t)(uintptr_t)a.original_pointer == ORIGINAL);
    // The sink stamped the contextual id (request_id == the hook's id).
    assert(a.request_id.high == hook->hook_id.high && a.request_id.low == hook->hook_id.low);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(img);
    printf("  full-lifecycle-rebinds-and-reports: PASS\n");
}

static void test_absent_symbol_fails_at_prepare(void) {
    // The router still routes it (describe() eligibility is target-kind + reach,
    // it does not know the symbol), so the honest failure surfaces at prepare.
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);

    hk_rebind_engine_ctx_t ectx = engine_ctx_for(img);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.absent", "not_imported", (void *)0x1);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == hk_rebind_vtable());  // routed...

    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);  // ...but preparation refused
    assert(hk_plan_state(plan) == HK_PLAN_FAILED);

    // Nothing was written, and committing a non-prepared plan-with-no-prepared
    // -hooks leaves the image untouched.
    assert(slot(img, GOT_OFF) == ORIGINAL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(img);
    printf("  absent-symbol-fails-at-prepare: PASS\n");
}

static void test_no_environment_fails_cleanly(void) {
    // Registered with NO context, prepare has no image to work on and must
    // fail cleanly rather than crash -- the router routed on capability alone.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), NULL));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.x", "malloc", (void *)0x1);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  no-environment-fails-cleanly: PASS\n");
}

// The image-scope gap, closed for the rebind adapter. The engine rewrites the
// IMPORTER's slots, so caller_image_scope is the selector that applies -- a
// request that names a different importer must be refused before any slot is
// read.
static void test_caller_image_scope_is_enforced(void) {
    uint8_t *img = aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);

    // The catalog describes the very image the context points at.
    hk_image_catalog_t *cat = hk_image_catalog_create();
    hk_image_entry_t e;
    memset(&e, 0, sizeof(e));
    e.path = "/usr/lib/libimporter.dylib";
    e.header = img;
    e.slide = (uintptr_t)img - (uintptr_t)V_BASE;
    assert(hk_image_catalog_add_entry(cat, &e));

    hk_rebind_engine_ctx_t ectx = engine_ctx_for(img);
    ectx.catalog = cat;

    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t right = symbol_spec("h.right", "malloc", (void *)(uintptr_t)REPLACEMENT);
    right.target.symbol.caller_image_scope.struct_size = sizeof(right.target.symbol.caller_image_scope);
    right.target.symbol.caller_image_scope.struct_version = HK_ABI_VERSION_3_0;
    right.target.symbol.caller_image_scope.kind = HK_IMAGE_EXACT_PATH;
    right.target.symbol.caller_image_scope.path = "/usr/lib/libimporter.dylib";

    hk_hook_spec_t wrong = right;
    wrong.stable_hook_id = "h.wrong";
    wrong.target.symbol.caller_image_scope.path = "/usr/lib/libsomeoneelse.dylib";

    hk_hook_t *hr = NULL, *hw = NULL;
    assert(hk_plan_add_hook(plan, &right, &hr) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &wrong, &hw) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    assert(hr->result.outcome == HK_OUTCOME_PREPARED);
    assert(hw->result.outcome == HK_OUTCOME_FAILED_SAFE);
    // The refusal identifies itself as an image-scope one, not a rebind-engine
    // one -- that is what the diag-base offset buys.
    assert(hw->result.error_code ==
           HK_REBIND_DIAG_IMAGE_SCOPE_BASE + (int64_t)HK_IMAGE_SCOPE_NO_MATCH);
    assert(hw->result.error_message.data &&
           strcmp(hw->result.error_message.data,
                  hk_image_scope_describe(HK_IMAGE_SCOPE_NO_MATCH)) == 0);

    hk_plan_release(plan);
    hk_runtime_release(rt);

    // Same wrong-scope request with NO catalog: skipped, so it prepares. This
    // is the device path today and the reason the policy is a skip.
    hk_rebind_engine_ctx_t noctx = engine_ctx_for(img);  // catalog stays NULL
    hk_runtime_t *rt2 = NULL;
    hk_plan_t *p2 = NULL;
    assert(hk_runtime_create(NULL, &rt2) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt2, hk_rebind_vtable(), &noctx));
    assert(hk_plan_create(rt2, NULL, &p2) == HK_STATUS_OK);
    hk_hook_t *h2 = NULL;
    assert(hk_plan_add_hook(p2, &wrong, &h2) == HK_STATUS_OK);
    assert(hk_plan_analyze(p2, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(p2, NULL) == HK_STATUS_OK);
    assert(h2->result.outcome == HK_OUTCOME_PREPARED);

    hk_plan_release(p2);
    hk_runtime_release(rt2);
    hk_image_catalog_destroy(cat);
    free(img);
    printf("  caller-image-scope-is-enforced: PASS\n");
}

// An absent symbol and a wrong image are different diagnoses now, not one
// undifferentiated prepare failure.
static void test_rebind_refusals_are_distinguishable(void) {
    uint8_t *img = aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);

    hk_rebind_engine_ctx_t ectx = engine_ctx_for(img);  // no catalog: scope skipped
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t absent = symbol_spec("h.absent", "not_imported", (void *)0x1);
    hk_hook_t *ha = NULL;
    assert(hk_plan_add_hook(plan, &absent, &ha) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(ha->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(ha->result.error_code == (int64_t)HK_REBIND_NOT_FOUND);
    assert(ha->result.error_message.data &&
           strcmp(ha->result.error_message.data, "the image imports no such symbol") == 0);
    assert(ha->result.error_domain.data &&
           strcmp(ha->result.error_domain.data, "fishhook") == 0);
    // Distinct from the image-scope band, which is what the offset guarantees.
    assert(ha->result.error_code < HK_REBIND_DIAG_IMAGE_SCOPE_BASE);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(img);
    printf("  rebind-refusals-are-distinguishable: PASS\n");
}

static void test_catalog_context_rebinds_every_matching_image(void) {
    uint8_t *first = aligned_alloc(64, IMG_SIZE);
    uint8_t *second = aligned_alloc(64, IMG_SIZE);
    assert(first && second);
    build_image(first);
    build_image(second);

    hk_image_catalog_t *catalog = hk_image_catalog_create();
    assert(catalog);
    hk_image_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.header = first;
    entry.slide = (uintptr_t)first - (uintptr_t)V_BASE;
    assert(hk_image_catalog_add_entry(catalog, &entry));
    entry.header = second;
    entry.slide = (uintptr_t)second - (uintptr_t)V_BASE;
    assert(hk_image_catalog_add_entry(catalog, &entry));

    hk_rebind_engine_ctx_t ectx = engine_ctx_for_catalog(catalog);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.all-importers", "malloc",
                                      (void *)(uintptr_t)REPLACEMENT);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(slot(first, GOT_OFF) == REPLACEMENT);
    assert(slot(first, GOT_OFF + 8) == REPLACEMENT);
    assert(slot(second, GOT_OFF) == REPLACEMENT);
    assert(slot(second, GOT_OFF + 8) == REPLACEMENT);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_image_catalog_destroy(catalog);
    free(first);
    free(second);
    printf("  catalog-context-rebinds-every-matching-image: PASS\n");
}

static void test_catalog_bundle_cache_revalidates_changed_slots(void) {
    uint8_t *img = aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);

    hk_image_catalog_t *catalog = hk_image_catalog_create();
    assert(catalog);
    hk_image_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.header = img;
    entry.slide = (uintptr_t)img - (uintptr_t)V_BASE;
    assert(hk_image_catalog_add_entry(catalog, &entry));

    hk_rebind_engine_ctx_t ectx = engine_ctx_for_catalog(catalog);
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));

    // A later plan can reuse a prepared catalog-wide bundle without becoming a
    // same-plan duplicate target.
    hk_hook_spec_t first_spec = symbol_spec("hook.cache.first", "malloc",
                                            (void *)(uintptr_t)REPLACEMENT);
    hk_plan_t *first_plan = NULL;
    hk_hook_t *first_hook = NULL;
    assert(hk_plan_create(rt, NULL, &first_plan) == HK_STATUS_OK);
    assert(hk_plan_add_hook(first_plan, &first_spec, &first_hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(first_plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(first_plan, NULL) == HK_STATUS_OK);
    assert(first_hook->result.outcome == HK_OUTCOME_PREPARED);
    hk_plan_release(first_plan);

    hk_hook_spec_t second_spec = first_spec;
    second_spec.stable_hook_id = "hook.cache.second";
    hk_plan_t *cached_plan = NULL;
    hk_hook_t *second_hook = NULL;
    assert(hk_plan_create(rt, NULL, &cached_plan) == HK_STATUS_OK);
    assert(hk_plan_add_hook(cached_plan, &second_spec, &second_hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(cached_plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(cached_plan, NULL) == HK_STATUS_OK);
    assert(second_hook->result.outcome == HK_OUTCOME_PREPARED);
    hk_plan_release(cached_plan);

    // A later plan must not use the captured originals after another writer
    // changed the live slots. The stale cache entry should be discarded and
    // preparation should recapture these values.
    put_u64(img, GOT_OFF, ORIGINAL + 1);
    put_u64(img, GOT_OFF + 8, ORIGINAL + 1);
    hk_plan_t *fresh_plan = NULL;
    hk_hook_t *fresh_hook = NULL;
    assert(hk_plan_create(rt, NULL, &fresh_plan) == HK_STATUS_OK);
    hk_hook_spec_t fresh_spec = symbol_spec("hook.cache.refresh", "malloc",
                                            (void *)(uintptr_t)REPLACEMENT);
    assert(hk_plan_add_hook(fresh_plan, &fresh_spec, &fresh_hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(fresh_plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(fresh_plan, NULL) == HK_STATUS_OK);
    assert(fresh_hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hk_plan_commit(fresh_plan, NULL) == HK_STATUS_OK);
    assert(fresh_hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(slot(img, GOT_OFF) == REPLACEMENT);
    assert(slot(img, GOT_OFF + 8) == REPLACEMENT);

    hk_plan_release(fresh_plan);
    hk_runtime_release(rt);
    hk_image_catalog_destroy(catalog);
    free(img);
    printf("  catalog-bundle-cache-revalidates-changed-slots: PASS\n");
}

static void test_required_original_rejects_ambiguous_catalog(void) {
    uint8_t *first = aligned_alloc(64, IMG_SIZE);
    uint8_t *second = aligned_alloc(64, IMG_SIZE);
    assert(first && second);
    build_image(first);
    build_image(second);
    put_u64(second, GOT_OFF, ORIGINAL + 1);
    put_u64(second, GOT_OFF + 8, ORIGINAL + 1);

    hk_image_catalog_t *catalog = hk_image_catalog_create();
    assert(catalog);
    hk_image_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.header = first;
    entry.slide = (uintptr_t)first - (uintptr_t)V_BASE;
    assert(hk_image_catalog_add_entry(catalog, &entry));
    entry.header = second;
    entry.slide = (uintptr_t)second - (uintptr_t)V_BASE;
    assert(hk_image_catalog_add_entry(catalog, &entry));

    hk_rebind_engine_ctx_t ectx = engine_ctx_for_catalog(catalog);
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_rebind_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.ambiguous-original", "malloc",
                                      (void *)(uintptr_t)REPLACEMENT);
    spec.original_requirement = HK_ORIGINAL_DIRECT_PREDECESSOR;
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(slot(first, GOT_OFF) == ORIGINAL);
    assert(slot(second, GOT_OFF) == ORIGINAL + 1);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    hk_image_catalog_destroy(catalog);
    free(first);
    free(second);
    printf("  required-original-rejects-ambiguous-catalog: PASS\n");
}

int main(void) {
    #define RUN_TEST(test) do { hk_ownership_reset_for_testing(); test(); } while (0)
    RUN_TEST(test_full_lifecycle_rebinds_and_reports);
    RUN_TEST(test_absent_symbol_fails_at_prepare);
    RUN_TEST(test_no_environment_fails_cleanly);
    RUN_TEST(test_caller_image_scope_is_enforced);
    RUN_TEST(test_rebind_refusals_are_distinguishable);
    RUN_TEST(test_catalog_context_rebinds_every_matching_image);
    RUN_TEST(test_catalog_bundle_cache_revalidates_changed_slots);
    RUN_TEST(test_required_original_rejects_ambiguous_catalog);
    #undef RUN_TEST
    hk_ownership_reset_for_testing();
    printf("all rebind wired tests passed\n");
    return 0;
}
