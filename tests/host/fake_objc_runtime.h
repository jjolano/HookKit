// A minimal in-memory Objective-C runtime for host tests.
//
// There is no ObjC runtime on the Linux host these tests run on -- not a stub,
// none: objc/runtime.h does not exist and no libobjc is installed. (The repo's
// tests/fixtures/headers are declaration-only stubs for the compiler front end;
// nothing in them executes a runtime call.) So HKObjCEngine takes the whole
// runtime as a function-pointer seam, and this is what the host suites drive
// it with.
//
// It implements the four behaviors the engine actually depends on:
//   - class_getInstanceMethod searches ancestors and returns a STABLE method
//     identity, so the same inherited method resolved from a subclass and from
//     its superclass compares equal (the local-vs-inherited test needs exactly
//     this)
//   - class methods live on the metaclass
//   - class_replaceMethod returns the previous IMP when the method was on THIS
//     class, and NULL when it instead ADDED an override
//   - method_getTypeEncoding may be absent
//
// Header-only and all-static: each including test binary gets its own copy,
// which is what keeps the fixtures independent between suites.

#ifndef HK_TESTS_FAKE_OBJC_RUNTIME_H
#define HK_TESTS_FAKE_OBJC_RUNTIME_H

#include <assert.h>
#include <string.h>

#include "../../src/engines/HKObjCEngine.h"

typedef struct {
    const char *sel;
    void *imp;
    const char *types;
} fake_method_t;

typedef struct fake_class {
    const char *name;
    struct fake_class *super;
    struct fake_class *meta;   // NULL on a metaclass itself
    fake_method_t methods[8];
    unsigned count;
} fake_class_t;

typedef struct {
    fake_class_t **classes;
    unsigned class_count;
    // Ordering probe: what the engine's out_original cell held at the moment
    // replace_method was called. Proves publish-before-replace rather than
    // assuming it.
    void **watch_cell;
    bool cell_was_set_at_replace;
    unsigned replace_calls;
} fake_rt_t;

// Selectors are interned so pointer identity works the way SEL does.
//
// The pool OWNS its strings -- it copies rather than retaining the caller's
// pointer. That is not tidiness: the pool is file-scoped and outlives every
// plan, while a selector name arriving from a hook spec points into the
// plan's deep copy (hk_hook_t.owned_objc_selector_name), which hk_plan_release
// frees. Retaining that pointer left the pool holding freed memory, and the
// next intern_sel's strcmp read it. It stayed invisible for a while because
// every selector the fixture itself uses is interned from a literal during
// world_init first, so the engine's lookup matched the existing entry and
// never stored the heap copy -- only a selector the fixture does NOT define
// (a deliberately absent one) reached the storing path. Found by ASan; a plain
// run read the freed bytes and passed.
static char g_sel_pool[16][32];
static unsigned g_sel_count;
static void *intern_sel(const char *name) {
    for (unsigned i = 0; i < g_sel_count; i++) {
        if (strcmp(g_sel_pool[i], name) == 0) return g_sel_pool[i];
    }
    assert(g_sel_count < 16 && strlen(name) < sizeof(g_sel_pool[0]));
    strcpy(g_sel_pool[g_sel_count], name);
    return g_sel_pool[g_sel_count++];
}

static void *f_get_class(void *ctx, const char *name) {
    fake_rt_t *rt = ctx;
    for (unsigned i = 0; i < rt->class_count; i++) {
        if (strcmp(rt->classes[i]->name, name) == 0) return rt->classes[i];
    }
    return NULL;
}
static void *f_get_metaclass(void *ctx, void *cls) {
    (void)ctx;
    return cls ? ((fake_class_t *)cls)->meta : NULL;
}
static void *f_get_superclass(void *ctx, void *cls) {
    (void)ctx;
    return cls ? ((fake_class_t *)cls)->super : NULL;
}
static void *f_register_selector(void *ctx, const char *name) {
    (void)ctx;
    return intern_sel(name);
}
// Searches cls then ancestors. Returns the method's own storage, so the same
// inherited method has one identity no matter where the search started.
static void *f_get_instance_method(void *ctx, void *cls, void *sel) {
    (void)ctx;
    for (fake_class_t *c = cls; c; c = c->super) {
        for (unsigned i = 0; i < c->count; i++) {
            if ((void *)c->methods[i].sel == sel) return &c->methods[i];
        }
    }
    return NULL;
}
static void *f_method_get_imp(void *ctx, void *method) {
    (void)ctx;
    return method ? ((fake_method_t *)method)->imp : NULL;
}
static const char *f_method_get_types(void *ctx, void *method) {
    (void)ctx;
    return method ? ((fake_method_t *)method)->types : NULL;
}
// Replaces in place when the method is on THIS class (returning the old IMP);
// otherwise ADDS a new override and returns NULL, as libobjc documents.
static void *f_replace_method(void *ctx, void *cls, void *sel, void *imp,
                              const char *types) {
    fake_rt_t *rt = ctx;
    rt->replace_calls++;
    if (rt->watch_cell) {
        rt->cell_was_set_at_replace = (*rt->watch_cell != NULL);
    }
    fake_class_t *c = cls;
    for (unsigned i = 0; i < c->count; i++) {
        if ((void *)c->methods[i].sel == sel) {
            void *old = c->methods[i].imp;
            c->methods[i].imp = imp;
            c->methods[i].types = types;
            return old;
        }
    }
    assert(c->count < 8);
    c->methods[c->count].sel = (const char *)sel;
    c->methods[c->count].imp = imp;
    c->methods[c->count].types = types;
    c->count++;
    return NULL;  // added an override; replaced nothing
}

static hk_objc_runtime_t make_runtime(fake_rt_t *ctx) {
    hk_objc_runtime_t rt;
    rt.get_class = f_get_class;
    rt.get_metaclass = f_get_metaclass;
    rt.get_superclass = f_get_superclass;
    rt.register_selector = f_register_selector;
    rt.get_instance_method = f_get_instance_method;
    rt.method_get_imp = f_method_get_imp;
    rt.method_get_types = f_method_get_types;
    rt.replace_method = f_replace_method;
    rt.ctx = ctx;
    return rt;
}

// ---- shared fixture ----------------------------------------------------

// Stand-in implementations. Only their addresses matter. Marked unused
// because this is a shared palette -- no single suite is expected to need
// every one of them, and -Wunused-function would otherwise force each suite
// to reference all five.
__attribute__((unused)) static void imp_base_foo(void) {}
__attribute__((unused)) static void imp_child_bar(void) {}
__attribute__((unused)) static void imp_class_ping(void) {}
__attribute__((unused)) static void imp_replacement(void) {}
__attribute__((unused)) static void imp_someone_else(void) {}

// Base <- Child, plus their metaclasses (BaseMeta <- ChildMeta).
// -foo lives on Base only (so it is INHERITED from Child).
// -bar lives on Child (LOCAL to Child).
// +ping lives on BaseMeta, i.e. a class method -- and note `ping` is
// deliberately absent from the instance side, which is what makes the
// metaclass test able to fail.
typedef struct {
    fake_class_t base, child, base_meta, child_meta;
    fake_class_t *list[4];
    fake_rt_t rt;
} world_t;

static void world_init(world_t *w) {
    memset(w, 0, sizeof(*w));

    w->base_meta.name = "Base(meta)";
    w->base_meta.methods[0].sel = intern_sel("ping");
    w->base_meta.methods[0].imp = (void *)imp_class_ping;
    w->base_meta.methods[0].types = "v@:";
    w->base_meta.count = 1;

    w->child_meta.name = "Child(meta)";
    w->child_meta.super = &w->base_meta;

    w->base.name = "Base";
    w->base.meta = &w->base_meta;
    w->base.methods[0].sel = intern_sel("foo");
    w->base.methods[0].imp = (void *)imp_base_foo;
    w->base.methods[0].types = "v@:";
    w->base.count = 1;

    w->child.name = "Child";
    w->child.super = &w->base;
    w->child.meta = &w->child_meta;
    w->child.methods[0].sel = intern_sel("bar");
    w->child.methods[0].imp = (void *)imp_child_bar;
    w->child.methods[0].types = NULL;  // no encoding -> placeholder path
    w->child.count = 1;

    w->list[0] = &w->base;
    w->list[1] = &w->child;
    w->list[2] = &w->base_meta;
    w->list[3] = &w->child_meta;
    w->rt.classes = w->list;
    w->rt.class_count = 4;
}

static unsigned imp_count_on(fake_class_t *c, const char *sel_name) {
    unsigned n = 0;
    void *sel = intern_sel(sel_name);
    for (unsigned i = 0; i < c->count; i++) {
        if ((void *)c->methods[i].sel == sel) n++;
    }
    return n;
}
static void *imp_on(fake_class_t *c, const char *sel_name) {
    void *sel = intern_sel(sel_name);
    for (unsigned i = 0; i < c->count; i++) {
        if ((void *)c->methods[i].sel == sel) return c->methods[i].imp;
    }
    return NULL;
}

#endif // HK_TESTS_FAKE_OBJC_RUNTIME_H
