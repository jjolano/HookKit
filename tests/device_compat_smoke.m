#import <HookKit/HookKit.h>
#import <HookKit/HookKitCompat.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <string.h>

static int hkCompatOrig(id self, SEL _cmd) { (void)self; (void)_cmd; return 1; }
static int hkCompatRepl(id self, SEL _cmd) { (void)self; (void)_cmd; return 42; }

// Targets for function hooks
static int target_a(int x) { return x + 1; }
static int target_b(int x) { return x + 2; }
static int repl_a(int x) { (void)x; return 100; }
static int repl_b(int x) { (void)x; return 200; }
static int (*orig_a)(int) = NULL;
static int (*orig_b)(int) = NULL;

static int target_d(int x){ return x+10; }
static int target_e(int x){ return x+20; }
static int repl_d(int x){ (void)x; return 400; }
static int repl_e(int x){ (void)x; return 500; }
static int (*orig_d)(int)=NULL; static int (*orig_e)(int)=NULL;

static int target_f(int x){ return x+30; }
static int repl_f(int x){ (void)x; return 600; }
static int (*orig_f)(int)=NULL;

static int need_fail(const char *msg) {
    fprintf(stderr, "Compat smoke FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    // 1. MSHookFunction shim (void-natural + int variant)
    if (hk_compat_MSHookFunction_int((void*)target_a, (void*)repl_a, (void**)&orig_a) != 0) return need_fail("MSHookFunction_int");
    if (!orig_a) return need_fail("MSHookFunction orig null");
    if (target_a(0) != 100) return need_fail("MSHookFunction repl not active");
    if (orig_a(5) != 6) return need_fail("MSHookFunction orig not callable");
    // void variant should not crash
    hk_compat_MSHookFunction((void*)target_b, (void*)repl_b, (void**)&orig_b);
    if (!orig_b || target_b(0) != 200) return need_fail("MSHookFunction void variant");

    // 2. LHHookFunctions batch (returns count)
    struct hk_compat_LHFunctionHook hooks[2] = {
        {(void*)target_d, (void*)repl_d, (void*)&orig_d, NULL},
        {(void*)target_e, (void*)repl_e, (void*)&orig_e, NULL},
    };
    int hooked = hk_compat_LHHookFunctions(hooks, 2);
    if (hooked != 2) { fprintf(stderr,"LHHookFunctions count %d !=2\n", hooked); return need_fail("LHHookFunctions count"); }
    if (!orig_d || !orig_e) return need_fail("LHHookFunctions orig null");
    if (target_d(0) != 400 || target_e(0) != 500) return need_fail("LHHookFunctions repl");
    if (orig_d(5) != 15 || orig_e(5) != 25) return need_fail("LHHookFunctions orig");

    // 3. substitute_hook_functions batch (returns SUBSTITUTE_OK 0)
    struct hk_compat_substitute_function_hook sub_hooks[1] = {{(void*)target_f, (void*)repl_f, (void*)&orig_f, 0}};
    int sub_ret = hk_compat_substitute_hook_functions(sub_hooks, 1, NULL, 0);
    if (sub_ret != 0) return need_fail("substitute_hook_functions ret");
    if (!orig_f || target_f(0)!=600) return need_fail("substitute repl");
    if (orig_f(5)!=35) return need_fail("substitute orig");

    // 4. LBHookMessage / MSHookMessageEx shim — ObjC method hook via compat
    // Create a class for ObjC test
    Class NSObject = objc_getClass("NSObject");
    if (!NSObject) return need_fail("NSObject");
    SEL sel = sel_registerName("hkCompatTestSel");
    Class TestCls = objc_allocateClassPair((Class)NSObject, "HKCompatTestCls", 0);
    if (!TestCls || !sel) return need_fail("class alloc");
    // Add method: - (int)hkCompatTestSel { return 1; }
    IMP imp_orig = (IMP)hkCompatOrig;
    IMP imp_repl = (IMP)hkCompatRepl;
    if (!class_addMethod(TestCls, sel, imp_orig, "i@:")) return need_fail("addMethod");
    objc_registerClassPair(TestCls);
    id obj = class_createInstance(TestCls, 0);
    int (*msgSend)(id, SEL) = (int (*)(id, SEL))objc_msgSend;
    if (msgSend(obj, sel) != 1) return need_fail("ObjC baseline");
    // Hook via LBHookMessage shim
    IMP orig_imp = NULL;
    if (hk_compat_LBHookMessage((__bridge void*)TestCls, (void*)sel, (void*)imp_repl, (void*)&orig_imp) != 0) return need_fail("LBHookMessage shim");
    if (!orig_imp) return need_fail("LBHookMessage orig null");
    if (msgSend(obj, sel) != 42) return need_fail("LBHookMessage repl");
    // Old via orig_imp should return 1
    int (*orig_call)(id,SEL) = (int(*)(id,SEL))orig_imp;
    if (orig_call(obj, sel) != 1) return need_fail("LBHookMessage orig call");

    // 5. Memory patch shims
    // Data patch via MSHookMemory
    static int my_data = 1234;
    int new_val = 5678;
    if (hk_compat_MSHookMemory_int(&my_data, &new_val, sizeof(my_data)) != 0) return need_fail("MSHookMemory");
    if (my_data != 5678) return need_fail("MSHookMemory not patched");
    // Restore via second patch
    int restore = 1234;
    // Need to patch back — expected bytes now is 5678, shim will read current as expected, so should succeed
    if (hk_compat_MSHookMemory_int(&my_data, &restore, sizeof(my_data)) != 0) return need_fail("MSHookMemory restore");
    if (my_data != 1234) return need_fail("MSHookMemory restore val");

    // LHPatchMemory batch
    static int data2 = 1111, data3 = 2222;
    int new2 = 3333, new3 = 4444;
    struct hk_compat_LHMemoryPatch mem_patches[2] = {
        {&data2, &new2, sizeof(data2), NULL},
        {&data3, &new3, sizeof(data3), NULL},
    };
    int patched = hk_compat_LHPatchMemory(mem_patches, 2);
    if (patched != 2) { fprintf(stderr,"LHPatchMemory patched %d !=2\n", patched); return need_fail("LHPatchMemory count"); }
    if (data2 != 3333 || data3 != 4444) return need_fail("LHPatchMemory vals");

    printf("HookKit Compat smoke: PASS\n");
    // also test hijacked macros compile: these are void, just ensure they expand
    // (already tested via hk_compat_*, macros are just wrappers)
    return 0;
}
