// Shared ARM64 prologue preflight for the inline backends — see
// HKInlinePreflight.h. Two layers: hk_inline_preflight_basic carries the
// backend-INDEPENDENT checks that gate every inline-capable dispatch;
// hk_inline_preflight adds the fixed-window scans that only the Dobby and
// litehook relocators need. Both backends call the full validator from their
// hook paths and their public preflightFunction: routes with the same
// overwrite size, so preflight agrees exactly with execution.
#import "HKInlinePreflight.h"

#include <errno.h>
#include <pthread.h>

#if __has_include(<ptrauth.h>)
#import <ptrauth.h>
#endif

#import "native/hk_arm64.h"
#import "native/hk_native.h"

// Executable-range probe: not declared in hk_native.h, whose public contract
// is readable-only (the Swift metadata reader depends on that). The inline
// preflight inspects CODE, so it needs the stronger check.
HK_INTERNAL bool hk_native_range_executable(const void *addr, size_t len);

// Backend-INDEPENDENT inline preflight: the checks every inline-capable
// dispatch must pass before any engine is reached (see
// hk_shared_inline_preflight_ok), and the front of hk_inline_preflight for
// the fixed-window backends. The checks below read only the first instruction
// of each address and never write, so a reject leaves the target untouched.
hookkit_status_t hk_inline_preflight_basic(void *function, void *replacement, int *outErrno) {
    if(outErrno) {
        *outErrno = 0;
    }

#if defined(__arm64__) || defined(__aarch64__)
#if __has_feature(ptrauth_calls)
    // Strip PAC so the raw address is what the relocator will inspect
    // (arm64e). Replacement is stripped too so the self-hook check below
    // compares raw addresses.
    function = ptrauth_strip(function, ptrauth_key_asia);
    replacement = ptrauth_strip(replacement, ptrauth_key_asia);
#endif

    // Mirror of the target check: the replacement is branched to, so it must
    // sit on an instruction boundary too (AArch64 instructions are 4-byte
    // aligned).
    if(((uintptr_t)function & 0x3) != 0 || ((uintptr_t)replacement & 0x3) != 0 || function == replacement) {
        if(outErrno) {
            *outErrno = EINVAL;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    // The replacement is jumped to, never inspected: its mapping only needs to
    // be mapped and executable for the branch to land. 4 bytes covers the
    // branch-target instruction (the page-rounded probe spans whatever it
    // straddles); same EFAULT convention as the target range check.
    if(!hk_native_range_readable(replacement, 4)
       || !hk_native_range_executable(replacement, 4)) {
        if(outErrno) {
            *outErrno = EFAULT;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    // The target's entry instruction is inspected by the engine's relocator;
    // a bogus non-NULL address must fail cleanly instead of faulting. (The
    // Mach VM probe is arm64-only, and so are the inline backends that use
    // this preflight.) The mapping must be executable too: the entry is code
    // — a readable-only mapping (a data blob) is not a patchable function.
    // 4 bytes covers the entry instruction (the page-rounded probe spans
    // whatever it straddles); the fixed-window backends extend the probe over
    // their full overwrite window in hk_inline_preflight.
    if(!hk_native_range_readable(function, 4)
       || !hk_native_range_executable(function, 4)) {
        if(outErrno) {
            *outErrno = EFAULT;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    return HK_OK;
#else
    // Not arm64/arm64e: no AArch64 decoder is compiled in here. Pass through
    // — the MS providers (Substrate, Substitute) validate their own Thumb
    // prologues, and the fixed-window inline backends are arm64-only.
    (void)function;
    (void)replacement;
    return HK_OK;
#endif
}

hookkit_status_t hk_inline_preflight(void *function, void *replacement, size_t window, int *outErrno) {
    // Thread gate, first because it is the cheapest check. The two backends
    // behind this validator (Dobby, litehook) patch a fixed prologue window
    // with no atomicity and no peer-thread quiescing, so a hook installed
    // once other threads are running can catch one mid-prologue. The main
    // thread is the practical proxy for "still at load time" — the same
    // contract the native engine enforces (HKNativeBackends.m).
    //
    // Deliberately NOT in hk_inline_preflight_basic: that gates ElleKit,
    // Substrate, Substitute and Frida too, and those ship production
    // relocators the codebase already trusts to be reached without this
    // validator. Gating here instead means an off-main-thread inline hook
    // skips the weak engines and routes to a strong one, which is the right
    // fallback rather than a blanket refusal.
    if(!pthread_main_np()) {
        if(outErrno) {
            *outErrno = EPERM;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    // The fixed-window rules below apply on top of the generic checks: the
    // window scanners dereference the prologue, and the generic checks make
    // sure a bogus address fails cleanly first.
    hookkit_status_t status = hk_inline_preflight_basic(function, replacement, outErrno);

    if(status != HK_OK) {
        return status;
    }

#if defined(__arm64__) || defined(__aarch64__)
    // Fail closed before the vendor hook: Dobby's relocator neither rejects
    // short functions (it reads its overwrite window without recognizing
    // early exits, smashing whatever follows) nor handles literal loads (it
    // UNIMPLEMENTED()s on some LDR-literal encodings and mishandles SIMD
    // literal loads); litehook copies the window to the trampoline verbatim,
    // smashing the pool or address the instruction points at. The checks
    // below read only the window and never write, so a reject leaves the
    // target untouched. The strong engines (ElleKit, Substrate, Substitute,
    // Frida) have their own production relocators and are deliberately NOT
    // gated here — they dispatch through hk_inline_preflight_basic only.

    // The window scans below dereference the prologue over `window` bytes; a
    // bogus non-NULL address whose window straddles the end of a mapping must
    // fail cleanly instead of faulting. The mapping must be executable too:
    // the window is code — a readable-only mapping (a data blob) is not a
    // patchable prologue.
    if(!hk_native_range_readable(function, window)
       || !hk_native_range_executable(function, window)) {
        if(outErrno) {
            *outErrno = EFAULT;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    // A terminator in the final fully-overwritten instruction is not "early":
    // the last instruction is replaced wholesale by the branch, so only
    // terminators in the preceding window-4 bytes can end the function inside
    // the patch. (A window of 4 bytes or less is a single overwritten
    // instruction: nothing to refuse.)
    size_t scan = (window > 4) ? window - 4 : 0;

    if(scan && hk_arm64_has_early_terminator(function, scan)) {
        // Function ends inside the overwrite window: patching would smash a
        // neighbor's bytes.
        if(outErrno) {
            *outErrno = EOPNOTSUPP;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    if(hk_arm64_has_aarch64_literal_load(function, window)) {
        // Literal load / ADR(ADRP) in the overwrite window: the relocator
        // fatally mishandles these.
        if(outErrno) {
            *outErrno = EOPNOTSUPP;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

    return HK_OK;
#else
    // Not arm64/arm64e: no AArch64 decoder is compiled in here. Pass through
    // — the MS providers (Substrate, Substitute) validate their own Thumb
    // prologues, and the fixed-window inline backends are arm64-only.
    (void)window;
    return HK_OK;
#endif
}
