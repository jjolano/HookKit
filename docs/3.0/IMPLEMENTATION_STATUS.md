# HookKit 3.0 — Implementation Status

Live ledger for the HookKit 3.0 rewrite. Updated in every milestone commit.
States: **not started**, **in progress**, **blocked**, **complete**.

Scope, invariants, and milestone definitions live in the master spec (not
checked in verbatim — see `ARCHITECTURE.md` for the condensed version of what
actually governs implementation). This file tracks *status only*.

Two things this rewrite must not disturb without explicit sign-off, recorded
here because they are easy to forget mid-milestone:

- `../shadow` has an active, device-verified execution plan
  (`shadow/docs/STEALTH-HARDENING-PLAN.md`) unrelated to this rewrite. Do not
  touch that repo until a milestone explicitly calls for it (Milestone 14) and
  the user has signed off.
- Nothing in this repo may claim device-verified, fuzz-tested, or
  performance-gated status without an actual run. Every entry below states
  its verification level explicitly: **host-verified**, **device-verified**,
  or **not yet verified**.

---

## Milestone 0 — Baseline freeze

**State: complete.**

| Task | State | Evidence |
|---|---|---|
| Clean host build/test | complete (host-verified) | `make test` — 5 suites, exit 0 |
| Clean builds, all 4 packaging lanes | complete (host-verified, cross-build) | `./build.sh all` — rootful-legacy, rootful-modern, rootless, roothide all exit 0; `check-exports`/`check-compat` 64/64 PASS |
| Historical tags archived/referenced | complete | all 11 tags present locally: v1.0.1, v2.1.1, v2.2.0–v2.2.5, v2.3.0, v2.4.0, v2.5.0 |
| HookKit 2.5 performance baseline | not started | needs device access (launch CPU, RSS, VM region count, hook-install time — see spec §28.3) |
| Shadow performance baseline | not started | needs device access; also blocked on not touching `shadow/` yet |
| Reverse-dependency audit (v1 module API) — started | in progress | see `V1_MODULE_COMPATIBILITY_AUDIT.md` (started, not concluded) |
| This ledger | complete | — |

Commit: `3e6bbdb` (native engine concurrency hardening — pre-existing
in-progress work reviewed and committed to reach a clean tree; not itself
HK3.0 architecture, but the baseline this ledger measures from).

Deviation from spec: performance baselines deferred — they require the
physical jailbroken test device, which has not been engaged yet this
initiative. Will request device time when a milestone actually needs it
rather than blocking Milestone 0 exit on it, since nothing downstream reads
the performance baseline until the release gates in §28.4.

---

## Milestone 1 — Architecture documents and schemas

**State: complete.**

| Task | State | Evidence |
|---|---|---|
| `ARCHITECTURE.md` | complete | commit `59d291c` |
| `PUBLIC_C_ABI.md` (draft) | complete | commit `59d291c` |
| `Schemas/hookkit-artifact.schema.json` | complete | commit `53a209f` |
| `Schemas/hookkit-provider-evidence.schema.json` | complete | commit `53a209f` |
| `Schemas/shadow-hook-manifest.schema.json` | complete | commit `53a209f` |
| `Schemas/shadow-route-report.schema.json` | complete | commit `53a209f` |
| `ENGINE_CONTRACT.md` (draft) | complete | this commit |
| Legacy compatibility policy doc (`LEGACY_ABI.md`) | complete | this commit |
| `V1_MODULE_COMPATIBILITY_AUDIT.md` | in progress (trending conclusion recorded; public-source search still open) | this commit |

Schema validation: all four `Schemas/*.json` files verified as both valid
JSON and structurally valid JSON Schema (draft 2020-12) via Python's
`jsonschema.validators.validator_for(...).check_schema()` — host-verified,
not just eyeballed.

Audit progress this iteration: checked every local sibling repo plus
`ios-repo`'s package metadata (real release notes/depictions, not just
source) for v1 module API consumers. Found Shadow announced a Modulous
migration twice (2023) for a "version 4" that was never tagged/released —
upstream stalled at v3.7.6 — and the current revival's own (superseded)
plan explicitly deleted Modulous, confirmed against current `shadow` master
(zero refs). Trending conclusion: v1 `HKSubstitutor` subset only, no full
module ABI. Not finalized — public-source search beyond local checkouts is
still open, tracked below.

Exit gate met: every non-negotiable concept from the spec has a named
representation across `ARCHITECTURE.md`/`PUBLIC_C_ABI.md`/`ENGINE_CONTRACT.md`/
`LEGACY_ABI.md`/the four schemas, and no native inline implementation work
has started (nothing under `Sources/Engines/` or `native/` touched this
milestone).

---

## Milestone 2 — Shadow manifest extraction
**State: in progress.**

| Task | State | Evidence |
|---|---|---|
| `Tools/shadow-manifest-extract/extract.py` | in progress (install-unit tier + `hookFunction:` pattern-scan tier) | commit `5970255` + this commit |
| `Tools/shadow-manifest-extract/validate.py` | complete | commit `5970255` |
| `Tools/shadow-manifest-extract/manual_overrides.yaml` | complete (format defined, still empty — the pattern-scan pass hasn't needed one yet) | commit `5970255` |
| `Tools/shadow-manifest-extract/test_extract.py` | in progress, grows with the extractor | this commit (added a reused-variable regression test) |
| `artifacts/shadow-current-manifest.json` | in progress — 113 targets: 22 install units + 91 pattern_scan child targets, from 9/22 units fully decomposed | this commit |
| `shdw_libc_install_group` descriptor parse | not started | covers 4 more units (libc/libc_lowlevel/libc_antidebugging/envvar) once done — it's a large table inside `libc.x`, not yet read closely |
| `logos_preprocess.py` | not started | needed for the 7+ files still using raw Logos `%hook` blocks |
| `clang_ast_extract.py` | not started | needed for per-hook decomposition generally, and as a stronger cross-check on the pattern_scan rows |
| Initial route-feasibility report | not started | depends on the above being further along |

This iteration deepened extraction past the install-unit tier for real,
rather than starting the route-feasibility report on top of a manifest that
was still 0% decomposed. Read every `shadowhook_*` entry function across all
22 `.x` files by hand first (`awk` survey of each function body) and found
the real pattern mix is more varied than one file suggested: direct
`[hooks hookFunction:...]` calls (7 files), delegation to a shared
`shdw_libc_install_group(hooks, GROUP)` (4 files, libc-family), real Logos
`%hook`/`%init` blocks (7+ files, confirmed with a comment-excluded `grep`
for `^%hook`/`^%end`/`^%init`), a descriptor table like DeviceCheck's
(1 file confirmed, more may exist unchecked), and two genuine special cases
(`vnode.x` is a pure IPC client with no HookKit substitution at all;
`objc_methodimpl.x` captures a function pointer directly, not through
`hookFunction:`).

Built the `hookFunction:` tier for the first pattern (structured, comment/
string-safe, brace-depth-aware scan — not a naive regex over raw text,
learned that lesson for real earlier this session). Also parsed
`kSHDWCoordinatorInstallers[]` (`dylib.x`) — the other real structured table
spec section 18.2 names — which cross-references cleanly against
`kSHDWInstallUnits[]` (both tables agree on all 22 unit IDs, a genuine
consistency check, not assumed) and gives every unit's exact
`current_implementation` function name mechanically.

validate.py caught two real bugs before either reached a commit: (1) an
extracted `null` for an optional string field the schema requires be a
string-or-absent — a bug in the earlier install-unit pass, fixed by omitting
the field instead of nulling it; (2) `resolve_symbol_variable` resolving
every reused-variable hook call in a function to the SAME symbol (mach.x
reuses one `sym` local across 7 resolve-then-hook steps; the first version
took the first assignment anywhere in the function instead of the nearest
one before each call site) — this one showed up as real duplicate
`stable_hook_id`s against the live repo, not a synthetic test failure. Fixed
and now covered by `test_reused_variable_resolves_to_nearest_preceding_assignment`.
Spot-checked the fix by hand against `mach.x`'s real 9 hook sites (2 direct
+ 7 via the reused variable) — exact match.

Added `pattern_scan` as a fifth `extraction_method` enum value
(`shadow-hook-manifest.schema.json`) alongside last iteration's
`structured_table` addition — a hand-rolled call-site scan over free-form
source is real evidence, weaker than a compiler-grade parse, and deserves
its own confidence label rather than being folded into `static_ast` (which
now specifically means real Clang-AST-backed extraction, not yet built) or
overstating `structured_table` (which means a literal struct-array parse).

What actually happened this iteration, precisely: read real Shadow source
(`ShadowCore.dylib/hooks/**/*`, `HookCoordinator.m`,
`Shadow.framework/HookConfiguration.{h,m}`) rather than build the extractor
blind against the spec's description. Found `kSHDWInstallUnits[]` — a real,
clean C array-of-structs the coordinator itself walks via `SHDWInstallUnits()`
— and built `extract.py` to parse it directly (proper comment/string-aware
tokenizer, not regex-over-raw-text: caught myself matching "%hook" inside a
plain code comment earlier this session with a naive `grep`, so the parser
strips comments correctly before scanning, and `test_extract.py` has a
regression fixture for exactly that mistake). Ran it for real, read-only,
against the live `shadow` repo (commit `efbc732e`) — 22/22 rows parsed,
schema-valid, cross-checked by hand against three real source files (see
below). Added `structured_table` as a fourth `extraction_method` enum value
in `shadow-hook-manifest.schema.json` (was `static_ast`/`logos_preprocess`/
`manual_override` only) because a direct array-of-structs parse is a
meaningfully stronger guarantee than either of those and deserves its own
label, not a shoehorned one — found only after seeing the real source, not
anticipated when the schema was written last iteration.

**What this is honestly NOT yet**: `SHDWInstallUnit` rows are
coordinator-level groups (e.g. `"Hook_Filesystem@c"`), not the individual
hook targets inside them. Confirmed by reading `DeviceCheckHooks.h`/`.m`:
that group alone has its own clean structured descriptor table
(`shdw_devicecheck_descriptors[]`, 6-field rows: class, selector, kind,
encoding, argCount, policy) which a similar direct-parse pass could cover —
but I have not yet checked whether the other ~19 groups all follow that
same descriptor-table shape or fall back to raw Logos `%hook` blocks (a real
mix exists: `grep` for real `^%hook`/`^%end` directives, comment-excluded,
found 7 files that still use them). Every extracted target's
`known_compatibility_risks` says this explicitly rather than silently
implying full coverage. `original_requirement`/`required_reach` per target
are reasoned defaults cited from the real `SHDWCapabilityKind` doc comments
in `HookConfiguration.h`, not verified per-hook — flagged as such in
`extract.py`'s module docstring, to be tightened by the next pass or
`manual_overrides.yaml`.

Boundary note: everything above was read-only against `shadow/` (source
reads + `git rev-parse HEAD` for provenance) — no commits, no build/Logos
preprocessing run against it yet. Running the actual Logos preprocessor or
Clang AST tooling against `shadow/`'s tree (needed for the per-hook
decomposition pass) is a bigger, noisier action than a source read and will
get its own heads-up before it happens, per the standing boundary at the
top of this file.

## Milestone 3 — ABI freeze candidate
**State: not started.**

## Milestone 4 — Core runtime and fake engines
**State: not started.**

## Milestone 5 — Image catalog and resolvers
**State: not started.**

## Milestone 6 — Non-generated-code engines (ObjC, rebind, memory, Swift)
**State: not started.** Note: HookKit 2.x already has working, host-and-device-tested implementations of all four (`Backends/`, `native/hk_swift.c`) — this milestone is about conforming them to the new engine contract and ABI, not writing them from scratch.

## Milestone 7 — Native terminal inline
**State: not started.** Note: the atomicity work committed at `3e6bbdb`
(one-page-per-trampoline, atomic near-branch via inbound thunk) is a real
head start on this milestone's mechanism, done inside the 2.x engine. Formal
"zero relocation / zero trampoline / zero executable allocation for strict
requests" separation per spec §13.4 is not yet implemented.

## Milestone 8 — Native relocating inline
**State: not started.** Note: `native/hk_arm64.c` relocator already exists
and is host-tested (`make test-reloc`) — this milestone hardens/ports it, not
build from zero.

## Milestone 9 — Static continuation decision
**State: not started.**

## Milestone 10 — Provider adapters
**State: not started.** Note: 2.x already vendors and integrates ElleKit,
Substrate, Substitute, Dobby, Frida/gum, litehook, fishhook (`vendor/`,
`Backends/`) — this milestone is evidence + certification against the new
contract, not new integration work.

## Milestone 11 — Legacy facade
**State: not started.**

## Milestone 12 — Deferred lifecycle
**State: not started.**

## Milestone 13 — Packaging beta
**State: not started.**

## Milestone 14 — Shadow migration
**State: not started.** Requires explicit user sign-off before any commit in
`shadow/` — see boundary note at top of this file.

## Milestone 15 — Canonical release candidate
**State: not started.**

---

## Open deviations from the spec (running list)

1. Performance baselines (Milestone 0) deferred pending device access — see
   above.
2. `V1_MODULE_COMPATIBILITY_AUDIT.md` conclusion is provisional — the
   public-source-search leg of spec §2.3's audit isn't done yet. Must close
   before Milestone 3's ABI freeze, not before Milestone 1's own exit gate
   (which only needs the concept named, which it now is).
