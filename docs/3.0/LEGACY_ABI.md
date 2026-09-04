# HookKit 3.0 — Legacy Compatibility Policy

Status: **canonical cutover implemented**. `HookKit.framework` is the only
3.0 framework identity; `HookKit3.framework` and its beta package are
retired. The canonical facade, packages, exports, and compatibility surface
ship together. Exact current exports and package/linker
metadata are release-gated; historical ABI snapshots are not.

## What must keep working, unrecompiled

Binary ABI compatibility (old `.o`/binaries load and run against HookKit 3
with no recompile, no relink):

- HookKit v2.1.1, v2.2.0–v2.2.5, v2.3.0, v2.4.0, v2.5.0
- The full v1.0.1 seven-class surface: `HKSubstitutor` plus `HookKitCore`,
  `HookKitModule`, `HookKitHook`, `HookKitClassHook`, `HookKitFunctionHook`
  and `HookKitMemoryHook`

Historical tagged headers and `.tbd` files informed the compatibility policy
and remain available in Git history. The current release does not rebuild or
compare historical selector/type-encoding snapshots.

Source compatibility (old `#import`s keep compiling, undeprecated-but-marked):

```objc
#import <HookKit.h>
#import <HookKit/HookKit.h>
```

```objc
#import <HookKit/Compat.h>   // v1 spelling; forwards to the umbrella
#import <HookKit/Core.h>     // v1 module classes
#import <HookKit/Hook.h>
#import <HookKit/Module.h>
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
`current_version = 3.0.1`, `compatibility_version = 2.5.1`. Exports the new
`hk_*` C ABI plus `_OBJC_CLASS_$_` / `_OBJC_METACLASS_$_` for all seven v1
classes. Everything else stays hidden — enforced today by
`tools/release/check_exports.sh` against
`packaging/exports/export-HookKit.list`.

Every package is version `3.0.1-1`. Canonical modern packages conflict with
and replace `me.jjolano.fmwk.hookkit.legacy`; the legacy package reciprocally
conflicts with, replaces, and provides `me.jjolano.fmwk.hookkit`.

## v1.x module API (`HookKitCore`/`HookKitModule`/Modulous)

`V1_MODULE_COMPATIBILITY_AUDIT.md` found no consumer of the six v1 module
classes and recommended retaining the `HKSubstitutor` subset alone. That
recommendation is superseded: the classes ship, as translators. The audit's
factual findings still stand and still govern the one part not restored.

**Restored** (`src/compatibility/HKLegacyModules.m`): all six classes, on
the same plan lifecycle `HKSubstitutor` drives. v1 put its provider seam at
`Module+Internal.h`, so every public `HookKitModule` method is v1's own code
unchanged and only the eight `_hook*` / `_openImage:` / `_findSymbol:image:`
primitives are new. `HookKitCore` serves one built-in module;
`registerModule:withIdentifier:` still stores and returns a caller's own
subclass.

**Not restored**: Modulous. No bundle is read from `/Library/Modulous/HookKit`,
`getModuleInfo` returns exactly one row for the built-in module, and the
`me.jjolano.hkmodule.*` packages stay in
`packaging/layout/DEBIAN/control`'s Conflicts/Replaces/Provides. A v1 consumer
that reads a *specific* provider identifier out of
`getModuleInfo` sees one HookKit row instead of a per-library list — the one
behavioural difference from v1, and a consequence of provider identity being
retired facade-wide (see "Provider identity" above), not of the shim.

Two translation choices worth knowing:

| v1 primitive | 3.0 behaviour |
|---|---|
| `_hookFunctions:` | Specs pre-built, then one plan for the batch; returns the exact installed count |
| `_hookRegions:` | Returns `-1` (declines). `expected_bytes` must be captured at execute time, so there is nothing to pre-build; v1's own per-op fallback runs |

Duplicate target identities in one `_hookFunctions:` batch are rejected per
operation. Callers that intentionally chain a target submit separate batches
after the earlier batch has committed.

## Test system pointer

`tests/device/device_legacy_abi.m` (both the `HKSubstitutor` and v1 module-class
sections), `tests/device/device_legacy_facade.m`, and
`tests/device/device_compat_smoke.m` are retained for current-device smoke.
`make test-header-compile` checks the current public headers, release builds
check package/linker metadata, and `tools/release/check_exports.sh` checks the
exact current export set. There is no historical selector/type-encoding or ABI
snapshot release gate. The accepted release bar does not claim physical armv7,
armv7s, or arm64e verification.
