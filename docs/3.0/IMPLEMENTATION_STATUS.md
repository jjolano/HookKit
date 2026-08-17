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
| Artifact ledger | in progress — write side + immutable-snapshot read path complete (`Sources/Core/HKArtifactLedger.{h,c}`, report owns an empty ledger, `hk_report_copy_artifacts` + snapshot accessors real); engine population is the next commit | this commit — see below |
| Ownership ledger | not started | |
| Original slots | not started | |
| Fake engines (`Sources/Core/HKEngineInternal.h`) | in progress — `describe()` + `prepare_one()` + `commit_one()` (all ungrouped); `revalidate_group`/`verify_group`/`compensate_group`/`inspect_continuation` not modeled yet (nothing calls them before compensation/verification exist) | this commit — see below |
| Fault injection | not started | |
| Model-based tests | not started | |

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
