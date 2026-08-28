// Shadow 3.7.6's HookKit profile (its vendor pin is covered by the v2.1.1
// ABI baseline). This models its facade calls; it does not execute Shadow.
#import <Foundation/Foundation.h>
#import <HookKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdio.h>

static int shadow376_message_original(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 7;
}

static int shadow376_message_replacement(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 42;
}

__attribute__((noinline)) static int shadow376_function_original(void) {
    return 11;
}

__attribute__((noinline)) static int shadow376_function_replacement(void) {
    return 99;
}

static int fail(const char *message) {
    fprintf(stderr, "Shadow 3.7.6 profile: FAIL: %s\n", message);
    return 1;
}

int main(void) {
    @autoreleasepool {
        hookkit_lib_t available = [HKSubstitutor getAvailableSubstitutorTypes];
        NSArray<NSDictionary *> *info = [HKSubstitutor getSubstitutorTypeInfo:available];
        NSArray<NSString *> *backend_ids = [HKSubstitutor getAvailableBackendIDs];
        if (!info) {
            return fail("legacy provider discovery");
        }
        if (!backend_ids || info.count != backend_ids.count) {
            return fail("legacy picker enumeration");
        }
        BOOL saw_fishhook = NO;
        for (NSDictionary *backend in info) {
            NSString *identifier = backend[@"id"];
            NSString *name = backend[@"name"];
            NSNumber *type = backend[@"type"];
            if (![identifier isKindOfClass:[NSString class]] || identifier.length == 0 ||
                ![name isKindOfClass:[NSString class]] || name.length == 0 ||
                ![type isKindOfClass:[NSNumber class]] ||
                (available & (hookkit_lib_t)type.unsignedIntegerValue) == 0) {
                return fail("legacy provider metadata");
            }
            saw_fishhook |= [identifier isEqualToString:@"fishhook"];
        }
        if (backend_ids.count == 0) {
            return fail("dynamic backend enumeration");
        }
        if ([backend_ids containsObject:@"rebind"] != saw_fishhook) {
            return fail("fishhook picker entry");
        }
        HKSubstitutor *selected =
            [HKSubstitutor substitutorWithBackendIDs:@[backend_ids.firstObject]];
        if (!selected) {
            return fail("per-instance backend selection");
        }

        HKSubstitutor *hooks = [HKSubstitutor defaultSubstitutor];
        if (!hooks) {
            return fail("default substitutor");
        }
        if (info.count > 0) {
            hookkit_lib_t selected_type =
                (hookkit_lib_t)[info.firstObject[@"type"] unsignedIntegerValue];
            [hooks setTypes:selected_type];
            [hooks initLibraries];
            if (hooks.activeType != selected_type) {
                return fail("legacy backend selection");
            }
        }
        // The following functional probe needs the automatic route because
        // this test intentionally enumerates every engine, including
        // operation-specific choices such as the ObjC and memory engines.
        [hooks setTypes:HK_LIB_NONE];

        Class super_class = objc_getClass("NSObject");
        SEL selector = sel_registerName("shadow376_value");
        Class profile_class = objc_allocateClassPair(super_class,
                                                      "HKShadow376ProfileObject", 0);
        if (!super_class || !selector || !profile_class ||
            !class_addMethod(profile_class, selector, (IMP)shadow376_message_original,
                             "i@:")) {
            return fail("class setup");
        }
        objc_registerClassPair(profile_class);
        id object = class_createInstance(profile_class, 0);
        int (*send_value)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
        if (!object || send_value(object, selector) != 7) {
            return fail("message baseline");
        }

        HKEnableBatching();
        void *old_message = NULL;
        void *old_function = NULL;
        if (HKHookMessage(profile_class, selector,
                          (void *)shadow376_message_replacement, &old_message) != HK_OK ||
            HKHookFunction((void *)shadow376_function_original,
                           (void *)shadow376_function_replacement, &old_function) != HK_OK ||
            send_value(object, selector) != 7 || shadow376_function_original() != 11) {
            return fail("batched hook queue");
        }

        void *image = HKOpenImage("/usr/lib/system/libdyld.dylib");
        if (!image || !HKFindSymbol(image, "dlopen")) {
            return fail("image and symbol lookup");
        }
        HKCloseImage(image);

        if (HKExecuteBatch() != HK_OK ||
            old_message != (void *)shadow376_message_original ||
            !old_function || send_value(object, selector) != 42 ||
            ((int (*)(id, SEL))old_message)(object, selector) != 7 ||
            shadow376_function_original() != 99 ||
            ((int (*)(void))old_function)() != 11) {
            return fail("batched hook execution");
        }
        HKDisableBatching();

        puts("Shadow 3.7.6 profile: PASS");
        return 0;
    }
}
