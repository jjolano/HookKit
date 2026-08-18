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
| `Tools/shadow-manifest-extract/extract.py` | in progress (install-unit + `hookFunction:` pattern-scan + `shdw_libc_hooks[]` structured-table tiers) | commits `5970255`, `bd52025`, this commit |
| `Tools/shadow-manifest-extract/validate.py` | complete | commit `5970255` |
| `Tools/shadow-manifest-extract/manual_overrides.yaml` | complete (format defined, still empty — nothing's needed one yet) | commit `5970255` |
| `Tools/shadow-manifest-extract/test_extract.py` | in progress, grows with the extractor | 8 tests total as of this commit |
| `artifacts/shadow-current-manifest.json` | in progress — 186 targets: 22 install units + 164 children (91 pattern_scan + 73 structured_table), 13/22 units decomposed | this commit |
| `shdw_libc_install_group` descriptor parse | **complete** | this commit — see below |
| `shdw_coord_*` composite units (symlookup, filesystem_objc, foundation_objc, hideapps) | not started | real finding this iteration: several units are coordinator-side composites calling 2+ named functions, not 1:1 with a single shadowhook_ body — see below, needs its own (bounded) resolution pass, not a full call-graph walker |
| `logos_preprocess.py` | not started | needed for the 7+ files still using raw Logos `%hook` blocks |
| `clang_ast_extract.py` | not started | needed for per-hook decomposition generally, and as a stronger cross-check on both pattern_scan and structured_table rows |
| Initial route-feasibility report | **complete (modeled pass)** | this commit — `Tools/route-feasibility/hk_route_report.py`, `artifacts/shadow-route-feasibility.json`, `docs/3.0/SHADOW_ROUTE_FEASIBILITY.md` |

**`shdw_libc_hooks[]` decomposition**, done this iteration: another real,
clean `shdw_hook_desc_t` array (72 rows: symbol, replacement, original,
installGroups bitmask, verifyGroups bitmask) in `libc.x`, shared across 4
install units (`Hook_Filesystem@c`, `Hook_LowLevelC`, `Hook_AntiDebugging`,
`Hook_EnvVars@c`) via the bitmask. The unit-to-group mapping is hand-verified
against real source, not inferred — cited per-constant in `extract.py`'s
`GROUP_TO_UNIT` comment, including the one indirect case
(`Hook_EnvVars@c`'s installer, `shdw_coord_envvars_c` in `dylib.x`, does its
own `unsetenv`/`setenv` scrubbing directly — not through HookKit at all —
*and* calls `shadowhook_libc_envvar(hooks)`, which is what actually reaches
`shdw_libc_install_group`). One symbol (`close`) is claimed by two groups
with different verify requirements (`LIBC | LOW` install, `LOW`-only
verify) — represented as two separate manifest rows
(`Hook_Filesystem@c::close`, `Hook_LowLevelC::close`), each with the correct
verification_method presence/absence, not collapsed into one. Covered by
`test_group_mask_split` / `test_libc_descriptor_table_dual_group_row`.

**Real finding not yet acted on**: reading `dylib.x`'s coordinator wrappers
closely turned up units whose installer is a composite function calling
*other* named `shadowhook_*` functions (`shdw_coord_symlookup` calls
`shadowhook_dyld_symlookup` + `shadowhook_dyld_symaddrlookup`;
`shdw_coord_filesystem_objc` calls 4 NSFile*-family functions) rather than
containing `[hooks hookFunction:...]` calls itself. The current pass
correctly reports these as "no call sites found... likely delegates"
(honest, not silently wrong) but doesn't follow the delegation. A bounded
one-level "if the body is just calls to other named functions, recurse into
each" pass would close this — deliberately not built yet this iteration to
avoid open-ended call-graph resolution; likely worth it as a small, scoped
next step rather than jumping straight to full Clang AST for it. Also
surfaced: `shadowhook_envpolicy` (called from `shdw_coord_envvars_c`) was
not found anywhere under `ShadowCore.dylib/hooks` — real gap, noted, not
silently dropped.

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

**Initial route-feasibility report**, this iteration: built
`Tools/route-feasibility/hk_route_report.py` per spec §18.4, deliberately
scoped to what the spec's own Milestone 2 language allows — "initial route
report using **modeled** engine capabilities," since no real
`hk_engine_vtable_t` exists yet (that's Milestones 4–10). The classifier is
a small, hand-authored table keyed on `(target_kind, required_reach)`,
against what's already true and working in HookKit 2.x
(`docs/3.0/ENGINE_CONTRACT.md`'s citations), not a reimplementation of the
real router — deliberately not attempting that, to avoid two divergent
router logics existing at once.

Result against the current 186-target manifest: 72 targets **routable**
(63 via plain import-rebind, 9 via the ObjC message engine — both
mechanisms HookKit 2.x already does today), 114 **needs_platform_decision**
— every one of those 114 resolved via runtime private-symbol lookup
(`findSymbolInImage`/`dlsym`), correctly *not* claimed as routable by a
specific engine, since which certified HK3 engine ends up serving
private-symbol-resolved targets is genuinely undetermined before Milestone
6/7/10 exist. `requires_dobby`/`requires_gum` both **false** — nothing
extracted so far demonstrates a need only those two specifically satisfy,
and the tool says so explicitly rather than guessing either way. Verified
the "all 114 unclassified targets share the identical blocking_reason"
claim in the generated doc against the actual JSON rather than asserting it
from the round numbers matching by coincidence. Output validated against
`Schemas/shadow-route-report.schema.json` (host-verified). 5 self-tests in
`Tools/route-feasibility/test_hk_route_report.py`.

Caveat stated in the report's own Markdown output, not just here: this
covers all 186 currently-extracted targets, but 9/22 Shadow install units
are still unit-level only (not yet decomposed into individual targets) —
so this is a first read, not the final answer spec §18.5's ABI-freeze gate
needs.

Caught before it shipped, not after: the first working version of the tool
literally repeated the same 186-target classification once per packaging
lane (`classify_target()` takes no lane argument, so it structurally cannot
differ per lane yet) — 15,632 lines of `artifacts/shadow-route-feasibility.json`
for 186 real targets, checked with `wc -l` on a hunch before committing.
Added `"all"` as a lane-agnostic shorthand to `shadow-route-report.schema.json`
(alongside `structured_table`/`pattern_scan` from earlier iterations — same
pattern of refining the schema against what actually turns out to be true,
not designing it perfectly upfront) and switched the tool to emit one `"all"`
entry instead of four identical ones. 3,917 lines now, same summary numbers,
still schema-valid. The fix is what real per-lane differentiation should
eventually replace, not a permanent shortcut — noted in both the schema's
own description and the tool's code comment.

## Milestone 3 — ABI freeze candidate
**State: in progress.**

| Task | State | Evidence |
|---|---|---|
| `Headers/HookKit/HookKitBase.h` | complete | this commit |
| `Headers/HookKit/HookKitTargets.h` | complete | this commit |
| `Headers/HookKit/HookKitResults.h` | complete | this commit |
| `Headers/HookKit/HookKitRuntime.h` | complete | this commit |
| `Headers/HookKit/HookKitPlan.h` | complete | this commit |
| `Headers/HookKit/HookKit.h` (new umbrella) | complete | this commit |
| Header compile tests (C, ObjC, C++, ObjC++) | complete | `Tests/Host/test_header_compile.{c,m,cpp,mm}`, wired into `make test` |
| `HookKitArtifacts.h` | complete | this commit — field-for-field transcription of `Schemas/hookkit-artifact.schema.json`; snapshot functions (`hk_report_copy_artifacts` etc.) declared only, not implemented (Milestone 4 territory) |
| `HookKitObjC.h` | not started | typed Class/SEL convenience wrappers |
| `HookKitSwift.h` | not started | |
| `HookKitLegacy.h` / `Compat.h` | not started | Milestone 11 territory, but the empty declaration exists in the spec's repo layout |
| ABI symbol manifest | in progress (symbols/version/archs done; ObjC metadata + enum values not yet) | `Tools/abi/extract_abi.py`, this commit — see below |

Deliberately safe by construction, not just by intent: the Makefile's
`HookKit_PUBLIC_HEADERS` still lists only the legacy `Headers/HookKit.h` —
these new files under `Headers/HookKit/` are not bundled into the built
framework and cannot affect `./build.sh`'s output. Verified, not assumed:
ran `./build.sh all` after adding them — same 64/64 PASS, same 4 `.deb`
artifacts, same benign warnings as the Milestone 0 baseline.

Two real gaps in the master spec's own text, filled in here (not
transcribed, since the spec never defined them): `hk_plan_config_t` is
referenced by `hk_plan_create` in section 6.30 but never given a field
list anywhere in the spec — filled with the minimal thing a plan actually
needs (an optional debug label), extend when a real requirement appears.
`hk_diagnostic_callback_fn` is referenced by `hk_runtime_config_t` in
section 6.29, same story — filled with a minimal string-message callback.
Both noted in-header, not silently invented.

One real design call made and stated, not left implicit: `hk_target_spec_t`
(`HookKitTargets.h`) is a union of symbol/address/objc/memory members —
deliberately no `swift` member. `HK_TARGET_SWIFT_VTABLE` still exists in
`hk_target_kind_t` for uniform reporting, but Swift hook *requests* are
meant to go through `HookKitSwift.h`'s own entry point (not yet written),
matching the master spec's repeated "separate API, no category membership"
language for Swift (section 13.7) — folding a Swift member into this union
would give `HookKitTargets.h` a dependency the rest of the design avoids.

Compile tests are real tests, not just "did it build": each one
`_Static_assert`s/`static_assert`s ABI-critical facts (the `HK_STRUCT_HEADER`
field order and offsets, several enum numeric values, a couple of bit
positions) that a mere successful compile wouldn't catch if silently
reordered, plus builds and inspects one real struct instance per file. The
ObjC/ObjC++ variants reuse the *existing* `tests/fake_headers` stub rather
than a new copy, and deliberately avoid `@selector()`/message sends — those
pull in the GNU ObjC runtime's module loader (`__objc_exec_class`) at link
time, which this host has no runtime to satisfy, the same constraint the
existing `test-substitute-classifier`/`test-original-publication` targets
already work within. All 4 compile clean under `-Wall -Wextra -Werror`
with zero warnings, host-verified.

**ABI symbol manifest / baseline extraction**, this iteration:
`Tools/abi/extract_abi.py` + `Schemas/hookkit-abi-baseline.schema.json`,
closing the loop on Milestone 0's still-open "ABI baseline manifests" task
at the same time as Milestone 3's own "ABI symbol manifest" deliverable —
one real tool serving both. Tool discovery (Mach-O-aware `nm`/`otool`/
`lipo`, via `$THEOS`'s toolchain layout) replicates
`scripts/check_compat.sh`'s `find_tool()` search order rather than
reinventing it — reused because that script isn't sourceable, not because
the logic was worth rederiving.

Extracted (real, from real binaries): install name, current/compatibility
version (parsed from `otool -l`'s `LC_ID_DYLIB` block — the regex proven
against an actual captured sample, with a regression test for
over-greedy matching across two `LC_ID_DYLIB`-shaped blocks in one file),
architectures, and exported symbols per arch. Cross-checked the extractor's
own output against ground truth it had no way to cheat from:
`scripts/export-HookKit.list` (the real, independently-maintained allowlist)
lists exactly `_OBJC_CLASS_$_HKSubstitutor`/`_OBJC_METACLASS_$_HKSubstitutor`
— exactly what `extract_abi.py` found. **Not implemented**, stated in the
schema and the tool's own docstring rather than faked empty: Objective-C
class/method/property metadata and historical enum numeric values — real
Mach-O ObjC metadata parsing is a meaningfully bigger task than load-command/
symbol-table reads, tracked as open work.

Produced one real baseline: `Tests/LegacyABI/Baselines/v2.5.0.json`,
schema-valid, from an actual `v2.5.0`-tagged build — not current HEAD
mislabeled as the release tag (HEAD has diverged from that tag by 9 commits,
even though none touch compiled source, so extracting from HEAD and calling
it "v2.5.0" would have been dishonest labeling for a claim this document is
supposed to be careful about). Used `git worktree add` to build the tagged
commit in complete isolation rather than `git checkout`-and-back in the main
tree — a plain checkout would have deleted `Tools/abi/extract_abi.py` itself
from the working directory the moment HEAD moved to a commit that predates
it, since it's a tracked file that doesn't exist in that tree. Worktree
removed cleanly afterward (`git worktree list` back to just the main tree);
`build/`/`.theos/` were touched by the interim `rootful-modern`-only builds
this required, so re-ran `make test` and `./build.sh all` afterward to
restore and reconfirm the full green baseline (64/64 PASS, 4 `.deb`s) —
not assumed still valid after the worktree detour.

Remaining 5 of the spec's 6 named baselines (`v1.0.1`, `v2.1.1`, `v2.2.5`,
`v2.3.0`, `v2.4.0`) not yet extracted — same worktree-build-extract
recipe, straightforward but time-costly (a full framework build per tag);
left for a future iteration rather than rushed through this one.

**`HookKitArtifacts.h`**, this iteration: written field-for-field from
`Schemas/hookkit-artifact.schema.json` (re-read in full first, same
schema-is-source-of-truth discipline as the earlier headers), closing the
one real gap left in Milestone 3 before Milestone 4's artifact-ledger work
builds on top of it — an internal ledger producing records of a public
shape that didn't exist yet would have been backwards. Reuses
`hk_mapping_kind_t`/`hk_effects_t` from `HookKitResults.h` rather than
duplicating them. One stated design call: no 3-state
verified/unverified/failed enum at the artifact level despite the schema
modeling one — `hk_hook_result_t.verified` is already a plain bool for the
same concept at the per-hook level, and a 3-state artifact enum a 2-state
hook-level bool can't fully represent would be a new inconsistency, not a
refinement; revisit together if a real need for the FAILED distinction
shows up. The 6 snapshot functions (`hk_report_copy_artifacts`,
`hk_runtime_copy_artifacts`, `hk_copy_process_artifacts`,
`hk_artifact_snapshot_count`/`copy_at`/`release`) are declared only, same
"fix the public shape first" order Milestone 3's other headers were done
in — no internal artifact ledger exists yet to implement them against.

Added to the umbrella (`Headers/HookKit/HookKit.h`); still not wired into
`HookKit_PUBLIC_HEADERS` in the Makefile, so `./build.sh all` output is
unaffected — reconfirmed (64/64 PASS, 4 `.deb`s) after adding it, not
assumed safe from the pattern alone.

Strengthened all 4 header compile tests (`Tests/Host/test_header_compile.{c,m,cpp,mm}`)
with real coverage of the new types rather than relying on the umbrella
include alone: `.c` gets `_Static_assert`s on the 3 new enums' numeric
values plus a sample `hk_artifact_t` construction; `.cpp` builds a real
`hk_artifact_mapping_t`/`hk_vm_protection_t` (the one nested struct
without `HK_STRUCT_HEADER`, previously unexercised); `.m` builds a sample
`hk_image_identity_t` (real `path`/`uuid` fields, no message sends — this
host's `tests/fake_headers/Foundation.h` declares `NSString` with no
methods and nothing to link a real ObjC runtime against, so an earlier
draft of this test tried bridging through a real `NSString` and had to be
rewritten to a plain C string once that constraint became concrete rather
than assumed); `.mm` takes the address of two of the new unimplemented
snapshot functions into correctly-typed function pointers, proving the
declarations themselves are well-formed C++ without needing to link an
implementation. All 4 compile clean under `-Wall -Wextra -Werror`,
host-verified, both standalone and through `make test-header-compile`.
Full `make test` and `./build.sh all` re-run clean after these changes.

## Milestone 4 — Core runtime and fake engines
**State: in progress.**

| Task | State | Evidence |
|---|---|---|
| IDs (`Sources/Core/HKIDs.{h,c}`) | complete | this commit |
| Runtime lifecycle (`Sources/Core/HKRuntime.c`, `HKRuntimeInternal.h`) | in progress (create/shutdown/release/owner_id/drain_pending only — no plan/domain tracking yet) | this commit |
| Plan lifecycle (`Sources/Core/HKPlan.c`) | **complete** — full `DRAFT → ANALYZED → PREPARING/PREPARED → COMMITTING/COMMITTED` path now real (`create`/`release`/`state`/`add_hook`/`analyze`/`prepare`/`commit`) | this commit — see below |
| Domains (`hk_plan_define_domain`, `HKPlanInternal.h`) | complete | commit `0b13100` |
| Router | in progress — `hk_plan_analyze` now consults registered engines and picks the first eligible one (target kind + reach subset only; spec section 9's full ranking has no criteria to rank on yet) | this commit — see below |
| Results / Reports (`hk_plan_analyze`, `Sources/Core/HKReport.c`) | in progress (`hk_plan_analyze` + `hk_hook_copy_result` complete; `hk_plan_prepare`/`commit` not started) | commit `c4adda7` |
| Artifact ledger | in progress — end-to-end for the commit path: engines record artifacts via a sink during `commit_one`, the commit loop stamps contextual IDs, and they reach the report's snapshot (`Sources/Core/HKArtifactLedger.{h,c}`, `commit_one` signature, `hk_report_adopt_artifact_ledger`); runtime/process-level accumulation still absent | this commit — see below |
| Ownership ledger | not started — deferred deliberately (see note below); an exclusivity/refusal gate was tried and reverted | commit `2cc4c3e`, reverted |
| Original slots | in progress — process-lifetime installed record + original slot + installed handle, all 4 public accessors real (`Sources/Core/HKInstalled.{h,c}`); slot survives plan/runtime release (tested); only created for active hooks whose engine publishes an original | this commit — see below |
| Fake engines (`Sources/Core/HKEngineInternal.h`) | in progress — `describe()` + `prepare_one()` + `commit_one(spec, sink)` (all ungrouped; `commit_one` now records artifacts, `fake_rebind` produces a real import-slot artifact); `revalidate_group`/`verify_group`/`compensate_group`/`inspect_continuation` not modeled yet (nothing calls them before compensation/verification exist) | this commit — see below |
| Fault injection | in progress — OOM sweep failing every allocation site (23) across the lifecycle exactly once (`Tests/Host/test_fault_injection.c`, linker `--wrap`); enforces OOM never advances plan state; ASan-clean | this commit — see below |
| Model-based tests | in progress — plan lifecycle state machine cross-checked against an independent reference model over random op sequences with full (state × op) coverage (`Tests/Host/test_plan_model.c`); success path only | commit `8a20209` |

**Ownership ledger — deferred, and why (a reverted misstep worth
recording).** An ownership ledger was built in commit `2cc4c3e` whose
commit-time gate refused a *second* hook on the same target with
`HK_OUTCOME_CONFLICT`. That is wrong: multiple consumers hooking the same
function/method is a first-class, designed-for scenario, not a conflict.
`HK_ORIGINAL_DIRECT_PREDECESSOR` (`PUBLIC_C_ABI.md`) is defined as "an
existing predecessor is acceptable (untouched function body after rebinding,
previous IMP, previous Swift slot)" — you hook something already hooked and
your original *is* the prior consumer's replacement. That is chaining, how
the tweak ecosystem and Shadow-coexisting-with-other-tweaks work; the
original-slot machinery (commit `ae8aaf5`) is its primitive.
`ARCHITECTURE.md` disclaiming "a generic arbitrary C-hook chaining framework"
is a product-scope disclaimer, not a refusal to let two hooks share a target.
The commit was reverted in full (back to `ae8aaf5`) at the user's direction.
When ownership is eventually built it is **chain coordination** (the next
hook's original = the previous replacement), which needs real engines that
can install-on-top — Milestone 6+, not the fake-engine stage.
`HK_OUTCOME_CONFLICT` is reserved for genuine mechanism incompatibility, not
mere same-target hooking.

First real (not header-only) implementation code this rewrite has shipped.
`hk_id_generate()`: one process-instance nonce (time+pid+ASLR entropy,
computed once via `pthread_once` — deliberately not cryptographic, since
`hk_id_t` only ever promised process-lifetime uniqueness, and reaching for
`arc4random` would raise an availability question against the legacy lane's
older deployment floor for no real benefit) plus an atomically-incremented
monotonic counter starting at 1 (0 stays available as a "no ID" sentinel).
`hk_runtime_create/shutdown/release/owner_id/drain_pending`: real, and
honestly minimal for what exists at this slice — shutdown has nothing to
quiesce and drain_pending has nothing to apply because no plan/domain/
engine tracking has been written yet (both said explicitly in code
comments, not left to look like unfinished stubs with no explanation).

Two design calls made and stated, not left implicit: `hk_runtime_create(NULL, ...)`
is defined to mean "every default" (no executor, no diagnostics,
`HK_INSTALL_CONTEXT_EARLY_PROCESS`) — the master spec's text never says
whether `config` is required, so this fills that gap in favor of not
forcing every caller to zero-construct a struct for the common case.
`struct_size` smaller than this build's `sizeof(hk_runtime_config_t)` is
rejected outright (`HK_STATUS_INVALID_ARGUMENT`) rather than tolerated as a
partial read — "unknown trailing fields are ignored" (spec) only ever means
a struct *larger* than expected, never smaller.

9 host tests (`Tests/Host/test_runtime_lifecycle.c`), wired into `make test`
as `test-runtime-lifecycle`. Real verification, not just "did the call
return OK": includes the internal header directly to check the config was
actually deep-copied field-by-field, checks two runtimes' IDs share a
process nonce but have distinct monotonic counters, checks the
`struct_size`-too-small rejection path, checks every NULL-tolerant entry
point actually tolerates NULL. Also run once under
`-fsanitize=address,undefined` (clean, host-verified) given this is real
allocation/atomics/pthread code, not just a header. Hit one real host-only
portability bug along the way: `clock_gettime`/`CLOCK_REALTIME` need
`_POSIX_C_SOURCE` defined before `<time.h>` under strict `-std=c11` on
glibc (Darwin's libc doesn't gate the same way) — fixed with a feature-test
macro and a comment explaining it's a host-toolchain concern, not a
target-platform one.

Not wired into `HookKit_PUBLIC_HEADERS` or any framework `_FILES` variable
— same safety-by-construction as Milestone 3's headers. Verified: `./build.sh all`
still produces the same 64/64 PASS and 4 `.deb` artifacts.

**Plan lifecycle + domains**, this iteration: `hk_plan_create/release/state`
and `hk_plan_define_domain` (`Sources/Core/HKPlan.c`,
`HKPlanInternal.h`), real.

The one design problem worth recording: a `hk_domain_t*` returned by
`hk_plan_define_domain` has to stay valid for the rest of the plan's life,
since callers hold onto it across many later `hk_plan_add_hook` calls
(`hk_hook_spec_t.domain`, spec section 6.24). Storing domains inline in a
growable array — the naive first design — would invalidate every
previously-returned pointer the moment a later domain triggers a `realloc`
that moves the array. Fixed by making `hk_plan_t` hold a growable array of
*pointers* to individually heap-allocated `hk_domain` structs instead: the
pointer array can move freely, the structs it points to never do. Proven,
not just designed-around-on-paper: `test_domain_pointers_stable_across_growth`
adds 37 domains (several times past the initial capacity of 4), then
re-checks all 37 previously-returned pointers *after* every growth event —
each must still report its own correct ID, not another domain's data and
not a freed allocation's garbage.

9 host tests (`test-plan-lifecycle`), covering: the pointer-stability
property above; `stable_domain_id` is actually deep-copied (mutates the
caller's buffer after the call, checks the domain's own copy is
unaffected — same style of real verification as the runtime config test);
duplicate `stable_domain_id` rejected; `hk_plan_define_domain` refused
outside `HK_PLAN_DRAFT` state (spec: "Only `HK_PLAN_DRAFT` accepts new
domains or hooks" — direct quote, not a paraphrase, so the test manufactures
a non-DRAFT plan by reaching into the internal struct rather than guessing
at how state transitions might work before `hk_plan_analyze` exists to
produce one for real); every NULL-tolerant path. Clean under
`-fsanitize=address,undefined` — meaningful here specifically, since
LeakSanitizer would have caught it if `hk_plan_release`'s domain-freeing
loop were wrong.

Real, stated gap: `hk_plan_config_t.debug_label` (a caller-owned string
pointer) is not deep-copied yet — harmless today because no public getter
reads it back, but noted in a code comment so it doesn't quietly become a
dangling-pointer bug the day one is added.

**`hk_plan_add_hook`**, this iteration: the deep-copy of the full target
union the previous entry deferred, done as its own focused pass.
Dispatches on `target_kind` to one of four branches (symbol/address/objc/
memory) — deliberately not `HK_TARGET_SWIFT_VTABLE` (rejected with
`HK_STATUS_UNAVAILABLE`, per `HookKitTargets.h`'s design note that Swift
goes through its own entry point, never this union) — each repointing the
copied target struct's string/bytes fields at the hook's own owned
allocations, same pattern as the domain work.

A real bug caught by *reasoning through the code before running it*, not
by a failing test: the memory-target branch only copies `base_image` when
`address_is_image_relative` is true, but the initial whole-struct copy
(`*dst = *src`) still copies whatever raw, non-owned `base_image.path`
pointer the caller happened to leave set regardless — a dangling-pointer
trap the moment the caller's spec goes out of scope, even though the field
is documented as "meaningful only when `address_is_image_relative`." Fixed
by explicitly zeroing `base_image` in the non-relative branch instead of
trusting a field the contract says not to read. Now has its own regression
test (`test_memory_target_non_relative_base_image_zeroed`) that
deliberately sets a stray `base_image.path` on a non-relative target and
checks it comes back `NULL`.

Real validation added beyond pure transcription, each checked by its own
test: `stable_hook_id` uniqueness within the plan (mirrors the domain
check); the `HK_ORIGINAL_CALLABLE_CONTINUATION` +
`HK_CONTINUATION_FORBIDDEN` contradiction rejected at add time — spec
section 6.7 says this "is rejected as contradictory before route analysis
even runs," and add_hook is the earliest point that can actually do that,
so it does, rather than deferring to the not-yet-written
`hk_plan_analyze`; a `spec->domain` pointer that doesn't belong to *this*
plan is rejected (a foreign or garbage pointer, never silently accepted);
a `commit_after` entry referencing a hook not yet added to this plan is
rejected (`hk_hook_spec_t` is deep-copied once at add time with no update
API, so a forward reference could never resolve to anything real later).

14 host tests (`test-hook-add`), covering all of the above plus: deep-copy
verification for each of the 4 supported target kinds (mutate the caller's
buffer after the call, check the hook's own copy is unaffected — symbol
name, objc class/selector names, memory replacement/expected/mask byte
buffers); `commit_after` array deep-copied the same way; `hk_hook_t*`
pointer stability across growth (37 hooks, same style as the domain
stress test). Clean under `-fsanitize=address,undefined` — this is the
test that matters most for that, given how many `malloc`/`free` paths and
error-unwinding branches this code has. Not tested yet (no fault-injection
harness exists — that's its own later Milestone 4 deliverable): the
out-of-memory paths themselves.

**`hk_plan_analyze` + `hk_report_t`**, this iteration: `Sources/Core/HKReport.c`
(new — matches the file name the spec's own repo layout, section 5, already
uses), `HKReportInternal.h`, and `hk_plan_analyze`/`hk_hook_copy_result` in
`HKPlan.c`.

The honest starting point stated plainly rather than dressed up: no engine
registry exists yet, so there are zero candidates for any hook to route to
— `hk_plan_analyze` reports `HK_OUTCOME_NO_ROUTE` for every hook, for real,
not as a placeholder standing in for smarter logic. "The router ran, found
nothing to route to, and said so truthfully" *is* what side-effect-free
analysis with nothing registered is supposed to look like — Milestone 6+'s
job is giving the router something real to find, not making analysis lie
about the current state to look more finished. `unmet_preferred_reach` on
each result correctly echoes back exactly what that hook's spec asked for
(verified per-hook in the test, not just "some non-zero value").

Judgment call stated in code: a `NO_ROUTE` result's `retryable` is set
`true` — registering an engine later could change the outcome on a fresh
analysis, even though nothing in this plan's state changed. `currently_valid`
is `true` for a different reason: the result accurately reflects reality
*right now*, which is a separate claim from whether that reality might
later change.

`hk_report_t` is a flat array of `hk_hook_result_t` value copies, not
pointers back into live hooks — proven independent in
`test_report_independent_of_plan_after_release` (release the report,
confirm the plan and its hooks are still fully readable afterward).
`hk_report_release` moved from `HKRuntime.c` (where it was a
permanent-looking no-op because nothing produced a report yet) into
`HKReport.c`, where `hk_report_t`'s concrete definition now lives.

Real slip caught by the build, not by careful reading this time: moving
`hk_report_release` broke the *existing* `test-runtime-lifecycle` and
`test-plan-lifecycle`/`test-hook-add` Makefile targets, which call it
directly or transitively (`hk_plan_analyze` is compiled into `HKPlan.c`
unconditionally, so linking `HKPlan.c` at all now requires
`HKReport.c`, whether or not a given test calls `hk_plan_analyze`) —
`make test` failed with undefined-reference linker errors until all three
targets' source lists were updated. Both fixed, full suite re-verified
green afterward rather than assumed fixed from reading the diff.

8 host tests (`test-plan-analyze`), covering all of the above plus:
re-analyzing an already-`ANALYZED` plan rejected (state machine is
linear — DRAFT is required, per the spec's own DRAFT → ANALYZED →
PREPARING → ... shape, section 6.25); a hook's own result correctly
transitions from `HK_OUTCOME_UNANALYZED` (set at `add_hook` time) to
`HK_OUTCOME_NO_ROUTE` after analysis; `out_report == NULL` still updates
every hook's stored result and still transitions plan state, with the
report itself released internally rather than leaked. Clean under
`-fsanitize=address,undefined`.

`hk_hook_original_slot`/`hk_original_slot_load`/`hk_hook_installed_handle`/
`hk_installed_hook_copy_result` remain declared in `HookKitPlan.h` but
unimplemented — they describe state that can't exist before a real commit
happens against a real engine, so there's nothing honest to return yet.
Left undefined rather than given a placeholder body that would compile but
lie; nothing currently links against them, so this doesn't block anything.

**Engine registry + router**, this iteration: `Sources/Core/HKEngineInternal.h`
(new), a fixed-size (16-slot) engine array on `hk_runtime_t`, and
`hk_runtime_register_engine_for_testing`. `hk_plan_analyze` upgrades from
"always `NO_ROUTE`" to actually checking registered engines — the first
piece of real router behavior this rewrite has, and the reason the
previous entry's "zero candidates" state was worth building honestly
first rather than faked, since this is exactly the code path it's now
exercising for real.

Deliberately minimal, stated as such rather than presented as the full
contract: the vtable has only `describe()` — no `prepare_group`/
`commit_group`/`revalidate_group`/`verify_group`/`compensate_group`/
`inspect_continuation` (spec section 8.1), because nothing calls those
until `hk_plan_prepare` exists to call them. Eligibility checks exactly 2
of spec section 9's ~14 criteria: target kind supported, and every
required-reach bit within the engine's achievable reach. Not checked yet,
named explicitly in a code comment rather than silently assumed passing:
image scope exactness, original-requirement/continuation-policy
compatibility, forbidden effects, install context, architecture/OS,
ownership conflicts, engine certification — each needs a concept this
rewrite hasn't built yet (image catalog is Milestone 5; ownership ledger
and certification are later Milestone 4/8/10 work). Multiple eligible
engines aren't ranked, just first-registration-order-wins — with only one
fake engine existing in any test so far, true ranking and
first-wins aren't even distinguishable yet; stated so this isn't mistaken
for the real section 9 algorithm once a second engine makes the
difference visible.

Not public API, and the registration function's own name says so:
`hk_runtime_register_engine_for_testing` is for `Sources/Core`/`Tests/Host`
only — the eventual real engines (Milestone 6+) are a fixed, compiled-in
set, not something arbitrary callers extend. The function will need
renaming the day a production engine actually calls it for real, not
patched around; noted in its own doc comment so that rename isn't a
surprise later.

5 host tests (`test-engine-registry`) with a fake "rebind-style" engine
(function-symbol targets, `HK_REACH_EXISTING_IMPORTS`) and a fake
"objc-style" one (objc-method targets, `HK_REACH_OBJC_DISPATCH`), covering:
the pre-existing zero-engines behavior is unchanged (regression check); an
eligible engine upgrades a hook to `HK_OUTCOME_ANALYZED` with the right
`achieved_reach` and `unmet_preferred_reach` (a preferred bit the engine
can't achieve shows up as unmet, not silently dropped); an engine matching
the target kind but not the required reach correctly stays `NO_ROUTE` (kind
match alone is never sufficient); first-eligible-wins correctly skips past
an engine registered earlier that doesn't match, rather than stopping or
mis-selecting on the mismatch. Clean under `-fsanitize=address,undefined`,
including confirming a `describe()` result's `engine_id` (a static string
literal in every fake engine defined so far) survives being read back out
of the result after the local `hk_engine_capabilities_t` that received it
has gone out of scope — safe by construction since literals aren't
stack-allocated, verified rather than assumed.

**`hk_plan_prepare`**, this iteration: the engine contract grows a real
`prepare_one(spec) -> bool` (ungrouped — real engines will need `prepare_GROUP`
for batching, spec section 8.1, but no fake engine here has anything to
batch), and `hk_hook_t` grows a `matched_engine` field so prepare calls the
*same* engine analyze found eligible rather than re-searching the registry
(which could legitimately return a different answer if engines were
registered/removed between the two calls — a real correctness concern, not
paranoia, given the registry is mutable runtime state).

Extracted `Tests/Host/fake_engines.h` once a second test file
(`test_plan_prepare.c`) needed the same fakes `test_engine_registry.c`
already had, rather than duplicating them again — three fakes now: a
rebind-style engine that always prepares successfully, an
always-fails-to-prepare engine (a second engine, not a mutable toggle flag
on a shared one, to keep tests order-independent), and the objc-style
engine with `prepare_one` deliberately left `NULL`.

That last one exercises a real judgment call: an engine that's eligible
for `describe()` purposes but never implemented `prepare_one` is a genuine
inconsistency, not a "does nothing" case to skip past silently — treated
as a preparation failure (`HK_OUTCOME_FAILED_SAFE`), covered by its own
test. Per-hook outcomes are precise (`HK_OUTCOME_PREPARED` /
`HK_OUTCOME_FAILED_SAFE` only — no `FAILED_PARTIAL`/`FAILED_UNKNOWN` at
this minimal a contract, since there's no partial-preparation concept
yet), and the plan-level rollup (`HK_PLAN_PREPARED`/`PARTIAL`/`FAILED`)
is derived from real attempted/prepared/failed counts, tested for a
genuine mixed outcome — two hooks in the same plan, one forced (via
direct internal-struct access, the same pattern `test_plan_lifecycle.c`
already uses) onto the failing engine so the PARTIAL path is exercised
for real rather than assumed correct from the FAILED and PREPARED cases
alone.

Real, stated gap: no domain-level gating yet (spec section 15.1's "domain
preparation gate" — one failed mandatory member should block its whole
domain). Each hook's own outcome is accurate regardless; only the
cross-hook, domain-aware blocking behavior is missing, because nothing yet
reads `hk_domain_spec_t.require_all_mandatory_prepared` during prepare.

6 host tests (`test-plan-prepare`), all 5 pre-existing suites re-verified
with zero regressions, all clean under `-fsanitize=address,undefined`.

**`hk_plan_commit`**, this iteration: closes the plan lifecycle end to end
for the first time. The engine contract grows `commit_one(spec) ->
hk_mutation_state_t` (ungrouped, same reasoning as `prepare_one`) — a
`hk_mutation_state_t` return, not `bool`, because the whole point of this
function is the engine's honest report of what actually happened
(`NONE`/`COMPLETE`/`PARTIAL`/`UNKNOWN`, spec section 6.27), which a plain
success/failure boolean can't carry.

The mapping this milestone exists to get right —
`COMPLETE → HK_OUTCOME_ACTIVE`, `NONE → FAILED_SAFE`,
`PARTIAL → FAILED_PARTIAL`, `UNKNOWN → FAILED_UNKNOWN` — is one of the
spec's core invariants (section 4.4: "another route may be attempted only
after `HK_MUTATION_NONE`"), so it got 4 dedicated fake engines in
`fake_engines.h` (one per mutation state) rather than trusting the switch
statement from reading it — each outcome is asserted from a real commit
call, not inferred. A fifth fake (`commit_one == NULL`, mirroring
`prepare_one == NULL`'s treatment last commit) confirmed a real judgment
call: an engine this inconsistent — prepared successfully, then can't even
be asked what commit did — is mapped to `HK_MUTATION_UNKNOWN`, not the more
optimistic `NONE`. Trusting "no commit function" to mean "definitely
touched nothing" would be exactly the kind of unverified-success shortcut
spec section 1.6 exists to rule out.

Stated honestly rather than presented as more than it is: no
fallback-after-partial-mutation logic exists, because there is no
fallback mechanism at all yet — each hook has exactly one `matched_engine`,
fixed at analyze time. Invariant #4 is vacuously satisfied (nothing exists
that could violate it), which is a different claim from "multi-engine
retry is correctly implemented." Also stated as a deliberate judgment
call: `hk_plan_commit` accepts a plan in `HK_PLAN_PREPARED` *or*
`HK_PLAN_PARTIAL` — the spec's own state-machine text only describes the
happy path, but a `PARTIAL` plan has hooks that DID prepare successfully
and are worth committing, and refusing outright seemed like it would
under-deliver relative to what the eventual domain-gate language (section
15.1) implies is intended.

8 host tests (`test-plan-commit`), all 6 pre-existing suites re-verified
with zero regressions, all clean under `-fsanitize=address,undefined`. A
plan can now go from an empty `hk_plan_create` all the way to committed
(fake-)hooks and back through every documented outcome and mutation state
the spec defines for this path — real coverage of the mechanism the rest
of Milestone 4 (artifact ledger, ownership ledger, original slots) and
Milestone 6+'s real engines build on top of.

**Domain preparation gate** (spec section 15.1), this iteration: closes
the gap stated in the previous two commits. `hk_domain_mandatory_gate_satisfied`
checks the one sub-condition that's actually checkable today — every
`HK_OPERATION_MANDATORY` hook in a `require_all_mandatory_prepared` domain
must have a route — computed once per domain before `hk_plan_prepare`'s
main loop, not re-scanned per hook. A hook whose domain gate fails is
marked `HK_OUTCOME_FAILED_SAFE` even if it individually would have
prepared successfully, proven by a dedicated test that registers a real
matching engine for the blocked hook and confirms it still doesn't
prepare. Not checked yet, stated rather than assumed satisfied:
ownership-reservation currency and domain dependency cycles — both need
concepts (an ownership ledger, a dependency graph) this rewrite hasn't
built yet.

Real bug caught by the test, not by inspection: the first version counted
a gate-blocked hook toward `failed` but not `attempted`, which broke the
`failed <= attempted` assumption the `PREPARED`/`PARTIAL`/`FAILED` rollup
depends on — a plan whose only hook was correctly gate-blocked came out
`PARTIAL` instead of `FAILED` (0 attempted, 0 prepared, 1 failed doesn't
satisfy either the `PREPARED` or the `FAILED` branch condition, so it fell
through to `PARTIAL` by default). `test_gate_on_blocks_whole_domain`
asserts the plan-level state explicitly, not just each hook's own
outcome, which is what caught it. Fixed by counting gate-blocked hooks as
attempted (the plan did process them; the gate is just a cheaper, earlier
refusal point than calling `prepare_one`), with the reasoning left in a
comment at the fix site.

4 host tests (`test-domain-gate`): no-domain hooks ungated (regression
check), the gate is a no-op when `require_all_mandatory_prepared` is
false, a real optional (non-mandatory) hook with no route does NOT trigger
the gate (only mandatory members count), and the core blocking behavior
itself. All 7 pre-existing suites re-verified with zero regressions, all
clean under `-fsanitize=address,undefined`.

**Artifact ledger — write side + snapshot read path**, this iteration
(`Sources/Core/HKArtifactLedger.{h,c}`): the internal collection engines
will accumulate `hk_artifact_t` records into, plus the immutable
`hk_artifact_snapshot_t` the public read path (`hk_report_copy_artifacts`
and the `hk_artifact_snapshot_count`/`copy_at`/`release` accessors declared
in `HookKitArtifacts.h` last commit) hands back. This is deliberately the
first of two commits: an engine has no way to *report* an artifact yet, so
the ledger is real but every report's ledger is empty. The alternative —
having `hk_plan_commit` synthesize plausible `hk_artifact_t` records from
the mutation state and target kind alone — was explicitly rejected: only
the committing engine knows what it actually mutated (which import slot,
what address, what original bytes), so anything the commit path invented
from the outside would be fabricated detail, exactly what the spec's
section 7 / mission rules forbid. The honest split is ledger-plumbing now,
engine-population (a `commit_one` signature change to accept an artifact
sink) next.

`hk_report_copy_artifacts` is now implemented (was declared-only last
commit); `hk_runtime_copy_artifacts` and `hk_copy_process_artifacts` stay
deliberately undefined — there is no runtime- or process-level artifact
accumulation yet (the runtime still doesn't track committed plans), and a
placeholder body returning an empty snapshot would imply an accumulation
path that doesn't exist. Same "left undefined rather than given a body that
would compile but lie" precedent as `hk_hook_original_slot` et al.

Ownership honesty, stated because it is a real limitation not a finished
property: both `append` and the snapshot copy `hk_artifact_t` **by value**.
That is a full correct copy of every inline/scalar field (the bulk of the
struct), but the borrowed-pointer fields (`image.path`, the
`engine_id`/`mechanism_id` string views, the `*_bytes` inline views) are
**shared, not deep-copied** — identical to the existing `hk_report_t`
result-copy precedent, which likewise leaves its view fields as borrowed
views. Safe today because nothing with a non-static lifetime flows through
(no producer exists; the direct unit test uses static literals). The
precise trigger for building owned-copy machinery: the first real engine
(Milestone 6+) that records an artifact carrying a dynamically-allocated
string or byte buffer. Deferred until there is a producer to need it, not
built speculatively now. The snapshot *is* independently allocated at the
array level, so ledger growth or destruction after a snapshot cannot touch
it — that independence (spec section 7.5) is real and tested.

7 host tests (`test-artifact-ledger`, all clean under
`-fsanitize=address,undefined`): empty-ledger snapshot + out-of-range
`copy_at`; append-and-snapshot with order/field integrity; snapshot
independence (grow *and destroy* the ledger after snapshotting, old
snapshot unchanged and still readable); geometric growth to 50 records
forcing several reallocs with per-record markers to catch corruption;
`copy_at` out of range leaving the caller's buffer byte-for-byte untouched;
NULL tolerance across every entry point; and the report-level read path
returning a valid empty snapshot plus its two argument-error cases
(including the out-param being cleared on the error path). The link-set
fix — `HKReport.c` now pulls in `HKArtifactLedger.c`, so the 7 other
targets linking `HKReport.c` each gained it too — was caught by a real
`make test` linker failure (`test-runtime-lifecycle` undefined reference),
the same failure mode the earlier `hk_report_release` move hit; fixed and
the full suite re-verified green. `./build.sh all` unaffected as expected
(no `Sources/Core/*.c` is in `HookKit_FILES`) — reconfirmed, not assumed.

**Artifact ledger — engines actually populate it now**, this iteration
(Commit 2 of the two the previous section set up): the write path is wired
end to end, so a committed hook's engine records real `hk_artifact_t`
records that reach the report's snapshot. The central design decision — who
fills which fields — is settled by what each party can actually know. The
engine's `commit_one` only ever receives an `hk_hook_spec_t`; it has no
access to the plan, the hook, or the runtime, so it *cannot* know the
operation's contextual identity (`artifact_id`, `plan_id`, `request_id`,
`runtime_owner_id`). Those are stamped centrally by `hk_artifact_sink_record`
from context the commit loop holds; the engine fills only mechanism facts
(kind, effects, `engine_id`, addresses, bytes, reversibility). This is
exactly why a sink exists rather than handing the engine the raw ledger: an
engine must not, and structurally cannot, forge those IDs. `commit_one`
gained a `hk_artifact_sink_t *sink` parameter (internal vtable only —
`hk_runtime_register_engine_for_testing`, so the blast radius is
`Sources/Core/*.c` + the shared fakes, nothing public); its old
mutation-state return is unchanged.

Wiring: `hk_plan_commit` builds a ledger before its commit loop, sets the
sink's fixed `plan_id`/`runtime_owner_id` once and its `request_id` per hook
(each artifact's originating request is *its own* hook, not a shared value),
then hands the populated ledger to the report via a new
`hk_report_adopt_artifact_ledger` that swaps out the empty ledger the report
was born with. The swap approach was chosen over changing `hk_report_create`'s
signature specifically to leave the analyze/prepare call sites and last
commit's `test-artifact-ledger` untouched — a smaller diff that keeps the
"report always has a ledger" invariant true at every point. OOM paths kept
honest: if the report can't be built after the loop, the commit path
destroys the ledger it owns rather than leaking it.

Fake engines: `fake_rebind` now produces a real import-slot artifact
(`HK_ARTIFACT_IMPORT_SLOT` / `HK_EFFECT_IMPORT_MUTATION`, reversible, a
fake nonzero slot address, `engine_id` a borrowed view of a string
literal), leaving the four contextual IDs zeroed precisely because the sink
owns them. The other three commit fakes (`none`/`partial`/`unknown`) record
nothing and say so in a comment — they exist for the mutation-state→outcome
mapping test, not the ledger; `NONE` genuinely made nothing to record, and
modeling a partial/unknown artifact is not their job (the pipe is already
proven end to end by `fake_rebind`).

3 new host tests in `test-plan-commit` (11 total now, all clean under
`-fsanitize=address,undefined`): the end-to-end proof — commit a rebind
hook, then read the report's artifact snapshot and check both the engine's
mechanism facts survived *and* all four contextual IDs were stamped
correctly (`plan_id` == the plan's, `request_id` == the hook's,
`runtime_owner_id` == the runtime's, `artifact_id` a real generated
nonzero); a refused (`NONE`) commit records **zero** artifacts (no phantom
record for an operation that touched nothing); and two hooks through one
engine each stamp *their own* `request_id` and get *distinct* `artifact_id`s
— an adversarial check that would fail if `request_id` were set once outside
the commit loop or `artifact_id` weren't freshly generated per record. Full
`make test` and `./build.sh all` (all 4 lanes) verified clean; the ledger's
own 7 tests re-verified green with the sink's `hk_artifact_sink_record`
added underneath them.

Still absent, stated: `hk_runtime_copy_artifacts` / `hk_copy_process_artifacts`
remain undefined — there is still no runtime- or process-level
accumulation, only per-report. Aggregating a runtime's committed plans'
artifacts is its own task, gated on the runtime tracking committed plans at
all (it doesn't yet).

**Original slots + installed handles**, this iteration
(`Sources/Core/HKInstalled.{h,c}`): the four public accessors that were
declared-but-undefined since Milestone 3 (`hk_hook_original_slot`,
`hk_original_slot_load`, `hk_hook_installed_handle`,
`hk_installed_hook_copy_result`) are now real. The requirement that shaped
the whole design is the survival rule from `PUBLIC_C_ABI.md`: "Active
installation data and original slots that live replacements still use must
survive runtime wrapper release." A replacement keeps calling through its
original slot long after the `hk_plan_t`/`hk_runtime_t`/`hk_hook_t` wrappers
are gone, so the installed record **cannot** be owned by any of them. It is
allocated with process lifetime and retained in a process-global registry,
intentionally never freed in production. `hk_installed_reset_for_testing`
frees the registry so host tests stay leak-clean under ASan; production has
no such call. This is the honest way to model an intentional
process-lifetime retention without leaving ASan unable to distinguish it
from a real leak — the leak is real, it is just deliberate, and the test-only
reset draws that line explicitly.

Wiring: `hk_plan_commit`, when a hook goes ACTIVE and its engine published
an original (a new `published_original` output field on the sink, reset per
hook so hook N never inherits hook N-1's — the same per-hook-reset
discipline the artifact `request_id` already follows), generates an
`installed_id`, stamps it plus `original_available` onto the hook's result
*before* snapshotting that result into the record, and links the record onto
the hook. OOM retaining the record is handled honestly: the mutation still
happened, but the optimistically-set `installed_id`/`original_available` are
rolled back because we cannot advertise a slot we failed to allocate.

Deliberate narrow scope, stated: an installed record is created **only** for
an active hook whose engine published an original — an active hook with no
original does not yet get an installed handle. This kept the blast radius to
zero for the existing `test-plan-commit` (its `fake_rebind` publishes no
original, so it creates no records and leaks nothing, needing no edits). A
new `fake_rebind_original` engine (separate, so it does not perturb other
tests) publishes a fixed nonzero sentinel original and also records it on its
artifact's `original_pointer` for inspectability. The original slot is
atomic (`_Atomic(void *)`) from the start because a future re-hook can
republish under a concurrent reader on the replacement's hot path, even
though no update path exists yet. Linking an artifact to its installed
record (the artifact's `installed_id` is still zero — artifacts are recorded
inside `commit_one`, before the record exists) is left as a stated follow-up.

5 host tests (`test-installed-original`, all clean under
`-fsanitize=address,undefined`): an active hook gets a handle + slot loading
the published pointer, with `installed_id`/`original_available` wired onto
the result and the handle's stored snapshot matching; the **survival** test
— capture the slot, release plan then runtime, and the slot still loads the
right pointer; an engine that publishes no original gives no slot/handle and
`original_available` stays false; a non-active (NO_ROUTE) hook gets neither;
and NULL tolerance across all four accessors plus a direct record with a NULL
out-param. Every HKPlan.c-linking Makefile target gained `HKInstalled.c`
(the commit path calls into it); `make test` and `./build.sh all` (all 4
lanes) verified clean.

**Model-based tests — plan lifecycle state machine**, this iteration
(`Tests/Host/test_plan_model.c`): an independent reference model
(`model_apply`, written from the documented lifecycle — "only DRAFT accepts
new domains/hooks", DRAFT→ANALYZED→PREPARED→COMMITTED — deliberately NOT
copied from `HKPlan.c`) predicts, for every (state, operation), whether the
op is accepted and the resulting state. Random operation sequences (a
reproducible LCG, eight fixed seeds × 40 steps) are applied to both the model
and a real plan, asserting at every step that accept/reject agrees, that a
rejection is specifically `HK_STATUS_INVALID_STATE` (unique ids + valid specs
mean state is the *only* possible rejection reason, so an `INVALID_ARGUMENT`
sneaking in would be caught), and that the plan's state equals the model's. A
coverage assertion then proves all 24 (4 states × 6 ops) cells were actually
exercised — a model-based test that only visited a corner would otherwise
pass vacuously. Found no divergence: the state machine matches the reference
for the success path.

Scope stated honestly: all hooks succeed (fake_rebind, no failures — and
ownership conflicts don't exist, that gate having been reverted), so the four
reachable states are DRAFT/ANALYZED/PREPARED/COMMITTED. The FAILED/PARTIAL
rollups and their transitions are already covered by `test-plan-prepare` /
`test-plan-commit` and not re-modeled here; `INVALIDATED`/`DISCARDED` are enum
values no operation produces yet (a real gap in the lifecycle, noted — those
transitions can't be modeled until something creates them). `make test` and
`./build.sh all` (all 4 lanes) verified clean.

**Fault injection — OOM sweep**, this iteration
(`Tests/Host/test_fault_injection.c`): every core allocation
(malloc/calloc/realloc) is routed through a linker `--wrap` interceptor that
fails exactly the Nth allocation. The sweep runs the full lifecycle (runtime +
plan + 2 hooks + analyze + prepare + commit) once per N = 1, 2, 3, … until a
run completes with no failure fired — meaning every allocation site was the
failure point exactly once (**23 sites** on this build). Beyond "doesn't
crash / doesn't leak" (built under `-fsanitize=address` for the leak half),
the load-bearing invariant is: **an `HK_STATUS_OUT_OF_MEMORY` return must
leave the plan's state unchanged** — which catches the classic bug of
advancing the state machine and then failing a late allocation. Result: no
crashes, no leaks, and the invariant held at all 23 sites — the core's
hand-written OOM paths (partial-cleanup in `add_hook`, the report/ledger
create failures, the `hk_report_create` OOM that frees a pre-built ledger)
are all clean. A genuine finding worth recording, not a bug but a boundary:
`fake_rebind` ignores a failed artifact-ledger append (`(void)`
`hk_artifact_sink_record`), so an OOM in that one append is *swallowed* and
commit still succeeds with the artifact silently dropped — consistent with
the artifact-ledger header's own stated "a real engine must instead degrade
to `HK_MUTATION_UNKNOWN`" future-work note; the sweep surfaces exactly where
that unimplemented degradation will need to live. `make test` and
`./build.sh all` (all 4 lanes) verified clean.

**Milestone 4 status.** The core runtime + plan lifecycle + fake-engine
contract are real and now well-covered: IDs, runtime/plan lifecycle, router,
results/reports, artifact ledger (through the commit path), original slots +
installed handles, model-based lifecycle tests, and this OOM sweep. Not done
and honestly out of scope for the fake-engine stage: the ownership ledger
(reverted — it must be chain-coordination with real engines, not a refusal
gate; see the note under the milestone table), and the grouped engine
operations (`prepare_group`/`commit_group`/`compensate`/`verify`) that only
matter once real engines and compensation exist (Milestone 6+). Next frontier
is Milestone 5 (image catalog and resolvers), which several deferred pieces
(address-precise ownership keys, private-symbol scans) depend on.

## Milestone 5 — Image catalog and resolvers
**State: in progress.**

| Task | State | Evidence |
|---|---|---|
| Image catalog structure + selector matching (`Sources/Core/HKImageCatalog.{h,c}`) | complete (host-verified) | this commit — see below |
| dyld population (`hk_image_catalog_populate_from_dyld`) | **not started — device-only** | declared as a seam; needs a jailbroken device to build/verify (real dyld) |
| Symbol table (nlist) search (`Sources/Resolvers/HKSymbolTable.{h,c}`) | complete (host-verified) | commit `5c3efe7` |
| Mach-O container parsing (`Sources/Resolvers/HKMachO.{h,c}`) | complete (host-verified) — header validation, bounded load-command iteration, LC_SYMTAB → symbol table view, `LC_SEGMENT_64` segments and sections | commit `0a8f09f` + this commit |
| Loaded-image (`__LINKEDIT`) offset translation | complete (host-verified) — `hk_macho_symtab_view_for_loaded_image`; lifts the file-image-only limitation | this commit — see below |
| Section flags / code-vs-data check | complete (host-verified) — `hk_macho_section_flags` + `hk_macho_section_is_code`, bounded (2.x's is not) | this commit — see below |
| Export trie resolver (`Sources/Resolvers/HKExportTrie.{h,c}`) | complete (host-verified) — ULEB128 decoding + trie walking, plus `hk_macho_find_export_trie` locating it in either load-command form | this commit — see below |
| Resolver selection (`Sources/Resolvers/HKSymbolResolve.{h,c}`) | complete (host-verified) — unified name normalization + visibility-driven source preference, with file-image and loaded-image source collection | this commit — see below |
| Chained fixups (`Sources/Resolvers/HKChainedFixups.{h,c}`) | complete (host-verified) — metadata *and* traversal: header, imports table, symbol pool, and chain walking to bind sites | commits `25347f0` + this commit |
| Import slot resolution (`Sources/Resolvers/HKImportSlots.{h,c}`) | complete (host-verified) — maps symbol-pointer slots to bound symbols via LC_DYSYMTAB indirect symbols, file and loaded layouts | this commit — see below |
| Private-symbol resolver | in progress — its search mechanism (symbol table) is done; still needs a device-side image reader to supply the table view | this commit |
| Shared-cache resolver | not started | device-only (shared cache layout) |
| ObjC / Swift resolvers | not started | |

**Image catalog — structure + selector matching**, this iteration
(`Sources/Core/HKImageCatalog.{h,c}`). The deliberate host/device split is
the whole shape of the file: the **selector-matching logic is
platform-agnostic and host-tested**; the **only device-only part is
populating the catalog from the live process** (dyld image list, a real
Mach-O runtime), which is declared as the `hk_image_catalog_populate_from_dyld`
seam but **not defined** — writing a dyld body that can't be compiled or run
on this Linux host (it isn't in any built target yet, and `#if __APPLE__`
would exclude it here) would be shipping unverifiable code, which this
project doesn't do. Tests populate synthetically via `hk_image_catalog_add_entry`.

Matching handles all six `hk_image_selector_kind_t` cases. It's written as
"does this entry satisfy the selector?" applied to each entry in order,
rather than "find the entries for this kind" — which gives natural dedup
(each entry considered once) and makes `EXPLICIT_SET` fall out as "matches
any sub-selector," recursively. Two correctness points worth stating: an
`EXACT_UUID` selector only matches an entry whose `uuid_present` is true (an
absent uuid is *not* an all-zero uuid — a zero-uuid selector must not match
an image that simply has no uuid), and an `EXACT_HEADER` selector with a
NULL header matches nothing (never a wildcard). The catalog also carries a
monotonic `generation` counter (bumped on every add) — the concept
`ARCHITECTURE.md` invariant #3 revalidates against; nothing revalidates yet
(that's Milestone 12's late-image work), but resolvers can start stamping it.

10 host tests (`test-image-catalog`, clean under
`-fsanitize=address,undefined`): each selector kind, `EXPLICIT_SET` union of
disjoint sub-selectors *and* dedup of overlapping ones, early-stop via the
visitor, generation bumping, path deep-copy stability (mutate the source
buffer after add, catalog copy intact), and NULL tolerance across every
entry point. `make test` and `./build.sh all` (all 4 lanes) verified clean.

**Symbol table (nlist) search**, this iteration
(`Sources/Resolvers/HKSymbolTable.{h,c}`) — the search mechanism behind the
private-symbol resolver.

*Reuse survey first, as the mission required.* `native/hk_symbols.c` already
walks a Mach-O symbol table in `hk_native_find_symbol`, but the entire file is
`#if defined(__arm64__)` and its walk is fused with `mmap`/`dlsym`/PAC signing/
slide application, with a single hardcoded name rule (`strcmp`, or skip one
leading underscore). None of it can compile or run on this host, and none of
it implements HK3.0's convention/visibility model. So the new file is not a
duplicate but the *pure core* the 2.x version never separated out: a function
over a **caller-supplied table view** (nlist array + string table), returning
the symbol's **unslid** `n_value` plus its `n_sect`. Slide application and
PAC signing stay with the caller — `n_sect` is returned precisely so the
device side can look up section flags to decide code-vs-data, exactly as 2.x
already does. That separation is the only reason any of this is host-testable.
The 2.x path is untouched and still serves the 2.x runtime during migration.

Real correctness content, not a transcription: **STAB rejection** — any entry
with an `N_STAB` bit is debug information whose fields do not carry symbol
meaning, and two STAB types (`N_BNSYM` 0x2e, `N_ENSYM` 0x4e) satisfy
`(n_type & N_TYPE) == N_SECT` *by coincidence*, so the STAB check must come
first and stand on its own (the test asserts that coincidence explicitly).
**Visibility** — `EXPORTED_ONLY` requires the `N_EXT` bit; `ANY` and
`PRIVATE_ALLOWED` deliberately behave identically for now and the header says
so rather than faking a difference that only becomes observable once an
export-trie resolver exists to be preferred instead. **Conventions** —
`MACHO_EXACT` compares byte-for-byte, while C / C++-mangled / Swift-mangled
also accept the table's leading-underscore form (all three acquire Mach-O's
underscore), honoring the ABI's "pass either form". **Bounds safety** — every
string read is bounded by the caller-declared `strings_size`, so a truncated
or unterminated string table cannot cause a read past the buffer.

`hk_macho_nlist64_t` is declared locally rather than included from
`<mach-o/nlist.h>` so this builds on a non-Apple host; the test
`_Static_assert`s its size (16) and every field offset, so divergence from
Apple's `struct nlist_64` cannot pass silently.

12 host tests (`test-symbol-table`, clean under
`-fsanitize=address,undefined`). The bounds-safety test was **verified to have
teeth**, not assumed: swapping the bounded comparison for a naive `strcmp`
makes ASan report a `heap-buffer-overflow` (READ of size 6) on the exactly
sized, deliberately unterminated table, while the real implementation passes —
so that test genuinely catches the bug it claims to. Also covered: both
boundary cases (unterminated vs. NUL-as-final-byte), bare/underscored lookup,
`MACHO_EXACT` refusing to normalize, mangled conventions normalizing,
`EXPORTED_ONLY` rejecting a local symbol, undefined/absolute/indirect/
zero-address entries rejected, deterministic first-match-in-table-order
(including when an earlier entry is skipped as invalid), out-of-range and zero
`n_strx`, and NULL/empty tolerance.

**Not implemented, deliberately not guessed at**: `hk_symbol_alias_policy_t`
(`ALIAS_FOLLOW` needs a second pass and a defined preference order among
same-address names) and `interior_address_permitted` — a caller's alias policy
is simply not consulted yet, stated in the header rather than approximated.

**Device-only work this does NOT cover** (and cannot, honestly, from this
host): the dyld catalog populator, the shared-cache resolver, and PAC signing
of results. Those need SSH to the jailbroken device to build and verify;
nothing here is device-verified. *(Corrected in the next entry: locating
`LC_SYMTAB` was listed here as device-only, but parsing a Mach-O is pure
buffer arithmetic — only obtaining the bytes is device-bound.)*

`make test` and `./build.sh all` (all 4 lanes) verified clean.

**Mach-O container parsing**, this iteration
(`Sources/Resolvers/HKMachO.{h,c}`) — closes the path from raw bytes to a
resolved symbol: image → `LC_SYMTAB` → symbol table view → symbol search.

*A correction to the previous entry, worth recording as a scoping error:* it
listed "finding `LC_SYMTAB` in a real mapped image" as device-only. Parsing a
Mach-O is pure arithmetic over bytes; only *obtaining* the bytes (mmap, dyld)
is device-bound. So more of Milestone 5 is honestly host-testable than that
entry projected, and this commit is the proof.

*Reuse survey first, again.* 2.x walks load commands **twice**: once bounded
against a mmap'd file size (`bind_ondisk_symbols`) and once *unbounded* over a
live dyld-validated header (`collect_section_flags`, which only guards a
degenerate `cmdsize` because dyld has already validated the list). Both sit
inside `#if defined(__arm64__)`, both use Apple's headers, neither can run
here. This is the single bounded walk, host-testable, with the structure
layouts and magic/command constants declared locally so it builds off-Apple.
The 2.x code is untouched.

Two robustness properties, both **verified to have teeth** rather than
asserted:
1. **Bounded reads and no infinite loop.** Every read is bounded by the
   caller-declared size, using subtraction (`end - offset`) so no addition can
   overflow. `cmdsize < 8` is rejected, so the cursor always advances — a
   `cmdsize` of 0 cannot park the walk forever (the test asserts the rejection,
   and its own termination is the rest of the proof). 64-bit Mach-O also
   requires 8-byte-aligned `cmdsize`, which is checked.
2. **Alignment safety.** Every multi-byte read goes through `memcpy`, so a
   buffer at *any* alignment parses correctly. Swapping those for a
   struct-pointer cast makes UBSan report `load of misaligned address ...
   which requires 4 byte alignment` on the deliberately odd-addressed test
   image, while the real implementation passes — so that test genuinely
   catches the bug it claims to.

One real correctness point found while writing it: `hk_macho_find_symtab_view`
hands back a **typed** `nlist` pointer that callers dereference, so unlike the
parser's own memcpy reads it genuinely requires 8-byte alignment
(`hk_macho_nlist64_t` contains a `uint64_t`). Any linker-produced image
satisfies that; a corrupt or hostile one might not, and a misaligned load is
undefined behaviour. So the alignment is validated and the view refused rather
than returned as an unsound pointer — tested from both directions (a
misaligned image, and an aligned image with a 4-aligned `symoff`).

15 host tests (`test-macho`, clean under `-fsanitize=address,undefined`).
The payoff is `end-to-end-symtab-to-symbol-search`: build an image, locate
`LC_SYMTAB`, feed the resulting view straight into the previous commit's
symbol search and resolve a real symbol — neither half mocked. Also covered:
magic dispatch (fat, 32-bit, byte-swapped, and garbage each get their own
distinct status rather than a generic failure), truncated buffers at several
sizes, `sizeofcmds` overrunning the buffer, in-order iteration, visitor early
stop, all three degenerate-`cmdsize` forms, a command overrunning the region,
`ncmds` claiming more than `sizeofcmds` holds, `LC_SYMTAB` range validation
(including an `nsyms` large enough to overflow a 32-bit span multiply — the
span is computed in 64 bits for exactly that reason), a truncated
`LC_SYMTAB`, and NULL tolerance.

**Stated limitation, not an oversight:** this is the **file-image layout**
only. `LC_SYMTAB`'s `symoff`/`stroff` are file offsets, correct for an image
laid out as on disk (what 2.x's `bind_ondisk_symbols` mmaps). A *loaded* image
is scattered at segment VM addresses, where those offsets must be translated
through the `__LINKEDIT` segment — that needs `LC_SEGMENT_64` parsing and is
deliberately absent rather than guessed at. *(Lifted in the next entry.)*
Byte-swapped and 32-bit images are likewise rejected with distinct statuses
rather than half-supported.

`make test` and `./build.sh all` (all 4 lanes) verified clean.

**`LC_SEGMENT_64`: segments, sections, and loaded-image translation**, this
iteration — lifts the file-image-only limitation stated directly above, and
adds the code-vs-data check callers need.

*Reuse survey first.* 2.x's `collect_section_flags` is the section-flags
precedent: two passes over the load commands (count sections, then collect
flags into a flat array indexed by `n_sect - 1`), reading each segment's
section array **without bounds checking** — sound only because it runs on a
live, dyld-validated image, as its own comment says. It is also inside
`#if defined(__arm64__)` and uses Apple's headers. The HK3.0 version keeps the
same `n_sect` numbering (1-based, across segments in load-command order) but
is fully bounded and needs no allocation: `hk_macho_section_flags` walks to
the requested index directly.

**The bounds check 2.x omits is load-bearing, and that was proven, not
asserted.** A segment's section array lives inside the segment command, so it
must fit within that command's own `cmdsize`. With a segment claiming
`nsects = 99` inside a 232-byte command, asking for section 50 reads roughly
4 KB into a 1 KB image. On identical input a guard-less build produces an ASan
`heap-buffer-overflow` (`READ of size 4`), while the real implementation
returns `HK_MACHO_MALFORMED` — so the guard prevents a genuine out-of-bounds
read, not a hypothetical one.

`segname` is a fixed 16-byte field that need not be NUL-terminated, so it is
copied out and terminated rather than ever read as a C string in place —
tested with an exactly-16-character name (which must match) and a
17-character query (which must not).

**Loaded-image translation** (`hk_macho_symtab_view_for_loaded_image`): a
loaded image is scattered at segment VM addresses, so `LC_SYMTAB`'s file
offsets are relative to `__LINKEDIT`, whose mapped base is
`slide + linkedit.vmaddr - linkedit.fileoff`. Both referenced ranges are
validated to lie inside `__LINKEDIT`'s declared file range, so a corrupt
`LC_SYMTAB` cannot yield a pointer into unrelated memory, and the
`vmaddr - fileoff` subtraction is guarded against underflow rather than
allowed to wrap.

The test for it uses a **negative control** rather than just asserting
success: a decoy symbol table is placed exactly where file-offset logic would
read, holding `n_value 0xDEAD`, while the real table sits where translation
lands, holding `0x4000`. The test asserts the translated path resolves to
`0x4000` *and* that the file-image path on the very same bytes resolves to
`0xDEAD` — so the two paths are demonstrably different rather than
incidentally equal, and a missing or wrong translation would return a visibly
wrong answer instead of quietly passing. The synthetic image is deliberately
non-degenerate (vmaddr delta `0x140` ≠ file offset `0x40`), since a
contiguous layout would make translation a no-op and the test vacuous.

22 host tests total in `test-macho` (8 new), clean under
`-fsanitize=address,undefined`. New coverage: segment lookup by name, the
16-byte `segname` handling, section flags with the code/data distinction,
`NO_SECT` and past-the-end indices, the section-array bounds guard, a
truncated segment command, the loaded-image translation with its negative
control, and `__LINKEDIT` range validation (symoff before the range, nlist
span past the end, string table past the end, and no `__LINKEDIT` at all
reported as missing rather than malformed).

**Still device-only, unchanged**: the dyld catalog populator, the shared-cache
resolver, and PAC signing. Nothing here is device-verified.

`make test` and `./build.sh all` (all 4 lanes) verified clean.

**Export trie resolver**, this iteration
(`Sources/Resolvers/HKExportTrie.{h,c}`) — the proper path for *exported*
symbols. dyld stores exports in a ULEB128-encoded prefix tree, not in the
symbol table; `HKSymbolTable` remains the private-symbol path. Added
`hk_macho_find_export_trie` alongside it, which handles both load-command
forms (modern `LC_DYLD_EXPORTS_TRIE`, and older images' `export_off`/
`export_size` inside `LC_DYLD_INFO(_ONLY)`) so callers never have to know
which an image uses.

*Reuse survey:* nothing in this repo decodes ULEB128 or walks an export trie.
fishhook rebinds GOT slots, litehook does its own scanning, and
`native/hk_symbols.c` reads only the symbol table and the shared cache's own
index — it matched a "trie" grep solely on the word "en**trie**s". So this is
genuinely new code rather than a second copy.

Three safety properties, **two of them proven to have teeth**:
1. **ULEB128 correctness.** Bounds-checked, and rejects both overlong
   encodings (>10 bytes) and values overflowing 64 bits — at shift 63 only one
   bit still fits, so a wider final chunk is refused. Tested against the
   canonical LEB128 example and `UINT64_MAX`'s exact 10-byte encoding.
2. **Bounded edge strings.** An edge string with no terminator before the end
   of the trie is refused. Removing that bound makes ASan report a
   `heap-buffer-overflow` on an exact-length heap buffer, while the real code
   returns `MALFORMED`.
3. **Cycle cap** — the one worth dwelling on. A trie edge with a *zero-length*
   string consumes no name characters, so if it points back at an ancestor the
   walk never terminates. Name length does not bound the walk; only the depth
   cap does. Verified by removing it: the walk **hangs indefinitely** (killed
   at a 5-second timeout) rather than crashing. This is a failure mode **no
   sanitizer detects** — ASan and UBSan both run happily forever — so the cap
   plus its test is the only defense, and "the tests pass under sanitizers"
   would have been a false assurance here.

A re-export is reported as `HK_EXPORT_UNSUPPORTED_KIND` rather than given an
invented address: it names another dylib instead of carrying one, so resolving
it means following into a different image through the image catalog. The
out-parameter is still filled (flags, library ordinal) so a caller can see
what was found, with `address` left zero.

15 host tests (`test-export-trie`, clean under `-fsanitize=address,undefined`).
The main trie is laid out and traced **by hand**, byte by byte, rather than
produced by a builder, so the bytes under test are auditable. The truncation
test exercises every prefix length and asserts the strong invariant — a
truncated trie either fails cleanly or returns the *right* answer, never a
wrong one — and pins the exact count of lengths that legitimately still
resolve (24..29, since resolving `_alpha` reads to offset 23 and never touches
`_beta`'s subtree), so it cannot silently degrade into "everything failed",
which would pass vacuously.

**Stated gap** (closed by the next entry): the walker is exact-name-only. The
trie stores linker-form names, so `malloc` must be queried as `_malloc`. The
leading-underscore normalization `HKSymbolTable` does internally is
deliberately not duplicated here — it belongs in the resolver-selection layer
above both. The asymmetry is recorded rather than papered over.

**Visibility semantics revisited**, now that an export resolver exists:
`HKSymbolTable.h` previously noted that `HK_SYMBOL_VISIBILITY_ANY` and
`PRIVATE_ALLOWED` behave identically "until an export-trie resolver exists".
That resolver now exists, and the conclusion is unchanged but for a better
reason, so the comment was updated rather than left stale: the difference
between those visibilities is about *which source to consult and in what
order*, which is a resolver-selection decision belonging to the layer above
both. Within a symbol-table search on its own there is genuinely nothing for
them to differ about — the symbol table is a private-symbol source by nature.

`make test` and `./build.sh all` (all 4 lanes) verified clean.

**Resolver selection**, this iteration
(`Sources/Resolvers/HKSymbolResolve.{h,c}`) — the layer two previous commits
deferred to *by name*. Built rather than deferred again. It owns exactly the
two decisions neither mechanism can make alone.

**1. Name normalization, now in one place.** Mach-O stores linker-form names,
so C `malloc` appears as `_malloc`. The trie walker is exact-name-only by
design; `HKSymbolTable` had an equivalent rule built in. Rather than keep two
implementations of one rule, this layer expands a query into an ordered
candidate list — the name as given, then the underscore-prefixed form — and
queries **both** sources with `HK_SYMBOL_NAME_MACHO_EXACT`, which switches off
`HKSymbolTable`'s internal normalization. The result is provably identical to
what `HKSymbolTable` did alone (an entry matches if it equals the query, or
equals `"_" + query`), but the rule now exists once. `HKSymbolTable` keeps its
internal handling so it remains correct standalone.

A subtlety worth stating: the prefixed candidate is tried **even when the
query already starts with an underscore**, because a C symbol literally named
`_hidden` appears as `__hidden` and only the prefixed form finds it. Tested.
Candidate order is exact-first, also tested against a table holding both
`malloc` and `_malloc` so the ordering is observable rather than assumed.

**2. Source preference — what finally makes `hk_symbol_visibility_t` mean
something.** Two commits ago the ledger recorded that `ANY` and
`PRIVATE_ALLOWED` behaved identically because a symbol-table search alone has
nothing to differ about. With two sources they genuinely differ:

- `EXPORTED_ONLY` — the export trie is dyld's authority on what is exported,
  so when a trie exists it is the **only** source consulted: absence from it
  means not exported, and falling back to the symbol table would contradict
  that. Images with no trie fall back to the symbol table filtered to `N_EXT`,
  the best answer available.
- `ANY` — trie first (authoritative, and exports are the common case), then
  the symbol table, which also covers private symbols.
- `PRIVATE_ALLOWED` — symbol table first, since that is the only place private
  symbols exist; then the trie, so an exported symbol stripped from the symbol
  table is still found. "Private allowed" permits private symbols, it does not
  require them.

The two mechanisms report addresses against **different bases** — a trie gives
an offset from the mach header, an nlist gives an unslid VM address — so the
sources struct carries both anchors and the resolver returns a single
comparable runtime address. For a file image, setting the slide to
`buffer - __TEXT.vmaddr` makes both agree; the constructor does that.

14 host tests (`test-symbol-resolve`, clean under
`-fsanitize=address,undefined`). The centerpiece places the **same name in
both sources with different addresses**, so the resolved address reveals which
source answered — without that, a test could not distinguish a real preference
order from two sources that happen to agree. All three visibilities are
checked against it, plus: `EXPORTED_ONLY` refusing to fall back when a trie
exists but lacks the symbol; `EXPORTED_ONLY` falling back without a trie *and*
still rejecting a local symbol; `PRIVATE_ALLOWED` reaching the trie for an
export stripped from the symbol table; re-exports reported with no invented
address; and both source constructors end to end, the loaded-image one over a
deliberately non-degenerate `__LINKEDIT` layout.

**Verified to have teeth:** removing the name-length check turns the prefixed
candidate copy into an ASan `stack-buffer-overflow` — a **WRITE of size 1031**,
the more dangerous direction — while the real code returns
`HK_RESOLVE_NAME_TOO_LONG`. The exact boundary (a name of precisely
`HK_RESOLVE_MAX_NAME`, which still fits `'_' + name + NUL`) is asserted to be
*accepted*, so an off-by-one in either direction fails the suite.

Also refactored while here: the `__LINKEDIT` base computation and range check
now live in shared helpers used by both the symbol-table and export-trie
loaded-image paths, rather than being repeated per table. Both pre-existing
suites re-verified green after the change.

**Deferred with a reason, not avoided (still open):** resolving a
`hk_symbol_target_t` against the *image catalog* (matching images by selector, then resolving in
each) needs a per-entry "how many bytes are safely readable from this header"
on `hk_image_entry_t`. That value is naturally produced by the dyld populator
— a device-only piece — so adding the field is better done together with it
than invented now.

**Import slot resolution**, this iteration
(`Sources/Resolvers/HKImportSlots.{h,c}`) — answers "which pointer slot in
this image binds to symbol X", the question a rebind engine (Milestone 6) must
answer before it can redirect anything. A symbol-pointer section (`__got`,
`__auth_got`, `__la_symbol_ptr`) holds one pointer per import; its `reserved1`
indexes LC_DYSYMTAB's *indirect symbol table*, where entry `reserved1 + i`
gives the symbol table index slot `i` binds to. Both file and loaded layouts
are supported, the latter translating the indirect table through `__LINKEDIT`
exactly as the symbol table already did.

*Reuse survey.* `vendor/fishhook/fishhook.c` performs this very walk in
`perform_rebinding_with_section` and is the reference for the mechanism, but
it is not reusable, for two reasons. Its walk is fused with the parts that
only exist on device — VM protection changes, arm64e pointer re-signing for
`__auth_got`, its own rebinding list. More importantly, it performs **no
bounds validation at all**: `indirect_symtab + section->reserved1`,
`symtab[symtab_index]`, and `strtab + strtab_offset` are each dereferenced
unchecked. That is perfectly sound in fishhook's context, because dyld has
already validated the image — but a parser handed arbitrary bytes cannot
assume it. All three are checked here, and each has its own test.

**The first of those checks was verified to have teeth, on a real
out-of-bounds read.** With the indirect table in its own exactly-sized
allocation and a section claiming `reserved1 = 3` for a 2-entry table, a
guard-less build produces an ASan `heap-buffer-overflow` (`READ of size 4`)
while the real code returns `HK_IMPORT_MALFORMED`. (The first attempt at this
proof was inconclusive because the table sat inside a larger image buffer, so
the overrun stayed in bounds — the guard only demonstrably matters once the
table is its own object, which is how it will be on device.)

Slots whose indirect entry is `INDIRECT_SYMBOL_LOCAL`/`ABS` name no symbol and
are skipped: they are not imports. `hk_import_slots_find` uses the **same**
linker-form candidate expansion as `hk_resolve_symbol`, now exported from
`HKSymbolResolve.h` as `hk_symbol_build_candidates` rather than reimplemented —
so the normalization rule established last commit still exists in exactly one
place, now with two users.

11 host tests (`test-import-slots`, clean under
`-fsanitize=address,undefined`), including the loaded-image case with a
**decoy** indirect table placed where raw file-offset logic would read, naming
a different symbol — so a missing translation returns a visibly wrong answer
rather than merely failing. Two layout bugs in my own fixture were caught this
way while writing it (the string table overlapping `LC_DYSYMTAB`, and two
nlist entries overlapping), which is the point of building fixtures that fail
loudly.

Also generalized in `HKMachO`: section walking is now a real iterator
(`hk_macho_iterate_sections`) exposing full `section_64` information, with
`hk_macho_section_flags` reimplemented on top of it, plus
`hk_macho_find_indirect_symbols` for LC_DYSYMTAB. Sections past index 255 stop
the walk rather than being reported under an aliased `n_sect`, since an nlist's
`n_sect` is a `uint8_t` and could not name them.

**Chained fixups — metadata half**, this iteration
(`Sources/Resolvers/HKChainedFixups.{h,c}`). This matters more than its size
suggests: `LC_DYLD_CHAINED_FIXUPS` is the **modern iOS 15+ import mechanism**,
and the `LC_DYSYMTAB` indirect-symbol path added last commit does not cover it
at all. On a current image that path simply finds nothing, so without this,
import resolution would silently miss rather than fail loudly.

**Deliberately split, and this is the split, not a deferral.** This commit is
the metadata side — the payload header, the imports table, and the symbol
string pool, answering "what does this image import, by name and ordinal".
The traversal side — walking page starts and fixup chains to find *which slot*
holds each bind — is the next piece: it needs per-pointer-format decoding for
five formats and cycle guards of its own, and keeping them apart means each is
actually tested rather than plausibly written.

*Reuse survey, with a genuinely different outcome this time.*
`vendor/litehook/fixup-chains.h` vendors **Apple's own definitions** and
includes only `<stdint.h>`, so unlike every other Apple header in this tree it
builds on this host. It is used as an authoritative **cross-check**: the test
includes it and `_Static_assert`s this parser's constants, structure sizes and
field offsets against Apple's enums and structs. Better still, the test builds
import entries *through Apple's bitfield structs* and decodes them with the
parser's shifts and masks, so the two must agree on the actual bit layout.
The parser deliberately does **not** use those bitfield structs directly:
bitfield layout is an ABI detail, and a parser should not depend on the host
compiler agreeing with the target's. The cross-check is what keeps the
transcription honest without taking on that dependency.

One correctness detail worth naming, because a naive parse gets it wrong:
`lib_ordinal` is documented as "-15 .. 240", meaning the top of the unsigned
range encodes the special `BIND_SPECIAL_DYLIB_*` ordinals as **negatives**. It
is sign-extended here (from 8 bits for the two 32-bit formats, 16 for
`ADDEND64`), and asserted — reading it unsigned would report the flat-lookup
ordinal as 254 rather than -2.

Unsupported inputs get **distinct** statuses rather than a generic failure, so
a caller can tell "I cannot read this" from "this is broken": a newer
`fixups_version` (refused rather than misparsed as version 0), a
zlib-compressed symbol pool (would need a decompressor), and an unknown
`imports_format`. That last one only fails when `imports_count > 0` — an
unknown format with nothing to decode is harmless and parses, which the test
pins.

10 host tests (`test-chained-fixups`, clean under
`-fsanitize=address,undefined`), plus the compile-time cross-checks. **Verified
to have teeth:** removing the symbol-pool bound and termination check turns an
unterminated pool at the end of an exactly-sized allocation into an ASan
`heap-buffer-overflow` (`READ of size 21`), while the real code returns
`HK_CHAINED_MALFORMED`. `hk_chained_imports_find` reuses
`hk_symbol_build_candidates`, so the normalization rule now has three users and
still exactly one implementation.

Note the cycle-safety proof this milestone's guidance calls for belongs to the
**traversal** half, not here: nothing in the metadata parser loops over
attacker-controlled links. That guard and its timeout-based proof come with the
chain walking — delivered in the next entry.

**Chained fixups — traversal half**, this iteration, completing what the
metadata commit split out and promised. Walks `starts_in_image` →
`starts_in_segment` → `page_start[]` → the fixup chains, reporting every
**bind** site as (slot offset, import ordinal) and skipping rebases, which
carry no symbol. Joined to the imports table, this answers for the modern
mechanism exactly what `HKImportSlots` answers for `LC_DYSYMTAB`.

Five arm64 formats are decoded (`ARM64E`, `ARM64E_USERLAND`,
`ARM64E_USERLAND24`, `PTR_64`, `PTR_64_OFFSET`); every other `pointer_format`
gets `HK_CHAINED_UNSUPPORTED_FORMAT` **before any walking**, since decoding
with the wrong bit layout would silently produce nonsense rather than fail.
The two families genuinely differ — arm64e puts `bind` at bit 62 with an 11-bit
`next` and stride 8, `PTR_64` puts `bind` at bit 63 with a 12-bit `next` and
stride 4 — and a test encodes through Apple's own bitfield structs for both, so
the parser decodes bits it did not lay out.

**A real bug in this implementation was caught by its own test.** The
`START_MULTI` overflow list is indexed into `page_start[]` itself, and those
entries live *beyond* `page_count` — which is precisely why
`dyld_chained_starts_in_segment` carries its own `size` field separate from
what `page_count` implies. My first version bounded the overflow list by
`page_count * 2`, which rejects every multi-start page. Fixed to bound by the
declared `size` (clamped to the blob, since `size` is not to be trusted past
it) while still using `page_count` for the page loop.

**Termination is structural here, not merely guarded — stated because it
differs from the export trie.** `next` counts stride units and `next == 0` ends
the chain, so any nonzero `next` advances by at least one stride; the offset
strictly increases and is bounded, so this cannot cycle the way a trie with a
zero-length edge can. The one way it *could* spin is a zero stride, which is
why unknown formats are rejected up front and why an explicit progress check
remains as defense in depth.

**Both guards verified to have teeth**, by the two different methods each
failure mode requires:
- *Bounds*: removing the segment/image checks turns a `next` of 0x7FF into an
  ASan `heap-buffer-overflow` (`READ of size 8`) on an exactly-sized image,
  while the real code returns `HK_CHAINED_MALFORMED`.
- *Progress*: injecting a stride-0 bug and removing the progress check makes
  the walk **hang** (killed at a 5-second timeout); with the check it returns
  `MALFORMED`. As with the export trie cap, **no sanitizer detects this** —
  ASan and UBSan run happily forever — so a timeout is the only proof
  available, and "passes under sanitizers" would have been false assurance.

19 host tests in `test-chained-fixups` (9 new), clean under
`-fsanitize=address,undefined`, plus compile-time cross-checks of the
`starts_in_segment` field offsets and every pointer-format and sentinel
constant against Apple's vendored definitions.

**Stated limitation:** traversal uses the **loaded** layout — a segment's
`segment_offset` is an offset from the image base. A file-on-disk image would
need its VM offsets translated to file offsets first; that is not done rather
than guessed at.

### Milestone 5 device conformance — FIRST DEVICE-VERIFIED RESULT

Everything above this line was verified against synthetic fixtures only. This
section is the first work in the HK3.0 rewrite verified against binaries
nobody here constructed, and against an independent tool.

**Device**: iPhone 7 (`iPhone9,3`, T8010), iOS 15.8.3 build 19H386, Darwin
21.6.0, reached over SSH. **arm64, not arm64e** — which bounds what this run
can claim (see the gaps below). The device was touched **read-only**: four
binaries were copied off it, nothing was installed, modified, or removed.

**Harness**: `Tools/conformance/macho_conformance.c` runs every Milestone 5
resolver over a real image and prints what it found. It is a tool, not a test —
the specimens are third-party binaries and are deliberately not committed.

**Specimens** (real, from the device): `liblz4.1.dylib` and `libz-ng.2.dylib`
(Procursus dylibs), `uicache` (a PIE executable), and HookKit's own installed
framework (fat: arm64 + arm64e).

**Result: every parser handled every specimen.** Concretely, for
`liblz4.1.dylib`: header, `__TEXT`/`__LINKEDIT`, 7 sections, `LC_SYMTAB`
(104 syms), a 1744-byte export trie, 15 indirect symbols yielding 8 import
slots, and a 176-byte chained-fixups payload with 8 imports and 8 bind sites.
For `uicache`: 29 load commands, 15 sections, 62 chained imports, 117 binds.

**Independent ground truth** (theos' Mach-O `nm`/`otool`, not my code):

| Claim | Mine | `nm`/`otool` |
|---|---|---|
| `_LZ4_compress_default` address | `0xaa28` (via export trie) | `0000000000aa28 T` |
| `liblz4` nsyms | 104 | 104 |
| `uicache` nsyms | 64 | 64 |
| `liblz4` sections | 7 | 7 |

**Cross-parser corroboration**, which is arguably stronger than either check
alone: the LC_DYSYMTAB path and the chained-fixups path are entirely separate
parsers reading entirely different structures, and they agree — on `liblz4`
both report 8 imports with `___chkstk_darwin` first; on HookKit's own arm64
slice both report `___objc_personality_v0` first. Neither was written with the
other's output in mind.

**Confirmed by real data, not assumed:** shipped binaries are **fat**, and the
library correctly refuses them with `HK_MACHO_FAT_UNSUPPORTED` while the tool
selects a slice. That division was a design choice made against fixtures; a
real framework confirms it is the right one.

**Gaps this run does NOT close, stated because the device bounds them:**
- **arm64e is unverified.** An iPhone 7 is arm64. The `ARM64E`,
  `ARM64E_USERLAND` and `ARM64E_USERLAND24` chained-pointer formats were
  therefore never exercised by real data, nor was PAC signing. Those need an
  A12-or-later device.
- ~~**Loaded-layout traversal is unverified.**~~ **Closed** — see the
  follow-up below.
- The dyld populator and shared-cache resolver remain unbuilt and untested;
  shared-cache dylibs are not files on disk, so they cannot be pulled this way
  at all.

**Loaded-layout conformance follow-up.** The gap above is now closed, and the
way it was closed is worth recording because the obvious version of this test
would have proved nothing.

`Tools/conformance/macho_conformance.c` gained a `[loaded]` mode that places
each segment at its `vmaddr` the way dyld would, then runs the *loaded* code
paths (`symtab_view_for_loaded_image`, `export_trie_for_loaded_image`,
`import_tables_from_loaded_image`, chained-bind traversal) over the result.
This needed a new `hk_macho_iterate_segments` in the library — name-by-name
lookup cannot compute an image's VM span — added with its own host test.

**The measurement that mattered:** before trusting the comparison, the tool now
reports how many segments actually land at a *different* offset in the mapped
layout than in the file. The answer for two of three specimens is **zero**:

| specimen | segments diverging |
|---|---|
| `liblz4.1.dylib` | 0 of 3 |
| `uicache` | 0 of 4 |
| HookKit's own framework | **1 of 4** (`__LINKEDIT`: vm `0x24000` vs file `0x20000`) |

So for `liblz4` and `uicache` the loaded run is the file run under another
name and demonstrates nothing — had the tool not measured this, the conformance
claim would have been hollow while looking thorough. Only HookKit's own
framework genuinely exercises the translation, and it is the ideal specimen for
it: the divergence is in `__LINKEDIT`, which is exactly where the symbol table,
export trie, indirect symbols and chained fixups all live, so a single 0x4000
shift exercises **every** `__LINKEDIT` translation at once.

On that specimen the two layouts agree exactly — 125 nsyms, 2144 strsize, an
80-byte export trie, 106 import slots (`___objc_personality_v0` first), and 213
chained binds — reached by completely different arithmetic. That is the
loaded-image translation verified against real data.

Still not the same as a live process: this is a faithful reconstruction of
dyld's mapping, not dyld's. It does not exercise page permissions, the shared
cache, or a real slide.

### Milestone 5 stock-take — what is host-testable vs device-gated

Taken deliberately, because the loop is approaching a real boundary and it is
better to name it than to drift into writing unverifiable code.

**Still genuinely host-testable** (pure buffer logic, synthetic fixtures):
chained-fixup **chain traversal** (the metadata half is now done — see above);
bind opcodes (`LC_DYLD_INFO`'s bind streams, the older one); ObjC metadata
sections (`__objc_classlist` and friends parse as plain data, though the
practical hooking path is the runtime API, not static parsing); and Swift
metadata — note `native/hk_swift.c` already parses class descriptors and is
already host-tested by `tests/test_swift_abi.c` against a hand-built metadata
blob, so that is a reuse candidate rather than new work.

**Genuinely device-gated** — needs SSH to the jailbroken test device, and
nothing here may be claimed without a real run: the dyld catalog populator
(`_dyld_image_count`/`_dyld_get_image_header`); the shared-cache resolver (the
cache's own layout and symbol index); PAC signing of resolved addresses;
actually *writing* a slot (VM protection, `__auth_got` re-signing); and
conformance against **real** system images, which is the only thing that can
confirm these parsers handle what Apple actually ships rather than what my
fixtures assume.

That last item deserves emphasis: every parser in Milestone 5 is verified
against synthetic images I constructed. That is genuine verification of the
logic, and it has caught real bugs — but it is *not* evidence that a real
`libsystem_kernel.dylib` parses correctly. Nothing in this milestone is
device-verified.

## Milestone 6 — Non-generated-code engines (ObjC, rebind, memory, Swift)
**State: in progress.**

| Task | State | Evidence |
|---|---|---|
| Rebind engine (`Sources/Engines/HKRebindEngine.{h,c}`) | in progress (host-verified) — prepare/commit over both import mechanisms, mutation-state-honest; write behind a device seam | this commit — see below |
| ObjC engine | not started | |
| Memory engine | not started | |
| Swift engine | not started | native/hk_swift.c exists (2.x), a reuse candidate |

**Rebind engine**, this iteration (`Sources/Engines/HKRebindEngine.{h,c}`) —
the first engine, and the point where every Milestone 5 resolver becomes an
actual hook. It rewrites an image's import slots so calls to an imported symbol
land on a replacement. Reach is `HK_REACH_EXISTING_IMPORTS`: it redirects
cross-image calls, which all go through a slot, and nothing else.

**Two phases, and the split is required by the invariants, not chosen for
style.** `prepare` enumerates the slots and reads their current values while
mutating nothing (invariant #2), which is also what makes the original known
before any replacement is reachable (invariant #5) — capturing originals
during the write loop would publish a replacement before its predecessor was
known. `commit` revalidates each slot against what prepare saw (invariant #3),
then writes. It consults **both** import mechanisms — LC_DYSYMTAB indirect
symbols and chained fixups — so a caller never has to know which era the image
comes from; a subtlety the code names explicitly is that the two report slot
addresses in *different coordinate systems* (unslid vmaddr + slide vs. offset
from the image base), and conflating them would put every write at the wrong
address.

**Mutation state reported honestly, which is the whole reason this is an
engine and not a loop.** A write that fails before any slot is touched is
`NONE` — a clean refusal the router may retry elsewhere. A write that fails
*after* a slot is already rewritten is `PARTIAL`, never a clean failure:
invariant #4 forbids a fallback over a half-modified image, and reporting
`NONE` there would invite exactly that. Revalidation enforces the same
distinction — a slot changed since prepare (plausibly by another hooking
consumer, since co-targeting is designed-for) is refused rather than silently
overwritten, as `NONE` if nothing was touched yet or `PARTIAL` if the image is
already mixed.

**Reuse survey:** 2.x rebinds imports via `vendor/fishhook`
(`Backends/HKFishhookBackend.m`). Reference for the mechanism, not reusable:
fused with the device-only parts, carries its own rebinding registry instead
of reporting artifacts, and predates the mutation-state and
original-publication contracts this engine exists to satisfy.

**The write is the only device-only part, and it is behind a seam**
(`hk_rebind_write_fn`). On device it must change VM protection, store, restore,
and on arm64e re-sign the pointer for an `__auth_got` slot — none of which can
run or be verified here. The host test supplies a seam that writes into a
buffer, exercising every decision the engine makes except the store itself.
**Not device-verified**, and the arm64e re-signing in particular has no host
analogue.

6 host tests (`test-rebind-engine`, clean under
`-fsanitize=address,undefined`): prepare finds both sites for a symbol bound by
two slots and leaves the image byte-for-byte unchanged; commit writes every
site and records one artifact each (with both pointers and reversibility);
failure before any write is `NONE`; failure after a write is `PARTIAL`;
revalidation refuses a slot changed since prepare (as `NONE` or `PARTIAL`
depending on how far the write got); and absent-symbol / argument handling.
**Verified to have teeth:** dropping the `+slide` in the slot-address
computation makes the test fault on a bad address rather than pass — the
address assertions are load-bearing.

Note: HookKit 2.x already has working, host-and-device-tested implementations of all four (`Backends/`, `native/hk_swift.c`) — this milestone is about conforming them to the new engine contract and ABI, not writing them from scratch.

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
