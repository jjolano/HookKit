// Device evidence gate for the actual rootless provider aliases. On the
// current test device libhooker and CydiaSubstrate both resolve to ElleKit.

#include "../vendor/libhooker/libhooker.h"

#include <dlfcn.h>
#include <stdio.h>

typedef void (*ms_hook_function_fn)(void *, void *, void **);

__attribute__((noinline)) static int ellekit_target(int value) {
    volatile int result = value;
    result += 13;
    return result;
}

__attribute__((noinline)) static int ellekit_replacement(int value) {
    return value + 70;
}

__attribute__((noinline)) static int substrate_alias_target(int value) {
    volatile int result = value;
    result += 17;
    return result;
}

__attribute__((noinline)) static int substrate_alias_replacement(int value) {
    return value + 90;
}

static int fail(const char *stage) {
    fprintf(stderr, "HookKit provider alias: FAIL: %s\n", stage);
    return 1;
}

int main(void) {
    void *handle = dlopen("/var/jb/usr/lib/libellekit.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!handle || !dlsym(handle, "EKHookFunction")) {
        return fail("ElleKit marker");
    }
    int (*lh_hook_functions)(const struct LHFunctionHook *, int) =
        (int (*)(const struct LHFunctionHook *, int))dlsym(handle, "LHHookFunctions");
    ms_hook_function_fn ms_hook_function =
        (ms_hook_function_fn)dlsym(handle, "MSHookFunction");
    if (!lh_hook_functions || !ms_hook_function) {
        return fail("compatibility exports");
    }

    void *ellekit_original = NULL;
    struct LHFunctionHook hook = {
        .function = (void *)ellekit_target,
        .replacement = (void *)ellekit_replacement,
        .oldptr = &ellekit_original,
        .options = NULL,
    };
    if (ellekit_target(1) != 14 || lh_hook_functions(&hook, 1) != 0 ||
        !ellekit_original || ellekit_target(1) != 71 ||
        ((int (*)(int))ellekit_original)(1) != 14) {
        return fail("LHHookFunctions");
    }

    void *substrate_original = NULL;
    if (substrate_alias_target(1) != 18) {
        return fail("MSHookFunction baseline");
    }
    ms_hook_function((void *)substrate_alias_target,
                     (void *)substrate_alias_replacement, &substrate_original);
    if (!substrate_original || substrate_alias_target(1) != 91 ||
        ((int (*)(int))substrate_original)(1) != 18) {
        return fail("MSHookFunction");
    }

    puts("HookKit provider alias (ElleKit/libhooker/CydiaSubstrate): PASS");
    return 0;
}
