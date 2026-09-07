# Controlled Observability Baseline

`tests/device/device_observability.m` observes its own
data, class, functions, and linked HookKit. No third-party targets, detector
tests, concealment, inspection spoofing, or anti-debugging. The metadata
correctness follow-up below changes reporting, not allocation/patch mechanisms.
The baseline and metadata sections are historical; the final footprint/safety
validation below records the current lifetime and writer behavior.
Each mode runs in a fresh process, at serialized startup, with a pinned native
route. This is a correctness/observation harness, not a stealth score or benchmark.

## Mechanism Matrix

These are mechanism properties, not promises that every observation changes on
every install. Existing smokes are coverage references, not new duplicate suites.

| Mechanism | Unavoidable state on successful installation | Baseline / existing coverage | Gaps |
| --- | --- | --- | --- |
| Memory patch | Requested bytes change; original bytes and ownership/artifacts can remain retained | `memory`: four owned data bytes, expected-byte check, retained original bytes | Not an executable-memory patch; no callable original applies |
| ObjC IMP | Method dispatch metadata points to a different IMP; predecessor remains callable | `objc`: owned class, dispatch 7 -> 107 via original; `device-objc-smoke` covers chains/deferred availability | Not all caches, inherited/class methods, or concurrent mutation |
| Relocating inline | Entry bytes change; displaced instructions and jump-back code exist in a continuation | `inline`: owned entry 9 -> 109 via original, continuation bytes/image, post-release calls | Near target only; does not force anonymous allocation or pool exhaustion |
| Static continuation | Entry bytes AND an already executable pool slot change; pool/continuation remain visible | `static`: public `inline-static` route, no-dynamic policy; existing `device-static-smoke` exercises internal pool seams | Static is not immutable text, zero memory cost, or unchanged VM topology; far/exhausted/blocked pool may refuse |
| Terminal inline | Entry bytes change; no callable original is supplied | `test-inline-engine`, `test-inline-wired` | No new terminal device mode; do not claim continuation coverage |
| Import rebinding | Selected import-slot pointers change; original entry need not change | `device-rebind-smoke` (`device_rebind.m`), `device-rebind-adapter-smoke` | No new VM/RSS measurements; does not reach saved pointers/direct calls; no new future-image test |
| Swift vtable | Selected metadata slot changes, not every Swift call site | `device-swift-real-smoke`, `device-swift-facade-real-smoke` | No new measurements; direct/inlined/witness dispatch and PAC coverage are separate |
| Provider inline | Entry/provider state changes; continuations, image loads or allocations depend on provider | Existing `device-provider*-smoke` targets | Not activated by this harness; provider internals may be only partly reported |

`device_smoke.m` remains legacy coverage, not a new provider matrix: despite its
historical labels, its first two function hooks pin `inline-relocating`. It also
contains a system-symbol scenario, so the isolated runner deliberately does NOT
execute it. Reused smokes are not all strictly owned-target-only runners.

## What Is Measured

- `OBS` records raw bytes (four data bytes or the first 16 code bytes), current
  IMP/original pointers, dyld image count, executable leaf VM-region count and
  summed virtual sizes, and `MACH_TASK_BASIC_INFO.resident_size` in bytes.
- Phases: before runtime creation, analyzed, prepared, committed, plan/runtime
  released, and snapshots released. The framework, identity checks and target
  setup are already loaded/warmed at the first sample; this is NOT a pre-load
  baseline. No image-count assertion is made.
- VM enumeration uses `vm_region_recurse_64(mach_task_self(), ...)`, descends
  submaps and counts current executable leaf mappings, including shared-cache
  mappings. Counts can split/coalesce without adding executable bytes. This is
  not allocation accounting, private dirty memory, or a trace of transient W/X
  protection changes. Enumeration and dyld counts are not atomic snapshots.
- RSS includes runtime/catalog setup, allocator caching, page faults, output and
  snapshot copies. It is neither a retained-artifact byte count nor a causal
  per-hook cost. No RSS delta assertion or optimization claim is made.
- `RESULT`/`ARTIFACT` are public API reports, not independently verified complete
  process inventories. Numeric enums are in `HookKitResults.h` and
  `HookKitArtifacts.h`; effects print in hex. `ORIGINAL` independently uses
  `dladdr` and `getsectiondata` to report whether the pointer is in `__hktramp`.
- Behavioral assertions require active/complete installation, patched data or
  dispatch, and callable predecessors/continuations after plan/runtime release.
  Retained snapshots and a fresh process snapshot must agree in count; memory
  artifacts must still contain the original bytes. Inline modes additionally
  assert actual pool backing, reserved mapping extent and R-X protection,
  prepare/commit metadata identity, capability versus observed effects, and
  the target artifact's actual replacement pointer. Retained and process
  snapshots repeat those artifact checks after release. This does not validate
  every field of every artifact. Release is NOT unhook: live code, original slots,
  ownership and process artifacts persist. No restoration or trampoline reclaim
  is attempted; process exit is the cleanup boundary.
- A native inline/static clean refusal (NO_ROUTE/FAILED_SAFE, mutation NONE)
  prints `SKIP` and diagnostics, asserts unchanged target/behavior, and exits 77.
  The result is inspected after preparation: a clean refusal bypasses commit
  (including when the plan is FAILED), logging `commit-not-attempted`.
  Partial/unknown mutation and failed behavioral assertions fail the run. The
  runner accepts 77 but preserves it in the log; it is not a success result.
- `prepare-refusal` is a focused regression check, not a hook-success mode. It
  requests mismatched expected bytes on the owned data buffer, asserts analysis
  succeeded followed by FAILED_SAFE/HK_PLAN_FAILED during preparation, then
  checks unchanged bytes/behavior and zero retained/process artifacts after
  release without committing. It exits 0 only when those assertions pass.
  `ARTIFACT` distinguishes `artifact_size` from `mapping_size` in its output.

## Reproduce

Linux/Theos host, SDK 16.5, signed rootless arm64 build, iOS 15 minimum. Run from
the repository root. Builds are serial and never install globally:

```bash
work=$(mktemp -d /tmp/opencode/hookkit-observability.XXXXXX)
mkdir -p "$work/build"
make -j1 test > "$work/host-tests.log" 2>&1
make -j1 all LIBRARY_NAME= ARCHS=arm64 TARGET=iphone:clang:16.5:15.0 \
  THEOS_PACKAGE_SCHEME=rootless FINALPACKAGE=1 THEOS_BUILD_DIR="$work" \
  _THEOS_LOCAL_DATA_DIR="$work/build" > "$work/framework-build.log" 2>&1
make -j1 device-observability THEOS_OBJ_DIR="$work/build/obj" \
  DEVICE_OBSERVABILITY_FRAMEWORK_DIR="$work/build/obj" > "$work/probe-build.log" 2>&1
read -rs -p 'Device SSH password: ' SSHPASS
export SSHPASS
bash tools/run-observability.sh "$work/build/obj" mobile@10.0.1.160 \
  > "$work/device-measurements.log" 2>&1
unset SSHPASS
```

The runner needs host `sshpass` for password authentication (omit `SSHPASS` for
SSH keys), Theos `otool`, and device `mktemp`, `tar`, `sha256sum`. It creates a
fresh `/var/jb/tmp/hookkit-observability.*`, immediately logs the validated path
as `REMOTE_DIRECTORY` before transfer, transfers the framework and probe,
compares both SHA-256 hashes, then passes expected paths AND UUIDs for in-process
assertions on every launch. `@executable_path` loads the isolated framework.
Five launches per mode (including `prepare-refusal`) are followed by directory removal and an unchanged-hash
check of the installed framework. No `/var/mobile` executable mapping, global
device/Theos install, respring, preferences, or live trampoline cleanup.
If SSH becomes unreachable, the EXIT trap reports cleanup failure with the
explicit directory path; remove only
the logged temporary directory once connectivity returns.

Compile-only reuse checks (normal recipes link `.theos/obj/HookKit.framework`):

```bash
make -j1 device-objc-smoke device-static-smoke device-smoke \
  device-rebind-smoke device-rebind-adapter-smoke \
  DEVICE_SMOKE_SDK="$THEOS/sdks/iPhoneOS16.5.sdk" DEVICE_SMOKE_MIN=15.0
```

## Recorded Baseline (Historical)

2026-09-07, iPhone9,3 / iPhone 7, iOS 15.8.3 rootless, arm64. Final probe:
`cf3926a18ab63991901b4f57e5119316`; framework UUID:
`73ca130dd1be3a1f861749da5e21fdf0`. All four modes passed 5/5 launches, no SKIPs.

| Mode | Direct observation | Executable regions before / prepared / committed / released | Retained artifacts | First launch RSS before / committed / released (bytes) |
| --- | --- | --- | --- | --- |
| Memory | `01020304` -> `05060708` | 17 / 17 / 17 / 17 | 1 memory patch | 2,785,280 / 2,867,200 / 2,867,200 |
| ObjC | IMP changes; original code bytes unchanged; 7 -> 107, original 7 | 17 / 17 / 17 / 17 | 1 method change | 2,768,896 / 2,867,200 / 2,867,200 |
| Inline | Four-byte entry branch; original 9, replacement 109 | 17 / 19 / 20 / 20 | 2 | 2,768,896 / 2,883,584 / 2,883,584 |
| Static | Four-byte entry branch; original 9, replacement 109 | 17 / 19 / 20 / 20 | 2 | 2,752,512 / 2,850,816 / 2,883,584 |

Image count was 101 and summed executable virtual size 1,777,025,024 bytes
throughout these runs. This includes shared mappings, not 1.77 GB of resident
probe memory. Both inline modes' originals resolved inside the framework's
`__TEXT,__hktramp`, including after release.

**Historical reporting discrepancies, now resolved:** this baseline labeled
pool-backed `inline-relocating` continuations DYNAMIC/ANONYMOUS with
`dynamic_exec=1`, reported a 128-byte buffer as the mapping extent, and printed
the target entry as `replacement_pointer`. See Metadata Correctness below for
the fix and replacement evidence. The baseline's `fully_inspected=0` artifact
flags are unchanged; this work does not claim exhaustive process inspection.

Host suite, six relevant device compile targets (including this probe), and
shell syntax/diff checks passed. The aggregate compile target also built the
probe; its optional Swift parse step reported `no Swift driver, probe skipped`.
Only the new probe was executed on device in
this deliverable; other architectures, concurrent installs, dynamic fallback,
and provider/Swift/rebind measurements remain gaps. Logs/builds are under
`/tmp/opencode/hookkit-observability-20260907/`, especially
`host-tests.log`, `device-compile-checks.log`, and `device-measurements.log`.
Earlier `device-runs.log` records a fixed harness-only early original-slot
lookup assertion; its cleanup also succeeded. No runtime code changed.

### Reviewer Follow-up

The preparation-refusal fix was compiled with `make -j1 device-observability`
against the same isolated framework. Probe UUID:
`b5f6ba1283c0351eae372b7341f0bdda`. All four installation modes passed 5/5
launches again, and `prepare-refusal` passed 5/5 with FAILED_SAFE, mutation NONE,
no commit, unchanged bytes/behavior, and zero artifacts after release.
`review-device-runs.log` records hash/UUID checks and successful directory cleanup
with the installed framework unchanged. `review-probe-compile.log` contains the
clean compile result. A local mocked-SSH transfer/cleanup failure verified early
directory logging, the explicit recovery path and exit 1; see
`check-cleanup-failure.sh` and `review-cleanup-failure.log` in the same log directory.
`bash -n tools/run-observability.sh` and `git diff --check` passed. This follow-up
did not rerun the host suite or force native pool exhaustion/VM-protection refusal;
the deterministic memory precondition tests the shared preparation-refusal path.
That harness-only follow-up did not change runtime metadata. The next step did:

## Metadata Correctness (Historical)

2026-09-07, same iPhone 7 rootless arm64 device. Internal-only changes; no public
header layout, symbol allowlist, or ABI version changed. No new allocation,
placement, protection, activation, or reclamation strategy was introduced.

- The existing relocation allocation callback now returns actual backing
  kind/base/size alongside its buffer pointer. Both production pool routes,
  anonymous fallback, host fake allocators, and the device/macOS smoke seams
  supply it. Invalid extents are refused and reclaimed before writing.
- The prepared plan owns that description. Continuation results, trampoline
  artifacts, retained failure artifacts and provider-hybrid original artifacts
  use it rather than a route-level static flag. The obsolete flags are removed.
- `artifact.size` remains the fixed 128-byte trampoline buffer capacity, not
  emitted code length. `mapping.base/size` and continuation mapping fields now
  describe the reserved backing extent: one 16,384-byte pool slot on this device,
  or the native allocator's `getpagesize()` anonymous allocation. Host buffer
  fakes report their actual buffer extent, not an invented device page size.
  This is not the whole Mach-O section, a possibly coalesced VM leaf region,
  private dirty memory, RSS, or a transient protection-change inventory.
- A successful seal supplies R-X mapping protection and a generated mapping ID
  shared by the prepared result and subsequent records. Failed preparation
  publishes no continuation mapping. Refused entry writes that reclaim storage
  clear its prepared metadata and override the core's cached result with NONE;
  unreclaimable storage retains its actual backing metadata.
- Default-route and ElleKit-hybrid capability masks cover both possible backing
  effects. Actual prepare/commit effects report only the backing used. The
  explicit static route still declares no executable allocation; requests
  forbidding dynamic memory still cannot select the default fallback-capable
  route. No behavior is inferred from a capability mask.
- The target artifact retains `spec.replacement`, including its callable pointer
  representation, separately from the stripped address used to emit branches.

Lifecycle boundaries remain explicit: static claim/release uses the existing
bitmap; dynamic allocation/free uses the existing native page functions. The
subsequent safety work now reclaims unpublished native backing on abandon/stale
refusal through those seams; activated or uncertain backing stays retained.
Provider-hybrid release still reclaims only before the provider was called.
Missing-free-callback behavior is not made leak-free. See the current results
below rather than treating the metadata-only step as the final lifetime policy.

### Verification

- `make -j1 test`: passed, including the existing allocation-failure sweep.
- ASan + UBSan: relocating engine, wired lifecycle, static pool/fallback,
  provider-vtable and fault-injection targets passed. The lifecycle OOM sweep
  failed each of its 44 allocation sites; it is not a device VM OOM test.
- New regressions cover actual host pool-backed default and explicit routes,
  exhausted-pool anonymous fallback, mapping identity/extent/protection,
  replacement pointers, allocation/invalid-mapping/seal/write refusal,
  reclaimed-versus-retained storage, unavailable-ledger failure, and both
  provider-hybrid backings. Host allocation/protection seams do not prove iOS VM
  behavior. Existing OOM handling and mutation-state promotion remain intact.
- Fresh signed rootless arm64 framework rebuild and both observability/static
  probe builds passed. Only the isolated observability probe was executed.
  Framework exports exactly match the existing arm64 allowlist.
- Final device matrix: `memory`, `objc`, `inline`, `static`, `prepare-refusal`
  each passed **5/5**, all exit 0, no skips. Both inline routes independently
  resolved inside `__hktramp`, reported STATIC/STATIC_HOOKKIT_SECTION,
  `dynamic_exec=0`, 16,384-byte mapping extent, 128-byte artifact span and the
  actual replacement function. Prepare/commit and post-release assertions passed.
  Refusal mode retained unchanged bytes/behavior and zero artifacts, no commit.

Final framework UUID: `52d4fa29154a302d8f49c40c8bcf767c`.
Final probe UUID: `3d6aec4b559f3c499c5ccd4dda2cbe61`.
SHA-256:

```text
9cccf2b9ba69c337151834623a244eafb08186cbf861333199aaba4331a0fe74  HookKit.framework/HookKit
023d903de703b5957bcea845608ad030ccb7546cab6797c3f4ae0ea17370635a  device_observability
```

Both transfer hashes and loaded-image paths/UUIDs were checked. Final temporary
directory `/var/jb/tmp/hookkit-observability.EgA7VA` was removed and absence
verified. Installed device framework hash stayed
`3531540b00a66ef913d48c7ed19bcf5b33e936bd57b1d859cbf006b36a5d0b19`.
Theos default and rootless framework hashes also stayed unchanged. No global
install, respring, preferences, commit or push occurred.

Logs/builds: `/tmp/opencode/hookkit-observability-20260907/metadata/`.
Final evidence: `host-tests-final.log`, `sanitizers-final.log`,
`framework-build-final.log`, `probe-build-final.log`,
`device-measurements-final.log`, `exports-final.log`, `theos-before.sha256`,
`theos-after.log`. Earlier logs in this subdirectory record the first passing
matrix before the reclaimed-continuation metadata regression was added; the
final matrix uses the rebuilt current framework.

## Final Footprint and Safety Validation

2026-09-07, same iPhone 7 / iOS 15.8.3 rootless arm64 device, final current build:
**110 processes passed, no skips**. Earlier UUIDs, hashes and coverage gaps above
belong to historical runs, not this final matrix.

| Coverage | Linkage | Processes / Result |
| --- | --- | --- |
| Native lifetime and writer checks | Source-linked | 10 modes x 5 = 50 passed |
| Public observability, including preparation refusal | Packaged framework | 5 modes x 5 = 25 passed |
| Default provider probe | Packaged framework | 5 processes; Dobby, Gum and ElleKit each passed 5/5 |
| ElleKit dynamic fallback | Packaged framework | 5 passed, exhausting all 8 actual pool slots |
| Rebind writer smoke | Source-linked | 5 passed |
| Rebind adapter smoke | Packaged framework | 5 passed |
| Swift synthetic / real / facade | Packaged framework | 5 each = 15 passed |

Native device coverage includes 320 abandoned and 160 stale preparations.
Unpublished native storage is reclaimed; activated/uncertain backing is retained.
Review also corrected mandatory-domain rollback's fresh result so it cannot
advertise a stale released continuation. Internal writers return mutation state
rather than Boolean success: an actual store followed by restoration failure is
UNKNOWN, propagated correctly through native, terminal, memory, rebind and Swift
paths. Device fault injection and host VM stubs do not establish actual kernel
refusal or partial-write behavior.

The runtime agent reported full host and full ASan/UBSan suites passing.
[PERFORMANCE.md](PERFORMANCE.md#final-execution-results) records the four-lane
compatibility/export results and resource evidence; its before/after benchmark
tables are historical cleanup controls, not an overall speedup claim.

Executed packaged framework SHA-256 (identical to the full rootless lane):

```text
e427d29b06a1ed087decd8525fb39ac6a37b4623933eb0843e88909b6d86bd6a
```

Final logs: `/tmp/opencode/hookkit-safety-20260907/`:
`native-final.log`, `public-final.log`, `providers-writers-final.log`,
`host-final.log`. Source hashes were unchanged across device and lane validation.
All temporary device and lane staging directories were cleaned; global Theos
and device providers/frameworks stayed unchanged. No commit, push or global
install occurred.

Intentional limits: real arm64e/PPL and legacy device execution, actual kernel
VM refusal/partial-write execution, and syscall overlap remain unproven;
executing target pages were excluded. Real absent/malformed/late provider-loader
integration was not implemented, with no loader seam or global changes.
Adapter availability rediscovery is covered only on host, unlike the packaged
real-provider execution above. Artifact inspection remains non-exhaustive;
these runs do not prove complete allocation accounting or RSS/latency savings.
