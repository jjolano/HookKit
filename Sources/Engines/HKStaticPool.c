// Fixed trampoline pool. See HKStaticPool.h.

#include "HKStaticPool.h"

#include <string.h>

bool hk_static_pool_init(hk_static_pool_t *pool, uintptr_t base,
                         size_t slot_size, unsigned slot_count) {
    if (!pool || base == 0 || slot_size == 0 || slot_count == 0 ||
        slot_count > HK_STATIC_POOL_MAX_SLOTS) {
        return false;
    }
    // The region must not wrap. Written as a division so no multiplication can
    // overflow before the check that would have caught it.
    if (slot_size > (SIZE_MAX / slot_count)) {
        return false;
    }
    memset(pool, 0, sizeof(*pool));
    pool->base = base;
    pool->slot_size = slot_size;
    pool->slot_count = slot_count;
    return true;
}

uintptr_t hk_static_pool_claim(hk_static_pool_t *pool, size_t size, uintptr_t near) {
    if (!pool || size == 0 || size > pool->slot_size) {
        return 0;
    }

    // Nearest-first, so the entry patch has the best chance of being a single
    // 4-byte B. A far slot is still usable; it just costs atomicity, which the
    // adapters refuse by default (see HKInlineVtable.h).
    unsigned best = pool->slot_count;
    uintptr_t best_distance = 0;
    for (unsigned i = 0; i < pool->slot_count; i++) {
        if (pool->used & (1ull << i)) {
            continue;
        }
        uintptr_t addr = pool->base + (uintptr_t)i * pool->slot_size;
        uintptr_t distance = addr > near ? addr - near : near - addr;
        if (best == pool->slot_count || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    if (best == pool->slot_count) {
        return 0;  // exhausted -- an expected outcome for a fixed budget
    }
    pool->used |= (1ull << best);
    return pool->base + (uintptr_t)best * pool->slot_size;
}

void hk_static_pool_release(hk_static_pool_t *pool, uintptr_t slot) {
    if (!pool || slot < pool->base) {
        return;
    }
    uintptr_t offset = slot - pool->base;
    if (offset % pool->slot_size != 0) {
        return;  // not a slot base: ignore rather than corrupt the bitmap
    }
    uintptr_t index = offset / pool->slot_size;
    if (index >= pool->slot_count) {
        return;
    }
    pool->used &= ~(1ull << index);
}

unsigned hk_static_pool_free_count(const hk_static_pool_t *pool) {
    if (!pool) {
        return 0;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < pool->slot_count; i++) {
        if (!(pool->used & (1ull << i))) {
            n++;
        }
    }
    return n;
}
