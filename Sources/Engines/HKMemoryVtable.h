// Memory-patch engine <-> runtime adapter -- Milestone 6. Presents the
// memory-patch engine (HKMemoryEngine.h) as an hk_engine_vtable_t so the plan
// lifecycle drives it, exactly as HKRebindVtable does for the rebind engine.
// This is the second engine wired in, which is the point: it shows the
// adapter pattern generalizes to a different engine AND a different target
// kind (HK_TARGET_MEMORY_PATCH), and it exercises the plan's memory-target
// path end to end for the first time.
//
// Same file-scoped environment as HKRebindVtable, and the same stated ceiling
// (one environment, fixed stash keyed by stable_hook_id). It preserves the
// two-phase invariant: prepare captures the region, commit writes it.
//
// The environment supplies what the SPEC cannot: how to write, and -- for an
// image-relative target -- where the image is mapped. Absolute-address targets
// need only the writer. On device the writer is hk_native_patch_memory and the
// image base comes from the image catalog (dyld populator, unbuilt); resolving
// a base_image *selector* to a base is the catalog's job and is NOT done here
// -- the environment hands the base in directly, a stand-in for that lookup.

#ifndef HK_ENGINES_MEMORY_VTABLE_H
#define HK_ENGINES_MEMORY_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "HKMemoryEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Where the target image is mapped, used only for image-relative targets;
    // absolute-address targets ignore it.
    uintptr_t image_base;
    hk_mempatch_write_fn write;
    void *write_ctx;
} hk_memory_binding_env_t;

// The engine to register. Handles HK_TARGET_MEMORY_PATCH targets.
const hk_engine_vtable_t *hk_memory_vtable(void);

void hk_memory_vtable_set_environment_for_testing(const hk_memory_binding_env_t *env);
void hk_memory_vtable_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_MEMORY_VTABLE_H
