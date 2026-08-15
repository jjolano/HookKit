# HookKit

A slim iOS developer framework that unifies nine hooking backends behind one API.

## Backends

Objective-C messages always use HookKit's runtime module; they do not select or dispatch through a backend. The implicit default routes functions through `FUNCTION_INLINE` then `FUNCTION_REBIND`, and memory patches through capable backend descriptors. A route that returns the side-effect-free `HK_ERR_NOT_SUPPORTED` result falls through to the next candidate; hard errors and partial installs stop. Explicit type/category constructors remain pinned unless the caller chooses `substitutorWithAutoCoverCategories:`. fishhook and litehook are compiled in on every arch, and Dobby on modern arm64/arm64e. Backends are not packaged separately: one package covers each supported rootful, rootless, or roothide profile described under "Building." (This replaces the v1 Modulous plugin-bundle architecture.)

### Selection

Backends are selected by name, by priority list, or by capability:

- `substitutorWithTypes:` names specific backends; the first available one wins in registry order and remains pinned. Dobby, `native`, Frida, Swift, and litehook are not selected by this explicit type API unless named; the implicit operation router may reach capable entries as described above.
- To override the priority order, pass an explicit list with `substitutorWithOrderedTypes:` — the first available entry wins:

```objc
HKSubstitutor *sub = [HKSubstitutor substitutorWithOrderedTypes:@[@(HK_LIB_SUBSTRATE), @(HK_LIB_FISHHOOK), @(HK_LIB_ELLEKIT)]];
```

- `substitutorWithCategory:` picks the first available backend in a backend-selected category's priority order. `HK_CAT_MESSAGE` is facade-native: it requires no backend, is always available, and produces an instance with `activeType == HK_LIB_NONE`. `substitutorWithOrderedCategories:` tries entries top to bottom, stopping at the first that resolves. Backend picker orders are:

  - `HK_CAT_FUNCTION_REBIND` — fishhook > litehook
  - `HK_CAT_FUNCTION_INLINE` — ElleKit > Dobby > Frida > litehook
  - `HK_CAT_PRIVATE_SYMBOL` — ElleKit > Cydia Substrate > Substitute > litehook

- The readonly `activeStrategy` property (one of `HKStrategyDefault`/`HKStrategyRebind`/`HKStrategyInline`/`HKStrategyPrivateSymbol`) reports how the winning backend resolves and applies function hooks — a hooking technique (rebind or inline, or the vendor default), or a resolution mode (`HKStrategyPrivateSymbol`: the backend locates the private/DSC symbol first, then hooks it via rebinding). It is the resolution result, not a request. One vendor can serve several categories: litehook covers rebind, inline, and private-symbol lookups. Swift vtables have no category (`HK_CAT_NONE`) — it is a separate API, not a message/function/memory engine.
- Resolution order: `orderedTypes` > `orderedCategories` > `types`/default. A requested-but-unavailable backend is **not** silently substituted: hook calls return `HK_ERR_NOT_SUPPORTED` and `activeType` is `HK_LIB_NONE`. `HK_CAT_MESSAGE` is the intentional exception: no backend is needed. Check `activeType` before relying on backend-specific operations.
- With no types set, `activeType` still reports the legacy pinned provider/fishhook backend used by image and symbol APIs. Function and memory hooks may select another backend per operation; `activeType` is not their winner.
- `getSubstitutorTypeInfo:` returns per-backend metadata including a `selectable` flag for settings-style pickers: substrate, substitute, and swift are excluded from user-facing selection.

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
| fishhook        | yes¶     | no     | yes      |
| Swift           | no*      | no     | no       |
| litehook        | yes§     | yes    | no       |

† Function hooking is arm64/arm64e only — these backends report unavailable on armv7 (see the native, Dobby, and Frida caveats).

‡ Via `MSHookMemory` when the installed Cydia Substrate exports it, and via the MS-compatible `SubHookMemory` shim on Substitute — unavailable when the installed library doesn't export it (see the memory caveat).

§ Exported-symbol or GOT-referenced C functions, rebinding by address; the inline variant (`HK_CAT_FUNCTION_INLINE`) has no original-call trampoline (see the litehook caveat).

¶ Exported symbols only, rebinding by symbol name (see the fishhook caveat).

\* Swift vtable hooking is a separate API — `hookSwiftMethodInClass:withName:...` / `hookSwiftVtableSlotInClass:withIndex:...` — not the message/function columns (see the Swift caveat).

° While batching is on, every supported hook kind is queued until `executeHooks`. fishhook applies function batches in one image walk; Frida publishes them in one interceptor transaction.

### native

HookKit's own backend, requiring no hooking library to be installed on the device. It implements inline function hooking with an ARM64 instruction relocator, memory patching, and symbol lookup that reads private symbols out of the dyld shared cache's local symbol table.

It is not the pinned default, but the implicit memory router can reach it after provider backends decline. Name it explicitly when you need its function or symbol engine.

Constraints: arm64/arm64e only — on armv7 it reports unavailable, since those devices always have Substrate. Inline patching needs relaxed codesigning, which holds in a tweak-injected process but not in an unmodified one. Function and memory patching require the main thread — the engine does not suspend peer threads, so off the main thread they return `HK_ERR_NOT_SUPPORTED` without patching. Targets too short to patch without clobbering the function that follows them are refused with `HK_ERR_NOT_SUPPORTED` (engine `SHORT_FUNCTION`, mapped alongside `UNSUPPORTED` and `RELOCATE`), nothing written. Hooks must be installed at load time: the patch is not atomic and so is not safe against code already running on another thread. Shared cache symbol parsing depends on the cache layout and should be re-verified each major iOS release.

### Dobby

Vendored static library hooking inline by address, so interior/private C functions are hookable (beyond fishhook's exported-symbols-only limit). It is reachable explicitly (`HK_LIB_DOBBY` or `HK_CAT_FUNCTION_INLINE`) and by the implicit function/memory router after earlier candidates decline. arm64/arm64e only — on armv7 it reports unavailable. The legacy rootful lane also reports it unavailable because the vendored archive uses the new arm64e ABI and hard-imports post-iOS-9 private symbols. Inline patching needs relaxed codesigning (same constraint as native). Memory patching is supported; no native batch primitive (while batching is on, hooks queue and drain one at a time at `executeHooks`).

### Frida

Hooks through the `HKGum.dylib` wrapper (frida-gum devkit, LGPL-2.1 with wxWindows exception) dlopen'd at runtime — the framework never links gum directly, keeping LGPL code out of the framework binary. The wrapper product ships arm64/arm64e only (no armv7 gum devkit), and batching is supported via gum interceptor transactions — `executeHooks` publishes the batch atomically, and partial failures are not rolled back. No memory patching. It is explicitly selectable (`HK_LIB_FRIDA`) and is a late implicit inline fallback after ElleKit and Dobby.

The wrapper is intentionally absent from the legacy rootful package: the current frida-gum archive hard-imports `os_unfair_lock` APIs unavailable on iOS 9 and has no old-ABI arm64e slice. Frida therefore reports unavailable there. Modern rootful carries a 14.0 floor and rootless/roothide carries 15.0 — see "Building." Inline patching needs relaxed codesigning (same as native/Dobby). Hooks must be installed at load time: the prologue patch is not atomic and so is not safe against code already running on another thread.

### fishhook

Rebinding by exported symbol name: private/interior addresses are not rebindable (`HK_ERR_NOT_SUPPORTED`). `old_ptr` reflects the state at hook time — fishhook retains the rebinding for all future image loads. arm64e PAC is handled: `__auth_got` slots are resigned with the asia key and slot-address discriminator, and `old_ptr` is resigned to the plain function-pointer scheme. A hook whose symbol no loaded image references is refused (`HK_ERR_NOT_SUPPORTED`) — the vendored fork's `rebind_symbols_checked` detects the no-op and `rebind_symbols_unbind` unregisters it, so nothing is retained for future image loads and the failure stays side-effect-free. The rebinding list is thread-safe. Compiled in on all archs with an iOS 9 floor.

### litehook

Explicitly selectable (`HK_LIB_LITEHOOK`) and also the compiled-in automatic fallback for rebind/memory coverage on every arch. It is strategy-aware via the category system — one backend, three strategies:

- default (`HKStrategyRebind`, `HK_CAT_FUNCTION_REBIND`): GOT/import slot rebinding by address (`litehook_rebind_symbol`), so exported and GOT-referenced C functions are hookable; `old_ptr` is the original function address, which is untouched (no original-call trampoline — same semantic as fishhook's `old_ptr`). A hook whose address no loaded image references through a GOT/import slot is reported as `HK_ERR_NOT_SUPPORTED` instead of a silent no-op. Vendor semantic: the image that **defines the replacement** is excluded from the rebind (upstream `_litehook_apply_global_rebind` skips `mh == sourceHeader`) — calls made from that image keep hitting the untouched slot, so a replacement defined in the caller's own dylib is only observable from other images.
- `HK_CAT_FUNCTION_INLINE` (`HKStrategyInline`): prologue inline trampolines via `litehook_hook_function`. There is no original-call trampoline — the original body is gone once hooked, so a requested `old_ptr` is refused with `HK_ERR_NOT_SUPPORTED` before anything is written (matching the descriptor's Unavailable original-publication policy); without an `old_ptr` request the hook installs.
- `HK_CAT_PRIVATE_SYMBOL` (`HKStrategyPrivateSymbol`): private-symbol resolution — the vendored DSC lookup (`litehook_find_dsc_symbol`) is stubbed out, so lookups fall back to HookKit's native bounded parser (reads private symbols from the dyld shared cache's local symbol table).
- Memory patching works on every supported arch (arm64e/arm64/armv7s/armv7, iOS 10+) — this is the only compiled-in backend that patches memory on armv7.
- The inline (`HK_CAT_FUNCTION_INLINE`) and private-symbol/DSC (`HK_CAT_PRIVATE_SYMBOL`) strategies are arm64/arm64e only — on armv7/armv7s only the rebind strategy and memory patching remain.

No native batch primitive (while batching is on, hooks queue and drain one at a time at `executeHooks`). MIT-licensed, no runtime dependency beyond libsystem.

### auto-cover

`substitutorWithAutoCoverCategories:` routes each function hook to the first available backend in the given categories whose side-effect-free preflight accepts the target, instead of pinning one backend at init:

```objc
HKSubstitutor *sub = [HKSubstitutor substitutorWithAutoCoverCategories:@[@(HK_CAT_FUNCTION_INLINE), @(HK_CAT_FUNCTION_REBIND)]];
```

- Per hook, category pickers are walked in priority order. Preflight declines are skipped; an invoked backend returning `HK_ERR_NOT_SUPPORTED` is also skipped because that status guarantees nothing was written. `HK_ERR`, `HK_ERR_PARTIAL`, and success are terminal.
- Requesting `old_ptr` skips candidates that cannot publish the original before activation.
- Batched hooks retry in grouped waves by backend, hook kind, and technique, preserving native batching without mixing inline and rebind routes from the same backend class. Rebind-only batches first resolve at drain time. Image/symbol APIs use the pinned first category backend; `activeType` reflects that fallback, not each hook's winner.
- Coverage story: a function too small for an inline patch is often still rebindable — fishhook and litehook rebind a GOT-referenced export regardless of body size — which is why `[FUNCTION_INLINE, FUNCTION_REBIND]` is the recommended composition.

### Swift

HookKit's own Swift vtable engine, sharing the native backend's memory-patching machinery. It rewrites the target method's slot in the class metadata vtable, so Swift callers of the method dispatch to the replacement. Hooks are installed by method name (`$s...`/`_$s...` exact mangled match, or a case-sensitive substring of the demangled name; the match must be unique — ambiguity fails loudly with every candidate logged) or by declaration-order slot index (stable per build, survives symbol stripping). v1 scope: the class's own methods only, non-generic classes, no resilient superclass, no async methods, no class methods, no extensions; `@objc dynamic` methods are hookable the same way (affects Swift callers only). The replacement must be a raw function pointer with the Swift calling convention (self in x20, heap context in x21 on arm64) — an `objc_msgSend`-convention IMP will misbehave.

The slot write is a single aligned pointer store (≈ atomic) with no code patching, so no relaxed codesigning is required for the write itself. Devirtualization and inlining can silently bypass vtable dispatch, and KVO-swizzled instances are unaffected. On arm64e the engine validates its signing recipe against the live slot before writing (PAC pre-write self-check): a mismatch fails the hook cleanly instead of corrupting memory. For stripped binaries, use the index API (declaration order, stable per build); name lookup cannot work without symbols. Swift 5 ABI (iOS 12.2+) and arm64/arm64e only. Opt-in (`HK_LIB_SWIFT`), separate API, no category membership.

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

- Publish-before-activation: originals are published into the caller's cell before a replacement becomes reachable — Cydia Substrate, Substitute, native, fishhook (rebind), litehook (rebind), Dobby (the vendored lib is rebuilt from upstream with a publication reorder patch), and Frida all publish first. Backends that cannot guarantee this refuse a requested `old_ptr` with `HK_ERR_NOT_SUPPORTED` before anything is written: ElleKit (its hooking writes the original into the out-cell only after the patch lands) and litehook (its inline has no original-call trampoline at all).
- Shared prologue preflight: inline-capable backends without a `preflightFunction:` of their own (ElleKit, Cydia Substrate, Substitute) are gated by a shared fixed-window prologue validator before dispatch — arm64/arm64e only; on ARMv7 the shared check is skipped, since Substrate and Substitute validate their own prologues there.
- Duplicate inline hooks: inline function hooks go through a process-wide ownership guard with explicit states — a duplicate of a PENDING hook (same address, same replacement, queued or in flight) is refused at hook time with `HK_ERR`; a duplicate of a TAINTED hook (a previous attempt failed mid-flight) is a hard `HK_ERR`; a same-hook re-hook against an INSTALLED entry returns `HK_OK`, reusing the guard's saved original (`HK_ERR` when an original was requested but none exists). Hooking the same address with a different replacement is refused with `HK_ERR_NOT_SUPPORTED`.

## Building

Requires [Theos](https://theos.dev). Jailbreak-root path resolution is compile-time per scheme via theos's bundled libroot (auto-linked as `-lroot`): the rootful package keeps paths as-is, the rootless package resolves via libroot (`/var/jb`), and the roothide target uses libroothide's `jbroot()`. There is no runtime root-detection dependency.

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

| Lane | Artifact | Package ID | HookKit slices and minimum iOS |
| --- | --- | --- | --- |
| `rootful-legacy` | `build/hookkit-rootful-legacy.deb` | `me.jjolano.fmwk.hookkit.legacy` | armv7 9.0, armv7s 9.0, arm64 9.0, old-ABI arm64e 12.0 |
| `rootful-modern` | `build/hookkit-rootful-modern.deb` | `me.jjolano.fmwk.hookkit` (`iphoneos-arm`) | arm64 14.0, new-ABI arm64e 14.0 |
| `rootless` | `build/hookkit-rootless.deb` | `me.jjolano.fmwk.hookkit` (`iphoneos-arm64`) | arm64 15.0, arm64e 15.0 |
| `roothide` | `build/hookkit-roothide.deb` | `me.jjolano.fmwk.hookkit` (`iphoneos-arm64e`) | arm64 15.0, arm64e 15.0 |

Modern rootful, rootless, and RootHide share the base package ID; APT selects them by package architecture. Legacy rootful keeps the distinct `.legacy` ID, conflicts with and replaces the base package, provides it for consumers such as Shadow, and is constrained to firmware below iOS 14. The two rootful binaries must not be lipo-merged: old- and new-ABI arm64e share the same architecture name but are incompatible.

Every lane runs `make clean`, the existing host tests, package verification, and export verification in isolated staging. `scripts/check_compat.sh` extracts the deb and reports `lipo -info` plus `vtool -show-build`/`otool -l` for every slice. Modern arm64e release builds require macOS with Xcode 12 or newer. The legacy lane defaults to `$THEOS/toolchain/oldabi/linux/iphone` and `$THEOS/sdks/iPhoneOS13.7.sdk`; `OLDABI_TOOLCHAIN` and `OLDABI_SDKS` remain optional overrides. RootHide requires the RootHide Theos fork. GitHub Actions builds the legacy lane on Linux and all modern lanes on macOS.

Provider signatures, return conventions, and original-publication order are source-audited against the exact snapshots in [`vendor/VENDORED.md`](vendor/VENDORED.md). This is still static coverage: PAC, dyld, shared-cache, page-protection, and injection behavior needs a real jailbroken device, external device lab, or beta-user report for each major iOS family.

## Advantages and Disadvantages

Advantages:

- Improved performance through use of batch hooking (if available).
- Ability to utilize different hooking libraries from your tweak. [Shadow](https://github.com/jjolano/shadow) is the primary consumer and provides this functionality.

Disadvantages:

- Library-specific functionality is not implemented uniformly: the Substrate-compatible backends (Cydia Substrate, Substitute) offer no native batch primitive (their hooks still queue while batching is on), fishhook only hooks exported C symbols, and Frida has no memory patching. For automatic instances, use the final hook/batch status rather than `activeType` to determine installation success.
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
