#ifndef HK_DYLD_CACHE_PATCHES_H
#define HK_DYLD_CACHE_PATCHES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../internal/HKPointerAuth.h"
#include "HKSymbolTable.h"

typedef enum {
    HK_CACHE_PATCH_OK = 0,
    HK_CACHE_PATCH_NOT_CACHE,
    HK_CACHE_PATCH_NO_METADATA,
    HK_CACHE_PATCH_NOT_FOUND,
    HK_CACHE_PATCH_INVALID_ARGUMENT,
    HK_CACHE_PATCH_MALFORMED,
    HK_CACHE_PATCH_UNSUPPORTED,
    HK_CACHE_PATCH_SCOPE_UNREPRESENTABLE,
} hk_cache_patch_status_t;

typedef struct {
    const void *cache_base;
    size_t cache_size;
    const void *image_header;
    size_t image_header_size;
    uintptr_t image_slide;
    const char *image_path;
    bool uuid_present;
    uint8_t uuid[16];
    bool include_shared_got;
} hk_cache_patch_target_t;

typedef struct {
    uintptr_t address;
    hk_pac_schema_t schema;
    int64_t addend;
    bool weak_import;
    bool shared_got;
} hk_cache_patch_site_t;

typedef bool (*hk_cache_patch_visit_fn)(void *ctx,
                                        const hk_cache_patch_site_t *site);

// Enumerates patch-table uses of `symbol_name` belonging to one loaded cache
// image. The live cache is read-only; only the returned runtime slots are used
// later by the rebind engine.
hk_cache_patch_status_t hk_dyld_cache_iterate_symbol_uses(
    const hk_cache_patch_target_t *target, const char *symbol_name,
    hk_symbol_name_convention_t convention, hk_cache_patch_visit_fn visit,
    void *ctx);

#endif
