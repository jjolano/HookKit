// Host test for the minimal internal engine contract + registry
// (Sources/Core/HKEngineInternal.h, HKRuntime.c's
// hk_runtime_register_engine_for_testing, HKPlan.c's hk_hook_analyze_one).
// The property under test: hk_plan_analyze actually consults registered
// engines now, upgrading eligible hooks from NO_ROUTE to ANALYZED, while
// hooks nothing can serve still honestly get NO_ROUTE.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKEngineInternal.h"
#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKReportInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"
#include "fake_engines.h"

static hk_hook_spec_t symbol_spec(const char *id, hk_reachability_t required, hk_reachability_t preferred) {
    hk_hook_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_hook_id = id;
    spec.target_kind = HK_TARGET_FUNCTION_SYMBOL;
    spec.target.symbol.name = "getpid";
    spec.required_reach = required;
    spec.preferred_reach = preferred;
    spec.original_requirement = HK_ORIGINAL_NONE;
    spec.continuation_policy = HK_CONTINUATION_ANY;
    spec.availability = HK_AVAILABILITY_REQUIRED_NOW;
    spec.role = HK_OPERATION_MANDATORY;
    return spec;
}

static bool versioned_engine_discover(void *ctx, hk_engine_discovery_t *out) {
    (void)ctx;
    out->available = true;
    return true;
}

static bool versioned_engine_analyze(void *ctx,
                                     const hk_hook_spec_t *request,
                                     hk_engine_analysis_t *out) {
    (void)ctx;
    (void)request;
    out->eligible = true;
    out->achieved_reach = HK_REACH_EXISTING_IMPORTS;
    return true;
}

typedef struct {
    uint32_t route_rank;
} ranked_route_ctx_t;

static bool ranked_engine_analyze(void *ctx,
                                  const hk_hook_spec_t *request,
                                  hk_engine_analysis_t *out) {
    const ranked_route_ctx_t *rank = ctx;
    (void)request;
    if (!rank || !out) {
        return false;
    }
    out->eligible = true;
    out->achieved_reach = HK_REACH_EXISTING_IMPORTS;
    out->route_rank = rank->route_rank;
    return true;
}

static const hk_engine_vtable_t ranked_engine = {
    .describe = fake_rebind_describe,
    .analyze_operation = ranked_engine_analyze,
    .prepare_one = fake_rebind_prepare_one,
};

static hk_engine_capabilities_t certified_arm64_describe(void) {
    hk_engine_capabilities_t caps = fake_rebind_describe();
    caps.engine_id = "certified-arm64";
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                         HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    return caps;
}

static const hk_engine_vtable_t certified_arm64_engine = {
    .describe = certified_arm64_describe,
    .prepare_one = fake_rebind_prepare_one,
};

static void test_no_engines_registered_still_no_route(void) {
    // Regression check: this was the only behavior before this iteration,
    // and it must not have silently changed.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_NO_ROUTE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  no-engines-registered-still-no-route: PASS\n");
}

static void test_eligible_engine_upgrades_to_analyzed(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS,
                                       HK_REACH_EXISTING_IMPORTS | HK_REACH_FUTURE_IMPORTS);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    const hk_hook_result_t *r = &report->results[0];
    assert(r->outcome == HK_OUTCOME_ANALYZED);
    assert(r->achieved_reach == HK_REACH_EXISTING_IMPORTS);
    // FUTURE_IMPORTS was preferred but the fake engine can't achieve it --
    // must show up as unmet, not silently dropped.
    assert(r->unmet_preferred_reach == HK_REACH_FUTURE_IMPORTS);
    assert(r->retryable == false);
    assert(r->diagnostic_engine_id.length == strlen("fake-rebind"));
    assert(memcmp(r->diagnostic_engine_id.data, "fake-rebind", r->diagnostic_engine_id.length) == 0);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  eligible-engine-upgrades-to-analyzed: PASS\n");
}

static void test_insufficient_reach_stays_no_route(void) {
    // The engine handles the target KIND but can't achieve the required
    // REACH -- must not be treated as eligible just because the kind matched.
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    // fake_rebind only achieves EXISTING_IMPORTS; require PRIVATE_ADDRESS too.
    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS | HK_REACH_PRIVATE_ADDRESS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_NO_ROUTE);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  insufficient-reach-stays-no-route: PASS\n");
}

static void test_first_eligible_wins_skips_ineligible(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // Register the objc engine FIRST -- it's ineligible for a symbol
    // target, so the search must correctly skip past it to the rebind
    // engine registered second, not stop (or crash) on the mismatch.
    assert(hk_runtime_register_engine_for_testing(rt, &fake_objc_engine));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("hook.a", HK_REACH_EXISTING_IMPORTS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);

    hk_report_t *report = NULL;
    assert(hk_plan_analyze(plan, &report) == HK_STATUS_OK);
    assert(report->results[0].outcome == HK_OUTCOME_ANALYZED);
    assert(memcmp(report->results[0].diagnostic_engine_id.data, "fake-rebind",
                  report->results[0].diagnostic_engine_id.length) == 0);

    hk_report_release(report);
    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  first-eligible-wins-skips-ineligible: PASS\n");
}

static void test_register_engine_rejects_null(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(NULL, &fake_rebind_engine) == false);
    assert(hk_runtime_register_engine_for_testing(rt, NULL) == false);
    hk_runtime_release(rt);
    printf("  register-engine-rejects-null: PASS\n");
}

// The router must pick on ORIGINAL REQUIREMENT when two engines are otherwise
// identical. This is the terminal/relocating inline pair's exact shape: both
// claim function-address targets with entry-point reach, and differ only in
// which originals they can hand back. Without this criterion the first
// registered would always win and the other's capability would be unreachable.
static void test_routes_on_original_requirement(void) {
    // Registered NONE-only FIRST, deliberately: that is the order that used to
    // break, since the narrower engine would have swallowed every request.
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_original_none_engine));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_original_any_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t none_req = symbol_spec("h.none", HK_REACH_ENTRYPOINT, 0);
    none_req.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    none_req.original_requirement = HK_ORIGINAL_NONE;

    hk_hook_spec_t cont_req = none_req;
    cont_req.stable_hook_id = "h.cont";
    cont_req.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;

    hk_hook_t *hn = NULL, *hc = NULL;
    assert(hk_plan_add_hook(plan, &none_req, &hn) == HK_STATUS_OK);
    assert(hk_plan_add_hook(plan, &cont_req, &hc) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);

    // NONE goes to the first eligible, which is the narrow one.
    assert(hn->matched_engine == &fake_original_none_engine);
    // CALLABLE_CONTINUATION SKIPS it and reaches the one that can serve it.
    assert(hc->matched_engine == &fake_original_any_engine);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  routes-on-original-requirement: PASS\n");
}

// An engine that declares nothing keeps its old behaviour exactly: zero is
// read as "any", so adding the field cannot make a previously-eligible engine
// silently ineligible. That property is why every pre-existing fake engine
// needed no edit.
static void test_undeclared_requirements_mean_any(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // fake_rebind_engine declares no original_requirements at all.
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.any", HK_REACH_EXISTING_IMPORTS, 0);
    spec.original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION;
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == &fake_rebind_engine);   // still eligible

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  undeclared-requirements-mean-any: PASS\n");
}

static void test_routes_on_preferred_reach(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_preferred_rebind_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.preferred", HK_REACH_EXISTING_IMPORTS,
                                      HK_REACH_EXISTING_IMPORTS | HK_REACH_FUTURE_IMPORTS);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == &fake_preferred_rebind_engine);
    assert(hook->result.unmet_preferred_reach == 0);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  routes-on-preferred-reach: PASS\n");
}

static void test_routes_on_explicit_rank(void) {
    ranked_route_ctx_t lower = {.route_rank = 1};
    ranked_route_ctx_t higher = {.route_rank = 2};
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_with_context(rt, &ranked_engine, &lower));
    assert(hk_runtime_register_engine_with_context(rt, &ranked_engine, &higher));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.rank", HK_REACH_EXISTING_IMPORTS,
                                      HK_REACH_EXISTING_IMPORTS);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->matched_engine == &ranked_engine);
    assert(hook->matched_engine_ctx == &higher);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  routes-on-explicit-rank: PASS\n");
}

static bool production_route_is_available(
    hk_engine_architecture_mask_t architecture,
    uint32_t ios_version,
    bool testing_registration)
{
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    rt->platform_architecture = architecture;
    rt->platform_ios_version = ios_version;
    assert(testing_registration
        ? hk_runtime_register_engine_for_testing(rt, &certified_arm64_engine)
        : hk_runtime_register_engine_with_context(rt, &certified_arm64_engine,
                                                  NULL));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.platform", HK_REACH_EXISTING_IMPORTS,
                                      0);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    bool available = hook->result.outcome == HK_OUTCOME_ANALYZED;
    hk_plan_release(plan);
    hk_runtime_release(rt);
    return available;
}

static void test_platform_certification_gates_production_routes(void) {
    assert(!production_route_is_available(HK_ENGINE_ARCHITECTURE_ARM64E,
                                          HK_ENGINE_IOS_VERSION(15, 0, 0),
                                          false));
    assert(!production_route_is_available(HK_ENGINE_ARCHITECTURE_ARM64,
                                          HK_ENGINE_IOS_VERSION(14, 0, 0),
                                          false));
    assert(production_route_is_available(HK_ENGINE_ARCHITECTURE_ARM64,
                                         HK_ENGINE_IOS_VERSION(15, 0, 0),
                                         false));
    assert(production_route_is_available(HK_ENGINE_ARCHITECTURE_ARM64E,
                                         HK_ENGINE_IOS_VERSION(14, 0, 0),
                                         true));
    printf("  platform-certification-gates-production-routes: PASS\n");
}

static void test_install_context_filters_engine(void) {
    hk_runtime_config_t config;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.struct_version = HK_ABI_VERSION_3_0;
    config.install_context = HK_INSTALL_CONTEXT_ARBITRARY_RUNTIME;

    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(&config, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_early_only_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.context", HK_REACH_EXISTING_IMPORTS, 0);
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(hook->matched_engine == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  install-context-filters-engine: PASS\n");
}

static hk_hook_spec_t exact_address_spec(const char *id) {
    hk_hook_spec_t spec = symbol_spec(id, HK_REACH_ENTRYPOINT |
                                      HK_REACH_EXACT_IMAGE_SCOPE, 0);
    spec.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    spec.target.address.address = 0x1000;
    spec.target.address.expected_image.struct_size = sizeof(spec.target.address.expected_image);
    spec.target.address.expected_image.struct_version = HK_ABI_VERSION_3_0;
    spec.target.address.expected_image.kind = HK_IMAGE_ANY_LOADED;
    return spec;
}

static void test_exact_image_scope_requires_catalog(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // This proves the core requires a populated catalog, not that one
    // happens to be absent: on a host build with a real dyld image list
    // (e.g. these tests running on macOS), hk_runtime_create leaves a
    // non-empty catalog behind. Force the empty case deliberately, the
    // same way test_exact_image_scope_routes_with_catalog below forces
    // the populated one.
    hk_image_catalog_destroy(rt->catalog);
    rt->catalog = NULL;
    assert(hk_runtime_register_engine_for_testing(rt, &fake_exact_address_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = exact_address_spec("h.exact.empty");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(hook->result.unmet_preferred_reach == 0);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  exact-image-scope-requires-catalog: PASS\n");
}

static void test_exact_image_scope_routes_with_catalog(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    // Linux runtime creation leaves live dyld population unavailable, but the
    // router's catalog contract is platform-neutral and can use a synthetic
    // entry for this analysis-only proof.
    rt->catalog = hk_image_catalog_create();
    assert(rt->catalog != NULL);
    hk_image_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.path = "/synthetic/exact-image.dylib";
    entry.header = (const void *)0x1000;
    assert(hk_image_catalog_add_entry(rt->catalog, &entry));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_exact_address_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = exact_address_spec("h.exact.present");
    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert((hook->result.achieved_reach & HK_REACH_EXACT_IMAGE_SCOPE) != 0);
    assert(hook->matched_engine == &fake_exact_address_engine);

    hk_plan_release(plan);
    hk_image_catalog_destroy(rt->catalog);
    rt->catalog = NULL;
    hk_runtime_release(rt);
    printf("  exact-image-scope-routes-with-catalog: PASS\n");
}

static void test_defining_image_scope_is_not_ignored(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_runtime_register_engine_for_testing(rt, &fake_rebind_engine));
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_hook_spec_t spec = symbol_spec("h.defining-scope",
                                      HK_REACH_EXISTING_IMPORTS, 0);
    spec.target.symbol.defining_image.struct_size = sizeof(hk_image_selector_t);
    spec.target.symbol.defining_image.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.defining_image.kind = HK_IMAGE_EXACT_PATH;
    spec.target.symbol.defining_image.path = "/usr/lib/libowner.dylib";

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_NO_ROUTE);
    assert(hook->matched_engine == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  defining-image-scope-is-not-ignored: PASS\n");
}

static void test_declared_symbol_scope_remains_achievable(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    rt->catalog = hk_image_catalog_create();
    assert(rt->catalog != NULL);
    hk_image_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.path = "/usr/lib/libowner.dylib";
    entry.header = (const void *)0x2000;
    assert(hk_image_catalog_add_entry(rt->catalog, &entry));
    assert(hk_runtime_register_engine_for_testing(rt, &fake_exact_symbol_engine));

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec(
        "h.defining-scope-supported",
        HK_REACH_EXISTING_IMPORTS | HK_REACH_EXACT_IMAGE_SCOPE, 0);
    spec.target.symbol.defining_image.struct_size = sizeof(hk_image_selector_t);
    spec.target.symbol.defining_image.struct_version = HK_ABI_VERSION_3_0;
    spec.target.symbol.defining_image.kind = HK_IMAGE_EXACT_PATH;
    spec.target.symbol.defining_image.path = "/usr/lib/libowner.dylib";

    hk_hook_t *hook = NULL;
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);
    assert((hook->result.achieved_reach & HK_REACH_EXACT_IMAGE_SCOPE) != 0);
    assert(hook->matched_engine == &fake_exact_symbol_engine);

    hk_plan_release(plan);
    hk_image_catalog_destroy(rt->catalog);
    rt->catalog = NULL;
    hk_runtime_release(rt);
    printf("  declared-symbol-scope-remains-achievable: PASS\n");
}

static void test_versioned_vtable_header_is_checked(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);

    hk_engine_vtable_t versioned = fake_rebind_engine;
    versioned.abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1 + 1u;
    versioned.struct_size = sizeof(versioned);
    assert(!hk_runtime_register_engine_for_testing(rt, &versioned));

    versioned.abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1;
    versioned.struct_size = offsetof(hk_engine_vtable_t, describe);
    assert(!hk_runtime_register_engine_for_testing(rt, &versioned));

    versioned.struct_size = sizeof(versioned);
    versioned.discover = versioned_engine_discover;
    versioned.analyze_operation = versioned_engine_analyze;
    assert(hk_runtime_register_engine_for_testing(rt, &versioned));

    hk_plan_t *plan = NULL;
    hk_hook_t *hook = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    hk_hook_spec_t spec = symbol_spec("h.versioned",
                                      HK_REACH_EXISTING_IMPORTS, 0);
    assert(hk_plan_add_hook(plan, &spec, &hook) == HK_STATUS_OK);
    assert(hk_plan_analyze(plan, NULL) == HK_STATUS_OK);
    assert(hook->result.outcome == HK_OUTCOME_ANALYZED);
    hk_plan_release(plan);

    hk_runtime_release(rt);
    printf("  versioned-vtable-header-is-checked: PASS\n");
}

int main(void) {
    test_routes_on_original_requirement();
    test_undeclared_requirements_mean_any();
    test_no_engines_registered_still_no_route();
    test_eligible_engine_upgrades_to_analyzed();
    test_insufficient_reach_stays_no_route();
    test_first_eligible_wins_skips_ineligible();
    test_register_engine_rejects_null();
    test_routes_on_preferred_reach();
    test_routes_on_explicit_rank();
    test_platform_certification_gates_production_routes();
    test_install_context_filters_engine();
    test_exact_image_scope_requires_catalog();
    test_exact_image_scope_routes_with_catalog();
    test_defining_image_scope_is_not_ignored();
    test_declared_symbol_scope_remains_achievable();
    test_versioned_vtable_header_is_checked();
    printf("all engine registry tests passed\n");
    return 0;
}
