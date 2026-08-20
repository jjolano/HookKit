// Host test for Sources/Engines/HKRelocInlineEngine.c -- the relocating inline
// engine.
//
// Both device-only operations are seams a buffer stands in for: the page
// allocator hands back malloc'd memory, and sealing is a no-op that records it
// happened. What that leaves untested is real vm_allocate/vm_protect
// behaviour, which is device-only and is not claimed. What it leaves TESTED is
// every decision: page-relative layout, the thunk-vs-far-branch choice, the
// full-window terminator rule (stricter than the terminal engine's on
// purpose), relocation refusal, the jump-back, revalidation, and the artifact
// pair.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Engines/HKRelocInlineEngine.h"
#include "../../Sources/Core/HKArtifactLedger.h"

#define A64_NOP   0xD503201Fu
#define A64_RET   0xD65F03C0u
#define A64_BRK0  0xD4200000u
#define A64_ADRP0 0x90000000u   // ADRP x0, #0 -- relocatable, and rewritten

// ---- seams --------------------------------------------------------------

typedef struct {
    uint8_t *page;
    size_t page_size;
    bool sealed;
    unsigned alloc_calls;
    unsigned seal_calls;
    bool refuse_alloc;
    bool refuse_seal;
    bool refuse_write;
    unsigned free_calls;
    unsigned write_calls;
} seam_t;

static uintptr_t seam_alloc(void *ctx, size_t size, uintptr_t near) {
    seam_t *s = ctx;
    s->alloc_calls++;
    if (s->refuse_alloc) return 0;
    (void)near;
    s->page = aligned_alloc(16, (size + 15) & ~(size_t)15);
    assert(s->page);
    memset(s->page, 0, size);
    s->page_size = size;
    return (uintptr_t)s->page;
}
static bool seam_seal(void *ctx, uintptr_t page, size_t size) {
    seam_t *s = ctx;
    s->seal_calls++;
    (void)page; (void)size;
    if (s->refuse_seal) return false;
    s->sealed = true;
    return true;
}
static bool seam_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    seam_t *s = ctx;
    s->write_calls++;
    if (s->refuse_write) return false;
    memcpy((void *)address, data, size);
    return true;
}
// The engine's reclaim seam. Distinct from seam_free below, which is the
// test's own teardown for a page the engine kept.
static void seam_free_page(void *ctx, uintptr_t page, size_t size) {
    seam_t *s = ctx; (void)size;
    s->free_calls++;
    free((void *)page);
    if ((uintptr_t)s->page == page) s->page = NULL;
}
static void seam_free(seam_t *s) { free(s->page); s->page = NULL; }

// Follows whichever branch form was emitted at `at`. hk_arm64_emit_branch
// picks a 4-byte B when the destination is within reach and the 16-byte
// LDR/BR/.quad form otherwise, so a test that assumed one form would be
// asserting an accident of where malloc put the page.
static uintptr_t branch_destination(uintptr_t at) {
    uint32_t insn;
    memcpy(&insn, (const void *)at, sizeof(insn));
    if ((insn & 0xFC000000u) == 0x14000000u) {   // B
        int32_t imm26 = (int32_t)((insn & 0x03FFFFFFu) << 6) >> 6;
        return (uintptr_t)((intptr_t)at + (intptr_t)imm26 * 4);
    }
    uint64_t dest;                                // LDR x16,#8 / BR x16 / .quad
    memcpy(&dest, (const uint8_t *)at + 8, sizeof(dest));
    return (uintptr_t)dest;
}

static uint32_t *make_fn(const uint32_t *insns, size_t count) {
    uint32_t *p = aligned_alloc(16, ((count * 4) + 15) & ~(size_t)15);
    assert(p);
    memcpy(p, insns, count * 4);
    return p;
}

// ---- tests --------------------------------------------------------------

static void test_prepare_builds_a_trampoline_and_commit_patches(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;
    uintptr_t replacement = target + 0x1000;

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_plan_t plan;
    assert(hk_reloc_prepare(target, replacement, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_OK);
    assert(plan.captured);
    assert(s.alloc_calls == 1 && s.seal_calls == 1 && s.sealed);
    // Which branch form the entry gets depends on where the allocator put the
    // page, which no test controls -- under ASan it lands far, under plain
    // malloc it lands near. So assert the INVARIANT rather than the accident:
    // the atomic flag means exactly "the patch is one 4-byte store".
    assert(plan.atomic_entry_patch == (plan.patch_size == 4));
    assert(plan.patch_size == 4 || plan.patch_size == 16);
    assert(plan.displaced_count == plan.patch_size / 4);
    // The original a caller invokes is the BODY, not the page front. Invoking
    // the front would hit the thunk and land back on the replacement.
    assert(plan.original_entry == plan.trampoline + HK_RELOC_THUNK_BYTES);

    // Prepare touched nothing in the target.
    assert(fn[0] == A64_NOP && fn[4] == A64_RET);

    // The body holds the displaced instruction followed by a jump back.
    const uint32_t *bodyp = (const uint32_t *)plan.original_entry;
    assert(bodyp[0] == A64_NOP);   // relocated verbatim
    // ...followed by a jump back to the instruction after the patch.
    assert(branch_destination((uintptr_t)&bodyp[1]) == target + 4);

    // The thunk reaches the replacement, in whichever form it needed.
    assert(branch_destination(plan.trampoline) == replacement);

    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink; memset(&sink, 0, sizeof(sink)); sink.ledger = ledger;
    assert(hk_reloc_commit(&plan, seam_write, &s, seam_free_page, &s, &sink) == HK_MUTATION_COMPLETE);

    // Control reaches the replacement -- via the thunk when the page landed
    // near enough for a 4-byte B, directly otherwise. Both are correct; which
    // one happened is an allocator accident, so follow whichever was emitted.
    uintptr_t first_hop = branch_destination(target);
    if (first_hop == plan.trampoline) {
        assert(plan.atomic_entry_patch);   // the thunk exists to buy this
        assert(branch_destination(plan.trampoline) == replacement);
    } else {
        assert(first_hop == replacement);
    }
    assert(fn[4] == A64_RET);   // nothing past the patch window moved

    // Two artifacts, and the trampoline is recorded FIRST because it existed
    // first -- the order the invariants depend on.
    assert(hk_artifact_ledger_count(ledger) == 2);
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    hk_artifact_t t, a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &t) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(snap, 1, &a) == HK_STATUS_OK);
    assert(t.kind == HK_ARTIFACT_TRAMPOLINE);
    assert(t.effects == HK_EFFECT_EXECUTABLE_ALLOCATION);
    assert(t.address == plan.trampoline && t.size == HK_RELOC_PAGE_BYTES);
    // The two carry DIFFERENT reversibility, which is the point of the flag
    // being per artifact: the entry bytes can go back, the page cannot.
    assert(!t.mechanically_reversible);
    assert(a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH);
    assert(a.mechanically_reversible);
    assert(a.original_pointer == (void *)plan.original_entry);
    assert(memcmp(a.original_bytes.inline_bytes.data, body, 4) == 0);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    seam_free(&s);
    free(fn);
    printf("  prepare-builds-a-trampoline-and-commit-patches: PASS\n");
}

// The whole reason the terminal engine exists separately: this one serves a
// callable original, and the displaced instruction really does run from the
// trampoline. ADRP is the interesting case -- it is PC-relative, so running it
// from a new address without rewriting would compute the wrong page.
static void test_pc_relative_prologue_is_rewritten(void) {
    const uint32_t body[] = {A64_ADRP0, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_plan_t plan;
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_OK);

    // The relocated copy is NOT a verbatim ADRP -- it was rewritten, which is
    // exactly what a terminal engine never has to do and never does.
    const uint32_t *bodyp = (const uint32_t *)plan.original_entry;
    assert(bodyp[0] != A64_ADRP0);
    assert(fn[0] == A64_ADRP0);   // and the target is untouched

    seam_free(&s);
    free(fn);
    printf("  pc-relative-prologue-is-rewritten: PASS\n");
}

// Stricter than the terminal engine, on purpose. Terminal inline allows a
// terminator in the LAST slot of the window because it re-executes nothing.
// Here the displaced instructions ARE re-executed, so a terminator anywhere
// among them would leave the body via a RET instead of the jump back.
static void test_terminator_anywhere_is_fatal(void) {
    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_plan_t plan;

    // A one-instruction RET function. The terminal engine ACCEPTS this with a
    // 4-byte patch; this engine must not.
    const uint32_t just_ret[] = {A64_RET, A64_NOP, A64_NOP, A64_NOP};
    uint32_t *fn = make_fn(just_ret, 4);
    assert(hk_reloc_prepare((uintptr_t)fn, (uintptr_t)fn + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan)
           == HK_RELOC_TARGET_TOO_SHORT);
    assert(!plan.captured);
    assert(fn[0] == A64_RET);          // untouched
    assert(s.seal_calls == 0);         // nothing was published

    seam_free(&s);
    free(fn);
    printf("  terminator-anywhere-is-fatal: PASS\n");
}

static void test_trap_stub_alignment_and_precondition(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_plan_t plan;

    const uint32_t trap[] = {A64_BRK0, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *tf = make_fn(trap, 5);
    assert(hk_reloc_prepare((uintptr_t)tf, (uintptr_t)tf + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_TRAP_STUB);
    seam_free(&s);
    free(tf);

    uint32_t *fn = make_fn(body, 5);
    memset(&s, 0, sizeof(s));
    for (uintptr_t off = 1; off < 4; off++) {
        assert(hk_reloc_prepare((uintptr_t)fn + off, (uintptr_t)fn + 0x1000, NULL, 0,
                                seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_MISALIGNED);
    }
    // Misalignment is caught before the page is even requested.
    assert(s.alloc_calls == 0);

    // Pinned prologue that does not match.
    const uint32_t not_there = A64_RET;
    assert(hk_reloc_prepare((uintptr_t)fn, (uintptr_t)fn + 0x1000,
                            (const uint8_t *)&not_there, 4,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan)
           == HK_RELOC_PRECONDITION_FAILED);
    seam_free(&s);
    free(fn);
    printf("  trap-stub-alignment-and-precondition: PASS\n");
}

static void test_seam_refusals(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;
    hk_reloc_plan_t plan;

    // No page: nothing else can proceed, and the target is untouched.
    seam_t s; memset(&s, 0, sizeof(s));
    s.refuse_alloc = true;
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_NO_TRAMPOLINE);
    assert(fn[0] == A64_NOP);

    // Built but never sealed: unusable, so preparation fails rather than
    // handing back a writable "trampoline" a hook would branch into -- and the
    // page is given back, not leaked.
    memset(&s, 0, sizeof(s));
    s.refuse_seal = true;
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_NO_TRAMPOLINE);
    assert(!plan.captured);
    assert(fn[0] == A64_NOP);
    assert(s.free_calls == 1);   // reclaimed on the failure path

    // The entry write refuses. The page is sealed but nothing branches to it,
    // so nothing can be executing in it -- it is RECLAIMED rather than left
    // behind. Still MUTATION_NONE and not PARTIAL: the target was never
    // written, so there is no partial mutation for invariant #4 to forbid a
    // fallback for.
    memset(&s, 0, sizeof(s));
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_OK);
    s.refuse_write = true;
    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink; memset(&sink, 0, sizeof(sink)); sink.ledger = ledger;
    assert(hk_reloc_commit(&plan, seam_write, &s, seam_free_page, &s, &sink) == HK_MUTATION_NONE);
    assert(fn[0] == A64_NOP);
    assert(s.free_calls == 1);               // reclaimed, not leaked
    // ...and NOTHING recorded, because nothing persists. An artifact claiming a
    // trampoline exists when it has been given back would be a false record.
    assert(hk_artifact_ledger_count(ledger) == 0);
    hk_artifact_ledger_destroy(ledger);

    // Without a reclaim seam the page cannot be given back, so it MUST be
    // reported instead -- an unreported leak is worse than the leak.
    memset(&s, 0, sizeof(s));
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_OK);
    s.refuse_write = true;
    ledger = hk_artifact_ledger_create();
    memset(&sink, 0, sizeof(sink)); sink.ledger = ledger;
    assert(hk_reloc_commit(&plan, seam_write, &s, NULL, NULL, &sink) == HK_MUTATION_NONE);
    assert(s.free_calls == 0);
    assert(hk_artifact_ledger_count(ledger) == 1);
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    hk_artifact_t t;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &t) == HK_STATUS_OK);
    assert(t.kind == HK_ARTIFACT_TRAMPOLINE);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    seam_free(&s);
    free(fn);
    printf("  seam-refusals: PASS\n");
}

static void test_revalidation_and_argument_validation(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;
    seam_t s; memset(&s, 0, sizeof(s));
    hk_reloc_plan_t plan;

    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_OK);
    fn[0] = A64_RET;   // someone else patched the entry in between
    assert(hk_reloc_commit(&plan, seam_write, &s, seam_free_page, &s, NULL) == HK_MUTATION_NONE);
    assert(fn[0] == A64_RET);       // their patch survives
    assert(s.write_calls == 0);     // and nothing was attempted
    seam_free(&s);

    memset(&s, 0, sizeof(s));
    assert(hk_reloc_prepare(0, target + 0x1000, NULL, 0, seam_alloc, seam_seal, seam_free_page, &s, &plan)
           == HK_RELOC_INVALID_ARGUMENT);
    assert(hk_reloc_prepare(target, 0, NULL, 0, seam_alloc, seam_seal, seam_free_page, &s, &plan)
           == HK_RELOC_INVALID_ARGUMENT);
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0, NULL, seam_seal, seam_free_page, &s, &plan)
           == HK_RELOC_INVALID_ARGUMENT);
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0, seam_alloc, NULL, seam_free_page, &s, &plan)
           == HK_RELOC_INVALID_ARGUMENT);
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0, seam_alloc, seam_seal, seam_free_page, &s, NULL)
           == HK_RELOC_INVALID_ARGUMENT);
    assert(s.alloc_calls == 0);     // none of those reached the seam

    // Restore the prologue the revalidation check above deliberately broke --
    // a RET first instruction is refused by the terminator rule, so leaving it
    // would make the prepare below fail for an unrelated reason.
    fn[0] = A64_NOP;

    assert(hk_reloc_commit(NULL, seam_write, &s, seam_free_page, &s, NULL) == HK_MUTATION_NONE);
    hk_reloc_plan_t uncaptured; memset(&uncaptured, 0, sizeof(uncaptured));
    assert(hk_reloc_commit(&uncaptured, seam_write, &s, seam_free_page, &s, NULL) == HK_MUTATION_NONE);
    assert(hk_reloc_prepare(target, target + 0x1000, NULL, 0,
                            seam_alloc, seam_seal, seam_free_page, &s, &plan) == HK_RELOC_OK);
    assert(hk_reloc_commit(&plan, NULL, &s, seam_free_page, &s, NULL) == HK_MUTATION_NONE);
    seam_free(&s);

    free(fn);
    printf("  revalidation-and-argument-validation: PASS\n");
}

int main(void) {
    test_prepare_builds_a_trampoline_and_commit_patches();
    test_pc_relative_prologue_is_rewritten();
    test_terminator_anywhere_is_fatal();
    test_trap_stub_alignment_and_precondition();
    test_seam_refusals();
    test_revalidation_and_argument_validation();
    printf("all relocating inline engine tests passed\n");
    return 0;
}
