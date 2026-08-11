#include <assert.h>
#include <stdio.h>

#include "../Internal/HKInlineGuard.h"

// Host-side test for the process-wide inline-ownership guard. Pure C, runs
// on the build machine; compile with:
//   clang -Wall -Wextra -O2 -o test_inline_guard tests/test_inline_guard.c Internal/HKInlineGuard.c

static void *fn1 = (void *)(uintptr_t)0x1000;
static void *fn2 = (void *)(uintptr_t)0x2000;
static void *fn3 = (void *)(uintptr_t)0x3000;
static void *fn4 = (void *)(uintptr_t)0x4000;
static void *fn5 = (void *)(uintptr_t)0x5000;
static void *fn6 = (void *)(uintptr_t)0x6000;
static void *fn7 = (void *)(uintptr_t)0x7000;
static void *repA = (void *)(uintptr_t)0x8000;
static void *repB = (void *)(uintptr_t)0x9000;
static void *repC = (void *)(uintptr_t)0xA000;

int main(void) {
    // 1. Reserve -> settle OK: RESERVED with a generation token, then the
    //    entry is INSTALLED; a duplicate of the same address+replacement is
    //    idempotent and returns the stored original. outOrig is untouched on
    //    RESERVED (the header only writes it for DUP_INSTALLED/DUP_TAINTED).
    uint64_t g1 = 0;
    void *orig = (void *)(uintptr_t)0xDEAD;
    assert(hk_inline_guard_reserve((uintptr_t)fn1, repA, 1, &g1, &orig) == HK_GUARD_RESERVED);
    assert(g1 > 0);
    assert(orig == (void *)(uintptr_t)0xDEAD);

    hk_inline_guard_update((uintptr_t)fn1, g1, 0, repA);  // HK_OK: saves original
    void *saved = NULL;
    assert(hk_inline_guard_reserve((uintptr_t)fn1, repA, 1, NULL, &saved) == HK_GUARD_DUP_INSTALLED);
    assert(saved == repA);

    // 2. PENDING duplicate refusal: same address + same replacement while a
    //    hook is in flight is DUP_PENDING — a success, but not RESERVED and
    //    not an idempotent DUP_INSTALLED.
    uint64_t g2 = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn2, repB, 1, &g2, NULL) == HK_GUARD_RESERVED);
    hk_guard_result_t r = hk_inline_guard_reserve((uintptr_t)fn2, repB, 1, NULL, NULL);
    assert(r == HK_GUARD_DUP_PENDING);
    assert(r != HK_GUARD_RESERVED && r != HK_GUARD_DUP_INSTALLED);
    hk_inline_guard_update((uintptr_t)fn2, g2, 0, repB);  // settle the in-flight hook

    // 3. Stale-generation drop: settling with an OLD token must not touch the
    //    newer reservation's entry.
    uint64_t gA = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn3, repA, 1, &gA, NULL) == HK_GUARD_RESERVED);
    hk_inline_guard_update((uintptr_t)fn3, gA, 0, repA);  // A INSTALLED

    uint64_t gB = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn3, repB, 1, &gB, NULL) == HK_GUARD_RESERVED);  // chain, same type
    assert(gB > gA);
    hk_inline_guard_update((uintptr_t)fn3, gA, 0, repA);  // stale token: dropped
    // The newer (pending) B entry is untouched: re-hooking B is still refused
    // as in-flight, not idempotent — a stale settle never installed B.
    assert(hk_inline_guard_reserve((uintptr_t)fn3, repB, 1, NULL, NULL) == HK_GUARD_DUP_PENDING);
    // Settling with the CURRENT token installs B with its original.
    hk_inline_guard_update((uintptr_t)fn3, gB, 0, repA);
    assert(hk_inline_guard_reserve((uintptr_t)fn3, repB, 1, NULL, &saved) == HK_GUARD_DUP_INSTALLED);
    assert(saved == repA);

    // 4. ERR taints: a mid-flight failure makes the same replacement a hard
    //    error (last known original, NULL when none) and blocks any different
    //    replacement.
    uint64_t g4 = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn4, repA, 1, &g4, NULL) == HK_GUARD_RESERVED);
    hk_inline_guard_update((uintptr_t)fn4, g4, 1, NULL);  // HK_ERR
    saved = (void *)(uintptr_t)0xDEAD;
    assert(hk_inline_guard_reserve((uintptr_t)fn4, repA, 1, NULL, &saved) == HK_GUARD_DUP_TAINTED);
    assert(saved == NULL);  // no original was ever installed
    assert(hk_inline_guard_reserve((uintptr_t)fn4, repC, 2, NULL, NULL) == HK_GUARD_BLOCKED);

    // 5. NOT_SUPPORTED on a FRESH reservation releases the entry: the address
    //    is usable again by any replacement.
    uint64_t g5 = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn5, repA, 1, &g5, NULL) == HK_GUARD_RESERVED);
    hk_inline_guard_update((uintptr_t)fn5, g5, 2, NULL);  // HK_ERR_NOT_SUPPORTED: wrote nothing
    assert(hk_inline_guard_reserve((uintptr_t)fn5, repB, 2, &g5, NULL) == HK_GUARD_RESERVED);
    hk_inline_guard_update((uintptr_t)fn5, g5, 0, repB);  // clean up

    // 6. NOT_SUPPORTED on a CHAINED reservation restores the previous owner:
    //    the entry reverts to A, INSTALLED, with A's original intact.
    uint64_t g6A = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn6, repA, 1, &g6A, NULL) == HK_GUARD_RESERVED);
    hk_inline_guard_update((uintptr_t)fn6, g6A, 0, repA);  // A INSTALLED, orig = repA
    uint64_t g6B = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn6, repB, 1, &g6B, NULL) == HK_GUARD_RESERVED);  // chain
    hk_inline_guard_update((uintptr_t)fn6, g6B, 2, NULL);  // chained hook wrote nothing
    assert(hk_inline_guard_reserve((uintptr_t)fn6, repA, 1, NULL, &saved) == HK_GUARD_DUP_INSTALLED);
    assert(saved == repA);  // A's original survived the failed chain

    // 7. Different-backend chain blocked: an INSTALLED entry refuses a
    //    different replacement from a different backend type, and is
    //    unchanged afterwards.
    uint64_t g7 = 0;
    assert(hk_inline_guard_reserve((uintptr_t)fn7, repA, 1, &g7, NULL) == HK_GUARD_RESERVED);
    hk_inline_guard_update((uintptr_t)fn7, g7, 0, repA);
    assert(hk_inline_guard_reserve((uintptr_t)fn7, repB, 2, NULL, NULL) == HK_GUARD_BLOCKED);
    assert(hk_inline_guard_reserve((uintptr_t)fn7, repA, 1, NULL, &saved) == HK_GUARD_DUP_INSTALLED);
    assert(saved == repA);

    // 8. Table-full fails CLOSED: once all 64 slots are occupied and no entry
    //    matches, reservation returns HK_GUARD_FULL instead of a silent OK.
    //    7 entries are live here (fn1..fn7), so 57 fresh addresses fit and
    //    the 58th is refused.
    int ok = 0;
    int full_at = -1;

    for(int i = 0; i < 64; i++) {
        hk_guard_result_t rr = hk_inline_guard_reserve((uintptr_t)(0x10000 + i * 8), repC, 1, NULL, NULL);

        if(rr == HK_GUARD_RESERVED) {
            ok += 1;
        } else {
            assert(rr == HK_GUARD_FULL);
            full_at = i;
            break;
        }
    }

    assert(ok == 57);
    assert(full_at == 57);

    // The refused address was not recorded: reserving it again is still FULL,
    // not DUP.
    assert(hk_inline_guard_reserve((uintptr_t)(0x10000 + full_at * 8), repC, 1, NULL, NULL) == HK_GUARD_FULL);

    printf("test_inline_guard: all assertions passed\n");
    return 0;
}
