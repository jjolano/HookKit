#ifndef hk_original_publication_h
#define hk_original_publication_h

#include <stddef.h>
#import <HookKit.h>

typedef struct {
    void **callerCell;
    void *savedCallerValue;
    void *value;
} HKOriginalPublication;

void hk_original_begin(HKOriginalPublication *publication);
void **hk_original_output_cell(HKOriginalPublication *publication);
void hk_original_publish(HKOriginalPublication *publication, void *original);
hookkit_status_t hk_original_finish(HKOriginalPublication *publication, hookkit_status_t status);

#endif
