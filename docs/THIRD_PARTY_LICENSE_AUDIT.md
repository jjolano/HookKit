# Third-party provenance and license audit

**Audited:** 2026-08-28
**Scope:** the committed vendored tree at `64636d5`, the current Makefile, and
the current Theos staging tree. This is a factual provenance and distribution
review, **not legal advice** or a legal-compliance opinion.

## Method

- Compared local license files and headers with the recorded upstream revision.
- Resolved short pins against the upstream GitHub API and inspected local diffs.
- Verified Frida's two release-asset SHA-256 values against GitHub's release
  manifest, then unpacked both devkits in temporary storage.
- Reviewed the release layout installed by the required base package at
  `usr/share/doc/hookkit` (under `var/jb` for rootless), including the
  version-pinned Frida component inventory.

## What is actually shipped

| Component | Current production role | Included in a package? |
| --- | --- | --- |
| Dobby | Statically linked into `HookKit.framework` on modern lanes | Yes: rootful-modern, rootless, roothide |
| Frida Gum | Statically linked into `HKGum.dylib` | Yes: rootful-modern, rootless, roothide |
| fishhook | Not listed in `HookKit_FILES` and not included anywhere else | No; source tree only |
| LiteHook | Not listed in `HookKit_FILES`; its Apple fixup header is used only by a host test | No; source/test tree only |
| libhooker | Header declarations for a dynamically discovered device provider | No libhooker code or header is packaged |
| Substitute | Header declarations for a dynamically discovered device provider | No Substitute code or header is packaged |
| Substrate | No current include or build reference | No; source tree only |

The consequence is important: only Dobby and Frida Gum create a current
third-party binary-distribution obligation for modern HookKit packages. The
other copies still matter for source-repository distribution and future use.

## Per-component findings

### Dobby — Apache-2.0; provenance locked

- The recorded pin resolves to upstream
  [`5dfc8546954ce3b3198132ab13fddb89ee92cdd7`](https://github.com/jmpews/Dobby/commit/5dfc8546954ce3b3198132ab13fddb89ee92cdd7).
  `vendor/dobby/LICENSE` is byte-identical to that revision's
  [Apache-2.0 license](https://github.com/jmpews/Dobby/blob/5dfc8546954ce3b3198132ab13fddb89ee92cdd7/LICENSE),
  and `dobby.h` differs only by HookKit's provenance comment. The upstream tree
  has no `NOTICE` file.
- `libdobby.a` is a locally rebuilt, modified binary. The exact source archive
  SHA-256, checked-in archive SHA-256, expected slices/member count, and
  source patch now live in `vendor/dobby/dobby.lock` and
  `vendor/dobby/patches/0001-publish-original-before-activation.patch`.
  `scripts/rebuild-dobby.sh --check` verifies all of them without rewriting
  the vendor archive.
- Apache-2.0 section 4 requires a license copy for recipients and prominent
  change notices in modified source that is distributed. See the
  [license text](https://www.apache.org/licenses/LICENSE-2.0.html).

**Technical remediation:** the Apache-2.0 text and the machine-applicable
patch/lock are now shipped or retained as above.

### Frida Gum — top-level license and composite-devkit notices installed

- `vendor/gum/COPYING` is byte-identical to Frida Gum
  [`17.17.0`'s COPYING](https://github.com/frida/frida-gum/blob/17.17.0/COPYING),
  which is wxWindows Library Licence 3.1. Its text references the GNU Library
  GPL v2-or-later and includes the wxWindows binary-object-code exception;
  avoid reducing that combination to a generic LGPL label. Frida documents its
  devkits as static-linking inputs in its
  [Gum README](https://github.com/frida/frida-gum#binaries).
- Both lockfile URLs and checksums match the official Frida `17.17.0` release:
  arm64 `559f62d2…c35cdb` and arm64e `f3c2fe3c…28d29f`. The devkit archives
  contain only `libfrida-gum.a`, `frida-gum.h`, and an example source file—no
  license bundle.
- The archive is not just Gum objects: its members include Capstone,
  liblzma/XZ, GLib/GObject/GIO, and other objects. The resulting universal
  `HKGum.dylib` in the current staging tree is about 17 MB. The vendored
  top-level `COPYING` is therefore not a verified complete notice inventory
  for the binary.

**Technical remediation:** `layout/usr/share/doc/hookkit/` now supplies a
version-pinned Frida devkit SBOM and the identified full license texts in the
base package. Every optional Gum package has an exact dependency on that base,
so the notices are installed without file ownership conflicts. This remains a
factual inventory, not a legal compatibility conclusion.

### fishhook — BSD-3-Clause; source-only, notices preserved

- The recorded pin resolves to
  [`aadc161ac3b80db07a9908851839a17ba63a9eb1`](https://github.com/facebook/fishhook/commit/aadc161ac3b80db07a9908851839a17ba63a9eb1).
  The local `LICENSE` is byte-identical to upstream's
  [BSD-3-Clause license](https://github.com/facebook/fishhook/blob/aadc161ac3b80db07a9908851839a17ba63a9eb1/LICENSE).
- The heavily modified `fishhook.c` and `fishhook.h` retain the upstream
  copyright and BSD notice; local changes begin after that notice.
- It is not compiled or packaged by the current Makefile.

**Source action:** no missing upstream notice was found. Keep the license with
the source, or remove this unused fork if retaining it has no near-term value.

### LiteHook and Apple headers — MIT plus APSL-2.0; source-only

- The recorded pin resolves to
  [`cb5c5a39f736b367e72ced1aa0bfeb79a8be269e`](https://github.com/opa334/litehook/commit/cb5c5a39f736b367e72ced1aa0bfeb79a8be269e).
  `vendor/litehook/LICENSE` is byte-identical to upstream's
  [MIT license](https://github.com/opa334/litehook/blob/cb5c5a39f736b367e72ced1aa0bfeb79a8be269e/LICENSE).
- `litehook.c` and `litehook.h` contain documented local changes. The two
  imported Apple headers, `dyld_cache_format.h` and `fixup-chains.h`, are
  byte-identical to the upstream copies and retain their APSL-2.0 headers.
- This is not MIT-only material: the Apple header notices point to the
  [Apple Public Source License 2.0](https://github.com/apple-oss-distributions/dyld/blob/main/APPLE_LICENSE).
  The license's sections 2.1–2.3 impose source, modified-code, and object-code
  conditions; the Apple headers themselves are unmodified.
- No LiteHook source is part of the shipped framework. The Apple fixup header
  is currently compiled only by `Tests/Host/test_chained_fixups.c`.

**Technical remediation:** `vendor/litehook/APSL-2.0.txt` now retains the
full APSL-2.0 text with the source-only headers.

### libhooker — BSD-4-Clause; content baseline verified, original copy pin absent

- The documented OSS 1.6.9 revision resolves to
  [`4f85a68daebaf7456c66e1f55184dca118022397`](https://github.com/coolstar/libhooker/commit/4f85a68daebaf7456c66e1f55184dca118022397).
  The local `LICENSE` is byte-identical to the license at that revision and
  current upstream master.
- Relative to 1.6.9, `libhooker.h` has only the two documented comment blocks;
  `libblackjack.h` has only the documented include/comment deltas. The original
  copy commit remains unrecorded, so this is a verified comparison baseline,
  not proof of the original import event.
- The bundled license contains the BSD advertising clause. HookKit dynamically loads a
  device-provided libhooker/ElleKit implementation; it does not ship that
  implementation or these headers in its package.

**Source action:** preserve the license and record an immutable comparison pin
when the next vendor refresh occurs.

### Substitute — public-domain/CC0 header; inventory corrected

- The exact upstream commit is
  [`83442f9005c21de839b5ed69bd3e0b4e59032020`](https://github.com/comex/substitute/commit/83442f9005c21de839b5ed69bd3e0b4e59032020).
  The local header differs only by the two documented HookKit changes.
- Upstream does have
  [`LICENSE.txt`](https://github.com/comex/substitute/blob/83442f9005c21de839b5ed69bd3e0b4e59032020/LICENSE.txt).
  It places most of the project under LGPL-2.1-or-later, but explicitly says
  `substitute.h` and generated files are public domain. The header itself also
  declares public-domain/CC0 treatment.
- HookKit copies only that header and dynamically discovers Substitute on a
  device; it does not ship Substitute's implementation.

**Source action:** the result for the vendored header remains public
domain/CC0, but `vendor/VENDORED.md` must not say that upstream has no license
file. This audit corrects that statement.

### Substrate — BSD-3-Clause header; canonical origin remains unverified

- `vendor/substrate/substrate.h` is byte-identical to the recorded Dopamine
  mirror at
  [`e89072adc591881146c9513a616fa68b7323d6a7`](https://github.com/opa334/Dopamine/blob/e89072adc591881146c9513a616fa68b7323d6a7/BaseBin/_external/include/substrate.h).
  It retains Jay Freeman's three-clause BSD notice.
- The reported canonical GitHub repositories `saurik/substrate` and
  `saurik/CydiaSubstrate` currently return 404. A mirror is useful for content
  identity but is not proof of original upstream provenance.
- The header has no current include or build reference.

**Source action:** keep the BSD notice if retained; otherwise remove this
unused header. Do not describe the mirror as canonical upstream.

## Release decision

The technical release blockers identified by this audit are implemented:

1. Every base package installs a fixed, verified notice tree; each optional
   Gum package depends on that exact base rather than duplicating its files.
2. Modern builds emit a base package plus a separate manual-opt-in Gum
   package; the base framework does not contain Gum.
3. Dobby has a retained source patch, lockfile, and reproducible verification
   path.

This audit remains factual provenance work, not legal advice or a legal
compatibility opinion. A maintainer may still choose independent legal review
before publishing. Removing unused source-only vendor copies remains a future
size and licensing-surface reduction, not a release prerequisite.
