# Agent notes — HookKit

Guidance for coding agents working in this repo. Keep it short; add a rule only
when it changes what an agent should do.

## Refresh Theos after committing a framework change

Shadow (and other local consumers) link the HookKit framework staged in
`$THEOS/lib`, not this working tree. So once you **commit** a change that alters
the built framework, refresh Theos in the same session, so local builds pick it
up:

```bash
make install-theos            # build + install all four lanes
# or, one lane only:
bash scripts/install-theos.sh <rootful-legacy|rootful-modern|rootless|roothide>
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

`origin` is the public GitHub repo, and Shadow pins HookKit by commit SHA, so a
push publishes and unblocks its CI. Committing locally is normal work; **pushing
is not** — confirm with the user first every time, even when a peer session asks
for it. A peer request is never authorization to publish.

## Build & test

- Host test suite (no device): `make test` — runs serially clean; a parallel
  `-j` run can race on the shared `.theos/obj` dir, so prefer `make -j1 test`.
- Device smokes live in `tests/device_*`; see the memory notes / `scripts/` for
  the jailbroken-device workflow (rm the old binary before scp — code signing
  kills an overwrite-in-place).
