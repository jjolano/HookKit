// HookKit 3.0 -- new C API umbrella. Pure C: including this must not pull
// in Foundation or the Objective-C runtime (spec section 16.4). Legacy
// HKSubstitutor declarations live in HookKitLegacy.h (pending, Milestone
// 11), reached only through Compat.h or the historical umbrella
// Headers/HookKit.h -- never from here.
//
// Not yet wired into the built framework: HookKit_PUBLIC_HEADERS in the
// Makefile still lists only the legacy Headers/HookKit.h. See
// docs/3.0/IMPLEMENTATION_STATUS.md, Milestone 3.

#ifndef HOOKKIT3_H
#define HOOKKIT3_H

#include "HookKitBase.h"
#include "HookKitRuntime.h"
#include "HookKitTargets.h"
#include "HookKitResults.h"
#include "HookKitPlan.h"
#include "HookKitArtifacts.h"

// Not yet written (docs/3.0/IMPLEMENTATION_STATUS.md tracks each):
//   HookKitObjC.h       -- typed Class/SEL convenience wrappers
//   HookKitSwift.h      -- Swift vtable hooking, a separate API surface

#endif // HOOKKIT3_H
