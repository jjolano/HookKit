// Native TERMINAL inline engine -- Milestone 7. Overwrites a function's entry
// with a branch to a replacement, and stops there: the original body is not
// preserved, relocated, or reachable.
//
// Reach: HK_REACH_ENTRYPOINT. Patching the entry catches every call that
// reaches the function, however it got there -- a direct intra-image call, a
// call through an import slot, a computed call through a saved pointer. That
// is strictly more reach than the rebind engine gets, and it is the reason to
// pay the costs below.
//
// WHAT "TERMINAL" BUYS, and why it is a separate milestone from the
// relocating one (spec section 13.4: zero relocation, zero trampoline, zero
// executable allocation for strict requests). Because nothing ever executes
// the overwritten prologue again:
//   - no instruction needs relocating, so the relocation-fragility checks a
//     relocating backend must apply DO NOT apply here (see below);
//   - no trampoline is allocated, so there is no executable allocation, no
//     page to manage, and nothing to leak;
//   - the only artifact is the patch itself.
// The price is exact and non-negotiable: there is no original. This engine
// serves HK_ORIGINAL_NONE and refuses everything else, rather than quietly
// allocating a trampoline to satisfy a continuation request -- which would be
// precisely the hidden fallback the whole design forbids.
//
// THE DIFFERENCE FROM THE RELOCATING ENGINE, stated because it is the single
// most likely thing to get wrong: `hk_arm64_has_aarch64_literal_load`
// exists to refuse ADR/ADRP and load-literal forms whose RELOCATION is fragile.
// Terminal inline relocates nothing, so a literal load inside the overwrite
// window is harmless -- it is being replaced and will never run. Applying that
// check here would decline safe targets for a reason that does not exist in
// this mechanism. It is deliberately NOT applied.
//
// What DOES still apply is overrun: if the target function ends inside the
// window we are about to overwrite, the tail of the branch lands in whatever
// follows it -- usually the next function. That is caught with the terminator
// scan, but with a bound the relocating backends do not use. A terminator at
// the LAST instruction of the window is fine: the window ends exactly where
// the function does, and nothing beyond it is touched. Only a terminator
// EARLIER than that means overrun. See hk_inline_prepare.
//
// THE WRITE IS THE ONLY DEVICE-ONLY PART, and it is behind a seam, as in the
// other three engines. On device it must change VM protection, write, restore,
// and invalidate the instruction cache; none of that can run or be verified
// here. Everything else -- branch sizing, encoding, preflight, revalidation --
// is arithmetic over buffers and runs on the host.
//
// Reuse survey: the mechanism core is native/hk_arm64.{h,c}, reused as-is and
// not reimplemented. That file states in its own header that it is free of
// Mach/Darwin dependencies specifically so it can be unit-tested on the build
// host, and `make test`'s test-reloc already compiles and runs it here -- so
// unlike native/hk_swift.c (arm64-gated device code, hence the deferred Swift
// engine) there was nothing to port. What is NOT reused is
// Internal/HKInlinePreflight.m: it is Objective-C and its central check is
// hk_native_range_executable, a live-process VM query that is device-only by
// nature. The instruction-inspection half of preflight it delegates to lives
// in hk_arm64.c and is used directly here.

#ifndef HK_ENGINES_INLINE_H
#define HK_ENGINES_INLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../Headers/HookKit/HookKitResults.h"
#include "../Core/HKArtifactLedger.h"
#include "../../native/hk_arm64.h"

#ifdef __cplusplus
extern "C" {
#endif

// The longest patch this engine can write: hk_arm64_emit_branch's worst case.
#define HK_INLINE_MAX_PATCH HK_A64_MAX_BRANCH_BYTES

typedef enum {
    HK_INLINE_OK = 0,
    HK_INLINE_INVALID_ARGUMENT,
    // A64 instructions are 4-byte aligned; a misaligned entry is not a
    // function entry, and patching it would corrupt two instructions.
    HK_INLINE_MISALIGNED,
    // The function ends inside the overwrite window, so the patch would run
    // past it into whatever follows.
    HK_INLINE_TARGET_TOO_SHORT,
    // The entry instruction is BRK/HLT/UDF. The shared cache builds private-API
    // stubs this way; the "original" is a trap, so hooking it is never
    // meaningful and is refused before anything is written.
    HK_INLINE_TRAP_STUB,
    // expected_initial_bytes was supplied and does not match what is there.
    HK_INLINE_PRECONDITION_FAILED,
    // The request wants an original this mechanism cannot provide. Refused
    // rather than silently upgraded to a trampoline-allocating hook.
    HK_INLINE_NEEDS_CONTINUATION,
} hk_inline_status_t;

// The one device-only operation. Returns false if the store could not be
// performed (protection change refused, address not writable, ...).
typedef bool (*hk_inline_write_fn)(void *ctx, uintptr_t address,
                                   const uint8_t *data, size_t size);

typedef struct {
    uintptr_t address;                    // where the patch goes
    uint8_t original[HK_INLINE_MAX_PATCH];  // read at prepare, before any write
    uint8_t patch[HK_INLINE_MAX_PATCH];     // the branch, encoded at prepare
    size_t size;                          // 4 or 16; covers both buffers
    bool captured;
} hk_inline_plan_t;

// Phase 1. Sizes and encodes the branch, reads the bytes it will replace, and
// preflights them. MUTATES NOTHING (ARCHITECTURE.md invariant #2).
//
// `expected_initial_bytes` may be NULL to skip the precondition; when given,
// `expected_size` bytes at the target must match. It is compared against what
// is actually there BEFORE any decision is final, so a caller that pinned the
// prologue gets a clean refusal rather than a patch over an unexpected body.
hk_inline_status_t hk_inline_prepare(uintptr_t target, uintptr_t replacement,
                                     hk_original_requirement_t original_requirement,
                                     const uint8_t *expected_initial_bytes,
                                     size_t expected_size,
                                     hk_inline_plan_t *out_plan);

// Phase 2. Revalidates the target against what prepare captured (invariant
// #3), then writes.
//
// Returns the honest mutation state:
//   NONE      nothing was written -- a clean refusal
//   COMPLETE  the branch was written
// There is no PARTIAL: the patch is one store of 4 or 16 bytes, and a store
// that fails leaves nothing behind. (A device writer that could tear would
// have to report that itself; the seam returns a bool, so a torn write is not
// representable here and would be a device-side bug, not a state this engine
// can produce.)
//
// `sink` may be NULL; when present, one HK_ARTIFACT_TARGET_TEXT_PATCH is
// recorded carrying the original bytes.
hk_mutation_state_t hk_inline_commit(const hk_inline_plan_t *plan,
                                     hk_inline_write_fn write, void *write_ctx,
                                     hk_artifact_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_INLINE_H
