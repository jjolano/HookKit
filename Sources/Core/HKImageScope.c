// Image-scope check. See HKImageScope.h for the one policy decision.

#include "HKImageScope.h"

#include <string.h>

#include "../Resolvers/HKMachO.h"

typedef struct {
    uintptr_t address;
    bool expect_uuid;
    const uint8_t *expected_uuid;

    bool saw_entry;        // the selector matched something
    bool saw_uuid_match;   // ...and at least one had the right UUID
    bool saw_unreadable;   // ...and at least one could not be bounded
    bool contained;        // ...and at least one contains the address
} scope_ctx_t;

static bool scope_visit(void *vctx, size_t index, const hk_image_entry_t *entry) {
    scope_ctx_t *ctx = vctx;
    (void)index;
    ctx->saw_entry = true;

    if (ctx->expect_uuid) {
        // An entry with no UUID recorded cannot satisfy a UUID requirement.
        // Treated as a mismatch rather than waved through: the caller asked
        // for a specific build and "unknown" is not that build.
        if (!entry->uuid_present ||
            memcmp(entry->uuid, ctx->expected_uuid, 16) != 0) {
            return true;  // keep looking; another load may be the right one
        }
    }
    ctx->saw_uuid_match = true;

    if (!entry->header) {
        ctx->saw_unreadable = true;
        return true;
    }

    // Derive the safe load-command bound from the header itself. This is the
    // whole reason hk_macho_peek_header exists: the catalog records where the
    // header is, not how far it may be read, and read_header cannot tell us
    // because it validates the region against the size we are computing.
    hk_macho_header_t header;
    if (hk_macho_peek_header(entry->header, HK_MACHO_HEADER_64_SIZE, &header) != HK_MACHO_OK) {
        ctx->saw_unreadable = true;
        return true;
    }
    const size_t bound = HK_MACHO_HEADER_64_SIZE + (size_t)header.sizeofcmds;

    uintptr_t start = 0, end = 0;
    if (hk_macho_image_span_for_loaded_image(entry->header, bound, entry->slide,
                                             &start, &end) != HK_MACHO_OK) {
        ctx->saw_unreadable = true;
        return true;
    }

    if (ctx->address >= start && ctx->address < end) {
        ctx->contained = true;
        return false;  // satisfied; no reason to keep walking
    }
    return true;
}

hk_image_scope_status_t hk_image_scope_check(const hk_image_catalog_t *catalog,
                                             const hk_image_selector_t *selector,
                                             bool expect_uuid,
                                             const uint8_t expected_uuid[16],
                                             uintptr_t address) {
    if (!selector) {
        return HK_IMAGE_SCOPE_OK;  // nothing was asked
    }
    if (expect_uuid && !expected_uuid) {
        return HK_IMAGE_SCOPE_OK;  // a UUID requirement with no UUID asks nothing
    }
    // The policy, and the only place it lives: no catalog means not checked.
    // See the header for why this is a skip and not a failure.
    if (!catalog || hk_image_catalog_count(catalog) == 0) {
        return HK_IMAGE_SCOPE_NO_CATALOG;
    }

    scope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.address = address;
    ctx.expect_uuid = expect_uuid;
    ctx.expected_uuid = expected_uuid;

    (void)hk_image_catalog_match(catalog, selector, scope_visit, &ctx);

    if (ctx.contained) {
        return HK_IMAGE_SCOPE_OK;
    }
    // Reported in increasing order of how much was established, so the status
    // always says how far the check actually got.
    if (!ctx.saw_entry) {
        return HK_IMAGE_SCOPE_NO_MATCH;
    }
    if (!ctx.saw_uuid_match) {
        return HK_IMAGE_SCOPE_UUID_MISMATCH;
    }
    if (ctx.saw_unreadable) {
        return HK_IMAGE_SCOPE_UNREADABLE_IMAGE;
    }
    return HK_IMAGE_SCOPE_ADDRESS_OUTSIDE;
}

// ---- identity form ------------------------------------------------------

typedef struct {
    const void *header;
    bool expect_uuid;
    const uint8_t *expected_uuid;
    bool saw_entry;
    bool saw_uuid_match;
    bool matched;
} header_ctx_t;

static bool header_visit(void *vctx, size_t index, const hk_image_entry_t *entry) {
    header_ctx_t *ctx = vctx;
    (void)index;
    ctx->saw_entry = true;
    if (ctx->expect_uuid) {
        if (!entry->uuid_present ||
            memcmp(entry->uuid, ctx->expected_uuid, 16) != 0) {
            return true;
        }
    }
    ctx->saw_uuid_match = true;
    if (entry->header == ctx->header) {
        ctx->matched = true;
        return false;
    }
    return true;
}

hk_image_scope_status_t hk_image_scope_check_header(const hk_image_catalog_t *catalog,
                                                    const hk_image_selector_t *selector,
                                                    bool expect_uuid,
                                                    const uint8_t expected_uuid[16],
                                                    const void *header) {
    if (!selector) {
        return HK_IMAGE_SCOPE_OK;  // nothing was asked
    }
    if (expect_uuid && !expected_uuid) {
        return HK_IMAGE_SCOPE_OK;
    }
    if (!catalog || hk_image_catalog_count(catalog) == 0) {
        return HK_IMAGE_SCOPE_NO_CATALOG;
    }

    header_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.header = header;
    ctx.expect_uuid = expect_uuid;
    ctx.expected_uuid = expected_uuid;

    (void)hk_image_catalog_match(catalog, selector, header_visit, &ctx);

    if (ctx.matched) {
        return HK_IMAGE_SCOPE_OK;
    }
    if (!ctx.saw_entry) {
        return HK_IMAGE_SCOPE_NO_MATCH;
    }
    if (!ctx.saw_uuid_match) {
        return HK_IMAGE_SCOPE_UUID_MISMATCH;
    }
    // Images matching the selector exist, but this is not one of them.
    return HK_IMAGE_SCOPE_NO_MATCH;
}

const char *hk_image_scope_describe(hk_image_scope_status_t status) {
    switch (status) {
        case HK_IMAGE_SCOPE_OK:
            return "address is inside the expected image";
        case HK_IMAGE_SCOPE_NO_CATALOG:
            return "image scope not checked: no image catalog is available";
        case HK_IMAGE_SCOPE_NO_MATCH:
            return "the image the request named is not loaded";
        case HK_IMAGE_SCOPE_UUID_MISMATCH:
            return "the image is loaded but is a different build than the request expected";
        case HK_IMAGE_SCOPE_ADDRESS_OUTSIDE:
            return "the address is not inside the image the request named";
        case HK_IMAGE_SCOPE_UNREADABLE_IMAGE:
            return "a matching image's header could not be parsed to bound it";
    }
    return "unknown image scope status";
}
