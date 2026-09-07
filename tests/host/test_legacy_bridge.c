// Real legacy bridge + plan + native adapter. Replace only runtime creation
// (to register host seams) and the optional post-commit API-error injection.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/core/HKOwnership.h"
#include "../../src/core/HKInstalled.h"
#include "../../src/engines/HKRelocInlineVtable.h"
#include "../../src/internal/HKPointerAuth.h"

static hk_status_t bridge_runtime(const hk_runtime_config_t *, const char *, hk_runtime_t **);
static hk_status_t bridge_commit(hk_plan_t *, hk_report_t **);
#define hk_runtime_create_with_backend_override bridge_runtime
#define hk_plan_commit bridge_commit
#include "../../src/compatibility/HKLegacyFacade.c"
#undef hk_plan_commit
#undef hk_runtime_create_with_backend_override

enum { WRITE_UNKNOWN, WRITE_PARTIAL, VERIFY_FAIL, REFUSE, SUCCESS };
static struct {
    uint32_t *targets[2];
    void *pages[2], *originals[2], *expected[2];
    unsigned allocations, frees, writes, mode;
    bool api_error;
} fixture;

static uintptr_t alloc_page(void *ctx, size_t size, uintptr_t near,
                             hk_artifact_mapping_t *mapping) {
    (void)ctx;
    unsigned i = near == (uintptr_t)fixture.targets[0] ? 0 : 1;
    assert(near == (uintptr_t)fixture.targets[i]);
    void *page = aligned_alloc(16, (size + 15) & ~(size_t)15);
    assert(page);
    fixture.pages[i] = page;
    fixture.expected[i] = (void *)hk_pac_make_callable((uintptr_t)page + HK_RELOC_THUNK_BYTES);
    fixture.allocations++;
    mapping->kind = HK_MAPPING_ANONYMOUS;
    mapping->base = (uintptr_t)page;
    mapping->size = size;
    return (uintptr_t)page;
}
static bool seal_page(void *ctx, uintptr_t page, size_t size) {
    (void)ctx; (void)page; (void)size; return true;
}
static void free_page(void *ctx, uintptr_t page, size_t size) {
    (void)ctx; (void)size;
    unsigned i = page == (uintptr_t)fixture.pages[0] ? 0 : 1;
    assert(page == (uintptr_t)fixture.pages[i]);
    free((void *)page);
    fixture.pages[i] = NULL;
    fixture.frees++;
}
static hk_mutation_state_t write_entry(void *ctx, uintptr_t target,
                                       const uint8_t *bytes, size_t size) {
    (void)ctx;
    unsigned i = target == (uintptr_t)fixture.targets[0] ? 0 : 1;
    assert(target == (uintptr_t)fixture.targets[i]);
    assert(fixture.originals[i] == fixture.expected[i]); // published BEFORE store
    fixture.writes++;
    if (fixture.mode == REFUSE) return HK_MUTATION_NONE;
    memcpy((void *)target, bytes, size);
    if (fixture.mode == VERIFY_FAIL) ((uint8_t *)target)[0] ^= 1;
    return fixture.mode == WRITE_UNKNOWN ? HK_MUTATION_UNKNOWN :
        fixture.mode == WRITE_PARTIAL ? HK_MUTATION_PARTIAL : HK_MUTATION_COMPLETE;
}
static hk_status_t bridge_runtime(const hk_runtime_config_t *config,
                                   const char *backends, hk_runtime_t **runtime) {
    (void)backends;
    hk_status_t status = hk_runtime_create(config, runtime);
    if (status != HK_STATUS_OK) return status;
    static hk_reloc_engine_ctx_t ctx = {.alloc = alloc_page, .seal = seal_page,
        .free_page = free_page, .write = write_entry, .allow_non_atomic_entry_patch = true};
    assert(hk_runtime_register_engine_with_context(*runtime, hk_reloc_inline_vtable(), &ctx));
    return HK_STATUS_OK;
}
static hk_status_t bridge_commit(hk_plan_t *plan, hk_report_t **report) {
    hk_status_t status = hk_plan_commit(plan, report);
    if (fixture.api_error && status == HK_STATUS_OK) {
        // Model report construction failing AFTER hook results were settled.
        hk_report_release(*report);
        *report = NULL;
        return HK_STATUS_OUT_OF_MEMORY;
    }
    return status;
}

int main(void) {
    for (unsigned single = 0; single < 2; single++) {
        for (unsigned api_error = 0; api_error < 2; api_error++) {
            for (unsigned mode = WRITE_UNKNOWN; mode <= SUCCESS; mode++) {
                memset(&fixture, 0, sizeof(fixture));
                fixture.mode = mode; fixture.api_error = api_error != 0;
                size_t count = single ? 1 : 2;
                hk_hook_spec_t specs[2];
                char ids[2][48];
                void **originals[2];
                int results[2] = {0};
                for (size_t i = 0; i < count; i++) {
                    fixture.targets[i] = aligned_alloc(16, 32);
                    assert(fixture.targets[i]);
                    for (size_t j = 0; j < 8; j++) fixture.targets[i][j] = 0xD503201Fu;
                    originals[i] = &fixture.originals[i];
                    assert(hk_legacy_build_function_spec(fixture.targets[i],
                        (void *)((uintptr_t)fixture.targets[i] + 64), originals[i],
                        ids[i], sizeof(ids[i]), &specs[i]) == HK_LEGACY_OK);
                }
                int expected = mode == SUCCESS ? HK_LEGACY_OK : mode == REFUSE ? HK_LEGACY_ERR_NOT_SUPPORTED :
                    mode == WRITE_PARTIAL ? HK_LEGACY_ERR_PARTIAL : HK_LEGACY_ERR;
                if (single) {
                    int status = hk_legacy_hook_function(fixture.targets[0], specs[0].replacement, originals[0]);
                    assert(status == (api_error ? HK_LEGACY_ERR : expected));
                } else {
                    int status = hk_legacy_apply_specs(specs, originals, count, results);
                    assert(status == (mode == SUCCESS ? HK_LEGACY_OK : HK_LEGACY_ERR));
                    for (size_t i = 0; i < count; i++) assert(results[i] == expected);
                }
                assert(fixture.allocations == count && fixture.writes == count);
                assert(fixture.frees == (mode == REFUSE ? count : 0));
                for (size_t i = 0; i < count; i++) {
                    assert(fixture.originals[i] == (mode == REFUSE ? NULL : fixture.expected[i]));
                    if (fixture.pages[i]) {
                        uint32_t instruction;
                        memcpy(&instruction, (uint8_t *)fixture.pages[i] + HK_RELOC_THUNK_BYTES, 4);
                        assert(instruction == 0xD503201Fu); // backing remains readable
                    }
                    free(fixture.pages[i]);
                    free(fixture.targets[i]);
                }
                hk_ownership_reset_for_testing();
                hk_installed_reset_for_testing();
            }
        }
    }
    puts("legacy bridge: batch/single originals survive PARTIAL, UNKNOWN, verification/API failure; NONE clears: PASS");
    return 0;
}
