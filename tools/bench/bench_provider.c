// bench_provider.c — provider discover + enumerate throughput (host: measures dlsym + memo path)
// ponytail: no real dlopen on host Linux, just exercises the router/effect checks.
#include "bench_common.h"
#include "../../src/core/HKRuntimeInternal.h"
#include "../../src/core/HKEngineInternal.h"
#include <string.h>

static bool dummy_discover(void *ctx, hk_engine_discovery_t *out){
    (void)ctx; if(out){out->available=true;} return true;
}
static hk_engine_capabilities_t dummy_describe(void){
    hk_engine_capabilities_t c; memset(&c,0,sizeof(c));
    c.engine_id="dummy-provider"; c.backend_group="dummy-provider"; c.display_name="Dummy";
    c.target_kinds=HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_ADDRESS);
    c.achievable_reach=HK_REACH_ENTRYPOINT;
    c.architectures=HK_ENGINE_ARCHITECTURE_ARM64; c.certified_architectures=HK_ENGINE_ARCHITECTURE_ARM64;
    c.minimum_ios_version=HK_ENGINE_IOS_VERSION(15,0,0);
    return c;
}
static const hk_engine_vtable_t dummy_vtable = {
    .abi_version=HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size=sizeof(hk_engine_vtable_t),
    .describe=dummy_describe,
    .discover=dummy_discover,
};

static hk_runtime_t *g_rt=NULL;
static bool enum_noop(void *c, hk_string_view_t a, hk_string_view_t b){(void)c;(void)a;(void)b; return true;}
static void bench_enumerate2(void *ctx){ (void)ctx; hk_runtime_enumerate_backends(g_rt, enum_noop, NULL); }

static void bench_create_release(void *ctx){
    (void)ctx;
    hk_runtime_t *rt=NULL;
    hk_runtime_create(NULL,&rt);
    hk_runtime_register_engine_with_context(rt,&dummy_vtable,NULL);
    hk_runtime_release(rt);
}

int main(int argc, char **argv){
    size_t iters=20000, warmup=200;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--iters")==0 && i+1<argc) iters=(size_t)atoi(argv[++i]);
        if(strcmp(argv[i],"--warmup")==0 && i+1<argc) warmup=(size_t)atoi(argv[++i]);
    }
    hk_runtime_create(NULL,&g_rt);
    hk_runtime_register_engine_with_context(g_rt,&dummy_vtable,NULL);
    hk_bench_run("enumerate_backends", bench_enumerate2, NULL, warmup, iters, 1);
    hk_bench_run("runtime_create+release", bench_create_release, NULL, warmup, iters/10, 1);
    hk_runtime_release(g_rt);
    return 0;
}
