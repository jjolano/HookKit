# HookKit

HookKit is a C-first iOS hooking runtime. A caller creates a runtime, builds a
plan, then analyzes, prepares, and commits explicit hook requests. Results
state exactly whether a target changed.

```c
#include <HookKit/HookKit.h>

hk_runtime_t *runtime = NULL;
hk_plan_t *plan = NULL;
hk_hook_t *hook = NULL;

hk_runtime_config_t config = {
    .struct_size = sizeof(config),
    .struct_version = HK_ABI_VERSION_3_0,
    .install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS,
};

hk_runtime_create(&config, &runtime);
hk_plan_create(runtime, NULL, &plan);
hk_plan_add_hook(plan, &spec, &hook); // `spec` is a fully initialized hk_hook_spec_t
hk_plan_analyze(plan, NULL);
hk_plan_prepare(plan, NULL);
hk_plan_commit(plan, NULL);
```

Read `hk_hook_result_t` with `hk_hook_copy_result()` after each stage. API
status says whether a call completed; `outcome` and `mutation` say what
happened to the individual target.

## Backend routing

`hk_runtime_create()` uses normal automatic routing. Discover currently
available IDs with `hk_runtime_enumerate_backends()`.

`hk_runtime_create_with_backend_override()` accepts a comma/space-separated,
strict per-runtime ID list. Only those function/memory engines remain
eligible; the built-in Objective-C engine remains eligible. An empty or
all-invalid list intentionally leaves no function/memory route. `NULL` means
automatic routing.

```c
hk_runtime_create_with_backend_override(
    &config, "provider-ellekit", &runtime);
```

Strict selection does not fall through to an unselected engine. A caller may
only retry another route after the failed hook result reports
`HK_MUTATION_NONE`; `PARTIAL` and `UNKNOWN` are terminal because the target
may have changed.

## Engines

The runtime includes built-in Objective-C, import-rebinding, memory-patch,
native inline, relocating inline, and Swift-vtable engines, plus certified
provider adapters where available. Dobby is excluded from the iOS 9–13 lane;
the optional Frida Gum provider is packaged separately on modern lanes.

Inline patching needs the process permissions appropriate for executable page
writes and must normally happen at load time. Function-entry engines report a
clean refusal when they can prove they have not changed a target; inspect the
per-hook mutation state rather than inferring it from a generic error.

## Headers

Use `<HookKit/HookKit.h>` for the C umbrella. `<HookKit/HookKitObjC.h>` is an
opt-in typed `Class`/`SEL` convenience header; it is intentionally not pulled
into the C umbrella. `<HookKit.h>` remains a forwarding include path to the
same C API.

## Building

Requires [Theos](https://theos.dev). Build each package lane independently:

```sh
./build.sh rootful-legacy
./build.sh rootful-modern
./build.sh rootless
./build.sh roothide
```

`rootful-legacy` retains the iOS 9–13 architecture/toolchain lane. It is an
OS-support lane, not a pre-3.0 API package. Modern lanes build the optional
Gum provider package separately. Every lane runs host tests, package checks,
and exact export checks.

## Install into Theos

Run `make install-theos` to build all lanes and install their verified
frameworks beneath `$THEOS/lib`:

| Lane | Framework path | Consumer setup |
| --- | --- | --- |
| `rootful-modern` | `$THEOS/lib/HookKit.framework` | Default/rootful Theos scheme; no extra search path. |
| `rootful-legacy` | `$THEOS/lib/iphone/rootful-legacy/HookKit.framework` | Use the legacy library-root override below. |
| `rootless` | `$THEOS/lib/iphone/rootless/HookKit.framework` | `THEOS_PACKAGE_SCHEME=rootless`; resolved automatically. |
| `roothide` | `$THEOS/lib/iphone/roothide/HookKit.framework` | `THEOS_PACKAGE_SCHEME=roothide`; resolved automatically. |

Link every lane with `MyTweak_EXTRA_FRAMEWORKS += HookKit`. Modern rootful owns
the default path. Legacy deliberately lives separately: its old arm64e ABI
cannot share a framework with the modern rootful binary. Put this before the
consumer's `common.mk` include:

```make
override THEOS_LIBRARY_PATH := $(THEOS)/lib/iphone/rootful-legacy
include $(THEOS)/makefiles/common.mk

# Keep ordinary local frameworks available after HookKit is selected.
ADDITIONAL_CFLAGS += -F$(THEOS)/lib
ADDITIONAL_LDFLAGS += -F$(THEOS)/lib
```

Rootful, modern, and rootless binaries use
`@rpath/HookKit.framework/HookKit`; Roothide uses its required
`@loader_path/.jbroot/Library/Frameworks/HookKit.framework/HookKit` identity.

## Verification

```sh
make test
bash scripts/check_exports.sh
```

The public ABI is declared under `Headers/HookKit/`; `HookKit.tbd` and
`scripts/export-HookKit.list` are the release export contract.
