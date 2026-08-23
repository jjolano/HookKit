#include "../Sources/Core/HKImageCatalog.h"

#include <stdio.h>

static bool count_image(void *ctx, size_t index, const hk_image_entry_t *entry) {
    size_t *count = (size_t *)ctx;
    (void)index;
    if (!entry || !entry->header || !entry->path) {
        return false;
    }
    (*count)++;
    return true;
}

int main(void) {
    hk_image_catalog_t *catalog = hk_image_catalog_create();
    if (!catalog || !hk_image_catalog_populate_from_dyld(catalog) ||
        hk_image_catalog_count(catalog) == 0) {
        puts("HookKit image catalog: FAIL");
        hk_image_catalog_destroy(catalog);
        return 1;
    }

    hk_image_selector_t selector = {0};
    selector.kind = HK_IMAGE_ANY_LOADED;
    size_t matched = 0;
    size_t visited = hk_image_catalog_match(catalog, &selector, count_image, &matched);
    bool pass = visited == matched && matched == hk_image_catalog_count(catalog);
    printf("HookKit image catalog: %s (%zu images)\n", pass ? "PASS" : "FAIL", matched);
    hk_image_catalog_destroy(catalog);
    return pass ? 0 : 1;
}
