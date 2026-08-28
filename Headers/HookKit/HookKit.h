// HookKit 3.0 C API umbrella. Pure C: including this must not pull in
// Foundation or the Objective-C runtime (spec section 16.4).

#ifndef HOOKKIT_H
#define HOOKKIT_H

#include "HookKitBase.h"
#include "HookKitRuntime.h"
#include "HookKitTargets.h"
#include "HookKitResults.h"
#include "HookKitPlan.h"
#include "HookKitResolver.h"
#include "HookKitArtifacts.h"
#include "HookKitSwift.h"

// HookKitObjC.h (typed Class/SEL convenience) exists but is deliberately NOT
// included here: it requires <objc/runtime.h>, and a C caller including this
// umbrella must not acquire an Objective-C dependency it never asked for.
// Import it directly from a .m/.mm.
//
// HookKitSwift.h IS included: unlike HookKitObjC.h it needs no ObjC runtime,
// so a plain-C caller loses nothing by getting it. It declares request types
// only -- the engine is device-gated by its supported Swift layouts (see the
// header).
//
#endif // HOOKKIT_H
