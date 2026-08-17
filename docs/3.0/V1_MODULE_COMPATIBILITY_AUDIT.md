# HookKit 1.x Module API — Reverse-Dependency Audit

Status: **in progress**, local evidence gathered, public-source search not
yet done. Required by spec §2.3 before ABI freeze (Milestone 3). Governs the
policy stated in `LEGACY_ABI.md` and the `CompatibilityV1/` decision in
`ARCHITECTURE.md`.

## What's being audited

Whether any real, current consumer links the HookKit 1.x module classes —
`HookKitCore`, `HookKitModule`, `HookKitHook`, `HookKitClassHook`,
`HookKitFunctionHook`, `HookKitMemoryHook` — or loads a HookKit Modulous
provider bundle. If yes, HK3.0 must preserve the full v1 module ABI, not
just the v1 `HKSubstitutor` subset (§2.3 policy).

## Method so far

Repo-wide grep for the six class names plus "Modulous" across every local
sibling repo under `/home/coder/projects/ios/`: `shadow`, `AltList`,
`libSandy`, `RootBridge`, `hksmoke`, `prebuilt`, `ios-repo`, `coder-connect`,
`Modulous` itself. Also checked `ios-repo`'s package metadata
(`.stage/releases.ndjson`, `.stage/changelog.json`,
`depictions/ios/me.jjolano.shadow.json`) — real "package metadata" per the
spec's own audit-source list (§2.3: "public source repositories, package
metadata, and known HookKit consumers"). Also confirmed current HookKit 2.x
source (`Headers/`, `Backends/`, `Internal/`, `native/`) has zero references
to any of the six class names — the replacement described in `README.md`
("This replaces the v1 Modulous plugin-bundle architecture") is complete on
the provider side already.

**Not yet done**: search of public source repositories/package indices
beyond what's checked out locally. Flagged as an open gap below rather than
silently treated as covered.

## Findings

1. **Shadow (flagship consumer) planned a Modulous migration but never
   shipped it.** Public release notes recorded in `ios-repo`'s package
   metadata: v3.6.9 (2023-03-04) says "The upcoming version 3.7.x of Shadow
   will migrate to using the Modulous Framework for its hooks... **Edit:
   This migration will be in version 4**"; v3.7.1 (2023-03-28) repeats "version
   4 will migrate to using the Modulous framework." No `v4` tag exists in the
   release history — the highest tag ever published is `v3.7.6`
   (2023-05, matching `v5-PLAN.md`'s own note that "Shadow upstream is
   abandoned, last release v3.7.6, May 2023"). **The Modulous migration was
   announced twice and shipped never.**

2. **The current Shadow revival explicitly rejected Modulous.** Shadow's
   `docs/v5-PLAN.md` (superseded, but this specific decision was carried out)
   states under its W0 workstream: "HookKit collapsed to a slim direct-hooking
   layer (ElleKit + fishhook, no plugin system)... **Modulous deleted**."
   Confirmed against Shadow's actual current `master`: `.gitmodules` lists
   only `vendor/HookKit.framework`, no Modulous submodule; zero "Modulous"
   references in any Makefile or `control` file repo-wide.

3. **No other local sibling repo references the six class names or
   Modulous**, with one non-consuming mention: `RootBridge/README.md` cites
   "HookKit or Modulous frameworks as an example" of correct `@rpath`
   install-name hygiene — a build-convention footnote, not an API dependency.
   `AltList`, `libSandy`, `hksmoke`, `coder-connect` have no matches at all.

4. **Current HookKit 2.x source has already fully removed the v1 module
   implementation** — confirmed by grep, zero matches for any of the six
   class names anywhere in `Headers/`, `Backends/`, `Internal/`, `native/`.
   There is nothing on the provider side left to accidentally keep binary
   compatibility with beyond the v1 `HKSubstitutor` subset, which is already
   what's shipped and tested (v1.0.1 tag present, `_OBJC_CLASS_$_HKSubstitutor`
   exported in every current build lane per the Milestone 0 baseline).

## Trending conclusion (not yet final — public-source gap open)

**Preserve the v1 `HKSubstitutor` subset unconditionally** (already true
today) and **do not preserve the full v1.x module ABI** — no real, shipped
consumer has been found anywhere, local or in the flagship consumer's own
published history, and the one serious attempt at adopting Modulous was
reversed before ever reaching a release.

This satisfies spec §2.3's exit condition ("if full module ABI is not
preserved, state that clearly in migration documentation and retain
installable HookKit 2.5 packages for those consumers") as a *provisional*
recommendation. Final sign-off requires closing the public-source-search
gap above before Milestone 3's ABI freeze — tracked in
`IMPLEMENTATION_STATUS.md`.

## What HK3.0 does instead

`CompatibilityV1/` (spec §5 repo layout) is not created. `LEGACY_ABI.md`'s
policy — preserve `HKSubstitutor` exactly, do not implement
`HookKitCore`/`HookKitModule`/bundle loading — stands unless the
public-source check surfaces a real consumer, in which case this document
gets updated before freeze, not after.
