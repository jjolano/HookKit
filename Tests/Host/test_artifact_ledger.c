// Host test for Sources/Core/HKArtifactLedger.c (the write side + the
// immutable snapshot read side) and HKReport.c's hk_report_copy_artifacts.
// Exercises the ledger directly -- no engine populates it yet (that is the
// next commit), so this is the only thing proving the append/snapshot
// plumbing actually works, growth included.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKArtifactLedger.h"
#include "../../Sources/Core/HKReportInternal.h"

// A distinct artifact per marker, so a snapshot can prove ordering and
// per-record integrity survived a realloc.
static hk_artifact_t make_marked_artifact(uint64_t marker) {
    hk_artifact_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(a);
    a.struct_version = HK_ABI_VERSION_3_0;
    a.artifact_id.high = 0xABCD;
    a.artifact_id.low = marker;
    a.kind = HK_ARTIFACT_IMPORT_SLOT;
    a.state = HK_ARTIFACT_COMMITTED;
    a.effects = HK_EFFECT_IMPORT_MUTATION;
    a.address = (uintptr_t)(0x1000 + marker);  // unique per record
    return a;
}

static void test_empty_ledger_snapshot(void) {
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    assert(ledger != NULL);
    assert(hk_artifact_ledger_count(ledger) == 0);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    assert(snap != NULL);
    assert(hk_artifact_snapshot_count(snap) == 0);

    hk_artifact_t out;
    memset(&out, 0xEE, sizeof(out));
    assert(hk_artifact_snapshot_copy_at(snap, 0, &out) == HK_STATUS_INVALID_ARGUMENT);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  empty-ledger-snapshot: PASS\n");
}

static void test_append_and_snapshot(void) {
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    assert(ledger != NULL);
    for (uint64_t i = 0; i < 3; i++) {
        hk_artifact_t a = make_marked_artifact(i);
        assert(hk_artifact_ledger_append(ledger, &a));
    }
    assert(hk_artifact_ledger_count(ledger) == 3);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 3);

    for (uint64_t i = 0; i < 3; i++) {
        hk_artifact_t out;
        assert(hk_artifact_snapshot_copy_at(snap, (size_t)i, &out) == HK_STATUS_OK);
        assert(out.artifact_id.low == i);            // order preserved
        assert(out.artifact_id.high == 0xABCD);
        assert(out.address == (uintptr_t)(0x1000 + i));
        assert(out.kind == HK_ARTIFACT_IMPORT_SLOT);
        assert(out.state == HK_ARTIFACT_COMMITTED);
        assert(out.effects == HK_EFFECT_IMPORT_MUTATION);
        assert(out.struct_size == sizeof(hk_artifact_t));
    }

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  append-and-snapshot: PASS\n");
}

static void test_snapshot_independent_of_later_appends(void) {
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    assert(ledger != NULL);
    for (uint64_t i = 0; i < 2; i++) {
        hk_artifact_t a = make_marked_artifact(i);
        assert(hk_artifact_ledger_append(ledger, &a));
    }

    hk_artifact_snapshot_t *early = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &early) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(early) == 2);

    // Grow the ledger AFTER the snapshot -- the snapshot must not change.
    hk_artifact_t more = make_marked_artifact(99);
    assert(hk_artifact_ledger_append(ledger, &more));
    assert(hk_artifact_ledger_count(ledger) == 3);
    assert(hk_artifact_snapshot_count(early) == 2);  // still the old view

    hk_artifact_snapshot_t *late = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &late) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(late) == 3);

    // Destroying the ledger must not invalidate a snapshot already taken.
    hk_artifact_ledger_destroy(ledger);
    hk_artifact_t out;
    assert(hk_artifact_snapshot_copy_at(early, 1, &out) == HK_STATUS_OK);
    assert(out.artifact_id.low == 1);
    assert(hk_artifact_snapshot_copy_at(late, 2, &out) == HK_STATUS_OK);
    assert(out.artifact_id.low == 99);

    hk_artifact_snapshot_release(early);
    hk_artifact_snapshot_release(late);
    printf("  snapshot-independent-of-later-appends: PASS\n");
}

static void test_growth_many(void) {
    // Enough appends to force several reallocs (cap doubles 4->8->16->32->64).
    const uint64_t n = 50;
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    assert(ledger != NULL);
    for (uint64_t i = 0; i < n; i++) {
        hk_artifact_t a = make_marked_artifact(i);
        assert(hk_artifact_ledger_append(ledger, &a));
    }
    assert(hk_artifact_ledger_count(ledger) == n);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == n);
    for (uint64_t i = 0; i < n; i++) {
        hk_artifact_t out;
        assert(hk_artifact_snapshot_copy_at(snap, (size_t)i, &out) == HK_STATUS_OK);
        assert(out.artifact_id.low == i);                 // no reordering/corruption
        assert(out.address == (uintptr_t)(0x1000 + i));
    }

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  growth-many: PASS\n");
}

static void test_copy_at_out_of_range_leaves_out_untouched(void) {
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    assert(ledger != NULL);
    hk_artifact_t a = make_marked_artifact(7);
    assert(hk_artifact_ledger_append(ledger, &a));

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 1);

    hk_artifact_t out;
    memset(&out, 0x5A, sizeof(out));
    assert(hk_artifact_snapshot_copy_at(snap, 1, &out) == HK_STATUS_INVALID_ARGUMENT);
    assert(hk_artifact_snapshot_copy_at(snap, 999, &out) == HK_STATUS_INVALID_ARGUMENT);
    // Untouched on failure: still the 0x5A sentinel byte pattern.
    unsigned char *bytes = (unsigned char *)&out;
    for (size_t i = 0; i < sizeof(out); i++) {
        assert(bytes[i] == 0x5A);
    }

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    printf("  copy-at-out-of-range-leaves-out-untouched: PASS\n");
}

static void test_null_tolerance(void) {
    hk_artifact_t a = make_marked_artifact(0);

    assert(!hk_artifact_ledger_append(NULL, &a));
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    assert(ledger != NULL);
    assert(!hk_artifact_ledger_append(ledger, NULL));
    assert(hk_artifact_ledger_count(ledger) == 0);
    assert(hk_artifact_ledger_count(NULL) == 0);

    // NULL ledger snapshots to a valid empty snapshot, not an error.
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(NULL, &snap) == HK_STATUS_OK);
    assert(snap != NULL && hk_artifact_snapshot_count(snap) == 0);
    hk_artifact_snapshot_release(snap);

    // NULL out-param is the one from_ledger error.
    assert(hk_artifact_snapshot_from_ledger(ledger, NULL) == HK_STATUS_INVALID_ARGUMENT);

    assert(hk_artifact_snapshot_count(NULL) == 0);
    hk_artifact_t out;
    assert(hk_artifact_snapshot_copy_at(NULL, 0, &out) == HK_STATUS_INVALID_ARGUMENT);

    hk_artifact_snapshot_release(NULL);  // must not crash
    hk_artifact_ledger_destroy(NULL);    // must not crash
    hk_artifact_ledger_destroy(ledger);
    printf("  null-tolerance: PASS\n");
}

static void test_report_copy_artifacts_empty(void) {
    // A real report (from the internal constructor) carries an empty ledger
    // until the commit path populates it -- prove the public read path
    // reaches it and returns a valid empty snapshot, and its arg checks.
    hk_report_t *report = hk_report_create(NULL, 0);
    assert(report != NULL);

    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(snap != NULL && hk_artifact_snapshot_count(snap) == 0);
    hk_artifact_snapshot_release(snap);

    hk_artifact_snapshot_t *sentinel = (hk_artifact_snapshot_t *)(void *)0x1;
    assert(hk_report_copy_artifacts(NULL, &sentinel) == HK_STATUS_INVALID_ARGUMENT);
    assert(sentinel == NULL);  // cleared even on the error path
    assert(hk_report_copy_artifacts(report, NULL) == HK_STATUS_INVALID_ARGUMENT);

    hk_report_release(report);
    printf("  report-copy-artifacts-empty: PASS\n");
}

int main(void) {
    test_empty_ledger_snapshot();
    test_append_and_snapshot();
    test_snapshot_independent_of_later_appends();
    test_growth_many();
    test_copy_at_out_of_range_leaves_out_untouched();
    test_null_tolerance();
    test_report_copy_artifacts_empty();
    printf("all artifact ledger tests passed\n");
    return 0;
}
