// Fault-injection (OOM) sweep over the plan lifecycle (Milestone 4).
//
// Every core allocation (malloc/calloc/realloc) is routed through a linker
// --wrap interceptor that can fail exactly the Nth allocation. The sweep runs
// a full lifecycle (runtime + plan + 2 hooks + analyze + prepare + commit)
// once per N = 1, 2, 3, ..., failing a different allocation each time, until an
// N runs with no failure fired (N exceeded the total allocation count) --
// which means every single allocation site has been the failure point exactly
// once. Built under -fsanitize=address so a leaked partial allocation on any
// OOM path is caught.
//
// The load-bearing invariant beyond "doesn't crash/leak": if an operation
// returns HK_STATUS_OUT_OF_MEMORY, the plan's state must be UNCHANGED. That
// catches the classic bug of advancing the state machine and *then* failing a
// late allocation, which would leave a "failed" operation having silently
// moved the plan forward.

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "fake_engines.h"

// ---- allocation interceptor (linked with -Wl,--wrap=malloc,calloc,realloc) --
// GNU ld only -- Apple's linker has no --wrap equivalent. On any other host
// this degrades to running the lifecycle once, uninstrumented (see main()).

static int g_fi_fired;    // set when an injection actually fired

#if defined(__linux__)
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);

static int g_fi_target;   // fail this (1-based) allocation while armed; 0 = disabled
static int g_fi_count;    // allocations seen since arm
static size_t g_fi_bytes; // requested bytes, not allocator usable size or RSS

static void fi_arm(int nth) { g_fi_target = nth; g_fi_count = 0; g_fi_fired = 0; g_fi_bytes = 0; }
static void fi_disarm(void) { g_fi_target = 0; }

static int fi_should_fail(void) {
    if (g_fi_target && ++g_fi_count == g_fi_target) {
        g_fi_fired = 1;
        return 1;
    }
    return 0;
}

void *__wrap_malloc(size_t n) {
    if (g_fi_target) g_fi_bytes += n;
    if (fi_should_fail()) return NULL;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b) {
    if (g_fi_target) g_fi_bytes += a * b;
    if (fi_should_fail()) return NULL;
    return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n) {
    if (g_fi_target) g_fi_bytes += n;
    if (fi_should_fail()) return NULL;
    return __real_realloc(p, n);
}
#else
// ponytail: no interceptor, so g_fi_fired can never fire -- main()'s loop
// runs the lifecycle once and stops. Upgrade path is a real macOS allocator
// interposer (malloc zone or DYLD_INTERPOSE) if this coverage gap matters.
static void fi_arm(int nth) { (void)nth; g_fi_fired = 0; }
static void fi_disarm(void) { }
#endif

// ---- the lifecycle under test -------------------------------------------

static hk_hook_spec_t symbol_spec(const char *id, const char *target_name) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;  // string literal -- no test-side allocation
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = target_name;
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void add_one(hk_plan_t *plan, const char *id, const char *target_name) {
    hk_hook_spec_t spec = symbol_spec(id, target_name);
    hk_hook_t *h = NULL;
    hk_status_t s = hk_plan_add_hook(plan, &spec, &h);
    // DRAFT at this point, valid unique spec: OK, or OOM if an alloc failed.
    assert(s == HK_STATUS_OK || s == HK_STATUS_OUT_OF_MEMORY);
}

// Runs a state-advancing op and enforces the OOM-consistency invariant.
static void oom_step(hk_plan_t *plan,
                     hk_status_t (*fn)(hk_plan_t *, hk_report_t **)) {
    hk_plan_state_t before = hk_plan_state(plan);
    hk_status_t s = fn(plan, NULL);
    if (s == HK_STATUS_OUT_OF_MEMORY) {
        assert(hk_plan_state(plan) == before);  // a failed op must not advance state
    } else {
        // OK (advanced), or INVALID_STATE because an earlier step already
        // failed and left the plan behind -- both legitimate.
        assert(s == HK_STATUS_OK || s == HK_STATUS_INVALID_STATE);
    }
}

// Runs one full lifecycle with the Nth allocation failing. Cleans up whatever
// it managed to create (ASan validates leak-freedom across the whole sweep).
static void run_once(int inject_nth) {
    hk_ownership_reset_for_testing();
    fi_arm(inject_nth);

    hk_runtime_t *rt = NULL;
    hk_status_t s = hk_runtime_create(NULL, &rt);
    if (s != HK_STATUS_OK) {
        assert(s == HK_STATUS_OUT_OF_MEMORY && rt == NULL);
        fi_disarm();
        return;
    }
    // Registering the engine is a fixed array write, no allocation.
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));

    hk_plan_t *plan = NULL;
    s = hk_plan_create(rt, NULL, &plan);
    if (s != HK_STATUS_OK) {
        assert(s == HK_STATUS_OUT_OF_MEMORY && plan == NULL);
        hk_runtime_release(rt);
        fi_disarm();
        return;
    }

    add_one(plan, "fi.a", "getpid");
    add_one(plan, "fi.b", "getppid");

    oom_step(plan, hk_plan_analyze);
    oom_step(plan, hk_plan_prepare);
    oom_step(plan, hk_plan_commit);

    // If nothing failed this run, the whole lifecycle must have succeeded --
    // proves the harness baseline is real, not a sequence that quietly no-ops.
    // (When an injection fired but was swallowed -- e.g. fake_rebind ignores a
    // ledger-append OOM -- g_fi_fired is set and this is correctly skipped.)
    if (!g_fi_fired) {
        assert(hk_plan_state(plan) == HK_PLAN_COMMITTED);
    }

    hk_plan_release(plan);
    hk_runtime_release(rt);
    fi_disarm();
}

static void test_ownership_key_reuse(void) {
    hk_ownership_reset_for_testing();
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    add_one(plan, "key.first", "getpid");
    struct hk_hook *hook = plan->hooks[0];
    uint8_t *key = NULL;
    size_t key_size = 0;
    assert(hk_ownership_target_key_copy(&hook->spec, &key, &key_size));
    assert(key != hook->target_key && key_size == hook->target_key_size);
    assert(memcmp(key, hook->target_key, key_size) == 0);

    hk_ownership_state_t state;
    hk_ownership_lock();
    fi_arm(1);
    hk_ownership_lookup_locked(hook->target_key, hook->target_key_size, &state);
    assert(!state.present && !g_fi_fired);
    fi_disarm();
    assert(hk_ownership_record_locked(hook->target_key, hook->target_key_size,
                                      "key-engine", (void *)1, (void *)2));
    // The record must own a copy, not retain the hook's storage.
    memset(hook->target_key, 0, hook->target_key_size);
    hk_plan_release(plan);
    hk_runtime_release(rt);

    fi_arm(1);
    hk_ownership_lookup_locked(key, key_size, &state);
    assert(state.present && state.head_replacement == (void *)1);
    const char *engine_id = state.engine_id;
    assert(hk_ownership_record_locked(key, key_size, "key-engine",
                                      (void *)3, (void *)1));
    hk_ownership_lookup_locked(key, key_size, &state);
    assert(state.present && state.head_replacement == (void *)3);
    assert(state.predecessor == (void *)1 && state.engine_id == engine_id);
    assert(strcmp(state.engine_id, "key-engine") == 0 && !g_fi_fired);
    fi_disarm();
    hk_ownership_unlock();
    free(key);
    hk_ownership_reset_for_testing();
    printf("  ownership-key-copy-and-allocation-free-reuse: PASS\n");
}

static void test_artifact_allocations(void) {
#if defined(__linux__)
    for (int shape = 0; shape < 4; shape++) {
        hk_artifact_t a = {0};
        uint8_t bytes[] = {1, 2, 3, 4};
        if (shape > 0) {
            a.engine_id = (hk_string_view_t){ .data = "objc", .length = 4 };
        }
        if (shape > 1) {
            a.mechanism_id = (hk_string_view_t){ .data = "class_replaceMethod", .length = 19 };
        }
        if (shape == 3) {
            a.image.path = "/temporary/image";
            a.original_bytes.representation = HK_BYTE_STORAGE_INLINE_AND_HASH;
            a.original_bytes.inline_bytes = (hk_bytes_view_t){ .data = bytes, .size = sizeof(bytes) };
            a.original_bytes.length = sizeof(bytes);
            a.expected_bytes = a.expected_mask = a.current_bytes = a.original_bytes;
        }
        fi_arm(INT_MAX);
        hk_artifact_ledger_t *source = hk_artifact_ledger_create();
        assert(source);
        for (int i = 0; i < 64; i++) assert(hk_artifact_ledger_append(source, &a));
        int append_count = g_fi_count;
        size_t append_bytes = g_fi_bytes;
        fi_arm(INT_MAX);
        hk_artifact_ledger_t *copy = hk_artifact_ledger_create();
        assert(copy && hk_artifact_ledger_append_ledger(copy, source));
        int copy_count = g_fi_count;
        size_t copy_bytes = g_fi_bytes;
        fi_arm(INT_MAX);
        hk_artifact_snapshot_t *snapshot = NULL;
        assert(hk_artifact_snapshot_from_ledger(copy, &snapshot) == HK_STATUS_OK);
        int snapshot_count = g_fi_count;
        size_t snapshot_bytes = g_fi_bytes;
        fi_disarm();
        assert(append_count == (shape ? 70 : 6));
        assert(copy_count == (shape ? 66 : 2));
        assert(snapshot_count == (shape ? 66 : 2));
        hk_artifact_ledger_destroy(source);
        hk_artifact_ledger_destroy(copy);
        assert(hk_artifact_snapshot_count(snapshot) == 64);
        hk_artifact_snapshot_release(snapshot);
        printf("  artifact-allocations shape=%d records=64: append=%d/%zu copy=%d/%zu snapshot=%d/%zu (calls/requested-bytes)\n",
               shape, append_count, append_bytes, copy_count, copy_bytes,
               snapshot_count, snapshot_bytes);
    }
#endif
}

static void test_artifact_copy_failures_and_lifetimes(void) {
    uint8_t bytes[] = {1, 2, 3, 4};
    hk_artifact_t a = {0};
    a.engine_id = (hk_string_view_t){ .data = "engine", .length = 6 };
    a.mechanism_id = (hk_string_view_t){ .data = "mechanism", .length = 9 };
    a.image.path = "/image";
    a.original_bytes.representation = HK_BYTE_STORAGE_INLINE;
    a.original_bytes.inline_bytes = (hk_bytes_view_t){ .data = bytes, .size = sizeof(bytes) };
    a.expected_bytes = a.expected_mask = a.current_bytes = a.original_bytes;
    hk_artifact_ledger_t *source = hk_artifact_ledger_create();
    assert(source && hk_artifact_ledger_append(source, &a));
    assert(hk_artifact_ledger_append(source, &a));
    for (int op = 0; op < 3; op++) {
        int n = 1;
        for (;; n++) {
            hk_artifact_ledger_t *dest = hk_artifact_ledger_create();
            assert(dest);
            // A full destination forces growth, then failure can occur after
            // a completed payload copy. The old four records must survive.
            for (int i = 0; i < 4; i++) assert(hk_artifact_ledger_append(dest, &a));
            hk_artifact_snapshot_t *snap = (hk_artifact_snapshot_t *)(uintptr_t)1;
            fi_arm(n);
            bool ok;
            if (op == 0) {
                ok = hk_artifact_ledger_append(dest, &a);
            } else if (op == 1) {
                ok = hk_artifact_ledger_append_ledger(dest, source);
            } else {
                hk_status_t status = hk_artifact_snapshot_from_ledger(source, &snap);
                assert(status == HK_STATUS_OK || status == HK_STATUS_OUT_OF_MEMORY);
                ok = status == HK_STATUS_OK;
                if (!ok) assert(snap == NULL);
            }
            fi_disarm();
            assert(ok == !g_fi_fired);
            assert(hk_artifact_ledger_count(dest) ==
                   (size_t)(4 + (ok && op < 2 ? op + 1 : 0)));
            if (op == 2) hk_artifact_snapshot_release(snap);
            assert(hk_artifact_snapshot_from_ledger(dest, &snap) == HK_STATUS_OK);
            hk_artifact_ledger_destroy(dest);
            hk_artifact_t out;
            assert(hk_artifact_snapshot_copy_at(snap, 3, &out) == HK_STATUS_OK);
            assert(memcmp(out.current_bytes.inline_bytes.data, bytes, sizeof(bytes)) == 0);
            hk_artifact_snapshot_release(snap);
            if (!g_fi_fired) break;
            assert(n < 32);
        }
#if defined(__linux__)
        assert(n == (op == 0 ? 3 : op == 1 ? 4 : 5));
#endif
        printf("  artifact-oom op=%d: %d allocation sites swept\n", op, n - 1);
    }

    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_append_artifacts(rt, source));
    assert(hk_artifact_process_append_ledger(source));
    hk_report_t *report = hk_report_create(NULL, 0);
    assert(report);
    hk_report_adopt_artifact_ledger(report, source);
    hk_artifact_snapshot_t *report_snap = NULL, *runtime_snap = NULL, *process_snap = NULL;
    assert(hk_report_copy_artifacts(report, &report_snap) == HK_STATUS_OK);
    hk_report_release(report);
    assert(hk_runtime_copy_artifacts(rt, &runtime_snap) == HK_STATUS_OK);
    hk_runtime_release(rt);
    assert(hk_copy_process_artifacts(&process_snap) == HK_STATUS_OK);
    hk_artifact_t report_a, runtime_a, process_a;
    assert(hk_artifact_snapshot_copy_at(report_snap, 0, &report_a) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(runtime_snap, 0, &runtime_a) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_copy_at(process_snap,
        hk_artifact_snapshot_count(process_snap) - 1, &process_a) == HK_STATUS_OK);
    assert(report_a.engine_id.data != runtime_a.engine_id.data);
    assert(runtime_a.engine_id.data != process_a.engine_id.data);
    hk_artifact_snapshot_release(report_snap);
    hk_artifact_snapshot_release(runtime_snap);
    assert(strcmp(process_a.engine_id.data, "engine") == 0);
    assert(strcmp(process_a.mechanism_id.data, "mechanism") == 0);
    assert(strcmp(process_a.image.path, "/image") == 0);
    assert(memcmp(process_a.original_bytes.inline_bytes.data, bytes, sizeof(bytes)) == 0);
    assert(memcmp(process_a.expected_bytes.inline_bytes.data, bytes, sizeof(bytes)) == 0);
    assert(memcmp(process_a.expected_mask.inline_bytes.data, bytes, sizeof(bytes)) == 0);
    assert(memcmp(process_a.current_bytes.inline_bytes.data, bytes, sizeof(bytes)) == 0);
    hk_artifact_snapshot_release(process_snap);
    printf("  artifact-report-runtime-process-lifetimes: PASS\n");
}

int main(void) {
    test_artifact_allocations();
    test_artifact_copy_failures_and_lifetimes();
    test_ownership_key_reuse();
    int n = 1;
    for (;;) {
        run_once(n);
        if (!g_fi_fired) {
            // This N exceeded the run's total allocation count: it completed
            // with no injection. Every allocation site has now been failed
            // exactly once across N=1..n-1.
            break;
        }
        n++;
        assert(n < 1000);  // safety: the lifecycle allocates far fewer than this
    }

#if defined(__linux__)
    // The --wrap interceptor is what makes this a sweep rather than a single
    // run: a silent interceptor failure (e.g. a toolchain that ignores --wrap)
    // would pass with zero sites covered. Fail closed on that.
    assert(n > 1);
    printf("all fault-injection tests passed (%d allocation sites swept, "
           "each failed once)\n", n - 1);
#else
    printf("fault-injection sweep skipped (no malloc interceptor on this "
           "platform; lifecycle ran once, uninstrumented)\n");
#endif
    return 0;
}
