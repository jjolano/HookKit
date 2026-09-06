// Keep live patch targets in a separate image from the test runner. HookKit
// temporarily removes execute permission from a target page while writing it;
// sharing that page with the installer would make the test fault by design.

__attribute__((visibility("default"), noinline))
int hk_live_terminal_target(int value) {
    volatile int result = value;
    result += 5;
    result += 6;
    result += 7;
    result += 8;
    return result;
}

// Separate target for the relocating engine: its entry patch displaces the
// prologue into a trampoline, so sharing the terminal target would leave the
// two tests patching over each other. Arithmetic-only on purpose: the
// relocator copies position-independent ALU/add-sub-immediate verbatim, so an
// MUL or shift risks tripping an unrelocatable refusal (or a code path the
// host suite never covers). Distinct from the terminal target by constants
// AND a final scaling step; identical-code folding still has to defeat both.
// The test asserts the computed baseline, not a magic number, so a future
// fold is caught as a value mismatch rather than a stale constant.
__attribute__((visibility("default"), noinline))
int hk_live_reloc_target(int value) {
    volatile int result = value;
    result += 11;
    result += result;
    result += 13;
    result -= 4;
    return result;
}
