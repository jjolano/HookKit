// Objective-C engine <-> runtime adapter -- Milestone 6. Presents the ObjC
// method engine (HKObjCEngine.h) as an hk_engine_vtable_t so the plan
// lifecycle drives it, the same way HKRebindVtable and HKMemoryVtable do for
// their engines. This is the third engine wired in and the first to reach
// HK_TARGET_OBJC_METHOD, so it is what exercises the plan's ObjC-target path
// end to end.
//
// Same file-scoped environment as the other two adapters, and the same stated
// ceiling (one environment, fixed stash keyed by stable_hook_id) -- the vtable
// contract threads no per-engine context, so prepared state has to live
// somewhere the adapter can find it again at commit.
//
// The environment supplies what the SPEC cannot: the Objective-C runtime
// itself. On device that is a set of one-line forwarders to libobjc
// (objc_getClass, object_getClass, class_getSuperclass, sel_registerName,
// class_getInstanceMethod, method_getImplementation, method_getTypeEncoding,
// class_replaceMethod); on the host it is an in-memory class table. Nothing
// else is needed -- the replacement IMP comes from hk_hook_spec_t.replacement
// and the target from hk_hook_spec_t.target.objc.
//
// Note on original publication: hk_objc_commit can hand the original back
// through an out-parameter, but hk_engine_vtable_t.commit_one has nowhere to
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

typedef struct {
    hk_objc_runtime_t runtime;
} hk_objc_binding_env_t;

// The engine to register. Handles HK_TARGET_OBJC_METHOD targets.
const hk_engine_vtable_t *hk_objc_vtable(void);

void hk_objc_vtable_set_environment_for_testing(const hk_objc_binding_env_t *env);
void hk_objc_vtable_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_OBJC_VTABLE_H
