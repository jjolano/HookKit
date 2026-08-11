#include "HKInlineGuard.h"

#include <pthread.h>
#include <stdio.h>

// Fixed-size entry table under one mutex; linear scan. The guard exists to
// prevent double-patching of one prologue, which is a live-hook-count concern:
// ponytail: 64 live inline hooks is the ceiling — the few dozen real
// consumers never approach it; a hash map is the upgrade if >64 live inline
// hooks per process ever shows up.

typedef struct {
    uintptr_t addr;
    void *replacement;
    void *orig;               // saved original of the last INSTALLED hook
    int type;
    hk_guard_state_t state;   // PENDING / INSTALLED / TAINTED
    uint64_t generation;      // bumped on every reservation; settle tokens must match
    void *prev_replacement;   // chaining: the INSTALLED replacement beneath, NULL when fresh
    bool used;
} hk_guard_entry_t;

#define HK_GUARD_MAX_ENTRIES 64

static hk_guard_entry_t g_entries[HK_GUARD_MAX_ENTRIES];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

hk_guard_result_t hk_inline_guard_reserve(uintptr_t address, void *replacement, int backendType, uint64_t *outGeneration, void **outOrig) {
    hk_guard_result_t result = HK_GUARD_BLOCKED;
    hk_guard_entry_t *entry = NULL;

    pthread_mutex_lock(&g_mutex);

    for(size_t i = 0; i < HK_GUARD_MAX_ENTRIES; i++) {
        if(g_entries[i].used && g_entries[i].addr == address) {
            entry = &g_entries[i];
            break;
        }
    }

    if(entry) {
        if(entry->replacement == replacement) {
            // Same replacement, same address: the outcome depends on state —
            // a hook in flight must not be double-fired, an installed one is
            // idempotent, a tainted one is a hard error.
            if(entry->state == HK_GUARD_PENDING) {
                result = HK_GUARD_DUP_PENDING;
            } else if(entry->state == HK_GUARD_INSTALLED) {
                if(outOrig) {
                    *outOrig = entry->orig;
                }
                result = HK_GUARD_DUP_INSTALLED;
            } else {
                if(outOrig) {
                    *outOrig = entry->orig;    // last known original, NULL when none
                }
                result = HK_GUARD_DUP_TAINTED;
            }
            goto unlock;
        }

        if(entry->state == HK_GUARD_TAINTED) {
            // The prologue may be half-written; nothing new may stack on it.
            result = HK_GUARD_BLOCKED;
            goto unlock;
        }

        if(entry->state == HK_GUARD_PENDING) {
            // A different hook is in flight; chaining over it is forbidden —
            // it could settle into anything.
            result = HK_GUARD_BLOCKED;
            goto unlock;
        }

        // INSTALLED with a different replacement: chaining. Only the same
        // backend type may chain — a second inline writer of a different kind
        // would double-patch the prologue.
        if(entry->type != backendType) {
            result = HK_GUARD_BLOCKED;
            goto unlock;
        }

        printf("[HKInlineGuard] note: chaining inline hook on %p via backend type %d\n", (void *)address, backendType);
        entry->prev_replacement = entry->replacement;
        entry->replacement = replacement;
        entry->state = HK_GUARD_PENDING;
        entry->generation++;
        if(outGeneration) {
            *outGeneration = entry->generation;
        }
        result = HK_GUARD_RESERVED;
        goto unlock;
    }

    for(size_t i = 0; i < HK_GUARD_MAX_ENTRIES; i++) {
        if(!g_entries[i].used) {
            hk_guard_entry_t *slot = &g_entries[i];
            slot->addr = address;
            slot->replacement = replacement;
            slot->orig = NULL;              // update() fills it when the hook lands
            slot->type = backendType;
            slot->state = HK_GUARD_PENDING;
            slot->prev_replacement = NULL;  // fresh reservation: nothing beneath
            slot->generation++;             // monotonic per slot: stale tokens never match again
            slot->used = true;
            if(outGeneration) {
                *outGeneration = slot->generation;
            }
            result = HK_GUARD_RESERVED;
            goto unlock;
        }
    }

    // Every slot is occupied and no entry matched: proceeding would leave
    // this hook unguarded, which is worse than declining it.
    result = HK_GUARD_FULL;

unlock:
    pthread_mutex_unlock(&g_mutex);
    return result;
}

void hk_inline_guard_update(uintptr_t address, uint64_t generation, int status, void *origValue) {
    pthread_mutex_lock(&g_mutex);

    for(size_t i = 0; i < HK_GUARD_MAX_ENTRIES; i++) {
        hk_guard_entry_t *entry = &g_entries[i];
        if(!entry->used || entry->addr != address) {
            continue;
        }

        // Stale settle protection: the token must match the entry's CURRENT
        // generation — a late completion can never settle another hook's
        // entry (the address may have been re-reserved since).
        if(entry->generation != generation) {
            break;
        }

        if(status == 0) {
            // HK_OK: the hook landed; origValue is the saved original that
            // future same-replacement re-hooks should return. For a chained
            // hook this settles the pending entry: replacement is now the
            // installed value.
            entry->orig = origValue;
            entry->prev_replacement = NULL;
            entry->state = HK_GUARD_INSTALLED;
        } else if(status == 1) {
            // HK_ERR: failed mid-flight — the prologue MAY be half-written.
            // Taint the entry so a later DIFFERENT hook still blocks (it
            // would stack on unknown bytes). The original is unknowable, so
            // origValue is discarded.
            entry->prev_replacement = NULL;
            entry->state = HK_GUARD_TAINTED;
        } else if(status == 2) {
            // HK_ERR_NOT_SUPPORTED: the backend wrote nothing. A FRESH
            // reservation releases the entry; a failed CHAINED reservation
            // restores the previous owner's INSTALLED state (its original was
            // never overwritten). Anything that cannot be proven untouched
            // goes TAINTED.
            if(!entry->prev_replacement && entry->state == HK_GUARD_PENDING) {
                entry->used = false;
            } else if(entry->prev_replacement && entry->state == HK_GUARD_PENDING) {
                entry->replacement = entry->prev_replacement;
                entry->prev_replacement = NULL;
                entry->state = HK_GUARD_INSTALLED;
            } else {
                entry->state = HK_GUARD_TAINTED;
            }
        }

        break;
    }

    pthread_mutex_unlock(&g_mutex);
}
