// Swift vtable hooking -- a SEPARATE API surface, not a target kind.
//
// `HK_TARGET_SWIFT_VTABLE` exists in `hk_target_kind_t` so results and
// artifacts can name a Swift hook uniformly, but Swift hook *requests* do not
// go through `hk_plan_add_hook` and `hk_target_spec_t`. HookKitTargets.h says
// so at its own union: there is deliberately no `swift` member, because
// folding one in would give that header a dependency on this one, which the
// rest of the design avoids. This is the entry point instead.
//
// WHY IT IS SEPARATE AT ALL, since "one more union member" is the obvious
// alternative: a Swift method is identified by things no other target kind
// has -- a mangled name, a demangled substring, or a declaration-order slot
// index into a class's vtable -- and resolving any of them needs Swift
// metadata, not a symbol table. The identification model genuinely differs, so
// it gets its own request type rather than a union member that shares almost
// nothing with its siblings.
//
// SCOPE LIMITATIONS, stated here rather than discovered at runtime. These are
// the mechanism's, taken from native/hk_swift.{h,c} which implements it:
//   - arm64/arm64e only. Elsewhere every entry point reports unsupported,
//     because the metadata layouts and (on arm64e) the pointer-authentication
//     handling are architecture-specific.
//   - CLASS methods reached through the vtable only. A Swift method that is
//     final, static, in an extension, or dispatched through a protocol witness
//     table has no vtable slot to patch and is not reachable this way.
//   - Whole-module-optimised or devirtualised call sites do not go through the
//     vtable either, so patching the slot does not affect them. That is a
//     property of how the caller was compiled, not something a hook can fix.
//   - Demangling is used for substring lookup and is resolved dynamically. If
//     the Swift runtime's demangler is unavailable, substring lookup cannot
//     work and only exact-mangled and slot-index forms remain.
//
// STATUS: request types only. The engine behind this is DEFERRED -- the
// mechanism exists in native/hk_swift.c but is arm64-gated device code that
// fuses resolution and patching, so a two-phase engine needs a slot-address
// primitive it does not expose. See docs/3.0/IMPLEMENTATION_STATUS.md,
// Milestone 6. Declaring the request shape now is what lets the rest of the
// ABI stop saying "pending"; it is not a claim that a hook will install.

#ifndef HOOKKIT3_SWIFT_H
#define HOOKKIT3_SWIFT_H

#include "HookKitBase.h"
#include "HookKitResults.h"

#ifdef __cplusplus
extern "C" {
#endif

// How a method is named. The three forms are not interchangeable and the
// choice is always the caller's -- inferring "looks mangled, so treat it as
// mangled" from the string's contents is exactly the kind of hidden decision
// the target design forbids elsewhere.
typedef enum {
    // A complete mangled symbol name (`$s...`). Exact, unambiguous by
    // construction, and brittle across compiler versions -- which is the
    // trade the caller is making.
    HK_SWIFT_NAME_MANGLED_EXACT = 0,
    // A substring of the DEMANGLED name, e.g. "MyClass.doThing". Readable and
    // stable across mangling changes, but it can match more than one slot;
    // see `require_unique`.
    HK_SWIFT_NAME_DEMANGLED_SUBSTRING,
    // No name at all: `slot_index` identifies the method directly.
    HK_SWIFT_NAME_SLOT_INDEX,
} hk_swift_name_kind_t;

typedef struct {
    HK_STRUCT_HEADER;

    // The class, as a Swift metadata pointer. Untyped here for the same
    // reason hk_objc_target_t carries `void *`: this header stays includable
    // from plain C. NULL to use `class_name`.
    void *metadata;
    // Used when `metadata` is NULL. Resolvable only for classes the runtime
    // can look up by name; a generic or local class may have no such name,
    // which is why the pointer form exists.
    const char *class_name;

    hk_swift_name_kind_t name_kind;
    // Meaningful for MANGLED_EXACT and DEMANGLED_SUBSTRING; NULL otherwise.
    const char *method_name;
    // Meaningful for SLOT_INDEX only. Declaration order within the class's
    // own vtable, not counting inherited slots.
    uint32_t slot_index;

    // For DEMANGLED_SUBSTRING: refuse when the substring matches more than one
    // slot, rather than taking the first.
    //
    // Defaults to the safe value (true, refuse) because it is zero-valued the
    // other way round would be a trap: silently hooking the first of several
    // overloads is the kind of wrong that shows up as a behaviour change far
    // from the hook. Set false only when the caller genuinely means "any
    // match will do".
    //
    // The mechanism reports every candidate slot when it refuses, so an
    // ambiguous request is a diagnosable one rather than a dead end.
    bool require_unique;

    hk_availability_t availability;
} hk_swift_target_t;

// Note the inversion this implies for a zeroed struct: `require_unique` is
// false when zero-initialised, which is NOT the documented default above.
// Callers should use hk_swift_target_init rather than memset-and-assign.
static inline hk_swift_target_t hk_swift_target_init(void) {
    hk_swift_target_t t;
    t.struct_size = sizeof(t);
    t.struct_version = HK_ABI_VERSION_3_0;
    t.metadata = 0;
    t.class_name = 0;
    t.name_kind = HK_SWIFT_NAME_MANGLED_EXACT;
    t.method_name = 0;
    t.slot_index = 0;
    t.require_unique = true;   // the documented default, set explicitly
    t.availability = HK_AVAILABILITY_REQUIRED_NOW;
    return t;
}

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT3_SWIFT_H
