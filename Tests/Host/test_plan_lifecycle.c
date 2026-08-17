// Host test for Sources/Core/HKPlan.c. The critical property under test
// is pointer stability: a hk_domain_t* returned by hk_plan_define_domain
// must stay valid and correct after later domains are added (and the
// internal array reallocated), since callers hold onto it across many
// hk_plan_add_hook calls via hk_hook_spec_t.domain.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/Core/HKPlanInternal.h"
#include "../../Sources/Core/HKRuntimeInternal.h"

static hk_domain_spec_t make_domain_spec(const char *id) {
    hk_domain_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.struct_version = HK_ABI_VERSION_3_0;
    spec.stable_domain_id = id;
    spec.domain_order = 0;
    spec.require_all_mandatory_prepared = true;
    spec.prefer_reversible_before_irreversible = true;
    spec.compensation_policy = HK_COMPENSATION_NONE;
    return spec;
}

static void test_create_requires_runtime(void) {
    hk_plan_t *plan = NULL;
    assert(hk_plan_create(NULL, NULL, &plan) == HK_STATUS_INVALID_ARGUMENT);
    assert(plan == NULL);
    printf("  create-requires-runtime: PASS\n");
}

static void test_create_starts_draft(void) {
    hk_runtime_t *rt = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);

    hk_plan_t *plan = NULL;
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);
    assert(plan != NULL);
    assert(hk_plan_state(plan) == HK_PLAN_DRAFT);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  create-starts-draft: PASS\n");
}

static void test_domain_id_deep_copied(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    char mutable_id[] = "identity.runtime";
    hk_domain_spec_t spec = make_domain_spec(mutable_id);

    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &spec, &domain) == HK_STATUS_OK);
    assert(domain != NULL);

    // Mutate the caller's buffer after the call -- if the domain merely
    // stored the pointer instead of deep-copying, this would corrupt it.
    memset(mutable_id, 'X', strlen(mutable_id));

    assert(strcmp(domain->stable_domain_id_owned, "identity.runtime") == 0);
    assert(strcmp(domain->spec.stable_domain_id, "identity.runtime") == 0);
    assert(domain->spec.stable_domain_id != mutable_id);  // repointed at the owned copy, not the caller's buffer

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  domain-id-deep-copied: PASS\n");
}

static void test_duplicate_domain_id_rejected(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t spec1 = make_domain_spec("dup");
    hk_domain_spec_t spec2 = make_domain_spec("dup");
    hk_domain_t *d1 = NULL, *d2 = NULL;

    assert(hk_plan_define_domain(plan, &spec1, &d1) == HK_STATUS_OK);
    assert(hk_plan_define_domain(plan, &spec2, &d2) == HK_STATUS_INVALID_ARGUMENT);
    assert(d2 == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  duplicate-domain-id-rejected: PASS\n");
}

// The property the internal-array-of-pointers design exists for: adding
// enough domains to force several realloc growths (initial capacity is 4)
// must never invalidate a previously-returned hk_domain_t*.
static void test_domain_pointers_stable_across_growth(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    enum { N = 37 };  // several times past the initial capacity of 4
    hk_domain_t *domains[N];
    char ids[N][16];

    for (int i = 0; i < N; i++) {
        snprintf(ids[i], sizeof(ids[i]), "domain.%d", i);
        hk_domain_spec_t spec = make_domain_spec(ids[i]);
        assert(hk_plan_define_domain(plan, &spec, &domains[i]) == HK_STATUS_OK);
    }

    // Re-check every previously-returned pointer AFTER all growth is done
    // -- each one must still report its own correct ID, not garbage from
    // a moved/freed allocation and not another domain's data.
    for (int i = 0; i < N; i++) {
        char expected[16];
        snprintf(expected, sizeof(expected), "domain.%d", i);
        assert(strcmp(domains[i]->stable_domain_id_owned, expected) == 0);
        assert(strcmp(domains[i]->spec.stable_domain_id, expected) == 0);
    }

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  domain-pointers-stable-across-growth: PASS\n");
}

static void test_define_domain_rejects_wrong_state(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    plan->state = HK_PLAN_ANALYZED;  // simulate having left DRAFT

    hk_domain_spec_t spec = make_domain_spec("late");
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &spec, &domain) == HK_STATUS_INVALID_STATE);
    assert(domain == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  define-domain-rejects-wrong-state: PASS\n");
}

static void test_define_domain_rejects_null_id(void) {
    hk_runtime_t *rt = NULL;
    hk_plan_t *plan = NULL;
    assert(hk_runtime_create(NULL, &rt) == HK_STATUS_OK);
    assert(hk_plan_create(rt, NULL, &plan) == HK_STATUS_OK);

    hk_domain_spec_t spec = make_domain_spec(NULL);
    hk_domain_t *domain = NULL;
    assert(hk_plan_define_domain(plan, &spec, &domain) == HK_STATUS_INVALID_ARGUMENT);
    assert(domain == NULL);

    hk_plan_release(plan);
    hk_runtime_release(rt);
    printf("  define-domain-rejects-null-id: PASS\n");
}

static void test_release_tolerates_null(void) {
    hk_plan_release(NULL);  // must not crash
    printf("  release-tolerates-null: PASS\n");
}

static void test_plan_state_of_null_is_discarded(void) {
    assert(hk_plan_state(NULL) == HK_PLAN_DISCARDED);
    printf("  plan-state-of-null-is-discarded: PASS\n");
}

int main(void) {
    test_create_requires_runtime();
    test_create_starts_draft();
    test_domain_id_deep_copied();
    test_duplicate_domain_id_rejected();
    test_domain_pointers_stable_across_growth();
    test_define_domain_rejects_wrong_state();
    test_define_domain_rejects_null_id();
    test_release_tolerates_null();
    test_plan_state_of_null_is_discarded();
    printf("all plan lifecycle tests passed\n");
    return 0;
}
