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
// TWO THINGS THIS ADAPTER DOES NOT DO, stated rather than silently skipped:
//
//   - `hk_address_target_t.may_strip_pac_or_thumb_state` is not acted on. On
//     arm64e a function pointer may carry a signature in its high bits, and
//     stripping it needs ptrauth intrinsics that exist only on device. The
//     address is used as given. A caller passing a signed pointer with this
//     flag set on device will need the strip performed before it reaches
//     here, and that belongs with the other device-only seams, not in a
//     host-testable adapter pretending to do it.
//
//   - `expected_image` / `expected_uuid` are not checked. Confirming that an
//     address lies inside a particular image with a particular UUID is the
//     image catalog's job (the dyld populator, still unbuilt). Ignoring them
//     silently would let a hook land in the wrong image after a slide change,
//     so this is a real gap, not a design choice -- it is recorded in the
//     ledger as such.

#ifndef HK_ENGINES_INLINE_VTABLE_H
#define HK_ENGINES_INLINE_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "HKInlineEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registered as the engine context. Caller-owned and not copied: it must
// outlive the runtime it is registered with.
typedef struct {
    hk_inline_write_fn write;
    void *write_ctx;
} hk_inline_engine_ctx_t;

// The engine to register with hk_runtime_register_engine_with_context, passing
// an hk_inline_engine_ctx_t. Handles HK_TARGET_FUNCTION_ADDRESS targets
// needing entry-point reach.
const hk_engine_vtable_t *hk_inline_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_INLINE_VTABLE_H
