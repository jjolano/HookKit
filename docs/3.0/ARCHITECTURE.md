# HookKit 3.0 — Architecture

## Mission

HookKit 3.0 is a major in-place rewrite of HookKit, the C-first runtime
underneath the `HKSubstitutor` API. It stays the same product under the same
name — this is not a new framework replacing HookKit, it is HookKit's next
major version.

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
8. Full HookKit 2.x binary ABI compatibility through `HKSubstitutor`.
9. Binary compatibility with the `HKSubstitutor` subset present in HookKit
   1.x.
10. A reverse-dependency-audited decision on the v1.x Modulous/module API
    (see `V1_MODULE_COMPATIBILITY_AUDIT.md`).
11. A Shadow rewrite using one HookKit runtime and one complete
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
slot, publishes it (and mirrors it to any legacy cell) before the
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
emission, or executable allocation. All work begins from an explicit
runtime or legacy-facade call.

## Repository layout

The rewrite moves toward:

```text
Headers/
  HookKit.h                    # historical umbrella; imports new + legacy API
  HookKit/
    HookKit.h                  # new C API only — no Foundation pulled in
    HookKitBase.h
    HookKitTargets.h
    HookKitRuntime.h
    HookKitPlan.h
    HookKitResults.h
    HookKitArtifacts.h
    HookKitObjC.h
    HookKitSwift.h
    HookKitLegacy.h             # deprecated HKSubstitutor declarations
    Compat.h                    # imports HookKitLegacy.h

Sources/
  Core/          # HKRuntime, HKPlan, HKDomain, HKOperation, HKRouter,
                 # HKResult, HKReport, HKArtifactLedger, HKOwnership,
                 # HKOriginalSlot, HKEffects, HKExecutor, HKDiagnostics
  Platform/Apple/  # HKImages, HKMachO, HKMemory, HKProtections, HKPtrauth, ...
  Resolvers/     # export/import/private-symbol/shared-cache/ObjC/Swift resolvers
  Engines/
    ObjC/ Rebind/ NativeInline/ Memory/ Swift/ Providers/
  Compatibility/ # HKLegacyRuntime, HKLegacyRequestTranslator,
                 # HKLegacyResultMapper, HKLegacyImage, HKSubstitutor
  CompatibilityV1/ # only if the v1 module audit finds it's needed

Schemas/         # hookkit-artifact, hookkit-provider-evidence,
                 # shadow-hook-manifest, shadow-route-report

ProviderEvidence/ # libhooker/ ellekit/ substrate/ substitute/ dobby/ gum/

Tests/            # Host/ Device/ Holdout/ LegacyABI/ Fuzz/ Fixtures/
Tools/            # abi/ shadow-manifest-extract/ route-feasibility/ provider-audit/
docs/3.0/
```

The existing 2.x implementation (`Backends/`, `Internal/`, `native/`,
`HKSubstitutor.m`, `HKBackendRegistry.m`) coexists during migration. It is
removed only once the compatibility facade runs entirely over the new
runtime (end of Milestone 11) — not deleted preemptively, and not kept
around past that point either.

## Relationship to HookKit 2.x

Every 2.x mechanism this rewrite needs already exists and is, in most
cases, already correct: nine backends, a working ARM64 relocator, a Swift
vtable engine, an inline-hook ownership guard with explicit PENDING/TAINTED
states, vendored and integrated providers. HK3.0 is substantially a
*formalization* project — turning invariants that are currently enforced by
careful code and comments (see `native/hk_native.h`, `Internal/HKInlineGuard.c`)
into an explicit, versioned, testable ABI — not a from-scratch build. Treat
existing 2.x behavior as the reference implementation to conform to the new
contract, not as legacy code to discard and reinvent.

## Compatibility policy

Full policy: `LEGACY_ABI.md` (Milestone 1, pending) and
`V1_MODULE_COMPATIBILITY_AUDIT.md`.

Summary: HookKit 2.1.1 through 2.5.0 fixture binaries must run against
HookKit 3 unrecompiled. The v1.0.1 `HKSubstitutor` subset must run
unrecompiled. The full v1.x module ABI (`HookKitCore`, `HookKitModule`,
Modulous bundle loading) is preserved only if the reverse-dependency audit
finds real surviving consumers — see the audit doc for current findings.
