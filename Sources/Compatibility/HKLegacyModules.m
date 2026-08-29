// HookKit v1 module surface, as 3.0 translators.
//
// v1 shipped seven Objective-C classes. HKSubstitutor is the one Shadow ever
// linked and lives in HKSubstitutor.m; the other six are here. They restore the
// v1 link and source surface without resurrecting Modulous: v1 already put its
// provider seam at Module+Internal.h, so the public HookKitModule methods below
// are v1's own, unchanged, and only the eight Internal primitives are new --
// backed by the same plan lifecycle HKSubstitutor drives.
//
// Not a plugin system: HookKitCore serves one built-in module and never reads a
// bundle from /Library/Modulous. registerModule:withIdentifier: still stores and
// returns a caller's own subclass, because that was v1 behaviour and costs
// nothing to keep.

#import "../../Headers/HookKit.h"
#import "../../Headers/HookKit/Module+Internal.h"
#import "HKLegacyFacade.h"

#import <mach-o/dyld.h>

#include <stdlib.h>

// The identifier v1 consumers see for the built-in module. v1 read this out of
// a bundle's ModuleInfo dictionary; there is exactly one module now.
static NSString *const HKLegacyBuiltinModuleIdentifier = @"me.jjolano.hookkit";

// File-static, not selectors: every method on these classes is v1 ABI surface,
// so helpers must not become part of it.
static HKSubstitutor *hk_legacy_substitutor(void) {
    return [HKSubstitutor defaultSubstitutor];
}

// v1's info dictionaries came from a bundle's ModuleInfo; there is one module
// now, and its Description names the engines the facade would actually route to
// so the row stays informative rather than a bare constant.
static NSDictionary *hk_legacy_builtin_module_info(void) {
    NSArray<NSString *> *backendIDs = [HKSubstitutor getAvailableBackendIDs];
    NSString *description = [backendIDs count]
        ? [NSString stringWithFormat:@"HookKit (%@)", [backendIDs componentsJoinedByString:@", "]]
        : @"HookKit";

    return @{
        @"Identifier": HKLegacyBuiltinModuleIdentifier,
        @"Description": description,
        @"Priority": @(0)
    };
}

#pragma mark - Hook carriers

// Verbatim v1 (Hook.m): plain data, no behaviour. The ABI here is the property
// layout and the three factory selectors.

@implementation HookKitHook
@end

@implementation HookKitClassHook
@synthesize objcClass, selector, replacement, orig;

+ (instancetype)hook:(Class)objcClass selector:(SEL)selector replacement:(void *)replacement orig:(void **)orig {
    HookKitClassHook* hook = [HookKitClassHook new];

    [hook setObjcClass:objcClass];
    [hook setSelector:selector];
    [hook setReplacement:replacement];
    [hook setOrig:orig];

    return hook;
}
@end

@implementation HookKitFunctionHook
@synthesize function, replacement, orig;

+ (instancetype)hook:(void *)function replacement:(void *)replacement orig:(void **)orig {
    HookKitFunctionHook* hook = [HookKitFunctionHook new];

    [hook setFunction:function];
    [hook setReplacement:replacement];
    [hook setOrig:orig];

    return hook;
}
@end

@implementation HookKitMemoryHook
@synthesize target, data, size;

+ (instancetype)hook:(void *)target data:(const void *)data size:(size_t)size {
    HookKitMemoryHook* hook = [HookKitMemoryHook new];

    [hook setTarget:target];
    [hook setData:data];
    [hook setSize:size];

    return hook;
}
@end

#pragma mark - HookKitModule

@implementation HookKitModule
@synthesize functionHookBatchingSupported, memoryHookBatchingSupported, nullImageSearchSupported;

- (instancetype)init {
    if((self = [super init])) {
        // Function specs can be built ahead of execution, so a function batch
        // is one plan lifecycle. Memory cannot: expected_bytes must be captured
        // at execute time, not enqueue time (HKSubstitutor.m:executeHooks), so
        // _hookRegions: declines and v1's own per-op fallback runs instead.
        functionHookBatchingSupported = YES;
        memoryHookBatchingSupported = NO;
        // findSymbolInImage:nil resolves through the image catalog, so the
        // caller never has to walk _dyld_image_count itself.
        nullImageSearchSupported = YES;
    }

    return self;
}

// --- v1 public surface (verbatim from v1 Module.m) ---

- (BOOL)executeHook:(__kindof HookKitHook *)hook {
    if([hook isKindOfClass:[HookKitClassHook class]]) {
        HookKitClassHook* classHook = hook;
        return [self _hookClass:[classHook objcClass] selector:[classHook selector] replacement:[classHook replacement] orig:[classHook orig]];
    }

    if([hook isKindOfClass:[HookKitFunctionHook class]]) {
        HookKitFunctionHook* functionHook = hook;
        return [self _hookFunction:[functionHook function] replacement:[functionHook replacement] orig:[functionHook orig]];
    }

    if([hook isKindOfClass:[HookKitMemoryHook class]]) {
        HookKitMemoryHook* memoryHook = hook;
        return [self _hookRegion:[memoryHook target] data:[memoryHook data] size:[memoryHook size]];
    }

    return NO;
}

- (int)executeHooks:(NSArray<__kindof HookKitHook *> *)hooks {
    int total = (int)[hooks count];
    int result = 0;

    NSMutableArray<HookKitFunctionHook *>* functionHooks = nil;

    if([self functionHookBatchingSupported]) {
        functionHooks = [NSMutableArray new];
    }

    NSMutableArray<HookKitMemoryHook *>* memoryHooks = nil;

    if([self memoryHookBatchingSupported]) {
        memoryHooks = [NSMutableArray new];
    }

    for(__kindof HookKitHook* hook in hooks) {
        if([hook isKindOfClass:[HookKitClassHook class]]) {
            if([self executeHook:hook]) {
                result += 1;
            }

            continue;
        }

        if([hook isKindOfClass:[HookKitFunctionHook class]]) {
            if(functionHooks) {
                [functionHooks addObject:hook];
            } else {
                if([self executeHook:hook]) {
                    result += 1;
                }
            }

            continue;
        }

        if([hook isKindOfClass:[HookKitMemoryHook class]]) {
            if(memoryHooks) {
                [memoryHooks addObject:hook];
            } else {
                if([self executeHook:hook]) {
                    result += 1;
                }
            }

            continue;
        }
    }

    int function_batch_result = functionHooks ? [self _hookFunctions:functionHooks] : -1;

    if(function_batch_result == -1) {
        // batching not supported, do one at a time
        for(HookKitFunctionHook* functionHook in functionHooks) {
            if([self executeHook:functionHook]) {
                result += 1;
            }
        }
    } else {
        result += function_batch_result;
    }

    int memory_batch_result = memoryHooks ? [self _hookRegions:memoryHooks] : -1;

    if(memory_batch_result == -1) {
        // batching not supported, do one at a time
        for(HookKitMemoryHook* memoryHook in memoryHooks) {
            if([self executeHook:memoryHook]) {
                result += 1;
            }
        }
    } else {
        result += memory_batch_result;
    }

    if(result < total) {
        NSLog(@"[HookKit] warning: successfully hooked less than expected (%d/%lu)", result, (unsigned long)total);
    }

    return result;
}

- (hookkit_image_t)openImageWithURL:(NSURL *)url {
    if(!url) {
        return NULL;
    }

    return (hookkit_image_t)[self _openImage:[[url path] fileSystemRepresentation]];
}

- (hookkit_image_t)openImageWithPath:(NSString *)path {
    if(!path) {
        return NULL;
    }

    NSURL* file_url = [NSURL fileURLWithPath:path isDirectory:NO];
    return [self openImageWithURL:file_url];
}

- (void)closeImage:(hookkit_image_t)image {
    if(image) {
        [self _closeImage:(void *)image];
    }
}

- (void *)findSymbolName:(NSString *)name {
    if(!name) {
        return NULL;
    }

    return [self findSymbolName:name inImage:NULL];
}

- (void *)findSymbolName:(NSString *)name inImage:(hookkit_image_t)image {
    if(!name) {
        return NULL;
    }

    if([self nullImageSearchSupported] || image) {
        return [self _findSymbol:[name UTF8String] image:(void *)image];
    }

    // iterate through all loaded dyld images and call findSymbol
    int count = (int)_dyld_image_count();

    for(int i = 0; i < count; i++) {
        const char* image_name = _dyld_get_image_name(i);

        if(image_name) {
            void* _image = [self _openImage:image_name];
            void* symbol = [self _findSymbol:[name UTF8String] image:_image];
            [self _closeImage:_image];

            if(symbol) {
                return symbol;
            }
        }
    }

    return NULL;
}
@end

#pragma mark - HookKitModule (Internal)

// v1's provider seam. A bundle used to supply these; the facade does now. Each
// maps a v1 BOOL/count onto the facade's hookkit_status_t, keeping v1's
// "did it install" contract -- HK_OK only, so a queued-but-not-installed or
// partial result never reads as success.

@implementation HookKitModule (Internal)

- (BOOL)_hookClass:(Class)objcClass selector:(SEL)selector replacement:(void *)replacement orig:(void **)orig {
    return [hk_legacy_substitutor() hookMessageInClass:objcClass
                                        withSelector:selector
                                     withReplacement:replacement
                                           outOldPtr:orig] == HK_OK;
}

- (BOOL)_hookFunction:(void *)function replacement:(void *)replacement orig:(void **)orig {
    return [hk_legacy_substitutor() hookFunction:function
                               withReplacement:replacement
                                     outOldPtr:orig] == HK_OK;
}

// One plan lifecycle for the whole batch, and an exact success count -- the
// per-op results the plural bridge writes are what v1 callers count on.
// Returns -1 for "not supported" so v1's per-op fallback takes over.
- (int)_hookFunctions:(NSArray<HookKitFunctionHook *> *)functions {
    size_t count = [functions count];

    if(count == 0) {
        return 0;
    }

    hk_hook_spec_t* specs = calloc(count, sizeof(*specs));
    void*** originals = calloc(count, sizeof(*originals));
    int* results = calloc(count, sizeof(*results));
    // id_buf must outlive hk_plan_add_hook's deep copy of the spec, so the
    // whole block stays alive until apply_specs returns.
    char (*id_bufs)[48] = calloc(count, sizeof(*id_bufs));

    if(!specs || !originals || !results || !id_bufs) {
        free(specs);
        free(originals);
        free(results);
        free(id_bufs);
        return -1;
    }

    size_t built = 0;

    for(size_t i = 0; i < count; i++) {
        HookKitFunctionHook* hook = functions[i];
        int status = hk_legacy_build_function_spec([hook function], [hook replacement],
                                                    [hook orig], id_bufs[built],
                                                    sizeof(id_bufs[built]),
                                                    &specs[built]);

        if(status == HK_LEGACY_OK) {
            originals[built] = [hook orig];
            built += 1;
        }
    }

    int result = 0;

    if(built > 0) {
        hk_legacy_apply_specs(specs, originals, built, results);

        for(size_t i = 0; i < built; i++) {
            if(results[i] == HK_LEGACY_OK) {
                result += 1;
            }
        }
    }

    free(specs);
    free(originals);
    free(results);
    free(id_bufs);

    return result;
}

- (BOOL)_hookRegion:(void *)target data:(const void *)data size:(size_t)size {
    return [hk_legacy_substitutor() hookMemory:target withData:data size:size] == HK_OK;
}

- (int)_hookRegions:(NSArray<HookKitMemoryHook *> *)regions {
    // Declined on purpose: expected_bytes must be captured at execute time, so
    // there is nothing to gain from pre-building a memory batch.
    (void)regions;
    return -1;
}

- (void *)_openImage:(const char *)path {
    if(!path) {
        return NULL;
    }

    return (void *)[hk_legacy_substitutor() openImage:@(path)];
}

- (void)_closeImage:(void *)image {
    [hk_legacy_substitutor() closeImage:(HKImageRef)image];
}

- (void *)_findSymbol:(const char *)symbol image:(void *)image {
    if(!symbol) {
        return NULL;
    }

    return [hk_legacy_substitutor() findSymbolInImage:(HKImageRef)image symbolName:@(symbol)];
}
@end

#pragma mark - HookKitCore

@implementation HookKitCore {
    NSMutableDictionary<NSString *, __kindof HookKitModule *>* registeredModules;
}

+ (instancetype)sharedInstance {
    static HookKitCore* sharedInstance = nil;
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        sharedInstance = [HookKitCore new];
    });

    return sharedInstance;
}

- (instancetype)init {
    if((self = [super init])) {
        registeredModules = [NSMutableDictionary new];
    }

    return self;
}

- (__kindof HookKitModule *)defaultModule {
    static HookKitModule* defaultModule = nil;
    static dispatch_once_t onceToken = 0;

    dispatch_once(&onceToken, ^{
        defaultModule = [HookKitModule new];
    });

    return defaultModule;
}

- (NSArray<NSDictionary *> *)getModuleInfo {
    return @[hk_legacy_builtin_module_info()];
}

- (NSDictionary *)getModuleInfoWithIdentifier:(NSString *)identifier {
    if([identifier isEqualToString:HKLegacyBuiltinModuleIdentifier]) {
        return hk_legacy_builtin_module_info();
    }

    return nil;
}

- (__kindof HookKitModule *)getModuleWithIdentifier:(NSString *)identifier {
    if(!identifier) {
        return nil;
    }

    __kindof HookKitModule* result = nil;

    @synchronized(registeredModules) {
        result = [registeredModules objectForKey:identifier];
    }

    if(!result && [identifier isEqualToString:HKLegacyBuiltinModuleIdentifier]) {
        result = [self defaultModule];
    }

    return result;
}

- (void)registerModule:(__kindof HookKitModule *)module withIdentifier:(NSString *)identifier {
    if(module && identifier) {
        @synchronized(registeredModules) {
            [registeredModules setObject:module forKey:identifier];
        }
    }
}
@end
