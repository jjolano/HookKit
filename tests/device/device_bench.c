// device_bench.c — on-device end-to-end + resolver + reloc + provider bench.
// Builds against the installed HookKit.framework; runs with real dyld images.
// ponytail: one binary, JSON lines to stdout, rm before scp per Makefile:539.

#include <HookKit/HookKit.h>
#include <HookKit/HookKitResolver.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <objc/runtime.h>
#include <os/signpost.h>
static os_log_t g_log;
static os_signpost_id_t g_spid;
#define SIGNPOST_BEGIN(name) os_signpost_interval_begin(g_log, g_spid, name)
#define SIGNPOST_END(name) os_signpost_interval_end(g_log, g_spid, name)
#else
#define SIGNPOST_BEGIN(name) do{}while(0)
#define SIGNPOST_END(name) do{}while(0)
#endif

static uint64_t now_ns(void){
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb={0,0};
    static int inited=0;
    if(!inited){ mach_timebase_info(&tb); inited=1; }
    uint64_t t=mach_absolute_time();
    return t * tb.numer / tb.denom;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
#endif
}
static int cmp_u64(const void *a,const void *b){
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;
    return (x>y)-(x<y);
}
static void print_stats(const char *name, uint64_t *v, size_t n, const char *unit){
    qsort(v,n,sizeof(uint64_t),cmp_u64);
    uint64_t mn=v[0], mx=v[n-1], med=v[n/2];
    size_t i95=n*95/100; if(i95>=n) i95=n-1;
    double sum=0; for(size_t i=0;i<n;i++) sum+=(double)v[i];
    double mean=sum/(double)n;
    double var=0; for(size_t i=0;i<n;i++){double d=(double)v[i]-mean; var+=d*d;}
    double sd=sqrt(var/(double)n);
    printf("%-28s %4zu  mean %8.0f median %8.0f p95 %8llu min %8llu max %8llu sd %6.0f %s\n",
           name,n,mean,(double)med,(unsigned long long)v[i95],
           (unsigned long long)mn,(unsigned long long)mx,sd,unit);
    printf("{\"bench\":\"%s\",\"count\":%zu,\"mean\":%.1f,\"median\":%.1f,\"p95\":%.1f,\"min\":%llu,\"max\":%llu,\"stddev\":%.1f,\"unit\":\"%s\"}\n",
           name,n,mean,(double)med,(double)v[i95],
           (unsigned long long)mn,(unsigned long long)mx,sd,unit);
    fflush(stdout);
}

// ---- large-scale plan lifecycle uses distinct, writable memory targets ----

static const uint8_t g_expected_byte = 0;
static const uint8_t g_replacement_byte = 1;

static hk_hook_spec_t memory_spec(const char *id, uint8_t *target){
    hk_hook_spec_t s; memset(&s,0,sizeof(s));
    s.struct_size=sizeof(s); s.struct_version=HK_ABI_VERSION_3_0;
    s.stable_hook_id=id;
    s.target_kind=HK_TARGET_MEMORY_PATCH;
    s.target.memory.struct_size=sizeof(s.target.memory);
    s.target.memory.struct_version=HK_ABI_VERSION_3_0;
    s.target.memory.address=(uintptr_t)target;
    s.target.memory.replacement_bytes.data=&g_replacement_byte;
    s.target.memory.replacement_bytes.size=1;
    s.target.memory.expected_bytes.data=&g_expected_byte;
    s.target.memory.expected_bytes.size=1;
    s.target.memory.size=1;
    s.target.memory.kind=HK_MEMORY_KIND_DATA;
    s.required_reach=HK_REACH_EXACT_MEMORY;
    return s;
}

static void run_memory_plan(int n, char ids[][16], uint64_t *out_total,
                            uint64_t *out_analyze, uint64_t *out_prepare,
                            uint64_t *out_commit){
    // ponytail: keep regions alive until exit; ownership is process-lifetime, so malloc
    // reuse in a later sample would turn a fresh benchmark target into a chain.
    uint8_t *targets=calloc((size_t)n,sizeof(*targets));
    hk_hook_t **hooks=calloc((size_t)n,sizeof(*hooks));
    assert(targets && hooks);

    uint64_t t0=now_ns();
    hk_runtime_t *rt=NULL;
    hk_plan_t *plan=NULL;
    assert(hk_runtime_create(NULL,&rt)==HK_STATUS_OK);
    assert(hk_plan_create(rt,NULL,&plan)==HK_STATUS_OK);
    for(int i=0;i<n;i++){
        hk_hook_spec_t s=memory_spec(ids[i],&targets[i]);
        assert(hk_plan_add_hook(plan,&s,&hooks[i])==HK_STATUS_OK);
    }

    hk_report_t *r=NULL;
    SIGNPOST_BEGIN("analyze");
    assert(hk_plan_analyze(plan,&r)==HK_STATUS_OK);
    SIGNPOST_END("analyze");
    uint64_t t1=now_ns();
    hk_report_release(r); r=NULL;
    SIGNPOST_BEGIN("prepare");
    assert(hk_plan_prepare(plan,&r)==HK_STATUS_OK);
    SIGNPOST_END("prepare");
    uint64_t t2=now_ns();
    hk_report_release(r); r=NULL;
    SIGNPOST_BEGIN("commit");
    assert(hk_plan_commit(plan,&r)==HK_STATUS_OK);
    SIGNPOST_END("commit");
    uint64_t t3=now_ns();
    hk_report_release(r);

    for(int i=0;i<n;i++){
        hk_hook_result_t result;
        assert(hk_hook_copy_result(hooks[i],&result)==HK_STATUS_OK);
        assert(result.outcome==HK_OUTCOME_ACTIVE);
    }
    free(hooks);
    hk_plan_release(plan);
    hk_runtime_release(rt);

    if(out_total) *out_total=t3-t0;
    if(out_analyze) *out_analyze=t1-t0;
    if(out_prepare) *out_prepare=t2-t1;
    if(out_commit) *out_commit=t3-t2;
}

static void bench_plan_e2e(int n, int iters, int warmup){
    char ids[1000][16];
    for(int i=0;i<n;i++) snprintf(ids[i],sizeof(ids[i]),"h%d",i);
    for(int w=0;w<warmup;w++) run_memory_plan(n,ids,NULL,NULL,NULL,NULL);
    uint64_t *total = malloc((size_t)iters*sizeof(uint64_t));
    uint64_t *analyze = malloc((size_t)iters*sizeof(uint64_t));
    uint64_t *prepare = malloc((size_t)iters*sizeof(uint64_t));
    uint64_t *commit = malloc((size_t)iters*sizeof(uint64_t));
    assert(total && analyze && prepare && commit);
    for(int i=0;i<iters;i++){
        run_memory_plan(n,ids,&total[i],&analyze[i],&prepare[i],&commit[i]);
    }
    char name[64];
    snprintf(name,sizeof(name),"device_e2e_total/%d",n); print_stats(name,total,(size_t)iters,"ns");
    snprintf(name,sizeof(name),"device_analyze/%d",n); print_stats(name,analyze,(size_t)iters,"ns");
    snprintf(name,sizeof(name),"device_prepare/%d",n); print_stats(name,prepare,(size_t)iters,"ns");
    snprintf(name,sizeof(name),"device_commit/%d",n); print_stats(name,commit,(size_t)iters,"ns");
    // also per-op
    for(int i=0;i<iters;i++){ analyze[i]/=(uint64_t)(n?n:1); prepare[i]/=(uint64_t)(n?n:1); commit[i]/=(uint64_t)(n?n:1); }
    snprintf(name,sizeof(name),"device_analyze_per/%d",n); print_stats(name,analyze,(size_t)iters,"ns/op");
    snprintf(name,sizeof(name),"device_prepare_per/%d",n); print_stats(name,prepare,(size_t)iters,"ns/op");
    free(total); free(analyze); free(prepare); free(commit);
}

static void bench_runtime_create(int iters){
    uint64_t *v=malloc((size_t)iters*sizeof(uint64_t));
    for(int i=0;i<iters;i++){
        uint64_t t0=now_ns();
        hk_runtime_t *rt=NULL; hk_runtime_create(NULL,&rt);
        uint64_t t1=now_ns();
        hk_runtime_release(rt);
        v[i]=t1-t0;
    }
    print_stats("device_runtime_create",v,(size_t)iters,"ns");
    free(v);
}



static bool enumerate_cb(void *ctx, hk_string_view_t id, hk_string_view_t name){
    (void)ctx; (void)id; (void)name; return true;
}
static void bench_enumerate_backends(int iters){
    hk_runtime_t *rt=NULL; hk_runtime_create(NULL,&rt);
    uint64_t *v=malloc((size_t)iters*sizeof(uint64_t));
    for(int i=0;i<iters;i++){
        uint64_t t0=now_ns();
        hk_runtime_enumerate_backends(rt, enumerate_cb, NULL);
        uint64_t t1=now_ns();
        v[i]=t1-t0;
    }
    print_stats("device_enumerate_backends",v,(size_t)iters,"ns");
    free(v); hk_runtime_release(rt);
}

// Resolver: real image — main executable + Foundation
static void bench_resolvers_real(void){
#if defined(__APPLE__)
    const struct mach_header *hdr = _dyld_get_image_header(0);
    if(!hdr) return;
    // Use HookKitResolver public API if available: just measure dyld image count walk vs catalog
    // Fallback: time _dyld_image_count loop + catalog match
    int iters=5000;
    uint64_t *v=malloc((size_t)iters*sizeof(uint64_t));
    hk_runtime_t *rt=NULL; hk_runtime_create(NULL,&rt);
    hk_image_selector_t sel; memset(&sel,0,sizeof(sel));
    sel.struct_size=sizeof(sel); sel.struct_version=HK_ABI_VERSION_3_0;
    sel.kind=HK_IMAGE_ANY_LOADED;
    // If HKImageCatalog is not public, just measure dyld iteration as proxy
    for(int i=0;i<iters;i++){
        uint64_t t0=now_ns();
        volatile uint32_t c = _dyld_image_count();
        (void)c;
        uint64_t t1=now_ns();
        v[i]=t1-t0;
    }
    print_stats("device_dyld_image_count",v,(size_t)iters,"ns");
    free(v);
    hk_runtime_release(rt);
#else
    (void)0;
#endif
}

// Live rebind single-symbol (real write via HookKit if we use plan; here just through rebind plan timing)
// Keep disabled unless device has writable test image — instead bench via plan symbol with existing import

int main(int argc, char **argv){
#if defined(__APPLE__)
    g_log = os_log_create("dev.hookkit", "bench");
    g_spid = os_signpost_id_generate(g_log);
#endif
    int iters_e2e=100;
    int iters_micro=5000;
    int warmup=10;
    bool include_1000=false;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--iters-e2e")==0 && i+1<argc) iters_e2e=atoi(argv[++i]);
        else if(strcmp(argv[i],"--iters")==0 && i+1<argc) iters_micro=atoi(argv[++i]);
        else if(strcmp(argv[i],"--warmup")==0 && i+1<argc) warmup=atoi(argv[++i]);
        else if(strcmp(argv[i],"--include-1000")==0) include_1000=true;
        else if(strcmp(argv[i],"--help")==0){ printf("usage: device_bench [--iters N] [--iters-e2e N] [--warmup N] [--include-1000]\n"); return 0; }
    }
    printf("HookKit device_bench — iters_micro=%d iters_e2e=%d warmup=%d\n", iters_micro, iters_e2e, warmup);
    bench_runtime_create(iters_micro/10);
    bench_enumerate_backends(iters_micro/10);
    bench_resolvers_real();
    int Ns[]={1,10,100,1000};
    size_t count=include_1000 ? sizeof(Ns)/sizeof(Ns[0]) : 3;
    for(size_t i=0;i<count;i++){
        // A real 1,000-target memory patch is intentionally opt-in and cold.
        int it = Ns[i] >= 1000 ? 1 : iters_e2e;
        int plan_warmup = Ns[i] >= 1000 ? 0 : warmup;
        bench_plan_e2e(Ns[i], it, plan_warmup);
    }
    // stderr, not printf: the bench patches every loaded image's puts slot,
    // so a final literal printf("...\n") would be optimized to a puts call
    // and swallowed (or, with a fake replacement, crash) at process exit.
    fprintf(stderr, "device_bench: PASS\n");
    return 0;
}
