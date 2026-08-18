#import <Foundation/Foundation.h>
#import <HookKit/HookKit.h>

#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int (*dobbyOriginal)(int);
static int (*nativeOriginal)(int);
static NSInteger (*messageOriginal)(id, SEL);
static int dobbyHits;
static int nativeHits;

// Replacement for the dyld trap-stub fail-soft check. It is never actually
// installed (the target is refused before dispatch), so its body is
// irrelevant; it only needs to be a valid non-NULL replacement pointer.
__attribute__((noinline)) static int dyldStubReplacement(void) {
    return 0;
}

// Trampoline page isolation — the regression test for the crash this release
// fixes. The old allocator bump-allocated trampolines out of a shared page
// and flipped that whole page to read-write to build each new one, stripping
// EXECUTE from up to 127 already-published trampolines for the duration. Any
// thread running an earlier hook during that window died on instruction
// fetch. The fix gives every trampoline its own page, so the check is simply:
// no two trampolines may share one.
//
// Deliberately NOT a live-concurrency test. The obvious version — hammer one
// hook from threads while installing more — cannot be written inside a single
// binary, because hk_write patches a target by flipping its page to
// read-write, which drops EXECUTE for the duration. Any code sharing a page
// with a patch target therefore faults if it runs during the install, patched
// or not. That is the documented load-time rule, not the bug under test, and
// it fires first: the hammer dies inside its own replacement function merely
// for sharing a page with the targets being installed. Separating them needs
// layout control that neither __attribute__((aligned(16384))) nor a >16KB pad
// function delivers — the alignment gets the binary SIGKILLed by AMFI (a
// __text alignment of 2^14 pushes the section off the start of __TEXT, and it
// dies before main with no crash report), and the linker hoists the pad to
// the front of __text regardless of source order. A second dylib would work;
// it is not worth it, because the property that makes the crash impossible is
// exactly what the page check below asserts, deterministically and with no
// timing dependence.
#define STRESS_TARGETS(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7) \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15)

#define STRESS_COUNT 16
#define STRESS_PAGE  16384
#define STRESS_PAGE_OF(p) ((uintptr_t)(p) & ~(uintptr_t)(STRESS_PAGE - 1))

static int (*stressOriginal[STRESS_COUNT])(int);
static int stressHits[STRESS_COUNT];

#define STRESS_DEFINE(n)                                                    \
    __attribute__((noinline)) static int stressTarget##n(int x) {           \
        int result = (n);                                                   \
        for(int i = 0; i < x; i++) result += i + (n);                       \
        return result;                                                      \
    }                                                                       \
    __attribute__((noinline)) static int stressReplacement##n(int x) {      \
        stressHits[n]++;                                                    \
        return stressOriginal[n](x) + 1000;                                 \
    }

STRESS_TARGETS(STRESS_DEFINE)

#define STRESS_ENTRY(n) { (void *)stressTarget##n, (void *)stressReplacement##n },

static const struct { void *target; void *replacement; } stressPairs[] = {
    STRESS_TARGETS(STRESS_ENTRY)
};

// Typed handles for verification, so the check calls through a real function
// pointer rather than round-tripping one through void *.
#define STRESS_CALLABLE(n) stressTarget##n,

static int (*const stressCallable[])(int) = { STRESS_TARGETS(STRESS_CALLABLE) };

// stressTargetN(4) == N + sum(i + N, i = 0..3) == 5N + 6; hooked, +1000.
static int stressWant(int n) {
    return 5 * n + 6 + 1000;
}

// No two trampolines on one page. Under the old shared arena all 16 landed on
// the same page and this returned NO.
static BOOL stressDistinctPages(int installed) {
    for(int i = 0; i < installed; i++) {
        if(!stressOriginal[i]) {
            return NO;
        }

        for(int j = i + 1; j < installed; j++) {
            if(STRESS_PAGE_OF((void *)stressOriginal[i])
               == STRESS_PAGE_OF((void *)stressOriginal[j])) {
                return NO;
            }
        }
    }

    return YES;
}

__attribute__((noinline)) static int dobbyTarget(int x) {
    int result = 0;
    for(int i = 1; i <= x; i++) result += i;
    return result;
}

__attribute__((noinline)) static int nativeTarget(int x) {
    int result = 0;
    for(int i = 0; i < x; i++) result += i * 3;
    return result;
}

__attribute__((noinline)) static int dobbyReplacement(int x) {
    dobbyHits++;
    return dobbyOriginal(x) + 100;
}

__attribute__((noinline)) static int nativeReplacement(int x) {
    nativeHits++;
    return nativeOriginal(x) + 200;
}

@interface DeviceSmokeProbe : NSObject
- (NSInteger)value;
@end

@implementation DeviceSmokeProbe
- (NSInteger)value { return 7; }
@end

static NSInteger messageReplacement(id self, SEL selector) {
    return messageOriginal(self, selector) + 3;
}

int main(void) {
    @autoreleasepool {
        setbuf(stdout, NULL);
        printf("HookKit device smoke starting\n");
        int failures = 0;
        volatile int argument = 10;

        HKSubstitutor *dobby = [HKSubstitutor substitutorWithTypes:HK_LIB_DOBBY];
        hookkit_status_t status = [dobby hookFunction:(void *)dobbyTarget
            withReplacement:(void *)dobbyReplacement outOldPtr:(void **)&dobbyOriginal];
        int dobbyValue = dobbyTarget(argument);
        BOOL dobbyOK = dobby.activeType == HK_LIB_DOBBY && status == HK_OK &&
            dobbyOriginal && dobbyHits == 1 && dobbyValue == 155 && dobbyOriginal(argument) == 55;
        printf("Dobby: %s status=%d original=%p hits=%d value=%d\n",
            dobbyOK ? "PASS" : "FAIL", status, dobbyOriginal, dobbyHits, dobbyValue);
        failures += !dobbyOK;

        HKSubstitutor *native = [HKSubstitutor substitutorWithTypes:HK_LIB_NATIVE];
        status = [native hookFunction:(void *)nativeTarget
            withReplacement:(void *)nativeReplacement outOldPtr:(void **)&nativeOriginal];
        int nativeValue = nativeTarget(argument);
        BOOL nativeOK = native.activeType == HK_LIB_NATIVE && status == HK_OK &&
            nativeOriginal && nativeHits == 1 && nativeValue == 335 && nativeOriginal(argument) == 135;
        printf("Native: %s status=%d original=%p hits=%d value=%d\n",
            nativeOK ? "PASS" : "FAIL", status, nativeOriginal, nativeHits, nativeValue);
        failures += !nativeOK;

        HKSubstitutor *message = [HKSubstitutor substitutorWithCategory:HK_CAT_MESSAGE];
        status = [message hookMessageInClass:DeviceSmokeProbe.class withSelector:@selector(value)
            withReplacement:(void *)messageReplacement outOldPtr:(void **)&messageOriginal];
        NSInteger messageValue = [[DeviceSmokeProbe new] value];
        BOOL messageOK = status == HK_OK && messageOriginal && messageValue == 10;
        printf("Message: %s status=%d original=%p value=%ld\n",
            messageOK ? "PASS" : "FAIL", status, messageOriginal, (long)messageValue);
        failures += !messageOK;

        void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_ANON | MAP_PRIVATE, -1, 0);
        uint32_t before = 0xD65F03C0;
        uint32_t after = 0xD503201F;
        BOOL memoryOK = page != MAP_FAILED;
        if(memoryOK) {
            memcpy(page, &before, sizeof(before));
            status = [dobby hookMemory:page withData:&after size:sizeof(after)];
            uint32_t actual = 0;
            memcpy(&actual, page, sizeof(actual));
            memoryOK = status == HK_OK && actual == after;
            munmap(page, 4096);
        }
        printf("Memory: %s\n", memoryOK ? "PASS" : "FAIL");
        failures += !memoryOK;

        // Entry patch shape: with a trampoline page placed within +/-128MB of
        // the target, the entry is a single B (0b000101 in the top 6 bits),
        // which is one aligned store and so atomic against a thread entering
        // the function mid-install. A 16-byte LDR/BR sequence here means the
        // near-page search fell back — correct, but torn-visible, so it is
        // reported rather than counted as a failure.
        uint32_t entry = 0;
        memcpy(&entry, (const void *)nativeTarget, sizeof(entry));
        printf("Entry patch: %s insn=0x%08x\n",
            (entry & 0xFC000000u) == 0x14000000u ? "atomic B" : "16-byte fallback", entry);

        HKSubstitutor *stress = [HKSubstitutor substitutorWithTypes:HK_LIB_NATIVE];
        int installed = 0;
        BOOL stressOK = YES;

        for(int i = 0; i < STRESS_COUNT; i++) {
            if([stress hookFunction:stressPairs[i].target
                withReplacement:stressPairs[i].replacement
                outOldPtr:(void **)&stressOriginal[i]] != HK_OK) {
                stressOK = NO;
                break;
            }

            installed++;
        }

        // Each hook must work, and each must still work after the ones that
        // followed it were installed — the sequential half of what the old
        // shared arena broke.
        for(int i = 0; stressOK && i < installed; i++) {
            if(stressCallable[i](4) != stressWant(i) || stressHits[i] != 1) {
                printf("Multi-hook: hook %d wrong (got %d want %d hits %d)\n",
                    i, stressCallable[i](4), stressWant(i), stressHits[i]);
                stressOK = NO;
            }
        }

        printf("Multi-hook: %s installed=%d/%d\n",
            stressOK ? "PASS" : "FAIL", installed, STRESS_COUNT);
        failures += !stressOK;

        BOOL pagesOK = installed > 1 && stressDistinctPages(installed);
        printf("Trampoline pages: %s (%d trampolines, %s)\n",
            pagesOK ? "PASS" : "FAIL", installed,
            pagesOK ? "all on distinct pages" : "SHARING pages — the old arena is back");
        failures += !pagesOK;

        // Fail-soft on dyld-cache trap stubs + fast NULL-image lookup. The
        // dyld shared cache stubs dyld's private APIs (dyld_image_get_installname
        // and friends) with an entry instruction that raises SIGTRAP, so
        // hooking one must return HK_ERR instead of crashing, and resolving
        // it must not walk every loaded image (the old per-image scan took
        // seconds per symbol). Both are version-dependent: on an iOS without
        // the stub the symbol may not resolve at all, in which case the
        // checks are skipped, not failed.
        HKSubstitutor *dyld = [HKSubstitutor defaultSubstitutor];
        uint64_t lookupStart = mach_absolute_time();
        void *dyldSym = [dyld findSymbolInImage:NULL symbolName:@"dyld_image_get_installname"];
        uint64_t lookupNanos = mach_absolute_time() - lookupStart;
        printf("Dyld lookup: %s addr=%p took %.1fms\n",
            dyldSym ? "resolved" : "unresolved (skipping)", dyldSym, lookupNanos / 1000000.0);

        if(dyldSym) {
            // The fast path must be fast: dlsym (exported) or the cache table
            // scan (private) is milliseconds, not the old seconds-per-symbol
            // per-image walk. 1s is a generous ceiling that still catches the
            // walk regressing.
            BOOL lookupOK = lookupNanos < 1000000000ull;
            printf("Dyld lookup speed: %s\n", lookupOK ? "PASS" : "FAIL");
            failures += !lookupOK;

            // hookFunction: on a trap stub must fail-soft (HK_ERR), never
            // SIGTRAP. The guard reads the entry instruction before any
            // backend is dispatched, so this call must simply return.
            // Version-agnostic: on an iOS where the symbol is REAL code (not
            // a stub), HK_OK is the correct outcome — only a trap entry is
            // required to fail. Decode the entry ourselves so the assertion
            // matches the actual target shape on this iOS.
            void *stubOriginal = NULL;
            status = [dyld hookFunction:dyldSym
                withReplacement:(void *)dyldStubReplacement
                outOldPtr:(void **)&stubOriginal];
            uint32_t entry = 0;
            memcpy(&entry, dyldSym, sizeof(entry));
            BOOL entryIsTrap = ((entry & 0xFFE0001Fu) == 0xD4200000u)    // BRK
                || ((entry & 0xFFE0001Fu) == 0xD4400000u)                // HLT
                || ((entry & 0xFFFF0000u) == 0u);                        // UDF
            BOOL failSoftOK = entryIsTrap
                ? (status == HK_ERR || status == HK_ERR_NOT_SUPPORTED) && stubOriginal == NULL
                : (status == HK_OK || status == HK_ERR || status == HK_ERR_NOT_SUPPORTED);
            printf("Dyld trap stub: %s status=%d original=%p entry=0x%08x (%s)\n",
                failSoftOK ? "PASS" : "FAIL", status, stubOriginal, entry,
                entryIsTrap ? "trap" : "real code");
            failures += !failSoftOK;
        }

        printf("HOOKKIT DEVICE SMOKE: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
}
