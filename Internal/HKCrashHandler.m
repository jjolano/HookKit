// Fatal-error reporting: logs uncaught exceptions and fatal signals to
// <NSTemporaryDirectory()>/HookKit-crash.log plus stderr, then chains to
// whatever handlers were installed before ours. See HKCrashHandler.h.
#import "HKCrashHandler.h"

#import <execinfo.h>
#import <fcntl.h>
#import <mach/mach.h>
#import <pthread.h>
#import <signal.h>
#import <stdatomic.h>
#import <sys/stat.h>
#import <time.h>
#import <unistd.h>

// Fatal signals we report. Deliberately a fixed set: no SIGPIPE/SIGUSR*
// (hosts commonly manage those themselves), no SIGKILL/SIGSTOP (uncatchable).
static const int kHKFatalSignals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP };
#define HK_FATAL_SIGNAL_COUNT (sizeof(kHKFatalSignals) / sizeof(kHKFatalSignals[0]))

// Signal names for the fixed set above; strsignal is not async-signal-safe.
static const char *kHKSignalNames[] = { "SIGSEGV", "SIGBUS", "SIGABRT", "SIGILL", "SIGFPE", "SIGTRAP" };

static NSUncaughtExceptionHandler *gHKPrevExceptionHandler;
static struct sigaction gHKPrevSigActions[NSIG];

// Log fd: opened once at install and never closed (also not by +uninstall),
// so every write path sees a valid fd and no close race exists.
static int gHKLogFd = -1;

// Guards +install/+uninstall state transitions. Normal (non-signal) context.
static pthread_mutex_t gHKStateLock = PTHREAD_MUTEX_INITIALIZER;
static BOOL gHKInstalled;

// Spinlock serializing multi-write log records. The signal path try-locks:
// each write() to an O_APPEND fd is atomic, so a contended handler falls back
// to unlocked single writes rather than spinning (worst case: two records
// interleave at line granularity).
static atomic_flag gHKLogLock = ATOMIC_FLAG_INIT;

static void HKLogWrite(const char *bytes, size_t length) {
    if(gHKLogFd >= 0) {
        write(gHKLogFd, bytes, length);
    }

    write(STDERR_FILENO, bytes, length);
}

static const char *HKSignalName(int sig) {
    for(size_t i = 0; i < HK_FATAL_SIGNAL_COUNT; i++) {
        if(kHKFatalSignals[i] == sig) {
            return kHKSignalNames[i];
        }
    }

    return "?";
}

// Runs the handler that was installed for `sig` before ours, if there was
// one. SIG_DFL/SIG_IGN are skipped — re-raising with the default disposition
// below still happens, so a fatal signal is never swallowed.
static void HKChainPreviousSignalHandler(int sig, siginfo_t *info, void *ucontext) {
    struct sigaction *previous = &gHKPrevSigActions[sig];

    if(previous->sa_flags & SA_SIGINFO) {
        if(previous->sa_sigaction != (void (*)(int, siginfo_t *, void *))SIG_DFL &&
           previous->sa_sigaction != (void (*)(int, siginfo_t *, void *))SIG_IGN) {
            previous->sa_sigaction(sig, info, ucontext);
        }
    } else if(previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN) {
        previous->sa_handler(sig);
    }
}

// Signal path: async-signal-safe only — no ObjC, no malloc, no locks held
// across writes. write/snprintf/time/mach_thread_self/backtrace only.
static void HKHandleSignal(int sig, siginfo_t *info, void *ucontext) {
    const int locked = !atomic_flag_test_and_set(&gHKLogLock);

    char header[512];
    int headerLength = snprintf(header, sizeof(header),
        "\n[%lld] HookKit crash: %s (%d), faulting address %p, pid %d, thread 0x%x\n",
        (long long)time(NULL), HKSignalName(sig), sig,
        info ? info->si_addr : NULL, getpid(), (unsigned)mach_thread_self());
    if(headerLength > 0) {
        HKLogWrite(header, (size_t)headerLength);
    }

    void *frames[64];
    const int frameCount = backtrace(frames, 64);
    if(gHKLogFd >= 0) {
        backtrace_symbols_fd(frames, frameCount, gHKLogFd);
    }

    backtrace_symbols_fd(frames, frameCount, STDERR_FILENO);

    if(locked) {
        atomic_flag_clear(&gHKLogLock);
    }

    // Chain, then die: restore the default disposition and re-raise so the
    // process terminates and the host's safe-mode machinery triggers. Never
    // return to the faulting code.
    HKChainPreviousSignalHandler(sig, info, ucontext);
    signal(sig, SIG_DFL);
    raise(sig);
}

// Exception path: normal context, full Foundation allowed. An uncaught
// exception is not a signal, so no async-signal-safety constraints apply.
static void HKHandleException(NSException *exception) {
    NSMutableString *report = [NSMutableString string];
    [report appendFormat:@"\n[%@] HookKit crash: uncaught exception %@\nreason: %@\nuserInfo: %@\n",
        [NSDate date], exception.name, exception.reason, exception.userInfo];
    [report appendFormat:@"thread: %@\ncall stack:\n%@\n",
        NSThread.currentThread, [exception.callStackSymbols componentsJoinedByString:@"\n"]];

    const char *utf8 = report.UTF8String;
    const size_t length = strlen(utf8);

    // Blocking spin is fine here (normal context); the signal path try-locks,
    // so this can never deadlock against a handler on the same thread.
    while(atomic_flag_test_and_set(&gHKLogLock)) {
    }

    HKLogWrite(utf8, length);
    atomic_flag_clear(&gHKLogLock);

    // Chain to whatever was installed before us; when there was none, return
    // and let the runtime terminate with its default uncaught-exception
    // handling. Never swallow.
    if(gHKPrevExceptionHandler) {
        gHKPrevExceptionHandler(exception);
    }
}

@implementation HKCrashHandler

+ (void)install {
    pthread_mutex_lock(&gHKStateLock);

    if(gHKInstalled) {
        pthread_mutex_unlock(&gHKStateLock);
        return;
    }

    gHKInstalled = YES;
    pthread_mutex_unlock(&gHKStateLock);

    NSString *logPath = [NSTemporaryDirectory() stringByAppendingPathComponent:@"HookKit-crash.log"];
    gHKLogFd = open(logPath.UTF8String, O_WRONLY | O_CREAT | O_APPEND, 0644);

    gHKPrevExceptionHandler = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(&HKHandleException);

    struct sigaction action;
    action.sa_sigaction = HKHandleSignal;
    sigemptyset(&action.sa_mask);
    // SA_RESETHAND: the disposition drops to SIG_DFL at handler entry, so a
    // re-firing signal on another thread dies immediately instead of
    // re-entering our handler; the explicit signal()/raise() in the handler
    // guarantees death for the crashing thread itself.
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;

    for(size_t i = 0; i < HK_FATAL_SIGNAL_COUNT; i++) {
        sigaction(kHKFatalSignals[i], &action, &gHKPrevSigActions[kHKFatalSignals[i]]);
    }
}

+ (void)uninstall {
    pthread_mutex_lock(&gHKStateLock);

    if(!gHKInstalled) {
        pthread_mutex_unlock(&gHKStateLock);
        return;
    }

    gHKInstalled = NO;
    pthread_mutex_unlock(&gHKStateLock);

    NSSetUncaughtExceptionHandler(gHKPrevExceptionHandler);

    for(size_t i = 0; i < HK_FATAL_SIGNAL_COUNT; i++) {
        sigaction(kHKFatalSignals[i], &gHKPrevSigActions[kHKFatalSignals[i]], NULL);
    }

    // Log fd deliberately stays open: install is the only opener, uninstall
    // only stops interception, and a close race with a handler in flight is
    // worse than one leaked descriptor.
}

@end
