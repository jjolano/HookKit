// Terminal inline engine <-> runtime adapter -- Milestone 7. Presents the
// terminal inline engine (HKInlineEngine.h) as an hk_engine_vtable_t so the
// plan lifecycle drives it. This is the fourth engine wired in and the first
// to reach HK_TARGET_FUNCTION_ADDRESS, so it exercises the plan's
// address-target path end to end.
//
// Built on the vtable's context-carrying entry points from the start, so it
// has no file-scoped state at all: the writer comes from the registered
// engine context and the prepared plan is handed back by the core.
//
// The context supplies the one thing the SPEC cannot: how to write executable
// memory. On device that is the VM-protection-changing, instruction-cache-
// invalidating store; on the host it is a plain buffer write.
//
// `expected_image` / `expected_uuid` ARE now checked, via
// HKImageScope.h, when the context supplies an image catalog. A NULL or empty
// catalog means the check is SKIPPED rather than failed -- catalog population
// is platform-specific and may not be available in a caller-supplied context.
// See HKImageScope.h for that policy in full.
//
// `may_strip_pac_or_thumb_state` is enforced at the adapter boundary. The
// canonical address is used consistently for scope checks, preflight, branch
// arithmetic, and ownership identity.

#ifndef HK_ENGINES_INLINE_VTABLE_H
#define HK_ENGINES_INLINE_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "../Core/HKImageScope.h"
#include "HKInlineEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Image-scope refusals are reported with codes offset past the engine's own
// hk_inline_status_t values, so a caller reading error_code can tell an
// image-scope refusal from an inline-engine one without a second field.
#define HK_INLINE_DIAG_IMAGE_SCOPE_BASE 100
// Refusing a non-atomic entry patch. Distinct code so it is not confused with
// an engine refusal or an image-scope one.
#define HK_INLINE_DIAG_NON_ATOMIC_PATCH 200

// Registered as the engine context. Caller-owned and not copied: it must
// outlive the runtime it is registered with.
typedef struct {
    hk_inline_write_fn write;
    void *write_ctx;
    // Optional. When present, a target's expected_image/expected_uuid are
    // enforced before anything is prepared; when NULL the check is skipped and
    // says so. Not owned.
    const hk_image_catalog_t *catalog;

    // Allow an entry patch that is NOT a single aligned store.
    //
    // Default (false) refuses one, and that default is the safe one for a
    // concrete, observed reason. Patching a live function's prologue with a
    // multi-instruction sequence gives another thread a window in which it can
    // enter the function part-patched. On a hot target this is not a rare
    // race: a sibling session reproduced a deterministic
    // EXC_BAD_ACCESS/KERN_PROTECTION_FAILURE 3/3 launches by inline-patching
    // syscall()/csops() in a live multi-threaded app, with the faulting PC in
    // the half-written page. The hazard is inherent to a non-atomic
    // multi-instruction patch, and this engine has the same shape whenever
    // the replacement is out of a B's reach.
    //
    // Setting this true says the caller knows the target is not concurrently
    // executing -- a cold function, or a process whose other threads are
    // suspended for the duration. It does NOT make the patch atomic.
    //
    // NOTE this does not address the other half of that report: on device the
    // W^X protection toggle around the write is itself a window, and that
    // lives in the write seam, not here. See the ledger.
    bool allow_non_atomic_entry_patch;
} hk_inline_engine_ctx_t;

// The engine to register with hk_runtime_register_engine_with_context, passing
// an hk_inline_engine_ctx_t. Handles HK_TARGET_FUNCTION_ADDRESS targets
// needing entry-point reach.
const hk_engine_vtable_t *hk_inline_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_INLINE_VTABLE_H
