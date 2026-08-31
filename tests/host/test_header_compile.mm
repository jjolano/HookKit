// Objective-C++ compile test for the new HookKit 3.0 headers -- see
// test_header_compile.c for what and why. Exercises the same headers
// through the one remaining language mode: C++ types/keywords (nullptr,
// static_assert) alongside ObjC types (Class, SEL) in a single translation
// unit. Deliberately not using @selector()/message sends -- see
// test_header_compile.m's comment on why (no ObjC runtime to link against
// on this host).

#include <cstddef>
#include <cstdint>

#import <Foundation/Foundation.h>

#include <cassert>
#include "../../include/HookKit/HookKit.h"
// The typed ObjC wrapper is not in the umbrella (it needs <objc/runtime.h>),
// so it is imported explicitly here -- which is exactly how a caller uses it.
#include "../../include/HookKit/HookKitObjC.h"

typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");

static_assert(HK_MEMORY_KIND_DATA == 1, "hk_memory_target_kind_t numeric values are part of the ABI");
static_assert(HK_IMAGE_EXPLICIT_SET == 5, "hk_image_selector_kind_t numeric values are part of the ABI");
static_assert(HK_ARTIFACT_FILE_MAPPING == 15, "hk_artifact_kind_t numeric values are part of the ABI");

// Taking their addresses into correctly-typed function pointers proves the
// declarations and the implemented snapshot ABI are well-formed C++.
static hk_status_t (*const g_copy_artifacts_fn)(const hk_report_t *, hk_artifact_snapshot_t **) = &hk_report_copy_artifacts;
static void (*const g_release_snapshot_fn)(hk_artifact_snapshot_t *) = &hk_artifact_snapshot_release;
static hk_status_t (*const g_find_symbol_fn)(hk_runtime_t *, const char *, const char *, void **) = &hk_runtime_find_symbol;

static hk_memory_target_t make_sample_memory_target() {
    hk_memory_target_t target;
    target.struct_size = sizeof(target);
    target.struct_version = HK_ABI_VERSION_3_0;
    target.address = 0x1000;
    target.address_is_image_relative = false;

    target.base_image.struct_size = sizeof(target.base_image);
    target.base_image.struct_version = HK_ABI_VERSION_3_0;
    target.base_image.kind = HK_IMAGE_ANY_LOADED;
    target.base_image.path = nullptr;
    target.base_image.header = nullptr;
    target.base_image.explicit_set = nullptr;
    target.base_image.explicit_set_count = 0;

    target.replacement_bytes.data = nullptr;
    target.replacement_bytes.size = 0;
    target.expected_bytes.data = nullptr;
    target.expected_bytes.size = 0;
    target.expected_mask.data = nullptr;
    target.expected_mask.size = 0;
    target.size = 0;
    target.kind = HK_MEMORY_KIND_CODE;
    return target;
}

// Same wrapper checks as the ObjC variant. Worth repeating under ObjC++
// specifically: the header wraps its declarations in extern "C", and a
// static-inline-only header getting that wrong shows up here and nowhere else.
static void exercise_objc_wrapper(void) {
    Class cls = (Class)0;
    SEL sel = (SEL)"hk_wrapper_selector";

    hk_objc_target_t inst = hk_objc_instance_method(cls, sel);
    assert(inst.method_kind == HK_OBJC_INSTANCE_METHOD);
    assert(inst.cls == (void *)cls && inst.sel == (void *)sel);
    assert(inst.inheritance_policy == HK_OBJC_LOCAL_METHOD_ONLY);
    assert(inst.availability == HK_AVAILABILITY_REQUIRED_NOW);

    hk_objc_target_t klass = hk_objc_class_method(cls, sel);
    assert(klass.method_kind == HK_OBJC_CLASS_METHOD);   // never inferred

    hk_objc_target_t named = hk_objc_target_make_named("NSString", "length",
                                                       HK_OBJC_INSTANCE_METHOD);
    assert(named.cls == NULL && named.sel == NULL);      // resolved later, not here

    hk_hook_spec_t spec;
    int replacement = 0;
    hk_objc_spec_init(&spec, "wrapper.hook", named, &replacement);
    assert(spec.target_kind == HK_TARGET_OBJC_METHOD);
    assert(spec.required_reach == HK_REACH_OBJC_DISPATCH);
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

int main() {
    exercise_swift_target();
    exercise_objc_wrapper();
    hk_memory_target_t target = make_sample_memory_target();
    SEL sel = (SEL)"hk_test_selector";

    if (target.address != 0x1000 || sel == nullptr) {
        return 1;
    }
    if (g_copy_artifacts_fn == nullptr || g_release_snapshot_fn == nullptr ||
        g_find_symbol_fn == nullptr) {
        return 1;
    }
    return 0;
}
