// Rebind engine <-> runtime adapter. See HKRebindVtable.h.
//
// The image and writer come from the registered engine context, and the
// prepared plan is handed back by the core. A small cache below reuses
// catalog-wide discovery work between hooks in the same runtime context.

#include "HKRebindVtable.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../Resolvers/HKMachO.h"

typedef struct {
    hk_rebind_target_t target;
    hk_rebind_plan_t plan;
} rebind_bundle_entry_t;

typedef struct {
    size_t count;
    bool originals_agree;
    uint64_t original;
    rebind_bundle_entry_t entries[];
} rebind_bundle_t;

static rebind_bundle_t *rebind_bundle_create(size_t capacity) {
    if (capacity > (SIZE_MAX - sizeof(rebind_bundle_t)) /
                    sizeof(rebind_bundle_entry_t)) {
        return NULL;
    }
    rebind_bundle_t *bundle = calloc(
        1, sizeof(*bundle) + capacity * sizeof(rebind_bundle_entry_t));
    if (bundle) {
        bundle->originals_agree = true;
    }
    return bundle;
}

static rebind_bundle_t *rebind_bundle_clone(const rebind_bundle_t *source) {
    if (!source || source->count >
                    (SIZE_MAX - sizeof(*source)) /
                        sizeof(source->entries[0])) {
        return NULL;
    }
    size_t size = sizeof(*source) +
                  source->count * sizeof(source->entries[0]);
    rebind_bundle_t *copy = malloc(size);
    if (copy) {
        memcpy(copy, source, size);
    }
    return copy;
}

// A catalog-wide bundle is immutable after preparation. Keep a small,
// process-wide cache because the same runtime context can serve many plans;
// the catalog generation and live-slot checks below keep entries bounded and
// reject stale captured originals.
#define HK_REBIND_BUNDLE_CACHE_SIZE 4
typedef struct {
    bool valid;
    const hk_rebind_engine_ctx_t *context;
    const hk_image_catalog_t *catalog;
    uint64_t generation;
    hk_symbol_name_convention_t convention;
    hk_rebind_write_fn write;
    void *write_ctx;
    char *symbol;
    rebind_bundle_t *bundle;
} rebind_bundle_cache_entry_t;

static pthread_mutex_t g_rebind_bundle_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static rebind_bundle_cache_entry_t
    g_rebind_bundle_cache[HK_REBIND_BUNDLE_CACHE_SIZE] = {0};
static uint32_t g_rebind_bundle_cache_next = 0;

static bool rebind_bundle_cache_supported(
    const hk_rebind_engine_ctx_t *context,
    const hk_hook_spec_t *spec) {
    return context && !context->image_base && context->catalog && spec &&
           spec->target_kind == HK_TARGET_FUNCTION_SYMBOL &&
           spec->target.symbol.name &&
           spec->target.symbol.caller_image_scope.kind == HK_IMAGE_ANY_LOADED;
}

static bool rebind_bundle_cache_key_matches(
    const rebind_bundle_cache_entry_t *entry,
    const hk_rebind_engine_ctx_t *context,
    const hk_hook_spec_t *spec,
    uint64_t generation) {
    return entry->valid && entry->context == context &&
           entry->catalog == context->catalog &&
           entry->generation == generation &&
           entry->convention == spec->target.symbol.name_convention &&
           entry->write == context->write &&
           entry->write_ctx == context->write_ctx && entry->symbol &&
           strcmp(entry->symbol, spec->target.symbol.name) == 0;
}

static void rebind_bundle_cache_clear_entry_locked(
    rebind_bundle_cache_entry_t *entry) {
    free(entry->symbol);
    free(entry->bundle);
    memset(entry, 0, sizeof(*entry));
}

static bool rebind_bundle_cache_matches_live(
    const rebind_bundle_t *bundle) {
    for (size_t i = 0; i < bundle->count; i++) {
        const hk_rebind_plan_t *plan = &bundle->entries[i].plan;
        for (uint32_t site = 0; site < plan->count; site++) {
            if (hk_rebind_read_slot(plan->sites[site].address) !=
                plan->sites[site].original) {
                return false;
            }
        }
    }
    return true;
}

static rebind_bundle_t *rebind_bundle_cache_copy(
    const hk_rebind_engine_ctx_t *context,
    const hk_hook_spec_t *spec,
    uint64_t generation) {
    rebind_bundle_t *copy = NULL;
    pthread_mutex_lock(&g_rebind_bundle_cache_lock);
    for (size_t i = 0; i < HK_REBIND_BUNDLE_CACHE_SIZE; i++) {
        rebind_bundle_cache_entry_t *entry = &g_rebind_bundle_cache[i];
        if (!rebind_bundle_cache_key_matches(entry, context, spec,
                                             generation)) {
            continue;
        }
        if (!rebind_bundle_cache_matches_live(entry->bundle)) {
            rebind_bundle_cache_clear_entry_locked(entry);
            break;
        }
        copy = rebind_bundle_clone(entry->bundle);
        break;
    }
    pthread_mutex_unlock(&g_rebind_bundle_cache_lock);
    return copy;
}

static void rebind_bundle_cache_clear_context(
    const hk_rebind_engine_ctx_t *context) {
    pthread_mutex_lock(&g_rebind_bundle_cache_lock);
    for (size_t i = 0; i < HK_REBIND_BUNDLE_CACHE_SIZE; i++) {
        if (g_rebind_bundle_cache[i].valid &&
            g_rebind_bundle_cache[i].context == context) {
            rebind_bundle_cache_clear_entry_locked(&g_rebind_bundle_cache[i]);
        }
    }
    pthread_mutex_unlock(&g_rebind_bundle_cache_lock);
}

static void rebind_bundle_cache_store(
    const hk_rebind_engine_ctx_t *context,
    const hk_hook_spec_t *spec,
    uint64_t generation,
    const rebind_bundle_t *bundle) {
    rebind_bundle_t *cached_bundle = rebind_bundle_clone(bundle);
    size_t symbol_size = strlen(spec->target.symbol.name) + 1;
    char *cached_symbol = malloc(symbol_size);
    if (!cached_bundle || !cached_symbol) {
        free(cached_bundle);
        free(cached_symbol);
        return;
    }
    memcpy(cached_symbol, spec->target.symbol.name, symbol_size);

    pthread_mutex_lock(&g_rebind_bundle_cache_lock);
    int victim = -1;
    for (size_t i = 0; i < HK_REBIND_BUNDLE_CACHE_SIZE; i++) {
        if (rebind_bundle_cache_key_matches(&g_rebind_bundle_cache[i],
                                            context, spec, generation)) {
            victim = (int)i;
            break;
        }
        if (!g_rebind_bundle_cache[i].valid && victim == -1) {
            victim = (int)i;
        }
    }
    if (victim == -1) {
        victim = (int)(g_rebind_bundle_cache_next %
                       HK_REBIND_BUNDLE_CACHE_SIZE);
        g_rebind_bundle_cache_next = (uint32_t)(victim + 1) %
                                     HK_REBIND_BUNDLE_CACHE_SIZE;
    }
    rebind_bundle_cache_entry_t *entry = &g_rebind_bundle_cache[victim];
    rebind_bundle_cache_clear_entry_locked(entry);
    entry->context = context;
    entry->catalog = context->catalog;
    entry->generation = generation;
    entry->convention = spec->target.symbol.name_convention;
    entry->write = context->write;
    entry->write_ctx = context->write_ctx;
    entry->symbol = cached_symbol;
    entry->bundle = cached_bundle;
    entry->valid = true;
    pthread_mutex_unlock(&g_rebind_bundle_cache_lock);
}

// prepare must not be handed a writer -- it mutates nothing, and the type
// system is the cheapest place to say so.
static hk_rebind_target_t target_from(const hk_rebind_engine_ctx_t *ctx,
                                      bool with_writer) {
    hk_rebind_target_t t;
    memset(&t, 0, sizeof(t));
    t.image_base = ctx->image_base;
    t.image_size = ctx->image_size;
    t.slide = ctx->slide;
    t.image_path = ctx->image_path;
    t.write = with_writer ? ctx->write : NULL;
    t.write_ctx = ctx->write_ctx;
    return t;
}

static hk_engine_capabilities_t rebind_describe(void) {
    hk_engine_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.engine_id = "fishhook";
    caps.backend_group = "HookKit";
    caps.display_name = "HookKit";
    caps.target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    caps.architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                         HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.certified_architectures = HK_ENGINE_ARCHITECTURE_ARM64 |
                                   HK_ENGINE_ARCHITECTURE_ARM64E;
    caps.minimum_ios_version = HK_ENGINE_IOS_VERSION(15, 0, 0);
    caps.achievable_reach = HK_REACH_EXISTING_IMPORTS;
    caps.original_requirements = HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_NONE) |
                                 HK_ORIGINAL_REQ_BIT(HK_ORIGINAL_DIRECT_PREDECESSOR);
    // One import slot rewritten per site; nothing allocated.
    caps.commit_effects = HK_EFFECT_IMPORT_MUTATION;
    caps.chainable_target_kinds = HK_TARGET_KIND_BIT(HK_TARGET_FUNCTION_SYMBOL);
    return caps;
}

typedef struct {
    const hk_rebind_engine_ctx_t *engine;
    const hk_hook_spec_t *spec;
    rebind_bundle_t *bundle;
    hk_rebind_status_t error;
} rebind_collect_ctx_t;

static bool rebind_collect_image(void *opaque, size_t index,
                                 const hk_image_entry_t *entry) {
    rebind_collect_ctx_t *ctx = opaque;
    (void)index;
    if (!entry || !entry->header || ctx->bundle->count == SIZE_MAX) {
        ctx->error = HK_REBIND_MALFORMED_IMAGE;
        return false;
    }

    hk_macho_header_t header;
    if (hk_macho_peek_header(entry->header, HK_MACHO_HEADER_64_SIZE,
                             &header) != HK_MACHO_OK ||
        (size_t)header.sizeofcmds > SIZE_MAX - HK_MACHO_HEADER_64_SIZE) {
        ctx->error = HK_REBIND_MALFORMED_IMAGE;
        return false;
    }

    rebind_bundle_entry_t *out = &ctx->bundle->entries[ctx->bundle->count];
    // The same target is retained through commit, so keep the writer in the
    // bundle. hk_rebind_prepare never calls it; retaining it here does not
    // weaken the prepare/commit split.
    out->target = target_from(ctx->engine, true);
    out->target.image_base = entry->header;
    out->target.image_size = HK_MACHO_HEADER_64_SIZE +
                             (size_t)header.sizeofcmds;
    out->target.slide = entry->slide;
    out->target.image_path = entry->path;
    out->target.uuid_present = entry->uuid_present;
    memcpy(out->target.uuid, entry->uuid, sizeof(out->target.uuid));
    out->target.include_shared_cache_got =
        ctx->spec->target.symbol.caller_image_scope.kind == HK_IMAGE_ANY_LOADED;

    // Import-slot iteration only needs the load-command region, but chained
    // fixup traversal also reads bind locations throughout the mapped image.
    // Use the loaded span when the catalog entry is a real image; synthetic
    // host entries may have no segments, so retain the command-region bound
    // for those fixtures.
    uintptr_t image_start = 0;
    uintptr_t image_end = 0;
    if (hk_macho_image_span_for_loaded_image(
            entry->header, out->target.image_size, entry->slide,
            &image_start, &image_end) == HK_MACHO_OK &&
        image_end > (uintptr_t)entry->header &&
        image_end - (uintptr_t)entry->header <= SIZE_MAX) {
        out->target.image_size = (size_t)(image_end -
                                          (uintptr_t)entry->header);
    }

    hk_rebind_status_t status = hk_rebind_prepare(
        &out->target, ctx->spec->target.symbol.name,
        ctx->spec->target.symbol.name_convention, &out->plan);
    if (status == HK_REBIND_NOT_FOUND) {
        return true;
    }
    if (status != HK_REBIND_OK) {
        ctx->error = status;
        return false;
    }

    // Cache-global GOT uses are reported while visiting every cached image.
    // Retain each runtime slot once so commit cannot encounter its own write
    // as a stale value in a later per-image plan.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < out->plan.count; i++) {
        bool duplicate = false;
        for (size_t e = 0; e < ctx->bundle->count && !duplicate; e++) {
            for (uint32_t s = 0; s < ctx->bundle->entries[e].plan.count; s++) {
                if (ctx->bundle->entries[e].plan.sites[s].address ==
                    out->plan.sites[i].address) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (!duplicate) {
            out->plan.sites[kept++] = out->plan.sites[i];
        }
    }
    out->plan.count = kept;
    if (kept == 0) {
        return true;
    }

    if (!out->plan.originals_agree) {
        ctx->bundle->originals_agree = false;
    }
    if (ctx->bundle->count == 0) {
        ctx->bundle->original = out->plan.original;
    } else if (ctx->bundle->original != out->plan.original) {
        ctx->bundle->originals_agree = false;
    }
    ctx->bundle->count++;
    return true;
}

// The engine's refusals are distinct diagnoses, not one undifferentiated
// failure. Messages are literals, per the diag contract.
static hk_prepare_result_t rebind_classify(hk_rebind_status_t st, hk_prepare_diag_t *diag) {
    if (st == HK_REBIND_OK) {
        return HK_PREPARE_OK;
    }
    diag->error_code = (int64_t)st;
    switch (st) {
        case HK_REBIND_NOT_FOUND:
            diag->error_message = "the image imports no such symbol"; break;
        case HK_REBIND_MALFORMED_IMAGE:
            diag->error_message = "the image's import metadata is structurally invalid"; break;
        case HK_REBIND_TOO_MANY_SITES:
            diag->error_message = "more import slots for this symbol than the engine can record"; break;
        case HK_REBIND_METADATA_UNAVAILABLE:
            diag->error_message = "authoritative on-disk import metadata is unavailable"; break;
        case HK_REBIND_PAC_MISMATCH:
            diag->error_message = "an authenticated import slot does not match its declared PAC schema"; break;
        case HK_REBIND_UNSUPPORTED_FORMAT:
            diag->error_message = "the image uses an unsupported chained-fixup or cache-patch format"; break;
        case HK_REBIND_SCOPE_UNREPRESENTABLE:
            diag->error_message = "shared-cache GOT uses cannot represent the requested caller scope"; break;
        case HK_REBIND_INVALID_ARGUMENT:
        case HK_REBIND_OK:
            diag->error_message = "invalid rebind target"; break;
    }
    return HK_PREPARE_FAILED;
}

static hk_prepare_result_t rebind_prepare_one_ctx_status(void *engine_ctx,
                                                         const hk_hook_spec_t *spec,
                                                         void **out_prepared,
                                                         hk_prepare_diag_t *out_diag) {
    const hk_rebind_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !out_prepared || spec->target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        out_diag->error_message = "rebind engine invoked without an image or with a non-symbol target";
        return HK_PREPARE_FAILED;
    }

    // A production runtime supplies the live catalog without pinning one
    // image. Prepare a plan for every matching importer so an ANY_LOADED or
    // explicit-set scope is honored instead of silently rebinding one image.
    if (!ctx->image_base && ctx->catalog) {
        bool use_bundle_cache = rebind_bundle_cache_supported(ctx, spec);
        uint64_t bundle_generation = use_bundle_cache
            ? hk_image_catalog_generation(ctx->catalog) : 0;
        if (use_bundle_cache) {
            rebind_bundle_t *cached = rebind_bundle_cache_copy(
                ctx, spec, bundle_generation);
            if (cached) {
                hk_prepare_result_t result;
                if (cached->count == 0) {
                    result = rebind_classify(HK_REBIND_NOT_FOUND, out_diag);
                    free(cached);
                    return result;
                }
                if (spec->original_requirement != HK_ORIGINAL_NONE &&
                    (!cached->originals_agree || cached->original == 0)) {
                    out_diag->error_message = "rebind sites have no single original";
                    free(cached);
                    return HK_PREPARE_FAILED;
                }
                *out_prepared = cached;
                return HK_PREPARE_OK;
            }
        }

        rebind_bundle_t *bundle = rebind_bundle_create(
            hk_image_catalog_count(ctx->catalog));
        if (!bundle) {
            out_diag->error_message = "out of memory";
            return HK_PREPARE_FAILED;
        }
        rebind_collect_ctx_t collect = {
            .engine = ctx,
            .spec = spec,
            .bundle = bundle,
            .error = HK_REBIND_OK,
        };
        (void)hk_image_catalog_match(ctx->catalog,
                                     &spec->target.symbol.caller_image_scope,
                                     rebind_collect_image, &collect);
        if (collect.error != HK_REBIND_OK) {
            hk_prepare_result_t result = rebind_classify(collect.error, out_diag);
            free(bundle);
            return result;
        }
        if (use_bundle_cache && bundle_generation ==
            hk_image_catalog_generation(ctx->catalog)) {
            rebind_bundle_cache_store(ctx, spec, bundle_generation, bundle);
        }
        if (bundle->count == 0) {
            hk_prepare_result_t result = rebind_classify(HK_REBIND_NOT_FOUND,
                                                         out_diag);
            free(bundle);
            return result;
        }
        if (spec->original_requirement != HK_ORIGINAL_NONE &&
            (!bundle->originals_agree || bundle->original == 0)) {
            out_diag->error_message = "rebind sites have no single original";
            free(bundle);
            return HK_PREPARE_FAILED;
        }
        *out_prepared = bundle;
        return HK_PREPARE_OK;
    }

    // The rebind engine rewrites import slots in the image this context points
    // at -- the IMPORTER. So caller_image_scope is the selector that applies
    // here; defining_image describes where the symbol is exported from, which
    // this mechanism never resolves and must not pretend to check.
    //
    // Identity, not containment: the question is whether THIS image is in
    // scope, so the header pointer is compared directly. Using the containment
    // form here would assume the header lies inside the image's own segment
    // span -- true of a real Mach-O, but an assumption the check cannot verify,
    // and one a synthetic image can violate.
    hk_image_scope_status_t scope =
        hk_image_scope_check_header(ctx->catalog, &spec->target.symbol.caller_image_scope,
                                    false, NULL, ctx->image_base);
    if (scope != HK_IMAGE_SCOPE_OK && scope != HK_IMAGE_SCOPE_NO_CATALOG) {
        out_diag->error_code = HK_REBIND_DIAG_IMAGE_SCOPE_BASE + (int64_t)scope;
        out_diag->error_message = hk_image_scope_describe(scope);
        return HK_PREPARE_FAILED;
    }

    hk_rebind_plan_t *plan = malloc(sizeof(*plan));
    if (!plan) {
        out_diag->error_message = "out of memory";
        return HK_PREPARE_FAILED;
    }
    hk_rebind_target_t target = target_from(ctx, false);  // prepare never writes
    target.include_shared_cache_got =
        spec->target.symbol.caller_image_scope.kind == HK_IMAGE_ANY_LOADED;
    hk_rebind_status_t st = hk_rebind_prepare(&target, spec->target.symbol.name,
                                              spec->target.symbol.name_convention, plan);
    hk_prepare_result_t result = rebind_classify(st, out_diag);
    if (result != HK_PREPARE_OK) {
        free(plan);  // nothing reserved on any non-OK status
        return result;
    }
    if (spec->original_requirement != HK_ORIGINAL_NONE &&
        (!plan->originals_agree || plan->original == 0)) {
        out_diag->error_message = "rebind sites have no single original";
        free(plan);
        return HK_PREPARE_FAILED;
    }
    *out_prepared = plan;
    return HK_PREPARE_OK;
}

static hk_mutation_state_t rebind_commit_one_ctx(void *engine_ctx,
                                                 const hk_hook_spec_t *spec,
                                                 void *prepared,
                                                 hk_artifact_sink_t *sink) {
    const hk_rebind_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !prepared) {
        // No captured plan -- prepare did not run or did not succeed for this
        // hook. Nothing was ever reserved, so nothing is touched.
        return HK_MUTATION_NONE;
    }
    if (!ctx->image_base && ctx->catalog) {
        // Later plans must capture the current slot values. Already-prepared
        // sibling hooks own independent copies, so clearing this template
        // cannot affect the commit in progress.
        rebind_bundle_cache_clear_context(ctx);
        rebind_bundle_t *bundle = prepared;
        uint32_t total_written = 0;
        for (size_t i = 0; i < bundle->count; i++) {
            uint32_t written = 0;
            hk_mutation_state_t state = hk_rebind_commit(
                &bundle->entries[i].target, &bundle->entries[i].plan,
                (uint64_t)(uintptr_t)spec->replacement, sink, &written);
            total_written += written;
            if (state != HK_MUTATION_COMPLETE) {
                if (state == HK_MUTATION_UNKNOWN) {
                    return HK_MUTATION_UNKNOWN;
                }
                if (state == HK_MUTATION_PARTIAL || total_written != 0) {
                    return HK_MUTATION_PARTIAL;
                }
                return HK_MUTATION_NONE;
            }
        }
        if (sink && bundle->originals_agree) {
            sink->published_original = (void *)(uintptr_t)bundle->original;
        }
        return HK_MUTATION_COMPLETE;
    }

    hk_rebind_target_t target = target_from(ctx, true);
    uint32_t written = 0;
    const hk_rebind_plan_t *plan = (const hk_rebind_plan_t *)prepared;
    hk_mutation_state_t state =
        hk_rebind_commit(&target, plan, (uint64_t)(uintptr_t)spec->replacement,
                         sink, &written);
    if (state == HK_MUTATION_COMPLETE && sink && plan->count > 0 &&
        plan->originals_agree) {
        sink->published_original = (void *)(uintptr_t)plan->original;
    }
    return state;
}

static hk_verify_result_t rebind_verify_one_ctx(void *engine_ctx,
                                                const hk_hook_spec_t *spec,
                                                void *prepared,
                                                hk_verify_diag_t *out_diag) {
    const hk_rebind_engine_ctx_t *ctx = engine_ctx;
    if (!ctx || !spec || !prepared ||
        spec->target_kind != HK_TARGET_FUNCTION_SYMBOL) {
        out_diag->error_message = "rebind verification received an invalid prepared plan";
        return HK_VERIFY_FAILED;
    }

    if (!ctx->image_base && ctx->catalog) {
        const rebind_bundle_t *bundle = prepared;
        for (size_t i = 0; i < bundle->count; i++) {
            for (uint32_t j = 0; j < bundle->entries[i].plan.count; j++) {
                uint64_t expected = 0;
                if (!hk_rebind_replacement_for_site(
                        &bundle->entries[i].plan.sites[j],
                        (uint64_t)(uintptr_t)spec->replacement, &expected) ||
                    hk_rebind_read_slot(bundle->entries[i].plan.sites[j].address) != expected) {
                    out_diag->error_message = "rebind slot readback does not match the replacement";
                    return HK_VERIFY_FAILED;
                }
            }
        }
        return HK_VERIFY_OK;
    }

    const hk_rebind_plan_t *plan = prepared;
    for (uint32_t i = 0; i < plan->count; i++) {
        uint64_t expected = 0;
        if (!hk_rebind_replacement_for_site(
                &plan->sites[i], (uint64_t)(uintptr_t)spec->replacement,
                &expected) ||
            hk_rebind_read_slot(plan->sites[i].address) != expected) {
            out_diag->error_message = "rebind slot readback does not match the replacement";
            return HK_VERIFY_FAILED;
        }
    }
    return HK_VERIFY_OK;
}

static void rebind_release_prepared(void *engine_ctx, void *prepared) {
    (void)engine_ctx;
    free(prepared);
}

static const hk_engine_vtable_t g_rebind_vtable = {
    .abi_version = HK_ENGINE_VTABLE_ABI_VERSION_1,
    .struct_size = sizeof(hk_engine_vtable_t),
    .describe = rebind_describe,
    .prepare_one_ctx_status = rebind_prepare_one_ctx_status,
    .commit_one_ctx = rebind_commit_one_ctx,
    .verify_one_ctx = rebind_verify_one_ctx,
    .release_prepared = rebind_release_prepared,
};

const hk_engine_vtable_t *hk_rebind_vtable(void) {
    return &g_rebind_vtable;
}
