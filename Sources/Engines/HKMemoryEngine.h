// Memory-patch engine -- Milestone 6. Writes a controlled byte sequence over
// a target region (a HK_TARGET_MEMORY_PATCH request). Same shape as the rebind
// engine: two phases with the write behind a device seam, so all of the
// decision-making is host-testable and only the store is device-only.
//
// The correctness content is the two checks that bracket the write, both of
// which exist to keep a patch from silently corrupting something:
//   - the caller-supplied PRECONDITION (expected_bytes under expected_mask,
//     spec 6.19): prepare refuses if the region does not already hold what the
//     caller asserts is there, so a patch aimed at the wrong address, or at a
//     region already modified, fails loudly instead of clobbering it.
//   - REVALIDATION before the write (ARCHITECTURE.md invariant #3): commit
//     re-reads the region and refuses if it changed since prepare -- e.g.
//     another consumer touched it in between.
//
// expected_mask affects only the precondition comparison ((current & mask) ==
// (expected & mask)); replacement_bytes is always written in full. A caller
// wanting to preserve some bits must reflect them in replacement_bytes.
//
// Reuse survey: 2.x patches memory via native/hk_native.c's
// hk_native_patch_memory (VM protection change + write, with arm64e handling).
// That is the reference for the device seam, not reusable here: it is the
// store itself, device-only, and carries none of the precondition /
// revalidation / artifact contract this engine exists to provide. On device
// it is exactly what backs hk_mempatch_write_fn.

#ifndef HK_ENGINES_MEMORY_H
#define HK_ENGINES_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../Core/HKArtifactLedger.h"
#include "../../Headers/HookKit/HookKitBase.h"  // hk_bytes_view_t

#ifdef __cplusplus
extern "C" {
#endif

// A hook-target patch is a handful of instructions; the cap is fixed so the
// engine allocates nothing, and a larger region is refused rather than
// truncated.
#define HK_MEMPATCH_MAX 256u

typedef enum {
    HK_MEMPATCH_OK = 0,
    HK_MEMPATCH_INVALID_ARGUMENT,
    HK_MEMPATCH_TOO_LARGE,          // region exceeds HK_MEMPATCH_MAX
    HK_MEMPATCH_PRECONDITION_FAILED, // region does not hold expected_bytes
} hk_mempatch_status_t;

// The device-only store: writes `size` bytes of `data` at `address`. Returns
// false if the store could not be performed (protection change refused, ...).
// Modeled as all-or-nothing; a device implementation that can write a region
// partially must report that to its caller so commit can return PARTIAL.
typedef bool (*hk_mempatch_write_fn)(void *ctx, uintptr_t address,
                                     const uint8_t *data, size_t size);

typedef struct {
    uint8_t original[HK_MEMPATCH_MAX];  // bytes read during prepare
    size_t size;
    bool captured;
} hk_mempatch_plan_t;

// Phase 1. Reads the region and enforces the precondition. Mutates nothing.
// `expected.data == NULL` skips the precondition (the legacy path captures the
// original at prepare instead of asserting it). A non-NULL `mask` must be the
// same length as `expected`; a NULL mask means compare every bit.
hk_mempatch_status_t hk_mempatch_prepare(uintptr_t address, size_t size,
                                         hk_bytes_view_t expected,
                                         hk_bytes_view_t mask,
                                         hk_mempatch_plan_t *out_plan);

// Phase 2. Revalidates the region against what prepare captured, then writes
// `replacement` (which must be exactly `plan->size` bytes). `sink` may be NULL.
//
// Returns:
//   NONE      nothing written -- revalidation failed, or the store refused
//             before touching anything. A clean refusal.
//   COMPLETE  the region was written.
// PARTIAL is not produced by a single-region patch here; see the seam note.
hk_mutation_state_t hk_mempatch_commit(uintptr_t address,
                                       const hk_mempatch_plan_t *plan,
                                       hk_bytes_view_t replacement,
                                       hk_mempatch_write_fn write, void *write_ctx,
                                       hk_artifact_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_MEMORY_H
