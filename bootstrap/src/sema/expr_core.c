/* bootstrap/src/sema/expr_core.c
 *
 * AI-Co Stage-0 expression core and evaluation-order model (WP-M0-13b1).
 *
 * See expr_core.h for the contract and documented decisions. This file
 * implements:
 *   - expr_precedence_of: the sec. 10.1 precedence model;
 *   - expr_order_plan: the sec. 10.4 evaluation-order model (a
 *     deterministic, total step plan for every expression form - the
 *     language has no unspecified evaluation order);
 *   - expr_core_check: the build-level const-context check (AIC-E0401)
 *     at the sites this package owns: array type extents (sec. 12.1),
 *     switch case labels (sec. 13.2), and local const declaration
 *     initializers (sec. 8.1). Global const/var initializers and enum
 *     member value expressions belong to WP-M0-12a and are NOT walked
 *     here (boundary test in expr_core_test.c proves no duplicate).
 *
 * Record conventions (corpus-consistent with WP-M0-12a):
 *   - AIC-E0401: message "expression is not a constant expression"
 *     (registry description), phase "semantic", severity "error",
 *     recovery "authoritative", primary span = the whole site expression
 *     (the array-extent expression, the case-label expression, or the
 *     local-const initializer expression).
 *   - Checked-arithmetic failures (AIC-E0405..E0411) at these sites are
 *     routed out as EvalFailureSite (node = the site expression) for the
 *     failure-record owner; never emitted here.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "expr_core.h"

#include "../diag/diag.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Precedence model (spec sec. 10.1)
 * ------------------------------------------------------------------------- */

ExprPrecedence expr_precedence_of(const AstNode *expr)
{
    if (!expr) return EXPR_PREC_NONE;
    switch (expr->kind) {
    case AST_EXPR_ASSIGN:
        return EXPR_PREC_ASSIGNMENT;
    case AST_EXPR_TERNARY:
        return EXPR_PREC_CONDITIONAL;
    case AST_EXPR_BINARY:
        switch (expr->u.binary.op) {
        case AST_BIN_MUL:
        case AST_BIN_DIV:
        case AST_BIN_MOD:
            return EXPR_PREC_MULTIPLICATIVE;
        case AST_BIN_ADD:
        case AST_BIN_SUB:
            return EXPR_PREC_ADDITIVE;
        case AST_BIN_SHL:
        case AST_BIN_SHR:
            return EXPR_PREC_SHIFT;
        case AST_BIN_LT:
        case AST_BIN_LE:
        case AST_BIN_GT:
        case AST_BIN_GE:
            return EXPR_PREC_RELATIONAL;
        case AST_BIN_EQ:
        case AST_BIN_NE:
            return EXPR_PREC_EQUALITY;
        case AST_BIN_BAND:
            return EXPR_PREC_BAND;
        case AST_BIN_BXOR:
            return EXPR_PREC_BXOR;
        case AST_BIN_BOR:
            return EXPR_PREC_BOR;
        case AST_BIN_LAND:
            return EXPR_PREC_LAND;
        case AST_BIN_LOR:
            return EXPR_PREC_LOR;
        }
        return EXPR_PREC_NONE;
    case AST_EXPR_UNARY:
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        return EXPR_PREC_UNARY;
    case AST_EXPR_INDEX:
    case AST_EXPR_SLICE:
    case AST_EXPR_CALL:
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
    case AST_EXPR_STRUCT_INIT:
        return EXPR_PREC_POSTFIX;
    default:
        /* literals, identifiers, array literals, parens (transparent) */
        return EXPR_PREC_NONE;
    }
}

/* ---------------------------------------------------------------------------
 * Evaluation-order model (spec sec. 10.4)
 * ------------------------------------------------------------------------- */

typedef struct PlanCtx {
    ExprStep *steps;
    size_t nsteps, cap;
    bool oom;          /* allocation failure; nothing owned */
    bool unsupported;  /* defensive: unknown/malformed node kind;
                        * nothing owned */
} PlanCtx;

static bool plan_push(PlanCtx *c, ExprStepKind kind, const AstNode *node,
                      bool conditional)
{
    ExprStep *grown;
    if (c->oom || c->unsupported) return false;
    if (c->nsteps == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 16;
        grown = (ExprStep *)realloc(c->steps, ncap * sizeof(ExprStep));
        if (!grown) { c->oom = true; return false; }
        c->steps = grown;
        c->cap = ncap;
    }
    c->steps[c->nsteps].kind = kind;
    c->steps[c->nsteps].node = node;
    c->steps[c->nsteps].conditional = conditional;
    c->nsteps++;
    return true;
}

/* Recursive planner. `as_location` requests the location evaluation of
 * the ROOT of `expr` (assignment target / address-of operand context);
 * it affects only the terminal step kind of location-capable forms
 * (ident, deref, index, member, arrow). `conditional` marks every step
 * produced by this subtree as conditional (short-circuit / ternary
 * branch propagation). */
static void plan_of(PlanCtx *c, const AstNode *expr, bool as_location,
                    bool conditional)
{
    size_t i;
    if (!expr || c->oom || c->unsupported) return;
    switch (expr->kind) {
    case AST_EXPR_INT_LITERAL:
    case AST_EXPR_STR_LITERAL:
    case AST_EXPR_BOOL_LITERAL:
    case AST_EXPR_NULL_LITERAL:
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_IDENT:
        plan_push(c, as_location ? EXPR_STEP_LOCATION : EXPR_STEP_VALUE,
                  expr, conditional);
        return;
    case AST_EXPR_PAREN:
        /* Parens are transparent: they do not change evaluation order. */
        plan_of(c, expr->u.paren.expr, as_location, conditional);
        return;
    case AST_EXPR_UNARY:
        if (expr->u.unary.op == AST_UN_ADDR) {
            /* address-of: evaluate the operand as a location, then the
             * address value is produced */
            plan_of(c, expr->u.unary.operand, true, conditional);
            plan_push(c, EXPR_STEP_VALUE, expr, conditional);
            return;
        }
        if (expr->u.unary.op == AST_UN_DEREF) {
            /* dereference: evaluate the pointer operand for its value,
             * then the deref yields a location (or the read value) */
            plan_of(c, expr->u.unary.operand, false, conditional);
            plan_push(c, as_location ? EXPR_STEP_LOCATION : EXPR_STEP_VALUE,
                      expr, conditional);
            return;
        }
        plan_of(c, expr->u.unary.operand, false, conditional);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_BINARY:
        plan_of(c, expr->u.binary.lhs, false, conditional);
        if (expr->u.binary.op == AST_BIN_LAND ||
            expr->u.binary.op == AST_BIN_LOR) {
            /* short-circuit: the right operand is evaluated only when the
             * left operand does not determine the outcome */
            plan_of(c, expr->u.binary.rhs, false, true);
        } else {
            plan_of(c, expr->u.binary.rhs, false, conditional);
        }
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_ASSIGN: {
        bool compound = (expr->u.assign.op != AST_ASGN_ASSIGN);
        /* destination location, then the source value, then store;
         * compound additionally reads the destination before storing */
        plan_of(c, expr->u.assign.target, true, conditional);
        plan_of(c, expr->u.assign.value, false, conditional);
        if (compound) {
            plan_push(c, EXPR_STEP_READ, expr->u.assign.target, conditional);
        }
        plan_push(c, EXPR_STEP_STORE, expr, conditional);
        return;
    }
    case AST_EXPR_TERNARY:
        plan_of(c, expr->u.branch.cond, false, conditional);
        /* exactly one branch is evaluated */
        plan_of(c, expr->u.branch.then, false, true);
        plan_of(c, expr->u.branch.els, false, true);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_INDEX:
        plan_of(c, expr->u.index_slice.base, false, conditional);
        plan_of(c, expr->u.index_slice.index, false, conditional);
        plan_push(c, as_location ? EXPR_STEP_LOCATION : EXPR_STEP_VALUE,
                  expr, conditional);
        return;
    case AST_EXPR_SLICE:
        plan_of(c, expr->u.index_slice.base, false, conditional);
        plan_of(c, expr->u.index_slice.lo, false, conditional);
        plan_of(c, expr->u.index_slice.hi, false, conditional);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_CALL:
        plan_of(c, expr->u.call.callee, false, conditional);
        for (i = 0; i < expr->u.call.nargs; i++) {
            plan_of(c, expr->u.call.args[i], false, conditional);
        }
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        /* evaluate the operand only; no member expression is evaluated */
        plan_of(c, expr->u.member.base, false, conditional);
        plan_push(c, as_location ? EXPR_STEP_LOCATION : EXPR_STEP_VALUE,
                  expr, conditional);
        return;
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < expr->u.array_literal.nelems; i++) {
            plan_of(c, expr->u.array_literal.elems[i], false, conditional);
        }
        /* repeat form: the count follows the element expression (textual
         * order; deterministic - no unspecified evaluation order) */
        plan_of(c, expr->u.array_literal.count, false, conditional);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_STRUCT_INIT:
        /* the base type name is not an evaluated expression; field
         * initializers evaluate left-to-right in literal order
         * (sec. 12.7) */
        for (i = 0; i < expr->u.struct_init.nfields; i++) {
            const AstNode *fi = expr->u.struct_init.fields[i];
            if (fi && fi->kind == AST_FIELD_INIT) {
                plan_of(c, fi->u.named.value, false, conditional);
            }
        }
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
        /* the operand is not evaluated (only its type is considered) */
        plan_push(c, EXPR_STEP_NOT_EVAL, expr->u.size_op.operand,
                  conditional);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        plan_of(c, expr->u.cast_wrap.expr, false, conditional);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        plan_of(c, expr->u.size_op.operand, false, conditional);
        plan_push(c, EXPR_STEP_VALUE, expr, conditional);
        return;
    default:
        /* defensive: unknown/malformed expression node kind - no
         * steps; the header contract returns EXPR_ORDER_UNSUPPORTED */
        c->unsupported = true;
        return;
    }
}

ExprOrderStatus expr_order_plan(const AstNode *expr, bool as_location,
                                ExprStep **out_steps, size_t *out_count)
{
    PlanCtx c;
    if (out_steps) *out_steps = NULL;
    if (out_count) *out_count = 0;
    if (!expr) return EXPR_ORDER_UNSUPPORTED;
    memset(&c, 0, sizeof(c));
    plan_of(&c, expr, as_location, false);
    if (c.unsupported) {
        free(c.steps);
        return EXPR_ORDER_UNSUPPORTED;
    }
    if (c.oom) {
        free(c.steps);
        return EXPR_ORDER_OOM;
    }
    if (out_steps) *out_steps = c.steps;
    if (out_count) *out_count = c.nsteps;
    return EXPR_ORDER_OK;
}

void expr_steps_free(ExprStep *steps)
{
    free(steps);
}

/* ---------------------------------------------------------------------------
 * Const-context check (AIC-E0401) - walker
 * ------------------------------------------------------------------------- */

typedef struct ExprCoreCtx {
    EvalCtx ev;
    DiagRecord **records;
    size_t nrecords, records_cap;
    EvalFailureSite *failures;
    size_t nfailures, failures_cap;
    bool oom;
    bool unsupported;
} ExprCoreCtx;

static DiagRecord *xc_new_record(ExprCoreCtx *c, const char *code,
                                 const char *message,
                                 const DiagSpan *primary)
{
    DiagRecord *r = diag_record_new();
    if (!r) { c->oom = true; return NULL; }
    if (!diag_record_set_code(r, code) ||
        !diag_record_set_message(r, message) ||
        !diag_record_set_primary_span(r, primary) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        c->oom = true;
        return NULL;
    }
    return r;
}

static bool xc_push_record(ExprCoreCtx *c, DiagRecord *r)
{
    DiagRecord **grown;
    if (c->oom) { if (r) diag_record_free(r); return false; }
    if (c->nrecords == c->records_cap) {
        size_t ncap = c->records_cap ? c->records_cap * 2 : 8;
        grown = (DiagRecord **)realloc(c->records,
                                       ncap * sizeof(DiagRecord *));
        if (!grown) {
            c->oom = true;
            if (r) diag_record_free(r);
            return false;
        }
        c->records = grown;
        c->records_cap = ncap;
    }
    c->records[c->nrecords++] = r;
    return true;
}

static bool xc_push_failure(ExprCoreCtx *c, const AstNode *node,
                            EvalFailure kind)
{
    EvalFailureSite *grown;
    if (c->oom) return false;
    if (c->nfailures == c->failures_cap) {
        size_t ncap = c->failures_cap ? c->failures_cap * 2 : 8;
        grown = (EvalFailureSite *)realloc(
            c->failures, ncap * sizeof(EvalFailureSite));
        if (!grown) { c->oom = true; return false; }
        c->failures = grown;
        c->failures_cap = ncap;
    }
    c->failures[c->nfailures].node = node;
    c->failures[c->nfailures].kind = kind;
    c->nfailures++;
    return true;
}

/* Evaluate one const-context site: on EVAL_NOT_CONST emit AIC-E0401 at the
 * site expression; on EVAL_FAILURE route the failure (the site node is the
 * whole site expression - the same convention as the WP-M0-12a/12b
 * handoff). */
static void xc_check_site(ExprCoreCtx *c, const AstNode *expr)
{
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    if (!expr) { c->unsupported = true; return; }
    st = const_eval_expr(&c->ev, expr, &v, &fail);
    if (st == EVAL_OK) {
        eval_value_free(&v);
        return;
    }
    if (st == EVAL_NOT_CONST) {
        DiagRecord *r = xc_new_record(c, "AIC-E0401",
                                      "expression is not a constant expression",
                                      expr->span);
        if (r) xc_push_record(c, r);
        return;
    }
    if (st == EVAL_FAILURE) {
        xc_push_failure(c, expr, fail);
        return;
    }
    if (st == EVAL_OOM) return;
    /* EVAL_UNSUPPORTED: defensive; no record */
    c->unsupported = true;
}

static bool walk_expr(ExprCoreCtx *c, const NameModule *module,
                      const AstNode *e);
static bool walk_type(ExprCoreCtx *c, const NameModule *module,
                      const AstNode *type_node);

/* Walk an AST type node: every array extent is a const-context site
 * (sec. 12.1). */
static bool walk_type(ExprCoreCtx *c, const NameModule *module,
                      const AstNode *type_node)
{
    if (!type_node || c->oom) return true;
    switch (type_node->kind) {
    case AST_TYPE_ARRAY:
        if (type_node->u.type_derived.len) {
            xc_check_site(c, type_node->u.type_derived.len);
        }
        return walk_type(c, module, type_node->u.type_derived.base);
    case AST_TYPE_PTR:
    case AST_TYPE_SLICE:
        return walk_type(c, module, type_node->u.type_derived.base);
    default:
        return true;
    }
}

/* Recursively walk an expression, descending into sub-expressions and
 * type operands (cast/wrap targets, sizeof/alignof type operands). No
 * expression itself is a const-context site of this package; the sites
 * are discovered by the statement walker (case labels, local const
 * initializers) and the type walker (array extents). */
static bool walk_expr(ExprCoreCtx *c, const NameModule *module,
                      const AstNode *e)
{
    size_t i;
    if (!e || c->oom) return true;
    switch (e->kind) {
    case AST_EXPR_ASSIGN:
        if (!walk_expr(c, module, e->u.assign.target)) return false;
        return walk_expr(c, module, e->u.assign.value);
    case AST_EXPR_UNARY:
        return walk_expr(c, module, e->u.unary.operand);
    case AST_EXPR_BINARY:
        if (!walk_expr(c, module, e->u.binary.lhs)) return false;
        return walk_expr(c, module, e->u.binary.rhs);
    case AST_EXPR_TERNARY:
        if (!walk_expr(c, module, e->u.branch.cond)) return false;
        if (!walk_expr(c, module, e->u.branch.then)) return false;
        return walk_expr(c, module, e->u.branch.els);
    case AST_EXPR_INDEX:
        if (!walk_expr(c, module, e->u.index_slice.base)) return false;
        return walk_expr(c, module, e->u.index_slice.index);
    case AST_EXPR_SLICE:
        if (!walk_expr(c, module, e->u.index_slice.base)) return false;
        if (!walk_expr(c, module, e->u.index_slice.lo)) return false;
        return walk_expr(c, module, e->u.index_slice.hi);
    case AST_EXPR_CALL:
        if (!walk_expr(c, module, e->u.call.callee)) return false;
        for (i = 0; i < e->u.call.nargs; i++) {
            if (!walk_expr(c, module, e->u.call.args[i])) return false;
        }
        return true;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        return walk_expr(c, module, e->u.member.base);
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < e->u.array_literal.nelems; i++) {
            if (!walk_expr(c, module, e->u.array_literal.elems[i])) {
                return false;
            }
        }
        return walk_expr(c, module, e->u.array_literal.count);
    case AST_EXPR_STRUCT_INIT:
        if (!walk_expr(c, module, e->u.struct_init.base)) return false;
        for (i = 0; i < e->u.struct_init.nfields; i++) {
            const AstNode *fi = e->u.struct_init.fields[i];
            if (fi && fi->kind == AST_FIELD_INIT) {
                if (!walk_expr(c, module, fi->u.named.value)) return false;
            }
        }
        return true;
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF:
        return walk_type(c, module, e->u.size_op.operand);
    case AST_EXPR_SIZEOF_EXPR:
        return walk_expr(c, module, e->u.size_op.operand);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        if (!walk_type(c, module, e->u.cast_wrap.type)) return false;
        return walk_expr(c, module, e->u.cast_wrap.expr);
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        return walk_expr(c, module, e->u.size_op.operand);
    case AST_EXPR_PAREN:
        return walk_expr(c, module, e->u.paren.expr);
    case AST_EXPR_INT_LITERAL:
    case AST_EXPR_STR_LITERAL:
    case AST_EXPR_BOOL_LITERAL:
    case AST_EXPR_NULL_LITERAL:
    case AST_EXPR_IDENT:
    default:
        return true;
    }
}

/* Recursively walk a statement, checking local const initializers and
 * case labels (the const-context sites of this package that live in
 * function bodies). */
static bool walk_stmt(ExprCoreCtx *c, const NameModule *module,
                      const AstNode *s)
{
    size_t i;
    if (!s || c->oom) return true;
    switch (s->kind) {
    case AST_BLOCK:
        for (i = 0; i < s->u.list.count; i++) {
            if (!walk_stmt(c, module, s->u.list.items[i])) return false;
        }
        return true;
    case AST_VAR_DECL:
        if (!walk_type(c, module, s->u.local_decl.type)) return false;
        return walk_expr(c, module, s->u.local_decl.init);
    case AST_CONST_DECL:
        /* local const initializer must be a constant expression
         * (sec. 8.1) */
        if (!walk_type(c, module, s->u.local_decl.type)) return false;
        if (s->u.local_decl.init) xc_check_site(c, s->u.local_decl.init);
        return true;
    case AST_IF:
        if (!walk_expr(c, module, s->u.branch.cond)) return false;
        if (!walk_stmt(c, module, s->u.branch.then)) return false;
        return walk_stmt(c, module, s->u.branch.els);
    case AST_WHILE:
        if (!walk_expr(c, module, s->u.while_loop.cond)) return false;
        return walk_stmt(c, module, s->u.while_loop.body);
    case AST_FOR:
        if (s->u.for_loop.init) {
            if (s->u.for_loop.init->kind == AST_VAR_DECL ||
                s->u.for_loop.init->kind == AST_CONST_DECL) {
                if (!walk_stmt(c, module, s->u.for_loop.init)) return false;
            } else {
                if (!walk_expr(c, module, s->u.for_loop.init)) return false;
            }
        }
        if (!walk_expr(c, module, s->u.for_loop.cond)) return false;
        if (!walk_expr(c, module, s->u.for_loop.step)) return false;
        return walk_stmt(c, module, s->u.for_loop.body);
    case AST_SWITCH:
        if (!walk_expr(c, module, s->u.switch_stmt.selector)) return false;
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (!cl) continue;
            if (cl->kind == AST_CASE_CLAUSE) {
                /* each case label is a constant expression (sec. 13.2).
                 * The whole-label check subsumes nested array extents
                 * (a nested non-const extent makes the whole label
                 * non-const), so the label expression is not descended
                 * into separately - that would duplicate records. */
                if (cl->u.clause.value) {
                    xc_check_site(c, cl->u.clause.value);
                }
            }
            if (!walk_stmt(c, module, cl->u.clause.body)) return false;
        }
        return true;
    case AST_RETURN:
        return walk_expr(c, module, s->u.ret.value);
    case AST_EXPR_STMT:
        return walk_expr(c, module, s->u.expr_stmt.expr);
    case AST_EMPTY_STMT:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_CASE_CLAUSE:
    case AST_DEFAULT_CLAUSE:
    default:
        return true;
    }
}

/* Walk one module-scope declaration. Global var/const initializers and
 * enum member value expressions are WP-M0-12a sites and are NOT walked
 * here; this package walks only the type positions (array extents) and
 * function bodies (local consts, case labels, nested types). */
static bool walk_decl(ExprCoreCtx *c, const NameModule *module,
                      const AstNode *decl)
{
    size_t i;
    if (!decl || c->oom) return true;
    switch (decl->kind) {
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        return walk_type(c, module, decl->u.global_decl.type);
    case AST_ENUM_DECL:
        return true;   /* member values: 12a's site */
    case AST_STRUCT_DECL:
        for (i = 0; i < decl->u.struct_decl.nfields; i++) {
            const AstNode *f = decl->u.struct_decl.fields[i];
            if (f) {
                if (!walk_type(c, module, f->u.named.type)) return false;
            }
        }
        return true;
    case AST_FN_DECL:
        for (i = 0; i < decl->u.fn_decl.nparams; i++) {
            const AstNode *p = decl->u.fn_decl.params[i];
            if (p) {
                if (!walk_type(c, module, p->u.named.type)) return false;
            }
        }
        if (!walk_type(c, module, decl->u.fn_decl.ret_type)) return false;
        return walk_stmt(c, module, decl->u.fn_decl.body);
    default:
        return true;
    }
}

ExprCoreStatus expr_core_check(const NameResult *result,
                               const LayoutBuild *layout,
                               DiagRecord ***out_records,
                               size_t *out_record_count,
                               EvalFailureSite **out_failures,
                               size_t *out_failure_count)
{
    ExprCoreCtx c;
    size_t m;
    if (!result || !layout) return EXPR_CORE_UNSUPPORTED;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    if (out_failures) *out_failures = NULL;
    if (out_failure_count) *out_failure_count = 0;
    memset(&c, 0, sizeof(c));
    eval_ctx_init(&c.ev, result, layout, NULL);
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *mod = result->modules[m];
        size_t d;
        if (!mod) continue;
        c.ev.module = mod;
        for (d = 0; d < mod->nmodule_scope; d++) {
            const NameSymbol *sym = mod->module_scope[d];
            if (!sym || !sym->decl) continue;
            if (!walk_decl(&c, mod, sym->decl)) break;
        }
        if (c.oom) break;
    }
    if (c.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        free(c.failures);
        eval_ctx_cleanup(&c.ev);
        return EXPR_CORE_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (out_failures) *out_failures = c.failures;
    if (out_failure_count) *out_failure_count = c.nfailures;
    eval_ctx_cleanup(&c.ev);
    if (c.nrecords) return EXPR_CORE_DIAG_ERROR;
    if (c.nfailures) return EXPR_CORE_FAILURE;
    if (c.unsupported) return EXPR_CORE_UNSUPPORTED;
    return EXPR_CORE_OK;
}
