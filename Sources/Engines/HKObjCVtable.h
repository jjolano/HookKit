// Objective-C engine <-> runtime adapter -- Milestone 6. Presents the ObjC
// method engine (HKObjCEngine.h) as an hk_engine_vtable_t so the plan
// lifecycle drives it. This is the third engine wired in and the first to
// reach HK_TARGET_OBJC_METHOD, so it is what exercises the plan's ObjC-target
// path end to end.
//
// It is also the first adapter on the vtable's context-carrying entry points
// (prepare_one_ctx/commit_one_ctx/release_prepared, see HKEngineInternal.h),
// which is why it looks different from HKRebindVtable and HKMemoryVtable.
// Those two still use the context-free pair and pay for it twice: a
// file-scoped environment, so only one can be configured per process, and a
// fixed side stash keyed by stable_hook_id, so prepare has somewhere to leave
// state for commit. This adapter has neither. The environment is an ordinary
// caller-owned struct registered as the engine context, and prepared state is
// handed back by the core, so several runtimes can drive this engine with
// different ObjC runtimes concurrently and there is no stash to overflow.
//
// The context supplies what the SPEC cannot: the Objective-C runtime itself.
// On device that is a set of one-line forwarders to libobjc; on the host it is
// an in-memory class table. Nothing else is needed -- the replacement IMP
// comes from hk_hook_spec_t.replacement and the target from
// hk_hook_spec_t.target.objc.
//
// Note on original publication: hk_objc_commit can hand the original back
// through an out-parameter, but no vtable commit entry point has anywhere to
// put it, so the adapter passes NULL and the original travels in the
// artifact's original_pointer -- which is how the rebind adapter reports its
// originals too. The engine still writes the cell before the replace when a
// direct caller supplies one; that ordering guarantee is not weakened here,
// only unused.

#ifndef HK_ENGINES_OBJC_VTABLE_H
#define HK_ENGINES_OBJC_VTABLE_H

#include "../Core/HKEngineInternal.h"
#include "HKObjCEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registered as the engine context. Caller-owned and not copied: it must
// outlive the runtime it is registered with.
typedef struct {
    hk_objc_runtime_t runtime;
} hk_objc_engine_ctx_t;

// The engine to register. Handles HK_TARGET_OBJC_METHOD targets. Register it
// with hk_runtime_register_engine_with_context and an hk_objc_engine_ctx_t;
// registering it without one leaves it with no runtime to call, and every
// preparation fails cleanly.
const hk_engine_vtable_t *hk_objc_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_OBJC_VTABLE_H
