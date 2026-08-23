#include <HookKit/HookKit.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>

typedef int hookkit_status_t;
enum { HK_OK = 0 };

static int hk3_value_original(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 7;
}

static int hk3_value_replacement(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 42;
}

__attribute__((noinline)) static int hk3_function_original(void) {
    return 11;
}

__attribute__((noinline)) static int hk3_function_replacement(void) {
    return 99;
}

__attribute__((noinline)) static int hk3_terminal_original(void) {
    return 17;
}

__attribute__((noinline)) static int hk3_terminal_replacement(void) {
    return 71;
}

static volatile int hk3_memory_value = 3;

static int fail(const char *message) {
    fprintf(stderr, "HookKit canonical facade: FAIL: %s\n", message);
    return 1;
}

int main(void) {
    Class super_class = objc_getClass("NSObject");
    SEL selector = sel_registerName("hk3_value");
    Class smoke_class = objc_allocateClassPair(super_class, "HK3LegacyFacadeObject", 0);
    if (!super_class || !selector || !smoke_class ||
        !class_addMethod(smoke_class, selector, (IMP)hk3_value_original, "i@:")) {
        return fail("class setup");
    }
    objc_registerClassPair(smoke_class);
    id object = class_createInstance(smoke_class, 0);
    int (*send_value)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
    if (!object || send_value(object, selector) != 7) {
        return fail("message baseline");
    }

    id hk_substitutor_class = (id)objc_getClass("HKSubstitutor");
    SEL new_selector = sel_registerName("new");
    id (*send_new)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    id substitutor = send_new(hk_substitutor_class, new_selector);
    if (!substitutor) {
        return fail("facade class load");
    }

    typedef hookkit_status_t (*message_hook_fn)(id, SEL, Class, SEL, void *, void **);
    typedef hookkit_status_t (*function_hook_fn)(id, SEL, void *, void *, void **);
    typedef hookkit_status_t (*memory_hook_fn)(id, SEL, void *, const void *, size_t);
    message_hook_fn send_message_hook = (message_hook_fn)objc_msgSend;
    function_hook_fn send_function_hook = (function_hook_fn)objc_msgSend;
    memory_hook_fn send_memory_hook = (memory_hook_fn)objc_msgSend;
    void *old_message = NULL;
    if (send_message_hook(substitutor,
                          sel_registerName("hookMessageInClass:withSelector:withReplacement:outOldPtr:"),
                          smoke_class, selector, (void *)hk3_value_replacement,
                          &old_message) != HK_OK ||
        old_message != (void *)hk3_value_original ||
        send_value(object, selector) != 42) {
        return fail("3.0 ObjC bridge");
    }

    void *old_function = NULL;
    if (send_function_hook(substitutor,
                           sel_registerName("hookFunction:withReplacement:outOldPtr:"),
                           (void *)hk3_function_original,
                           (void *)hk3_function_replacement,
                           &old_function) != HK_OK ||
        !old_function || hk3_function_original() != 99 ||
        ((int (*)(void))old_function)() != 11) {
        return fail("3.0 relocating function bridge");
    }

    if (send_function_hook(substitutor,
                           sel_registerName("hookFunction:withReplacement:outOldPtr:"),
                           (void *)hk3_terminal_original,
                           (void *)hk3_terminal_replacement,
                           NULL) != HK_OK ||
        hk3_terminal_original() != 71) {
        return fail("3.0 terminal function bridge");
    }

    int replacement = 8;
    if (send_memory_hook(substitutor,
                         sel_registerName("hookMemory:withData:size:"),
                         (void *)&hk3_memory_value, &replacement,
                         sizeof(replacement)) != HK_OK ||
        hk3_memory_value != replacement) {
        return fail("3.0 memory bridge");
    }

    id batched = send_new(hk_substitutor_class, new_selector);
    void (*set_batching)(id, SEL, bool) = (void (*)(id, SEL, bool))objc_msgSend;
    hookkit_status_t (*execute_hooks)(id, SEL) =
        (hookkit_status_t (*)(id, SEL))objc_msgSend;
    set_batching(batched, sel_registerName("setBatching:"), true);
    int second_replacement = 13;
    if (send_memory_hook(batched,
                         sel_registerName("hookMemory:withData:size:"),
                         (void *)&hk3_memory_value, &second_replacement,
                         sizeof(second_replacement)) != HK_OK ||
        hk3_memory_value != replacement ||
        execute_hooks(batched, sel_registerName("executeHooks")) != HK_OK ||
        hk3_memory_value != second_replacement) {
        return fail("3.0 legacy batching");
    }

    puts("HookKit canonical facade: PASS");
    return 0;
}
