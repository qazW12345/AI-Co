/* bootstrap/src/ir/ir_builder_decl_test.c
 *
 * WP-M0-16c1b IR builder Phase A mapping unit/integration tests: module
 * graph + imports, top-level declarations (struct layout facts, enum
 * continuation values, global const without storage, global var with
 * IRConst initializer, function with param slots and body placeholder),
 * the deterministic storage model (ir_builder_add_slot), type interning
 * and constant deduplication (incl. sizeof/alignof as IRConst_INT, enum
 * and address constants, struct-literal declaration-order reordering,
 * and composite const references: a global const/var initializer that
 * references another module-scope const of struct/array-of-struct type
 * reuses the referenced const's IRConst, satisfying AC3 dedup),
 * runtime module mapping, and the defensive IR_BUILDER_UNSUPPORTED
 * surface (slice-typed global initializers).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-ir16c1b' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_builder_decl_test.c \
 *     bootstrap/src/ir/ir_builder_decl.c bootstrap/src/ir/ir_builder_core.c \
 *     bootstrap/src/ir/ir_core.c bootstrap/src/ir/ir_dump.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-ir16c1b/ir_builder_decl_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-ir16c1b)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#include "ir_builder_decl.h"
#include "ir_dump.h"

#include "../parse/parse.h"

#include <direct.h>
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
 * Shared pipeline: load -> lex -> parse -> name_resolve -> completeness ->
 * layout -> convert -> optype -> const_eval_check (mirrors eval_core_test).
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    NameResult *result;
    DiagRecord **recs;
    size_t rn;
    NameStatus st;
    DiagRecord **trecs;
    size_t trn;
    TypeCheckStatus tst;
    LayoutBuild *build;
    DiagRecord **lrecs;
    size_t lrn;
    LayoutStatus lst;
    DiagRecord **crecs;
    size_t crn;
    ConvertStatus cst;
    DiagRecord **orecs;
    size_t orn;
    OptypeStatus ost;
    DiagRecord **erecs;
    size_t ern;
    EvalFailureSite *efails;
    size_t efailn;
    ConstEvalStatus esc;
} Pipeline;

/* Run the full pipeline over one source text (entry file "input.ai",
 * project root "."). `extra_root` is an optional directory the test
 * wrote imported module files into (may be NULL). */
static void pipeline_run_ex(Pipeline *p, const char *src_text,
                            const char *extra_root)
{
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;
    const char *root = extra_root != NULL ? extra_root : ".";
    const char *entry = extra_root != NULL ? "main.ai" : "input.ai";

    memset(p, 0, sizeof(*p));
    ld = load_source_from_bytes(entry, (const uint8_t *)src_text,
                                strlen(src_text), &p->src, &p->recs, &p->rn);
    CHECK(ld == LOAD_OK);
    if (ld != LOAD_OK) return;
    lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    CHECK(lx == LEX_OK);
    if (lx != LEX_OK) return;
    ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(root, "main", entry, p->src, p->program,
                         &p->result, &p->recs, &p->rn);
    if (p->st != NAME_OK) return;
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
    if (p->tst != TYPE_CHECK_OK) return;
    p->lst = types_layout_build(p->result, &p->build, &p->lrecs, &p->lrn);
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
}

static void pipeline_run(Pipeline *p, const char *src_text)
{
    pipeline_run_ex(p, src_text, NULL);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    layout_build_free(p->build);
    types_records_free(p->lrecs, p->lrn);
    types_records_free(p->crecs, p->crn);
    types_records_free(p->orecs, p->orn);
    types_records_free(p->erecs, p->ern);
    free(p->efails);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* Run the pipeline AND ir_builder_build; on IR_BUILDER_OK *out_build is
 * owned by the caller (ir_build_free). Returns the builder status. */
static IrBuilderStatus pipeline_build(Pipeline *p, const char *src_text,
                                      IrBuild **out_build)
{
    IrBuilderStatus bs;
    *out_build = NULL;
    pipeline_run(p, src_text);
    if (p->result == NULL || p->build == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* pipeline did not complete */
    }
    bs = ir_builder_build(p->result, p->build, out_build);
    return bs;
}

/* ---------------------------------------------------------------------------
 * Multi-module fixtures: write imported modules under a scratch directory
 * (bootstrap/stage0 is gitignored build output; no C: drive writes).
 * ------------------------------------------------------------------------- */

#define FIXTURE_ROOT "bootstrap/stage0/ir16c1b_fixtures"

static void fixture_mkdir(void)
{
    _mkdir("bootstrap");
    _mkdir("bootstrap/stage0");
    _mkdir(FIXTURE_ROOT);
}

static void fixture_write(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "FAIL: cannot write fixture %s\n", path);
        g_failures++;
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void fixture_cleanup_files(void)
{
    remove(FIXTURE_ROOT "/main.ai");
    remove(FIXTURE_ROOT "/util.ai");
    remove(FIXTURE_ROOT "/util2.ai");
}

/* ---------------------------------------------------------------------------
 * IR graph lookup helpers
 * ------------------------------------------------------------------------- */

/* The first build module whose name matches `fqn`. */
static IrNode *find_module(IrBuild *b, const char *fqn)
{
    size_t i;
    for (i = 0; i < b->nmodules; i++) {
        IrNode *m = b->modules[i];
        if (m != NULL && m->u.module.name != NULL &&
            strcmp(m->u.module.name, fqn) == 0) {
            return m;
        }
    }
    return NULL;
}

/* The declaration node of `module` whose name matches `fqn`. */
static IrNode *find_decl(IrBuild *b, const char *module_fqn,
                         const char *decl_fqn)
{
    IrNode *m = find_module(b, module_fqn);
    size_t i;
    if (m == NULL) {
        return NULL;
    }
    for (i = 0; i < m->u.module.ndecls; i++) {
        IrNode *d = m->u.module.decls[i];
        const char *n = NULL;
        switch (d->kind) {
        case IR_STRUCT_DECL:   n = d->u.struct_decl.name; break;
        case IR_ENUM_DECL:     n = d->u.enum_decl.name; break;
        case IR_GLOBAL_CONST:  n = d->u.global_const.name; break;
        case IR_GLOBAL_VAR:    n = d->u.global_var.name; break;
        case IR_FUNCTION:      n = d->u.function.name; break;
        default: break;
        }
        if (n != NULL && strcmp(n, decl_fqn) == 0) {
            return d;
        }
    }
    return NULL;
}

static IrConst *global_const_value(IrNode *node)
{
    return node != NULL && node->kind == IR_GLOBAL_CONST
               ? node->u.global_const.value : NULL;
}

static IrConst *global_var_init(IrNode *node)
{
    return node != NULL && node->kind == IR_GLOBAL_VAR
               ? node->u.global_var.init : NULL;
}

/* Run ir_core_verify and record the outcome; returns IR_OK when the
 * graph passes (no AIC-I0501 records). */
static IrStatus verify_build(IrBuild *b, DiagRecord ***out_recs,
                             size_t *out_n)
{
    *out_recs = NULL;
    *out_n = 0;
    return ir_core_verify(b, out_recs, out_n);
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* AC1: module units in canonical order (entry first, then imports
 * depth-first in import order), each with its IR_IMPORT list and
 * top-level declarations in source order. */
static void test_module_import_graph(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *main_m, *util_m, *util2_m;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    fixture_cleanup_files();
    fixture_mkdir();
    fixture_write(FIXTURE_ROOT "/util2.ai",
                  "module util2;\nvar v: i32 = 2;\n");
    fixture_write(FIXTURE_ROOT "/util.ai",
                  "module util;\nimport util2;\nvar u: i32 = 1;\n");
    pipeline_run_ex(&p,
                    "module main;\nimport util;\n",
                    FIXTURE_ROOT);
    CHECK(p.result != NULL && p.build != NULL);
    if (p.result == NULL || p.build == NULL) {
        pipeline_free(&p);
        return;
    }
    bs = ir_builder_build(p.result, p.build, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        /* canonical order: entry, then imports depth-first */
        CHECK(b->nmodules == 3);
        if (b->nmodules == 3) {
            CHECK(strcmp(b->modules[0]->u.module.name, "main") == 0);
            CHECK(strcmp(b->modules[1]->u.module.name, "util") == 0);
            CHECK(strcmp(b->modules[2]->u.module.name, "util2") == 0);
        }
        main_m = find_module(b, "main");
        util_m = find_module(b, "util");
        util2_m = find_module(b, "util2");
        CHECK(main_m != NULL && util_m != NULL && util2_m != NULL);
        if (main_m != NULL) {
            CHECK(main_m->u.module.nimports == 1);
            if (main_m->u.module.nimports == 1) {
                CHECK(main_m->u.module.imports[0]->kind == IR_IMPORT);
                CHECK(strcmp(main_m->u.module.imports[0]->u.import.name,
                             "util") == 0);
            }
            CHECK(main_m->u.module.ndecls == 0);
        }
        if (util_m != NULL) {
            CHECK(util_m->u.module.nimports == 1);
            if (util_m->u.module.nimports == 1) {
                CHECK(strcmp(util_m->u.module.imports[0]->u.import.name,
                             "util2") == 0);
            }
            CHECK(util_m->u.module.ndecls == 1);
            if (util_m->u.module.ndecls == 1) {
                CHECK(util_m->u.module.decls[0]->kind == IR_GLOBAL_VAR);
                CHECK(strcmp(util_m->u.module.decls[0]->u.global_var.name,
                             "util.u") == 0);
            }
        }
        if (util2_m != NULL) {
            CHECK(util2_m->u.module.ndecls == 1);
            if (util2_m->u.module.ndecls == 1) {
                CHECK(strcmp(util2_m->u.module.decls[0]->u.global_var.name,
                             "util2.v") == 0);
            }
        }
        /* source order within a module is the declaration array order */
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
    fixture_cleanup_files();
}

/* AC1: struct declarations carry layout facts (offsets/size/align) and
 * field types in declaration order; by-value nested struct fields
 * reference the interned struct type descriptor. */
static void test_struct_layout_facts(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "struct Pair { p: Point; b: u8; }\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *point, *pair;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        point = find_decl(b, "main", "main.Point");
        pair = find_decl(b, "main", "main.Pair");
        CHECK(point != NULL && pair != NULL);
        if (point != NULL) {
            CHECK(point->kind == IR_STRUCT_DECL);
            CHECK(point->u.struct_decl.size == 8);
            CHECK(point->u.struct_decl.align == 4);
            CHECK(point->u.struct_decl.nfields == 2);
            if (point->u.struct_decl.nfields == 2) {
                CHECK(strcmp(point->u.struct_decl.fields[0].name, "x") == 0);
                CHECK(point->u.struct_decl.fields[0].type->kind == IRT_I32);
                CHECK(point->u.struct_decl.fields[0].byte_offset == 0);
                CHECK(strcmp(point->u.struct_decl.fields[1].name, "y") == 0);
                CHECK(point->u.struct_decl.fields[1].byte_offset == 4);
            }
        }
        if (pair != NULL) {
            CHECK(pair->u.struct_decl.size == 12);
            CHECK(pair->u.struct_decl.align == 4);
            CHECK(pair->u.struct_decl.nfields == 2);
            if (pair->u.struct_decl.nfields == 2) {
                CHECK(pair->u.struct_decl.fields[0].type->kind == IRT_STRUCT);
                CHECK(pair->u.struct_decl.fields[0].type->u.decl == point);
                CHECK(pair->u.struct_decl.fields[0].byte_offset == 0);
                CHECK(pair->u.struct_decl.fields[1].type->kind == IRT_U8);
                CHECK(pair->u.struct_decl.fields[1].byte_offset == 8);
            }
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* AC1: enum declarations carry the underlying integer type and the
 * resolved member values (continuation values from the layout package),
 * in declaration order. */
static void test_enum_continuation(void)
{
    static const char src[] =
        "module main;\n"
        "enum Color: u8 { Red, Green = 5, Blue }\n"
        "enum Signed: i32 { A = -1, B }\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *color, *sign;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        color = find_decl(b, "main", "main.Color");
        sign = find_decl(b, "main", "main.Signed");
        CHECK(color != NULL && sign != NULL);
        if (color != NULL) {
            CHECK(color->kind == IR_ENUM_DECL);
            CHECK(color->u.enum_decl.underlying->kind == IRT_U8);
            CHECK(color->u.enum_decl.nmembers == 3);
            if (color->u.enum_decl.nmembers == 3) {
                CHECK(strcmp(color->u.enum_decl.members[0].name, "Red") == 0);
                CHECK(color->u.enum_decl.members[0].value == 0);
                CHECK(strcmp(color->u.enum_decl.members[1].name, "Green") == 0);
                CHECK(color->u.enum_decl.members[1].value == 5);
                CHECK(strcmp(color->u.enum_decl.members[2].name, "Blue") == 0);
                CHECK(color->u.enum_decl.members[2].value == 6);
            }
        }
        if (sign != NULL) {
            CHECK(sign->u.enum_decl.underlying->kind == IRT_I32);
            CHECK(sign->u.enum_decl.nmembers == 2);
            if (sign->u.enum_decl.nmembers == 2) {
                CHECK(sign->u.enum_decl.members[0].value == -1);
                CHECK(sign->u.enum_decl.members[1].value == 0);
            }
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* AC1: global consts have no addressable storage (no IR_GLOBAL node
 * exists in the build) and carry an IRConst value; global vars carry a
 * static slot + IRConst initializer. Also covers IRC_STR and IRC_NULL. */
static void test_global_const_var_storage(void)
{
    static const char src[] =
        "module main;\n"
        "const c: i32 = 42;\n"
        "var g: i32 = 7;\n"
        "const s: str = \"hi\";\n"
        "const p: i32* = null;\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *cn, *gn, *sn, *pn;
    IrConst *cv, *gv, *sv, *pv;
    size_t i;
    bool found_global_node = false;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        cn = find_decl(b, "main", "main.c");
        gn = find_decl(b, "main", "main.g");
        sn = find_decl(b, "main", "main.s");
        pn = find_decl(b, "main", "main.p");
        CHECK(cn != NULL && gn != NULL && sn != NULL && pn != NULL);
        cv = global_const_value(cn);
        gv = global_var_init(gn);
        sv = global_const_value(sn);
        pv = global_const_value(pn);
        CHECK(cv != NULL && gv != NULL && sv != NULL && pv != NULL);
        if (cv != NULL) {
            CHECK(cv->kind == IRC_INT);
            CHECK(cv->type->kind == IRT_I32);
            CHECK(cv->u.int_bits == 42);
        }
        if (gv != NULL) {
            CHECK(gv->kind == IRC_INT);
            CHECK(gv->u.int_bits == 7);
        }
        if (sv != NULL) {
            CHECK(sv->kind == IRC_STR);
            CHECK(sv->u.str.len == 2);
            CHECK(sv->u.str.len == 2 &&
                  memcmp(sv->u.str.bytes, "hi", 2) == 0);
        }
        if (pv != NULL) {
            CHECK(pv->kind == IRC_NULL);
            CHECK(pv->type->kind == IRT_PTR);
            CHECK(pv->type->u.ptr.elem->kind == IRT_I32);
        }
        /* const has no addressable storage: no IR_GLOBAL node anywhere */
        for (i = 0; i < b->nnodes; i++) {
            if (b->nodes[i]->kind == IR_GLOBAL) {
                found_global_node = true;
            }
        }
        CHECK(found_global_node == false);
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* AC2: the deterministic storage model - parameter slots first (in
 * parameter order), then local slots in first-declaration order, then
 * temporaries with deterministic indices; the function body is the Phase
 * A placeholder block. */
static void test_function_storage_model(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32, b: bool) -> void { return; }\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fn;
    IrSlot *local, *temp;
    DiagSpan *span = diag_span_new_point("input.ai", 2, 1, 5);
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        fn = find_decl(b, "main", "main.f");
        CHECK(fn != NULL);
        if (fn != NULL) {
            CHECK(fn->kind == IR_FUNCTION);
            CHECK(fn->u.function.ret_type->kind == IRT_VOID);
            CHECK(fn->u.function.noreturn == false);
            CHECK(fn->u.function.nparams == 2);
            CHECK(fn->u.function.nslots == 2);
            if (fn->u.function.nparams == 2) {
                CHECK(strcmp(fn->u.function.params[0].name, "a") == 0);
                CHECK(fn->u.function.params[0].type->kind == IRT_I32);
                CHECK(fn->u.function.params[0].slot_index == 0);
                CHECK(strcmp(fn->u.function.params[1].name, "b") == 0);
                CHECK(fn->u.function.params[1].type->kind == IRT_BOOL);
                CHECK(fn->u.function.params[1].slot_index == 1);
            }
            if (fn->u.function.nslots == 2) {
                CHECK(fn->u.function.slots[0]->kind == IR_SLOT_PARAM);
                CHECK(fn->u.function.slots[0]->index == 0);
                CHECK(strcmp(fn->u.function.slots[0]->name, "a") == 0);
                CHECK(fn->u.function.slots[1]->kind == IR_SLOT_PARAM);
                CHECK(fn->u.function.slots[1]->index == 1);
                CHECK(strcmp(fn->u.function.slots[1]->name, "b") == 0);
            }
            CHECK(fn->u.function.body != NULL);
            CHECK(fn->u.function.body->kind == IR_BLOCK);
            CHECK(fn->u.function.body->u.block.nstmts == 0);

            /* storage model API: locals then temporaries continue the
             * slot table with deterministic indices */
            local = ir_builder_add_slot(b, fn, IR_SLOT_LOCAL, "x",
                                        ir_type_i32(b), span);
            CHECK(local != NULL);
            if (local != NULL) {
                CHECK(local->index == 2);
                CHECK(local->kind == IR_SLOT_LOCAL);
                CHECK(strcmp(local->name, "x") == 0);
            }
            temp = ir_builder_add_slot(b, fn, IR_SLOT_TEMP, NULL,
                                       ir_type_bool(b), NULL);
            CHECK(temp != NULL);
            if (temp != NULL) {
                CHECK(temp->index == 3);
                CHECK(temp->kind == IR_SLOT_TEMP);
                CHECK(temp->name == NULL);
            }
            CHECK(fn->u.function.nslots == 4);
            if (fn->u.function.nslots == 4) {
                CHECK(fn->u.function.slots[2]->kind == IR_SLOT_LOCAL);
                CHECK(fn->u.function.slots[2]->index == 2);
                CHECK(fn->u.function.slots[3]->kind == IR_SLOT_TEMP);
                CHECK(fn->u.function.slots[3]->index == 3);
            }
            CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
            ir_records_free(recs, nrecs);
        }
        ir_build_free(b);
    }
    diag_span_free(span);
    pipeline_free(&p);
}

/* AC3: type interning - identical types share one interned descriptor;
 * composite descriptors are created on first occurrence in canonical
 * order after the 13 pre-interned base types. */
static void test_type_interning(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "var a1: i32[4] = [1, 2, 3, 4];\n"
        "var a2: i32[4] = [5, 6, 7, 8];\n"
        "var p1: Point* = null;\n"
        "var p2: Point* = null;\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *a1, *a2, *p1, *p2;
    IrType *arr_t, *ptr_t, *point_t;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        a1 = find_decl(b, "main", "main.a1");
        a2 = find_decl(b, "main", "main.a2");
        p1 = find_decl(b, "main", "main.p1");
        p2 = find_decl(b, "main", "main.p2");
        CHECK(a1 != NULL && a2 != NULL && p1 != NULL && p2 != NULL);
        if (a1 != NULL && a2 != NULL && p1 != NULL && p2 != NULL) {
            arr_t = a1->u.global_var.type;
            CHECK(a2->u.global_var.type == arr_t);   /* shared descriptor */
            CHECK(arr_t->kind == IRT_ARRAY);
            CHECK(arr_t->u.array.elem->kind == IRT_I32);
            CHECK(arr_t->u.array.extent == 4);
            CHECK(arr_t->size == 16);
            CHECK(arr_t->align == 4);

            ptr_t = p1->u.global_var.type;
            CHECK(p2->u.global_var.type == ptr_t);   /* shared descriptor */
            CHECK(ptr_t->kind == IRT_PTR);
            point_t = ptr_t->u.ptr.elem;
            CHECK(point_t->kind == IRT_STRUCT);
            CHECK(point_t->u.decl->kind == IR_STRUCT_DECL);
            CHECK(point_t->size == 8);
            CHECK(point_t->align == 4);

            /* first-occurrence canonical order: base types 0..12, then
             * i32[4] (a1), then Point (elem of Point*), then Point* */
            CHECK(arr_t->id == 13);
            CHECK(point_t->id == 14);
            CHECK(ptr_t->id == 15);
            CHECK(b->ntypes == 16);
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* AC3: constant deduplication - identical constants share one IRConst
 * (first-occurrence representative); sizeof/alignof map to IRConst_INT
 * of type usize; enum members map to IRConst_ENUM; address-of maps to
 * IRConst_ADDR targeting the IR_GLOBAL_VAR node. */
static void test_const_dedup_and_forms(void)
{
    static const char src[] =
        "module main;\n"
        "struct S { a: i8; b: i32; }\n"
        "enum Color: u8 { Red, Green = 5, Blue }\n"
        "const k1: i32 = 5;\n"
        "const k2: i32 = 5;\n"
        "const s1: str = \"abc\";\n"
        "const s2: str = \"abc\";\n"
        "const e: Color = Color.Red;\n"
        "const sz: usize = sizeof(S);\n"
        "const al: usize = alignof(S);\n"
        "var g: i32 = 1;\n"
        "const pg: i32* = &g;\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *k1n, *k2n, *s1n, *s2n, *en, *szn, *aln, *gn, *pgn;
    IrConst *k1v, *k2v, *s1v, *s2v, *ev, *szv, *alv, *pgv;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        k1n = find_decl(b, "main", "main.k1");
        k2n = find_decl(b, "main", "main.k2");
        s1n = find_decl(b, "main", "main.s1");
        s2n = find_decl(b, "main", "main.s2");
        en = find_decl(b, "main", "main.e");
        szn = find_decl(b, "main", "main.sz");
        aln = find_decl(b, "main", "main.al");
        gn = find_decl(b, "main", "main.g");
        pgn = find_decl(b, "main", "main.pg");
        CHECK(k1n != NULL && k2n != NULL && s1n != NULL && s2n != NULL &&
              en != NULL && szn != NULL && aln != NULL && gn != NULL &&
              pgn != NULL);
        k1v = global_const_value(k1n);
        k2v = global_const_value(k2n);
        s1v = global_const_value(s1n);
        s2v = global_const_value(s2n);
        ev = global_const_value(en);
        szv = global_const_value(szn);
        alv = global_const_value(aln);
        pgv = global_const_value(pgn);
        CHECK(k1v != NULL && k2v != NULL && s1v != NULL && s2v != NULL &&
              ev != NULL && szv != NULL && alv != NULL && pgv != NULL);
        /* dedup: identical constants share one IRConst */
        if (k1v != NULL && k2v != NULL) {
            CHECK(k1v == k2v);
            CHECK(k1v->kind == IRC_INT);
            CHECK(k1v->type->kind == IRT_I32);
            CHECK(k1v->u.int_bits == 5);
        }
        if (s1v != NULL && s2v != NULL) {
            CHECK(s1v == s2v);
            CHECK(s1v->kind == IRC_STR);
            CHECK(s1v->u.str.len == 3 &&
                  memcmp(s1v->u.str.bytes, "abc", 3) == 0);
        }
        /* enum member constant */
        if (ev != NULL) {
            CHECK(ev->kind == IRC_ENUM);
            CHECK(ev->type->kind == IRT_ENUM);
            CHECK(ev->u.en.value == 0);
            CHECK(ev->u.en.enum_decl->kind == IR_ENUM_DECL);
        }
        /* sizeof/alignof as IRConst_INT of type usize (contract 4.5) */
        if (szv != NULL) {
            CHECK(szv->kind == IRC_INT);
            CHECK(szv->type->kind == IRT_USIZE);
            CHECK(szv->u.int_bits == 8);   /* struct S: i8@0, i32@4 -> size 8 */
        }
        if (alv != NULL) {
            CHECK(alv->kind == IRC_INT);
            CHECK(alv->type->kind == IRT_USIZE);
            CHECK(alv->u.int_bits == 4);
        }
        /* address constant references the global var's IR node */
        if (pgv != NULL && gn != NULL) {
            CHECK(pgv->kind == IRC_ADDR);
            CHECK(pgv->type->kind == IRT_PTR);
            CHECK(pgv->u.addr.target == gn);
            CHECK(pgv->u.addr.offset == 0);
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* AC1/AC3: struct constants emit field values in declaration order even
 * when the literal lists them in another order (spec 12.7); nested
 * struct literals inside array constants reorder too. */
static void test_struct_literal_reorder(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "const p: Point = Point { y: 2, x: 1 };\n"
        "const ps: Point[2] = [Point { y: 4, x: 3 }, Point { x: 5, y: 6 }];\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *pn, *psn;
    IrConst *pv, *psv;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        pn = find_decl(b, "main", "main.p");
        psn = find_decl(b, "main", "main.ps");
        CHECK(pn != NULL && psn != NULL);
        pv = global_const_value(pn);
        psv = global_const_value(psn);
        CHECK(pv != NULL && psv != NULL);
        if (pv != NULL) {
            CHECK(pv->kind == IRC_STRUCT);
            CHECK(pv->u.strukt.count == 2);
            if (pv->u.strukt.count == 2) {
                /* declaration order: x first, then y */
                CHECK(pv->u.strukt.items[0]->kind == IRC_INT);
                CHECK(pv->u.strukt.items[0]->u.int_bits == 1);
                CHECK(pv->u.strukt.items[1]->u.int_bits == 2);
            }
        }
        if (psv != NULL) {
            CHECK(psv->kind == IRC_ARRAY);
            CHECK(psv->u.arr.count == 2);
            if (psv->u.arr.count == 2) {
                CHECK(psv->u.arr.items[0]->kind == IRC_STRUCT);
                CHECK(psv->u.arr.items[0]->u.strukt.count == 2);
                CHECK(psv->u.arr.items[0]->u.strukt.items[0]->u.int_bits == 3);
                CHECK(psv->u.arr.items[0]->u.strukt.items[1]->u.int_bits == 4);
                CHECK(psv->u.arr.items[1]->u.strukt.items[0]->u.int_bits == 5);
                CHECK(psv->u.arr.items[1]->u.strukt.items[1]->u.int_bits == 6);
            }
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* MAJOR-1 remediation (reviewer2 t_e1758837): global const/var
 * initializers that reference another module-scope const of composite
 * type map by reusing the referenced const's IRConst (AC3 dedup), or,
 * for forward references, by recovering field names from the referenced
 * const's own initializer AST. Covers the three reproduced forms
 * (`const b: Point = a;`, `var g: Point = a;`, `[a, a]` inside an array
 * literal), a forward reference, and a whole-array const reference. */
static void test_const_ref_composite(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "const a: Point = Point { x: 1, y: 2 };\n"
        "const b: Point = a;\n"
        "var g: Point = a;\n"
        "const ps: Point[2] = [a, a];\n"
        "const fwd: Point = later;\n"
        "const later: Point = Point { y: 4, x: 3 };\n"
        "const pc: Point[2] = ps;\n"
        "const c1: Point = c2;\n"
        "const c2: Point = c3;\n"
        "const c3: Point = Point { x: 7, y: 8 };\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *an, *bn, *gn, *psn, *fwdn, *latn, *pcn, *c1n, *c2n, *c3n;
    IrConst *av, *bv, *gv, *psv, *fwdv, *latv, *pcv, *c1v, *c2v, *c3v;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        an = find_decl(b, "main", "main.a");
        bn = find_decl(b, "main", "main.b");
        gn = find_decl(b, "main", "main.g");
        psn = find_decl(b, "main", "main.ps");
        fwdn = find_decl(b, "main", "main.fwd");
        latn = find_decl(b, "main", "main.later");
        pcn = find_decl(b, "main", "main.pc");
        c1n = find_decl(b, "main", "main.c1");
        c2n = find_decl(b, "main", "main.c2");
        c3n = find_decl(b, "main", "main.c3");
        CHECK(an != NULL && bn != NULL && gn != NULL && psn != NULL &&
              fwdn != NULL && latn != NULL && pcn != NULL &&
              c1n != NULL && c2n != NULL && c3n != NULL);
        av = global_const_value(an);
        bv = global_const_value(bn);
        gv = global_var_init(gn);
        psv = global_const_value(psn);
        fwdv = global_const_value(fwdn);
        latv = global_const_value(latn);
        pcv = global_const_value(pcn);
        c1v = global_const_value(c1n);
        c2v = global_const_value(c2n);
        c3v = global_const_value(c3n);
        CHECK(av != NULL && bv != NULL && gv != NULL && psv != NULL &&
              fwdv != NULL && latv != NULL && pcv != NULL &&
              c1v != NULL && c2v != NULL && c3v != NULL);
        if (av != NULL) {
            CHECK(av->kind == IRC_STRUCT);
            CHECK(av->u.strukt.count == 2);
            if (av->u.strukt.count == 2) {
                CHECK(av->u.strukt.items[0]->u.int_bits == 1);
                CHECK(av->u.strukt.items[1]->u.int_bits == 2);
            }
        }
        /* const ref to a struct const: b's IRConst IS a's (AC3 dedup) */
        if (bv != NULL) {
            CHECK(bv == av);
        }
        /* global var initialized from a struct const: same IRConst */
        if (gv != NULL) {
            CHECK(gv == av);
        }
        /* struct const refs inside an array literal dedup to a's const */
        if (psv != NULL) {
            CHECK(psv->kind == IRC_ARRAY);
            CHECK(psv->u.arr.count == 2);
            if (psv->u.arr.count == 2) {
                CHECK(psv->u.arr.items[0] == av);
                CHECK(psv->u.arr.items[1] == av);
            }
        }
        /* forward reference (fwd declared before later): names recovered
         * from later's initializer -> declaration order x=3, y=4; after
         * the whole build interning dedups fwd's const to later's */
        if (fwdv != NULL && latv != NULL) {
            CHECK(fwdv == latv);
            CHECK(fwdv->kind == IRC_STRUCT);
            if (fwdv->kind == IRC_STRUCT && fwdv->u.strukt.count == 2) {
                CHECK(fwdv->u.strukt.items[0]->u.int_bits == 3);
                CHECK(fwdv->u.strukt.items[1]->u.int_bits == 4);
            }
        }
        /* whole-array const reference: pc's array const IS ps's */
        if (pcv != NULL && psv != NULL) {
            CHECK(pcv == psv);
        }
        /* chained forward reference (c1 -> c2 -> c3, declared in
         * reverse): all three dedup to the terminal struct's const */
        if (c1v != NULL && c2v != NULL && c3v != NULL) {
            CHECK(c1v == c3v);
            CHECK(c2v == c3v);
            CHECK(c1v->kind == IRC_STRUCT);
            if (c1v->kind == IRC_STRUCT && c1v->u.strukt.count == 2) {
                CHECK(c1v->u.strukt.items[0]->u.int_bits == 7);
                CHECK(c1v->u.strukt.items[1]->u.int_bits == 8);
            }
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* MAJOR-1 remediation: cross-module composite const reference. The
 * referenced const lives in an imported module whose declarations are
 * filled after the entry module (canonical order: entry first, then
 * imports), so the referenced const's IRConst is not yet mapped when
 * the entry module's const/var is filled; the field/element names are
 * recovered from the referenced const's own initializer AST in its
 * module. */
static void test_const_ref_composite_cross_module(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *util_a, *main_b, *main_g, *main_pc;
    IrConst *av, *bv, *gv, *pcv;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    fixture_cleanup_files();
    fixture_mkdir();
    fixture_write(FIXTURE_ROOT "/util.ai",
                  "module util;\n"
                  "pub struct Point { x: i32; y: i32; }\n"
                  "pub const a: Point = Point { y: 2, x: 1 };\n"
                  "pub const ps: Point[2] = [a, a];\n");
    pipeline_run_ex(&p,
                    "module main;\n"
                    "import util;\n"
                    "const b: util.Point = util.a;\n"
                    "var g: util.Point = util.a;\n"
                    "const pc: util.Point[2] = util.ps;\n",
                    FIXTURE_ROOT);
    CHECK(p.result != NULL && p.build != NULL);
    if (p.result == NULL || p.build == NULL) {
        pipeline_free(&p);
        fixture_cleanup_files();
        return;
    }
    bs = ir_builder_build(p.result, p.build, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        util_a = find_decl(b, "util", "util.a");
        main_b = find_decl(b, "main", "main.b");
        main_g = find_decl(b, "main", "main.g");
        main_pc = find_decl(b, "main", "main.pc");
        CHECK(util_a != NULL && main_b != NULL && main_g != NULL &&
              main_pc != NULL);
        av = global_const_value(util_a);
        bv = global_const_value(main_b);
        gv = global_var_init(main_g);
        pcv = global_const_value(main_pc);
        CHECK(av != NULL && bv != NULL && gv != NULL && pcv != NULL);
        if (av != NULL) {
            CHECK(av->kind == IRC_STRUCT);
            CHECK(av->u.strukt.count == 2);
            if (av->u.strukt.count == 2) {
                /* declaration order despite literal y-first: x=1, y=2 */
                CHECK(av->u.strukt.items[0]->u.int_bits == 1);
                CHECK(av->u.strukt.items[1]->u.int_bits == 2);
            }
        }
        /* entry-module const/var referencing an imported struct const
         * dedup to the same IRConst */
        if (bv != NULL) {
            CHECK(bv == av);
        }
        if (gv != NULL) {
            CHECK(gv == av);
        }
        if (pcv != NULL) {
            CHECK(pcv->kind == IRC_ARRAY);
            CHECK(pcv->u.arr.count == 2);
            if (pcv->u.arr.count == 2) {
                CHECK(pcv->u.arr.items[0] == av);
                CHECK(pcv->u.arr.items[1] == av);
            }
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
    fixture_cleanup_files();
}

/* MAJOR-2 remediation (reviewer2 t_0234b81a): accepted cross-type
 * const references must produce type-consistent IR. `const a: i32 = 5;`
 * reused at a wider declared position (`const b: i64 = a;`,
 * `var g: i64 = a;`, `const arr: i64[2] = [a, a];`) previously attached
 * the i32 IRConst directly (AIC-I0501 invariant 3 failures on the
 * scalar/var forms; silently I32 items inside the I64[2] array). The
 * reuse path is type-aware: same-type references still dedup, and
 * cross-type integer references map to the declared type (the convert
 * phase accepts the widening, spec 11.6 Table 11.1). Covers the three
 * reproduced forms plus unsigned->signed widening and a negative
 * i32->i64 bit-pattern conversion, and the plain-literal-in-wider-array
 * form of the same defect class. */
static void test_const_ref_cross_type(void)
{
    static const char src[] =
        "module main;\n"
        "const a: i32 = 5;\n"
        "const b: i64 = a;\n"
        "var g: i64 = a;\n"
        "const arr: i64[2] = [a, a];\n"
        "const lit: i64[2] = [1, 2];\n"
        "const u: u32 = 4294967295u32;\n"
        "const w: i64 = u;\n"
        "const neg: i32 = -1;\n"
        "const n64: i64 = neg;\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *an, *bn, *gn, *arrn, *litn, *un, *wn, *negn, *n64n;
    IrConst *av, *bv, *gv, *arrv, *litv, *uv, *wv, *negv, *n64v;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        an = find_decl(b, "main", "main.a");
        bn = find_decl(b, "main", "main.b");
        gn = find_decl(b, "main", "main.g");
        arrn = find_decl(b, "main", "main.arr");
        litn = find_decl(b, "main", "main.lit");
        un = find_decl(b, "main", "main.u");
        wn = find_decl(b, "main", "main.w");
        negn = find_decl(b, "main", "main.neg");
        n64n = find_decl(b, "main", "main.n64");
        CHECK(an != NULL && bn != NULL && gn != NULL && arrn != NULL &&
              litn != NULL && un != NULL && wn != NULL && negn != NULL &&
              n64n != NULL);
        av = global_const_value(an);
        bv = global_const_value(bn);
        gv = global_var_init(gn);
        arrv = global_const_value(arrn);
        litv = global_const_value(litn);
        uv = global_const_value(un);
        wv = global_const_value(wn);
        negv = global_const_value(negn);
        n64v = global_const_value(n64n);
        CHECK(av != NULL && bv != NULL && gv != NULL && arrv != NULL &&
              litv != NULL && uv != NULL && wv != NULL && negv != NULL &&
              n64v != NULL);
        /* control: the i32 source const keeps its own type */
        if (av != NULL) {
            CHECK(av->kind == IRC_INT);
            CHECK(av->type->kind == IRT_I32);
            CHECK(av->u.int_bits == 5);
        }
        /* const ref reused at a wider declared type: b carries I64 5 */
        if (bv != NULL) {
            CHECK(bv->kind == IRC_INT);
            CHECK(bv->type->kind == IRT_I64);
            CHECK(bv->u.int_bits == 5);
        }
        /* global var initialized from an i32 const at an i64 position */
        if (gv != NULL) {
            CHECK(gv->kind == IRC_INT);
            CHECK(gv->type->kind == IRT_I64);
            CHECK(gv->u.int_bits == 5);
        }
        /* array literal of i32 const refs at an i64[2] position: every
         * item carries the declared element type */
        if (arrv != NULL) {
            CHECK(arrv->kind == IRC_ARRAY);
            CHECK(arrv->type->kind == IRT_ARRAY);
            if (arrv->type->kind == IRT_ARRAY) {
                CHECK(arrv->type->u.array.elem->kind == IRT_I64);
                CHECK(arrv->type->u.array.extent == 2);
            }
            CHECK(arrv->u.arr.count == 2);
            if (arrv->u.arr.count == 2) {
                CHECK(arrv->u.arr.items[0]->kind == IRC_INT);
                CHECK(arrv->u.arr.items[0]->type->kind == IRT_I64);
                CHECK(arrv->u.arr.items[0]->u.int_bits == 5);
                CHECK(arrv->u.arr.items[1]->kind == IRC_INT);
                CHECK(arrv->u.arr.items[1]->type->kind == IRT_I64);
                CHECK(arrv->u.arr.items[1]->u.int_bits == 5);
            }
        }
        /* same defect class: plain i32 literals in an i64[2] position */
        if (litv != NULL) {
            CHECK(litv->kind == IRC_ARRAY);
            CHECK(litv->type->kind == IRT_ARRAY);
            CHECK(litv->u.arr.count == 2);
            if (litv->u.arr.count == 2) {
                CHECK(litv->u.arr.items[0]->type->kind == IRT_I64);
                CHECK(litv->u.arr.items[0]->u.int_bits == 1);
                CHECK(litv->u.arr.items[1]->type->kind == IRT_I64);
                CHECK(litv->u.arr.items[1]->u.int_bits == 2);
            }
        }
        /* unsigned -> signed widening (different sign, target wider):
         * the bit pattern is re-read as the declared type */
        if (uv != NULL) {
            CHECK(uv->kind == IRC_INT);
            CHECK(uv->type->kind == IRT_U32);
            CHECK(uv->u.int_bits == 4294967295ULL);
        }
        if (wv != NULL) {
            CHECK(wv->kind == IRC_INT);
            CHECK(wv->type->kind == IRT_I64);
            CHECK(wv->u.int_bits == 4294967295ULL);
        }
        /* negative i32 -> i64: the int64 EvalInt carries the sign, so
         * the i64 bit pattern is the sign-extended value */
        if (negv != NULL) {
            CHECK(negv->kind == IRC_INT);
            CHECK(negv->type->kind == IRT_I32);
            CHECK(negv->u.int_bits == 0xFFFFFFFFULL);
        }
        if (n64v != NULL) {
            CHECK(n64v->kind == IRC_INT);
            CHECK(n64v->type->kind == IRT_I64);
            CHECK(n64v->u.int_bits == 0xFFFFFFFFFFFFFFFFULL);
        }
        /* the whole graph must pass the project verifier (the scalar
         * forms previously failed AIC-I0501 invariant 3) */
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* Module graph edge: runtime modules (rt.*) are mapped as module units
 * with IR_FUNCTION declarations; the noreturn flag is set on
 * rt.proc.exit / rt.trap.report only (contract 4.2). */
static void test_runtime_modules(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.io;\n"
        "import rt.proc;\n"
        "import rt.trap;\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *main_m, *io_m, *proc_m, *trap_m;
    IrNode *exit_fn, *report_fn, *write_fn;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        main_m = find_module(b, "main");
        io_m = find_module(b, "rt.io");
        proc_m = find_module(b, "rt.proc");
        trap_m = find_module(b, "rt.trap");
        CHECK(main_m != NULL && io_m != NULL && proc_m != NULL &&
              trap_m != NULL);
        if (main_m != NULL) {
            CHECK(main_m->u.module.nimports == 3);
            if (main_m->u.module.nimports == 3) {
                CHECK(strcmp(main_m->u.module.imports[0]->u.import.name,
                             "rt.io") == 0);
                CHECK(strcmp(main_m->u.module.imports[1]->u.import.name,
                             "rt.proc") == 0);
                CHECK(strcmp(main_m->u.module.imports[2]->u.import.name,
                             "rt.trap") == 0);
            }
        }
        /* runtime surface: rt.io has 7 members, all IR_FUNCTION */
        if (io_m != NULL) {
            size_t i;
            CHECK(io_m->u.module.ndecls == 7);
            for (i = 0; i < io_m->u.module.ndecls && i < 7; i++) {
                CHECK(io_m->u.module.decls[i]->kind == IR_FUNCTION);
            }
        }
        exit_fn = find_decl(b, "rt.proc", "rt.proc.exit");
        report_fn = find_decl(b, "rt.trap", "rt.trap.report");
        write_fn = find_decl(b, "rt.io", "rt.io.write");
        CHECK(exit_fn != NULL && report_fn != NULL && write_fn != NULL);
        if (exit_fn != NULL) {
            CHECK(exit_fn->u.function.noreturn == true);
            CHECK(exit_fn->u.function.body == NULL);
            CHECK(exit_fn->u.function.ret_type->kind == IRT_VOID);
        }
        if (report_fn != NULL) {
            CHECK(report_fn->u.function.noreturn == true);
            CHECK(report_fn->u.function.body == NULL);
        }
        if (write_fn != NULL) {
            CHECK(write_fn->u.function.noreturn == false);
            CHECK(write_fn->u.function.body != NULL);
            CHECK(write_fn->u.function.body->kind == IR_BLOCK);
        }
        CHECK(verify_build(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        /* runtime graphs must also round-trip through the deterministic
         * dump (16b2, read-only): synthetic spans and NULL noreturn
         * bodies must reconstruct byte-identically */
        CHECK(ir_dump_verify(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        ir_build_free(b);
    }
    pipeline_free(&p);
}

/* Disclosed gap (ir_builder_core.h / ir_builder_decl.h): a slice-typed
 * global initializer (EvalValue EVAL_VAL_SLICE) is outside the closed
 * IRConst set -> IR_BUILDER_UNSUPPORTED with nothing owned. */
static void test_defensive_unsupported(void)
{
    static const char src[] =
        "module main;\n"
        "var arr: i32[3] = [1, 2, 3];\n"
        "var s: i32[] = arr[..];\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    LayoutBuild lb;
    IrBuild *out = NULL;
    NameResult r;

    bs = pipeline_build(&p, src, &b);
    /* The pipeline must accept the program (slice of a static array is a
     * constant expression); the builder then hits the disclosed gap. */
    CHECK(p.result != NULL && p.build != NULL);
    if (p.result != NULL && p.build != NULL) {
        CHECK(bs == IR_BUILDER_UNSUPPORTED);
        CHECK(b == NULL);
    }
    pipeline_free(&p);
    if (b != NULL) {
        ir_build_free(b);
    }

    /* Defensive entry validation with the 16c1b mappers installed:
     * malformed module array -> IR_BUILDER_UNSUPPORTED, nothing owned. */
    memset(&r, 0, sizeof(r));
    memset(&lb, 0, sizeof(lb));
    r.modules = NULL;
    r.nmodules = 1;
    out = NULL;
    bs = ir_builder_build(&r, &lb, &out);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(out == NULL);
}

/* Determinism + invariants end to end: a representative build passes
 * ir_dump_verify (round-trip byte-identical + no AIC-I0501), and two
 * builds of the same source produce byte-identical dumps. */
static void test_verify_roundtrip(void)
{
    static const char src[] =
        "module main;\n"
        "struct S { a: i8; b: i32; }\n"
        "enum Color: u8 { Red, Green = 5, Blue }\n"
        "const k: i32 = 5;\n"
        "var g: i32 = 7;\n"
        "fn f(x: i32) -> void { return; }\n";
    Pipeline p1, p2;
    IrBuild *b1 = NULL, *b2 = NULL;
    IrBuilderStatus bs;
    DiagBuf d1, d2;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    bs = pipeline_build(&p1, src, &b1);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b1 != NULL);
    if (bs == IR_BUILDER_OK && b1 != NULL) {
        /* round-trip + invariant verification (16b2, read-only) */
        CHECK(ir_dump_verify(b1, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        diag_buf_init(&d1);
        CHECK(ir_dump_write(b1, &d1));
        ir_build_free(b1);
        b1 = NULL;

        /* a second build of the same source is byte-identical */
        bs = pipeline_build(&p2, src, &b2);
        CHECK(bs == IR_BUILDER_OK);
        CHECK(b2 != NULL);
        if (bs == IR_BUILDER_OK && b2 != NULL) {
            diag_buf_init(&d2);
            CHECK(ir_dump_write(b2, &d2));
            CHECK(d1.len == d2.len);
            CHECK(d1.len == d2.len &&
                  memcmp(d1.data, d2.data, d1.len) == 0);
            diag_buf_free(&d2);
        }
        diag_buf_free(&d1);
    }
    if (b1 != NULL) {
        ir_build_free(b1);
    }
    if (b2 != NULL) {
        ir_build_free(b2);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

int main(void)
{
    ir_builder_decl_install();

    test_module_import_graph();
    fprintf(stderr, "after test_module_import_graph\n");
    test_struct_layout_facts();
    fprintf(stderr, "after test_struct_layout_facts\n");
    test_enum_continuation();
    fprintf(stderr, "after test_enum_continuation\n");
    test_global_const_var_storage();
    fprintf(stderr, "after test_global_const_var_storage\n");
    test_function_storage_model();
    fprintf(stderr, "after test_function_storage_model\n");
    test_type_interning();
    fprintf(stderr, "after test_type_interning\n");
    test_const_dedup_and_forms();
    fprintf(stderr, "after test_const_dedup_and_forms\n");
    test_struct_literal_reorder();
    fprintf(stderr, "after test_struct_literal_reorder\n");
    test_const_ref_composite();
    fprintf(stderr, "after test_const_ref_composite\n");
    test_const_ref_composite_cross_module();
    fprintf(stderr, "after test_const_ref_composite_cross_module\n");
    test_const_ref_cross_type();
    fprintf(stderr, "after test_const_ref_cross_type\n");
    test_runtime_modules();
    fprintf(stderr, "after test_runtime_modules\n");
    test_defensive_unsupported();
    fprintf(stderr, "after test_defensive_unsupported\n");
    test_verify_roundtrip();
    fprintf(stderr, "after test_verify_roundtrip\n");

    /* restore the defensive default stubs so later tests in the same
     * binary run against them (single-build compiler convention) */
    ir_builder_set_module_mapper(NULL);
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_body_mapper(NULL);

    if (g_failures) {
        fprintf(stderr, "ir_builder_decl_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("ir_builder_decl_test: %d checks, 0 failures\n", g_checks);
    return 0;
}

