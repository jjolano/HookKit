// bench_plan.c — host plan lifecycle benchmark, large-scale sweep.
// ponytail: fake engines, no device seam, stdlib only.
#include "bench_common.h"
#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "../../Tests/Host/fake_engines.h"
#include <assert.h>
#include <string.h>

static void *g_replacement = (void *)0x1234;

typedef struct {
    int n; // hooks per plan
    hk_target_kind_t kind;
    const hk_engine_vtable_t *engine;
} bench_cfg_t;

static hk_hook_spec_t make_spec(const char *id, hk_target_kind_t kind) {
    hk_hook_spec_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = sizeof(s);
    s.struct_version = HK_ABI_VERSION_3_0;
    s.stable_hook_id = id;
    s.target_kind = kind;
    s.replacement = g_replacement;
    if (kind == HK_TARGET_FUNCTION_SYMBOL) {
        s.target.symbol.struct_size = sizeof(s.target.symbol);
        s.target.symbol.struct_version = HK_ABI_VERSION_3_0;
        s.target.symbol.name = "mySym";
        s.target.symbol.defining_image.kind = HK_IMAGE_ANY_LOADED;
        s.target.symbol.caller_image_scope.kind = HK_IMAGE_ANY_LOADED;
    } else if (kind == HK_TARGET_FUNCTION_ADDRESS) {
        s.target.address.struct_size = sizeof(s.target.address);
        s.target.address.struct_version = HK_ABI_VERSION_3_0;
        s.target.address.address = 0x1000;
    } else if (kind == HK_TARGET_OBJC_METHOD) {
        s.target.objc.struct_size = sizeof(s.target.objc);
        s.target.objc.struct_version = HK_ABI_VERSION_3_0;
        s.target.objc.class_name = "CLS";
        s.target.objc.selector_name = "sel";
    } else if (kind == HK_TARGET_MEMORY_PATCH) {
        static uint8_t repl[4] = {1,2,3,4};
        static uint8_t expect[4] = {0,0,0,0};
        s.target.memory.struct_size = sizeof(s.target.memory);
        s.target.memory.struct_version = HK_ABI_VERSION_3_0;
        s.target.memory.address = 0x2000;
        s.target.memory.replacement_bytes.data = repl;
        s.target.memory.replacement_bytes.size = sizeof(repl);
        s.target.memory.expected_bytes.data = expect;
        s.target.memory.expected_bytes.size = sizeof(expect);
    }
    return s;
}

// Bench fake for objc that actually prepares/commits — fake_objc in fake_engines.h has no prepare_one.
static hk_engine_capabilities_t bench_objc_describe(void){
    hk_engine_capabilities_t c; memset(&c,0,sizeof(c));
    c.engine_id="bench-objc";
    c.target_kinds=HK_TARGET_KIND_BIT(HK_TARGET_OBJC_METHOD);
    c.achievable_reach=HK_REACH_OBJC_DISPATCH;
    return c;
}
static bool bench_objc_prepare(const hk_hook_spec_t *s){ (void)s; return true; }
static hk_mutation_state_t bench_objc_commit(const hk_hook_spec_t *s, hk_artifact_sink_t *sink){
    (void)s; (void)sink; return HK_MUTATION_COMPLETE;
}
static const hk_engine_vtable_t bench_objc_engine = {
    .describe=bench_objc_describe,
    .prepare_one=bench_objc_prepare,
    .commit_one=bench_objc_commit,
};

static void bench_one(const bench_cfg_t *cfg, hk_bench_series_t *s_add, hk_bench_series_t *s_analyze, hk_bench_series_t *s_prepare, hk_bench_series_t *s_commit, hk_bench_series_t *s_e2e) {
    char ids[1024][32];
    for (int i = 0; i < cfg->n; i++) snprintf(ids[i], sizeof(ids[i]), "h%d", i);
    const hk_engine_vtable_t *eng = cfg->engine;
    // Use bench-objc for objc kind so prepare/commit succeed
    if (cfg->kind == HK_TARGET_OBJC_METHOD) eng = &bench_objc_engine;

    // e2e
    {
        uint64_t t0 = hk_bench_now_ns();
        hk_runtime_t *rt = NULL;
        assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
        assert(hk_runtime_register_engine_for_testing(rt, eng));
        hk_plan_t *plan = NULL;
        assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
        for (int i = 0; i < cfg->n; i++) {
            hk_hook_spec_t spec = make_spec(ids[i], cfg->kind);
            hk_hook_t *hook = NULL;
            assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
        }
        hk_report_t *r = NULL;
        (void)hk_plan_analyze(plan, &r); if(r) hk_report_release(r); r=NULL;
        (void)hk_plan_prepare(plan, &r); if(r) hk_report_release(r); r=NULL;
        (void)hk_plan_commit(plan, &r); if(r) hk_report_release(r);
        hk_plan_release(plan);
        hk_runtime_release(rt);
        uint64_t t1 = hk_bench_now_ns();
        hk_bench_series_push(s_e2e, t1 - t0);
    }
    // isolated add / analyze / prepare / commit
    {
        hk_runtime_t *rt = NULL; assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
        assert(hk_runtime_register_engine_for_testing(rt, eng));
        hk_plan_t *plan = NULL; assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
        uint64_t t0 = hk_bench_now_ns();
        for (int i = 0; i < cfg->n; i++) {
            hk_hook_spec_t spec = make_spec(ids[i], cfg->kind);
            hk_hook_t *hook = NULL;
            assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
        }
        uint64_t t1 = hk_bench_now_ns();
        hk_bench_series_push(s_add, (t1 - t0) / (cfg->n ? (uint64_t)cfg->n : 1));

        hk_report_t *r = NULL;
        t0 = hk_bench_now_ns(); (void)hk_plan_analyze(plan, &r); t1 = hk_bench_now_ns();
        hk_bench_series_push(s_analyze, (t1 - t0) / (cfg->n ? (uint64_t)cfg->n : 1)); if(r) hk_report_release(r); r=NULL;
        t0 = hk_bench_now_ns(); (void)hk_plan_prepare(plan, &r); t1 = hk_bench_now_ns();
        hk_bench_series_push(s_prepare, (t1 - t0) / (cfg->n ? (uint64_t)cfg->n : 1)); if(r) hk_report_release(r); r=NULL;
        t0 = hk_bench_now_ns(); (void)hk_plan_commit(plan, &r); t1 = hk_bench_now_ns();
        hk_bench_series_push(s_commit, (t1 - t0) / (cfg->n ? (uint64_t)cfg->n : 1)); if(r) hk_report_release(r);

        hk_plan_release(plan); hk_runtime_release(rt);
    }
}

static void usage(void) {
    fprintf(stderr, "usage: bench_plan [--iters N] [--warmup N]\n");
}

int main(int argc, char **argv) {
    size_t iters = 200;
    size_t warmup = 20;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i+1 < argc) iters = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--warmup") == 0 && i+1 < argc) warmup = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
    }

    // Pin warmup by running a tiny sweep once
    {
        bench_cfg_t c = {1, HK_TARGET_FUNCTION_SYMBOL, &fake_rebind_engine};
        hk_bench_series_t d1,d2,d3,d4,d5; hk_bench_series_init(&d1,"x","ns"); hk_bench_series_init(&d2,"x","ns"); hk_bench_series_init(&d3,"x","ns"); hk_bench_series_init(&d4,"x","ns"); hk_bench_series_init(&d5,"x","ns");
        for (size_t i=0;i<warmup;i++) bench_one(&c,&d1,&d2,&d3,&d4,&d5);
        hk_bench_series_free(&d1); hk_bench_series_free(&d2); hk_bench_series_free(&d3); hk_bench_series_free(&d4); hk_bench_series_free(&d5);
    }

    struct { const char *name; hk_target_kind_t kind; const hk_engine_vtable_t *eng; } kinds[] = {
        {"symbol", HK_TARGET_FUNCTION_SYMBOL, &fake_rebind_engine},
        {"objc", HK_TARGET_OBJC_METHOD, &fake_objc_engine},
        {"address", HK_TARGET_FUNCTION_ADDRESS, &fake_original_any_engine},
    };
    int Ns[] = {1,10,100,1000};

    for (size_t ki = 0; ki < sizeof(kinds)/sizeof(kinds[0]); ki++) {
        for (size_t ni = 0; ni < sizeof(Ns)/sizeof(Ns[0]); ni++) {
            bench_cfg_t cfg = {Ns[ni], kinds[ki].kind, kinds[ki].eng};
            hk_bench_series_t s_add, s_analyze, s_prepare, s_commit, s_e2e;
            char n_add[64], n_ana[64], n_pre[64], n_com[64], n_e2e[64];
            snprintf(n_add,sizeof(n_add),"add/%s/%d",kinds[ki].name,Ns[ni]); hk_bench_series_init(&s_add,n_add,"ns/op");
            snprintf(n_ana,sizeof(n_ana),"analyze/%s/%d",kinds[ki].name,Ns[ni]); hk_bench_series_init(&s_analyze,n_ana,"ns/op");
            snprintf(n_pre,sizeof(n_pre),"prepare/%s/%d",kinds[ki].name,Ns[ni]); hk_bench_series_init(&s_prepare,n_pre,"ns/op");
            snprintf(n_com,sizeof(n_com),"commit/%s/%d",kinds[ki].name,Ns[ni]); hk_bench_series_init(&s_commit,n_com,"ns/op");
            snprintf(n_e2e,sizeof(n_e2e),"e2e/%s/%d",kinds[ki].name,Ns[ni]); hk_bench_series_init(&s_e2e,n_e2e,"ns");

            for (size_t i=0;i<iters;i++) bench_one(&cfg,&s_add,&s_analyze,&s_prepare,&s_commit,&s_e2e);

            hk_bench_print(s_add.name, &s_add);
            hk_bench_print(s_analyze.name, &s_analyze);
            hk_bench_print(s_prepare.name, &s_prepare);
            hk_bench_print(s_commit.name, &s_commit);
            hk_bench_print(s_e2e.name, &s_e2e);

            hk_bench_series_free(&s_add); hk_bench_series_free(&s_analyze); hk_bench_series_free(&s_prepare); hk_bench_series_free(&s_commit); hk_bench_series_free(&s_e2e);
        }
    }

    // Single large mixed test (symbol+address) to surface routing cost
    {
        hk_bench_series_t s; hk_bench_series_init(&s, "e2e/mixed/1000","ns");
        char ids[1000][32];
        for (int i=0;i<1000;i++) snprintf(ids[i],sizeof(ids[i]),"m%d",i);
        for (size_t i=0;i<iters;i++) {
            uint64_t t0=hk_bench_now_ns();
            hk_runtime_t *rt=NULL; assert(hk_runtime_create(NULL,&rt)==HK_STATUS_OK);
            assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
            assert(hk_runtime_register_engine_for_testing(rt, &fake_original_any_engine));
            hk_plan_t *plan=NULL; assert(hk_plan_create(rt,NULL,&plan)==HK_STATUS_OK);
            for(int k=0;k<1000;k++){
                hk_target_kind_t kind = (k%2==0)? HK_TARGET_FUNCTION_SYMBOL : HK_TARGET_FUNCTION_ADDRESS;
                hk_hook_spec_t spec = make_spec(ids[k], kind);
                hk_hook_t *h=NULL; assert(hk_plan_add_hook(plan,&spec,&h)==HK_STATUS_OK);
            }
            hk_report_t *r=NULL;
            assert(hk_plan_analyze(plan,&r)==HK_STATUS_OK); hk_report_release(r);
            assert(hk_plan_prepare(plan,&r)==HK_STATUS_OK); hk_report_release(r);
            assert(hk_plan_commit(plan,&r)==HK_STATUS_OK); hk_report_release(r);
            hk_plan_release(plan); hk_runtime_release(rt);
            uint64_t t1=hk_bench_now_ns();
            hk_bench_series_push(&s, t1-t0);
        }
        hk_bench_print(s.name,&s);
        hk_bench_series_free(&s);
    }
    return 0;
}
