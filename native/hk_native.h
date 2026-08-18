#ifndef hk_native_h
#define hk_native_h

// HookKit's own hooking engine: no ElleKit, Substrate or Substitute required.
//
// arm64/arm64e only. On armv7 every entry point degrades to "unsupported" --
// those devices always have Substrate, so no Thumb relocator is worth writing.
//
// Obtaining W^X is the load-bearing constraint. On A12+ the page tables are
// PPL-protected, so a dirty executable page needs relaxed codesigning. In a
// tweak-injected process that already holds: the injector had to relax it to
// load this dylib at all. Where it does not, every call fails cleanly.
//
// Hooks are still a load-time operation, the same assumption ElleKit and
// Substrate make, but the engine no longer relies on that alone:
//
//  - The entry patch is a single 4-byte B whenever a trampoline page can be
//    placed within +/-128MB of the target, so a thread ENTERING the function
//    mid-install sees either the old first instruction or the new branch.
//    Out of range it degrades to the 16-byte sequence, which is torn-visible.
//  - A thread already INSIDE the prologue is unrecoverable either way: no
//    non-quiescing inline hooker can fix that, and this one does not suspend
//    peer threads.
//  - Published trampolines are never made non-executable to build the next
//    one, so installing a hook cannot fault an unrelated hook that is running.
//  - Writes to live mappings are serialized process-wide, so concurrent
//    patches cannot race each other's protection flips.
//
// The load-time rule is stricter than "do not hook a running function": do not
// hook a function whose PAGE holds anything that is running. hk_write flips
// the target's page to read-write to get write access, dropping EXECUTE for
// the window, so an unrelated function sharing that page faults on instruction
// fetch too. Inherent — see the comment on hk_write.

#include <stdbool.h>
#include <stddef.h>

// Internal to the framework: HookKit's only exported symbol is _HKSubstitutor
// (see HookKit.tbd), and these must not join the dynamic export table.
#ifndef HK_INTERNAL
#define HK_INTERNAL __attribute__((visibility("hidden")))
#endif

// Error codes reported by hk_native_last_error() alongside raw kern_return_t
// values (which are positive).
#define HK_NATIVE_ERR_UNSUPPORTED    (-1)
#define HK_NATIVE_ERR_SHORT_FUNCTION (-2)   // target too short to patch without clobbering its neighbour
#define HK_NATIVE_ERR_RELOCATE       (-3)   // prologue contains something unrelocatable
#define HK_NATIVE_ERR_NO_MEMORY      (-4)
#define HK_NATIVE_ERR_UNREADABLE     (-5)   // target/range not mapped readable

// True when this build can hook at all (arm64/arm64e).
HK_INTERNAL bool hk_native_supported(void);

// Error from the most recent failing call. Process-wide, not per-thread: read
// it immediately after the call that failed.
HK_INTERNAL int hk_native_last_error(void);

// Side-effect-free capability preflight: exactly the checks the engine runs
// before writing (PAC strip, alignment, self-hook, short-function over the
// actual 4- or 16-byte branch window, with the final overwritten instruction
// excluded). Returns 0 when hk_native_hook_function would attempt the patch,
// otherwise an HK_NATIVE_ERR_* code. The engine's own hook path validates
// through this function, so a preflight accept and the hook can never
// disagree on the checks they share.
HK_INTERNAL int hk_native_preflight_function(void *target, void *replacement);

// True when the byte range [addr, addr+len) is mapped and readable in this
// process. Probes via a Mach VM region walk and never touches the range
// itself, so a bogus non-NULL address reports false instead of faulting.
// Used before dereferencing addresses derived from untrusted metadata
// (class pointers, prologue windows).
HK_INTERNAL bool hk_native_range_readable(const void *addr, size_t len);

// Inline function hook. On success *out_orig receives a callable pointer to the
// original implementation (PAC-signed on arm64e).
HK_INTERNAL bool hk_native_hook_function(void *target, void *replacement, void **out_orig);

// Raw memory patch, no relocation. The region's original protection is restored
// afterwards, so this is safe on data as well as code.
//
// Note the blast radius when the protection flip is refused (arm64e under
// PPL): the fallback rebuilds the whole page and swaps the mapping with
// vm_remap, so the unit of change is a page, not `size`. Fine for code being
// patched at load time; use hk_native_patch_pointer for anything live.
HK_INTERNAL bool hk_native_patch_memory(void *target, const void *data, size_t size);

// Single-copy-atomic store of one pointer into an aligned slot, for live
// metadata (the Swift vtable engine). Readers see either the old or the new
// pointer, never a mix and never an unmapped page — which is exactly what
// hk_native_patch_memory cannot promise, since its arm64e fallback swaps the
// whole page mapping out from under them. Fails rather than falling back to
// the remap, and fails on a misaligned slot.
HK_INTERNAL bool hk_native_patch_pointer(void *slot, void *value);

// Symbol lookup. Images must already be loaded -- an unloaded image has no
// runtime addresses to report.
typedef struct hk_image hk_image;

HK_INTERNAL hk_image *hk_native_open_image(const char *path);
HK_INTERNAL void hk_native_close_image(hk_image *image);
HK_INTERNAL void *hk_native_find_symbol(hk_image *image, const char *name);

// Fast NULL-image private-symbol lookup: one scan over the dyld shared
// cache's local-symbols table (all cached dylibs at once), instead of the
// per-image walk backends use. Returns the runtime address, or NULL when the
// symbol is not in the cache (exported symbols should be resolved with
// dlsym first — this covers only the private/non-exported ones). The table
// is mapped once for the process lifetime, so the scan is safe concurrently.
HK_INTERNAL void *hk_native_find_cache_symbol(const char *name);

// Scan-path variant of hk_native_open_image: same lookup minus the dlopen
// fallback handle (dlsym already missed before a NULL-image scan runs, and
// dlopen+dlclose per image dominated the scan's cost). For the NULL-image
// private-symbol scan call sites only.
HK_INTERNAL hk_image *hk_native_open_image_scan(const char *path);

#endif
