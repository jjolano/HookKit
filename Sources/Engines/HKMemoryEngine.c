// Memory-patch engine. See HKMemoryEngine.h for the two checks that bracket
// the write and why the store is a device seam.

#include "HKMemoryEngine.h"

#include <string.h>

// (current & mask) == (expected & mask), byte by byte. A NULL mask compares
// every bit. Reads exactly `size` bytes from each side, never more.
static bool masked_equal(const uint8_t *current, const uint8_t *expected,
                         const uint8_t *mask, size_t size) {
    for (size_t i = 0; i < size; i++) {
        uint8_t m = mask ? mask[i] : 0xffu;
        if ((current[i] & m) != (expected[i] & m)) {
            return false;
        }
    }
    return true;
}

hk_mempatch_status_t hk_mempatch_prepare(uintptr_t address, size_t size,
                                         hk_bytes_view_t expected,
                                         hk_bytes_view_t mask,
                                         hk_mempatch_plan_t *out_plan) {
    if (!out_plan || address == 0 || size == 0) {
        return HK_MEMPATCH_INVALID_ARGUMENT;
    }
    if (size > HK_MEMPATCH_MAX) {
        return HK_MEMPATCH_TOO_LARGE;
    }
    // A precondition, if given, must describe exactly the region; a mask, if
    // given, must match the precondition. Anything else is a caller error, not
    // a precondition failure.
    if (expected.data) {
        if (expected.size != size) {
            return HK_MEMPATCH_INVALID_ARGUMENT;
        }
        if (mask.data && mask.size != size) {
            return HK_MEMPATCH_INVALID_ARGUMENT;
        }
    }

    memset(out_plan, 0, sizeof(*out_plan));
    memcpy(out_plan->original, (const void *)address, size);
    out_plan->size = size;
    out_plan->captured = true;

    if (expected.data &&
        !masked_equal(out_plan->original, expected.data,
                      mask.data ? mask.data : NULL, size)) {
        out_plan->captured = false;  // nothing reserved on a precondition failure
        return HK_MEMPATCH_PRECONDITION_FAILED;
    }
    return HK_MEMPATCH_OK;
}

hk_mutation_state_t hk_mempatch_commit(uintptr_t address,
                                       const hk_mempatch_plan_t *plan,
                                       hk_bytes_view_t replacement,
                                       hk_mempatch_write_fn write, void *write_ctx,
                                       hk_artifact_sink_t *sink) {
    if (!plan || !plan->captured || !write || address == 0) {
        return HK_MUTATION_NONE;
    }
    // The replacement must cover exactly the prepared region -- writing fewer
    // or more bytes than were captured (and will be reported as the original)
    // would make the artifact record a lie.
    if (!replacement.data || replacement.size != plan->size) {
        return HK_MUTATION_NONE;
    }

    // Invariant #3: the region must still hold what prepare captured. If it
    // changed since, something else touched it -- refuse rather than overwrite.
    if (memcmp((const void *)address, plan->original, plan->size) != 0) {
        return HK_MUTATION_NONE;
    }

    if (!write(write_ctx, address, replacement.data, plan->size)) {
        return HK_MUTATION_NONE;  // store refused before touching anything
    }

    if (sink) {
        hk_artifact_t a;
        memset(&a, 0, sizeof(a));
        a.struct_size = sizeof(a);
        a.struct_version = HK_ABI_VERSION_3_0;
        a.kind = HK_ARTIFACT_MEMORY_PATCH;
        a.state = HK_ARTIFACT_COMMITTED;
        a.effects = HK_EFFECT_MEMORY_MUTATION;
        a.engine_id.data = "memory";
        a.engine_id.length = 6;
        a.address = address;
        a.size = plan->size;
        // The original bytes are carried inline so a reversal has them; the
        // record is honest that they came from a real read at prepare.
        a.original_bytes.representation = HK_BYTE_STORAGE_INLINE;
        a.original_bytes.inline_bytes.data = plan->original;
        a.original_bytes.inline_bytes.size = plan->size;
        a.original_bytes.length = plan->size;
        // A byte patch is reversible: the original is a plain store away. (A
        // relocating inline hook would not be -- that distinction is why this
        // flag exists per artifact rather than per engine.)
        a.mechanically_reversible = true;
        a.safe_to_reverse_after_activation = true;
        (void)hk_artifact_sink_record(sink, &a);
    }
    return HK_MUTATION_COMPLETE;
}
