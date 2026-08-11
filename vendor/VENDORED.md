# Vendored components

Everything under `vendor/` is third-party code or headers checked into this
repository. This file records, per component, the upstream source, license,
local modifications, and the gaps where provenance is unknown. All upstream
pins below were byte-verified against upstream by the provenance research
lane (2026-08-11); discrepancies from the pre-verification state are noted
where relevant.

## Inventory

| Directory | Contents | Upstream | Version/SHA | License | Local patches |
|---|---|---|---|---|---|
| `vendor/fishhook/` | fishhook.c, fishhook.h | https://github.com/facebook/fishhook | `aadc161ac3b80db07a9908851839a17ba63a9eb1` (2021-10-12); base byte-identical to that commit, recorded in commit 51e1c7b | BSD-3-Clause (header comment, 2013 Facebook) | yes — heavy fork, see below |
| `vendor/litehook/` | litehook.c, litehook.h, dyld_cache_format.h, fixup-chains.h, LICENSE | https://github.com/opa334/litehook | `cb5c5a39f736b367e72ced1aa0bfeb79a8be269e` (main, 2026-07-31); vendored copies byte-identical to pristine upstream at that commit (verified against every upstream commit) | MIT (LICENSE, Lars Fröder 2022-2024); dyld_cache_format.h carries Apple APSL 2.0 header | yes — see below |
| `vendor/dobby/` | dobby.h, libdobby.a, LICENSE | https://github.com/jmpews/Dobby | `5dfc8546954ce3b3198132ab13fddb89ee92cdd7` (2024-03-14); release "latest" ships dobby-iphoneos-all.tar.gz (URL in dobby.h comment); in-tree dobby.h = upstream include/dobby.h + one provenance comment line, otherwise byte-identical | Apache-2.0 (LICENSE vendored) | yes — binary rebuilt from source with one reorder patch (publication-before-activation), see below; dobby.h unchanged |
| `vendor/gum/` | frida-gum.h, libfrida-gum.a, hkgum.c, COPYING | https://github.com/frida/frida-gum | tag 17.17.0 = `ddc10c5559cbb41a3dd72866bfba6ff3945ffa5c`; devkit tarballs from frida/frida release 17.17.0 (2026-08-05); frida-gum.h byte-identical to devkit; libfrida-gum.a = lipo of both official devkit slices (SHA-256 verified per slice) | wxWindows Library Licence 3.1 (LGPL-2.1 + wxWindows exception; COPYING vendored) | hkgum.c wrapper only — see below |
| `vendor/libhooker/` | libhooker.h, libblackjack.h, LICENSE | https://github.com/coolstar/libhooker | master; only milestone is OSS 1.6.9 commit `4f85a68dae` (2023-04-17); in-tree headers predate that release (unchanged since HookKit's initial commit 75cdb22) | BSD-4-Clause (LICENSE vendored) | small header deltas — see below |
| `vendor/substitute/` | substitute.h | https://github.com/comex/substitute | master; header frozen since `83442f9005` (2015-07-16); no v2 git tag exists upstream | public domain / CC0 1.0 (header comment; upstream has no LICENSE file — fetch of master/LICENSE 404s) | 2 small deltas — see below |
| `vendor/substrate/` | substrate.h | no canonical upstream repo (saurik/substrate and saurik/CydiaSubstrate 404; newest mobilesubstrate deb is 0.9.6301, 2017) | 0.9.7101-era header (copyright 2008-2019 saurik), byte-identical to `https://github.com/opa334/Dopamine` BaseBin `_external/include/substrate.h` @ `e89072adc591881146c9513a616fa68b7323d6a7` — pin = that mirror commit | 3-clause BSD (header text) | none (header only) |

## Local patches

### fishhook (heavily modified fork)

Base facebook/fishhook @ `aadc161ac3b80db07a9908851839a17ba63a9eb1` (2021-10-12),
recorded in commit 51e1c7b and byte-verified. Fork additions postdate the
fork:

- `__AUTH_CONST` scanning.
- matched/failed rebind stats.
- publish-callback API (`rebind_symbols_hook`).
- recursive-mutex + add-image callback re-registration guard.
- `18a29b7` (2026-08-11) — drop `dladdr()` header validation in the rebind
  scan: the call is dead weight (the `Dl_info` is never used) and a
  self-hosting hazard — once this library's own `dladdr` slot is rebound, the
  validation re-enters the replacement and can jump through a still-NULL
  original (PC=0 SIGSEGV, observed on-device).
- `a066270` (2026-08-07) — arm64e PAC fix, no-op reporting.
- `5b9d973` (2026-08-07) — symbol matching and batching kind guards.

Rebuild: none; compiled from source by the Makefile (`HookKit_FILES`).

### litehook (heavily modified fork)

Base https://github.com/opa334/litehook @
`cb5c5a39f736b367e72ced1aa0bfeb79a8be269e` (main, 2026-07-31). The vendored
litehook.c/.h, dyld_cache_format.h, fixup-chains.h and LICENSE were
byte-identical to pristine upstream at that commit (verified against every
upstream commit). Committed local patches:

- `8c267fd` (2026-08-09) — `litehook_rebind_symbol` commits the global rebind
  record only on first match; reports `HK_ERR_NOT_SUPPORTED` on zero match
  (upstream appended the record unconditionally).
- `d172b0f` (2026-08-09) — hardening: protection restore, locked rebinds,
  32-bit strategy gates.
- `a99f14d` (2026-08-10) — crash prevention hardening (~160 lines).
- `cfc736d` (2026-08-10) — off-by-one fix in rebind record append.

Plus the current uncommitted working-tree DSC-parser removal (see In-flight
below): `litehook_locate_dsc` / `litehook_find_dsc_symbol` stubbed out,
replaced by `native/hk_symbols.c`.

Rebuild: none; compiled from source by the Makefile.

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

Rebuild: `libdobby.a` from source (arm64 + arm64e slices, theos clang
13.0.0) — see Rebuild commands below.

### gum (wrapper only; devkit binary is pristine)

- `a99f14d` (2026-08-10) — `hkgum_begin_transaction`/`hkgum_end_transaction`
  return `void`: frida-gum 17.17's transaction API reports no failure, so the
  previous `int` wrappers faked a failure channel. Only `hkgum.c` is local;
  `frida-gum.h` and `libfrida-gum.a` are unmodified devkit output (verified).

Rebuild: none — devkit static lib from the frida 17.17.0 release; `hkgum.c`
is compiled by the Makefile (`HKGum` product).

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

License: public domain / CC0 1.0 per the header comment. No LICENSE file is
vendored — upstream has none (fetch of master/LICENSE 404s).

Rebuild: none — header only.

### substrate (header only)

No canonical upstream repo exists (saurik/substrate and saurik/CydiaSubstrate
404; the newest mobilesubstrate deb is 0.9.6301 from 2017). The in-tree
header is 0.9.7101-era (copyright 2008-2019 saurik), byte-identical to the
Dopamine BaseBin mirror commit `e89072adc591881146c9513a616fa68b7323d6a7`.
License: 3-clause BSD per the header text. Rebuild: none — header only.

## In-flight (uncommitted, concurrent vendor lane, 2026-08-11)

Beyond the committed patches above, the working tree carries further local
modifications not yet in git history:

- fishhook: protection-restore and rebind-stats work, plus the
  publish-callback API (`rebind_symbols_hook`).
- litehook: the DSC-parser removal is **confirmed stubbed** in the working
  tree — `litehook_locate_dsc` / `litehook_find_dsc_symbol` return NULL/0,
  replaced by `native/hk_symbols.c`.

Update this file when that lane lands.

## Rebuild commands

- fishhook, litehook: none — built from source by `make`.
- dobby: rebuilt from source (2026-08-11) at upstream commit `5dfc854` with the
  publication-before-activation patch (see Local patches). Commands, per arch
  (`SDK=$HOME/theos/sdks/iPhoneOS16.5.sdk`, `TC=$HOME/theos/toolchain/linux/iphone/bin`,
  `$ARCH`/`$MIN` = `arm64`/`9.3` and `arm64e`/`14.0`):

  ```
  cmake -S . -B build-$ARCH -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 \
    -DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_OSX_SYSROOT=$SDK \
    -DCMAKE_C_COMPILER=$TC/clang -DCMAKE_CXX_COMPILER=$TC/clang++ -DCMAKE_ASM_COMPILER=$TC/clang \
    -DCMAKE_AR=$TC/ar -DCMAKE_RANLIB=$TC/ranlib -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release -DDOBBY_BUILD_EXAMPLE=OFF -DDOBBY_BUILD_TEST=OFF \
    "-DCMAKE_C_FLAGS=-target $ARCH-apple-ios$MIN -isysroot $SDK" \
    "-DCMAKE_CXX_FLAGS=-target $ARCH-apple-ios$MIN -isysroot $SDK" \
    "-DCMAKE_ASM_FLAGS=-target $ARCH-apple-ios$MIN -isysroot $SDK -x assembler-with-cpp"
  cmake --build build-$ARCH --target dobby_static -j8
  lipo -create build-arm64/libdobby.a build-arm64e/libdobby.a -output libdobby.a
  ```

  Re-fetch from https://github.com/jmpews/Dobby/releases only to re-base the
  patch on a newer upstream commit.
- gum: none — `libfrida-gum.a`/`frida-gum.h` from the frida 17.17.0 devkit;
  re-fetch from
  https://github.com/frida/frida/releases/download/17.17.0/frida-gum-devkit-17.17.0-ios-arm64.tar.xz
  and `...-ios-arm64e.tar.xz` (lipo the two slices) to update.

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
