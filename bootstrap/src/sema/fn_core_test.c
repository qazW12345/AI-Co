/* bootstrap/src/sema/fn_core_test.c
 *
 * WP-M0-13d1 function return rules tests: the return-value-mismatch rule
 * (AIC-E0415, spec sec. 13.4) per DIAGNOSTIC-CONTRACT sec. 11.5 and the
 * negative-corpus anchor (derived-semantic-return-value-in-void).
 *
 * The tests run the full pipeline through load -> lex -> parse ->
 * name_resolve -> completeness -> layout -> convert -> optype ->
 * const_eval_check (12a) -> rec_fail_emit (12b2) -> expr_core_check
 * (13b1) -> expr_ops_check (13b2) -> stmt_core_check (13c1) ->
 * stmt_reach_check (13c2) -> fn_core_check (13d1) so the boundary tests
 * can prove fn_core emits only its own record:
 *   - AIC-E0416/E0417 (13c2), AIC-E0412/E0413/E0414/E0420 (13c1), and
 *     AIC-E0418/reserved names (13d2) are never produced here;
 *   - return-value TYPE compatibility is owned by the 11c conversion
 *     check: a type-mismatched `return expr;` produces a convert-phase
 *     record and the pipeline stops before fn_core (fn_core checks only
 *     presence/absence of the value against the return type's void-ness).
 *
 * Corpus anchor note: tests/negative/cases/derived-semantic-return-value-
 * in-void/input.ai as committed (commit 1953277) includes the mandatory
 * `-> void` return type (`fn noop() -> void {`, spec sec. 5.2 fn_decl),
 * so the case reaches the semantic stage and pins AIC-E0415 at line 3,
 * cols 3..12 (byte offsets 35..44) - the whole `return 5;` statement -
 * matching the corpus expected record's line/col/offset. The embedded
 * spelling below matches that committed input.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-sema-d1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/fn_core_test.c \
 *     bootstrap/src/sema/fn_core.c \
 *     bootstrap/src/sema/stmt_reach.c \
 *     bootstrap/src/sema/stmt_core.c \
 *     bootstrap/src/sema/expr_ops.c \
 *     bootstrap/src/sema/expr_core.c \
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
 *   ./bootstrap/stage0/msvc-sema-d1/fn_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-sema-d1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "fn_core.h"
#include "stmt_reach.h"
#include "stmt_core.h"
#include "expr_core.h"
#include "expr_ops.h"
#include "../const/eval_fail_rec.h"

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
 * Full pipeline: load -> lex -> parse -> name_resolve -> completeness ->
 * layout -> convert -> optype -> const_eval_check (12a) ->
 * rec_fail_emit (12b2) -> expr_core_check (13b1) -> expr_ops_check (13b2)
 * -> stmt_core_check (13c1) -> stmt_reach_check (13c2) -> fn_core_check
 * (13d1).
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
    DiagRecord **lrecs;     /* layout records */
    size_t lrn;
    LayoutStatus lst;
    DiagRecord **crecs;     /* convert records */
    size_t crn;
    ConvertStatus cst;
    DiagRecord **orecs;     /* optype records */
    size_t orn;
    OptypeStatus ost;
    DiagRecord **erecs;     /* 12a const records */
    size_t ern;
    EvalFailureSite *efails;
    size_t efailn;
    ConstEvalStatus esc;
    DiagRecord **rrecs;     /* 12b2 records */
    size_t rrn;
    RecFailStatus rsc;
    DiagRecord **xrecs;     /* 13b1 records */
    size_t xrn;
    EvalFailureSite *xfails;
    size_t xfailn;
    ExprCoreStatus xsc;
    DiagRecord **opsrecs;   /* 13b2 records */
    size_t opsrn;
    ExprOpsStatus osc;
    DiagRecord **srecs;     /* 13c1 records (AIC-E0412..E0414/E0420) */
    size_t srn;
    StmtCoreStatus ssc;
    DiagRecord **rrecs2;    /* 13c2 records (AIC-E0416/E0417) */
    size_t rrn2;
    StmtReachStatus rsc2;
    DiagRecord **frecs;     /* 13d1 records (AIC-E0415) */
    size_t frn;
    FnCoreStatus fsc;
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
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR &&
        p->lst != LAYOUT_UNEVALUABLE) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    if (p->ost == OPTYPE_DIAG_ERROR) return;
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
    p->rsc = rec_fail_emit(p->result, p->build, &p->rrecs, &p->rrn);
    p->xsc = expr_core_check(p->result, p->build, &p->xrecs, &p->xrn,
                             &p->xfails, &p->xfailn);
    p->osc = expr_ops_check(p->result, p->build, &p->opsrecs, &p->opsrn);
    p->ssc = stmt_core_check(p->result, p->build, &p->srecs, &p->srn);
    p->rsc2 = stmt_reach_check(p->result, p->build, &p->rrecs2, &p->rrn2);
    p->fsc = fn_core_check(p->result, p->build, &p->frecs, &p->frn);
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
    types_records_free(p->xrecs, p->xrn);
    free(p->xfails);
    types_records_free(p->opsrecs, p->opsrn);
    types_records_free(p->srecs, p->srn);
    types_records_free(p->rrecs2, p->rrn2);
    types_records_free(p->frecs, p->frn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* Check a record's code, phase/severity/recovery, and that its primary
 * span starts exactly at `marker` (and, when len > 0, spans exactly
 * `len` bytes). */
static void check_fail_span(const DiagRecord *r, const char *src,
                            const char *code, const char *marker,
                            int64_t len)
{
    const char *hit;
    int64_t off;
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->code && strcmp(r->code, code) == 0);
    CHECK(r->severity && strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(r->phase && strcmp(r->phase, DIAG_PHASE_SEMANTIC) == 0);
    CHECK(r->recovery &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    CHECK(r->message != NULL && r->message[0] != '\0');
    CHECK(r->primary_span != NULL);
    if (!r->primary_span) return;
    hit = strstr(src, marker);
    CHECK(hit != NULL);
    if (!hit) return;
    off = (int64_t)(hit - src);
    CHECK(r->primary_span->start.offset == off);
    if (len > 0) CHECK(r->primary_span->end.offset == off + len);
}

/* Check a record's message equals the exact expected text. */
static void check_message(const DiagRecord *r, const char *expect)
{
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->message && strcmp(r->message, expect) == 0);
}

/* ---------------------------------------------------------------------------
 * 1. Valid programs: void fn with bare return / block end, non-void fn
 *    with values on every path, returns nested in loops/switches
 * ------------------------------------------------------------------------- */

static void test_returns_ok(void)
{
    static const char src[] =
        "module main;\n"
        "fn void_bare() -> void {\n"
        "  return;\n"
        "}\n"
        "fn void_blockend() -> void {\n"
        "  var x: i32 = 1;\n"
        "}\n"
        "fn nonvoid(x: i32) -> i32 {\n"
        "  return x;\n"
        "}\n"
        "fn nonvoid_if(x: i32) -> i32 {\n"
        "  if (x > 0) { return 1; } else { return 0; }\n"
        "}\n"
        "fn nonvoid_switch(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { return 0; }\n"
        "    default: { return 1; }\n"
        "  }\n"
        "}\n"
        "fn nonvoid_loop(x: i32) -> i32 {\n"
        "  while (x < 3) {\n"
        "    x += 1;\n"
        "    if (x == 2) { return x; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_OK);
    CHECK(p.frn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 2. E0415 corpus anchor: derived-semantic-return-value-in-void (value in
 *    a void function). The committed corpus input has `fn noop() -> void {`
 *    (commit 1953277, see the header note); the embedded spelling below
 *    matches it and pins the record at line 3, cols 3..12 (byte offsets
 *    35..44) = the whole `return 5;` statement.
 * ------------------------------------------------------------------------- */

static void test_e0415_value_in_void(void)
{
    static const char src[] =
        "module main;\n"
        "fn noop() -> void {\n"
        "  return 5;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 1);
    if (p.frn != 1) { pipeline_free(&p); return; }
    /* primary span = the whole return statement "return 5;" */
    check_fail_span(p.frecs[0], src, "AIC-E0415", "return 5;", 9);
    check_message(p.frecs[0], "return value in void function");
    /* the record is on line 3, cols 3..12 - the corpus expected record's
     * line/col shape */
    CHECK(p.frecs[0]->primary_span->start.line == 3);
    CHECK(p.frecs[0]->primary_span->start.col == 3);
    CHECK(p.frecs[0]->primary_span->end.line == 3);
    CHECK(p.frecs[0]->primary_span->end.col == 12);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. E0415 derived: bare `return;` in a non-void function (missing value)
 * ------------------------------------------------------------------------- */

static void test_e0415_missing_nonvoid(void)
{
    static const char src_bare[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  return;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* a bare return among value returns: only the bare one is flagged */
    static const char src_mixed[] =
        "module main;\n"
        "fn g(x: i32) -> i32 {\n"
        "  if (x > 0) { return x; }\n"
        "  return;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* void fn with a bare return is valid */
    static const char src_void_ok[] =
        "module main;\n"
        "fn h() -> void {\n"
        "  return;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_bare);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 1);
    if (p.frn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.frecs[0], src_bare, "AIC-E0415", "return;", 7);
    check_message(p.frecs[0], "return value missing in non-void function");
    pipeline_free(&p);

    pipeline_run_mem(&p, src_mixed);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 1);
    if (p.frn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.frecs[0], src_mixed, "AIC-E0415", "return;", 7);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_void_ok);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_OK);
    CHECK(p.frn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. E0415 derived: multiple offending returns -> one record per return;
 *    returns nested in loops/switch bodies are checked
 * ------------------------------------------------------------------------- */

static void test_e0415_multiple_and_nested(void)
{
    static const char src_multiple[] =
        "module main;\n"
        "fn f(x: i32) -> void {\n"
        "  return x;\n"
        "  return 1;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_nested[] =
        "module main;\n"
        "fn g(n: i32) -> void {\n"
        "  while (n < 3) {\n"
        "    n += 1;\n"
        "    if (n == 2) { return n; }\n"
        "  }\n"
        "  switch (n) {\n"
        "    case 0: { return 0; }\n"
        "    default: { return 1; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_multiple);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    /* E0417 (stmt_reach) also fires for the statement after the first
     * return; fn_core emits one E0415 per offending return */
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 2);
    if (p.frn != 2) { pipeline_free(&p); return; }
    check_fail_span(p.frecs[0], src_multiple, "AIC-E0415", "return x;", 9);
    check_fail_span(p.frecs[1], src_multiple, "AIC-E0415", "return 1;", 9);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_nested);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 3);
    if (p.frn != 3) { pipeline_free(&p); return; }
    check_fail_span(p.frecs[0], src_nested, "AIC-E0415", "return n;", 9);
    check_fail_span(p.frecs[1], src_nested, "AIC-E0415", "return 0;", 9);
    check_fail_span(p.frecs[2], src_nested, "AIC-E0415", "return 1;", 9);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. Boundaries: fn_core emits only AIC-E0415; E0416/E0417 stay in
 *    stmt_reach (13c2); type compatibility stays in convert (11c)
 * ------------------------------------------------------------------------- */

static void test_boundaries(void)
{
    /* E0416 corpus anchors (13c2) must pass unchanged with fn_core
     * silent: 18-5-semantic-fn-no-return */
    static const char src_e0416_switch[] =
        "module main;\n"
        "fn bad(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { var x: i32 = 1; break; }\n"
        "    case 1: { return 1; }\n"
        "    default: { return 0; }\n"
        "  }\n"
        "}\n";
    /* E0416 corpus anchor (13c2): derived-semantic-fn-missing-return */
    static const char src_e0416_if[] =
        "module main;\n"
        "fn missing(n: i32) -> i32 {\n"
        "  if (n > 0) { return n; }\n"
        "}\n";
    /* E0417 (13c2) fires; fn_core stays silent: unreachable statement
     * after a bare return in a non-void fn -> E0417 + E0415 both fire
     * (distinct rules, distinct spans) */
    static const char src_e0417_e0415[] =
        "module main;\n"
        "fn h() -> i32 {\n"
        "  return;\n"
        "  var x: i32 = 1;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* value-in-void with no path issue: fn_core's record coexists with
     * nothing from 13c2 (a void fn falls off the end validly) */
    static const char src_void_value[] =
        "module main;\n"
        "fn k() -> void {\n"
        "  return 1;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* return-value TYPE mismatch is owned by the 11c convert check: the
     * pipeline stops with a convert-phase record before fn_core runs */
    static const char src_type_mismatch[] =
        "module main;\n"
        "fn t() -> i32 {\n"
        "  return \"str\";\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_e0416_switch);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);  /* E0416 from 13c2 */
    CHECK(p.rrn2 == 1);
    check_fail_span(p.rrecs2[0], src_e0416_switch, "AIC-E0416", "fn bad", 6);
    CHECK(p.fsc == FN_CORE_OK);              /* fn_core silent */
    CHECK(p.frn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_e0416_if);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);  /* E0416 from 13c2 */
    CHECK(p.rrn2 == 1);
    check_fail_span(p.rrecs2[0], src_e0416_if, "AIC-E0416", "fn missing", 10);
    CHECK(p.fsc == FN_CORE_OK);              /* fn_core silent */
    CHECK(p.frn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_e0417_e0415);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);  /* E0417 from 13c2 */
    CHECK(p.rrn2 == 1);
    check_fail_span(p.rrecs2[0], src_e0417_e0415, "AIC-E0417",
                    "var x: i32 = 1;", 15);
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);      /* E0415 from 13d1 */
    CHECK(p.frn == 1);
    check_fail_span(p.frecs[0], src_e0417_e0415, "AIC-E0415", "return;", 7);
    /* fn_core never emits 13c1/13c2 codes */
    CHECK(strcmp(p.frecs[0]->code, "AIC-E0412") != 0);
    CHECK(strcmp(p.frecs[0]->code, "AIC-E0413") != 0);
    CHECK(strcmp(p.frecs[0]->code, "AIC-E0414") != 0);
    CHECK(strcmp(p.frecs[0]->code, "AIC-E0416") != 0);
    CHECK(strcmp(p.frecs[0]->code, "AIC-E0417") != 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_void_value);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 1);
    check_fail_span(p.frecs[0], src_void_value, "AIC-E0415", "return 1;", 9);
    pipeline_free(&p);

    /* type mismatch: convert owns it; fn_core is never reached */
    pipeline_run_mem(&p, src_type_mismatch);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn >= 1);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. Determinism: two runs produce byte-identical records
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> void {\n"
        "  return 1;\n"
        "}\n"
        "fn g() -> i32 {\n"
        "  return;\n"
        "}\n"
        "fn h(x: i32) -> i32 {\n"
        "  if (x > 0) { return x; }\n"
        "  return;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    if (p1.st != NAME_OK) { pipeline_free(&p1); pipeline_free(&p2); return; }
    CHECK(p1.fsc == p2.fsc);
    CHECK(p1.frn == p2.frn);
    CHECK(p1.frn == 3);
    /* f: E0415 (return 1 in void); g: E0415 (bare return in non-void);
     * h: E0415 (bare return in non-void) - sorted by span offset:
     * return 1;, return;, return; */
    if (p1.frn != p2.frn || p1.frn != 3) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.frn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.frecs[i]));
        CHECK(diag_emit_record(&b2, p2.frecs[i]));
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
    test_returns_ok();
    fprintf(stderr, "after test_returns_ok\n");
    test_e0415_value_in_void();
    fprintf(stderr, "after test_e0415_value_in_void\n");
    test_e0415_missing_nonvoid();
    fprintf(stderr, "after test_e0415_missing_nonvoid\n");
    test_e0415_multiple_and_nested();
    fprintf(stderr, "after test_e0415_multiple_and_nested\n");
    test_boundaries();
    fprintf(stderr, "after test_boundaries\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");

    if (g_failures) {
        fprintf(stderr, "fn_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("fn_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
