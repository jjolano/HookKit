// Milestone 9's static continuation: the fixed pool (HKStaticPool.h) and the
// static vtable that runs the relocating engine over it.
//
// The claim under test is narrow and worth stating: a static continuation is
// the SAME mechanism as a dynamic one, differing only in where the executable
// memory came from and therefore in what the engine declares. If these tests
// pass while the reloc-inline suite also passes, that claim holds; if the two
// vtables ever need different behaviour, it has stopped being true.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/engines/HKRelocInlineVtable.h"
#include "../../src/engines/HKStaticPool.h"

#define A64_NOP 0xD503201Fu
#define A64_RET 0xD65F03C0u
#define SLOTS   4u
#define SLOT_BYTES 16384u

// ---- pool-backed seams --------------------------------------------------
//
// On device these unprotect a slot, re-protect it, and release it. Here the
// region is an ordinary buffer, so the protection steps are no-ops that record
// they happened -- the ORDER is what matters and is asserted.

typedef struct {
    hk_static_pool_t pool;
    uint8_t *region;
    unsigned claims;
    unsigned seals;
    unsigned releases;
    bool sealed_before_any_release;
    bool fallback;
    bool refuse_alloc;
    bool refuse_seal;
    bool refuse_write;
    uint8_t *dynamic_page;
} pool_seam_t;

static uintptr_t pool_alloc(void *ctx, size_t size, uintptr_t near,
                             hk_artifact_mapping_t *mapping) {
    pool_seam_t *p = ctx;
    if (p->refuse_alloc) return 0;
    uintptr_t slot = hk_static_pool_claim(&p->pool, size, near);
    if (slot) {
        p->claims++;
        mapping->kind = HK_MAPPING_STATIC_HOOKKIT_SECTION;
        mapping->base = slot;
        mapping->size = p->pool.slot_size;
    } else if (p->fallback) {
        p->dynamic_page = aligned_alloc(SLOT_BYTES, SLOT_BYTES);
        if (!p->dynamic_page) return 0;
        slot = (uintptr_t)p->dynamic_page;
        mapping->kind = HK_MAPPING_ANONYMOUS;
        mapping->base = slot;
        mapping->size = SLOT_BYTES;
    }
    return slot;   // on device: also make the slot writable
}
static bool pool_seal(void *ctx, uintptr_t slot, size_t size) {
    pool_seam_t *p = ctx; (void)slot; (void)size;
    p->seals++;
    return !p->refuse_seal;   // on device: restore the slot to executable
}
static void pool_free(void *ctx, uintptr_t slot, size_t size) {
    pool_seam_t *p = ctx; (void)size;
    p->releases++;
    if (hk_static_pool_contains(&p->pool, slot)) {
        hk_static_pool_release(&p->pool, slot);
    } else {
        assert(slot == (uintptr_t)p->dynamic_page);
        free(p->dynamic_page);
        p->dynamic_page = NULL;
    }
}
static bool pool_write(void *ctx, uintptr_t address, const uint8_t *data, size_t size) {
    pool_seam_t *p = ctx;
    if (p->refuse_write) return false;
    memcpy((void *)address, data, size);
    return true;
}

static void pool_seam_init(pool_seam_t *p) {
    memset(p, 0, sizeof(*p));
    p->region = aligned_alloc(SLOT_BYTES, SLOT_BYTES * SLOTS);
    assert(p->region);
    memset(p->region, 0, SLOT_BYTES * SLOTS);
    assert(hk_static_pool_init(&p->pool, (uintptr_t)p->region,
                               SLOT_BYTES, SLOTS));
}
static void pool_seam_free(pool_seam_t *p) {
    free(p->region); p->region = NULL;
    free(p->dynamic_page); p->dynamic_page = NULL;
}

static hk_reloc_engine_ctx_t static_ctx(pool_seam_t *p) {
    hk_reloc_engine_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.alloc = pool_alloc; c.seal = pool_seal; c.free_page = pool_free;
    c.seam_ctx = p;
    c.write = pool_write; c.write_ctx = p;
    return c;
}

static uint32_t *make_fn(const uint32_t *insns, size_t count) {
    uint32_t *q = aligned_alloc(16, ((count * 4) + 15) & ~(size_t)15);
    assert(q); memcpy(q, insns, count * 4);
    return q;
}

static hk_hook_spec_t address_spec(const char *id, uintptr_t target, void *replacement) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = target;
    spec.replacement = replacement;
    spec.required_reach = HK_REACH_ENTRYPOINT;
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

// ---- pool ---------------------------------------------------------------

static void test_pool_claim_release_and_exhaustion(void) {
    uint8_t region[64 * 4];
    hk_static_pool_t pool;
    assert(hk_static_pool_init(&pool, (uintptr_t)region, 64, 4));
    assert(hk_static_pool_free_count(&pool) == 4);

    uintptr_t a = hk_static_pool_claim(&pool, 64, (uintptr_t)region);
    uintptr_t b = hk_static_pool_claim(&pool, 64, (uintptr_t)region);
    assert(a && b && a != b);
    assert(hk_static_pool_free_count(&pool) == 2);

    // A fixed budget really does run out, and says so rather than growing.
    assert(hk_static_pool_claim(&pool, 64, 0));
    assert(hk_static_pool_claim(&pool, 64, 0));
    assert(hk_static_pool_claim(&pool, 64, 0) == 0);
    assert(hk_static_pool_free_count(&pool) == 0);

    hk_static_pool_release(&pool, a);
    assert(hk_static_pool_free_count(&pool) == 1);

    // A request larger than a slot cannot be served by splitting or spanning.
    // Asserted while a slot IS free -- on an exhausted pool this would return
    // 0 for the wrong reason and prove nothing about the size check.
    assert(hk_static_pool_claim(&pool, 65, 0) == 0);
    assert(hk_static_pool_free_count(&pool) == 1);      // and nothing consumed
    assert(hk_static_pool_claim(&pool, 64, 0) == a);    // the freed slot returns
    printf("  pool-claim-release-and-exhaustion: PASS\n");
}

static void test_pool_rejects_bad_input_and_stray_releases(void) {
    uint8_t region[64 * 2];
    hk_static_pool_t pool;
    assert(!hk_static_pool_init(NULL, (uintptr_t)region, 64, 2));
    assert(!hk_static_pool_init(&pool, 0, 64, 2));
    assert(!hk_static_pool_init(&pool, (uintptr_t)region, 0, 2));
    assert(!hk_static_pool_init(&pool, (uintptr_t)region, 64, 0));
    assert(!hk_static_pool_init(&pool, (uintptr_t)region, 64, HK_STATIC_POOL_MAX_SLOTS + 1));

    assert(hk_static_pool_init(&pool, (uintptr_t)region, 64, 2));
    uintptr_t a = hk_static_pool_claim(&pool, 64, 0);
    assert(a);
    // Releases that are not slot bases must not corrupt the bitmap: below the
    // region, past it, and mid-slot.
    hk_static_pool_release(&pool, (uintptr_t)region - 8);
    hk_static_pool_release(&pool, (uintptr_t)region + 64 * 8);
    hk_static_pool_release(&pool, a + 4);
    // Far enough out that the slot index exceeds the bitmap's width. Without
    // the range check this is a shift by >= 64, which is undefined behaviour
    // rather than a harmless no-op -- UBSan is what catches it, so this
    // assertion alone is not the whole test.
    hk_static_pool_release(&pool, (uintptr_t)region + 64 * 200);
    assert(hk_static_pool_free_count(&pool) == 1);   // unchanged
    hk_static_pool_release(&pool, a);                // the real one still works
    assert(hk_static_pool_free_count(&pool) == 2);
    printf("  pool-rejects-bad-input-and-stray-releases: PASS\n");
}

// ---- the static engine --------------------------------------------------

static void test_backing_metadata(bool static_route, bool exhaust) {
    hk_ownership_reset_for_testing();
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    pool_seam_t p; pool_seam_init(&p);
    p.fallback = !static_route;
    if (exhaust) {
        for (unsigned i = 0; i < SLOTS; i++)
            assert(hk_static_pool_claim(&p.pool, HK_RELOC_PAGE_BYTES, target));
    }
    hk_reloc_engine_ctx_t ectx = static_ctx(&p);
    const hk_engine_vtable_t *vtable = static_route
        ? hk_static_inline_vtable() : hk_reloc_inline_vtable();
    hk_effects_t effect = exhaust ? HK_EFFECT_EXECUTABLE_ALLOCATION : HK_EFFECT_STATIC_CONTINUATION_USE;
    hk_mapping_kind_t kind = exhaust ? HK_MAPPING_ANONYMOUS : HK_MAPPING_STATIC_HOOKKIT_SECTION;

    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, vtable, &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // The request that the DYNAMIC engine is refused for: a callable original
    // with no new executable memory. This is the whole point of the milestone.
    hk_hook_spec_t spec = address_spec("h.static", target, (void *)(target + 0x1000));
    spec.continuation_policy = static_route ? HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY : HK_CONTINUATION_ANY;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == vtable);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hook->result.declared_prepare_effects == vtable->describe().prepare_effects);
    assert(hook->result.observed_prepare_effects == effect);
    assert(hook->result.continuation.mapping_kind == kind);
    assert(hook->result.continuation.mapping_size == SLOT_BYTES);
    hk_continuation_info_t prepared = hook->result.continuation;
    // A slot was taken from the fixed budget -- not a page from the system.
    assert(p.claims == (exhaust ? 0u : 1u) && p.seals == 1);
    assert(hk_static_pool_free_count(&p.pool) == (exhaust ? 0 : SLOTS - 1));

    hk_report_t *report = NULL;
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hook->result.continuation.kind == (exhaust ? HK_CONTINUATION_KIND_DYNAMIC : HK_CONTINUATION_KIND_STATIC));
    assert(hook->result.continuation.executable_memory_allocated == exhaust);
    assert(hook->result.observed_commit_effects == (effect | HK_EFFECT_TARGET_TEXT_MUTATION));
    assert(memcmp(&prepared, &hook->result.continuation, sizeof(prepared)) == 0);
    assert(fn[0] != A64_NOP);

    // The artifacts say the same thing the capabilities did: a text patch, and
    // a trampoline that is NOT reported as an executable allocation.
    hk_artifact_snapshot_t *snap = NULL;
    assert(hk_report_copy_artifacts(report, &snap) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snap) == 2);
    hk_artifact_t t, a;
    assert(hk_artifact_snapshot_copy_at(snap, 0, &t) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(snap, 1, &a) == HK_STATUS_OK);
    assert(t.kind == (exhaust ? HK_ARTIFACT_TRAMPOLINE : HK_ARTIFACT_STATIC_CONTINUATION));
    assert(t.effects == effect && t.size == HK_RELOC_PAGE_BYTES);
    assert(t.mapping.kind == kind && t.mapping.base == t.address && t.mapping.size == SLOT_BYTES);
    assert(t.mapping.base == prepared.mapping_base && t.mapping.size == prepared.mapping_size);
    assert(t.mapping.mapping_id.high == prepared.mapping_id.high && t.mapping.mapping_id.low == prepared.mapping_id.low);
    assert(t.mapping.protection.read && t.mapping.protection.execute && !t.mapping.protection.write);
    assert(a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH && a.replacement_pointer == spec.replacement);

    hk_artifact_snapshot_release(snap);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    pool_seam_free(&p);
    free(fn);
    printf("  backing-metadata static-route=%d exhausted=%d: PASS\n", static_route, exhaust);
}

// The two vtables differ in exactly one observable way, and it is the routing
// consequence. Same target, same request -- only the policy changes.
static void test_static_is_eligible_where_dynamic_is_not(void) {
    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fn = make_fn(body, 5);
    uintptr_t target = (uintptr_t)fn;

    pool_seam_t p; pool_seam_init(&p);
    hk_reloc_engine_ctx_t sctx = static_ctx(&p);
    hk_reloc_engine_ctx_t dctx = static_ctx(&p);   // same seams; only the
                                                   // DECLARATION differs

    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // Dynamic registered FIRST, so it wins on order whenever it is eligible.
    assert(hk_runtime_register_engine_with_context(rt, hk_reloc_inline_vtable(), &dctx));
    assert(hk_runtime_register_engine_with_context(rt, hk_static_inline_vtable(), &sctx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t any = address_spec("h.any", target, (void *)(target + 0x1000));
    hk_hook_spec_t noexec = address_spec("h.noexec", target + 4, (void *)(target + 0x1000));
    noexec.continuation_policy = HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY;

    hk_hook_t *ha = NULL, *hn = NULL;
    assert(hk_plan_add_hook(plan, &any, &ha) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &noexec, &hn) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);

    // Unconstrained: the dynamic one wins on registration order.
    assert(ha->matched_engine == hk_reloc_inline_vtable());
    // Constrained: it is skipped and the static one serves the SAME request
    // that would otherwise have been NO_ROUTE.
    assert(hn->matched_engine == hk_static_inline_vtable());

    hk_plan_release(plan);
    hk_runtime_release(rt);
    pool_seam_free(&p);
    free(fn);
    printf("  static-is-eligible-where-dynamic-is-not: PASS\n");
}

// A fixed budget can run out, and the failure has to be clean: no partial
// install, nothing written, and the slots that were taken stay taken.
static void test_pool_exhaustion_fails_cleanly(void) {
    pool_seam_t p; pool_seam_init(&p);
    hk_reloc_engine_ctx_t ectx = static_ctx(&p);

    const uint32_t body[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
    uint32_t *fns[SLOTS + 1];
    for (unsigned i = 0; i < SLOTS + 1; i++) fns[i] = make_fn(body, 5);

    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_static_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    char ids[SLOTS + 1][16];
    hk_hook_t *hooks[SLOTS + 1];
    for (unsigned i = 0; i < SLOTS + 1; i++) {
        snprintf(ids[i], sizeof(ids[i]), "h.%u", i);
        hk_hook_spec_t spec = address_spec(ids[i], (uintptr_t)fns[i],
                                           (void *)((uintptr_t)fns[i] + 0x1000));
        hooks[i] = NULL;
        assert(hk_plan_add_hook(plan, &spec, &hooks[i]) == HK_STATUS_OK);
    }
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);

    for (unsigned i = 0; i < SLOTS; i++) {
        assert(hooks[i]->result.outcome == HK_OUTCOME_PREPARED);
    }
    // The one past the budget fails, and says the pool is why.
    assert(hooks[SLOTS]->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hooks[SLOTS]->result.error_code == (int64_t)HK_RELOC_NO_TRAMPOLINE);
    assert(fns[SLOTS][0] == A64_NOP);            // nothing written for it
    assert(hk_static_pool_free_count(&p.pool) == 0);
    assert(p.releases == 0);                     // and nobody else's slot freed

    hk_plan_release(plan);
    hk_runtime_release(rt);
    pool_seam_free(&p);
    for (unsigned i = 0; i < SLOTS + 1; i++) free(fns[i]);
    printf("  pool-exhaustion-fails-cleanly: PASS\n");
}

// A refused preparation must give its slot back, or a fixed pool bleeds out
// one slot per failure until it is empty.
static void test_failed_preparation_returns_its_slot(void) {
    // A one-instruction RET: refused by the terminator rule, AFTER the slot is
    // claimed -- which is exactly the path that used to leak a page.
    const uint32_t tiny[] = {A64_RET, A64_NOP, A64_NOP, A64_NOP};
    uint32_t *fn = make_fn(tiny, 4);

    pool_seam_t p; pool_seam_init(&p);
    hk_reloc_engine_ctx_t ectx = static_ctx(&p);
    hk_runtime_t *rt = NULL; hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, hk_static_inline_vtable(), &ectx));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec("h.short", (uintptr_t)fn,
                                       (void *)((uintptr_t)fn + (1ull << 40)));
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);

    assert(p.claims == 1);
    assert(p.releases == 1);
    assert(hk_static_pool_free_count(&p.pool) == SLOTS);   // budget intact

    hk_plan_release(plan);
    hk_runtime_release(rt);
    pool_seam_free(&p);
    free(fn);
    printf("  failed-preparation-returns-its-slot: PASS\n");
}

// The unified free seam uses hk_static_pool_contains to tell a pool slot (which
// gets returned to the bitmap) from a dynamically allocated page (deallocated).
static void test_pool_contains_bounds(void) {
    uint8_t region[64 * 4];
    hk_static_pool_t pool;
    assert(hk_static_pool_init(&pool, (uintptr_t)region, 64, 4));
    assert(hk_static_pool_contains(&pool, (uintptr_t)region));          // first byte
    assert(hk_static_pool_contains(&pool, (uintptr_t)region + 64 * 4 - 1)); // last byte
    assert(!hk_static_pool_contains(&pool, (uintptr_t)region - 1));     // just below
    assert(!hk_static_pool_contains(&pool, (uintptr_t)region + 64 * 4)); // just past end
    assert(!hk_static_pool_contains(NULL, (uintptr_t)region));
    printf("  pool-contains-bounds: PASS\n");
}

static void test_backing_failure_metadata(void) {
    for (unsigned dynamic = 0; dynamic < 2; dynamic++) {
        for (unsigned failure = 0; failure < 5; failure++) {
            uint32_t fn[] = {A64_NOP, A64_NOP, A64_NOP, A64_NOP, A64_RET};
            pool_seam_t p; pool_seam_init(&p);
            p.fallback = dynamic;
            if (dynamic) {
                for (unsigned i = 0; i < SLOTS; i++)
                    assert(hk_static_pool_claim(&p.pool, HK_RELOC_PAGE_BYTES, 0));
            }
            p.refuse_alloc = failure == 0;
            p.refuse_seal = failure == 1;
            p.refuse_write = failure == 2 || failure == 3;
            hk_reloc_plan_t plan;
            hk_reloc_status_t status = hk_reloc_prepare((uintptr_t)fn, (uintptr_t)fn + 0x1000,
                NULL, 0, pool_alloc, pool_seal, pool_free, &p, &plan);
            hk_effects_t effect = dynamic ? HK_EFFECT_EXECUTABLE_ALLOCATION : HK_EFFECT_STATIC_CONTINUATION_USE;
            if (failure < 2) {
                assert(status == HK_RELOC_NO_TRAMPOLINE && !plan.captured);
                assert(plan.mapping.kind == HK_MAPPING_NONE && plan.trampoline == 0);
                hk_continuation_info_t info;
                hk_reloc_describe_continuation(&plan, &info);
                assert(info.kind == HK_CONTINUATION_KIND_NONE && info.mapping_size == 0);
                assert(p.releases == (failure == 1 ? 1u : 0u));
                assert(hk_static_pool_free_count(&p.pool) == (dynamic ? 0 : SLOTS));
                assert(!p.dynamic_page && fn[0] == A64_NOP);
            } else {
                assert(status == HK_RELOC_OK);
                hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
                assert(ledger);
                // An unavailable ledger must not turn a completed write into
                // a clean refusal. The core checks record_failed separately.
                hk_artifact_sink_t sink = {.ledger = failure == 4 ? NULL : ledger};
                assert(hk_reloc_commit(&plan, pool_write, &p,
                    failure == 3 ? NULL : pool_free, &p, &sink) ==
                    (failure == 4 ? HK_MUTATION_COMPLETE : HK_MUTATION_NONE));
                assert(p.releases == (failure == 2 ? 1u : 0u));
                if (failure == 2) {
                    hk_continuation_info_t info;
                    hk_reloc_describe_continuation(&plan, &info);
                    assert(!plan.captured && plan.trampoline == 0);
                    assert(info.kind == HK_CONTINUATION_KIND_NONE && info.address == 0 && info.mapping_size == 0);
                }
                assert(hk_artifact_ledger_count(ledger) == (failure == 3 ? 1u : 0u));
                if (failure == 3) {
                    hk_artifact_snapshot_t *snapshot = NULL;
                    assert(hk_artifact_snapshot_from_ledger(ledger, &snapshot) == HK_STATUS_OK);
                    hk_artifact_t t;
                    assert(hk_artifact_snapshot_copy_at(snapshot, 0, &t) == HK_STATUS_OK);
                    assert(t.effects == effect && t.mapping.size == SLOT_BYTES);
                    assert(t.kind == (dynamic ? HK_ARTIFACT_TRAMPOLINE : HK_ARTIFACT_STATIC_CONTINUATION));
                    assert(t.mapping.kind == plan.mapping.kind && t.mapping.base == plan.mapping.base);
                    assert(t.replacement_pointer == NULL && fn[0] == A64_NOP);
                    hk_artifact_snapshot_release(snapshot);
                }
                if (failure == 4) {
                    assert(sink.record_failed);
                    assert(sink.observed_effects == 0); // no ledger accepted a record
                    assert(fn[0] != A64_NOP);
                }
                hk_artifact_ledger_destroy(ledger);
            }
            pool_seam_free(&p);
        }
    }
    puts("  backing-failure-metadata: PASS");
}

int main(void) {
    test_pool_claim_release_and_exhaustion();
    test_pool_rejects_bad_input_and_stray_releases();
    test_pool_contains_bounds();
    test_backing_metadata(true, false);
    test_backing_metadata(false, false);
    test_backing_metadata(false, true);
    test_backing_failure_metadata();
    test_static_is_eligible_where_dynamic_is_not();
    test_pool_exhaustion_fails_cleanly();
    test_failed_preparation_returns_its_slot();
    printf("all static continuation tests passed\n");
    return 0;
}
