# Vendored components

Everything under `vendor/` is third-party code, headers, or pinned build-input
metadata. This file records, per component, the upstream source, license, local
modifications, and the gaps where provenance is unknown. All upstream pins
below were byte-verified against upstream by the provenance research lane
(2026-08-11); discrepancies from the pre-verification state are noted where
relevant.

## Inventory

| Directory | Contents | Upstream | Version/SHA | License | Local patches |
|---|---|---|---|---|---|
| `vendor/litehook/` | fixup-chains.h, APSL-2.0.txt | https://github.com/opa334/litehook | `cb5c5a39f736b367e72ced1aa0bfeb79a8be269e` (main, 2026-07-31); retained files byte-identical to pristine upstream at that commit | APSL-2.0 (Apple header; full text retained) | none (host-test fixture) |
| `vendor/dobby/` | dobby.h, libdobby.a, LICENSE, dobby.lock, patches/0001-publish-original-before-activation.patch | https://github.com/jmpews/Dobby | `5dfc8546954ce3b3198132ab13fddb89ee92cdd7` (2024-03-14); lock records source and archive SHA-256 values | Apache-2.0 (LICENSE vendored) | yes — binary rebuilt from source with one reorder patch (publication-before-activation), see below; dobby.h unchanged |
| `vendor/gum/` | hkgum.c, COPYING, gum.lock (generated devkit ignored) | https://github.com/frida/frida-gum | tag 17.17.0 = `ddc10c5559cbb41a3dd72866bfba6ff3945ffa5c`; official arm64/arm64e devkits are pinned by URL and SHA-256 in gum.lock | wxWindows Library Licence 3.1 (`COPYING` vendored; GNU Library GPL v2-or-later text plus wxWindows exception) | hkgum.c wrapper only — see below |
| `vendor/libhooker/` | libhooker.h, LICENSE | https://github.com/coolstar/libhooker | master; only milestone is OSS 1.6.9 commit `4f85a68dae` (2023-04-17); in-tree header predates that release (unchanged since HookKit's initial commit 75cdb22) | BSD-4-Clause (LICENSE vendored) | small header deltas — see below |
| `vendor/substitute/` | substitute.h | https://github.com/comex/substitute | master; header frozen since `83442f9005` (2015-07-16); no v2 git tag exists upstream | public domain / CC0 1.0 for this header (header comment; upstream `LICENSE.txt` expressly places substitute.h and generated files in the public domain) | 2 small deltas — see below |

## Component notes

### Apple fixup-chain test fixture

`fixup-chains.h` and `APSL-2.0.txt` are byte-identical to LiteHook
`cb5c5a39f736b367e72ced1aa0bfeb79a8be269e` (main, 2026-07-31).
`fixup-chains.h` retains Apple's APSL-2.0 header, and `APSL-2.0.txt` retains
the full license text. `tests/host/test_chained_fixups.c` uses the header to
cross-check the parser's structs and bitfields. Rebuild: none; host-test
fixture only.

### dobby (binary rebuilt from source, one internal reorder patch)

Base https://github.com/jmpews/Dobby @
`5dfc8546954ce3b3198132ab13fddb89ee92cdd7` (2024-03-14). The vendored
`libdobby.a` was rebuilt from that commit with one local patch; `dobby.h` is
byte-identical to upstream `include/dobby.h` (+ the provenance comment line)
and the patch touches no public symbol or signature — ABI unchanged.

- `2026-08-11` — `source/InterceptRouting/InlineHookRouting.h`: publish the
  relocated original into the caller's out cell **before** activation.
  Upstream `DobbyHook` assigns `*out_origin_func` *after*
  `routing->Active()` (the trampoline is already written over the prologue by
  then). The patch moves the `if (out_origin_func)` block (and the
  `arm64e_pac_strip_and_sign` on the cell) ahead of `routing->Active()`;
  ordering only — the error check still runs after `Active()` so a failed
  commit still reports -1. Verified at the instruction level in both slices:
  arm64 `str x8,[x19]` at 0x314 precedes the `blr` Active call at 0x324
  (upstream: `blr` at 0x300 before `str` at 0x334); arm64e `str` at 0x3a8
  before `blraa` at 0x3d4 (upstream: `blraa` at 0x394 before `str` at 0x3c4).

Rebuild/verify: `tools/dependencies/rebuild-dobby.sh --check` verifies the pinned source
archive, machine-applicable patch, checked-in archive SHA-256, slices, member
count, arm64e ABI, and required exports. `--rebuild` requires explicit
`DOBBY_SDK` and `DOBBY_TOOLCHAIN` paths and writes an ignored candidate under
`.theos/dobby-rebuild/`; it never replaces the checked-in archive. The lock
records iOS 14.0 for both arm64 and arm64e.

### gum (wrapper only; devkit binary is pristine)

- `a99f14d` (2026-08-10) — `hkgum_begin_transaction`/`hkgum_end_transaction`
  return `void`: frida-gum 17.17's transaction API reports no failure, so the
  previous `int` wrappers faked a failure channel. Only `hkgum.c` is local;
  `tools/dependencies/fetch-gum.sh` obtains `frida-gum.h` and `libfrida-gum.a` from the
  unmodified, checksum-pinned devkits when a modern package is built.

Rebuild: none — `tools/dependencies/fetch-gum.sh` downloads the two Frida 17.17.0
devkits, verifies their SHA-256 values from `gum.lock`, and lipo-merges their
static libraries; `hkgum.c` is compiled by the Makefile (`HKGum` product).

### libhooker (header only)

Base https://github.com/coolstar/libhooker, master. The in-tree header
predates the 1.6.9 OSS release (commit `4f85a68dae`, 2023-04-17); it is
unchanged since HookKit's initial commit 75cdb22. Deltas vs master:

- `libhooker.h`: +12 lines of local doc-comment paragraphs.

Rebuild: none — header only.

### substitute (header only)

Base https://github.com/comex/substitute, master; header frozen since
`83442f9005` (2015-07-16). No v2 git tag exists upstream (honest pin: master
+ 2 local deltas):

- `__has_include("substitute-internal.h")` seam added at the top.
- `void *dlhandle;` removed from `struct substitute_image`.

License: public domain / CC0 1.0 per the header comment. Upstream has a
`LICENSE.txt`: its repository-wide default is LGPL-2.1-or-later, but it
expressly places `substitute.h` and generated files in the public domain. The
file is not vendored because HookKit copies only this header.

Rebuild: none — header only.

### Runtime provider contract audit (source only)

Audited 2026-08-15 against exact upstream snapshots; these sources are not
vendored or linked into HookKit:

- ElleKit `1017a0d09606`; the same ABI and symlink layout were checked across
  releases `v0.4.3`, `v0.6.3`, `v1.0`, and `v1.1.3` (`24033cc3cfa7`):
  `LBHookMessage` is void, `LHHookFunctions` and `LHPatchMemory` return zero
  on success, originals are written after the patch call, and the package
  symlinks both `libhooker.dylib` and `libblackjack.dylib` to
  `libellekit.dylib`.
- libhooker 1.6.9 OSS `4f85a68daeba`: the build produces separate libhooker
  and libblackjack dylibs; the batch APIs continue after individual failures
  and return only a success count, while function originals are created before
  the shadow-page commit.
- sbingner/ substitute `211873b3c184`: the native
  `substitute_find_private_syms` out-array already contains final pointers.
  `substitute_sym_to_ptr` is declared in the public header but has no exported
  implementation, so it is not part of HookKit's native-API availability
  test.

This validates adapter signatures, return conventions, and publication order
only. An installed jailbreak package may come from a different commit; PAC,
dyld, page-protection, and injection behavior remains device-unverified.

## Rebuild commands

- dobby: run `bash tools/dependencies/rebuild-dobby.sh --check` before release. To build
  a reviewed candidate, set `DOBBY_SDK` and `DOBBY_TOOLCHAIN`, then run
  `bash tools/dependencies/rebuild-dobby.sh --rebuild`. Replacing `libdobby.a` requires
  a deliberate SHA update in `dobby.lock`.
- gum: none — run `bash tools/dependencies/fetch-gum.sh`; it downloads the two
  Frida 17.17.0 devkits recorded in `vendor/gum/gum.lock`, verifies them,
  and lipo-merges the two static-library slices. Update the lock together
  with a Frida version bump.

## Known provenance gaps (honest remainder)

1. substitute: no version tag upstream — pinned to master @ `83442f9005`
   (frozen 2015-07-16) with 2 documented deltas.
2. libhooker: vendored header predates the 1.6.9 OSS release (`4f85a68dae`);
   the exact upstream commit it was copied from is not recorded.
