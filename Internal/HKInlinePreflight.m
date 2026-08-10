// Shared fixed-window ARM64 prologue validator for the Dobby and litehook
// inline backends — see HKInlinePreflight.h. Both backends call this from
// their hook paths and their public preflightFunction: routes with the same
// overwrite size, so preflight agrees exactly with execution.
#import "HKInlinePreflight.h"

#include <errno.h>

#if __has_include(<ptrauth.h>)
#import <ptrauth.h>
#endif

#import "native/hk_arm64.h"
#import "native/hk_native.h"

hookkit_status_t hk_inline_preflight(void *function, void *replacement, size_t window, int *outErrno) {
    // Fail closed before the vendor hook: Dobby's relocator neither rejects
    // short functions (it reads its overwrite window without recognizing
    // early exits, smashing whatever follows) nor handles literal loads (it
    // UNIMPLEMENTED()s on some LDR-literal encodings and mishandles SIMD
    // literal loads); litehook copies the window to the trampoline verbatim,
    // smashing the pool or address the instruction points at. The checks
    // below read only the window and never write, so a reject leaves the
    // target untouched.
    if(outErrno) {
        *outErrno = 0;
    }

#if __has_feature(ptrauth_calls)
    // Strip PAC so the raw address is what the relocator will inspect
    // (arm64e). Replacement is stripped too so the self-hook check below
    // compares raw addresses.
    function = ptrauth_strip(function, ptrauth_key_asia);
    replacement = ptrauth_strip(replacement, ptrauth_key_asia);
#endif

    if(((uintptr_t)function & 0x3) != 0 || function == replacement) {
        if(outErrno) {
            *outErrno = EINVAL;
        }

        return HK_ERR_NOT_SUPPORTED;
    }

#if defined(__arm64__) || defined(__aarch64__)
    // The window scan below dereferences the prologue; a bogus non-NULL
    // address must fail cleanly instead of faulting. (The Mach VM probe is
    // arm64-only, and so are the inline backends that use this preflight.)
    if(!hk_native_range_readable(function, window)) {
        if(outErrno) {
            *outErrno = EFAULT;
        }

        return HK_ERR_NOT_SUPPORTED;
    }
#endif

    if(hk_arm64_has_early_terminator(function, window)) {
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
}
