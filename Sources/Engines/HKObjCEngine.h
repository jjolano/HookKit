// Objective-C method engine -- Milestone 6. Redirects a selector's dispatch by
// replacing the implementation the runtime hands out for it.
//
// Reach: HK_REACH_OBJC_DISPATCH. It changes what `objc_msgSend` finds, which
// is every message send to that selector on that class and its subclasses,
// and nothing else -- a direct call to the IMP that already has the pointer
// in hand is unaffected. That is a property of the mechanism.
//
// TWO PHASES, same reason as the other engines:
//   prepare  resolves class -> method -> IMP and captures the original. It
//            MUTATES NOTHING (ARCHITECTURE.md invariant #2), which is what
//            makes the original known before any replacement is reachable
//            (invariant #5).
//   commit   re-resolves and checks the IMP still matches what prepare
//            captured (invariant #3), then replaces.
//
// THE RUNTIME IS THE DEVICE-ONLY PART, and it is behind a seam. There is no
// Objective-C runtime on the Linux host this is developed on -- not a stubbed
// one, none at all: `objc/runtime.h` does not exist and no libobjc is
// installed. The repo's existing .m tests use tests/fake_headers, which are
// declaration-only stubs that give the compiler front end an NSObject/Class/
// SEL vocabulary; nothing there ever executes a runtime call. So every
// runtime primitive this engine needs is a function pointer in
// hk_objc_runtime_t, and the host tests drive it with an in-memory class
// table. What that verifies is every DECISION the engine makes -- metaclass
// selection, the local-vs-inherited test, availability policy, revalidation,
// publish-before-replace ordering, and the reversibility distinction below.
// What it does NOT verify is real libobjc behavior; that is device-only and
// is not claimed here.
//
#ifndef HK_ENGINES_OBJC_H
#define HK_ENGINES_OBJC_H

#include <stdbool.h>
#include <stddef.h>

#include "../../Headers/HookKit/HookKitTargets.h"
#include "../Core/HKArtifactLedger.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HK_OBJC_OK = 0,
    HK_OBJC_INVALID_ARGUMENT,
    HK_OBJC_CLASS_NOT_FOUND,
    HK_OBJC_METHOD_NOT_FOUND,
    // The method exists but only on an ancestor, and the request asked for
    // HK_OBJC_LOCAL_METHOD_ONLY. Refused rather than silently widened: adding
    // an override changes dispatch for every sibling subclass too.
    HK_OBJC_INHERITED_REFUSED,
    // Resolved to a method with no implementation -- there is no original to
    // publish, so there is nothing safe to replace.
    HK_OBJC_NO_IMPLEMENTATION,
    // The target is absent and the request said HK_AVAILABILITY_OPTIONAL_IF_PRESENT.
    // Distinct from NOT_FOUND because it is a satisfied request, not a failure.
    HK_OBJC_NOT_APPLICABLE,
    // Retained for ABI stability and no longer produced: deferral used to be
    // refused here for want of a retry mechanism, which Milestone 12 built.
    // An absent deferred target now reports HK_OBJC_NOT_APPLICABLE like any
    // other conditional one, and the plan decides whether that means "skip"
    // or "wait".
    HK_OBJC_UNSUPPORTED_POLICY,
} hk_objc_status_t;

// The device-only surface: the Objective-C runtime, as function pointers.
// Each maps to exactly one libobjc call, named for it, with no added
// behavior -- so the device binding is a set of one-line forwarders and the
// engine's logic is the only thing being tested here.
typedef struct {
    // objc_getClass. Used only when the target names a class by string.
    void *(*get_class)(void *ctx, const char *name);
    // object_getClass(cls) -- a class's metaclass, where its class methods live.
    void *(*get_metaclass)(void *ctx, void *cls);
    // class_getSuperclass. Needed for the local-vs-inherited test below.
    void *(*get_superclass)(void *ctx, void *cls);
    // sel_registerName. Used only when the target names a selector by string.
    void *(*register_selector)(void *ctx, const char *name);
    // class_getInstanceMethod: searches cls AND its ancestors. NULL cls
    // must return NULL (the root-class case relies on it).
    void *(*get_instance_method)(void *ctx, void *cls, void *sel);
    void *(*method_get_imp)(void *ctx, void *method);
    // method_getTypeEncoding. May return NULL for a method with no encoding.
    const char *(*method_get_types)(void *ctx, void *method);
    // class_replaceMethod. Returns the IMP it actually replaced when the
    // method lived on `cls`; NULL when it instead ADDED a new override over
    // an inherited method. See hk_objc_commit for why that distinction matters.
    void *(*replace_method)(void *ctx, void *cls, void *sel, void *imp,
                            const char *types);
    void *ctx;
} hk_objc_runtime_t;

typedef struct {
    void *cls;     // the class the replace will be applied to -- the METAclass
                   // when the target is a class method
    void *sel;
    void *method;  // as resolved at prepare
    void *original_imp;
    const char *types;  // type encoding to carry into the replace
    // True when the method lives on `cls` itself rather than an ancestor.
    // This is not bookkeeping: it decides both whether an inheritance policy
    // is satisfied and whether the change is cleanly reversible.
    bool is_local;
    bool captured;
} hk_objc_plan_t;

// Phase 1. Resolves the target and captures the original IMP. Mutates nothing.
hk_objc_status_t hk_objc_prepare(const hk_objc_runtime_t *rt,
                                 const hk_objc_target_t *target,
                                 hk_objc_plan_t *out_plan);

// Phase 2. Re-resolves, checks the IMP still matches what prepare captured,
// then replaces.
//
// Returns the honest mutation state:
//   NONE      nothing was replaced -- a clean refusal (revalidation failed,
//             or the plan was never captured)
//   COMPLETE  the implementation was replaced
// There is no PARTIAL here: one replace either happens or does not.
//
// `out_original`, when non-NULL, receives the original IMP and is written
// BEFORE the replace call (invariant #5) so a caller holding that cell never
// observes an active replacement whose predecessor is unknown. If the replace
// then reports an authoritative predecessor of its own -- which it does only
// when the method was local -- the cell is updated to that, since the runtime
// is a better witness than a prior read.
//
// `sink` may be NULL; when present, one HK_ARTIFACT_OBJC_METHOD_CHANGE is
// recorded.
hk_mutation_state_t hk_objc_commit(const hk_objc_runtime_t *rt,
                                   const hk_objc_plan_t *plan,
                                   void *replacement,
                                   void **out_original,
                                   hk_artifact_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_OBJC_H
