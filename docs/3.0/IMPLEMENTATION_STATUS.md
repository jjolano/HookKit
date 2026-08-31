# HookKit 3.0 — Implementation Status

HookKit 3 ships the native `hk_*` runtime API and the v1/2 compatibility
facade. `HKSubstitutor` and all six v1 module classes remain exported as
translators over the 3.0 plan lifecycle, and the compatibility headers remain
part of the framework. Historical ABI snapshot fixtures and comparison tooling
are intentionally absent.

Supported release lanes remain independent of that API cleanup:

- `rootful-legacy`: iOS 9–13, armv7/armv7s/arm64/old-ABI arm64e.
- `rootful-modern`: iOS 14+, arm64/arm64e.
- `rootless` and `roothide`: iOS 15+, arm64/arm64e.

The release checks are `make test`, each `./build.sh` lane, and
`tools/release/check_exports.sh`. They cover current public-header compilation,
package/linker compatibility, and exact current exports; compatibility device
smokes live in `tests/device/`. No release check claims to compare historical
selector or type-encoding snapshots. Historical milestone detail lives in Git.
