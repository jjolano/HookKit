// Execute the actual native writer on host buffers, replacing only Mach/VM
// operations. This is not a bool-writer simulation and executes no A64 code.
#define _DEFAULT_SOURCE 1
#ifndef __aarch64__
#define __aarch64__ 1
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/native/hk_native.c"
#include "../../src/engines/HKRelocInlineVtable.h"
#include "../../src/engines/HKInlineEngine.h"
#include "../../src/engines/HKMemoryEngine.h"
#include "../../src/engines/HKRebindEngine.h"

enum failure { OK, PROBE, RESTORE, ALLOCATE, TEMP_SEAL, REMAP, REMAP_OK };
static enum failure failure;
static unsigned protects, remaps, frees;
static unsigned restore_failure_call = 2;
static uintptr_t temporary;

kern_return_t vm_region_64(mach_port_t task, vm_address_t *address,
    vm_size_t *size, int flavor, vm_region_info_t info,
    mach_msg_type_number_t *count, mach_port_t *object) {
    (void)task; (void)address; (void)flavor; (void)count;
    *object = MACH_PORT_NULL;
    *size = (size_t)getpagesize();
    info->protection = VM_PROT_READ | VM_PROT_EXECUTE;
    return failure == PROBE ? KERN_INVALID_ADDRESS : KERN_SUCCESS;
}
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t port) {
    (void)task; (void)port; return KERN_SUCCESS;
}
kern_return_t vm_protect(mach_port_t task, vm_address_t address, vm_size_t size,
                         int maximum, vm_prot_t protection) {
    (void)task; (void)address; (void)size; (void)maximum; (void)protection;
    protects++;
    if ((failure >= ALLOCATE && protects == 1) ||
        (failure == RESTORE && protects == restore_failure_call) ||
        (failure == TEMP_SEAL && protects == 2)) return KERN_INVALID_ADDRESS;
    return KERN_SUCCESS;
}
kern_return_t vm_allocate(mach_port_t task, vm_address_t *address, vm_size_t size, int flags) {
    (void)task; (void)flags;
    if (failure == ALLOCATE) return KERN_INVALID_ADDRESS;
    *address = temporary = (uintptr_t)aligned_alloc((size_t)getpagesize(), size);
    assert(*address);
    return KERN_SUCCESS;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t address, vm_size_t size) {
    (void)task; (void)size;
    assert(address == temporary);
    free((void *)address); temporary = 0;
    return KERN_SUCCESS;
}
kern_return_t vm_remap(mach_port_t task, vm_address_t *dst, vm_size_t size,
    vm_address_t mask, int flags, mach_port_t source_task, vm_address_t src,
    int copy, vm_prot_t *cur, vm_prot_t *max, int inherit) {
    (void)task; (void)mask; (void)flags; (void)source_task; (void)copy;
    (void)cur; (void)max; (void)inherit;
    remaps++;
    if (failure == REMAP) return KERN_INVALID_ADDRESS;
    memcpy((void *)*dst, (void *)src, size);
    return KERN_SUCCESS;
}
void sys_icache_invalidate(void *address, size_t size) { (void)address; (void)size; }

static hk_mutation_state_t native_write(void *ctx, uintptr_t dst, const uint8_t *src, size_t size) {
    (void)ctx;
    return hk_native_patch_memory((void *)dst, src, size);
}
static hk_mutation_state_t native_pointer(void *ctx, uintptr_t dst, uint64_t value) {
    (void)ctx;
    return hk_native_patch_pointer((void *)dst, (void *)(uintptr_t)value);
}
static uintptr_t alloc_backing(void *ctx, size_t size, uintptr_t near, hk_artifact_mapping_t *mapping) {
    (void)near;
    size_t page_size = (size_t)getpagesize();
    assert(size <= page_size);
    uintptr_t page = (uintptr_t)aligned_alloc(page_size, page_size);
    assert(page);
    mapping->kind = *(bool *)ctx ? HK_MAPPING_STATIC_HOOKKIT_SECTION : HK_MAPPING_ANONYMOUS;
    mapping->base = page; mapping->size = page_size;
    return page;
}
static bool seal_backing(void *ctx, uintptr_t page, size_t size) {
    (void)ctx; (void)page; (void)size; return true;
}
static void free_backing(void *ctx, uintptr_t page, size_t size) {
    (void)ctx; (void)size; frees++; free((void *)page);
}

int main(void) {
    const uint32_t body[] = {0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD65F03C0};
    uint32_t *target = aligned_alloc((size_t)getpagesize(), (size_t)getpagesize());
    assert(target);
    memset(target, 0, (size_t)getpagesize());
    for (unsigned backing = 0; backing < 2; backing++) {
        bool is_static = backing != 0;
        hk_reloc_engine_ctx_t ctx = {.alloc = alloc_backing, .seal = seal_backing,
            .free_page = free_backing, .seam_ctx = &is_static,
            .write = native_write, .allow_non_atomic_entry_patch = true};
        const hk_engine_vtable_t *vtable = is_static ? hk_static_inline_vtable() : hk_reloc_inline_vtable();
        for (failure = OK; failure <= REMAP_OK; failure++) {
            memcpy(target, body, sizeof(body)); protects = remaps = frees = 0;
            hk_hook_spec_t spec = {.target_kind = HK_TARGET_FUNCTION_ADDRESS,
                .replacement = (void *)((uintptr_t)target + 64)};
            spec.target.address.address = (uintptr_t)target;
            void *prepared = NULL;
            hk_prepare_diag_t diag = {0};
            assert(vtable->prepare_one_ctx_status(&ctx, &spec, &prepared, &diag) == HK_PREPARE_OK);
            uintptr_t page = ((hk_reloc_plan_t *)prepared)->trampoline;
            hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
            hk_artifact_sink_t sink = {.ledger = ledger};
            hk_mutation_state_t expected = failure == OK || failure == REMAP_OK ? HK_MUTATION_COMPLETE :
                failure == RESTORE || failure == REMAP ? HK_MUTATION_UNKNOWN : HK_MUTATION_NONE;
            assert(vtable->commit_one_ctx(&ctx, &spec, prepared, &sink) == expected);
            assert(temporary == 0);
            assert((target[0] != body[0]) == (failure == OK || failure == RESTORE || failure == REMAP_OK));
            assert(hk_artifact_ledger_count(ledger) == (expected == HK_MUTATION_NONE ? 0u : 2u));
            assert(sink.continuation.readable == (expected != HK_MUTATION_NONE));
            if (expected != HK_MUTATION_NONE) {
                // A repeated commit must not enable retry or reclaim uncertainty.
                assert(((hk_reloc_plan_t *)prepared)->activated);
                assert(hk_reloc_commit(prepared, native_write, NULL, free_backing, NULL, NULL) == HK_MUTATION_UNKNOWN);
            }
            vtable->release_prepared(&ctx, prepared);
            assert(frees == (expected == HK_MUTATION_NONE ? 1u : 0u));
            if (expected != HK_MUTATION_NONE) free((void *)page);
            hk_artifact_ledger_destroy(ledger);
        }
    }
    // The same real post-store failure must not become NONE in sibling engines.
    failure = RESTORE; protects = 0; memcpy(target, body, sizeof(body));
    hk_inline_plan_t inline_plan;
    assert(hk_inline_prepare((uintptr_t)target, (uintptr_t)target + 64,
        HK_ORIGINAL_NONE, NULL, 0, &inline_plan) == HK_INLINE_OK);
    assert(hk_inline_commit(&inline_plan, native_write, NULL, NULL) == HK_MUTATION_UNKNOWN);
    protects = 0; memcpy(target, body, sizeof(body));
    hk_mempatch_plan_t memory_plan;
    assert(hk_mempatch_prepare((uintptr_t)target, 4, (hk_bytes_view_t){0},
        (hk_bytes_view_t){0}, &memory_plan) == HK_MEMPATCH_OK);
    uint32_t replacement = 123;
    assert(hk_mempatch_commit((uintptr_t)target, &memory_plan,
        (hk_bytes_view_t){(uint8_t *)&replacement, 4}, native_write, NULL, NULL) == HK_MUTATION_UNKNOWN);
    assert(target[0] == replacement);
    protects = 0;
    void **slot = (void **)target;
    *slot = NULL;
    assert(hk_native_patch_pointer(slot, target) == HK_MUTATION_UNKNOWN && *slot == target);
    failure = ALLOCATE; protects = 0;
    assert(hk_native_patch_pointer(slot, NULL) == HK_MUTATION_NONE && *slot == target);
    // UNKNOWN on either the first or a later rebind slot cannot be flattened
    // to NONE/PARTIAL, and no following slot may be attempted.
    for (unsigned first = 0; first < 2; first++) {
        failure = RESTORE; protects = 0; restore_failure_call = first ? 2 : 4;
        memset(slot, 0, 3 * sizeof(void *));
        hk_rebind_plan_t plan = {.count = 3};
        for (unsigned i = 0; i < 3; i++) plan.sites[i].address = (uintptr_t)&slot[i];
        hk_rebind_target_t rebind_target = {.write = native_pointer};
        hk_artifact_ledger_t *ledger = hk_artifact_ledger_create();
        hk_artifact_sink_t sink = {.ledger = ledger};
        uint32_t written = 99;
        assert(hk_rebind_commit(&rebind_target, &plan, (uintptr_t)target, &sink, &written) == HK_MUTATION_UNKNOWN);
        assert(written == (first ? 0u : 1u));
        assert(slot[0] == target && slot[2] == NULL);
        assert(hk_artifact_ledger_count(ledger) == (first ? 1u : 2u));
        hk_artifact_snapshot_t *snapshot = NULL;
        assert(hk_artifact_snapshot_from_ledger(ledger, &snapshot) == HK_STATUS_OK);
        hk_artifact_t artifact;
        assert(hk_artifact_snapshot_copy_at(snapshot, written, &artifact) == HK_STATUS_OK);
        assert(artifact.state == HK_ARTIFACT_PARTIALLY_APPLIED && !artifact.verified);
        hk_artifact_snapshot_release(snapshot);
        hk_artifact_ledger_destroy(ledger);
    }
    free(target);
    puts("native write: real store/restore failure, remap uncertainty, no-store refusal, retention: PASS");
    return 0;
}
