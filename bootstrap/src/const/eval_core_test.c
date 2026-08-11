/* bootstrap/src/const/eval_core_test.c
 *
 * WP-M0-12a unit and integration tests: the constant-expression
 * evaluator core (spec sec. 10.5) - composition, typed constant values,
 * sizeof/alignof, static addresses, enum members, checked arithmetic
 * routing (sec. 11.3), cast/wrap (sec. 11.5), and the build-level
 * AIC-E0401 rejection of non-const uses.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-const' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/const/eval_core_test.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-const/eval_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-const)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "eval_core.h"

#include "../parse/parse.h"

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
 * Shared pipeline: load -> lex -> parse -> name_resolve ->
 * types_check_completeness -> types_layout_build -> types_convert_check ->
 * types_optype_check -> const_eval_check.
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    NameResult *result;
    DiagRecord **recs;      /* name-phase records */
    size_t rn;
    NameStatus st;
    DiagRecord **trecs;     /* completeness records */
    size_t trn;
    TypeCheckStatus tst;
    LayoutBuild *build;
    DiagRecord **lrecs;     /* layout records (AIC-T0301) */
    size_t lrn;
    LayoutStatus lst;
    DiagRecord **crecs;     /* convert records (AIC-T0307) */
    size_t crn;
    ConvertStatus cst;
    DiagRecord **orecs;     /* optype records (AIC-T03xx) */
    size_t orn;
    OptypeStatus ost;
    DiagRecord **erecs;     /* const records (AIC-E0401) */
    size_t ern;
    EvalFailureSite *efails;
    size_t efailn;
    ConstEvalStatus esc;
} Pipeline;

static void pipeline_run_mem(Pipeline *p, const char *src_text)
{
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;

    memset(p, 0, sizeof(*p));
    ld = load_source_from_bytes("input.ai", (const uint8_t *)src_text,
                                strlen(src_text), &p->src, &p->recs, &p->rn);
    CHECK(ld == LOAD_OK);
    if (ld != LOAD_OK) return;
    lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    CHECK(lx == LEX_OK);
    if (lx != LEX_OK) return;
    ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
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

/* ---------------------------------------------------------------------------
 * AST helpers
 * ------------------------------------------------------------------------- */

static const AstNode *find_global_decl(const AstNode *program, const char *name)
{
    size_t i;
    for (i = 0; i < program->u.program.ndecls; i++) {
        const AstNode *d = program->u.program.decls[i];
        const char *n = NULL;
        switch (d->kind) {
        case AST_GLOBAL_CONST_DECL:
        case AST_GLOBAL_VAR_DECL:
            n = d->u.global_decl.name;
            break;
        case AST_ENUM_DECL:
            n = d->u.enum_decl.name;
            break;
        case AST_FN_DECL:
            n = d->u.fn_decl.name;
            break;
        default:
            continue;
        }
        if (n && strcmp(n, name) == 0) return d;
    }
    return NULL;
}

static const AstNode *global_init(const AstNode *decl)
{
    return decl->u.global_decl.init;
}

static const AstNode *find_local_const(const AstNode *fn_decl,
                                       const char *name)
{
    const AstNode *body;
    size_t i;
    if (!fn_decl || fn_decl->kind != AST_FN_DECL || !fn_decl->u.fn_decl.body) {
        return NULL;
    }
    body = fn_decl->u.fn_decl.body;
    for (i = 0; i < body->u.list.count; i++) {
        const AstNode *it = body->u.list.items[i];
        if (it->kind == AST_CONST_DECL && it->u.local_decl.name &&
            strcmp(it->u.local_decl.name, name) == 0) {
            return it;
        }
    }
    return NULL;
}

/* Evaluate an expression in the pipeline's first module. */
static EvalStatus eval_in_pipeline(Pipeline *p, const AstNode *expr,
                                   EvalValue *out, EvalFailure *fail)
{
    EvalCtx ctx;
    EvalStatus st;
    eval_ctx_init(&ctx, p->result, p->build, p->result->modules[0]);
    st = const_eval_expr(&ctx, expr, out, fail);
    eval_ctx_cleanup(&ctx);
    return st;
}

/* ---------------------------------------------------------------------------
 * Value assertions
 * ------------------------------------------------------------------------- */

static void check_type_is(const EvalValue *v, const char *tname)
{
    char *d = v->type ? type_describe(v->type) : NULL;
    CHECK(d != NULL && strcmp(d, tname) == 0);
    free(d);
}

static void check_int_val(const EvalValue *v, const char *tname,
                          int64_t want, bool big)
{
    CHECK(v->kind == EVAL_VAL_INT);
    if (v->kind != EVAL_VAL_INT) return;
    CHECK(v->u.i.v == want);
    CHECK(v->u.i.big == big);
    check_type_is(v, tname);
}

static void check_bool_val(const EvalValue *v, bool want)
{
    CHECK(v->kind == EVAL_VAL_BOOL);
    if (v->kind != EVAL_VAL_BOOL) return;
    CHECK(v->u.b == want);
    check_type_is(v, "bool");
}

static void check_str_val(const EvalValue *v, const char *bytes, size_t len)
{
    CHECK(v->kind == EVAL_VAL_STR);
    if (v->kind != EVAL_VAL_STR) return;
    CHECK(v->u.str.len == len);
    CHECK(len == 0 || memcmp(v->u.str.bytes, bytes, len) == 0);
    check_type_is(v, "str");
}

static void check_addr_val(const EvalValue *v, const char *tname,
                           const char *sym_name, int64_t byte_offset)
{
    CHECK(v->kind == EVAL_VAL_ADDR);
    if (v->kind != EVAL_VAL_ADDR) return;
    CHECK((sym_name == NULL) == (v->u.addr.sym == NULL));
    if (sym_name && v->u.addr.sym) {
        CHECK(strcmp(v->u.addr.sym->name, sym_name) == 0);
    }
    CHECK(v->u.addr.byte_offset == byte_offset);
    check_type_is(v, tname);
}

static void check_slice_val(const EvalValue *v, const char *tname,
                            const char *sym_name, int64_t lo, int64_t hi)
{
    CHECK(v->kind == EVAL_VAL_SLICE);
    if (v->kind != EVAL_VAL_SLICE) return;
    CHECK(v->u.slice.sym != NULL &&
          strcmp(v->u.slice.sym->name, sym_name) == 0);
    CHECK(v->u.slice.lo == lo);
    CHECK(v->u.slice.hi == hi);
    check_type_is(v, tname);
}

/* ---------------------------------------------------------------------------
 * Record shape assertions (phase "semantic", severity error, recovery
 * authoritative)
 * ------------------------------------------------------------------------- */

static bool rec_is_e0401(const DiagRecord *r, const AstNode *expr)
{
    if (strcmp(r->code, "AIC-E0401") != 0) return false;
    if (strcmp(r->message, "expression is not a constant expression") != 0) {
        return false;
    }
    if (strcmp(r->severity, "error") != 0) return false;
    if (strcmp(r->phase, "semantic") != 0) return false;
    if (r->recovery == NULL || strcmp(r->recovery, "authoritative") != 0) {
        return false;
    }
    if (r->primary_span == NULL) return false;
    if (expr) {
        if (r->primary_span->start.offset != expr->span->start.offset ||
            r->primary_span->end.offset != expr->span->end.offset) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_int_literals_and_const_names(void)
{
    static const char src[] =
        "module main;\n"
        "const A: i32 = 5;\n"
        "const B: i64 = A + 1;\n"
        "const C: u64 = 0xFFFF_FFFF_FFFF_FFFFu64;\n"
        "const D: i8 = -128i8;\n"
        "const E: bool = true;\n"
        "const F: str = \"hi\";\n"
        "const G: i32 = -(A);\n"
        "const H: i64 = B;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.cst == CONVERT_OK || p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_OK || p.ost == OPTYPE_UNKNOWN);
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.ern == 0);
    CHECK(p.efailn == 0);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "A")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i32", 5, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "B")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    /* The raw initializer expression A + 1 has natural type i32; the
     * declared-type wrap (i64) applies when the const NAME is
     * referenced (see H below). */
    check_int_val(&v, "i32", 6, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "C")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "u64", -1, true);   /* 2^64-1 */
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "D")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i8", -128, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "E")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_bool_val(&v, true);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "F")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_str_val(&v, "hi", 2);
    eval_value_free(&v);

    /* -(A): unary minus over a const reference */
    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "G")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i32", -5, false);
    eval_value_free(&v);

    /* H: i64 = B; referencing the const name applies the declared-type
     * wrap (the value of B is i64, per spec sec. 8.1) */
    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "H")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i64", 6, false);
    eval_value_free(&v);

    pipeline_free(&p);
}

static void test_enum_members(void)
{
    static const char src[] =
        "module main;\n"
        "enum Color: u8 { Red, Green = 5, Blue }\n"
        "const C1: Color = Color.Red;\n"
        "const C2: Color = Color.Green;\n"
        "const C3: Color = Color.Blue;\n"
        "const U: u8 = cast<u8>(Color.Blue);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.ern == 0);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "C1")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "Color", 0, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "C2")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "Color", 5, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "C3")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "Color", 6, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "U")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "u8", 6, false);
    eval_value_free(&v);

    pipeline_free(&p);
}

static void test_sizof_alignof(void)
{
    static const char src[] =
        "module main;\n"
        "struct S { a: i8; b: i32; }\n"
        "enum E: u16 { X, Y }\n"
        "var g: i32 = 7;\n"
        "var s: S = S { a: 1i8, b: 2 };\n"
        "const N1: usize = sizeof(i8);\n"
        "const N2: usize = sizeof(S);\n"
        "const N3: usize = sizeof(E);\n"
        "const N4: usize = sizeof(str);\n"
        "const N5: usize = sizeof(i32[4]);\n"
        "const N6: usize = sizeof(g);\n"
        "const N7: usize = alignof(S);\n"
        "const N8: usize = sizeof(s.b);\n"
        "const N9: usize = alignof(i32[4]);\n"
        "const N10: usize = sizeof(i32[2][3]);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    struct { const char *name; int64_t want; } cases[] = {
        { "N1", 1 }, { "N2", 8 }, { "N3", 2 }, { "N4", 16 },
        { "N5", 16 }, { "N6", 4 }, { "N7", 4 }, { "N8", 4 },
        { "N9", 4 }, { "N10", 24 }
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.ern == 0);
    CHECK(p.efailn == 0);

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st = eval_in_pipeline(&p,
                              global_init(find_global_decl(p.program,
                                                           cases[i].name)),
                              &v, &fail);
        CHECK(st == EVAL_OK);
        check_int_val(&v, "usize", cases[i].want, false);
        eval_value_free(&v);
    }

    pipeline_free(&p);
}

static void test_static_addresses(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = 7;\n"
        "var arr: i32[4] = [1, 2, 3, 4];\n"
        "const P: i32* = &g;\n"
        "const Q: i32* = &arr[0];\n"
        "const R: i32* = &arr[2];\n"
        "const S: i32[] = arr[1..3];\n"
        "const T: i32[] = arr[..];\n"
        "const U: i32* = &arr[0] + 2;\n"
        "const V: isize = &arr[2] - &arr[0];\n"
        "const W: i32[4]* = &arr;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.ern == 0);
    CHECK(p.efailn == 0);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "P")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32*", "g", 0);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "Q")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32*", "arr", 0);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "R")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32*", "arr", 8);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "S")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_slice_val(&v, "i32[]", "arr", 1, 3);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "T")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_slice_val(&v, "i32[]", "arr", 0, 4);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "U")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32*", "arr", 8);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "V")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "isize", 2, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "W")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32[4]*", "arr", 0);
    eval_value_free(&v);

    pipeline_free(&p);
}

static void test_binary_arithmetic_and_comparisons(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = 5;\n"
        "const A: bool = 1 < 2;\n"
        "const B: bool = 200u8 == 200;\n"
        "const C: bool = \"abc\" < \"abd\";\n"
        "const D: bool = 3 == 3;\n"
        "const E: i32 = 7 / 2;\n"
        "const F: i32 = 7 % 3;\n"
        "const G: i32 = 1 << 4;\n"
        "const H: i32 = -8 >> 2;\n"
        "const I: bool = true && false;\n"
        "const J: bool = true || false;\n"
        "const K: bool = !true;\n"
        "const L: i32 = ~5;\n"
        "const M: i32 = 5 + 3 * 2;\n"
        "const N: i32 = 0x0F & 0x03;\n"
        "const O: i32 = 0x0F | 0x30;\n"
        "const P: i32 = 0x0F ^ 0x33;\n"
        "const Q: bool = &g == &g;\n"
        "const R: bool = &arr[0] != &arr[1];\n"
        "var arr: i32[2] = [1, 2];\n"
        "const S: bool = null == &g;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    struct { const char *name; bool want; } bools[] = {
        { "A", true }, { "B", true }, { "C", true }, { "D", true },
        { "I", false }, { "J", true }, { "K", false }, { "Q", true },
        { "R", true }, { "S", false }
    };
    struct { const char *name; int64_t want; } ints[] = {
        { "E", 3 }, { "F", 1 }, { "G", 16 }, { "H", -2 }, { "L", -6 },
        { "M", 11 }, { "N", 3 }, { "O", 63 }, { "P", 60 }
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.ern == 0);
    CHECK(p.efailn == 0);

    for (i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
        st = eval_in_pipeline(&p,
                              global_init(find_global_decl(p.program,
                                                           bools[i].name)),
                              &v, &fail);
        CHECK(st == EVAL_OK);
        check_bool_val(&v, bools[i].want);
        eval_value_free(&v);
    }
    for (i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        st = eval_in_pipeline(&p,
                              global_init(find_global_decl(p.program,
                                                           ints[i].name)),
                              &v, &fail);
        CHECK(st == EVAL_OK);
        check_int_val(&v, "i32", ints[i].want, false);
        eval_value_free(&v);
    }

    pipeline_free(&p);
}

static void test_cast_wrap(void)
{
    static const char src[] =
        "module main;\n"
        "enum Color: u8 { Red, Green = 5, Blue }\n"
        "var g: i32 = 5;\n"
        "const A: u8 = cast<u8>(200);\n"
        "const B: i8 = cast<i8>(-5);\n"
        "const C: i32 = cast<i32>(true);\n"
        "const D: bool = cast<bool>(1);\n"
        "const E: bool = cast<bool>(0);\n"
        "const F: u64 = wrap<u64>(-1i64);\n"
        "const G: u8 = wrap<u8>(300);\n"
        "const H: i8 = wrap<i8>(255u16);\n"
        "const I: Color = cast<Color>(5);\n"
        "const J: u8 = cast<u8>(Color.Blue);\n"
        "const K: i32* = cast<i32*>(12345);\n"
        "const L: i32* = cast<i32*>(null);\n"
        "const M: bool = null == &g;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.ern == 0);
    CHECK(p.efailn == 0);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "A")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "u8", 200, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "B")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i8", -5, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "C")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i32", 1, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "D")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_bool_val(&v, true);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "E")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_bool_val(&v, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "F")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "u64", -1, true);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "G")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "u8", 44, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "H")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "i8", -1, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "I")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "Color", 5, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "J")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_int_val(&v, "u8", 6, false);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "K")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32*", NULL, 12345);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "L")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_addr_val(&v, "i32*", NULL, 0);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "M")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_bool_val(&v, false);
    eval_value_free(&v);

    pipeline_free(&p);
}

static void test_composites(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "const P: Point = Point { x: 1, y: 2 };\n"
        "const A: i32[3] = [1, 2, 3];\n"
        "const R: i32[3] = [7; 3];\n"
        "const X: i32 = A[0];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_DIAG_ERROR);   /* A[0] is not a const form */
    CHECK(p.ern == 1);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "P")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    CHECK(v.kind == EVAL_VAL_STRUCT);
    if (v.kind == EVAL_VAL_STRUCT) {
        CHECK(v.u.st.nfields == 2);
        if (v.u.st.nfields == 2) {
            check_int_val(&v.u.st.fields[0], "i32", 1, false);
            check_int_val(&v.u.st.fields[1], "i32", 2, false);
        }
        check_type_is(&v, "Point");
    }
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "A")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    CHECK(v.kind == EVAL_VAL_ARRAY);
    if (v.kind == EVAL_VAL_ARRAY) {
        CHECK(v.u.array.nelems == 3);
        if (v.u.array.nelems == 3) {
            check_int_val(&v.u.array.elems[0], "i32", 1, false);
            check_int_val(&v.u.array.elems[1], "i32", 2, false);
            check_int_val(&v.u.array.elems[2], "i32", 3, false);
        }
    }
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "R")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    CHECK(v.kind == EVAL_VAL_ARRAY);
    if (v.kind == EVAL_VAL_ARRAY) {
        CHECK(v.u.array.nelems == 3);
        if (v.u.array.nelems == 3) {
            check_int_val(&v.u.array.elems[0], "i32", 7, false);
            check_int_val(&v.u.array.elems[1], "i32", 7, false);
            check_int_val(&v.u.array.elems[2], "i32", 7, false);
        }
    }
    eval_value_free(&v);

    /* A[0]: indexing an array VALUE is not a const form (sec. 10.5) */
    CHECK(rec_is_e0401(p.erecs[0], global_init(find_global_decl(p.program, "X"))));

    pipeline_free(&p);
}

static void test_not_const_rejections(void)
{
    static const char src[] =
        "module main;\n"
        "fn helper() -> i32 { return 1; }\n"
        "var g: i32 = 5;\n"
        "const A: i32 = helper();\n"
        "const B: i32 = g;\n"
        "const C: i32 = true ? 1 : 2;\n"
        "const D: usize = len(\"hi\");\n"
        "const E: i32 = \"hi\"[0];\n"
        "const F: i32 = 1 + helper();\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *names[] = { "A", "B", "C", "D", "E", "F" };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.cst == CONVERT_OK || p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_OK || p.ost == OPTYPE_UNKNOWN);
    CHECK(p.esc == CONST_EVAL_DIAG_ERROR);
    CHECK(p.ern == 6);
    CHECK(p.efailn == 0);

    /* records sorted by span offset; each spans its initializer expr */
    for (i = 0; i < p.ern && i < 6; i++) {
        const AstNode *decl = find_global_decl(p.program, names[i]);
        CHECK(rec_is_e0401(p.erecs[i], global_init(decl)));
    }

    pipeline_free(&p);
}

static void test_failure_routing(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i32 = 5 / 0;\n"
        "const b: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "const c: i8 = cast<i8>(300);\n"
        "const d: i32 = 1 << 33;\n"
        "var e: i32[2] = [1, 2];\n"
        "const f: i32* = &e[5];\n"
        "const h: u8 = 0u8 - 1u8;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *names[] = { "a", "b", "c", "d", "f", "h" };
    EvalFailure kinds[] = {
        EVAL_FAIL_DIV_ZERO, EVAL_FAIL_OVERFLOW, EVAL_FAIL_CAST_RANGE,
        EVAL_FAIL_SHIFT_RANGE, EVAL_FAIL_INDEX_RANGE, EVAL_FAIL_OVERFLOW
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ern == 0);             /* 12a never emits failure records */
    CHECK(p.efailn == 6);
    if (p.efailn != 6) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 6; i++) {
        const AstNode *decl = find_global_decl(p.program, names[i]);
        CHECK(p.efails[i].node == global_init(decl));
        CHECK(p.efails[i].kind == kinds[i]);
    }

    pipeline_free(&p);
}

static void test_const_cycles(void)
{
    static const char src[] =
        "module main;\n"
        "const A: i32 = B;\n"
        "const B: i32 = A;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.esc == CONST_EVAL_DIAG_ERROR);
    CHECK(p.ern == 2);
    CHECK(p.efailn == 0);
    if (p.ern >= 2) {
        CHECK(rec_is_e0401(p.erecs[0],
                           global_init(find_global_decl(p.program, "A"))));
        CHECK(rec_is_e0401(p.erecs[1],
                           global_init(find_global_decl(p.program, "B"))));
    }

    pipeline_free(&p);
}

static void test_local_const_api(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  const K: i32 = 40 + 2;\n"
        "  return K;\n"
        "}\n"
        "fn g(x: i32) -> i32 {\n"
        "  const L: i32 = x;\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_OK);   /* the walk covers globals only */
    CHECK(p.ern == 0);

    {
        const AstNode *k = find_local_const(find_global_decl(p.program, "f"),
                                            "K");
        CHECK(k != NULL);
        if (k) {
            st = eval_in_pipeline(&p, k->u.local_decl.init, &v, &fail);
            CHECK(st == EVAL_OK);
            check_int_val(&v, "i32", 42, false);
            eval_value_free(&v);
        }
    }
    {
        const AstNode *l = find_local_const(find_global_decl(p.program, "g"),
                                            "L");
        CHECK(l != NULL);
        if (l) {
            st = eval_in_pipeline(&p, l->u.local_decl.init, &v, &fail);
            CHECK(st == EVAL_NOT_CONST);   /* a parameter is not a const form */
        }
    }

    pipeline_free(&p);
}

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "fn helper() -> i32 { return 1; }\n"
        "var g: i32 = 5;\n"
        "const A: i32 = helper();\n"
        "const B: i32 = g;\n"
        "const C: i32 = 5 / 0;\n"
        "const D: usize = sizeof(i32[3]);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    EvalValue v1, v2;
    EvalFailure f1 = EVAL_FAIL_NONE, f2 = EVAL_FAIL_NONE;
    EvalStatus s1, s2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.esc == p2.esc);
    CHECK(p1.esc == CONST_EVAL_DIAG_ERROR);   /* A, B non-const; C div-zero */
    CHECK(p1.ern == p2.ern);
    CHECK(p1.ern == 2);
    CHECK(p1.efailn == p2.efailn);
    CHECK(p1.efailn == 1);
    if (p1.ern == p2.ern && p1.ern == 2) {
        for (i = 0; i < p1.ern; i++) {
            CHECK(strcmp(p1.erecs[i]->code, p2.erecs[i]->code) == 0);
            CHECK(p1.erecs[i]->primary_span->start.offset ==
                  p2.erecs[i]->primary_span->start.offset);
            CHECK(p1.erecs[i]->primary_span->end.offset ==
                  p2.erecs[i]->primary_span->end.offset);
        }
    }
    if (p1.efailn == p2.efailn && p1.efailn == 1) {
        CHECK(p1.efails[0].kind == p2.efails[0].kind);
        /* the two pipelines parse separately, so node pointers differ;
         * determinism is asserted on the source spans instead */
        CHECK(p1.efails[0].node != NULL && p2.efails[0].node != NULL);
        if (p1.efails[0].node && p2.efails[0].node) {
            CHECK(p1.efails[0].node->span->start.offset ==
                  p2.efails[0].node->span->start.offset);
            CHECK(p1.efails[0].node->span->end.offset ==
                  p2.efails[0].node->span->end.offset);
        }
    }

    /* value determinism: D evaluates identically in both pipelines */
    s1 = eval_in_pipeline(&p1, global_init(find_global_decl(p1.program, "D")),
                          &v1, &f1);
    s2 = eval_in_pipeline(&p2, global_init(find_global_decl(p2.program, "D")),
                          &v2, &f2);
    CHECK(s1 == EVAL_OK && s2 == EVAL_OK);
    if (s1 == EVAL_OK && s2 == EVAL_OK) {
        CHECK(v1.u.i.v == v2.u.i.v && v1.u.i.big == v2.u.i.big);
        check_int_val(&v1, "usize", 12, false);
        eval_value_free(&v1);
        eval_value_free(&v2);
    }

    pipeline_free(&p1);
    pipeline_free(&p2);
}

static void test_str_slices(void)
{
    static const char src[] =
        "module main;\n"
        "const A: str = \"hello\"[1..3];\n"
        "const B: str = \"hello\"[..];\n"
        "const C: str = \"caf\\xC3\\xA9\"[3..5];\n"
        "const D: str = \"caf\\xC3\\xA9\"[2..4];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    /* C slices on the code point boundary (0xC3 0xA9 is one code point);
     * D slices INSIDE the two-byte sequence -> EVAL_FAIL_STR_BOUNDARY */
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ern == 0);
    CHECK(p.efailn == 1);
    if (p.efailn == 1) {
        CHECK(p.efails[0].kind == EVAL_FAIL_STR_BOUNDARY);
        CHECK(p.efails[0].node == global_init(find_global_decl(p.program, "D")));
    }

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "A")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_str_val(&v, "el", 2);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "B")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_str_val(&v, "hello", 5);
    eval_value_free(&v);

    st = eval_in_pipeline(&p, global_init(find_global_decl(p.program, "C")),
                          &v, &fail);
    CHECK(st == EVAL_OK);
    check_str_val(&v, "\xC3\xA9", 2);
    eval_value_free(&v);

    pipeline_free(&p);
}

int main(void)
{
    test_int_literals_and_const_names();
    fprintf(stderr, "after test_int_literals\n");
    test_enum_members();
    fprintf(stderr, "after test_enum_members\n");
    test_sizof_alignof();
    fprintf(stderr, "after test_sizof_alignof\n");
    test_static_addresses();
    fprintf(stderr, "after test_static_addresses\n");
    test_binary_arithmetic_and_comparisons();
    fprintf(stderr, "after test_binary_arithmetic\n");
    test_cast_wrap();
    fprintf(stderr, "after test_cast_wrap\n");
    test_composites();
    fprintf(stderr, "after test_composites\n");
    test_not_const_rejections();
    fprintf(stderr, "after test_not_const_rejections\n");
    test_failure_routing();
    fprintf(stderr, "after test_failure_routing\n");
    test_const_cycles();
    fprintf(stderr, "after test_const_cycles\n");
    test_local_const_api();
    fprintf(stderr, "after test_local_const_api\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_str_slices();
    fprintf(stderr, "after test_str_slices\n");

    if (g_failures) {
        fprintf(stderr, "eval_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("eval_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
