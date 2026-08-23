// HookKit 3.0 -- the generic artifact manifest (spec section 7).
// docs/3.0/ARCHITECTURE.md invariant #6: HookKit exposes complete generic
// artifacts through immutable snapshots and never redacts, hides, or
// filters them by caller identity.
//
// Field-for-field transcription of Schemas/hookkit-artifact.schema.json,
// which was itself designed from the spec's prose (section 7.3) back in
// Milestone 1 -- writing the header from the schema, not from the prose a
// second time, is what keeps the two from drifting apart. Real, stated
// gap left over from Milestone 3 until now: this header didn't exist, so
// no artifact ledger could produce anything of this shape -- closed here
// before Milestone 4's ledger work continues on top of it.

#ifndef HOOKKIT_ARTIFACTS_H
#define HOOKKIT_ARTIFACTS_H

#include <stdbool.h>
#include <stdint.h>

#include "HookKitBase.h"
#include "HookKitResults.h"  // hk_effects_t, hk_mapping_kind_t
#include "HookKitRuntime.h"  // hk_runtime_t, hk_report_t (opaque, for the snapshot functions below)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HK_ARTIFACT_IMPORT_SLOT = 0,
    HK_ARTIFACT_TARGET_TEXT_PATCH,
    HK_ARTIFACT_MEMORY_PATCH,

    HK_ARTIFACT_TRAMPOLINE,
    HK_ARTIFACT_STATIC_CONTINUATION,
    HK_ARTIFACT_DYNAMIC_EXECUTABLE_ALLOCATION,
    HK_ARTIFACT_BRANCH_ISLAND,

    HK_ARTIFACT_OBJC_METHOD_CHANGE,
    HK_ARTIFACT_SWIFT_VTABLE_CHANGE,
    HK_ARTIFACT_ORIGINAL_POINTER,

    HK_ARTIFACT_LOADED_PROVIDER_IMAGE,
    HK_ARTIFACT_PROVIDER_ACTIVATION,
    HK_ARTIFACT_IMAGE_CALLBACK,
    HK_ARTIFACT_LATE_IMAGE_APPLICATION,

    HK_ARTIFACT_MEMORY_PROTECTION_TRANSITION,
    HK_ARTIFACT_FILE_MAPPING,
    HK_ARTIFACT_THREAD_CREATION,
    HK_ARTIFACT_UNKNOWN_PROCESS_MUTATION,
} hk_artifact_kind_t;

typedef enum {
    HK_ARTIFACT_PLANNED = 0,
    HK_ARTIFACT_PREPARED,
    HK_ARTIFACT_PENDING,
    HK_ARTIFACT_COMMITTED,
    HK_ARTIFACT_VERIFIED,
    HK_ARTIFACT_LATE_APPLIED,
    HK_ARTIFACT_PARTIALLY_APPLIED,
    HK_ARTIFACT_COMPENSATED,
    HK_ARTIFACT_ROLLED_BACK,
    HK_ARTIFACT_INVALIDATED,
    HK_ARTIFACT_DISCARDED,
    HK_ARTIFACT_OBSERVED_EXISTING,
} hk_artifact_state_t;

// spec section 7.4: never invent a custom checksum. SHA-256 only, hence a
// fixed 32-byte digest rather than a variable-length hash view.
typedef enum {
    HK_BYTE_STORAGE_NONE = 0,
    HK_BYTE_STORAGE_INLINE,
    HK_BYTE_STORAGE_HASH,
    HK_BYTE_STORAGE_INLINE_AND_HASH,
} hk_byte_storage_representation_t;

typedef struct {
    HK_STRUCT_HEADER;

    hk_byte_storage_representation_t representation;
    hk_bytes_view_t inline_bytes;          // meaningful when representation is INLINE or INLINE_AND_HASH
    uint8_t sha256[32];                    // meaningful when representation is HASH or INLINE_AND_HASH
    size_t length;                         // the real region length, even when only a hash is carried
} hk_byte_storage_t;

typedef struct {
    bool read;
    bool write;
    bool execute;
} hk_vm_protection_t;

// Real, stated gap: this is a minimal identity (path + UUID + slide), not
// a full resolved image-catalog entry -- Milestone 5's image catalog owns
// that richer concept. An artifact only needs enough to identify WHICH
// image, not everything the catalog will eventually know about it.
typedef struct {
    HK_STRUCT_HEADER;

    const char *path;
    bool uuid_present;
    uint8_t uuid[16];
    uintptr_t slide;
} hk_image_identity_t;

typedef struct {
    HK_STRUCT_HEADER;

    hk_id_t mapping_id;
    hk_mapping_kind_t kind;
    uintptr_t base;
    size_t size;
    hk_vm_protection_t protection;
} hk_artifact_mapping_t;

// One immutable artifact record. Every optional/not-applicable field for
// a given artifact_kind stays zeroed rather than populated with a
// plausible-looking default -- e.g. a HK_ARTIFACT_THREAD_CREATION record
// has no meaningful `address`, and it must read as absent (0), not as an
// unexplained stray value.
typedef struct {
    HK_STRUCT_HEADER;

    hk_id_t artifact_id;
    hk_id_t runtime_owner_id;
    hk_id_t plan_id;
    hk_id_t request_id;
    hk_id_t installed_id;

    hk_artifact_kind_t kind;
    hk_artifact_state_t state;
    hk_effects_t effects;                  // bitwise-OR, at least one bit set (spec: "effects" is required, minItems 1)

    hk_string_view_t engine_id;
    hk_string_view_t mechanism_id;

    hk_image_identity_t image;
    uint64_t image_generation;
    uint64_t installed_generation;

    uintptr_t address;
    size_t size;

    hk_byte_storage_t original_bytes;
    hk_byte_storage_t expected_bytes;
    hk_byte_storage_t expected_mask;
    hk_byte_storage_t current_bytes;

    void *original_pointer;
    void *replacement_pointer;
    uintptr_t import_slot_address;

    uintptr_t continuation_address;
    uintptr_t jump_back_destination;

    hk_vm_protection_t original_protection;
    hk_vm_protection_t current_protection;

    hk_artifact_mapping_t mapping;

    bool mechanically_reversible;
    bool safe_to_reverse_after_activation;

    // No "unverified"/"verified"/"verification_failed" enum here despite
    // the schema modeling it as one -- hk_hook_result_t already has a
    // plain `verified` bool for the same concept at the per-hook level
    // (HookKitResults.h), and introducing a 3-state artifact-level enum
    // that a 2-state hook-level bool can't fully represent would be a new
    // inconsistency, not a refinement. Revisit together if a real need
    // for the FAILED distinction (vs. just unverified) shows up.
    bool verified;

    bool fully_inspected;
} hk_artifact_t;

typedef struct hk_artifact_snapshot hk_artifact_snapshot_t;

// Deep-copied, immutable snapshots (spec section 7.5) -- never shared
// mutable storage with the report/runtime/process state they were copied
// from. Report, runtime, and process snapshots are populated by committed
// artifacts; analyze/prepare snapshots are empty.

hk_status_t hk_report_copy_artifacts(
    const hk_report_t *report,
    hk_artifact_snapshot_t **out_snapshot);

hk_status_t hk_runtime_copy_artifacts(
    const hk_runtime_t *runtime,
    hk_artifact_snapshot_t **out_snapshot);

hk_status_t hk_copy_process_artifacts(
    hk_artifact_snapshot_t **out_snapshot);

size_t hk_artifact_snapshot_count(const hk_artifact_snapshot_t *snapshot);

hk_status_t hk_artifact_snapshot_copy_at(
    const hk_artifact_snapshot_t *snapshot,
    size_t index,
    hk_artifact_t *out_artifact);

void hk_artifact_snapshot_release(hk_artifact_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_ARTIFACTS_H
