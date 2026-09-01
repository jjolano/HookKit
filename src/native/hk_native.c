#include "hk_native.h"
#include "../internal/HKPointerAuth.h"

#if defined(__arm64__) || defined(__aarch64__)

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define hk_strip_code(p) ((void *)hk_pac_strip_code((uintptr_t)(p)))

// Tag the anonymous trampoline mapping so a vm_region walk sees a named,
// expected owner -- a JIT executable allocator, the kind ubiquitous JS engines
// create -- rather than an untagged anonymous executable region, which is the
// shape a hooking scan keys on. This only relabels the mapping; it is still
// anonymous, which is exactly why the static-continuation pool exists for
// callers that must avoid an anonymous executable region altogether.
#if defined(VM_MEMORY_JAVASCRIPT_JIT_EXECUTABLE_ALLOCATOR)
#define HK_TRAMP_VM_TAG VM_MEMORY_JAVASCRIPT_JIT_EXECUTABLE_ALLOCATOR
#else
#define HK_TRAMP_VM_TAG VM_MEMORY_APPLICATION_SPECIFIC_1
#endif

bool hk_native_supported(void) {
    return true;
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

// One-region memo for the probe walks. A single inline hook probes the same
// code region 6-12 times (basic checks, window scan, the engine's own
// preflight re-probing the same addresses), and each probe is a vm_region_64
// trap plus a port deallocate — the dominant per-hook cost on the inline
// paths. A probe fully inside the memoized region with the needed bits
// granted skips the trap entirely; a walk refreshes the memo.
//
// Thread-local, and load-bearing that it is: the five fields are written
// without barriers, and the fast path reads `valid` FIRST, so a shared memo
// lets a reader pair one region's base/end with another's protection. That
// torn read would report an unmapped range as protected — inside the very
// probe whose job is to stop the window scanners from dereferencing it. Per
// thread there is one writer, so no pairing is possible. Sharing the memo
// across threads buys nothing anyway: probe bursts are per hook install.
// ponytail: advisory, single entry — a region unmapped and re-queried between
// probes returns stale "protected". The probes gate a dereference that
// happens microseconds later on live code; a concurrent unmap of the target
// in that window is already a race the walk itself does not close. An
// address→region map is the upgrade if probe traffic ever outgrows one entry.
static _Thread_local struct {
    vm_address_t base;
    vm_address_t end;        // base + size
    vm_prot_t protection;
    bool valid;
} hk_memo_region;

// True when every VM region covering [start, end) grants ALL of `required`
// protections. Per-region, not uniform: mixed regions pass as long as each
// grants the bits (the readable probe's documented contract). start/end are
// page-aligned, caller-validated bounds.
static bool hk_range_all_protected(vm_address_t start, vm_address_t end, vm_prot_t required) {
    if(hk_memo_region.valid
       && start >= hk_memo_region.base && end <= hk_memo_region.end
       && (hk_memo_region.protection & required) == required) {
        return true;
    }

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
            // Observed a mapping change (unmapped region, or a region that
            // lacks the bits): drop any memoized region so it is never served.
            hk_memo_region.valid = false;
            return false;
        }

        // Single region covers the whole range: memoize it for the next probe.
        if(region + region_size >= end) {
            hk_memo_region.base = region;
            hk_memo_region.end = region + region_size;
            hk_memo_region.protection = info.protection;
            hk_memo_region.valid = true;
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

// Serializes every write to a live mapping. hk_write is the single choke
// point for hook installs, memory patches and Swift vtable slots, so one lock
// here covers all of them. Without it two patches landing on the same page
// race the protect/restore pair — the second memcpy hits a page the first has
// already resealed read-execute, which faults — and on the remap path both
// threads snapshot the page and the second remap silently discards the first
// patch, leaving a hook whose caller holds a live trampoline but whose target
// was never redirected.
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;

// Write `size` bytes over `dst`, which may be read-only or read/execute-only.
// Callers go through hk_write; this is the body, run under g_write_lock.
//
// Unavoidable hazard, stated once here because every caller inherits it: the
// VM_PROT_COPY flip below drops EXECUTE from the target's whole page for the
// duration of the memcpy. Anything else on that page that runs in the window
// faults on instruction fetch — not just the function being patched. There is
// no way around it on iOS: the alternative remap path leaves the range briefly
// unmapped instead, and a page left read-write-execute is refused at
// instruction fetch whatever vm_protect returned. This is the real content of
// "hooks must be installed at load time".
static kern_return_t hk_write_locked(void *dst, const void *src, size_t size) {
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

static kern_return_t hk_write(void *dst, const void *src, size_t size) {
    pthread_mutex_lock(&g_write_lock);
    kern_return_t kr = hk_write_locked(dst, src, size);
    pthread_mutex_unlock(&g_write_lock);
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

// ONE PAGE PER TRAMPOLINE, and that is the whole fix for the crash this
// replaced. The previous design bump-allocated slots out of a shared arena
// and flipped the entire arena to R-W to build each new trampoline, stripping
// EXECUTE from up to 127 already-published trampolines for the duration: any
// thread running an earlier hook during that window faulted on instruction
// fetch. Giving each trampoline its own page means the R-W -> R-X seal can
// only ever touch a page that has published nothing.
//
// Keeping one arena permanently R-W-X instead does NOT work on iOS, and it
// fails in a way that looks like it works: vm_protect(RWX) returns
// KERN_SUCCESS, and then the first instruction fetch from the page dies with
// KERN_PROTECTION_FAILURE, PC equal to the fault address. The kernel refuses
// to execute a mapping that is simultaneously writable, whatever vm_protect
// said. The R-W -> R-X transition below is the one iOS actually honours.
//
// ponytail: 16KB per native inline hook, against 128 bytes before. The native
// engine is opt-in and hooks arrive in handfuls, so a few hundred KB is the
// realistic worst case. The upgrade, if a consumer ever installs enough
// native inline hooks for that to matter, is a write-alias: map the arena
// twice through one memory object, write through the R-W view and execute
// through the R-X one, so packing returns without either view ever being
// both. Do not attempt it without testing on a PPL device (A12+) — that is
// exactly where the kernel is most likely to refuse the executable view.
// Allocate `size` bytes, preferring a location within +/-128MB of `anchor` so
// the entry patch can be a single 4-byte B. Falls back to anywhere, which
// costs the 16-byte non-atomic patch but is never wrong. Returns 0 on failure.
// ponytail: searches upward only — iOS leaves large stretches of free VA above
// the shared cache and above loaded images, so the first hole is a few probes
// away. Add a downward walk if a real target ever turns up with 128MB of
// solid mappings above it.
static vm_address_t tramp_map_near(uint64_t anchor, size_t size) {
    mach_port_t task = mach_task_self();
    vm_address_t addr = 0;
    vm_address_t probe = (vm_address_t)((anchor + size - 1) & ~((uint64_t)size - 1));
    vm_address_t limit = (vm_address_t)(anchor + (1ULL << 27));   // B reaches +/-128MB

    while(probe && probe + size > probe && probe + size <= limit) {
        vm_address_t region = probe;
        vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;

        kern_return_t kr = vm_region_64(task, &region, &region_size,
                                        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                        &count, &object);

        if(object != MACH_PORT_NULL) {
            mach_port_deallocate(task, object);
        }

        // Nothing mapped from here on, or the next mapping starts far enough
        // past `probe` to leave the hole we need: claim it.
        if(kr != KERN_SUCCESS || region >= probe + size) {
            addr = probe;

            if(vm_allocate(task, &addr, size,
                           VM_FLAGS_FIXED | VM_MAKE_TAG(HK_TRAMP_VM_TAG)) == KERN_SUCCESS) {
                return addr;
            }

            // Lost the hole to another mapper; step over it and keep looking.
            probe += size;
            continue;
        }

        probe = region + region_size;
    }

    addr = 0;

    if(vm_allocate(task, &addr, size,
                   VM_FLAGS_ANYWHERE | VM_MAKE_TAG(HK_TRAMP_VM_TAG)) != KERN_SUCCESS) {
        return 0;
    }

    return addr;
}

uintptr_t hk_native_reloc_alloc(size_t size, uintptr_t near) {
    size_t page = (size_t)getpagesize();
    if (size == 0 || size > page) {
        return 0;
    }
    return (uintptr_t)tramp_map_near((uint64_t)near, page);
}

bool hk_native_reloc_seal(uintptr_t page, size_t size) {
    (void)size;
    if (!page) {
        return false;
    }
    kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)page,
                                  (vm_size_t)getpagesize(), FALSE,
                                  VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr == KERN_SUCCESS) {
        sys_icache_invalidate((void *)page, (size_t)getpagesize());
    }
    return kr == KERN_SUCCESS;
}

void hk_native_reloc_free(uintptr_t page, size_t size) {
    (void)size;
    if (page) {
        (void)vm_deallocate(mach_task_self(), (vm_address_t)page,
                            (vm_size_t)getpagesize());
    }
}

bool hk_native_reloc_unprotect(uintptr_t page, size_t size) {
    (void)size;
    if (!page) {
        return false;
    }
    // VM_PROT_COPY forces a copy-on-write break, which is what makes a pool
    // slot in a code-signed __TEXT page (max_protection R-X, so a plain
    // R-W raise is refused) privately writable. Without it this returns
    // KERN_PROTECTION_FAILURE and the trampoline can never be built.
    kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)page,
                                  (vm_size_t)getpagesize(), FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    return kr == KERN_SUCCESS;
}

#pragma mark - Patching

bool hk_native_patch_memory(void *target, const void *data, size_t size) {
    if(!target || !data || size == 0) {
        return false;
    }

    // Patching a region with its own bytes is a no-op that can only fail.
    if(target == data) {
        return false;
    }

    kern_return_t kr = hk_write(hk_strip_code(target), data, size);
    return kr == KERN_SUCCESS;
}

bool hk_native_patch_pointer(void *slot, void *value) {
    // Alignment is the whole contract: an unaligned store is not single-copy
    // atomic, and a reader could observe half of each pointer.
    if(!slot || ((uintptr_t)slot & (sizeof(void *) - 1)) != 0) {
        return false;
    }

    vm_address_t start = 0;
    vm_address_t end = 0;
    vm_prot_t restore = VM_PROT_NONE;

    if(!hk_range_bounds(slot, sizeof(void *), &start, &end)) {
        return false;
    }

    if(!hk_range_protection(start, end, &restore)) {
        return false;
    }

    mach_port_t task = mach_task_self();
    vm_size_t len = end - start;

    pthread_mutex_lock(&g_write_lock);

    kern_return_t kr = vm_protect(task, start, len, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

    if(kr == KERN_SUCCESS) {
        __atomic_store_n((void **)slot, value, __ATOMIC_RELEASE);
        kr = vm_protect(task, start, len, FALSE, restore);
    }

    pthread_mutex_unlock(&g_write_lock);

    return kr == KERN_SUCCESS;
}

#else   // !arm64

bool hk_native_supported(void) {
    return false;
}

bool hk_native_patch_memory(void *target, const void *data, size_t size) {
    (void)target; (void)data; (void)size;
    return false;
}

bool hk_native_patch_pointer(void *slot, void *value) {
    (void)slot; (void)value;
    return false;
}

bool hk_native_range_readable(const void *addr, size_t len) {
    (void)addr; (void)len;
    return false;
}

uintptr_t hk_native_reloc_alloc(size_t size, uintptr_t near) {
    (void)size; (void)near;
    return 0;
}

bool hk_native_reloc_seal(uintptr_t page, size_t size) {
    (void)page; (void)size;
    return false;
}

void hk_native_reloc_free(uintptr_t page, size_t size) {
    (void)page; (void)size;
}

bool hk_native_reloc_unprotect(uintptr_t page, size_t size) {
    (void)page; (void)size;
    return false;
}

#endif
