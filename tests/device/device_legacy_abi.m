#import <Foundation/Foundation.h>
#import <HookKit.h>

#include <stdio.h>

__attribute__((noinline)) static int hk_legacy_abi_function(void) {
    return 5;
}

__attribute__((noinline)) static int hk_legacy_abi_replacement(void) {
    return 9;
}

static volatile int hk_legacy_abi_memory = 1;
static volatile int hk_legacy_abi_batched_memory = 4;

// Separate targets for the v1 module-class section so the two halves of this
// smoke cannot mask each other.
__attribute__((noinline)) static int hk_v1_module_function(void) {
    return 11;
}

__attribute__((noinline)) static int hk_v1_module_replacement(void) {
    return 22;
}

__attribute__((noinline)) static int hk_v1_batch_function_a(void) {
    return 31;
}

__attribute__((noinline)) static int hk_v1_batch_replacement_a(void) {
    return 32;
}

__attribute__((noinline)) static int hk_v1_batch_function_b(void) {
    return 41;
}

__attribute__((noinline)) static int hk_v1_batch_replacement_b(void) {
    return 42;
}

static volatile int hk_v1_module_memory = 7;

int main(void) {
    @autoreleasepool {
        hookkit_lib_t available_types = [HKSubstitutor getAvailableSubstitutorTypes];
        hookkit_cat_t available_categories = [HKSubstitutor getAvailableCategories];
        NSArray *type_info = [HKSubstitutor getSubstitutorTypeInfo:available_types];
        if (!type_info || available_categories != HK_CAT_NONE) {
            puts("HookKit legacy ABI: FAIL introspection");
            return 1;
        }

        HKSubstitutor *substitutor = [HKSubstitutor defaultSubstitutor];
        if (!substitutor) {
            puts("HookKit legacy ABI: FAIL default");
            return 1;
        }
        // v1 type bits are opaque enumeration tokens; leave this ABI probe on
        // the automatic route for its mixed function/memory exercise.
        [substitutor setTypes:HK_LIB_NONE];
        [substitutor initLibraries];
        (void)substitutor.activeType;
        (void)substitutor.activeStrategy;

        HKImageRef image = [substitutor openImage:@"/usr/lib/libobjc.A.dylib"];
        if (image) {
            NSArray<NSValue *> *symbols = nil;
            (void)[substitutor findSymbolsInImage:image
                                      symbolNames:@[@"objc_msgSend"]
                                        outSymbols:&symbols];
            (void)[substitutor findSymbolInImage:image symbolName:@"objc_msgSend"];
            [substitutor closeImage:image];
        }

        void *old_function = NULL;
        if ([substitutor hookFunction:(void *)hk_legacy_abi_function
                       withReplacement:(void *)hk_legacy_abi_replacement
                             outOldPtr:&old_function] != HK_OK ||
            !old_function || hk_legacy_abi_function() != 9) {
            puts("HookKit legacy ABI: FAIL function");
            return 1;
        }

        int replacement = 2;
        if ([substitutor hookMemory:(void *)&hk_legacy_abi_memory
                            withData:&replacement
                                size:sizeof(replacement)] != HK_OK ||
            hk_legacy_abi_memory != replacement) {
            puts("HookKit legacy ABI: FAIL memory");
            return 1;
        }

        HKSubstitutor *batched = [HKSubstitutor new];
        [batched setBatching:YES];
        int second_replacement = 3;
        if ([batched hookMemory:(void *)&hk_legacy_abi_batched_memory
                        withData:&second_replacement
                            size:sizeof(second_replacement)] != HK_OK ||
            hk_legacy_abi_batched_memory != 4 ||
            [batched executeHooks] != HK_OK ||
            hk_legacy_abi_batched_memory != second_replacement) {
            puts("HookKit legacy ABI: FAIL batching");
            return 1;
        }

        hookkit_lib_t errno_type = HK_LIB_NONE;
        (void)[substitutor getLibErrno:&errno_type];

        // --- v1 module classes ---
        // These six are shims over the same plans HKSubstitutor drives, so what
        // needs proving is the v1 surface: Core hands out a module, the three
        // carriers round-trip, executeHook: dispatches on carrier class, and
        // executeHooks: batches functions through one plan with an exact count.
        HookKitCore *core = [HookKitCore sharedInstance];
        HookKitModule *module = [core defaultModule];
        if (!core || !module) {
            puts("HookKit legacy ABI: FAIL v1 core");
            return 1;
        }

        NSArray<NSDictionary *> *module_info = [core getModuleInfo];
        if ([module_info count] == 0) {
            puts("HookKit legacy ABI: FAIL v1 module info");
            return 1;
        }
        NSString *identifier = [[module_info objectAtIndex:0] objectForKey:@"Identifier"];
        if (!identifier ||
            ![core getModuleInfoWithIdentifier:identifier] ||
            [core getModuleWithIdentifier:identifier] != module) {
            puts("HookKit legacy ABI: FAIL v1 module lookup");
            return 1;
        }

        // registerModule: must win over the built-in for its identifier.
        HookKitModule *registered = [HookKitModule new];
        [core registerModule:registered withIdentifier:@"hk.test.module"];
        if ([core getModuleWithIdentifier:@"hk.test.module"] != registered) {
            puts("HookKit legacy ABI: FAIL v1 register");
            return 1;
        }

        // Carriers round-trip their fields, and executeHook: routes each kind.
        void *v1_old = NULL;
        HookKitFunctionHook *function_hook =
            [HookKitFunctionHook hook:(void *)hk_v1_module_function
                          replacement:(void *)hk_v1_module_replacement
                                 orig:&v1_old];
        if ([function_hook function] != (void *)hk_v1_module_function ||
            [function_hook orig] != &v1_old ||
            ![module executeHook:function_hook] ||
            !v1_old || hk_v1_module_function() != 22) {
            puts("HookKit legacy ABI: FAIL v1 function hook");
            return 1;
        }

        int v1_replacement = 8;
        HookKitMemoryHook *memory_hook =
            [HookKitMemoryHook hook:(void *)&hk_v1_module_memory
                               data:&v1_replacement
                               size:sizeof(v1_replacement)];
        if ([memory_hook size] != sizeof(v1_replacement) ||
            ![module executeHook:memory_hook] ||
            hk_v1_module_memory != v1_replacement) {
            puts("HookKit legacy ABI: FAIL v1 memory hook");
            return 1;
        }

        // executeHooks: with functionHookBatchingSupported drives _hookFunctions:,
        // which builds both specs and applies them through one plan. The return
        // value is the exact installed count -- that is the part worth checking.
        void *batch_old_a = NULL;
        void *batch_old_b = NULL;
        NSArray *batch = @[
            [HookKitFunctionHook hook:(void *)hk_v1_batch_function_a
                          replacement:(void *)hk_v1_batch_replacement_a
                                 orig:&batch_old_a],
            [HookKitFunctionHook hook:(void *)hk_v1_batch_function_b
                          replacement:(void *)hk_v1_batch_replacement_b
                                 orig:&batch_old_b]
        ];
        if (![module functionHookBatchingSupported] ||
            [module executeHooks:batch] != 2 ||
            !batch_old_a || !batch_old_b ||
            hk_v1_batch_function_a() != 32 ||
            hk_v1_batch_function_b() != 42) {
            puts("HookKit legacy ABI: FAIL v1 batch");
            return 1;
        }

        // Image/symbol surface: v1 routes both through the same facade lookup.
        hookkit_image_t v1_image = [module openImageWithPath:@"/usr/lib/libobjc.A.dylib"];
        if (v1_image) {
            (void)[module findSymbolName:@"objc_msgSend" inImage:v1_image];
            [module closeImage:v1_image];
        }
        if (![module nullImageSearchSupported] ||
            ![module findSymbolName:@"objc_msgSend"]) {
            puts("HookKit legacy ABI: FAIL v1 symbol lookup");
            return 1;
        }

        puts("HookKit legacy ABI: PASS");
        return 0;
    }
}
