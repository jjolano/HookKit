#include "../../Headers/HookKit/HookKitSwift.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    hk_swift_target_t target = hk_swift_target_init();
    hk_swift_plan_t *plan = NULL;

    assert(hk_swift_prepare(&target, &plan) == HK_STATUS_UNAVAILABLE);
    assert(plan == NULL);
    assert(hk_swift_hook(&target, (void *)0x1, NULL) == HK_STATUS_UNAVAILABLE);
    assert(hk_swift_commit(NULL, (void *)0x1, NULL) == HK_STATUS_INVALID_ARGUMENT);

    puts("all swift engine surface tests passed");
    return 0;
}
