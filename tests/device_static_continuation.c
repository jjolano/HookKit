// Arm64 device gate for Milestone 9. The pool lives in a pre-existing
// executable Mach-O section; hook-time allocation is only a fixed-slot claim.

#include <assert.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../Sources/Core/HKPlanInternal.h"
#include "../Sources/Core/HKReportInternal.h"
#include "../Sources/Core/HKRuntimeInternal.h"
#include "../Sources/Engines/HKRelocInlineVtable.h"
#include "../Sources/Engines/HKStaticPool.h"
#include "../native/hk_native.h"

#define DEVICE_PAGE_BYTES 16384u

__attribute__((used, aligned(DEVICE_PAGE_BYTES), section("__TEXT,__hookkit")))
static const uint8_t hk_static_pool_page[DEVICE_PAGE_BYTES] = {0};

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

static uintptr_t static_pool_alloc(void *opaque, size_t size, uintptr_t near) {
    static_pool_ctx_t *ctx = opaque;
    uintptr_t slot = hk_static_pool_claim(&ctx->pool, size, near);
    if (!slot || !page_protect(ctx, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
        if (slot) hk_static_pool_release(&ctx->pool, slot);
        return 0;
    }
    ctx->alloc_calls++;
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

static bool static_pool_write(void *opaque, uintptr_t address,
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
    int (*original)(int) = (int (*)(int))hk_original_slot_load(slot);
    assert(original && original(3) == 63);

    // The continuation lives in the in-image pool, not an anonymous page: its
    // trampoline base falls inside __TEXT,__hktramp. (arm64 device: the slot
    // base carries no PAC; an arm64e build would strip it first.)
    uintptr_t base = hook->result.continuation.mapping_base;
    assert(base >= sect_lo && base < sect_hi);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    puts("HookKit default-prefers-pool: PASS");
}

int main(void) {
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
    engine.static_continuation = true;

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
