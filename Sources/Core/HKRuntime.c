// hk_runtime_t lifecycle. hk_runtime_shutdown still has nothing to quiesce
// (no executor threads exist), but hk_runtime_drain_pending is real as of
// Milestone 12 and lives in HKPlan.c, beside the per-hook engine dispatch it
// shares with prepare/commit. The runtime now owns one thing beyond itself:
// deferred hooks transferred out of released plans.
//
// Loading this translation unit does no implicit work (docs/3.0/
// ARCHITECTURE.md invariant #7) -- everything here runs only from an
// explicit hk_runtime_create call. hk_runtime_create itself does no
// provider activation, image traversal, callback registration, or thread
// creation: calloc, one ID generation, one struct copy.

#include "HKIDs.h"
#include "HKRuntimeInternal.h"
#include "../../native/hk_native.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <Availability.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/sysctl.h>

#include "../../vendor/substitute/substitute.h"
#if defined(HOOKKIT_CANONICAL_3) && \
    (defined(__arm64__) || defined(__arm64e__)) && !defined(HK_NO_DOBBY)
#include "../../vendor/dobby/dobby.h"
#include "../../vendor/libhooker/libhooker.h"

// HKInlinePreflight.h is intentionally Objective-C-facing because the 2.x
// backends import Foundation. The two C entry points below have no ObjC types;
// declare that tiny internal seam here so the C-first runtime never imports
// Foundation merely to read a function prologue.
extern int hk_inline_preflight_basic(void *function, void *replacement,
                                     int *out_error);
extern int hk_inline_preflight(void *function, void *replacement,
                               size_t window, int *out_error);
enum { HK_PLATFORM_DOBBY_INSPECTION_BYTES = 16 };

typedef struct {
    pthread_mutex_t lock;
    void *handle;
    int (*hook)(void *, void *, void **);
    void (*begin_transaction)(void);
    void (*end_transaction)(void);
} hk_platform_gum_state_t;

typedef struct {
    pthread_mutex_t lock;
    void *handle;
    int (*hook_functions)(const struct LHFunctionHook *, int);
} hk_platform_ellekit_state_t;

static hk_platform_gum_state_t g_platform_gum = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static hk_platform_ellekit_state_t g_platform_ellekit = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static bool hk_platform_dobby_validate(void *ctx, const hk_hook_spec_t *spec,
                                       hk_prepare_diag_t *out_diag) {
    (void)ctx;
    int provider_error = 0;
    if (hk_inline_preflight((void *)spec->target.address.address,
                            spec->replacement,
                            HK_PLATFORM_DOBBY_INSPECTION_BYTES,
                            &provider_error) == 0) {
        return true;
    }
    out_diag->error_code = provider_error;
    out_diag->error_message = "Dobby preflight refused the target";
    return false;
}

static int hk_platform_dobby_hook(void *ctx, void *target, void *replacement,
                                  void **out_original) {
    (void)ctx;
    return DobbyHook(target, replacement, out_original);
}

static bool hk_platform_gum_discover(void *ctx) {
    (void)ctx;
    return dlopen_preflight("/var/jb/usr/lib/HKGum.dylib") ||
           dlopen_preflight("/usr/lib/HKGum.dylib");
}

static bool hk_platform_gum_validate(void *ctx, const hk_hook_spec_t *spec,
                                     hk_prepare_diag_t *out_diag) {
    (void)ctx;
    int provider_error = 0;
    if (hk_inline_preflight_basic((void *)spec->target.address.address,
                                  spec->replacement, &provider_error) == 0) {
        return true;
    }
    out_diag->error_code = provider_error;
    out_diag->error_message = "Gum preflight refused the target";
    return false;
}

static bool hk_platform_gum_prepare(void *ctx, hk_prepare_diag_t *out_diag) {
    hk_platform_gum_state_t *state = ctx;
    bool activated_now = false;
    pthread_mutex_lock(&state->lock);
    if (!state->hook) {
        static const char *const paths[] = {
            "/var/jb/usr/lib/HKGum.dylib",
            "/usr/lib/HKGum.dylib",
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            void *handle = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
            if (!handle) {
                continue;
            }
            int (*hook)(void *, void *, void **) =
                (int (*)(void *, void *, void **))dlsym(handle,
                                                         "hkgum_hook_function");
            void (*begin_transaction)(void) =
                (void (*)(void))dlsym(handle, "hkgum_begin_transaction");
            void (*end_transaction)(void) =
                (void (*)(void))dlsym(handle, "hkgum_end_transaction");
            if (hook && begin_transaction && end_transaction) {
                state->handle = handle;
                state->hook = hook;
                state->begin_transaction = begin_transaction;
                state->end_transaction = end_transaction;
                activated_now = true;
                break;
            }
            // A constructor may already have run, so report the attempted
            // image load/activation even though the ABI was unusable.
            dlclose(handle);
            out_diag->observed_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                        HK_EFFECT_PROVIDER_ACTIVATION |
                                        HK_EFFECT_UNKNOWN_PROCESS_MUTATION;
        }
    }
    bool ready = state->hook && state->begin_transaction && state->end_transaction;
    pthread_mutex_unlock(&state->lock);
    if (!ready) {
        out_diag->error_message = "HKGum.dylib is unavailable or has an incomplete wrapper ABI";
        return false;
    }
    if (activated_now) {
        out_diag->observed_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                    HK_EFFECT_PROVIDER_ACTIVATION |
                                    HK_EFFECT_UNKNOWN_PROCESS_MUTATION;
    }
    return true;
}

static int hk_platform_gum_hook(void *ctx, void *target, void *replacement,
                                void **out_original) {
    hk_platform_gum_state_t *state = ctx;
    pthread_mutex_lock(&state->lock);
    if (!state->hook || !state->begin_transaction || !state->end_transaction) {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }
    state->begin_transaction();
    int result = state->hook(target, replacement, out_original);
    state->end_transaction();
    pthread_mutex_unlock(&state->lock);
    return result;
}

static bool hk_platform_ellekit_discover(void *ctx) {
    (void)ctx;
    static const char *const paths[] = {
        "/var/jb/usr/lib/libellekit.dylib",
        "/usr/lib/libellekit.dylib",
        "/var/jb/usr/lib/libhooker.dylib",
        "/usr/lib/libhooker.dylib",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (dlopen_preflight(paths[i])) {
            return true;
        }
    }
    return false;
}

static bool hk_platform_ellekit_validate(void *ctx,
                                         const hk_hook_spec_t *spec,
                                         hk_prepare_diag_t *out_diag) {
    (void)ctx;
    int provider_error = 0;
    if (hk_inline_preflight_basic((void *)spec->target.address.address,
                                  spec->replacement, &provider_error) == 0) {
        return true;
    }
    out_diag->error_code = provider_error;
    out_diag->error_message = "ElleKit preflight refused the target";
    return false;
}

static bool hk_platform_ellekit_prepare(void *ctx,
                                        hk_prepare_diag_t *out_diag) {
    hk_platform_ellekit_state_t *state = ctx;
    bool activated_now = false;
    pthread_mutex_lock(&state->lock);
    if (!state->hook_functions) {
        static const char *const paths[] = {
            "/var/jb/usr/lib/libellekit.dylib",
            "/usr/lib/libellekit.dylib",
            "/var/jb/usr/lib/libhooker.dylib",
            "/usr/lib/libhooker.dylib",
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            void *handle = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
            if (!handle) {
                continue;
            }
            int (*hook_functions)(const struct LHFunctionHook *, int) =
                (int (*)(const struct LHFunctionHook *, int))dlsym(
                    handle, "LHHookFunctions");
            if (hook_functions && dlsym(handle, "EKHookFunction")) {
                state->handle = handle;
                state->hook_functions = hook_functions;
                activated_now = true;
                break;
            }
            dlclose(handle);
            out_diag->observed_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                         HK_EFFECT_PROVIDER_ACTIVATION |
                                         HK_EFFECT_UNKNOWN_PROCESS_MUTATION;
        }
    }
    bool ready = state->hook_functions != NULL;
    pthread_mutex_unlock(&state->lock);
    if (!ready) {
        out_diag->error_message =
            "ElleKit is unavailable or lacks its marker/libhooker ABI";
        return false;
    }
    if (activated_now) {
        out_diag->observed_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                     HK_EFFECT_PROVIDER_ACTIVATION |
                                     HK_EFFECT_UNKNOWN_PROCESS_MUTATION;
    }
    return true;
}

static int hk_platform_ellekit_hook(void *ctx, void *target,
                                    void *replacement, void **out_original) {
    hk_platform_ellekit_state_t *state = ctx;
    pthread_mutex_lock(&state->lock);
    if (!state->hook_functions) {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }
    struct LHFunctionHook hook = {
        .function = target,
        .replacement = replacement,
        .oldptr = out_original,
        .options = NULL,
    };
    int result = state->hook_functions(&hook, 1);
    pthread_mutex_unlock(&state->lock);
    return result;
}
#endif

typedef struct {
    pthread_mutex_t lock;
    void *handle;
    int (*native_hook)(const struct substitute_function_hook *, size_t,
                       struct substitute_function_hook_record **, int);
    void (*ms_hook)(void *, void *, void **);
} hk_platform_substitute_state_t;

static hk_platform_substitute_state_t g_platform_substitute = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static bool hk_platform_substitute_load_locked(
    hk_platform_substitute_state_t *state) {
    if (state->native_hook || state->ms_hook) {
        return true;
    }
    state->native_hook =
        (int (*)(const struct substitute_function_hook *, size_t,
                struct substitute_function_hook_record **, int))dlsym(
            RTLD_DEFAULT, "substitute_hook_functions");
    state->ms_hook = (void (*)(void *, void *, void **))dlsym(
        RTLD_DEFAULT, "MSHookFunction");
    if (!state->ms_hook) {
        state->ms_hook = (void (*)(void *, void *, void **))dlsym(
            RTLD_DEFAULT, "SubHookFunction");
    }
    if (state->native_hook || state->ms_hook) {
        return true;
    }
    static const char *const paths[] = {
        "/var/jb/usr/lib/libsubstitute.0.dylib",
        "/usr/lib/libsubstitute.0.dylib",
        "/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate",
        "/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        void *handle = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            continue;
        }
        int (*native_hook)(const struct substitute_function_hook *, size_t,
                           struct substitute_function_hook_record **, int) =
            (int (*)(const struct substitute_function_hook *, size_t,
                    struct substitute_function_hook_record **, int))dlsym(
                handle, "substitute_hook_functions");
        void (*ms_hook)(void *, void *, void **) =
            (void (*)(void *, void *, void **))dlsym(handle, "MSHookFunction");
        if (!ms_hook) {
            ms_hook = (void (*)(void *, void *, void **))dlsym(handle,
                                                                 "SubHookFunction");
        }
        if (native_hook || ms_hook) {
            state->handle = handle;
            state->native_hook = native_hook;
            state->ms_hook = ms_hook;
            return true;
        }
        dlclose(handle);
    }
    return false;
}

static bool hk_platform_substitute_discover(void *ctx) {
    hk_platform_substitute_state_t *state = ctx;
    if (!state) {
        return false;
    }
    pthread_mutex_lock(&state->lock);
    bool loaded = state->native_hook || state->ms_hook;
    pthread_mutex_unlock(&state->lock);
    if (loaded) {
        return true;
    }
    return dlsym(RTLD_DEFAULT, "MSHookFunction") != NULL ||
           dlsym(RTLD_DEFAULT, "SubHookFunction") != NULL ||
           dlopen_preflight("/usr/lib/libsubstitute.0.dylib") ||
           dlopen_preflight("/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate");
}

static bool hk_platform_substitute_prepare(void *ctx,
                                           hk_prepare_diag_t *out_diag) {
    hk_platform_substitute_state_t *state = ctx;
    if (!state) {
        out_diag->error_message = "missing Substitute provider state";
        return false;
    }
    pthread_mutex_lock(&state->lock);
    bool was_loaded = state->native_hook || state->ms_hook;
    bool ready = hk_platform_substitute_load_locked(state);
    pthread_mutex_unlock(&state->lock);
    if (!ready) {
        out_diag->error_message =
            "Substitute/Cydia Substrate function-hook ABI is unavailable";
        return false;
    }
    if (!was_loaded) {
        out_diag->observed_effects = HK_EFFECT_PROVIDER_IMAGE_LOAD |
                                    HK_EFFECT_PROVIDER_ACTIVATION |
                                    HK_EFFECT_UNKNOWN_PROCESS_MUTATION;
    }
    return true;
}

static bool hk_platform_substitute_validate(void *ctx,
                                            const hk_hook_spec_t *spec,
                                            hk_prepare_diag_t *out_diag) {
    (void)ctx;
    if (!spec || spec->target.address.address == 0 || !spec->replacement) {
        out_diag->error_message = "invalid Substitute function target";
        return false;
    }
    return true;
}

static bool hk_platform_substitute_read(void *ctx, const void *target,
                                        uint8_t *out, size_t size) {
    (void)ctx;
    if (!target || !out || size == 0 || size > UINT32_MAX) {
        return false;
    }
    uintptr_t address = (uintptr_t)target;
#if defined(__arm__) && !defined(__aarch64__)
    address &= ~(uintptr_t)1u;  // Thumb function-pointer marker.
#endif
    vm_address_t copied = 0;
    mach_msg_type_number_t copied_size = 0;
    kern_return_t result = vm_read(mach_task_self(), (vm_address_t)address,
                                   (vm_size_t)size, &copied, &copied_size);
    if (result != KERN_SUCCESS || !copied || copied_size != size) {
        if (copied) {
            vm_deallocate(mach_task_self(), copied, copied_size);
        }
        return false;
    }
    memcpy(out, (const void *)copied, size);
    vm_deallocate(mach_task_self(), copied, copied_size);
    return true;
}

static int hk_platform_substitute_hook(void *ctx, void *target,
                                       void *replacement, void **out_original) {
    hk_platform_substitute_state_t *state = ctx;
    if (!state) {
        return -1;
    }
    pthread_mutex_lock(&state->lock);
    int (*native_hook)(const struct substitute_function_hook *, size_t,
                       struct substitute_function_hook_record **, int) =
        state->native_hook;
    void (*ms_hook)(void *, void *, void **) = state->ms_hook;
    pthread_mutex_unlock(&state->lock);
    if (native_hook) {
        struct substitute_function_hook hook = {
            .function = target,
            .replacement = replacement,
            .old_ptr = out_original,
            .options = 0,
        };
        return native_hook(&hook, 1, NULL, 0);
    }
    if (!ms_hook) {
        return -1;
    }
    void *probe = NULL;
    void **output = out_original ? out_original : &probe;
    errno = 0;
    ms_hook(target, replacement, output);
    return errno == 0 && *output ? 0 : -1;
}

static void *hk_platform_get_class(void *ctx, const char *name) {
    (void)ctx;
    return (void *)objc_getClass(name);
}

static void *hk_platform_get_metaclass(void *ctx, void *cls) {
    (void)ctx;
    return (void *)object_getClass((id)cls);
}

static void *hk_platform_get_superclass(void *ctx, void *cls) {
    (void)ctx;
    return (void *)class_getSuperclass((Class)cls);
}

static void *hk_platform_register_selector(void *ctx, const char *name) {
    (void)ctx;
    return (void *)sel_registerName(name);
}

static void *hk_platform_get_instance_method(void *ctx, void *cls, void *sel) {
    (void)ctx;
    return (void *)class_getInstanceMethod((Class)cls, (SEL)sel);
}

static void *hk_platform_method_get_imp(void *ctx, void *method) {
    (void)ctx;
    return (void *)method_getImplementation((Method)method);
}

static const char *hk_platform_method_get_types(void *ctx, void *method) {
    (void)ctx;
    return method_getTypeEncoding((Method)method);
}

static void *hk_platform_replace_method(void *ctx, void *cls, void *sel,
                                         void *imp, const char *types) {
    (void)ctx;
    return (void *)class_replaceMethod((Class)cls, (SEL)sel, (IMP)imp, types);
}

static bool hk_platform_write_memory(void *ctx, uintptr_t address,
                                     const uint8_t *data, size_t size) {
    (void)ctx;
    return hk_native_patch_memory((void *)address, data, size);
}

static bool hk_platform_write_pointer(void *ctx, uintptr_t address,
                                      uint64_t value) {
    (void)ctx;
    return hk_native_patch_pointer((void *)address,
                                   (void *)(uintptr_t)value);
}

static uintptr_t hk_platform_reloc_alloc(void *ctx, size_t size, uintptr_t near) {
    (void)ctx;
    return hk_native_reloc_alloc(size, near);
}

static bool hk_platform_reloc_seal(void *ctx, uintptr_t page, size_t size) {
    (void)ctx;
    return hk_native_reloc_seal(page, size);
}

static void hk_platform_reloc_free(void *ctx, uintptr_t page, size_t size) {
    (void)ctx;
    hk_native_reloc_free(page, size);
}
#endif

static void hk_runtime_register_platform_engines(hk_runtime_t *runtime);

static hk_engine_architecture_mask_t hk_runtime_platform_architecture(void) {
#if defined(__arm64e__)
    return HK_ENGINE_ARCHITECTURE_ARM64E;
#elif defined(__arm64__)
    return HK_ENGINE_ARCHITECTURE_ARM64;
#elif defined(__armv7s__) || defined(__ARM_ARCH_7S__)
    return HK_ENGINE_ARCHITECTURE_ARMV7S;
#elif defined(__arm__)
    return HK_ENGINE_ARCHITECTURE_ARMV7;
#else
    return 0;
#endif
}

#if defined(__APPLE__)
static uint32_t hk_runtime_parse_ios_version(const char *value) {
    if (!value || !value[0]) {
        return 0;
    }

    char *end = NULL;
    unsigned long major = strtoul(value, &end, 10);
    if (end == value || major > UINT32_MAX / 10000u) {
        return 0;
    }

    unsigned long minor = 0;
    unsigned long patch = 0;
    if (*end == '.') {
        const char *next = end + 1;
        minor = strtoul(next, &end, 10);
        if (end == next || minor > 99u) {
            return 0;
        }
    }
    if (*end == '.') {
        const char *next = end + 1;
        patch = strtoul(next, &end, 10);
        if (end == next || patch > 99u) {
            return 0;
        }
    }
    if (*end != '\0') {
        return 0;
    }
    return HK_ENGINE_IOS_VERSION(major, minor, patch);
}
#endif

static uint32_t hk_runtime_platform_ios_version(void) {
#if defined(__APPLE__)
    char version[32] = {0};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, NULL, 0) == 0) {
        uint32_t running = hk_runtime_parse_ios_version(version);
        if (running != 0) {
            return running;
        }
    }
#endif
#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED)
    // The deployment target is the conservative fallback for old systems
    // without kern.osproductversion, not evidence of the current OS.
    return __IPHONE_OS_VERSION_MIN_REQUIRED;
#else
    return 0;
#endif
}

// NULL config is a deliberate design choice beyond what the master spec's
// text specifies (it never says whether config is required) -- treated as
// "use every default": no executor (caller must drain_pending()), no
// diagnostics, HK_INSTALL_CONTEXT_EARLY_PROCESS. Makes the common case
// (a plain hk_runtime_create(NULL, &rt)) not require constructing a
// struct just to zero it.
hk_status_t hk_runtime_create(
    const hk_runtime_config_t *config,
    hk_runtime_t **out_runtime)
{
    if (!out_runtime) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    *out_runtime = NULL;

    if (config && config->struct_size < sizeof(hk_runtime_config_t)) {
        // Smaller than this implementation's understanding of
        // hk_runtime_config_t's 3.0.0 shape is malformed, not a partial
        // read to tolerate -- "unknown trailing fields are ignored" only
        // ever means LARGER, never smaller (docs/3.0/PUBLIC_C_ABI.md).
        return HK_STATUS_INVALID_ARGUMENT;
    }

    hk_runtime_t *runtime = (hk_runtime_t *)calloc(1, sizeof(hk_runtime_t));
    if (!runtime) {
        return HK_STATUS_OUT_OF_MEMORY;
    }

    runtime->owner_id = hk_id_generate();
    runtime->platform_architecture = hk_runtime_platform_architecture();
    runtime->platform_ios_version = hk_runtime_platform_ios_version();

    if (config) {
        // struct_size may exceed sizeof(hk_runtime_config_t) (a newer
        // caller with fields this build predates) -- copy only what this
        // implementation knows about, never past its own struct size.
        size_t copy_size = config->struct_size < sizeof(hk_runtime_config_t)
                          ? config->struct_size
                          : sizeof(hk_runtime_config_t);
        memcpy(&runtime->config, config, copy_size);
    }
    runtime->config.struct_size = sizeof(hk_runtime_config_t);
    runtime->config.struct_version = HK_ABI_VERSION_3_0;

    runtime->artifacts = hk_artifact_ledger_create();
    if (!runtime->artifacts) {
        free(runtime);
        return HK_STATUS_OUT_OF_MEMORY;
    }

#if defined(__APPLE__)
    runtime->catalog = hk_image_catalog_create();
    if (!runtime->catalog) {
        hk_artifact_ledger_destroy(runtime->artifacts);
        free(runtime);
        return HK_STATUS_OUT_OF_MEMORY;
    }
#endif

    atomic_init(&runtime->shutdown_called, false);

    hk_runtime_register_platform_engines(runtime);

    *out_runtime = runtime;
    return HK_STATUS_OK;
}

void hk_runtime_shutdown(hk_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    atomic_store_explicit(&runtime->shutdown_called, true, memory_order_release);
}

// Does not generically unhook active targets. Installed hooks outlive the
// runtime wrapper by design, per docs/3.0/PUBLIC_C_ABI.md.
void hk_runtime_release(hk_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    // The deferred queue is the one thing the runtime genuinely owns beyond
    // itself: those hooks were transferred out of released plans, so nobody
    // else can free them. Installed hooks are deliberately NOT touched -- they
    // outlive the runtime wrapper by design.
    hk_runtime_free_pending_hooks(runtime);
#if defined(__APPLE__)
    hk_image_catalog_destroy(runtime->catalog);
#endif
    hk_artifact_ledger_destroy(runtime->artifacts);
    free(runtime);
}

hk_id_t hk_runtime_owner_id(const hk_runtime_t *runtime) {
    if (!runtime) {
        hk_id_t zero;
        zero.high = 0;
        zero.low = 0;
        return zero;
    }
    return runtime->owner_id;
}

bool hk_runtime_append_artifacts(hk_runtime_t *runtime,
                                 const hk_artifact_ledger_t *source) {
    if (!runtime || !runtime->artifacts || !source) {
        return false;
    }
    return hk_artifact_ledger_append_ledger(runtime->artifacts, source);
}

hk_status_t hk_runtime_copy_artifacts(const hk_runtime_t *runtime,
                                      hk_artifact_snapshot_t **out_snapshot) {
    if (out_snapshot) {
        *out_snapshot = NULL;
    }
    if (!runtime || !out_snapshot) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    return hk_artifact_snapshot_from_ledger(runtime->artifacts, out_snapshot);
}

// Not public API -- see HKEngineInternal.h/HKRuntimeInternal.h. `vtable`
// is not owned or copied: the built-in static vtables and test seams both
// outlive the runtime, like a caller-owned hk_hook_spec_t.replacement.
static bool hk_runtime_register_engine(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable,
    void *engine_ctx,
    bool testing_registration)
{
    if (!runtime || !vtable) {
        return false;
    }
    if (vtable->abi_version != 0 || vtable->struct_size != 0) {
        if (vtable->abi_version != HK_ENGINE_VTABLE_ABI_VERSION_1 ||
            vtable->struct_size <
                offsetof(hk_engine_vtable_t, describe) +
                sizeof(vtable->describe)) {
            return false;
        }
    }
    if (runtime->engine_count >= HK_RUNTIME_MAX_ENGINES) {
        return false;
    }
    runtime->engines[runtime->engine_count] = vtable;
    runtime->engine_ctxs[runtime->engine_count] = engine_ctx;
    runtime->engine_testing[runtime->engine_count] = testing_registration;
    runtime->engine_count++;
    return true;
}

bool hk_runtime_register_engine_with_context(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable,
    void *engine_ctx)
{
    return hk_runtime_register_engine(runtime, vtable, engine_ctx, false);
}

bool hk_runtime_register_engine_for_testing(
    hk_runtime_t *runtime,
    const hk_engine_vtable_t *vtable)
{
    return hk_runtime_register_engine(runtime, vtable, NULL, true);
}

static void hk_runtime_register_platform_engines(hk_runtime_t *runtime) {
#if defined(__APPLE__)
    runtime->objc_engine.runtime.get_class = hk_platform_get_class;
    runtime->objc_engine.runtime.get_metaclass = hk_platform_get_metaclass;
    runtime->objc_engine.runtime.get_superclass = hk_platform_get_superclass;
    runtime->objc_engine.runtime.register_selector = hk_platform_register_selector;
    runtime->objc_engine.runtime.get_instance_method = hk_platform_get_instance_method;
    runtime->objc_engine.runtime.method_get_imp = hk_platform_method_get_imp;
    runtime->objc_engine.runtime.method_get_types = hk_platform_method_get_types;
    runtime->objc_engine.runtime.replace_method = hk_platform_replace_method;
    runtime->objc_engine.runtime.ctx = NULL;
    (void)hk_runtime_register_engine_with_context(runtime,
                                                   hk_objc_vtable(),
                                                   &runtime->objc_engine);

    (void)hk_image_catalog_populate_from_dyld(runtime->catalog);

    if (hk_native_supported()) {
        runtime->rebind_engine.image_base = NULL;
        runtime->rebind_engine.image_size = 0;
        runtime->rebind_engine.slide = 0;
        runtime->rebind_engine.write = hk_platform_write_pointer;
        runtime->rebind_engine.write_ctx = NULL;
        runtime->rebind_engine.catalog = runtime->catalog;
        (void)hk_runtime_register_engine_with_context(runtime,
                                                       hk_rebind_vtable(),
                                                       &runtime->rebind_engine);

        runtime->memory_engine.write = hk_platform_write_memory;
        runtime->memory_engine.write_ctx = NULL;
        runtime->memory_engine.catalog = runtime->catalog;

        runtime->inline_engine.write = hk_platform_write_memory;
        runtime->inline_engine.write_ctx = NULL;
        runtime->inline_engine.catalog = runtime->catalog;
        runtime->inline_engine.allow_non_atomic_entry_patch = false;

        runtime->reloc_engine.alloc = hk_platform_reloc_alloc;
        runtime->reloc_engine.seal = hk_platform_reloc_seal;
        runtime->reloc_engine.free_page = hk_platform_reloc_free;
        runtime->reloc_engine.seam_ctx = NULL;
        runtime->reloc_engine.write = hk_platform_write_memory;
        runtime->reloc_engine.write_ctx = NULL;
        runtime->reloc_engine.catalog = runtime->catalog;
        runtime->reloc_engine.allow_non_atomic_entry_patch = false;

        (void)hk_runtime_register_engine_with_context(runtime,
                                                       hk_memory_vtable(),
                                                       &runtime->memory_engine);
#if defined(HOOKKIT_CANONICAL_3) && \
    (defined(__arm64__) || defined(__arm64e__)) && !defined(HK_NO_DOBBY)
        // Dobby and Gum expose the same plain-C hook ABI on both AArch64
        // slices. The arm64e slice is built with the modern ptrauth ABI and
        // uses the same PAC-aware preflight/pointer paths as native engines.
        memset(&runtime->dobby_provider, 0, sizeof(runtime->dobby_provider));
        runtime->dobby_provider.kind = HK_PROVIDER_DOBBY;
        runtime->dobby_provider.validate = hk_platform_dobby_validate;
        runtime->dobby_provider.hook = hk_platform_dobby_hook;
        (void)hk_runtime_register_engine_with_context(runtime,
                                                       hk_dobby_provider_vtable(),
                                                       &runtime->dobby_provider);

        memset(&runtime->gum_provider, 0, sizeof(runtime->gum_provider));
        runtime->gum_provider.kind = HK_PROVIDER_GUM;
        runtime->gum_provider.provider_ctx = &g_platform_gum;
        runtime->gum_provider.discover = hk_platform_gum_discover;
        runtime->gum_provider.prepare = hk_platform_gum_prepare;
        runtime->gum_provider.validate = hk_platform_gum_validate;
        runtime->gum_provider.hook = hk_platform_gum_hook;
        (void)hk_runtime_register_engine_with_context(runtime,
                                                       hk_gum_provider_vtable(),
                                                       &runtime->gum_provider);

        memset(&runtime->ellekit_provider, 0, sizeof(runtime->ellekit_provider));
        runtime->ellekit_provider.kind = HK_PROVIDER_ELLEKIT;
        runtime->ellekit_provider.provider_ctx = &g_platform_ellekit;
        runtime->ellekit_provider.discover = hk_platform_ellekit_discover;
        runtime->ellekit_provider.prepare = hk_platform_ellekit_prepare;
        runtime->ellekit_provider.validate = hk_platform_ellekit_validate;
        runtime->ellekit_provider.hook = hk_platform_ellekit_hook;
        // ElleKit's AArch64 patch forms are 4-byte B/BRK, a 12-byte
        // ADRP/MOVK/BR sequence, or a 16-byte literal branch. Relocate the
        // audited maximum; a provider without a proven ceiling is never
        // configured for the hybrid path.
        runtime->ellekit_provider.max_overwrite_size = 16;
        runtime->ellekit_provider.alloc = hk_platform_reloc_alloc;
        runtime->ellekit_provider.seal = hk_platform_reloc_seal;
        runtime->ellekit_provider.free_page = hk_platform_reloc_free;
        (void)hk_runtime_register_engine_with_context(
            runtime, hk_ellekit_provider_vtable(),
            &runtime->ellekit_provider);
#endif
        (void)hk_runtime_register_engine_with_context(runtime,
                                                       hk_inline_vtable(),
                                                       &runtime->inline_engine);
        (void)hk_runtime_register_engine_with_context(runtime,
                                                       hk_reloc_inline_vtable(),
                                                       &runtime->reloc_engine);
    }
#if defined(HOOKKIT_CANONICAL_3)
    // Older devices rely on the installed provider's relocator. This remains
    // an HookKit engine lifecycle, not a re-entry into the retired 2.x router.
    memset(&runtime->substitute_provider, 0,
           sizeof(runtime->substitute_provider));
    runtime->substitute_provider.kind = HK_PROVIDER_SUBSTITUTE;
    runtime->substitute_provider.provider_ctx = &g_platform_substitute;
    runtime->substitute_provider.discover = hk_platform_substitute_discover;
    runtime->substitute_provider.prepare = hk_platform_substitute_prepare;
    runtime->substitute_provider.validate = hk_platform_substitute_validate;
    runtime->substitute_provider.hook = hk_platform_substitute_hook;
    runtime->substitute_provider.read = hk_platform_substitute_read;
    (void)hk_runtime_register_engine_with_context(
        runtime, hk_substitute_provider_vtable(),
        &runtime->substitute_provider);
#endif
#else
    (void)runtime;
#endif
}

// hk_runtime_drain_pending now lives in HKPlan.c, next to the per-hook
// engine dispatch it shares with hk_plan_prepare/commit -- a retry must
// take the same path a first attempt takes.

// hk_report_release now lives in Sources/Core/HKReport.c, where
// hk_report_t's concrete definition does. It used to be a permanent-
// looking no-op here (every report was NULL because nothing produced one
// yet); hk_plan_analyze is the first real producer.
