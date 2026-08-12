/* bootstrap/src/sema/expr_core_test.c
 *
 * WP-M0-13b1 expression core tests: the precedence model (spec sec.
 * 10.1), the evaluation-order model (sec. 10.4), and the const-context
 * check (AIC-E0401) at the const-context sites this package owns -
 * array type extents (sec. 12.1), switch case labels (sec. 13.2), and
 * local const declaration initializers (sec. 8.1). Global const/var
 * initializers and enum member value expressions belong to WP-M0-12a
 * and are covered by the boundary test (no duplicate E0401).
 *
 * The precedence/evaluation-order tests are purely structural
 * (expr_order_plan walks the AST only), so they use a parse-only
 * pipeline; the const-context tests run the full pipeline through
 * layout/convert/optype/const_eval (12a) and expr_core (13b1).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-sema-b1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/expr_core_test.c \
 *     bootstrap/src/sema/expr_core.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-sema-b1/expr_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-sema-b1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "expr_core.h"

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
 * Parse-only pipeline (structural tests: precedence, evaluation order)
 * ------------------------------------------------------------------------- */

typedef struct ParseOnly {
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    DiagRecord **recs;
    size_t rn;
} ParseOnly;

static void parse_only_run(ParseOnly *p, const char *src_text)
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
}

static void parse_only_free(ParseOnly *p)
{
    name_records_free(p->recs, p->rn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* ---------------------------------------------------------------------------
 * Full pipeline: load -> lex -> parse -> name_resolve -> completeness ->
 * layout -> convert -> optype -> const_eval_check (12a) -> expr_core_check
 * (13b1). Both const stages run so the boundary test can prove 13b1 does
 * not duplicate 12a's E0401 records.
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
    EvalFailureSite *efails;
    size_t efailn;
    ConstEvalStatus esc;
    DiagRecord **xrecs;     /* 13b1 expr_core records (AIC-E0401) */
    size_t xrn;
    EvalFailureSite *xfails;
    size_t xfailn;
    ExprCoreStatus xsc;
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
    /* LAYOUT_UNEVALUABLE is a routing signal, not a rejection: the driver
     * sends such programs to the const stage, and 13b1 owns the
     * const-context rejection for array extents (layout.h boundary note).
     * Only a real layout diagnostic or failure stops the pipeline. */
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR &&
        p->lst != LAYOUT_UNEVALUABLE) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    if (p->ost == OPTYPE_DIAG_ERROR) return;
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
    p->xsc = expr_core_check(p->result, p->build, &p->xrecs, &p->xrn,
                             &p->xfails, &p->xfailn);
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
    types_records_free(p->xrecs, p->xrn);
    free(p->xfails);
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
    if (!program) return NULL;
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
        case AST_STRUCT_DECL:
            n = d->u.struct_decl.name;
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

/* Offset of the first occurrence of `marker` in `src`, or -1. */
static int64_t marker_off(const char *src, const char *marker)
{
    const char *hit = strstr(src, marker);
    return hit ? (int64_t)(hit - src) : -1;
}

/* ---------------------------------------------------------------------------
 * Plan assertions
 * ------------------------------------------------------------------------- */

typedef struct ExpectedStep {
    ExprStepKind kind;
    const char *marker;    /* unique source text of the node this step concerns */
    bool conditional;
} ExpectedStep;

/* Run expr_order_plan on `expr` (value mode unless loc=true) and check the
 * step sequence against `exp`. Marker text must be unique in `src`. */
static void check_plan(const AstNode *expr, bool loc,
                       const ExpectedStep *exp, size_t n,
                       const char *src)
{
    ExprStep *steps = NULL;
    size_t nsteps = 0;
    ExprOrderStatus st = expr_order_plan(expr, loc, &steps, &nsteps);
    size_t i;
    CHECK(st == EXPR_ORDER_OK);
    if (st != EXPR_ORDER_OK) return;
    CHECK(nsteps == n);
    if (nsteps != n) {
        fprintf(stderr, "  plan: got %u steps, want %u\n",
                (unsigned)nsteps, (unsigned)n);
        expr_steps_free(steps);
        return;
    }
    for (i = 0; i < n; i++) {
        int64_t off = -1;
        const char *hit = exp[i].marker ? strstr(src, exp[i].marker) : NULL;
        CHECK(steps[i].kind == exp[i].kind);
        CHECK(steps[i].conditional == exp[i].conditional);
        CHECK(steps[i].node != NULL);
        if (exp[i].marker && steps[i].node && steps[i].node->span) {
            off = steps[i].node->span->start.offset;
            CHECK(hit != NULL);
            if (hit) CHECK(off == (int64_t)(hit - src));
        }
        if (steps[i].kind != exp[i].kind ||
            steps[i].conditional != exp[i].conditional) {
            fprintf(stderr, "  step %u: got kind=%d cond=%d node_off=%lld\n",
                    (unsigned)i, (int)steps[i].kind,
                    (int)steps[i].conditional,
                    (long long)(steps[i].node && steps[i].node->span
                                ? steps[i].node->span->start.offset : -1));
        }
    }
    expr_steps_free(steps);
}

/* ---------------------------------------------------------------------------
 * Record assertions
 * ------------------------------------------------------------------------- */

/* Check an E0401 record's shape and that its primary span covers exactly
 * [marker_off(src, marker) + sub_off, + len). */
static void check_e0401_span(const DiagRecord *r, const char *src,
                             const char *marker, int64_t sub_off,
                             int64_t len)
{
    const char *hit;
    int64_t off;
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->code && strcmp(r->code, "AIC-E0401") == 0);
    CHECK(r->severity && strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(r->phase && strcmp(r->phase, DIAG_PHASE_SEMANTIC) == 0);
    CHECK(r->recovery &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    CHECK(r->message &&
          strcmp(r->message, "expression is not a constant expression") == 0);
    CHECK(r->primary_span != NULL);
    if (!r->primary_span) return;
    hit = strstr(src, marker);
    CHECK(hit != NULL);
    if (!hit) return;
    off = (int64_t)(hit - src) + sub_off;
    CHECK(r->primary_span->start.offset == off);
    CHECK(r->primary_span->end.offset == off + len);
}

/* ---------------------------------------------------------------------------
 * 1. Precedence model (spec sec. 10.1)
 * ------------------------------------------------------------------------- */

static void test_precedence_model(void)
{
    static const struct {
        const char *expr;
        ExprPrecedence prec;
    } kCases[] = {
        { "aa * bb + cc", EXPR_PREC_ADDITIVE },   /* + binds looser than * */
        { "aa << bb + cc", EXPR_PREC_SHIFT },     /* shift below additive */
        { "aa < bb << cc", EXPR_PREC_RELATIONAL },
        { "aa == bb < cc", EXPR_PREC_EQUALITY },
        { "aa & bb == cc", EXPR_PREC_BAND },
        { "aa ^ bb & cc", EXPR_PREC_BXOR },
        { "aa | bb ^ cc", EXPR_PREC_BOR },
        { "aa && bb | cc", EXPR_PREC_LAND },
        { "aa || bb && cc", EXPR_PREC_LOR },
        { "c0 ? aa : bb", EXPR_PREC_CONDITIONAL },
        { "aa = c0 ? bb : cc", EXPR_PREC_ASSIGNMENT },
        { "-xx", EXPR_PREC_UNARY },
        { "arr[idx]", EXPR_PREC_POSTFIX },
        { "fnc(px)", EXPR_PREC_POSTFIX },
        { "pp->ff", EXPR_PREC_POSTFIX },
        { "sizeof(qq)", EXPR_PREC_UNARY },
        { "cast<i32>(zz)", EXPR_PREC_UNARY },
        { "len(aa)", EXPR_PREC_UNARY },
        { "42", EXPR_PREC_NONE },
        { "xx", EXPR_PREC_NONE },
    };
    size_t i;
    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char src[256];
        ParseOnly p;
        const AstNode *g, *e;
        snprintf(src, sizeof src, "module main;\n"
                 "var g: i32 = %s;\n"
                 "fn main() -> i32 { return 0; }\n", kCases[i].expr);
        parse_only_run(&p, src);
        g = find_global_decl(p.program, "g");
        e = g ? global_init(g) : NULL;
        CHECK(e != NULL);
        if (e) CHECK(expr_precedence_of(e) == kCases[i].prec);
        parse_only_free(&p);
    }
    CHECK(expr_precedence_of(NULL) == EXPR_PREC_NONE);
}

/* ---------------------------------------------------------------------------
 * 2. Evaluation order: binary operators (sec. 10.4, left-to-right)
 * ------------------------------------------------------------------------- */

static void test_eval_order_binary(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = aa + bb * cc;\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *g, *e;
    ExpectedStep exp[] = {
        { EXPR_STEP_VALUE, "aa", false },
        { EXPR_STEP_VALUE, "bb", false },
        { EXPR_STEP_VALUE, "cc", false },
        { EXPR_STEP_VALUE, "bb * cc", false },
        { EXPR_STEP_VALUE, "aa + bb * cc", false },
    };
    parse_only_run(&p, src);
    g = find_global_decl(p.program, "g");
    e = g ? global_init(g) : NULL;
    CHECK(e != NULL);
    if (e) check_plan(e, false, exp, sizeof(exp) / sizeof(exp[0]), src);
    parse_only_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. Evaluation order: call, index, slice (callee/args; a then i; a x y)
 * ------------------------------------------------------------------------- */

static void test_eval_order_call_index_slice(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = fnc(px, py);\n"
        "var h: i32 = arr[idx];\n"
        "var s: i32 = arr2[x0..y0];\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *g, *h, *s;
    ExpectedStep call_exp[] = {
        { EXPR_STEP_VALUE, "fnc", false },
        { EXPR_STEP_VALUE, "px", false },
        { EXPR_STEP_VALUE, "py", false },
        { EXPR_STEP_VALUE, "fnc(px, py)", false },
    };
    ExpectedStep idx_exp[] = {
        { EXPR_STEP_VALUE, "arr", false },
        { EXPR_STEP_VALUE, "idx", false },
        { EXPR_STEP_VALUE, "arr[idx]", false },
    };
    ExpectedStep sl_exp[] = {
        { EXPR_STEP_VALUE, "arr2", false },
        { EXPR_STEP_VALUE, "x0", false },
        { EXPR_STEP_VALUE, "y0", false },
        { EXPR_STEP_VALUE, "arr2[x0..y0]", false },
    };
    parse_only_run(&p, src);
    g = find_global_decl(p.program, "g");
    h = find_global_decl(p.program, "h");
    s = find_global_decl(p.program, "s");
    CHECK(g && h && s);
    if (g) check_plan(global_init(g), false, call_exp,
                      sizeof(call_exp) / sizeof(call_exp[0]), src);
    if (h) check_plan(global_init(h), false, idx_exp,
                      sizeof(idx_exp) / sizeof(idx_exp[0]), src);
    if (s) check_plan(global_init(s), false, sl_exp,
                      sizeof(sl_exp) / sizeof(sl_exp[0]), src);
    parse_only_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. Evaluation order: assignment and compound assignment
 *    (dest location before source; read/store for compound)
 * ------------------------------------------------------------------------- */

static void test_eval_order_assignment_compound(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = tgt1 = b1;\n"
        "var h: i32 = tgt2 += b2;\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *g, *h;
    ExpectedStep asgn_exp[] = {
        { EXPR_STEP_LOCATION, "tgt1", false },
        { EXPR_STEP_VALUE, "b1", false },
        { EXPR_STEP_STORE, "tgt1 = b1", false },
    };
    ExpectedStep cpd_exp[] = {
        { EXPR_STEP_LOCATION, "tgt2", false },
        { EXPR_STEP_VALUE, "b2", false },
        { EXPR_STEP_READ, "tgt2", false },
        { EXPR_STEP_STORE, "tgt2 += b2", false },
    };
    parse_only_run(&p, src);
    g = find_global_decl(p.program, "g");
    h = find_global_decl(p.program, "h");
    CHECK(g && h);
    if (g) check_plan(global_init(g), false, asgn_exp,
                      sizeof(asgn_exp) / sizeof(asgn_exp[0]), src);
    if (h) check_plan(global_init(h), false, cpd_exp,
                      sizeof(cpd_exp) / sizeof(cpd_exp[0]), src);
    parse_only_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. Evaluation order: short-circuit && || and ?: (conditional steps)
 * ------------------------------------------------------------------------- */

static void test_eval_order_short_circuit_ternary(void)
{
    static const char src[] =
        "module main;\n"
        "var g: bool = la && lb;\n"
        "var h: bool = lc || ld;\n"
        "var t: i32 = c0 ? x0 : y0;\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *g, *h, *t;
    ExpectedStep land_exp[] = {
        { EXPR_STEP_VALUE, "la", false },
        { EXPR_STEP_VALUE, "lb", true },
        { EXPR_STEP_VALUE, "la && lb", false },
    };
    ExpectedStep lor_exp[] = {
        { EXPR_STEP_VALUE, "lc", false },
        { EXPR_STEP_VALUE, "ld", true },
        { EXPR_STEP_VALUE, "lc || ld", false },
    };
    ExpectedStep ter_exp[] = {
        { EXPR_STEP_VALUE, "c0", false },
        { EXPR_STEP_VALUE, "x0", true },
        { EXPR_STEP_VALUE, "y0", true },
        { EXPR_STEP_VALUE, "c0 ? x0 : y0", false },
    };
    parse_only_run(&p, src);
    g = find_global_decl(p.program, "g");
    h = find_global_decl(p.program, "h");
    t = find_global_decl(p.program, "t");
    CHECK(g && h && t);
    if (g) check_plan(global_init(g), false, land_exp,
                      sizeof(land_exp) / sizeof(land_exp[0]), src);
    if (h) check_plan(global_init(h), false, lor_exp,
                      sizeof(lor_exp) / sizeof(lor_exp[0]), src);
    if (t) check_plan(global_init(t), false, ter_exp,
                      sizeof(ter_exp) / sizeof(ter_exp[0]), src);
    parse_only_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. Evaluation order: member access (operand only), sizeof/alignof
 *    (operand NOT evaluated), unary, address-of, deref, cast/wrap, len/ptr
 * ------------------------------------------------------------------------- */

static void test_eval_order_member_sizeof_unary(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = pp->ff;\n"
        "var h: usize = sizeof(q1);\n"
        "var i: i32 = -xx;\n"
        "var j: i32 = *q2;\n"
        "var k: i32 = &yy;\n"
        "var l: i32 = cast<i32>(zz);\n"
        "var m: usize = len(aa);\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *g, *h, *i, *j, *k, *l, *m;
    ExpectedStep mem_exp[] = {
        { EXPR_STEP_VALUE, "pp", false },
        { EXPR_STEP_VALUE, "pp->ff", false },
    };
    ExpectedStep sz_exp[] = {
        { EXPR_STEP_NOT_EVAL, "q1", false },
        { EXPR_STEP_VALUE, "sizeof(q1)", false },
    };
    ExpectedStep neg_exp[] = {
        { EXPR_STEP_VALUE, "xx", false },
        { EXPR_STEP_VALUE, "-xx", false },
    };
    ExpectedStep deref_exp[] = {
        { EXPR_STEP_VALUE, "q2", false },
        { EXPR_STEP_VALUE, "*q2", false },
    };
    ExpectedStep addr_exp[] = {
        { EXPR_STEP_LOCATION, "yy", false },
        { EXPR_STEP_VALUE, "&yy", false },
    };
    ExpectedStep cast_exp[] = {
        { EXPR_STEP_VALUE, "zz", false },
        { EXPR_STEP_VALUE, "cast<i32>(zz)", false },
    };
    ExpectedStep len_exp[] = {
        { EXPR_STEP_VALUE, "aa", false },
        { EXPR_STEP_VALUE, "len(aa)", false },
    };
    parse_only_run(&p, src);
    g = find_global_decl(p.program, "g");
    h = find_global_decl(p.program, "h");
    i = find_global_decl(p.program, "i");
    j = find_global_decl(p.program, "j");
    k = find_global_decl(p.program, "k");
    l = find_global_decl(p.program, "l");
    m = find_global_decl(p.program, "m");
    CHECK(g && h && i && j && k && l && m);
    if (g) check_plan(global_init(g), false, mem_exp,
                      sizeof(mem_exp) / sizeof(mem_exp[0]), src);
    if (h) check_plan(global_init(h), false, sz_exp,
                      sizeof(sz_exp) / sizeof(sz_exp[0]), src);
    if (i) check_plan(global_init(i), false, neg_exp,
                      sizeof(neg_exp) / sizeof(neg_exp[0]), src);
    if (j) check_plan(global_init(j), false, deref_exp,
                      sizeof(deref_exp) / sizeof(deref_exp[0]), src);
    if (k) check_plan(global_init(k), false, addr_exp,
                      sizeof(addr_exp) / sizeof(addr_exp[0]), src);
    if (l) check_plan(global_init(l), false, cast_exp,
                      sizeof(cast_exp) / sizeof(cast_exp[0]), src);
    if (m) check_plan(global_init(m), false, len_exp,
                      sizeof(len_exp) / sizeof(len_exp[0]), src);
    parse_only_free(&p);
}

/* Location-mode plan: an index expression as an assignment target is
 * evaluated for its location. */
static void test_eval_order_location_mode(void)
{
    static const char src[] =
        "module main;\n"
        "var h: i32 = tgt2[idx2] = v2;\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *h;
    ExpectedStep idx_loc[] = {
        { EXPR_STEP_VALUE, "tgt2", false },
        { EXPR_STEP_VALUE, "idx2", false },
        { EXPR_STEP_LOCATION, "tgt2[idx2]", false },
    };
    parse_only_run(&p, src);
    h = find_global_decl(p.program, "h");
    CHECK(h != NULL);
    if (h) {
        const AstNode *tgt = global_init(h)->u.assign.target;
        CHECK(tgt != NULL && tgt->kind == AST_EXPR_INDEX);
        if (tgt) check_plan(tgt, true, idx_loc,
                            sizeof(idx_loc) / sizeof(idx_loc[0]), src);
    }
    parse_only_free(&p);
}

/* ---------------------------------------------------------------------------
 * 7. Evaluation order: literals, array literals, struct init, parens
 * ------------------------------------------------------------------------- */

static void test_eval_order_literals_arrays_struct(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "var g: i32 = 42;\n"
        "var h: i32[2] = [xx, yy];\n"
        "var i: i32[2] = [ee; nn];\n"
        "var j: Point = Point { x: aa, y: bb };\n"
        "var k: i32 = (aa2 + bb2);\n"
        "fn main() -> i32 { return 0; }\n";
    ParseOnly p;
    const AstNode *g, *h, *i, *j, *k;
    ExpectedStep lit_exp[] = {
        { EXPR_STEP_VALUE, "42", false },
    };
    ExpectedStep arr_exp[] = {
        { EXPR_STEP_VALUE, "xx", false },
        { EXPR_STEP_VALUE, "yy", false },
        { EXPR_STEP_VALUE, "[xx, yy]", false },
    };
    ExpectedStep rep_exp[] = {
        { EXPR_STEP_VALUE, "ee", false },
        { EXPR_STEP_VALUE, "nn", false },
        { EXPR_STEP_VALUE, "[ee; nn]", false },
    };
    ExpectedStep st_exp[] = {
        { EXPR_STEP_VALUE, "aa", false },
        { EXPR_STEP_VALUE, "bb", false },
        { EXPR_STEP_VALUE, "Point { x: aa, y: bb }", false },
    };
    ExpectedStep paren_exp[] = {
        { EXPR_STEP_VALUE, "aa2", false },
        { EXPR_STEP_VALUE, "bb2", false },
        { EXPR_STEP_VALUE, "aa2 + bb2", false },
    };
    parse_only_run(&p, src);
    g = find_global_decl(p.program, "g");
    h = find_global_decl(p.program, "h");
    i = find_global_decl(p.program, "i");
    j = find_global_decl(p.program, "j");
    k = find_global_decl(p.program, "k");
    CHECK(g && h && i && j && k);
    if (g) check_plan(global_init(g), false, lit_exp,
                      sizeof(lit_exp) / sizeof(lit_exp[0]), src);
    if (h) check_plan(global_init(h), false, arr_exp,
                      sizeof(arr_exp) / sizeof(arr_exp[0]), src);
    if (i) check_plan(global_init(i), false, rep_exp,
                      sizeof(rep_exp) / sizeof(rep_exp[0]), src);
    if (j) check_plan(global_init(j), false, st_exp,
                      sizeof(st_exp) / sizeof(st_exp[0]), src);
    if (k) check_plan(global_init(k), false, paren_exp,
                      sizeof(paren_exp) / sizeof(paren_exp[0]), src);
    parse_only_free(&p);
}

/* ---------------------------------------------------------------------------
 * 8. Const-context: array type extents (sec. 12.1) -> AIC-E0401
 * ------------------------------------------------------------------------- */

static void test_const_context_array_extent(void)
{
    /* Non-const extent in a struct field type. */
    static const char bad_field[] =
        "module main;\n"
        "var n: i32 = 5;\n"
        "struct S { f: i32[n]; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, bad_field);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_DIAG_ERROR);
    CHECK(p.xrn == 1);
    if (p.xrn == 1) {
        r = p.xrecs[0];
        check_e0401_span(r, bad_field, "i32[n]", 4, 1);   /* the extent `n` */
    }
    CHECK(p.ern == 0);   /* 12a owns global inits, not extents */
    pipeline_free(&p);

    /* Non-const extent in a parameter type. */
    {
        static const char bad_param[] =
            "module main;\n"
            "var n: i32 = 5;\n"
            "fn use(p: i32[n]) -> i32 { return 0; }\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        pipeline_run_mem(&q, bad_param);
        CHECK(q.st == NAME_OK);
        if (q.st != NAME_OK) { pipeline_free(&q); return; }
        CHECK(q.xsc == EXPR_CORE_DIAG_ERROR);
        CHECK(q.xrn == 1);
        if (q.xrn == 1) {
            check_e0401_span(q.xrecs[0], bad_param, "i32[n]", 4, 1);
        }
        pipeline_free(&q);
    }

    /* Non-const extent in a global var type. */
    {
        static const char bad_global[] =
            "module main;\n"
            "var n: i32 = 5;\n"
            "var a: i32[n] = [0; 5];\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        pipeline_run_mem(&q, bad_global);
        CHECK(q.st == NAME_OK);
        if (q.st != NAME_OK) { pipeline_free(&q); return; }
        CHECK(q.xsc == EXPR_CORE_DIAG_ERROR);
        CHECK(q.xrn == 1);
        if (q.xrn == 1) {
            check_e0401_span(q.xrecs[0], bad_global, "i32[n]", 4, 1);
        }
        pipeline_free(&q);
    }

    /* Non-const extent nested inside a RUNTIME expression (sizeof type
     * operand): the array type's extent is a const-context site wherever
     * the type appears (sec. 12.1), including inside sizeof/cast type
     * operands of a runtime local var initializer. */
    {
        static const char bad_nested[] =
            "module main;\n"
            "var n: i32 = 5;\n"
            "fn f() -> usize {\n"
            "  var x: usize = sizeof(i32[n]);\n"
            "  return x;\n"
            "}\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        pipeline_run_mem(&q, bad_nested);
        CHECK(q.st == NAME_OK);
        if (q.st != NAME_OK) { pipeline_free(&q); return; }
        CHECK(q.xsc == EXPR_CORE_DIAG_ERROR);
        CHECK(q.xrn == 1);
        if (q.xrn == 1) {
            check_e0401_span(q.xrecs[0], bad_nested, "i32[n]", 4, 1);
        }
        pipeline_free(&q);
    }

    /* Valid const extents: a module const and a constant expression. */
    {
        static const char good[] =
            "module main;\n"
            "const N: i32 = 5;\n"
            "struct S { f: i32[N]; }\n"
            "var a: i32[N + 1] = [0; 6];\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        pipeline_run_mem(&q, good);
        CHECK(q.st == NAME_OK);
        if (q.st != NAME_OK) { pipeline_free(&q); return; }
        CHECK(q.xsc == EXPR_CORE_OK);
        CHECK(q.xrn == 0);
        CHECK(q.xfailn == 0);
        pipeline_free(&q);
    }
}

/* ---------------------------------------------------------------------------
 * 9. Const-context: case labels (sec. 13.2) -> AIC-E0401
 * ------------------------------------------------------------------------- */

static void test_const_context_case_label(void)
{
    static const char bad[] =
        "module main;\n"
        "var n: i32 = 5;\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case n: { return 1; }\n"
        "    default: { return 0; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, bad);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_DIAG_ERROR);
    CHECK(p.xrn == 1);
    if (p.xrn == 1) {
        check_e0401_span(p.xrecs[0], bad, "case n", 5, 1);   /* the label `n` */
    }
    pipeline_free(&p);

    {
        static const char good[] =
            "module main;\n"
            "const K: i32 = 3;\n"
            "fn f(x: i32) -> i32 {\n"
            "  switch (x) {\n"
            "    case K + 1: { return 1; }\n"
            "    default: { return 0; }\n"
            "  }\n"
            "}\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        pipeline_run_mem(&q, good);
        CHECK(q.st == NAME_OK);
        if (q.st != NAME_OK) { pipeline_free(&q); return; }
        CHECK(q.xsc == EXPR_CORE_OK);
        CHECK(q.xrn == 0);
        pipeline_free(&q);
    }
}

/* ---------------------------------------------------------------------------
 * 10. Const-context: local const declarations (sec. 8.1) -> AIC-E0401
 * ------------------------------------------------------------------------- */

static void test_const_context_local_const(void)
{
    static const char bad[] =
        "module main;\n"
        "var n: i32 = 5;\n"
        "fn f() -> i32 {\n"
        "  const C: i32 = n;\n"
        "  return C;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, bad);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_DIAG_ERROR);
    CHECK(p.xrn == 1);
    if (p.xrn == 1) {
        check_e0401_span(p.xrecs[0], bad, "const C: i32 = n", 15, 1);
    }
    pipeline_free(&p);

    {
        static const char good[] =
            "module main;\n"
            "const K: i32 = 5;\n"
            "fn f() -> i32 {\n"
            "  const C: i32 = K + 1;\n"
            "  for (const D: i32 = 2; D < 3; ) { return C; }\n"
            "  return C;\n"
            "}\n"
            "fn main() -> i32 { return 0; }\n";
        Pipeline q;
        pipeline_run_mem(&q, good);
        CHECK(q.st == NAME_OK);
        if (q.st != NAME_OK) { pipeline_free(&q); return; }
        CHECK(q.xsc == EXPR_CORE_OK);
        CHECK(q.xrn == 0);
        pipeline_free(&q);
    }
}

/* ---------------------------------------------------------------------------
 * 11. Boundary: global const/var initializers and enum member values are
 *     12a's sites (WP-M0-12a const_eval_check) - 13b1 must not duplicate
 * ------------------------------------------------------------------------- */

static void test_const_context_boundary_12a(void)
{
    static const char src[] =
        "module main;\n"
        "var n: i32 = 5;\n"
        "const G: i32 = n;\n"          /* global const init: 12a's site */
        "var v: i32 = n;\n"            /* global var init: 12a's site */
        "enum E: i32 { A = n }\n"      /* enum member value: 12a's site */
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    /* 12a reports the three non-const global/enum sites. */
    CHECK(p.esc == CONST_EVAL_DIAG_ERROR);
    CHECK(p.ern == 3);
    /* 13b1 walks only its own sites (extents/case labels/local consts) and
     * must NOT duplicate any of the three. */
    CHECK(p.xsc == EXPR_CORE_OK);
    CHECK(p.xrn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 12. Const-context: checked-arithmetic failure at an owned site is routed
 *     out (EvalFailureSite), never emitted as E0401 or a failure record
 * ------------------------------------------------------------------------- */

static void test_const_context_failure_routing(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  const C: i32 = 1 / 0;\n"
        "  return C;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_FAILURE);
    CHECK(p.xrn == 0);                 /* composition holds; no E0401 */
    CHECK(p.xfailn == 1);
    if (p.xfailn == 1) {
        CHECK(p.xfails[0].kind == EVAL_FAIL_DIV_ZERO);
        CHECK(p.xfails[0].node != NULL);
        CHECK(p.xfails[0].node->span &&
              p.xfails[0].node->span->start.offset ==
              marker_off(src, "1 / 0"));
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 13. Determinism: two runs produce byte-identical records
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "var n: i32 = 5;\n"
        "struct S { f: i32[n]; }\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case n: { const C: i32 = n; return C; }\n"
        "    default: { return 0; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    CHECK(p1.xsc == p2.xsc);
    CHECK(p1.xrn == p2.xrn);
    CHECK(p1.xrn == 3);
    if (p1.xrn != p2.xrn || p1.xrn != 3) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.xrn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.xrecs[i]));
        CHECK(diag_emit_record(&b2, p2.xrecs[i]));
        CHECK(b1.len == b2.len);
        CHECK(b1.len == 0 || memcmp(b1.data, b2.data, b1.len) == 0);
        diag_buf_free(&b1);
        diag_buf_free(&b2);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_precedence_model();
    fprintf(stderr, "after test_precedence_model\n");
    test_eval_order_binary();
    fprintf(stderr, "after test_eval_order_binary\n");
    test_eval_order_call_index_slice();
    fprintf(stderr, "after test_eval_order_call_index_slice\n");
    test_eval_order_assignment_compound();
    fprintf(stderr, "after test_eval_order_assignment_compound\n");
    test_eval_order_short_circuit_ternary();
    fprintf(stderr, "after test_eval_order_short_circuit_ternary\n");
    test_eval_order_member_sizeof_unary();
    fprintf(stderr, "after test_eval_order_member_sizeof_unary\n");
    test_eval_order_location_mode();
    fprintf(stderr, "after test_eval_order_location_mode\n");
    test_eval_order_literals_arrays_struct();
    fprintf(stderr, "after test_eval_order_literals_arrays_struct\n");
    test_const_context_array_extent();
    fprintf(stderr, "after test_const_context_array_extent\n");
    test_const_context_case_label();
    fprintf(stderr, "after test_const_context_case_label\n");
    test_const_context_local_const();
    fprintf(stderr, "after test_const_context_local_const\n");
    test_const_context_boundary_12a();
    fprintf(stderr, "after test_const_context_boundary_12a\n");
    test_const_context_failure_routing();
    fprintf(stderr, "after test_const_context_failure_routing\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");

    if (g_failures) {
        fprintf(stderr, "expr_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("expr_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
