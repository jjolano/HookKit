#ifndef HK_POINTER_AUTH_H
#define HK_POINTER_AUTH_H

#include <stdbool.h>
#include <stdint.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_include
#define __has_include(x) 0
#endif

typedef enum {
    HK_PAC_KEY_IA = 0,
    HK_PAC_KEY_IB = 1,
    HK_PAC_KEY_DA = 2,
    HK_PAC_KEY_DB = 3,
} hk_pac_key_t;

typedef struct {
    bool authenticated;
    hk_pac_key_t key;
    uint16_t diversity;
    bool address_diversity;
} hk_pac_schema_t;

#if defined(HK_PTRAUTH_TEST)

#define HK_PAC_TEST_ADDRESS_MASK UINT64_C(0x0000FFFFFFFFFFFF)

static inline uintptr_t hk_pac_test_strip(uintptr_t value) {
    return value & (uintptr_t)HK_PAC_TEST_ADDRESS_MASK;
}

static inline uintptr_t hk_pac_test_sign(uintptr_t value, hk_pac_key_t key,
                                         uintptr_t discriminator) {
    uintptr_t raw = hk_pac_test_strip(value);
    uint64_t hash = (uint64_t)raw ^ ((uint64_t)discriminator * UINT64_C(0x9E3779B185EBCA87)) ^
                    ((uint64_t)key * UINT64_C(0xD6E8FEB86659FD93));
    uint64_t tag = ((hash >> 17) ^ (hash >> 41) ^ hash) & UINT64_C(0xFFFF);
    if (tag == 0) {
        tag = 1;
    }
    return raw | (uintptr_t)(tag << 48);
}

static inline uintptr_t hk_pac_blend(uintptr_t address, uintptr_t diversity) {
    return address ^ ((uintptr_t)diversity << 32) ^ diversity;
}

static inline uintptr_t hk_pac_strip_key(uintptr_t value, hk_pac_key_t key) {
    (void)key;
    return hk_pac_test_strip(value);
}

static inline uintptr_t hk_pac_sign_key(uintptr_t value, hk_pac_key_t key,
                                        uintptr_t discriminator) {
    return hk_pac_test_sign(value, key, discriminator);
}

#elif __has_feature(ptrauth_calls) && __has_include(<ptrauth.h>)

#include <ptrauth.h>

static inline uintptr_t hk_pac_blend(uintptr_t address, uintptr_t diversity) {
    return (uintptr_t)ptrauth_blend_discriminator((void *)address, diversity);
}

static inline uintptr_t hk_pac_strip_key(uintptr_t value, hk_pac_key_t key) {
    switch (key) {
    case HK_PAC_KEY_IB:
        return (uintptr_t)ptrauth_strip((void *)value, ptrauth_key_asib);
    case HK_PAC_KEY_DA:
        return (uintptr_t)ptrauth_strip((void *)value, ptrauth_key_asda);
    case HK_PAC_KEY_DB:
        return (uintptr_t)ptrauth_strip((void *)value, ptrauth_key_asdb);
    case HK_PAC_KEY_IA:
    default:
        return (uintptr_t)ptrauth_strip((void *)value, ptrauth_key_asia);
    }
}

static inline uintptr_t hk_pac_sign_key(uintptr_t value, hk_pac_key_t key,
                                        uintptr_t discriminator) {
    switch (key) {
    case HK_PAC_KEY_IB:
        return (uintptr_t)ptrauth_sign_unauthenticated(
            (void *)value, ptrauth_key_asib, discriminator);
    case HK_PAC_KEY_DA:
        return (uintptr_t)ptrauth_sign_unauthenticated(
            (void *)value, ptrauth_key_asda, discriminator);
    case HK_PAC_KEY_DB:
        return (uintptr_t)ptrauth_sign_unauthenticated(
            (void *)value, ptrauth_key_asdb, discriminator);
    case HK_PAC_KEY_IA:
    default:
        return (uintptr_t)ptrauth_sign_unauthenticated(
            (void *)value, ptrauth_key_asia, discriminator);
    }
}

#else

static inline uintptr_t hk_pac_blend(uintptr_t address, uintptr_t diversity) {
    (void)address;
    return diversity;
}

static inline uintptr_t hk_pac_strip_key(uintptr_t value, hk_pac_key_t key) {
    (void)key;
    return value;
}

static inline uintptr_t hk_pac_sign_key(uintptr_t value, hk_pac_key_t key,
                                        uintptr_t discriminator) {
    (void)key;
    (void)discriminator;
    return value;
}

#endif

static inline uintptr_t hk_pac_strip_code(uintptr_t value) {
    return hk_pac_strip_key(value, HK_PAC_KEY_IA);
}

static inline uintptr_t hk_pac_make_callable(uintptr_t value) {
    if (value == 0) {
        return 0;
    }
    return hk_pac_sign_key(hk_pac_strip_code(value), HK_PAC_KEY_IA, 0);
}

static inline uintptr_t hk_pac_slot_discriminator(const hk_pac_schema_t *schema,
                                                   uintptr_t slot_address) {
    return schema->address_diversity
        ? hk_pac_blend(slot_address, schema->diversity)
        : schema->diversity;
}

static inline uintptr_t hk_pac_strip_slot(uintptr_t value,
                                          const hk_pac_schema_t *schema) {
    return schema->authenticated ? hk_pac_strip_key(value, schema->key) : value;
}

static inline uintptr_t hk_pac_sign_slot(uintptr_t value,
                                         const hk_pac_schema_t *schema,
                                         uintptr_t slot_address) {
    value = hk_pac_strip_code(value);
    return schema->authenticated
        ? hk_pac_sign_key(value, schema->key,
                          hk_pac_slot_discriminator(schema, slot_address))
        : value;
}

static inline bool hk_pac_slot_matches(uintptr_t value,
                                       const hk_pac_schema_t *schema,
                                       uintptr_t slot_address) {
    return !schema->authenticated ||
           hk_pac_sign_slot(hk_pac_strip_slot(value, schema), schema,
                            slot_address) == value;
}

#endif
