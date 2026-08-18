// Host test for Sources/Engines/HKMemoryEngine.c. The write is behind a seam,
// so the parts that matter -- the masked precondition, revalidation before the
// write, and honest mutation state -- are all exercised here against a buffer.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Engines/HKMemoryEngine.h"
#include "../../Sources/Core/HKArtifactLedger.h"

#define REGION 4u
static const uint8_t ORIGINAL[REGION]    = {0x11, 0x22, 0x33, 0x44};
static const uint8_t REPLACEMENT[REGION] = {0xAA, 0xBB, 0xCC, 0xDD};

static bool buffer_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    (void)ctx;
    memcpy((void *)address, data, size);
    return true;
}
static bool refuse_write(void *ctx, uintptr_t a, const uint8_t *d, size_t s) {
    (void)ctx; (void)a; (void)d; (void)s;
    return false;
}

static hk_bytes_view_t view(const uint8_t *d, size_t n) {
    hk_bytes_view_t v; v.data = d; v.size = n; return v;
}
static const hk_bytes_view_t NONE_VIEW = {NULL, 0};

static void test_prepare_commit_writes_and_records(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    uintptr_t addr = (uintptr_t)region;

    hk_mempatch_plan_t plan;
    // Precondition matches, so preparation succeeds and captures the original.
    assert(hk_mempatch_prepare(addr, REGION, view(ORIGINAL, REGION), NONE_VIEW, &plan)
           == HK_MEMPATCH_OK);
    assert(plan.captured && plan.size == REGION);
    assert(memcmp(plan.original, ORIGINAL, REGION) == 0);
    // Prepare mutated nothing.
    assert(memcmp(region, ORIGINAL, REGION) == 0);

    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.ledger = ledger;

    assert(hk_mempatch_commit(addr, &plan, view(REPLACEMENT, REGION), buffer_write, NULL, &sink)
           == HK_MUTATION_COMPLETE);
    assert(memcmp(region, REPLACEMENT, REGION) == 0);

    // The artifact carries the region and the ORIGINAL bytes (for reversal).
    assert(hk_artifact_ledger_count(ledger) == 1);
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_MEMORY_PATCH);
    assert(a.effects == HK_EFFECT_MEMORY_MUTATION);
    assert(a.address == addr && a.size == REGION);
    assert(a.original_bytes.representation == HK_BYTE_STORAGE_INLINE);
    assert(a.original_bytes.inline_bytes.size == REGION);
    assert(memcmp(a.original_bytes.inline_bytes.data, ORIGINAL, REGION) == 0);
    assert(a.mechanically_reversible);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  prepare-commit-writes-and-records: PASS\n");
}

static void test_precondition_refuses_wrong_region(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    hk_mempatch_plan_t plan;

    // The caller asserts different bytes than are actually there -- likely a
    // wrong address, or a region already modified. Refuse, capture nothing.
    uint8_t wrong[REGION] = {0x11, 0x22, 0x33, 0x99};
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(wrong, REGION), NONE_VIEW, &plan)
           == HK_MEMPATCH_PRECONDITION_FAILED);
    assert(!plan.captured);

    // And commit on that un-captured plan touches nothing.
    assert(hk_mempatch_commit((uintptr_t)region, &plan, view(REPLACEMENT, REGION),
                              buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(memcmp(region, ORIGINAL, REGION) == 0);
    printf("  precondition-refuses-wrong-region: PASS\n");
}

static void test_mask_selects_which_bytes_matter(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    hk_mempatch_plan_t plan;

    // expected differs from the region ONLY in the last byte...
    uint8_t expected[REGION] = {0x11, 0x22, 0x33, 0xFF};
    // ...but a mask of 0 on that byte excludes it from the comparison -> OK.
    uint8_t mask_ignore_last[REGION] = {0xFF, 0xFF, 0xFF, 0x00};
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(expected, REGION),
                               view(mask_ignore_last, REGION), &plan) == HK_MEMPATCH_OK);

    // A mask that INCLUDES the differing byte fails.
    uint8_t mask_full[REGION] = {0xFF, 0xFF, 0xFF, 0xFF};
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(expected, REGION),
                               view(mask_full, REGION), &plan) == HK_MEMPATCH_PRECONDITION_FAILED);

    // Nibble-level: expect 0x4X but region has 0x44; mask 0xF0 compares only
    // the high nibble, which matches.
    uint8_t expect_hi[REGION] = {0x11, 0x22, 0x33, 0x4A};
    uint8_t mask_hi[REGION]   = {0xFF, 0xFF, 0xFF, 0xF0};
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(expect_hi, REGION),
                               view(mask_hi, REGION), &plan) == HK_MEMPATCH_OK);
    printf("  mask-selects-which-bytes-matter: PASS\n");
}

static void test_revalidation_refuses_a_changed_region(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    hk_mempatch_plan_t plan;
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(ORIGINAL, REGION), NONE_VIEW, &plan)
           == HK_MEMPATCH_OK);

    // Something else changed the region between prepare and commit.
    region[0] = 0xEE;
    assert(hk_mempatch_commit((uintptr_t)region, &plan, view(REPLACEMENT, REGION),
                              buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    // Their value is left intact, not overwritten.
    assert(region[0] == 0xEE && region[1] == 0x22);
    printf("  revalidation-refuses-a-changed-region: PASS\n");
}

static void test_write_refusal_and_size_mismatch(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    hk_mempatch_plan_t plan;
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, NONE_VIEW, NONE_VIEW, &plan)
           == HK_MEMPATCH_OK);  // no precondition: just capture

    // The store refuses -> NONE, region untouched.
    assert(hk_mempatch_commit((uintptr_t)region, &plan, view(REPLACEMENT, REGION),
                              refuse_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(memcmp(region, ORIGINAL, REGION) == 0);

    // A replacement that is not exactly the prepared size is refused: writing
    // it would make the artifact's original-bytes record a different region.
    uint8_t short_repl[2] = {0xAA, 0xBB};
    assert(hk_mempatch_commit((uintptr_t)region, &plan, view(short_repl, 2),
                              buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(memcmp(region, ORIGINAL, REGION) == 0);
    printf("  write-refusal-and-size-mismatch: PASS\n");
}

static void test_argument_validation(void) {
    uint8_t region[REGION];
    memcpy(region, ORIGINAL, REGION);
    hk_mempatch_plan_t plan;

    assert(hk_mempatch_prepare(0, REGION, NONE_VIEW, NONE_VIEW, &plan) == HK_MEMPATCH_INVALID_ARGUMENT);
    assert(hk_mempatch_prepare((uintptr_t)region, 0, NONE_VIEW, NONE_VIEW, &plan) == HK_MEMPATCH_INVALID_ARGUMENT);
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, NONE_VIEW, NONE_VIEW, NULL) == HK_MEMPATCH_INVALID_ARGUMENT);
    assert(hk_mempatch_prepare((uintptr_t)region, HK_MEMPATCH_MAX + 1, NONE_VIEW, NONE_VIEW, &plan) == HK_MEMPATCH_TOO_LARGE);

    // expected present but the wrong length, or a mask that mismatches expected.
    uint8_t two[2] = {0x11, 0x22};
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(two, 2), NONE_VIEW, &plan)
           == HK_MEMPATCH_INVALID_ARGUMENT);
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, view(ORIGINAL, REGION), view(two, 2), &plan)
           == HK_MEMPATCH_INVALID_ARGUMENT);

    // commit guards.
    assert(hk_mempatch_prepare((uintptr_t)region, REGION, NONE_VIEW, NONE_VIEW, &plan) == HK_MEMPATCH_OK);
    assert(hk_mempatch_commit(0, &plan, view(REPLACEMENT, REGION), buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(hk_mempatch_commit((uintptr_t)region, NULL, view(REPLACEMENT, REGION), buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(hk_mempatch_commit((uintptr_t)region, &plan, view(REPLACEMENT, REGION), NULL, NULL, NULL) == HK_MUTATION_NONE);
    printf("  argument-validation: PASS\n");
}

int main(void) {
    test_prepare_commit_writes_and_records();
    test_precondition_refuses_wrong_region();
    test_mask_selects_which_bytes_matter();
    test_revalidation_refuses_a_changed_region();
    test_write_refusal_and_size_mismatch();
    test_argument_validation();
    printf("all memory engine tests passed\n");
    return 0;
}
