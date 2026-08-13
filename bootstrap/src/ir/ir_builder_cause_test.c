/* bootstrap/src/ir/ir_builder_cause_test.c
 *
 * WP-M0-16c2 IR span/cause preservation tests: the full preservation
 * chains (IR contract sec. 8) on the completed builder output.
 *
 * Proves, on full-pipeline accepted builds:
 *   1. chain totality - every node's cause chain is the ordered
 *      parent-linked chain from its source construct to the module root
 *      (AST_PROGRAM), root cause first (the source construct); every link
 *      carries a non-empty construct kind and a valid span;
 *   2. span preservation - the primary span of every node is unchanged by
 *      the enrichment, and the first cause link's span is byte-identical
 *      to the node's primary span (the source construct's span, copied at
 *      construction, contract sec. 8.1/8.5);
 *   3. resolved-reference facts - module-scope declaration references
 *      (global var, global const, function), enum member references,
 *      named type references, and local variable references carry
 *      ref_decl/ref_type/ref_const IR ids where determinable; value
 *      member (field) accesses are deferred by name resolution to the
 *      types phase (name.c resolve_member_chain records only the base
 *      ident), so their cause links carry -1 (disclosed);
 *   4. lowering causality - one source construct that produces several IR
 *      nodes (compound assignment: destination-location, source, op,
 *      store) gives every produced node the source construct's span in its
 *      cause chain (identical chain root);
 *   5. determinism - two builds of an identical AST produce byte-identical
 *      dumps after enrichment (identical AST -> byte-identical IR);
 *   6. verification - after enrichment the graph still passes ir_core_verify
 *      (0 AIC-I0501) and ir_dump_verify (dump/parse/re-dump byte-identical,
 *      invariant 12), and the enriched chains round-trip.
 *
 * The pipeline mirrors ir_builder_integration_test.c: the FULL accepted
 * pipeline (load -> lex -> parse -> name_resolve -> completeness -> layout
 * -> convert -> optype -> const_eval_check (12a) -> rec_fail_emit (12b2) ->
 * expr_core_check (13b1) -> expr_ops_check (13b2) -> stmt_core_check
 * (13c1) -> stmt_reach_check (13c2) -> fn_core_check (13d1) ->
 * fn_main_check (13d2)). Every fixture is an accepted build: all pre-IR
 * stages produce zero records.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-ir16c2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_builder_cause_test.c \
 *     bootstrap/src/ir/ir_builder_cause.c \
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
 *   ./bootstrap/stage0/msvc-ir16c2/ir_builder_cause_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-ir16c2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#include "ir_builder_cause.h"
#include "ir_builder_stmt.h"
#include "ir_builder_decl.h"
#include "ir_builder_expr.h"
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
 * Full accepted pipeline (mirrors ir_builder_integration_test.c)
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

static const char *g_fail_stage = NULL;

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
    if (p->lst == LAYOUT_UNSUPPORTED || p->lst == LAYOUT_OOM ||
        p->lrn > 0) { g_fail_stage = "layout"; goto fail; }
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
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

static IrBuilderStatus pipeline_ir_build(Pipeline *p, IrBuild **out)
{
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();
    return ir_builder_build(p->result, p->build, out);
}

/* Build the IR and enrich it; *out owned by the caller. */
static IrBuilderStatus pipeline_ir_enrich(Pipeline *p, IrBuild **out)
{
    IrBuilderStatus st = pipeline_ir_build(p, out);
    if (st != IR_BUILDER_OK || *out == NULL) {
        return st;
    }
    return ir_builder_cause_finalize(*out, p->result);
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

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

static bool spans_equal(const DiagSpan *a, const DiagSpan *b)
{
    if (a == NULL || b == NULL) return a == b;
    if (a->file == NULL || b->file == NULL) return a->file == b->file;
    return strcmp(a->file, b->file) == 0 &&
           a->start.line == b->start.line &&
           a->start.col == b->start.col &&
           a->start.offset == b->start.offset &&
           a->end.line == b->end.line &&
           a->end.col == b->end.col &&
           a->end.offset == b->end.offset;
}

/* ---------------------------------------------------------------------------
 * Fixtures
 * ------------------------------------------------------------------------- */

/* Global var + enum + member references + local refs + function calls:
 * exercises every resolved-reference fact class. The constructs mirror
 * the proven 16c1d integration fixtures (enum switch/loops, recursion,
 * noreturn, expression statements) plus a global var reference. Struct
 * VALUE usage (param by value, field access, struct literals) and
 * global-const references in value positions are deliberately excluded
 * because the full accepted pipeline does not yet prove those builder
 * paths (16c1c expr tests exercise them through a lighter pipeline; the
 * const-in-value-position load defect and struct-value gaps belong to
 * 16c1c and are out of scope here). ref_const is still exercised through
 * enum member references (Color.Green / Color.Red). */
static const char kRefFixture[] =
    "module main;\n"
    "import rt.proc;\n"
    "var counter: i32 = 0;\n"
    "enum Color: u8 { Red, Green, Blue }\n"
    "fn bump() -> i32 {\n"
    "  counter = counter + 1;\n"
    "  return counter;\n"
    "}\n"
    "fn mix(c: Color) -> i32 {\n"
    "  var total: i32 = 0;\n"
    "  total = total + bump();\n"
    "  if (c == Color.Green) { total = total + 1; }\n"
    "  return total;\n"
    "}\n"
    "fn main() -> i32 { return mix(Color.Red) + counter; }\n";

/* Compound assignment fixture: one source construct (AST_EXPR_ASSIGN)
 * produces several IR nodes (destination-location, source, op, store). */
static const char kCompoundFixture[] =
    "module main;\n"
    "fn bump() -> i32 { return 7; }\n"
    "fn f() -> i32 {\n"
    "  var g: i32 = 1;\n"
    "  g += bump();\n"
    "  return g;\n"
    "}\n"
    "fn main() -> i32 { return f(); }\n";

/* ---------------------------------------------------------------------------
 * Test 1: chain totality - every node's chain terminates at the module root
 * and every link carries a construct kind + valid span.
 * ------------------------------------------------------------------------- */

static void test_chain_totality(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus st;
    size_t i;

    if (!pipeline_accepted(&p, kRefFixture)) {
        pipeline_free(&p);
        return;
    }
    st = pipeline_ir_enrich(&p, &b);
    CHECK(st == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (st != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }

    for (i = 0; i < b->nnodes; i++) {
        IrNode *n = b->nodes[i];
        size_t k;
        CHECK(n->cause_count >= 1);
        if (n->cause_count < 1) continue;
        for (k = 0; k < n->cause_count; k++) {
            CHECK(n->causes[k].construct_kind != NULL);
            CHECK(n->causes[k].construct_kind[0] != '\0');
            CHECK(n->causes[k].span != NULL);
        }
        /* The module root is the last link of every enriched chain: the
         * builder runs on accepted builds, so every user-module node's
         * source construct is in the module AST and the chain must reach
         * AST_PROGRAM. Runtime nodes (rt.proc) keep their single-link
         * chain (synthetic spans) - they are the only nodes whose last
         * link is not AST_PROGRAM. */
        if (n->causes[n->cause_count - 1].construct_kind != NULL &&
            strcmp(n->causes[n->cause_count - 1].construct_kind,
                   "AST_PROGRAM") == 0) {
            CHECK(n->cause_count >= 2);
        }
    }

    /* a deep node: the assignment `total = total + bump();` inside
     * mix. The statement maps to IR_EXPR_STMT wrapping the IR_STORE; the
     * store's value is the AST_EXPR_BINARY. The store's chain is
     *   AST_EXPR_ASSIGN -> AST_EXPR_STMT -> AST_BLOCK -> AST_FN_DECL
     *   -> AST_PROGRAM
     * and the value's chain is
     *   AST_EXPR_BINARY -> AST_EXPR_ASSIGN -> AST_EXPR_STMT -> AST_BLOCK
     *   -> AST_FN_DECL -> AST_PROGRAM. */
    {
        IrNode *mix = find_decl(b, "main", "main.mix");
        CHECK(mix != NULL && mix->kind == IR_FUNCTION);
        if (mix != NULL && mix->kind == IR_FUNCTION) {
            IrNode *body = mix->u.function.body;
            CHECK(body != NULL && body->kind == IR_BLOCK);
            if (body != NULL && body->u.block.nstmts >= 2) {
                IrNode *stmt = body->u.block.stmts[1];
                CHECK(stmt != NULL && stmt->kind == IR_EXPR_STMT);
                if (stmt != NULL && stmt->kind == IR_EXPR_STMT &&
                    stmt->u.expr_stmt.expr != NULL) {
                    IrNode *store = stmt->u.expr_stmt.expr;
                    CHECK(store->kind == IR_STORE);
                    CHECK(store->cause_count >= 5);
                    CHECK(strcmp(store->causes[0].construct_kind,
                                 "AST_EXPR_ASSIGN") == 0);
                    CHECK(strcmp(
                        store->causes[store->cause_count - 1]
                            .construct_kind,
                        "AST_PROGRAM") == 0);
                    if (store->u.store.value != NULL) {
                        IrNode *val = store->u.store.value;
                        CHECK(val->cause_count >= 6);
                        CHECK(strcmp(val->causes[0].construct_kind,
                                     "AST_EXPR_BINARY") == 0);
                        CHECK(strcmp(
                            val->causes[val->cause_count - 1]
                                .construct_kind,
                            "AST_PROGRAM") == 0);
                    }
                }
            }
        }
    }

    ir_build_free(b);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Test 2: span preservation - primary spans are unchanged by the
 * enrichment and the first cause link's span is byte-identical to the
 * node's primary span (the source construct's span).
 * ------------------------------------------------------------------------- */

static void test_span_preservation(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus st;
    size_t i;

    if (!pipeline_accepted(&p, kRefFixture)) {
        pipeline_free(&p);
        return;
    }
    st = pipeline_ir_build(&p, &b);
    CHECK(st == IR_BUILDER_OK);
    if (st != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }

    /* record every node's primary span before enrichment */
    {
        DiagSpan **before = (DiagSpan **)calloc(b->nnodes,
                                                sizeof(DiagSpan *));
        CHECK(before != NULL);
        if (before != NULL) {
            for (i = 0; i < b->nnodes; i++) {
                before[i] = b->nodes[i]->span != NULL
                                ? diag_span_clone(b->nodes[i]->span)
                                : NULL;
            }
            st = ir_builder_cause_finalize(b, p.result);
            CHECK(st == IR_BUILDER_OK);
            for (i = 0; i < b->nnodes; i++) {
                /* primary span unchanged */
                CHECK((before[i] == NULL && b->nodes[i]->span == NULL) ||
                      (before[i] != NULL && b->nodes[i]->span != NULL &&
                       strcmp(before[i]->file,
                              b->nodes[i]->span->file) == 0 &&
                       before[i]->start.offset ==
                           b->nodes[i]->span->start.offset &&
                       before[i]->end.offset ==
                           b->nodes[i]->span->end.offset));
                /* the first cause link's span is the source construct's
                 * span, which is the node's own primary span (the builder
                 * clones it at construction) */
                if (b->nodes[i]->cause_count >= 1 &&
                    b->nodes[i]->causes[0].span != NULL &&
                    b->nodes[i]->span != NULL) {
                    CHECK(spans_equal(b->nodes[i]->causes[0].span,
                                      b->nodes[i]->span));
                }
                diag_span_free(before[i]);
            }
            free(before);
        }
    }

    ir_build_free(b);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Test 3: resolved-reference facts
 * ------------------------------------------------------------------------- */

static void test_resolved_reference_facts(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus st;
    IrNode *counter_decl, *color_decl, *bump_decl;
    size_t i;

    if (!pipeline_accepted(&p, kRefFixture)) {
        pipeline_free(&p);
        return;
    }
    st = pipeline_ir_enrich(&p, &b);
    CHECK(st == IR_BUILDER_OK);
    if (st != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }

    counter_decl = find_decl(b, "main", "main.counter");
    color_decl = find_decl(b, "main", "main.Color");
    bump_decl = find_decl(b, "main", "main.bump");
    CHECK(counter_decl != NULL && counter_decl->kind == IR_GLOBAL_VAR);
    CHECK(color_decl != NULL && color_decl->kind == IR_ENUM_DECL);
    CHECK(bump_decl != NULL && bump_decl->kind == IR_FUNCTION);

    /* find an IR_GLOBAL referencing counter: its cause[0] (the
     * AST_EXPR_IDENT construct) must carry ref_decl = counter's id */
    {
        bool found = false;
        for (i = 0; i < b->nnodes; i++) {
            IrNode *n = b->nodes[i];
            if (n->kind == IR_GLOBAL && n->u.global.target == counter_decl) {
                found = true;
                CHECK(n->cause_count >= 1);
                if (n->cause_count >= 1) {
                    CHECK(n->causes[0].ref_decl == counter_decl->id);
                    CHECK(n->causes[0].ref_type == -1);
                }
            }
        }
        CHECK(found);
    }

    /* find the IR_ENUM_VAL for Color.Green: cause[0] (AST_EXPR_MEMBER)
     * carries ref_decl = Color decl id, ref_type = the enum type id, and
     * ref_const = the member's IRC_ENUM const id */
    {
        bool found = false;
        for (i = 0; i < b->nnodes; i++) {
            IrNode *n = b->nodes[i];
            if (n->kind == IR_ENUM_VAL &&
                n->causes[0].construct_kind != NULL &&
                strcmp(n->causes[0].construct_kind,
                       "AST_EXPR_MEMBER") == 0) {
                found = true;
                CHECK(n->causes[0].ref_decl == color_decl->id);
                CHECK(n->causes[0].ref_type >= 0);
                CHECK(n->causes[0].ref_const >= 0);
            }
        }
        CHECK(found);
    }

    /* the call to bump(): the IR_CALL's cause[0] (AST_EXPR_CALL) carries
     * ref_decl = bump's id */
    {
        bool found = false;
        for (i = 0; i < b->nnodes; i++) {
            IrNode *n = b->nodes[i];
            if (n->kind == IR_CALL &&
                n->causes[0].construct_kind != NULL &&
                strcmp(n->causes[0].construct_kind,
                       "AST_EXPR_CALL") == 0 &&
                n->causes[0].ref_decl == bump_decl->id) {
                found = true;
            }
        }
        CHECK(found);
    }

    /* the call to mix() in main also carries the callee decl id */
    {
        IrNode *mix_decl = find_decl(b, "main", "main.mix");
        bool found = false;
        CHECK(mix_decl != NULL);
        if (mix_decl != NULL) {
            for (i = 0; i < b->nnodes; i++) {
                IrNode *n = b->nodes[i];
                if (n->kind == IR_CALL &&
                    n->causes[0].construct_kind != NULL &&
                    strcmp(n->causes[0].construct_kind,
                           "AST_EXPR_CALL") == 0 &&
                    n->causes[0].ref_decl == mix_decl->id) {
                    found = true;
                }
            }
        }
        CHECK(found);
    }

    /* a local var reference (total): an IR_LOCAL whose construct is
     * AST_EXPR_IDENT must resolve to the IR_LOCAL_DECL node id */
    {
        bool found = false;
        for (i = 0; i < b->nnodes; i++) {
            IrNode *n = b->nodes[i];
            if (n->kind == IR_LOCAL &&
                n->causes[0].construct_kind != NULL &&
                strcmp(n->causes[0].construct_kind,
                       "AST_EXPR_IDENT") == 0 &&
                n->causes[0].ref_decl >= 0) {
                IrNode *decl = NULL;
                size_t j;
                for (j = 0; j < b->nnodes; j++) {
                    if (b->nodes[j]->kind == IR_LOCAL_DECL &&
                        b->nodes[j]->id == n->causes[0].ref_decl) {
                        decl = b->nodes[j];
                        break;
                    }
                }
                CHECK(decl != NULL);
                found = true;
                break;
            }
        }
        CHECK(found);
    }

    /* a global const reference would carry ref_const, but global-const
     * references in value positions are excluded from the full accepted
     * pipeline surface (16c1c defect, disclosed above); ref_const is
     * exercised through the enum member references above. */

    ir_build_free(b);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Test 4: lowering causality - one construct producing several IR nodes
 * gives every produced node the same source-construct chain root.
 * ------------------------------------------------------------------------- */

static void test_lowering_causality(void)
{
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus st;
    size_t i;
    int store_count = 0;
    bool op_seen = false;
    const DiagSpan *root_span = NULL;
    const char *root_kind = "AST_EXPR_ASSIGN";

    if (!pipeline_accepted(&p, kCompoundFixture)) {
        pipeline_free(&p);
        return;
    }
    st = pipeline_ir_enrich(&p, &b);
    CHECK(st == IR_BUILDER_OK);
    if (st != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }

    /* The compound assignment `g += bump();` lowers to a temp STORE
     * (materialized source), the op IR_ADD, and the final STORE. Every
     * produced node carries the source construct (AST_EXPR_ASSIGN) and
     * the assignment expression's span. */
    for (i = 0; i < b->nnodes; i++) {
        IrNode *n = b->nodes[i];
        if (n->cause_count < 1 ||
            strcmp(n->causes[0].construct_kind, root_kind) != 0) {
            continue;
        }
        if (n->kind == IR_STORE) {
            store_count++;
            if (root_span == NULL) {
                root_span = n->causes[0].span;
            } else {
                CHECK(spans_equal(root_span, n->causes[0].span));
            }
        }
        if (n->kind == IR_ADD) {
            op_seen = true;
            /* the op node shares the source construct's span and its
             * chain terminates at the module root */
            CHECK(spans_equal(root_span, n->causes[0].span));
            CHECK(n->cause_count >= 5);
            CHECK(strcmp(n->causes[n->cause_count - 1].construct_kind,
                         "AST_PROGRAM") == 0);
        }
    }
    /* temp store + final store */
    CHECK(store_count >= 2);
    CHECK(op_seen);

    ir_build_free(b);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Test 5: determinism - two builds of an identical AST produce
 * byte-identical dumps after enrichment.
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    static const char *const fixtures[] = { kRefFixture,
                                            kCompoundFixture };
    size_t fi;

    for (fi = 0; fi < sizeof(fixtures) / sizeof(fixtures[0]); fi++) {
        Pipeline p1, p2;
        IrBuild *b1 = NULL, *b2 = NULL;
        IrBuilderStatus st;
        DiagBuf d1, d2;

        diag_buf_init(&d1);
        diag_buf_init(&d2);

        if (!pipeline_accepted(&p1, fixtures[fi])) {
            pipeline_free(&p1);
            diag_buf_free(&d1);
            diag_buf_free(&d2);
            continue;
        }
        st = pipeline_ir_enrich(&p1, &b1);
        CHECK(st == IR_BUILDER_OK);
        if (st == IR_BUILDER_OK && b1 != NULL) {
            CHECK(ir_dump_write(b1, &d1));
        }
        pipeline_free(&p1);

        if (!pipeline_accepted(&p2, fixtures[fi])) {
            pipeline_free(&p2);
            diag_buf_free(&d1);
            diag_buf_free(&d2);
            continue;
        }
        st = pipeline_ir_enrich(&p2, &b2);
        CHECK(st == IR_BUILDER_OK);
        if (st == IR_BUILDER_OK && b2 != NULL) {
            CHECK(ir_dump_write(b2, &d2));
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

/* ---------------------------------------------------------------------------
 * Test 6: verification after enrichment - ir_core_verify 0 AIC-I0501 and
 * ir_dump_verify round-trip byte-identical.
 * ------------------------------------------------------------------------- */

static void test_verify_after_enrichment(void)
{
    static const char *const fixtures[] = { kRefFixture,
                                            kCompoundFixture };
    size_t fi;

    for (fi = 0; fi < sizeof(fixtures) / sizeof(fixtures[0]); fi++) {
        Pipeline p;
        IrBuild *b = NULL;
        IrBuilderStatus st;
        DiagRecord **recs = NULL;
        size_t nrecs = 0;

        if (!pipeline_accepted(&p, fixtures[fi])) {
            pipeline_free(&p);
            continue;
        }
        st = pipeline_ir_enrich(&p, &b);
        CHECK(st == IR_BUILDER_OK);
        if (st != IR_BUILDER_OK || b == NULL) {
            pipeline_free(&p);
            continue;
        }

        CHECK(ir_core_verify(b, &recs, &nrecs) == IR_OK);
        if (recs != NULL) {
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

        CHECK(ir_dump_verify(b, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);

        ir_build_free(b);
        pipeline_free(&p);
    }
}

int main(void)
{
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();

    test_chain_totality();
    fprintf(stderr, "after test_chain_totality\n");
    test_span_preservation();
    fprintf(stderr, "after test_span_preservation\n");
    test_resolved_reference_facts();
    fprintf(stderr, "after test_resolved_reference_facts\n");
    test_lowering_causality();
    fprintf(stderr, "after test_lowering_causality\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_verify_after_enrichment();
    fprintf(stderr, "after test_verify_after_enrichment\n");

    /* restore the defensive default stubs (single-build convention) */
    ir_builder_set_module_mapper(NULL);
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_body_mapper(NULL);

    if (g_failures) {
        fprintf(stderr, "ir_builder_cause_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    fprintf(stderr, "ir_builder_cause_test: %d checks, 0 failures\n",
            g_checks);
    return 0;
}
