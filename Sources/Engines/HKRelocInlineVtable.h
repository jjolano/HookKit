// Relocating inline engine <-> runtime adapter -- Milestone 8. Fifth engine
// wired in, and the second to reach HK_TARGET_FUNCTION_ADDRESS.
//
// That second part is the interesting one. This adapter and HKInlineVtable
// describe themselves identically on target kind and reach; they differ ONLY
// in which originals they can serve, and the router now picks on exactly that
// (`hk_engine_capabilities_t.original_requirements`). So registering both in
// one runtime is the intended configuration, not a conflict:
//
//   HK_ORIGINAL_NONE                  -> either can serve it. Terminal wins
//                                        when registered first, which is the
//                                        right default: no page allocated, no
//                                        relocation, nothing to leak.
//   HK_ORIGINAL_DIRECT_PREDECESSOR    -> only this one.
//   HK_ORIGINAL_CALLABLE_CONTINUATION -> only this one.
//
// Register terminal FIRST if both are present. Nothing enforces that ordering
// and it is not a correctness requirement -- this engine serves NONE
// correctly, just with a trampoline nobody asked for -- but it is the
// difference between paying for a page and not.
//
// Built on the context-carrying, status-returning vtable entry points from the
// start, so it never had a file-scoped environment or a bool ceiling to
// retire.

#ifndef HK_ENGINES_RELOC_INLINE_VTABLE_H
#define HK_ENGINES_RELOC_INLINE_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "../Core/HKImageScope.h"
#include "HKRelocInlineEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Image-scope refusals are reported with codes offset past hk_reloc_status_t
// so a caller reading error_code can tell them apart without a second field.
#define HK_RELOC_DIAG_IMAGE_SCOPE_BASE 100
// Refusing a non-atomic entry patch -- see allow_non_atomic_entry_patch.
#define HK_RELOC_DIAG_NON_ATOMIC_PATCH 200

// Registered as the engine context. Caller-owned and not copied: it must
// outlive the runtime it is registered with.
typedef struct {
    // The two device-only seams. On device: vm_allocate, then vm_protect
    // R-W -> R-X. See HKRelocInlineEngine.h for the two facts about iOS that
    // make sealing a separate step rather than a flag on the allocation.
    hk_reloc_alloc_fn alloc;
    hk_reloc_seal_fn seal;
    // Gives a page back when a preparation fails after allocating, or when an
    // entry patch never lands. May be NULL, but then those pages leak.
    hk_reloc_free_fn free_page;
    void *seam_ctx;

    hk_reloc_write_fn write;
    void *write_ctx;

    // Optional. When present, the target's expected_image/expected_uuid are
    // enforced before a page is even requested. NULL means the check is
    // skipped and says so -- see HKImageScope.h. Not owned.
    const hk_image_catalog_t *catalog;

    // Allow an entry patch that is NOT a single aligned store. Same meaning
    // and same default (false, refuse) as the terminal adapter's -- see
    // HKInlineVtable.h for the observed crash behind that default.
    //
    // This engine reaches the hazard less often: the inbound thunk exists so
    // the entry patch can be a 4-byte B, and it is whenever the page lands
    // within a B's reach of the target. The non-atomic form only appears when
    // the allocator could not place it near, which is exactly the case worth
    // refusing rather than papering over.
    bool allow_non_atomic_entry_patch;
} hk_reloc_engine_ctx_t;

// The engine to register with hk_runtime_register_engine_with_context, passing
// an hk_reloc_engine_ctx_t. Declares HK_EFFECT_EXECUTABLE_ALLOCATION, so a
// request forbidding that is routed away from it.
const hk_engine_vtable_t *hk_reloc_inline_vtable(void);

// The SAME engine, described differently -- Milestone 9's static continuation.
//
// Identical prepare/commit/release: the survey found the relocating engine
// needs no change for this, because its seams already say "give me a writable
// region / make it executable / give it back", which is what a fixed pool does
// (see HKStaticPool.h). Only two things differ, and both are declarations
// rather than behaviour:
//
//   - it does NOT declare HK_EFFECT_EXECUTABLE_ALLOCATION, because a pool slot
//     was mapped executable at load and nothing new is created at hook time;
//   - it is therefore eligible for a request carrying
//     HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY or
//     HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY, which the dynamic one is
//     not.
//
// REGISTERING IT IS A PROMISE ABOUT THE SEAMS, and the vtable cannot check it.
// Point its ctx at pool-backed alloc/seal/free (HKStaticPool.h) and nothing
// else. Registering this vtable with a ctx whose alloc calls vm_allocate would
// declare "no allocation" while allocating -- a lie the router would believe,
// and exactly the kind of silent misreport the effects vocabulary exists to
// prevent. The two vtables share an engine precisely so that this stays the
// ONLY difference a caller has to get right.
const hk_engine_vtable_t *hk_static_inline_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_RELOC_INLINE_VTABLE_H
