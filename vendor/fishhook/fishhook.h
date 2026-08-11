// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef fishhook_h
#define fishhook_h

#include <stddef.h>
#include <stdint.h>

#if !defined(FISHHOOK_EXPORT)
#define FISHHOOK_VISIBILITY __attribute__((visibility("hidden")))
#else
#define FISHHOOK_VISIBILITY __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

/*
 * A structure representing a particular intended rebinding from a symbol
 * name to its replacement
 */
struct rebinding {
  const char *name;
  void *replacement;
  void **replaced;
};

/*
 * Outcome tally for a rebind call. matched counts the indirect symbol slots
 * actually rewritten; failed counts matching slots whose write had to be
 * skipped because the section's pages could not be made writable (never
 * written — a skipped write is reported, not attempted blind). A call where
 * failed > 0 is a partial rebind.
 */
struct rebind_stats {
  uint32_t matched;
  uint32_t failed;
  // Set (1) when at least one section's original page protection could not
  // be restored after its slots were written: those pages may remain
  // writable, so matched > 0 && restore_failed is a hard/partial result,
  // not a clean success.
  uint32_t restore_failed;
};

/*
 * Outcome tally for rebind_symbols_hook: matched counts the indirect symbol
 * slots actually rewritten; failed counts matching slots whose write had to
 * be skipped because the section's pages could not be made writable.
 */
struct rebind_result {
  size_t matched;
  size_t failed;
  // Set (1) when at least one section's original page protection could not
  // be restored after its slots were written (see
  // struct rebind_stats.restore_failed).
  size_t restore_failed;
};

/*
 * Called by rebind_symbols_hook with the caller's original implementation
 * pointer (resigned to the standard C function-pointer scheme on arm64e)
 * BEFORE the first replacement slot of a rebind goes live, so the caller can
 * publish its original before any caller of the hooked symbol can observe the
 * replacement. Invoked at most once per rebind entry, on the first writable
 * matching slot; later matching slots and future image loads skip it.
 */
typedef void (*rebind_publish_fn)(void *context, void *original);

/*
 * For each rebinding in rebindings, rebinds references to external, indirect
 * symbols with the specified name to instead point at replacement for each
 * image in the calling process as well as for all future images that are loaded
 * by the process. If rebind_functions is called more than once, the symbols to
 * rebind are added to the existing list of rebindings, and if a given symbol
 * is rebound more than once, the later rebinding will take precedence.
 */
FISHHOOK_VISIBILITY
int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel);

/*
 * Like rebind_symbols, but additionally reports in outMatched how many
 * indirect symbol slots were actually rewritten in the currently loaded
 * images. A count of 0 means the names matched no loaded reference, which
 * lets callers distinguish a real rebinding from a silent no-op. outMatched
 * may be NULL (then this behaves exactly like rebind_symbols).
 */
FISHHOOK_VISIBILITY
int rebind_symbols_checked(struct rebinding rebindings[],
                           size_t rebindings_nel,
                           size_t *outMatched);

/*
 * Like rebind_symbols, but additionally reports in outStats how many slots
 * were rewritten (matched) and how many matching slots could not be written
 * (failed, see struct rebind_stats). outStats may be NULL (then this behaves
 * exactly like rebind_symbols).
 */
FISHHOOK_VISIBILITY
int rebind_symbols_stats(struct rebinding rebindings[],
                         size_t rebindings_nel,
                         struct rebind_stats *outStats);

/*
 * Like rebind_symbols_stats, but additionally solves the C1 ordering
 * problem: for the FIRST writable matching slot of each rebinding entry the
 * original pointer is captured (and, on arm64e, resigned) and handed to
 * publish(context, original) BEFORE the replacement is written into the
 * slot — the caller's original is therefore observable no later than the
 * first replacement. Subsequent matching slots skip the callback, and the
 * callback/context are cleared once the initial scan finishes, so future
 * image loads apply the rebind silently. result may be NULL (matched/failed
 * are then not reported); publish may be NULL (then this behaves exactly
 * like rebind_symbols_stats).
 */
FISHHOOK_VISIBILITY
int rebind_symbols_hook(struct rebinding rebindings[],
                        size_t count,
                        struct rebind_result *result,
                        rebind_publish_fn publish,
                        void *context);

/*
 * Rebinds as above, but only in the specified image. The header should point
 * to the mach-o header, the slide should be the slide offset. Others as above.
 */
FISHHOOK_VISIBILITY
int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel);

/*
 * Like rebind_symbols_image, but additionally reports matched/failed slot
 * counts in outStats (see struct rebind_stats). outStats may be NULL (then
 * this behaves exactly like rebind_symbols_image).
 */
FISHHOOK_VISIBILITY
int rebind_symbols_image_stats(void *header,
                               intptr_t slide,
                               struct rebinding rebindings[],
                               size_t rebindings_nel,
                               struct rebind_stats *outStats);

/*
 * Removes the entries previously added by rebind_symbols /
 * rebind_symbols_checked, matched by the exact name/replacement/replaced
 * pointers stored at that time (each caller's pointers are unique per call).
 * The entry storage is freed; the caller keeps ownership of the pointed-to
 * name/replaced memory. Returns 0 if any entry was removed, -1 if none
 * matched. Lets a caller that detected a no-op (matched == 0) undo the
 * registration so it leaves no state for future image loads.
 */
FISHHOOK_VISIBILITY
int rebind_symbols_unbind(struct rebinding rebindings[],
                          size_t rebindings_nel);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //fishhook_h

