// Opt-in crash reporting for hosts that load HookKit into another process.
//
// HookKit never installs crash handlers on its own: nothing in the framework
// calls +install (not +load, not HKSubstitutor init). A host that wants crash
// reports calls [HKCrashHandler install] once, at startup. Reports go to
// <NSTemporaryDirectory()>/HookKit-crash.log and stderr.
//
// The handlers are for REPORTING, not recovery: exceptions are forwarded to
// the previously-installed handler (the runtime's default termination applies
// when there was none), and fatal signals are re-raised with their default
// disposition so the process still dies and the host's safe-mode machinery
// still triggers. Pre-existing exception/signal handlers are preserved and
// chain — ours logs first, then calls theirs. A previously-ignored fatal
// signal is still re-raised with default disposition: nothing in the fatal
// set is ever swallowed.
//
// Currently internal to the framework: the file lives under Internal/ and the
// class is deliberately absent from scripts/export-HookKit.list, so hosts
// cannot call it from outside the framework until it is promoted to
// Headers/HookKit/ and its symbols are added to the export list.
#ifndef hookkit_crash_handler_h
#define hookkit_crash_handler_h

#import <Foundation/Foundation.h>

@interface HKCrashHandler : NSObject

// Installs uncaught-exception and fatal-signal (SIGSEGV/SIGBUS/SIGABRT/
// SIGILL/SIGFPE/SIGTRAP) reporting handlers, chaining to whatever was
// installed before. Idempotent; safe to call more than once.
+ (void)install;

// Restores the exception and signal handlers that were in place before
// +install.
+ (void)uninstall;

@end

#endif
