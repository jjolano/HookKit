// HookKit 3.0 — reversible compat shim for raw hooking calls.
// Replaces substrate / substitute / libhooker / ElleKit entry points with
// HookKit 3 plans. Include BEFORE the provider header so macros win. Remove
// the import or define HOOKKIT_COMPAT_HIJACK=0 to revert to the original lib.
//
// Pure %hook tweaks need no header — the hookkit Logos generator handles
// %hook/%orig directly. This header is only for tweaks that call
// MSHookFunction / substitute_hook_functions / LHHookFunctions / LBHookMessage
// / MSHookMemory / LHPatchMemory in %ctor.
//
// void-natural: MSHookFunction etc remain void (CydiaSubstrate.h:89) so
// existing code compiles. Use hk_compat_*_int variants for int checking.

#ifndef HOOKKIT_COMPAT_H
#define HOOKKIT_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "HookKitBase.h"
#include "HookKitRuntime.h"
#include "HookKitPlan.h"
#include "HookKitTargets.h"
#include "HookKitResults.h"

#ifndef HOOKKIT_COMPAT_SUBSTRATE
#define HOOKKIT_COMPAT_SUBSTRATE 1
#endif
#ifndef HOOKKIT_COMPAT_SUBSTITUTE
#define HOOKKIT_COMPAT_SUBSTITUTE 1
#endif
#ifndef HOOKKIT_COMPAT_LIBHOOKER
#define HOOKKIT_COMPAT_LIBHOOKER 1
#endif
#ifndef HOOKKIT_COMPAT_MEMORY
#define HOOKKIT_COMPAT_MEMORY 1
#endif
#ifndef HOOKKIT_COMPAT_HIJACK
#define HOOKKIT_COMPAT_HIJACK 1
#endif

static __attribute__((unused)) hk_runtime_t *_hk_compat_rt_storage = NULL;
static void _hk_compat_rt_init(void) {
    hk_runtime_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.struct_version = HK_ABI_VERSION_3_0;
    cfg.install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS;
    (void)hk_runtime_create(&cfg, &_hk_compat_rt_storage);
}
// Runtime singleton — early-process, lazy.
static inline hk_runtime_t *_hk_compat_runtime(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)pthread_once(&once, _hk_compat_rt_init);
    return _hk_compat_rt_storage;
}

// One hook via ephemeral plan — void-natural wrapper discards status.
static inline __attribute__((unused)) int _hk_compat_hook_one(hk_hook_spec_t *spec, void **out_orig) {
    hk_runtime_t *rt = _hk_compat_runtime();
    if (!rt) return -1;
    hk_plan_t *plan = NULL;
    if (hk_plan_create(rt, NULL, &plan) != HK_STATUS_OK || !plan) return -1;
    hk_hook_t *hook = NULL;
    if (hk_plan_add_hook(plan, spec, &hook) != HK_STATUS_OK) { hk_plan_release(plan); return -1; }
    (void)hk_plan_analyze(plan, NULL);
    (void)hk_plan_prepare(plan, NULL);
    (void)hk_plan_commit(plan, NULL);
    int ok = 0;
    if (hook) {
        hk_hook_result_t r;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        r.struct_version = HK_ABI_VERSION_3_0;
        if (hk_hook_copy_result(hook, &r) == HK_STATUS_OK) {
            ok = (r.outcome == HK_OUTCOME_ACTIVE || r.outcome == HK_OUTCOME_ALREADY_ACTIVE) ? 0 : -1;
            if (ok == 0 && out_orig) {
                void *o = hk_original_slot_load(hk_hook_original_slot(hook));
                if (o) *out_orig = o;
            }
        } else {
            ok = -1;
        }
    } else {
        ok = -1;
    }
    hk_plan_release(plan);
    return ok;
}

// Batch helper — N hooks in one plan for LHHookFunctions/substitute_hook_functions.
static inline __attribute__((unused)) int _hk_compat_hook_batch(hk_hook_spec_t *specs, void **const *origs, size_t n) {
    if (n == 0) return 0;
    hk_runtime_t *rt = _hk_compat_runtime();
    if (!rt) return -1;
    hk_plan_t *plan = NULL;
    if (hk_plan_create(rt, NULL, &plan) != HK_STATUS_OK || !plan) return -1;
    hk_hook_t **hooks = (hk_hook_t **)calloc(n, sizeof(hk_hook_t *));
    if (!hooks) { hk_plan_release(plan); return -1; }
    char idbuf[32];
    for (size_t i = 0; i < n; i++) {
        // stable_hook_id must be unique — caller fills it; if empty synthesize
        if (!specs[i].stable_hook_id || !specs[i].stable_hook_id[0]) {
            snprintf(idbuf, sizeof(idbuf), "compat.batch.%zu.%p", i, (void*)&specs[i]);
            specs[i].stable_hook_id = idbuf; // deep-copied by hk_plan_add_hook
        }
        if (hk_plan_add_hook(plan, &specs[i], &hooks[i]) != HK_STATUS_OK) {
            // keep going — mimic libhooker partial success semantics
            hooks[i] = NULL;
        }
    }
    (void)hk_plan_analyze(plan, NULL);
    (void)hk_plan_prepare(plan, NULL);
    (void)hk_plan_commit(plan, NULL);
    int ok_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (!hooks[i]) continue;
        hk_hook_result_t r;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        r.struct_version = HK_ABI_VERSION_3_0;
        if (hk_hook_copy_result(hooks[i], &r) == HK_STATUS_OK &&
            (r.outcome == HK_OUTCOME_ACTIVE || r.outcome == HK_OUTCOME_ALREADY_ACTIVE)) {
            ok_count++;
            if (origs && origs[i]) {
                void *o = hk_original_slot_load(hk_hook_original_slot(hooks[i]));
                if (o) *(void**)origs[i] = o;
            }
        }
    }
    free(hooks);
    hk_plan_release(plan);
    return ok_count;
}

// Helpers to build specs.

static inline __attribute__((unused)) void _hk_compat_make_function_spec(hk_hook_spec_t *out, const char *sid, void *func, void *rep) {
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->struct_version = HK_ABI_VERSION_3_0;
    out->stable_hook_id = sid ? sid : "compat.func";
    out->target_kind = HK_TARGET_FUNCTION_ADDRESS;
    out->target.address.struct_size = sizeof(out->target.address);
    out->target.address.struct_version = HK_ABI_VERSION_3_0;
    out->target.address.address = (uintptr_t)func;
    out->replacement = rep;
    out->required_reach = HK_REACH_ENTRYPOINT;
    out->original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    out->continuation_policy = HK_CONTINUATION_ANY;
    out->availability = HK_AVAILABILITY_REQUIRED_NOW;
    out->role = HK_OPERATION_MANDATORY;
}

static inline __attribute__((unused)) void _hk_compat_make_objc_spec(hk_hook_spec_t *out, const char *sid, void *cls, void *sel, void *rep, bool is_meta) {
    (void)is_meta;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->struct_version = HK_ABI_VERSION_3_0;
    out->stable_hook_id = sid ? sid : "compat.objc";
    out->target_kind = HK_TARGET_OBJC_METHOD;
    out->target.objc.struct_size = sizeof(out->target.objc);
    out->target.objc.struct_version = HK_ABI_VERSION_3_0;
    out->target.objc.cls = cls;
    out->target.objc.sel = sel;
    out->target.objc.method_kind = is_meta ? HK_OBJC_CLASS_METHOD : HK_OBJC_INSTANCE_METHOD;
    out->target.objc.inheritance_policy = HK_OBJC_LOCAL_METHOD_ONLY;
    out->target.objc.availability = HK_AVAILABILITY_REQUIRED_NOW;
    out->replacement = rep;
    out->required_reach = HK_REACH_OBJC_DISPATCH;
    out->availability = HK_AVAILABILITY_REQUIRED_NOW;
    out->role = HK_OPERATION_MANDATORY;
}

static inline __attribute__((unused)) void _hk_compat_make_memory_spec(hk_hook_spec_t *out, const char *sid, void *dst, const void *data, size_t sz) {
    // Memory target requires expected_bytes — read current bytes as expected.
    // Allocated buffer is owned by the hook after deep-copy, so free after add.
    uint8_t *expected = NULL;
    if (sz > 0) {
        expected = (uint8_t *)malloc(sz);
        if (expected) {
            // Best-effort read; if fails, expected stays 0-filled which will still be checked at commit
            // Use memmove-equivalent via direct read — HookKit will revalidate anyway.
            // On iOS this is readable; on host tests it succeeds.
            __builtin_memcpy(expected, dst, sz);
            // If dst is not readable, keep zeros — commit will fail safely with HK_OUTCOME_FAILED_SAFE
        }
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->struct_version = HK_ABI_VERSION_3_0;
    out->stable_hook_id = sid ? sid : "compat.mem";
    out->target_kind = HK_TARGET_MEMORY_PATCH;
    out->target.memory.struct_size = sizeof(out->target.memory);
    out->target.memory.struct_version = HK_ABI_VERSION_3_0;
    out->target.memory.address = (uintptr_t)dst;
    out->target.memory.address_is_image_relative = false;
    out->target.memory.replacement_bytes.data = (const uint8_t *)data;
    out->target.memory.replacement_bytes.size = sz;
    out->target.memory.expected_bytes.data = expected;
    out->target.memory.expected_bytes.size = expected ? sz : 0;
    out->target.memory.size = sz;
    out->target.memory.kind = HK_MEMORY_KIND_DATA;
    out->replacement = NULL;
    out->required_reach = HK_REACH_EXACT_MEMORY;
    out->availability = HK_AVAILABILITY_REQUIRED_NOW;
    out->role = HK_OPERATION_MANDATORY;
}

// Substrate: void MSHookFunction(void *sym, void *rep, void **out)
// int variant: 0 ok, -1 fail
static inline __attribute__((unused)) int hk_compat_MSHookFunction_int(void *sym, void *rep, void **out) {
    if (!sym || !rep) return -1;
    hk_hook_spec_t spec;
    _hk_compat_make_function_spec(&spec, "compat.substrate.func", sym, rep);
    return _hk_compat_hook_one(&spec, out);
}
static inline __attribute__((unused)) void hk_compat_MSHookFunction(void *sym, void *rep, void **out) {
    (void)hk_compat_MSHookFunction_int(sym, rep, out);
}
static inline __attribute__((unused)) int hk_compat_MSHookMessageEx_int(void *cls, void *sel, void *imp, void **out) {
    if (!cls || !sel || !imp) return -1;
    hk_hook_spec_t spec;
    // Caller should use cls as Class and sel as SEL — we treat generically.
    // Method kind defaults to instance; class methods work via metaclass lookup at commit.
    _hk_compat_make_objc_spec(&spec, "compat.substrate.msg", cls, sel, imp, false);
    return _hk_compat_hook_one(&spec, out);
}
static inline __attribute__((unused)) void hk_compat_MSHookMessageEx(void *cls, void *sel, void *imp, void **out) {
    (void)hk_compat_MSHookMessageEx_int(cls, sel, imp, out);
}
static inline __attribute__((unused)) void hk_compat_MSHookMemory(void *dst, const void *data, size_t sz);
static inline __attribute__((unused)) int hk_compat_MSHookMemory_int(void *dst, const void *data, size_t sz) {
    if (!dst || !data || sz == 0) return -1;
    hk_hook_spec_t spec;
    _hk_compat_make_memory_spec(&spec, "compat.substrate.mem", dst, data, sz);
    int r = _hk_compat_hook_one(&spec, NULL);
    free((void*)spec.target.memory.expected_bytes.data);
    return r;
}
static inline __attribute__((unused)) void hk_compat_MSHookMemory(void *dst, const void *data, size_t sz) {
    (void)hk_compat_MSHookMemory_int(dst, data, sz);
}

// Substitute: struct substitute_function_hook { void *function; void *replacement; void *old_ptr; int options; }
struct hk_compat_substitute_function_hook {
    void *function;
    void *replacement;
    void *old_ptr;
    int options;
};
static inline __attribute__((unused)) int hk_compat_substitute_hook_functions(const struct hk_compat_substitute_function_hook *hooks, size_t nhooks, void *recordp, int options) {
    (void)recordp; (void)options;
    if (!hooks || nhooks == 0) return 0; // SUBSTITUTE_OK
    hk_hook_spec_t *specs = (hk_hook_spec_t *)calloc(nhooks, sizeof(hk_hook_spec_t));
    void **origs = (void **)calloc(nhooks, sizeof(void *));
    char (*ids)[32] = (char (*)[32])calloc(nhooks, 32);
    if (!specs || !origs || !ids) { free(specs); free(origs); free(ids); return 5; /* SUBSTITUTE_ERR_OOM */ }
    for (size_t i = 0; i < nhooks; i++) {
        snprintf(ids[i], 32, "compat.substitute.%zu", i);
        _hk_compat_make_function_spec(&specs[i], ids[i], hooks[i].function, hooks[i].replacement);
        origs[i] = hooks[i].old_ptr; // void *old_ptr is actually void** old_ptr in public header — handle both
    }
    // HookKit batch expects void** const* pointing at storage — we synthesize pointer array to those storages.
    void ***out_ptrs = (void ***)calloc(nhooks, sizeof(void**));
    for (size_t i = 0; i < nhooks; i++) out_ptrs[i] = (void**)hooks[i].old_ptr;
    int ok = _hk_compat_hook_batch(specs, (void**const*)out_ptrs, nhooks);
    free(out_ptrs);
    free(specs); free(origs); free(ids);
    if (ok == (int)nhooks) return 0; // SUBSTITUTE_OK
    if (ok == 0) return 1; // SUBSTITUTE_ERR_FUNC_TOO_SHORT (generic fail)
    return 1; // partial still maps to error — caller checks count not ok; HookKit partial is terminal
}

// Substitute ObjC: int substitute_hook_objc_message(Class, SEL, void*, void* old_ptr, bool *created)
static inline __attribute__((unused)) int hk_compat_substitute_hook_objc_message(void *cls, void *sel, void *rep, void *old_ptr, bool *created) {
    if (created) *created = false;
    hk_hook_spec_t spec;
    _hk_compat_make_objc_spec(&spec, "compat.substitute.objc", cls, sel, rep, false);
    void **out = (void**)old_ptr;
    int r = _hk_compat_hook_one(&spec, out);
    return r == 0 ? 0 : 11; // SUBSTITUTE_OK or SUBSTITUTE_ERR_NO_SUCH_SELECTOR
}

// libhooker/ElleKit: struct LHFunctionHook { void *function; void *replacement; void *oldptr; LHFunctionHookOptions *options; }
struct hk_compat_LHFunctionHook {
    void *function;
    void *replacement;
    void *oldptr;
    void *options;
};
struct hk_compat_LHMemoryPatch {
    void *destination;
    const void *data;
    size_t size;
    void *options;
};
static inline __attribute__((unused)) int hk_compat_LHHookFunctions(const struct hk_compat_LHFunctionHook *hooks, int count) {
    if (!hooks || count <= 0) return 0;
    hk_hook_spec_t *specs = (hk_hook_spec_t *)calloc((size_t)count, sizeof(hk_hook_spec_t));
    void ***out_ptrs = (void ***)calloc((size_t)count, sizeof(void**));
    char (*ids)[32] = (char (*)[32])calloc((size_t)count, 32);
    if (!specs || !out_ptrs || !ids) { free(specs); free(out_ptrs); free(ids); return 0; }
    for (int i = 0; i < count; i++) {
        snprintf(ids[i], 32, "compat.libhooker.%d", i);
        _hk_compat_make_function_spec(&specs[i], ids[i], hooks[i].function, hooks[i].replacement);
        out_ptrs[i] = (void**)hooks[i].oldptr;
    }
    int ok = _hk_compat_hook_batch(specs, (void**const*)out_ptrs, (size_t)count);
    free(specs); free(out_ptrs); free(ids);
    return ok; // libhooker returns count on full/partial — matches
}
static inline __attribute__((unused)) int hk_compat_LBHookMessage_int(void *cls, void *sel, void *rep, void *old_ptr) {
    hk_hook_spec_t spec;
    _hk_compat_make_objc_spec(&spec, "compat.libhooker.msg", cls, sel, rep, false);
    return _hk_compat_hook_one(&spec, (void**)old_ptr);
}
static inline __attribute__((unused)) int hk_compat_LBHookMessage(void *cls, void *sel, void *rep, void *old_ptr) {
    return hk_compat_LBHookMessage_int(cls, sel, rep, old_ptr) == 0 ? 0 : 1; // LIBHOOKER_OK or ERR
}
static inline __attribute__((unused)) int hk_compat_LHPatchMemory(const struct hk_compat_LHMemoryPatch *patches, int count) {
    if (!patches || count <= 0) return 0;
    hk_hook_spec_t *specs = (hk_hook_spec_t *)calloc((size_t)count, sizeof(hk_hook_spec_t));
    if (!specs) return 0;
    for (int i = 0; i < count; i++) {
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "compat.libhooker.mem.%d", i);
        // Must heap-copy id string because spec deep-copies only after next iteration
        char *idcopy = (char*)malloc(32); if (idcopy) strcpy(idcopy, idbuf);
        _hk_compat_make_memory_spec(&specs[i], idcopy ? idcopy : idbuf, patches[i].destination, patches[i].data, patches[i].size);
        // free after deep-copy in batch helper — we need to keep idcopy alive until batch does add
        // So store idcopy to free after batch
        specs[i].stable_hook_id = idcopy ? idcopy : specs[i].stable_hook_id;
    }
    hk_runtime_t *rt = _hk_compat_runtime();
    hk_plan_t *plan = NULL;
    if (!rt || hk_plan_create(rt, NULL, &plan) != HK_STATUS_OK) {
        for (int i = 0; i < count; i++) free((void*)specs[i].stable_hook_id);
        for (int i = 0; i < count; i++) free((void*)specs[i].target.memory.expected_bytes.data);
        free(specs); return 0;
    }
    hk_hook_t **hooks = (hk_hook_t **)calloc((size_t)count, sizeof(hk_hook_t*));
    for (int i = 0; i < count; i++) (void)hk_plan_add_hook(plan, &specs[i], &hooks[i]);
    (void)hk_plan_analyze(plan, NULL);
    (void)hk_plan_prepare(plan, NULL);
    (void)hk_plan_commit(plan, NULL);
    int ok = 0;
    for (int i = 0; i < count; i++) {
        if (!hooks[i]) continue;
        hk_hook_result_t r; memset(&r,0,sizeof(r)); r.struct_size=sizeof(r); r.struct_version=HK_ABI_VERSION_3_0;
        if (hk_hook_copy_result(hooks[i], &r)==HK_STATUS_OK && (r.outcome==HK_OUTCOME_ACTIVE||r.outcome==HK_OUTCOME_ALREADY_ACTIVE)) ok++;
    }
    for (int i = 0; i < count; i++) {
        free((void*)specs[i].stable_hook_id);
        free((void*)specs[i].target.memory.expected_bytes.data);
    }
    free(specs); free(hooks); hk_plan_release(plan);
    return ok;
}

// Hijack macros — define after real headers so they replace calls in tweak code only.
#if HOOKKIT_COMPAT_HIJACK
#if HOOKKIT_COMPAT_SUBSTRATE
#undef MSHookFunction
#define MSHookFunction(sym,rep,out) ((void)hk_compat_MSHookFunction((void*)(sym),(void*)(rep),(void**)(out)))
#undef MSHookMessageEx
#define MSHookMessageEx(cls,sel,imp,out) ((void)hk_compat_MSHookMessageEx((void*)(cls),(void*)(sel),(void*)(imp),(void**)(out)))
#if HOOKKIT_COMPAT_MEMORY
#undef MSHookMemory
#define MSHookMemory(dst,data,sz) ((void)hk_compat_MSHookMemory((void*)(dst),(const void*)(data),(size_t)(sz)))
#endif
#endif
#if HOOKKIT_COMPAT_SUBSTITUTE
#undef substitute_hook_functions
#define substitute_hook_functions(h,n,r,o) hk_compat_substitute_hook_functions((const struct hk_compat_substitute_function_hook*)(h),(size_t)(n),(void*)(r),(int)(o))
#undef substitute_hook_objc_message
#define substitute_hook_objc_message(c,s,r,o,cr) hk_compat_substitute_hook_objc_message((void*)(c),(void*)(s),(void*)(r),(void*)(o),(bool*)(cr))
#endif
#if HOOKKIT_COMPAT_LIBHOOKER
#undef LHHookFunctions
#define LHHookFunctions(h,c) hk_compat_LHHookFunctions((const struct hk_compat_LHFunctionHook*)(h),(int)(c))
#undef LBHookMessage
#define LBHookMessage(c,s,r,o) ((int)hk_compat_LBHookMessage((void*)(c),(void*)(s),(void*)(r),(void*)(o)))
#if HOOKKIT_COMPAT_MEMORY
#undef LHPatchMemory
#define LHPatchMemory(p,c) hk_compat_LHPatchMemory((const struct hk_compat_LHMemoryPatch*)(p),(int)(c))
#endif
#endif
#endif // HIJACK

#endif // HOOKKIT_COMPAT_H
