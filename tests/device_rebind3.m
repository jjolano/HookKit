#include "../Sources/Engines/HKRebindEngine.h"
#include "../Sources/Resolvers/HKMachO.h"
#include "../native/hk_native.h"

#import <objc/NSObject.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int (*g_original_puts)(const char *);
static unsigned g_replacement_hits;

static int replacement_puts(const char *message) {
    g_replacement_hits++;
    return g_original_puts(message);
}

static bool write_pointer(void *ctx, uintptr_t address, uint64_t value) {
    (void)ctx;
    return hk_native_patch_pointer((void *)address, (void *)(uintptr_t)value);
}

int main(void) {
    (void)[NSObject class];
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
    if (!header || header->magic != MH_MAGIC_64) {
        puts("HookKit3 rebind: FAIL (main image)");
        return 1;
    }

    intptr_t slide = _dyld_get_image_vmaddr_slide(image_index);
    size_t commands = sizeof(*header) + header->sizeofcmds;
    uintptr_t start = 0;
    uintptr_t end = 0;
    if (hk_macho_image_span_for_loaded_image(header, commands,
                                             (uintptr_t)slide, &start, &end) != HK_MACHO_OK ||
        (uintptr_t)header < start || end <= (uintptr_t)header) {
        puts("HookKit3 rebind: FAIL (image span)");
        return 1;
    }

    hk_rebind_target_t target;
    memset(&target, 0, sizeof(target));
    target.image_base = header;
    target.image_size = end - (uintptr_t)header;
    target.slide = (uintptr_t)slide;
    target.write = write_pointer;

    hk_rebind_plan_t plan;
    hk_rebind_status_t prepare = hk_rebind_prepare(
        &target, "puts", HK_SYMBOL_NAME_C, &plan);
    if (prepare != HK_REBIND_OK || plan.count == 0 || !plan.originals_agree) {
        printf("HookKit3 rebind: FAIL (prepare=%d sites=%u)\n",
               prepare, plan.count);
        return 1;
    }

    g_original_puts = (int (*)(const char *))(uintptr_t)plan.original;
    if (!g_original_puts) {
        puts("HookKit3 rebind: FAIL (missing original)");
        return 1;
    }

    puts("HookKit3 rebind before");
    uint32_t written = 0;
    hk_mutation_state_t state = hk_rebind_commit(
        &target, &plan, (uint64_t)(uintptr_t)replacement_puts,
        NULL, &written);
    if (state != HK_MUTATION_COMPLETE || written != plan.count) {
        printf("HookKit3 rebind: FAIL (commit=%d written=%u sites=%u)\n",
               state, written, plan.count);
        return 1;
    }

    puts("HookKit3 rebind after");
    if (g_replacement_hits != 1) {
        g_original_puts("HookKit3 rebind: FAIL (replacement not called)");
        return 1;
    }
    g_original_puts("HookKit3 rebind: PASS");
    return 0;
}
