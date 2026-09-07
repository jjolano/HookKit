// Compile in place of an existing host suite, using bench_footprint.py.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define main footprint_suite_main
#include HK_BENCH_SOURCE
#undef main

static size_t allocation_calls, requested_bytes, free_calls;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_aligned_alloc(size_t, size_t);
void __real_free(void *);

void *__wrap_malloc(size_t size) {
    allocation_calls++; requested_bytes += size;
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    allocation_calls++; requested_bytes += count * size;
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
    allocation_calls++; requested_bytes += size;
    return __real_realloc(ptr, size);
}
void *__wrap_aligned_alloc(size_t alignment, size_t size) {
    allocation_calls++; requested_bytes += size;
    return __real_aligned_alloc(alignment, size);
}
void __wrap_free(void *ptr) {
    if (ptr) free_calls++;
    __real_free(ptr);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char *end = NULL;
    unsigned long repetitions = strtoul(argv[1], &end, 10);
    assert(end && !*end && repetitions > 0 && repetitions <= 1000);
    assert(freopen("/dev/null", "w", stdout));
    struct timespec start, stop;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (unsigned long i = 0; i < repetitions; i++) {
        assert(footprint_suite_main() == 0);
    }
    assert(clock_gettime(CLOCK_MONOTONIC, &stop) == 0);
    double elapsed = (stop.tv_sec - start.tv_sec) * 1e6 +
                     (stop.tv_nsec - start.tv_nsec) / 1e3;
    fprintf(stderr, "%lu,%.3f,%zu,%zu,%zu\n", repetitions, elapsed,
            allocation_calls, requested_bytes, free_calls);
    return 0;
}
