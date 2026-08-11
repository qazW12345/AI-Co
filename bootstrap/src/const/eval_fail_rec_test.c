/* bootstrap/src/const/eval_fail_rec_test.c
 *
 * WP-M0-12b2 unit and integration tests: const-failure record emission
 * and the remaining failure sites (spec sec. 11.3 / 11.5 / 12.1 /
 * 12.2 / 12.4 / 12.5). Covers:
 *   - classification and records for the four kinds 12b1 excluded
 *     (cast-range AIC-E0408, index/slice bound AIC-E0409, str-slice
 *     code-point boundary AIC-E0410, pointer-difference divisibility
 *     AIC-E0411) plus record emission for all seven const codes
 *     (AIC-E0405 .. E0411);
 *   - the never-trap guarantee on the extreme values (the process must
 *     survive every constant failure below);
 *   - deterministic record ordering (contract sec. 9) and byte-level
 *     determinism across independent pipelines;
 *   - exact-record conformance against the negative-corpus anchors for
 *     const failures (the seven cases with codes AIC-E0405..E0411).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-const-b2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/const/eval_fail_rec_test.c \
 *     bootstrap/src/const/eval_fail_rec.c \
 *     bootstrap/src/const/eval_fail_arith.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-const-b2/eval_fail_rec_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-const-b2)
 *
 * The corpus-anchor test reads tests/negative/cases/<name>/input.ai at
 * runtime, so run the executable from the repository root (the build
 * entry points and this test's build comments assume that).
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "eval_fail_rec.h"

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
 * types_optype_check -> const_eval_check (12a) -> rec_fail_emit (12b2).
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
    DiagRecord **erecs;     /* 12a const records (AIC-E0401) */
    size_t ern;
    EvalFailureSite *efails;/* 12a routed failures (all kinds) */
    size_t efailn;
    ConstEvalStatus esc;
    DiagRecord **rrecs;     /* 12b2 const failure records (E0405..E0411) */
    size_t rrn;
    RecFailStatus rsc;
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
    p->rsc = rec_fail_emit(p->result, p->build, &p->rrecs, &p->rrn);
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
    types_records_free(p->rrecs, p->rrn);
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
    default:
        return NULL;
    }
}

/* First AST_EXPR_CAST node at or below `e`. */
static const AstNode *find_cast(const AstNode *e)
{
    const AstNode *sub;
    size_t i;
    if (!e) return NULL;
    if (e->kind == AST_EXPR_CAST) return e;
    switch (e->kind) {
    case AST_EXPR_BINARY:
        if ((sub = find_cast(e->u.binary.lhs))) return sub;
        return find_cast(e->u.binary.rhs);
    case AST_EXPR_UNARY:
        return find_cast(e->u.unary.operand);
    case AST_EXPR_PAREN:
        return find_cast(e->u.paren.expr);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        return find_cast(e->u.cast_wrap.expr);
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < e->u.array_literal.nelems; i++) {
            if ((sub = find_cast(e->u.array_literal.elems[i]))) return sub;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* First AST_EXPR_SLICE node at or below `e`. */
static const AstNode *find_slice(const AstNode *e)
{
    const AstNode *sub;
    size_t i;
    if (!e) return NULL;
    if (e->kind == AST_EXPR_SLICE) return e;
    switch (e->kind) {
    case AST_EXPR_BINARY:
        if ((sub = find_slice(e->u.binary.lhs))) return sub;
        return find_slice(e->u.binary.rhs);
    case AST_EXPR_UNARY:
        return find_slice(e->u.unary.operand);
    case AST_EXPR_PAREN:
        return find_slice(e->u.paren.expr);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        return find_slice(e->u.cast_wrap.expr);
    case AST_EXPR_INDEX:
        if ((sub = find_slice(e->u.index_slice.base))) return sub;
        return find_slice(e->u.index_slice.index);
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < e->u.array_literal.nelems; i++) {
            if ((sub = find_slice(e->u.array_literal.elems[i]))) return sub;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* The index expression of an &arr[i] form (the first AST_EXPR_INDEX
 * node's index child at or below `e`). */
static const AstNode *find_index_bound(const AstNode *e)
{
    const AstNode *sub;
    if (!e) return NULL;
    if (e->kind == AST_EXPR_INDEX) return e->u.index_slice.index;
    switch (e->kind) {
    case AST_EXPR_BINARY:
        if ((sub = find_index_bound(e->u.binary.lhs))) return sub;
        return find_index_bound(e->u.binary.rhs);
    case AST_EXPR_UNARY:
        return find_index_bound(e->u.unary.operand);
    case AST_EXPR_PAREN:
        return find_index_bound(e->u.paren.expr);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        return find_index_bound(e->u.cast_wrap.expr);
    default:
        return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Record assertions
 * ------------------------------------------------------------------------- */

static void check_record_code(const DiagRecord *r, const char *code)
{
    DiagBuf buf;
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->code != NULL && strcmp(r->code, code) == 0);
    CHECK(r->severity != NULL && strcmp(r->severity, "error") == 0);
    CHECK(r->phase != NULL && strcmp(r->phase, "semantic") == 0);
    CHECK(r->recovery != NULL &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    /* the record must be contract-valid and emittable */
    diag_buf_init(&buf);
    CHECK(diag_emit_record(&buf, r));
    diag_buf_free(&buf);
}

/* Exact record: code, message, and the full primary span. */
static void check_record_exact(const DiagRecord *r, const char *code,
                               const char *message,
                               int64_t sl, int64_t sc, int64_t so,
                               int64_t el, int64_t ec, int64_t eo)
{
    check_record_code(r, code);
    if (!r) return;
    CHECK(r->message != NULL && strcmp(r->message, message) == 0);
    CHECK(r->primary_span != NULL);
    if (r->primary_span) {
        CHECK(r->primary_span->start.line == sl);
        CHECK(r->primary_span->start.col == sc);
        CHECK(r->primary_span->start.offset == so);
        CHECK(r->primary_span->end.line == el);
        CHECK(r->primary_span->end.col == ec);
        CHECK(r->primary_span->end.offset == eo);
    }
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* Kind -> code mapping for all seven const codes; NULL otherwise. */
static void test_rec_fail_code(void)
{
    CHECK(rec_fail_code(EVAL_FAIL_OVERFLOW) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_OVERFLOW), "AIC-E0405") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_DIV_ZERO) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_DIV_ZERO), "AIC-E0406") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_SHIFT_RANGE) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_SHIFT_RANGE), "AIC-E0407") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_CAST_RANGE) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_CAST_RANGE), "AIC-E0408") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_INDEX_RANGE) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_INDEX_RANGE), "AIC-E0409") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_STR_BOUNDARY) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_STR_BOUNDARY), "AIC-E0410") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_PTR_DIFF) != NULL &&
          strcmp(rec_fail_code(EVAL_FAIL_PTR_DIFF), "AIC-E0411") == 0);
    CHECK(rec_fail_code(EVAL_FAIL_NONE) == NULL);
}

/* Cast-range failures (spec sec. 11.5 -> AIC-E0408): the record's
 * primary span is the cast, the message names the value and target. */
static void test_cast_range_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "enum E: i8 { A = 1, B = 2 }\n"
        "var a: i8 = cast<i8>(200);\n"
        "var b: bool = cast<bool>(2);\n"
        "var c: i16 = cast<i16>(cast<i16>(40000));\n"
        "var d: E = cast<E>(5);\n"
        "var e: u8 = cast<u8>(0xFFFFFFFFFFFFFFFFu64);\n"
        "var ok: i8 = cast<i8>(100);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 5);
    if (p.rrn != 5) {
        pipeline_free(&p);
        return;
    }
    /* sorted by offset: a (line 3), b (4), c (5), d (6), e (7) */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "a"));
        CHECK(p.rrecs[0]->primary_span->start.offset ==
              find_cast(site)->span->start.offset);
        CHECK(p.rrecs[0]->primary_span->end.offset ==
              find_cast(site)->span->end.offset);
    }
    check_record_exact(p.rrecs[0], "AIC-E0408",
                       "constant cast out of range: 200 does not fit in i8",
                       3, 13, 53, 3, 26, 66);
    check_record_exact(p.rrecs[1], "AIC-E0408",
                       "constant cast out of range: 2 does not fit in bool",
                       4, 15, 82, 4, 28, 95);
    check_record_exact(p.rrecs[2], "AIC-E0408",
                       "constant cast out of range: 40000 does not fit in i16",
                       5, 24, 120, 5, 40, 136);
    check_record_exact(p.rrecs[3], "AIC-E0408",
                       "constant cast out of range: 5 does not fit in E",
                       6, 12, 150, 6, 22, 160);
    check_record_exact(p.rrecs[4], "AIC-E0408",
                       "constant cast out of range: 18446744073709551615 "
                       "does not fit in u8",
                       7, 13, 174, 7, 44, 205);

    /* e: the big u64 value renders unsigned */
    CHECK(p.rrecs[4]->message != NULL &&
          strstr(p.rrecs[4]->message, "18446744073709551615") != NULL);

    pipeline_free(&p);
}

/* Index/slice bound failures (spec sec. 12.1 / 12.4 -> AIC-E0409): the
 * record's primary span is the bound expression. */
static void test_index_range_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "var arr: i32[3] = [1, 2, 3];\n"
        "var p: i32* = &arr[10];\n"
        "var q: i32* = &arr[-1];\n"
        "var r: i32* = &arr[0xFFFFFFFFFFFFFFFFu64];\n"
        "var s: i32[] = arr[10..];\n"
        "var t: i32[] = arr[..10];\n"
        "var u: i32[] = arr[10..2];\n"
        "var v: i32[] = arr[1..2];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 6);
    if (p.rrn != 6) {
        pipeline_free(&p);
        return;
    }
    /* p: &arr[10] -- bound span is the index expression 10 */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "p"));
        const AstNode *bound = find_index_bound(site);
        CHECK(bound != NULL);
        if (bound) {
            CHECK(p.rrecs[0]->primary_span->start.offset ==
                  bound->span->start.offset);
            CHECK(p.rrecs[0]->primary_span->end.offset ==
                  bound->span->end.offset);
        }
    }
    check_record_exact(p.rrecs[0], "AIC-E0409",
                       "constant index 10 out of range for array of length 3",
                       3, 20, 61, 3, 22, 63);
    check_record_exact(p.rrecs[1], "AIC-E0409",
                       "constant index -1 out of range for array of length 3",
                       4, 20, 85, 4, 22, 87);
    check_record_exact(p.rrecs[2], "AIC-E0409",
                       "constant index 18446744073709551615 out of range "
                       "for array of length 3",
                       5, 20, 109, 5, 41, 130);
    check_record_exact(p.rrecs[3], "AIC-E0409",
                       "constant slice bound 10 out of range for extent 3",
                       6, 20, 152, 6, 22, 154);
    check_record_exact(p.rrecs[4], "AIC-E0409",
                       "constant slice bound 10 out of range for extent 3",
                       7, 22, 180, 7, 24, 182);
    check_record_exact(p.rrecs[5], "AIC-E0409",
                       "constant slice bound 10 out of range for extent 3",
                       8, 20, 204, 8, 22, 206);

    pipeline_free(&p);
}

/* str-slice code-point boundary failures (spec sec. 12.2 -> AIC-E0410):
 * the record's primary span is the slice expression. */
static void test_str_boundary_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "const s: str = \"h\\xC3\\xA9llo\";\n"
        "var a: str = \"h\\xC3\\xA9llo\"[2..3];\n"
        "var b: str = s[2..3];\n"
        "var c: str = s[1..2];\n"
        "var d: str = s[3..5];\n"
        "var e: str = \"h\\xC3\\xA9llo\"[0..2];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 4);
    if (p.rrn != 4) {
        pipeline_free(&p);
        return;
    }
    /* a: literal base slice [2..3] -- span is the slice expression */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "a"));
        const AstNode *sl = find_slice(site);
        CHECK(sl != NULL);
        if (sl) {
            CHECK(p.rrecs[0]->primary_span->start.offset ==
                  sl->span->start.offset);
            CHECK(p.rrecs[0]->primary_span->end.offset ==
                  sl->span->end.offset);
        }
    }
    check_record_exact(p.rrecs[0], "AIC-E0410",
                       "constant str slice not on code point boundary",
                       3, 14, 57, 3, 34, 77);
    check_record_exact(p.rrecs[1], "AIC-E0410",
                       "constant str slice not on code point boundary",
                       4, 14, 92, 4, 21, 99);
    check_record_exact(p.rrecs[2], "AIC-E0410",
                       "constant str slice not on code point boundary",
                       5, 14, 114, 5, 21, 121);
    check_record_exact(p.rrecs[3], "AIC-E0410",
                       "constant str slice not on code point boundary",
                       7, 14, 158, 7, 34, 178);

    pipeline_free(&p);
}

/* Pointer-difference divisibility (spec sec. 12.5 -> AIC-E0411): the
 * record's primary span is the subtraction. The reachable constant
 * path today uses raw addresses from integer->pointer casts (the
 * same-object T*->U* cast path is blocked by a WP-M0-12a defect
 * routed for remediation: pointer casts lose the object identity).
 * Different-object pointer differences are not constant expressions
 * (12a owns E0401; this package emits no E0411 for them). */
static void test_ptr_diff_kinds(void)
{
    static const char src[] =
        "module main;\n"
        "var arr: u8[4] = [1u8, 2u8, 3u8, 4u8];\n"
        "var d: isize = cast<i32*>(1) - cast<i32*>(0);\n"
        "var e: isize = cast<i64*>(1) - cast<i64*>(0);\n"
        "var ok1: isize = cast<i32*>(4) - cast<i32*>(0);\n"
        "var ok2: isize = &arr[1] - &arr[0];\n"
        "var ok3: isize = &arr[3] - &arr[1];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 2);
    if (p.rrn != 2) {
        pipeline_free(&p);
        return;
    }
    /* d: the subtraction is the located node */
    {
        const AstNode *site = global_init(find_global_decl(p.program, "d"));
        const AstNode *sub = find_binop(site, AST_BIN_SUB);
        CHECK(sub != NULL);
        if (sub) {
            CHECK(p.rrecs[0]->primary_span->start.offset ==
                  sub->span->start.offset);
            CHECK(p.rrecs[0]->primary_span->end.offset ==
                  sub->span->end.offset);
        }
    }
    check_record_exact(p.rrecs[0], "AIC-E0411",
                       "constant pointer difference not divisible by "
                       "element size",
                       3, 16, 67, 3, 45, 96);
    check_record_exact(p.rrecs[1], "AIC-E0411",
                       "constant pointer difference not divisible by "
                       "element size",
                       4, 16, 113, 4, 45, 142);

    pipeline_free(&p);
}

/* Different-object pointer difference: not a constant expression;
 * 12a emits AIC-E0401, this package emits no E0411 record. */
static void test_ptr_diff_not_const(void)
{
    static const char src[] =
        "module main;\n"
        "var a2: i32 = 1;\n"
        "var b2: i32 = 2;\n"
        "var f: isize = &b2 - &a2;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_DIAG_ERROR);
    CHECK(p.ern == 1);                 /* 12a's E0401 for the site */
    CHECK(p.rsc == REC_FAIL_OK);       /* 12b2 emits nothing */
    CHECK(p.rrn == 0);

    pipeline_free(&p);
}

/* Never-trap guarantee on the extreme values for the remaining kinds:
 * the process must survive every constant failure below (a host trap
 * would abort the test). */
static void test_never_traps(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = cast<i8>(9223372036854775807i64);\n"
        "var b: i8 = cast<i8>(-9223372036854775808i64);\n"
        "var c: bool = cast<bool>(0xFFFFFFFFFFFFFFFFu64);\n"
        "var arr: i32[3] = [1, 2, 3];\n"
        "var d: i32* = &arr[9223372036854775807i64];\n"
        "var e: i32* = &arr[0xFFFFFFFFFFFFFFFFu64];\n"
        "var f: i32* = &arr[-9223372036854775808i64];\n"
        "var g: i32[] = arr[9223372036854775807..];\n"
        "var h: i32[] = arr[..9223372036854775807];\n"
        "var u: u8[4] = [1u8, 2u8, 3u8, 4u8];\n"
        "var i: isize = cast<i32*>(9223372036854775807i64) - cast<i32*>(0);\n"
        "var j: isize = cast<i32*>(0) - cast<i32*>(1);\n"
        "var k: str = \"\\xC3\\xA9\\xC3\\xA9\"[0..1];\n"
        "var l: str = \"\\xC3\\xA9\"[1..1];\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *want[] = {
        "AIC-E0408", "AIC-E0408", "AIC-E0408",
        "AIC-E0409", "AIC-E0409", "AIC-E0409", "AIC-E0409", "AIC-E0409",
        "AIC-E0411", "AIC-E0411", "AIC-E0410", "AIC-E0410"
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 12);
    if (p.rrn != 12) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 12; i++) {
        check_record_code(p.rrecs[i], want[i]);
    }
    /* e: the big u64 index renders unsigned; f: INT64_MIN is a valid
     * (negative) bound value, never multiplied */
    CHECK(p.rrecs[4]->message != NULL &&
          strstr(p.rrecs[4]->message, "18446744073709551615") != NULL);
    CHECK(p.rrecs[5]->message != NULL &&
          strstr(p.rrecs[5]->message, "-9223372036854775808") != NULL);

    pipeline_free(&p);
}

/* Determinism: two independent pipelines over the same source produce
 * byte-identical JSONL for the emitted records (same code, message,
 * spans, ordering). */
static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = cast<i8>(300);\n"
        "var arr: i32[3] = [1, 2, 3];\n"
        "var b: i32* = &arr[9];\n"
        "const s: str = \"\\xC3\\xA9\";\n"
        "var c: str = s[1..1];\n"
        "var d: isize = cast<i32*>(1) - cast<i32*>(0);\n"
        "var e: i32 = 5 / 0;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.rsc == p2.rsc);
    CHECK(p1.rsc == REC_FAIL_FAILURE);
    CHECK(p1.rrn == p2.rrn);
    CHECK(p1.rrn == 5);
    if (p1.rrn != p2.rrn || p1.rrn != 5) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.rrn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.rrecs[i]));
        CHECK(diag_emit_record(&b2, p2.rrecs[i]));
        CHECK(b1.len == b2.len);
        CHECK(b1.len == 0 || memcmp(b1.data, b2.data, b1.len) == 0);
        diag_buf_free(&b1);
        diag_buf_free(&b2);
    }

    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* All seven const codes emitted from one source, in the contract
 * sec. 9 deterministic order (by span offset). */
static void test_emission_all_codes(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i32 = 5 / 0;\n"
        "var b: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "var c: i8 = cast<i8>(300);\n"
        "var d: i32 = 1 << 33;\n"
        "var arr: i32[3] = [1, 2, 3];\n"
        "var e: i32* = &arr[9];\n"
        "var s: str = \"\\xC3\\xA9\"[1..2];\n"
        "var u: u8[4] = [1u8, 2u8, 3u8, 4u8];\n"
        "var f: isize = cast<i32*>(1) - cast<i32*>(0);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const char *want[] = {
        "AIC-E0406", "AIC-E0405", "AIC-E0408", "AIC-E0407",
        "AIC-E0409", "AIC-E0410", "AIC-E0411"
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 7);
    if (p.rrn != 7) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < 7; i++) {
        check_record_code(p.rrecs[i], want[i]);
    }
    /* records are sorted by start offset (strictly increasing) */
    for (i = 1; i < 7; i++) {
        CHECK(p.rrecs[i]->primary_span != NULL &&
              p.rrecs[i - 1]->primary_span != NULL);
        if (p.rrecs[i]->primary_span && p.rrecs[i - 1]->primary_span) {
            CHECK(p.rrecs[i - 1]->primary_span->start.offset <
                  p.rrecs[i]->primary_span->start.offset);
        }
    }

    pipeline_free(&p);
}

/* Enum member value expressions are const-context sites: a failure
 * there is emitted and ordered after the preceding declarations. */
static void test_enum_site_records(void)
{
    static const char src[] =
        "module main;\n"
        "enum E: u8 { A = 1, B = 2, C = cast<u8>(300) }\n"
        "var x: i32 = 5 / 0;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    /* 11b's bounded subset cannot evaluate `cast<u8>(300)`; it
     * delegates the site to the const evaluator
     * (LAYOUT_UNEVALUABLE, not a diagnostic). */
    CHECK(p.lst == LAYOUT_OK || p.lst == LAYOUT_UNEVALUABLE);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 2);
    if (p.rrn != 2) {
        pipeline_free(&p);
        return;
    }
    check_record_code(p.rrecs[0], "AIC-E0408");   /* enum member C value */
    check_record_code(p.rrecs[1], "AIC-E0406");   /* var x initializer */
    CHECK(p.rrecs[0]->primary_span != NULL &&
          p.rrecs[1]->primary_span != NULL);
    if (p.rrecs[0]->primary_span && p.rrecs[1]->primary_span) {
        CHECK(p.rrecs[0]->primary_span->start.offset <
              p.rrecs[1]->primary_span->start.offset);
    }
    /* the enum-member record's span is the member value expression */
    {
        const AstNode *ed = find_global_decl(p.program, "E");
        CHECK(ed != NULL);
        if (ed) {
            const AstNode *mem = ed->u.enum_decl.members[2]->u.named.value;
            CHECK(mem != NULL);
            if (mem) {
                CHECK(p.rrecs[0]->primary_span->start.offset ==
                      mem->span->start.offset);
                CHECK(p.rrecs[0]->primary_span->end.offset ==
                      mem->span->end.offset);
            }
        }
    }

    pipeline_free(&p);
}

/* Classification API: every routed kind classifies with the right
 * code/site/op_node and kind-specific facts; non-const sites return
 * REC_FAIL_NOT_CONST. */
static void test_classify_api(void)
{
    static const char src[] =
        "module main;\n"
        "var a: i8 = cast<i8>(300);\n"
        "var arr: i32[3] = [1, 2, 3];\n"
        "var b: i32* = &arr[9];\n"
        "var c: i32 = 1 << 33;\n"
        "var d: i32 = 5 / 0;\n"
        "var e: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "const s: str = \"\\xC3\\xA9\";\n"
        "var f: str = s[1..1];\n"
        "var u: u8[4] = [1u8, 2u8, 3u8, 4u8];\n"
        "var g: isize = cast<i32*>(1) - cast<i32*>(0);\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    EvalFailure want_kinds[] = {
        EVAL_FAIL_CAST_RANGE, EVAL_FAIL_INDEX_RANGE, EVAL_FAIL_SHIFT_RANGE,
        EVAL_FAIL_DIV_ZERO, EVAL_FAIL_OVERFLOW, EVAL_FAIL_STR_BOUNDARY,
        EVAL_FAIL_PTR_DIFF
    };
    const char *want_codes[] = {
        "AIC-E0408", "AIC-E0409", "AIC-E0407",
        "AIC-E0406", "AIC-E0405", "AIC-E0410", "AIC-E0411"
    };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.esc == CONST_EVAL_FAILURE);
    CHECK(p.efailn == 7);
    if (p.efailn != 7) {
        pipeline_free(&p);
        return;
    }
    for (i = 0; i < p.efailn; i++) {
        EvalCtx ctx;
        RecFailValue v;
        RecFailStatus st;
        eval_ctx_init(&ctx, p.result, p.build, p.result->modules[0]);
        st = rec_fail_classify(&ctx, p.efails[i].node, p.efails[i].kind, &v);
        eval_ctx_cleanup(&ctx);
        CHECK(st == REC_FAIL_FAILURE);
        CHECK(v.kind == p.efails[i].kind);
        CHECK(v.kind == want_kinds[i]);
        CHECK(v.site == p.efails[i].node);
        CHECK(v.code != NULL && strcmp(v.code, want_codes[i]) == 0);
        CHECK(v.op_node != NULL);
        if (v.op_node) {
            CHECK(v.op_node->span->start.offset >=
                  v.site->span->start.offset);
            CHECK(v.op_node->span->end.offset <= v.site->span->end.offset);
        }
    }

    /* kind-specific facts */
    {
        EvalCtx ctx;
        RecFailValue v;
        eval_ctx_init(&ctx, p.result, p.build, p.result->modules[0]);
        /* a: cast facts */
        CHECK(rec_fail_classify(&ctx, p.efails[0].node, p.efails[0].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.cast_value.v == 300 && !v.cast_value.big);
        CHECK(strcmp(v.cast_target, "i8") == 0);
        /* b: bound facts */
        CHECK(rec_fail_classify(&ctx, p.efails[1].node, p.efails[1].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.bound_value.v == 9 && !v.bound_value.big);
        CHECK(v.extent == 3);
        CHECK(!v.is_slice_bound);
        /* c: shift facts (count + left operand type name) */
        CHECK(rec_fail_classify(&ctx, p.efails[2].node, p.efails[2].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.b.v == 33);
        CHECK(strcmp(v.op_type, "i32") == 0);
        /* d: div-zero operator facts */
        CHECK(rec_fail_classify(&ctx, p.efails[3].node, p.efails[3].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.op == AST_BIN_DIV && !v.is_unary);
        /* e: overflow operator facts */
        CHECK(rec_fail_classify(&ctx, p.efails[4].node, p.efails[4].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.op == AST_BIN_ADD && !v.is_unary);
        /* f: str boundary facts */
        CHECK(rec_fail_classify(&ctx, p.efails[5].node, p.efails[5].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.slice_lo == 1 && v.slice_hi == 1);
        /* g: ptr-diff operator facts */
        CHECK(rec_fail_classify(&ctx, p.efails[6].node, p.efails[6].kind,
                                &v) == REC_FAIL_FAILURE);
        CHECK(v.op == AST_BIN_SUB && !v.is_unary);
        eval_ctx_cleanup(&ctx);
    }

    /* non-const site: classifying a site outside the sec. 10.5
     * composition is out of the classify contract (it classifies
     * already-routed EVAL_FAILURE sites from 12a); it must not claim
     * an allocation failure. The no-record behavior for non-const
     * sites is covered by test_ptr_diff_not_const and by the
     * classify-API non-const probe below. */
    {
        static const char nc_src[] =
            "module main;\n"
            "fn helper() -> i32 { return 1; }\n"
            "const A: i32 = helper();\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        EvalCtx ctx;
        RecFailValue v;
        EvalStatus est;
        EvalValue ev;
        EvalFailure fail = EVAL_FAIL_NONE;
        RecFailStatus rst;
        pipeline_run_mem(&q, nc_src);
        CHECK(q.st == NAME_OK);
        CHECK(q.esc == CONST_EVAL_DIAG_ERROR);
        eval_ctx_init(&ctx, q.result, q.build, q.result->modules[0]);
        est = const_eval_expr(&ctx,
                              global_init(find_global_decl(q.program, "A")),
                              &ev, &fail);
        CHECK(est == EVAL_NOT_CONST);
        if (est == EVAL_NOT_CONST) {
            rst = rec_fail_classify(&ctx,
                                    global_init(find_global_decl(q.program, "A")),
                                    EVAL_FAIL_OVERFLOW, &v);
            CHECK(rst != REC_FAIL_OOM);
        }
        eval_ctx_cleanup(&ctx);
        pipeline_free(&q);
    }

    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Negative-corpus anchors (exact records)
 * ------------------------------------------------------------------------- */

static char *read_file_bytes(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return buf;
}

typedef struct AnchorExpect {
    const char *dir;     /* tests/negative/cases/<dir>/ */
    const char *code;
    const char *message;
    int64_t sl, sc, so, el, ec, eo;
} AnchorExpect;

static const AnchorExpect kAnchors[] = {
    { "18-4-const-overflow", "AIC-E0405", "constant expression overflow",
      2, 13, 25, 2, 40, 52 },
    { "18-4-const-div-zero", "AIC-E0406", "constant division by zero",
      2, 14, 26, 2, 19, 31 },
    { "derived-semantic-shift-out-of-range", "AIC-E0407",
      "constant shift count out of range: 33 exceeds i32 bit width",
      2, 14, 26, 2, 21, 33 },
    { "18-4-const-cast-out-of-range", "AIC-E0408",
      "constant cast out of range: 200 does not fit in i8",
      3, 13, 52, 3, 26, 65 },
    { "derived-semantic-index-out-of-range", "AIC-E0409",
      "constant index 10 out of range for array of length 3",
      3, 20, 61, 3, 22, 63 },
    { "derived-semantic-str-slice-boundary", "AIC-E0410",
      "constant str slice not on code point boundary",
      3, 16, 53, 3, 23, 60 },
    { "derived-semantic-ptr-diff-divisible", "AIC-E0411",
      "constant pointer difference not divisible by element size",
      2, 16, 28, 2, 45, 57 },
};

static void test_corpus_anchors(void)
{
    size_t k;
    for (k = 0; k < sizeof(kAnchors) / sizeof(kAnchors[0]); k++) {
        const AnchorExpect *a = &kAnchors[k];
        char path[256];
        char *src;
        size_t srclen;
        Pipeline p;
        snprintf(path, sizeof(path), "tests/negative/cases/%s/input.ai",
                 a->dir);
        src = read_file_bytes(path, &srclen);
        CHECK(src != NULL);
        if (!src) continue;
        pipeline_run_mem(&p, src);
        free(src);
        CHECK(p.st == NAME_OK);
        CHECK(p.esc == CONST_EVAL_FAILURE);   /* 12a routed the failure */
        CHECK(p.rsc == REC_FAIL_FAILURE);
        CHECK(p.rrn == 1);
        if (p.rrn == 1) {
            check_record_exact(p.rrecs[0], a->code, a->message,
                               a->sl, a->sc, a->so, a->el, a->ec, a->eo);
        }
        pipeline_free(&p);
    }
}

int main(void)
{
    test_rec_fail_code();
    fprintf(stderr, "after test_rec_fail_code\n");
    test_cast_range_kinds();
    fprintf(stderr, "after test_cast_range_kinds\n");
    test_index_range_kinds();
    fprintf(stderr, "after test_index_range_kinds\n");
    test_str_boundary_kinds();
    fprintf(stderr, "after test_str_boundary_kinds\n");
    test_ptr_diff_kinds();
    fprintf(stderr, "after test_ptr_diff_kinds\n");
    test_ptr_diff_not_const();
    fprintf(stderr, "after test_ptr_diff_not_const\n");
    test_never_traps();
    fprintf(stderr, "after test_never_traps\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_emission_all_codes();
    fprintf(stderr, "after test_emission_all_codes\n");
    test_enum_site_records();
    fprintf(stderr, "after test_enum_site_records\n");
    test_classify_api();
    fprintf(stderr, "after test_classify_api\n");
    test_corpus_anchors();
    fprintf(stderr, "after test_corpus_anchors\n");

    if (g_failures) {
        fprintf(stderr, "eval_fail_rec_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("eval_fail_rec_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
