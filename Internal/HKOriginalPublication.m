#import "HKOriginalPublication.h"

void hk_original_begin(HKOriginalPublication *publication) {
    if(!publication->callerCell) {
        publication->savedCallerValue = NULL;
        publication->value = NULL;
        return;
    }

    publication->savedCallerValue = *publication->callerCell;
    *publication->callerCell = NULL;
    publication->value = NULL;
}

void **hk_original_output_cell(HKOriginalPublication *publication) {
    return publication->callerCell;
}

void hk_original_publish(HKOriginalPublication *publication, void *original) {
    if(original) {
        publication->value = original;
        if(publication->callerCell) {
            *publication->callerCell = original;
        }
    }
}

hookkit_status_t hk_original_finish(HKOriginalPublication *publication, hookkit_status_t status) {
    if(status == HK_ERR_NOT_SUPPORTED) {
        if(publication->callerCell) {
            *publication->callerCell = publication->savedCallerValue;
        }
        return status;
    }

    if(status == HK_OK && publication->callerCell && !publication->value) {
        return HK_ERR;
    }

    return status;
}
