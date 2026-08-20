// Fixed trampoline pool -- Milestone 9's static continuation.
//
// A "static" continuation is one whose executable memory ALREADY EXISTED when
// the process started: a HookKit-owned section mapped executable at load
// (hence HK_MAPPING_STATIC_HOOKKIT_SECTION in the results ABI). Nothing is
// allocated at hook time, which is what a caller means by
// HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY / HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY:
// not "do not write to code" but "do not create new executable mappings",
// which some hardening and attestation setups refuse outright.
//
// THE SURVEY RESULT THAT SHAPED THIS: the relocating engine needs no change at
// all. Its seams already describe exactly what a pool does --
//   alloc  "give me a writable region of `size` I can build in"
//   seal   "make it executable"
//   free   "give it back"
// For a fresh page that is vm_allocate / vm_protect(R-X) / vm_deallocate. For a
// pool slot it is claim-and-make-writable / make-executable / release. The
// asymmetry worth naming is that a pool slot arrives R-X and must be made
// WRITABLE before the engine builds in it, where a fresh page arrives R-W --
// so a device pool's alloc does an unprotect that a device page's alloc does
// not. The seam contract covers both; only the implementation differs.
//
// This pool is the host-testable half. It owns no memory: the caller supplies
// the region, which on device is the linker section and in a test is an
// ordinary buffer.

#ifndef HK_ENGINES_STATIC_POOL_H
#define HK_ENGINES_STATIC_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Slots are tracked in a single word, so a pool holds at most 64. That is a
// deliberate cap rather than an accident: a static pool is a fixed budget by
// definition, and 64 concurrent relocating hooks in one process is already
// far past what any real consumer installs.
#define HK_STATIC_POOL_MAX_SLOTS 64u

typedef struct {
    uintptr_t base;      // start of the caller-supplied executable region
    size_t slot_size;    // bytes per slot; must be >= what the engine needs
    unsigned slot_count; // <= HK_STATIC_POOL_MAX_SLOTS
    uint64_t used;       // one bit per slot
} hk_static_pool_t;

// Initialises a pool over [base, base + slot_size*slot_count). Returns false
// on a region that cannot hold what it claims, or a slot count over the cap.
bool hk_static_pool_init(hk_static_pool_t *pool, uintptr_t base,
                         size_t slot_size, unsigned slot_count);

// Claims a free slot of at least `size` bytes. Returns 0 when the pool is
// exhausted -- which is a real, expected outcome for a fixed budget, not an
// error condition to paper over.
//
// `near` is the same placement hint the engine's alloc seam takes. Slots are
// searched nearest-first so a caller gets the best chance at a 4-byte atomic
// entry patch; a pool that lands far still works, just not atomically.
uintptr_t hk_static_pool_claim(hk_static_pool_t *pool, size_t size, uintptr_t near);

// Returns a slot. A pointer that is not a slot base of this pool is ignored
// rather than corrupting the bitmap -- the engine hands back what it was
// given, and a mismatch means a bug worth failing quietly over rather than
// scribbling on unrelated state.
void hk_static_pool_release(hk_static_pool_t *pool, uintptr_t slot);

unsigned hk_static_pool_free_count(const hk_static_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_STATIC_POOL_H
