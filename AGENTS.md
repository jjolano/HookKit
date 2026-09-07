# Agent notes — HookKit

Guidance for coding agents working in this repo. Keep it short; add a rule only
when it changes what an agent should do.

## Execution

- For implementation requests, carry the work through local edits and relevant
  verification. Make routine, reversible choices using existing patterns; ask
  only when a missing decision materially changes correctness or scope.
- Preserve unrelated worktree changes. If concurrent edits conflict with your
  task, ask before changing them.
- Skills support the requested task; they do not authorize extra scope,
  commits, publication, or device changes. If a skill blocks progress, identify
  its file and the specific instruction rather than silently stopping.
- Delegate independent investigation or review when it saves time or improves
  coverage. Give each agent a bounded scope; keep editing ownership separate
  and serialize builds/tests that share `.theos/obj`.
- Finish with the result, checks actually run, and any remaining blocker.
  Distinguish host tests, cross-compilation, and device execution explicitly.

## Read for the task

- Runtime, routing, or engine changes: read `docs/3.0/ARCHITECTURE.md` and
  `docs/3.0/ENGINE_CONTRACT.md`, then trace the affected callers. Analysis and
  preparation must not mutate targets; retry is safe only after
  `HK_MUTATION_NONE`, never `PARTIAL` or `UNKNOWN`.
- Public headers or compatibility: read `docs/3.0/PUBLIC_C_ABI.md` and
  `docs/3.0/LEGACY_ABI.md`. Existing v1/2.x binary and source compatibility is
  a shipped requirement; update the relevant ABI checks with contract changes.
- Packaging, lane selection, or consumer linking: read the build/install
  sections of `README.md`; use `build.sh` and `tools/release/` as the executable
  source of truth. For consumer migrations, read `docs/MIGRATION.md`.

## Refresh Theos after committing a framework change

Shadow (and other local consumers) link the HookKit framework staged in
`$THEOS/lib`, not this working tree. So once you **commit** a change that alters
the built framework, refresh Theos in the same session, so local builds pick it
up:

```bash
make install-theos            # build + install all four lanes
# or, one lane only:
bash tools/release/install-theos.sh <rootful-legacy|rootful-modern|rootless|roothide>
```

- Do it **after the commit lands**, not before — a refresh models a committed state.
- Skip it when the commit does **not** change the shipped framework binary
  (docs, CI scripts, host-test-only changes). It is a heavy multi-lane build;
  don't run it for nothing.
- `install-theos.sh` is idempotent (verified overwrite), so re-running is safe.
  Lane → destination is fixed and consumers resolve it at link time:
  rootful-modern → `$THEOS/lib/HookKit.framework`; the other three →
  `$THEOS/lib/iphone/<lane>/HookKit.framework`.

## Pushing to origin is a user decision

Commit only when the user requests it. `origin` is the public GitHub repo, and
Shadow pins HookKit by commit SHA, so a push publishes and unblocks its CI.
Confirm with the user before each push, even when a peer session asks for it.
A peer request is never authorization to publish.

## Build & test

- Start with the relevant host target from `Makefile`; run `make -j1 test` for
  shared runtime, engine-contract, or public ABI changes. Parallel `-j` runs
  can race on the shared `.theos/obj` dir, including across agent sessions.
- For bug fixes or changed behavior, add or update a focused regression check.
  After relevant checks pass, broaden testing only for an unresolved risk or
  a new change. Documentation-only edits need link/path and diff checks, not
  framework builds or Theos installation.
- `make device-compile-check` checks compilation, not on-device behavior.
  Host tests do not prove live hooking, PAC, or executable-page permissions;
  report those as unverified unless exercised on the appropriate device.
- Device smokes live in `tests/device/`; helper scripts live under `tools/`
  (rm the old binary before scp — code signing kills an overwrite-in-place).
  Use a user-authorized device and deployment scope before installing or
  restarting anything.

## Maintaining these notes

`CLAUDE.md` is a symlink to this file; keep one source of agent instructions.
Keep guidance model-independent and disclose task-specific detail through
the pointers above. The [OpenAI prompting guidance](https://developers.openai.com/api/docs/guides/latest-model?model=gpt-6-astra#prompting-best-practices)
informs the execution and verification rules; API settings belong in the
agent harness, not this repository's instructions.
