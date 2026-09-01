# HookKit 3.0 — Architecture

## Mission

HookKit 3.0 is the C-first next major version of HookKit. It stays the same
product under the same name — this is not a replacement framework.

It delivers:

1. A C-first HookKit 3 runtime and public ABI.
2. Declarative target, reachability, continuation, mutation, side-effect,
   domain, ownership, result, and artifact models.
3. A true continuation-free native terminal-inline engine.
4. Built-in Objective-C, import-rebinding, memory-patch, native inline, and
   Swift engines.
5. Certified adapters for the external providers HookKit already vendors
   (ElleKit-compatible/libhooker, Substrate, Substitute, Dobby, Gum/Frida).
6. Deferred image and class lifecycle support.
7. Immutable per-hook and process-wide artifact snapshots.
8. A Shadow rewrite using the native HookKit runtime and one complete
    startup-qualified manifest (Milestone 14, gated on explicit sign-off —
    see `IMPLEMENTATION_STATUS.md`).

## Product model

HookKit 3 **is**: a deterministic in-process runtime that analyzes,
prepares, commits, verifies, owns, and reports interception operations
across Objective-C dispatch, import slots, function entry points, Swift
vtables, and controlled memory patches.

HookKit 3 **is not**: a user-facing provider selector, a Shadow settings
engine, a jailbreak-detection policy engine, a generic arbitrary C-hook
chaining framework, a universal safe-unhook framework, a runtime provider
marketplace, an artifact concealment layer, or a system that installs
additional coverage in response to detector activity.

The last two points are load-bearing for how HookKit relates to Shadow:
HookKit reports everything it does, honestly and completely, to every
caller. Any product-level decision to act differently based on detector
state belongs entirely to the caller (Shadow), never to HookKit itself —
see `HK3_SHADOW_INTEGRATION_CONTRACT.md` (Milestone 14).

## Non-negotiable invariants

These are the rules a route, an engine, or a fallback may never violate.
Every engine contract, router decision, and test in this rewrite exists to
enforce one of these seven.

### 1. Analysis is process-side-effect-free

`hk_plan_analyze()` may allocate ordinary heap memory and inspect (read-only)
already-loaded state: engine descriptors, image metadata, target bytes,
import values, Objective-C/Swift metadata, VM protections. It may never
`dlopen`, activate a provider, run a provider constructor, register an image
callback, create a thread, allocate executable memory, create a trampoline
or branch island, persist provider state, or mutate anything.

### 2. Preparation never mutates requested targets

`hk_plan_prepare()` may perform request-permitted *non-target* effects:
provider activation, provider image loading, read-only private-symbol
scans, file-backed symbol-cache mappings, executable continuation
allocation, callback registration, provider-global init. It may never touch
target text, import slots, Objective-C IMPs, Swift vtable slots, or
requested memory-patch bytes.

### 3. Commit revalidates before writing

Preparation captures exact preconditions. Immediately before a mandatory
domain starts mutating anything, every mandatory member, its ownership, its
image generation, and its dependency state are revalidated. Immediately
before each mutating primitive, the exact target state is re-read; stale
state is refused; compare-and-exchange is used where possible; where it
isn't, the residual race is reported, not hidden.

### 4. Fallback depends on mutation state

An engine attempt returns exactly one of `HK_MUTATION_NONE`,
`HK_MUTATION_COMPLETE`, `HK_MUTATION_PARTIAL`, `HK_MUTATION_UNKNOWN`.
Another route may be attempted only after `HK_MUTATION_NONE`. A single
error code is never the fallback signal.

### 5. Original publication precedes activation

When an original or predecessor is required, HookKit owns a stable original
slot and publishes it before the
replacement becomes reachable. An engine that cannot guarantee
publication-before-activation is ineligible for that request.

### 6. Artifacts are never hidden

HookKit exposes complete generic artifacts through immutable snapshots. It
never redacts them for a caller, hides provider images, hides generated
executable mappings, hides import slots, hides target patches, or filters
by caller identity.

### 7. No implicit constructor work

Loading `HookKit.framework` does nothing: no provider activation, image
traversal, callback registration, thread creation, hook installation, log
emission, or executable allocation. All work begins from an explicit runtime
call.

## Repository layout

The repository uses:

```text
include/
  HookKit.h                    # Objective-C v1/2 compatibility umbrella
  HookKit/
    HookKit.h                  # C API umbrella
    Compat.h Core.h Hook.h Module.h
    HookKitBase.h HookKitTargets.h HookKitRuntime.h HookKitPlan.h
    HookKitResults.h HookKitArtifacts.h HookKitObjC.h HookKitSwift.h
src/
  compatibility/              # v1/2 facade and module-class translators
  core/                       # runtime, plans, reports, ownership, artifacts
  engines/                    # built-in engines and provider adapters
  internal/                   # framework-private Objective-C/platform seams
  native/                     # native patching, relocation, and Swift support
  resolvers/                  # Mach-O, export, import, and symbol resolution
tests/
  host/
  macos/
  device/
  fixtures/headers/
tools/
  bench/
  conformance/
  dependencies/
  logos-hookkit/
  provider-evidence/
  release/
  shadow-manifest-extract/
metadata/
  provider-evidence/
  schemas/
  manifests/
packaging/
  abi/
  exports/
  layout/
  resources/
vendor/
docs/3.0/
.theos/                       # generated build, release, bench, and rebuild output
```

HookKit 3 ships the `hk_*` runtime together with the v1/2 compatibility
facade, compatibility classes, headers, and linker surface. Historical ABI
snapshot tooling is not part of the repository or release gate.
