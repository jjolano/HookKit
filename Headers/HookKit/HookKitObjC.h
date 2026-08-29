// Typed Objective-C convenience over HookKitTargets.h.
//
// `hk_objc_target_t` carries `cls` and `sel` as `void *` on purpose:
// HookKitTargets.h must stay importable from plain C, and typing those fields
// as `Class`/`SEL` would drag `<objc/runtime.h>` into every translation unit
// that touches any target kind. This header is where the typed spelling
// belongs, and it is a SEPARATE header for exactly that reason -- including it
// is an opt-in that says "this file already speaks Objective-C".
//
// Deliberately NOT included by HookKit.h. The umbrella stays C-only; a C
// caller that includes it must not acquire an ObjC dependency it never asked
// for. Import this one directly from a .m/.mm.
//
// Everything here is `static inline` and allocates nothing: these are
// constructors that fill a target struct, not a parallel API. There is no
// behavior to get wrong, which is the point -- the value is that the compiler
// checks the Class/SEL types instead of a `void *` accepting anything.

#ifndef HOOKKIT_OBJC_H
#define HOOKKIT_OBJC_H

#if !defined(__OBJC__)
#error "HookKitObjC.h requires Objective-C. Plain C callers should use HookKitTargets.h and its void*-typed cls/sel fields directly."
#endif

#include <objc/runtime.h>
#include <string.h>

#include "HookKitBase.h"
#include "HookKitTargets.h"
#include "HookKitResults.h"
#include "HookKitPlan.h"

#ifdef __cplusplus
extern "C" {
#endif

// A target naming the class and selector by POINTER. Use when the caller
// already holds them -- it reaches a class the name would not (an unregistered
// or renamed one), and skips a lookup.
//
// Defaults chosen to match the enums' zero values, so a caller who wants them
// need set nothing: HK_OBJC_LOCAL_METHOD_ONLY and HK_AVAILABILITY_REQUIRED_NOW.
// Both are the conservative choice -- refuse rather than widen the blast
// radius to an inherited method, and fail now rather than silently defer.
// Assign over the returned struct's fields to change either.
static inline hk_objc_target_t hk_objc_target_make(Class cls, SEL sel,
                                                   hk_objc_method_kind_t kind) {
    hk_objc_target_t t;
    memset(&t, 0, sizeof(t));
    t.struct_size = sizeof(t);
    t.struct_version = HK_ABI_VERSION_3_0;
    #if defined(__has_feature)
    #if __has_feature(objc_arc)
    t.cls = (__bridge void *)cls;
    #else
    t.cls = (void *)cls;
    #endif
    #else
    t.cls = (void *)cls;
    #endif
    t.sel = (void *)sel;
    t.method_kind = kind;
    t.inheritance_policy = HK_OBJC_LOCAL_METHOD_ONLY;
    t.availability = HK_AVAILABILITY_REQUIRED_NOW;
    return t;
}

// A target naming the class and selector by STRING. Use when the class may not
// be loaded yet, or when the caller does not want to force a lookup at request
// time. Resolution happens during preparation, not here.
//
// The strings are borrowed, not copied -- this constructor allocates nothing.
// They must outlive the call to hk_plan_add_hook, which deep-copies them.
static inline hk_objc_target_t hk_objc_target_make_named(const char *class_name,
                                                         const char *selector_name,
                                                         hk_objc_method_kind_t kind) {
    hk_objc_target_t t;
    memset(&t, 0, sizeof(t));
    t.struct_size = sizeof(t);
    t.struct_version = HK_ABI_VERSION_3_0;
    t.class_name = class_name;
    t.selector_name = selector_name;
    t.method_kind = kind;
    t.inheritance_policy = HK_OBJC_LOCAL_METHOD_ONLY;
    t.availability = HK_AVAILABILITY_REQUIRED_NOW;
    return t;
}

// `-[cls sel]` and `+[cls sel]`, spelled the way they are written in source.
// The method kind is never inferred from anything -- an instance and a class
// method can share a selector, and guessing which was meant is exactly the
// kind of hidden decision the target design forbids.
static inline hk_objc_target_t hk_objc_instance_method(Class cls, SEL sel) {
    return hk_objc_target_make(cls, sel, HK_OBJC_INSTANCE_METHOD);
}
static inline hk_objc_target_t hk_objc_class_method(Class cls, SEL sel) {
    return hk_objc_target_make(cls, sel, HK_OBJC_CLASS_METHOD);
}

// Opt-in to inherited-method hooking. Default is HK_OBJC_LOCAL_METHOD_ONLY
// (safe — refuse rather than widen). Call this only when blast radius is
// intended; see MIGRATION.md. Logos `%hook` uses it only when the method
// carries __attribute__((annotate("hookkit:allow_inherited"))) AND
// %config hook_inheritance=allow_inherited is set.
// ponytail: one-line setter, no new struct field, no ABI change
static inline void hk_objc_target_allow_inherited(hk_objc_target_t *t) {
    if (t) t->inheritance_policy = HK_OBJC_ALLOW_INHERITED_OVERRIDE;
}

// Fills a hook spec's ObjC target in one step, since a caller almost always
// wants the spec rather than a bare target. Sets target_kind for the caller --
// a spec whose target_kind disagrees with the union member that was filled is
// a silent misroute, and this is the one place that can be prevented rather
// than documented.
//
// `replacement` is the new implementation. `stable_hook_id` is borrowed, same
// as the name strings above.
static inline void hk_objc_spec_init(hk_hook_spec_t *out_spec,
                                     const char *stable_hook_id,
                                     hk_objc_target_t target,
                                     void *replacement) {
    if (!out_spec) {
        return;
    }
    memset(out_spec, 0, sizeof(*out_spec));
    out_spec->struct_size = sizeof(*out_spec);
    out_spec->struct_version = HK_ABI_VERSION_3_0;
    out_spec->stable_hook_id = stable_hook_id;
    out_spec->target_kind = HK_TARGET_OBJC_METHOD;
    out_spec->target.objc = target;
    out_spec->replacement = replacement;
    out_spec->required_reach = HK_REACH_OBJC_DISPATCH;
    // The target's own availability is the one that governs resolution; the
    // spec-level field mirrors it so the two cannot disagree by omission.
    out_spec->availability = target.availability;
    out_spec->role = HK_OPERATION_MANDATORY;
}

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_OBJC_H
