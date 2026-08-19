// Native relocating inline engine. See HKRelocInlineEngine.h for the phase
// split, the thunk's purpose, and the two device seams.

#include "HKRelocInlineEngine.h"

#include <string.h>

hk_reloc_status_t hk_reloc_prepare(uintptr_t target, uintptr_t replacement,
                                   const uint8_t *expected_initial_bytes,
                                   size_t expected_size,
                                   hk_reloc_alloc_fn alloc, hk_reloc_seal_fn seal,
                                   void *seam_ctx,
                                   hk_reloc_plan_t *out_plan) {
    if (!out_plan || target == 0 || replacement == 0 || !alloc || !seal) {
        return HK_RELOC_INVALID_ARGUMENT;
    }
    if (expected_initial_bytes && (expected_size == 0 || expected_size > HK_RELOC_MAX_PATCH)) {
        return HK_RELOC_INVALID_ARGUMENT;
    }
    memset(out_plan, 0, sizeof(*out_plan));

    if ((target & 3u) != 0) {
        return HK_RELOC_MISALIGNED;
    }

    // The page comes first because the entry patch's SIZE depends on where it
    // landed: a thunk within a B's reach makes the patch 4 bytes, otherwise it
    // has to be the 16-byte form. Nothing about the target is touched here.
    const uintptr_t page = alloc(seam_ctx, HK_RELOC_PAGE_BYTES, target);
    if (page == 0 || (page & 3u) != 0) {
        return HK_RELOC_NO_TRAMPOLINE;
    }
    const uintptr_t thunk_addr = page;
    const uintptr_t body_addr = page + HK_RELOC_THUNK_BYTES;

    // Prefer branching at the thunk: 4 bytes is one aligned store, and
    // therefore atomic against a thread entering the function mid-patch.
    // Falling back to the 16-byte form is honest, not silent -- the plan says
    // which happened.
    size_t patch_size;
    uintptr_t branch_dest;
    if (hk_arm64_branch_size(target, thunk_addr) == 4) {
        patch_size = 4;
        branch_dest = thunk_addr;
        out_plan->atomic_entry_patch = true;
    } else {
        patch_size = hk_arm64_branch_size(target, replacement);
        branch_dest = replacement;
        out_plan->atomic_entry_patch = (patch_size == 4);
    }
    if (patch_size == 0 || patch_size > HK_RELOC_MAX_PATCH) {
        return HK_RELOC_INVALID_ARGUMENT;
    }
    const uint32_t displaced = (uint32_t)(patch_size / 4u);
    if (displaced == 0 || displaced > HK_RELOC_MAX_DISPLACED) {
        return HK_RELOC_INVALID_ARGUMENT;
    }

    // Read what the patch will replace -- the only read of the target, and
    // what commit revalidates against (invariant #3).
    memcpy(out_plan->original, (const void *)target, patch_size);

    if (expected_initial_bytes) {
        const size_t n = expected_size < patch_size ? expected_size : patch_size;
        if (memcmp(out_plan->original, expected_initial_bytes, n) != 0) {
            return HK_RELOC_PRECONDITION_FAILED;
        }
    }

    uint32_t first;
    memcpy(&first, out_plan->original, sizeof(first));
    if (hk_arm64_insn_is_trap(first)) {
        return HK_RELOC_TRAP_STUB;
    }

    // FULL-window terminator scan, and this is exactly where the relocating
    // engine must be stricter than the terminal one. Terminal inline can allow
    // a terminator in the LAST slot because it re-executes nothing and only
    // has to avoid overrunning the function. Here the displaced instructions
    // ARE re-executed from the trampoline, so a RET or unconditional branch
    // among them would return or branch away from the middle of the body,
    // never reaching the jump back.
    if (hk_arm64_has_early_terminator(out_plan->original, patch_size)) {
        return HK_RELOC_TARGET_TOO_SHORT;
    }

    // Build the body: the relocated prologue, then a jump back to the
    // instruction after the patch. hk_arm64_relocate resolves every rewrite to
    // an absolute address, so the body works wherever the page landed.
    uint8_t body[HK_RELOC_BODY_BYTES];
    memset(body, 0, sizeof(body));
    const size_t relocated =
        hk_arm64_relocate((const uint32_t *)out_plan->original, (uint64_t)target,
                          displaced, (uint32_t *)body, sizeof(body));
    if (relocated == 0) {
        // Unrelocatable form, or a branch whose target lands inside the bytes
        // being overwritten -- the relocator refuses both rather than
        // mis-relocating, so this engine does too.
        return HK_RELOC_UNRELOCATABLE;
    }

    const uintptr_t resume = target + patch_size;
    const size_t back = hk_arm64_emit_branch((uint32_t *)(body + relocated),
                                             (uint64_t)(body_addr + relocated),
                                             (uint64_t)resume);
    if (back == 0 || relocated + back > sizeof(body)) {
        return HK_RELOC_UNRELOCATABLE;
    }

    // The thunk: an absolute jump to the replacement, so a 4-byte B at the
    // entry can reach an arbitrarily distant replacement.
    uint8_t thunk[HK_RELOC_THUNK_BYTES];
    memset(thunk, 0, sizeof(thunk));
    const size_t thunk_len = hk_arm64_emit_branch((uint32_t *)thunk,
                                                  (uint64_t)thunk_addr,
                                                  (uint64_t)replacement);
    if (thunk_len == 0 || thunk_len > sizeof(thunk)) {
        return HK_RELOC_UNRELOCATABLE;
    }

    // The page is still writable at this point; publishing it is the seam's
    // job, not a store this engine performs.
    memcpy((void *)thunk_addr, thunk, thunk_len);
    memcpy((void *)body_addr, body, relocated + back);

    if (!seal(seam_ctx, page, HK_RELOC_PAGE_BYTES)) {
        return HK_RELOC_NO_TRAMPOLINE;  // built but never executable: unusable
    }

    // The entry patch itself, encoded now and written at commit.
    const size_t written = hk_arm64_emit_branch((uint32_t *)out_plan->patch,
                                                (uint64_t)target, (uint64_t)branch_dest);
    if (written != patch_size) {
        return HK_RELOC_INVALID_ARGUMENT;  // sizer and emitter disagreed
    }

    out_plan->address = target;
    out_plan->trampoline = page;
    out_plan->trampoline_size = HK_RELOC_PAGE_BYTES;
    // The ORIGINAL is the body, not the page: a caller invoking the page front
    // would hit the thunk and land back on the replacement, which is a loop.
    out_plan->original_entry = body_addr;
    out_plan->patch_size = patch_size;
    out_plan->displaced_count = displaced;
    out_plan->captured = true;
    return HK_RELOC_OK;
}

hk_mutation_state_t hk_reloc_commit(const hk_reloc_plan_t *plan,
                                    hk_reloc_write_fn write, void *write_ctx,
                                    hk_artifact_sink_t *sink) {
    if (!plan || !plan->captured || !write || plan->address == 0 ||
        plan->patch_size == 0 || plan->patch_size > HK_RELOC_MAX_PATCH) {
        return HK_MUTATION_NONE;
    }

    // Invariant #3. If the entry changed since prepare read it, something else
    // patched it -- refuse rather than overwrite a live hook and report an
    // original that is already stale.
    if (memcmp((const void *)plan->address, plan->original, plan->patch_size) != 0) {
        return HK_MUTATION_NONE;
    }

    if (!write(write_ctx, plan->address, plan->patch, plan->patch_size)) {
        // Refused before touching the entry. The trampoline is already sealed
        // and now unreferenced -- a leaked executable page, NOT a mutated
        // target. Reported as NONE (nothing was written) with the page still
        // recorded below, rather than PARTIAL, because PARTIAL would forbid a
        // fallback route under invariant #4 and there is no partial mutation
        // here to forbid one for.
        if (sink && plan->trampoline) {
            hk_artifact_t t;
            memset(&t, 0, sizeof(t));
            t.struct_size = sizeof(t);
            t.struct_version = HK_ABI_VERSION_3_0;
            t.kind = HK_ARTIFACT_TRAMPOLINE;
            t.state = HK_ARTIFACT_COMMITTED;
            t.effects = HK_EFFECT_EXECUTABLE_ALLOCATION;
            t.engine_id.data = "inline-relocating";
            t.engine_id.length = 17;
            t.address = plan->trampoline;
            t.size = plan->trampoline_size;
            t.mechanically_reversible = false;
            (void)hk_artifact_sink_record(sink, &t);
        }
        return HK_MUTATION_NONE;
    }

    if (sink) {
        // The trampoline, recorded first: it existed before the entry patch,
        // and an artifact ledger that implied otherwise would misdescribe the
        // order the invariants depend on.
        hk_artifact_t t;
        memset(&t, 0, sizeof(t));
        t.struct_size = sizeof(t);
        t.struct_version = HK_ABI_VERSION_3_0;
        t.kind = HK_ARTIFACT_TRAMPOLINE;
        t.state = HK_ARTIFACT_COMMITTED;
        t.effects = HK_EFFECT_EXECUTABLE_ALLOCATION;
        t.engine_id.data = "inline-relocating";
        t.engine_id.length = 17;
        t.address = plan->trampoline;
        t.size = plan->trampoline_size;
        // An executable page cannot be un-allocated safely while a thread may
        // still be inside it, so this is not reversible the way a byte patch
        // is -- and saying so is the point of the flag being per artifact.
        t.mechanically_reversible = false;
        (void)hk_artifact_sink_record(sink, &t);

        hk_artifact_t a;
        memset(&a, 0, sizeof(a));
        a.struct_size = sizeof(a);
        a.struct_version = HK_ABI_VERSION_3_0;
        a.kind = HK_ARTIFACT_TARGET_TEXT_PATCH;
        a.state = HK_ARTIFACT_COMMITTED;
        a.effects = HK_EFFECT_TARGET_TEXT_MUTATION;
        a.engine_id.data = "inline-relocating";
        a.engine_id.length = 17;
        a.address = plan->address;
        a.size = plan->patch_size;
        a.original_bytes.representation = HK_BYTE_STORAGE_INLINE;
        a.original_bytes.inline_bytes.data = plan->original;
        a.original_bytes.inline_bytes.size = plan->patch_size;
        a.original_bytes.length = plan->patch_size;
        a.original_pointer = (void *)plan->original_entry;
        a.replacement_pointer = (void *)plan->address;
        // The entry bytes are held and can be put back. That restores dispatch
        // but does NOT reclaim the page -- which is why the two artifacts carry
        // different reversibility rather than one verdict for the install.
        a.mechanically_reversible = true;
        a.safe_to_reverse_after_activation = true;
        (void)hk_artifact_sink_record(sink, &a);
    }
    return HK_MUTATION_COMPLETE;
}
