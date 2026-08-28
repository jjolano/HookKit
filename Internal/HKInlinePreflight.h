// Shared ARM64 prologue preflight for the inline backends. Two layers:
//
// hk_inline_preflight_basic — the backend-INDEPENDENT checks: PAC strip,
// 4-byte alignment, self-hook rejection, readable+executable replacement and
// target entry. Every inline-capable dispatch runs these before any engine is
// reached (see hk_shared_inline_preflight_ok), so a misaligned, self-hooked,
// unmapped or non-executable target is refused side-effect-free on EVERY
// inline backend, whatever its relocator.
//
// hk_inline_preflight — the basic checks PLUS the fixed-window scans (early
// terminator over window-4, ADR/LDR-literal over window) that only the
// fixed-window relocators need. Dobby and litehook overwrite a fixed window
// of the target's prologue (Dobby 16 bytes, litehook 20 bytes), and both
// refuse — before any write — a target whose window would be unsafe to
// clobber. Their hook paths and public preflightFunction: routes call the
// same validator with the same overwrite size, so preflight agrees exactly
// with execution. The strong engines (ElleKit, Substrate, Substitute, Frida)
// bring their own production relocators, which decide instruction eligibility
// themselves: they are NOT window-gated, so their dispatches run the basic
// checks only.
//
// The validator is side-effect-free: it reads only the overwrite window and
// never writes, so a reject leaves the target untouched. It also owns pointer
// canonicalization (PAC strip on arm64e) before inspecting, so the hook paths
// and preflight inspect the same raw address.
#ifndef hookkit_inline_preflight_h
#define hookkit_inline_preflight_h

#import <HookKit/HookKit.h>

#include <stddef.h>
#include <stdint.h>

// Overwrite windows, in bytes: Dobby's relocator takes 16 bytes, litehook's
// trampoline emits 5 instructions (4x MOVK + BR) = 20 bytes.
#define HK_INLINE_PREFLIGHT_DOBBY_WINDOW    16
#define HK_INLINE_PREFLIGHT_LITEHOOK_WINDOW 20

// Validates `function`/`replacement` for an inline overwrite on any backend.
// Returns HK_STATUS_OK when the target may be handed to the engine's own
// relocator; otherwise HK_STATUS_UNAVAILABLE with *outErrno (may be NULL) set to the
// reason (EINVAL: misaligned target or self-hook; EFAULT: replacement or
// target entry unmapped or non-executable).
hk_status_t hk_inline_preflight_basic(void *function, void *replacement, int *outErrno);

// Validates `function`/`replacement` for an inline overwrite of `window`
// bytes: hk_inline_preflight_basic plus the fixed-window scans. Returns HK_STATUS_OK
// when the prologue can be overwritten safely; otherwise
// HK_STATUS_UNAVAILABLE with *outErrno (may be NULL) set to the reason
// (EINVAL: misaligned target or self-hook; EFAULT: replacement or target
// unmapped or non-executable; EOPNOTSUPP: the function ends inside the window
// or a literal load / ADR(ADRP) sits in it).
hk_status_t hk_inline_preflight(void *function, void *replacement, size_t window, int *outErrno);

// True when the target's entry instruction is an unconditional trap
// (BRK/HLT/UDF on arm64; dyld's shared-cache private-API stubs such as
// dyld_image_get_installname are trap stubs). Such a target can never be
// hooked meaningfully — the "original" would be the trap itself — and some
// vendors raise SIGTRAP while attempting it, so the facade refuses it with
// HK_ERR before any backend is dispatched. Side-effect-free: reads only the
// entry instruction. Always false on non-arm64 (no AArch64 decode exists).
bool hk_inline_target_is_trap_stub(void *function);

#endif
