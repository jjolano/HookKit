# Measured Runtime Footprint

## Scope and Result

2026-09-07 host audit, baseline `7bee7f0fd46ec9d885d42abc1d3863456134af1b`.
Baseline measurements preceded runtime edits. Prior canonical ownership keys
and packed artifact payloads were already present and were not changed.

The change is resource lifetime and mutation safety, not routing speed:

- An abandoned native preparation now returns its sealed, unactivated backing
  through the existing free seam when prepared state is released.
- A stale entry uses the existing refused-write cleanup path immediately,
  clearing its continuation description if backing was reclaimed.
- Once an entry write may have published a branch (COMPLETE, PARTIAL or UNKNOWN),
  backing survives release, including protection-restore failure, failed
  verification, artifact-recording failure, or other later uncertainty.
- Releasing unmutated prepared state centrally invalidates the live result's
  continuation metadata. Mandatory-domain rollback reports no readable mapping
  and `currently_valid=false`, rather than copying released preparation facts.
- No reclaim callback means no reclamation. No published continuation is
  uninstalled or recycled. Provider discovery, provider ownership, public
  headers/exports, routing and allocation policy are unchanged. Internal native
  writer callbacks now return `hk_mutation_state_t`, not `bool`.

## Audit

| Area | Inspected Behavior | Decision |
| --- | --- | --- |
| Runtime registry and backend selection (`HKRuntime.c`) | Fixed engine/context arrays; stack-only backend enumeration/deduplication and stable ordering. Descriptors are returned by value. | No descriptor cache or registry allocation change. |
| Plan routing (`HKPlan.c`) | Each hook evaluates discovery, operation analysis, reach, original requirements, constraints, install context and platform. Preferred reach then route rank win; registration order breaks ties. Prepared state retains the selected engine/context. | Preserve ordering and fresh discovery, including future calls after a provider becomes available. |
| Provider discovery (`HKRuntime.c`, `HKProviderVtable.c`) | Dobby is linked; Gum/ElleKit file preflights already have process caches. Substitute checks loaded function pointers and live `dlsym` before its file cache. Preparation activates providers separately. | No additional positive or negative cache. Linux fake-provider timings do not measure dyld, signature checks, vendor initialization, or vendor allocation. |
| Native terminal, memory, ObjC adapters | One prepared heap object per operation, freed on failure/release. Core instruction/byte/pointer capture is embedded; no owned executable continuation in these adapters. | No speculative pooling. |
| Rebind adapter/engine | Single-image plan or catalog-sized flexible bundle, with embedded per-image site arrays. Bundle release frees one allocation. Existing bounded caches: 4 file views, 8 symbol entries, 256 cache-patch entries; eviction releases owned paths/symbols/file mappings. | Potential bundle over-reservation needs a representative importer workload before redesign. Existing caches and slot revalidation remain unchanged. |
| Swift surface | Separate prepare/commit API, not the plan engine registry. One prepared metadata/slot object; release frees it, not executable code. | Allocation unchanged. Explicitly compare the pointer-write result to COMPLETE; any non-NONE attempt consumes the prepared slot plan so it cannot be retried. |
| Provider adapter | One prepared object with embedded optional relocation plan. Native provider continuations are vendor-owned. Hybrid release already frees only before `provider_called`; provider failure after invocation is UNKNOWN and retains backing. | Preserve this boundary; do not apply native `activated` state to the provider-owned write path. |
| Relocating/static adapters | One prepared object plus a page or pool slot; release previously freed only the object even when commit never ran. | Fix the demonstrated unactivated backing retention. |
| Artifact ledger/report/snapshot | Stack-built engine artifacts are deep-copied into one packed payload per artifact; item arrays grow geometrically. Reports, runtime/process ledgers and snapshots own independent copies. Installed records and process artifacts intentionally outlive plans. | Preserve independent snapshots, accounting and process lifetime. No shared-payload reference counting or redundant packing work. |
| Core plan allocations | Stable hook/domain objects, deep-copied request fields and canonical target keys; phase result arrays/reports; grouped-operation arrays; existing linear common-case commit ordering with general dependency ordering fallback. | No new index, cache, or ABI/layout refactor. |

## Publication Proof

All repository callers of `hk_reloc_prepare`,
`hk_reloc_prepare_continuation`, and `hk_reloc_commit` were traced:

1. `HKRelocInlineVtable.c` is the only production caller of native
   `hk_reloc_prepare`/`hk_reloc_commit`. Both relocating and static vtables use
   the same prepare/commit/release callbacks. Their grouped callbacks are NULL.
2. Prepare writes/seals backing, but does not modify the target. Core
   `hk_hook_inspect_prepared_continuation` copies descriptive metadata, not an
   installed original slot. `HKEngineInternal.h` explicitly describes this as
   inspection of what an operation *will publish*. A prepare result/snapshot
   does not transfer ownership of executable backing. Existing provider-hybrid
   release and native refused-write cleanup already follow this distinction.
3. Native commit revalidates bytes, then invokes a mutation-valued write seam.
   The original claim that a Boolean false proved no write was **incorrect**:
   `hk_write_locked` could memcpy the branch and then fail to restore protection.
   Both memory and pointer writers now report that outcome as UNKNOWN. Failed
   overwrite-remap also reports UNKNOWN, not a promise the target survived.
   Only explicit NONE permits reclamation. Every other result sets `activated`
   before artifact recording and before core verification. Subsequent native
   commit returns UNKNOWN without touching/reclaiming backing, even if someone
   restored the entry bytes. Partial/uncertain target artifacts are recorded as
   PARTIALLY_APPLIED and are not advertised as safe to reverse.
4. The adapter only supplies `sink->published_original` after COMPLETE. Core
   normal commit and pending-drain dispatch use this same adapter; installed
   original slots are created during commit processing, not preparation.
   Later core UNKNOWN outcomes cannot clear `activated`.
5. `hk_hook_release_prepared` balances successful preparation on destruction,
   retry/re-preparation, inspection/effect rejection and mandatory-domain
   rollback. It clears the live result's prepared continuation as well as the
   private cache when mutation is NONE. Domain rollback copies those cleared
   facts, with `currently_valid=false`, into its new report. Previously returned
   immutable reports remain historical snapshots, not backing-ownership handles.
   Core stale-catalog/dependency/ownership refusals do not invoke the
   native writer, and their backing is reclaimed at plan release. The regression
   covers a stale catalog separately from an engine-detected stale entry.
6. `HKProviderVtable.c` alone calls `hk_reloc_prepare_continuation`. It performs
   the vendor write itself and retains its existing `provider_called` guard.
   It does not call `hk_reloc_commit` or native adapter release.
7. Remaining direct callers are host engine/static tests and
   `tests/macos/test_live_reloc_inline.m`. Direct callers own the returned
   struct/backing; only the adapter owns prepared-object destruction. The
   macOS live test commits and intentionally keeps its live continuation.
8. Production `hk_platform_reloc_free` returns a static slot to the locked
   bitmap or calls `hk_native_reloc_free` for an anonymous page. Neither path
   changes a different continuation's page protections. Pool reuse later goes
   through the existing claim/unprotect/build/seal sequence.

The new private `hk_reloc_plan_t.activated` member is not public ABI. The host
provider benchmark's requested-byte totals were unchanged in the initial
footprint comparison, including its embedded relocation plan. The subsequent
writer migration changes only internal function/callback contracts. There is no
claim of an on-device binary-size saving.

The native byte writer feeds relocating/static inline, terminal inline and memory
adapters through `HKRuntime.c`. The native pointer writer feeds rebind and Swift.
All four plan-engine seams propagate PARTIAL/UNKNOWN instead of converting them
to NONE. Rebind stops at the first uncertain slot, preserves UNKNOWN even after
earlier completed slots, and records the potentially changed slot. Its
`out_written` counts only COMPLETE callbacks. Swift's existing public status API
still reports write failure without offering a mutation-state/retry guarantee;
the original pointer remains published and the consumed preparation cannot retry.
The pointer writer now reads restoration protection under the same write lock,
and both native writers invalidate the thread-local protection memo afterward.

## Host Measurements

**Historical initial-footprint measurements:** the tables below predate the
review-driven mutation-safety/domain-result corrections and later independent
provider-test additions. They are not measurements of the final safety patch.

Environment: Linux x86-64, Clang 22.1.8, existing target flags
`-std=c11 -O2 -Wall -Wextra -Werror`, no sanitizers for timings.

```sh
python3 tests/host/bench_footprint.py --trials 5
```

The runner derives compiler commands from existing host Makefile targets; it
does not edit the Makefile. Each fresh process repeats an entire existing test
suite 1/10/100/1000 times. These are **suite repetitions, not hooks per plan**.
No warmup is subtracted. Timings include assertions, fixture work and printing
to `/dev/null`, but exclude compilation, process startup and exit cleanup.
The same four test sources were used in that initial before/after comparison.
Current provider/native suites have additional regressions; rerunning them does
not produce an apples-to-apples timing delta against these historical fixtures.

Linker wrappers count `malloc`, `calloc`, `realloc`, `aligned_alloc` requests
and non-NULL explicit `free` calls from linked objects. Requested bytes include
the full size of each realloc request, not just growth. Shared-library internal
allocations are not wrapped. Call counts and requested bytes are **not live
bytes, peak heap, mapping size, leaks, RSS, or physical footprint**. In particular,
realloc and process-lifetime records prevent subtracting free calls from
allocation calls to infer leaks. The process artifact ledger grows across suite
repetitions, as it did before this change.

Five-process median elapsed microseconds, with min/max shown:

| Suite | Repetitions | Before Median [Min, Max] | After Median [Min, Max] |
| --- | ---: | ---: | ---: |
| plan-analyze | 1 | 28.429 [25.491, 34.501] | 28.660 [22.240, 37.421] |
| plan-analyze | 10 | 51.531 [44.121, 64.691] | 49.980 [44.829, 61.110] |
| plan-analyze | 100 | 260.862 [249.842, 293.533] | 231.512 [230.901, 245.041] |
| plan-analyze | 1000 | 2290.353 [2174.553, 3035.909] | 2254.022 [2141.762, 2456.544] |
| plan-prepare | 1 | 32.291 [27.961, 51.030] | 34.760 [27.520, 39.641] |
| plan-prepare | 10 | 82.980 [63.620, 96.020] | 84.000 [66.091, 99.749] |
| plan-prepare | 100 | 431.762 [362.332, 595.273] | 502.042 [367.102, 579.644] |
| plan-prepare | 1000 | 3586.992 [3358.049, 4727.088] | 4763.877 [3480.770, 6308.927] |
| plan-commit | 1 | 90.231 [84.360, 106.511] | 90.191 [70.269, 153.632] |
| plan-commit | 10 | 379.603 [347.682, 566.593] | 368.553 [346.202, 508.433] |
| plan-commit | 100 | 2970.728 [2777.006, 3109.957] | 3562.111 [2958.437, 4007.053] |
| plan-commit | 1000 | 28910.473 [28034.738, 29877.909] | 29450.600 [28394.624, 34771.122] |
| provider-vtable | 1 | 70.430 [60.641, 95.519] | 72.911 [67.790, 84.331] |
| provider-vtable | 10 | 278.853 [256.651, 331.862] | 269.913 [256.131, 368.622] |
| provider-vtable | 100 | 1948.592 [1838.891, 2127.133] | 2391.002 [1764.550, 2693.476] |
| provider-vtable | 1000 | 17485.275 [16361.118, 19943.500] | 18392.346 [17262.300, 21130.243] |

Allocation metrics were identical before/after and across all five trials:

| Suite | Repetitions | Allocation Calls | Requested Bytes | Free Calls |
| --- | ---: | ---: | ---: | ---: |
| plan-analyze | 1 | 81 | 18533 | 81 |
| plan-analyze | 10 | 810 | 185330 | 810 |
| plan-analyze | 100 | 8100 | 1853300 | 8100 |
| plan-analyze | 1000 | 81000 | 18533000 | 81000 |
| plan-prepare | 1 | 123 | 28690 | 123 |
| plan-prepare | 10 | 1230 | 286900 | 1230 |
| plan-prepare | 100 | 12300 | 2869000 | 12300 |
| plan-prepare | 1000 | 123000 | 28690000 | 123000 |
| plan-commit | 1 | 698 | 201441 | 680 |
| plan-commit | 10 | 6948 | 2156466 | 6800 |
| plan-commit | 100 | 69411 | 20947932 | 68000 |
| plan-commit | 1000 | 694014 | 204389760 | 680000 |
| provider-vtable | 1 | 355 | 138371 | 333 |
| provider-vtable | 10 | 3508 | 1326086 | 3330 |
| provider-vtable | 100 | 35011 | 12644132 | 33300 |
| provider-vtable | 1000 | 350015 | 141798992 | 333000 |

No speedup is established. Some after medians are slower, particularly prepare;
this shared-host suite harness is not a controlled CPU benchmark. The four
workloads do not execute the changed native commit/release callbacks (provider
hybrids use their own release). This is a baseline/control, not attribution of
timing changes to the resource fix.

An additional native-wired baseline was recorded before editing that suite:

| Repetitions | Median Us [Min, Max] | Allocation Calls | Requested Bytes | Free Calls |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 52.600 [44.161, 69.972] | 288 | 76068 | 275 |
| 10 | 183.522 [179.971, 212.940] | 2857 | 842832 | 2750 |
| 100 | 1367.948 [1217.396, 1512.039] | 28510 | 8131080 | 27500 |
| 1000 | 12048.282 [11448.498, 12657.806] | 285013 | 78777144 | 275000 |

`python3 tests/host/bench_footprint.py test-reloc-inline-wired` runs the current
suite. Its new lifetime regression makes that workload different, so do not
compare its new timing/allocation totals to the table as an optimization delta.

## Resource Evidence and Tests

Before the runtime edit, the added native-wired regression failed on abandoned
preparation: one allocation and seal, zero writes, zero free-seam calls after
plan release. After the edit that same path has exactly one free-seam call and
no retained fixture page. These are explicit seam counters, not allocator/RSS
inferences.

| Case | Verified After Behavior |
| --- | --- |
| Abandon prepared native plan | One allocation/seal/free; no entry write or original slot. |
| Entry changes before native commit | No write; reclaim immediately; continuation kind/address cleared. |
| Catalog changes before core commit | STALE_PLAN, no write; reclaim on release. |
| Native write succeeds | Backing survives report/plan/runtime release. |
| Native write succeeds but readback fails | FAILED_UNKNOWN; backing survives release. |
| Native memcpy/store succeeds but restoration fails | UNKNOWN; backing retained; uncertain patch accounted for; no retry. |
| Overwrite remap fails | UNKNOWN; backing retained even if the host stub leaves the old bytes intact. |
| Writer explicitly returns PARTIAL | FAILED_PARTIAL; backing survives release, no retry. |
| Entry restored after successful native commit | Repeat commit returns UNKNOWN without writing or reclaiming. |
| No free seam | Backing stays retained; fixture explicitly frees it at teardown. |
| Four-slot static pool, 1000 prepare/release cycles | 1000 claims/seals/releases; all four slots free after every iteration. |
| Mandatory sibling fails preparation (static/anonymous backing) | Earlier native backing reclaimed once; fresh hook result/report has empty continuation and `currently_valid=false`. |

```sh
make -j1 test
make -j1 test HOOKKIT_SANITIZE=address,undefined
```

Both full host runs passed, including C/C++/ObjC/ObjC++ header compilation,
ownership/domain/lifecycle tests, artifact snapshots, allocation fault injection,
provider tests, and the new native/static regressions. Leak detection remained
enabled. An initial sanitizer run exposed a missing teardown for the new
Linux-only synthetic catalog fixture; that fixture was corrected before the
successful full rerun.

The review-driven domain regression failed against the earlier cleanup and then
passed after central result invalidation. The new `test-native-write` target is
included in `make test`. It compiles the actual `hk_native.c` writer against
host-only Mach/cache-control fixture headers, not a simulated Boolean wrapper.
Tests exercise actual memcpy/atomic-store ordering, restoration failure,
pre-store refusal, temporary-mapping failures, overwrite-remap uncertainty,
static/anonymous continuation retention, and terminal/memory/rebind propagation.
Both full plain and ASan/UBSan host suites passed after this correction, including
the independently added provider regressions. The VM functions are still stubs;
these tests establish control-flow/mutation accounting, not kernel behavior.

Required callback migrations were completed in:

- `tests/device/device_static_continuation.c` and `tests/device/device_rebind.m`;
- `tests/macos/test_live_hooks.m` and `tests/macos/test_live_reloc_inline.m`;
- host inline, memory, rebind, relocating/static and Swift writer fixtures.

Device/macOS fixture callbacks now return mutation states; direct device setup
writes compare explicitly against COMPLETE. Their pre-store fault injection
returns NONE, not false. Native/core/engine sources and these four fixture files
passed serial arm64 and arm64e iOS 15 syntax checks using the iPhoneOS16.5 SDK and
modern Theos compiler. This is cross-target type/syntax validation, not a linked
framework build, macOS runtime validation, or device execution.
The changed runtime/native/engine sources also passed serial armv7/armv7s iOS 9
syntax checks with iPhoneOS14.5 and `HK_NO_DOBBY`, covering the unsupported native
writer stubs and legacy provider compilation. No framework binaries were linked.

## Legacy Bridge Follow-Up

This follow-up postdates the device/lane matrix below; that matrix does not
validate the subsequent facade source changes.

The legacy batch original-pointer loss is present in HEAD and commit `2152400`,
before the footprint changes: early publication was followed by
`map_result(..., NULL)` overwriting that pointer. The shared mapper now clears
outputs only for proven NONE, never transiently nulling them during uncertain
mutation. Single-call commit API errors also consult the settled hook result,
since report construction can fail after a target was modified. Public ABI and
legacy error values are unchanged. `test-legacy-bridge` exercises the actual
facade/plan/native adapter with host writer seams and an injected post-commit API
error; it does not claim allocator-failure injection inside that call.
The regression failed before the mapper fix. Both full host and full
ASan/UBSan suites, including this target, passed afterward (runtime-agent runs).

The bounded post-facade validation on the arm64 iPhone 7 passed **35 processes,
no skips**:

- Packaged `device-legacy-facade-smoke`, five runs: owned single and batched
  functions retain callable originals after the bridge releases its internal
  plans/runtimes; the batch replacement itself calls its published original.
- Source-linked `device-legacy-bridge-smoke`, five runs of the existing 20-case
  host bridge fixture: simulated PARTIAL/UNKNOWN, verification failure, explicit
  NONE, success, and post-commit API errors preserve/clear outputs as required.
  Heap-buffer writes and retained-byte reads are real; mutation/error outcomes
  are injected. No kernel failure or uncertain-original callability is proven.
- Packaged public observability, five runs each of memory, ObjC, inline, static,
  and prepare-refusal: 25 passes with runtime path/UUID verification.

Current framework SHA-256:
`b5dfbed00b64c1d9897efd584f3f78633aebdc85829762ef3bde6a5b64781d17`;
executed arm64 UUID: `f99736c35713353e8fc9c2e06735da77`.
The current `build.sh rootless` core/Gum packages passed compatibility and exact
export gates (arm64/arm64e, iOS 15, arm64e ABI `80`), with no warnings. Only
redundant per-lane host tests were skipped via `HOOKKIT_SKIP_LANE_TEST=1`.
Other lanes and the 110-process matrix below were **not rerun after the facade
edit**. No broad system-symbol legacy smoke was run.

Evidence: `/tmp/opencode/hookkit-legacy-final-20260907/`, especially
`legacy-device.log`, `public-final.log`, `rootless-build-compat.log`, and
`source-before.sha256`. A final validation-agent `make -j1 test` also passed
(`host-final.log`); sanitizers were not rerun in this bounded device follow-up.
Fresh `/var/jb/tmp` deployments were hash-checked and
removed; installed framework/provider hashes were unchanged. No global install,
Theos refresh, respring, commit, or push occurred.

## OPEN: Rebind Cache Race

**OPEN, not fixed or deterministically reproduced in this session.** The rebind
file-cache concern follows from ownership/locking inspection, not a concurrent
crash reproduction. `src/engines/HKRebindEngine.c:444-451` copies a borrowed
slice/fixups blob and unlocks before parsing at lines 493-495. Another preparation
can evict it and `munmap` it at lines 625-630. The miss path likewise transfers
ownership and unlocks at lines 640-656 before parsing at lines 665-667. Core
prepare has no enclosing process-wide lock that closes either window.

No cache code was changed in this follow-up. The bounded recommendation is to
retain the file-cache lock through cached-view parsing and parse a miss while
its local mapping is still owned, before publishing it into the cache. Moving
publication after parsing also avoids the insertion-OOM branch closing the
mapping before use. A focused deterministic regression is still required: use
at least five real file-backed synthetic images for the four-entry cache, pause
a reader after hit unlock or miss publication, force its mapping's eviction,
then resume parsing. Ensure the symbol-cache lookup misses so it cannot bypass
the vulnerable parse. Cover both paths; borrowed-buffer fixtures and successful
single-image rebind smokes do not prove concurrent eviction safety.

## Pre-Facade Execution Results

**Historical: this matrix and all four lane checks predate the legacy facade
edit.** They superseded the earlier compile-only status for the native writer
and domain-result corrections, not for the later facade change. The runtime
agent reported full host and full ASan/UBSan suites passing at that point.
That arm64 device run passed **110 processes, no skips**, covering native
lifetime, public observations, packaged providers, rebind and Swift. See
[the final device matrix](OBSERVABILITY.md#final-footprint-and-safety-validation)
for source-linked versus packaged coverage, exact framework identity and logs.
Concrete reuse evidence is 1000 host prepare/release cycles through four slots,
plus 320 abandoned and 160 stale device preparations. Activated or uncertain
backing remains retained; reclamation is not an uninstall operation.

All four `build.sh` lanes passed compatibility and exact-export checks:

| Lane | Architectures | arm64e ABI | Minimum iOS | SDK |
| --- | --- | --- | --- | --- |
| rootful-legacy | armv7 / armv7s / arm64 / arm64e | 00 | 9 (arm64e: 12) | 13.7 |
| rootful-modern | arm64 / arm64e | 80 | 14 | 16.5 |
| rootless | arm64 / arm64e | 80 | 15 | 16.5 |
| roothide | arm64 / arm64e | 80 | 15 | 16.5 |

Legacy produced 38 incompatible-ABI compiler warnings with Clang 11.1 and
ld64 609; output was verified to carry the intended ABI. Modern lanes had no
warnings and verified arm64e subtype `0x80000002`. These are cross-build/ABI
checks, not legacy or arm64e device execution. The full rootless lane framework
was byte-identical to the packaged framework executed on device. Source hashes
were unchanged across device and lane validation. Temporary device and lane
staging directories were cleaned; global Theos/device providers and frameworks
were unchanged. No commit, push or global install occurred.

Remaining limits are real arm64e/PPL and legacy device execution, actual kernel
VM refusal/partial-write execution, and proven overlapping syscalls. Executing
target pages were excluded from overlap testing. Real absent/malformed/late
provider-loader integration was intentionally not implemented: no loader seam
or global changes were introduced. Adapter availability rediscovery is host-only
coverage, distinct from the successful real-provider device runs.

Returning a pool slot restores capacity, not file-backed section size or dirty
page contents. A production anonymous free attempts deallocation of one actual
OS page (`getpagesize()`), not merely `HK_RELOC_PAGE_BYTES`; its physical-footprint
effect and success must be measured on device. No RSS or device latency saving
is claimed from these results. Historical before/after timings remain cleanup
controls, not evidence of an overall speedup.
