// Live relocating-inline test -- arm64 macOS only. Split out of
// test_live_hooks.m: the trampoline page comes from vm_allocate +
// vm_protect(R-X), which macOS refuses to execute on arm64e (MAP_JIT-less
// W^X). iOS honors the R-W -> R-X transition, so this path is covered there
// and on arm64 macOS; the arm64e macOS job compiles the closure but does not
// run this file.
#import <Foundation/Foundation.h>

#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

#include "../../src/internal/HKPointerAuth.h"
#include "../../src/engines/HKRelocInlineEngine.h"
#include "../../src/native/hk_native.h"

__attribute__((noinline))
static int reloc_replacement(int value) {
    return value + 300;
}

static bool native_write(void *ctx, uintptr_t address,
                         const uint8_t *data, size_t size) {
    (void)ctx;
    return hk_native_patch_memory((void *)address, data, size);
}

// Relocating-inline live test: entry patched, original stays callable
// through the trampoline. Uses the real native page seams
// (vm_allocate/vm_protect), which needs live executable allocation, not a buffer stand-in --
// the reason this runs as its own binary rather than inside the suite.
static uintptr_t live_reloc_alloc(void *ctx, size_t size, uintptr_t near) {
    (void)ctx;
    return hk_native_reloc_alloc(size, near);
}

static bool live_reloc_seal(void *ctx, uintptr_t page, size_t size) {
    (void)ctx;
    return hk_native_reloc_seal(page, size);
}

static void live_reloc_free(void *ctx, uintptr_t page, size_t size) {
    (void)ctx;
    hk_native_reloc_free(page, size);
}

static void test_reloc_inline_hook(void *targets) {
    int (*reloc_target)(int) =
        (int (*)(int))dlsym(targets, "hk_live_reloc_target");
    assert(reloc_target);

    // Baseline is computed how the target computes it, not a magic number:
    // if the toolchain ever folds this target into another function, the
    // value assert below catches the fold instead of asserting a stale 57.
    int baseline = reloc_target(2);
    assert(baseline == (2 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20 + 21 + 22 + 23 + 24 + 25 + 26));
    hk_reloc_plan_t plan;
    uintptr_t target = hk_pac_strip_code((uintptr_t)reloc_target);
    uintptr_t replacement = hk_pac_strip_code((uintptr_t)reloc_replacement);
    assert(hk_reloc_prepare(target, replacement, NULL, 0,
                            live_reloc_alloc, live_reloc_seal,
                            live_reloc_free, NULL, &plan) == HK_RELOC_OK);
    assert(plan.captured && plan.original_entry != 0);
    assert(hk_reloc_commit(&plan, native_write, NULL,
                           live_reloc_free, NULL, NULL) ==
           HK_MUTATION_COMPLETE);
    assert(reloc_target(2) == 302);
    int (*original)(int) = (int (*)(int))plan.original_entry;
    assert(original(2) == baseline);
    // Deliberately leaked, like every live trampoline: a page a patched
    // entry still branches to is never reclaimed.
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/live-targets.dylib\n", argv[0]);
        return 2;
    }
    void *targets = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!targets) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    @autoreleasepool {
        test_reloc_inline_hook(targets);
        printf("HookKit macOS live reloc-inline: PASS\n");
    }
    return 0;
}
