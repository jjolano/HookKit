// HookKit 3 canonical compatibility facade.
//
// This keeps the historical Objective-C ABI intentionally thin: it only
// translates calls into HookKit 3 plans/engines and never selects or invokes
// the former 2.x backend router.

#import "../../Headers/HookKit.h"
#import "HKLegacyFacade.h"

#include "../Core/HKImageCatalog.h"
#include "../Core/HKRuntimeInternal.h"
#include "../Resolvers/HKMachO.h"
#include "../Resolvers/HKSymbolResolve.h"

#import <objc/runtime.h>

#include <stdlib.h>
#include <string.h>

enum { HK_LEGACY_IMAGE_MAGIC = 0x484B3349u };  // canonical image wrapper magic

// Keep the private selector ABI from the old facade without importing its
// backend router. NS_ENUM(uint8_t, ...) intentionally encodes an out pointer
// as ^C rather than Objective-C's special char * spelling.
typedef NS_ENUM(uint8_t, HKLegacyTechnique) {
    HKLegacyTechniqueNone,
};

// This is a public ABI detail of 2.x: do not add fields.  The actual HookKit
// image snapshot lives in rawHandle.
struct HKImage {
    uint32_t magic;
    hookkit_lib_t ownerType;
    void *rawHandle;
};

typedef struct {
    hk_runtime_t *runtime;
    const void *header;
    uintptr_t slide;
} hk_legacy_image_t;

typedef enum {
    HK_LEGACY_OP_MESSAGE,
    HK_LEGACY_OP_FUNCTION,
    HK_LEGACY_OP_MEMORY,
} hk_legacy_operation_kind_t;

@interface HKLegacyOperation : NSObject {
@public
    hk_legacy_operation_kind_t kind;
    Class objcClass;
    SEL selector;
    void *target;
    void *replacement;
    void **oldPtr;
    NSData *bytes;
    // Batched MESSAGE/FUNCTION ops carry a pre-built spec; id_buf holds the
    // unique stable_hook_id and must outlive hk_plan_add_hook's deep copy.
    hk_hook_spec_t spec;
    char id_buf[48];
}
@end

@implementation HKLegacyOperation
@end

static bool hk_legacy_capture_image(void *opaque, size_t index,
                                     const hk_image_entry_t *entry) {
    (void)index;
    hk_legacy_image_t *image = opaque;
    if (!entry || !entry->header) {
        return true;
    }
    image->header = entry->header;
    image->slide = entry->slide;
    return false;
}

static bool hk_legacy_find_image(void *opaque, size_t index,
                                  const hk_image_entry_t *entry) {
    (void)index;
    struct {
        const char *name;
        void *result;
    } *ctx = opaque;
    if (!entry || !entry->header || ctx->result) {
        return !ctx->result;
    }

    hk_macho_header_t header;
    if (hk_macho_peek_header(entry->header, HK_MACHO_HEADER_64_SIZE,
                             &header) != HK_MACHO_OK) {
        return true;
    }
    hk_symbol_resolution_t resolved;
    if (hk_resolve_loaded_image_symbol(
            entry->header, HK_MACHO_HEADER_64_SIZE + header.sizeofcmds,
            entry->slide, ctx->name, HK_SYMBOL_NAME_C,
            HK_SYMBOL_VISIBILITY_ANY, &resolved) == HK_RESOLVE_OK) {
        ctx->result = (void *)resolved.address;
    }
    return ctx->result == NULL;
}

static void *hk_legacy_find_symbol_in_payload(const hk_legacy_image_t *image,
                                                const char *name) {
    if (!image || !image->header || !name || !name[0]) {
        return NULL;
    }
    hk_macho_header_t header;
    if (hk_macho_peek_header(image->header, HK_MACHO_HEADER_64_SIZE,
                             &header) != HK_MACHO_OK) {
        return NULL;
    }
    hk_symbol_resolution_t resolved;
    return hk_resolve_loaded_image_symbol(
        image->header, HK_MACHO_HEADER_64_SIZE + header.sizeofcmds,
        image->slide, name, HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY,
        &resolved) == HK_RESOLVE_OK ? (void *)resolved.address : NULL;
}

@interface HKSubstitutor ()
- (void)noteHookResult:(hookkit_status_t)status;
- (void)noteHookResult:(hookkit_status_t)status fromBackend:(id)backend;
- (hookkit_lib_t)backendType;
- (hookkit_lib_t)typeForBackend:(id)backend;
- (unsigned char)resolvedTechniqueForBackend:(id)backend;
- (BOOL)backendHasNativeBatch:(id)backend;
- (BOOL)enqueueKind:(int)kind status:(hookkit_status_t *)outStatus
              build:(void (^)(id operation))build;
- (hookkit_status_t)executeOperation:(id)operation onBackend:(id)backend;
- (void)drainGroup:(NSArray *)hooks onBackend:(id)backend;
- (void)drainRoutedHooks:(NSArray *)hooks;
- (BOOL)routeFunctionOperation:(id)operation;
- (BOOL)routeMemoryOperation:(id)operation;
- (id)hk_backendForAutoCoverFunction:(void *)function
                         replacement:(void *)replacement;
- (id)hk_backendForAutoCoverFunction:(void *)function
                         replacement:(void *)replacement
                      requestOriginal:(BOOL)requestOriginal
                            technique:(HKLegacyTechnique *)technique;
- (id)hk_backendForAutoCoverFunction:(void *)function
                           replacement:(void *)replacement
                       requestOriginal:(BOOL)requestOriginal
                    startingAtCursor:(NSUInteger)cursor
                      nextCursor:(NSUInteger *)nextCursor
                         technique:(HKLegacyTechnique *)technique;
@end

@implementation HKSubstitutor {
    hookkit_lib_t _types;
    BOOL _batching;
    NSMutableArray<HKLegacyOperation *> *_queuedOperations;
    int _lastError;
}

+ (id)defaultBackend {
    return nil;
}

+ (instancetype)defaultSubstitutor {
    static HKSubstitutor *substitutor;
    @synchronized(self) {
        if (!substitutor) {
            substitutor = [self new];
        }
    }
    return substitutor;
}

+ (hookkit_lib_t)getAvailableSubstitutorTypes {
    return HK_LIB_NONE;
}

+ (hookkit_cat_t)getAvailableCategories {
    return HK_CAT_NONE;
}

+ (NSArray<NSDictionary *> *)getSubstitutorTypeInfo:(hookkit_lib_t)types {
    (void)types;
    return @[];
}

+ (instancetype)substitutorWithTypes:(hookkit_lib_t)types {
    HKSubstitutor *substitutor = [self new];
    substitutor.types = types;
    return substitutor;
}

+ (instancetype)substitutorWithOrderedTypes:(NSArray<NSNumber *> *)types {
    HKSubstitutor *substitutor = [self new];
    if ([types.firstObject isKindOfClass:[NSNumber class]]) {
        substitutor.types = (hookkit_lib_t)types.firstObject.unsignedIntegerValue;
    }
    return substitutor;
}

+ (instancetype)substitutorWithOrderedCategories:(NSArray<NSNumber *> *)categories {
    (void)categories;
    return [self new];
}

+ (instancetype)substitutorWithAutoCoverCategories:(NSArray<NSNumber *> *)categories {
    (void)categories;
    return [self new];
}

+ (instancetype)substitutorWithCategory:(hookkit_cat_t)category {
    (void)category;
    return [self new];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _queuedOperations = [NSMutableArray new];
    }
    return self;
}

- (hookkit_lib_t)types { return _types; }
- (void)setTypes:(hookkit_lib_t)types { _types = types; }
- (BOOL)batching { return _batching; }
- (void)setBatching:(BOOL)batching { _batching = batching; }
- (hookkit_lib_t)activeType { return HK_LIB_NONE; }
- (HKStrategy)activeStrategy { return HKStrategyDefault; }

- (void)initLibraries {
    // 3.0 deliberately creates/activates only while an operation executes.
}

- (hookkit_lib_t)backendType { return HK_LIB_NONE; }
- (hookkit_lib_t)typeForBackend:(id)backend { (void)backend; return HK_LIB_NONE; }
- (unsigned char)resolvedTechniqueForBackend:(id)backend { (void)backend; return 0; }
- (BOOL)backendHasNativeBatch:(id)backend { (void)backend; return NO; }

- (void)noteHookResult:(hookkit_status_t)status {
    _lastError = (int)status;
}

- (void)noteHookResult:(hookkit_status_t)status fromBackend:(id)backend {
    (void)backend;
    [self noteHookResult:status];
}

- (hookkit_status_t)executeOperation:(HKLegacyOperation *)operation onBackend:(id)backend {
    (void)backend;
    switch (operation->kind) {
    case HK_LEGACY_OP_MESSAGE:
        return (hookkit_status_t)hk_legacy_hook_objc((__bridge void *)operation->objcClass,
                                                       (void *)operation->selector,
                                                       operation->replacement,
                                                       operation->oldPtr);
    case HK_LEGACY_OP_FUNCTION:
        return (hookkit_status_t)hk_legacy_hook_function(operation->target,
                                                           operation->replacement,
                                                           operation->oldPtr);
    case HK_LEGACY_OP_MEMORY:
        return (hookkit_status_t)hk_legacy_hook_memory(operation->target,
                                                         operation->bytes.bytes,
                                                         operation->bytes.length);
    }
    return HK_ERR;
}

- (BOOL)enqueueKind:(int)kind status:(hookkit_status_t *)outStatus
              build:(void (^)(id operation))build {
    (void)kind;
    (void)build;
    if (outStatus) {
        *outStatus = HK_ERR_NOT_SUPPORTED;
    }
    return NO;
}

- (void)drainGroup:(NSArray *)hooks onBackend:(id)backend {
    for (HKLegacyOperation *operation in hooks) {
        (void)[self executeOperation:operation onBackend:backend];
    }
}

- (void)drainRoutedHooks:(NSArray *)hooks {
    [self drainGroup:hooks onBackend:nil];
}

- (BOOL)routeFunctionOperation:(id)operation { (void)operation; return NO; }
- (BOOL)routeMemoryOperation:(id)operation { (void)operation; return NO; }

- (id)hk_backendForAutoCoverFunction:(void *)function
                         replacement:(void *)replacement {
    (void)function;
    (void)replacement;
    return nil;
}

- (id)hk_backendForAutoCoverFunction:(void *)function
                         replacement:(void *)replacement
                      requestOriginal:(BOOL)requestOriginal
                            technique:(HKLegacyTechnique *)technique {
    (void)function;
    (void)replacement;
    (void)requestOriginal;
    if (technique) *technique = 0;
    return nil;
}

- (id)hk_backendForAutoCoverFunction:(void *)function
                           replacement:(void *)replacement
                       requestOriginal:(BOOL)requestOriginal
                    startingAtCursor:(NSUInteger)cursor
                      nextCursor:(NSUInteger *)nextCursor
                         technique:(HKLegacyTechnique *)technique {
    (void)function;
    (void)replacement;
    (void)requestOriginal;
    (void)cursor;
    if (nextCursor) *nextCursor = 0;
    if (technique) *technique = 0;
    return nil;
}

- (hookkit_status_t)hookMessageInClass:(Class)objcClass
                         withSelector:(SEL)selector
                      withReplacement:(void *)replacement
                             outOldPtr:(void **)oldPtr {
    if (!objcClass || !selector || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }
    Class dispatchClass = class_getInstanceMethod(objcClass, selector)
        ? objcClass : object_getClass(objcClass);
    if (_batching) {
        HKLegacyOperation *operation = [HKLegacyOperation new];
        operation->kind = HK_LEGACY_OP_MESSAGE;
        Class dispatchClass = class_getInstanceMethod(objcClass, selector)
            ? objcClass : object_getClass(objcClass);
        operation->objcClass = dispatchClass;
        operation->selector = selector;
        operation->replacement = replacement;
        operation->oldPtr = oldPtr;
        int build = hk_legacy_build_objc_spec(
            (__bridge void *)dispatchClass, (void *)selector, replacement,
            oldPtr, operation->id_buf, sizeof(operation->id_buf),
            &operation->spec);
        if (build != HK_LEGACY_OK) {
            [self noteHookResult:build];
            return build;
        }
        @synchronized(self) { [_queuedOperations addObject:operation]; }
        [self noteHookResult:HK_OK];
        return HK_OK;
    }
    hookkit_status_t status = (hookkit_status_t)hk_legacy_hook_objc(
        (__bridge void *)dispatchClass, (void *)selector, replacement, oldPtr);
    [self noteHookResult:status];
    return status;
}

- (hookkit_status_t)hookFunction:(void *)function
                  withReplacement:(void *)replacement
                         outOldPtr:(void **)oldPtr {
    if (!function || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }
    if (_batching) {
        HKLegacyOperation *operation = [HKLegacyOperation new];
        operation->kind = HK_LEGACY_OP_FUNCTION;
        operation->target = function;
        operation->replacement = replacement;
        operation->oldPtr = oldPtr;
        int build = hk_legacy_build_function_spec(
            function, replacement, oldPtr,
            operation->id_buf, sizeof(operation->id_buf), &operation->spec);
        if (build != HK_LEGACY_OK) {
            [self noteHookResult:build];
            return build;
        }
        @synchronized(self) { [_queuedOperations addObject:operation]; }
        [self noteHookResult:HK_OK];
        return HK_OK;
    }
    hookkit_status_t status = (hookkit_status_t)hk_legacy_hook_function(
        function, replacement, oldPtr);
    [self noteHookResult:status];
    return status;
}

- (hookkit_status_t)hookMemory:(void *)target
                      withData:(const void *)data
                           size:(size_t)size {
    if (!target || !data || size == 0) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }
    if (_batching) {
        HKLegacyOperation *operation = [HKLegacyOperation new];
        operation->kind = HK_LEGACY_OP_MEMORY;
        operation->target = target;
        operation->bytes = [NSData dataWithBytes:data length:size];
        @synchronized(self) { [_queuedOperations addObject:operation]; }
        [self noteHookResult:HK_OK];
        return HK_OK;
    }
    hookkit_status_t status = (hookkit_status_t)hk_legacy_hook_memory(target, data, size);
    [self noteHookResult:status];
    return status;
}

- (hookkit_status_t)hookSwiftMethodInClass:(Class)objcClass
                                  withName:(NSString *)name
                           withReplacement:(void *)replacement
                                  outOldPtr:(void **)oldPtr {
    if (!objcClass || ![name isKindOfClass:[NSString class]] || name.length == 0 || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }
    hookkit_status_t status = (hookkit_status_t)hk_legacy_hook_swift_method(
        (__bridge void *)objcClass, name.UTF8String, replacement, oldPtr);
    [self noteHookResult:status];
    return status;
}

- (hookkit_status_t)hookSwiftVtableSlotInClass:(Class)objcClass
                                      withIndex:(NSUInteger)index
                               withReplacement:(void *)replacement
                                      outOldPtr:(void **)oldPtr {
    if (!objcClass || index > UINT32_MAX || !replacement) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }
    hookkit_status_t status = (hookkit_status_t)hk_legacy_hook_swift_slot(
        (__bridge void *)objcClass, (uint32_t)index, replacement, oldPtr);
    [self noteHookResult:status];
    return status;
}

- (HKImageRef)openImage:(NSString *)path {
    if (![path isKindOfClass:[NSString class]] || path.length == 0) {
        return NULL;
    }
    hk_runtime_t *runtime = NULL;
    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK || !runtime) {
        return NULL;
    }
    hk_legacy_image_t *payload = calloc(1, sizeof(*payload));
    struct HKImage *image = calloc(1, sizeof(*image));
    if (!payload || !image) {
        free(payload);
        free(image);
        hk_runtime_release(runtime);
        return NULL;
    }
    payload->runtime = runtime;
    hk_image_selector_t selector = {0};
    selector.struct_size = sizeof(selector);
    selector.struct_version = HK_ABI_VERSION_3_0;
    selector.kind = HK_IMAGE_EXACT_PATH;
    selector.path = path.fileSystemRepresentation;
    (void)hk_image_catalog_match(runtime->catalog, &selector,
                                 hk_legacy_capture_image, payload);
    if (!payload->header) {
        hk_runtime_release(runtime);
        free(payload);
        free(image);
        return NULL;
    }
    image->magic = HK_LEGACY_IMAGE_MAGIC;
    image->ownerType = HK_LIB_NONE;
    image->rawHandle = payload;
    return image;
}

- (void)closeImage:(HKImageRef)opaqueImage {
    struct HKImage *image = (struct HKImage *)opaqueImage;
    if (!image || image->magic != HK_LEGACY_IMAGE_MAGIC) {
        return;
    }
    hk_legacy_image_t *payload = image->rawHandle;
    image->magic = 0;
    if (payload) {
        hk_runtime_release(payload->runtime);
        free(payload);
    }
    free(image);
}

- (void *)findSymbolInImage:(HKImageRef)opaqueImage symbolName:(NSString *)symbolName {
    if (![symbolName isKindOfClass:[NSString class]] || symbolName.length == 0) {
        return NULL;
    }
    const char *name = symbolName.UTF8String;
    const struct HKImage *image = (const struct HKImage *)opaqueImage;
    if (image) {
        if (image->magic != HK_LEGACY_IMAGE_MAGIC) {
            return NULL;
        }
        return hk_legacy_find_symbol_in_payload(image->rawHandle, name);
    }

    hk_runtime_t *runtime = NULL;
    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK || !runtime) {
        return NULL;
    }
    struct { const char *name; void *result; } context = { name, NULL };
    hk_image_selector_t selector = {0};
    selector.struct_size = sizeof(selector);
    selector.struct_version = HK_ABI_VERSION_3_0;
    selector.kind = HK_IMAGE_ANY_LOADED;
    (void)hk_image_catalog_match(runtime->catalog, &selector,
                                 hk_legacy_find_image, &context);
    hk_runtime_release(runtime);
    return context.result;
}

- (hookkit_status_t)findSymbolsInImage:(HKImageRef)image
                           symbolNames:(NSArray<NSString *> *)symbolNames
                            outSymbols:(NSArray<NSValue *> **)outSymbols {
    if (![symbolNames isKindOfClass:[NSArray class]]) {
        [self noteHookResult:HK_ERR_INVALID_ARGUMENT];
        return HK_ERR_INVALID_ARGUMENT;
    }
    NSMutableArray<NSValue *> *symbols = [NSMutableArray arrayWithCapacity:symbolNames.count];
    NSUInteger found = 0;
    for (id candidate in symbolNames) {
        void *symbol = [candidate isKindOfClass:[NSString class]]
            ? [self findSymbolInImage:image symbolName:candidate] : NULL;
        [symbols addObject:[NSValue valueWithPointer:symbol]];
        found += symbol != NULL;
    }
    if (outSymbols) {
        *outSymbols = [symbols copy];
    }
    hookkit_status_t status = found == symbolNames.count ? HK_OK
        : (found ? HK_ERR_PARTIAL : HK_ERR_NOT_SUPPORTED);
    [self noteHookResult:status];
    return status;
}

- (hookkit_status_t)executeHooks {
    NSArray<HKLegacyOperation *> *operations;
    @synchronized(self) {
        operations = [_queuedOperations copy];
        [_queuedOperations removeAllObjects];
    }
    if (operations.count == 0) {
        [self noteHookResult:HK_OK];
        return HK_OK;
    }

    // MESSAGE/FUNCTION ops share ONE runtime/plan lifecycle -- the fixed
    // per-plan cost (runtime/image-catalog init, analysis, report churn)
    // otherwise dominates every hook. MEMORY ops execute individually: their
    // expected-byte capture must happen at execute time, not enqueue time.
    size_t total = operations.count;
    size_t succeeded = 0;

    NSMutableArray<HKLegacyOperation *> *batched = [NSMutableArray array];
    for (HKLegacyOperation *operation in operations) {
        if (operation->kind == HK_LEGACY_OP_MEMORY) {
            hookkit_status_t status = [self executeOperation:operation
                                                  onBackend:nil];
            succeeded += status == HK_OK;
            continue;
        }
        [batched addObject:operation];
    }

    if (batched.count > 0) {
        size_t count = batched.count;
        hk_hook_spec_t *specs = malloc(count * sizeof(*specs));
        void ***originals = malloc(count * sizeof(*originals));
        int *results = malloc(count * sizeof(*results));
        if (specs && originals && results) {
            for (size_t i = 0; i < count; i++) {
                HKLegacyOperation *operation = batched[i];
                specs[i] = operation->spec;
                originals[i] = operation->oldPtr;
            }
            hk_legacy_apply_specs(specs, originals, count, results);
            for (size_t i = 0; i < count; i++) {
                succeeded += results[i] == HK_LEGACY_OK;
            }
        } else {
            // Allocation failure: fall back to the per-op path rather than
            // silently dropping queued hooks.
            for (HKLegacyOperation *operation in batched) {
                hookkit_status_t status = [self executeOperation:operation
                                                      onBackend:nil];
                succeeded += status == HK_OK;
            }
        }
        free(specs);
        free(originals);
        free(results);
    }

    hookkit_status_t status = succeeded == total ? HK_OK
        : (succeeded ? HK_ERR_PARTIAL : HK_ERR);
    [self noteHookResult:status];
    return status;
}

- (int)getLibErrno:(hookkit_lib_t *)outType {
    if (outType) {
        *outType = HK_LIB_NONE;
    }
    return _lastError;
}

@end
