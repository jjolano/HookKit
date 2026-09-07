// Arm64 device gate for Milestone 9. The pool lives in a pre-existing
// executable Mach-O section; hook-time allocation is only a fixed-slot claim.

#include <assert.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/core/HKPlanInternal.h"
#include "../../src/core/HKReportInternal.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/engines/HKRelocInlineVtable.h"
#include "../../src/engines/HKStaticPool.h"
#include "../../src/native/hk_native.h"

#define DEVICE_PAGE_BYTES 16384u

__attribute__((used, aligned(DEVICE_PAGE_BYTES), section("__TEXT,__hookkit")))
static const uint8_t hk_static_pool_page[DEVICE_PAGE_BYTES] = {0};

// Catalogued, page-isolated owned code fixtures, separate from __text and pools.
__attribute__((used, aligned(DEVICE_PAGE_BYTES), section("__TEXT,__hkfixture")))
static const uint8_t hk_fixture_pages[3 * DEVICE_PAGE_BYTES] = {0};

__attribute__((noinline))
static int static_target(int value) {
    volatile int result = value;
    result += 1;
    result += 2;
    result += 3;
    result += 4;
    result += 5;
    return result;
}

__attribute__((noinline))
static int static_replacement(int value) {
    return value + 100;
}

typedef struct {
    hk_static_pool_t pool;
    vm_address_t page;
    vm_size_t page_size;
    unsigned alloc_calls;
    unsigned seal_calls;
    unsigned free_calls;
} static_pool_ctx_t;

static bool page_protect(static_pool_ctx_t *ctx, vm_prot_t protection) {
    return vm_protect(mach_task_self(), ctx->page, ctx->page_size, FALSE,
                      protection) == KERN_SUCCESS;
}

static uintptr_t static_pool_alloc(void *opaque, size_t size, uintptr_t near,
                                    hk_artifact_mapping_t *mapping) {
    static_pool_ctx_t *ctx = opaque;
    uintptr_t slot = hk_static_pool_claim(&ctx->pool, size, near);
    if (!slot || !page_protect(ctx, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
        if (slot) hk_static_pool_release(&ctx->pool, slot);
        return 0;
    }
    ctx->alloc_calls++;
    mapping->kind = HK_MAPPING_STATIC_HOOKKIT_SECTION;
    mapping->base = slot;
    mapping->size = ctx->page_size;
    return slot;
}

static bool static_pool_seal(void *opaque, uintptr_t page, size_t size) {
    static_pool_ctx_t *ctx = opaque;
    if (!page_protect(ctx, VM_PROT_READ | VM_PROT_EXECUTE)) return false;
    sys_icache_invalidate((void *)page, size);
    ctx->seal_calls++;
    return true;
}

static void static_pool_free(void *opaque, uintptr_t page, size_t size) {
    static_pool_ctx_t *ctx = opaque;
    (void)page;
    (void)size;
    (void)page_protect(ctx, VM_PROT_READ | VM_PROT_EXECUTE);
    hk_static_pool_release(&ctx->pool, ctx->page);
    ctx->free_calls++;
}

static hk_mutation_state_t static_pool_write(void *opaque, uintptr_t address,
                              const uint8_t *data, size_t size) {
    (void)opaque;
    return hk_native_patch_memory((void *)address, data, size);
}

static bool page_is_executable(vm_address_t address) {
    vm_address_t region = address;
    vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &region, &region_size,
                                    VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && region <= address &&
           (info.protection & VM_PROT_EXECUTE) != 0;
}

static hk_hook_spec_t address_spec(uintptr_t target, void *replacement) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = "device.static.continuation";
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.struct_size = sizeof(spec.target.address);
    spec.target.address.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.address = target;
    spec.replacement = replacement;
    spec.required_reach = HK_REACH_ENTRYPOINT;
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    spec.continuation_policy = HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

__attribute__((noinline))
static int default_target(int value) {
    volatile int result = value;
    result += 10;
    result += 20;
    result += 30;
    return result;
}

__attribute__((noinline))
static int default_replacement(int value) {
    return value + 500;
}

static const hk_original_slot_t *default_original;

// The shipped behavior: with NO caller opt-in, a fresh runtime's default
// relocating engine prefers the in-image __hktramp pool for a near target, so
// the continuation is not an anonymous executable page. Proven by finding the
// continuation's page inside the process's own __TEXT,__hktramp section.
static void test_default_prefers_in_image_pool(void) {
    unsigned long sect_size = 0;
    uint8_t *sect = getsectiondata(&_mh_execute_header, "__TEXT", "__hktramp",
                                   &sect_size);
    assert(sect && sect_size >= DEVICE_PAGE_BYTES);   // the section is mapped
    uintptr_t sect_lo = (uintptr_t)sect;
    uintptr_t sect_hi = sect_lo + sect_size;

    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_report_t *report = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);  // auto engines
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);

    // A plain relocating hook: callable original, no forbid constraint, default
    // continuation policy. Nothing here asks for a static continuation.
    hk_hook_spec_t spec = address_spec((uintptr_t)default_target,
                                       (void *)default_replacement);
    spec.stable_hook_id = "device.default.pool";
    spec.continuation_policy = HK_CONTINUATION_ANY;

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    hk_report_release(report);
    report = NULL;
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE && hook->result.verified);

    // The hook works, and the original is still reachable through it.
    assert(default_target(3) == 503);
    const hk_original_slot_t *slot = hk_hook_original_slot(hook);
    assert(slot);
    default_original = slot;
    int (*original)(int) = (int (*)(int))hk_original_slot_load(slot);
    assert(original && original(3) == 63);

    // The continuation lives in the in-image pool, not an anonymous page: its
    // trampoline base falls inside __TEXT,__hktramp. (arm64 device: the slot
    // base carries no PAC; an arm64e build would strip it first.)
    uintptr_t base = hook->result.continuation.mapping_base;
    assert(base >= sect_lo && base < sect_hi);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_STATIC);
    assert(hook->result.continuation.mapping_kind == HK_MAPPING_STATIC_HOOKKIT_SECTION);
    assert(hook->result.continuation.mapping_size == DEVICE_PAGE_BYTES);
    assert(!hook->result.continuation.executable_memory_allocated);
    assert(hook->result.observed_prepare_effects == HK_EFFECT_STATIC_CONTINUATION_USE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    assert(default_target(3) == 503 && original(3) == 63);
    puts("HookKit default-prefers-pool: PASS");
}

// These modes each require a fresh process: release is not unhook.
static hk_hook_t *add_owned_hook(hk_runtime_t *runtime, hk_plan_t **plan,
                                 uintptr_t target, bool static_only) {
    hk_hook_spec_t spec = address_spec(target, (void *)static_replacement);
    spec.continuation_policy = static_only
        ? HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY : HK_CONTINUATION_ANY;
    hk_hook_t *hook = NULL;
    assert(hk_plan_create(runtime, NULL, plan) == HK_STATUS_OK);
    assert(hk_plan_add_hook(*plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(*plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert(hook->matched_engine == (static_only ? hk_static_inline_vtable()
                                              : hk_reloc_inline_vtable()));
    return hook;
}

static void check_refusal(hk_runtime_t *runtime, hk_hook_t *hook,
                           const uint8_t *before) {
    assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
    assert(hk_hook_result_refused_cleanly(&hook->result));
    assert(hook->result.mutation == HK_MUTATION_NONE);
    assert(hook->result.continuation.address == 0);
    const hk_original_slot_t *slot = hk_hook_original_slot(hook);
    assert(!slot || !hk_original_slot_load(slot));
    assert(memcmp(before, (void *)static_target, 16) == 0);
    assert(static_target(2) == 17);
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_runtime_copy_artifacts(runtime, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 0);
    hk_artifact_snapshot_release(snapshot);
}

static void test_abandoned_reuse(void) {
    test_default_prefers_in_image_pool();
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    uintptr_t reused = 0;
    uint8_t before[16];
    memcpy(before, (void *)static_target, sizeof(before));
    for (unsigned i = 0; i < 64; i++) {
        hk_plan_t *plan = NULL;
        hk_hook_t *hook = add_owned_hook(runtime, &plan, (uintptr_t)static_target, true);
        assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
        assert(hook->result.outcome == HK_OUTCOME_PREPARED);
        assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_STATIC);
        uintptr_t base = hook->result.continuation.mapping_base;
        if (i) assert(base == reused);
        reused = base;
        assert(!hk_hook_original_slot(hook));
        hk_plan_release(plan);
        assert(memcmp(before, (void *)static_target, sizeof(before)) == 0);
        assert(static_target(2) == 17 && default_target(3) == 503);
        int (*original)(int) = (void *)hk_original_slot_load(default_original);
        assert(original && original(3) == 63);
    }
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_runtime_copy_artifacts(runtime, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 0);
    hk_artifact_snapshot_release(snapshot);
    hk_runtime_release(runtime);
    int (*original)(int) = (void *)hk_original_slot_load(default_original);
    assert(original && original(3) == 63 && default_target(3) == 503);
    printf("PASS abandoned-reuse cycles=64 slot=%p active-original-after-teardown=callable simulation=none\n",
           (void *)reused);
}

static void test_stale_cleanup(void) {
    uintptr_t page = (uintptr_t)hk_fixture_pages;
    const uint32_t code[] = {0x11000400, 0xd503201f, 0xd503201f, 0xd503201f, 0xd65f03c0};
    assert(hk_native_patch_memory((void *)page, code, sizeof(code)) == HK_MUTATION_COMPLETE);
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    uintptr_t reused = 0;
    for (unsigned i = 0; i < 32; i++) {
        hk_plan_t *plan = NULL;
        hk_hook_t *hook = add_owned_hook(runtime, &plan, page, true);
        assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
        assert(hook->result.outcome == HK_OUTCOME_PREPARED);
        uintptr_t base = hook->result.continuation.mapping_base;
        if (i) assert(base == reused);
        reused = base;
        // Real, controlled edit to our idle fixture between prepare and commit.
        // Alternate add #1 / add #2; neither version branches to the preparation.
        uint32_t changed = i % 2 ? 0x11000400 : 0x11000800;
        assert(hk_native_patch_memory((void *)page, &changed, sizeof(changed)) == HK_MUTATION_COMPLETE);
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
        assert(hook->result.outcome == HK_OUTCOME_FAILED_SAFE);
        assert(hk_hook_result_refused_cleanly(&hook->result));
        assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_NONE);
        assert(!hook->result.continuation.address && !hk_hook_original_slot(hook));
        assert(memcmp((void *)page, &changed, sizeof(changed)) == 0);
        int (*target)(int) = (void *)page;
        assert(target(2) == (i % 2 ? 3 : 4));
        hk_plan_release(plan);
    }
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_runtime_copy_artifacts(runtime, &snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 0);
    hk_artifact_snapshot_release(snapshot);
    hk_runtime_release(runtime);
    printf("PASS stale-cleanup cycles=32 slot=%p zero-artifacts fixture-edit=real simulation=none\n",
           (void *)reused);
}

static void test_exhaustion(bool static_only) {
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    hk_reloc_engine_ctx_t *backing = &runtime->static_engine;
    unsigned long section_size = 0;
    uint8_t *section = getsectiondata(&_mh_execute_header, "__TEXT", "__hktramp",
                                     &section_size);
    assert(section && section_size % DEVICE_PAGE_BYTES == 0);
    size_t count = section_size / DEVICE_PAGE_BYTES;
    assert(count && count <= HK_STATIC_POOL_MAX_SLOTS);
    uintptr_t held[HK_STATIC_POOL_MAX_SLOTS] = {0};
    // Claim every REAL slot, rather than treating an allocation refusal as
    // evidence of exhaustion. No published trampoline is touched.
    for (size_t i = 0; i < count; i++) {
        hk_artifact_mapping_t mapping = {0};
        held[i] = backing->alloc(backing->seam_ctx, HK_RELOC_PAGE_BYTES,
                                 (uintptr_t)static_target, &mapping);
        assert(held[i] && mapping.kind == HK_MAPPING_STATIC_HOOKKIT_SECTION);
        assert(held[i] >= (uintptr_t)section &&
               held[i] - (uintptr_t)section < section_size);
        for (size_t j = 0; j < i; j++) assert(held[i] != held[j]);
        assert(backing->seal(backing->seam_ctx, held[i], HK_RELOC_PAGE_BYTES));
    }
    hk_artifact_mapping_t mapping = {0};
    assert(!backing->alloc(backing->seam_ctx, HK_RELOC_PAGE_BYTES,
                           (uintptr_t)static_target, &mapping));
    uint8_t before[16];
    memcpy(before, (void *)static_target, sizeof(before));
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = add_owned_hook(runtime, &plan, (uintptr_t)static_target, static_only);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(memcmp(before, (void *)static_target, sizeof(before)) == 0);
    if (static_only) {
        check_refusal(runtime, hook, before);
        assert(hk_plan_state(plan) == HK_PLAN_FAILED);
        assert(hook->result.error_code == HK_RELOC_NO_TRAMPOLINE);
    } else {
        assert(hook->result.outcome == HK_OUTCOME_PREPARED);
        assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_DYNAMIC);
        assert(hook->result.continuation.mapping_kind == HK_MAPPING_ANONYMOUS);
        assert(hook->result.continuation.executable_memory_allocated);
        assert(hook->result.observed_prepare_effects == HK_EFFECT_EXECUTABLE_ALLOCATION);
        assert(page_is_executable(hook->result.continuation.mapping_base));
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
        assert(hook->result.outcome == HK_OUTCOME_ACTIVE && hook->result.verified);
        assert(hook->result.observed_commit_effects ==
               (HK_EFFECT_EXECUTABLE_ALLOCATION | HK_EFFECT_TARGET_TEXT_MUTATION));
        assert(static_target(2) == 102);
        int (*original)(int) = (void *)hk_original_slot_load(hk_hook_original_slot(hook));
        assert(original && original(2) == 17);
        hk_artifact_snapshot_t *snapshot = NULL;
        assert(hk_runtime_copy_artifacts(runtime, &snapshot) == HK_STATUS_OK);
        assert(hk_artifact_snapshot_count(snapshot) == 2);
        hk_artifact_t trampoline;
        assert(hk_artifact_snapshot_copy_at(snapshot, 0, &trampoline) == HK_STATUS_OK);
        assert(trampoline.kind == HK_ARTIFACT_TRAMPOLINE);
        assert(trampoline.mapping.kind == HK_MAPPING_ANONYMOUS);
        assert(trampoline.mapping.base == hook->result.continuation.mapping_base);
        assert(trampoline.effects == HK_EFFECT_EXECUTABLE_ALLOCATION);
        hk_artifact_snapshot_release(snapshot);
    }
    for (size_t i = 0; i < count; i++)
        backing->free_page(backing->seam_ctx, held[i], HK_RELOC_PAGE_BYTES);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    printf("PASS exhaustion slots=%zu route=%s simulation=none\n", count,
           static_only ? "static-only-clean-refusal" : "anonymous-fallback");
}

typedef struct {
    hk_reloc_engine_ctx_t native;
    const char *failure;
    unsigned allocations, seals, writes, frees, injected;
    uintptr_t page;
} fault_ctx_t;

static uintptr_t fault_alloc(void *opaque, size_t size, uintptr_t near,
                              hk_artifact_mapping_t *mapping) {
    fault_ctx_t *ctx = opaque;
    ctx->allocations++;
    if (!strcmp(ctx->failure, "alloc")) { ctx->injected++; return 0; }
    ctx->page = ctx->native.alloc(ctx->native.seam_ctx, size, near, mapping);
    assert(ctx->page); // A real refusal is not the injected failure requested.
    return ctx->page;
}

static bool fault_seal(void *opaque, uintptr_t page, size_t size) {
    fault_ctx_t *ctx = opaque;
    ctx->seals++;
    if (!strcmp(ctx->failure, "seal")) { ctx->injected++; return false; }
    bool sealed = ctx->native.seal(ctx->native.seam_ctx, page, size);
    assert(sealed);
    return sealed;
}

static void fault_free(void *opaque, uintptr_t page, size_t size) {
    fault_ctx_t *ctx = opaque;
    assert(page == ctx->page && ctx->frees == 0);
    // The production pool free returns a bitmap slot; restore its protection
    // explicitly after the simulated seal failure before returning that slot.
    if (!strcmp(ctx->failure, "seal"))
        assert(ctx->native.seal(ctx->native.seam_ctx, page, size));
    ctx->native.free_page(ctx->native.seam_ctx, page, size);
    ctx->frees++;
}

static hk_mutation_state_t fault_write(void *opaque, uintptr_t address, const uint8_t *data, size_t size) {
    fault_ctx_t *ctx = opaque;
    (void)address; (void)data; (void)size;
    assert(!strcmp(ctx->failure, "write"));
    ctx->writes++;
    ctx->injected++;
    return HK_MUTATION_NONE; // SIMULATION: refusal BEFORE any store, not partial/kernel failure.
}

static void test_injected_failure(const char *failure) {
    printf("SIMULATION failure=%s seam-return-only kernel-failure-proven=no\n", failure);
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    fault_ctx_t ctx = {.native = runtime->reloc_engine, .failure = failure};
    hk_reloc_engine_ctx_t engine = ctx.native;
    engine.alloc = fault_alloc; engine.seal = fault_seal; engine.free_page = fault_free;
    engine.write = fault_write; engine.seam_ctx = &ctx; engine.write_ctx = &ctx;
    runtime->engine_count = 0;
    assert(hk_runtime_register_engine_with_context(runtime, hk_reloc_inline_vtable(), &engine));
    uint8_t before[16];
    memcpy(before, (void *)static_target, sizeof(before));
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = add_owned_hook(runtime, &plan, (uintptr_t)static_target, false);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    if (!strcmp(failure, "write")) {
        assert(hook->result.outcome == HK_OUTCOME_PREPARED);
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
        assert(ctx.writes == 1);
    } else {
        assert(hk_plan_state(plan) == HK_PLAN_FAILED);
        assert(hook->result.error_code == HK_RELOC_NO_TRAMPOLINE);
        assert(ctx.writes == 0);
    }
    check_refusal(runtime, hook, before);
    assert(ctx.injected == 1 && ctx.allocations == 1);
    assert(ctx.seals == (strcmp(failure, "alloc") != 0));
    assert(ctx.frees == (strcmp(failure, "alloc") != 0));
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    hk_artifact_snapshot_t *snapshot = NULL;
    assert(hk_copy_process_artifacts(&snapshot) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(snapshot) == 0);
    hk_artifact_snapshot_release(snapshot);
    printf("PASS simulated-%s unchanged-target zero-artifacts frees=%u\n", failure, ctx.frees);
}

typedef struct {
    hk_runtime_t *runtime;
    hk_plan_t *plan;
    hk_hook_t *hook;
} install_job_t;
static unsigned workers_ready, start_workers, prepared_workers, commit_workers, stop_reader;
static unsigned long reader_calls;
static int (*running_target)(int);
static const hk_original_slot_t *running_original;

static int running_replacement(int value) {
    int (*original)(int) = (void *)hk_original_slot_load(running_original);
    assert(original);
    return original(value) + 100;
}

static void *run_earlier_hook(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&stop_reader, __ATOMIC_ACQUIRE)) {
        assert(running_target(2) == 103);
        __atomic_add_fetch(&reader_calls, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void *install_owned_hook(void *opaque) {
    install_job_t *job = opaque;
    __atomic_add_fetch(&workers_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE)) sched_yield();
    assert(hk_plan_prepare(job->plan, NULL) == HK_STATUS_OK);
    assert(job->hook->result.outcome == HK_OUTCOME_PREPARED);
    __atomic_add_fetch(&prepared_workers, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&commit_workers, __ATOMIC_ACQUIRE)) sched_yield();
    assert(hk_plan_commit(job->plan, NULL) == HK_STATUS_OK);
    assert(job->hook->result.outcome == HK_OUTCOME_ACTIVE && job->hook->result.verified);
    return NULL;
}

static void test_concurrent(bool same_page) {
    // A separate image section avoids __text alignment tricks while satisfying
    // normal image-scope validation. Its pages contain no test/engine code.
    uintptr_t pages[3];
    const uint32_t code[] = {0x11000400, 0xd503201f, 0xd503201f, 0xd503201f,
                             0xd65f03c0}; // add w0,w0,#1; nop x3; ret
    for (unsigned i = 0; i < 3; i++) {
        pages[i] = (uintptr_t)hk_fixture_pages + i * DEVICE_PAGE_BYTES;
        assert(pages[i] && pages[i] % DEVICE_PAGE_BYTES == 0);
        assert(hk_native_patch_memory((void *)pages[i], code, sizeof(code)) == HK_MUTATION_COMPLETE);
        assert(hk_native_patch_memory((void *)(pages[i] + 64), code, sizeof(code)) == HK_MUTATION_COMPLETE);
    }
    uintptr_t targets[] = {pages[0], pages[1], same_page ? pages[1] + 64 : pages[2]};
    assert(targets[0] / DEVICE_PAGE_BYTES != targets[1] / DEVICE_PAGE_BYTES);
    assert(targets[0] / DEVICE_PAGE_BYTES != targets[2] / DEVICE_PAGE_BYTES);
    assert((targets[1] / DEVICE_PAGE_BYTES == targets[2] / DEVICE_PAGE_BYTES) == same_page);
    hk_runtime_t *runtime = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    hk_plan_t *earlier = NULL;
    hk_hook_spec_t spec = address_spec(targets[0], (void *)running_replacement);
    spec.continuation_policy = HK_CONTINUATION_ANY;
    hk_hook_t *hook = NULL;
    assert(hk_plan_create(runtime, NULL, &earlier) == HK_STATUS_OK);
    assert(hk_plan_add_hook(earlier, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(earlier, NULL) == HK_STATUS_OK);
    assert(hk_plan_prepare(earlier, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hk_plan_commit(earlier, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE && hook->result.verified);
    running_original = hk_hook_original_slot(hook);
    running_target = (void *)targets[0];
    install_job_t jobs[2] = {0};
    pthread_t reader, workers[2];
    // Separate owners avoid concurrently mutating one runtime's local ledger;
    // the production pool, ownership registry and native writer remain shared.
    for (unsigned i = 0; i < 2; i++) {
        assert(hk_runtime_create(NULL, &jobs[i].runtime) == HK_STATUS_OK);
        jobs[i].hook = add_owned_hook(jobs[i].runtime, &jobs[i].plan, targets[i + 1], false);
    }
    assert(pthread_create(&reader, NULL, run_earlier_hook, NULL) == 0);
    for (unsigned i = 0; i < 2; i++)
        assert(pthread_create(&workers[i], NULL, install_owned_hook, &jobs[i]) == 0);
    while (__atomic_load_n(&workers_ready, __ATOMIC_ACQUIRE) != 2 ||
           !__atomic_load_n(&reader_calls, __ATOMIC_ACQUIRE)) sched_yield();
    unsigned long before = __atomic_load_n(&reader_calls, __ATOMIC_ACQUIRE);
    __atomic_store_n(&start_workers, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&prepared_workers, __ATOMIC_ACQUIRE) != 2) sched_yield();
    // Keep both prepared continuations alive together; neither may share the
    // earlier continuation's page or each other's page.
    uintptr_t base = hook->result.continuation.mapping_base;
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_STATIC);
    for (unsigned i = 0; i < 2; i++) {
        assert(jobs[i].hook->result.continuation.kind == HK_CONTINUATION_KIND_STATIC);
        assert(jobs[i].hook->result.continuation.mapping_base % DEVICE_PAGE_BYTES == 0);
        assert(memcmp((void *)targets[i + 1], code, sizeof(code)) == 0);
    }
    assert(jobs[0].hook->result.continuation.mapping_base != base);
    assert(jobs[1].hook->result.continuation.mapping_base != base);
    assert(jobs[0].hook->result.continuation.mapping_base !=
           jobs[1].hook->result.continuation.mapping_base);
    __atomic_store_n(&commit_workers, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; i++) assert(pthread_join(workers[i], NULL) == 0);
    unsigned long after = __atomic_load_n(&reader_calls, __ATOMIC_ACQUIRE);
    __atomic_store_n(&stop_reader, 1, __ATOMIC_RELEASE);
    assert(pthread_join(reader, NULL) == 0);
    assert(after > before);
    for (unsigned i = 0; i < 2; i++) {
        int (*target)(int) = (void *)targets[i + 1];
        int (*original)(int) = (void *)hk_original_slot_load(hk_hook_original_slot(jobs[i].hook));
        assert(target(2) == 102 && original && original(2) == 3);
        hk_plan_release(jobs[i].plan);
        hk_runtime_release(jobs[i].runtime);
    }
    assert(running_target(2) == 103);
    hk_plan_release(earlier);
    hk_runtime_release(runtime);
    // Published fixture/continuation pages stay mapped until process exit.
    printf("PASS concurrent targets=%s reader-calls-during-window=%lu fixture-pages=3 simulation=none\n",
           same_page ? "same-idle-page" : "distinct-idle-pages", after - before);
    puts("GAP scheduler overlap inside individual VM syscalls not proven; executing target page never patched");
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    alarm(30); // Bound assertion failures/deadlocks to this disposable process.
    assert(getpagesize() == DEVICE_PAGE_BYTES);
    if (argc == 2 && strcmp(argv[1], "baseline")) {
        if (!strcmp(argv[1], "exhaust-fallback")) test_exhaustion(false);
        else if (!strcmp(argv[1], "exhaust-static")) test_exhaustion(true);
        else if (!strcmp(argv[1], "fail-alloc")) test_injected_failure("alloc");
        else if (!strcmp(argv[1], "fail-seal")) test_injected_failure("seal");
        else if (!strcmp(argv[1], "fail-write")) test_injected_failure("write");
        else if (!strcmp(argv[1], "concurrent-distinct")) test_concurrent(false);
        else if (!strcmp(argv[1], "concurrent-same-page")) test_concurrent(true);
        else if (!strcmp(argv[1], "abandoned-reuse")) test_abandoned_reuse();
        else if (!strcmp(argv[1], "stale-cleanup")) test_stale_cleanup();
        else return 2;
        return 0;
    }
    if (argc != 1 && argc != 2) return 2;
    test_default_prefers_in_image_pool();

    const vm_size_t page_size = (vm_size_t)getpagesize();
    assert(page_size == DEVICE_PAGE_BYTES);
    assert(((uintptr_t)hk_static_pool_page % page_size) == 0);
    assert(page_is_executable((vm_address_t)hk_static_pool_page));

    static_pool_ctx_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.page = (vm_address_t)hk_static_pool_page;
    pool.page_size = page_size;
    assert(hk_static_pool_init(&pool.pool, pool.page, page_size, 1));

    hk_reloc_engine_ctx_t engine;
    memset(&engine, 0, sizeof(engine));
    engine.alloc = static_pool_alloc;
    engine.seal = static_pool_seal;
    engine.free_page = static_pool_free;
    engine.seam_ctx = &pool;
    engine.write = static_pool_write;
    engine.write_ctx = &pool;
    engine.allow_non_atomic_entry_patch = false;

    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_report_t *report = NULL;
    hk_artifact_snapshot_t *artifacts = NULL;
    assert(hk_runtime_create(NULL, &runtime) == HK_STATUS_OK);
    // hk_runtime_create now auto-registers a static engine backed by the
    // runtime's own __hktramp pool (see the default path exercised below).
    // This scenario isolates the engine mechanism over a CALLER-supplied pool,
    // so clear the auto set first and register only ours -- otherwise the
    // router would pick the auto engine (same vtable, registered first) and
    // this test's controlled counters would never move.
    runtime->engine_count = 0;
    assert(hk_runtime_register_engine_with_context(
        runtime, hk_static_inline_vtable(), &engine));
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = address_spec((uintptr_t)static_target,
                                       (void *)static_replacement);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    hk_report_release(report);
    report = NULL;
    assert(hook->matched_engine == hk_static_inline_vtable());

    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_PREPARED);
    assert(hook->result.declared_prepare_effects ==
           HK_EFFECT_STATIC_CONTINUATION_USE);
    assert(hook->result.observed_prepare_effects ==
           HK_EFFECT_STATIC_CONTINUATION_USE);
    assert(pool.alloc_calls == 1 && pool.seal_calls == 1);
    assert(hk_static_pool_free_count(&pool.pool) == 0);

    assert(hk_plan_commit(plan, &report) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ACTIVE);
    assert(hook->result.verified);
    assert(hook->result.continuation.kind == HK_CONTINUATION_KIND_STATIC);
    assert(hook->result.continuation.mapping_kind ==
           HK_MAPPING_STATIC_HOOKKIT_SECTION);
    assert(hook->result.continuation.mapping_base == pool.page);
    assert(!hook->result.continuation.executable_memory_allocated);
    assert(hook->result.observed_commit_effects ==
           (HK_EFFECT_TARGET_TEXT_MUTATION | HK_EFFECT_STATIC_CONTINUATION_USE));
    assert(pool.free_calls == 0);

    assert(static_target(2) == 102);
    const hk_original_slot_t *slot = hk_hook_original_slot(hook);
    assert(slot);
    int (*original)(int) = (int (*)(int))hk_original_slot_load(slot);
    assert(original && original(2) == 17);

    assert(hk_report_copy_artifacts(report, &artifacts) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(artifacts) == 2);
    hk_artifact_t trampoline;
    memset(&trampoline, 0, sizeof(trampoline));
    assert(hk_artifact_snapshot_copy_at(artifacts, 0, &trampoline) == HK_STATUS_OK);
    assert(trampoline.kind == HK_ARTIFACT_STATIC_CONTINUATION);
    assert((trampoline.effects & HK_EFFECT_EXECUTABLE_ALLOCATION) == 0);
    assert(trampoline.mapping.kind == HK_MAPPING_STATIC_HOOKKIT_SECTION);
    assert(trampoline.mapping.base == pool.page);

    hk_artifact_snapshot_release(artifacts);
    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    puts("HookKit static continuation: PASS");
    return 0;
}
