#include "hk_native.h"

#if defined(__arm64__) || defined(__aarch64__)

#include "hk_arm64.h"

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define hk_strip_code(p)  ptrauth_strip((p), ptrauth_key_function_pointer)
#define hk_sign_code(p)   ptrauth_sign_unauthenticated((void *)(p), ptrauth_key_function_pointer, 0)
#else
#define hk_strip_code(p)  (p)
#define hk_sign_code(p)   ((void *)(p))
#endif

static int hk_errno = 0;

bool hk_native_supported(void) {
    return true;
}

int hk_native_last_error(void) {
    return hk_errno;
}

#pragma mark - Memory

// Overflow-checked, page-rounded bounds for [addr, addr+len): false when the
// range is empty or wraps the address space; *start/*end come back
// page-aligned. Single owner of the `+ page - 1` rounding, so no caller can
// round a wrapped range into something that looks valid.
static bool hk_range_bounds(const void *addr, size_t len, vm_address_t *start, vm_address_t *end) {
    if(!addr || len == 0) {
        return false;
    }

    vm_size_t page = (vm_size_t)getpagesize();
    vm_address_t base = (vm_address_t)addr;
    vm_address_t last = base + len;

    if(last < base) {
        return false;   // address overflow
    }

    *start = base & ~(page - 1);
    vm_address_t rounded = last + (page - 1);

    if(rounded < last) {
        return false;   // rounding overflow: range touches the top of the address space
    }

    *end = rounded & ~(page - 1);
    return true;
}

// True when every VM region covering [start, end) grants ALL of `required`
// protections. Per-region, not uniform: mixed regions pass as long as each
// grants the bits (the readable probe's documented contract). start/end are
// page-aligned, caller-validated bounds.
static bool hk_range_all_protected(vm_address_t start, vm_address_t end, vm_prot_t required) {
    while(start < end) {
        vm_address_t region = start;
        vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;

        kern_return_t kr = vm_region_64(mach_task_self(), &region, &region_size,
                                        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                        &count, &object);

        if(object != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object);
        }

        if(kr != KERN_SUCCESS || region > start || region_size == 0
           || (info.protection & required) != required) {
            return false;
        }

        start = region + region_size;
    }

    return true;
}

// Current protection of the [start, end) byte range, provided it is uniform
// across every VM region it crosses. A patch spanning regions with different
// protections cannot restore them all to one value, so a mixed range fails
// closed before anything is written.
static bool hk_range_protection(vm_address_t start, vm_address_t end, vm_prot_t *out) {
    vm_prot_t first = VM_PROT_NONE;
    bool have = false;

    while(start < end) {
        vm_address_t region = start;
        vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;

        kern_return_t kr = vm_region_64(mach_task_self(), &region, &region_size,
                                        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                        &count, &object);

        if(object != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object);
        }

        // A region starting past `start` means the range is unmapped there.
        if(kr != KERN_SUCCESS || region > start || region_size == 0) {
            return false;
        }

        if(!have) {
            first = info.protection;
            have = true;
        } else if(info.protection != first) {
            return false;   // mixed protections: fail closed
        }

        start = region + region_size;
    }

    *out = first;
    return have;
}

// Write `size` bytes over `dst`, which may be read-only or read/execute-only.
static kern_return_t hk_write(void *dst, const void *src, size_t size) {
    mach_port_t task = mach_task_self();
    vm_address_t start = 0;
    vm_address_t end = 0;
    vm_prot_t restore = VM_PROT_NONE;

    // Fail closed on arithmetic overflow: a wrapped range would protect (and
    // memcpy) the wrong bytes.
    if(!hk_range_bounds(dst, size, &start, &end)) {
        return KERN_INVALID_ADDRESS;
    }

    vm_size_t len = end - start;

    // Fail closed. Guessing R-X here would leave a patched data region
    // unwritable to its owner; guessing R-W would leave code writable.
    // The whole range must share one protection: restoring a single value
    // over differently protected regions would flatten them.
    if(!hk_range_protection(start, end, &restore)) {
        return KERN_INVALID_ADDRESS;
    }

    // VM_PROT_COPY forces the copy-on-write break, giving us a private dirty
    // page we are allowed to write to.
    kern_return_t kr = vm_protect(task, start, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

    if(kr == KERN_SUCCESS) {
        memcpy(dst, src, size);
        sys_icache_invalidate(dst, size);

        // The patch landed; only the restore can still fail. Report it --
        // claiming success would leave the page stuck at its patched
        // protection (e.g. code left non-executable).
        kr = vm_protect(task, start, len, FALSE, restore);

        if(kr != KERN_SUCCESS) {
            return kr;
        }

        return KERN_SUCCESS;
    }

    // Refused (arm64e under PPL is the usual reason): patch a private copy and
    // remap it over the target instead.
    vm_address_t tmp = 0;
    kr = vm_allocate(task, &tmp, len, VM_FLAGS_ANYWHERE);

    if(kr != KERN_SUCCESS) {
        return kr;
    }

    memcpy((void *)tmp, (const void *)start, len);
    memcpy((void *)(tmp + ((vm_address_t)dst - start)), src, size);

    kr = vm_protect(task, tmp, len, FALSE, restore);

    if(kr != KERN_SUCCESS) {
        vm_deallocate(task, tmp, len);
        return kr;
    }

    vm_address_t dest = start;
    vm_prot_t cur = VM_PROT_NONE;
    vm_prot_t max = VM_PROT_NONE;

    kr = vm_remap(task, &dest, len, 0, VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED,
                  task, tmp, FALSE, &cur, &max, VM_INHERIT_NONE);

    // The new mapping holds its own reference to the object, so dropping our
    // temporary mapping does not unmap what we just installed.
    vm_deallocate(task, tmp, len);

    if(kr == KERN_SUCCESS) {
        sys_icache_invalidate(dst, size);
    }

    return kr;
}

// True when every page of [addr, addr+len) lies inside a VM region with read
// permission. Walks regions instead of touching the range, so a bogus
// non-NULL address yields false instead of a fault. The preflight paths and
// the Swift metadata reader call this before dereferencing addresses that
// came from untrusted metadata.
bool hk_native_range_readable(const void *addr, size_t len) {
    vm_address_t start = 0;
    vm_address_t end = 0;

    if(!hk_range_bounds(addr, len, &start, &end)) {
        return false;
    }

    return hk_range_all_protected(start, end, VM_PROT_READ);
}

// Executable variant, for the code-patching paths: a function hook overwrites
// CODE, so a readable-only mapping (a data blob) is not a patchable prologue
// and must be refused instead of silently patched. Undeclared in hk_native.h
// (whose public probe is deliberately readable-only — the Swift metadata
// reader depends on that); the inline preflight declares it locally.
HK_INTERNAL bool hk_native_range_executable(const void *addr, size_t len) {
    vm_address_t start = 0;
    vm_address_t end = 0;

    if(!hk_range_bounds(addr, len, &start, &end)) {
        return false;
    }

    return hk_range_all_protected(start, end, VM_PROT_EXECUTE);
}

#pragma mark - Trampolines

// Worst case per hook: 4 relocated instructions at 24 bytes each, plus a
// 16-byte absolute jump back.
#define HK_TRAMPOLINE_SLOT 128

// Bump-allocated arena of executable slots. The lock guards this struct only —
// it says nothing about the safety of patching live code, which remains a
// load-time-only operation.
static struct {
    vm_address_t page;
    size_t size;
    size_t used;
    pthread_mutex_t lock;
} tramp = { 0, 0, 0, PTHREAD_MUTEX_INITIALIZER };

// Restore the arena to executable. The page holds every trampoline handed out
// so far, so it must go back to R-X on ANY exit path -- leaving it writable
// would strip execute permission from live hooks installed earlier.
static kern_return_t tramp_seal(void) {
    if(!tramp.page) {
        return KERN_SUCCESS;
    }

    return vm_protect(mach_task_self(), tramp.page, tramp.size, FALSE,
                      VM_PROT_READ | VM_PROT_EXECUTE);
}

// Returns a writable HK_TRAMPOLINE_SLOT-byte slot. Every successful call must
// be paired with tramp_commit (kept) or tramp_abort (discarded).
static void *tramp_alloc(void) {
    size_t page = (size_t)getpagesize();

    if(!tramp.page || tramp.used + HK_TRAMPOLINE_SLOT > tramp.size) {
        vm_address_t addr = 0;

        if(vm_allocate(mach_task_self(), &addr, page, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
            hk_errno = HK_NATIVE_ERR_NO_MEMORY;
            return NULL;
        }

        tramp.page = addr;
        tramp.size = page;
        tramp.used = 0;
    }

    kern_return_t kr = vm_protect(mach_task_self(), tramp.page, tramp.size, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE);

    if(kr != KERN_SUCCESS) {
        hk_errno = kr;
        return NULL;
    }

    return (void *)(tramp.page + tramp.used);
}

static bool tramp_commit(void *slot, size_t used) {
    kern_return_t kr = tramp_seal();

    if(kr != KERN_SUCCESS) {
        hk_errno = kr;
        return false;
    }

    tramp.used += HK_TRAMPOLINE_SLOT;
    sys_icache_invalidate(slot, used);
    return true;
}

// Give the slot back without publishing it, resealing the page.
static void tramp_abort(void) {
    tramp_seal();
}

#pragma mark - Hooking

// The engine's own capability gate, shared by hk_native_hook_function and the
// backend preflight so the two can never disagree on the checks they share.
int hk_native_preflight_function(void *target, void *replacement) {
    if(!target || !replacement) {
        return HK_NATIVE_ERR_UNSUPPORTED;
    }

    void *raw_target = hk_strip_code(target);
    void *raw_replacement = hk_strip_code(replacement);

    // AArch64 instructions are 4-byte aligned; a misaligned target is not an
    // instruction boundary, and patching it would smash whatever it points at.
    if(((uintptr_t)raw_target & 3u) != 0) {
        return HK_NATIVE_ERR_UNSUPPORTED;
    }

    // Hooking a function with itself is a no-op that would clobber the
    // original bytes with a branch back to the same place.
    if(raw_target == raw_replacement) {
        return HK_NATIVE_ERR_UNSUPPORTED;
    }

    uint64_t target_addr = (uint64_t)(uintptr_t)raw_target;
    size_t patch_bytes = hk_arm64_branch_size(target_addr, (uint64_t)(uintptr_t)raw_replacement);

    // The window scan below dereferences the target; a bogus non-NULL
    // address must fail cleanly instead of faulting.
    if(!hk_native_range_readable(raw_target, patch_bytes)) {
        return HK_NATIVE_ERR_UNREADABLE;
    }

    // A function hook overwrites CODE: a readable-only mapping (a data blob)
    // is not a patchable prologue. Same code as the unreadable case — both
    // are target-shape capability misses.
    if(!hk_native_range_executable(raw_target, patch_bytes)) {
        return HK_NATIVE_ERR_UNREADABLE;
    }

    // Mirror of the target checks: the replacement is branched to, so it must
    // sit on an instruction boundary and live in an executable mapping too (a
    // data blob cannot be jumped to). 4 bytes covers the branch-target
    // instruction; the page-rounded probe spans whatever it straddles.
    if(((uintptr_t)raw_replacement & 3u) != 0) {
        return HK_NATIVE_ERR_UNSUPPORTED;
    }

    if(!hk_native_range_executable(raw_replacement, 4)) {
        return HK_NATIVE_ERR_UNREADABLE;
    }

    const uint32_t *src = (const uint32_t *)raw_target;

    // A terminator before the last instruction we would overwrite means the
    // function ends inside the patch -- the remaining bytes belong to whatever
    // follows it. Refuse rather than corrupt an unrelated function. (The
    // helper recognizes the authenticated forms too: RETAA/RETAB/BRAA-family,
    // which arm64e micro-thunks use.) The last instruction is excluded
    // because it is fully replaced by the branch, exactly like the original
    // loop's `i + 1 < insn_count` bound.
    if(hk_arm64_has_early_terminator(src, patch_bytes - 4)) {
        return HK_NATIVE_ERR_SHORT_FUNCTION;
    }

    return 0;
}

bool hk_native_hook_function(void *target, void *replacement, void **out_orig) {
    int preflight_errno = hk_native_preflight_function(target, replacement);

    if(preflight_errno != 0) {
        hk_errno = preflight_errno;
        return false;
    }

    void *raw_target = hk_strip_code(target);
    void *raw_replacement = hk_strip_code(replacement);
    uint64_t target_addr = (uint64_t)(uintptr_t)raw_target;
    size_t patch_bytes = hk_arm64_branch_size(target_addr, (uint64_t)(uintptr_t)raw_replacement);
    size_t insn_count = patch_bytes / 4;
    const uint32_t *src = (const uint32_t *)raw_target;

    pthread_mutex_lock(&tramp.lock);

    uint32_t *slot = tramp_alloc();

    if(!slot) {
        pthread_mutex_unlock(&tramp.lock);
        return false;
    }

    size_t written = hk_arm64_relocate(src, target_addr, insn_count, slot,
                                       HK_TRAMPOLINE_SLOT - HK_A64_MAX_BRANCH_BYTES);

    if(written == 0) {
        tramp_abort();
        pthread_mutex_unlock(&tramp.lock);
        hk_errno = HK_NATIVE_ERR_RELOCATE;
        return false;
    }

    uint64_t slot_addr = (uint64_t)(uintptr_t)slot;
    written += hk_arm64_emit_branch(slot + (written / 4), slot_addr + written,
                                    target_addr + patch_bytes);

    // Seal the trampoline before patching the target: once the target branches
    // away, the trampoline must already be executable.
    if(!tramp_commit(slot, written)) {
        tramp_abort();
        pthread_mutex_unlock(&tramp.lock);
        return false;
    }

    pthread_mutex_unlock(&tramp.lock);

    // C1: hand the sealed trampoline to the caller BEFORE the write — the
    // original must be observable from the moment the patch lands, and a
    // crash mid-write must not lose it. (The trampoline is already sealed and
    // executable: tramp_commit ran above, before the target was touched.)
    if(out_orig) {
        *out_orig = hk_sign_code(slot);
    }

    uint32_t patch[HK_A64_MAX_BRANCH_BYTES / 4];
    size_t emitted = hk_arm64_emit_branch(patch, target_addr, (uint64_t)(uintptr_t)raw_replacement);
    kern_return_t kr = hk_write(raw_target, patch, emitted);

    if(kr != KERN_SUCCESS) {
        hk_errno = kr;
        return false;
    }

    hk_errno = 0;
    return true;
}

bool hk_native_patch_memory(void *target, const void *data, size_t size) {
    if(!target || !data || size == 0) {
        hk_errno = HK_NATIVE_ERR_UNSUPPORTED;
        return false;
    }

    // Patching a region with its own bytes is a no-op that can only fail.
    if(target == data) {
        hk_errno = HK_NATIVE_ERR_UNSUPPORTED;
        return false;
    }

    kern_return_t kr = hk_write(hk_strip_code(target), data, size);
    hk_errno = (kr == KERN_SUCCESS) ? 0 : kr;
    return kr == KERN_SUCCESS;
}

#else   // !arm64

bool hk_native_supported(void) {
    return false;
}

int hk_native_last_error(void) {
    return HK_NATIVE_ERR_UNSUPPORTED;
}

int hk_native_preflight_function(void *target, void *replacement) {
    (void)target; (void)replacement;
    return HK_NATIVE_ERR_UNSUPPORTED;
}

bool hk_native_hook_function(void *target, void *replacement, void **out_orig) {
    (void)target; (void)replacement; (void)out_orig;
    return false;
}

bool hk_native_patch_memory(void *target, const void *data, size_t size) {
    (void)target; (void)data; (void)size;
    return false;
}

bool hk_native_range_readable(const void *addr, size_t len) {
    (void)addr; (void)len;
    return false;
}

#endif
