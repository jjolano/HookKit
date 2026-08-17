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
| `HookKitArtifacts.h` | not started | needs real struct design; Milestone 1 deferred this to the JSON schema, which isn't the same as a C struct |
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

## Milestone 4 — Core runtime and fake engines
**State: in progress.**

| Task | State | Evidence |
|---|---|---|
| IDs (`Sources/Core/HKIDs.{h,c}`) | complete | this commit |
| Runtime lifecycle (`Sources/Core/HKRuntime.c`, `HKRuntimeInternal.h`) | in progress (create/shutdown/release/owner_id/drain_pending only — no plan/domain tracking yet) | this commit |
| Plan lifecycle (`Sources/Core/HKPlan.c`) | in progress (create/release/state complete; add_hook/analyze/prepare/commit not started) | this commit |
| Domains (`hk_plan_define_domain`, `HKPlanInternal.h`) | complete | this commit — see below |
| Router | not started | |
| Results / Reports | not started | `hk_report_t` still has no concrete definition — every `out_report` is `NULL` today |
| Artifact ledger | not started | |
| Ownership ledger | not started | |
| Original slots | not started | |
| Fake engines | not started | |
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
dangling-pointer bug the day one is added. `hk_plan_add_hook` and
everything past `DRAFT` (`analyze`/`prepare`/`commit`) are not implemented
— hooks need a real deep-copy of `hk_hook_spec_t`'s full target union
(symbol/address/objc/memory, each with their own nested strings and image
selectors), which is real additional design work deserving its own pass,
not something to rush alongside the domain pointer-stability problem in
the same commit.

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
