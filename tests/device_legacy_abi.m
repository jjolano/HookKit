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
        if ([batched hookMemory:(void *)&hk_legacy_abi_memory
                        withData:&second_replacement
                            size:sizeof(second_replacement)] != HK_OK ||
            hk_legacy_abi_memory != replacement ||
            [batched executeHooks] != HK_OK ||
            hk_legacy_abi_memory != second_replacement) {
            puts("HookKit legacy ABI: FAIL batching");
            return 1;
        }

        hookkit_lib_t errno_type = HK_LIB_NONE;
        (void)[substitutor getLibErrno:&errno_type];
        puts("HookKit legacy ABI: PASS");
        return 0;
    }
}
