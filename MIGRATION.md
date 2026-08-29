# HookKit 3.0 — Migrate a generic tweak (2 min)

HookKit 3 is C-first. Existing Logos/Substrate tweaks migrate with **no source rewrite** for `%hook`, and **one import** for raw `MSHook*` calls. Recompile only; revert by deleting the import/flag.

## 1. Install HookKit into Theos (once)

```sh
make install-theos            # all 4 lanes, framework + hookkit Logos generator
# or one lane:
bash scripts/install-theos.sh rootless
```

Lane → framework path (`README.md:Install into Theos` — do not drift):
* `rootful-modern` → `$THEOS/lib/HookKit.framework` (default)
* `rootful-legacy` → `$THEOS/lib/iphone/rootful-legacy/HookKit.framework`
* `rootless` → `$THEOS/lib/iphone/rootless/HookKit.framework`
* `roothide` → `$THEOS/lib/iphone/roothide/HookKit.framework`

Every lane links `MyTweak_EXTRA_FRAMEWORKS += HookKit`. Legacy needs before `common.mk`:

```make
override THEOS_LIBRARY_PATH := $(THEOS)/lib/iphone/rootful-legacy
include $(THEOS)/makefiles/common.mk
ADDITIONAL_CFLAGS += -F$(THEOS)/lib
ADDITIONAL_LDFLAGS += -F$(THEOS)/lib
```

## 2. Migrate — pick your tweak type

### A. Pure Logos (`%hook` / `%orig`) — no source change

```make
# Tweak.mk — 2 lines, reversible
Tweak_EXTRA_FRAMEWORKS += HookKit
Tweak_LOGOSFLAGS += -c generator=hookkit
```

```objc
// Tweak.x — unchanged
#import <logos/logos.h>
%hook SBApplication
- (void)launch { %orig; NSLog(@"hooked"); }
%end
%ctor { %init; } // hookkit generator emits HookKit plan inline; no CydiaSubstrate link
```

`hookkit` generator replaces `MobileSubstrate`'s `__asm__(".linker_option \"-framework\", \"CydiaSubstrate\"")` with HookKit, and calls `_hk_hookkit_hook_message` (singleton `HK_INSTALL_CONTEXT_EARLY_PROCESS`). Remove the 2 lines → back to Substrate. Verify:

```sh
otool -l .theos/obj/.../Tweak.dylib | grep -A1 LC_LOAD_DYLIB  # shows HookKit, not CydiaSubstrate
```

### B. Raw calls (`MSHookFunction` / `substitute_hook_functions` / `LHHookFunctions` / `LBHookMessage` / memory) — one import

```objc
// Tweak.xm — insert BEFORE the provider header
#import <HookKit/HookKitCompat.h>   // must be before <substrate.h> etc
#import <substrate.h>
#import <substitute.h>
#import <libhooker/libhooker.h>

%ctor {
    MSHookFunction((void*)malloc, (void*)my_malloc, (void**)&orig_malloc); // now → HookKit, void-natural
    LHHookFunctions((struct LHFunctionHook[]){{(void*)open, (void*)my_open, (void**)&orig_open}}, 1);
    substitute_hook_functions((struct substitute_function_hook[]){{func, rep, &old}}, 1, NULL, 0);
    LBHookMessage([MyClass class], @selector(foo), (void*)my_imp, (void*)&orig_imp);
    // memory: shim reads current bytes as expected_bytes, validates at commit
    MSHookMemory((void*)0x1000, data, sz);
    LHPatchMemory((struct LHMemoryPatch[]){{dst, data, sz}}, 1);
}
```

`HookKitCompat.h` hijacks `MSHookFunction`/`MSHookMessageEx`/`MSHookMemory`/`substitute_hook_functions`/`substitute_hook_objc_message`/`LHHookFunctions`/`LBHookMessage`/`LHPatchMemory` to HookKit plans (batch where the API is batch). Gates:

```c
#define HOOKKIT_COMPAT_SUBSTRATE  1
#define HOOKKIT_COMPAT_SUBSTITUTE 1
#define HOOKKIT_COMPAT_LIBHOOKER  1  // covers ElleKit
#define HOOKKIT_COMPAT_MEMORY     1
#define HOOKKIT_COMPAT_HIJACK     1  // 0 → header only defines hk_compat_* without hijacking
```

Need error codes? Use `hk_compat_*` int variants:

```c
int r = hk_compat_MSHookFunction_int(sym, rep, &out); // 0 ok
int n = LHHookFunctions(hooks, 2); // returns hooked count, HookKit-backed
int err = substitute_hook_functions(hooks, n, NULL, 0); // SUBSTITUTE_OK mapping
```

Remove `#import <HookKit/HookKitCompat.h>` or `-DHOOKKIT_COMPAT_HIJACK=0` → back to original libs. Keep Logos generator flag if you also have `%hook`.

## 3. Automated

```sh
python3 Tools/migrate.py --check Tweak.x Makefile       # report counts, what would change
python3 Tools/migrate.py --apply Tweak.x Makefile       # inserts 2 Makefile lines + Compat import where needed
python3 Tools/migrate.py --revert Tweak.x Makefile      # removes them
python3 Tools/migrate.py --check --all                  # scans *.x *.xm Makefile*
```

Idempotent, dry-run by default (`--check`).

## 4. Recompile & verify

```sh
make clean && make package
make test                    # host tests
bash scripts/check_exports.sh # HookKit.tbd unchanged — Compat.h is static inline, no exports
```

Device: `tests/device_lifecycle_smoke.c:1` / `tests/device_objc_smoke.m:1` pattern — runtime + plan + orig slot.

## 5. FAQ

* **Routing?** `hk_runtime_create()` auto-routes; `hk_runtime_create_with_backend_override()` pins IDs (`README.md:Backend routing`). Compat shim uses auto-routing.
* **Retrying another route?** Only when `hk_hook_result_refused_cleanly(&r)` — i.e. `mutation==HK_MUTATION_NONE && (outcome==HK_OUTCOME_NO_ROUTE||outcome==HK_OUTCOME_FAILED_SAFE)`. Never after `PARTIAL`/`UNKNOWN` (`README.md:Backend routing`).
* **Shared runtime?** `hk_shared_runtime()` is the process singleton (`HK_INSTALL_CONTEXT_EARLY_PROCESS`, `pthread_once` memo). Prefer it over per-TU statics; cuts per-hook `calloc+HKIDs` cost.
* **Import-slot artifacts?** `hk_artifact_is_import_slot(&a)` — iterate `hk_report_copy_artifacts(report,&snap); for(size_t i=0;i<hk_artifact_snapshot_count(snap);i++){hk_artifact_t a; hk_artifact_snapshot_copy_at(snap,i,&a); if(hk_artifact_is_import_slot(&a))/* import slot */; } hk_artifact_snapshot_release(snap);` No new `hk_artifact_t` kind.
* **Calling original?** `%orig` unchanged. Raw: `MSHookFunction` orig pointer still filled via `hk_original_slot_load`.
* **Memory `expected_bytes`?** Compat shim reads current bytes at `dst` as expected, revalidates at `hk_plan_commit`. Commit fails safe (`HK_OUTCOME_FAILED_SAFE`) if mismatch — not a blind write.
* **Back to Logos?** Delete the 2 `Makefile` lines (+ Compat import). Theos Logos again emits `CydiaSubstrate` link, source unchanged.
* **Inheritance?** Default `HK_OBJC_LOCAL_METHOD_ONLY` (safe). Opt-in per-method `__attribute__((annotate("hookkit:allow_inherited")))` gated by `%config hook_inheritance=allow_inherited` (i.e. both required). Widening blast radius: an inherited hook may attach to a superclass impl and affect all subclasses — use only when you intend that.
