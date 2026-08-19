// Host test for Sources/Engines/HKInlineEngine.c -- the terminal inline
// engine, driven directly.
//
// Everything this engine decides is arithmetic over buffers, so it all runs
// here: branch sizing (near vs far), encoding, the overrun bound, the trap-stub
// and alignment refusals, the original-requirement refusal, revalidation, and
// honest mutation state. The only device-only part is the store, which is
// behind a seam a buffer stands in for.
//
// Targets are ordinary heap buffers standing in for mapped code. That is
// enough because the engine never executes them -- it reads instructions,
// decides, and writes bytes.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Engines/HKInlineEngine.h"
#include "../../Sources/Core/HKArtifactLedger.h"

// ---- A64 encodings used as fixtures ------------------------------------
#define A64_NOP   0xD503201Fu
#define A64_RET   0xD65F03C0u
#define A64_BRK0  0xD4200000u   // BRK #0
#define A64_ADRP0 0x90000000u   // ADRP x0, #0 -- relocation-fragile, irrelevant here

static bool buffer_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    (void)ctx;
    memcpy((void *)address, data, size);
    return true;
}
static bool refuse_write(void *ctx, uintptr_t a, const uint8_t *d, size_t s) {
    (void)ctx; (void)a; (void)d; (void)s;
    return false;
}

// A 4-byte-aligned block of instructions standing in for a function.
typedef struct {
    uint32_t *code;
    uintptr_t addr;
    size_t count;
} fn_t;

static fn_t make_fn(const uint32_t *insns, size_t count) {
    fn_t f;
    f.code = aligned_alloc(16, ((count * 4) + 15) & ~(size_t)15);
    assert(f.code);
    memcpy(f.code, insns, count * 4);
    f.addr = (uintptr_t)f.code;
    f.count = count;
    return f;
}
static void free_fn(fn_t *f) { free(f->code); }

// A replacement near enough for a 4-byte B: within +/-128MB of the target.
static uintptr_t near_replacement(uintptr_t target) { return target + 0x1000; }
// Far enough to force the 16-byte absolute form. Chosen well past 128MB.
static uintptr_t far_replacement(uintptr_t target) { return target + (1ull << 40); }

static void test_near_target_uses_a_four_byte_branch(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);
    uintptr_t repl = near_replacement(f.addr);

    hk_inline_plan_t plan;
    assert(hk_inline_prepare(f.addr, repl, HK_ORIGINAL_NONE, NULL, 0, &plan) == HK_INLINE_OK);
    assert(plan.captured);
    assert(plan.size == 4);                       // a plain B reaches
    assert(plan.address == f.addr);
    assert(memcmp(plan.original, body, 4) == 0);  // captured what it will replace
    assert(f.code[0] == A64_NOP);                 // prepare mutated nothing

    hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
    hk_artifact_sink_t sink; memset(&sink, 0, sizeof(sink)); sink.ledger = ledger;

    assert(hk_inline_commit(&plan, buffer_write, NULL, &sink) == HK_MUTATION_COMPLETE);
    // A B was written, and only the first instruction changed.
    assert((f.code[0] & 0xFC000000u) == 0x14000000u);
    assert(f.code[1] == A64_NOP && f.code[4] == A64_RET);
    // ...and it branches where it should: B's imm26 is a signed word offset.
    int32_t imm26 = (int32_t)((f.code[0] & 0x03FFFFFFu) << 6) >> 6;
    assert(f.addr + (intptr_t)imm26 * 4 == repl);

    assert(hk_artifact_ledger_count(ledger) == 1);
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_artifact_snapshot_from_ledger(ledger, &snap) == HK_STATUS_OK);
    hk_artifact_t a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &a) == HK_STATUS_OK);
    assert(a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH);
    assert(a.effects == HK_EFFECT_TARGET_TEXT_MUTATION);
    assert(a.address == f.addr && a.size == 4);
    assert(memcmp(a.original_bytes.inline_bytes.data, body, 4) == 0);
    assert(a.mechanically_reversible);

    hk_artifact_snapshot_release(snap);
    hk_artifact_ledger_destroy(ledger);
    free_fn(&f);
    printf("  near-target-uses-a-four-byte-branch: PASS\n");
}

static void test_far_target_uses_the_absolute_form(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 6);
    uintptr_t repl = far_replacement(f.addr);

    hk_inline_plan_t plan;
    assert(hk_inline_prepare(f.addr, repl, HK_ORIGINAL_NONE, NULL, 0, &plan) == HK_INLINE_OK);
    assert(plan.size == 16);   // out of B range -> LDR/BR/.quad
    assert(memcmp(plan.original, body, 16) == 0);

    assert(hk_inline_commit(&plan, buffer_write, NULL, NULL) == HK_MUTATION_COMPLETE);
    // The absolute form carries its destination as inline data, which is what
    // makes it position-independent -- check the address really is there.
    uint64_t dest;
    memcpy(&dest, &f.code[2], sizeof(dest));
    assert(dest == repl);
    assert(f.code[5] == A64_RET);  // nothing past the 16-byte window moved
    free_fn(&f);
    printf("  far-target-uses-the-absolute-form: PASS\n");
}

// The overrun bound, and both sides of it. This is the check that differs from
// a relocating backend's, so it gets the most scrutiny.
static void test_overrun_bound_is_the_last_instruction(void) {
    uintptr_t probe = 0;

    // Case 1: function is 4 instructions ending in RET, and the window is 16.
    // The terminator sits at the LAST slot -- the patch ends exactly where the
    // function does, so this is allowed.
    {
        const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_RET, A64_NOP};
        fn_t f = make_fn(body, 5);
        probe = f.addr;
        hk_inline_plan_t plan;
        assert(hk_inline_prepare(f.addr, far_replacement(f.addr), HK_ORIGINAL_NONE,
                                 NULL, 0, &plan) == HK_INLINE_OK);
        assert(plan.size == 16);
        free_fn(&f);
    }

    // Case 2: same 16-byte window, but the RET is one instruction earlier. Now
    // the function is 3 instructions and the patch would run 4 bytes past it,
    // into whatever follows. Refused.
    {
        const uint32_t body[] = {A64_NOP, A64_NOP, A64_RET, A64_NOP, A64_NOP};
        fn_t f = make_fn(body, 5);
        hk_inline_plan_t plan;
        assert(hk_inline_prepare(f.addr, far_replacement(f.addr), HK_ORIGINAL_NONE,
                                 NULL, 0, &plan) == HK_INLINE_TARGET_TOO_SHORT);
        assert(!plan.captured);
        assert(f.code[0] == A64_NOP);   // untouched
        free_fn(&f);
    }

    // Case 3: a one-instruction function (just RET) patched with a 4-byte
    // branch. The window is exactly the function, so this is allowed -- a
    // whole-window terminator scan would wrongly refuse it.
    {
        const uint32_t body[] = {A64_RET, A64_NOP};
        fn_t f = make_fn(body, 2);
        hk_inline_plan_t plan;
        assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                                 NULL, 0, &plan) == HK_INLINE_OK);
        assert(plan.size == 4);
        assert(hk_inline_commit(&plan, buffer_write, NULL, NULL) == HK_MUTATION_COMPLETE);
        assert(f.code[1] == A64_NOP);   // the next instruction is untouched
        free_fn(&f);
    }

    (void)probe;
    printf("  overrun-bound-is-the-last-instruction: PASS\n");
}

// The M7-vs-M8 separation: a literal load in the window is fatal to a
// relocating backend and irrelevant to this one, because nothing is relocated.
static void test_literal_load_in_window_is_allowed(void) {
    const uint32_t body[] = {A64_ADRP0, A64_NOP, A64_ADRP0, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 6);

    hk_inline_plan_t plan;
    // Both the near (4-byte) and far (16-byte) windows contain ADRP.
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);
    assert(hk_inline_prepare(f.addr, far_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);
    assert(plan.size == 16);
    assert(hk_inline_commit(&plan, buffer_write, NULL, NULL) == HK_MUTATION_COMPLETE);
    free_fn(&f);
    printf("  literal-load-in-window-is-allowed: PASS\n");
}

static void test_trap_stub_is_refused(void) {
    const uint32_t body[] = {A64_BRK0, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);

    hk_inline_plan_t plan;
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_TRAP_STUB);
    assert(!plan.captured);
    assert(f.code[0] == A64_BRK0);
    free_fn(&f);
    printf("  trap-stub-is-refused: PASS\n");
}

static void test_original_requirement_is_refused(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);
    hk_inline_plan_t plan;

    // This mechanism destroys the prologue, so neither form of original is
    // available. Refused rather than silently upgraded to a trampoline hook.
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr),
                             HK_ORIGINAL_CALLABLE_CONTINUATION, NULL, 0, &plan)
           == HK_INLINE_NEEDS_CONTINUATION);
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr),
                             HK_ORIGINAL_DIRECT_PREDECESSOR, NULL, 0, &plan)
           == HK_INLINE_NEEDS_CONTINUATION);
    assert(!plan.captured);
    assert(f.code[0] == A64_NOP);

    // ...and HK_ORIGINAL_NONE is served, so the refusal is about the
    // requirement and not a blanket rejection.
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);
    free_fn(&f);
    printf("  original-requirement-is-refused: PASS\n");
}

static void test_misaligned_target_is_refused(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);
    hk_inline_plan_t plan;

    for (uintptr_t off = 1; off < 4; off++) {
        assert(hk_inline_prepare(f.addr + off, near_replacement(f.addr),
                                 HK_ORIGINAL_NONE, NULL, 0, &plan) == HK_INLINE_MISALIGNED);
    }
    assert(f.code[0] == A64_NOP);
    free_fn(&f);
    printf("  misaligned-target-is-refused: PASS\n");
}

static void test_expected_bytes_precondition(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);
    hk_inline_plan_t plan;

    // Matching prologue: accepted.
    const uint32_t want_ok = A64_NOP;
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             (const uint8_t *)&want_ok, 4, &plan) == HK_INLINE_OK);

    // Different prologue than the caller pinned: refused, nothing captured.
    const uint32_t want_bad = A64_RET;
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             (const uint8_t *)&want_bad, 4, &plan)
           == HK_INLINE_PRECONDITION_FAILED);
    assert(!plan.captured);
    assert(f.code[0] == A64_NOP);
    free_fn(&f);
    printf("  expected-bytes-precondition: PASS\n");
}

static void test_revalidation_refuses_a_changed_entry(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);
    hk_inline_plan_t plan;
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);

    // Someone else patched the entry between prepare and commit.
    f.code[0] = A64_RET;
    assert(hk_inline_commit(&plan, buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(f.code[0] == A64_RET);   // their patch is left intact
    free_fn(&f);
    printf("  revalidation-refuses-a-changed-entry: PASS\n");
}

static void test_write_refusal_and_argument_validation(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    fn_t f = make_fn(body, 5);
    hk_inline_plan_t plan;
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);

    // The store refuses -> NONE, entry untouched.
    assert(hk_inline_commit(&plan, refuse_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(f.code[0] == A64_NOP);

    // prepare guards.
    assert(hk_inline_prepare(0, near_replacement(f.addr), HK_ORIGINAL_NONE, NULL, 0, &plan)
           == HK_INLINE_INVALID_ARGUMENT);
    assert(hk_inline_prepare(f.addr, 0, HK_ORIGINAL_NONE, NULL, 0, &plan)
           == HK_INLINE_INVALID_ARGUMENT);
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE, NULL, 0, NULL)
           == HK_INLINE_INVALID_ARGUMENT);
    // expected bytes present but a nonsense length.
    const uint32_t want = A64_NOP;
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             (const uint8_t *)&want, 0, &plan) == HK_INLINE_INVALID_ARGUMENT);
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             (const uint8_t *)&want, HK_INLINE_MAX_PATCH + 1, &plan)
           == HK_INLINE_INVALID_ARGUMENT);

    // commit guards.
    assert(hk_inline_prepare(f.addr, near_replacement(f.addr), HK_ORIGINAL_NONE,
                             NULL, 0, &plan) == HK_INLINE_OK);
    assert(hk_inline_commit(NULL, buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(hk_inline_commit(&plan, NULL, NULL, NULL) == HK_MUTATION_NONE);
    hk_inline_plan_t uncaptured; memset(&uncaptured, 0, sizeof(uncaptured));
    assert(hk_inline_commit(&uncaptured, buffer_write, NULL, NULL) == HK_MUTATION_NONE);
    assert(f.code[0] == A64_NOP);
    free_fn(&f);
    printf("  write-refusal-and-argument-validation: PASS\n");
}

int main(void) {
    test_near_target_uses_a_four_byte_branch();
    test_far_target_uses_the_absolute_form();
    test_overrun_bound_is_the_last_instruction();
    test_literal_load_in_window_is_allowed();
    test_trap_stub_is_refused();
    test_original_requirement_is_refused();
    test_misaligned_target_is_refused();
    test_expected_bytes_precondition();
    test_revalidation_refuses_a_changed_entry();
    test_write_refusal_and_argument_validation();
    printf("all inline engine tests passed\n");
    return 0;
}
