# HookKit

A slim iOS developer framework that unifies nine hooking backends behind one API.

## Backends

Objective-C messages always use HookKit's runtime module; they do not select or dispatch through a backend. The implicit default routes functions through `FUNCTION_INLINE` then `FUNCTION_REBIND`, and memory patches through capable backend descriptors. A route that returns the side-effect-free `HK_ERR_NOT_SUPPORTED` result falls through to the next candidate; hard errors and partial installs stop. Modern core packages statically link Dobby on arm64/arm64e. Frida Gum is a separate, manual-opt-in provider package; fishhook and LiteHook remain source-only compatibility material.

### Selection

Ask the runtime which backend IDs are currently available, then select an
ordered subset for one substitutor. The selection is strict for its function
and memory hooks: an unsupported target returns `HK_ERR_NOT_SUPPORTED` rather
than falling through to an unselected backend. Objective-C message hooks
remain facade-native. No backend plist is read.

```objc
NSArray<NSString *> *ids = [HKSubstitutor getAvailableBackendIDs];
HKSubstitutor *sub = [HKSubstitutor substitutorWithBackendIDs:@[@"provider-ellekit"]];
```

### Capability matrix

Message hooking is facade-native and available independently of this matrix.

| Backend         | Function | Memory | Native batch° |
|-----------------|----------|--------|----------|
| ElleKit         | yes†     | yes    | no       |
| Cydia Substrate | yes      | yes‡   | no       |
| Substitute      | yes      | yes‡   | no       |
| native          | yes†     | yes    | no       |
| Dobby           | yes†     | yes    | no       |
| Frida           | yes†     | no     | yes      |
| rebind (`fishhook` compatibility ID) | yes¶ | no | yes |
| Swift           | no*      | no     | no       |

† Function hooking is arm64/arm64e only — these backends report unavailable on armv7 (see the native, Dobby, and Frida caveats).

‡ Via `MSHookMemory` when the installed Cydia Substrate exports it, and via the MS-compatible `SubHookMemory` shim on Substitute — unavailable when the installed library doesn't export it (see the memory caveat).

¶ Imported/exported C symbols only, rebound by symbol name. `fishhook` is the
legacy compatibility ID for HookKit's built-in rebind engine.

\* Swift vtable hooking is a separate API — `hookSwiftMethodInClass:withName:...` / `hookSwiftVtableSlotInClass:withIndex:...` — not the message/function columns (see the Swift caveat).

° While batching is on, every supported hook kind is queued until `executeHooks`. The built-in rebind engine applies function batches in one image walk; Frida publishes them in one interceptor transaction.

### native

HookKit's own backend, requiring no hooking library to be installed on the device. It implements inline function hooking with an ARM64 instruction relocator, memory patching, and symbol lookup that reads private symbols out of the dyld shared cache's local symbol table.

It is not the pinned default, but the implicit memory router can reach it after provider backends decline. Name it explicitly when you need its function or symbol engine.

Constraints: arm64/arm64e only — on armv7 it reports unavailable, since those devices always have Substrate. Inline patching needs relaxed codesigning, which holds in a tweak-injected process but not in an unmodified one. Function and memory patching require the main thread — the engine does not suspend peer threads, so off the main thread they return `HK_ERR_NOT_SUPPORTED` without patching. Targets too short to patch without clobbering the function that follows them are refused with `HK_ERR_NOT_SUPPORTED` (engine `SHORT_FUNCTION`, mapped alongside `UNSUPPORTED` and `RELOCATE`), nothing written. Shared cache symbol parsing depends on the cache layout and should be re-verified each major iOS release.

Hooks are still a load-time operation, but the engine no longer leans on that alone:

- **The entry patch is a single 4-byte `B`** whenever a trampoline page can be placed within ±128MB of the target, which the allocator searches for. One aligned store, so a thread *entering* the function while it is patched sees either the old first instruction or the new branch — never a half-written sequence. Out of range it degrades to the 16-byte `LDR`/`BR` form, which is torn-visible; the device smoke binary reports which form was used.
- **A thread already inside the prologue is still unrecoverable.** No non-quiescing inline hooker can fix that, and this one does not suspend peer threads.
- **Installing a hook cannot fault a hook that is already running.** Every trampoline gets its own page, so the read-write → read-execute seal at publish time only ever touches a page that has published nothing. The previous design bump-allocated trampolines out of one shared page and flipped that page to read-write to build each new one, stripping EXECUTE from up to 127 live trampolines for the duration. The cost is 16KB per native inline hook; the backend is opt-in and hooks arrive in handfuls, so a few hundred KB is the realistic worst case.
- **Writes to live mappings are serialized process-wide,** so two concurrent patches cannot race each other's protection flips (which previously left the second `memcpy` hitting a page the first had already resealed) or, on the remap path, silently discard one of the two.

What "load time" actually means here is stricter than "do not hook a function that is running": **do not hook a function whose 16KB page holds anything that is running.** `hk_write` obtains write access by flipping the target's page to read-write, which drops EXECUTE for the window, so an unrelated function sharing that page faults on instruction fetch just as surely as the target would. In a tweak that hooks several functions in one image — the normal case — those functions and their callers routinely share pages. This is inherent: on iOS there is no way to patch a file-backed code page without either a non-executable window (the protection flip) or an unmapped one (`vm_remap`), and a permanently read-write-execute mapping is refused at instruction fetch no matter what `vm_protect` returned.

Note the blast radius of `hookMemory:` on this backend when the protection flip is refused (arm64e under PPL): the fallback rebuilds the whole page and swaps the mapping with `vm_remap`, so the unit of change is a page, not the patch size. Fine for code patched at load time; the Swift engine deliberately does not use it (see below).

### Dobby

Vendored static library hooking inline by address, so interior/private C functions are hookable (beyond the rebind engine's imported-symbol limit). It is reachable explicitly (`HK_LIB_DOBBY` or `HK_CAT_FUNCTION_INLINE`) and by the implicit function/memory router after earlier candidates decline. arm64/arm64e only — on armv7 it reports unavailable. The legacy rootful lane also reports it unavailable because the vendored archive uses the new arm64e ABI and hard-imports post-iOS-9 private symbols. Inline patching needs relaxed codesigning (same constraint as native), and requires the main thread: Dobby patches a fixed prologue window with no atomicity and no peer-thread quiescing, so off the main thread its preflight and hook both return `HK_ERR_NOT_SUPPORTED` without writing anything, and an implicit route falls through to the next candidate. Memory patching is supported; no native batch primitive (while batching is on, hooks queue and drain one at a time at `executeHooks`).

### Frida

Hooks through the `HKGum.dylib` wrapper (Frida Gum 17.17.0, wxWindows Library Licence 3.1: GNU Library GPL v2-or-later text with the wxWindows exception) `dlopen`ed at runtime. The framework never links Gum directly. The wrapper ships only in the separate `me.jjolano.fmwk.hookkit.gum` package, which is a manual opt-in installed after the matching base HookKit package. It installs to `/usr/lib` on rootful/RootHide and `/var/jb/usr/lib` on rootless. The wrapper is arm64/arm64e only; batching uses Gum interceptor transactions, `executeHooks` publishes the batch atomically, and partial failures are not rolled back. No memory patching. It is explicitly selectable (`HK_LIB_FRIDA`) and is a late implicit inline fallback after ElleKit and Dobby.

The Gum package is intentionally absent from the legacy rootful lane. Its package dependency requires iOS 15 or newer in every modern lane, including modern rootful (whose core package remains iOS 14+). If it is installed or removed while a host process is running, restart that process: provider discovery is memoized. Inline patching needs relaxed codesigning (same constraint as native/Dobby). Hooks should still be installed at load time, but Frida is deliberately **not** main-thread gated the way native and Dobby are: Gum ships a production relocator and interceptor transactions, so an off-main-thread inline hook is better served by falling through to it than refused outright. In the implicit `FUNCTION_INLINE` order that is exactly what happens — ElleKit, then (off the main thread) straight past Dobby to Frida. The installed package notice tree includes the version-pinned [Frida Gum SBOM](layout/usr/share/doc/hookkit/Frida-Gum-17.17.0-SBOM.md) and license texts.

### fishhook and LiteHook

The vendored fishhook and LiteHook copies are source-only compatibility and
reference material. They are not compiled into `HookKit.framework`,
`HKGum.dylib`, or either release package. The legacy `fishhook` selector maps
to HookKit's built-in `rebind` engine; it does not load the vendored fishhook
source.

### auto-cover

`substitutorWithAutoCoverCategories:` routes each function hook to the first available backend in the given categories whose side-effect-free preflight accepts the target, instead of pinning one backend at init:

```objc
HKSubstitutor *sub = [HKSubstitutor substitutorWithAutoCoverCategories:@[@(HK_CAT_FUNCTION_INLINE), @(HK_CAT_FUNCTION_REBIND)]];
```

- Per hook, category pickers are walked in priority order. Preflight declines are skipped; an invoked backend returning `HK_ERR_NOT_SUPPORTED` is also skipped because that status guarantees nothing was written. `HK_ERR`, `HK_ERR_PARTIAL`, and success are terminal.
- Requesting `old_ptr` skips candidates that cannot publish the original before activation.
- Batched hooks retry in grouped waves by backend, hook kind, and technique, preserving native batching without mixing inline and rebind routes from the same backend class. Rebind-only batches first resolve at drain time. Image/symbol APIs use the pinned first category backend; `activeType` reflects that fallback, not each hook's winner.
- Coverage story: a function too small for an inline patch is often still rebindable through HookKit's built-in import-slot engine, which is why `[FUNCTION_INLINE, FUNCTION_REBIND]` is the recommended composition.

### Swift

HookKit's own Swift vtable engine, sharing the native backend's memory-patching machinery. It rewrites the target method's slot in the class metadata vtable, so Swift callers of the method dispatch to the replacement. Hooks are installed by method name (`$s...`/`_$s...` exact mangled match, or a case-sensitive substring of the demangled name; the match must be unique — ambiguity fails loudly with every candidate logged) or by declaration-order slot index (stable per build, survives symbol stripping). Current scope: the class's own methods only, non-generic classes, no resilient superclass, no async methods, no class methods, no extensions; `@objc dynamic` methods are hookable the same way (affects Swift callers only). The replacement must be a raw function pointer with the Swift calling convention (self in x20, heap context in x21 on arm64) — an `objc_msgSend`-convention IMP will misbehave.

The slot write is a genuinely single-copy-atomic aligned pointer store with no code patching, so no relaxed codesigning is required for the write itself, and no main-thread gate applies — readers dispatching through the class see either the old or the new pointer. It deliberately does not go through the memory-patch path: that path's arm64e fallback swaps the entire `__DATA_CONST` page mapping when the protection flip is refused, which is far too coarse for live class metadata. Where the flip is refused the Swift hook fails cleanly instead of remapping. Devirtualization and inlining can silently bypass vtable dispatch, and KVO-swizzled instances are unaffected. On arm64e the engine validates its signing recipe against the live slot before writing (PAC pre-write self-check): a mismatch fails the hook cleanly instead of corrupting memory. For stripped binaries, use the index API (declaration order, stable per build); name lookup cannot work without symbols. Swift 5 ABI (iOS 12.2+) and arm64/arm64e only. Opt-in (`HK_LIB_SWIFT`), separate API, no category membership.

### memory

`hookMemory:` works via `MSHookMemory` on Cydia Substrate — but only when the installed Substrate exports it (unc0ver-class Substrate does). On Substitute it works via the MS-compatible shim (`MSHookMemory`, which libsubstitute maps to `SubHookMemory`) — not via the native path, which has no separate memory hook; when the shim is absent, `hookMemory:` reports `HK_ERR_NOT_SUPPORTED`. Substitute function hooks use `substitute_hook_functions` when available, falling back to the MS-compatible path; neither path is used for raw-address `hookMemory:`.

## Usage

```objc
HKEnableBatching();

void *(*orig_malloc)(size_t);
hookkit_status_t queued = HKHookFunction((void *)&malloc, (void *)&my_malloc,
                                         (void **)&orig_malloc);
hookkit_status_t installed = HKExecuteBatch();

if (queued != HK_OK || installed != HK_OK) {
    // Disable or verify the dependent feature.
}
```

`HKHookMessage`, `HKHookMemory`, `HKOpenImage`, and `HKFindSymbol` macros are also available.

Without batching, a hook method returns its final status. With batching, `HK_OK` only means queued; `executeHooks` returns the aggregate result. On `HK_ERR_PARTIAL`, verify or disable dependent features. `activeType` does not prove installation.

`hookMessageInClass:` dispatches class-method-only selectors through the metaclass (`object_getClass()`), so class methods hook correctly regardless of selected backend; instance methods pass the class as-is.

`openImage:` returns an owned, single-threaded handle. Close it exactly once and do not use it after `closeImage:`.

## Semantics

Normative semantics — the symbol-name convention, status codes, the `HK_ERR_NOT_SUPPORTED` contract, the fail-closed prologue preflight, arch gates, `getLibErrno:`, threading, batch storage lifetime, and availability probing — are specified in the `HKSubstitutor` interface comment in `Headers/HookKit.h`, which is the source of truth. This section keeps only the backend-operational behavior not covered there:

- Publish-before-activation: originals are published into the caller's cell before a replacement becomes reachable — Cydia Substrate, Substitute, native, the built-in rebind engine, Dobby (the vendored lib is rebuilt from upstream with a publication reorder patch), and Frida all publish first. Backends that cannot guarantee this refuse a requested `old_ptr` with `HK_ERR_NOT_SUPPORTED` before anything is written; ElleKit is the current example because it writes the original into the out-cell only after the patch lands.
- Shared prologue preflight: inline-capable backends without a `preflightFunction:` of their own (ElleKit, Cydia Substrate, Substitute) are gated by a shared fixed-window prologue validator before dispatch — arm64/arm64e only; on ARMv7 the shared check is skipped, since Substrate and Substitute validate their own prologues there.
- Duplicate inline hooks: inline function hooks go through a process-wide ownership guard with explicit states — a duplicate of a PENDING hook (same address, same replacement, queued or in flight) is refused at hook time with `HK_ERR`; a duplicate of a TAINTED hook (a previous attempt failed mid-flight) is a hard `HK_ERR`; a same-hook re-hook against an INSTALLED entry returns `HK_OK`, reusing the guard's saved original (`HK_ERR` when an original was requested but none exists). Hooking the same address with a different replacement is refused with `HK_ERR_NOT_SUPPORTED`. The guard's reach is HookKit, not the process: it cannot see a prologue another tweak patched directly through Substrate, ElleKit or its own inline engine, and two inline writers on one prologue is a crash no library-local guard can prevent. It also holds 64 live entries; past that, further inline hooks are refused rather than installed unguarded.

## Building

Requires [Theos](https://theos.dev). Jailbreak-root path resolution is compile-time per scheme via theos's bundled libroot (auto-linked as `-lroot`): the rootful package keeps paths as-is, the rootless package resolves via libroot (`/var/jb`), and the roothide target uses libroothide's `jbroot()`. There is no runtime root-detection dependency.

Only the optional Gum package build fetches the pinned Frida Gum devkits,
validates their SHA-256 digests, and combines the arm64/arm64e archives
locally. The core package stays small and does not fetch Gum. The generated
header and archive are ignored by Git; `curl`, `tar` with xz support, and
Xcode's `lipo` are required. The legacy lane has no Gum package.

```sh
# macOS
OLDABI_DEVELOPER_DIR=/Applications/Xcode-11.7.app/Contents/Developer ./build.sh rootful-legacy
./build.sh rootful-modern
./build.sh rootless
```

Linux can build the old-ABI lane with the pinned pre-Clang-12 toolchain and
SDK instead of Xcode:

```sh
OLDABI_TMP=$(mktemp -d)
mkdir -p "$THEOS/toolchain/oldabi" "$THEOS/sdks"
curl -fL -o "$OLDABI_TMP/toolchain.tar.xz" https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-x86_64.tar.xz
echo 'a72a7a577e2fbe2838b6b5e9c72034fa7d114af96f0e1d4b016f18730ce4056e  '"$OLDABI_TMP/toolchain.tar.xz" | sha256sum -c -
tar -xJf "$OLDABI_TMP/toolchain.tar.xz" -C "$THEOS/toolchain/oldabi"
curl -fL -o "$OLDABI_TMP/iPhoneOS13.7.sdk.tar.xz" https://github.com/theos/sdks/releases/download/master-146e41f/iPhoneOS13.7.sdk.tar.xz
echo '661d1a8c518025f084d8c1e70dd8767581fb5730fb5950378f0915d840a7b5c3  '"$OLDABI_TMP/iPhoneOS13.7.sdk.tar.xz" | sha256sum -c -
tar -xJf "$OLDABI_TMP/iPhoneOS13.7.sdk.tar.xz" -C "$THEOS/sdks"
rm -rf "$OLDABI_TMP"
./build.sh rootful-legacy
```

| Lane | Core artifact | Optional Gum artifact | Package ID(s) | HookKit slices and minimum iOS |
| --- | --- | --- | --- | --- |
| `rootful-legacy` | `build/hookkit-rootful-legacy.deb` | none | `me.jjolano.fmwk.hookkit.legacy` | armv7 9.0, armv7s 9.0, arm64 9.0, old-ABI arm64e 12.0 |
| `rootful-modern` | `build/hookkit-rootful-modern.deb` | `build/hookkit-rootful-modern-gum.deb` | `me.jjolano.fmwk.hookkit`; `.gum` (`iphoneos-arm`) | arm64 14.0, new-ABI arm64e 14.0 |
| `rootless` | `build/hookkit-rootless.deb` | `build/hookkit-rootless-gum.deb` | `me.jjolano.fmwk.hookkit`; `.gum` (`iphoneos-arm64`) | arm64 15.0, arm64e 15.0 |
| `roothide` | `build/hookkit-roothide.deb` | `build/hookkit-roothide-gum.deb` | `me.jjolano.fmwk.hookkit`; `.gum` (`iphoneos-arm64e`) | arm64 15.0, arm64e 15.0 |

Modern rootful, rootless, and RootHide share the base package ID; APT selects them by package architecture. Install a Gum artifact manually only after the matching base package: it depends on that exact version and iOS 15+, and declares no automatic recommendation. Legacy rootful keeps the distinct `.legacy` ID, conflicts with and replaces the base package, provides it for consumers such as Shadow, and is constrained to firmware below iOS 14. The two rootful binaries must not be lipo-merged: old- and new-ABI arm64e share the same architecture name but are incompatible.

Every lane runs `make clean`, the existing host tests, package verification, and export verification in isolated staging. `scripts/check_compat.sh` verifies the core and, on modern lanes, the separate Gum package including its notice tree. `scripts/rebuild-dobby.sh --check` verifies Dobby's locked archive and source patch. Modern arm64e release builds require macOS with Xcode 12 or newer. The legacy lane defaults to `$THEOS/toolchain/oldabi/linux/iphone` and `$THEOS/sdks/iPhoneOS13.7.sdk`; `OLDABI_TOOLCHAIN` and `OLDABI_SDKS` remain optional overrides. RootHide requires the RootHide Theos fork. GitHub Actions builds the legacy lane on Linux and all modern lanes on macOS.

Provider signatures, return conventions, and original-publication order are source-audited against the exact snapshots in [`vendor/VENDORED.md`](vendor/VENDORED.md). This is still static coverage: PAC, dyld, shared-cache, page-protection, and injection behavior needs a real jailbroken device, external device lab, or beta-user report for each major iOS family.

## Advantages and Disadvantages

Advantages:

- Improved performance through use of batch hooking (if available).
- Ability to utilize different hooking libraries from your tweak. Shadow is the primary consumer and provides this functionality.

Disadvantages:

- Library-specific functionality is not implemented uniformly: the Substrate-compatible backends (Cydia Substrate, Substitute) offer no native batch primitive (their hooks still queue while batching is on), the rebind engine only hooks imported C symbols, and Frida has no memory patching. For automatic instances, use the final hook/batch status rather than `activeType` to determine installation success.
- Existing tweaks will need to be rewritten/recompiled to use HookKit.

## Credits

- [fishhook](https://github.com/facebook/fishhook)
- [Dobby](https://github.com/jmpews/Dobby)
- [frida-gum](https://github.com/frida/frida-gum)
- [libhooker](https://github.com/coolstar/libhooker) / [ElleKit](https://github.com/tealbathingsuit/ellekit)
- [litehook](https://github.com/opa334/litehook)
- [libroot](https://github.com/opa334/libroot) — rootless path resolution (bundled with theos)
- [Substitute](https://github.com/sbingner/substitute)
- [Cydia Substrate](http://www.cydiasubstrate.com)
- [apple/swift](https://github.com/apple/swift) — Swift 5 ABI documentation (`include/swift/ABI/Metadata.h`, `MetadataValues.h`, `stdlib/public/runtime/Metadata.cpp`, `lib/IRGen/GenMeta.cpp`), which the Swift vtable backend's offsets and pointer-authentication recipe were verified against
