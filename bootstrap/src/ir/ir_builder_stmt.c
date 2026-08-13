/* bootstrap/src/ir/ir_builder_stmt.c
 *
 * AI-Co Stage-0 IR builder Phase B statement mapping and terminators
 * (WP-M0-16c1d).
 *
 * Implements the Phase B statement mapping of the accepted canonical IR
 * contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) sections
 * 5.2, 5.6, 6.1 and 12.1 over the resolved, validated build, as
 * declared in ir_builder_stmt.h:
 *
 *   - statements of contract 5.2 (IR_BLOCK, IR_LOCAL_DECL, IR_IF /
 *     IR_WHILE / IR_FOR first-class, IR_SWITCH / IR_CASE / IR_DEFAULT,
 *     IR_BREAK / IR_CONTINUE with enclosing-stack target resolution,
 *     IR_RETURN, IR_EXPR_STMT, IR_EMPTY, IR_CALL_TERM for noreturn
 *     runtime calls);
 *   - terminator rules of contract 5.6 (case/default bodies and
 *     non-void tails terminate; no statement after a terminator; void
 *     tail fall-off allowed): the mapper maps accepted builds whose
 *     reachability was verified pre-IR (AIC-E0412/E0416/E0417) and
 *     defensively refuses malformed input with nothing owned;
 *   - the storage model of contract 4.3: local slots created in
 *     first-declaration order (source order) and registered with
 *     ir_builder_expr_register_local before any statement referencing
 *     them is lowered;
 *   - the deterministic two-phase construction: the driver skeleton
 *     (16c1a) calls this body mapper once per function in canonical
 *     order; node ids are assigned in construction order (the AST
 *     walk order), so identical ASTs produce identical IR.
 *
 * Expression positions are lowered through the 16c1c expression
 * lowerer (ir_builder_expr_to_value / to_any), which appends its
 * intermediate nodes to the current block (block-appending
 * convention), so the block's statement order is the evaluation order.
 * Positions evaluated exactly once at their point (if/switch
 * conditions, initializers, return values, expression statements) may
 * carry appended intermediates in the enclosing block. Positions
 * evaluated once PER ITERATION (while/for conditions, the for step)
 * have no per-iteration statement list in the closed IR: an
 * intermediate-appending lowering there would be hoisted out of the
 * loop, changing observable behavior. Those forms are refused with
 * IR_BUILDER_UNSUPPORTED (disclosed representable-surface gap; see
 * ir_builder_stmt.h and the package completion report).
 *
 * Determinism: node construction order is the AST walk order (spec
 * 10.4); local slots are created in source order; the enclosing stack
 * is per-body call state (no process-global scratch); the noreturn
 * runtime signature patch is idempotent and keyed by the callee's
 * parameter count.
 */
#include "ir_builder_stmt.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Per-body mapper state
 * ------------------------------------------------------------------------- */

typedef struct StmtCtx {
    BuilderCtx *ctx;
    const NameModule *module;
    const NameSymbol *fn_sym;
    IrNode *fn_node;           /* IR_FUNCTION whose body is being mapped */
    IrNode *block;             /* current block (statements appended here) */

    /* Enclosing constructs for break/continue target resolution
     * (IR_SWITCH / IR_WHILE / IR_FOR). Lifespan: one body mapping call. */
    IrNode **encl;
    size_t encl_n;
    size_t encl_cap;
} StmtCtx;

/* ---------------------------------------------------------------------------
 * Small allocation helpers (build-owned memory; set build->oom on failure)
 * ------------------------------------------------------------------------- */

static void *build_alloc(BuilderCtx *ctx, size_t n, size_t size)
{
    IrBuild *b = ctx->build;
    void *p;
    if (b->oom || size == 0) {
        return NULL;
    }
    p = calloc(n, size);
    if (p == NULL) {
        b->oom = true;
    }
    return p;
}

/* Grow the enclosing stack (mapper-scratch; realloc, ctx->oom on
 * failure). Returns false on allocation failure. */
static bool encl_grow(StmtCtx *s)
{
    size_t ncap = s->encl_cap ? s->encl_cap * 2 : 8;
    IrNode **p = (IrNode **)realloc(s->encl, ncap * sizeof(*p));
    if (p == NULL) {
        s->ctx->oom = true;
        return false;
    }
    s->encl = p;
    s->encl_cap = ncap;
    return true;
}

/* ---------------------------------------------------------------------------
 * Cause links (minimal structural; full preservation is WP-M0-16c2)
 * ------------------------------------------------------------------------- */

static void add_cause(BuilderCtx *ctx, IrNode *node, const char *kind)
{
    ir_node_add_cause(ctx->build, node, kind, node->span, -1, -1, -1);
}

/* Create a node with the construct's span and one root cause link. */
static IrNode *mk_node(BuilderCtx *ctx, IrNodeKind kind,
                       const DiagSpan *span, const char *construct_kind)
{
    IrNode *n = ir_node_new(ctx->build, kind, span);
    if (n == NULL) {
        return NULL;
    }
    add_cause(ctx, n, construct_kind);
    return n;
}

/* ---------------------------------------------------------------------------
 * Declaration / symbol lookup
 * ------------------------------------------------------------------------- */

/* The declaration IR node whose fully qualified name matches `fqn`
 * (mirrors the 16c1c expr package's find_decl_node; a small local copy
 * because that helper is static). */
static IrNode *find_decl_node(IrBuild *b, const char *fqn)
{
    size_t mi, di;
    if (b == NULL || fqn == NULL) {
        return NULL;
    }
    for (mi = 0; mi < b->nmodules; mi++) {
        IrNode *m = b->modules[mi];
        if (m == NULL) {
            continue;
        }
        for (di = 0; di < m->u.module.ndecls; di++) {
            IrNode *d = m->u.module.decls[di];
            const char *n = NULL;
            if (d == NULL) {
                continue;
            }
            switch (d->kind) {
            case IR_STRUCT_DECL:  n = d->u.struct_decl.name; break;
            case IR_ENUM_DECL:    n = d->u.enum_decl.name; break;
            case IR_GLOBAL_CONST: n = d->u.global_const.name; break;
            case IR_GLOBAL_VAR:   n = d->u.global_var.name; break;
            case IR_FUNCTION:     n = d->u.function.name; break;
            default: break;
            }
            if (n != NULL && strcmp(n, fqn) == 0) {
                return d;
            }
        }
    }
    return NULL;
}

/* The NameSymbol whose declaring AST node is `decl_node` (symbol
 * identity, so shadowing resolves correctly). Local variable symbols
 * carry decl == the AST_VAR_DECL/AST_CONST_DECL node. Returns NULL when
 * no symbol declares that node (defensive; accepted builds always have
 * one). */
static const NameSymbol *symbol_for_decl(const NameResult *result,
                                         const AstNode *decl_node)
{
    size_t i;
    if (result == NULL || decl_node == NULL) {
        return NULL;
    }
    for (i = 0; i < result->nsyms; i++) {
        const NameSymbol *sym = result->syms[i];
        if (sym != NULL && sym->decl == decl_node) {
            return sym;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Terminator classification (contract 5.6)
 * ------------------------------------------------------------------------- */

static bool is_terminator_kind(IrNodeKind kind)
{
    switch (kind) {
    case IR_RETURN: case IR_BREAK: case IR_CONTINUE:
    case IR_CALL_TERM: case IR_TRAP:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Enclosing-stack target resolution (invariant 7; contract 5.2)
 * ------------------------------------------------------------------------- */

/* The nearest enclosing construct a break targets: an IR_SWITCH or an
 * IR_WHILE/IR_FOR (spec 13.2). Returns NULL when none (defensive;
 * accepted builds reject with AIC-E0414). */
static IrNode *encl_break_target(StmtCtx *s)
{
    size_t i = s->encl_n;
    while (i > 0) {
        IrNode *n = s->encl[--i];
        if (n->kind == IR_SWITCH || n->kind == IR_WHILE ||
            n->kind == IR_FOR) {
            return n;
        }
    }
    return NULL;
}

/* The nearest enclosing loop a continue targets: an IR_WHILE/IR_FOR
 * only, skipping IR_SWITCH (continue inside a switch inside a loop
 * continues the loop; spec 13.2). Returns NULL when none (defensive;
 * AIC-E0414 pre-IR). */
static IrNode *encl_continue_target(StmtCtx *s)
{
    size_t i = s->encl_n;
    while (i > 0) {
        IrNode *n = s->encl[--i];
        if (n->kind == IR_WHILE || n->kind == IR_FOR) {
            return n;
        }
    }
    return NULL;
}

static bool encl_push(StmtCtx *s, IrNode *n)
{
    if (s->encl_n == s->encl_cap && !encl_grow(s)) {
        return false;
    }
    s->encl[s->encl_n++] = n;
    return true;
}

static void encl_pop(StmtCtx *s)
{
    if (s->encl_n > 0) {
        s->encl_n--;
    }
}

/* ---------------------------------------------------------------------------
 * Expression lowering helpers
 * ------------------------------------------------------------------------- */

static IrBuilderStatus lower_value(StmtCtx *s, const AstNode *expr,
                                   IrType *expected, IrNode **out)
{
    return ir_builder_expr_to_value(s->ctx, s->module, s->fn_sym, s->block,
                                    expr, expected, out);
}

/* Lower an expression that is evaluated once PER ITERATION (a while/for
 * condition, the for step). The closed IR has no per-iteration
 * statement list for these positions; if the lowering appended
 * intermediate effect nodes to the enclosing block they would be
 * hoisted out of the loop, changing observable behavior. Refuse such
 * forms with IR_BUILDER_UNSUPPORTED (disclosed gap). A pure value node
 * (no intermediates) is returned normally. */
static IrBuilderStatus lower_per_iteration(StmtCtx *s, const AstNode *expr,
                                           IrType *expected,
                                           bool want_value,
                                           IrNode **out)
{
    IrBuild *b = s->ctx->build;
    size_t before = s->block->u.block.nstmts;
    IrBuilderStatus st;
    IrNode *n = NULL;

    if (want_value) {
        st = lower_value(s, expr, expected, &n);
    } else {
        st = ir_builder_expr_to_any(s->ctx, s->module, s->fn_sym, s->block,
                                    expr, expected, &n);
    }
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (n == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (s->block->u.block.nstmts != before) {
        /* intermediates would execute before the loop, not per
         * iteration (representable-surface gap) */
        return IR_BUILDER_UNSUPPORTED;
    }
    if (b->oom) {
        return IR_BUILDER_OOM;
    }
    if (out != NULL) {
        *out = n;
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Runtime noreturn signature patch (spec 15.1-15.4; see header gap note 3)
 * ------------------------------------------------------------------------- */

/* Attach the spec signature (parameters + parameter slots; void return)
 * to a noreturn runtime callee on first use as an IR_CALL_TERM callee.
 * Mirrors ir_builder_expr.c's ensure_runtime_signature for exactly the
 * two noreturn functions (rt.proc.exit: code: i32; rt.trap.report:
 * code: u32, message: str - the only functions whose calls are
 * terminators, contract 4.2). Idempotent: patches only when the 16c1b
 * placeholder still has no parameters. The return type stays void (the
 * spec's void-returning list). */
static IrBuilderStatus stmt_ensure_noreturn_signature(StmtCtx *s,
                                                      IrNode *fn_node)
{
    IrBuild *b = s->ctx->build;
    IrParam *params;
    const char *name;
    const char *pnames[2];
    IrType *ptypes[2];
    size_t np = 0, i;

    if (fn_node == NULL || fn_node->kind != IR_FUNCTION ||
        fn_node->u.function.name == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (fn_node->u.function.nparams != 0) {
        return IR_BUILDER_OK;   /* already patched (idempotent) */
    }
    name = fn_node->u.function.name;
    if (strcmp(name, "rt.proc.exit") == 0) {
        pnames[0] = "code";
        ptypes[0] = ir_type_i32(b);
        np = 1;
    } else if (strcmp(name, "rt.trap.report") == 0) {
        pnames[0] = "code";
        ptypes[0] = ir_type_u32(b);
        pnames[1] = "message";
        ptypes[1] = ir_type_str(b);
        np = 2;
    } else {
        /* A noreturn callee outside the two known runtime functions is
         * outside the accepted surface (16c1b marks only those two
         * noreturn). */
        return IR_BUILDER_UNSUPPORTED;
    }
    params = (IrParam *)build_alloc(s->ctx, np, sizeof(*params));
    if (params == NULL) {
        return IR_BUILDER_OOM;
    }
    fn_node->u.function.params = params;
    fn_node->u.function.nparams = np;
    for (i = 0; i < np; i++) {
        size_t nl = strlen(pnames[i]);
        params[i].name = (char *)malloc(nl + 1);
        if (params[i].name == NULL) {
            b->oom = true;
            return IR_BUILDER_OOM;
        }
        memcpy(params[i].name, pnames[i], nl + 1);
        params[i].type = ptypes[i];
        params[i].slot_index = (int64_t)i;
        params[i].span = diag_span_clone(fn_node->span);
        if (params[i].span == NULL) {
            b->oom = true;
            return IR_BUILDER_OOM;
        }
        {
            IrSlot *sl = ir_builder_add_slot(b, fn_node, IR_SLOT_PARAM,
                                             params[i].name, params[i].type,
                                             params[i].span);
            if (sl == NULL) {
                return IR_BUILDER_OOM;
            }
        }
    }
    return IR_BUILDER_OK;
}

/* True when `expr` (an AST_EXPR_CALL) is a call to a noreturn runtime
 * function (rt.proc.exit / rt.trap.report) - the IR_CALL_TERM case
 * (contract 4.2/5.2; mirrors the 16c1c stmt_core's noreturn-call
 * detection and the 16c1b noreturn flag). */
static bool stmt_is_noreturn_call(StmtCtx *s, const AstNode *expr)
{
    const AstNode *callee_ast;
    const NameSymbol *fsym;
    IrNode *callee_node;
    if (expr == NULL || expr->kind != AST_EXPR_CALL ||
        expr->u.call.callee == NULL) {
        return false;
    }
    callee_ast = expr->u.call.callee;
    fsym = name_symbol_for_node(s->module, callee_ast);
    if (fsym == NULL || fsym->kind != NAME_SYM_FN || fsym->fqn == NULL) {
        return false;
    }
    callee_node = find_decl_node(s->ctx->build, fsym->fqn);
    return callee_node != NULL && callee_node->kind == IR_FUNCTION &&
           callee_node->u.function.noreturn;
}

/* Build an IR_CALL_TERM for an expression statement that is a call to a
 * noreturn runtime function (rt.proc.exit / rt.trap.report), mirroring
 * the 16c1c lower_call arg lowering but producing the terminator kind.
 * The callee's spec signature is attached on first use. */
static IrBuilderStatus stmt_lower_call_term(StmtCtx *s,
                                            const AstNode *expr)
{
    IrBuild *b = s->ctx->build;
    const AstNode *callee_ast;
    const NameSymbol *fsym;
    IrNode *callee_node;
    IrNode *term;
    IrBuilderStatus st;
    size_t i;

    if (expr == NULL || expr->kind != AST_EXPR_CALL ||
        expr->u.call.callee == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    callee_ast = expr->u.call.callee;
    fsym = name_symbol_for_node(s->module, callee_ast);
    if (fsym == NULL || fsym->kind != NAME_SYM_FN || fsym->fqn == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    callee_node = find_decl_node(b, fsym->fqn);
    if (callee_node == NULL || callee_node->kind != IR_FUNCTION) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (!callee_node->u.function.noreturn) {
        return IR_BUILDER_UNSUPPORTED;   /* not a terminator callee */
    }
    st = stmt_ensure_noreturn_signature(s, callee_node);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (callee_node->u.function.nparams != expr->u.call.nargs) {
        return IR_BUILDER_UNSUPPORTED;   /* count mismatch: pre-IR
                                          * (AIC-T0312); defensive */
    }
    term = mk_node(s->ctx, IR_CALL_TERM, expr->span, "AST_EXPR_CALL");
    if (term == NULL) {
        return IR_BUILDER_OOM;
    }
    term->u.call_term.callee = callee_node;
    for (i = 0; i < expr->u.call.nargs; i++) {
        const AstNode *arg_ast = expr->u.call.args[i];
        const IrType *pt = callee_node->u.function.params[i].type;
        IrNode *arg = NULL;
        st = ir_builder_expr_to_value(s->ctx, s->module, s->fn_sym,
                                      s->block, arg_ast, (IrType *)pt, &arg);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (arg == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        ir_call_term_add_arg(b, term, arg);
        if (b->oom) {
            return IR_BUILDER_OOM;
        }
    }
    ir_block_add_stmt(b, s->block, term);
    if (b->oom) {
        return IR_BUILDER_OOM;
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Local declarations (contract 4.3 storage model)
 * ------------------------------------------------------------------------- */

/* Create the IR_LOCAL_DECL node for an AST_VAR_DECL: create the slot
 * (first-declaration order), register the symbol, lower the
 * initializer, and return the node (NOT appended; the caller appends it
 * as a statement or attaches it as a for-init). */
static IrBuilderStatus stmt_local_decl(StmtCtx *s, const AstNode *decl,
                                       IrNode **out)
{
    IrBuild *b = s->ctx->build;
    const char *name;
    const AstNode *type_ast, *init_ast;
    IrType *t;
    const NameSymbol *sym;
    IrSlot *slot;
    IrNode *init = NULL;
    IrNode *node;
    IrBuilderStatus st;

    if (out != NULL) {
        *out = NULL;
    }
    if (decl == NULL || decl->kind != AST_VAR_DECL ||
        decl->u.local_decl.name == NULL || decl->u.local_decl.type == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    name = decl->u.local_decl.name;
    type_ast = decl->u.local_decl.type;
    init_ast = decl->u.local_decl.init;
    if (init_ast == NULL) {
        /* accepted builds always carry an initializer (AIC-E0403) */
        return IR_BUILDER_UNSUPPORTED;
    }
    t = ir_builder_type_from_ast(s->ctx, s->module, type_ast);
    if (t == NULL || t->kind == IRT_VOID) {
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
    }
    sym = symbol_for_decl(s->ctx->result, decl);
    if (sym == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* defensive */
    }
    slot = ir_builder_add_slot(b, s->fn_node, IR_SLOT_LOCAL, name, t,
                               decl->span);
    if (slot == NULL) {
        return IR_BUILDER_OOM;
    }
    /* register before lowering the initializer and any later statement
     * that references the variable (identifier lowering resolves
     * through this table) */
    ir_builder_expr_register_local(b, s->fn_sym, sym, slot->index);
    st = lower_value(s, init_ast, t, &init);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (init == NULL || init->type == NULL ||
        !ir_type_identical(init->type, t)) {
        return IR_BUILDER_UNSUPPORTED;
    }
    node = mk_node(s->ctx, IR_LOCAL_DECL, decl->span, "AST_VAR_DECL");
    if (node == NULL) {
        return IR_BUILDER_OOM;
    }
    node->u.local_decl.slot_index = slot->index;
    node->u.local_decl.init = init;
    if (out != NULL) {
        *out = node;
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Statement list mapping
 * ------------------------------------------------------------------------- */

static IrBuilderStatus stmt_map_stmt(StmtCtx *s, const AstNode *stmt);

/* Map the statements of an AST_BLOCK into s->block, enforcing "no
 * statement after a terminator" (contract 5.6; defensive, AIC-E0417
 * pre-IR makes it unreachable on accepted builds). */
static IrBuilderStatus stmt_map_into_current(StmtCtx *s,
                                             const AstNode *ast_block)
{
    IrBuild *b = s->ctx->build;
    size_t i;
    IrBuilderStatus st;

    if (ast_block == NULL || ast_block->kind != AST_BLOCK ||
        (ast_block->u.list.count > 0 && ast_block->u.list.items == NULL)) {
        return IR_BUILDER_UNSUPPORTED;
    }
    for (i = 0; i < ast_block->u.list.count; i++) {
        if (s->block->u.block.nstmts > 0 &&
            is_terminator_kind(
                s->block->u.block.stmts[s->block->u.block.nstmts - 1]->
                    kind)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        st = stmt_map_stmt(s, ast_block->u.list.items[i]);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (b->oom) {
            return IR_BUILDER_OOM;
        }
    }
    return IR_BUILDER_OK;
}

/* Map the statements of an AST_BLOCK into a fresh IR_BLOCK node and
 * return it (not appended). Used for controlled bodies (if/while/for/
 * case/default). The enclosing stack is already set by the caller when
 * the body's break/continue must resolve to the containing construct. */
static IrBuilderStatus stmt_new_block(StmtCtx *s, const AstNode *ast_block,
                                      IrNode **out)
{
    IrNode *block;
    IrNode *saved;
    IrBuilderStatus st;

    if (out != NULL) {
        *out = NULL;
    }
    if (ast_block == NULL || ast_block->kind != AST_BLOCK) {
        return IR_BUILDER_UNSUPPORTED;
    }
    block = mk_node(s->ctx, IR_BLOCK, ast_block->span, "AST_BLOCK");
    if (block == NULL) {
        return IR_BUILDER_OOM;
    }
    saved = s->block;
    s->block = block;
    st = stmt_map_into_current(s, ast_block);
    s->block = saved;
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (out != NULL) {
        *out = block;
    }
    return IR_BUILDER_OK;
}

/* Map a controlled body (case/default) with the terminator rule: the
 * body must end in a terminator (contract 5.6; AIC-E0412 pre-IR).
 * The check requires the LAST statement to be a terminator node
 * (IR_RETURN / IR_BREAK / IR_CONTINUE / IR_CALL_TERM / IR_TRAP). A body
 * whose termination is compound (trailing if/loop whose branches all
 * terminate) is conservatively refused; see header gap note 4. */
static IrBuilderStatus stmt_map_case_body(StmtCtx *s,
                                          const AstNode *ast_block,
                                          IrNode **out)
{
    IrBuilderStatus st = stmt_new_block(s, ast_block, out);
    IrNode *block;
    if (st != IR_BUILDER_OK) {
        return st;
    }
    block = *out;
    if (block->u.block.nstmts == 0 ||
        !is_terminator_kind(
            block->u.block.stmts[block->u.block.nstmts - 1]->kind)) {
        return IR_BUILDER_UNSUPPORTED;   /* no fall-through (defensive) */
    }
    return IR_BUILDER_OK;
}

/* Map a case value constant expression to an IRConst of the selector
 * type (spec 13.2; contract 5.2 "case values are resolved constants"). */
static IrBuilderStatus stmt_case_value(StmtCtx *s, const AstNode *value_ast,
                                       IrType *sel_type, IrConst **out)
{
    EvalCtx ec;
    EvalValue ev;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus est;
    bool supported = true;
    IrConst *c;

    if (out != NULL) {
        *out = NULL;
    }
    if (value_ast == NULL || sel_type == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    eval_ctx_init(&ec, s->ctx->result, s->ctx->layout, s->module);
    est = const_eval_expr(&ec, value_ast, &ev, &fail);
    if (est == EVAL_OOM || ec.oom) {
        eval_ctx_cleanup(&ec);
        return IR_BUILDER_OOM;
    }
    if (est != EVAL_OK) {
        eval_ctx_cleanup(&ec);
        return IR_BUILDER_UNSUPPORTED;   /* defensive; accepted builds
                                          * evaluate case values */
    }
    c = ir_builder_const_from_eval(s->ctx, s->module, sel_type, value_ast,
                                   &ev, &supported);
    eval_value_free(&ev);
    eval_ctx_cleanup(&ec);
    if (c == NULL) {
        return supported ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
    }
    if (out != NULL) {
        *out = c;
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Statement mapping
 * ------------------------------------------------------------------------- */

static IrBuilderStatus stmt_map_stmt(StmtCtx *s, const AstNode *stmt)
{
    IrBuild *b = s->ctx->build;
    IrBuilderStatus st;
    IrNode *node;

    if (stmt == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    switch (stmt->kind) {

    case AST_BLOCK: {
        /* A nested block: create an IR_BLOCK statement and map its
         * items into it (contract 5.2 IR_BLOCK; locals declared inside
         * are scoped to it). */
        IrNode *block;
        st = stmt_new_block(s, stmt, &block);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        ir_block_add_stmt(b, s->block, block);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_VAR_DECL: {
        st = stmt_local_decl(s, stmt, &node);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_CONST_DECL:
        /* Local consts have no storage (contract 4.3); references
         * resolve through the const evaluator at reference sites. The
         * declaration emits no node. */
        return IR_BUILDER_OK;

    case AST_IF: {
        IrNode *cond = NULL;
        IrNode *then_block = NULL;
        IrNode *else_block = NULL;

        st = lower_value(s, stmt->u.branch.cond, NULL, &cond);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (cond == NULL || cond->type == NULL ||
            cond->type->kind != IRT_BOOL) {
            return IR_BUILDER_UNSUPPORTED;   /* AIC-T0310 pre-IR */
        }
        st = stmt_new_block(s, stmt->u.branch.then, &then_block);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (stmt->u.branch.els != NULL) {
            if (stmt->u.branch.els->kind == AST_BLOCK) {
                st = stmt_new_block(s, stmt->u.branch.els, &else_block);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
            } else if (stmt->u.branch.els->kind == AST_IF) {
                /* else-if chain: map the nested IR_IF as the single
                 * statement of the else block (contract 5.2: IR_IF
                 * mirrors if/else if chains structurally) */
                IrNode *saved = s->block;
                else_block = mk_node(s->ctx, IR_BLOCK,
                                     stmt->u.branch.els->span, "AST_BLOCK");
                if (else_block == NULL) {
                    return IR_BUILDER_OOM;
                }
                s->block = else_block;
                st = stmt_map_stmt(s, stmt->u.branch.els);
                s->block = saved;
                if (st != IR_BUILDER_OK) {
                    return st;
                }
                if (else_block->u.block.nstmts == 0) {
                    return IR_BUILDER_UNSUPPORTED;   /* malformed */
                }
            } else {
                return IR_BUILDER_UNSUPPORTED;   /* malformed else */
            }
        }
        node = mk_node(s->ctx, IR_IF, stmt->span, "AST_IF");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.if_stmt.cond = cond;
        node->u.if_stmt.then_block = then_block;
        node->u.if_stmt.else_block = else_block;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_WHILE: {
        IrNode *cond = NULL;
        IrNode *body = NULL;

        st = lower_per_iteration(s, stmt->u.while_loop.cond, NULL,
                                 true, &cond);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (cond == NULL || cond->type == NULL ||
            cond->type->kind != IRT_BOOL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        node = mk_node(s->ctx, IR_WHILE, stmt->span, "AST_WHILE");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.while_stmt.cond = cond;
        /* push the loop before mapping its body so break/continue
         * inside it resolve to this loop */
        if (!encl_push(s, node)) {
            return IR_BUILDER_OOM;
        }
        st = stmt_new_block(s, stmt->u.while_loop.body, &body);
        encl_pop(s);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        node->u.while_stmt.body = body;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_FOR: {
        IrNode *init = NULL;
        IrNode *cond = NULL;
        IrNode *step = NULL;
        IrNode *body = NULL;

        if (stmt->u.for_loop.init != NULL) {
            if (stmt->u.for_loop.init->kind == AST_VAR_DECL) {
                st = stmt_local_decl(s, stmt->u.for_loop.init, &init);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
            } else if (stmt->u.for_loop.init->kind == AST_CONST_DECL) {
                /* for-init const: compile-time binding, no node */
                init = NULL;
            } else {
                /* expression init: wrap in IR_EXPR_STMT (a statement) */
                IrNode *val = NULL;
                st = ir_builder_expr_to_any(s->ctx, s->module, s->fn_sym,
                                            s->block,
                                            stmt->u.for_loop.init, NULL,
                                            &val);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
                if (val == NULL) {
                    return IR_BUILDER_UNSUPPORTED;
                }
                init = mk_node(s->ctx, IR_EXPR_STMT,
                               stmt->u.for_loop.init->span, "AST_EXPR_STMT");
                if (init == NULL) {
                    return IR_BUILDER_OOM;
                }
                init->u.expr_stmt.expr = val;
            }
        }
        if (stmt->u.for_loop.cond != NULL) {
            st = lower_per_iteration(s, stmt->u.for_loop.cond, NULL,
                                     true, &cond);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (cond == NULL || cond->type == NULL ||
                cond->type->kind != IRT_BOOL) {
                return IR_BUILDER_UNSUPPORTED;
            }
        }
        if (stmt->u.for_loop.step != NULL) {
            st = lower_per_iteration(s, stmt->u.for_loop.step, NULL,
                                     false, &step);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        }
        node = mk_node(s->ctx, IR_FOR, stmt->span, "AST_FOR");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.for_stmt.init = init;
        node->u.for_stmt.cond = cond;
        node->u.for_stmt.step = step;
        if (!encl_push(s, node)) {
            return IR_BUILDER_OOM;
        }
        st = stmt_new_block(s, stmt->u.for_loop.body, &body);
        encl_pop(s);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        node->u.for_stmt.body = body;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_SWITCH: {
        IrNode *selector = NULL;
        IrNode **cases = NULL;
        size_t ncases = 0;
        IrNode *default_clause = NULL;
        size_t i;

        st = lower_value(s, stmt->u.switch_stmt.selector, NULL, &selector);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (selector == NULL || selector->type == NULL ||
            (selector->type->kind != IRT_ENUM &&
             !(selector->type->kind >= IRT_I8 &&
               selector->type->kind <= IRT_USIZE))) {
            return IR_BUILDER_UNSUPPORTED;   /* AIC-T0311 pre-IR */
        }
        if (stmt->u.switch_stmt.ncases > 0) {
            cases = (IrNode **)build_alloc(s->ctx,
                                           stmt->u.switch_stmt.ncases,
                                           sizeof(*cases));
            if (cases == NULL) {
                return IR_BUILDER_OOM;
            }
        }
        node = mk_node(s->ctx, IR_SWITCH, stmt->span, "AST_SWITCH");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.switch_stmt.selector = selector;
        /* push the switch before mapping case bodies so break inside a
         * case targets the switch; continue skips it (encl_continue_
         * target only finds loops) */
        if (!encl_push(s, node)) {
            return IR_BUILDER_OOM;
        }
        for (i = 0; i < stmt->u.switch_stmt.ncases; i++) {
            const AstNode *clause = stmt->u.switch_stmt.cases[i];
            IrNode *body = NULL;
            if (clause == NULL) {
                encl_pop(s);
                return IR_BUILDER_UNSUPPORTED;
            }
            if (clause->kind == AST_CASE_CLAUSE) {
                IrConst *cv = NULL;
                IrNode *case_node;
                st = stmt_case_value(s, clause->u.clause.value,
                                     selector->type, &cv);
                if (st != IR_BUILDER_OK) {
                    encl_pop(s);
                    return st;
                }
                st = stmt_map_case_body(s, clause->u.clause.body, &body);
                if (st != IR_BUILDER_OK) {
                    encl_pop(s);
                    return st;
                }
                case_node = mk_node(s->ctx, IR_CASE, clause->span,
                                    "AST_CASE_CLAUSE");
                if (case_node == NULL) {
                    encl_pop(s);
                    return IR_BUILDER_OOM;
                }
                case_node->u.case_clause.value = cv;
                case_node->u.case_clause.body = body;
                cases[ncases++] = case_node;
            } else if (clause->kind == AST_DEFAULT_CLAUSE) {
                IrNode *def_node;
                st = stmt_map_case_body(s, clause->u.clause.body, &body);
                if (st != IR_BUILDER_OK) {
                    encl_pop(s);
                    return st;
                }
                def_node = mk_node(s->ctx, IR_DEFAULT, clause->span,
                                   "AST_DEFAULT_CLAUSE");
                if (def_node == NULL) {
                    encl_pop(s);
                    return IR_BUILDER_OOM;
                }
                def_node->u.default_clause.body = body;
                if (default_clause != NULL) {
                    encl_pop(s);
                    return IR_BUILDER_UNSUPPORTED;   /* duplicate default:
                                                      * pre-IR AIC-E0420 */
                }
                default_clause = def_node;
            } else {
                encl_pop(s);
                return IR_BUILDER_UNSUPPORTED;   /* malformed clause */
            }
        }
        encl_pop(s);
        node->u.switch_stmt.cases = cases;
        node->u.switch_stmt.ncases = ncases;
        node->u.switch_stmt.default_clause = default_clause;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_BREAK: {
        IrNode *target = encl_break_target(s);
        if (target == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* AIC-E0414 pre-IR */
        }
        node = mk_node(s->ctx, IR_BREAK, stmt->span, "AST_BREAK");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.break_stmt.target = target;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_CONTINUE: {
        IrNode *target = encl_continue_target(s);
        if (target == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* AIC-E0414 pre-IR */
        }
        node = mk_node(s->ctx, IR_CONTINUE, stmt->span, "AST_CONTINUE");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.continue_stmt.target = target;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_RETURN: {
        IrType *ret_type = s->fn_node->u.function.ret_type;
        IrNode *value = NULL;
        if (ret_type != NULL && ret_type->kind == IRT_VOID) {
            if (stmt->u.ret.value != NULL) {
                return IR_BUILDER_UNSUPPORTED;   /* AIC-E0415 pre-IR */
            }
        } else {
            if (stmt->u.ret.value == NULL) {
                return IR_BUILDER_UNSUPPORTED;   /* AIC-E0415 pre-IR */
            }
            st = lower_value(s, stmt->u.ret.value, ret_type, &value);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (value == NULL || value->type == NULL ||
                ret_type == NULL ||
                !ir_type_identical(value->type, ret_type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
        }
        node = mk_node(s->ctx, IR_RETURN, stmt->span, "AST_RETURN");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.return_stmt.value = value;
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
    }

    case AST_EXPR_STMT: {
        const AstNode *expr = stmt->u.expr_stmt.expr;
        if (expr == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (stmt_is_noreturn_call(s, expr)) {
            /* A call to a noreturn function is a terminator (contract
             * 5.2 IR_CALL_TERM). Refuse (never silently map it as an
             * ordinary IR_CALL) when construction fails. */
            st = stmt_lower_call_term(s, expr);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
        }
        {
            IrNode *val = NULL;
            st = ir_builder_expr_to_any(s->ctx, s->module, s->fn_sym,
                                        s->block, expr, NULL, &val);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (val == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            node = mk_node(s->ctx, IR_EXPR_STMT, stmt->span,
                           "AST_EXPR_STMT");
            if (node == NULL) {
                return IR_BUILDER_OOM;
            }
            node->u.expr_stmt.expr = val;
            ir_block_add_stmt(b, s->block, node);
            return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
        }
    }

    case AST_EMPTY_STMT:
        node = mk_node(s->ctx, IR_EMPTY, stmt->span, "AST_EMPTY_STMT");
        if (node == NULL) {
            return IR_BUILDER_OOM;
        }
        ir_block_add_stmt(b, s->block, node);
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;

    default:
        return IR_BUILDER_UNSUPPORTED;
    }
}

/* ---------------------------------------------------------------------------
 * Body mapper (the 16c1d seam entry)
 * ------------------------------------------------------------------------- */

IrBuilderStatus ir_builder_stmt_body(BuilderCtx *ctx,
                                     const NameModule *module,
                                     const NameSymbol *fn_sym)
{
    StmtCtx s;
    IrNode *fn_node;
    const AstNode *body_ast;
    IrBuilderStatus st;
    IrBuild *b;

    memset(&s, 0, sizeof(s));
    s.ctx = ctx;
    s.module = module;
    s.fn_sym = fn_sym;
    b = ctx->build;

    if (fn_sym == NULL || fn_sym->fqn == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (fn_sym->decl == NULL) {
        /* Runtime built-in: no source body. The 16c1b placeholder body
         * (or NULL for noreturn functions) is owned by the 16c1c
         * signature patch; nothing to map here. */
        return IR_BUILDER_OK;
    }
    if (fn_sym->decl->kind != AST_FN_DECL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    fn_node = find_decl_node(b, fn_sym->fqn);
    if (fn_node == NULL || fn_node->kind != IR_FUNCTION) {
        return IR_BUILDER_UNSUPPORTED;
    }
    s.fn_node = fn_node;
    s.block = fn_node->u.function.body;
    if (s.block == NULL || s.block->kind != IR_BLOCK) {
        return IR_BUILDER_UNSUPPORTED;   /* malformed: no body block */
    }
    body_ast = fn_sym->decl->u.fn_decl.body;
    if (body_ast == NULL || body_ast->kind != AST_BLOCK) {
        return IR_BUILDER_UNSUPPORTED;   /* malformed body */
    }
    /* Map the body's statements INTO the existing body IR_BLOCK (the
     * 16c1b placeholder). Nested AST_BLOCK statements create their own
     * IR_BLOCK children via stmt_new_block. */
    st = stmt_map_into_current(&s, body_ast);
    free(s.encl);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    return ctx->build->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------- */

void ir_builder_stmt_install(void)
{
    ir_builder_set_body_mapper(ir_builder_stmt_body);
}
