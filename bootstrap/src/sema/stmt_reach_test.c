/* bootstrap/src/sema/stmt_reach_test.c
 *
 * WP-M0-13c2 reachability tests: the non-void path-without-return rule
 * (AIC-E0416) and the unreachable-statement rule (AIC-E0417), per spec
 * sec. 13.4-13.5 and the negative-corpus anchors (18-5-semantic-fn-no-
 * return, derived-semantic-fn-missing-return, derived-semantic-
 * unreachable, 18-5-semantic-case-no-terminate,
 * derived-semantic-break-outside-loop).
 *
 * The tests run the full pipeline through load -> lex -> parse ->
 * name_resolve -> completeness -> layout -> convert -> optype ->
 * const_eval_check (12a) -> rec_fail_emit (12b2) -> expr_core_check
 * (13b1) -> expr_ops_check (13b2) -> stmt_core_check (13c1) ->
 * stmt_reach_check (13c2) so the boundary tests can prove stmt_reach
 * emits only its own records:
 *   - AIC-E0412/E0413/E0414/E0420 (13c1) and AIC-E0415 (13d) are never
 *     produced here; E0412 + E0416 coexist for
 *     18-5-semantic-case-no-terminate;
 *   - derived-semantic-break-outside-loop produces only E0414 (13c1):
 *     an invalid break is ordinary flow for reachability, so the
 *     following `return 0;` stays reachable and no E0416/E0417 fires.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-sema-c2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/stmt_reach_test.c \
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
 *   ./bootstrap/stage0/msvc-sema-c2/stmt_reach_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-sema-c2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
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
 * -> stmt_core_check (13c1) -> stmt_reach_check (13c2).
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
 * 1. Valid programs: returns on every path, exhaustive switch,
 *    non-terminating while(true)/for(;;), void fall-off, noreturn tail
 * ------------------------------------------------------------------------- */

static void test_reach_ok(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.proc;\n"
        "import rt.trap;\n"
        "fn f(x: i32) -> i32 {\n"
        "  if (x > 0) { return 1; } else { return 0; }\n"
        "}\n"
        "fn classify(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { return 0; }\n"
        "    case 1: { return 1; }\n"
        "    default: { return 2; }\n"
        "  }\n"
        "}\n"
        "fn loop_ok() -> i32 {\n"
        "  while (true) { }\n"
        "}\n"
        "fn loop_ok2() -> i32 {\n"
        "  for (;;) { }\n"
        "}\n"
        "fn void_ok() -> void {\n"
        "  var x: i32 = 1;\n"
        "}\n"
        "fn tail_noret() -> i32 {\n"
        "  rt.proc.exit(0);\n"
        "}\n"
        "fn g(x: i32) -> i32 {\n"
        "  while (x < 3) {\n"
        "    x += 1;\n"
        "    if (x == 2) { continue; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_OK);
    CHECK(p.rrn2 == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 2. E0416 corpus anchors: 18-5-semantic-fn-no-return and
 *    derived-semantic-fn-missing-return
 * ------------------------------------------------------------------------- */

static void test_e0416_corpus(void)
{
    /* byte-identical to tests/negative/cases/18-5-semantic-fn-no-return
     * except a trailing EOF newline the corpus file lacks (spans
     * unaffected); E0412 does not fire (every case body terminates) */
    static const char src_switch[] =
        "module main;\n"
        "fn bad(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { var x: i32 = 1; break; }\n"
        "    case 1: { return 1; }\n"
        "    default: { return 0; }\n"
        "  }\n"
        "}\n";
    /* byte-identical to tests/negative/cases/derived-semantic-fn-missing-return
     * except a trailing EOF newline the corpus file lacks (spans
     * unaffected) */
    static const char src_if[] =
        "module main;\n"
        "fn missing(n: i32) -> i32 {\n"
        "  if (n > 0) { return n; }\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src_switch);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_OK);          /* no E0412: bodies terminate */
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    /* primary span = "fn bad" (the fn keyword through the name end) */
    check_fail_span(p.rrecs2[0], src_switch, "AIC-E0416", "fn bad", 6);
    check_message(p.rrecs2[0],
                  "non-void function 'bad' has a path without return");
    pipeline_free(&p);

    pipeline_run_mem(&p, src_if);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_if, "AIC-E0416", "fn missing", 10);
    check_message(p.rrecs2[0],
                  "non-void function 'missing' has a path without return");
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. E0416 derived cases: switch without default, break case, if without
 *    else, loop-only return, while(true) with break
 * ------------------------------------------------------------------------- */

static void test_e0416_derived(void)
{
    /* switch without default: a selector with no matching case reaches
     * the tail (sec. 13.2/13.4) */
    static const char src_nodefault[] =
        "module main;\n"
        "fn f(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { return 0; }\n"
        "    case 1: { return 1; }\n"
        "  }\n"
        "}\n";
    /* switch with default but a break case: the break path reaches the
     * statement after the switch -> tail without return */
    static const char src_breakcase[] =
        "module main;\n"
        "fn g(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { break; }\n"
        "    default: { return 1; }\n"
        "  }\n"
        "}\n";
    /* if without else and no terminating path */
    static const char src_if[] =
        "module main;\n"
        "fn h(n: i32) -> i32 {\n"
        "  if (n > 0) { return n; }\n"
        "  var x: i32 = 1;\n"
        "}\n";
    /* only return inside a runtime-cond loop: the loop may run zero
     * times (sec. 13.5 "a function whose only return is inside a loop
     * must still have a terminating path") */
    static const char src_loopreturn[] =
        "module main;\n"
        "fn k(n: i32) -> i32 {\n"
        "  while (n < 3) { return 1; }\n"
        "}\n";
    /* while(true) WITH a break: the break path reaches the tail (sec.
     * 13.5 "with no break is considered non-terminating ... if it
     * cannot reach the tail" - a break makes it reachable) */
    static const char src_wb[] =
        "module main;\n"
        "fn wb(n: i32) -> i32 {\n"
        "  while (true) {\n"
        "    if (n > 0) { break; }\n"
        "  }\n"
        "}\n";
    /* empty non-void body: the only path reaches the tail */
    static const char src_empty[] =
        "module main;\n"
        "fn e() -> i32 {\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src_nodefault);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_nodefault, "AIC-E0416", "fn f", 4);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_breakcase);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_OK);          /* break case is a valid terminator */
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_breakcase, "AIC-E0416", "fn g", 4);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_if);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_if, "AIC-E0416", "fn h", 4);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_loopreturn);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_loopreturn, "AIC-E0416", "fn k", 4);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_wb);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_wb, "AIC-E0416", "fn wb", 5);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_empty);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_empty, "AIC-E0416", "fn e", 4);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. E0417 corpus anchor: derived-semantic-unreachable
 * ------------------------------------------------------------------------- */

static void test_e0417_corpus(void)
{
    /* byte-identical to tests/negative/cases/derived-semantic-unreachable
     * except a trailing EOF newline the corpus file lacks (spans
     * unaffected); exactly ONE record despite two unreachable
     * statements (one root-cause record per block) */
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  return 0;\n"
        "  var x: i32 = 5;\n"
        "  return x;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src, "AIC-E0417", "var x: i32 = 5;", 15);
    check_message(p.rrecs2[0], "unreachable statement");
    /* the reachable path returns -> no E0416 */
    CHECK(strcmp(p.rrecs2[0]->code, "AIC-E0416") != 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. E0417 derived cases: after break/continue in loop bodies, after a
 *    noreturn call; nested-block scoping; no flag after an always-
 *    returning if/switch (conservative, sec. 13.5 E0417 list)
 * ------------------------------------------------------------------------- */

static void test_e0417_derived(void)
{
    static const char src_loopbreak[] =
        "module main;\n"
        "fn f(x: i32) -> i32 {\n"
        "  while (x < 3) {\n"
        "    break;\n"
        "    x += 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_loopcontinue[] =
        "module main;\n"
        "fn g(x: i32) -> i32 {\n"
        "  for (var i: i32 = 0; i < 3; i += 1) {\n"
        "    continue;\n"
        "    x += 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_noret[] =
        "module main;\n"
        "import rt.proc;\n"
        "fn h() -> i32 {\n"
        "  rt.proc.exit(0);\n"
        "  var x: i32 = 1;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* terminator inside a nested block does not flag the outer block's
     * following statement (same-block rule) */
    static const char src_nested[] =
        "module main;\n"
        "fn k(x: i32) -> i32 {\n"
        "  if (x > 0) { return 1; }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* conservative: an if whose branches always return does NOT make the
     * following statement unreachable for E0417 (only the four listed
     * terminator forms count); E0416 also does not fire because no path
     * reaches the tail */
    static const char src_alwaysreturn[] =
        "module main;\n"
        "fn m(x: i32) -> i32 {\n"
        "  if (x > 0) { return 1; } else { return 2; }\n"
        "  var y: i32 = 3;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_loopbreak);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_loopbreak, "AIC-E0417", "x += 1;", 7);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_loopcontinue);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_loopcontinue, "AIC-E0417",
                    "x += 1;", 7);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_noret);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_noret, "AIC-E0417",
                    "var x: i32 = 1;", 15);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_nested);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_OK);
    CHECK(p.rrn2 == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_alwaysreturn);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_OK);
    CHECK(p.rrn2 == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. Boundaries: stmt_reach emits only its own records
 * ------------------------------------------------------------------------- */

static void test_boundaries(void)
{
    /* 18-5-semantic-case-no-terminate: E0412 (13c1) + E0416 (13c2)
     * coexist; stmt_reach must not produce E0412 */
    static const char src_case_no_terminate[] =
        "module main;\n"
        "fn bad(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { var x: i32 = 1; }\n"
        "    case 1: { return 1; }\n"
        "  }\n"
        "}\n";
    /* derived-semantic-break-outside-loop: ONLY E0414 (13c1); an invalid
     * break is ordinary flow for reachability -> no E0416/E0417 */
    static const char src_break_outside[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  break;\n"
        "  return 0;\n"
        "}\n";
    /* duplicate switch case (E0413, 13c1) with every body returning:
     * stmt_reach must not add E0416 (no path without return) */
    static const char src_dup_case[] =
        "module main;\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case 0: { return 0; }\n"
        "    case 0: { return 1; }\n"
        "    default: { return 2; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_case_no_terminate);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);  /* E0412 from 13c1 */
    CHECK(p.srn == 1);
    check_fail_span(p.srecs[0], src_case_no_terminate, "AIC-E0412",
                    "case 0", 6);
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR); /* E0416 from 13c2 */
    CHECK(p.rrn2 == 1);
    if (p.rrn2 != 1) { pipeline_free(&p); return; }
    check_fail_span(p.rrecs2[0], src_case_no_terminate, "AIC-E0416",
                    "fn bad", 6);
    /* stmt_reach never emits 13c1's codes */
    CHECK(strcmp(p.rrecs2[0]->code, "AIC-E0412") != 0);
    CHECK(strcmp(p.rrecs2[0]->code, "AIC-E0413") != 0);
    CHECK(strcmp(p.rrecs2[0]->code, "AIC-E0414") != 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_break_outside);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);  /* E0414 from 13c1 */
    CHECK(p.srn == 1);
    check_fail_span(p.srecs[0], src_break_outside, "AIC-E0414",
                    "break;", 6);
    CHECK(p.rsc2 == STMT_REACH_OK);        /* reachability silent */
    CHECK(p.rrn2 == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_dup_case);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);  /* E0413 from 13c1 */
    CHECK(p.srn == 1);
    CHECK(p.rsc2 == STMT_REACH_OK);        /* no E0416: every path returns */
    CHECK(p.rrn2 == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 7. Determinism: two runs produce byte-identical records
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case 0: { break; }\n"
        "    default: { return 0; }\n"
        "  }\n"
        "}\n"
        "fn g(n: i32) -> i32 {\n"
        "  if (n > 0) { return n; }\n"
        "  var z: i32 = 1;\n"
        "}\n"
        "fn h() -> i32 {\n"
        "  return 0;\n"
        "  var a: i32 = 1;\n"
        "  var b: i32 = 2;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    if (p1.st != NAME_OK) { pipeline_free(&p1); pipeline_free(&p2); return; }
    CHECK(p1.rsc2 == p2.rsc2);
    CHECK(p1.rrn2 == p2.rrn2);
    CHECK(p1.rrn2 == 3);
    /* f: E0416 (break case reaches the tail); g: E0416 (if without
     * else); h: E0417 (var a after return) - sorted by span offset:
     * fn f, fn g, var a */
    if (p1.rrn2 != p2.rrn2 || p1.rrn2 != 3) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.rrn2; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.rrecs2[i]));
        CHECK(diag_emit_record(&b2, p2.rrecs2[i]));
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
    test_reach_ok();
    fprintf(stderr, "after test_reach_ok\n");
    test_e0416_corpus();
    fprintf(stderr, "after test_e0416_corpus\n");
    test_e0416_derived();
    fprintf(stderr, "after test_e0416_derived\n");
    test_e0417_corpus();
    fprintf(stderr, "after test_e0417_corpus\n");
    test_e0417_derived();
    fprintf(stderr, "after test_e0417_derived\n");
    test_boundaries();
    fprintf(stderr, "after test_boundaries\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");

    if (g_failures) {
        fprintf(stderr, "stmt_reach_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("stmt_reach_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
