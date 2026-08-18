// Image catalog -- Milestone 5. The set of loaded Mach-O images HookKit
// resolves selectors and symbols against. Internal (callers pass
// hk_image_selector_t in specs; HookKit resolves them), not public API.
//
// Deliberate host/device split (this is the whole reason the file is shaped
// this way): the SELECTOR-MATCHING logic below is platform-agnostic and
// host-tested against synthetic entries. The only device-only part is
// POPULATING the catalog from the live process, which needs dyld
// (_dyld_image_count / _dyld_get_image_header / ...) and a real Mach-O
// runtime -- that seam is hk_image_catalog_populate_from_dyld, declared but
// NOT implemented here, because it cannot be compiled or run on this Linux
// host and this project does not ship code it can't verify. Tests populate
// via hk_image_catalog_add_entry instead.

#ifndef HK_CORE_IMAGE_CATALOG_H
#define HK_CORE_IMAGE_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../Headers/HookKit/HookKitTargets.h"  // hk_image_selector_t

#ifdef __cplusplus
extern "C" {
#endif

// One catalog entry. `path` is a view into catalog-owned storage (stable
// until the catalog is destroyed); `header` is a borrowed pointer (a
// mach_header* on device, any distinct sentinel in host tests).
typedef struct {
    const char *path;
    bool uuid_present;
    uint8_t uuid[16];
    const void *header;
    uintptr_t slide;
    bool is_main_executable;
} hk_image_entry_t;

typedef struct hk_image_catalog hk_image_catalog_t;

hk_image_catalog_t *hk_image_catalog_create(void);
void hk_image_catalog_destroy(hk_image_catalog_t *catalog);

// Copies `entry` into the catalog (deep-copying its path). `entry->path`
// need not outlive the call. Returns false on OOM. Bumps the generation.
bool hk_image_catalog_add_entry(hk_image_catalog_t *catalog,
                                const hk_image_entry_t *entry);

size_t hk_image_catalog_count(const hk_image_catalog_t *catalog);

// Monotonic counter bumped whenever the image set changes (spec
// ARCHITECTURE.md invariant #3: a resolution records the generation it was
// made at, and commit revalidates against it). Nothing revalidates against
// it yet -- that lands with late-image handling (Milestone 12) -- but the
// counter is real so resolvers can start stamping it now.
uint64_t hk_image_catalog_generation(const hk_image_catalog_t *catalog);

// Visits each entry matching `selector`, in catalog (index) order, exactly
// once -- even when an EXPLICIT_SET selector's sub-selectors overlap. If
// `visit` returns false, iteration stops early. Returns the number of
// entries visited. A NULL catalog/selector/visit visits nothing.
typedef bool (*hk_image_visit_fn)(void *ctx, size_t index,
                                  const hk_image_entry_t *entry);
size_t hk_image_catalog_match(const hk_image_catalog_t *catalog,
                              const hk_image_selector_t *selector,
                              hk_image_visit_fn visit, void *ctx);

// DEVICE-ONLY SEAM, intentionally not defined here (see file header):
// populates the catalog from the live dyld image list. Milestone 5's device
// work; requires SSH to a jailbroken device to build and verify.
//   void hk_image_catalog_populate_from_dyld(hk_image_catalog_t *catalog);

#ifdef __cplusplus
}
#endif

#endif // HK_CORE_IMAGE_CATALOG_H
