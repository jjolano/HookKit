// Objective-C compile test for the new HookKit 3.0 headers -- see
// test_header_compile.c for what and why. This variant additionally
// proves the headers interoperate with real Class/SEL values (not just
// void*-typed placeholders), since hk_objc_target_t's cls/sel fields exist
// specifically to accept them.

#include <stddef.h>
#include <assert.h>

#import <Foundation/Foundation.h>

#include "../../Headers/HookKit/HookKit.h"

typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
_Static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
_Static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");

_Static_assert(HK_OBJC_ALLOW_INHERITED_OVERRIDE == 1, "hk_objc_inheritance_policy_t numeric values are part of the ABI");
_Static_assert(HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE == 2, "hk_availability_t numeric values are part of the ABI");

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
    return 0;
}
