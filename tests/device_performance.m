#import <HookKit.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static int perfTarget(int value) {
    volatile int result = value;
    result += 1;
    result += 2;
    result += 3;
    result += 4;
    return result;
}

__attribute__((noinline)) static int perfReplacement(int value) {
    return value + 100;
}

static uint64_t cpu_microseconds(void) {
    task_thread_times_info_data_t info;
    mach_msg_type_number_t count = TASK_THREAD_TIMES_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_THREAD_TIMES_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return 0;
    }
    return (uint64_t)info.user_time.seconds * 1000000u +
           (uint64_t)info.user_time.microseconds +
           (uint64_t)info.system_time.seconds * 1000000u +
           (uint64_t)info.system_time.microseconds;
}

static uint64_t resident_bytes(void) {
    task_basic_info_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size;
}

static unsigned vm_region_count(void) {
    unsigned count = 0;
    vm_address_t address = 0;
    for (;;) {
        vm_address_t region = address;
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t status = vm_region_64(
            mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
            (vm_region_info_t)&info, &info_count, &object);
        if (object != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object);
        }
        if (status != KERN_SUCCESS || size == 0 || region + size < region) {
            break;
        }
        count++;
        address = region + size;
    }
    return count;
}

static uint64_t elapsed_microseconds(uint64_t start, uint64_t end) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (end - start) * timebase.numer / timebase.denom / 1000u;
}

int main(void) {
    setbuf(stdout, NULL);

    // These first samples include the process/framework startup work already
    // charged to this task when main begins.
    uint64_t startup_cpu = cpu_microseconds();
    uint64_t startup_rss = resident_bytes();
    unsigned startup_regions = vm_region_count();

    int (*original)(int) = NULL;
    HKSubstitutor *substitutor = [HKSubstitutor substitutorWithTypes:HK_LIB_NATIVE];
    uint64_t hook_start = mach_absolute_time();
    hookkit_status_t status = [substitutor hookFunction:(void *)perfTarget
        withReplacement:(void *)perfReplacement outOldPtr:(void **)&original];
    uint64_t hook_end = mach_absolute_time();

    int hooked = perfTarget(2);
    bool pass = status == HK_OK && original && hooked == 102 && original(2) == 12;
    printf("HookKit2.5 performance: %s startup_cpu_us=%llu post_cpu_us=%llu "
           "startup_rss=%llu post_rss=%llu startup_vm_regions=%u "
           "post_vm_regions=%u hook_wall_us=%llu\n",
           pass ? "PASS" : "FAIL",
           (unsigned long long)startup_cpu,
           (unsigned long long)cpu_microseconds(),
           (unsigned long long)startup_rss,
           (unsigned long long)resident_bytes(),
           startup_regions, vm_region_count(),
           (unsigned long long)elapsed_microseconds(hook_start, hook_end));
    return pass ? 0 : 1;
}
