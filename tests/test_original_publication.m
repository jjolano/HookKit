#import "HKOriginalPublication.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    void *stale = (void *)(uintptr_t)0x1111;
    HKOriginalPublication publication = {
        .callerCell = &stale
    };

    hk_original_begin(&publication);
    assert(stale == NULL);
    assert(hk_original_output_cell(&publication) == &stale);

    void *original = (void *)(uintptr_t)0x2222;
    hk_original_publish(&publication, original);
    hk_original_publish(&publication, NULL);
    assert(stale == original);
    assert(hk_original_finish(&publication, HK_OK) == HK_OK);

    stale = (void *)(uintptr_t)0x3333;
    publication = (HKOriginalPublication){ .callerCell = &stale };
    hk_original_begin(&publication);
    assert(hk_original_finish(&publication, HK_ERR_NOT_SUPPORTED) == HK_ERR_NOT_SUPPORTED);
    assert(stale == (void *)(uintptr_t)0x3333);

    stale = (void *)(uintptr_t)0x4444;
    publication = (HKOriginalPublication){ .callerCell = &stale };
    hk_original_begin(&publication);
    assert(hk_original_finish(&publication, HK_OK) == HK_ERR);

    publication = (HKOriginalPublication){ 0 };
    hk_original_begin(&publication);
    assert(hk_original_output_cell(&publication) == NULL);
    assert(hk_original_finish(&publication, HK_OK) == HK_OK);

    puts("test_original_publication: all assertions passed");
    return 0;
}
