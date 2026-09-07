// Owned-process baseline only. No restoration: active hooks live until exit.
#include <HookKit/HookKit.h>
#include <HookKit/HookKitObjC.h>
#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const hk_original_slot_t *original_slot;
static hk_continuation_info_t inline_continuation;
static uint8_t memory_target[] = {1, 2, 3, 4};
static const uint8_t memory_before[] = {1, 2, 3, 4}, memory_after[] = {5, 6, 7, 8};

__attribute__((noinline)) static int function_target(int value) {
    volatile int result = value;
    result += 1;
    result += 2;
    result += 3;
    return result;
}
static int function_replacement(int value) {
    int (*original)(int) = hk_original_slot_load(original_slot);
    assert(original);
    return original(value) + 100;
}
static int method_original(id self, SEL selector) {
    (void)self; (void)selector;
    return 7;
}
static int method_replacement(id self, SEL selector) {
    int (*original)(id, SEL) = hk_original_slot_load(original_slot);
    assert(original);
    return original(self, selector) + 100;
}

static void identity(void *symbol, const char *path_env, const char *uuid_env) {
    Dl_info info;
    char expected[PATH_MAX], actual[PATH_MAX], uuid[33] = {0};
    assert(getenv(path_env) && getenv(uuid_env));
    assert(dladdr(symbol, &info));
    assert(realpath(getenv(path_env), expected) && realpath(info.dli_fname, actual));
    assert(strcmp(expected, actual) == 0);
    const struct mach_header_64 *header = info.dli_fbase;
    assert(header->magic == MH_MAGIC_64);
    const struct load_command *command = (const void *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (command->cmd == LC_UUID) {
            const struct uuid_command *u = (const void *)command;
            for (unsigned j = 0; j < 16; j++) snprintf(uuid + 2*j, 3, "%02x", u->uuid[j]);
        }
        command = (const void *)((const char *)command + command->cmdsize);
    }
    assert(uuid[0] && strcmp(uuid, getenv(uuid_env)) == 0);
    printf("IDENTITY path=%s uuid=%s\n", actual, uuid);
}

static void observe(const char *phase, const void *target, size_t size, Method method) {
    struct mach_task_basic_info task;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    assert(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&task, &count) == KERN_SUCCESS);
    vm_address_t address = 0;
    vm_size_t region_size = 0;
    natural_t depth = 0;
    size_t executable_regions = 0;
    uint64_t executable_bytes = 0;
    for (;;) {
        vm_region_submap_info_data_64_t info;
        count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = vm_region_recurse_64(mach_task_self(), &address, &region_size,
            &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr == KERN_INVALID_ADDRESS) break;
        assert(kr == KERN_SUCCESS);
        if (info.is_submap) { depth++; continue; }
        if (info.protection & VM_PROT_EXECUTE) {
            executable_regions++;
            executable_bytes += region_size;
        }
        assert(region_size && address + region_size > address);
        address += region_size;
    }
    printf("OBS phase=%s images=%u exec_regions=%zu exec_bytes=%llu rss=%llu target=%p bytes=",
        phase, _dyld_image_count(), executable_regions,
        (unsigned long long)executable_bytes, (unsigned long long)task.resident_size, target);
    for (size_t i = 0; i < size; i++) printf("%02x", ((const uint8_t *)target)[i]);
    printf(" imp=%p original=%p\n", method ? method_getImplementation(method) : NULL,
        original_slot ? hk_original_slot_load(original_slot) : NULL);
    void *original = original_slot ? hk_original_slot_load(original_slot) : NULL;
    if (original) {
        Dl_info image = {0};
        unsigned long section_size = 0;
        uint8_t *section = NULL;
        if (dladdr(original, &image))
            section = getsectiondata(image.dli_fbase, "__TEXT", "__hktramp", &section_size);
        bool in_pool = section && (uintptr_t)original >= (uintptr_t)section &&
            (uintptr_t)original - (uintptr_t)section < section_size;
        printf("ORIGINAL phase=%s image=%s in_hktramp=%d bytes=", phase,
            image.dli_fname ? image.dli_fname : "<not resolved by dladdr>", in_pool);
        for (size_t i = 0; i < 16; i++) printf("%02x", ((const uint8_t *)original)[i]);
        putchar('\n');
    }
}

static void check_inline_continuation(const hk_hook_result_t *result, bool static_only) {
    const hk_continuation_info_t *c = &result->continuation;
    Dl_info image = {0};
    unsigned long section_size = 0;
    assert(dladdr((void *)hk_runtime_create, &image));
    uint8_t *section = getsectiondata(image.dli_fbase, "__TEXT", "__hktramp", &section_size);
    // Fresh process, owned near target: both routes must actually use the pool.
    assert(section && c->mapping_base >= (uintptr_t)section);
    assert(c->mapping_size <= section_size && c->mapping_base - (uintptr_t)section <= section_size - c->mapping_size);
    assert(c->kind == HK_CONTINUATION_KIND_STATIC && c->mapping_kind == HK_MAPPING_STATIC_HOOKKIT_SECTION);
    assert(!c->executable_memory_allocated && c->mapping_size == (size_t)getpagesize());
    assert(c->mapping_base % (uintptr_t)getpagesize() == 0);
    assert(c->address >= c->mapping_base && c->address - c->mapping_base < c->mapping_size);
    assert(c->mapping_protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    assert(c->mapping_id.high || c->mapping_id.low);
    assert(c->relocated_instruction_count && c->jump_back_destination ==
           (uintptr_t)function_target + c->relocated_instruction_count * 4);
    assert(result->declared_prepare_effects == (static_only ? HK_EFFECT_STATIC_CONTINUATION_USE :
           HK_EFFECT_STATIC_CONTINUATION_USE | HK_EFFECT_EXECUTABLE_ALLOCATION));
    assert(result->observed_prepare_effects == HK_EFFECT_STATIC_CONTINUATION_USE);
    vm_address_t base = c->mapping_base;
    vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t vm;
    for (;;) {
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        assert(vm_region_recurse_64(mach_task_self(), &base, &size, &depth,
            (vm_region_recurse_info_t)&vm, &count) == KERN_SUCCESS);
        if (!vm.is_submap) break;
        depth++;
    }
    assert(base <= c->mapping_base && size >= c->mapping_size &&
           c->mapping_base - base <= size - c->mapping_size);
    assert(vm.protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    inline_continuation = *c;
}

static void artifacts(const char *phase, hk_artifact_snapshot_t *snapshot) {
    printf("ARTIFACTS phase=%s count=%zu\n", phase, hk_artifact_snapshot_count(snapshot));
    for (size_t i = 0; i < hk_artifact_snapshot_count(snapshot); i++) {
        hk_artifact_t a;
        assert(hk_artifact_snapshot_copy_at(snapshot, i, &a) == HK_STATUS_OK);
        printf("ARTIFACT kind=%d state=%d effects=%llx address=%p artifact_size=%zu mapping=%d base=%p mapping_size=%zu original=%p replacement=%p inspected=%d\n",
            a.kind, a.state, (unsigned long long)a.effects, (void *)a.address, a.size,
            a.mapping.kind, (void *)a.mapping.base, a.mapping.size,
            a.original_pointer, a.replacement_pointer, a.fully_inspected);
        if (a.kind == HK_ARTIFACT_MEMORY_PATCH) {
            assert(a.original_bytes.inline_bytes.size == sizeof(memory_before));
            assert(memcmp(a.original_bytes.inline_bytes.data, memory_before, sizeof(memory_before)) == 0);
        } else if (a.kind == HK_ARTIFACT_TARGET_TEXT_PATCH) {
            assert(a.address == (uintptr_t)function_target);
            assert(a.replacement_pointer == (void *)function_replacement);
            assert(a.original_pointer == hk_original_slot_load(original_slot));
            assert(a.effects == HK_EFFECT_TARGET_TEXT_MUTATION);
        } else if (a.kind == HK_ARTIFACT_STATIC_CONTINUATION || a.kind == HK_ARTIFACT_TRAMPOLINE) {
            assert(a.kind == HK_ARTIFACT_STATIC_CONTINUATION && a.effects == HK_EFFECT_STATIC_CONTINUATION_USE);
            assert(a.size == 128 && a.size < a.mapping.size); // fixed buffer, not allocation size
            assert(a.mapping.kind == inline_continuation.mapping_kind);
            assert(a.mapping.base == inline_continuation.mapping_base && a.mapping.size == inline_continuation.mapping_size);
            assert(a.mapping.mapping_id.high == inline_continuation.mapping_id.high &&
                   a.mapping.mapping_id.low == inline_continuation.mapping_id.low);
            assert(a.mapping.protection.read && a.mapping.protection.execute && !a.mapping.protection.write);
            assert(a.address == a.mapping.base && a.continuation_address == inline_continuation.address);
            assert(a.jump_back_destination == inline_continuation.jump_back_destination);
        }
    }
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    if (argc != 2 || (strcmp(argv[1], "memory") && strcmp(argv[1], "objc") &&
                     strcmp(argv[1], "inline") && strcmp(argv[1], "static") &&
                     strcmp(argv[1], "prepare-refusal"))) {
        fprintf(stderr, "usage: %s memory|objc|inline|static|prepare-refusal\n", argv[0]);
        return 2;
    }
    identity((void *)hk_runtime_create, "HK_EXPECT_FRAMEWORK", "HK_EXPECT_FRAMEWORK_UUID");
    identity((void *)main, "HK_EXPECT_PROBE", "HK_EXPECT_PROBE_UUID");
    bool refusal_check = strcmp(argv[1], "prepare-refusal") == 0;
    bool memory = refusal_check || strcmp(argv[1], "memory") == 0, objc = strcmp(argv[1], "objc") == 0;
    bool static_only = strcmp(argv[1], "static") == 0;
    Method method = NULL;
    id object = nil;
    SEL selector = sel_registerName("baselineValue");
    int (*send)(id, SEL) = (void *)objc_msgSend;
    hk_hook_spec_t spec = {0};
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = argv[1];
    spec.role = HK_OPERATION_MANDATORY;
    const void *target = (void *)function_target;
    size_t size = 16;
    if (memory) {
        target = memory_target; size = sizeof(memory_target);
        spec.target_kind = HK_TARGET_MEMORY_PATCH;
        spec.target.memory.struct_size = sizeof(spec.target.memory);
        spec.target.memory.struct_version = HK_ABI_VERSION_3_0;
        spec.target.memory.address = (uintptr_t)target;
        spec.target.memory.size = size;
        spec.target.memory.kind = HK_MEMORY_KIND_DATA;
        // A mismatched precondition reaches analysis but deterministically fails preparation.
        spec.target.memory.expected_bytes = (hk_bytes_view_t){refusal_check ? memory_after : memory_before, size};
        spec.target.memory.replacement_bytes = (hk_bytes_view_t){memory_after, size};
        spec.required_reach = HK_REACH_EXACT_MEMORY;
    } else if (objc) {
        Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "HKObservabilityOwned", 0);
        assert(cls && class_addMethod(cls, selector, (IMP)method_original, "i@:"));
        objc_registerClassPair(cls);
        object = class_createInstance(cls, 0);
        method = class_getInstanceMethod(cls, selector);
        assert(object && method && send(object, selector) == 7);
        target = (void *)method_original;
        hk_objc_spec_init(&spec, argv[1], hk_objc_instance_method(cls, selector), (void *)method_replacement);
        spec.original_requirement = HK_ORIGINAL_DIRECT_PREDECESSOR;
    } else {
        assert(function_target(3) == 9);
        spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
        spec.target.address.struct_size = sizeof(spec.target.address);
        spec.target.address.struct_version = HK_ABI_VERSION_3_0;
        spec.target.address.address = (uintptr_t)target;
        spec.replacement = (void *)function_replacement;
        spec.required_reach = HK_REACH_ENTRYPOINT;
        spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
        spec.continuation_policy = static_only ? HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY : HK_CONTINUATION_ANY;
    }
    uint8_t before[16];
    memcpy(before, target, size);
    observe("before-runtime", target, size, method);
    hk_runtime_t *runtime = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create_with_backend_override(NULL,
        memory || objc ? "memory" : static_only ? "inline-static" : "inline-relocating", &runtime) == HK_STATUS_OK);
    assert(hk_plan_create(runtime, NULL, &plan) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    hk_hook_result_t result;
    assert(hk_hook_copy_result(hook, &result) == HK_STATUS_OK);
    if (refusal_check) assert(result.outcome == HK_OUTCOME_ANALYZED);
    observe("analyzed", target, size, method);
    assert(hk_plan_prepare(plan, NULL) == HK_STATUS_OK);
    observe("prepared", target, size, method);
    assert(memcmp(before, target, size) == 0);
    if (objc) assert(method_getImplementation(method) == (IMP)method_original);
    assert(hk_hook_copy_result(hook, &result) == HK_STATUS_OK);
    bool prepare_refused = hk_hook_result_refused_cleanly(&result);
    if (refusal_check) {
        assert(prepare_refused && result.outcome == HK_OUTCOME_FAILED_SAFE);
        assert(hk_plan_state(plan) == HK_PLAN_FAILED);
    }
    if (!prepare_refused) {
        assert(result.outcome == HK_OUTCOME_PREPARED);
        if (!memory && !objc) check_inline_continuation(&result, static_only);
        assert(hk_plan_commit(plan, NULL) == HK_STATUS_OK);
        original_slot = hk_hook_original_slot(hook);
        assert(hk_hook_copy_result(hook, &result) == HK_STATUS_OK);
        if (!memory && !objc) {
            assert(memcmp(&inline_continuation, &result.continuation, sizeof(inline_continuation)) == 0);
            assert(result.observed_commit_effects == (HK_EFFECT_STATIC_CONTINUATION_USE | HK_EFFECT_TARGET_TEXT_MUTATION));
        }
    }
    printf("RESULT mode=%s outcome=%d mutation=%d engine=%.*s error=%lld message=%.*s continuation=%d mapping=%d base=%p size=%zu dynamic_exec=%d\n",
        argv[1], result.outcome, result.mutation, (int)result.diagnostic_engine_id.length,
        result.diagnostic_engine_id.data ?: "", (long long)result.error_code,
        (int)result.error_message.length, result.error_message.data ?: "",
        result.continuation.kind, result.continuation.mapping_kind,
        (void *)result.continuation.mapping_base, result.continuation.mapping_size,
        result.continuation.executable_memory_allocated);
    bool skipped = (refusal_check || (!memory && !objc)) && hk_hook_result_refused_cleanly(&result);
    if (!skipped) assert(result.outcome == HK_OUTCOME_ACTIVE && result.mutation == HK_MUTATION_COMPLETE);
    observe(prepare_refused ? "commit-not-attempted" : "committed", target, size, method);
    hk_artifact_snapshot_t *retained = NULL, *process = NULL;
    assert(hk_runtime_copy_artifacts(runtime, &retained) == HK_STATUS_OK);
    size_t artifact_count = hk_artifact_snapshot_count(retained);
    assert(skipped ? artifact_count == 0 : artifact_count > 0);
    artifacts("runtime", retained);
    hk_plan_release(plan);
    hk_runtime_release(runtime);
    // Do not consult released hooks/results or free live trampoline storage.
    if (skipped) original_slot = NULL;
    observe("released", target, size, method);
    assert(hk_copy_process_artifacts(&process) == HK_STATUS_OK);
    assert(hk_artifact_snapshot_count(process) == artifact_count);
    artifacts("retained-snapshot", retained);
    artifacts("process-after-release", process);
    if (skipped) {
        assert(memcmp(before, target, size) == 0 && function_target(3) == 9);
        if (refusal_check)
            puts("PASS mode=prepare-refusal: expected FAILED_SAFE/HK_PLAN_FAILED; no commit, unchanged target/behavior, zero artifacts after release");
        else
            printf("SKIP mode=%s: native route refused without target mutation; see RESULT diagnostics\n", argv[1]);
    } else if (memory) {
        assert(memcmp(target, memory_after, size) == 0);
    } else if (objc) {
        assert(memcmp(before, target, size) == 0);
        assert(method_getImplementation(method) == (IMP)method_replacement);
        assert(send(object, selector) == 107);
        assert(((int (*)(id, SEL))hk_original_slot_load(original_slot))(object, selector) == 7);
    } else {
        assert(memcmp(before, target, size) != 0);
        assert(function_target(3) == 109);
        assert(((int (*)(int))hk_original_slot_load(original_slot))(3) == 9);
        if (static_only) assert(result.continuation.kind == HK_CONTINUATION_KIND_STATIC && !result.continuation.executable_memory_allocated);
    }
    hk_artifact_snapshot_release(retained);
    hk_artifact_snapshot_release(process);
    observe("snapshots-released", target, size, method);
    if (!skipped) printf("PASS mode=%s behavior and retained artifacts; release is not unhook\n", argv[1]);
    return skipped && !refusal_check ? 77 : 0;
}
