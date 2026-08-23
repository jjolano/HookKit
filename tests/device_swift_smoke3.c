#include <HookKit/HookKit.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void original_method(void) {}
static void replacement_method(void) {}

static int fail(const char *message) {
    fprintf(stderr, "HookKit3 Swift smoke: FAIL: %s\n", message);
    return 1;
}

int main(void) {
    uint8_t *metadata = calloc(1, 0x200);
    uint8_t *descriptor = calloc(1, 0x100);
    if (!metadata || !descriptor) {
        free(metadata);
        free(descriptor);
        return fail("allocation");
    }

    *(uintptr_t *)(metadata + 0x20) = 2;  // Apple stable-ABI Swift metadata bit
    *(uintptr_t *)(metadata + 0x40) = (uintptr_t)descriptor;
    *(uint32_t *)descriptor = (16u | (1u << 31));  // class + has vtable
    *(uint32_t *)(descriptor + 0x2c) = 0x20;       // vtable offset
    *(uint32_t *)(descriptor + 0x30) = 1;          // one slot
    *(uint32_t *)(descriptor + 0x34) = 0x10;      // ordinary instance method

    void **slot = (void **)(metadata + 0x100);
    *slot = (void *)original_method;

    hk_swift_target_t target = hk_swift_target_init();
    target.metadata = metadata;
    target.name_kind = HK_SWIFT_NAME_SLOT_INDEX;
    target.slot_index = 0;

    hk_swift_plan_t *plan = NULL;
    if (hk_swift_prepare(&target, &plan) != HK_STATUS_OK || !plan) {
        free(metadata);
        free(descriptor);
        return fail("prepare");
    }
    if (*slot != (void *)original_method) {
        hk_swift_plan_release(plan);
        free(metadata);
        free(descriptor);
        return fail("prepare wrote the slot");
    }

    void *old = NULL;
    if (hk_swift_commit(plan, (void *)replacement_method, &old) != HK_STATUS_OK ||
        old != (void *)original_method || *slot != (void *)replacement_method) {
        hk_swift_plan_release(plan);
        free(metadata);
        free(descriptor);
        return fail("commit");
    }

    hk_swift_plan_release(plan);
    free(metadata);
    free(descriptor);
    puts("HookKit3 Swift smoke: PASS (synthetic arm64 metadata)");
    return 0;
}
