#import <Foundation/Foundation.h>
#import <HookKit.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

extern void *hk3_swift_probe_metadata(void);
extern int64_t hk3_swift_probe_call_target(void);
extern void *hk3_swift_probe_replacement(void);

static int fail(const char *message) {
    fprintf(stderr, "HookKit canonical Swift facade smoke: FAIL: %s\n", message);
    return 1;
}

static bool restore_slot(HKSubstitutor *substitutor, Class metadata,
                         uint32_t index, void *original) {
    void *ignored = NULL;
    return [substitutor hookSwiftVtableSlotInClass:metadata
                                         withIndex:index
                                   withReplacement:original
                                         outOldPtr:&ignored] == HK_OK;
}

static bool hook_name_and_restore(HKSubstitutor *substitutor, Class metadata,
                                  uint32_t index, NSString *name,
                                  void *replacement) {
    void *original = NULL;
    if ([substitutor hookSwiftMethodInClass:metadata
                                  withName:name
                           withReplacement:replacement
                                 outOldPtr:&original] != HK_OK ||
        !original || original == replacement) {
        return false;
    }
    bool changed = hk3_swift_probe_call_target() == 42;
    return restore_slot(substitutor, metadata, index, original) &&
           changed && hk3_swift_probe_call_target() == 7;
}

int main(void) {
    @autoreleasepool {
        Class metadata = (__bridge Class)hk3_swift_probe_metadata();
        void *replacement = hk3_swift_probe_replacement();
        HKSubstitutor *substitutor = [HKSubstitutor defaultSubstitutor];
        if (!metadata || !replacement || !substitutor) {
            return fail("probe setup");
        }
        if (hk3_swift_probe_call_target() != 7) {
            return fail("baseline dispatch");
        }

        uint32_t target_index = UINT32_MAX;
        for (uint32_t index = 0; index < 16; index++) {
            void *original = NULL;
            if ([substitutor hookSwiftVtableSlotInClass:metadata
                                              withIndex:index
                                        withReplacement:replacement
                                              outOldPtr:&original] != HK_OK) {
                continue;
            }
            if (!original || original == replacement) {
                return fail("slot original");
            }
            bool changed = hk3_swift_probe_call_target() == 42;
            if (!restore_slot(substitutor, metadata, index, original) ||
                hk3_swift_probe_call_target() != 7) {
                return fail("slot restore");
            }
            if (changed) {
                target_index = index;
                break;
            }
        }
        if (target_index == UINT32_MAX) {
            return fail("target slot");
        }

        // This is the stable mangled name emitted by the fixed probe module.
        if (!hook_name_and_restore(substitutor, metadata, target_index,
                                   @"$s13HK3SwiftProbeAAC6targetSiyF",
                                   replacement)) {
            return fail("exact mangled lookup");
        }
        if (!hook_name_and_restore(substitutor, metadata, target_index,
                                   @"HK3SwiftProbe.target", replacement)) {
            return fail("demangled lookup");
        }

        puts("HookKit canonical Swift facade smoke: PASS");
        return 0;
    }
}
