// Host test for Sources/Engines/HKRebindEngine.c.
//
// The write is behind a seam, so everything the engine decides is testable
// here: which slots it finds, that prepare mutates nothing, that it
// revalidates before writing, and -- the part that matters most -- that it
// reports mutation state honestly when a write fails partway through. A
// failure after one slot is already rewritten is NOT a clean failure the
// router may retry elsewhere; it is PARTIAL, and saying otherwise would let
// the runtime attempt a second route over a half-modified image.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Engines/HKRebindEngine.h"

// Loaded-layout image: load commands, a __got with two slots, and __LINKEDIT
// holding the symbol/indirect tables at translated offsets.
//   commands   [32,360)
//   __got      img+0x180, 2 slots            (__DATA vmaddr V+0x180)
//   nlist      img+0x240   strings img+0x260   indirect img+0x280
#define V_BASE      0x100000000ull
#define IMG_SIZE    0x600u
#define GOT_OFF     0x180u
#define NLIST_OFF   0x240u
#define STR_OFF     0x260u
#define INDIR_OFF   0x280u
#define ORIGINAL    0xAAAA0001ull
#define REPLACEMENT 0xBBBB0002ull

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
    put_u32(img, seg_data + 64, 1);                     // nsects

    memcpy(img + sect, "__got", 5);
    memcpy(img + sect + 16, "__DATA", 6);
    put_u64(img, sect + 32, V_BASE + GOT_OFF);          // addr
    put_u64(img, sect + 40, 16);                        // 2 slots
    put_u32(img, sect + 64, HK_S_NON_LAZY_SYMBOL_POINTERS);
    put_u32(img, sect + 68, 0);                         // reserved1

    // __LINKEDIT vmaddr - fileoff = V+0x240-0x40, so linkedit_base = img+0x200
    // and the table offsets below land where they are written.
    put_u32(img, seg_le, HK_LC_SEGMENT_64);
    put_u32(img, seg_le + 4, 72);
    memcpy(img + seg_le + 8, "__LINKEDIT", 10);
    put_u64(img, seg_le + 24, V_BASE + 0x240);
    put_u64(img, seg_le + 32, 0x400);
    put_u64(img, seg_le + 40, 0x40);
    put_u64(img, seg_le + 48, 0x400);

    put_u32(img, symtab, HK_LC_SYMTAB);
    put_u32(img, symtab + 4, HK_SYMTAB_COMMAND_SIZE);
    put_u32(img, symtab + 8, 0x40);                     // -> img+0x240
    put_u32(img, symtab + 12, 2);
    put_u32(img, symtab + 16, 0x60);                    // -> img+0x260
    put_u32(img, symtab + 20, 32);

    put_u32(img, dysym, HK_LC_DYSYMTAB);
    put_u32(img, dysym + 4, HK_DYSYMTAB_COMMAND_SIZE);
    put_u32(img, dysym + 56, 0x80);                     // -> img+0x280
    put_u32(img, dysym + 60, 2);

    put_u32(img, NLIST_OFF, 1);      img[NLIST_OFF + 4] = 0x01;   // "_malloc", N_UNDF|N_EXT
    put_u32(img, NLIST_OFF + 16, 9); img[NLIST_OFF + 20] = 0x01;  // "_free"
    memcpy(img + STR_OFF, "\0_malloc\0_free", 15);

    // BOTH slots bind the same symbol, so one rebind rewrites two sites --
    // which is what makes the partial-write cases below meaningful.
    put_u32(img, INDIR_OFF, 0);
    put_u32(img, INDIR_OFF + 4, 0);

    put_u64(img, GOT_OFF, ORIGINAL);
    put_u64(img, GOT_OFF + 8, ORIGINAL);
}

// ---- the write seam -----------------------------------------------------

typedef struct {
    int calls;
    int fail_on_call;   // 1-based; 0 = never fail
} writer_t;

static bool test_write(void *ctx, uintptr_t address, uint64_t value) {
    writer_t *w = (writer_t *)ctx;
    w->calls++;
    if (w->fail_on_call && w->calls == w->fail_on_call) {
        return false;
    }
    memcpy((void *)address, &value, sizeof(value));
    return true;
}

static hk_rebind_target_t make_target(uint8_t *img, writer_t *w) {
    hk_rebind_target_t t;
    memset(&t, 0, sizeof(t));
    t.image_base = img;
    t.image_size = IMG_SIZE;
    t.slide = (uintptr_t)img - (uintptr_t)V_BASE;
    t.write = test_write;
    t.write_ctx = w;
    return t;
}

static uint64_t slot(const uint8_t *img, size_t off) {
    uint64_t v; memcpy(&v, img + off, sizeof(v)); return v;
}

// ---- tests --------------------------------------------------------------

static void test_prepare_finds_sites_and_mutates_nothing(void) {
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    uint8_t *before = (uint8_t *)malloc(IMG_SIZE);
    assert(img && before);
    build_image(img);
    memcpy(before, img, IMG_SIZE);

    writer_t w = {0, 0};
    hk_rebind_target_t t = make_target(img, &w);
    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);

    // Both slots bind "_malloc", found via the bare C name.
    assert(plan.count == 2);
    assert(plan.sites[0].address == (uintptr_t)img + GOT_OFF);
    assert(plan.sites[1].address == (uintptr_t)img + GOT_OFF + 8);
    assert(plan.sites[0].original == ORIGINAL && plan.sites[1].original == ORIGINAL);
    assert(plan.originals_agree && plan.original == ORIGINAL);

    // Invariant #2: preparation mutates nothing. Byte-for-byte, not just the
    // slots -- a stray write anywhere would show up here.
    assert(memcmp(img, before, IMG_SIZE) == 0);
    assert(w.calls == 0);

    free(before); free(img);
    printf("  prepare-finds-sites-and-mutates-nothing: PASS\n");
}

static void test_commit_writes_every_site(void) {
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);
    writer_t w = {0, 0};
    hk_rebind_target_t t = make_target(img, &w);

    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);

    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.ledger = ledger;

    uint32_t written = 0;
    assert(hk_rebind_commit(&t, &plan, REPLACEMENT, &sink, &written) == HK_MUTATION_COMPLETE);
    assert(written == 2 && w.calls == 2);
    assert(slot(img, GOT_OFF) == REPLACEMENT);
    assert(slot(img, GOT_OFF + 8) == REPLACEMENT);

    // One artifact per written site, carrying both pointers.
    assert(hk_artifact_ledger_count(ledger) == 2);
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_IMPORT_SLOT);
    assert(a.effects == HK_EFFECT_IMPORT_MUTATION);
    assert((uint64_t)(uintptr_t)a.original_pointer == ORIGINAL);
    assert((uint64_t)(uintptr_t)a.replacement_pointer == REPLACEMENT);
    assert(a.import_slot_address == (uintptr_t)img + GOT_OFF);
    assert(a.mechanically_reversible);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    free(img);
    printf("  commit-writes-every-site: PASS\n");
}

static void test_failure_before_any_write_is_none(void) {
    // A clean refusal: nothing was touched, so the router may legitimately try
    // another route.
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);
    writer_t w = {0, 1};   // fail on the first write
    hk_rebind_target_t t = make_target(img, &w);

    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);
    uint32_t written = 99;
    assert(hk_rebind_commit(&t, &plan, REPLACEMENT, NULL, &written) == HK_MUTATION_NONE);
    assert(written == 0);
    assert(slot(img, GOT_OFF) == ORIGINAL);       // untouched
    assert(slot(img, GOT_OFF + 8) == ORIGINAL);
    free(img);
    printf("  failure-before-any-write-is-none: PASS\n");
}

static void test_failure_after_a_write_is_partial(void) {
    // The invariant that matters (#4). One slot is already rewritten when the
    // second write fails. Reporting this as a clean failure would let the
    // runtime try another route over a half-modified image.
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);
    writer_t w = {0, 2};   // fail on the second write
    hk_rebind_target_t t = make_target(img, &w);

    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);
    uint32_t written = 0;
    assert(hk_rebind_commit(&t, &plan, REPLACEMENT, NULL, &written) == HK_MUTATION_PARTIAL);
    assert(written == 1);
    assert(slot(img, GOT_OFF) == REPLACEMENT);    // the mixed state, reported
    assert(slot(img, GOT_OFF + 8) == ORIGINAL);
    free(img);
    printf("  failure-after-a-write-is-partial: PASS\n");
}

static void test_revalidation_refuses_a_changed_slot(void) {
    // Invariant #3. Something else changed the slot between prepare and
    // commit -- plausibly another hooking consumer. Writing anyway would
    // silently destroy their work.
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);

    // Changed before ANY write: nothing touched, so NONE.
    build_image(img);
    writer_t w = {0, 0};
    hk_rebind_target_t t = make_target(img, &w);
    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);
    put_u64(img, GOT_OFF, 0xDEAD);              // someone else got there first
    uint32_t written = 0;
    assert(hk_rebind_commit(&t, &plan, REPLACEMENT, NULL, &written) == HK_MUTATION_NONE);
    assert(written == 0 && w.calls == 0);
    assert(slot(img, GOT_OFF) == 0xDEAD);        // their value left intact

    // Changed on the SECOND slot only: the first is written before the
    // mismatch is seen, so the image is mixed and this is PARTIAL.
    build_image(img);
    writer_t w2 = {0, 0};
    hk_rebind_target_t t2 = make_target(img, &w2);
    assert(hk_rebind_prepare(&t2, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);
    put_u64(img, GOT_OFF + 8, 0xDEAD);
    assert(hk_rebind_commit(&t2, &plan, REPLACEMENT, NULL, &written) == HK_MUTATION_PARTIAL);
    assert(written == 1);
    assert(slot(img, GOT_OFF) == REPLACEMENT);
    assert(slot(img, GOT_OFF + 8) == 0xDEAD);
    free(img);
    printf("  revalidation-refuses-a-changed-slot: PASS\n");
}

static void test_absent_symbol_and_arguments(void) {
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);
    writer_t w = {0, 0};
    hk_rebind_target_t t = make_target(img, &w);
    hk_rebind_plan_t plan;

    assert(hk_rebind_prepare(&t, "nosuchsymbol", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_NOT_FOUND);
    // Exact mode does not add the underscore, so the bare name misses.
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_MACHO_EXACT, &plan) == HK_REBIND_NOT_FOUND);
    assert(hk_rebind_prepare(&t, "_malloc", HK_SYMBOL_NAME_MACHO_EXACT, &plan) == HK_REBIND_OK);

    assert(hk_rebind_prepare(NULL, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_INVALID_ARGUMENT);
    assert(hk_rebind_prepare(&t, NULL, HK_SYMBOL_NAME_C, &plan) == HK_REBIND_INVALID_ARGUMENT);
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, NULL) == HK_REBIND_INVALID_ARGUMENT);

    // A commit with no writer, or an empty plan, touches nothing.
    hk_rebind_target_t no_writer = t;
    no_writer.write = NULL;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) == HK_REBIND_OK);
    assert(hk_rebind_commit(&no_writer, &plan, REPLACEMENT, NULL, NULL) == HK_MUTATION_NONE);
    assert(slot(img, GOT_OFF) == ORIGINAL);

    hk_rebind_plan_t empty;
    memset(&empty, 0, sizeof(empty));
    assert(hk_rebind_commit(&t, &empty, REPLACEMENT, NULL, NULL) == HK_MUTATION_NONE);
    assert(hk_rebind_commit(NULL, &plan, REPLACEMENT, NULL, NULL) == HK_MUTATION_NONE);
    free(img);
    printf("  absent-symbol-and-arguments: PASS\n");
}

static void test_nonweak_null_slot_is_malformed(void) {
    uint8_t *img = (uint8_t *)aligned_alloc(64, IMG_SIZE);
    assert(img);
    build_image(img);
    put_u64(img, GOT_OFF, 0);
    writer_t w = {0, 0};
    hk_rebind_target_t t = make_target(img, &w);
    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&t, "malloc", HK_SYMBOL_NAME_C, &plan) ==
           HK_REBIND_MALFORMED_IMAGE);
    assert(w.calls == 0);
    free(img);
    printf("  nonweak-null-slot-is-malformed: PASS\n");
}

int main(void) {
    test_prepare_finds_sites_and_mutates_nothing();
    test_commit_writes_every_site();
    test_failure_before_any_write_is_none();
    test_failure_after_a_write_is_partial();
    test_revalidation_refuses_a_changed_slot();
    test_absent_symbol_and_arguments();
    test_nonweak_null_slot_is_malformed();
    printf("all rebind engine tests passed\n");
    return 0;
}
