#ifndef hk_inline_guard_h
#define hk_inline_guard_h

#include <stdbool.h>
#include <stdint.h>

// Process-wide inline-ownership guard: prevents HookKit-vs-HookKit
// contention — two HKSubstitutor instances (or one instance hooking twice)
// installing DIFFERENT inline hooks on the same function address through
// DIFFERENT inline backends (native/Dobby/Frida/litehook-inline/ElleKit/
// Substrate/Substitute), which would double-patch one prologue.
//
// Pure C on purpose: the host-side test compiles this file directly, with no
// Foundation/ObjC in the picture.

// State of an owned inline-hook entry:
//   HK_GUARD_PENDING   - reserved, the backend has not reported the outcome
//                        yet (hook in flight or queued in a batch)
//   HK_GUARD_INSTALLED - the hook landed; the entry holds the saved original
//   HK_GUARD_TAINTED   - a hook failed mid-flight: the prologue MAY be
//                        half-written, so the original is unknowable and no
//                        other hook may take this address
typedef enum {
    HK_GUARD_PENDING,
    HK_GUARD_INSTALLED,
    HK_GUARD_TAINTED
} hk_guard_state_t;

// Reserve an inline hook target. Returns:
//   HK_GUARD_RESERVED      - target free (or same-type chaining); hook may
//                            proceed; entry reserved in HK_GUARD_PENDING.
//                            *outGeneration receives the reservation's
//                            generation token, which the settle call must
//                            pass back
//   HK_GUARD_DUP_PENDING   - same address + same replacement already reserved
//                            but NOT yet installed: refused — a hook is in
//                            flight; do not invoke the backend (nothing was
//                            written)
//   HK_GUARD_DUP_INSTALLED - same address + same replacement already
//                            installed: idempotent success (outOrig receives
//                            the saved original)
//   HK_GUARD_DUP_TAINTED    - same address + same replacement but the entry
//                            is tainted (a previous hook failed mid-flight):
//                            error — nothing may be installed on this
//                            address (outOrig receives the last known
//                            original, NULL when none)
//   HK_GUARD_BLOCKED       - same address, DIFFERENT replacement, DIFFERENT
//                            backend type: caller must NOT invoke the backend
//                            (nothing was written)
//   HK_GUARD_FULL          - all ownership slots are occupied and no entry
//                            matched: nothing was reserved. Caller must refuse
//                            the hook -- proceeding would leave it unguarded.
// Same backend type + different replacement is allowed (provider chaining)
// and logs; the entry stays PENDING until settled.
typedef enum {
    HK_GUARD_RESERVED,
    HK_GUARD_DUP_PENDING,
    HK_GUARD_DUP_INSTALLED,
    HK_GUARD_DUP_TAINTED,
    HK_GUARD_BLOCKED,
    HK_GUARD_FULL
} hk_guard_result_t;

// outGeneration is written only when HK_GUARD_RESERVED is returned (each
// reservation bumps the entry's generation); outOrig is written only for
// HK_GUARD_DUP_INSTALLED / HK_GUARD_DUP_TAINTED.
hk_guard_result_t hk_inline_guard_reserve(uintptr_t address, void *replacement, int backendType, uint64_t *outGeneration, void **outOrig);

// Settle a reservation after the backend call. `generation` must be the
// token hk_inline_guard_reserve wrote to *outGeneration: a stale settle (a
// later reservation for the same address bumped the generation) is dropped,
// so a late completion can never settle another hook's entry.
//   status 0 (HK_OK)                 -> store origValue; entry INSTALLED
//   status 1 (HK_ERR)                -> entry TAINTED (keep entry so later
//                                       different hooks still block)
//   status 2 (HK_ERR_NOT_SUPPORTED)  -> release entry (backend wrote nothing)
// status is int for host-testability.
void hk_inline_guard_update(uintptr_t address, uint64_t generation, int status, void *origValue);

#endif
