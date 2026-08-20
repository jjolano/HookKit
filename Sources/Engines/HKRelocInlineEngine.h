// Native RELOCATING inline engine -- Milestone 8. Overwrites a function's
// entry with a branch, and preserves the displaced prologue in a trampoline so
// the original stays callable.
//
// Reach: HK_REACH_ENTRYPOINT, same as the terminal engine. The difference is
// not reach, it is the original: terminal inline serves HK_ORIGINAL_NONE only,
// this one serves HK_ORIGINAL_CALLABLE_CONTINUATION. That is the entire reason
// to pay its costs.
//
// WHAT IT COSTS, stated against Milestone 7's list of what terminal inline
// buys by refusing to do this:
//   - the displaced instructions are relocated, so PC-relative forms must be
//     rewritten and anything unrelocatable is fatal;
//   - an executable page is allocated, so there is a resource to seal, leak,
//     and account for;
//   - the install is TWO writes, not one (seal the trampoline, then patch the
//     entry), which is what makes the phase split below load-bearing.
//
// THE PHASE SPLIT, and why it differs from the terminal engine's:
//   prepare  allocates and seals the trampoline. That is an executable
//            allocation -- a non-target effect, which ARCHITECTURE.md
//            invariant #2 permits at prepare (prepare is forbidden from
//            mutating a TARGET, not from having declared effects). It is also
//            what makes the original publishable before any replacement is
//            reachable (invariant #5): the sealed trampoline IS the original,
//            and it exists before anything branches anywhere.
//   commit   revalidates the entry against what prepare read, then patches it.
//            One aligned store where possible -- see the thunk below.
//
// THE INBOUND THUNK IS NOT AN OPTIMIZATION. The trampoline page carries, at
// its front, an absolute jump to the replacement; the entry patch branches
// THERE rather than directly at the replacement. The point is that a `B` to a
// nearby thunk is 4 bytes -- one aligned store, and therefore atomic against a
// thread entering the function mid-patch. Branching straight to a far
// replacement needs a 16-byte sequence, which a thread can enter halfway
// through. The thunk is used whenever the page landed within a `B`'s reach;
// when it did not, the 16-byte form is the honest fallback and the torn-patch
// window is real. Reported, not hidden: the plan records which form was used.
//
// TWO DEVICE-ONLY SEAMS, and they are narrower than "the trampoline":
//   hk_reloc_alloc_fn   obtain an executable page (vm_allocate on device)
//   hk_reloc_seal_fn    R-W -> R-X once the page is built (vm_protect)
// Everything else -- relocation, layout, thunk and jump-back emission,
// preflight, revalidation -- is arithmetic over buffers and runs on the host.
// The relocation core needed no adaptation at all: hk_arm64_relocate already
// writes into a caller-provided buffer and resolves every rewrite to an
// absolute address, so where the trampoline lands does not affect it.
//
// Two device-only facts about that page, recorded because a host cannot check
// them and this engine's seam contract depends on them (see
// native/hk_native.c, which learned them the hard way): each trampoline wants
// its OWN page, because flipping a shared arena to R-W strips EXECUTE from
// already-published trampolines and faults any thread running one; and a
// permanently R-W-X page does not work on iOS -- vm_protect(RWX) returns
// KERN_SUCCESS and the first instruction fetch then dies. The R-W -> R-X
// transition is the one iOS honours, which is why sealing is its own seam
// rather than a flag on the allocation.

#ifndef HK_ENGINES_RELOC_INLINE_H
#define HK_ENGINES_RELOC_INLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../Headers/HookKit/HookKitResults.h"
#include "../Core/HKArtifactLedger.h"
#include "../../native/hk_arm64.h"

#ifdef __cplusplus
extern "C" {
#endif

// Trampoline layout, matching 2.x's so the two agree on what a page holds:
//   [0 .. THUNK)  inbound thunk -- an absolute jump to the replacement
//   [THUNK .. )   body -- the relocated prologue followed by a jump back
#define HK_RELOC_THUNK_BYTES HK_A64_MAX_BRANCH_BYTES
// Worst case: 4 relocated instructions at 24 bytes each, plus a 16-byte jump
// back. The 4 is the most instructions an entry patch can displace.
#define HK_RELOC_MAX_DISPLACED 4u
#define HK_RELOC_BODY_BYTES (HK_RELOC_MAX_DISPLACED * HK_A64_MAX_RELOC_BYTES + \
                             HK_A64_MAX_BRANCH_BYTES)
#define HK_RELOC_PAGE_BYTES (HK_RELOC_THUNK_BYTES + HK_RELOC_BODY_BYTES)

// The longest entry patch, same as the terminal engine's.
#define HK_RELOC_MAX_PATCH HK_A64_MAX_BRANCH_BYTES

typedef enum {
    HK_RELOC_OK = 0,
    HK_RELOC_INVALID_ARGUMENT,
    HK_RELOC_MISALIGNED,
    // A displaced instruction ends the function. Unlike terminal inline, this
    // is fatal wherever it appears in the window -- the relocated copy would
    // return or branch away from the middle of the trampoline body.
    HK_RELOC_TARGET_TOO_SHORT,
    HK_RELOC_TRAP_STUB,
    HK_RELOC_PRECONDITION_FAILED,
    // A displaced instruction cannot be rewritten to run from a new address,
    // or its branch target lands inside the bytes being overwritten.
    HK_RELOC_UNRELOCATABLE,
    // The page seam refused, or handed back something unusable.
    HK_RELOC_NO_TRAMPOLINE,
} hk_reloc_status_t;

// Obtain an executable page of `size` bytes. `near` is a placement HINT, not a
// requirement -- landing within a B's reach of it lets the entry patch be a
// single atomic store, and failing to is handled rather than fatal. Returns 0
// on failure.
typedef uintptr_t (*hk_reloc_alloc_fn)(void *ctx, size_t size, uintptr_t near);
// Transition the page from writable to executable. Returns false if refused.
typedef bool (*hk_reloc_seal_fn)(void *ctx, uintptr_t page, size_t size);
// Give a page back. Called ONLY for a page nothing can be executing: one whose
// preparation failed after allocating, or whose entry patch never landed. A
// page reached by a live entry patch is never passed here -- reclaiming that
// would free code a thread may be inside. On device this is vm_deallocate.
typedef void (*hk_reloc_free_fn)(void *ctx, uintptr_t page, size_t size);
// Write the entry patch. Same shape as the terminal engine's seam.
typedef bool (*hk_reloc_write_fn)(void *ctx, uintptr_t address,
                                  const uint8_t *data, size_t size);

typedef struct {
    uintptr_t address;                     // the entry being patched
    uintptr_t trampoline;                  // the page, as handed back
    size_t trampoline_size;
    // What a caller invokes to reach the original: the body, not the page.
    uintptr_t original_entry;
    uint8_t original[HK_RELOC_MAX_PATCH];  // bytes the patch replaces
    uint8_t patch[HK_RELOC_MAX_PATCH];
    size_t patch_size;                     // 4 or 16
    uint32_t displaced_count;              // instructions moved into the body
    // True when the entry patch is a single 4-byte B (via the thunk), and so
    // an atomic aligned store. False means a 16-byte sequence, which a thread
    // can enter part-written -- surfaced rather than hidden.
    bool atomic_entry_patch;
    bool captured;
} hk_reloc_plan_t;

// Phase 1. Allocates and seals the trampoline, relocates the prologue into it,
// and works out the entry patch. Does NOT touch the target.
// `free_page` may be NULL, but then a preparation that fails after the page is
// allocated LEAKS it -- which is what ASan caught when this parameter did not
// exist. Supply it.
hk_reloc_status_t hk_reloc_prepare(uintptr_t target, uintptr_t replacement,
                                   const uint8_t *expected_initial_bytes,
                                   size_t expected_size,
                                   hk_reloc_alloc_fn alloc, hk_reloc_seal_fn seal,
                                   hk_reloc_free_fn free_page, void *seam_ctx,
                                   hk_reloc_plan_t *out_plan);

// Phase 2. Revalidates the entry against what prepare read, then patches it.
// Records HK_ARTIFACT_TARGET_TEXT_PATCH and, for the page,
// HK_ARTIFACT_TRAMPOLINE. `sink` may be NULL.
// If the patch does not land, the trampoline is RECLAIMED via `free_page`
// rather than left behind: nothing branches to it, so nothing can be executing
// in it, and keeping it would be a leaked executable page for a hook that
// never happened. Nothing is recorded either -- MUTATION_NONE with no
// artifacts is the honest report when nothing persists. `free_page` may be
// NULL, in which case the page is kept and an artifact IS recorded, so it
// stays accounted for even though it cannot be reclaimed.
hk_mutation_state_t hk_reloc_commit(const hk_reloc_plan_t *plan,
                                    hk_reloc_write_fn write, void *write_ctx,
                                    hk_reloc_free_fn free_page, void *seam_ctx,
                                    hk_artifact_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_RELOC_INLINE_H
