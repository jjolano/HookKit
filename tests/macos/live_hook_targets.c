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
// two tests patching over each other. Deliberately shaped so no compiler
// folds it into the terminal target: distinct arithmetic chain AND distinct
// final scaling (multiply vs pure adds), so identical-code folding has two
// differences to defeat, not one. If a toolchain ever folds them anyway,
// test_reloc_inline_hook's pre-hook value assert catches it immediately.
__attribute__((visibility("default"), noinline))
int hk_live_reloc_target(int value) {
    volatile int result = value;
    result += 11;
    result *= 3;
    result += 13;
    result -= 4;
    return result;
}
