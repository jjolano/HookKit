#import <HookKit.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Machine-readable output contract (one line per phase, key=value tokens):
//   hkperf meta      version=<framework version> loadavg1=<float>
//   hkperf startup   PASS|FAIL cpu_us= rss= regions=
//   hkperf single    PASS|FAIL wall_us=
//   hkperf batch_c   PASS|FAIL|SKIP count= enqueue_us= commit_us= per_hook_us=
//   hkperf batch_objc PASS|FAIL|SKIP count= enqueue_us= commit_us= per_hook_us=
// Aggregate min/median/p95 across samples with scripts/device-perf-ab.sh.

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

static const char *linked_framework_version(void) {
    static char version[32];
    NSString *path = @"/var/jb/Library/Frameworks/HookKit.framework";
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        path = @"/Library/Frameworks/HookKit.framework";
    }
    NSBundle *bundle = [NSBundle bundleWithPath:path];
    NSString *short_version =
        bundle.infoDictionary[@"CFBundleShortVersionString"];
    if (!short_version) {
        short_version = bundle.infoDictionary[@"CFBundleVersion"];
    }
    strlcpy(version, short_version ? short_version.UTF8String : "unknown",
            sizeof(version));
    return version;
}

// ---- batched C-function targets -------------------------------------------

#define PERF_COUNT 32
#define PERF_TARGETS \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) \
    X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

#define X(n) __attribute__((noinline)) static int perf_fn_##n(int value) { \
    volatile int result = value; result += 1; result += 2; result += 3;   \
    result += 4; return result; }
PERF_TARGETS
#undef X

#define X(n) __attribute__((noinline)) static int perf_repl_##n(int value) { \
    return value + 100; }
PERF_TARGETS
#undef X

static void *perf_targets[PERF_COUNT];
static void *perf_repls[PERF_COUNT];
static int (*perf_origs[PERF_COUNT])(int);

__attribute__((constructor)) static void perf_tables_init(void) {
#define X(n) perf_targets[n] = (void *)perf_fn_##n; \
             perf_repls[n] = (void *)perf_repl_##n;
    PERF_TARGETS
#undef X
}

// ---- batched Objective-C targets ------------------------------------------

#define OBJC_COUNT 16
#define OBJC_METHODS \
    Y(0) Y(1) Y(2) Y(3) Y(4) Y(5) Y(6) Y(7) Y(8) Y(9) Y(10) Y(11) Y(12) \
    Y(13) Y(14) Y(15)

@interface PerfObj : NSObject
#define Y(n) - (int)m##n:(int)value;
OBJC_METHODS
#undef Y
@end

@implementation PerfObj
#define Y(n) - (int)m##n:(int)value { volatile int r = value; r += 1; return r + 2; }
OBJC_METHODS
#undef Y
@end

#define Y(n) __attribute__((noinline)) static int perf_objc_repl_##n(       \
    id self __attribute__((unused)), SEL cmd __attribute__((unused)),       \
    int value) { return value + 100; }
OBJC_METHODS
#undef Y

static void *objc_repls[OBJC_COUNT];
static IMP objc_origs[OBJC_COUNT];

__attribute__((constructor)) static void objc_tables_init(void) {
#define Y(n) objc_repls[n] = (void *)perf_objc_repl_##n;
    OBJC_METHODS
#undef Y
}

// ----------------------------------------------------------------------------

static bool batch_supported(HKSubstitutor *substitutor) {
    return [substitutor respondsToSelector:@selector(setBatching:)] &&
           [substitutor respondsToSelector:@selector(executeHooks)];
}

int main(void) {
    setbuf(stdout, NULL);

    double loadavg[3] = { -1, -1, -1 };
    getloadavg(loadavg, 3);

    // These first samples include the process/framework startup work already
    // charged to this task when main begins.
    uint64_t startup_cpu = cpu_microseconds();
    uint64_t startup_rss = resident_bytes();
    unsigned startup_regions = vm_region_count();

    printf("hkperf meta version=%s loadavg1=%.2f\n",
           linked_framework_version(), loadavg[0]);

    int (*original)(int) = NULL;
    HKSubstitutor *substitutor = [HKSubstitutor
        substitutorWithBackendIDs:@[@"inline-relocating", @"memory"]];
    uint64_t hook_start = mach_absolute_time();
    hookkit_status_t status = [substitutor hookFunction:(void *)perfTarget
        withReplacement:(void *)perfReplacement outOldPtr:(void **)&original];
    uint64_t hook_end = mach_absolute_time();

    int hooked = perfTarget(2);
    bool single_pass = status == HK_OK && original &&
                       hooked == 102 && original(2) == 12;
    printf("hkperf startup %s cpu_us=%llu rss=%llu regions=%u\n",
           single_pass ? "PASS" : "FAIL",
           (unsigned long long)startup_cpu,
           (unsigned long long)startup_rss, startup_regions);
    printf("hkperf single %s wall_us=%llu\n",
           single_pass ? "PASS" : "FAIL",
           (unsigned long long)elapsed_microseconds(hook_start, hook_end));

    bool all_pass = single_pass;

    // Batched C-function installs: enqueue N, commit once. Reports the split
    // between plan/enqueue cost and the single patch publication.
    if (!batch_supported(substitutor)) {
        printf("hkperf batch_c SKIP reason=no-batching-api\n");
    } else {
        memset(perf_origs, 0, sizeof(perf_origs));
        substitutor.batching = YES;
        uint64_t enqueue_start = mach_absolute_time();
        hookkit_status_t batch_status = HK_OK;
        for (int i = 0; i < PERF_COUNT && batch_status == HK_OK; i++) {
            batch_status = [substitutor hookFunction:perf_targets[i]
                  withReplacement:perf_repls[i]
                       outOldPtr:(void **)&perf_origs[i]];
        }
        uint64_t enqueue_end = mach_absolute_time();
        hookkit_status_t commit_status = [substitutor executeHooks];
        uint64_t commit_end = mach_absolute_time();
        substitutor.batching = NO;

        bool batch_pass = batch_status == HK_OK && commit_status == HK_OK &&
                          perf_fn_0(2) == 102 && perf_origs[0](2) == 12;
        uint64_t total = elapsed_microseconds(enqueue_start, commit_end);
        printf("hkperf batch_c %s count=%d enqueue_us=%llu commit_us=%llu "
               "per_hook_us=%.1f\n",
               batch_pass ? "PASS" : "FAIL", PERF_COUNT,
               (unsigned long long)elapsed_microseconds(enqueue_start, enqueue_end),
               (unsigned long long)elapsed_microseconds(enqueue_end, commit_end),
               (double)total / PERF_COUNT);
        all_pass = all_pass && batch_pass;
    }

    // Batched Objective-C method installs (Shadow's dominant disposition).
    if (!batch_supported(substitutor)) {
        printf("hkperf batch_objc SKIP reason=no-batching-api\n");
    } else {
        memset(objc_origs, 0, sizeof(objc_origs));
        Class cls = [PerfObj class];
        substitutor.batching = YES;
        uint64_t enqueue_start = mach_absolute_time();
        hookkit_status_t batch_status = HK_OK;
        for (int i = 0; i < OBJC_COUNT && batch_status == HK_OK; i++) {
            char selector_name[16];
            snprintf(selector_name, sizeof(selector_name), "m%d:", i);
            batch_status = [substitutor hookMessageInClass:cls
                withSelector:sel_registerName(selector_name)
                withReplacement:objc_repls[i]
                outOldPtr:(void **)&objc_origs[i]];
        }
        uint64_t enqueue_end = mach_absolute_time();
        hookkit_status_t commit_status = [substitutor executeHooks];
        uint64_t commit_end = mach_absolute_time();
        substitutor.batching = NO;

        PerfObj *probe = [PerfObj new];
        typedef int (*imp_fn)(id, SEL, int);
        bool objc_pass = batch_status == HK_OK && commit_status == HK_OK &&
                         [probe m0:2] == 102 &&
                         ((imp_fn)objc_origs[0])(probe, @selector(m0:), 2) == 5;
        uint64_t total = elapsed_microseconds(enqueue_start, commit_end);
        printf("hkperf batch_objc %s count=%d enqueue_us=%llu commit_us=%llu "
               "per_hook_us=%.1f enqueue_status=%d commit_status=%d\n",
               objc_pass ? "PASS" : "FAIL", OBJC_COUNT,
               (unsigned long long)elapsed_microseconds(enqueue_start, enqueue_end),
               (unsigned long long)elapsed_microseconds(enqueue_end, commit_end),
               (double)total / OBJC_COUNT,
               batch_status, commit_status);
        all_pass = all_pass && objc_pass;
    }

    return all_pass ? 0 : 1;
}
