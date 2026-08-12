/* bootstrap/src/sema/stmt_core.c
 *
 * AI-Co Stage-0 statement rules and switch/break/continue (WP-M0-13c1).
 *
 * See stmt_core.h for the contract and documented decisions. This file
 * implements:
 *   - the switch no-fall-through rule (AIC-E0412): each case/default
 *     body's final statement must be a terminator (break, return,
 *     continue when the switch is inside a loop, or a noreturn call);
 *   - the duplicate-case-value rule (AIC-E0413): case labels are
 *     constant expressions of the selector's type; labels with equal
 *     evaluated values in one switch are rejected, the record pointing
 *     at the first occurrence's label;
 *   - the break/continue placement rule (AIC-E0414): break is valid
 *     inside a loop or a switch; continue is valid only inside a loop.
 *
 * The check is purely syntactic on statement structure with loop/switch
 * depth context and noreturn-call resolution; no expression semantics
 * are evaluated here (duplicate detection uses the WP-M0-12a public
 * evaluator for label values only).
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "stmt_core.h"

#include "../diag/diag.h"
#include "../load/load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context and record plumbing
 * ------------------------------------------------------------------------- */

typedef struct StmtCtx {
    EvalCtx ev;                 /* const-evaluator context (label values) */
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unsupported;
} StmtCtx;

static DiagRecord *stmt_new_record(StmtCtx *c, const char *code,
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

static bool stmt_push_record(StmtCtx *c, DiagRecord *r)
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

/* ---------------------------------------------------------------------------
 * Case-label helpers (source text slice and label span)
 * ------------------------------------------------------------------------- */

/* Source text of a label value expression, sliced from the module source
 * (the LoadSource line/offset index is authoritative). Returns a
 * malloc'd NUL-terminated copy, or NULL on defensive failure (module
 * without source; unreachable for user modules on a valid build). */
static char *stmt_label_text(const NameModule *module, const AstNode *value)
{
    int64_t s, e;
    size_t len;
    char *out;
    if (!module || !module->src || !value || !value->span) return NULL;
    s = value->span->start.offset;
    e = value->span->end.offset;
    if (s < 0 || e < s || (size_t)e > module->src->len) return NULL;
    len = (size_t)(e - s);
    out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, module->src->text + s, len);
    out[len] = '\0';
    return out;
}

/* The case label span: from the start of the `case` keyword (the clause
 * node's span start) through the end of the value expression - the
 * corpus-pinned "case 0" span. For a default clause the span is the
 * `default` keyword (7 bytes from the clause start; documented decision,
 * no corpus anchor). Returns NULL on defensive failure (missing source);
 * the caller falls back to the clause span. */
static DiagSpan *stmt_label_span(const NameModule *module,
                                 const AstNode *clause)
{
    int64_t start, end;
    if (!module || !module->src || !clause || !clause->span) return NULL;
    start = clause->span->start.offset;
    if (clause->kind == AST_CASE_CLAUSE && clause->u.clause.value &&
        clause->u.clause.value->span) {
        end = clause->u.clause.value->span->end.offset;
    } else {
        end = start + 7;   /* "default" */
    }
    if (start < 0 || end < start || (size_t)end > module->src->len) {
        return NULL;
    }
    return load_span_range(module->src, start, end);
}

/* ---------------------------------------------------------------------------
 * Terminator detection (AIC-E0412)
 * ------------------------------------------------------------------------- */

/* A noreturn call: an expression statement whose expression is a call
 * whose callee resolves to the compiler-provided rt.proc.exit or
 * rt.trap.report function (fqn "rt.proc.exit" / "rt.trap.report").
 * The name resolver records the member-chain reference on the outermost
 * member node, so name_symbol_for_node(module, callee) resolves it. */
static bool stmt_is_noreturn_call(const NameModule *module,
                                  const AstNode *stmt)
{
    const AstNode *expr, *callee;
    const NameSymbol *sym;
    if (!stmt || stmt->kind != AST_EXPR_STMT) return false;
    expr = stmt->u.expr_stmt.expr;
    if (!expr || expr->kind != AST_EXPR_CALL) return false;
    callee = expr->u.call.callee;
    if (!callee) return false;
    sym = name_symbol_for_node(module, callee);
    if (!sym || sym->kind != NAME_SYM_FN || !sym->fqn) return false;
    return strcmp(sym->fqn, "rt.proc.exit") == 0 ||
           strcmp(sym->fqn, "rt.trap.report") == 0;
}

/* True when `s` is a valid case-body terminator in the current context
 * (loop_depth > 0 at the switch): break, return, continue (only inside
 * a loop), or a noreturn call (rt.proc.exit / rt.trap.report). */
static bool stmt_is_terminator(const NameModule *module, const AstNode *s,
                               int loop_depth)
{
    if (!s) return false;
    switch (s->kind) {
    case AST_BREAK:
    case AST_RETURN:
        return true;
    case AST_CONTINUE:
        return loop_depth > 0;
    case AST_EXPR_STMT:
        return stmt_is_noreturn_call(module, s);
    default:
        return false;
    }
}

/* Emit AIC-E0412 for one case/default clause whose body's final
 * statement is not a valid terminator (or the body is empty). */
static void stmt_check_case_terminator(StmtCtx *c, const NameModule *module,
                                       const AstNode *clause, int loop_depth)
{
    const AstNode *body, *final_stmt = NULL;
    DiagSpan *lspan;
    DiagRecord *r;
    char msg[256];
    const char *label = NULL;
    char *owned_label = NULL;

    if (!clause || c->oom) return;
    body = clause->u.clause.body;
    if (!body || body->kind != AST_BLOCK) { c->unsupported = true; return; }
    if (body->u.list.count > 0) {
        final_stmt = body->u.list.items[body->u.list.count - 1];
    }
    if (final_stmt && stmt_is_terminator(module, final_stmt, loop_depth)) {
        return;
    }

    /* message: corpus-pinned "switch case <label> body lacks ..." with
     * the label source text; default clause: "switch default body
     * lacks ..." (documented decision) */
    if (clause->kind == AST_CASE_CLAUSE && clause->u.clause.value) {
        owned_label = stmt_label_text(module, clause->u.clause.value);
        label = owned_label ? owned_label : "?";
        snprintf(msg, sizeof(msg),
                 "switch case %s body lacks a terminating statement; "
                 "fall-through is prohibited", label);
    } else {
        snprintf(msg, sizeof(msg),
                 "switch default body lacks a terminating statement; "
                 "fall-through is prohibited");
    }

    lspan = stmt_label_span(module, clause);
    r = stmt_new_record(c, "AIC-E0412", msg,
                        lspan ? lspan : (clause->span ? clause->span : NULL));
    if (r) stmt_push_record(c, r);
    diag_span_free(lspan);
    free(owned_label);
}

/* ---------------------------------------------------------------------------
 * Duplicate case values (AIC-E0413)
 * ------------------------------------------------------------------------- */

/* One evaluated case label seen so far in the current switch. */
typedef struct StmtSeenLabel {
    EvalInt value;             /* evaluated label value (v, big) */
    const AstNode *clause;     /* the clause that introduced the value */
} StmtSeenLabel;

/* Render an EvalInt deterministically (the WP-M0-12b2 render_eval_int
 * convention): big values (two's complement in the int64 field,
 * [2^63, 2^64-1]) as unsigned, others as signed. */
static void stmt_render_int(const EvalInt *v, char *buf, size_t sz)
{
    if (v->big) {
        snprintf(buf, sz, "%llu", (unsigned long long)(uint64_t)v->v);
    } else {
        snprintf(buf, sz, "%lld", (long long)v->v);
    }
}

/* Evaluate one case label and register it for duplicate detection.
 * Labels that do not evaluate (EVAL_NOT_CONST / EVAL_FAILURE /
 * EVAL_UNSUPPORTED) are skipped - the const-context stages own those
 * records - and labels of non-integer kind are skipped defensively. */
static void stmt_register_label(StmtCtx *c, const NameModule *module,
                                const AstNode *clause,
                                StmtSeenLabel **seen, size_t *nseen,
                                size_t *seen_cap)
{
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    size_t i;
    const AstNode *value;

    if (!clause || clause->kind != AST_CASE_CLAUSE || c->oom) return;
    value = clause->u.clause.value;
    if (!value) { c->unsupported = true; return; }

    st = const_eval_expr(&c->ev, value, &v, &fail);
    if (st == EVAL_OOM) { c->ev.oom = true; return; }
    if (st != EVAL_OK) return;      /* NOT_CONST / FAILURE / UNSUPPORTED:
                                     * skipped - the const-context stages
                                     * own those records (header contract) */
    if (v.kind != EVAL_VAL_INT) {   /* defensive; a valid pipeline never */
        eval_value_free(&v);        /* reaches here (selector is int/enum) */
        return;
    }

    /* compare against every earlier label in this switch */
    for (i = 0; i < *nseen; i++) {
        if ((*seen)[i].value.v == v.u.i.v &&
            (*seen)[i].value.big == v.u.i.big) {
            /* duplicate: emit at the FIRST occurrence's label */
            char nbuf[32];
            char msg[256];
            DiagSpan *lspan;
            DiagRecord *r;
            stmt_render_int(&v.u.i, nbuf, sizeof(nbuf));
            snprintf(msg, sizeof(msg), "duplicate switch case value: %s",
                     nbuf);
            lspan = stmt_label_span(module, (*seen)[i].clause);
            r = stmt_new_record(c, "AIC-E0413", msg,
                                lspan ? lspan :
                                    ((*seen)[i].clause->span ?
                                     (*seen)[i].clause->span : NULL));
            if (r) stmt_push_record(c, r);
            diag_span_free(lspan);
            break;
        }
    }
    if (i == *nseen) {
        StmtSeenLabel *grown;
        if (*nseen == *seen_cap) {
            size_t ncap = *seen_cap ? *seen_cap * 2 : 4;
            grown = (StmtSeenLabel *)realloc(
                *seen, ncap * sizeof(StmtSeenLabel));
            if (!grown) { eval_value_free(&v); c->oom = true; return; }
            *seen = grown;
            *seen_cap = ncap;
        }
        (*seen)[*nseen].value = v.u.i;
        (*seen)[*nseen].clause = clause;
        (*nseen)++;
    }
    eval_value_free(&v);
}

/* ---------------------------------------------------------------------------
 * Statement walker
 * ------------------------------------------------------------------------- */

static bool stmt_walk_stmt(StmtCtx *c, const NameModule *module,
                           const AstNode *s, int loop_depth, int switch_depth);

/* Check one switch: E0412 for every clause body, E0413 duplicate labels,
 * then walk the clause bodies (switch_depth + 1). */
static bool stmt_check_switch(StmtCtx *c, const NameModule *module,
                              const AstNode *sw, int loop_depth,
                              int switch_depth)
{
    StmtSeenLabel *seen = NULL;
    size_t nseen = 0, seen_cap = 0, i;

    for (i = 0; i < sw->u.switch_stmt.ncases; i++) {
        const AstNode *cl = sw->u.switch_stmt.cases[i];
        if (!cl) continue;
        if (cl->kind != AST_CASE_CLAUSE && cl->kind != AST_DEFAULT_CLAUSE) {
            c->unsupported = true;
            free(seen);
            return false;
        }
        stmt_check_case_terminator(c, module, cl, loop_depth);
        stmt_register_label(c, module, cl, &seen, &nseen, &seen_cap);
        if (c->oom || c->ev.oom || c->unsupported) break;
    }
    free(seen);

    if (c->oom || c->ev.oom || c->unsupported) return false;

    for (i = 0; i < sw->u.switch_stmt.ncases; i++) {
        const AstNode *cl = sw->u.switch_stmt.cases[i];
        if (!cl || !cl->u.clause.body) continue;
        if (!stmt_walk_stmt(c, module, cl->u.clause.body, loop_depth,
                            switch_depth + 1)) {
            return false;
        }
        if (c->oom) return false;
    }
    return true;
}

static bool stmt_walk_stmt(StmtCtx *c, const NameModule *module,
                           const AstNode *s, int loop_depth, int switch_depth)
{
    size_t i;
    if (!s || c->oom || c->ev.oom) return true;
    switch (s->kind) {
    case AST_BLOCK:
        for (i = 0; i < s->u.list.count; i++) {
            if (!stmt_walk_stmt(c, module, s->u.list.items[i],
                                loop_depth, switch_depth)) {
                return false;
            }
        }
        return true;
    case AST_IF:
        if (!stmt_walk_stmt(c, module, s->u.branch.then,
                            loop_depth, switch_depth)) {
            return false;
        }
        return stmt_walk_stmt(c, module, s->u.branch.els,
                              loop_depth, switch_depth);
    case AST_WHILE:
        return stmt_walk_stmt(c, module, s->u.while_loop.body,
                              loop_depth + 1, switch_depth);
    case AST_FOR:
        /* init/cond/step are expressions (or a declaration); statements
         * never nest inside them - only the body is walked */
        return stmt_walk_stmt(c, module, s->u.for_loop.body,
                              loop_depth + 1, switch_depth);
    case AST_SWITCH:
        return stmt_check_switch(c, module, s, loop_depth, switch_depth);
    case AST_BREAK:
        if (loop_depth == 0 && switch_depth == 0) {
            DiagRecord *r = stmt_new_record(
                c, "AIC-E0414", "break outside loop or switch", s->span);
            if (r) stmt_push_record(c, r);
        }
        return true;
    case AST_CONTINUE:
        if (loop_depth == 0) {
            DiagRecord *r = stmt_new_record(
                c, "AIC-E0414", "continue outside loop", s->span);
            if (r) stmt_push_record(c, r);
        }
        return true;
    case AST_RETURN:
    case AST_EXPR_STMT:
    case AST_VAR_DECL:
    case AST_CONST_DECL:
    case AST_EMPTY_STMT:
        /* no nested statements; return/expr/decl rules are owned by
         * other packages */
        return true;
    default:
        c->unsupported = true;
        return false;
    }
}

/* Walk one module-scope declaration. Only function bodies contain
 * statements; global var/const initializers, enum member values, and
 * struct fields are expressions owned by other packages. */
static bool stmt_walk_decl(StmtCtx *c, const NameModule *module,
                           const AstNode *decl)
{
    if (!decl || c->oom || c->ev.oom) return true;
    if (decl->kind == AST_FN_DECL) {
        return stmt_walk_stmt(c, module, decl->u.fn_decl.body, 0, 0);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Build-level entry
 * ------------------------------------------------------------------------- */

StmtCoreStatus stmt_core_check(const NameResult *result,
                               const LayoutBuild *layout,
                               DiagRecord ***out_records,
                               size_t *out_record_count)
{
    StmtCtx c;
    size_t m;
    if (!result || !layout) return STMT_CORE_UNSUPPORTED;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
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
            if (!stmt_walk_decl(&c, mod, sym->decl)) break;
        }
        if (c.oom || c.ev.oom) break;
    }
    if (c.oom || c.ev.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        eval_ctx_cleanup(&c.ev);
        return STMT_CORE_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    eval_ctx_cleanup(&c.ev);
    if (c.nrecords) return STMT_CORE_DIAG_ERROR;
    if (c.unsupported) return STMT_CORE_UNSUPPORTED;
    return STMT_CORE_OK;
}
