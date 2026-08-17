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

#include "../../Headers/HookKit/HookKit.h"

typedef struct { HK_STRUCT_HEADER; int x; } hk_test_struct_t;
static_assert(offsetof(hk_test_struct_t, struct_size) == 0, "struct_size must be first");
static_assert(offsetof(hk_test_struct_t, struct_version) == sizeof(uint32_t), "struct_version must be second");

static_assert(HK_MEMORY_KIND_DATA == 1, "hk_memory_target_kind_t numeric values are part of the ABI");
static_assert(HK_IMAGE_EXPLICIT_SET == 5, "hk_image_selector_kind_t numeric values are part of the ABI");
static_assert(HK_ARTIFACT_FILE_MAPPING == 15, "hk_artifact_kind_t numeric values are part of the ABI");

// hk_report_copy_artifacts() etc. are declared but not implemented yet
// (Milestone 4). Taking their address into a correctly-typed function
// pointer proves the declarations themselves are well-formed C++ (no
// implicit-int, no incomplete-type-by-value) without needing to link
// against an implementation.
static hk_status_t (*const g_copy_artifacts_fn)(const hk_report_t *, hk_artifact_snapshot_t **) = &hk_report_copy_artifacts;
static void (*const g_release_snapshot_fn)(hk_artifact_snapshot_t *) = &hk_artifact_snapshot_release;

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

int main() {
    hk_memory_target_t target = make_sample_memory_target();
    SEL sel = (SEL)"hk_test_selector";

    if (target.address != 0x1000 || sel == nullptr) {
        return 1;
    }
    if (g_copy_artifacts_fn == nullptr || g_release_snapshot_fn == nullptr) {
        return 1;
    }
    return 0;
}
