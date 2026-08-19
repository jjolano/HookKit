// Rebind engine <-> runtime adapter -- Milestone 6. Presents the rebind
// engine (HKRebindEngine.h) as an hk_engine_vtable_t so the Milestone 4 plan
// lifecycle (analyze -> prepare -> commit) drives it end to end and its
// artifacts land in the report, exactly as a fake engine's do.
//
// Why an adapter. A real engine needs two things the SPEC cannot carry: the
// image to operate on with a way to write it, and prepare's captured
// originals at commit time (ARCHITECTURE.md invariant #5). Both come through
// the vtable's context-carrying entry points -- the context is an ordinary
// caller-owned struct registered alongside the engine, and prepared state is
// handed back by the core.
//
// This adapter originally used a file-scoped environment and a side stash
// keyed by stable_hook_id, because the vtable was context-free. That ceiling
// is gone: several runtimes can now drive this engine against different
// images concurrently, and there is no fixed stash to overflow.
//
// On device the context is not a test fixture: image_base/size/slide come
// from the image catalog (the dyld populator, still unbuilt), and the writer
// is the VM-protection-changing, arm64e-re-signing store -- all device-only.

#ifndef HK_ENGINES_REBIND_VTABLE_H
#define HK_ENGINES_REBIND_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "../Core/HKImageScope.h"
#include "HKRebindEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Image-scope refusals are reported with codes offset past hk_rebind_status_t
// so a caller reading error_code can tell them apart without a second field.
#define HK_REBIND_DIAG_IMAGE_SCOPE_BASE 100

// What the adapter needs to act: which image, and how to write a slot.
// Registered as the engine context; caller-owned and not copied, so it must
// outlive the runtime it is registered with.
typedef struct {
    const void *image_base;
    size_t image_size;
    uintptr_t slide;
    hk_rebind_write_fn write;
    void *write_ctx;
    // Optional. When present, the target's caller_image_scope is enforced
    // against the image this context points at, before any slot is read.
    // NULL means the check is skipped and says so -- see HKImageScope.h.
    // Not owned.
    const hk_image_catalog_t *catalog;
} hk_rebind_engine_ctx_t;

// The engine to register with hk_runtime_register_engine_with_context, passing
// an hk_rebind_engine_ctx_t. Handles function-symbol targets needing
// existing-imports reach. Registered without a context it has no image to act
// on, and every preparation fails cleanly.
const hk_engine_vtable_t *hk_rebind_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_REBIND_VTABLE_H
