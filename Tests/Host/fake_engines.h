// Shared fake engines for Sources/Core host tests (HKEngineInternal.h's
// contract). Extracted here once test_plan_prepare.c needed the same
// fakes test_engine_registry.c already had, rather than duplicating them
// a second time.

#ifndef HK_TEST_FAKE_ENGINES_H
#define HK_TEST_FAKE_ENGINES_H

#include <stdlib.h>
#include <string.h>

#include "../../Sources/Core/HKEngineInternal.h"

// Handles function-symbol targets needing only existing-imports reach,
// and always prepares successfully. Mirrors the real rebind engine's
// eventual shape (Milestone 6) closely enough to be a meaningful
// stand-in, without pretending to implement anything beyond that.
static inline hk_engine_capabilities_t fake_rebind_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-rebind";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    caps.commit_effects = HK_EFFECT_IMPORT_MUTATION;
    return caps;
}
static inline bool fake_rebind_prepare_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return true;
}
// Mimics the rebind engine's real product: one import-slot rewrite. Fills
// only mechanism facts -- the sink stamps artifact_id/plan_id/request_id/
// runtime_owner_id (which is exactly why those are left zeroed here). The
// engine_id view points at a string literal (static lifetime), matching the
// borrowed-view ownership the ledger currently assumes.
static inline hk_mutation_state_t fake_rebind_commit_one(const hk_hook_spec_t *spec,
                                                         hk_artifact_sink_t *sink) {
    (void)spec;
    hk_artifact_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(a);
    a.struct_version = HK_ABI_VERSION_3_0;
    a.kind = HK_ARTIFACT_IMPORT_SLOT;
    a.state = HK_ARTIFACT_COMMITTED;
    a.effects = HK_EFFECT_IMPORT_MUTATION;
    a.engine_id.data = "fake-rebind";
    a.engine_id.length = strlen("fake-rebind");
    a.import_slot_address = 0xF00D1000;  // a fake but nonzero slot address
    a.mechanically_reversible = true;
    (void)hk_artifact_sink_record(sink, &a);  // fakes cannot OOM; see contract
    return HK_MUTATION_COMPLETE;
}
static inline hk_verify_result_t fake_rebind_verify_one(const hk_hook_spec_t *spec,
                                                        hk_verify_diag_t *out_diag) {
    (void)spec;
    (void)out_diag;
    return HK_VERIFY_OK;
}
static const hk_engine_vtable_t fake_rebind_engine = {
    .describe = fake_rebind_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_rebind_commit_one,
    .verify_one = fake_rebind_verify_one,
};

// Reports a real import mutation whose artifact cannot be copied. SIZE_MAX
// is rejected before the ledger reads the view, so this exercises the plan's
// failed-record path without an allocator-specific test seam.
static inline hk_engine_capabilities_t fake_failed_artifact_record_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-failed-artifact-record";
    return caps;
}
static inline hk_mutation_state_t fake_failed_artifact_record_commit_one(
    const hk_hook_spec_t *spec, hk_artifact_sink_t *sink) {
    (void)spec;
    hk_artifact_t artifact;
    memset(&artifact, 0, sizeof(artifact));
    artifact.struct_size = sizeof(artifact);
    artifact.struct_version = HK_ABI_VERSION_3_0;
    artifact.kind = HK_ARTIFACT_IMPORT_SLOT;
    artifact.state = HK_ARTIFACT_COMMITTED;
    artifact.effects = HK_EFFECT_IMPORT_MUTATION;
    artifact.engine_id.data = "fake-failed-artifact-record";
    artifact.engine_id.length = SIZE_MAX;
    (void)hk_artifact_sink_record(sink, &artifact);
    return HK_MUTATION_COMPLETE;
}
static const hk_engine_vtable_t fake_failed_artifact_record_engine = {
    .describe = fake_failed_artifact_record_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_failed_artifact_record_commit_one,
};

// Grouped preparation probe. It has no one-hook preparation callback on
// purpose: a plan with two adjacent hooks can succeed only if the core calls
// the grouped entry point and carries each operation's result back.
static size_t fake_group_prepare_calls;
static size_t fake_group_prepare_members;
static size_t fake_group_revalidate_calls;
static size_t fake_group_commit_calls;
static size_t fake_group_verify_calls;
static size_t fake_group_compensate_calls;

static inline void fake_group_prepare_reset(void) {
    fake_group_prepare_calls = 0;
    fake_group_prepare_members = 0;
    fake_group_revalidate_calls = 0;
    fake_group_commit_calls = 0;
    fake_group_verify_calls = 0;
    fake_group_compensate_calls = 0;
}

static inline hk_engine_capabilities_t fake_group_rebind_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-group-rebind";
    caps.native_grouping = true;
    return caps;
}

static inline bool fake_group_prepare(void *engine_ctx,
                                      hk_engine_operation_t *operations,
                                      size_t operation_count) {
    (void)engine_ctx;
    fake_group_prepare_calls++;
    fake_group_prepare_members += operation_count;
    for (size_t i = 0; i < operation_count; i++) {
        operations[i].prepare_result = HK_PREPARE_OK;
        operations[i].prepared = NULL;
    }
    return true;
}

static inline bool fake_group_revalidate(void *engine_ctx,
                                         hk_engine_operation_t *operations,
                                         size_t operation_count) {
    (void)engine_ctx;
    fake_group_revalidate_calls++;
    for (size_t i = 0; i < operation_count; i++) {
        operations[i].revalidated = true;
    }
    return true;
}

static inline bool fake_group_commit(void *engine_ctx,
                                     hk_engine_operation_t *operations,
                                     size_t operation_count) {
    (void)engine_ctx;
    fake_group_commit_calls++;
    for (size_t i = 0; i < operation_count; i++) {
        operations[i].mutation = fake_rebind_commit_one(
            operations[i].spec, operations[i].sink);
    }
    return true;
}

static inline bool fake_group_verify(void *engine_ctx,
                                     hk_engine_operation_t *operations,
                                     size_t operation_count) {
    (void)engine_ctx;
    fake_group_verify_calls++;
    for (size_t i = 0; i < operation_count; i++) {
        operations[i].verify_result = fake_rebind_verify_one(
            operations[i].spec, &operations[i].verify_diag);
    }
    return true;
}

static inline hk_engine_capabilities_t fake_group_compensating_describe(void) {
    hk_engine_capabilities_t caps = fake_group_rebind_describe();
    caps.engine_id = "fake-group-compensating";
    caps.supports_compensation = true;
    return caps;
}

static inline bool fake_group_verify_second_fails(
    void *engine_ctx,
    hk_engine_operation_t *operations,
    size_t operation_count) {
    (void)engine_ctx;
    fake_group_verify_calls++;
    for (size_t i = 0; i < operation_count; i++) {
        operations[i].verify_result = i == 0
            ? HK_VERIFY_OK
            : HK_VERIFY_FAILED;
        if (i != 0) {
            operations[i].verify_diag.error_message =
                "fake grouped readback mismatch";
        }
    }
    return true;
}

static inline bool fake_group_compensate(void *engine_ctx,
                                         hk_engine_operation_t *operations,
                                         size_t operation_count) {
    (void)engine_ctx;
    fake_group_compensate_calls++;
    for (size_t i = 0; i < operation_count; i++) {
        if (operations[i].mutation != HK_MUTATION_NONE) {
            operations[i].compensated = true;
        }
    }
    return true;
}

static const hk_engine_vtable_t fake_group_compensating_engine = {
    .describe = fake_group_compensating_describe,
    .prepare_group_ctx = fake_group_prepare,
    .revalidate_group_ctx = fake_group_revalidate,
    .commit_group_ctx = fake_group_commit,
    .verify_group_ctx = fake_group_verify_second_fails,
    .compensate_group_ctx = fake_group_compensate,
};

static const hk_engine_vtable_t fake_group_rebind_engine = {
    .describe = fake_group_rebind_describe,
    .prepare_group_ctx = fake_group_prepare,
    .revalidate_group_ctx = fake_group_revalidate,
    .commit_group_ctx = fake_group_commit,
    .verify_group_ctx = fake_group_verify,
};

static inline hk_verify_result_t fake_verification_fails(const hk_hook_spec_t *spec,
                                                         hk_verify_diag_t *out_diag) {
    (void)spec;
    out_diag->error_message = "fake readback mismatch";
    return HK_VERIFY_FAILED;
}
static const hk_engine_vtable_t fake_verification_failure_engine = {
    .describe = fake_rebind_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_rebind_commit_one,
    .verify_one = fake_verification_fails,
};

// Declares an import mutation but records a different commit effect. The
// core must reject this as an unknown mutation rather than trusting the
// engine's COMPLETE result.
static inline hk_engine_capabilities_t fake_undeclared_effect_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-undeclared-effect";
    caps.commit_effects = HK_EFFECT_IMPORT_MUTATION;
    return caps;
}
static inline hk_mutation_state_t fake_undeclared_effect_commit_one(
    const hk_hook_spec_t *spec, hk_artifact_sink_t *sink) {
    (void)spec;
    hk_artifact_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(a);
    a.struct_version = HK_ABI_VERSION_3_0;
    a.kind = HK_ARTIFACT_MEMORY_PATCH;
    a.state = HK_ARTIFACT_COMMITTED;
    a.effects = HK_EFFECT_MEMORY_MUTATION;
    a.engine_id.data = "fake-undeclared-effect";
    a.engine_id.length = strlen("fake-undeclared-effect");
    a.mechanically_reversible = true;
    (void)hk_artifact_sink_record(sink, &a);
    return HK_MUTATION_COMPLETE;
}
static const hk_engine_vtable_t fake_undeclared_effect_engine = {
    .describe = fake_undeclared_effect_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_undeclared_effect_commit_one,
};

// Commit-order probe. The core still records the ordinary fake import
// artifact; this log only makes dependency/order sequencing visible.
#define FAKE_COMMIT_ORDER_LOG_CAP 32u
static const char *fake_commit_order_log[FAKE_COMMIT_ORDER_LOG_CAP];
static size_t fake_commit_order_log_count;

static inline void fake_commit_order_log_reset(void) {
    fake_commit_order_log_count = 0;
    memset(fake_commit_order_log, 0, sizeof(fake_commit_order_log));
}

static inline hk_mutation_state_t fake_order_commit_one(const hk_hook_spec_t *spec,
                                                        hk_artifact_sink_t *sink) {
    if (fake_commit_order_log_count < FAKE_COMMIT_ORDER_LOG_CAP) {
        fake_commit_order_log[fake_commit_order_log_count++] = spec->stable_hook_id;
    }
    return fake_rebind_commit_one(spec, sink);
}

static const hk_engine_vtable_t fake_order_engine = {
    .describe = fake_rebind_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_order_commit_one,
};

// Same mechanism with a wider reach, used to make preferred-reach ranking
// observable without pulling a second production engine into this router test.
static inline hk_engine_capabilities_t fake_preferred_rebind_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-preferred-rebind";
    caps.achievable_reach |= HK_REACH_FUTURE_IMPORTS;
    return caps;
}
static const hk_engine_vtable_t fake_preferred_rebind_engine = {
    .describe = fake_preferred_rebind_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_rebind_commit_one,
};

static inline hk_engine_capabilities_t fake_early_only_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-early-only";
    caps.install_contexts = HK_INSTALL_CONTEXT_BIT(HK_INSTALL_CONTEXT_EARLY_PROCESS);
    return caps;
}
static const hk_engine_vtable_t fake_early_only_engine = {
    .describe = fake_early_only_describe,
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_rebind_commit_one,
};

// Exact-image routing probe. The descriptor claims the capability only for
// address targets; the core still requires a populated runtime catalog before
// it reports the bit as achieved.
static inline hk_engine_capabilities_t fake_exact_address_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-exact-address";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.exact_image_scope_targets = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.achievable_reach = HK_REACH_ENTRYPOINT | HK_REACH_EXACT_IMAGE_SCOPE;
    return caps;
}
static const hk_engine_vtable_t fake_exact_address_engine = {
    .describe = fake_exact_address_describe,
    .prepare_one = fake_rebind_prepare_one,
};

static inline hk_engine_capabilities_t fake_exact_symbol_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-exact-symbol";
    caps.exact_image_scope_targets =
        HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach |= HK_REACH_EXACT_IMAGE_SCOPE;
    return caps;
}
static const hk_engine_vtable_t fake_exact_symbol_engine = {
    .describe = fake_exact_symbol_describe,
    .prepare_one = fake_rebind_prepare_one,
};

// Like fake_rebind, but also publishes an original pointer -- a real rebind
// always preserves the prior import value the replacement can call through.
// Separate from fake_rebind so tests that don't care about installed records
// don't accumulate them. The published pointer is a fixed nonzero sentinel
// (a stand-in for a real code address, same spirit as the fake slot address
// above); FAKE_ORIGINAL below is what a test asserts came back out of the slot.
#define FAKE_ORIGINAL_PTR ((void *)0xC0DE4444)
static inline hk_mutation_state_t fake_rebind_original_commit_one(const hk_hook_spec_t *spec,
                                                                  hk_artifact_sink_t *sink) {
    (void)spec;
    hk_artifact_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(a);
    a.struct_version = HK_ABI_VERSION_3_0;
    a.kind = HK_ARTIFACT_IMPORT_SLOT;
    a.state = HK_ARTIFACT_COMMITTED;
    a.effects = HK_EFFECT_IMPORT_MUTATION;
    a.engine_id.data = "fake-rebind-original";
    a.engine_id.length = strlen("fake-rebind-original");
    a.import_slot_address = 0xF00D2000;
    a.original_pointer = FAKE_ORIGINAL_PTR;  // inspectable record of the preserved original
    a.mechanically_reversible = true;
    (void)hk_artifact_sink_record(sink, &a);
    sink->published_original = FAKE_ORIGINAL_PTR;  // the live pointer the slot will hold
    return HK_MUTATION_COMPLETE;
}
static const hk_engine_vtable_t fake_rebind_original_engine = {
    .describe = fake_rebind_describe,  // same eligibility as fake_rebind
    .prepare_one = fake_rebind_prepare_one,
    .commit_one = fake_rebind_original_commit_one,
};

// A tiny stateful chain engine for ownership tests. Its global head stands
// in for one real import slot: prepare captures it, commit requires that
// captured predecessor to match both the core's ownership witness and the
// current slot, then publishes the predecessor before advancing the head.
#define FAKE_CHAIN_BASE ((void *)0xC0DE5000)
static void *fake_chain_head = FAKE_CHAIN_BASE;
static size_t fake_chain_commit_calls;

typedef struct {
    void *original;
} fake_chain_prepared_t;

static inline void fake_chain_reset(void) {
    fake_chain_head = FAKE_CHAIN_BASE;
    fake_chain_commit_calls = 0;
}

static inline hk_engine_capabilities_t fake_chain_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "fake-chain";
    caps.chainable_target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    return caps;
}

static inline hk_prepare_result_t fake_chain_prepare(
    void *engine_ctx,
    const hk_hook_spec_t *spec,
    void **out_prepared,
    hk_prepare_diag_t *out_diag) {
    (void)engine_ctx;
    (void)spec;
    (void)out_diag;
    fake_chain_prepared_t *prepared = calloc(1, sizeof(*prepared));
    if (!prepared) {
        return HK_PREPARE_FAILED;
    }
    prepared->original = fake_chain_head;
    *out_prepared = prepared;
    return HK_PREPARE_OK;
}

static inline hk_mutation_state_t fake_chain_commit(
    void *engine_ctx,
    const hk_hook_spec_t *spec,
    void *prepared_value,
    hk_artifact_sink_t *sink) {
    (void)engine_ctx;
    fake_chain_prepared_t *prepared = prepared_value;
    fake_chain_commit_calls++;
    if (!spec || !prepared || prepared->original != fake_chain_head ||
        (sink && sink->require_predecessor_match &&
         prepared->original != sink->required_predecessor)) {
        return HK_MUTATION_NONE;
    }
    if (sink) {
        hk_artifact_t artifact;
        memset(&artifact, 0, sizeof(artifact));
        artifact.struct_size = sizeof(artifact);
        artifact.struct_version = HK_ABI_VERSION_3_0;
        artifact.kind = HK_ARTIFACT_IMPORT_SLOT;
        artifact.state = HK_ARTIFACT_COMMITTED;
        artifact.effects = HK_EFFECT_IMPORT_MUTATION;
        artifact.engine_id.data = "fake-chain";
        artifact.engine_id.length = strlen("fake-chain");
        artifact.import_slot_address = 0xF00D5000;
        artifact.original_pointer = prepared->original;
        artifact.replacement_pointer = spec->replacement;
        artifact.mechanically_reversible = true;
        (void)hk_artifact_sink_record(sink, &artifact);
        sink->published_original = prepared->original;
    }
    fake_chain_head = spec->replacement;
    return HK_MUTATION_COMPLETE;
}

static inline hk_verify_result_t fake_chain_verify(
    void *engine_ctx,
    const hk_hook_spec_t *spec,
    void *prepared_value,
    hk_verify_diag_t *out_diag) {
    (void)engine_ctx;
    (void)prepared_value;
    if (!spec || fake_chain_head != spec->replacement) {
        out_diag->error_message = "fake chain head does not match replacement";
        return HK_VERIFY_FAILED;
    }
    return HK_VERIFY_OK;
}

static inline void fake_chain_release(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t fake_chain_engine = {
    .describe = fake_chain_describe,
    .prepare_one_ctx_status = fake_chain_prepare,
    .commit_one_ctx = fake_chain_commit,
    .verify_one_ctx = fake_chain_verify,
    .release_prepared = fake_chain_release,
};

// Same eligibility shape as fake_rebind_engine, but always fails to
// prepare -- for testing hk_plan_prepare's failure path without needing
// mutable global test state (a second always-failing engine is simpler
// and less order-dependent than a toggle flag on a shared one).
static inline hk_engine_capabilities_t fake_always_fails_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-always-fails";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline bool fake_always_fails_prepare_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return false;
}
static const hk_engine_vtable_t fake_always_fails_engine = {
    .describe = fake_always_fails_describe,
    .prepare_one = fake_always_fails_prepare_one,
};

// Handles objc-method targets. No prepare_one (NULL) -- deliberately, to
// exercise hk_plan_prepare's handling of an engine that was eligible for
// describe() purposes but never implemented preparation (a real
// inconsistency the router should catch, not silently skip).
static inline hk_engine_capabilities_t fake_objc_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-objc";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    caps.achievable_reach = HK_REACH_OBJC_DISPATCH;
    return caps;
}
static const hk_engine_vtable_t fake_objc_engine = {
    .describe = fake_objc_describe,
    .prepare_one = NULL,
    .commit_one = NULL,
};

// The four fakes below all handle function-symbol/existing-imports (same
// shape as fake_rebind_engine) and always prepare successfully -- they
// exist purely to exercise hk_plan_commit's mutation-state-to-outcome
// mapping, one real code path per fake, rather than trusting the switch
// statement's cases from reading it. Mutation-state semantics are one of
// the spec's core invariants (section 4.4/6.27), which is why this gets
// more fakes than prepare's testing needed.
static inline bool fake_commit_helper_prepare_one(const hk_hook_spec_t *spec) {
    (void)spec;
    return true;
}

static inline hk_engine_capabilities_t fake_commit_none_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-commit-none";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
// These three record no artifact: they exist to exercise commit's
// mutation-state-to-outcome mapping, not the ledger. NONE genuinely made
// nothing to record; PARTIAL/UNKNOWN could in principle record something,
// but modeling a partial/unknown artifact is not what these fakes are for
// (the artifact pipe is proven end-to-end by fake_rebind above). Left as a
// stated scope line, not an oversight.
static inline hk_mutation_state_t fake_commit_none_commit_one(const hk_hook_spec_t *spec,
                                                              hk_artifact_sink_t *sink) {
    (void)spec;
    (void)sink;
    return HK_MUTATION_NONE;  // refused cleanly, nothing touched
}
static const hk_engine_vtable_t fake_commit_none_engine = {
    .describe = fake_commit_none_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = fake_commit_none_commit_one,
};

static inline hk_engine_capabilities_t fake_commit_partial_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-commit-partial";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline hk_mutation_state_t fake_commit_partial_commit_one(const hk_hook_spec_t *spec,
                                                                 hk_artifact_sink_t *sink) {
    (void)spec;
    (void)sink;
    return HK_MUTATION_PARTIAL;
}
static const hk_engine_vtable_t fake_commit_partial_engine = {
    .describe = fake_commit_partial_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = fake_commit_partial_commit_one,
};

static inline hk_engine_capabilities_t fake_commit_unknown_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-commit-unknown";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
static inline hk_mutation_state_t fake_commit_unknown_commit_one(const hk_hook_spec_t *spec,
                                                                 hk_artifact_sink_t *sink) {
    (void)spec;
    (void)sink;
    return HK_MUTATION_UNKNOWN;
}
static const hk_engine_vtable_t fake_commit_unknown_engine = {
    .describe = fake_commit_unknown_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = fake_commit_unknown_commit_one,
};

// prepares successfully but has no commit_one at all -- the commit-time
// analogue of fake_objc_engine's missing prepare_one.
static inline hk_engine_capabilities_t fake_no_commit_one_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fake-no-commit-one";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    return caps;
}
// Two engines that describe themselves IDENTICALLY except for which originals
// they serve -- the exact shape of the terminal/relocating inline pair. They
// exist to test that the router picks on that axis, without pulling either
// real engine (and its device seams) into a router test.
static inline hk_engine_capabilities_t fake_original_none_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "orig-none-only";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.achievable_reach = HK_REACH_ENTRYPOINT;
    caps.original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE);
    return caps;
}
static const hk_engine_vtable_t fake_original_none_engine = {
    .describe = fake_original_none_describe,
    .prepare_one = fake_commit_helper_prepare_one,
};

static inline hk_engine_capabilities_t fake_original_any_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "orig-any";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    caps.achievable_reach = HK_REACH_ENTRYPOINT;
    caps.original_requirements = HK_ORIGINAL_REQ_ALL;
    return caps;
}
static const hk_engine_vtable_t fake_original_any_engine = {
    .describe = fake_original_any_describe,
    .prepare_one = fake_commit_helper_prepare_one,
};

static const hk_engine_vtable_t fake_no_commit_one_engine = {
    .describe = fake_no_commit_one_describe,
    .prepare_one = fake_commit_helper_prepare_one,
    .commit_one = NULL,
};

#endif // HK_TEST_FAKE_ENGINES_H
