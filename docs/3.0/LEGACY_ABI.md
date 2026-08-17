# HookKit 3.0 — Legacy Compatibility Policy (draft)

Status: **draft for Milestone 1** — policy only. The facade implementation
is Milestone 11; the ABI fixture/baseline test system is built alongside it.
See `V1_MODULE_COMPATIBILITY_AUDIT.md` for the v1 module ABI decision this
policy depends on.

## What must keep working, unrecompiled

Binary ABI compatibility (old `.o`/binaries load and run against HookKit 3
with no recompile, no relink):

- HookKit v2.1.1, v2.2.0–v2.2.5, v2.3.0, v2.4.0, v2.5.0
- The `HKSubstitutor` API surface present in HookKit v1.0.1

All 11 tags are present locally (verified in `IMPLEMENTATION_STATUS.md`,
Milestone 0) — the fixture/baseline work in Milestone 11/17 builds against
real tagged headers and `.tbd` files, not reconstructions.

Source compatibility (old `#import`s keep compiling, undeprecated-but-marked):

```objc
#import <HookKit.h>
#import <HookKit/Compat.h>
```

Every historical `hookkit_status_t` value, `hookkit_lib_t` bit,
`hookkit_cat_t` bit, `HKStrategy` value, `HKSubstitutor` selector and
property encoding, and existing convenience macro is preserved exactly.
Deprecated, never gated behind a special compiler flag to keep building.

## What this does *not* mean

The legacy facade translates old calls into permissive legacy request
profiles against the new runtime — it does not get to weaken new-API
semantics to make that translation convenient. Concretely (spec §1.4):

- A legacy call with `old_ptr == NULL` may still permit continuation
  machinery, because that was always historical behavior for those call
  shapes — but this is expressed as a *specific legacy translation rule*
  (§ Request translation below), never as a change to what
  `HK_CONTINUATION_FORBIDDEN` means for new-API callers.
- New requests built with `HK_CONTINUATION_FORBIDDEN` never get continuation
  machinery, full stop, regardless of what the legacy path does elsewhere in
  the same process.
- Legacy provider selection (`HK_LIB_DOBBY`, `HK_LIB_FRIDA`, ...) is allowed
  to use a private compatibility SPI to name a provider. New public C
  callers are never allowed to name a provider or engine — that stays a
  legacy-only escape hatch, not a new capability smuggled into the modern
  API through the facade's plumbing.

## Request translation (policy summary)

Full table lives in the facade implementation notes (Milestone 11); the
governing rules going in:

| Legacy call shape | New-API translation |
|---|---|
| Message hook | ObjC target; direct predecessor iff `old_ptr != NULL`; permissive legacy effect constraints |
| Function hook, no original | `HK_ORIGINAL_NONE` / `HK_CONTINUATION_ANY` — never inferred as continuation-forbidden just because `old_ptr` is null |
| Explicit inline + original | `HK_ORIGINAL_CALLABLE_CONTINUATION` |
| Explicit rebind + original | `HK_ORIGINAL_DIRECT_PREDECESSOR` |
| Automatic inline-then-rebind + original | Tries `CALLABLE_CONTINUATION` via the legacy inline profile first; only on confirmed `HK_MUTATION_NONE` (no route, no mutation) falls back to `DIRECT_PREDECESSOR` via the legacy rebind profile. These stay two distinct new-API analyses under a private compatibility route-selection rule — **not** a new "any original kind" value added to the public ABI. |
| Memory hook | Private compatibility mode: bytes captured at preparation become commit preconditions, revalidated before commit, full artifacts emitted, explicitly marked as having no consumer-supplied expected bytes |
| Swift hook | New Swift engine, direct-predecessor requirement |

## Provider identity vs. reported identity

Legacy code can request `HK_LIB_DOBBY`, `HK_LIB_FRIDA`, `HK_LIB_SWIFT`, etc.
by name. Where the new engine backing that name is semantically compatible,
`activeType` keeps reporting the legacy identity the caller asked for; the
*actual* engine identity that served the request is only visible through new
diagnostics/artifacts (`hk_hook_result_t.diagnostic_engine_id`), never
substituted silently into the legacy-facing value. An explicit request for
an unavailable legacy provider returns legacy unsupported behavior — it
never silently switches identity to something else that happened to be
available.

## Framework identity

Canonical 3.0 build: `@rpath/HookKit.framework/HookKit`,
`current_version = 3.0.0`, `compatibility_version = 3.0.0`. Exports the new
`hk_*` C ABI, `_OBJC_CLASS_$_HKSubstitutor` /
`_OBJC_METACLASS_$_HKSubstitutor`, and any v1 classes the module audit
decides to retain. Everything else stays hidden — enforced today by
`scripts/check_exports.sh` (ran clean against all 4 lanes as part of the
Milestone 0 baseline) and extended, not replaced, for the new ABI symbols.

Beta identity (`HOOKKIT_BETA_IDENTITY=1` build variable, not a source fork):
`HookKit3.framework` / `@rpath/HookKit3.framework/HookKit3`, new API only,
`HKSubstitutor` deliberately **not** exported — this is what lets the beta
runtime and a real HookKit 2.5 coexist in one process (e.g. during Shadow's
own migration testing) without a duplicate Objective-C class definition
crash.

## v1.x module API (`HookKitCore`/`HookKitModule`/Modulous)

Governed by `V1_MODULE_COMPATIBILITY_AUDIT.md`, not decided here. Current
audit finding (started at Milestone 0, not yet concluded): Shadow — the
flagship and previously-only-known consumer — has zero Modulous references
anywhere in its current build files; the Modulous-deletion decision in
Shadow's own (superseded) `v5-PLAN.md` was actually executed. This trends
toward "preserve the v1 `HKSubstitutor` subset only," pending a check of the
other local sibling repos and any public consumers, per spec §2.3's
required audit before ABI freeze.

## Test system pointer

Milestone 11 builds `Tests/LegacyABI/Baselines/{v1.0.1,v2.1.1,v2.2.0...v2.5.0}.json`
(exported symbols, ObjC classes/methods/encodings, enum values, header
checksums) plus fixture clients built against the real historical headers
and `.tbd`s, run unrecompiled against HookKit 3. `Tools/abi/extract_abi.py` /
`compare_abi.py` / `scripts/check_legacy_abi.sh` fail CI on any removed
class/selector, changed encoding, changed enum value, missing install name,
missing architecture, wrong compatibility version, or missing historical
umbrella header. Not built yet — tracked in `IMPLEMENTATION_STATUS.md`
under Milestone 11.
