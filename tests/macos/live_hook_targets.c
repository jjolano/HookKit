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
// two tests patching over each other. Deliberately LONG and branch-free:
// the relocating engine's safety scales with prologue length (a terminator
// or unrelocatable form anywhere in the patch window is fatal), so a stubby
// 5-instruction body leaves zero margin -- one load-bearing prologue
// instruction the relocator refuses (auth, literal, system) and prepare
// fails. Sixteen volatile adds give the window real content while staying
// in the verbatim-copy class the relocator never refuses. Distinct
// constants defeat identical-code folding; the test asserts the computed
// baseline, not a magic number, so a future fold reads as a value mismatch.
__attribute__((visibility("default"), noinline))
int hk_live_reloc_target(int value) {
    volatile int result = value;
    result += 11; result += 12; result += 13; result += 14;
    result += 15; result += 16; result += 17; result += 18;
    result += 19; result += 20; result += 21; result += 22;
    result += 23; result += 24; result += 25; result += 26;
    return result;
}
