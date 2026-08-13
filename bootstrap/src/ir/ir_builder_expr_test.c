/* bootstrap/src/ir/ir_builder_expr_test.c
 *
 * WP-M0-16c1c IR builder Phase B expression mapping/lowering unit and
 * integration tests: every value-producing node of contract 5.3
 * (constants, IR_LOCAL/IR_GLOBAL refs, FIELD_ADDR/INDEX_ADDR/DEREF/
 * LOAD/STORE, arithmetic/bitwise/logical/comparison incl. SLICE_EQ,
 * SELECT, CALL, LEN/PTR/SLICE, CAST/WRAP, PTR_ADD/SUB/DIFF, ZERO),
 * value categories (5.4), evaluation order (6.1 = spec 10.4), compound
 * assignment lowering, struct/array literals (incl. repetition-form
 * `[e; N]` evaluate-exactly-once per IRC-N1), and the defensive
 * IR_BUILDER_UNSUPPORTED representable-surface gaps.
 *
 * The tests drive the expression lowerer directly (the statement mapper
 * 16c1d is not installed yet): each test parses a source, resolves it,
 * builds the IR (Phase A via 16c1b), then calls ir_builder_expr_lower /
 * ir_builder_expr_to_value / ir_builder_expr_to_lvalue on specific AST
 * expressions and inspects the produced IR. The result node is attached
 * to the function body block as an IR_EXPR_STMT (mirroring what 16c1d
 * will do) so ir_core_verify / ir_dump_verify validate the whole graph
 * (invariant 1 reachability, invariant 4 typing, invariant 9 trap
 * codes, invariant 10 store/lvalue rules). Test functions use void
 * return types so the tail-termination invariant (5) is not exercised
 * (statement mapping is 16c1d's card).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-ir16c1c' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_builder_expr_test.c \
 *     bootstrap/src/ir/ir_builder_expr.c bootstrap/src/ir/ir_builder_decl.c \
 *     bootstrap/src/ir/ir_builder_core.c bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/ir/ir_dump.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-ir16c1c/ir_builder_expr_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-ir16c1c)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#include "ir_builder_expr.h"
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
 * layout -> convert -> optype -> const_eval_check (mirrors decl_test).
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

/* Run the pipeline AND ir_builder_build with the 16c1b + 16c1c mappers;
 * on IR_BUILDER_OK *out_build is owned by the caller. */
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
    bs = ir_builder_build(p->result, p->build, out_build);
    return bs;
}

/* ---------------------------------------------------------------------------
 * AST / symbol lookup helpers
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

/* The module-scope function symbol of the entry module. */
static const NameSymbol *find_fn_sym(Pipeline *p, const char *name)
{
    const NameModule *m = p->result->modules[0];
    size_t i;
    for (i = 0; i < m->nmodule_scope; i++) {
        const NameSymbol *s = m->module_scope[i];
        if (s != NULL && s->kind == NAME_SYM_FN && s->name != NULL &&
            strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

/* The symbol of an identifier reference with the given name (the name
 * resolver records identifier-expression references in the module refs
 * table; test sources use each name exactly once). */
static const NameSymbol *find_ident_sym(Pipeline *p, const char *name)
{
    const NameModule *m = p->result->modules[0];
    size_t i;
    for (i = 0; i < m->nrefs; i++) {
        const AstNode *node = m->refs[i].node;
        if (node != NULL && node->kind == AST_EXPR_IDENT &&
            node->u.ident.name != NULL &&
            strcmp(node->u.ident.name, name) == 0) {
            return m->refs[i].sym;
        }
    }
    return NULL;
}

/* The AST function declaration with the given name. */
static AstNode *find_fn_ast(AstNode *program, const char *name)
{
    size_t i;
    if (program == NULL || program->u.program.decls == NULL) {
        return NULL;
    }
    for (i = 0; i < program->u.program.ndecls; i++) {
        AstNode *d = program->u.program.decls[i];
        if (d != NULL && d->kind == AST_FN_DECL && d->u.fn_decl.name != NULL &&
            strcmp(d->u.fn_decl.name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

/* The expression of the i-th statement of the function body. The test
 * sources use expression statements (`expr;`) whose expression is the
 * construct under test. */
static const AstNode *stmt_expr(AstNode *program, const char *fn_name,
                                size_t index)
{
    AstNode *f = find_fn_ast(program, fn_name);
    if (f == NULL || f->u.fn_decl.body == NULL ||
        index >= f->u.fn_decl.body->u.list.count) {
        return NULL;
    }
    {
        AstNode *s = f->u.fn_decl.body->u.list.items[index];
        if (s != NULL && s->kind == AST_EXPR_STMT) {
            return s->u.expr_stmt.expr;
        }
    }
    return NULL;
}

/* The initializer expression of the local declaration with the given
 * name in the function body. */
static const AstNode *var_init_expr(AstNode *program, const char *fn_name,
                                    const char *var_name)
{
    AstNode *f = find_fn_ast(program, fn_name);
    size_t i;
    if (f == NULL || f->u.fn_decl.body == NULL) {
        return NULL;
    }
    for (i = 0; i < f->u.fn_decl.body->u.list.count; i++) {
        AstNode *s = f->u.fn_decl.body->u.list.items[i];
        if (s != NULL && (s->kind == AST_VAR_DECL ||
                          s->kind == AST_CONST_DECL) &&
            s->u.local_decl.name != NULL &&
            strcmp(s->u.local_decl.name, var_name) == 0) {
            return s->u.local_decl.init;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * IR graph helpers
 * ------------------------------------------------------------------------- */

/* Create local slots (first-declaration order, contract 4.3) for every
 * AST_VAR_DECL in the function body and register the symbol -> slot
 * mapping (mirrors what the 16c1d statement mapper will do). */
static void setup_fn_locals(Pipeline *p, BuilderCtx *ctx,
                            IrNode *fn_node, const char *fn_name)
{
    AstNode *f = find_fn_ast(p->program, fn_name);
    size_t i;
    if (f == NULL || f->u.fn_decl.body == NULL) {
        return;
    }
    for (i = 0; i < f->u.fn_decl.body->u.list.count; i++) {
        AstNode *s = f->u.fn_decl.body->u.list.items[i];
        if (s != NULL && s->kind == AST_VAR_DECL &&
            s->u.local_decl.name != NULL && s->u.local_decl.type != NULL) {
            const NameSymbol *sym = find_ident_sym(p, s->u.local_decl.name);
            IrType *t = ir_builder_type_from_ast(
                ctx, p->result->modules[0], s->u.local_decl.type);
            IrSlot *slot;
            const NameSymbol *fn_sym = find_fn_sym(p, fn_name);
            if (t == NULL || sym == NULL || fn_sym == NULL) {
                continue;
            }
            slot = ir_builder_add_slot(ctx->build, fn_node, IR_SLOT_LOCAL,
                                       s->u.local_decl.name, t, s->span);
            if (slot != NULL) {
                ir_builder_expr_register_local(ctx->build, fn_sym, sym,
                                               slot->index);
            }
        }
    }
}

/* Wrap a lowered value/effect node in an IR_EXPR_STMT and append it to
 * the block (the statement-mapper convention), so the node is reachable
 * for ir_core_verify. */
static void attach_expr(IrBuild *b, IrNode *block, const DiagSpan *span,
                        IrNode *node)
{
    IrNode *stmt = ir_node_new(b, IR_EXPR_STMT, span);
    if (stmt == NULL) {
        return;
    }
    ir_node_add_cause(b, stmt, "AST_EXPR_STMT", span, -1, -1, -1);
    stmt->u.expr_stmt.expr = node;
    ir_block_add_stmt(b, block, stmt);
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

/* Append an IR_RETURN terminator (with a zero constant of the return
 * type) to a non-void function body whose tail does not yet terminate.
 * The statement mapper (16c1d) is not installed, so tests that declare
 * non-void helper functions (call targets such as add/make) must
 * terminate those bodies the way 16c1d will. Runtime functions are
 * patched by the implementation (unreachable IR_TRAP stub), so this
 * helper skips already-terminating bodies. */
static void terminate_fn_body(IrBuild *b, IrNode *fn_node)
{
    IrNode *body;
    IrNode *ret, *val;
    IrType *rt;
    if (fn_node == NULL || fn_node->kind != IR_FUNCTION) {
        return;
    }
    body = fn_node->u.function.body;
    rt = fn_node->u.function.ret_type;
    if (body == NULL || body->kind != IR_BLOCK || rt == NULL ||
        rt->kind == IRT_VOID) {
        return;
    }
    if (body->u.block.nstmts > 0) {
        IrNode *last = body->u.block.stmts[body->u.block.nstmts - 1];
        if (last != NULL && (last->kind == IR_RETURN ||
                             last->kind == IR_CALL_TERM ||
                             last->kind == IR_TRAP)) {
            return;   /* already terminates (e.g. runtime stub) */
        }
    }
    val = ir_node_new(b, IR_INT, fn_node->span);
    if (val == NULL) {
        return;
    }
    val->type = rt;
    val->u.constant.value = ir_const_int(b, rt, 0);
    ir_node_add_cause(b, val, "AST_EXPR_INT_LITERAL", fn_node->span,
                      -1, -1, -1);
    ret = ir_node_new(b, IR_RETURN, fn_node->span);
    if (ret == NULL) {
        return;
    }
    ret->u.return_stmt.value = val;
    ir_node_add_cause(b, ret, "AST_RETURN", fn_node->span, -1, -1, -1);
    ir_block_add_stmt(b, body, ret);
}

/* Count nodes of a kind in the build. */
static size_t count_kind(IrBuild *b, IrNodeKind kind)
{
    size_t i, n = 0;
    for (i = 0; i < b->nnodes; i++) {
        if (b->nodes[i]->kind == kind) {
            n++;
        }
    }
    return n;
}

/* Count IR_CALL nodes whose callee name matches. */
static size_t count_calls_to(IrBuild *b, const char *fqn)
{
    size_t i, n = 0;
    for (i = 0; i < b->nnodes; i++) {
        IrNode *nd = b->nodes[i];
        if (nd->kind == IR_CALL && nd->u.call.callee != NULL &&
            nd->u.call.callee->u.function.name != NULL &&
            strcmp(nd->u.call.callee->u.function.name, fqn) == 0) {
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* AC1: scalar constants - IR_INT (incl. typed-node re-typing of a
 * widened literal and the i32 default), IR_BOOL, IR_STR (composite),
 * IR_NULL (typed by the pointer context). */
static void test_scalar_constants(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32) -> void {\n"
        "  var i: i64 = 1;\n"
        "  var j: i32 = 2;\n"
        "  var ok: bool = true;\n"
        "  var s: str = \"hi\";\n"
        "  var p: i32* = null;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;
    setup_fn_locals(&p, &ctx, fn_node, "f");

    /* var i: i64 = 1; -> IR_INT re-typed to the declared i64 */
    {
        const AstNode *e = var_init_expr(p.program, "f", "i");
        CHECK(e != NULL && e->kind == AST_EXPR_INT_LITERAL);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE,
                                       ir_type_i64(b), &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_INT);
                CHECK(r.type->kind == IRT_I64);
                CHECK(r.node->u.constant.value->kind == IRC_INT);
                CHECK(r.node->u.constant.value->u.int_bits == 1);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* var j: i32 = 2; -> natural i32 */
    {
        const AstNode *e = var_init_expr(p.program, "f", "j");
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_INT);
                CHECK(r.type->kind == IRT_I32);
                CHECK(r.node->u.constant.value->u.int_bits == 2);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* var ok: bool = true; -> IR_BOOL */
    {
        const AstNode *e = var_init_expr(p.program, "f", "ok");
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_BOOL);
                CHECK(r.type->kind == IRT_BOOL);
                CHECK(r.node->u.constant.value->kind == IRC_BOOL);
                CHECK(r.node->u.constant.value->u.b == true);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* var s: str = "hi"; -> IR_STR, composite */
    {
        const AstNode *e = var_init_expr(p.program, "f", "s");
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_COMPOSITE);
                CHECK(r.node->kind == IR_STR);
                CHECK(r.type->kind == IRT_STR);
                CHECK(r.node->u.constant.value->kind == IRC_STR);
                CHECK(r.node->u.constant.value->u.str.len == 2);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* var p: i32* = null; -> IR_NULL with the declared pointer type */
    {
        const AstNode *e = var_init_expr(p.program, "f", "p");
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE,
                                       ir_type_ptr(b, ir_type_i32(b)), &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_NULL);
                CHECK(r.type->kind == IRT_PTR);
                CHECK(r.type->u.ptr.elem->kind == IRT_I32);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* a bare null literal without a pointer context is unsupported */
    {
        const AstNode *e = var_init_expr(p.program, "f", "p");
        if (e != NULL) {
            IrExprResult r2;
            memset(&r2, 0xAA, sizeof(r2));
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r2);
            CHECK(bs == IR_BUILDER_UNSUPPORTED);
            CHECK(r2.node == NULL);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: arithmetic/bitwise/unary nodes with their trap codes: ADD/SUB/MUL
 * (R0802), DIV/MOD (R0803), SHL/SHR (R0804), NEG (R0802), BNOT, LNOT,
 * BAND/BOR/BXOR; result types (common-type promotion for literals). */
static void test_arith_and_traps(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32, b: i32, c: bool) -> void {\n"
        "  a + b; a - b; a * b; a / b; a % b; a << 1; a >> 1;\n"
        "  a & b; a | b; a ^ b; ~a; !c; -a;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    size_t idx;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        static const struct { IrNodeKind kind; const char *trap; } expect[] = {
            { IR_ADD, "AIC-R0802" }, { IR_SUB, "AIC-R0802" },
            { IR_MUL, "AIC-R0802" }, { IR_DIV, "AIC-R0803" },
            { IR_MOD, "AIC-R0803" }, { IR_SHL, "AIC-R0804" },
            { IR_SHR, "AIC-R0804" }, { IR_BAND, NULL },
            { IR_BOR, NULL }, { IR_BXOR, NULL }, { IR_BNOT, NULL },
            { IR_LNOT, NULL }, { IR_NEG, "AIC-R0802" }
        };
        size_t n = sizeof(expect) / sizeof(expect[0]);
        for (idx = 0; idx < n; idx++) {
            const AstNode *e = stmt_expr(p.program, "f", idx);
            CHECK(e != NULL);
            if (e == NULL) {
                continue;
            }
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                continue;
            }
            CHECK(r.cat == IR_EXPR_SCALAR);
            CHECK(r.node->kind == expect[idx].kind);
            if (expect[idx].trap != NULL) {
                CHECK(r.node->trap_code != NULL &&
                      strcmp(r.node->trap_code, expect[idx].trap) == 0);
            } else {
                CHECK(r.node->trap_code == NULL);
            }
            if (r.node->kind != IR_LNOT && r.node->kind != IR_BNOT &&
                r.node->kind != IR_NEG) {
                CHECK(r.type->kind == IRT_I32);
            } else if (r.node->kind == IR_LNOT) {
                CHECK(r.type->kind == IRT_BOOL);
            }
            /* children: left then right (evaluation order) */
            if (r.node->kind == IR_ADD || r.node->kind == IR_SUB ||
                r.node->kind == IR_MUL || r.node->kind == IR_DIV ||
                r.node->kind == IR_MOD || r.node->kind == IR_SHL ||
                r.node->kind == IR_SHR || r.node->kind == IR_BAND ||
                r.node->kind == IR_BOR || r.node->kind == IR_BXOR) {
                CHECK(r.node->u.binary.left != NULL);
                CHECK(r.node->u.binary.left->kind == IR_LOAD);
                CHECK(r.node->u.binary.right != NULL);
                if (r.node->u.binary.right->kind == IR_INT) {
                    CHECK(r.node->u.binary.right->type->kind == IRT_I32);
                }
            }
            attach_expr(b, block, e->span, r.node);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC2: comparisons - IR_EQ..IR_GE (bool result), pointer == null
 * (null typed by the pointer context), str equality, slice equality
 * (IR_SLICE_EQ). */
static void test_comparisons(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32, p: i32*, s1: str, s2: str, sl1: u8[], sl2: u8[]) "
        "-> void {\n"
        "  a == a; a != a; a < a; a <= a; a > a; a >= a;\n"
        "  p == null; sl1 == sl2; s1 == s2;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        static const IrNodeKind expect[] = {
            IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE
        };
        size_t idx;
        for (idx = 0; idx < 6; idx++) {
            const AstNode *e = stmt_expr(p.program, "f", idx);
            CHECK(e != NULL);
            if (e == NULL) {
                continue;
            }
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                continue;
            }
            CHECK(r.node->kind == expect[idx]);
            CHECK(r.type->kind == IRT_BOOL);
            CHECK(r.node->u.binary.left != NULL);
            CHECK(r.node->u.binary.left->kind == IR_LOAD);
            CHECK(r.node->u.binary.right != NULL);
            attach_expr(b, block, e->span, r.node);
        }
    }
    /* p == null -> IR_EQ with an IR_NULL typed as i32* */
    {
        const AstNode *e = stmt_expr(p.program, "f", 6);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_EQ);
                CHECK(r.node->u.binary.left->kind == IR_LOAD);
                CHECK(r.node->u.binary.left->type->kind == IRT_PTR);
                CHECK(r.node->u.binary.right->kind == IR_NULL);
                CHECK(r.node->u.binary.right->type->kind == IRT_PTR);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* sl1 == sl2 -> IR_SLICE_EQ */
    {
        const AstNode *e = stmt_expr(p.program, "f", 7);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_SLICE_EQ);
                CHECK(r.type->kind == IRT_BOOL);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* s1 == s2 -> IR_EQ on str */
    {
        const AstNode *e = stmt_expr(p.program, "f", 8);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_EQ);
                CHECK(r.node->u.binary.left->type->kind == IRT_STR);
                CHECK(r.node->u.binary.right->type->kind == IRT_STR);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC2: value categories - an identifier lowers to an LVALUE (IR_LOCAL /
 * IR_GLOBAL), WANT_VALUE loads it (IR_LOAD); a composite (struct) local
 * is COMPOSITE (address-resident, never loaded); a global var lowers to
 * IR_GLOBAL; a literal is SCALAR and WANT_LVALUE on it is rejected. */
static void test_value_categories(void)
{
    static const char src[] =
        "module main;\n"
        "struct S { x: i32; }\n"
        "var g: i32 = 1;\n"
        "fn f(a: i32, s: S) -> void {\n"
        "  a; 42;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    const AstNode *e;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* param a: WANT_ANY -> LVALUE IR_LOCAL; WANT_VALUE -> IR_LOAD */
    e = stmt_expr(p.program, "f", 0);
    CHECK(e != NULL && e->kind == AST_EXPR_IDENT);
    if (e != NULL) {
        bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                   block, e, IR_EXPR_WANT_ANY, NULL, &r);
        CHECK(bs == IR_BUILDER_OK);
        if (bs == IR_BUILDER_OK) {
            CHECK(r.cat == IR_EXPR_LVALUE);
            CHECK(r.node->kind == IR_LOCAL);
            CHECK(r.type->kind == IRT_I32);
            attach_expr(b, block, e->span, r.node);
        }
        bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                   block, e, IR_EXPR_WANT_VALUE, NULL, &r);
        CHECK(bs == IR_BUILDER_OK);
        if (bs == IR_BUILDER_OK) {
            CHECK(r.cat == IR_EXPR_SCALAR);
            CHECK(r.node->kind == IR_LOAD);
            CHECK(r.node->u.load.lvalue->kind == IR_LOCAL);
            CHECK(r.type->kind == IRT_I32);
            CHECK(r.node->trap_code == NULL);   /* non-bool load: no trap */
            attach_expr(b, block, e->span, r.node);
        }
    }
    /* global g -> IR_GLOBAL LVALUE (lvalue of scalar type) */
    {
        IrNode *gn = find_decl(b, "main", "main.g");
        CHECK(gn != NULL && gn->kind == IR_GLOBAL_VAR);
    }
    /* literal 42 -> SCALAR; WANT_LVALUE rejected */
    e = stmt_expr(p.program, "f", 1);
    CHECK(e != NULL && e->kind == AST_EXPR_INT_LITERAL);
    if (e != NULL) {
        bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                   block, e, IR_EXPR_WANT_ANY, NULL, &r);
        CHECK(bs == IR_BUILDER_OK);
        if (bs == IR_BUILDER_OK) {
            CHECK(r.cat == IR_EXPR_SCALAR);
            CHECK(r.node->kind == IR_INT);
            attach_expr(b, block, e->span, r.node);
        }
        bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                   block, e, IR_EXPR_WANT_LVALUE, NULL, &r);
        CHECK(bs == IR_BUILDER_UNSUPPORTED);   /* no non-lvalue store */
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC2: member/arrow access - FIELD_ADDR on a struct lvalue base,
 * IR_DEREF + FIELD_ADDR for p->f, enum member access -> IR_ENUM_VAL. */
static void test_member_arrow_enum(void)
{
    static const char src[] =
        "module main;\n"
        "struct S { x: i32; y: u8; }\n"
        "enum Color: u8 { Red, Green }\n"
        "fn f(s: S, p: S*) -> void {\n"
        "  s.x; p->x; Color.Red;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* s.x -> FIELD_ADDR (LVALUE), WANT_VALUE -> LOAD(FIELD_ADDR) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_MEMBER);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_LVALUE);
                CHECK(r.node->kind == IR_FIELD_ADDR);
                CHECK(r.type->kind == IRT_I32);
                CHECK(r.node->u.field_addr.field_index == 0);
                CHECK(r.node->u.field_addr.base->kind == IR_LOCAL);
                attach_expr(b, block, e->span, r.node);
            }
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_LOAD);
                CHECK(r.node->u.load.lvalue->kind == IR_FIELD_ADDR);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* p->x -> DEREF + FIELD_ADDR */
    {
        const AstNode *e = stmt_expr(p.program, "f", 1);
        CHECK(e != NULL && e->kind == AST_EXPR_ARROW);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_FIELD_ADDR);
                CHECK(r.node->u.field_addr.base->kind == IR_DEREF);
                CHECK(r.node->u.field_addr.base->u.deref.ptr != NULL);
                CHECK(r.node->u.field_addr.base->trap_code != NULL &&
                      strcmp(r.node->u.field_addr.base->trap_code,
                             "AIC-R0809") == 0);
                attach_expr(b, block, e->span, r.node);
            }
            /* WANT_LVALUE on p->x is accepted (pointer-to-struct base) */
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_LVALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_LVALUE);
                CHECK(r.node->kind == IR_FIELD_ADDR);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* Color.Red -> IR_ENUM_VAL */
    {
        const AstNode *e = stmt_expr(p.program, "f", 2);
        CHECK(e != NULL && e->kind == AST_EXPR_MEMBER);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_ENUM_VAL);
                CHECK(r.type->kind == IRT_ENUM);
                CHECK(r.node->u.constant.value->kind == IRC_ENUM);
                CHECK(r.node->u.constant.value->u.en.value == 0);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC2: indexing and slicing - INDEX_ADDR on an array base (LVALUE,
 * constant index re-typed to usize, trap R0807), str indexing yields a
 * value address never an lvalue (WANT_LVALUE rejected), SLICE node
 * (trap R0807 array / R0808 str), LEN / PTR builtins. */
static void test_index_slice_len_ptr(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(arr: i32[4], buf: u8[16], s: str) -> void {\n"
        "  arr[1]; buf[0]; s[2]; arr[1..3]; s[1..2]; len(arr); ptr(arr);\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    size_t idx;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* arr[1] -> INDEX_ADDR LVALUE, index IR_INT usize, trap R0807 */
    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_INDEX);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_LVALUE);
                CHECK(r.node->kind == IR_INDEX_ADDR);
                CHECK(r.type->kind == IRT_I32);
                CHECK(r.node->trap_code != NULL &&
                      strcmp(r.node->trap_code, "AIC-R0807") == 0);
                CHECK(r.node->u.index_addr.index->kind == IR_INT);
                CHECK(r.node->u.index_addr.index->type->kind == IRT_USIZE);
                CHECK(r.node->u.index_addr.index->u.constant.value->
                          u.int_bits == 1);
                CHECK(r.node->u.index_addr.base->kind == IR_LOCAL);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* buf[0] -> element u8 */
    {
        const AstNode *e = stmt_expr(p.program, "f", 1);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_INDEX_ADDR);
                CHECK(r.type->kind == IRT_U8);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* s[2] -> str indexing: value address, never an lvalue */
    {
        const AstNode *e = stmt_expr(p.program, "f", 2);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_COMPOSITE);
                CHECK(r.node->kind == IR_INDEX_ADDR);
                CHECK(r.type->kind == IRT_U8);
                CHECK(r.node->trap_code != NULL &&
                      strcmp(r.node->trap_code, "AIC-R0807") == 0);
                attach_expr(b, block, e->span, r.node);
            }
            /* WANT_LVALUE on a str index is rejected (never an lvalue) */
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_LVALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_UNSUPPORTED);
            /* WANT_VALUE loads the byte value */
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_LOAD);
                CHECK(r.type->kind == IRT_U8);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* arr[1..3] -> IR_SLICE (u8... i32[]), trap R0807 */
    {
        const AstNode *e = stmt_expr(p.program, "f", 3);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_COMPOSITE);
                CHECK(r.node->kind == IR_SLICE);
                CHECK(r.type->kind == IRT_SLICE);
                CHECK(r.type->u.slice.elem->kind == IRT_I32);
                CHECK(r.node->trap_code != NULL &&
                      strcmp(r.node->trap_code, "AIC-R0807") == 0);
                CHECK(r.node->u.slice.start != NULL);
                CHECK(r.node->u.slice.start->kind == IR_INT);
                CHECK(r.node->u.slice.start->type->kind == IRT_USIZE);
                CHECK(r.node->u.slice.end != NULL);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* s[1..2] -> IR_SLICE of str, trap R0808 (code-point boundary) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 4);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_SLICE);
                CHECK(r.type->kind == IRT_STR);
                CHECK(r.node->trap_code != NULL &&
                      strcmp(r.node->trap_code, "AIC-R0808") == 0);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* len(arr) -> IR_LEN usize; ptr(arr) -> IR_PTR i32* */
    {
        static const IrNodeKind expect[] = { IR_LEN, IR_PTR };
        for (idx = 0; idx < 2; idx++) {
            const AstNode *e = stmt_expr(p.program, "f", 5 + idx);
            CHECK(e != NULL);
            if (e == NULL) {
                continue;
            }
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                continue;
            }
            CHECK(r.cat == IR_EXPR_SCALAR);
            CHECK(r.node->kind == expect[idx]);
            CHECK(r.node->u.unary.operand != NULL);
            if (expect[idx] == IR_LEN) {
                CHECK(r.type->kind == IRT_USIZE);
            } else {
                CHECK(r.type->kind == IRT_PTR);
                CHECK(r.type->u.ptr.elem->kind == IRT_I32);
            }
            attach_expr(b, block, e->span, r.node);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: user-function calls and pointer arithmetic - IR_CALL (callee,
 * args, return type), PTR_ADD / PTR_SUB (R0816), PTR_DIFF (isize,
 * R0816). */
static void test_call_and_pointer_arith(void)
{
    static const char src[] =
        "module main;\n"
        "fn add(x: i32, y: i32) -> i32 { return 0; }\n"
        "fn f(a: i32, p: i32*, q: i32*) -> void {\n"
        "  add(a, 1); p + 1; p - 1; p - q;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* add(a, 1) -> IR_CALL with callee main.add, 2 args, ret i32 */
    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_CALL);
        if (e != NULL) {
            IrNode *add_fn = find_decl(b, "main", "main.add");
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_CALL);
                CHECK(r.type->kind == IRT_I32);
                CHECK(r.node->u.call.callee == add_fn);
                CHECK(r.node->u.call.nargs == 2);
                CHECK(r.node->u.call.args[0]->kind == IR_LOAD);
                CHECK(r.node->u.call.args[1]->kind == IR_INT);
                CHECK(r.node->u.call.args[1]->type->kind == IRT_I32);
                attach_expr(b, block, e->span, r.node);
            }
            /* the non-void helper's tail must terminate for verify
             * (statement mapping is 16c1d's card) */
            terminate_fn_body(b, add_fn);
        }
    }
    /* p + 1 -> PTR_ADD (R0816); p - 1 -> PTR_SUB; p - q -> PTR_DIFF */
    {
        static const IrNodeKind expect[] = { IR_PTR_ADD, IR_PTR_SUB,
                                             IR_PTR_DIFF };
        static const char *traps[] = { "AIC-R0816", "AIC-R0816",
                                       "AIC-R0816" };
        size_t idx;
        for (idx = 0; idx < 3; idx++) {
            const AstNode *e = stmt_expr(p.program, "f", 1 + idx);
            CHECK(e != NULL);
            if (e == NULL) {
                continue;
            }
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                continue;
            }
            CHECK(r.cat == IR_EXPR_SCALAR);
            CHECK(r.node->kind == expect[idx]);
            CHECK(r.node->trap_code != NULL &&
                  strcmp(r.node->trap_code, traps[idx]) == 0);
            if (expect[idx] == IR_PTR_DIFF) {
                CHECK(r.type->kind == IRT_ISIZE);
                CHECK(r.node->u.binary.left->type->kind == IRT_PTR);
            } else {
                CHECK(r.type->kind == IRT_PTR);
                CHECK(r.type->u.ptr.elem->kind == IRT_I32);
                CHECK(r.node->u.ptr_arith.ptr->kind == IR_LOAD);
                CHECK(r.node->u.ptr_arith.ptr->type->kind == IRT_PTR);
                CHECK(r.node->u.ptr_arith.offset != NULL);
            }
            attach_expr(b, block, e->span, r.node);
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC3: runtime-call signature attachment (header gap note 7) - a
 * call to rt.io.write patches the runtime IR_FUNCTION with the spec
 * signature (3 params, usize return) and the IR_CALL args match. */
static void test_runtime_call_signatures(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.io;\n"
        "fn f(buf: u8[16], h: usize) -> void {\n"
        "  rt.io.write(h, buf[..], 5);\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block, *write_fn;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    write_fn = find_decl(b, "rt.io", "rt.io.write");
    CHECK(fn_sym != NULL && fn_node != NULL && write_fn != NULL);
    if (fn_sym == NULL || fn_node == NULL || write_fn == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_CALL);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_CALL);
                CHECK(r.node->u.call.callee == write_fn);
                /* patched signature */
                CHECK(write_fn->u.function.nparams == 3);
                CHECK(write_fn->u.function.params[0].type->kind ==
                      IRT_USIZE);
                CHECK(write_fn->u.function.params[1].type->kind ==
                      IRT_SLICE);
                CHECK(write_fn->u.function.params[1].type->u.slice.elem->
                          kind == IRT_U8);
                CHECK(write_fn->u.function.params[2].type->kind ==
                      IRT_USIZE);
                CHECK(write_fn->u.function.ret_type->kind == IRT_USIZE);
                CHECK(write_fn->u.function.nslots == 3);
                CHECK(write_fn->u.function.slots[0]->kind == IR_SLOT_PARAM);
                /* args: h (usize load), buf[..] (slice), 5 (usize) */
                CHECK(r.node->u.call.nargs == 3);
                CHECK(r.node->u.call.args[0]->type->kind == IRT_USIZE);
                CHECK(r.node->u.call.args[1]->kind == IR_SLICE);
                CHECK(r.node->u.call.args[1]->type->kind == IRT_SLICE);
                CHECK(r.node->u.call.args[2]->kind == IR_INT);
                CHECK(r.node->u.call.args[2]->type->kind == IRT_USIZE);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1 (reviewer2 MAJOR-3 t_a0d93e6d): 0-argument non-void runtime
 * functions (rt.io.stdin/stdout/stderr -> usize per spec 15.2,
 * rt.proc.args -> u8[][] per spec 15.3) must carry their spec return
 * type on the patched IR_FUNCTION and on the IR_CALL node (contract
 * 5.3: IR_CALL result type = callee return type). The 16c1b
 * placeholder has 0 params and void return, so a param-count match
 * (0 == 0) alone must NOT skip the ret_type patch; the invariant-5
 * unreachable IR_TRAP tail is appended with the patch. */
static void test_runtime_signatures_zero_arg(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.io;\n"
        "import rt.proc;\n"
        "fn f() -> void {\n"
        "  var h: usize = rt.io.stdin();\n"
        "  var o: usize = rt.io.stdout();\n"
        "  var e: usize = rt.io.stderr();\n"
        "  var a: u8[][] = rt.proc.args();\n"
        "}\n";
    static const struct {
        const char *var;
        const char *module_fqn;
        const char *fn_fqn;
        IrTypeKind expect_kind;
    } cases[] = {
        { "h", "rt.io", "rt.io.stdin", IRT_USIZE },
        { "o", "rt.io", "rt.io.stdout", IRT_USIZE },
        { "e", "rt.io", "rt.io.stderr", IRT_USIZE },
        { "a", "rt.proc", "rt.proc.args", IRT_SLICE },
    };
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    size_t ci;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const AstNode *e = var_init_expr(p.program, "f", cases[ci].var);
        IrNode *callee = find_decl(b, cases[ci].module_fqn,
                                   cases[ci].fn_fqn);
        CHECK(e != NULL && e->kind == AST_EXPR_CALL);
        CHECK(callee != NULL);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR ||
                      r.cat == IR_EXPR_COMPOSITE);
                CHECK(r.node->kind == IR_CALL);
                CHECK(r.node->type != NULL &&
                      r.node->type->kind == cases[ci].expect_kind);
                CHECK(r.node->u.call.callee == callee);
                attach_expr(b, block, e->span, r.node);
            }
        }
        if (callee != NULL) {
            /* the 0-arg signature patch must set ret_type even though
             * nparams already matches (0 == 0) */
            CHECK(callee->u.function.nparams == 0);
            CHECK(callee->u.function.ret_type != NULL &&
                  callee->u.function.ret_type->kind ==
                      cases[ci].expect_kind);
            /* invariant-5 trap tail appended with the ret_type patch */
            CHECK(callee->u.function.body != NULL &&
                  callee->u.function.body->kind == IR_BLOCK);
            if (callee->u.function.body != NULL &&
                callee->u.function.body->kind == IR_BLOCK) {
                CHECK(callee->u.function.body->u.block.nstmts == 1);
                CHECK(callee->u.function.body->u.block.nstmts > 0 &&
                      callee->u.function.body->u.block.stmts[0]->kind ==
                          IR_TRAP);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC3: struct and array literals - IR_ZERO + field/element stores
 * into a temporary image; literal-order field evaluation. */
static void test_struct_and_array_literals(void)
{
    static const char src[] =
        "module main;\n"
        "struct S { x: i32; y: i32; }\n"
        "fn f(a: i32) -> void {\n"
        "  var s: S = S { y: a, x: 1 };\n"
        "  var arr: i32[3] = [1, 2, 3];\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    size_t nzero, nstore, nfield;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;
    setup_fn_locals(&p, &ctx, fn_node, "f");

    /* var s: S = S { y: a, x: 1 }; -> ZERO + 2 field stores; the field
     * stores appear in literal order (y first, then x) */
    {
        const AstNode *e = var_init_expr(p.program, "f", "s");
        CHECK(e != NULL && e->kind == AST_EXPR_STRUCT_INIT);
        if (e != NULL) {
            IrType *st = NULL;
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE,
                                       st, &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_COMPOSITE);
                CHECK(r.node->kind == IR_LOCAL);
                CHECK(r.type->kind == IRT_STRUCT);
                attach_expr(b, block, e->span, r.node);
                nzero = count_kind(b, IR_ZERO);
                nstore = count_kind(b, IR_STORE);
                CHECK(nzero == 1);
                CHECK(nstore == 2);
                /* first store targets field 1 (y, literal order) */
                {
                    IrNode *s1 = NULL;
                    size_t i;
                    for (i = 0; i < block->u.block.nstmts; i++) {
                        IrNode *s = block->u.block.stmts[i];
                        if (s->kind == IR_STORE) {
                            s1 = s;
                            break;
                        }
                    }
                    CHECK(s1 != NULL);
                    if (s1 != NULL) {
                        CHECK(s1->u.store.dest->kind == IR_FIELD_ADDR);
                        CHECK(s1->u.store.dest->u.field_addr.field_index ==
                              1);
                        CHECK(s1->u.store.value->kind == IR_LOAD);
                    }
                }
            }
        }
    }
    /* var arr: i32[3] = [1, 2, 3]; -> ZERO + 3 element stores */
    {
        const AstNode *e = var_init_expr(p.program, "f", "arr");
        CHECK(e != NULL && e->kind == AST_EXPR_ARRAY_LITERAL);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE,
                                       ir_type_array(b, ir_type_i32(b), 3),
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_COMPOSITE);
                CHECK(r.node->kind == IR_LOCAL);
                CHECK(r.type->kind == IRT_ARRAY);
                CHECK(r.type->u.array.extent == 3);
                attach_expr(b, block, e->span, r.node);
                nzero = count_kind(b, IR_ZERO);
                nstore = count_kind(b, IR_STORE);
                nfield = count_kind(b, IR_FIELD_ADDR);
                CHECK(nzero == 2);
                CHECK(nstore == 5);
                CHECK(nfield == 2);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC3 (IRC-N1): the repetition form `[e; N]` evaluates e exactly once -
 * a call element produces exactly ONE IR_CALL and N element stores all
 * referencing the same call node; N == 0 still evaluates e once. */
static void test_repetition_eval_once(void)
{
    static const char src[] =
        "module main;\n"
        "fn make() -> i32 { return 7; }\n"
        "fn f() -> void {\n"
        "  var a: i32[4] = [make(); 4];\n"
        "  var z: i32[0] = [make(); 0];\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    size_t ncalls, nindex, i;
    IrNode *call_node = NULL;
    bool all_same = true;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* [make(); 4] -> exactly ONE call, 4 stores referencing it */
    {
        const AstNode *e = var_init_expr(p.program, "f", "a");
        CHECK(e != NULL && e->kind == AST_EXPR_ARRAY_LITERAL &&
              e->u.array_literal.count != NULL);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE,
                                       ir_type_array(b, ir_type_i32(b), 4),
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                attach_expr(b, block, e->span, r.node);
                ncalls = count_calls_to(b, "main.make");
                nindex = count_kind(b, IR_INDEX_ADDR);
                CHECK(ncalls == 1);
                CHECK(nindex == 4);
                /* every element store references the same call node */
                for (i = 0; i < block->u.block.nstmts; i++) {
                    IrNode *s = block->u.block.stmts[i];
                    if (s->kind == IR_STORE) {
                        if (call_node == NULL) {
                            call_node = s->u.store.value;
                        } else if (s->u.store.value != call_node) {
                            all_same = false;
                        }
                    }
                }
                CHECK(call_node != NULL && call_node->kind == IR_CALL);
                CHECK(all_same);
            }
        }
    }
    /* [make(); 0] -> still one evaluation (IRC-N1: e evaluated once even
     * when N == 0; the call node is appended to the block) */
    {
        const AstNode *e = var_init_expr(p.program, "f", "z");
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE,
                                       ir_type_array(b, ir_type_i32(b), 0),
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                attach_expr(b, block, e->span, r.node);
                CHECK(count_calls_to(b, "main.make") == 2);
                CHECK(count_kind(b, IR_INDEX_ADDR) == 4);   /* still 4 */
            }
        }
    }
    /* the non-void helper's tail must terminate for verify (statement
     * mapping is 16c1d's card) */
    terminate_fn_body(b, find_decl(b, "main", "main.make"));
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC2: assignment and compound assignment - IR_STORE with the
 * destination lvalue and the value; compound lowers to
 * STORE(dest, OP(LOAD(dest), src)) with the op's trap; pointer +=
 * lowers to STORE(dest, PTR_ADD(dest, offset)). */
static void test_assign_compound(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32, p: i32*) -> void {\n"
        "  a = 5; a += 2; a <<= 1; p += 1;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;
    size_t idx;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* a = 5 -> IR_STORE(local a, IR_INT 5); category EFFECT */
    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_ASSIGN);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_EFFECT);
                CHECK(r.node->kind == IR_STORE);
                CHECK(r.node->u.store.dest->kind == IR_LOCAL);
                CHECK(r.node->u.store.value->kind == IR_INT);
                CHECK(r.node->u.store.value->type->kind == IRT_I32);
                attach_expr(b, block, e->span, r.node);
            }
            /* assignment in a value position is unsupported (gap 3) */
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_UNSUPPORTED);
        }
    }
    /* a += 2 -> STORE(dest, IR_ADD(IR_LOAD(dest), temp)) with R0802:
     * the source is materialized into a temp so it evaluates before
     * the destination read (spec 10.4; reviewer2 MAJOR-1) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 1);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_STORE);
                CHECK(r.node->u.store.value->kind == IR_ADD);
                CHECK(r.node->u.store.value->trap_code != NULL &&
                      strcmp(r.node->u.store.value->trap_code,
                             "AIC-R0802") == 0);
                CHECK(r.node->u.store.value->u.binary.left->kind ==
                      IR_LOAD);
                CHECK(r.node->u.store.value->u.binary.left->
                          u.load.lvalue == r.node->u.store.dest);
                /* spec 10.4: the source is evaluated before the
                 * destination read - it is materialized into a temp (the
                 * op's right operand is a temp LOAD, not the source
                 * directly) whose store statement precedes the final
                 * store (reviewer2 MAJOR-1) */
                CHECK(r.node->u.store.value->u.binary.right->kind ==
                      IR_LOAD);
                CHECK(r.node->u.store.value->u.binary.right->
                          u.load.lvalue->kind == IR_LOCAL);
                attach_expr(b, block, e->span, r.node);
                {
                    IrNode *tmp_store = NULL;
                    size_t si;
                    for (si = 0; si < block->u.block.nstmts; si++) {
                        IrNode *s = block->u.block.stmts[si];
                        if (s->kind == IR_STORE) {
                            tmp_store = s;
                            break;
                        }
                    }
                    CHECK(tmp_store != NULL);
                    if (tmp_store != NULL) {
                        CHECK(tmp_store->u.store.dest ==
                              r.node->u.store.value->u.binary.right->
                                  u.load.lvalue);
                        CHECK(tmp_store->u.store.value->kind == IR_INT);
                    }
                }
            }
        }
    }
    /* a <<= 1 -> STORE(dest, IR_SHL(LOAD(dest), 1)) with R0804 */
    {
        const AstNode *e = stmt_expr(p.program, "f", 2);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_STORE);
                CHECK(r.node->u.store.value->kind == IR_SHL);
                CHECK(r.node->u.store.value->trap_code != NULL &&
                      strcmp(r.node->u.store.value->trap_code,
                             "AIC-R0804") == 0);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* p += 1 -> STORE(dest, PTR_ADD(LOAD(dest), temp)) with R0816:
     * the pointer operand is the loaded destination pointer VALUE
     * (contract 5.3/5.4; reviewer2 MAJOR-2), and the offset source is
     * materialized into a temp so the source evaluates before the
     * destination read (spec 10.4; reviewer2 MAJOR-1) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 3);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_STORE);
                CHECK(r.node->u.store.value->kind == IR_PTR_ADD);
                CHECK(r.node->u.store.value->trap_code != NULL &&
                      strcmp(r.node->u.store.value->trap_code,
                             "AIC-R0816") == 0);
                CHECK(r.node->u.store.value->u.ptr_arith.ptr->kind ==
                      IR_LOAD);
                CHECK(r.node->u.store.value->u.ptr_arith.ptr->
                          u.load.lvalue == r.node->u.store.dest);
                CHECK(r.node->u.store.value->u.ptr_arith.ptr->type !=
                          NULL &&
                      r.node->u.store.value->u.ptr_arith.ptr->type->kind ==
                          IRT_PTR);
                CHECK(r.node->u.store.value->u.ptr_arith.offset->kind ==
                      IR_LOAD);
                CHECK(r.node->u.store.value->u.ptr_arith.offset->
                          u.load.lvalue->kind == IR_LOCAL);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    (void)idx;
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1/AC3 (reviewer2 MAJOR-1 t_a0d93e6d): compound assignment must
 * evaluate the source BEFORE the destination read (spec 10.4:
 * destination location, then b, then read the destination, apply op,
 * then store). The IR's fixed per-node child order (IR_STORE:
 * destination then value; the binary op: left then right) cannot
 * express that inside a single STORE tree, so the source is
 * materialized into a temporary: block order becomes
 *   STORE(tmp, src); STORE(dest, OP(LOAD(dest), LOAD(tmp))).
 * With a side-effecting source this is observable: `g += bump()` where
 * bump() sets g = 100 and returns 1 must compute 100 + 1 == 101
 * (destination read AFTER the source), not 1 + 1 == 2. */
static void test_compound_eval_order(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = 1;\n"
        "fn bump() -> i32 { g = 100; return 1; }\n"
        "fn f() -> void {\n"
        "  g += bump();\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    memset(&r, 0, sizeof(r));
    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        IrNode *tmp_store, *final_store, *op;
        CHECK(e != NULL && e->kind == AST_EXPR_ASSIGN);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                ir_build_free(b);
                pipeline_free(&p);
                return;
            }
            CHECK(r.cat == IR_EXPR_EFFECT);
            CHECK(r.node->kind == IR_STORE);
            attach_expr(b, block, e->span, r.node);
        }
        /* block order: the source-materialization store runs first,
         * then the final store (the block's statement order is the
         * evaluation order, contract 6.1 = spec 10.4) */
        CHECK(block->u.block.nstmts == 2);
        if (block->u.block.nstmts < 2) {
            ir_build_free(b);
            pipeline_free(&p);
            return;
        }
        tmp_store = block->u.block.stmts[0];
        CHECK(tmp_store->kind == IR_STORE);
        CHECK(tmp_store->u.store.dest->kind == IR_LOCAL);
        CHECK(tmp_store->u.store.value->kind == IR_CALL);
        CHECK(tmp_store->u.store.value->u.call.callee != NULL &&
              tmp_store->u.store.value->u.call.callee->u.function.name !=
                  NULL &&
              strcmp(tmp_store->u.store.value->u.call.callee->
                         u.function.name, "main.bump") == 0);
        CHECK(block->u.block.stmts[1]->kind == IR_EXPR_STMT);
        final_store = block->u.block.stmts[1]->u.expr_stmt.expr;
        CHECK(final_store != NULL && final_store->kind == IR_STORE);
        CHECK(final_store->u.store.dest->kind == IR_GLOBAL);
        op = final_store->u.store.value;
        CHECK(op != NULL && op->kind == IR_ADD);
        if (op != NULL && op->kind == IR_ADD && r.node != NULL) {
            /* the destination read follows the source evaluation: the
             * op's left (destination read) is a LOAD of the final
             * store's dest; the op's right is a LOAD of the temp that
             * holds the already-evaluated source */
            CHECK(op->u.binary.left->kind == IR_LOAD);
            CHECK(op->u.binary.left->u.load.lvalue ==
                  final_store->u.store.dest);
            CHECK(op->u.binary.right->kind == IR_LOAD);
            CHECK(op->u.binary.right->u.load.lvalue ==
                  tmp_store->u.store.dest);
            CHECK(final_store == r.node);
        }
    }
    terminate_fn_body(b, find_decl(b, "main", "main.bump"));
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1 (reviewer2 NEW MAJOR t_9002530a): compound assignment with a
 * side-effecting DESTINATION LOCATION must evaluate the destination
 * location BEFORE the source (spec 10.4: destination location, then b,
 * then read the destination, apply op, then store). `*getp() += bump()`
 * where getp() is a call: the location's pointer operand is materialized
 * into a temp AHEAD of the source, so the block order becomes
 *   STORE(tmp_ptr, CALL getp); STORE(tmp_src, CALL bump);
 *   STORE(DEREF(LOAD(tmp_ptr)), ADD(LOAD(DEREF(LOAD(tmp_ptr))), LOAD(tmp_src)))
 * Observable: with getp() setting g = 200 and bump() setting g = 100,
 * the spec order (getp() first) leaves g = 200; the pre-fix order
 * (bump() first) leaves g = 100. */
static void test_compound_dest_location_order(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = 1;\n"
        "var cell: i32 = 5;\n"
        "fn bump() -> i32 { g = 100; return 1; }\n"
        "fn getp() -> i32* { g = 200; return null; }\n"
        "fn f() -> void {\n"
        "  *getp() += bump();\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    memset(&r, 0, sizeof(r));
    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        IrNode *loc_store, *src_store, *final_stmt;
        CHECK(e != NULL && e->kind == AST_EXPR_ASSIGN);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                ir_build_free(b);
                pipeline_free(&p);
                return;
            }
            CHECK(r.cat == IR_EXPR_EFFECT);
            CHECK(r.node->kind == IR_STORE);
            attach_expr(b, block, e->span, r.node);
        }
        /* block order (the block's statement order is the evaluation
         * order, contract 6.1 = spec 10.4): the destination location's
         * side-effecting pointer operand materializes FIRST, then the
         * source, then the final store (reviewer2 NEW MAJOR) */
        CHECK(block->u.block.nstmts == 3);
        if (block->u.block.nstmts < 3) {
            ir_build_free(b);
            pipeline_free(&p);
            return;
        }
        loc_store = block->u.block.stmts[0];
        CHECK(loc_store->kind == IR_STORE);
        CHECK(loc_store->u.store.dest->kind == IR_LOCAL);
        CHECK(loc_store->u.store.dest->type != NULL &&
              loc_store->u.store.dest->type->kind == IRT_PTR);
        CHECK(loc_store->u.store.value->kind == IR_CALL);
        CHECK(loc_store->u.store.value->u.call.callee != NULL &&
              loc_store->u.store.value->u.call.callee->u.function.name !=
                  NULL &&
              strcmp(loc_store->u.store.value->u.call.callee->
                         u.function.name, "main.getp") == 0);
        src_store = block->u.block.stmts[1];
        CHECK(src_store->kind == IR_STORE);
        CHECK(src_store->u.store.dest->kind == IR_LOCAL);
        CHECK(src_store->u.store.value->kind == IR_CALL);
        CHECK(src_store->u.store.value->u.call.callee != NULL &&
              src_store->u.store.value->u.call.callee->u.function.name !=
                  NULL &&
              strcmp(src_store->u.store.value->u.call.callee->
                         u.function.name, "main.bump") == 0);
        final_stmt = block->u.block.stmts[2];
        CHECK(final_stmt->kind == IR_EXPR_STMT);
        {
            IrNode *final_store = final_stmt->u.expr_stmt.expr;
            IrNode *op;
            CHECK(final_store != NULL && final_store->kind == IR_STORE);
            CHECK(final_store->u.store.dest->kind == IR_DEREF);
            /* the destination location is DEREF(LOAD(tmp_ptr)): the
             * pointer operand is the loaded materialized pointer, so the
             * location was already evaluated at stmt 0 (getp() runs
             * before bump()) */
            CHECK(final_store->u.store.dest->u.deref.ptr->kind == IR_LOAD);
            CHECK(final_store->u.store.dest->u.deref.ptr->u.load.lvalue ==
                  loc_store->u.store.dest);
            op = final_store->u.store.value;
            CHECK(op != NULL && op->kind == IR_ADD);
            if (op != NULL && op->kind == IR_ADD) {
                /* the destination read follows the location and the
                 * source: op.left = LOAD(DEREF(LOAD(tmp_ptr))) (read of
                 * the materialized location), op.right = LOAD(tmp_src) */
                CHECK(op->u.binary.left->kind == IR_LOAD);
                CHECK(op->u.binary.left->u.load.lvalue ==
                      final_store->u.store.dest);
                CHECK(op->u.binary.right->kind == IR_LOAD);
                CHECK(op->u.binary.right->u.load.lvalue ==
                      src_store->u.store.dest);
                CHECK(final_store == r.node);
            }
        }
    }
    terminate_fn_body(b, find_decl(b, "main", "main.bump"));
    /* getp returns i32*: the shared terminate_fn_body appends IR_INT 0
     * (valid only for integer returns), so terminate it with an IR_NULL
     * pointer return instead */
    {
        IrNode *gfn = find_decl(b, "main", "main.getp");
        if (gfn != NULL && gfn->u.function.body != NULL) {
            IrNode *val = ir_node_new(b, IR_NULL, gfn->span);
            IrNode *ret;
            if (val != NULL) {
                val->type = gfn->u.function.ret_type;
                ir_node_add_cause(b, val, "AST_EXPR_NULL", gfn->span,
                                  -1, -1, -1);
                ret = ir_node_new(b, IR_RETURN, gfn->span);
                if (ret != NULL) {
                    ret->u.return_stmt.value = val;
                    ir_node_add_cause(b, ret, "AST_RETURN", gfn->span,
                                      -1, -1, -1);
                    ir_block_add_stmt(b, gfn->u.function.body, ret);
                }
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1 (reviewer2 NEW MAJOR t_e1fc0c67): compound assignment whose
 * destination location is a pure read of mutable global storage that
 * the source can mutate must evaluate the destination location BEFORE
 * the source (spec 10.4: destination location, then b). `*gp += bump()`
 * with gp a global pointer and bump() reassigning gp: the pointer read
 * (part of the location) is materialized into a temp AHEAD of the
 * source, so the block order becomes
 *   STORE(tmp_ptr, LOAD(GLOBAL gp)); STORE(tmp_src, CALL bump);
 *   STORE(DEREF(LOAD(tmp_ptr)), ADD(LOAD(DEREF(LOAD(tmp_ptr))), LOAD(tmp_src)))
 * Observable: with gp = &cell initially and bump() setting gp = &other,
 * the spec order stores into cell (the location chosen before bump);
 * the pre-fix order reads gp after bump and stores into other - the
 * "wrong cell updated if the pointer choice depends on the side
 * effect" aliasing case. */
static void test_compound_dest_global_ptr_order(void)
{
    static const char src[] =
        "module main;\n"
        "var cell: i32 = 5;\n"
        "var other: i32 = 9;\n"
        "var gp: i32* = &cell;\n"
        "fn bump() -> i32 { gp = &other; return 1; }\n"
        "fn f() -> void {\n"
        "  *gp += bump();\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    memset(&r, 0, sizeof(r));
    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        IrNode *loc_store, *src_store, *final_stmt;
        CHECK(e != NULL && e->kind == AST_EXPR_ASSIGN);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                ir_build_free(b);
                pipeline_free(&p);
                return;
            }
            CHECK(r.cat == IR_EXPR_EFFECT);
            CHECK(r.node->kind == IR_STORE);
            attach_expr(b, block, e->span, r.node);
        }
        /* block order (the block's statement order is the evaluation
         * order, contract 6.1 = spec 10.4): the destination location's
         * global-pointer read materializes FIRST (before the source),
         * then the source, then the final store (reviewer2 NEW MAJOR
         * alias-through-global) */
        CHECK(block->u.block.nstmts == 3);
        if (block->u.block.nstmts < 3) {
            ir_build_free(b);
            pipeline_free(&p);
            return;
        }
        loc_store = block->u.block.stmts[0];
        CHECK(loc_store->kind == IR_STORE);
        CHECK(loc_store->u.store.dest->kind == IR_LOCAL);
        CHECK(loc_store->u.store.dest->type != NULL &&
              loc_store->u.store.dest->type->kind == IRT_PTR);
        CHECK(loc_store->u.store.value->kind == IR_LOAD);
        CHECK(loc_store->u.store.value->u.load.lvalue->kind == IR_GLOBAL);
        CHECK(loc_store->u.store.value->u.load.lvalue->u.global.target !=
                  NULL &&
              loc_store->u.store.value->u.load.lvalue->u.global.target->
                  u.global_var.name != NULL &&
              strcmp(loc_store->u.store.value->u.load.lvalue->
                         u.global.target->u.global_var.name, "main.gp") == 0);
        src_store = block->u.block.stmts[1];
        CHECK(src_store->kind == IR_STORE);
        CHECK(src_store->u.store.dest->kind == IR_LOCAL);
        CHECK(src_store->u.store.value->kind == IR_CALL);
        CHECK(src_store->u.store.value->u.call.callee != NULL &&
              src_store->u.store.value->u.call.callee->u.function.name !=
                  NULL &&
              strcmp(src_store->u.store.value->u.call.callee->
                         u.function.name, "main.bump") == 0);
        final_stmt = block->u.block.stmts[2];
        CHECK(final_stmt->kind == IR_EXPR_STMT);
        {
            IrNode *final_store = final_stmt->u.expr_stmt.expr;
            IrNode *op;
            CHECK(final_store != NULL && final_store->kind == IR_STORE);
            CHECK(final_store->u.store.dest->kind == IR_DEREF);
            /* the destination location is DEREF(LOAD(tmp_ptr)): the
             * pointer operand is the loaded materialized pointer, so
             * the location (the gp read) was already evaluated at
             * stmt 0 before bump() ran */
            CHECK(final_store->u.store.dest->u.deref.ptr->kind == IR_LOAD);
            CHECK(final_store->u.store.dest->u.deref.ptr->u.load.lvalue ==
                  loc_store->u.store.dest);
            op = final_store->u.store.value;
            CHECK(op != NULL && op->kind == IR_ADD);
            if (op != NULL && op->kind == IR_ADD) {
                /* the destination read follows the location and the
                 * source: op.left = LOAD(DEREF(LOAD(tmp_ptr))) (read of
                 * the materialized location), op.right = LOAD(tmp_src) */
                CHECK(op->u.binary.left->kind == IR_LOAD);
                CHECK(op->u.binary.left->u.load.lvalue ==
                      final_store->u.store.dest);
                CHECK(op->u.binary.right->kind == IR_LOAD);
                CHECK(op->u.binary.right->u.load.lvalue ==
                      src_store->u.store.dest);
                CHECK(final_store == r.node);
            }
        }
    }
    terminate_fn_body(b, find_decl(b, "main", "main.bump"));
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1 (reviewer2 NEW MAJOR t_e1fc0c67): compound assignment whose
 * destination location is a pure read of mutable global storage that
 * the source can mutate - indexed form. `a[giu] += bump()` with giu a
 * global usize and bump() mutating giu: the index read (part of the
 * destination location per spec 10.4 indexing "evaluate a, then i")
 * is materialized into a temp AHEAD of the source, so the block order
 * becomes
 *   STORE(tmp_idx, LOAD(GLOBAL giu)); STORE(tmp_src, CALL bump);
 *   STORE(INDEX_ADDR(GLOBAL a, LOAD(tmp_idx)), ADD(...))
 * Observable: with giu = 1 and bump() setting giu = 2, the spec order
 * updates a[1]; the pre-fix order reads giu after bump and updates
 * a[2]. */
static void test_compound_dest_global_index_order(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32 = 1;\n"
        "var giu: usize = 1;\n"
        "var a: i32[4] = [0, 0, 0, 0];\n"
        "fn bump() -> i32 { giu = 2; g = 100; return 1; }\n"
        "fn f() -> void {\n"
        "  a[giu] += bump();\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    memset(&r, 0, sizeof(r));
    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        IrNode *loc_store, *src_store, *final_stmt;
        CHECK(e != NULL && e->kind == AST_EXPR_ASSIGN);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_ANY, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs != IR_BUILDER_OK) {
                ir_build_free(b);
                pipeline_free(&p);
                return;
            }
            CHECK(r.cat == IR_EXPR_EFFECT);
            CHECK(r.node->kind == IR_STORE);
            attach_expr(b, block, e->span, r.node);
        }
        /* block order (the block's statement order is the evaluation
         * order, contract 6.1 = spec 10.4): the destination location's
         * global-index read materializes FIRST (before the source),
         * then the source, then the final store (reviewer2 NEW MAJOR
         * alias-through-global) */
        CHECK(block->u.block.nstmts == 3);
        if (block->u.block.nstmts < 3) {
            ir_build_free(b);
            pipeline_free(&p);
            return;
        }
        loc_store = block->u.block.stmts[0];
        CHECK(loc_store->kind == IR_STORE);
        CHECK(loc_store->u.store.dest->kind == IR_LOCAL);
        CHECK(loc_store->u.store.dest->type != NULL &&
              loc_store->u.store.dest->type->kind == IRT_USIZE);
        CHECK(loc_store->u.store.value->kind == IR_LOAD);
        CHECK(loc_store->u.store.value->u.load.lvalue->kind == IR_GLOBAL);
        CHECK(loc_store->u.store.value->u.load.lvalue->u.global.target !=
                  NULL &&
              loc_store->u.store.value->u.load.lvalue->u.global.target->
                  u.global_var.name != NULL &&
              strcmp(loc_store->u.store.value->u.load.lvalue->
                         u.global.target->u.global_var.name, "main.giu") == 0);
        src_store = block->u.block.stmts[1];
        CHECK(src_store->kind == IR_STORE);
        CHECK(src_store->u.store.dest->kind == IR_LOCAL);
        CHECK(src_store->u.store.value->kind == IR_CALL);
        CHECK(src_store->u.store.value->u.call.callee != NULL &&
              src_store->u.store.value->u.call.callee->u.function.name !=
                  NULL &&
              strcmp(src_store->u.store.value->u.call.callee->
                         u.function.name, "main.bump") == 0);
        final_stmt = block->u.block.stmts[2];
        CHECK(final_stmt->kind == IR_EXPR_STMT);
        {
            IrNode *final_store = final_stmt->u.expr_stmt.expr;
            IrNode *op;
            CHECK(final_store != NULL && final_store->kind == IR_STORE);
            CHECK(final_store->u.store.dest->kind == IR_INDEX_ADDR);
            /* the destination location is INDEX_ADDR(GLOBAL a,
             * LOAD(tmp_idx)): the base is the array's global ref and
             * the index operand is the loaded materialized index, so
             * the location (the giu read) was already evaluated at
             * stmt 0 before bump() ran */
            CHECK(final_store->u.store.dest->u.index_addr.base->kind ==
                  IR_GLOBAL);
            CHECK(final_store->u.store.dest->u.index_addr.base->
                      u.global.target != NULL &&
                  final_store->u.store.dest->u.index_addr.base->
                      u.global.target->u.global_var.name != NULL &&
                  strcmp(final_store->u.store.dest->u.index_addr.base->
                             u.global.target->u.global_var.name,
                         "main.a") == 0);
            CHECK(final_store->u.store.dest->u.index_addr.index->kind ==
                  IR_LOAD);
            CHECK(final_store->u.store.dest->u.index_addr.index->
                      u.load.lvalue == loc_store->u.store.dest);
            op = final_store->u.store.value;
            CHECK(op != NULL && op->kind == IR_ADD);
            if (op != NULL && op->kind == IR_ADD) {
                /* the destination read follows the location and the
                 * source: op.left = LOAD(INDEX_ADDR(GLOBAL a,
                 * LOAD(tmp_idx))) (read of the materialized location),
                 * op.right = LOAD(tmp_src) */
                CHECK(op->u.binary.left->kind == IR_LOAD);
                CHECK(op->u.binary.left->u.load.lvalue ==
                      final_store->u.store.dest);
                CHECK(op->u.binary.right->kind == IR_LOAD);
                CHECK(op->u.binary.right->u.load.lvalue ==
                      src_store->u.store.dest);
                CHECK(final_store == r.node);
            }
        }
    }
    terminate_fn_body(b, find_decl(b, "main", "main.bump"));
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC1: ternary, cast/wrap, sizeof/alignof - IR_SELECT (bool condition,
 * same-typed branches), IR_CAST (R0801) / IR_WRAP, sizeof/alignof as
 * IR_INT(usize) constants. */
static void test_ternary_cast_sizeof(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32, c: bool) -> void {\n"
        "  c ? a : 3; cast<i64>(a); wrap<u8>(a); sizeof(i32); "
        "alignof(i32);\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* c ? a : 3 -> IR_SELECT(cond bool, then LOAD, else IR_INT) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_TERNARY);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_SELECT);
                CHECK(r.type->kind == IRT_I32);
                CHECK(r.node->u.select.cond->kind == IR_LOAD);
                CHECK(r.node->u.select.cond->type->kind == IRT_BOOL);
                CHECK(r.node->u.select.then_value->kind == IR_LOAD);
                CHECK(r.node->u.select.else_value->kind == IR_INT);
                CHECK(r.node->u.select.else_value->type->kind == IRT_I32);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* cast<i64>(a) -> IR_CAST (R0801), result i64 */
    {
        const AstNode *e = stmt_expr(p.program, "f", 1);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_CAST);
                CHECK(r.type->kind == IRT_I64);
                CHECK(r.node->trap_code != NULL &&
                      strcmp(r.node->trap_code, "AIC-R0801") == 0);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* wrap<u8>(a) -> IR_WRAP, never traps */
    {
        const AstNode *e = stmt_expr(p.program, "f", 2);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_WRAP);
                CHECK(r.type->kind == IRT_U8);
                CHECK(r.node->trap_code == NULL);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    /* sizeof(i32) / alignof(i32) -> IR_INT(usize) with the spec facts */
    {
        static const uint64_t expect[] = { 4, 4 };
        size_t idx;
        for (idx = 0; idx < 2; idx++) {
            const AstNode *e = stmt_expr(p.program, "f", 3 + idx);
            CHECK(e != NULL);
            if (e == NULL) {
                continue;
            }
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.cat == IR_EXPR_SCALAR);
                CHECK(r.node->kind == IR_INT);
                CHECK(r.type->kind == IRT_USIZE);
                CHECK(r.node->u.constant.value->u.int_bits ==
                      expect[idx]);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* AC3: defensive IR_BUILDER_UNSUPPORTED with nothing owned for the
 * disclosed representable-surface gaps. */
static void test_defensive_unsupported(void)
{
    static const char src[] =
        "module main;\n"
        "fn f(a: i32, p: i32*, sl: u8[], i: i32, b: i8) -> void {\n"
        "  &a; &*p; sl[i]; b < b;\n"
        "}\n";
    Pipeline p;
    IrBuild *b = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx;
    IrNode *fn_node, *block;
    IrExprResult r;
    const NameSymbol *fn_sym;

    bs = pipeline_build(&p, src, &b);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b != NULL);
    if (bs != IR_BUILDER_OK || b == NULL) {
        pipeline_free(&p);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.result = p.result;
    ctx.layout = p.build;
    ctx.build = b;
    fn_sym = find_fn_sym(&p, "f");
    fn_node = find_decl(b, "main", "main.f");
    CHECK(fn_sym != NULL && fn_node != NULL);
    if (fn_sym == NULL || fn_node == NULL) {
        ir_build_free(b);
        pipeline_free(&p);
        return;
    }
    block = fn_node->u.function.body;

    /* &a (address-of a plain scalar param) -> UNSUPPORTED (gap 1) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 0);
        CHECK(e != NULL && e->kind == AST_EXPR_UNARY);
        if (e != NULL) {
            memset(&r, 0xAA, sizeof(r));
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_UNSUPPORTED);
            CHECK(r.node == NULL);
        }
    }
    /* &*p (address-of a dereference) -> UNSUPPORTED (gap 1) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 1);
        if (e != NULL) {
            memset(&r, 0xAA, sizeof(r));
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_UNSUPPORTED);
            CHECK(r.node == NULL);
        }
    }
    /* sl[i] with an i32 runtime index -> UNSUPPORTED (gap 4) */
    {
        const AstNode *e = stmt_expr(p.program, "f", 2);
        if (e != NULL) {
            memset(&r, 0xAA, sizeof(r));
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_UNSUPPORTED);
            CHECK(r.node == NULL);
        }
    }
    /* b < b where b: i8 - same-type runtime comparison is supported */
    {
        const AstNode *e = stmt_expr(p.program, "f", 3);
        if (e != NULL) {
            bs = ir_builder_expr_lower(&ctx, p.result->modules[0], fn_sym,
                                       block, e, IR_EXPR_WANT_VALUE, NULL,
                                       &r);
            CHECK(bs == IR_BUILDER_OK);
            if (bs == IR_BUILDER_OK) {
                CHECK(r.node->kind == IR_LT);
                attach_expr(b, block, e->span, r.node);
            }
        }
    }
    verify_ok(b);
    ir_build_free(b);
    pipeline_free(&p);
}

/* Determinism + invariants end to end: a representative build with
 * lowered expressions passes ir_dump_verify (round-trip byte-identical
 * + no AIC-I0501), and two builds of the same source produce
 * byte-identical dumps. */
static void test_verify_roundtrip(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.io;\n"
        "struct S { x: i32; y: i32; }\n"
        "enum Color: u8 { Red, Green }\n"
        "fn add(a: i32, b: i32) -> i32 { return 0; }\n"
        "fn f(c: bool, p: S*, h: usize) -> void {\n"
        "  var s: S = S { x: 1, y: 2 };\n"
        "  var arr: i32[4] = [1, 2, 3, 4];\n"
        "  var rep: i32[4] = [add(1, 2); 4];\n"
        "  var q: i32* = null;\n"
        "  s.x + 1; c ? 1 : 2; p->x; Color.Red;\n"
        "  var buf: u8[16] = [0u8; 16];\n"
        "  rt.io.write(h, buf[..], 4);\n"
        "}\n";
    Pipeline p1, p2;
    IrBuild *b1 = NULL, *b2 = NULL;
    IrBuilderStatus bs;
    BuilderCtx ctx1, ctx2;
    IrNode *fn_node;
    const NameSymbol *fn_sym;
    DiagBuf d1, d2;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;
    size_t i;

    diag_buf_init(&d1);
    diag_buf_init(&d2);

    /* build 1 */
    bs = pipeline_build(&p1, src, &b1);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b1 != NULL);
    if (bs == IR_BUILDER_OK && b1 != NULL) {
        memset(&ctx1, 0, sizeof(ctx1));
        ctx1.result = p1.result;
        ctx1.layout = p1.build;
        ctx1.build = b1;
        fn_sym = find_fn_sym(&p1, "f");
        fn_node = find_decl(b1, "main", "main.f");
        if (fn_sym != NULL && fn_node != NULL) {
            IrNode *block = fn_node->u.function.body;
            setup_fn_locals(&p1, &ctx1, fn_node, "f");
            for (i = 0; i < 7; i++) {
                const AstNode *e = stmt_expr(p1.program, "f", i);
                IrExprResult r;
                if (e == NULL) {
                    continue;
                }
                bs = ir_builder_expr_lower(&ctx1, p1.result->modules[0],
                                           fn_sym, block, e,
                                           IR_EXPR_WANT_ANY, NULL, &r);
                CHECK(bs == IR_BUILDER_OK);
                if (bs == IR_BUILDER_OK) {
                    attach_expr(b1, block, e->span, r.node);
                }
            }
            /* the non-void helper's tail must terminate for verify
             * (statement mapping is 16c1d's card) */
            terminate_fn_body(b1, find_decl(b1, "main", "main.add"));
        }
        CHECK(ir_dump_verify(b1, &recs, &nrecs) == IR_OK);
        ir_records_free(recs, nrecs);
        CHECK(ir_dump_write(b1, &d1));
        ir_build_free(b1);
        b1 = NULL;
    }

    /* build 2: identical lowering -> byte-identical dump */
    bs = pipeline_build(&p2, src, &b2);
    CHECK(bs == IR_BUILDER_OK);
    CHECK(b2 != NULL);
    if (bs == IR_BUILDER_OK && b2 != NULL) {
        memset(&ctx2, 0, sizeof(ctx2));
        ctx2.result = p2.result;
        ctx2.layout = p2.build;
        ctx2.build = b2;
        fn_sym = find_fn_sym(&p2, "f");
        fn_node = find_decl(b2, "main", "main.f");
        if (fn_sym != NULL && fn_node != NULL) {
            IrNode *block = fn_node->u.function.body;
            setup_fn_locals(&p2, &ctx2, fn_node, "f");
            for (i = 0; i < 7; i++) {
                const AstNode *e = stmt_expr(p2.program, "f", i);
                IrExprResult r;
                if (e == NULL) {
                    continue;
                }
                bs = ir_builder_expr_lower(&ctx2, p2.result->modules[0],
                                           fn_sym, block, e,
                                           IR_EXPR_WANT_ANY, NULL, &r);
                if (bs == IR_BUILDER_OK) {
                    attach_expr(b2, block, e->span, r.node);
                }
            }
            terminate_fn_body(b2, find_decl(b2, "main", "main.add"));
        }
        CHECK(ir_dump_write(b2, &d2));
        CHECK(d1.len == d2.len);
        CHECK(d1.len == d2.len &&
              memcmp(d1.data, d2.data, d1.len) == 0);
        diag_buf_free(&d2);
    }
    if (b1 != NULL) {
        ir_build_free(b1);
    }
    if (b2 != NULL) {
        ir_build_free(b2);
    }
    if (d1.data != NULL) {
        diag_buf_free(&d1);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

int main(void)
{
    ir_builder_decl_install();
    ir_builder_expr_install();

    test_scalar_constants();
    fprintf(stderr, "after test_scalar_constants\n");
    test_arith_and_traps();
    fprintf(stderr, "after test_arith_and_traps\n");
    test_comparisons();
    fprintf(stderr, "after test_comparisons\n");
    test_value_categories();
    fprintf(stderr, "after test_value_categories\n");
    test_member_arrow_enum();
    fprintf(stderr, "after test_member_arrow_enum\n");
    test_index_slice_len_ptr();
    fprintf(stderr, "after test_index_slice_len_ptr\n");
    test_call_and_pointer_arith();
    fprintf(stderr, "after test_call_and_pointer_arith\n");
    test_runtime_call_signatures();
    fprintf(stderr, "after test_runtime_call_signatures\n");
    test_runtime_signatures_zero_arg();
    fprintf(stderr, "after test_runtime_signatures_zero_arg\n");
    test_struct_and_array_literals();
    fprintf(stderr, "after test_struct_and_array_literals\n");
    test_repetition_eval_once();
    fprintf(stderr, "after test_repetition_eval_once\n");
    test_assign_compound();
    fprintf(stderr, "after test_assign_compound\n");
    test_compound_eval_order();
    fprintf(stderr, "after test_compound_eval_order\n");
    test_compound_dest_location_order();
    fprintf(stderr, "after test_compound_dest_location_order\n");
    test_compound_dest_global_ptr_order();
    fprintf(stderr, "after test_compound_dest_global_ptr_order\n");
    test_compound_dest_global_index_order();
    fprintf(stderr, "after test_compound_dest_global_index_order\n");
    test_ternary_cast_sizeof();
    fprintf(stderr, "after test_ternary_cast_sizeof\n");
    test_defensive_unsupported();
    fprintf(stderr, "after test_defensive_unsupported\n");
    test_verify_roundtrip();
    fprintf(stderr, "after test_verify_roundtrip\n");

    /* restore the defensive default stubs so later tests in the same
     * binary run against them (single-build compiler convention) */
    ir_builder_set_module_mapper(NULL);
    ir_builder_set_decl_mapper(NULL);
    ir_builder_set_body_mapper(NULL);

    if (g_failures) {
        fprintf(stderr, "ir_builder_expr_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("ir_builder_expr_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
