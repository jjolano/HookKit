#include "../vendor/dobby/dobby.h"

#include <dlfcn.h>
#include <stdio.h>

typedef int (*provider_hook_fn)(void *, void *, void **);
typedef void (*provider_transaction_fn)(void);

__attribute__((noinline)) static int dobby_target(int value) {
    return value + 7;
}

__attribute__((noinline)) static int dobby_replacement(int value) {
    return value + 42;
}

__attribute__((noinline)) static int gum_target(int value) {
    return value + 11;
}

__attribute__((noinline)) static int gum_replacement(int value) {
    return value + 55;
}

static int fail(const char *provider, const char *reason) {
    fprintf(stderr, "HookKit provider %s: FAIL: %s\n", provider, reason);
    return 1;
}

static int test_dobby(void) {
    void *original = NULL;
    if (dobby_target(1) != 8) {
        return fail("Dobby", "baseline");
    }
    if (DobbyHook((void *)dobby_target, (void *)dobby_replacement, &original) != 0 ||
        !original) {
        return fail("Dobby", "hook");
    }
    if (dobby_target(1) != 43 || ((int (*)(int))original)(1) != 8) {
        return fail("Dobby", "replacement/original");
    }
    if (DobbyDestroy((void *)dobby_target) != 0 || dobby_target(1) != 8) {
        return fail("Dobby", "restore");
    }
    puts("HookKit provider Dobby: PASS");
    return 0;
}

static int test_gum(void) {
    void *handle = dlopen("/var/jb/usr/lib/HKGum.dylib", RTLD_LAZY);
    if (!handle) {
        return fail("Gum", "HKGum.dylib unavailable");
    }
    provider_hook_fn hook = (provider_hook_fn)dlsym(handle, "hkgum_hook_function");
    provider_transaction_fn begin =
        (provider_transaction_fn)dlsym(handle, "hkgum_begin_transaction");
    provider_transaction_fn end =
        (provider_transaction_fn)dlsym(handle, "hkgum_end_transaction");
    if (!hook || !begin || !end) {
        return fail("Gum", "wrapper ABI incomplete");
    }
    void *original = NULL;
    if (gum_target(1) != 12) {
        return fail("Gum", "baseline");
    }
    begin();
    int status = hook((void *)gum_target, (void *)gum_replacement, &original);
    end();
    if (status != 0 || !original) {
        return fail("Gum", "hook");
    }
    if (gum_target(1) != 56 || ((int (*)(int))original)(1) != 12) {
        return fail("Gum", "replacement/original");
    }
    puts("HookKit provider Gum: PASS");
    return 0;
}

int main(void) {
    return test_dobby() != 0 || test_gum() != 0 ? 1 : 0;
}
