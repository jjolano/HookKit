// bench_common.h — minimal timing + stats for HookKit benches.
// ponytail: stdlib clock_gettime / mach_absolute_time only, no dep.
#ifndef HK_BENCH_COMMON_H
#define HK_BENCH_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#include <math.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

static inline uint64_t hk_bench_now_ns(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb = {0, 0};
    static int inited = 0;
    if (!inited) {
        mach_timebase_info(&tb);
        inited = 1;
    }
    uint64_t t = mach_absolute_time();
    return t * tb.numer / tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline int hk_bench_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    const char *name;
    const char *unit;
    uint64_t *samples; // ns per op or ns total
    size_t count;
    size_t capacity;
} hk_bench_series_t;

static inline void hk_bench_series_init(hk_bench_series_t *s, const char *name, const char *unit) {
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->unit = unit;
}

static inline void hk_bench_series_push(hk_bench_series_t *s, uint64_t v) {
    if (s->count == s->capacity) {
        size_t nc = s->capacity ? s->capacity * 2 : 64;
        uint64_t *n = (uint64_t *)realloc(s->samples, nc * sizeof(uint64_t));
        if (!n) abort();
        s->samples = n;
        s->capacity = nc;
    }
    s->samples[s->count++] = v;
}

static inline void hk_bench_series_free(hk_bench_series_t *s) {
    free(s->samples);
    s->samples = NULL;
    s->count = s->capacity = 0;
}

typedef struct {
    double mean;
    double median;
    double p95;
    double p99;
    uint64_t min;
    uint64_t max;
    double stddev;
} hk_bench_stats_t;

static inline hk_bench_stats_t hk_bench_compute(hk_bench_series_t *s) {
    hk_bench_stats_t st = {0};
    if (s->count == 0) return st;
    qsort(s->samples, s->count, sizeof(uint64_t), hk_bench_cmp_u64);
    st.min = s->samples[0];
    st.max = s->samples[s->count - 1];
    st.median = (double)s->samples[s->count / 2];
    size_t i95 = (size_t)(s->count * 0.95);
    if (i95 >= s->count) i95 = s->count - 1;
    size_t i99 = (size_t)(s->count * 0.99);
    if (i99 >= s->count) i99 = s->count - 1;
    st.p95 = (double)s->samples[i95];
    st.p99 = (double)s->samples[i99];
    double sum = 0;
    for (size_t i = 0; i < s->count; i++) sum += (double)s->samples[i];
    st.mean = sum / (double)s->count;
    double var = 0;
    for (size_t i = 0; i < s->count; i++) {
        double d = (double)s->samples[i] - st.mean;
        var += d * d;
    }
    st.stddev = sqrt(var / (double)s->count);
    return st;
}

static inline void hk_bench_print(const char *bench, hk_bench_series_t *s) {
    hk_bench_stats_t st = hk_bench_compute(s);
    // human + json line
    printf("%-28s %7zu samples  mean %8.0f  median %8.0f  p95 %8.0f  p99 %8.0f  min %8llu  max %8llu  stddev %6.0f  %s\n",
           bench, s->count, st.mean, st.median, st.p95, st.p99,
           (unsigned long long)st.min, (unsigned long long)st.max, st.stddev,
           s->unit);
    printf("{\"bench\":\"%s\",\"count\":%zu,\"mean\":%.1f,\"median\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"min\":%llu,\"max\":%llu,\"stddev\":%.1f,\"unit\":\"%s\"}\n",
           bench, s->count, st.mean, st.median, st.p95, st.p99,
           (unsigned long long)st.min, (unsigned long long)st.max, st.stddev,
           s->unit);
}

// Simple warmup + measure helper: fn called iters times, each iteration timed as ns/op if per_op else ns total.
typedef void (*hk_bench_fn)(void *ctx);

static inline void hk_bench_run(const char *name, hk_bench_fn fn, void *ctx, size_t warmup, size_t iters, int per_op_divisor) {
    for (size_t i = 0; i < warmup; i++) fn(ctx);
    hk_bench_series_t s;
    hk_bench_series_init(&s, name, per_op_divisor > 1 ? "ns/op" : "ns");
    for (size_t i = 0; i < iters; i++) {
        uint64_t t0 = hk_bench_now_ns();
        fn(ctx);
        uint64_t t1 = hk_bench_now_ns();
        uint64_t dt = t1 - t0;
        if (per_op_divisor > 1) dt /= (uint64_t)per_op_divisor;
        hk_bench_series_push(&s, dt);
    }
    hk_bench_print(name, &s);
    hk_bench_series_free(&s);
}

#endif
