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
#include "../../native/hk_arm64.h"

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
#include <TargetConditionals.h>

#include "../../vendor/substitute/substitute.h"
#if defined(HOOKKIT_CANONICAL_3) && \
    (defined(__arm64__) || defined(__arm64e__)) && !defined(HK_NO_DOBBY)
#include "../../vendor/dobby/dobby.h"
#include "../../vendor/libhooker/libhooker.h"

// HKInlinePreflight.h is Objective-C-facing because provider headers import
// Foundation. The two C entry points below have no ObjC types; declare that
// tiny internal seam here so the C-first runtime never imports Foundation
// merely to read a function prologue.
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
    // dlopen_preflight validates code signatures and costs ~0.3 ms per call
    // on an arm64 test device during profiling. These paths are static files: their
    // preflight verdict cannot change while the process runs, so memoize.
    // Without this, every hook re-pays discovery for every provider engine
    // during plan analysis (~470 us/hook combined across providers).
    static int cached = -1; // -1 unknown, 0 unavailable, 1 available
    static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
    if (cached < 0) {
        pthread_mutex_lock(&cache_lock);
        if (cached < 0) {
            cached = (dlopen_preflight("/var/jb/usr/lib/HKGum.dylib") ||
                      dlopen_preflight("/usr/lib/HKGum.dylib"))
                         ? 1
                         : 0;
        }
        pthread_mutex_unlock(&cache_lock);
    }
    return cached != 0;
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
    // Memoized for the same reason as hk_platform_gum_discover: four
    // dlopen_preflight signature validations per call dominate plan analysis
    // if repeated per hook.
    static int cached = -1; // -1 unknown, 0 unavailable, 1 available
    static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
    if (cached < 0) {
        pthread_mutex_lock(&cache_lock);
        if (cached < 0) {
            static const char *const paths[] = {
                "/var/jb/usr/lib/libellekit.dylib",
                "/usr/lib/libellekit.dylib",
                "/var/jb/usr/lib/libhooker.dylib",
                "/usr/lib/libhooker.dylib",
            };
            cached = 0;
            for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
                if (dlopen_preflight(paths[i])) {
                    cached = 1;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&cache_lock);
    }
    return cached != 0;
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

#if defined(HOOKKIT_CANONICAL_3)
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
    if (dlsym(RTLD_DEFAULT, "MSHookFunction") != NULL ||
        dlsym(RTLD_DEFAULT, "SubHookFunction") != NULL) {
        return true;
    }
    // The preflight pair is memoized (see hk_platform_gum_discover): the
    // files are static, so their verdict cannot change mid-process. The
    // dlsym checks above stay live on purpose -- a late dlopen of Substitute
    // must still flip discovery to available.
    static int cached = -1; // -1 unknown, 0 unavailable, 1 available
    static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
    if (cached < 0) {
        pthread_mutex_lock(&cache_lock);
        if (cached < 0) {
            cached = (dlopen_preflight("/usr/lib/libsubstitute.0.dylib") ||
                      dlopen_preflight("/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate"))
                         ? 1
                         : 0;
        }
        pthread_mutex_unlock(&cache_lock);
    }
    return cached != 0;
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
#endif

// Every function from here to the matching #endif below is called only
// from hk_runtime_register_platform_engines's TARGET_OS_IOS-gated section
// (this same file) -- see the comment there for why.
#if TARGET_OS_IOS
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

// Static-continuation backing: a fixed executable region carved into the
// HookKit image's own __TEXT. A continuation built here lives inside an
// already-mapped, code-signed executable region rather than a fresh anonymous
// page -- removing the anonymous-executable signal a hooking scan keys on. The
// relocating engine prefers it by DEFAULT (see hk_platform_reloc_alloc), so
// the stealthy continuation needs no caller opt-in.
//
// Slots are ONE PAGE each, not packed: seal/unprotect act at page granularity,
// so two continuations sharing a page would flip each other's protection --
// making a sealed, possibly-executing slot writable (a W^X fault). Page
// isolation is a correctness requirement here, not waste.
//
// Trades: the section is (slots x page) bytes of real, file-backed dylib size
// (a page-granular pool cannot be zero-fill in __TEXT without a custom linker
// segment); writing a slot dirties a signed __TEXT page (an integrity check vs
// disk still sees the divergence); and the fixed location means only a target
// within a B's +/-128MB gets the pool -- a far one falls to a near page, so the
// pool mostly serves near targets. SLOTS is deliberately small to bound the
// size cost; raise it if a consumer installs more concurrent near hooks.
#define HK_STATIC_TRAMP_PAGE  16384u  // iOS arm64 kernel page
#define HK_STATIC_TRAMP_SLOTS 8u
// const with an explicit initializer, deliberately: it forces the section to
// be emitted as file-backed, mapped-executable __TEXT rather than zerofill --
// a zerofill section is not executable and vm_protect on it fails. The bytes
// are written at hook time through slot pointers (after unprotect), never
// through this symbol, so the const does not obstruct the build.
__attribute__((section("__TEXT,__hktramp"), used, aligned(HK_STATIC_TRAMP_PAGE)))
static const uint8_t g_hk_static_tramp[HK_STATIC_TRAMP_SLOTS * HK_STATIC_TRAMP_PAGE] = {0};

// One pool per PROCESS, not per runtime: chained owners run separate runtimes
// in one process and all share this single fixed section, so a per-runtime
// pool would hand the same slots out twice. The mutex guards the slot bitmap;
// the vm ops below touch distinct slots and need no lock.
static hk_static_pool_t g_hk_static_pool;
static pthread_mutex_t g_hk_static_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_hk_static_pool_once = PTHREAD_ONCE_INIT;

static void hk_static_pool_setup(void) {
    // Slot size is a full page so each slot's seal/unprotect is page-isolated;
    // the engine only uses the first HK_RELOC_PAGE_BYTES of it.
    (void)hk_static_pool_init(&g_hk_static_pool, (uintptr_t)g_hk_static_tramp,
                              HK_STATIC_TRAMP_PAGE, HK_STATIC_TRAMP_SLOTS);
}

static void hk_static_pool_return(uintptr_t slot) {
    pthread_mutex_lock(&g_hk_static_pool_lock);
    hk_static_pool_release(&g_hk_static_pool, slot);
    pthread_mutex_unlock(&g_hk_static_pool_lock);
}

// Claim a writable pool slot, or 0. Pool slots arrive R-X (mapped at load), so
// unlike a fresh R-W vm_allocate page they must be unprotected before the
// engine builds in them; seal restores R-X. `require_reach` gates the claim on
// the slot being within a 4-byte B of `near`: the default engine wants that (an
// out-of-range slot would force the non-atomic entry patch it refuses) and can
// fall back to a near page, while the forbid-dynamic engine passes false and
// serves near targets only, failing honestly on a far one rather than allocating.
static uintptr_t hk_static_pool_take(size_t size, uintptr_t near,
                                     bool require_reach) {
    pthread_once(&g_hk_static_pool_once, hk_static_pool_setup);
    pthread_mutex_lock(&g_hk_static_pool_lock);
    uintptr_t slot = hk_static_pool_claim(&g_hk_static_pool, size, near);
    pthread_mutex_unlock(&g_hk_static_pool_lock);
    if (slot == 0) {
        return 0;  // exhausted -- an honest fixed-budget outcome
    }
    // hk_arm64_branch_size is the engine's own reach test, so the gate can
    // never disagree with what the engine will accept at prepare.
    if ((require_reach && hk_arm64_branch_size((uint64_t)near, (uint64_t)slot) != 4) ||
        !hk_native_reloc_unprotect(slot, size)) {
        hk_static_pool_return(slot);
        return 0;
    }
    return slot;
}

// Default relocating-inline backing: prefer the in-image pool -- no anonymous
// executable region for a hooking scan to flag -- whenever it can serve this
// target atomically, and fall back to a near, tagged anonymous page otherwise.
// This makes the stealthy continuation the default with no caller opt-in.
static uintptr_t hk_platform_reloc_alloc(void *ctx, size_t size, uintptr_t near) {
    (void)ctx;
    uintptr_t slot = hk_static_pool_take(size, near, true);
    if (slot) {
        return slot;
    }
    return hk_native_reloc_alloc(size, near);
}

static bool hk_platform_reloc_seal(void *ctx, uintptr_t page, size_t size) {
    (void)ctx;
    return hk_native_reloc_seal(page, size);  // R-X + icache flush, pool or page
}

// One free seam for both origins: a pool slot is permanent section memory
// returned to the bitmap; anything else was vm_allocated and is deallocated.
// Only an unpublished trampoline ever reaches here.
static void hk_platform_reloc_free(void *ctx, uintptr_t page, size_t size) {
    (void)ctx;
    pthread_once(&g_hk_static_pool_once, hk_static_pool_setup);
    if (hk_static_pool_contains(&g_hk_static_pool, page)) {
        hk_static_pool_return(page);
        return;
    }
    hk_native_reloc_free(page, size);
}

// Forbid-dynamic backing: the same pool, but pool-only -- no anonymous
// fallback, because the request refused dynamic executable memory outright. A
// far or exhausted pool fails the hook honestly rather than allocating.
static uintptr_t hk_platform_static_alloc(void *ctx, size_t size, uintptr_t near) {
    (void)ctx;
    return hk_static_pool_take(size, near, false);
}
#endif
#endif

static void hk_runtime_register_platform_engines(hk_runtime_t *runtime);

static hk_engine_architecture_mask_t hk_runtime_platform_architecture(void) {
    // hk_engine_supports_platform (HKEngineInternal.h) treats architecture
    // == 0 as "host test build" and waives certification for it. Apple
    // clang defines __arm64__ for a native macOS-on-Apple-Silicon compile
    // (these host tests) exactly as it does for a real iOS target, so
    // that alone can't tell the two apart -- TARGET_OS_IOS can, same as
    // hk_runtime_register_platform_engines above.
#if defined(__APPLE__) && TARGET_OS_IOS
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
static hk_status_t hk_runtime_create_impl(
    const hk_runtime_config_t *config,
    const char *backend_override,
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
    if (backend_override) {
        hk_runtime_apply_backend_override(runtime, backend_override);
    }

    *out_runtime = runtime;
    return HK_STATUS_OK;
}

hk_status_t hk_runtime_create(
    const hk_runtime_config_t *config,
    hk_runtime_t **out_runtime)
{
    return hk_runtime_create_impl(config, NULL, out_runtime);
}

hk_status_t hk_runtime_create_with_backend_override(
    const hk_runtime_config_t *config,
    const char *backend_ids,
    hk_runtime_t **out_runtime)
{
    return hk_runtime_create_impl(config, backend_ids, out_runtime);
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

static bool hk_runtime_vtable_has_field(const hk_engine_vtable_t *vtable,
                                        size_t offset, size_t size) {
    if (!vtable) {
        return false;
    }
    return (vtable->abi_version == 0 && vtable->struct_size == 0) ||
           vtable->struct_size >= offset + size;
}

// The selectable group token an engine belongs to. Falls back to engine_id
// so an engine that declares no group enumerates and pins as itself.
static const char *hk_engine_group_token(const hk_engine_capabilities_t *caps) {
    return caps->backend_group && caps->backend_group[0]
               ? caps->backend_group
               : caps->engine_id;
}

hk_status_t hk_runtime_enumerate_backends(
    hk_runtime_t *runtime,
    hk_backend_enumerator_fn enumerator,
    void *context)
{
    if (!runtime || !enumerator) {
        return HK_STATUS_INVALID_ARGUMENT;
    }
    const char *seen[HK_RUNTIME_MAX_ENGINES];
    size_t seen_count = 0;
    for (size_t i = 0; i < runtime->engine_count; i++) {
        const hk_engine_vtable_t *engine = runtime->engines[i];
        if (!engine || !engine->describe) {
            continue;
        }
        hk_engine_capabilities_t capabilities = engine->describe();
        if (!capabilities.engine_id || !capabilities.engine_id[0] ||
            !hk_engine_supports_platform(&capabilities,
                                         runtime->platform_architecture,
                                         runtime->platform_ios_version,
                                         runtime->engine_testing[i])) {
            continue;
        }
        if (hk_runtime_vtable_has_field(
                engine, offsetof(hk_engine_vtable_t, discover),
                sizeof(engine->discover)) && engine->discover) {
            hk_engine_discovery_t discovery;
            memset(&discovery, 0, sizeof(discovery));
            if (!engine->discover(runtime->engine_ctxs[i], &discovery) ||
                !discovery.available) {
                continue;
            }
        }
        const char *group = hk_engine_group_token(&capabilities);
        bool already = false;
        for (size_t s = 0; s < seen_count; s++) {
            if (strcmp(seen[s], group) == 0) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        seen[seen_count++] = group;
        const char *label = capabilities.display_name
                                ? capabilities.display_name
                                : group;
        hk_string_view_t backend_id = { .data = group, .length = strlen(group) };
        hk_string_view_t display_name = {
            .data = label,
            .length = strlen(label),
        };
        if (!enumerator(context, backend_id, display_name)) {
            break;
        }
    }
    return HK_STATUS_OK;
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
    // Every engine registered below assumes it is patching/introspecting a
    // live iOS process. __APPLE__ is also true when this file is compiled
    // as a plain macOS host binary (e.g. Tests/Host/*.c, which run on this
    // very function) -- TARGET_OS_IOS distinguishes that case, where none
    // of these engines have a real target and must not auto-claim one out
    // from under a test's own fakes (Tests/Host/fake_engines.h) registered
    // right after hk_runtime_create returns. Only dyld catalog population
    // just below is genuinely platform-neutral and stays unconditional.
#if TARGET_OS_IOS
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
#endif

    (void)hk_image_catalog_populate_from_dyld(runtime->catalog);

#if TARGET_OS_IOS
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

        // The same engine backed by the fixed __TEXT pool. Registered AFTER
        // the dynamic one so an unconstrained request still prefers the
        // dynamic page (equal reach/rank -> first registered wins): the router
        // picks ONE engine and does not fall back at prepare, so preempting
        // here would fail every hook once the fixed pool exhausts. This engine
        // is instead reached only when the request forbids dynamic executable
        // memory, which filters the dynamic one out and leaves this eligible.
        pthread_once(&g_hk_static_pool_once, hk_static_pool_setup);
        runtime->static_engine = runtime->reloc_engine;
        runtime->static_engine.alloc = hk_platform_static_alloc;  // pool-only, no anon
        runtime->static_engine.seal = hk_platform_reloc_seal;     // shared (pool or page)
        runtime->static_engine.free_page = hk_platform_reloc_free;  // address-discriminated
        runtime->static_engine.seam_ctx = NULL;  // seams use the process-global pool
        runtime->static_engine.static_continuation = true;
        (void)hk_runtime_register_engine_with_context(
            runtime, hk_static_inline_vtable(), &runtime->static_engine);
    }
#endif
#if defined(HOOKKIT_CANONICAL_3)
    // Older devices rely on the installed provider's relocator within the
    // same HookKit engine lifecycle.
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

// --- Explicit backend selection ------------------------------------------
// Strict C11 on purpose (no strtok_r/strdup/strcasecmp) so the -std=c11
// -Werror host tests compile it unchanged.

#define HK_BACKEND_UNRANKED (1L << 30)  // sorts after any real token index

// Case-insensitive equality of the span text[0..len) against C-string id.
static bool hk_ci_span_eq(const char *text, size_t len, const char *id) {
    size_t i = 0;
    for (; i < len && id[i]; i++) {
        char a = text[i], b = id[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) {
            return false;
        }
    }
    return i == len && id[i] == '\0';
}

// A token matches an engine_id exactly, or with the "provider-" prefix
// omitted, so "ellekit" selects "provider-ellekit".
static bool hk_backend_span_matches(const char *tok, size_t len, const char *id) {
    if (!id) {
        return false;
    }
    if (hk_ci_span_eq(tok, len, id)) {
        return true;
    }
    static const char prefix[] = "provider-";
    size_t pn = sizeof(prefix) - 1;
    return strncmp(id, prefix, pn) == 0 && hk_ci_span_eq(tok, len, id + pn);
}

// 0-based index of the first comma/space-separated token in `list` matching
// `id`, or -1 if none. Read-only walk -- no copy, no mutation.
static int hk_backend_token_index(const char *list, const char *id) {
    if (!list || !id) {
        return -1;
    }
    int idx = 0;
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len && hk_backend_span_matches(start, len, id)) {
            return idx;
        }
        idx++;
    }
    return -1;
}

// Lowest list position at which an engine is named, by its group token or its
// own engine_id (whichever appears earlier). -1 when the list names neither.
static int hk_engine_token_rank(const hk_engine_vtable_t *engine,
                                const char *list) {
    if (!engine || !engine->describe) {
        return -1;
    }
    hk_engine_capabilities_t caps = engine->describe();
    if (!caps.engine_id) {
        return -1;
    }
    int a = hk_backend_token_index(list, hk_engine_group_token(&caps));
    int b = hk_backend_token_index(list, caps.engine_id);
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

static void hk_runtime_retain_ordered_engines(hk_runtime_t *runtime,
                                              const bool *drop,
                                              const char *order) {
    size_t n = runtime->engine_count;
    size_t idx_order[HK_RUNTIME_MAX_ENGINES];
    long keys[HK_RUNTIME_MAX_ENGINES];
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (drop[i]) {
            continue;
        }
        int rank = order ? hk_engine_token_rank(runtime->engines[i], order) : -1;
        idx_order[kept] = i;
        keys[kept] = rank >= 0 ? (long)rank : HK_BACKEND_UNRANKED + (long)i;
        kept++;
    }
    for (size_t a = 1; a < kept; a++) {  // stable insertion sort, n <= 16
        size_t vi = idx_order[a];
        long vk = keys[a];
        size_t b = a;
        while (b > 0 && keys[b - 1] > vk) {
            idx_order[b] = idx_order[b - 1];
            keys[b] = keys[b - 1];
            b--;
        }
        idx_order[b] = vi;
        keys[b] = vk;
    }

    const hk_engine_vtable_t *new_engines[HK_RUNTIME_MAX_ENGINES];
    void *new_ctxs[HK_RUNTIME_MAX_ENGINES];
    bool new_testing[HK_RUNTIME_MAX_ENGINES];
    for (size_t k = 0; k < kept; k++) {
        size_t src = idx_order[k];
        new_engines[k] = runtime->engines[src];
        new_ctxs[k] = runtime->engine_ctxs[src];
        new_testing[k] = runtime->engine_testing[src];
    }
    for (size_t k = 0; k < kept; k++) {
        runtime->engines[k] = new_engines[k];
        runtime->engine_ctxs[k] = new_ctxs[k];
        runtime->engine_testing[k] = new_testing[k];
    }
    runtime->engine_count = kept;
}

void hk_runtime_apply_backend_override(hk_runtime_t *runtime,
                                       const char *backend_ids) {
    if (!runtime || runtime->engine_count == 0 || !backend_ids) {
        return;
    }
    bool drop[HK_RUNTIME_MAX_ENGINES];
    for (size_t i = 0; i < runtime->engine_count; i++) {
        const hk_engine_vtable_t *engine = runtime->engines[i];
        const char *id = engine && engine->describe
            ? engine->describe().engine_id : NULL;
        // ObjC messages are a facade-native operation, never a selected
        // provider route, so an explicit function backend must not disable it.
        drop[i] = !id || (strcmp(id, "objc") != 0 &&
                          hk_engine_token_rank(engine, backend_ids) < 0);
    }
    hk_runtime_retain_ordered_engines(runtime, drop, backend_ids);
}

// hk_runtime_drain_pending now lives in HKPlan.c, next to the per-hook
// engine dispatch it shares with hk_plan_prepare/commit -- a retry must
// take the same path a first attempt takes.

// hk_report_release now lives in Sources/Core/HKReport.c, where
// hk_report_t's concrete definition does. It used to be a permanent-
// looking no-op here (every report was NULL because nothing produced one
// yet); hk_plan_analyze is the first real producer.
