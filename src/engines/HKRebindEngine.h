// Rebind engine -- Milestone 6. The first engine that turns resolution into
// an actual hook: it rewrites an image's import slots so calls to an imported
// symbol land on a replacement instead.
//
// Reach: HK_REACH_EXISTING_IMPORTS. It redirects call sites that go through
// an import slot, which is every cross-image call, and nothing else -- calls
// inside the defining image do not touch a slot and are unaffected. That is a
// property of the mechanism, not a limitation to be worked around here.
//
// TWO PHASES, and the split is required rather than stylistic:
//   prepare  enumerates the slots and reads their current values. It MUTATES
//            NOTHING (ARCHITECTURE.md invariant #2), and it is what makes the
//            original known before any replacement becomes reachable
//            (invariant #5) -- capturing originals during the write loop
//            would publish a replacement before its predecessor was known.
//   commit   revalidates each slot against what prepare saw (invariant #3)
//            and writes.
//
// Import metadata is authoritative and mutually exclusive: ordinary chained
// images are walked from their original file words, shared-cache images use
// the live cache patch table, and only legacy images use LC_DYSYMTAB.
//
// The write is behind a seam. PAC selection and signing are host-tested with
// tagged pointers; a physical arm64e run remains the hardware certification
// gate for CPU authentication and real cache/protection behavior.
//
#ifndef HK_ENGINES_REBIND_H
#define HK_ENGINES_REBIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/HKArtifactLedger.h"
#include "../internal/HKPointerAuth.h"
#include "../resolvers/HKImportSlots.h"
#include "../resolvers/HKSymbolTable.h"

#ifdef __cplusplus
extern "C" {
#endif

// Slots for one symbol in one image. Real images have a handful; the cap is
// fixed so the engine allocates nothing, and exceeding it is reported rather
// than silently truncated -- a partial rewrite that looked complete would be
// exactly the kind of dishonest result the mutation-state contract forbids.
#define HK_REBIND_MAX_SITES 32u

typedef enum {
    HK_REBIND_OK = 0,
    HK_REBIND_NOT_FOUND,          // the image imports no such symbol
    HK_REBIND_INVALID_ARGUMENT,
    HK_REBIND_MALFORMED_IMAGE,
    HK_REBIND_TOO_MANY_SITES,
    HK_REBIND_METADATA_UNAVAILABLE,
    HK_REBIND_PAC_MISMATCH,
    HK_REBIND_UNSUPPORTED_FORMAT,
    HK_REBIND_SCOPE_UNREPRESENTABLE,
} hk_rebind_status_t;

// The one device-only operation. Returns false if the store could not be
// performed (protection change refused, address not writable, ...).
typedef bool (*hk_rebind_write_fn)(void *ctx, uintptr_t address, uint64_t value);

typedef struct {
    const void *image_base;   // the mach header, as mapped
    size_t image_size;        // how much is readable from it
    uintptr_t slide;          // dyld slide; 0 for an unslid buffer
    const char *image_path;   // original file, for authoritative chain words
    const void *file_image;   // optional host/test seam; preferred over path
    size_t file_image_size;
    const void *cache_base;   // optional host/test seam; live cache otherwise
    size_t cache_size;
    bool uuid_present;
    uint8_t uuid[16];
    bool include_shared_cache_got;
    hk_rebind_write_fn write; // required for commit, unused by prepare
    void *write_ctx;
} hk_rebind_target_t;

typedef struct {
    uintptr_t address;   // runtime address of the slot
    uint64_t original;   // value read during prepare, before any write
    uint64_t callable_original; // standard IA/discriminator-zero base target
    int64_t addend;
    hk_pac_schema_t schema;
    bool weak_import;
    bool from_chained;   // found via chained fixups rather than LC_DYSYMTAB
    bool from_cache;
} hk_rebind_site_t;

typedef struct {
    hk_rebind_site_t sites[HK_REBIND_MAX_SITES];
    uint32_t count;
    // Every site for one symbol normally holds the same original. If they do
    // not, the image is doing something unusual and the caller is told rather
    // than handed an arbitrary one of them.
    bool originals_agree;
    uint64_t original;   // meaningful when count > 0 && originals_agree
} hk_rebind_plan_t;

// Read-only slot witness shared by commit and post-commit verification.
uint64_t hk_rebind_read_slot(uintptr_t address);
bool hk_rebind_replacement_for_site(const hk_rebind_site_t *site,
                                    uint64_t replacement,
                                    uint64_t *out_value);

// Phase 1. Enumerates slots and reads their current values. Mutates nothing.
hk_rebind_status_t hk_rebind_prepare(const hk_rebind_target_t *target,
                                     const char *symbol_name,
                                     hk_symbol_name_convention_t convention,
                                     hk_rebind_plan_t *out_plan);

// Phase 2. Revalidates each slot against what prepare recorded, then writes.
//
// Returns the honest mutation state (spec section 4.4):
//   NONE      nothing was written -- a clean refusal, another route may be tried
//   COMPLETE  every site was written
//   PARTIAL   some sites were written and then one failed. NO fallback may be
//             attempted after this; the image is in a mixed state and the
//             caller is told so rather than left to assume.
// `sink` may be NULL; when present, one artifact is recorded per written site.
hk_mutation_state_t hk_rebind_commit(const hk_rebind_target_t *target,
                                     const hk_rebind_plan_t *plan,
                                     uint64_t replacement,
                                     hk_artifact_sink_t *sink,
                                     uint32_t *out_written);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_REBIND_H
