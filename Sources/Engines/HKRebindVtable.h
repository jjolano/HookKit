// Rebind engine <-> runtime adapter -- Milestone 6. Presents the rebind
// engine (HKRebindEngine.h) as an hk_engine_vtable_t so the Milestone 4 plan
// lifecycle (analyze -> prepare -> commit) drives it end to end and its
// artifacts land in the report, exactly as a fake engine's do.
//
// Why an adapter, and what it papers over. The engine vtable
// (HKEngineInternal.h) is context-free: describe()/prepare_one(spec)/
// commit_one(spec, sink) take no per-engine environment and thread no
// prepared state from prepare to commit. A real engine needs both -- the
// image to operate on and how to write it, plus prepare's captured originals
// at commit time (ARCHITECTURE.md invariant #5). Rather than widen that shared
// signature (which would touch every fake engine), this adapter supplies both
// through a FILE-SCOPED environment set by the caller before the plan runs.
//
// That environment is a stated ceiling, not the finished design:
//   - one environment at a time, so one image per registered engine;
//   - prepared state is stashed in a small fixed table keyed by
//     stable_hook_id, not carried through the vtable.
// It is enough to prove the wiring host-side, and it PRESERVES the two-phase
// invariant -- prepare genuinely runs first and captures originals, commit
// uses them. The proper fix is to give the vtable per-engine context and a
// per-hook state handoff; that is separate, wider work.
//
// On device the environment is not a test fixture: image_base/size/slide come
// from the image catalog (the dyld populator, still unbuilt), and the writer
// is the VM-protection-changing, arm64e-re-signing store -- all device-only.

#ifndef HK_ENGINES_REBIND_VTABLE_H
#define HK_ENGINES_REBIND_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "HKRebindEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the adapter needs to act: which image, and how to write a slot.
typedef struct {
    const void *image_base;
    size_t image_size;
    uintptr_t slide;
    hk_rebind_write_fn write;
    void *write_ctx;
} hk_rebind_binding_env_t;

// The engine to register with hk_runtime_register_engine_for_testing. Handles
// function-symbol targets needing existing-imports reach.
const hk_engine_vtable_t *hk_rebind_vtable(void);

// Sets the (file-scoped) environment the adapter operates against, and clears
// any stashed prepared state. On device this is filled from the platform
// layer; the name marks that the *host* path is a fixture. Passing NULL
// detaches the environment (prepare then fails cleanly).
void hk_rebind_vtable_set_environment_for_testing(const hk_rebind_binding_env_t *env);

// Clears the environment and all stashed plans. Call between independent runs.
void hk_rebind_vtable_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_REBIND_VTABLE_H
