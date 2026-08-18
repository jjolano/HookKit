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
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "fake_engines.h"

// ---- allocation interceptor (linked with -Wl,--wrap=malloc,calloc,realloc) --

extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);

static int g_fi_target;   // fail this (1-based) allocation while armed; 0 = disabled
static int g_fi_count;    // allocations seen since arm
static int g_fi_fired;    // set when an injection actually fired

static void fi_arm(int nth) { g_fi_target = nth; g_fi_count = 0; g_fi_fired = 0; }
static void fi_disarm(void) { g_fi_target = 0; }

static int fi_should_fail(void) {
    if (g_fi_target && ++g_fi_count == g_fi_target) {
        g_fi_fired = 1;
        return 1;
    }
    return 0;
}

void *__wrap_malloc(size_t n) {
    if (fi_should_fail()) return NULL;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b) {
    if (fi_should_fail()) return NULL;
    return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n) {
    if (fi_should_fail()) return NULL;
    return __real_realloc(p, n);
}

// ---- the lifecycle under test -------------------------------------------

static hk_hook_spec_t symbol_spec(const char *id) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;  // string literal -- no test-side allocation
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = "getpid";
    spec.required_reach = HK_REACH_EXISTING_IMPORTS;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static void add_one(hk_plan_t *plan, const char *id) {
    hk_hook_spec_t spec = symbol_spec(id);
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

    add_one(plan, "fi.a");
    add_one(plan, "fi.b");

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

int main(void) {
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

    printf("all fault-injection tests passed (%d allocation sites swept, "
           "each failed once)\n", n - 1);
    return 0;
}
