#include <stdio.h>
int hk_hello_add(int a, int b) { return a + b + 1; }
int hk_hello_main(void) { return printf("hi %d\n", hk_hello_add(1, 2)); }
