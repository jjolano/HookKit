// HookKit 3.0 -- target kinds, image scope, and per-kind target specs.
// See docs/3.0/PUBLIC_C_ABI.md ("Target kinds and specs") for the prose
// this was drafted from, and docs/3.0/IMPLEMENTATION_STATUS.md for status.
//
// The master spec (section 6.16-6.20) describes each target kind's fields
// as prose, not C -- the concrete struct layouts below are this file's own
// design, not a transcription. Notable calls made here, stated rather than
// left implicit:
//
//   - hk_target_spec_t is a plain union, not a tagged struct: the sibling
//     hk_target_kind_t field on hk_hook_spec_t (HookKitPlan.h) is already
//     the discriminant, so the union carries no redundant internal tag.
//   - Swift is deliberately NOT a union member here. HK_TARGET_SWIFT_VTABLE
//     exists in hk_target_kind_t for uniform reporting (results, artifacts),
//     but Swift hook *requests* go through HookKitSwift.h's own entry
//     point, not hk_plan_add_hook + this union -- matching the master
//     spec's repeated "separate API, no category membership" language for
//     Swift (section 13.7). Folding a Swift member in here would give
//     HookKitTargets.h a dependency on HookKitSwift.h that the rest of the
//     design goes out of its way to avoid.
//   - Class/SEL are carried as `void *` here, not `Class`/`SEL`, to keep
//     this header importable from plain C without <objc/runtime.h>.
//     HookKitObjC.h (not yet written) is where a typed convenience wrapper
//     belongs.
//   - Memory targets use an explicit `address_is_image_relative` bool
//     rather than overloading an image-selector value to mean "absolute
//     address" -- the whole spec's "no hidden fallback" principle applied
//     at struct-design granularity: a field's meaning should never depend
//     on silently inferring intent from another field's contents.

#ifndef HOOKKIT3_TARGETS_H
#define HOOKKIT3_TARGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HookKitBase.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HK_TARGET_FUNCTION_SYMBOL = 0,
    HK_TARGET_FUNCTION_ADDRESS,
    HK_TARGET_OBJC_METHOD,
    HK_TARGET_MEMORY_PATCH,
    HK_TARGET_SWIFT_VTABLE,  // reporting only -- see file header comment
} hk_target_kind_t;

// ---- Image scope -----------------------------------------------------
//
// Three separate scopes exist across the ABI and are never conflated: the
// defining-image selector (where the symbol/target lives), the
// caller/import-image scope (which importers get rebound), and an
// excluded-image set (spec section 6.15). This one type expresses any of
// them; which role a given hk_image_selector_t plays is determined by
// which struct field it fills, not by anything in the selector itself.

typedef enum {
    HK_IMAGE_ANY_LOADED = 0,
    HK_IMAGE_MAIN_EXECUTABLE,
    HK_IMAGE_EXACT_PATH,
    HK_IMAGE_EXACT_UUID,
    HK_IMAGE_EXACT_HEADER,
    HK_IMAGE_EXPLICIT_SET,
} hk_image_selector_kind_t;

typedef struct hk_image_selector {
    HK_STRUCT_HEADER;

    hk_image_selector_kind_t kind;

    const char *path;                          // HK_IMAGE_EXACT_PATH
    uint8_t uuid[16];                           // HK_IMAGE_EXACT_UUID
    const void *header;                         // HK_IMAGE_EXACT_HEADER (struct mach_header*/mach_header_64*, untyped here)
    const struct hk_image_selector *const *explicit_set;  // HK_IMAGE_EXPLICIT_SET
    size_t explicit_set_count;
} hk_image_selector_t;

// ---- Symbol target (HK_TARGET_FUNCTION_SYMBOL) ------------------------

typedef enum {
    HK_SYMBOL_NAME_C = 0,
    HK_SYMBOL_NAME_MACHO_EXACT,
    HK_SYMBOL_NAME_CXX_MANGLED,
    HK_SYMBOL_NAME_SWIFT_MANGLED,
} hk_symbol_name_convention_t;

typedef enum {
    HK_SYMBOL_VISIBILITY_ANY = 0,
    HK_SYMBOL_VISIBILITY_EXPORTED_ONLY,
    HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED,
} hk_symbol_visibility_t;

typedef enum {
    HK_SYMBOL_ALIAS_EXACT_ONLY = 0,
    HK_SYMBOL_ALIAS_FOLLOW,
} hk_symbol_alias_policy_t;

typedef struct {
    HK_STRUCT_HEADER;

    const char *name;  // leading underscore normalized internally -- pass either form
    hk_symbol_name_convention_t name_convention;
    hk_symbol_visibility_t visibility;

    hk_image_selector_t defining_image;
    hk_image_selector_t caller_image_scope;

    hk_symbol_alias_policy_t alias_policy;
    bool interior_address_permitted;
} hk_symbol_target_t;

// ---- Address target (HK_TARGET_FUNCTION_ADDRESS) ----------------------

typedef struct {
    HK_STRUCT_HEADER;

    uintptr_t address;

    hk_image_selector_t expected_image;
    bool expected_uuid_present;
    uint8_t expected_uuid[16];

    const uint8_t *expected_initial_bytes;  // optional; NULL to skip the check
    size_t expected_initial_bytes_size;

    bool may_strip_pac_or_thumb_state;
} hk_address_target_t;

// ---- Objective-C target (HK_TARGET_OBJC_METHOD) -----------------------

typedef enum {
    HK_OBJC_INSTANCE_METHOD = 0,
    HK_OBJC_CLASS_METHOD,
} hk_objc_method_kind_t;

typedef enum {
    HK_OBJC_LOCAL_METHOD_ONLY = 0,
    HK_OBJC_ALLOW_INHERITED_OVERRIDE,
} hk_objc_inheritance_policy_t;

typedef enum {
    HK_AVAILABILITY_REQUIRED_NOW = 0,
    HK_AVAILABILITY_OPTIONAL_IF_PRESENT,
    HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE,
} hk_availability_t;

typedef struct {
    HK_STRUCT_HEADER;

    void *cls;               // Class, as void* -- see file header comment; NULL to use class_name
    const char *class_name;  // used when cls is NULL

    void *sel;                  // SEL, as void*; NULL to use selector_name
    const char *selector_name;  // used when sel is NULL

    hk_objc_method_kind_t method_kind;              // stated explicitly, never inferred
    hk_objc_inheritance_policy_t inheritance_policy;
    hk_availability_t availability;
} hk_objc_target_t;

// ---- Memory target (HK_TARGET_MEMORY_PATCH) ----------------------------

typedef enum {
    HK_MEMORY_KIND_CODE = 0,
    HK_MEMORY_KIND_DATA,
} hk_memory_target_kind_t;

typedef struct {
    HK_STRUCT_HEADER;

    uintptr_t address;
    bool address_is_image_relative;  // when true, address is relative to base_image
    hk_image_selector_t base_image;  // meaningful only when address_is_image_relative

    hk_bytes_view_t replacement_bytes;
    hk_bytes_view_t expected_bytes;  // required for a new-API request (spec 6.19); a
                                      // legacy-compat-only path may capture these at
                                      // preparation instead -- see HookKitLegacy.h (pending)
    hk_bytes_view_t expected_mask;
    size_t size;

    hk_memory_target_kind_t kind;
} hk_memory_target_t;

// ---- Union --------------------------------------------------------------

typedef union {
    hk_symbol_target_t symbol;
    hk_address_target_t address;
    hk_objc_target_t objc;
    hk_memory_target_t memory;
    // No `swift` member -- see file header comment.
} hk_target_spec_t;

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT3_TARGETS_H
