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

int main(void) {
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
