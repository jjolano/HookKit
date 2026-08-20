// Compatibility-only surface -- Milestone 11.
//
// Everything here exists because HookKit 2.x shipped an API that 3.0 would not
// have designed, and breaking existing consumers is not on the table. Nothing
// in this header is for new code. Each entry says what 2.x call forced it to
// exist, so that when the last such consumer is gone the entry can go too.
//
// THE FIRST AND CURRENTLY ONLY ENTRY: memory patches with no precondition.
//
// `hk_memory_target_t.expected_bytes` is REQUIRED for a new-API request. A
// patch that does not say what it expects to find cannot be revalidated, so
// committing one means writing over whatever happens to be there -- a
// different build, a already-patched region, or the wrong address entirely.
// `hk_plan_add_hook` therefore rejects a memory target without one.
//
// 2.x's `-[HKSubstitutor hookMemory:withData:size:]` has no parameter for it
// and never did. Its callers are not able to supply what they were never
// asked for, so the legacy path captures the region at PREPARATION and treats
// that as the precondition. That is strictly weaker: it detects a change
// between prepare and commit, which is the invariant #3 window, but it cannot
// detect that the address was wrong to begin with. The weakening is the point
// of routing it through a separate, named entry point rather than relaxing the
// rule for everyone.

#ifndef HOOKKIT3_LEGACY_H
#define HOOKKIT3_LEGACY_H

#include "HookKitBase.h"
#include "HookKitPlan.h"
#include "HookKitTargets.h"

#ifdef __cplusplus
extern "C" {
#endif

// Adds a hook exactly as hk_plan_add_hook does, except that a
// HK_TARGET_MEMORY_PATCH target may omit `expected_bytes`. When it does, the
// engine captures the region at preparation and revalidates against that.
//
// DO NOT USE IN NEW CODE. A new-API caller knows what it expects to find and
// should say so; this exists only so 2.x's hookMemory: keeps working. Every
// other target kind behaves identically to hk_plan_add_hook, so this is not a
// general "relaxed" variant -- it relaxes exactly one rule.
//
// Returns the same statuses as hk_plan_add_hook.
hk_status_t hk_plan_add_hook_legacy(hk_plan_t *plan,
                                    const hk_hook_spec_t *spec,
                                    hk_hook_t **out_hook);

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT3_LEGACY_H
