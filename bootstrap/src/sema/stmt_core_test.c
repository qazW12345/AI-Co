/* bootstrap/src/sema/stmt_core_test.c
 *
 * WP-M0-13c1 statement rules and switch/break/continue tests: the
 * switch no-fall-through rule (AIC-E0412), the duplicate-case-value
 * rule (AIC-E0413), and the break/continue placement rule
 * (AIC-E0414), per spec sec. 13.2-13.3 and the negative-corpus
 * anchors (18-5-semantic-case-no-terminate,
 * derived-semantic-duplicate-case, derived-semantic-break-outside-loop).
 *
 * The tests run the full pipeline through load -> lex -> parse ->
 * name_resolve -> completeness -> layout -> convert -> optype ->
 * const_eval_check (12a) -> rec_fail_emit (12b2) -> expr_core_check
 * (13b1) -> expr_ops_check (13b2) -> stmt_core_check (13c1) so the
 * boundary tests can prove stmt_core emits only its own records:
 *   - AIC-E0416/E0417 (reachability, WP-M0-13c2) are never produced;
 *   - AIC-E0401 (13b1) and AIC-E0405..E0411 (13b2) at case labels are
 *     not duplicated by E0413 (a non-const or failing label is skipped
 *     in duplicate detection);
 *   - expression/declaration/return rules (13a/13b/13d) produce no
 *     records here.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-sema-c1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/stmt_core_test.c \
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
 *   ./bootstrap/stage0/msvc-sema-c1/stmt_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-sema-c1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
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
 * -> stmt_core_check (13c1).
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
    DiagRecord **srecs;     /* 13c1 records (AIC-E0412..E0414) */
    size_t srn;
    StmtCoreStatus ssc;
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
    /* LAYOUT_UNEVALUABLE is a routing signal, not a rejection: the
     * const stages own the extent evaluation (layout.h boundary note). */
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
 * 1. Valid terminators: break, return, continue inside a loop, noreturn
 *    calls - no E0412/E0414 on a clean program
 * ------------------------------------------------------------------------- */

static void test_terminators_ok(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.proc;\n"
        "import rt.trap;\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case 0: { break; }\n"
        "    case 1: { return 1; }\n"
        "    default: { rt.proc.exit(0); }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn g() -> i32 {\n"
        "  var i: i32 = 0;\n"
        "  while (i < 3) {\n"
        "    switch (i) {\n"
        "      case 0: { continue; }\n"
        "      default: { break; }\n"
        "    }\n"
        "    i += 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_OK);
    CHECK(p.srn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 2. E0412: case body without a terminating statement (corpus anchor
 *    18-5-semantic-case-no-terminate) plus empty block and default
 * ------------------------------------------------------------------------- */

static void test_e0412_corpus(void)
{
    /* byte-identical to tests/negative/cases/18-5-semantic-case-no-terminate
     * except a trailing EOF newline the corpus file lacks (spans
     * unaffected); the E0412 record; E0416 belongs to 13c2 and is not
     * produced here */
    static const char src[] =
        "module main;\n"
        "fn bad(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { var x: i32 = 1; }\n"
        "    case 1: { return 1; }\n"
        "  }\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 1);
    if (p.srn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.srecs[0], src, "AIC-E0412", "case 0", 6);
    check_message(p.srecs[0],
                  "switch case 0 body lacks a terminating statement; "
                  "fall-through is prohibited");
    /* the corpus also expects E0416 but that is 13c2's record - stmt_core
     * must not produce it */
    CHECK(p.srecs[0]->code &&
          strcmp(p.srecs[0]->code, "AIC-E0416") != 0);
    pipeline_free(&p);
}

static void test_e0412_more(void)
{
    static const char src[] =
        "module main;\n"
        "fn h() -> i32 { return 0; }\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case 0: { }\n"                 /* empty block */
        "    case 1: { h(); }\n"            /* non-noreturn call */
        "    default: { var y: i32 = 2; }\n" /* default without terminator */
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 3);
    if (p.srn != 3) { pipeline_free(&p); return; }
    /* records sorted by span offset: case 0, case 1, default */
    check_fail_span(p.srecs[0], src, "AIC-E0412", "case 0", 6);
    check_message(p.srecs[0],
                  "switch case 0 body lacks a terminating statement; "
                  "fall-through is prohibited");
    check_fail_span(p.srecs[1], src, "AIC-E0412", "case 1", 6);
    check_message(p.srecs[1],
                  "switch case 1 body lacks a terminating statement; "
                  "fall-through is prohibited");
    check_fail_span(p.srecs[2], src, "AIC-E0412", "default", 7);
    check_message(p.srecs[2],
                  "switch default body lacks a terminating statement; "
                  "fall-through is prohibited");
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. E0413: duplicate case values (corpus anchor
 *    derived-semantic-duplicate-case) and non-duplicates
 * ------------------------------------------------------------------------- */

static void test_e0413_corpus(void)
{
    /* byte-identical to tests/negative/cases/derived-semantic-duplicate-case
     * except a trailing EOF newline the corpus file lacks (spans
     * unaffected) */
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = 1;\n"
        "  switch (x) {\n"
        "    case 0: { return 0; }\n"
        "    case 0: { return 1; }\n"
        "    default: { return 2; }\n"
        "  }\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 1);
    if (p.srn != 1) { pipeline_free(&p); return; }
    /* primary span = the FIRST case label of the duplicated pair */
    check_fail_span(p.srecs[0], src, "AIC-E0413", "case 0", 6);
    check_message(p.srecs[0], "duplicate switch case value: 0");
    /* no E0412: both bodies terminate */
    CHECK(strcmp(p.srecs[0]->code, "AIC-E0412") != 0);
    pipeline_free(&p);
}

static void test_e0413_nonduplicate_and_enum(void)
{
    static const char src[] =
        "module main;\n"
        "enum E: i32 { A = 0, B = 1 }\n"
        "fn f(x: i32, e: E) -> i32 {\n"
        "  switch (x) {\n"
        "    case 0: { break; }\n"
        "    case 1: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  switch (e) {\n"
        "    case E.A: { break; }\n"
        "    case E.B: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_enum_dup[] =
        "module main;\n"
        "enum E: i32 { A = 0, B = 1 }\n"
        "fn f(e: E) -> i32 {\n"
        "  switch (e) {\n"
        "    case E.A: { break; }\n"
        "    case E.A: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_OK);
    CHECK(p.srn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_enum_dup);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 1);
    if (p.srn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.srecs[0], src_enum_dup, "AIC-E0413",
                    "case E.A", 8);
    check_message(p.srecs[0], "duplicate switch case value: 0");
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. E0414: break/continue placement (corpus anchor
 *    derived-semantic-break-outside-loop) and valid placements
 * ------------------------------------------------------------------------- */

static void test_e0414_corpus(void)
{
    /* byte-identical to tests/negative/cases/derived-semantic-break-outside-loop
     * except a trailing EOF newline the corpus file lacks (spans
     * unaffected) */
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  break;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 1);
    if (p.srn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.srecs[0], src, "AIC-E0414", "break;", 6);
    check_message(p.srecs[0], "break outside loop or switch");
    pipeline_free(&p);
}

static void test_e0414_continue_and_switch(void)
{
    static const char src_continue[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  continue;\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* continue inside a switch that is NOT inside a loop: both E0412
     * (continue is not a valid terminator outside a loop) and E0414
     * (continue outside loop) - documented decision */
    static const char src_switch[] =
        "module main;\n"
        "fn g(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case 0: { continue; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_ok[] =
        "module main;\n"
        "fn h(x: i32) -> i32 {\n"
        "  var i: i32 = 0;\n"
        "  while (i < 3) {\n"
        "    i += 1;\n"
        "    if (i == 2) { continue; }\n"
        "    if (i == 3) { break; }\n"
        "  }\n"
        "  for (var j: i32 = 0; j < 3; j += 1) {\n"
        "    if (j == 1) { continue; }\n"
        "    break;\n"
        "  }\n"
        "  switch (x) {\n"
        "    case 0: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn k() -> i32 {\n"
        "  var i: i32 = 0;\n"
        "  while (i < 3) {\n"
        "    switch (i) {\n"
        "      case 0: { continue; }\n"
        "      default: { break; }\n"
        "    }\n"
        "    i += 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_continue);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 1);
    if (p.srn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.srecs[0], src_continue, "AIC-E0414", "continue;", 9);
    check_message(p.srecs[0], "continue outside loop");
    pipeline_free(&p);

    pipeline_run_mem(&p, src_switch);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);
    CHECK(p.srn == 2);
    if (p.srn != 2) { pipeline_free(&p); return; }
    /* sorted by offset: the E0412 at the case label, then the E0414 at
     * the continue statement */
    check_fail_span(p.srecs[0], src_switch, "AIC-E0412", "case 0", 6);
    check_message(p.srecs[0],
                  "switch case 0 body lacks a terminating statement; "
                  "fall-through is prohibited");
    check_fail_span(p.srecs[1], src_switch, "AIC-E0414", "continue;", 9);
    check_message(p.srecs[1], "continue outside loop");
    pipeline_free(&p);

    pipeline_run_mem(&p, src_ok);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_OK);
    CHECK(p.srn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. Boundaries: stmt_core emits only its own records
 * ------------------------------------------------------------------------- */

static void test_boundaries(void)
{
    /* non-const case label: 13b1 emits AIC-E0401; stmt_core must not
     * emit E0413 (the label is skipped) and the body terminates */
    static const char src_nonconst[] =
        "module main;\n"
        "fn f(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case n: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* const-eval failure at a case label: 13b2 emits AIC-E0406; stmt_core
     * must not emit E0413 */
    static const char src_fail[] =
        "module main;\n"
        "fn g(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 1 / 0: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* reachability (E0416/E0417) is 13c2's record: a non-void function
     * without a return produces NO stmt_core record */
    static const char src_reach[] =
        "module main;\n"
        "fn noret(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src_nonconst);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_DIAG_ERROR);   /* 13b1 E0401 */
    CHECK(p.xrn == 1);
    CHECK(p.osc == EXPR_OPS_OK);
    CHECK(p.ssc == STMT_CORE_OK);
    CHECK(p.srn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_fail);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_FAILURE);      /* 13b1 routes the failure */
    CHECK(p.xrn == 0);
    CHECK(p.osc == EXPR_OPS_DIAG_ERROR);    /* 13b2 emits E0406 */
    CHECK(p.opsrn == 1);
    CHECK(p.ssc == STMT_CORE_OK);
    CHECK(p.srn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_reach);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.ssc == STMT_CORE_OK);
    CHECK(p.srn == 0);                      /* E0416 is 13c2's, not here */
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. EVAL_UNSUPPORTED case label: skipped, walk continues (MIN-1 fix)
 * ------------------------------------------------------------------------- */

static void test_eval_unsupported_label_skip(void)
{
    /* An EVAL_UNSUPPORTED case label (`&C` where C is a const;
     * address-of-const is AIC-E0402, owned by a later package, so the
     * evaluator returns EVAL_UNSUPPORTED with no record - eval_core.h
     * boundary note) must be SKIPPED in duplicate detection and must
     * NOT abort the build walk: a later switch in the same build with
     * a genuine E0412 violation still produces its record (header
     * contract: labels that do not evaluate are skipped; the
     * const-context stages own those records). */
    static const char src[] =
        "module main;\n"
        "const C: i32 = 1;\n"
        "fn f(x: i32) -> i32 {\n"
        "  switch (x) {\n"
        "    case &C: { break; }\n"          /* EVAL_UNSUPPORTED label */
        "    default: { break; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "fn g(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { var q: i32 = 1; }\n"  /* E0412: no terminator */
        "    case 1: { return 1; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    /* the `&C` label is EVAL_UNSUPPORTED in the const stages too:
     * 12a does not own case labels, 13b1/13b2 mark defensive with no
     * record - so stmt_core is the only stage that sees the site */
    CHECK(p.esc == CONST_EVAL_OK);
    CHECK(p.xsc == EXPR_CORE_UNSUPPORTED);
    CHECK(p.xrn == 0);
    CHECK(p.osc == EXPR_OPS_UNSUPPORTED);
    CHECK(p.opsrn == 0);
    /* stmt_core skips the label and CONTINUES: g's E0412 is emitted */
    CHECK(p.ssc == STMT_CORE_DIAG_ERROR);   /* NOT STMT_CORE_UNSUPPORTED */
    CHECK(p.srn == 1);
    if (p.srn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.srecs[0], src, "AIC-E0412", "case 0", 6);
    check_message(p.srecs[0],
                  "switch case 0 body lacks a terminating statement; "
                  "fall-through is prohibited");
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
        "    case 0: { var a: i32 = 1; }\n"
        "    case 1: { break; }\n"
        "    case 1: { break; }\n"
        "    default: { }\n"
        "  }\n"
        "  break;\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    if (p1.st != NAME_OK) { pipeline_free(&p1); pipeline_free(&p2); return; }
    CHECK(p1.ssc == p2.ssc);
    CHECK(p1.srn == p2.srn);
    CHECK(p1.srn == 4);   /* E0412 x2 (case 0, default) + E0413 (case 1) + E0414 (break) */
    if (p1.srn != p2.srn || p1.srn != 4) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.srn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.srecs[i]));
        CHECK(diag_emit_record(&b2, p2.srecs[i]));
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
    test_terminators_ok();
    fprintf(stderr, "after test_terminators_ok\n");
    test_e0412_corpus();
    fprintf(stderr, "after test_e0412_corpus\n");
    test_e0412_more();
    fprintf(stderr, "after test_e0412_more\n");
    test_e0413_corpus();
    fprintf(stderr, "after test_e0413_corpus\n");
    test_e0413_nonduplicate_and_enum();
    fprintf(stderr, "after test_e0413_nonduplicate_and_enum\n");
    test_e0414_corpus();
    fprintf(stderr, "after test_e0414_corpus\n");
    test_e0414_continue_and_switch();
    fprintf(stderr, "after test_e0414_continue_and_switch\n");
    test_boundaries();
    fprintf(stderr, "after test_boundaries\n");
    test_eval_unsupported_label_skip();
    fprintf(stderr, "after test_eval_unsupported_label_skip\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");

    if (g_failures) {
        fprintf(stderr, "stmt_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("stmt_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
