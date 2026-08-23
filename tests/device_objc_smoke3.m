#import <HookKit/HookKit.h>
#import <HookKit/HookKitObjC.h>
#include <objc/message.h>
#include <stdio.h>

static int hk3_original(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 7;
}

static int hk3_replacement(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 42;
}

static int hk3_deferred_original(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 13;
}

static int hk3_deferred_replacement(id self, SEL selector) {
    (void)self;
    (void)selector;
    return 99;
}

static int fail(const char *message) {
    fprintf(stderr, "HookKit3 ObjC smoke: FAIL: %s\n", message);
    return 1;
}

static int run_deferred_lifecycle_smoke(void) {
    const char *failure = NULL;
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_report_t *report = NULL;
    hk_artifact_snapshot_t *snapshot = NULL;
    Class deferred_class = Nil;
    SEL deferred_selector = NULL;
    id object = nil;
    int (*send_value)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
    bool installed = false;

    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK || !runtime) {
        failure = "deferred runtime create";
        goto done;
    }
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK) {
        failure = "deferred plan create";
        goto done;
    }

    hk_objc_target_t target = hk_objc_target_make_named(
        "HK3DeferredDeviceSmokeObject", "hk3_deferred_value",
        HK_OBJC_INSTANCE_METHOD);
    target.availability = HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE;
    hk_hook_spec_t spec;
    hk_objc_spec_init(&spec, "device-objc-deferred", target,
                      (void *)hk3_deferred_replacement);

    hk_hook_t *hook = NULL;
    if (hk_plan_add_hook(plan, &spec, &hook) != HK_STATUS_OK ||
        hk_plan_analyze(plan, NULL) != HK_STATUS_OK ||
        hk_plan_prepare(plan, NULL) != HK_STATUS_OK) {
        failure = "deferred prepare";
        goto done;
    }
    hk_hook_result_t result;
    if (hk_hook_copy_result(hook, &result) != HK_STATUS_OK ||
        result.outcome != HK_OUTCOME_PENDING) {
        failure = "deferred pending outcome";
        goto done;
    }

    hk_plan_release(plan);
    plan = NULL;
    if (hk_runtime_drain_pending(runtime, &report) != HK_STATUS_OK ||
        !report || hk_hook_copy_result(hook, &result) != HK_STATUS_OK ||
        result.outcome != HK_OUTCOME_PENDING) {
        failure = "deferred pending retry";
        goto done;
    }
    hk_report_release(report);
    report = NULL;

    Class super_class = objc_getClass("NSObject");
    deferred_selector = sel_registerName("hk3_deferred_value");
    deferred_class = objc_allocateClassPair(
        super_class, "HK3DeferredDeviceSmokeObject", 0);
    if (!super_class || !deferred_class || !deferred_selector ||
        !class_addMethod(deferred_class, deferred_selector,
                         (IMP)hk3_deferred_original, "i@:")) {
        failure = "deferred class setup";
        goto done;
    }
    objc_registerClassPair(deferred_class);
    object = class_createInstance(deferred_class, 0);
    if (!object || send_value(object, deferred_selector) != 13 ||
        hk_runtime_drain_pending(runtime, &report) != HK_STATUS_OK || !report ||
        hk_report_copy_artifacts(report, &snapshot) != HK_STATUS_OK ||
        hk_artifact_snapshot_count(snapshot) != 1 ||
        send_value(object, deferred_selector) != 99) {
        failure = "deferred active retry";
        goto done;
    }
    installed = true;

done:
    if (installed) {
        class_replaceMethod(deferred_class, deferred_selector,
                            (IMP)hk3_deferred_original, "i@:");
    }
    hk_artifact_snapshot_release(snapshot);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    return failure ? fail(failure) : 0;
}

int main(void) {
    Class super_class = objc_getClass("NSObject");
    if (!super_class) {
        return fail("NSObject lookup");
    }
    Class smoke_class = objc_allocateClassPair(super_class, "HK3DeviceSmokeObject", 0);
    SEL selector = sel_registerName("hk3_value");
    if (!smoke_class || !selector ||
        !class_addMethod(smoke_class, selector, (IMP)hk3_original, "i@:")) {
        return fail("class setup");
    }
    objc_registerClassPair(smoke_class);
    id object = class_createInstance(smoke_class, 0);
    int (*send_value)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
    if (!object || send_value(object, selector) != 7) {
        return fail("baseline method result");
    }

    hk_runtime_t *runtime = NULL;
    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK) {
        return fail("runtime create");
    }

    hk_plan_t *plan = NULL;
    if (hk_plan_create(runtime, NULL, &plan) != HK_STATUS_OK) {
        hk_runtime_release(runtime);
        return fail("plan create");
    }

    hk_hook_spec_t spec;
    hk_objc_spec_init(&spec, "device-objc", hk_objc_instance_method(
        smoke_class, selector),
        (void *)hk3_replacement);

    hk_hook_t *hook = NULL;
    if (hk_plan_add_hook(plan, &spec, &hook) != HK_STATUS_OK) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("hook add");
    }

    hk_report_t *report = NULL;
    hk_hook_result_t result;
    if (hk_plan_analyze(plan, &report) != HK_STATUS_OK) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("analyze");
    }
    hk_report_release(report);
    if (hk_hook_copy_result(hook, &result) != HK_STATUS_OK ||
        result.outcome != HK_OUTCOME_ANALYZED) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("objc route");
    }

    if (hk_plan_prepare(plan, &report) != HK_STATUS_OK) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("prepare");
    }
    hk_report_release(report);
    if (hk_hook_copy_result(hook, &result) != HK_STATUS_OK ||
        result.outcome != HK_OUTCOME_PREPARED) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("objc preparation");
    }

    if (hk_plan_commit(plan, &report) != HK_STATUS_OK) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("commit");
    }
    hk_report_release(report);
    if (send_value(object, selector) != 42) {
        hk_plan_release(plan);
        hk_runtime_release(runtime);
        return fail("replacement dispatch");
    }

    hk_plan_release(plan);
    hk_runtime_release(runtime);
    if (run_deferred_lifecycle_smoke() != 0) {
        return 1;
    }
    printf("HookKit3 ObjC smoke: PASS\n");
    return 0;
}
