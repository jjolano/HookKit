#import <Foundation/Foundation.h>

#include <assert.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/internal/HKPointerAuth.h"
#include "../../src/engines/HKInlineEngine.h"
#include "../../src/engines/HKMemoryEngine.h"
#include "../../src/engines/HKObjCEngine.h"
#include "../../src/engines/HKRebindEngine.h"
#include "../../src/engines/HKRelocInlineEngine.h"
#include "../../src/resolvers/HKMachO.h"
#include "../../src/native/hk_native.h"

__attribute__((noinline))
static int terminal_replacement(int value) {
    return value + 200;
}

__attribute__((noinline))
static int reloc_replacement(int value) {
    return value + 300;
}

static bool native_write(void *ctx, uintptr_t address,
                         const uint8_t *data, size_t size) {
    (void)ctx;
    return hk_native_patch_memory((void *)address, data, size);
}

static void test_inline_hooks(void *targets) {
    int (*terminal_target)(int) =
        (int (*)(int))dlsym(targets, "hk_live_terminal_target");
    assert(terminal_target);

    assert(terminal_target(1) == 27);
    hk_inline_plan_t plan;
    uintptr_t target = hk_pac_strip_code((uintptr_t)terminal_target);
    uintptr_t replacement = hk_pac_strip_code((uintptr_t)terminal_replacement);
    assert(hk_inline_prepare(target, replacement, HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);
    assert(hk_inline_commit(&plan, native_write, NULL, NULL) ==
           HK_MUTATION_COMPLETE);
    assert(terminal_target(1) == 201);
}

// The relocating engine through the same live path: entry patched, original
// still callable through the trampoline. Uses the real native page seams
// (vm_allocate/vm_protect), which is what makes this macOS-only -- it needs
// live executable allocation, not a buffer stand-in.
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
    assert(baseline == ((2 + 11) + (2 + 11) + 13 - 4));
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

static volatile uint32_t g_memory_value = UINT32_C(0x11223344);

static hk_bytes_view_t bytes(const void *data, size_t size) {
    hk_bytes_view_t view = {(const uint8_t *)data, size};
    return view;
}

static void test_memory_hook(void) {
    const uint32_t original = UINT32_C(0x11223344);
    const uint32_t replacement = UINT32_C(0xaabbccdd);
    const hk_bytes_view_t no_mask = {NULL, 0};
    hk_mempatch_plan_t plan;
    assert(hk_mempatch_prepare((uintptr_t)&g_memory_value,
                               sizeof(g_memory_value),
                               bytes(&original, sizeof(original)), no_mask,
                               &plan) == HK_MEMPATCH_OK);
    assert(hk_mempatch_commit((uintptr_t)&g_memory_value, &plan,
                              bytes(&replacement, sizeof(replacement)),
                              native_write, NULL, NULL) ==
           HK_MUTATION_COMPLETE);
    assert(g_memory_value == replacement);
}

@interface HKMacLiveProbe : NSObject
- (int)value;
@end

@implementation HKMacLiveProbe
- (int)value { return 7; }
@end

static void *live_get_class(void *ctx, const char *name) {
    (void)ctx;
    return (void *)objc_getClass(name);
}

static void *live_get_metaclass(void *ctx, void *cls) {
    (void)ctx;
    return (void *)object_getClass((id)cls);
}

static void *live_get_superclass(void *ctx, void *cls) {
    (void)ctx;
    return (void *)class_getSuperclass((Class)cls);
}

static void *live_register_selector(void *ctx, const char *name) {
    (void)ctx;
    return (void *)sel_registerName(name);
}

static void *live_get_instance_method(void *ctx, void *cls, void *sel) {
    (void)ctx;
    return cls ? (void *)class_getInstanceMethod((Class)cls, (SEL)sel) : NULL;
}

static void *live_method_get_imp(void *ctx, void *method) {
    (void)ctx;
    return (void *)method_getImplementation((Method)method);
}

static const char *live_method_get_types(void *ctx, void *method) {
    (void)ctx;
    return method_getTypeEncoding((Method)method);
}

static void *live_replace_method(void *ctx, void *cls, void *sel,
                                 void *imp, const char *types) {
    (void)ctx;
    return (void *)class_replaceMethod((Class)cls, (SEL)sel, (IMP)imp, types);
}

static int objc_replacement(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 42;
}

static void test_objc_hook(void) {
    hk_objc_runtime_t runtime = {
        .get_class = live_get_class,
        .get_metaclass = live_get_metaclass,
        .get_superclass = live_get_superclass,
        .register_selector = live_register_selector,
        .get_instance_method = live_get_instance_method,
        .method_get_imp = live_method_get_imp,
        .method_get_types = live_method_get_types,
        .replace_method = live_replace_method,
    };
    SEL selector = @selector(value);
    hk_objc_target_t target;
    memset(&target, 0, sizeof(target));
    target.struct_size = sizeof(target);
    target.struct_version = HK_ABI_VERSION_3_0;
    target.cls = (void *)[HKMacLiveProbe class];
    target.sel = (void *)selector;
    target.method_kind = HK_OBJC_INSTANCE_METHOD;
    target.inheritance_policy = HK_OBJC_LOCAL_METHOD_ONLY;
    target.availability = HK_AVAILABILITY_REQUIRED_NOW;

    HKMacLiveProbe *probe = [HKMacLiveProbe new];
    int (*send_value)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
    assert(send_value(probe, selector) == 7);

    hk_objc_plan_t plan;
    assert(hk_objc_prepare(&runtime, &target, &plan) == HK_OBJC_OK);
    void *original = NULL;
    assert(hk_objc_commit(&runtime, &plan, (void *)objc_replacement,
                          &original, NULL) == HK_MUTATION_COMPLETE);
    assert(original && send_value(probe, selector) == 42);
    assert(((int (*)(id, SEL))original)(probe, selector) == 7);

    class_replaceMethod([HKMacLiveProbe class], selector, (IMP)original, "i@:");
    assert(send_value(probe, selector) == 7);
}

static int (*g_original_puts)(const char *);
static unsigned g_puts_hits;

static int replacement_puts(const char *message) {
    g_puts_hits++;
    return g_original_puts(message);
}

static bool write_pointer(void *ctx, uintptr_t address, uint64_t value) {
    (void)ctx;
    return hk_native_patch_pointer((void *)address, (void *)(uintptr_t)value);
}

static void test_rebind_hook(void) {
    uint32_t image_index = UINT32_MAX;
    const struct mach_header_64 *header = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header_64 *candidate =
            (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (candidate && candidate->magic == MH_MAGIC_64 &&
            candidate->filetype == MH_EXECUTE) {
            image_index = i;
            header = candidate;
            break;
        }
    }
    assert(header && image_index != UINT32_MAX);

    intptr_t slide = _dyld_get_image_vmaddr_slide(image_index);
    size_t commands = sizeof(*header) + header->sizeofcmds;
    uintptr_t start = 0;
    uintptr_t end = 0;
    assert(hk_macho_image_span_for_loaded_image(
               header, commands, (uintptr_t)slide, &start, &end) == HK_MACHO_OK);

    hk_rebind_target_t target;
    memset(&target, 0, sizeof(target));
    target.image_base = header;
    target.image_size = end - (uintptr_t)header;
    target.slide = (uintptr_t)slide;
    target.image_path = _dyld_get_image_name(image_index);
    target.write = write_pointer;

    hk_rebind_plan_t plan;
    hk_rebind_status_t status =
        hk_rebind_prepare(&target, "puts", HK_SYMBOL_NAME_C, &plan);
    if (status != HK_REBIND_OK) {
        fprintf(stderr, "rebind prepare failed: %d (%s)\n", status,
                target.image_path);
        abort();
    }
    assert(plan.count > 0 && plan.originals_agree);
    g_original_puts = (int (*)(const char *))(uintptr_t)plan.original;
    assert(g_original_puts);

    uint32_t written = 0;
    assert(hk_rebind_commit(&target, &plan,
                            (uint64_t)(uintptr_t)replacement_puts,
                            NULL, &written) == HK_MUTATION_COMPLETE);
    assert(written == plan.count);
    puts("HookKit macOS rebind probe");
    assert(g_puts_hits == 1);
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
        test_inline_hooks(targets);
        test_reloc_inline_hook(targets);
        test_memory_hook();
        test_objc_hook();
        test_rebind_hook();
        g_original_puts("HookKit macOS live hooks: PASS");
    }
    return 0;
}
