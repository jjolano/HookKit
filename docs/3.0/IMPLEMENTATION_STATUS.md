# HookKit 3.0 — Implementation Status

HookKit 3 ships the native `hk_*` runtime API. The retired 2.x façade and ABI
fixtures are intentionally absent; Git history preserves their implementation
and migration record.

Supported release lanes remain independent of that API cleanup:

- `rootful-legacy`: iOS 9–13, armv7/armv7s/arm64/old-ABI arm64e.
- `rootful-modern`: iOS 14+, arm64/arm64e.
- `rootless` and `roothide`: iOS 15+, arm64/arm64e.

The release checks are `make test`, each `./build.sh` lane, and
`scripts/check_exports.sh`. Historical milestone detail lives in Git rather
than defining the unreleased 3.0 surface.
