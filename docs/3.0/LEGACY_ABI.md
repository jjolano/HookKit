# HookKit 3.0 — Legacy Compatibility Policy

Status: **canonical cutover implemented**. `HookKit.framework` is the only
3.0 framework identity; `HookKit3.framework` and its beta package are
retired. The canonical facade, packages, exports, and five historical ABI
baselines are release-gated together.

## What must keep working, unrecompiled

Binary ABI compatibility (old `.o`/binaries load and run against HookKit 3
with no recompile, no relink):

- HookKit v2.1.1, v2.2.0–v2.2.5, v2.3.0, v2.4.0, v2.5.0
- The retained `HKSubstitutor` subset recorded by `V1_MODULE_COMPATIBILITY_AUDIT.md`

All historical tags are present locally (verified in `IMPLEMENTATION_STATUS.md`,
Milestone 0) — the fixture/baseline work in Milestone 11 builds against
real tagged headers and `.tbd` files, not reconstructions.

Source compatibility (old `#import`s keep compiling, undeprecated-but-marked):

```objc
#import <HookKit.h>
#import <HookKit/HookKit.h>
```

Every historical `hookkit_status_t` value, `hookkit_lib_t` bit,
`hookkit_cat_t` bit, `HKStrategy` value, `HKSubstitutor` selector and
property encoding, and existing convenience macro is preserved exactly.
Deprecated, never gated behind a special compiler flag to keep building.

## Canonical facade behavior

`HKSubstitutor` is a compatibility translator, not a second runtime. Every
mutating legacy call creates and drives a HookKit 3 plan; the retired 2.x
router and backend implementations are not linked into canonical packages.

- Type/category inputs are accepted for source compatibility but do not select
  an engine. Discovery returns `HK_LIB_NONE` / `HK_CAT_NONE`; `activeType` is
  `HK_LIB_NONE` and `activeStrategy` is `HKStrategyDefault`.
- ObjC, function, memory, and supported Swift calls translate to their HK3
  target types. Batching drains queued message/function/memory requests once,
  in order, through HK3 lifecycles.
- The legacy lane may use the HK3 Substitute/Substrate provider adapter for a
  function entrypoint when its audited ABI is present. A provider failure that
  may have mutated state is terminal and is never retried through an old
  fallback.
- ARMv7/ARMv7s memory and Swift routes fail closed when no certified HK3
  engine exists; no raw legacy writer or ARM/Thumb relocator is retained.
- `getLibErrno:` returns the normalized final HookKit status and always writes
  `HK_LIB_NONE` to its optional type output.

## Request translation (policy summary)

Full table lives in the facade implementation notes (Milestone 11); the
governing rules going in:

| Legacy call shape | New-API translation |
|---|---|
| Message hook | ObjC target; direct predecessor iff `old_ptr != NULL`; permissive legacy effect constraints |
| Function hook, no original | `HK_ORIGINAL_NONE` / `HK_CONTINUATION_ANY` — never inferred as continuation-forbidden just because `old_ptr` is null |
| Function hook with original | `HK_ORIGINAL_CALLABLE_CONTINUATION`; a provider continuation is published after successful commit |
| Memory hook | Private compatibility mode: bytes captured at preparation become commit preconditions, revalidated before commit, full artifacts emitted, explicitly marked as having no consumer-supplied expected bytes |
| Swift hook | New Swift engine, direct-predecessor requirement |

## Provider identity

Legacy provider identity and auto-cover routing are deliberately retired.
The facade reports no selected provider (`HK_LIB_NONE`); HK3 chooses only an
eligible built-in engine and reports its actual identity through HK3 results
and artifacts.

## Framework identity

Canonical 3.0 build: `@rpath/HookKit.framework/HookKit`,
`current_version = 3.0.0`, `compatibility_version = 2.5.1`. Exports the new
`hk_*` C ABI, `_OBJC_CLASS_$_HKSubstitutor` /
`_OBJC_METACLASS_$_HKSubstitutor`, and any v1 classes the module audit
decides to retain. Everything else stays hidden — enforced today by
`scripts/check_exports.sh` and the five historical ABI baselines.

Every package is version `3.0.0-1`. Canonical modern packages conflict with
and replace both `me.jjolano.fmwk.hookkit.legacy` and the retired
`me.jjolano.fmwk.hookkit3`; the legacy package reciprocally replaces the
modern and retired packages.

## v1.x module API (`HookKitCore`/`HookKitModule`/Modulous)

Governed by `V1_MODULE_COMPATIBILITY_AUDIT.md`, not decided here. Current
audit finding (started at Milestone 0): Shadow — the
flagship and previously-only-known consumer — has zero Modulous references
anywhere in its current build files; the Modulous-deletion decision in
Shadow's own (superseded) `v5-PLAN.md` was actually executed. This trends
toward "preserve the v1 `HKSubstitutor` subset only"; the public-consumer
search/sign-off is still an ABI-freeze gate.

## Test system pointer

`tests/device_legacy_abi.m` and `tests/device_legacy_facade3.m` are retained
for current-device smoke. `scripts/check_legacy_abi.sh` validates all five
historical JSON baselines against each built framework. The accepted release
bar is package/source/ABI validation across all lanes plus the existing arm64
device smoke; it does not claim physical armv7, armv7s, or arm64e verification.
