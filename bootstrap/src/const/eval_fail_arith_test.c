/* bootstrap/src/const/eval_fail_arith_test.c
 *
 * WP-M0-12b1 unit and integration tests: checked-arithmetic failure
 * evaluation (spec sec. 11.3) - typed failure kinds for overflow,
 * division/remainder by zero, and shift count out of range
 * (AIC-E0405/E0406/E0407), the never-trap guarantee on the extreme
 * values, deterministic failure ordering, and the classification API
 * consumed by WP-M0-12b2 for record emission.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-const-b1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/const/eval_fail_arith_test.c \
 *     bootstrap/src/const/eval_fail_arith.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-const-b1/eval_fail_arith_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-const-b1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "eval_fail_arith.h"

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
 * types_optype_check -> const_eval_check -> arith_fail_check.
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
    ArithFailValue *afails; /* typed arithmetic failures (12b1) */
    size_t afailn;
    ArithFailStatus ast2;
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
    /* LAYOUT_UNEVALUABLE is not a diagnostic: 11b's bounded subset
     * delegates the member-value/array-extent site to the const
     * evaluator (12), so the pipeline continues (mirrors the driver
     * contract in layout.h). */
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR &&
        p->lst != LAYOUT_UNEVALUABLE) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
    p->ast2 = arith_fail_check(p->result, p->build, &p->afails, &p->afailn);
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
    free(p->afails);
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

/* Depth-first structural search for the first binary node with `op`. */
static const AstNode *find_binop(const AstNode *e, AstBinaryOp op)
{
    const AstNode *sub;
    size_t i;
    if (!e) return NULL;
    if (e->kind == AST_EXPR_BINARY && e->u.binary.op == op) return e;
    switch (e->kind) {
    case AST_EXPR_BINARY:
        if ((sub = find_binop(e->u.binary.lhs, op))) return sub;
        return find_binop(e->u.binary.rhs, op);
    case AST_EXPR_UNARY:
        return find_binop(e->u.unary.operand, op);
    case AST_EXPR_PAREN:
        return find_binop(e->u.paren.expr, op);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        return find_binop(e->u.cast_wrap.expr, op);
    case AST_EXPR_INDEX:
        if ((sub = find_binop(e->u.index_slice.base, op))) return sub;
        return find_binop(e->u.index_slice.index, op);
    case AST_EXPR_SLICE:
        if ((sub = find_binop(e->u.index_slice.base, op))) return sub;
        if ((sub = find_binop(e->u.index_slice.lo, op))) return sub;
        return find_binop(e->u.index_slice.hi, op);
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < e->u.array_literal.nelems; i++) {
            if ((sub = find_binop(e->u.array_literal.elems[i], op))) return sub;
        }
        return NULL;
    case AST_EXPR_STRUCT_INIT:
        for (i = 0; i < e->u.struct_init.nfields; i++) {
            const AstNode *fi = e->u.struct_init.fields[i];
            if (fi && (sub = find_binop(fi->u.named.value, op))) return sub;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* First AST_EXPR_IDENT node at or below `e` (const-reference leaf). */
static const AstNode *find_ident(const AstNode *e)
{
    const AstNode *sub;
    if (!e) return NULL;
    if (e->kind == AST_EXPR_IDENT) return e;
    switch (e->kind) {
    case AST_EXPR_BINARY:
        if ((sub = find_ident(e->u.binary.lhs))) return sub;
        return find_ident(e->u.binary.rhs);
    case AST_EXPR_UNARY:
        return find_ident(e->u.unary.operand);
    case AST_EXPR_PAREN:
        return find_ident(e->u.paren.expr);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        return find_ident(e->u.cast_wrap.expr);
    default:
        return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Typed-failure assertions
 * ------------------------------------------------------------------------- */

static void check_fail_kind(const ArithFailValue *f, EvalFailure kind,
                            const char *code)
{
    CHECK(f->kind == kind);
    CHECK(f->code != NULL && strcmp(f->code, code) == 0);
}

static void check_fail_site(const ArithFailValue *f, const AstNode *site)
{
    CHECK(f->site == site);
}

/* op_node must be at/below site and point at the expected binary op. */
static void check_fail_op_binary(const ArithFailValue *f, const AstNode *site,
                                 AstBinaryOp op)
{
    CHECK(f->op_node != NULL);
    CHECK(f->op_node == find_binop(site, op));
    CHECK(!f->is_unary);
    CHECK(f->op == op);
}

/* op_node must be the site itself when the whole initializer is the
 * failing operation. */
static void check_fail_op_at_site(const ArithFailValue *f,
                                  const AstNode *site, AstBinaryOp op)
{
    CHECK(f->op_node == site);
    CHECK(!f->is_unary);
    CHECK(f->op == op);
}

static void check_fail_unary(const ArithFailValue *f, const AstNode *site)
{
    CHECK(f->is_unary);
    CHECK(f->op_node == site);
}

static void check_fail_int(const ArithFailValue *f, int64_t a, int64_t b,
                           int width, bool is_signed)
{
    CHECK(f->a.v == a);
    CHECK(!f->a.big);
    CHECK(f->b.v == b);
    CHECK(!f->b.big);
    CHECK(f->width == width);
    CHECK(f->is_signed == is_signed);
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* Overflow: signed add/sub/mul, unsigned wrap, unary negation of the
 * signed minimum, and min / -1, min % -1 (spec sec. 11.3 -> AIC-E0405). */
static void test_overflow_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "var b: i8 = cast<i8>(-128) - cast<i8>(1);\n"
        "var c: i8 = cast<i8>(100) * cast<i8>(2);\n"
        "var d: u8 = 0u8 - 1u8;\n"
        "var e: u64 = 0u64 - 1u64;\n"
        "var f: u8 = 255u8 + 1u8;\n"
        "var g: i8 = -(cast<i8>(-128));\n"
        "var h: i64 = -9223372036854775808i64 / -1;\n"
        "var i: i32 = -2147483648i32 % -1;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *names[] = { "a", "b", "c", "d", "e", "f", "g", "h", "i" };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ast2 == ARITH_FAIL_FAILURE);
    CHECK(p.afailn == 9);
    if (p.afailn != 9) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 9; i++) {
        const AstNode *decl = find_global_decl(p.program, names[i]);
        const AstNode *site = global_init(decl);
        CHECK(p.afails[i].kind == EVAL_FAIL_OVERFLOW);
        CHECK(p.afails[i].code != NULL &&
              strcmp(p.afails[i].code, "AIC-E0405") == 0);
        check_fail_site(&p.afails[i], site);
        CHECK(p.afails[i].op_node != NULL);
    }
    check_fail_op_at_site(&p.afails[0], global_init(find_global_decl(p.program, "a")),
                          AST_BIN_ADD);
    check_fail_int(&p.afails[0], 127, 1, 8, true);
    check_fail_op_at_site(&p.afails[1], global_init(find_global_decl(p.program, "b")),
                          AST_BIN_SUB);
    check_fail_int(&p.afails[1], -128, 1, 8, true);
    check_fail_op_at_site(&p.afails[2], global_init(find_global_decl(p.program, "c")),
                          AST_BIN_MUL);
    check_fail_int(&p.afails[2], 100, 2, 8, true);
    check_fail_op_at_site(&p.afails[3], global_init(find_global_decl(p.program, "d")),
                          AST_BIN_SUB);
    check_fail_int(&p.afails[3], 0, 1, 8, false);
    check_fail_op_at_site(&p.afails[4], global_init(find_global_decl(p.program, "e")),
                          AST_BIN_SUB);
    check_fail_int(&p.afails[4], 0, 1, 64, false);
    check_fail_op_at_site(&p.afails[5], global_init(find_global_decl(p.program, "f")),
                          AST_BIN_ADD);
    check_fail_int(&p.afails[5], 255, 1, 8, false);
    check_fail_unary(&p.afails[6], global_init(find_global_decl(p.program, "g")));
    check_fail_int(&p.afails[6], -128, 0, 8, true);
    check_fail_op_at_site(&p.afails[7], global_init(find_global_decl(p.program, "h")),
                          AST_BIN_DIV);
    check_fail_int(&p.afails[7], INT64_MIN, -1, 64, true);
    check_fail_op_at_site(&p.afails[8], global_init(find_global_decl(p.program, "i")),
                          AST_BIN_MOD);
    check_fail_int(&p.afails[8], INT32_MIN, -1, 32, true);

    pipeline_free(&p);
}

/* Division/remainder by zero, signed and unsigned (AIC-E0406). */
static void test_div_zero_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i32 = 5 / 0;\n"
        "var b: i32 = 5 % 0;\n"
        "var c: u64 = 5u64 / 0u64;\n"
        "var d: i64 = -7i64 % 0i64;\n"
        "var e: u32 = 0u32 / 0u32;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *names[] = { "a", "b", "c", "d", "e" };
    AstBinaryOp ops[] = { AST_BIN_DIV, AST_BIN_MOD, AST_BIN_DIV,
                          AST_BIN_MOD, AST_BIN_DIV };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ast2 == ARITH_FAIL_FAILURE);
    CHECK(p.afailn == 5);
    if (p.afailn != 5) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 5; i++) {
        const AstNode *decl = find_global_decl(p.program, names[i]);
        check_fail_kind(&p.afails[i], EVAL_FAIL_DIV_ZERO, "AIC-E0406");
        check_fail_site(&p.afails[i], global_init(decl));
        check_fail_op_at_site(&p.afails[i], global_init(decl), ops[i]);
    }
    check_fail_int(&p.afails[0], 5, 0, 32, true);
    check_fail_int(&p.afails[2], 5, 0, 64, false);

    pipeline_free(&p);
}

/* Shift count out of range: negative, >= width, and u64 width
 * boundary (AIC-E0407; op_node is the shift). */
static void test_shift_range_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i32 = 1 << 33;\n"
        "var b: i32 = 1 << -1;\n"
        "var c: i8 = cast<i8>(1) << cast<i8>(8);\n"
        "var d: u64 = 1u64 << 64u64;\n"
        "var e: u64 = 1u64 >> 64u64;\n"
        "var f: i64 = 1i64 << 64i64;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *names[] = { "a", "b", "c", "d", "e", "f" };
    AstBinaryOp ops[] = { AST_BIN_SHL, AST_BIN_SHL, AST_BIN_SHL,
                          AST_BIN_SHL, AST_BIN_SHR, AST_BIN_SHL };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ast2 == ARITH_FAIL_FAILURE);
    CHECK(p.afailn == 6);
    if (p.afailn != 6) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 6; i++) {
        const AstNode *decl = find_global_decl(p.program, names[i]);
        const AstNode *site = global_init(decl);
        check_fail_kind(&p.afails[i], EVAL_FAIL_SHIFT_RANGE, "AIC-E0407");
        check_fail_site(&p.afails[i], site);
        check_fail_op_binary(&p.afails[i], site, ops[i]);
    }
    check_fail_int(&p.afails[0], 1, 33, 32, true);
    check_fail_int(&p.afails[1], 1, -1, 32, true);
    check_fail_int(&p.afails[2], 1, 8, 8, true);
    check_fail_int(&p.afails[3], 1, 64, 64, false);
    check_fail_int(&p.afails[4], 1, 64, 64, false);

    pipeline_free(&p);
}

/* Failure location: the failing operator is found inside a larger
 * expression; a const reference stops at the reference node; a
 * non-arithmetic kind (cast range) is not this package's. */
static void test_failure_location(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = (cast<i8>(127) + cast<i8>(1)) * cast<i8>(2);\n"
        "var b: i32 = (1 + 2) * (3 << 33);\n"
        "var c: i32 = 5 / 0 + 1;\n"
        "var d: i32 = 1 + (5 % 0);\n"
        "const A: i32 = 6 / 0;\n"
        "var e: i32 = A + 1;\n"
        "var f: i32 = cast<i32>(300);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    (void)p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ast2 == ARITH_FAIL_FAILURE);
    /* a: overflow (inner +), b: shift (inner <<), c: div-zero (inner /),
     * d: div-zero (inner %), A: div-zero (A's own site), e: div-zero via
     * const ref A. f evaluates fine (cast<i32>(300) is representable);
     * the cast-range NOT-arithmetic case is covered by
     * test_classify_api. */
    CHECK(p.afailn == 6);
    if (p.afailn != 6) {
        pipeline_free(&p);
        return;
    }

    /* a: op_node is the inner '+' (overflow at the add, not the mul). */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "a"));
        check_fail_kind(&p.afails[0], EVAL_FAIL_OVERFLOW, "AIC-E0405");
        check_fail_site(&p.afails[0], site);
        check_fail_op_binary(&p.afails[0], site, AST_BIN_ADD);
        CHECK(p.afails[0].op_node != site);
        check_fail_int(&p.afails[0], 127, 1, 8, true);
    }
    /* b: op_node is the inner shift `3 << 33`. */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "b"));
        check_fail_kind(&p.afails[1], EVAL_FAIL_SHIFT_RANGE, "AIC-E0407");
        check_fail_site(&p.afails[1], site);
        check_fail_op_binary(&p.afails[1], site, AST_BIN_SHL);
        CHECK(p.afails[1].op_node != site);
    }
    /* c, d: the failing operator is the inner division/modulo; the site
     * is the whole initializer. */
    check_fail_kind(&p.afails[2], EVAL_FAIL_DIV_ZERO, "AIC-E0406");
    check_fail_op_binary(&p.afails[2],
                         global_init(find_global_decl(p.program, "c")),
                         AST_BIN_DIV);
    CHECK(p.afails[2].op_node !=
          global_init(find_global_decl(p.program, "c")));
    check_fail_kind(&p.afails[3], EVAL_FAIL_DIV_ZERO, "AIC-E0406");
    check_fail_op_binary(&p.afails[3],
                         global_init(find_global_decl(p.program, "d")),
                         AST_BIN_MOD);
    CHECK(p.afails[3].op_node !=
          global_init(find_global_decl(p.program, "d")));
    /* A: the const's own site is the failing expression. */
    check_fail_kind(&p.afails[4], EVAL_FAIL_DIV_ZERO, "AIC-E0406");
    check_fail_op_at_site(&p.afails[4],
                          global_init(find_global_decl(p.program, "A")),
                          AST_BIN_DIV);
    /* e: the failure reaches this site through the const reference A;
     * op_node is the reference node, not a cross-site operator. */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "e"));
        check_fail_kind(&p.afails[5], EVAL_FAIL_DIV_ZERO, "AIC-E0406");
        check_fail_site(&p.afails[5], site);
        CHECK(p.afails[5].op_node == find_ident(site));
    }

    pipeline_free(&p);
}

/* Never-trap guarantee on the extreme values: the process must survive
 * every constant failure below (a host trap would abort the test). */
static void test_never_traps(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i64 = -9223372036854775808i64 / -1;\n"
        "var b: i64 = -9223372036854775808i64 % -1;\n"
        "var c: i64 = -(-9223372036854775808i64);\n"
        "var d: u64 = 0xFFFF_FFFF_FFFF_FFFFu64 + 1u64;\n"
        "var e: u64 = 0xFFFF_FFFF_FFFF_FFFFu64 * 2u64;\n"
        "var f: u64 = 1u64 << 64u64;\n"
        "var g: u64 = 1u64 << 63u64;\n"
        "var h: u64 = 5u64 / 0u64;\n"
        "var i: i64 = 0i64 / 0i64;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalFailure want[] = {
        EVAL_FAIL_OVERFLOW, EVAL_FAIL_OVERFLOW, EVAL_FAIL_OVERFLOW,
        EVAL_FAIL_OVERFLOW, EVAL_FAIL_OVERFLOW, EVAL_FAIL_SHIFT_RANGE,
        /* g (1u64 << 63) is a valid shift: no failure. */
        EVAL_FAIL_DIV_ZERO, EVAL_FAIL_DIV_ZERO
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ast2 == ARITH_FAIL_FAILURE);
    CHECK(p.afailn == 8);
    if (p.afailn != 8) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 8; i++) {
        CHECK(p.afails[i].kind == want[i]);
    }
    check_fail_int(&p.afails[0], INT64_MIN, -1, 64, true);
    /* d: left operand is 0xFFFFFFFFFFFFFFFFu64 -> EvalInt {v: -1, big: true} */
    CHECK(p.afails[3].a.big);
    CHECK(p.afails[3].a.v == (int64_t)0xFFFFFFFFFFFFFFFFLL);
    CHECK(p.afails[3].b.v == 1);
    CHECK(!p.afails[3].b.big);
    CHECK(p.afails[3].width == 64);
    CHECK(!p.afails[3].is_signed);
    check_fail_int(&p.afails[5], 1, 64, 64, false);

    pipeline_free(&p);
}

/* Determinism: two independent pipelines over the same source produce
 * identical typed failures (kind, code, site span, op_node span). */
static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "var b: i32 = 1 << 33;\n"
        "var c: i32 = 5 / 0;\n"
        "var d: u8 = 0u8 - 1u8;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.ast2 == p2.ast2);
    CHECK(p1.ast2 == ARITH_FAIL_FAILURE);
    CHECK(p1.afailn == p2.afailn);
    CHECK(p1.afailn == 4);
    if (p1.afailn != p2.afailn || p1.afailn != 4) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.afailn; i++) {
        CHECK(p1.afails[i].kind == p2.afails[i].kind);
        CHECK(p1.afails[i].code != NULL && p2.afails[i].code != NULL &&
              strcmp(p1.afails[i].code, p2.afails[i].code) == 0);
        CHECK(p1.afails[i].a.v == p2.afails[i].a.v);
        CHECK(p1.afails[i].a.big == p2.afails[i].a.big);
        CHECK(p1.afails[i].b.v == p2.afails[i].b.v);
        CHECK(p1.afails[i].width == p2.afails[i].width);
        CHECK(p1.afails[i].is_signed == p2.afails[i].is_signed);
        CHECK(p1.afails[i].site != NULL && p2.afails[i].site != NULL);
        if (p1.afails[i].site && p2.afails[i].site) {
            CHECK(p1.afails[i].site->span->start.offset ==
                  p2.afails[i].site->span->start.offset);
            CHECK(p1.afails[i].site->span->end.offset ==
                  p2.afails[i].site->span->end.offset);
        }
        CHECK(p1.afails[i].op_node != NULL && p2.afails[i].op_node != NULL);
        if (p1.afails[i].op_node && p2.afails[i].op_node) {
            CHECK(p1.afails[i].op_node->span->start.offset ==
                  p2.afails[i].op_node->span->start.offset);
            CHECK(p1.afails[i].op_node->span->end.offset ==
                  p2.afails[i].op_node->span->end.offset);
        }
    }

    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* Classification API consumed by 12b2: classify an already-routed
 * EvalFailureSite (from 12a's const_eval_check) into a typed failure;
 * non-arithmetic routed kinds return ARITH_FAIL_NOT_ARITH. */
static void test_classify_api(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "var b: i32 = 5 / 0;\n"
        "var c: i32 = 1 << 33;\n"
        "var d: i8 = cast<i8>(300);\n"
        "var e: i32[2] = [1, 2];\n"
        "var f: i32* = &e[5];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    size_t i;
    int arith_seen = 0;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    /* a,b,c are arithmetic; d cast-range; f index-range. e is fine. */
    CHECK(p.efailn == 5);
    CHECK(p.afailn == 3);
    if (p.efailn != 5) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < p.efailn; i++) {
        EvalCtx ctx;
        ArithFailValue v;
        ArithFailStatus st;
        eval_ctx_init(&ctx, p.result, p.build, p.result->modules[0]);
        st = arith_fail_classify(&ctx, p.efails[i].node, p.efails[i].kind,
                                 &v);
        eval_ctx_cleanup(&ctx);
        if (p.efails[i].kind == EVAL_FAIL_OVERFLOW ||
            p.efails[i].kind == EVAL_FAIL_DIV_ZERO ||
            p.efails[i].kind == EVAL_FAIL_SHIFT_RANGE) {
            CHECK(st == ARITH_FAIL_FAILURE);
            CHECK(v.kind == p.efails[i].kind);
            CHECK(v.site == p.efails[i].node);
            arith_seen++;
        } else {
            CHECK(st == ARITH_FAIL_NOT_ARITH);
        }
    }
    CHECK(arith_seen == 3);

    /* arith_fail_code maps only the three arithmetic kinds. */
    CHECK(arith_fail_code(EVAL_FAIL_OVERFLOW) != NULL &&
          strcmp(arith_fail_code(EVAL_FAIL_OVERFLOW), "AIC-E0405") == 0);
    CHECK(arith_fail_code(EVAL_FAIL_DIV_ZERO) != NULL &&
          strcmp(arith_fail_code(EVAL_FAIL_DIV_ZERO), "AIC-E0406") == 0);
    CHECK(arith_fail_code(EVAL_FAIL_SHIFT_RANGE) != NULL &&
          strcmp(arith_fail_code(EVAL_FAIL_SHIFT_RANGE), "AIC-E0407") == 0);
    CHECK(arith_fail_code(EVAL_FAIL_CAST_RANGE) == NULL);
    CHECK(arith_fail_code(EVAL_FAIL_INDEX_RANGE) == NULL);
    CHECK(arith_fail_code(EVAL_FAIL_STR_BOUNDARY) == NULL);
    CHECK(arith_fail_code(EVAL_FAIL_PTR_DIFF) == NULL);
    CHECK(arith_fail_code(EVAL_FAIL_NONE) == NULL);

    pipeline_free(&p);
}

/* Enum member value expressions are const-context sites: an arithmetic
 * failure in a member value is typed and ordered after the globals. */
static void test_enum_member_sites(void)
{
    static const char src[] =
        "module main;\n"
        "enum E: u32 { A = 1, B = 2, C = 3u32 / 0u32 }\n"
        "var x: i32 = 1 << 33;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const AstNode *ed;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    /* 11b's bounded subset cannot evaluate `3u32 / 0u32`; it delegates
     * the site to the const evaluator (LAYOUT_UNEVALUABLE, not a
     * diagnostic). */
    CHECK(p.lst == LAYOUT_UNEVALUABLE);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.ast2 == ARITH_FAIL_FAILURE);
    /* walk order (declaration order): enum E's member C value first,
     * then the global var x initializer. */
    CHECK(p.afailn == 2);
    if (p.afailn != 2) {
        pipeline_free(&p);
        return;
    }
    check_fail_kind(&p.afails[0], EVAL_FAIL_DIV_ZERO, "AIC-E0406");
    ed = find_global_decl(p.program, "E");
    CHECK(ed != NULL);
    if (ed) {
        CHECK(p.afails[0].site == ed->u.enum_decl.members[2]->u.named.value);
    }
    check_fail_kind(&p.afails[1], EVAL_FAIL_SHIFT_RANGE, "AIC-E0407");
    check_fail_site(&p.afails[1],
                    global_init(find_global_decl(p.program, "x")));

    pipeline_free(&p);
}

int main(void)
{
    test_overflow_kinds();
    fprintf(stderr, "after test_overflow_kinds\n");
    test_div_zero_kinds();
    fprintf(stderr, "after test_div_zero_kinds\n");
    test_shift_range_kinds();
    fprintf(stderr, "after test_shift_range_kinds\n");
    test_failure_location();
    fprintf(stderr, "after test_failure_location\n");
    test_never_traps();
    fprintf(stderr, "after test_never_traps\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_classify_api();
    fprintf(stderr, "after test_classify_api\n");
    test_enum_member_sites();
    fprintf(stderr, "after test_enum_member_sites\n");

    if (g_failures) {
        fprintf(stderr, "eval_fail_arith_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("eval_fail_arith_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
