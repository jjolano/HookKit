// Image-scope check: does a given address actually lie in the image a request
// says it should, and is that image the build the request expected?
//
// This closes a recorded correctness gap. Every address-bearing target carries
// an image selector and optionally a UUID (`hk_address_target_t.expected_image`
// / `expected_uuid`, `hk_memory_target_t.base_image`, the defining-image
// selector on symbol targets), and until now nothing consulted them -- an
// engine patched whatever address it was handed. A caller who pinned the image
// got no protection from a slide change, a re-linked build, or a stale address
// computed against a different load of the same dylib.
//
// It is assembled from pieces that already existed rather than new machinery:
//   - hk_image_catalog_match          selector -> entries   (host-tested)
//   - hk_macho_peek_header            the safe read bound   (host-tested)
//   - hk_macho_image_span_for_loaded_image  containment     (host-tested)
// The only genuinely new thing here is the policy that combines them.
//
// THE ONE POLICY DECISION, stated up front because it is what a reader will
// want to challenge: **a NULL or empty catalog means the check is SKIPPED, not
// failed** (HK_IMAGE_SCOPE_NO_CATALOG, which callers treat as "not checked").
// The catalog is filled by hk_image_catalog_populate_from_dyld, which is
// device-only and NOT BUILT. Failing closed would therefore make every
// image-scoped hook fail on device -- a fabricated safety that protects
// nothing and breaks everything. Skipping leaves the gap open on device, but
// open and *visible*: the status says so, and the check goes live the moment
// the populator lands, with no other code changing.

#ifndef HK_CORE_IMAGE_SCOPE_H
#define HK_CORE_IMAGE_SCOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/HookKit/HookKitTargets.h"
#include "HKImageCatalog.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // The address lies inside an image satisfying the selector and, when
    // requested, carrying the expected UUID.
    HK_IMAGE_SCOPE_OK = 0,
    // Nothing to check against. NOT a failure -- see the header.
    HK_IMAGE_SCOPE_NO_CATALOG,
    // The catalog holds no image matching the selector: the image the caller
    // named is not loaded.
    HK_IMAGE_SCOPE_NO_MATCH,
    // An image matched the selector, but none matching also carried the
    // expected UUID -- typically a different build of the same path.
    HK_IMAGE_SCOPE_UUID_MISMATCH,
    // The image is there and is the right build, but the address is not
    // inside it. This is the slide-change case.
    HK_IMAGE_SCOPE_ADDRESS_OUTSIDE,
    // A matching image's header could not be parsed far enough to bound it.
    // Distinct from ADDRESS_OUTSIDE because "cannot tell" is not "no".
    HK_IMAGE_SCOPE_UNREADABLE_IMAGE,
} hk_image_scope_status_t;

// Checks `address` against `selector` (+ `expected_uuid` when
// `expect_uuid` is true). `catalog` may be NULL.
//
// A selector matching several images is satisfied if ANY of them accepts the
// address -- the selector names a scope, not a single image, and the same path
// can legitimately be loaded more than once.
//
// Returns the FIRST fatal reason encountered across matching entries, in
// increasing order of how much was established: NO_MATCH (no image at all)
// then UUID_MISMATCH (image but wrong build) then ADDRESS_OUTSIDE (right
// build, wrong address). That ordering means the status always reports the
// furthest the check actually got, which is what a caller needs to act on.
hk_image_scope_status_t hk_image_scope_check(const hk_image_catalog_t *catalog,
                                             const hk_image_selector_t *selector,
                                             bool expect_uuid,
                                             const uint8_t expected_uuid[16],
                                             uintptr_t address);

// Identity, not containment: is the image whose mach header is at `header` one
// that `selector` names?
//
// This is a genuinely different question from hk_image_scope_check, and the
// distinction is worth stating because conflating them is the obvious mistake.
// An engine that operates on a whole image (the rebind engine rewrites the
// IMPORTER's slots) needs to know whether THAT image is in scope. Asking
// "is the header address inside a matching image" looks equivalent and is not
// reliably so -- it assumes the header lies within the image's own segment
// span, which holds for a real Mach-O (__TEXT maps fileoff 0 at the image
// base) but is not something the check can verify, and a synthetic or unusual
// image can violate it. Comparing header pointers asks the question directly.
//
// Same catalog policy: NULL/empty catalog reports NO_CATALOG, a skip.
// Never returns ADDRESS_OUTSIDE or UNREADABLE_IMAGE -- no span is computed.
hk_image_scope_status_t hk_image_scope_check_header(const hk_image_catalog_t *catalog,
                                                    const hk_image_selector_t *selector,
                                                    bool expect_uuid,
                                                    const uint8_t expected_uuid[16],
                                                    const void *header);

// A short, stable description for a status, for engine diagnostics. Static
// storage; never NULL.
const char *hk_image_scope_describe(hk_image_scope_status_t status);

#ifdef __cplusplus
}
#endif

#endif // HK_CORE_IMAGE_SCOPE_H
