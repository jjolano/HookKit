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
| `vendor/fishhook/` | fishhook.c, fishhook.h | https://github.com/facebook/fishhook | `aadc161ac3b80db07a9908851839a17ba63a9eb1` (2021-10-12); base byte-identical to that commit, recorded in commit 51e1c7b | BSD-3-Clause (header comment, 2013 Facebook) | yes — heavy fork, see below |
| `vendor/litehook/` | litehook.c, litehook.h, dyld_cache_format.h, fixup-chains.h, LICENSE, APSL-2.0.txt | https://github.com/opa334/litehook | `cb5c5a39f736b367e72ced1aa0bfeb79a8be269e` (main, 2026-07-31); vendored copies byte-identical to pristine upstream at that commit (verified against every upstream commit) | MIT (LICENSE, Lars Fröder 2022-2024); dyld_cache_format.h carries Apple APSL 2.0 header | yes — see below |
| `vendor/dobby/` | dobby.h, libdobby.a, LICENSE, dobby.lock, patches/0001-publish-original-before-activation.patch | https://github.com/jmpews/Dobby | `5dfc8546954ce3b3198132ab13fddb89ee92cdd7` (2024-03-14); lock records source and archive SHA-256 values | Apache-2.0 (LICENSE vendored) | yes — binary rebuilt from source with one reorder patch (publication-before-activation), see below; dobby.h unchanged |
| `vendor/gum/` | hkgum.c, COPYING, gum.lock (generated devkit ignored) | https://github.com/frida/frida-gum | tag 17.17.0 = `ddc10c5559cbb41a3dd72866bfba6ff3945ffa5c`; official arm64/arm64e devkits are pinned by URL and SHA-256 in gum.lock | wxWindows Library Licence 3.1 (`COPYING` vendored; GNU Library GPL v2-or-later text plus wxWindows exception) | hkgum.c wrapper only — see below |
| `vendor/libhooker/` | libhooker.h, libblackjack.h, LICENSE | https://github.com/coolstar/libhooker | master; only milestone is OSS 1.6.9 commit `4f85a68dae` (2023-04-17); in-tree headers predate that release (unchanged since HookKit's initial commit 75cdb22) | BSD-4-Clause (LICENSE vendored) | small header deltas — see below |
| `vendor/substitute/` | substitute.h | https://github.com/comex/substitute | master; header frozen since `83442f9005` (2015-07-16); no v2 git tag exists upstream | public domain / CC0 1.0 for this header (header comment; upstream `LICENSE.txt` expressly places substitute.h and generated files in the public domain) | 2 small deltas — see below |
| `vendor/substrate/` | substrate.h | no canonical upstream repo (saurik/substrate and saurik/CydiaSubstrate 404; newest mobilesubstrate deb is 0.9.6301, 2017) | 0.9.7101-era header (copyright 2008-2019 saurik), byte-identical to `https://github.com/opa334/Dopamine` BaseBin `_external/include/substrate.h` @ `e89072adc591881146c9513a616fa68b7323d6a7` — pin = that mirror commit | 3-clause BSD (header text) | none (header only) |

## Local patches

### fishhook (heavily modified fork)

Base facebook/fishhook @ `aadc161ac3b80db07a9908851839a17ba63a9eb1` (2021-10-12),
recorded in commit 51e1c7b and byte-verified. Fork additions postdate the
fork:

- `__AUTH_CONST` scanning.
- matched/failed rebind stats.
- side-effect-free import-slot preflight and batch no-op pruning.
- publish-callback API (`rebind_symbols_hook`).
- recursive-mutex + add-image callback re-registration guard.
- `18a29b7` (2026-08-11) — drop `dladdr()` header validation in the rebind
  scan: the call is dead weight (the `Dl_info` is never used) and a
  self-hosting hazard — once this library's own `dladdr` slot is rebound, the
  validation re-enters the replacement and can jump through a still-NULL
  original (PC=0 SIGSEGV, observed on-device).
- `a066270` (2026-08-07) — arm64e PAC fix, no-op reporting.
- `5b9d973` (2026-08-07) — symbol matching and batching kind guards.

Rebuild: none; retained source-only and not compiled or packaged by the
current Makefile.

### litehook (heavily modified fork)

Base https://github.com/opa334/litehook @
`cb5c5a39f736b367e72ced1aa0bfeb79a8be269e` (main, 2026-07-31). The vendored
litehook.c/.h, dyld_cache_format.h, fixup-chains.h and LICENSE were
byte-identical to pristine upstream at that commit (verified against every
upstream commit). Committed local patches:

- `8c267fd` (2026-08-09) — `litehook_rebind_symbol` commits the global rebind
  record only on first match and reports a clean unavailable result on zero
  matches (upstream appended the record unconditionally).
- side-effect-free address-slot preflight used by automatic backend routing.
- `d172b0f` (2026-08-09) — hardening: protection restore, locked rebinds,
  32-bit strategy gates.
- `a99f14d` (2026-08-10) — crash prevention hardening (~160 lines).
- `cfc736d` (2026-08-10) — off-by-one fix in rebind record append.

- `fbe2235` (2026-08-11) — removed the DSC parser by stubbing
  `litehook_locate_dsc` / `litehook_find_dsc_symbol`; `src/native/hk_symbols.c`
  replaces it.

Rebuild: none; retained source-only and not compiled or packaged by the
current Makefile.

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

### libhooker (headers only)

Base https://github.com/coolstar/libhooker, master. The in-tree headers
predate the 1.6.9 OSS release (commit `4f85a68dae`, 2023-04-17); they are
unchanged since HookKit's initial commit 75cdb22. Deltas vs master:

- `libhooker.h`: +12 lines of local doc-comment paragraphs.
- `libblackjack.h`: `#include <objc/objc.h>` removed; `@param class` vs
  upstream's `@param objcClass`.

Rebuild: none — headers only.

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

### substrate (header only)

No canonical upstream repo exists (saurik/substrate and saurik/CydiaSubstrate
404; the newest mobilesubstrate deb is 0.9.6301 from 2017). The in-tree
header is 0.9.7101-era (copyright 2008-2019 saurik), byte-identical to the
Dopamine BaseBin mirror commit `e89072adc591881146c9513a616fa68b7323d6a7`.
License: 3-clause BSD per the header text. Rebuild: none — header only.

## Rebuild commands

- fishhook, litehook: source-only; no release build or package consumes them.
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
2. substrate: no canonical repo — pinned to the opa334/Dopamine BaseBin
   mirror commit `e89072adc591881146c9513a616fa68b7323d6a7`.
3. libhooker: vendored headers predate the 1.6.9 OSS release (`4f85a68dae`);
   the exact upstream commit they were copied from is not recorded.
4. fishhook: base SHA is recorded only in commit 51e1c7b's message, not
   in-tree — now byte-verified, but a tree-local pin file would survive
   history rewrites.
