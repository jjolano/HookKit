#include <HookKit/HookKit.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

extern void *hk_swift_probe_metadata(void);
extern int64_t hk_swift_probe_call_target(void);
extern void *hk_swift_probe_replacement(void);

static int fail(const char *message) {
    fprintf(stderr, "HookKit real Swift smoke: FAIL: %s\n", message);
    return 1;
}

static bool restore_target(const hk_swift_target_t *target, void *original) {
    hk_swift_plan_t *plan = NULL;
    void *ignored = NULL;
    bool restored = hk_swift_prepare(target, &plan) == HK_STATUS_OK &&
                    hk_swift_commit(plan, original, &ignored) == HK_STATUS_OK;
    hk_swift_plan_release(plan);
    return restored;
}

int main(void) {
    void *metadata = hk_swift_probe_metadata();
    void *replacement = hk_swift_probe_replacement();
    if (!metadata || !replacement) {
        return fail("probe metadata");
    }
    if (hk_swift_probe_call_target() != 7) {
        return fail("baseline dispatch");
    }

    hk_swift_target_t target = hk_swift_target_init();
    target.metadata = metadata;
    target.name_kind = HK_SWIFT_NAME_SLOT_INDEX;
    int last_swift_error = 0;

    // Initializers may occupy vtable entries before the two ordinary methods.
    // Try the bounded class-local vtable range, mutating at most one probe slot
    // at a time and restoring it immediately.
    for (uint32_t index = 0; index < 16; index++) {
        target.slot_index = index;
        hk_swift_plan_t *plan = NULL;
        if (hk_swift_prepare(&target, &plan) != HK_STATUS_OK) {
            last_swift_error = hk_swift_last_error_code();
            hk_swift_plan_release(plan);
            continue;
        }
        if (hk_swift_probe_call_target() != 7) {
            hk_swift_plan_release(plan);
            return fail("prepare changed dispatch");
        }

        void *original = NULL;
        if (hk_swift_commit(plan, replacement, &original) != HK_STATUS_OK ||
            !original || original == replacement) {
            hk_swift_plan_release(plan);
            return fail("commit");
        }
        hk_swift_plan_release(plan);

        bool target_changed = hk_swift_probe_call_target() == 42;
        if (!restore_target(&target, original)) {
            return fail("restore");
        }
        if (hk_swift_probe_call_target() != 7) {
            return fail("restored dispatch");
        }
        if (target_changed) {
            puts("HookKit real Swift smoke: PASS");
            return 0;
        }
    }

    const uintptr_t *words = metadata;
    fprintf(stderr,
            "HookKit real Swift smoke: native error %d metadata=%p data=%#llx flags=%#llx desc=%#llx\n",
            last_swift_error,
            metadata,
            (unsigned long long)words[4],
            (unsigned long long)words[5],
            (unsigned long long)words[8]);
    return fail("target slot not found");
}
