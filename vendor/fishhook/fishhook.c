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

#include "fishhook.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#if __has_include(<ptrauth.h>)
#include <ptrauth.h>
#endif

#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

// On iOS 14+ arm64e, authenticated pointer sections (__auth_got) live in the
// __AUTH_CONST segment, which stock fishhook never scans — symbols there
// silently never rebind. Scan it too; non-arm64e binaries simply have no such
// segment.
#ifndef SEG_AUTH_CONST
#define SEG_AUTH_CONST  "__AUTH_CONST"
#endif

struct rebindings_entry {
  struct rebinding *rebindings;
  size_t rebindings_nel;
  struct rebindings_entry *next;
  // Number of indirect symbol slots this entry actually rewrote across all
  // images. Filled by rebind_symbols_checked() so callers can distinguish a
  // real rebinding from a silent no-op (symbol referenced by no loaded
  // image).
  size_t matched;
  // Number of matching slots whose write had to be skipped because the
  // section's pages could not be made writable. Filled alongside matched so
  // callers can detect a partial rebind (see struct rebind_stats).
  size_t failed;
  // Set when at least one section's original protection could not be
  // restored after its slots were written (see struct rebind_stats
  // restore_failed). Sticky: ORs across every section and image this entry
  // is applied to.
  bool restore_failed;
  // C1 publish state (rebind_symbols_hook only): the callback receives the
  // caller's original pointer exactly once, on the first writable matching
  // slot, before the replacement write. Cleared (publish/publish_context
  // NULLed) once the initial scan finishes so future image loads apply the
  // rebind silently.
  rebind_publish_fn publish;
  void *publish_context;
  bool published;
  // Per-rebinding publish cells (rebind_symbols_hook_batch only): a parallel
  // array of rebindings_nel void** cells. When present, the caller's original
  // is written into publish_cells[j] at the same match site as
  // rebindings[j].replaced — BEFORE the slot goes live — so a batch of N
  // rebindings publishes each caller's original mid-walk. The entry-level
  // `publish` callback fires only once per entry and cannot serve a batch.
  // Owned by the entry (malloc'd copy); freed and NULLed after the initial
  // scan, exactly like `publish`, so future image loads never touch a
  // borrowed (now-invalid) caller cell.
  void ***publish_cells;
};

// Serializes the global rebinding list against concurrent rebind_symbols
// calls and dyld's _dyld_register_func_for_add_image callback, which walks
// the same list on its own thread. Recursive: the first rebind_symbols call
// registers the callback, which dyld fires synchronously for existing
// images.
static pthread_mutex_t rebindings_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static struct rebindings_entry *_rebindings_head;

// True once _dyld_register_func_for_add_image has been called, set under
// rebindings_lock in rebind_symbols_common. Guards against re-registering
// the callback when a sole (e.g. zero-match, unbound) entry is removed and
// a later rebind_symbols call finds the list empty again: registering twice
// makes dyld fire the callback for every already-loaded image again, so the
// new rebinding would be applied twice.
static bool add_image_callback_registered = false;

static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                              struct rebinding rebindings[],
                              size_t nel,
                              void **publish_cells[]) {
  struct rebindings_entry *new_entry = (struct rebindings_entry *) malloc(sizeof(struct rebindings_entry));
  if (!new_entry) {
    return -1;
  }
  new_entry->rebindings = (struct rebinding *) malloc(sizeof(struct rebinding) * nel);
  if (!new_entry->rebindings) {
    free(new_entry);
    return -1;
  }
  new_entry->publish_cells = NULL;
  if (publish_cells) {
    new_entry->publish_cells = (void ***) malloc(sizeof(void **) * nel);
    if (!new_entry->publish_cells) {
      free(new_entry->rebindings);
      free(new_entry);
      return -1;
    }
    memcpy(new_entry->publish_cells, publish_cells, sizeof(void **) * nel);
  }
  memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
  new_entry->rebindings_nel = nel;
  // Zero-init so a fresh entry deterministically reports "nothing matched"
  // through outMatched; a garbage count would look like a real rebinding.
  // The counters accumulate across future image loads, so they are only
  // reset here, at allocation.
  new_entry->matched = 0;
  new_entry->failed = 0;
  new_entry->restore_failed = false;
  // No publish callback by default; rebind_symbols_hook fills it after
  // prepend and clears it again once the initial scan is done.
  new_entry->publish = NULL;
  new_entry->publish_context = NULL;
  new_entry->published = false;
  new_entry->next = *rebindings_head;
  *rebindings_head = new_entry;
  return 0;
}

static int get_protection(void *addr, vm_prot_t *prot, vm_prot_t *max_prot,
                          vm_address_t *region_addr, vm_size_t *region_size) {
  mach_port_t task = mach_task_self();
  vm_size_t size = 0;
  vm_address_t address = (vm_address_t)addr;
  memory_object_name_t object = MACH_PORT_NULL;
  kern_return_t info_ret;
#ifdef __LP64__
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  vm_region_basic_info_data_64_t info;
  info_ret = vm_region_64(
      task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
#else
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT;
  vm_region_basic_info_data_t info;
  info_ret = vm_region(task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &count, &object);
#endif
  if (info_ret == KERN_SUCCESS) {
    if (prot != NULL)
      *prot = info.protection;

    if (max_prot != NULL)
      *max_prot = info.max_protection;

    if (region_addr != NULL)
      *region_addr = address;

    if (region_size != NULL)
      *region_size = size;
  }

  if (object != MACH_PORT_NULL) {
    mach_port_deallocate(task, object);
  }

  return info_ret == KERN_SUCCESS ? 0 : -1;
}

static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab,
                                           char *strtab,
                                           uint32_t *indirect_symtab) {
  uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
  void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);

  // On arm64e, entries in __auth_got hold pointers signed with the asia key,
  // discriminated by the address of the slot itself (addrDiv in the chained
  // fixup encoding). dyld signs with ptrauth_sign_unauthenticated(ptr, asia,
  // blend(&slot, 0)), and callers authenticate with the same discriminator.
  // A raw write into an auth slot crashes at the caller's auth branch, so
  // resign here; writing a signed pointer into a plain slot is harmless
  // (plain br/blr ignores PAC bits). On non-arm64e the whole block compiles
  // out and behavior is stock.
#if __has_feature(ptrauth_calls)
  bool section_needs_auth = strcmp(section->sectname, "__auth_got") == 0;
#endif

  // H5: never leak VM_PROT_COPY|RW. Record the original protection of the VM
  // region covering the section, make the section writable, write, and
  // restore the original protection on every path out of this function. If
  // the region cannot be queried, or the section crosses into a neighbour
  // region (whose protection would be a guess), fail closed: no slot of this
  // section is written, and every matching slot is counted as failed so the
  // caller sees the partial rebind.
  vm_prot_t original_protection = VM_PROT_NONE;
  vm_address_t region_addr = 0;
  vm_size_t region_size = 0;
  bool writable = false;
  if (get_protection(indirect_symbol_bindings, &original_protection, NULL,
                     &region_addr, &region_size) == 0 &&
      (uintptr_t)indirect_symbol_bindings + section->size <= (uintptr_t)region_addr + region_size) {
    /**
     * 1. Moved the vm protection modifying codes to here to reduce the
     *    changing scope.
     * 2. Adding VM_PROT_WRITE mode unconditionally because vm_region
     *    API on some iOS/Mac reports mismatch vm protection attributes.
     * -- Lianfu Hao Jun 16th, 2021
     *
     * Once we failed to change the vm protection, we
     * MUST NOT continue the following write actions!
     * iOS 15 has corrected the const segments prot.
     * -- Lionfore Hao Jun 11th, 2021
     **/
    writable = vm_protect(mach_task_self(), (uintptr_t)indirect_symbol_bindings,
                          section->size, 0,
                          VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS;
  }

  // Snapshot the chain-head entry's match tally before this section's scan:
  // a restore failure below is only attributable to the owning call if its
  // entry actually wrote slots in this section (older entries' owners have
  // already settled).
  size_t head_matched_before = rebindings ? rebindings->matched : 0;

  for (uint i = 0; i < section->size / sizeof(void *); i++) {
    uint32_t symtab_index = indirect_symbol_indices[i];
    if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
        symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
      continue;
    }
    uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
    char *symbol_name = strtab + strtab_offset;
    bool symbol_name_longer_than_1 = symbol_name[0] && symbol_name[1];
    struct rebindings_entry *cur = rebindings;
    while (cur) {
      for (uint j = 0; j < cur->rebindings_nel; j++) {
        if (symbol_name_longer_than_1 && strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
          if (!writable) {
            // The slot matched but the section's pages could not be made
            // writable: skip the write (never write into a region that was
            // not unprotected) and report the failure so callers can detect
            // a partial rebind.
            cur->failed += 1;
            goto symbol_loop;
          }

          // C1: the caller's original must be observable no later than the
          // first replacement slot that goes live. Publish it BEFORE the
          // write below — exactly once per entry; subsequent matching slots
          // skip the callback (the caller's cell is already filled). The
          // recursive lock makes a re-entrant fishhook call from the
          // callback safe.
          if (!cur->published) {
            cur->published = true;
            if (cur->publish) {
#if __has_feature(ptrauth_calls)
              // The slot holds a pointer signed with the slot address as
              // discriminator; resign with diversity 0, the standard C
              // function pointer scheme (same value the replaced cell would
              // receive).
              void *original = ptrauth_sign_unauthenticated(
                  ptrauth_strip(indirect_symbol_bindings[i], ptrauth_key_asia),
                  ptrauth_key_asia, 0);
#else
              void *original = indirect_symbol_bindings[i];
#endif
              cur->publish(cur->publish_context, original);
            }
          }

          if (cur->rebindings[j].replaced != NULL && indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
#if __has_feature(ptrauth_calls)
            // The slot holds a pointer signed with the slot address as
            // discriminator — not directly callable as a plain C function
            // pointer (the compiler emits blraaz, asia/div-0). Resign with
            // diversity 0, the standard C function pointer scheme.
            *(cur->rebindings[j].replaced) = ptrauth_sign_unauthenticated(
                ptrauth_strip(indirect_symbol_bindings[i], ptrauth_key_asia),
                ptrauth_key_asia, 0);
#else
            *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
#endif
          }

          // Batch publication (rebind_symbols_hook_batch): write the caller's
          // original into its per-rebinding publish cell, BEFORE the slot is
          // rewritten below, so the original is observable the instant the
          // replacement goes live — the per-rebinding equivalent of the
          // entry-level `publish` callback. Same anti-double-apply guard as
          // the replaced write: skip when the slot already holds the
          // replacement (a re-application would otherwise publish the
          // replacement as the "original").
          if (cur->publish_cells != NULL && cur->publish_cells[j] != NULL
              && indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
#if __has_feature(ptrauth_calls)
            *(cur->publish_cells[j]) = ptrauth_sign_unauthenticated(
                ptrauth_strip(indirect_symbol_bindings[i], ptrauth_key_asia),
                ptrauth_key_asia, 0);
#else
            *(cur->publish_cells[j]) = indirect_symbol_bindings[i];
#endif
          }
#if __has_feature(ptrauth_calls)
          void *replacement = cur->rebindings[j].replacement;
          if (section_needs_auth && replacement != NULL) {
            // Strip first: a caller may pass an already-signed pointer.
            // Sign with the slot address as discriminator, as dyld does.
            replacement = ptrauth_sign_unauthenticated(
                ptrauth_strip(replacement, ptrauth_key_asia),
                ptrauth_key_asia, (uintptr_t)&indirect_symbol_bindings[i]);
          }
          indirect_symbol_bindings[i] = replacement;
#else
          indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
#endif
          cur->matched += 1;
          goto symbol_loop;
        }
      }
      cur = cur->next;
    }
  symbol_loop:;
  }

  // Restore the section's original protection — VM_PROT_COPY|RW must never
  // leak onto pages that were read-only before the rebind.
  if (writable) {
    kern_return_t restore_err = vm_protect(mach_task_self(), (uintptr_t)indirect_symbol_bindings,
                                           section->size, 0, original_protection);
    if (restore_err != KERN_SUCCESS && rebindings && rebindings->matched != head_matched_before) {
      // The writes succeeded but the pages stay writable: report a
      // hard/partial result instead of masking it as success. The entry is
      // retained — the mutation already happened and future-image state is
      // unchanged.
      rebindings->restore_failed = true;
    }
  }
}

static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide) {
  // NOTE: the stock fishhook validates the header with dladdr() here, but
  // that call is dead weight (the Dl_info is never used — every caller
  // supplies a header from dyld itself) and it is a self-hosting hazard:
  // once this library's own dladdr import slot is rebound, the validation
  // re-enters the replacement. If that replacement's original is published
  // only after the scan completes (batched/deferred publication), the
  // re-entry jumps through a NULL original (PC=0 SIGSEGV, observed
  // on-device: ShadowCore ctor → hookFunction:dladdr → rebind scan →
  // replaced_dladdr → original_dladdr still NULL). dyld-provided headers
  // are always valid, so the guard is unnecessary — drop it.
  if (!header) {
    return;
  }

  segment_command_t *cur_seg_cmd;
  segment_command_t *linkedit_segment = NULL;
  struct symtab_command* symtab_cmd = NULL;
  struct dysymtab_command* dysymtab_cmd = NULL;

  uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
        linkedit_segment = cur_seg_cmd;
      }
    } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
      symtab_cmd = (struct symtab_command*)cur_seg_cmd;
    } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
      dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
    }
  }

  if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
      !dysymtab_cmd->nindirectsyms) {
    return;
  }

  // Find base symbol/string table addresses
  uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
  nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
  char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);

  // Get indirect symbol table (array of uint32_t indices into symbol table)
  uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

  cur = (uintptr_t)header + sizeof(mach_header_t);
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
          strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0 &&
          strcmp(cur_seg_cmd->segname, SEG_AUTH_CONST) != 0) {
        continue;
      }
      for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
        section_t *sect =
          (section_t *)(cur + sizeof(segment_command_t)) + j;
        if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
        if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
      }
    }
  }
}

static void _rebind_symbols_for_image(const struct mach_header *header,
                                      intptr_t slide) {
    pthread_mutex_lock(&rebindings_lock);
    rebind_symbols_for_image(_rebindings_head, header, slide);
    pthread_mutex_unlock(&rebindings_lock);
}

// Shared core of rebind_symbols / rebind_symbols_checked /
// rebind_symbols_stats / rebind_symbols_hook: prepends the rebindings,
// applies them to every currently loaded image, and reports how many
// indirect symbol slots were actually rewritten (0 = the names matched no
// loaded reference — a silent no-op the caller can now detect), how many
// matching slots could not be written, and whether any section's protection
// could not be restored after a write. Every out-param may be NULL. When
// publish is non-NULL the entry carries it (with context) through the
// initial scan so the C1 publish fires on the first writable match; it is
// cleared again before returning so future image loads never re-invoke it.
static int rebind_symbols_common(struct rebinding rebindings[],
                                 size_t rebindings_nel,
                                 size_t *outMatched,
                                 struct rebind_stats *outStats,
                                 struct rebind_result *outResult,
                                 rebind_publish_fn publish,
                                 void *publish_context,
                                 void **publish_cells[]) {
  int retval;

  pthread_mutex_lock(&rebindings_lock);

  retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel, publish_cells);
  if (retval < 0) {
    pthread_mutex_unlock(&rebindings_lock);
    return retval;
  }

  // Pin the entry we just prepended: the scans below can invoke the publish
  // callback (arbitrary caller code), which may re-enter fishhook and
  // prepend a NEW head entry. Everything this call settles — the
  // matched/failed/restore_failed accounting and the publish-context
  // cleanup — must use this pinned pointer, never a mid-call re-read of
  // _rebindings_head (which would report/clean the wrong entry and could
  // retain a stack-allocated publish context on ours).
  struct rebindings_entry *entry = _rebindings_head;
  entry->publish = publish;
  entry->publish_context = publish_context;

  // The add-image callback is registered exactly once, guarded by a flag
  // under this lock — not by the "_rebindings_head->next is NULL" heuristic.
  // That heuristic re-registered the callback when a sole entry (e.g. a
  // zero-match rebind later removed by rebind_symbols_unbind) left the list
  // empty before a later call; dyld then fired the callback for every
  // already-loaded image again and the new rebinding was applied twice.
  // Registration invokes the callback synchronously for existing images, so
  // the first call needs no manual walk; every later call applies its new
  // entry (and the earlier ones, which are re-applied harmlessly) to the
  // already-loaded images directly.
  if (!add_image_callback_registered) {
    // Set the flag BEFORE registering: registration fires the callback
    // synchronously for already-loaded images, and the flag must already be
    // set so that synchronous invocation (or anything it re-enters) cannot
    // register a second callback.
    add_image_callback_registered = true;
    _dyld_register_func_for_add_image(_rebind_symbols_for_image);
  } else {
    uint32_t c = _dyld_image_count();
    for (uint32_t i = 0; i < c; i++) {
      // Scan with the pinned entry (the lock is already held): a reentrant
      // prepend must not redirect this walk's remaining images to the new
      // entry — its own call applies it.
      rebind_symbols_for_image(entry, _dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }
  }

  if (outMatched) {
    *outMatched = entry->matched;
  }

  if (outStats) {
    outStats->matched = (uint32_t)entry->matched;
    outStats->failed = (uint32_t)entry->failed;
    outStats->restore_failed = entry->restore_failed ? 1 : 0;
  }

  if (outResult) {
    outResult->matched = entry->matched;
    outResult->failed = entry->failed;
    outResult->restore_failed = entry->restore_failed ? 1 : 0;
  }

  // C1: no dangling publish state — future image loads (add-image callback)
  // must apply the entry silently, never re-invoking the caller's callback
  // or touching its context. Clean the PINNED entry: a reentrant call may
  // have made _rebindings_head point at a different entry.
  entry->publish = NULL;
  entry->publish_context = NULL;
  // Same for the batch publish cells: they are borrowed caller cells, valid
  // only for this initial scan. Free the owned array and NULL it so future
  // image loads (which re-apply the retained entry) never write a stale cell.
  if (entry->publish_cells) {
    free(entry->publish_cells);
    entry->publish_cells = NULL;
  }

  pthread_mutex_unlock(&rebindings_lock);
  return retval;
}

int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
  return rebind_symbols_common(rebindings, rebindings_nel, NULL, NULL, NULL, NULL, NULL, NULL);
}

int rebind_symbols_checked(struct rebinding rebindings[],
                           size_t rebindings_nel,
                           size_t *outMatched) {
  return rebind_symbols_common(rebindings, rebindings_nel, outMatched, NULL, NULL, NULL, NULL, NULL);
}

int rebind_symbols_stats(struct rebinding rebindings[],
                         size_t rebindings_nel,
                         struct rebind_stats *outStats) {
  // Zero the result on every entry path; the core overwrites it on success
  // (under the lock).
  if (outStats) {
    outStats->matched = 0;
    outStats->failed = 0;
    outStats->restore_failed = 0;
  }
  return rebind_symbols_common(rebindings, rebindings_nel, NULL, outStats, NULL, NULL, NULL, NULL);
}

int rebind_symbols_hook(struct rebinding rebindings[],
                        size_t count,
                        struct rebind_result *result,
                        rebind_publish_fn publish,
                        void *context) {
  // Zero the result on every entry path: a failed prepend or an aborted
  // scan must never leave stale counts behind. The core overwrites it on
  // success (under the lock).
  if (result) {
    result->matched = 0;
    result->failed = 0;
    result->restore_failed = 0;
  }

  return rebind_symbols_common(rebindings, count, NULL, NULL, result, publish, context, NULL);
}

int rebind_symbols_hook_batch(struct rebinding rebindings[],
                              void **publish_cells[],
                              size_t count,
                              struct rebind_result *result) {
  // Batch of N rebindings applied in ONE image walk (O(images + N) instead of
  // the O(images) per call that looping rebind_symbols_hook costs). Each
  // rebindings[j].replaced receives the original for retention across future
  // image loads; each publish_cells[j] (a borrowed caller cell) receives it
  // mid-walk, before that symbol's slot goes live — the per-rebinding
  // equivalent of rebind_symbols_hook's single publish callback, which fires
  // only once per entry and so cannot serve a batch. publish_cells[j] may be
  // NULL for a rebinding that wants no live publication.
  if (result) {
    result->matched = 0;
    result->failed = 0;
    result->restore_failed = 0;
  }

  return rebind_symbols_common(rebindings, count, NULL, NULL, result, NULL, NULL, publish_cells);
}

static int rebind_symbols_image_common(void *header,
                                       intptr_t slide,
                                       struct rebinding rebindings[],
                                       size_t rebindings_nel,
                                       struct rebind_stats *outStats) {
    struct rebindings_entry *rebindings_head = NULL;
    int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel, NULL);
    rebind_symbols_for_image(rebindings_head, (const struct mach_header *) header, slide);
    if (outStats && rebindings_head) {
      outStats->matched = (uint32_t)rebindings_head->matched;
      outStats->failed = (uint32_t)rebindings_head->failed;
      outStats->restore_failed = rebindings_head->restore_failed ? 1 : 0;
    }
    if (rebindings_head) {
      free(rebindings_head->rebindings);
    }
    free(rebindings_head);
    return retval;
}

int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel) {
    return rebind_symbols_image_common(header, slide, rebindings, rebindings_nel, NULL);
}

int rebind_symbols_image_stats(void *header,
                               intptr_t slide,
                               struct rebinding rebindings[],
                               size_t rebindings_nel,
                               struct rebind_stats *outStats) {
    // Zero the result on every entry path; the common core overwrites it on
    // success.
    if (outStats) {
      outStats->matched = 0;
      outStats->failed = 0;
      outStats->restore_failed = 0;
    }
    return rebind_symbols_image_common(header, slide, rebindings, rebindings_nel, outStats);
}

// Removes the entries previously prepended for rebindings by a
// rebind_symbols/rebind_symbols_checked call, matching on the exact
// name/replacement/replaced pointers stored at that time (each caller's
// pointers are unique per call). Frees the entry storage only; the caller
// keeps ownership of the pointed-to name/replaced memory. Returns 0 if any
// entry was removed, -1 if none matched. Lets a caller that detected a no-op
// (matched == 0) undo the registration so nothing persists for future image
// loads.
int rebind_symbols_unbind(struct rebinding rebindings[],
                          size_t rebindings_nel) {
  int removed = -1;

  pthread_mutex_lock(&rebindings_lock);

  struct rebindings_entry **link = &_rebindings_head;
  while (*link) {
    struct rebindings_entry *entry = *link;
    if (entry->rebindings_nel == rebindings_nel) {
      bool same = true;
      for (size_t i = 0; i < rebindings_nel; i++) {
        if (entry->rebindings[i].name != rebindings[i].name ||
            entry->rebindings[i].replacement != rebindings[i].replacement ||
            entry->rebindings[i].replaced != rebindings[i].replaced) {
          same = false;
          break;
        }
      }
      if (same) {
        *link = entry->next;
        free(entry->rebindings);
        // Normally NULL here (rebind_symbols_common frees it after the initial
        // scan); free defensively in case an entry is unbound before then.
        free(entry->publish_cells);
        free(entry);
        removed = 0;
        continue;
      }
    }
    link = &entry->next;
  }

  pthread_mutex_unlock(&rebindings_lock);
  return removed;
}
