#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../Internal/HKPointerAuth.h"
#include "../../Sources/Core/HKOwnership.h"

#if defined(HK_EXPECT_NATIVE_PTRAUTH)
#if !defined(__APPLE__) || !__has_feature(ptrauth_calls)
#error "native PAC test requires Apple pointer-authentication calls"
#endif
typedef int (*pac_test_fn_t)(int);

__attribute__((noinline)) static int pac_test_target(int value) {
    return value + 1;
}
#endif

int main(void) {
    const uintptr_t raw = UINT64_C(0x1234567890);
    const uintptr_t slot = UINT64_C(0x45678000);
    hk_pac_schema_t ia = {
        .authenticated = true,
        .key = HK_PAC_KEY_IA,
        .diversity = 0x1234,
        .address_diversity = true,
    };
    uintptr_t signed_ia = hk_pac_sign_slot(raw, &ia, slot);
    assert(signed_ia != raw);
    assert(hk_pac_strip_slot(signed_ia, &ia) == raw);
    assert(hk_pac_slot_matches(signed_ia, &ia, slot));

    hk_pac_schema_t wrong = ia;
    wrong.key = HK_PAC_KEY_IB;
    assert(!hk_pac_slot_matches(signed_ia, &wrong, slot));
    wrong = ia;
    wrong.diversity++;
    assert(!hk_pac_slot_matches(signed_ia, &wrong, slot));
    assert(!hk_pac_slot_matches(signed_ia, &ia, slot + 8));

    for (unsigned key = HK_PAC_KEY_IA; key <= HK_PAC_KEY_DB; key++) {
        hk_pac_schema_t schema = ia;
        schema.key = (hk_pac_key_t)key;
        uintptr_t value = hk_pac_sign_slot(raw, &schema, slot);
        assert(hk_pac_strip_slot(value, &schema) == raw);
        assert(hk_pac_slot_matches(value, &schema, slot));
    }

    uintptr_t callable = hk_pac_make_callable(signed_ia);
    assert(callable != raw);
    assert(hk_pac_strip_code(callable) == raw);

#if defined(HK_EXPECT_NATIVE_PTRAUTH)
    volatile uintptr_t native_callable =
        hk_pac_make_callable((uintptr_t)(pac_test_fn_t)pac_test_target);
    assert(((pac_test_fn_t)(uintptr_t)native_callable)(41) == 42);
#endif

    hk_hook_spec_t recorded;
    memset(&recorded, 0, sizeof(recorded));
    recorded.target_kind = HK_TARGET_FUNCTION_ADDRESS;
    recorded.target.address.address = raw;
    recorded.target.address.may_strip_pac_or_thumb_state = true;
    hk_ownership_lock();
    assert(hk_ownership_record_locked(&recorded, "pac-test", (void *)1,
                                      (void *)2));
    hk_ownership_unlock();

    hk_hook_spec_t query = recorded;
    query.target.address.address = callable;
    hk_ownership_state_t state;
    hk_ownership_lock();
    assert(hk_ownership_lookup_locked(&query, &state) == HK_OWNERSHIP_FOUND);
    hk_ownership_unlock();
    assert(state.present && state.head_replacement == (void *)1);

    query.target.address.address = raw;
    query.target.address.may_strip_pac_or_thumb_state = false;
    hk_ownership_lock();
    assert(hk_ownership_lookup_locked(&query, &state) == HK_OWNERSHIP_FOUND);
    hk_ownership_unlock();
#if defined(HK_EXPECT_NATIVE_PTRAUTH)
    printf("native pointer-auth tests passed\n");
#else
    printf("pointer-auth simulation tests passed\n");
#endif
    return 0;
}
