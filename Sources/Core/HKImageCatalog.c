// Image catalog implementation -- the platform-agnostic half (structure +
// selector matching). See HKImageCatalog.h for the host/device split; the
// dyld populator is deliberately absent.

#include "HKImageCatalog.h"

#include <stdlib.h>
#include <string.h>

struct hk_image_catalog {
    hk_image_entry_t *entries;   // path points into per-entry owned storage
    size_t count;
    size_t capacity;
    uint64_t generation;
};

hk_image_catalog_t *hk_image_catalog_create(void) {
    return (hk_image_catalog_t *)calloc(1, sizeof(hk_image_catalog_t));
}

void hk_image_catalog_destroy(hk_image_catalog_t *catalog) {
    if (!catalog) {
        return;
    }
    for (size_t i = 0; i < catalog->count; i++) {
        free((char *)catalog->entries[i].path);  // catalog-owned copy
    }
    free(catalog->entries);
    free(catalog);
}

bool hk_image_catalog_add_entry(hk_image_catalog_t *catalog,
                                const hk_image_entry_t *entry) {
    if (!catalog || !entry) {
        return false;
    }
    // Deep-copy the path first, so a mid-add OOM leaves the catalog unchanged.
    char *owned_path = NULL;
    if (entry->path) {
        size_t n = strlen(entry->path) + 1;
        owned_path = (char *)malloc(n);
        if (!owned_path) {
            return false;
        }
        memcpy(owned_path, entry->path, n);
    }

    if (catalog->count == catalog->capacity) {
        size_t new_capacity = catalog->capacity == 0 ? 8 : catalog->capacity * 2;
        hk_image_entry_t *grown = (hk_image_entry_t *)realloc(
            catalog->entries, new_capacity * sizeof(hk_image_entry_t));
        if (!grown) {
            free(owned_path);
            return false;
        }
        catalog->entries = grown;
        catalog->capacity = new_capacity;
    }

    hk_image_entry_t *slot = &catalog->entries[catalog->count];
    *slot = *entry;             // copies uuid (by value), header/slide/flags
    slot->path = owned_path;    // repoint at the owned copy
    catalog->count++;
    catalog->generation++;
    return true;
}

size_t hk_image_catalog_count(const hk_image_catalog_t *catalog) {
    return catalog ? catalog->count : 0;
}

uint64_t hk_image_catalog_generation(const hk_image_catalog_t *catalog) {
    return catalog ? catalog->generation : 0;
}

// Does a single entry satisfy a selector? Written as "test the entry" rather
// than "find entries for the kind" so that (a) each entry is naturally
// considered once -- the caller iterates entries in order, dedup for free --
// and (b) EXPLICIT_SET is just "matches any sub-selector", recursively.
static bool entry_matches_selector(const hk_image_entry_t *e,
                                   const hk_image_selector_t *sel) {
    switch (sel->kind) {
    case HK_IMAGE_ANY_LOADED:
        return true;
    case HK_IMAGE_MAIN_EXECUTABLE:
        return e->is_main_executable;
    case HK_IMAGE_EXACT_PATH:
        return e->path && sel->path && strcmp(e->path, sel->path) == 0;
    case HK_IMAGE_EXACT_UUID:
        return e->uuid_present && memcmp(e->uuid, sel->uuid, sizeof(e->uuid)) == 0;
    case HK_IMAGE_EXACT_HEADER:
        return e->header != NULL && e->header == sel->header;
    case HK_IMAGE_EXPLICIT_SET:
        for (size_t i = 0; i < sel->explicit_set_count; i++) {
            const hk_image_selector_t *sub = sel->explicit_set[i];
            if (sub && entry_matches_selector(e, sub)) {
                return true;  // union: any sub-selector matching is enough
            }
        }
        return false;
    default:
        return false;  // unknown selector kind matches nothing
    }
}

size_t hk_image_catalog_match(const hk_image_catalog_t *catalog,
                              const hk_image_selector_t *selector,
                              hk_image_visit_fn visit, void *ctx) {
    if (!catalog || !selector || !visit) {
        return 0;
    }
    size_t visited = 0;
    for (size_t i = 0; i < catalog->count; i++) {
        if (entry_matches_selector(&catalog->entries[i], selector)) {
            visited++;
            if (!visit(ctx, i, &catalog->entries[i])) {
                break;  // visitor asked to stop
            }
        }
    }
    return visited;
}
