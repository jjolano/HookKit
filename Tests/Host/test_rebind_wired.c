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

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "../../Sources/Engines/HKRebindVtable.h"

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
           strcmp(ha->result.error_domain.data, "rebind") == 0);
    // Distinct from the image-scope band, which is what the offset guarantees.
    assert(ha->result.error_code < HK_REBIND_DIAG_IMAGE_SCOPE_BASE);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    free(img);
    printf("  rebind-refusals-are-distinguishable: PASS\n");
}

int main(void) {
    test_full_lifecycle_rebinds_and_reports();
    test_absent_symbol_fails_at_prepare();
    test_no_environment_fails_cleanly();
    test_caller_image_scope_is_enforced();
    test_rebind_refusals_are_distinguishable();
    printf("all rebind wired tests passed\n");
    return 0;
}
