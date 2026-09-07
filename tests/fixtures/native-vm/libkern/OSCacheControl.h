#ifndef HK_TEST_CACHE_CONTROL_H
#define HK_TEST_CACHE_CONTROL_H
#include <stddef.h>
#define sys_icache_invalidate hk_test_icache_invalidate
void sys_icache_invalidate(void *, size_t);
#endif
