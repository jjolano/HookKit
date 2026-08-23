// Memory-patch engine <-> runtime adapter -- Milestone 6. Presents the
// memory-patch engine (HKMemoryEngine.h) as an hk_engine_vtable_t so the plan
// lifecycle drives it, exactly as HKRebindVtable does for the rebind engine.
// This is the second engine wired in, which is the point: it shows the
// adapter pattern generalizes to a different engine AND a different target
// kind (HK_TARGET_MEMORY_PATCH), and it exercises the plan's memory-target
// path end to end for the first time.
//
// Uses the vtable's context-carrying entry points, like HKRebindVtable: the
// context is an ordinary caller-owned struct and prepared state is handed back
// by the core, so there is no file-scoped environment and no stash. It
// preserves the two-phase invariant: prepare captures the region, commit
// writes it.
//
// The context supplies what the SPEC cannot: how to write, and -- for an
// image-relative target -- where the image is mapped. Absolute-address targets
// need only the writer. On device the writer is hk_native_patch_memory and the
// image base comes from the image catalog (dyld populator); resolving
// a base_image *selector* to a base is the catalog's job and is NOT done here
// -- the context hands the base in directly, a stand-in for that lookup.

#ifndef HK_ENGINES_MEMORY_VTABLE_H
#define HK_ENGINES_MEMORY_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "../Core/HKImageScope.h"
#include "HKMemoryEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Image-scope refusals are reported with codes offset past
// hk_mempatch_status_t so a caller reading error_code can tell them apart
// without a second field.
#define HK_MEMORY_DIAG_IMAGE_SCOPE_BASE 100

// Registered as the engine context; caller-owned and not copied, so it must
// outlive the runtime it is registered with.
typedef struct {
    // Where the target image is mapped, used only for image-relative targets;
    // absolute-address targets ignore it.
    uintptr_t image_base;
    hk_mempatch_write_fn write;
    void *write_ctx;
    // Optional. When present, the target's base_image is enforced against the
    // address the patch will land on, before the region is read. NULL means
    // the check is skipped and says so -- see HKImageScope.h. Not owned.
    const hk_image_catalog_t *catalog;
} hk_memory_engine_ctx_t;

// The engine to register with hk_runtime_register_engine_with_context, passing
// an hk_memory_engine_ctx_t. Handles HK_TARGET_MEMORY_PATCH targets.
const hk_engine_vtable_t *hk_memory_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_MEMORY_VTABLE_H
