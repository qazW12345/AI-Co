/* bootstrap/src/ir/ir_builder_stmt_test.c
 *
 * WP-M0-16c1d IR builder Phase B statement mapping/terminator unit and
 * integration tests: every statement of contract 5.2 (IR_BLOCK,
 * IR_LOCAL_DECL, IR_IF with else-if chains, IR_WHILE, IR_FOR with
 * init/cond/step scoping, IR_SWITCH/IR_CASE/IR_DEFAULT, IR_BREAK /
 * IR_CONTINUE with enclosing-stack target resolution incl. continue
 * inside a switch inside a loop, IR_RETURN, IR_EXPR_STMT, IR_EMPTY,
 * IR_CALL_TERM for rt.proc.exit / rt.trap.report), terminator rules
 * 5.6 (case/default bodies end in a terminator; no statement after a
 * terminator; void tail fall-off allowed), and the end-to-end
 * acceptance proof: produced graphs pass ir_core_verify (no AIC-I0501)
 * and ir_dump_verify (dump/parse/re-dump byte-identical), and
 * identical ASTs produce byte-identical dumps.
 *
 * The pipeline mirrors ir_builder_expr_test.c (load -> lex -> parse ->
 * name -> completeness -> layout -> convert -> optype ->
 * const_eval_check): it stops BEFORE the stmt_core/stmt_reach stages so
 * the mapper's own defensive terminator checks are reachable (the
 * integration test runs the full accepted pipeline). Test sources use
 * only constructs the full pipeline accepts; the defensive tests
 * intentionally feed constructs stmt_core would reject (case body
 * without a terminator, statement after a terminator) to prove the
 * mapper refuses them with nothing owned instead of silently producing
 * an invalid graph.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-ir16c1d' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_builder_stmt_test.c \
 *     bootstrap/src/ir/ir_builder_stmt.c bootstrap/src/ir/ir_builder_expr.c \
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
 *   ./bootstrap/stage0/msvc-ir16c1d/ir_builder_stmt_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-ir16c1d)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#include "ir_builder_stmt.h"
#include "ir_dump.h"

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
 * Shared pipeline: load -> lex -> parse -> name_resolve -> completeness ->
 * layout -> convert -> optype -> const_eval_check (mirrors expr_test).
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

static void pipeline_run(Pipeline *p, const char *src_text)
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

/* Run the pipeline AND ir_builder_build with the 16c1b + 16c1c + 16c1d
 * mappers installed; on IR_BUILDER_OK *out_build is owned by the
 * caller. */
static IrBuilderStatus pipeline_build(Pipeline *p, const char *src_text,
                                      IrBuild **out_build)
{
    IrBuilderStatus bs;
    *out_build = NULL;
    pipeline_run(p, src_text);
    if (p->result == NULL || p->build == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* pipeline did not complete */
    }
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();
    bs = ir_builder_build(p->result, p->build, out_build);
    return bs;
}

/* Bypass variant: runs load -> lex -> parse -> name -> completeness ->
 * layout only, then ir_builder_build. Used for constructs whose
 * accepted IR shape must be proven but whose full pipeline is blocked
 * by a PRE-EXISTING, out-of-scope defect in the read-only
 * types/convert.c stage: check_stmt() for AST_IF calls check_block() on
 * the else branch even when it is an AST_IF (else-if chain), reading
 * the wrong union member and faulting. That defect (WP-M0-11c area)
 * crashes before the builder, so the mapper's else-if support is
 * exercised through this path; the integration test (full accepted
 * pipeline) avoids else-if chains until the convert defect is fixed. */
static IrBuilderStatus pipeline_build_noconvert(Pipeline *p,
                                                const char *src_text,
                                                IrBuild **out_build)
{
    IrBuilderStatus bs;
    memset(p, 0, sizeof(*p));
    *out_build = NULL;
    {
        LoadStatus ld = load_source_from_bytes(
            "input.ai", (const uint8_t *)src_text, strlen(src_text),
            &p->src, &p->recs, &p->rn);
        CHECK(ld == LOAD_OK);
        if (ld != LOAD_OK) return IR_BUILDER_UNSUPPORTED;
    }
    {
        LexStatus lx = lex_tokenize(p->src, &p->toks, &p->tn,
                                    &p->recs, &p->rn);
        CHECK(lx == LEX_OK);
        if (lx != LEX_OK) return IR_BUILDER_UNSUPPORTED;
    }
    {
        ParseStatus ps = parse_program(p->toks, p->tn, &p->program,
                                       &p->recs, &p->rn);
        CHECK(ps == PARSE_OK);
        if (ps != PARSE_OK) return IR_BUILDER_UNSUPPORTED;
    }
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
                         &p->result, &p->recs, &p->rn);
    if (p->st != NAME_OK) return IR_BUILDER_UNSUPPORTED;
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
    if (p->tst != TYPE_CHECK_OK) return IR_BUILDER_UNSUPPORTED;
    p->lst = types_layout_build(p->result, &p->build, &p->lrecs, &p->lrn);
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (p->result == NULL || p->build == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();
    bs = ir_builder_build(p->result, p->build, out_build);
    return bs;
}

/* ---------------------------------------------------------------------------
 * IR graph lookup helpers
 * ------------------------------------------------------------------------- */

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

/* Verify a build and fail the test on any AIC-I0501 record. */
static void verify_ok(IrBuild *b)
{
    DiagRecord **recs = NULL;
    size_t n = 0;
    IrStatus s = ir_core_verify(b, &recs, &n);
    CHECK(s == IR_OK);
    if (s == IR_VIOLATION && recs != NULL && n > 0 && recs[0] != NULL &&
        recs[0]->message != NULL) {
        fprintf(stderr, "  verify violation: %s\n", recs[0]->message);
    }
    ir_records_free(recs, n);
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* AC1: IR_BLOCK + IR_LOCAL_DECL mapping. A local var declaration creates
 * a slot (first-declaration order), registers the symbol, and emits
 * IR_LOCAL_DECL with the lowered initializer; a local const declaration
 * emits no node (no storage); a nested block maps to an IR_BLOCK
 * statement. */
static void test_block_and_local_decl(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> void {\n"
        "  var a: i32 = 1;\n"
        "  const c: i32 = 2;\n"
        "  {\n"
        "    var b: i64 = 3;\n"
        "  }\n"
        "  var a2: i32 = a + 1;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fn_node, *body;
    IrNode *block_stmt = NULL;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_node != NULL && fn_node->kind == IR_FUNCTION);
    body = fn_node != NULL ? fn_node->u.function.body : NULL;
    CHECK(body != NULL && body->kind == IR_BLOCK);
    if (body == NULL || body->kind != IR_BLOCK) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    /* two var decls + one nested block = 3 statements (const emits none) */
    CHECK(body->u.block.nstmts == 3);
    if (body->u.block.nstmts == 3) {
        CHECK(body->u.block.stmts[0]->kind == IR_LOCAL_DECL);
        CHECK(body->u.block.stmts[0]->u.local_decl.slot_index == 0);
        CHECK(body->u.block.stmts[0]->u.local_decl.init != NULL);
        CHECK(body->u.block.stmts[0]->u.local_decl.init->kind == IR_INT);
        CHECK(body->u.block.stmts[2]->kind == IR_LOCAL_DECL);
        CHECK(body->u.block.stmts[2]->u.local_decl.slot_index == 2);
        /* a2 = a + 1 references the registered local a */
        CHECK(body->u.block.stmts[2]->u.local_decl.init->kind == IR_ADD);
        CHECK(body->u.block.stmts[2]->u.local_decl.init->u.binary.left->kind ==
              IR_LOAD);
    }
    /* the nested block maps to an IR_BLOCK statement with one IR_LOCAL_DECL */
    block_stmt = (body->u.block.nstmts >= 2) ? body->u.block.stmts[1] : NULL;
    CHECK(block_stmt != NULL && block_stmt->kind == IR_BLOCK);
    if (block_stmt != NULL && block_stmt->kind == IR_BLOCK) {
        CHECK(block_stmt->u.block.nstmts == 1);
        CHECK(block_stmt->u.block.stmts[0]->kind == IR_LOCAL_DECL);
        CHECK(block_stmt->u.block.stmts[0]->u.local_decl.slot_index == 1);
    }
    /* slots in first-declaration order: 0=a(i32), 1=b(i64), 2=a2(i32) */
    if (fn_node != NULL) {
        CHECK(fn_node->u.function.nslots == 3);
        if (fn_node->u.function.nslots == 3) {
            CHECK(fn_node->u.function.slots[0]->kind == IR_SLOT_LOCAL);
            CHECK(fn_node->u.function.slots[0]->type->kind == IRT_I32);
            CHECK(fn_node->u.function.slots[1]->type->kind == IRT_I64);
            CHECK(fn_node->u.function.slots[2]->type->kind == IRT_I32);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: IR_IF with else-if chains. `if (a) {} else if (b) {} else {}`
 * maps to an IR_IF whose else_block is an IR_BLOCK containing the nested
 * IR_IF (structural mirror of the else-if chain). */
static void test_if_else_chain(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: bool, b: bool) -> void {\n"
        "  if (a) { var x: i32 = 1; }\n"
        "  else if (b) { var y: i32 = 2; }\n"
        "  else { var z: i32 = 3; }\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fn_node, *body, *outer;

    bs = pipeline_build_noconvert(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    fn_node = find_decl(b, "main", "main.f");
    body = fn_node != NULL ? fn_node->u.function.body : NULL;
    CHECK(body != NULL && body->kind == IR_BLOCK);
    if (body == NULL || body->kind != IR_BLOCK || body->u.block.nstmts < 1) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    outer = body->u.block.stmts[0];
    CHECK(outer->kind == IR_IF);
    if (outer->kind == IR_IF) {
        CHECK(outer->u.if_stmt.cond->type->kind == IRT_BOOL);
        CHECK(outer->u.if_stmt.then_block != NULL);
        CHECK(outer->u.if_stmt.then_block->kind == IR_BLOCK);
        CHECK(outer->u.if_stmt.then_block->u.block.nstmts == 1);
        CHECK(outer->u.if_stmt.then_block->u.block.stmts[0]->kind ==
              IR_LOCAL_DECL);
        /* else-if: else_block is a block containing the nested IR_IF */
        CHECK(outer->u.if_stmt.else_block != NULL);
        CHECK(outer->u.if_stmt.else_block->kind == IR_BLOCK);
        if (outer->u.if_stmt.else_block != NULL &&
            outer->u.if_stmt.else_block->kind == IR_BLOCK) {
            IrNode *inner = outer->u.if_stmt.else_block->u.block.nstmts > 0
                                ? outer->u.if_stmt.else_block->u.block.stmts[0]
                                : NULL;
            CHECK(inner != NULL && inner->kind == IR_IF);
            if (inner != NULL && inner->kind == IR_IF) {
                CHECK(inner->u.if_stmt.else_block != NULL);
                CHECK(inner->u.if_stmt.else_block->kind == IR_BLOCK);
                CHECK(inner->u.if_stmt.else_block->u.block.nstmts == 1);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: IR_WHILE and IR_FOR. A while maps to IR_WHILE (bool cond + body);
 * a for maps to IR_FOR with init/cond/step. The for-init var is scoped to
 * the for statement: its slot exists and cond/step/body references
 * resolve. The step `i = i + 1` lowers to an IR_STORE expression. */
static void test_while_and_for(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(n: i32) -> void {\n"
        "  var i: i32 = 0;\n"
        "  while (i < n) { i = i + 1; }\n"
        "  for (var j: i32 = 0; j < n; j = j + 1) { i = i + j; }\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fn_node, *body, *wh, *fr;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    fn_node = find_decl(b, "main", "main.f");
    body = fn_node != NULL ? fn_node->u.function.body : NULL;
    CHECK(body != NULL && body->kind == IR_BLOCK);
    if (body == NULL || body->kind != IR_BLOCK || body->u.block.nstmts < 3) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    wh = body->u.block.stmts[1];
    CHECK(wh->kind == IR_WHILE);
    if (wh->kind == IR_WHILE) {
        CHECK(wh->u.while_stmt.cond->type->kind == IRT_BOOL);
        CHECK(wh->u.while_stmt.body->kind == IR_BLOCK);
        CHECK(wh->u.while_stmt.body->u.block.nstmts == 1);
        CHECK(wh->u.while_stmt.body->u.block.stmts[0]->kind == IR_EXPR_STMT);
    }
    fr = body->u.block.stmts[2];
    CHECK(fr->kind == IR_FOR);
    if (fr->kind == IR_FOR) {
        /* init: the for-scoped IR_LOCAL_DECL */
        CHECK(fr->u.for_stmt.init != NULL);
        CHECK(fr->u.for_stmt.init->kind == IR_LOCAL_DECL);
        CHECK(fr->u.for_stmt.init->u.local_decl.slot_index == 2);
        CHECK(fr->u.for_stmt.init->u.local_decl.init->kind == IR_INT);
        /* cond: bool */
        CHECK(fr->u.for_stmt.cond != NULL);
        CHECK(fr->u.for_stmt.cond->type->kind == IRT_BOOL);
        /* step: expression (assignment lowers to IR_STORE) */
        CHECK(fr->u.for_stmt.step != NULL);
        CHECK(fr->u.for_stmt.step->kind == IR_STORE);
        /* body: IR_BLOCK */
        CHECK(fr->u.for_stmt.body != NULL);
        CHECK(fr->u.for_stmt.body->kind == IR_BLOCK);
        CHECK(fr->u.for_stmt.body->u.block.nstmts == 1);
    }
    /* slots: 0=n(param), 1=i, 2=j(for-scoped) */
    if (fn_node != NULL) {
        CHECK(fn_node->u.function.nslots == 3);
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: IR_SWITCH / IR_CASE / IR_DEFAULT with integer and enum
 * selectors. Case values are resolved constants of the selector type;
 * the switch selector is lowered once. */
static void test_switch_cases(void)
{
    static const char src[] =
        "module main;\n"
        "enum Color: u8 { Red, Green, Blue }\n"
        "fn f(n: i32, c: Color) -> void {\n"
        "  switch (n) {\n"
        "    case 0: { break; }\n"
        "    case 1: { break; }\n"
        "    default: { break; }\n"
        "  }\n"
        "  switch (c) {\n"
        "    case Color.Red: { break; }\n"
        "    case Color.Green: { break; }\n"
        "  }\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fn_node, *body, *sw1, *sw2;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    fn_node = find_decl(b, "main", "main.f");
    body = fn_node != NULL ? fn_node->u.function.body : NULL;
    CHECK(body != NULL && body->kind == IR_BLOCK);
    if (body == NULL || body->kind != IR_BLOCK || body->u.block.nstmts < 2) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    sw1 = body->u.block.stmts[0];
    CHECK(sw1->kind == IR_SWITCH);
    if (sw1->kind == IR_SWITCH) {
        CHECK(sw1->u.switch_stmt.selector->type->kind == IRT_I32);
        CHECK(sw1->u.switch_stmt.ncases == 2);
        CHECK(sw1->u.switch_stmt.default_clause != NULL);
        CHECK(sw1->u.switch_stmt.default_clause->kind == IR_DEFAULT);
        if (sw1->u.switch_stmt.ncases == 2) {
            CHECK(sw1->u.switch_stmt.cases[0]->kind == IR_CASE);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.value != NULL);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.value->kind ==
                  IRC_INT);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.value->type->
                      kind == IRT_I32);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.value->u.
                      int_bits == 0);
            CHECK(sw1->u.switch_stmt.cases[1]->u.case_clause.value->u.
                      int_bits == 1);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.body->kind ==
                  IR_BLOCK);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.body->u.block.
                      nstmts == 1);
            CHECK(sw1->u.switch_stmt.cases[0]->u.case_clause.body->u.block.
                      stmts[0]->kind == IR_BREAK);
        }
        if (sw1->u.switch_stmt.default_clause != NULL) {
            CHECK(sw1->u.switch_stmt.default_clause->u.default_clause.body->
                      kind == IR_BLOCK);
        }
    }
    sw2 = body->u.block.stmts[1];
    CHECK(sw2->kind == IR_SWITCH);
    if (sw2->kind == IR_SWITCH) {
        CHECK(sw2->u.switch_stmt.selector->type->kind == IRT_ENUM);
        CHECK(sw2->u.switch_stmt.ncases == 2);
        CHECK(sw2->u.switch_stmt.default_clause == NULL);
        if (sw2->u.switch_stmt.ncases == 2) {
            CHECK(sw2->u.switch_stmt.cases[0]->u.case_clause.value->kind ==
                  IRC_ENUM);
            CHECK(sw2->u.switch_stmt.cases[0]->u.case_clause.value->type->
                      kind == IRT_ENUM);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC2: break/continue target resolution. break targets the nearest
 * enclosing switch or loop; continue targets the nearest enclosing loop
 * and SKIPS a switch (continue inside a switch inside a loop continues
 * the loop). */
static void test_break_continue_targets(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(n: i32) -> void {\n"
        "  while (n > 0) {\n"
        "    switch (n) {\n"
        "      case 1: { continue; }\n"
        "      default: { break; }\n"
        "    }\n"
        "    break;\n"
        "  }\n"
        "  for (var i: i32 = 0; i < n; i = i + 1) {\n"
        "    continue;\n"
        "  }\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *fn_node, *body, *wh, *sw, *fr;
    IrNode *cont, *brk_sw, *brk_wh, *cont_fr;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    fn_node = find_decl(b, "main", "main.f");
    body = fn_node != NULL ? fn_node->u.function.body : NULL;
    CHECK(body != NULL && body->kind == IR_BLOCK);
    if (body == NULL || body->kind != IR_BLOCK || body->u.block.nstmts < 2) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    wh = body->u.block.stmts[0];
    CHECK(wh->kind == IR_WHILE);
    fr = body->u.block.stmts[1];
    CHECK(fr->kind == IR_FOR);
    if (wh->kind != IR_WHILE || fr->kind != IR_FOR) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    sw = wh->u.while_stmt.body->u.block.stmts[0];
    CHECK(sw->kind == IR_SWITCH);
    if (sw->kind != IR_SWITCH) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    /* continue inside the switch must target the while (not the switch) */
    cont = sw->u.switch_stmt.cases[0]->u.case_clause.body->u.block.stmts[0];
    CHECK(cont->kind == IR_CONTINUE);
    CHECK(cont->u.continue_stmt.target == wh);
    /* break inside the switch's default must target the switch */
    brk_sw = sw->u.switch_stmt.default_clause->u.default_clause.body->u.block.
                 stmts[0];
    CHECK(brk_sw->kind == IR_BREAK);
    CHECK(brk_sw->u.break_stmt.target == sw);
    /* the while-body break targets the while */
    brk_wh = wh->u.while_stmt.body->u.block.stmts[1];
    CHECK(brk_wh->kind == IR_BREAK);
    CHECK(brk_wh->u.break_stmt.target == wh);
    /* continue in the for body targets the for */
    cont_fr = fr->u.for_stmt.body->u.block.stmts[0];
    CHECK(cont_fr->kind == IR_CONTINUE);
    CHECK(cont_fr->u.continue_stmt.target == fr);
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: IR_RETURN (void without value, non-void with value), IR_EXPR_STMT
 * (call / assignment), IR_EMPTY. A non-void tail ends in IR_RETURN; a
 * void tail may fall off. */
static void test_return_expr_empty(void)
{
    static const char src[] =
        "module main;\n"
        "fn add(a: i32, b: i32) -> i32 {\n"
        "  return a + b;\n"
        "}\n"
        "fn f() -> void {\n"
        "  ;\n"
        "  var x: i32 = 0;\n"
        "  x = 5;\n"
        "  return;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *add_node, *f_node, *body;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    add_node = find_decl(b, "main", "main.add");
    CHECK(add_node != NULL && add_node->kind == IR_FUNCTION);
    if (add_node != NULL && add_node->kind == IR_FUNCTION) {
        body = add_node->u.function.body;
        CHECK(body != NULL && body->u.block.nstmts == 1);
        CHECK(body->u.block.stmts[0]->kind == IR_RETURN);
        CHECK(body->u.block.stmts[0]->u.return_stmt.value != NULL);
        CHECK(body->u.block.stmts[0]->u.return_stmt.value->kind == IR_ADD);
        CHECK(body->u.block.stmts[0]->u.return_stmt.value->type->kind ==
              IRT_I32);
    }
    f_node = find_decl(b, "main", "main.f");
    CHECK(f_node != NULL && f_node->kind == IR_FUNCTION);
    if (f_node != NULL && f_node->kind == IR_FUNCTION) {
        body = f_node->u.function.body;
        CHECK(body != NULL && body->u.block.nstmts == 4);
        if (body != NULL && body->u.block.nstmts == 4) {
            CHECK(body->u.block.stmts[0]->kind == IR_EMPTY);
            CHECK(body->u.block.stmts[1]->kind == IR_LOCAL_DECL);
            CHECK(body->u.block.stmts[2]->kind == IR_EXPR_STMT);
            CHECK(body->u.block.stmts[2]->u.expr_stmt.expr->kind == IR_STORE);
            CHECK(body->u.block.stmts[3]->kind == IR_RETURN);
            CHECK(body->u.block.stmts[3]->u.return_stmt.value == NULL);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: IR_CALL_TERM for expression statements that call rt.proc.exit /
 * rt.trap.report. The callee's spec signature (params + param slots) is
 * attached on first use; argument count/types match. */
static void test_call_term_noreturn(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.proc;\n"
        "import rt.trap;\n"
        "fn f(code: i32) -> void {\n"
        "  rt.proc.exit(code);\n"
        "}\n"
        "fn g() -> void {\n"
        "  rt.trap.report(70u32, \"boom\");\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    IrNode *exit_fn, *report_fn, *f_node, *g_node;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    exit_fn = find_decl(b, "rt.proc", "rt.proc.exit");
    report_fn = find_decl(b, "rt.trap", "rt.trap.report");
    CHECK(exit_fn != NULL && exit_fn->kind == IR_FUNCTION);
    CHECK(exit_fn != NULL && exit_fn->u.function.noreturn);
    CHECK(report_fn != NULL && report_fn->kind == IR_FUNCTION);
    CHECK(report_fn != NULL && report_fn->u.function.noreturn);
    /* spec signature attached: exit(code: i32), report(code: u32,
     * message: str) */
    CHECK(exit_fn != NULL && exit_fn->u.function.nparams == 1);
    CHECK(exit_fn != NULL && exit_fn->u.function.params[0].type->kind ==
          IRT_I32);
    CHECK(report_fn != NULL && report_fn->u.function.nparams == 2);
    CHECK(report_fn != NULL && report_fn->u.function.params[0].type->kind ==
          IRT_U32);
    CHECK(report_fn != NULL && report_fn->u.function.params[1].type->kind ==
          IRT_STR);
    f_node = find_decl(b, "main", "main.f");
    g_node = find_decl(b, "main", "main.g");
    CHECK(f_node != NULL && f_node->u.function.body->u.block.nstmts == 1);
    CHECK(f_node != NULL &&
          f_node->u.function.body->u.block.stmts[0]->kind == IR_CALL_TERM);
    CHECK(f_node != NULL &&
          f_node->u.function.body->u.block.stmts[0]->u.call_term.callee ==
              exit_fn);
    CHECK(f_node != NULL &&
          f_node->u.function.body->u.block.stmts[0]->u.call_term.nargs == 1);
    CHECK(g_node != NULL && g_node->u.function.body->u.block.nstmts == 1);
    CHECK(g_node != NULL &&
          g_node->u.function.body->u.block.stmts[0]->kind == IR_CALL_TERM);
    CHECK(g_node != NULL &&
          g_node->u.function.body->u.block.stmts[0]->u.call_term.nargs == 2);
    CHECK(g_node != NULL &&
          g_node->u.function.body->u.block.stmts[0]->u.call_term.args[0]->
              type->kind == IRT_U32);
    CHECK(g_node != NULL &&
          g_node->u.function.body->u.block.stmts[0]->u.call_term.args[1]->
              type->kind == IRT_STR);
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC4: defensive terminator checks. A case body that does not end in a
 * terminator and a statement after a terminator are refused with
 * IR_BUILDER_UNSUPPORTED and nothing owned (these reach the builder only
 * because this test's pipeline stops before stmt_core/stmt_reach). A
 * case body whose termination is compound (trailing if whose branches
 * all terminate) is also refused per header gap note 4. */
static void test_defensive_terminators(void)
{
    static const char src_bad_case[] =
        "module main;\n"
        "fn f(n: i32) -> void {\n"
        "  switch (n) {\n"
        "    case 0: { var x: i32 = 1; }\n"
        "    default: { break; }\n"
        "  }\n"
        "}\n";
    static const char src_after_term[] =
        "module main;\n"
        "fn f() -> void {\n"
        "  return;\n"
        "  var x: i32 = 1;\n"
        "}\n";
    static const char src_compound_term[] =
        "module main;\n"
        "fn f(n: i32, c: bool) -> void {\n"
        "  switch (n) {\n"
        "    case 0: { if (c) { break; } else { break; } }\n"
        "    default: { break; }\n"
        "  }\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;

    bs = pipeline_build(&p, src_bad_case, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_after_term, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_compound_term, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);
}

/* MAJOR-1 regression (reviewer2 t_6e6e83af comment 1462 / gate
 * t_ebe0fd26): a non-void function tail ending in an always-true loop
 * (while(true) / for(;;)) with no break is accepted by the pre-IR
 * reachability analysis (spec 13.5) but the ir_core_verify
 * invariant-5 analysis cannot certify the loop body as never-exiting
 * when it is empty or ends in an if without else; a faithful mapping
 * would produce a graph that fails verification (AIC-I0501). The
 * mapper refuses the class with IR_BUILDER_UNSUPPORTED and nothing
 * owned (header gap note 6). This pipeline stops before
 * stmt_core/stmt_reach, so the probes reach the mapper directly
 * (defensive). Controls whose tails ARE certifiable still build and
 * verify. */
static void test_nonvoid_loop_tail_refused(void)
{
    static const char src_a[] =
        "module main;\n"
        "fn f() -> i32 { while (true) { } }\n";
    static const char src_e[] =
        "module main;\n"
        "fn f() -> i32 { for (;;) { } }\n";
    static const char src_g[] =
        "module main;\n"
        "fn f(c: bool) -> i32 { while (true) { if (c) { continue; } } }\n";
    static const char src_nested_block[] =
        "module main;\n"
        "fn f() -> i32 { { while (true) { } } }\n";
    static const char src_if_branch[] =
        "module main;\n"
        "fn f(c: bool) -> i32 { if (c) { while (true) { } } "
        "else { return 1; } }\n";
    static const char src_else_empty[] =
        "module main;\n"
        "fn f(c: bool) -> i32 { while (true) { if (c) { continue; } "
        "else { } } }\n";
    static const char src_break_exits[] =
        "module main;\n"
        "fn f(c: bool) -> i32 { while (true) { if (c) { break; } } }\n";
    static const char src_f[] =
        "module main;\n"
        "fn f() -> i32 { while (true) { continue; } }\n";
    static const char src_f2[] =
        "module main;\n"
        "fn f() -> i32 { for (;;) { continue; } }\n";
    static const char src_b[] =
        "module main;\n"
        "fn f(c: bool) -> i32 { if (c) { return 1; } else { return 2; } }\n";
    static const char src_h[] =
        "module main;\n"
        "fn f(n: i32) -> i32 {\n"
        "  switch (n) {\n"
        "    case 0: { return 1; }\n"
        "    case 1: { return 2; }\n"
        "    default: { return 3; }\n"
        "  }\n"
        "}\n";
    static const char src_void_tails[] =
        "module main;\n"
        "fn f() -> void { while (true) { } }\n"
        "fn g() -> void { for (;;) { } }\n";
    static const char src_certified_if[] =
        "module main;\n"
        "fn f(c: bool) -> i32 { while (true) { if (c) { continue; } "
        "else { continue; } } }\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;

    /* Refused: the always-true loop body is not certifiable as
     * never-exiting (empty body; body ending in an if without else;
     * any other non-certifiable shape). */
    bs = pipeline_build(&p, src_a, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_e, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_g, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_nested_block, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_if_branch, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    bs = pipeline_build(&p, src_else_empty, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    /* An exiting break also makes the loop uncertifiable. The full
     * pipeline rejects this source earlier at stmt_reach (AIC-E0416);
     * this pipeline stops before that stage, so the mapper's defensive
     * refusal is what must hold. */
    bs = pipeline_build(&p, src_break_exits, &b);
    CHECK(bs == IR_BUILDER_UNSUPPORTED);
    CHECK(b == NULL);
    pipeline_free(&p);

    /* Controls: certifiable non-void tails and void tails still build
     * and verify. */
    bs = pipeline_build(&p, src_f, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        verify_ok(b);
        ir_build_free(b);
        b = NULL;
    }
    pipeline_free(&p);

    bs = pipeline_build(&p, src_f2, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        verify_ok(b);
        ir_build_free(b);
        b = NULL;
    }
    pipeline_free(&p);

    bs = pipeline_build(&p, src_b, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        verify_ok(b);
        ir_build_free(b);
        b = NULL;
    }
    pipeline_free(&p);

    bs = pipeline_build(&p, src_h, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        verify_ok(b);
        ir_build_free(b);
        b = NULL;
    }
    pipeline_free(&p);

    bs = pipeline_build(&p, src_void_tails, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        verify_ok(b);
        ir_build_free(b);
        b = NULL;
    }
    pipeline_free(&p);

    bs = pipeline_build(&p, src_certified_if, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs == IR_BUILDER_OK && b != NULL) {
        verify_ok(b);
        ir_build_free(b);
        b = NULL;
    }
    pipeline_free(&p);
}

/* AC1/AC3: void tail fall-off is allowed; a non-void tail terminates.
 * Full builds pass ir_core_verify; dump/parse/re-dump byte-identical;
 * identical AST -> byte-identical dump (determinism). */
static void test_verify_roundtrip(void)
{
    static const char src[] =
        "module main;\n"
        "enum Color: u8 { Red, Green, Blue }\n"
        "fn fib(n: i32) -> i32 {\n"
        "  if (n < 2) { return n; }\n"
        "  return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "fn f(a: i32, c: Color) -> void {\n"
        "  var i: i32 = 0;\n"
        "  while (i < a) {\n"
        "    i = i + 1;\n"
        "    switch (c) {\n"
        "      case Color.Red: { continue; }\n"
        "      case Color.Green: { break; }\n"
        "      default: { return; }\n"
        "    }\n"
        "  }\n"
        "  for (var j: i32 = 0; j < a; j = j + 1) {\n"
        "    i = i + j;\n"
        "  }\n"
        "  ;\n"
        "}\n";
    Pipeline p1, p2;
    IrBuild *b1 = NULL, *b2 = NULL;
    IrBuilderStatus bs;
    DiagBuf d1, d2;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;

    diag_buf_init(&d1);
    diag_buf_init(&d2);

    bs = pipeline_build(&p1, src, &b1);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b1 != NULL);
    if (bs == IR_BUILDER_OK && b1 != NULL) {
        verify_ok(b1);
        CHECK(ir_dump_verify(b1, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        CHECK(ir_dump_write(b1, &d1));
        ir_build_free(b1);
        b1 = NULL;
    }

    /* build 2: identical AST -> byte-identical dump */
    bs = pipeline_build(&p2, src, &b2);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b2 != NULL);
    if (bs == IR_BUILDER_OK && b2 != NULL) {
        CHECK(ir_dump_write(b2, &d2));
        ir_build_free(b2);
        b2 = NULL;
    }
    CHECK(d1.len == d2.len);
    CHECK(d1.len == d2.len && memcmp(d1.data, d2.data, d1.len) == 0);
    diag_buf_free(&d1);
    diag_buf_free(&d2);
    pipeline_free(&p1);
    pipeline_free(&p2);
}

int main(void)
{
    ir_builder_decl_install();
    ir_builder_expr_install();
    ir_builder_stmt_install();

    test_block_and_local_decl();
    fprintf(stderr, "after test_block_and_local_decl\n");
    test_if_else_chain();
    fprintf(stderr, "after test_if_else_chain\n");
    test_while_and_for();
    fprintf(stderr, "after test_while_and_for\n");
    test_switch_cases();
    fprintf(stderr, "after test_switch_cases\n");
    test_break_continue_targets();
    fprintf(stderr, "after test_break_continue_targets\n");
    test_return_expr_empty();
    fprintf(stderr, "after test_return_expr_empty\n");
    test_call_term_noreturn();
    fprintf(stderr, "after test_call_term_noreturn\n");
    test_defensive_terminators();
    fprintf(stderr, "after test_defensive_terminators\n");
    test_nonvoid_loop_tail_refused();
    fprintf(stderr, "after test_nonvoid_loop_tail_refused\n");
    test_verify_roundtrip();
    fprintf(stderr, "after test_verify_roundtrip\n");

    /* restore the defensive default stubs so later tests in the same
     * binary run against them (single-build compiler convention) */
    ir_builder_set_module_mapper(NULL);
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_body_mapper(NULL);

    if (g_failures) {
        fprintf(stderr, "ir_builder_stmt_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    fprintf(stderr, "ir_builder_stmt_test: %d checks, 0 failures\n",
            g_checks);
    return 0;
}
