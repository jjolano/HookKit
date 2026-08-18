// Internal artifact ledger -- Milestone 4's "Artifact ledger" task.
//
// The ledger is the mutable, append-only internal collection an owner (a
// report today; a runtime/process later) accumulates hk_artifact_t records
// into as engines commit mutations. The public read path never sees the
// ledger directly: callers get an immutable hk_artifact_snapshot_t (spec
// section 7.5 -- a deep, independent copy taken at a point in time, so the
// ledger growing afterwards cannot mutate a snapshot already handed out).
//
// This header is the WRITE side plus the snapshot bridge. The public READ
// side (hk_artifact_snapshot_count/copy_at/release) is declared in
// HookKitArtifacts.h and implemented in HKArtifactLedger.c against the
// snapshot type defined there.
//
// Not public API: no caller outside Sources/Core/*.c touches the ledger.
//
// Ownership honesty (matches the hk_report_t precedent in
// HKReportInternal.h): both append and snapshot copy hk_artifact_t BY
// VALUE. That is a full, correct copy of every inline/scalar field -- which
// is the bulk of the struct -- but the struct's borrowed-pointer fields
// (image.path, the engine_id/mechanism_id string views, and the *_bytes
// inline_bytes views) are SHARED, not deep-copied. No engine populates the
// ledger yet (Commit 2 of this feature wires that up, using static string
// literals), so nothing with a non-static lifetime flows through here today.
// The day a real engine (Milestone 6+) records an artifact carrying a
// dynamically-allocated string or byte buffer, owned-copy machinery has to
// land here -- that is the precise trigger, deferred until there is a
// producer to need it rather than built speculatively now.

#ifndef HK_CORE_ARTIFACT_LEDGER_H
#define HK_CORE_ARTIFACT_LEDGER_H

#include <stdbool.h>
#include <stddef.h>

#include "../../Headers/HookKit/HookKitArtifacts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_artifact_ledger hk_artifact_ledger_t;

// The recorder an engine's commit_one writes artifacts into. The engine
// fills only the MECHANISM facts it actually knows -- kind, effects,
// engine_id/mechanism_id, address/size/bytes, the mechanism-specific
// pointers, protection, reversibility. It has no access to the CONTEXTUAL
// identity of the operation (it only receives an hk_hook_spec_t, not the
// plan/hook/runtime), so hk_artifact_sink_record stamps those centrally: a
// fresh artifact_id plus the plan_id/request_id/runtime_owner_id the commit
// path holds. This split is exactly why the sink exists rather than handing
// the engine the raw ledger -- an engine must not (and cannot) forge those
// IDs itself. plan_id/runtime_owner_id are fixed for a commit; request_id
// is updated by the commit loop before each engine call.
typedef struct {
    hk_artifact_ledger_t *ledger;
    hk_id_t plan_id;
    hk_id_t runtime_owner_id;
    hk_id_t request_id;

    // Engine OUTPUT for the current hook: the original/continuation pointer
    // it preserved, or NULL if it produced none. The commit loop resets
    // this to NULL before each hook and reads it after commit_one to build
    // the hook's original slot (HKInstalled.h). Distinct from the artifact
    // channel: the artifact is the inspectable record, this is the live
    // pointer a replacement will actually load through.
    void *published_original;
} hk_artifact_sink_t;

// Stamps the contextual IDs onto a copy of *artifact (overwriting any the
// engine left set -- the sink is authoritative for those four fields) and
// appends it to the sink's ledger. Returns false on a NULL argument or on
// the ledger's OOM. A false return means "not recorded", never "the
// mutation did not happen"; how a production engine reacts to losing a
// record (its mutation is real but now untracked) is deferred until a real
// engine exists -- fake engines here cannot OOM.
bool hk_artifact_sink_record(hk_artifact_sink_t *sink, const hk_artifact_t *artifact);

// Creates an empty ledger. NULL on OOM.
hk_artifact_ledger_t *hk_artifact_ledger_create(void);

void hk_artifact_ledger_destroy(hk_artifact_ledger_t *ledger);

// Appends one artifact (copied by value -- see the ownership note above).
// Returns false on OOM ONLY; a false return means "the ledger could not
// record this artifact", never "the artifact/mutation did not happen" --
// the caller (the commit path) must treat an untracked-but-real mutation as
// exactly the kind of not-fully-known state HK_MUTATION_UNKNOWN describes,
// not silently drop it.
bool hk_artifact_ledger_append(hk_artifact_ledger_t *ledger,
                               const hk_artifact_t *artifact);

size_t hk_artifact_ledger_count(const hk_artifact_ledger_t *ledger);

// Deep-copies the ledger's current contents into a fresh immutable
// snapshot (spec section 7.5). The snapshot is independent of the ledger:
// later appends do not change it. Out-param set to a new snapshot the
// caller must release with hk_artifact_snapshot_release. A NULL or empty
// ledger yields a valid snapshot of count 0, not an error. Returns
// HK_STATUS_OUT_OF_MEMORY (out untouched) if the copy cannot be allocated.
hk_status_t hk_artifact_snapshot_from_ledger(const hk_artifact_ledger_t *ledger,
                                             hk_artifact_snapshot_t **out_snapshot);

#ifdef __cplusplus
}
#endif

#endif // HK_CORE_ARTIFACT_LEDGER_H
