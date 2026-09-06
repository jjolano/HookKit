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
// two tests patching over each other. Same shape (all-relocatable NOP-like
// body plus arithmetic), distinct symbol.
__attribute__((visibility("default"), noinline))
int hk_live_reloc_target(int value) {
    volatile int result = value;
    result += 11;
    result += 12;
    result += 13;
    result += 14;
    return result;
}
