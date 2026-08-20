// Objective-C compile test for the new HookKit 3.0 headers -- see
// test_header_compile.c for what and why. This variant additionally
// proves the headers interoperate with real Class/SEL values (not just
// void*-typed placeholders), since hk_objc_target_t's cls/sel fields exist
// specifically to accept them.

#include <stddef.h>
#include <string.h>
#include <assert.h>

#import <Foundation/Foundation.h>

#include "../../Headers/HookKit/HookKit.h"
// The typed ObjC wrapper is not in the umbrella (it needs <objc/runtime.h>),
// so it is imported explicitly here -- which is exactly how a caller uses it.
#include "../../Headers/HookKit/HookKitObjC.h"

typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
_Static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
_Static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");

_Static_assert(HK_OBJC_ALLOW_INHERITED_OVERRIDE == 1, "hk_objc_inheritance_policy_t numeric values are part of the ABI");
_Static_assert(HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE == 2, "hk_availability_t numeric values are part of the ABI");
_Static_assert(HK_ARTIFACT_MEMORY_PROTECTION_TRANSITION == 14, "hk_artifact_kind_t numeric values are part of the ABI");

// Deliberately no real NSString message sends here either (see this
// file's header comment) -- the fake Foundation.h this host builds
// against declares NSString with no methods and nothing to link a real
// ObjC runtime against. A plain C string still exercises the field.
static hk_image_identity_t make_sample_image_identity(void) {
    hk_image_identity_t image;
    image.struct_size = sizeof(image);
    image.struct_version = HK_ABI_VERSION_3_0;
    image.path = "/usr/lib/libsystem_kernel.dylib";
    image.uuid_present = true;
    memset(image.uuid, 0xAB, sizeof(image.uuid));
    image.slide = 0;
    return image;
}

static hk_objc_target_t make_sample_objc_target(void) {
    hk_objc_target_t target;
    target.struct_size = sizeof(target);
    target.struct_version = HK_ABI_VERSION_3_0;

    // Real Class/SEL-typed values, not just void* placeholders -- proves
    // the deliberately-untyped struct fields (see HookKitTargets.h's
    // header comment on why they're void*) accept real ObjC types, not
    // just opaque pointers that happen to be the right size. Deliberately
    // NOT using @selector()/+class/message sends: those pull in the GNU
    // ObjC runtime's module loader (__objc_exec_class) at link time, which
    // this host has no runtime to satisfy -- same reason the existing
    // test-substitute-classifier/test-original-publication targets don't
    // use them either. Plain casts still prove the type-level interop
    // this test exists for.
    Class cls = (Class)0;
    SEL sel = (SEL)"hk_test_selector";

    target.cls = (void *)cls;
    target.class_name = NULL;
    target.sel = (void *)sel;
    target.selector_name = NULL;
    target.method_kind = HK_OBJC_INSTANCE_METHOD;
    target.inheritance_policy = HK_OBJC_LOCAL_METHOD_ONLY;
    target.availability = HK_AVAILABILITY_REQUIRED_NOW;
    return target;
}

// The wrapper exists so the COMPILER checks Class/SEL instead of a void* that
// accepts anything. These calls are the check: they would not compile if the
// parameter types were wrong, and they pin the documented defaults.
static void exercise_objc_wrapper(void) {
    Class cls = (Class)0;
    SEL sel = (SEL)"hk_wrapper_selector";

    hk_objc_target_t inst = hk_objc_instance_method(cls, sel);
    assert(inst.method_kind == HK_OBJC_INSTANCE_METHOD);
    assert(inst.cls == (void *)cls && inst.sel == (void *)sel);
    assert(inst.struct_size == sizeof(hk_objc_target_t));
    // Documented defaults: the conservative choice on both axes.
    assert(inst.inheritance_policy == HK_OBJC_LOCAL_METHOD_ONLY);
    assert(inst.availability == HK_AVAILABILITY_REQUIRED_NOW);

    hk_objc_target_t klass = hk_objc_class_method(cls, sel);
    assert(klass.method_kind == HK_OBJC_CLASS_METHOD);   // never inferred

    hk_objc_target_t named = hk_objc_target_make_named("NSString", "length",
                                                       HK_OBJC_INSTANCE_METHOD);
    assert(named.cls == NULL && named.sel == NULL);      // resolved later, not here
    assert(named.class_name && named.selector_name);

    // The spec initializer sets target_kind for the caller: a spec whose kind
    // disagrees with the filled union member is a silent misroute.
    hk_hook_spec_t spec;
    int replacement = 0;
    hk_objc_spec_init(&spec, "wrapper.hook", named, &replacement);
    assert(spec.target_kind == HK_TARGET_OBJC_METHOD);
    assert(spec.required_reach == HK_REACH_OBJC_DISPATCH);
    assert(spec.replacement == &replacement);
    assert(spec.target.objc.class_name == named.class_name);
    // The two availability fields mirror rather than disagree by omission.
    assert(spec.availability == named.availability);

    hk_objc_spec_init(NULL, "ignored", named, &replacement);  // tolerates NULL
}

// The Swift request types. Included in all four variants deliberately: unlike
// HookKitObjC.h this header needs no ObjC runtime, and the C and C++ passes
// are what prove that claim rather than assert it.
static void exercise_swift_target(void) {
    hk_swift_target_t t = hk_swift_target_init();
    assert(t.struct_size == sizeof(hk_swift_target_t));
    // The documented default is require_unique = TRUE, which is the opposite
    // of what a zeroed struct gives -- so the initializer is load-bearing and
    // not a convenience.
    assert(t.require_unique);
    assert(t.name_kind == HK_SWIFT_NAME_MANGLED_EXACT);
    assert(t.availability == HK_AVAILABILITY_REQUIRED_NOW);

    hk_swift_target_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    assert(!zeroed.require_unique);   // exactly the trap the initializer avoids

    t.name_kind = HK_SWIFT_NAME_DEMANGLED_SUBSTRING;
    t.method_name = "MyClass.doThing";
    t.class_name = "MyModule.MyClass";
    assert(t.method_name && t.class_name);

    t.name_kind = HK_SWIFT_NAME_SLOT_INDEX;
    t.slot_index = 3;
    assert(t.slot_index == 3);
}

int main(void) {
    exercise_swift_target();
    exercise_objc_wrapper();
    hk_objc_target_t target = make_sample_objc_target();
    if (target.sel == NULL || target.method_kind != HK_OBJC_INSTANCE_METHOD) {
        return 1;
    }

    hk_image_identity_t image = make_sample_image_identity();
    if (!image.uuid_present || image.uuid[0] != 0xAB || strcmp(image.path, "/usr/lib/libsystem_kernel.dylib") != 0) {
        return 1;
    }
    return 0;
}
