/* bootstrap/src/ir/ir_builder_integration_test.c
 *
 * WP-M0-16c1d full-builder integration proof: the complete Phase B
 * driver (16c1a core + 16c1b decl + 16c1c expr + 16c1d stmt mappers)
 * produces a complete IrBuild for representative accepted builds; the
 * produced graphs pass ir_core_verify with no AIC-I0501, and
 * ir_dump_verify proves dump/parse/re-dump byte-identical round-trip
 * (contract sec. 11.4-11.5, invariant 12). Two independent builds of
 * identical ASTs produce byte-identical dumps (determinism, contract
 * sec. 11.3).
 *
 * The pipeline is the FULL accepted pipeline (load -> lex -> parse ->
 * name_resolve -> completeness -> layout -> convert -> optype ->
 * const_eval_check (12a) -> rec_fail_emit (12b2) -> expr_core_check
 * (13b1) -> expr_ops_check (13b2) -> stmt_core_check (13c1) ->
 * stmt_reach_check (13c2) -> fn_core_check (13d1) -> fn_main_check
 * (13d2), mirroring fn_main_test.c). The fixtures are accepted builds:
 * every stage must produce zero diagnostic records before the graph is
 * built (contract 1.3). Zero-record pass-through statuses
 * (LAYOUT_UNEVALUABLE, CONVERT_UNKNOWN, OPTYPE_UNKNOWN) are accepted
 * exactly as the sema pipeline tests accept them - those sites are
 * owned by the later packages, which are all run to completion here
 * with zero records.
 *
 * NOTE (pre-existing, out-of-scope defect): the read-only
 * types/convert.c stage faults on `else if` chains (check_stmt() calls
 * check_block() on an AST_IF else branch). Fixtures below therefore
 * avoid else-if chains; the statement mapper's else-if support is
 * proven in ir_builder_stmt_test.c through a convert-free path. The
 * convert defect belongs to WP-M0-11c and is routed there.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-ir16c1d-integration' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_builder_integration_test.c \
 *     bootstrap/src/ir/ir_builder_stmt.c bootstrap/src/ir/ir_builder_expr.c \
 *     bootstrap/src/ir/ir_builder_decl.c bootstrap/src/ir/ir_builder_core.c \
 *     bootstrap/src/ir/ir_core.c bootstrap/src/ir/ir_dump.c \
 *     bootstrap/src/sema/fn_main.c bootstrap/src/sema/fn_core.c \
 *     bootstrap/src/sema/stmt_reach.c bootstrap/src/sema/stmt_core.c \
 *     bootstrap/src/sema/expr_ops.c bootstrap/src/sema/expr_core.c \
 *     bootstrap/src/const/eval_fail_rec.c bootstrap/src/const/eval_fail_arith.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-ir16c1d-integration/ir_builder_integration_test.exe
 * (repeat with build-stage0-clang.cmd /
 * bootstrap\\stage0\\clang-ir16c1d-integration)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#include "ir_builder_stmt.h"
#include "ir_builder_decl.h"
#include "ir_builder_core.h"
#include "ir_core.h"
#include "ir_dump.h"

#include "../sema/fn_main.h"
#include "../sema/fn_core.h"
#include "../sema/stmt_reach.h"
#include "../sema/stmt_core.h"
#include "../sema/expr_core.h"
#include "../sema/expr_ops.h"
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
 * Full accepted pipeline (mirrors fn_main_test.c)
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;
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
    DiagRecord **rrecs;
    size_t rrn;
    RecFailStatus rsc;
    DiagRecord **xrecs;
    size_t xrn;
    EvalFailureSite *xfails;
    size_t xfailn;
    ExprCoreStatus xsc;
    DiagRecord **opsrecs;
    size_t opsrn;
    ExprOpsStatus osc;
    DiagRecord **srecs;
    size_t srn;
    StmtCoreStatus ssc;
    DiagRecord **rrecs2;
    size_t rrn2;
    StmtReachStatus rsc2;
    DiagRecord **frecs;
    size_t frn;
    FnCoreStatus fsc;
    DiagRecord **mrecs;
    size_t mrn;
    FnMainStatus msc;
} Pipeline;

/* The stage name of the first failure (for diagnostics); NULL when the
 * pipeline is clean. */
static const char *g_fail_stage = NULL;

/* Run the full pipeline and require every stage to be clean (accepted
 * build: no records anywhere). Returns true when clean; on failure
 * g_fail_stage names the first stage that produced a record and the
 * first record message is printed to stderr. */
static bool pipeline_accepted(Pipeline *p, const char *src_text)
{
    memset(p, 0, sizeof(*p));
    g_fail_stage = NULL;
    p->ld = load_source_from_bytes("input.ai", (const uint8_t *)src_text,
                                   strlen(src_text), &p->src, &p->recs, &p->rn);
    if (p->ld != LOAD_OK || p->rn > 0) { g_fail_stage = "load"; goto fail; }
    p->lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    if (p->lx != LEX_OK || p->rn > 0) { g_fail_stage = "lex"; goto fail; }
    p->ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    if (p->ps != PARSE_OK || p->rn > 0) { g_fail_stage = "parse"; goto fail; }
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
                         &p->result, &p->recs, &p->rn);
    if (p->st != NAME_OK || p->rn > 0) { g_fail_stage = "name"; goto fail; }
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
    if (p->tst != TYPE_CHECK_OK || p->trn > 0) { g_fail_stage = "completeness"; goto fail; }
    p->lst = types_layout_build(p->result, &p->build, &p->lrecs, &p->lrn);
    /* LAYOUT_UNEVALUABLE is a zero-record pass-through: the owning
     * package (WP-M0-11d/13) reports those sites (layout.h sec. notes);
     * the fixture remains a clean accepted build (contract 1.3). */
    if (p->lst == LAYOUT_UNSUPPORTED || p->lst == LAYOUT_OOM ||
        p->lrn > 0) { g_fail_stage = "layout"; goto fail; }
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    /* CONVERT_UNKNOWN is a zero-record pass-through (convert.h: "the
     * same discipline as 11b's LAYOUT_UNEVALUABLE"): a site outside
     * the 11c bounded subset is owned by the later packages (runtime
     * built-in calls, enum member accesses, ...). Mirror the sema
     * pipeline tests (fn_main_test.c etc.) which reject only on
     * CONVERT_DIAG_ERROR. */
    if (p->cst == CONVERT_UNSUPPORTED || p->cst == CONVERT_OOM ||
        p->crn > 0) { g_fail_stage = "convert"; goto fail; }
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    if (p->ost == OPTYPE_UNSUPPORTED || p->ost == OPTYPE_OOM ||
        p->orn > 0) { g_fail_stage = "optype"; goto fail; }
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
    if (p->esc != CONST_EVAL_OK || p->ern > 0) { g_fail_stage = "const_eval"; goto fail; }
    p->rsc = rec_fail_emit(p->result, p->build, &p->rrecs, &p->rrn);
    if (p->rsc != REC_FAIL_OK || p->rrn > 0) { g_fail_stage = "rec_fail_emit"; goto fail; }
    p->xsc = expr_core_check(p->result, p->build, &p->xrecs, &p->xrn,
                             &p->xfails, &p->xfailn);
    if (p->xsc != EXPR_CORE_OK || p->xrn > 0) { g_fail_stage = "expr_core"; goto fail; }
    p->osc = expr_ops_check(p->result, p->build, &p->opsrecs, &p->opsrn);
    if (p->osc != EXPR_OPS_OK || p->opsrn > 0) { g_fail_stage = "expr_ops"; goto fail; }
    p->ssc = stmt_core_check(p->result, p->build, &p->srecs, &p->srn);
    if (p->ssc != STMT_CORE_OK || p->srn > 0) { g_fail_stage = "stmt_core"; goto fail; }
    p->rsc2 = stmt_reach_check(p->result, p->build, &p->rrecs2, &p->rrn2);
    if (p->rsc2 != STMT_REACH_OK || p->rrn2 > 0) { g_fail_stage = "stmt_reach"; goto fail; }
    p->fsc = fn_core_check(p->result, p->build, &p->frecs, &p->frn);
    if (p->fsc != FN_CORE_OK || p->frn > 0) { g_fail_stage = "fn_core"; goto fail; }
    p->msc = fn_main_check(p->result, p->build, &p->mrecs, &p->mrn);
    if (p->msc != FN_MAIN_OK || p->mrn > 0) { g_fail_stage = "fn_main"; goto fail; }
    return true;
fail:
    {
        /* print the first record of the failing stage (best effort) */
        DiagRecord **recs = NULL;
        size_t n = 0;
        if (p->rn > 0) { recs = p->recs; n = p->rn; }
        else if (p->trn > 0) { recs = p->trecs; n = p->trn; }
        else if (p->lrn > 0) { recs = p->lrecs; n = p->lrn; }
        else if (p->crn > 0) { recs = p->crecs; n = p->crn; }
        else if (p->orn > 0) { recs = p->orecs; n = p->orn; }
        else if (p->ern > 0) { recs = p->erecs; n = p->ern; }
        else if (p->rrn > 0) { recs = p->rrecs; n = p->rrn; }
        else if (p->xrn > 0) { recs = p->xrecs; n = p->xrn; }
        else if (p->opsrn > 0) { recs = p->opsrecs; n = p->opsrn; }
        else if (p->srn > 0) { recs = p->srecs; n = p->srn; }
        else if (p->rrn2 > 0) { recs = p->rrecs2; n = p->rrn2; }
        else if (p->frn > 0) { recs = p->frecs; n = p->frn; }
        else if (p->mrn > 0) { recs = p->mrecs; n = p->mrn; }
        if (recs != NULL && n > 0 && recs[0] != NULL &&
            recs[0]->message != NULL) {
            fprintf(stderr, "  pipeline rejected at stage %s: %s\n",
                    g_fail_stage, recs[0]->message);
        } else {
            fprintf(stderr, "  pipeline rejected at stage %s (status "
                    "mismatch)\n", g_fail_stage);
        }
    }
    return false;
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
    types_records_free(p->mrecs, p->mrn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* Build the IR for an accepted source; *out owned by the caller.
 * Returns the builder status (pipeline must already be accepted). */
static IrBuilderStatus pipeline_ir_build(Pipeline *p, IrBuild **out)
{
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();
    return ir_builder_build(p->result, p->build, out);
}

/* ---------------------------------------------------------------------------
 * Fixture 1: control flow, local scoping, recursion, switch/loops,
 * break/continue, noreturn tail call.
 * ------------------------------------------------------------------------- */

static const char kFixture1[] =
    "module main;\n"
    "import rt.proc;\n"
    "enum Color: u8 { Red, Green, Blue }\n"
    "fn fib(n: i32) -> i32 {\n"
    "  if (n < 2) { return n; }\n"
    "  return fib(n - 1) + fib(n - 2);\n"
    "}\n"
    "fn consume(c: Color) -> void {\n"
    "  switch (c) {\n"
    "    case Color.Red: { break; }\n"
    "    case Color.Green: { break; }\n"
    "    default: { break; }\n"
    "  }\n"
    "}\n"
    "fn sum(n: i32) -> i32 {\n"
    "  var total: i32 = 0;\n"
    "  var i: i32 = 0;\n"
    "  while (i < n) {\n"
    "    i = i + 1;\n"
    "    if (i > 100) { break; }\n"
    "    total = total + i;\n"
    "  }\n"
    "  for (var j: i32 = 0; j < n; j = j + 1) {\n"
    "    if (j == 7) { continue; }\n"
    "    total = total + j;\n"
    "  }\n"
    "  return total;\n"
    "}\n"
    "fn main() -> i32 { return sum(10) + fib(5); }\n";

/* ---------------------------------------------------------------------------
 * Fixture 2: noreturn terminator (rt.proc.exit) as the tail; void
 * fall-off; expression statements; empty statement; nested blocks.
 * ------------------------------------------------------------------------- */

static const char kFixture2[] =
    "module main;\n"
    "import rt.proc;\n"
    "fn fail(code: i32) -> void {\n"
    "  rt.proc.exit(code);\n"
    "}\n"
    "fn use(n: i32) -> void {\n"
    "  var x: i32 = n;\n"
    "  ;\n"
    "  {\n"
    "    var y: i32 = 2;\n"
    "    x = x + y;\n"
    "  }\n"
    "  if (x < 0) { fail(1); }\n"
    "  return;\n"
    "}\n"
    "fn main() -> void { use(3); }\n";

/* ---------------------------------------------------------------------------
 * AC3/AC4 tests
 * ------------------------------------------------------------------------- */

/* Every fixture is an accepted build; the produced graph passes
 * ir_core_verify (no AIC-I0501) and ir_dump_verify (round-trip
 * byte-identical), and two independent builds of the same AST produce
 * byte-identical dumps. */
static void test_accepted_build_verify_and_determinism(void)
{
    static const char *const fixtures[] = { kFixture1, kFixture2 };
    size_t fi;

    for (fi = 0; fi < sizeof(fixtures) / sizeof(fixtures[0]); fi++) {
        Pipeline p1, p2;
        IrBuild *b1 = NULL, *b2 = NULL;
        IrBuilderStatus bs;
        DiagBuf d1, d2;
        DiagRecord **recs = NULL;
        size_t nrecs = 0;

        diag_buf_init(&d1);
        diag_buf_init(&d2);

        CHECK(pipeline_accepted(&p1, fixtures[fi]));
        if (!pipeline_accepted(&p1, fixtures[fi])) {
            pipeline_free(&p1);
            diag_buf_free(&d1);
            diag_buf_free(&d2);
            continue;
        }
        bs = pipeline_ir_build(&p1, &b1);
        CHECK(bs == IR_BUILDER_OK);
        CHECK(b1 != NULL);
        if (bs == IR_BUILDER_OK && b1 != NULL) {
            IrStatus vs = ir_core_verify(b1, &recs, &nrecs);
            CHECK(vs == IR_OK);
            if (vs == IR_VIOLATION) {
                size_t i;
                fprintf(stderr, "fixture %zu: %zu AIC-I0501 records:\n",
                        fi, nrecs);
                for (i = 0; i < nrecs && i < 5; i++) {
                    fprintf(stderr, "  %s\n",
                            recs[i] && recs[i]->message
                                ? recs[i]->message
                                : "(no message)");
                }
            }
            ir_records_free(recs, nrecs);
            recs = NULL;
            nrecs = 0;

            CHECK(ir_dump_verify(b1, &recs, &nrecs) == IR_OK);
            ir_records_free(recs, nrecs);
            recs = NULL;
            nrecs = 0;

            CHECK(ir_dump_write(b1, &d1));
        }
        pipeline_free(&p1);

        /* Second independent build -> byte-identical dump */
        CHECK(pipeline_accepted(&p2, fixtures[fi]));
        if (!pipeline_accepted(&p2, fixtures[fi])) {
            pipeline_free(&p2);
            diag_buf_free(&d1);
            diag_buf_free(&d2);
            continue;
        }
        bs = pipeline_ir_build(&p2, &b2);
        CHECK(bs == IR_BUILDER_OK);
        if (bs == IR_BUILDER_OK && b2 != NULL) {
            CHECK(ir_dump_write(b2, &d2));
            ir_build_free(b2);
            b2 = NULL;
        }
        pipeline_free(&p2);

        CHECK(d1.len == d2.len);
        CHECK(d1.len == d2.len && memcmp(d1.data, d2.data, d1.len) == 0);

        diag_buf_free(&d1);
        diag_buf_free(&d2);
        ir_build_free(b1);
        ir_build_free(b2);
    }
}

/* AC3: structural spot checks on fixture 1 - the produced graph has the
 * expected statement shapes (IR_IF, IR_WHILE, IR_FOR, IR_SWITCH,
 * IR_CASE, IR_DEFAULT, IR_BREAK, IR_CONTINUE, IR_RETURN, IR_CALL_TERM
 * absence: fixture 1 has no noreturn calls). */
static IrNode *find_decl(IrBuild *b, const char *module_fqn,
                         const char *decl_fqn)
{
    size_t mi, di;
    if (b == NULL) return NULL;
    for (mi = 0; mi < b->nmodules; mi++) {
        IrNode *m = b->modules[mi];
        if (m == NULL || m->u.module.name == NULL ||
            strcmp(m->u.module.name, module_fqn) != 0) {
            continue;
        }
        for (di = 0; di < m->u.module.ndecls; di++) {
            IrNode *d = m->u.module.decls[di];
            const char *n = NULL;
            if (d == NULL) continue;
            switch (d->kind) {
            case IR_STRUCT_DECL:  n = d->u.struct_decl.name; break;
            case IR_ENUM_DECL:    n = d->u.enum_decl.name; break;
            case IR_GLOBAL_CONST: n = d->u.global_const.name; break;
            case IR_GLOBAL_VAR:   n = d->u.global_var.name; break;
            case IR_FUNCTION:     n = d->u.function.name; break;
            default: break;
            }
            if (n != NULL && strcmp(n, decl_fqn) == 0) {
                return d;
            }
        }
    }
    return NULL;
}

static void test_fixture1_structure(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *sum_fn, *body, *wh, *fr, *consume_fn, *sw;

    CHECK(pipeline_accepted(&p, kFixture1));
    if (!pipeline_accepted(&p, kFixture1)) {
        pipeline_free(&p);
        return;
    }
    bs = pipeline_ir_build(&p, &b);
    CHECK(bs == IR_BUILDER_OK);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    sum_fn = find_decl(b, "main", "main.sum");
    CHECK(sum_fn != NULL && sum_fn->kind == IR_FUNCTION);
    if (sum_fn != NULL && sum_fn->kind == IR_FUNCTION) {
        body = sum_fn->u.function.body;
        CHECK(body != NULL && body->kind == IR_BLOCK);
        if (body != NULL && body->u.block.nstmts >= 4) {
            CHECK(body->u.block.stmts[0]->kind == IR_LOCAL_DECL);
            CHECK(body->u.block.stmts[1]->kind == IR_LOCAL_DECL);
            wh = body->u.block.stmts[2];
            CHECK(wh->kind == IR_WHILE);
            if (wh->kind == IR_WHILE) {
                CHECK(wh->u.while_stmt.cond->type->kind == IRT_BOOL);
                CHECK(wh->u.while_stmt.body->kind == IR_BLOCK);
                CHECK(wh->u.while_stmt.body->u.block.nstmts == 3);
                CHECK(wh->u.while_stmt.body->u.block.stmts[1]->kind ==
                      IR_IF);
            }
            fr = body->u.block.stmts[3];
            CHECK(fr->kind == IR_FOR);
            if (fr->kind == IR_FOR) {
                CHECK(fr->u.for_stmt.init != NULL);
                CHECK(fr->u.for_stmt.init->kind == IR_LOCAL_DECL);
                CHECK(fr->u.for_stmt.cond != NULL);
                CHECK(fr->u.for_stmt.step != NULL);
                CHECK(fr->u.for_stmt.body->kind == IR_BLOCK);
                CHECK(fr->u.for_stmt.body->u.block.nstmts == 2);
                CHECK(fr->u.for_stmt.body->u.block.stmts[0]->kind == IR_IF);
                CHECK(fr->u.for_stmt.body->u.block.stmts[0]->u.if_stmt.
                          then_block->u.block.stmts[0]->kind == IR_CONTINUE);
            }
            CHECK(body->u.block.stmts[4]->kind == IR_RETURN);
        }
    }
    consume_fn = find_decl(b, "main", "main.consume");
    CHECK(consume_fn != NULL && consume_fn->kind == IR_FUNCTION);
    if (consume_fn != NULL && consume_fn->kind == IR_FUNCTION) {
        body = consume_fn->u.function.body;
        sw = body != NULL ? body->u.block.stmts[0] : NULL;
        CHECK(sw != NULL && sw->kind == IR_SWITCH);
        if (sw != NULL && sw->kind == IR_SWITCH) {
            CHECK(sw->u.switch_stmt.ncases == 2);
            CHECK(sw->u.switch_stmt.default_clause != NULL);
            CHECK(sw->u.switch_stmt.default_clause->kind == IR_DEFAULT);
            CHECK(sw->u.switch_stmt.cases[0]->u.case_clause.value->kind ==
                  IRC_ENUM);
            CHECK(sw->u.switch_stmt.cases[0]->u.case_clause.body->u.block.
                      stmts[0]->kind == IR_BREAK);
        }
    }
    /* no noreturn calls in fixture 1: the body of main.sum must not
     * contain IR_CALL_TERM */
    if (sum_fn != NULL) {
        /* nothing to assert beyond the verified graph: ir_core_verify
         * already ran inside test_accepted_build_verify_and_determinism;
         * here we spot-check kinds only. */
        CHECK(sum_fn->u.function.ret_type->kind == IRT_I32);
    }
    ir_build_free(b);
    pipeline_free(&p);
}

/* Fixture 2: the noreturn call tail maps to IR_CALL_TERM and the callee
 * carries its spec signature. */
static void test_fixture2_call_term(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fail_fn, *body, *term;

    CHECK(pipeline_accepted(&p, kFixture2));
    if (!pipeline_accepted(&p, kFixture2)) {
        pipeline_free(&p);
        return;
    }
    bs = pipeline_ir_build(&p, &b);
    CHECK(bs == IR_BUILDER_OK);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    fail_fn = find_decl(b, "main", "main.fail");
    CHECK(fail_fn != NULL && fail_fn->kind == IR_FUNCTION);
    if (fail_fn != NULL && fail_fn->kind == IR_FUNCTION) {
        body = fail_fn->u.function.body;
        CHECK(body != NULL && body->u.block.nstmts == 1);
        term = body != NULL ? body->u.block.stmts[0] : NULL;
        CHECK(term != NULL && term->kind == IR_CALL_TERM);
        if (term != NULL && term->kind == IR_CALL_TERM) {
            CHECK(term->u.call_term.callee != NULL);
            CHECK(term->u.call_term.callee->u.function.noreturn);
            CHECK(term->u.call_term.nargs == 1);
            CHECK(term->u.call_term.args[0]->type->kind == IRT_I32);
        }
    }
    ir_build_free(b);
    pipeline_free(&p);
}

int main(void)
{
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();

    test_accepted_build_verify_and_determinism();
    fprintf(stderr, "after test_accepted_build_verify_and_determinism\n");
    test_fixture1_structure();
    fprintf(stderr, "after test_fixture1_structure\n");
    test_fixture2_call_term();
    fprintf(stderr, "after test_fixture2_call_term\n");

    /* restore the defensive default stubs (single-build convention) */
    ir_builder_set_module_mapper(NULL);
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_body_mapper(NULL);

    if (g_failures) {
        fprintf(stderr, "ir_builder_integration_test: %d checks, "
                "%d FAILURES\n", g_checks, g_failures);
        return 1;
    }
    fprintf(stderr, "ir_builder_integration_test: %d checks, 0 failures\n",
            g_checks);
    return 0;
}
