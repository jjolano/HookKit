#include <HookKit/HookKit.h>

#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *libobjc = NULL;
    const char *libdyld = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *path = _dyld_get_image_name(i);
        if (path && !libobjc && strstr(path, "/libobjc")) {
            libobjc = path;
        }
        if (path && !libdyld && strstr(path, "/libdyld")) {
            libdyld = path;
        }
    }
    if (!libobjc || !libdyld) {
        puts("HookKit3 resolver: FAIL (required image not loaded)");
        return 1;
    }

    hk_runtime_t *runtime = NULL;
    if (hk_runtime_create(NULL, &runtime) != HK_STATUS_OK || !runtime) {
        puts("HookKit3 resolver: FAIL (runtime)");
        return 1;
    }

    void *exported = NULL;
    void *private_symbol = NULL;
    hk_status_t exported_status = hk_runtime_find_symbol(
        runtime, libobjc, "objc_msgSend", &exported);
    hk_status_t private_status = hk_runtime_find_symbol(
        runtime, libdyld, "dyld_image_get_installname", &private_symbol);
    printf("HookKit3 resolver: %s (%s, export=%s, private=%s)\n",
           exported_status == HK_STATUS_OK && private_status == HK_STATUS_OK
               ? "PASS" : "FAIL",
           libobjc, exported ? "ok" : "missing",
           private_symbol ? libdyld : "missing");
    hk_runtime_release(runtime);
    return exported_status == HK_STATUS_OK && private_status == HK_STATUS_OK ? 0 : 1;
}
