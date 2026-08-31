// bench_reloc.c — hk_arm64 relocator + branch emission (host)
// ponytail: no device alloc, pure arithmetic.
#include "bench_common.h"
#include "../../src/native/hk_arm64.h"
#include <string.h>
#include <stdint.h>

// typical prologue: stp x29,x30,[sp,#-16]! ; mov x29,sp ; adrp x0,_ + ldr etc.
static uint32_t g_prologue[] = {
    0xa9bf7bfd, // stp x29,x30,[sp,#-16]!
    0x910003fd, // mov x29,sp
    0x90000008, // adrp x8, 0x10000
    0x94000004, // bl 0x10
    0xd65f03c0, // ret
};

static void bench_relocate(void *ctx){
    (void)ctx;
    uint32_t dst[64];
    (void)hk_arm64_relocate(g_prologue, 0x100000000ull, 4, dst, sizeof(dst));
}
static void bench_branch_near(void *ctx){
    (void)ctx;
    uint32_t buf[4];
    (void)hk_arm64_emit_branch(buf, 0x100000000ull, 0x100000100ull);
}
static void bench_branch_far(void *ctx){
    (void)ctx;
    uint32_t buf[4];
    (void)hk_arm64_emit_branch(buf, 0x100000000ull, 0x200000000ull);
}
static void bench_has_terminator(void *ctx){
    (void)ctx;
    (void)hk_arm64_has_early_terminator(g_prologue, sizeof(g_prologue));
}
static void bench_has_literal(void *ctx){
    (void)ctx;
    (void)hk_arm64_has_aarch64_literal_load(g_prologue, sizeof(g_prologue));
}

int main(int argc, char **argv){
    size_t iters=200000;
    size_t warmup=500;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--iters")==0 && i+1<argc) iters=(size_t)atoi(argv[++i]);
        if(strcmp(argv[i],"--warmup")==0 && i+1<argc) warmup=(size_t)atoi(argv[++i]);
    }
    hk_bench_run("relocate_4", bench_relocate, NULL, warmup, iters, 1);
    hk_bench_run("branch_near_4B", bench_branch_near, NULL, warmup, iters, 1);
    hk_bench_run("branch_far_16B", bench_branch_far, NULL, warmup, iters, 1);
    hk_bench_run("has_terminator", bench_has_terminator, NULL, warmup, iters, 1);
    hk_bench_run("has_literal", bench_has_literal, NULL, warmup, iters, 1);
    return 0;
}
