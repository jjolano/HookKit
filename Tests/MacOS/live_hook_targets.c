// Keep live patch targets in a separate image from the test runner. HookKit
// temporarily removes execute permission from a target page while writing it;
// sharing that page with the installer would make the test fault by design.

__attribute__((visibility("default"), noinline))
int hk_live_native_target(int value) {
    volatile int result = value;
    result += 1;
    result += 2;
    result += 3;
    result += 4;
    return result;
}

__attribute__((visibility("default"), noinline))
int hk_live_terminal_target(int value) {
    volatile int result = value;
    result += 5;
    result += 6;
    result += 7;
    result += 8;
    return result;
}
