// Rebind engine. See HKRebindEngine.h for why prepare and commit are separate
// phases and why the write is the only part behind a device seam.

#include "HKRebindEngine.h"

#include <string.h>

#include "../Resolvers/HKChainedFixups.h"
#include "../Resolvers/HKMachO.h"
#include "../Resolvers/HKSymbolResolve.h"

static uint64_t read_slot(const void *base, uintptr_t address) {
    (void)base;
    uint64_t v;
    memcpy(&v, (const void *)address, sizeof(v));
    return v;
}

// ---- phase 1: prepare (mutates nothing) ---------------------------------

typedef struct {
    const hk_symbol_candidates_t *candidates;
    uintptr_t slide;
    hk_rebind_plan_t *plan;
    bool overflow;
} collect_ctx_t;

static bool add_site(hk_rebind_plan_t *plan, uintptr_t address, bool from_chained) {
    if (plan->count >= HK_REBIND_MAX_SITES) {
        return false;
    }
    hk_rebind_site_t *s = &plan->sites[plan->count++];
    s->address = address;
    s->original = read_slot(NULL, address);
    s->from_chained = from_chained;
    return true;
}

// LC_DYSYMTAB path: slot_vmaddr is an UNSLID VM address, so the slide applies.
static bool dysymtab_slot_cb(void *ctx, const hk_import_slot_t *slot) {
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    for (unsigned i = 0; i < c->candidates->count; i++) {
        if (strcmp(slot->symbol_name, c->candidates->names[i]) == 0) {
            if (!add_site(c->plan, (uintptr_t)slot->slot_vmaddr + c->slide, false)) {
                c->overflow = true;
                return false;
            }
            break;
        }
    }
    return true;
}

typedef struct {
    const hk_chained_fixups_t *fixups;
    const hk_symbol_candidates_t *candidates;
    uintptr_t image_base;
    hk_rebind_plan_t *plan;
    bool overflow;
    bool malformed;
} chained_ctx_t;

// Chained-fixups path: slot_image_offset is an offset FROM THE IMAGE BASE, a
// different coordinate system from the LC_DYSYMTAB path's unslid vmaddr.
// Conflating them would put every write at the wrong address.
static bool chained_bind_cb(void *ctx, const hk_chained_bind_t *bind) {
    chained_ctx_t *c = (chained_ctx_t *)ctx;
    hk_chained_import_t import;
    if (hk_chained_import_at(c->fixups, bind->import_ordinal, &import) != HK_CHAINED_OK) {
        c->malformed = true;
        return false;
    }
    for (unsigned i = 0; i < c->candidates->count; i++) {
        if (strcmp(import.symbol_name, c->candidates->names[i]) == 0) {
            if (!add_site(c->plan, c->image_base + (uintptr_t)bind->slot_image_offset, true)) {
                c->overflow = true;
                return false;
            }
            break;
        }
    }
    return true;
}

hk_rebind_status_t hk_rebind_prepare(const hk_rebind_target_t *target,
                                     const char *symbol_name,
                                     hk_symbol_name_convention_t convention,
                                     hk_rebind_plan_t *out_plan) {
    if (!target || !target->image_base || !symbol_name || !out_plan) {
        return HK_REBIND_INVALID_ARGUMENT;
    }
    memset(out_plan, 0, sizeof(*out_plan));

    // The same linker-form expansion every other resolver uses, from the one
    // place it lives.
    hk_symbol_candidates_t candidates;
    if (hk_symbol_build_candidates(symbol_name, convention, &candidates) != HK_RESOLVE_OK) {
        return HK_REBIND_INVALID_ARGUMENT;
    }

    const void *image = target->image_base;
    size_t size = target->image_size;

    // Mechanism 1: LC_DYSYMTAB indirect symbols.
    hk_import_tables_t tables;
    if (hk_import_tables_from_loaded_image(image, size, target->slide, &tables) == HK_IMPORT_OK &&
        tables.indirect_symbols) {
        collect_ctx_t c = { &candidates, target->slide, out_plan, false };
        hk_import_status_t st = hk_import_slots_iterate(image, size, &tables, dysymtab_slot_cb, &c);
        if (c.overflow) {
            return HK_REBIND_TOO_MANY_SITES;
        }
        if (st != HK_IMPORT_OK) {
            return HK_REBIND_MALFORMED_IMAGE;
        }
    }

    // Mechanism 2: chained fixups. An image uses one or the other; consulting
    // both means the caller never has to know which era it came from.
    const void *blob = NULL;
    size_t blob_size = 0;
    if (hk_macho_chained_fixups_for_loaded_image(image, size, target->slide, &blob, &blob_size)
        == HK_MACHO_OK) {
        hk_chained_fixups_t fixups;
        if (hk_chained_fixups_parse(blob, blob_size, &fixups) == HK_CHAINED_OK) {
            chained_ctx_t c = { &fixups, &candidates, (uintptr_t)image, out_plan, false, false };
            hk_chained_status_t st =
                hk_chained_fixups_iterate_binds(&fixups, image, size, chained_bind_cb, &c);
            if (c.overflow) {
                return HK_REBIND_TOO_MANY_SITES;
            }
            if (c.malformed || st != HK_CHAINED_OK) {
                return HK_REBIND_MALFORMED_IMAGE;
            }
        }
    }

    if (out_plan->count == 0) {
        return HK_REBIND_NOT_FOUND;
    }

    // Do the sites agree on what the original is? Normally yes; if not, the
    // caller is told rather than handed an arbitrary one.
    out_plan->original = out_plan->sites[0].original;
    out_plan->originals_agree = true;
    for (uint32_t i = 1; i < out_plan->count; i++) {
        if (out_plan->sites[i].original != out_plan->original) {
            out_plan->originals_agree = false;
            break;
        }
    }
    return HK_REBIND_OK;
}

// ---- phase 2: commit ----------------------------------------------------

static void record_artifact(hk_artifact_sink_t *sink, const hk_rebind_site_t *site,
                            uint64_t replacement) {
    if (!sink) {
        return;
    }
    hk_artifact_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = sizeof(a);
    a.struct_version = HK_ABI_VERSION_3_0;
    a.kind = HK_ARTIFACT_IMPORT_SLOT;
    a.state = HK_ARTIFACT_COMMITTED;
    a.effects = HK_EFFECT_IMPORT_MUTATION;
    a.engine_id.data = "rebind";
    a.engine_id.length = 6;
    a.import_slot_address = site->address;
    a.address = site->address;
    a.original_pointer = (void *)(uintptr_t)site->original;
    a.replacement_pointer = (void *)(uintptr_t)replacement;
    // Restoring a slot is a plain store of the value we already hold, so this
    // is genuinely reversible -- unlike a relocated inline patch.
    a.mechanically_reversible = true;
    a.safe_to_reverse_after_activation = true;
    (void)hk_artifact_sink_record(sink, &a);
}

hk_mutation_state_t hk_rebind_commit(const hk_rebind_target_t *target,
                                     const hk_rebind_plan_t *plan,
                                     uint64_t replacement,
                                     hk_artifact_sink_t *sink,
                                     uint32_t *out_written) {
    if (out_written) {
        *out_written = 0;
    }
    if (!target || !plan || !target->write || plan->count == 0) {
        return HK_MUTATION_NONE;  // nothing attempted, nothing touched
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < plan->count; i++) {
        const hk_rebind_site_t *site = &plan->sites[i];

        // Invariant #3: revalidate immediately before the write. If the slot
        // no longer holds what prepare saw, something else changed it since --
        // possibly another hooking consumer -- and writing would silently
        // destroy their work.
        if (read_slot(target->image_base, site->address) != site->original) {
            // Refusing on the FIRST site means nothing was touched; refusing
            // later means the image is already mixed.
            if (out_written) { *out_written = written; }
            return (written == 0) ? HK_MUTATION_NONE : HK_MUTATION_PARTIAL;
        }

        if (!target->write(target->write_ctx, site->address, replacement)) {
            if (out_written) { *out_written = written; }
            // Invariant #4: after a partial mutation no fallback may be
            // attempted, so this must be reported as PARTIAL, never as a
            // clean failure the router could retry elsewhere.
            return (written == 0) ? HK_MUTATION_NONE : HK_MUTATION_PARTIAL;
        }
        record_artifact(sink, site, replacement);
        written++;
    }

    if (out_written) {
        *out_written = written;
    }
    return HK_MUTATION_COMPLETE;
}
