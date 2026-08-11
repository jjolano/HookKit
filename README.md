# HookKit

A slim iOS developer framework that unifies nine hooking backends behind one API.

## Backends

The best backend available on the device is picked at runtime, in priority order: ElleKit > Cydia Substrate > Substitute > fishhook — with Dobby, litehook, native, Frida, and Swift vtables available on request but never picked automatically. fishhook and litehook are compiled in and always present, making fishhook the fallback floor on every arch; Dobby is compiled in on arm64/arm64e only, and is opt-in via category or capability selection. Backends are not packaged separately: the single package `me.jjolano.fmwk.hookkit` works on every jailbreak. (This replaces the v1 Modulous plugin-bundle architecture.)

### Selection

Backends are selected by name, by priority list, or by capability:

- `substitutorWithTypes:` names specific backends; the first available one wins, in the fixed table priority above. Dobby, `native`, Frida, Swift, and litehook are **never picked automatically** — they only resolve when explicitly named (`substitutorWithTypes:`) or reached through a category.
- To override the priority order, pass an explicit list with `substitutorWithOrderedTypes:` — the first available entry wins:

```objc
HKSubstitutor *sub = [HKSubstitutor substitutorWithOrderedTypes:@[@(HK_LIB_SUBSTRATE), @(HK_LIB_FISHHOOK), @(HK_LIB_ELLEKIT)]];
```

- `substitutorWithCategory:` (`HK_CAT_MESSAGE`, `HK_CAT_FUNCTION_REBIND`, `HK_CAT_FUNCTION_INLINE`, `HK_CAT_PRIVATE_SYMBOL`) picks the first available backend in that category's priority order — callers request a capability, not a library. `substitutorWithOrderedCategories:` tries a list of categories top to bottom, stopping at the first that resolves. The per-category picker orders are:

  - `HK_CAT_MESSAGE` — ElleKit > Cydia Substrate > Substitute > native
  - `HK_CAT_FUNCTION_REBIND` — fishhook > litehook
  - `HK_CAT_FUNCTION_INLINE` — ElleKit > Dobby > Frida > litehook
  - `HK_CAT_PRIVATE_SYMBOL` — ElleKit > Cydia Substrate > Substitute > litehook

- The readonly `activeStrategy` property (one of `HKStrategyDefault`/`HKStrategyRebind`/`HKStrategyInline`/`HKStrategyPrivateSymbol`) reports how the winning backend resolves and applies function hooks — a hooking technique (rebind or inline, or the vendor default), or a resolution mode (`HKStrategyPrivateSymbol`: the backend locates the private/DSC symbol first, then hooks it via rebinding). It is the resolution result, not a request. One vendor can serve several categories: litehook covers rebind, inline, and private-symbol lookups. Swift vtables have no category (`HK_CAT_NONE`) — it is a separate API, not a message/function/memory engine.
- Resolution order: `orderedTypes` > `orderedCategories` > `types`/default. A requested-but-unavailable backend is **not** silently substituted: hook calls return `HK_ERR_NOT_SUPPORTED` and `activeType` is `HK_LIB_NONE`. Check `activeType` before relying on a backend.
- The framework's default set differs from an explicit request: with no types set, only the *automatic* backends are eligible — ElleKit, Cydia Substrate, Substitute, or fishhook, in that order, first available wins. The opt-in backends (Dobby, native, Frida, Swift, litehook) are never eligible for the automatic pick, even when available; explicit `types` can name any of them.
- `getSubstitutorTypeInfo:` returns per-backend metadata including a `selectable` flag for settings-style pickers: substrate, substitute, and swift are excluded from user-facing selection.

### Capability matrix

| Backend         | Message | Function | Memory | Native batch° |
|-----------------|---------|----------|--------|----------|
| ElleKit         | yes     | yes†     | yes    | no       |
| Cydia Substrate | yes     | yes      | yes‡   | no       |
| Substitute      | yes     | yes      | yes‡   | no       |
| native          | yes     | yes†     | yes    | no       |
| Dobby           | no      | yes†     | yes    | no       |
| Frida           | no      | yes†     | no     | yes      |
| fishhook        | no      | yes¶     | no     | no       |
| Swift           | no      | no*      | no     | no       |
| litehook        | no      | yes§     | yes    | no       |

† Function hooking is arm64/arm64e only — these backends report unavailable on armv7 (see the native, Dobby, and Frida caveats).

‡ Via `MSHookMemory` when the installed Cydia Substrate exports it, and via the MS-compatible `SubHookMemory` shim on Substitute — unavailable when the installed library doesn't export it (see the memory caveat).

§ Exported-symbol or GOT-referenced C functions, rebinding by address; the inline variant (`HK_CAT_FUNCTION_INLINE`) has no original-call trampoline (see the litehook caveat).

¶ Exported symbols only, rebinding by symbol name (see the fishhook caveat).

\* Swift vtable hooking is a separate API — `hookSwiftMethodInClass:withName:...` / `hookSwiftVtableSlotInClass:withIndex:...` — not the message/function columns (see the Swift caveat).

° While batching is on, every supported hook kind is queued and nothing applies until `executeHooks` (queue-always); the column marks the backends that apply the drained batch at once through a native primitive (only Frida, atomically).

### native

HookKit's own hooking engine, requiring no hooking library to be installed on the device. It implements inline function hooking with an ARM64 instruction relocator, memory patching, ObjC message hooking through the runtime, and symbol lookup that reads private symbols out of the dyld shared cache's local symbol table.

It is opt-in rather than the default so that devices with a battle-tested engine installed keep using it. Use it when you need HookKit to stand alone, or to dogfood it before promoting it.

Constraints: arm64/arm64e only — on armv7 it reports unavailable, since those devices always have Substrate. Inline patching needs relaxed codesigning, which holds in a tweak-injected process but not in an unmodified one. Function and memory patching require the main thread — the engine does not suspend peer threads, so off the main thread they return `HK_ERR_NOT_SUPPORTED` without patching. Targets too short to patch without clobbering the function that follows them are refused with `HK_ERR_NOT_SUPPORTED` (engine `SHORT_FUNCTION`, mapped alongside `UNSUPPORTED` and `RELOCATE`), nothing written. Hooks must be installed at load time: the patch is not atomic and so is not safe against code already running on another thread. Shared cache symbol parsing depends on the cache layout and should be re-verified each major iOS release.

### Dobby

Vendored static library hooking inline by address, so interior/private C functions are hookable (beyond fishhook's exported-symbols-only limit). Opt-in (`HK_LIB_DOBBY` or the `HK_CAT_FUNCTION_INLINE` category) — no longer part of the automatic pick. arm64/arm64e only — on armv7 it reports unavailable. Inline patching needs relaxed codesigning (same constraint as native). Memory patching is supported; no ObjC message hooking, no native batch primitive (while batching is on, hooks queue and drain one at a time at `executeHooks`).

### Frida

Hooks through the `HKGum.dylib` wrapper (frida-gum devkit, LGPL-2.1 with wxWindows exception) dlopen'd at runtime — the framework never links gum directly, keeping LGPL code out of the framework binary. The wrapper product ships arm64/arm64e only (no armv7 gum devkit), and batching is supported via gum interceptor transactions — `executeHooks` publishes the batch atomically, and partial failures are not rolled back. No memory patching, no ObjC message hooking. Opt-in (`HK_LIB_FRIDA`) — Dobby is compiled in and lighter, so Frida is only picked when explicitly requested.

The wrapper is built with a 9.0/12.0 arm64 floor, so the arm64 slice loads on iOS 12/13; only the arm64e slice carries the 14.0 minos (theos's clang forces arm64e to ≥ 14.0) — see "Building". Inline patching needs relaxed codesigning (same as native/Dobby). Hooks must be installed at load time: the prologue patch is not atomic and so is not safe against code already running on another thread.

### fishhook

Rebinding by exported symbol name: private/interior addresses are not rebindable (`HK_ERR_NOT_SUPPORTED`). `old_ptr` reflects the state at hook time — fishhook retains the rebinding for all future image loads. arm64e PAC is handled: `__auth_got` slots are resigned with the asia key and slot-address discriminator, and `old_ptr` is resigned to the plain function-pointer scheme. A hook whose symbol no loaded image references is refused (`HK_ERR_NOT_SUPPORTED`) — the vendored fork's `rebind_symbols_checked` detects the no-op and `rebind_symbols_unbind` unregisters it, so nothing is retained for future image loads and the failure stays side-effect-free. The rebinding list is thread-safe. Compiled in on all archs; the floor on armv7.

### litehook

Opt-in (`HK_LIB_LITEHOOK`), never selected automatically, but compiled in and available on every arch. It is strategy-aware via the category system — one backend, three strategies:

- default (`HKStrategyRebind`, `HK_CAT_FUNCTION_REBIND`): GOT/import slot rebinding by address (`litehook_rebind_symbol`), so exported and GOT-referenced C functions are hookable; `old_ptr` is the original function address, which is untouched (no original-call trampoline — same semantic as fishhook's `old_ptr`). A hook whose address no loaded image references through a GOT/import slot is reported as `HK_ERR_NOT_SUPPORTED` instead of a silent no-op.
- `HK_CAT_FUNCTION_INLINE` (`HKStrategyInline`): prologue inline trampolines via `litehook_hook_function`. There is no original-call trampoline — the original body is gone once hooked, so a requested `old_ptr` is refused with `HK_ERR_NOT_SUPPORTED` before anything is written (matching the descriptor's Unavailable original-publication policy); without an `old_ptr` request the hook installs.
- `HK_CAT_PRIVATE_SYMBOL` (`HKStrategyPrivateSymbol`): private-symbol resolution — the vendored DSC lookup (`litehook_find_dsc_symbol`) is stubbed out, so lookups fall back to HookKit's native bounded parser (reads private symbols from the dyld shared cache's local symbol table).
- Memory patching works on every supported arch (arm64e/arm64/armv7s/armv7, iOS 10+) — this is the only compiled-in backend that patches memory on armv7.
- The inline (`HK_CAT_FUNCTION_INLINE`) and private-symbol/DSC (`HK_CAT_PRIVATE_SYMBOL`) strategies are arm64/arm64e only — on armv7/armv7s only the rebind strategy and memory patching remain.

No ObjC message hooking, no native batch primitive (while batching is on, hooks queue and drain one at a time at `executeHooks`). MIT-licensed, no runtime dependency beyond libsystem.

### auto-cover

`substitutorWithAutoCoverCategories:` routes each non-batched function hook to the first available backend in the given categories whose side-effect-free preflight accepts the target, instead of pinning one backend at init:

```objc
HKSubstitutor *sub = [HKSubstitutor substitutorWithAutoCoverCategories:@[@(HK_CAT_FUNCTION_INLINE), @(HK_CAT_FUNCTION_REBIND)]];
```

- Per hook, the categories' pickers are walked in priority order; the first available backend whose `preflightFunction:` returns `HK_OK` is invoked exactly once. Backends without a preflight are presumed to accept.
- Every picker declining returns `HK_ERR_NOT_SUPPORTED` with nothing written (fail closed). Routing is preflight-driven only — it never consults a hook result, because a failed invocation may have mutated the target.
- Batch hooks and the image/symbol APIs use the first available category backend, pinned at init (batches must not split across backends); `activeType` reflects that pinned fallback, and the per-hook winner is not reported.
- Coverage story: a function too small for an inline patch is often still rebindable — fishhook and litehook rebind a GOT-referenced export regardless of body size — which is why `[FUNCTION_INLINE, FUNCTION_REBIND]` is the recommended composition.

### Swift

HookKit's own Swift vtable engine, sharing the native backend's memory-patching machinery. It rewrites the target method's slot in the class metadata vtable, so Swift callers of the method dispatch to the replacement. Hooks are installed by method name (`$s...`/`_$s...` exact mangled match, or a case-sensitive substring of the demangled name; the match must be unique — ambiguity fails loudly with every candidate logged) or by declaration-order slot index (stable per build, survives symbol stripping). v1 scope: the class's own methods only, non-generic classes, no resilient superclass, no async methods, no class methods, no extensions; `@objc dynamic` methods are hookable the same way (affects Swift callers only). The replacement must be a raw function pointer with the Swift calling convention (self in x20, heap context in x21 on arm64) — an `objc_msgSend`-convention IMP will misbehave.

The slot write is a single aligned pointer store (≈ atomic) with no code patching, so no relaxed codesigning is required for the write itself. Devirtualization and inlining can silently bypass vtable dispatch, and KVO-swizzled instances are unaffected. On arm64e the engine validates its signing recipe against the live slot before writing (PAC pre-write self-check): a mismatch fails the hook cleanly instead of corrupting memory. For stripped binaries, use the index API (declaration order, stable per build); name lookup cannot work without symbols. Swift 5 ABI (iOS 12.2+) and arm64/arm64e only. Opt-in (`HK_LIB_SWIFT`), separate API, no category membership.

### memory

`hookMemory:` works via `MSHookMemory` on Cydia Substrate — but only when the installed Substrate exports it (unc0ver-class Substrate does). On Substitute it works via the MS-compatible shim (`MSHookMemory`, which libsubstitute maps to `SubHookMemory`) — not via the native path, which has no separate memory hook; when the shim is absent, `hookMemory:` reports `HK_ERR_NOT_SUPPORTED`. Substitute's function and message hooks go through the native libsubstitute API (`substitute_hook_functions` / `substitute_hook_objc_message`) when available, falling back to the MS-compatible path otherwise, with native-API failures returning `HK_ERR`; neither path is used for raw-address `hookMemory:`.

## Usage

```objc
HKSubstitutor *sub = [HKSubstitutor defaultSubstitutor]; // or substitutorWithTypes:

// Batching is queue-always: every supported hook kind queues; nothing applies until executeHooks.
HKEnableBatching();

void (*orig_malloc)(void *);
HKHookFunction(&malloc, &my_malloc, &orig_malloc);

[sub hookMessageInClass:[NSString class]
           withSelector:@selector(length)
         withReplacement:&my_length
               outOldPtr:&orig_length];

HKExecuteBatch();
```

`HKHookMessage`, `HKHookMemory`, `HKOpenImage`, and `HKFindSymbol` macros are also available.

`hookMessageInClass:` dispatches class-method-only selectors through the metaclass (`object_getClass()`), so class methods hook correctly on every backend; instance methods pass the class as-is.

## Semantics

Normative semantics — the symbol-name convention, status codes, the `HK_ERR_NOT_SUPPORTED` contract, the fail-closed prologue preflight, arch gates, `getLibErrno:`, threading, batch storage lifetime, and availability probing — are specified in the `HKSubstitutor` interface comment in `Headers/HookKit/Compat.h`, which is the source of truth. This section keeps only the backend-operational behavior not covered there:

- Publish-before-activation: originals are published into the caller's cell before a replacement becomes reachable — Cydia Substrate, Substitute, native, fishhook (rebind), litehook (rebind), and Frida all publish first. Backends that cannot guarantee this refuse a requested `old_ptr` with `HK_ERR_NOT_SUPPORTED` before anything is written: ElleKit (its hooking writes the original into the out-cell only after the patch lands) and litehook (its inline has no original-call trampoline at all).
- Shared prologue preflight: inline-capable backends without a `preflightFunction:` of their own (ElleKit, Cydia Substrate, Substitute) are gated by a shared fixed-window prologue validator before dispatch — arm64/arm64e only; on ARMv7 the shared check is skipped, since Substrate and Substitute validate their own prologues there.
- Duplicate inline hooks: inline function hooks go through a process-wide ownership guard with explicit states — a duplicate of a PENDING hook (same address, same replacement, queued or in flight) is refused at hook time with `HK_ERR`; a duplicate of a TAINTED hook (a previous attempt failed mid-flight) is a hard `HK_ERR`; a same-hook re-hook against an INSTALLED entry returns `HK_OK`, reusing the guard's saved original (`HK_ERR` when an original was requested but none exists). Hooking the same address with a different replacement is refused with `HK_ERR_NOT_SUPPORTED`.

## Building

Requires [Theos](https://theos.dev). Jailbreak-root path resolution is compile-time per scheme via theos's bundled libroot (auto-linked as `-lroot`): the rooted package keeps paths as-is, the rootless package resolves via libroot (`/var/jb`), and the roothide target uses libroothide's `jbroot()`. There is no runtime root-detection dependency.

```
./build.sh all|rootless|rooted|roothide
```

- `rootless` — iphoneos-arm64 deb (arm64/arm64e), iOS 12+.
- `rooted` — one fat iphoneos-arm deb spanning armv7 through arm64e, iOS 9+.
- `roothide` — iOS 15–17 with a random-named jbroot; requires the roothide Theos fork (`THEOS_PACKAGE_SCHEME=roothide`) and libroothide.

Theos bumps the arm64e slice minos to 14.0; a build-time warning about this is expected and known.

## Advantages and Disadvantages

Advantages:

- Improved performance through use of batch hooking (if available).
- Ability to utilize different hooking libraries from your tweak. [Shadow](https://github.com/jjolano/shadow) is the primary consumer and provides this functionality.

Disadvantages:

- Library-specific functionality is not implemented uniformly: the Substrate-compatible backends (Cydia Substrate, Substitute) offer no native batch primitive (their hooks still queue while batching is on), fishhook only hooks exported C symbols, Frida has no memory patching — check `activeType` and the capability matrix before relying on a capability.
- Existing tweaks will need to be rewritten/recompiled to use HookKit.

## Credits

- [fishhook](https://github.com/facebook/fishhook)
- [Dobby](https://github.com/jmpews/Dobby)
- [frida-gum](https://github.com/frida/frida-gum)
- [libhooker](https://github.com/coolstar/libhooker) / [ElleKit](https://github.com/evelyneee/ellekit)
- [litehook](https://github.com/opa334/litehook)
- [libroot](https://github.com/opa334/libroot) — rootless path resolution (bundled with theos)
- [Substitute](https://github.com/sbingner/substitute)
- [Cydia Substrate](http://www.cydiasubstrate.com)
- [apple/swift](https://github.com/apple/swift) — Swift 5 ABI documentation (`include/swift/ABI/Metadata.h`, `MetadataValues.h`, `stdlib/public/runtime/Metadata.cpp`, `lib/IRGen/GenMeta.cpp`), which the Swift vtable backend's offsets and pointer-authentication recipe were verified against