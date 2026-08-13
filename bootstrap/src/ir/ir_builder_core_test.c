/* bootstrap/src/ir/ir_builder_core_test.c
 *
 * WP-M0-16c1a IR builder foundation unit tests: entry validation and the
 * status/ownership contract of `ir_builder_build` (IR_BUILDER_OK with
 * *out_build owned; IR_BUILDER_UNSUPPORTED / IR_BUILDER_OOM with nothing
 * owned, no crash, no partial ownership), the builder context allocation
 * discipline, and the canonical-order two-phase driver skeleton (Phase A
 * module/declaration order, Phase B body order) verified with recording
 * mappers through the registered-hook seam.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-ir16c1a' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_builder_core_test.c \
 *     bootstrap/src/ir/ir_builder_core.c bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-ir16c1a/ir_builder_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-ir16c1a)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "ir_builder_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Hand-built fixtures (the builder borrows these; never freed here)
 * ------------------------------------------------------------------------- */

/* One stack NameResult with the given modules array. */
static NameResult make_result(NameModule **modules, size_t nmodules)
{
    NameResult r;
    memset(&r, 0, sizeof(r));
    r.modules = modules;
    r.nmodules = nmodules;
    return r;
}

/* One stack module with the given name and module-scope symbols. */
static NameModule make_module(const char *fqn, NameSymbol **scope,
                              size_t nscope)
{
    NameModule m;
    memset(&m, 0, sizeof(m));
    m.fqn = (char *)fqn;   /* borrowed; only read by recording mappers */
    m.module_scope = scope;
    m.nmodule_scope = nscope;
    return m;
}

/* One stack symbol with the given kind and name. */
static NameSymbol make_symbol(NameSymbolKind kind, const char *name)
{
    NameSymbol s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.name = (char *)name;   /* borrowed; only read by recording mappers */
    return s;
}

/* ---------------------------------------------------------------------------
 * Recording mappers (verify the two-phase canonical-order skeleton)
 * ------------------------------------------------------------------------- */

typedef struct Visit {
    int phase;             /* 1 = Phase A, 2 = Phase B */
    const char *what;      /* "module" | "decl" | "body" */
    const char *mod;       /* module fqn */
    const char *sym;       /* symbol name ("" for module visits) */
} Visit;

static Visit g_visits[64];
static size_t g_nvisits;

static void visit_record(int phase, const char *what, const char *mod,
                         const char *sym)
{
    if (g_nvisits < 64) {
        g_visits[g_nvisits].phase = phase;
        g_visits[g_nvisits].what = what;
        g_visits[g_nvisits].mod = mod;
        g_visits[g_nvisits].sym = sym;
        g_nvisits++;
    }
}

static IrBuilderStatus rec_module(BuilderCtx *ctx, const NameModule *module)
{
    (void)ctx;
    visit_record(ctx->phase, "module", module->fqn, "");
    return IR_BUILDER_OK;
}

static IrBuilderStatus rec_decl(BuilderCtx *ctx, const NameModule *module,
                                const NameSymbol *sym)
{
    (void)ctx;
    visit_record(ctx->phase, "decl", module->fqn, sym->name);
    return IR_BUILDER_OK;
}

static IrBuilderStatus rec_body(BuilderCtx *ctx, const NameModule *module,
                                const NameSymbol *fn_sym)
{
    (void)ctx;
    visit_record(ctx->phase, "body", module->fqn, fn_sym->name);
    return IR_BUILDER_OK;
}

static IrBuilderStatus oom_decl(BuilderCtx *ctx, const NameModule *module,
                                const NameSymbol *sym)
{
    (void)ctx;
    (void)module;
    (void)sym;
    return IR_BUILDER_OOM;
}

static IrBuilderStatus oom_module(BuilderCtx *ctx, const NameModule *module)
{
    (void)ctx;
    (void)module;
    return IR_BUILDER_OOM;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* AC1: entry-level malformed input -> IR_BUILDER_UNSUPPORTED, nothing
 * owned, no crash. */
static void test_entry_validation(void)
{
    NameResult r;
    LayoutBuild lb;
    IrBuild *out = NULL;
    IrBuilderStatus st;

    memset(&r, 0, sizeof(r));
    memset(&lb, 0, sizeof(lb));

    /* NULL result */
    out = (IrBuild *)0x1;   /* sentinel: nothing owned on failure */
    st = ir_builder_build(NULL, &lb, &out);
    CHECK(st == IR_BUILDER_UNSUPPORTED);
    CHECK(out == NULL);   /* *out_build cleared: nothing owned */

    /* NULL layout */
    out = (IrBuild *)0x1;
    st = ir_builder_build(&r, NULL, &out);
    CHECK(st == IR_BUILDER_UNSUPPORTED);
    CHECK(out == NULL);   /* nothing owned */

    /* NULL out_build (cannot report ownership; must not crash) */
    st = ir_builder_build(&r, &lb, NULL);
    CHECK(st == IR_BUILDER_UNSUPPORTED);

    /* malformed module list entry -> defensive UNSUPPORTED, nothing owned */
    {
        NameModule *mods[1] = { NULL };
        NameResult r2 = make_result(mods, 1);
        out = NULL;
        st = ir_builder_build(&r2, &lb, &out);
        CHECK(st == IR_BUILDER_UNSUPPORTED);
        CHECK(out == NULL);
    }

    /* malformed scope entry -> defensive UNSUPPORTED, nothing owned */
    {
        NameSymbol *scope[1] = { NULL };
        NameModule m = make_module("main", scope, 1);
        NameModule *mods[1] = { &m };
        NameResult r2 = make_result(mods, 1);
        out = NULL;
        st = ir_builder_build(&r2, &lb, &out);
        CHECK(st == IR_BUILDER_UNSUPPORTED);
        CHECK(out == NULL);
    }
}

/* MIN-1 (reviewer2 t_4258d6a7): NULL-array pointers with nonzero counts
 * are malformed input -> IR_BUILDER_UNSUPPORTED, nothing owned, no crash.
 * Unreachable from the accepted pipeline (name resolution always
 * allocates these arrays when counts are nonzero; IR contract sec. 1.3),
 * but the defensive contract (header "Malformed input that cannot be
 * mapped returns IR_BUILDER_UNSUPPORTED with nothing owned") requires no
 * crash. */
static void test_null_array_guards(void)
{
    LayoutBuild lb;
    IrBuild *out = NULL;
    IrBuilderStatus st;

    memset(&lb, 0, sizeof(lb));

    /* NameResult{.modules=NULL, .nmodules>0}: refuse before any deref */
    {
        NameResult r;
        memset(&r, 0, sizeof(r));
        r.modules = NULL;
        r.nmodules = 1;
        out = NULL;
        st = ir_builder_build(&r, &lb, &out);
        CHECK(st == IR_BUILDER_UNSUPPORTED);
        CHECK(out == NULL);   /* nothing owned */
    }

    /* the guard fires regardless of the count value */
    {
        NameResult r;
        memset(&r, 0, sizeof(r));
        r.modules = NULL;
        r.nmodules = 2;
        out = NULL;
        st = ir_builder_build(&r, &lb, &out);
        CHECK(st == IR_BUILDER_UNSUPPORTED);
        CHECK(out == NULL);
    }

    /* NameModule{.module_scope=NULL, .nmodule_scope>0}: refuse before
     * deref inside the module loop */
    {
        NameModule m = make_module("main", NULL, 1);
        NameModule *mods[1] = { &m };
        NameResult r = make_result(mods, 1);
        out = NULL;
        st = ir_builder_build(&r, &lb, &out);
        CHECK(st == IR_BUILDER_UNSUPPORTED);
        CHECK(out == NULL);
    }

    /* compliant case: non-NULL arrays with nonzero counts still map fine;
     * the guards must not reject valid input. */
    {
        NameSymbol s1 = make_symbol(NAME_SYM_GLOBAL_VAR, "g");
        NameSymbol *scope[1] = { &s1 };
        NameModule m = make_module("main", scope, 1);
        NameModule *mods[1] = { &m };
        NameResult r = make_result(mods, 1);
        out = NULL;
        g_nvisits = 0;
        ir_builder_set_module_mapper(rec_module);
        ir_builder_set_decl_mapper(rec_decl);
        ir_builder_set_body_mapper(rec_body);
        st = ir_builder_build(&r, &lb, &out);
        CHECK(st == IR_BUILDER_OK);
        CHECK(out != NULL);
        if (out != NULL) {
            ir_build_free(out);   /* owned by the caller */
        }
        /* phase A: module + decl visits; phase B skips the non-fn symbol */
        CHECK(g_nvisits == 2);
        ir_builder_set_module_mapper(NULL);
        ir_builder_set_decl_mapper(NULL);
        ir_builder_set_body_mapper(NULL);
    }
}

/* AC1: an accepted build with an empty surface -> IR_BUILDER_OK with
 * *out_build owned; the caller owns and frees the build. */
static void test_empty_build_ok(void)
{
    NameResult r;
    LayoutBuild lb;
    IrBuild *out = NULL;
    IrBuilderStatus st;

    memset(&r, 0, sizeof(r));   /* zero modules: nothing to map */
    memset(&lb, 0, sizeof(lb));

    st = ir_builder_build(&r, &lb, &out);
    CHECK(st == IR_BUILDER_OK);
    CHECK(out != NULL);
    if (st == IR_BUILDER_OK && out != NULL) {
        /* base types are interned at build creation (13 spec types) */
        CHECK(out->ntypes == 13);
        CHECK(out->nnodes == 0);
        ir_build_free(out);   /* owned by the caller */
    }
}

/* AC1: an accepted build with module content is outside the 16c1a
 * representable surface (default mappers) -> IR_BUILDER_UNSUPPORTED with
 * nothing owned. */
static void test_unsupported_default_mappers(void)
{
    NameSymbol s1 = make_symbol(NAME_SYM_FN, "main");
    NameSymbol *scope[1] = { &s1 };
    NameModule m = make_module("main", scope, 1);
    NameModule *mods[1] = { &m };
    NameResult r = make_result(mods, 1);
    LayoutBuild lb;
    IrBuild *out = NULL;
    IrBuilderStatus st;

    memset(&lb, 0, sizeof(lb));
    st = ir_builder_build(&r, &lb, &out);
    CHECK(st == IR_BUILDER_UNSUPPORTED);
    CHECK(out == NULL);   /* nothing owned */
}

/* AC2: the two-phase canonical-order driver skeleton. Phase A visits
 * modules in canonical order (entry first, then imports) and declarations
 * in source order; Phase B visits only function bodies, after all of
 * Phase A, in canonical order. */
static void test_two_phase_order(void)
{
    /* entry module "main": var g, fn f, struct S, fn h (source order) */
    NameSymbol g = make_symbol(NAME_SYM_GLOBAL_VAR, "g");
    NameSymbol f = make_symbol(NAME_SYM_FN, "f");
    NameSymbol S = make_symbol(NAME_SYM_STRUCT, "S");
    NameSymbol h = make_symbol(NAME_SYM_FN, "h");
    NameSymbol *main_scope[4] = { &g, &f, &S, &h };
    NameModule main_m = make_module("main", main_scope, 4);

    /* imported module "util": fn g2, const c (source order) */
    NameSymbol g2 = make_symbol(NAME_SYM_FN, "g2");
    NameSymbol c = make_symbol(NAME_SYM_GLOBAL_CONST, "c");
    NameSymbol *util_scope[2] = { &g2, &c };
    NameModule util_m = make_module("util", util_scope, 2);

    /* canonical module order: entry first, then imports depth-first */
    NameModule *mods[2] = { &main_m, &util_m };
    NameResult r = make_result(mods, 2);
    LayoutBuild lb;
    IrBuild *out = NULL;
    IrBuilderStatus st;

    memset(&lb, 0, sizeof(lb));
    g_nvisits = 0;
    ir_builder_set_module_mapper(rec_module);
    ir_builder_set_decl_mapper(rec_decl);
    ir_builder_set_body_mapper(rec_body);

    st = ir_builder_build(&r, &lb, &out);
    CHECK(st == IR_BUILDER_OK);
    CHECK(out != NULL);
    if (out != NULL) {
        ir_build_free(out);
    }

    /* Phase A: modules in canonical order; declarations in source order */
    CHECK(g_nvisits == 11);
    if (g_nvisits == 11) {
        CHECK(g_visits[0].phase == 1 && strcmp(g_visits[0].what, "module") == 0
              && strcmp(g_visits[0].mod, "main") == 0);
        CHECK(g_visits[1].phase == 1 && strcmp(g_visits[1].what, "decl") == 0
              && strcmp(g_visits[1].sym, "g") == 0);
        CHECK(g_visits[2].phase == 1 && strcmp(g_visits[2].what, "decl") == 0
              && strcmp(g_visits[2].sym, "f") == 0);
        CHECK(g_visits[3].phase == 1 && strcmp(g_visits[3].what, "decl") == 0
              && strcmp(g_visits[3].sym, "S") == 0);
        CHECK(g_visits[4].phase == 1 && strcmp(g_visits[4].what, "decl") == 0
              && strcmp(g_visits[4].sym, "h") == 0);
        CHECK(g_visits[5].phase == 1 && strcmp(g_visits[5].what, "module") == 0
              && strcmp(g_visits[5].mod, "util") == 0);
        CHECK(g_visits[6].phase == 1 && strcmp(g_visits[6].what, "decl") == 0
              && strcmp(g_visits[6].sym, "g2") == 0);
        CHECK(g_visits[7].phase == 1 && strcmp(g_visits[7].what, "decl") == 0
              && strcmp(g_visits[7].sym, "c") == 0);
        /* Phase B: function bodies only, after all of Phase A */
        CHECK(g_visits[8].phase == 2 && strcmp(g_visits[8].what, "body") == 0
              && strcmp(g_visits[8].mod, "main") == 0
              && strcmp(g_visits[8].sym, "f") == 0);
        CHECK(g_visits[9].phase == 2 && strcmp(g_visits[9].what, "body") == 0
              && strcmp(g_visits[9].mod, "main") == 0
              && strcmp(g_visits[9].sym, "h") == 0);
        CHECK(g_visits[10].phase == 2 && strcmp(g_visits[10].what, "body") == 0
              && strcmp(g_visits[10].mod, "util") == 0
              && strcmp(g_visits[10].sym, "g2") == 0);
    }

    /* restore defaults so later tests run against the defensive stubs */
    ir_builder_set_module_mapper(NULL);
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_body_mapper(NULL);
}

/* AC1: allocation failure inside a mapping step -> IR_BUILDER_OOM with
 * nothing owned (the partial build is released). */
static void test_oom_ownership(void)
{
    NameSymbol s1 = make_symbol(NAME_SYM_FN, "main");
    NameSymbol *scope[1] = { &s1 };
    NameModule m = make_module("main", scope, 1);
    NameModule *mods[1] = { &m };
    NameResult r = make_result(mods, 1);
    LayoutBuild lb;
    IrBuild *out = NULL;
    IrBuilderStatus st;

    memset(&lb, 0, sizeof(lb));
    /* the module mapper must accept the module so the decl mapper runs */
    ir_builder_set_module_mapper(rec_module);
    ir_builder_set_decl_mapper(oom_decl);
    st = ir_builder_build(&r, &lb, &out);
    CHECK(st == IR_BUILDER_OOM);
    CHECK(out == NULL);   /* nothing owned */
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_module_mapper(NULL);

    /* OOM from the module mapper too */
    out = NULL;
    ir_builder_set_module_mapper(oom_module);
    st = ir_builder_build(&r, &lb, &out);
    CHECK(st == IR_BUILDER_OOM);
    CHECK(out == NULL);
    ir_builder_set_module_mapper(NULL);
}

/* Context allocation discipline: non-NULL on success, NULL + sticky oom
 * flag on failure (size 0 / already-oom). */
static void test_ctx_alloc(void)
{
    BuilderCtx ctx;
    void *p;

    memset(&ctx, 0, sizeof(ctx));
    p = ir_builder_ctx_alloc(&ctx, 32);
    CHECK(p != NULL);
    CHECK(ctx.oom == false);
    free(p);

    /* size 0 is rejected without touching the flag */
    p = ir_builder_ctx_alloc(&ctx, 0);
    CHECK(p == NULL);
    CHECK(ctx.oom == false);

    /* an already-oom context returns NULL without allocating */
    ctx.oom = true;
    p = ir_builder_ctx_alloc(&ctx, 32);
    CHECK(p == NULL);
}

int main(void)
{
    test_entry_validation();
    fprintf(stderr, "after test_entry_validation\n");
    test_null_array_guards();
    fprintf(stderr, "after test_null_array_guards\n");
    test_empty_build_ok();
    fprintf(stderr, "after test_empty_build_ok\n");
    test_unsupported_default_mappers();
    fprintf(stderr, "after test_unsupported_default_mappers\n");
    test_two_phase_order();
    fprintf(stderr, "after test_two_phase_order\n");
    test_oom_ownership();
    fprintf(stderr, "after test_oom_ownership\n");
    test_ctx_alloc();
    fprintf(stderr, "after test_ctx_alloc\n");

    if (g_failures) {
        fprintf(stderr, "ir_builder_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("ir_builder_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
