// Native terminal inline engine. See HKInlineEngine.h for what "terminal"
// buys and why the relocation-fragility checks deliberately do not apply.

#include "HKInlineEngine.h"

#include <string.h>

hk_inline_status_t hk_inline_prepare(uintptr_t target, uintptr_t replacement,
                                     hk_original_requirement_t original_requirement,
                                     const uint8_t *expected_initial_bytes,
                                     size_t expected_size,
                                     hk_inline_plan_t *out_plan) {
    if (!out_plan || target == 0 || replacement == 0) {
        return HK_INLINE_INVALID_ARGUMENT;
    }
    if (expected_initial_bytes && (expected_size == 0 || expected_size > HK_INLINE_MAX_PATCH)) {
        return HK_INLINE_INVALID_ARGUMENT;
    }
    memset(out_plan, 0, sizeof(*out_plan));

    // Refused up front, before reading anything: this mechanism destroys the
    // prologue, so there is no predecessor to hand back and no continuation to
    // call. Satisfying either would mean allocating a trampoline, which is the
    // relocating engine's job -- a caller who asked for an original gets a
    // refusal, not a different mechanism than the one they asked for.
    if (original_requirement != HK_ORIGINAL_NONE) {
        return HK_INLINE_NEEDS_CONTINUATION;
    }

    // A64 instructions are 4-byte aligned. A misaligned "entry" is not one.
    if ((target & 3u) != 0) {
        return HK_INLINE_MISALIGNED;
    }

    const size_t size = hk_arm64_branch_size(target, replacement);
    if (size == 0 || size > HK_INLINE_MAX_PATCH) {
        return HK_INLINE_INVALID_ARGUMENT;
    }

    // Read what will be replaced. This is the only read of the target, and it
    // is what makes the original known before anything becomes reachable
    // (invariant #5) and what commit revalidates against (invariant #3).
    memcpy(out_plan->original, (const void *)target, size);

    // The caller pinned the prologue: honor it before any other judgment, so a
    // mismatch reads as "not the function you meant" rather than as one of the
    // shape refusals below.
    if (expected_initial_bytes) {
        const size_t n = expected_size < size ? expected_size : size;
        if (memcmp(out_plan->original, expected_initial_bytes, n) != 0) {
            return HK_INLINE_PRECONDITION_FAILED;
        }
    }

    uint32_t first;
    memcpy(&first, out_plan->original, sizeof(first));
    if (hk_arm64_insn_is_trap(first)) {
        return HK_INLINE_TRAP_STUB;
    }

    // Overrun check, and the bound is the whole point. A terminator at the
    // LAST instruction of the window is fine -- the function ends exactly
    // where the patch does, and nothing past it is touched. Only a terminator
    // before that means the function is shorter than the window and the tail
    // of the branch would land in whatever follows. So the scan covers every
    // instruction EXCEPT the last one.
    //
    // A relocating backend cannot use this bound: it has to re-execute the
    // instructions it displaced, and a terminator among them would return or
    // branch from the middle of the relocated copy. Terminal inline re-executes
    // nothing, which is exactly why it can be this precise.
    if (size > 4 && hk_arm64_has_early_terminator(out_plan->original, size - 4)) {
        return HK_INLINE_TARGET_TOO_SHORT;
    }

    // Deliberately NOT checked: hk_arm64_has_aarch64_literal_load. See the
    // header -- it guards RELOCATION, and nothing here is relocated.

    const size_t written = hk_arm64_emit_branch((uint32_t *)out_plan->patch, target, replacement);
    if (written != size) {
        // The sizer and the emitter disagreeing would mean writing a patch of
        // a different length than the window that was preflighted.
        return HK_INLINE_INVALID_ARGUMENT;
    }

    out_plan->address = target;
    out_plan->size = size;
    out_plan->captured = true;
    return HK_INLINE_OK;
}

hk_mutation_state_t hk_inline_commit(const hk_inline_plan_t *plan,
                                     hk_inline_write_fn write, void *write_ctx,
                                     hk_artifact_sink_t *sink) {
    if (!plan || !plan->captured || !write || plan->address == 0 ||
        plan->size == 0 || plan->size > HK_INLINE_MAX_PATCH) {
        return HK_MUTATION_NONE;
    }

    // Invariant #3: the entry must still hold what prepare captured. If it
    // changed since, something else patched it -- refuse rather than overwrite
    // a hook that is already live and report an original that is stale.
    if (memcmp((const void *)plan->address, plan->original, plan->size) != 0) {
        return HK_MUTATION_NONE;
    }

    if (!write(write_ctx, plan->address, plan->patch, plan->size)) {
        return HK_MUTATION_NONE;  // store refused before touching anything
    }

    if (sink) {
        hk_artifact_t a;
        memset(&a, 0, sizeof(a));
        a.struct_size = sizeof(a);
        a.struct_version = HK_ABI_VERSION_3_0;
        a.kind = HK_ARTIFACT_TARGET_TEXT_PATCH;
        a.state = HK_ARTIFACT_COMMITTED;
        a.effects = HK_EFFECT_TARGET_TEXT_MUTATION;
        a.engine_id.data = "inline-terminal";
        a.engine_id.length = 15;
        a.address = plan->address;
        a.size = plan->size;
        a.original_bytes.representation = HK_BYTE_STORAGE_INLINE;
        a.original_bytes.inline_bytes.data = plan->original;
        a.original_bytes.inline_bytes.size = plan->size;
        a.original_bytes.length = plan->size;
        // Reversible in the narrow, honest sense: the original bytes are held
        // and putting them back restores the entry exactly, because nothing
        // was relocated and no trampoline holds a copy. This is the one place
        // terminal inline is SAFER than the relocating kind, which cannot say
        // the same.
        a.mechanically_reversible = true;
        a.safe_to_reverse_after_activation = true;
        (void)hk_artifact_sink_record(sink, &a);
    }
    return HK_MUTATION_COMPLETE;
}
