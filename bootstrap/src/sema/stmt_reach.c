/* bootstrap/src/sema/stmt_reach.c
 *
 * AI-Co Stage-0 reachability analysis (WP-M0-13c2).
 *
 * See stmt_reach.h for the contract and documented decisions. This file
 * implements:
 *   - AIC-E0416 (non-void function path without return, sec. 13.4-13.5):
 *     conservative fall-through analysis over the function body;
 *   - AIC-E0417 (unreachable statement, sec. 13.5): block-scoped
 *     syntactic check after return/break/continue/noreturn call.
 *
 * The analysis is purely structural on statement shape plus name
 * resolution for noreturn calls; no expression semantics are evaluated
 * here and no const evaluation is performed.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "stmt_reach.h"

#include "../diag/diag.h"
#include "../load/load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context and record plumbing
 * ------------------------------------------------------------------------- */

typedef struct ReachCtx {
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unsupported;
} ReachCtx;

static DiagRecord *reach_new_record(ReachCtx *c, const char *code,
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

static bool reach_push_record(ReachCtx *c, DiagRecord *r)
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
 * Noreturn-call detection (mirrors stmt_core's terminator check)
 * ------------------------------------------------------------------------- */

/* A noreturn call: an expression statement whose expression is a call
 * whose callee resolves to the compiler-provided rt.proc.exit or
 * rt.trap.report function (fqn "rt.proc.exit" / "rt.trap.report"). */
static bool reach_is_noreturn_call(const NameModule *module,
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

/* ---------------------------------------------------------------------------
 * Source helpers (fn header span for E0416)
 * ------------------------------------------------------------------------- */

static int64_t reach_skip_trivia(const LoadSource *src, int64_t pos)
{
    const char *t = src->text;
    int64_t len = (int64_t)src->len;
    while (pos < len) {
        unsigned char ch = (unsigned char)t[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            pos++;
        } else if (ch == '/' && pos + 1 < len && t[pos + 1] == '/') {
            while (pos < len && t[pos] != '\n') pos++;
        } else if (ch == '/' && pos + 1 < len && t[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) pos++;
            if (pos + 1 < len) pos += 2;
            else pos = len;
        } else {
            break;
        }
    }
    return pos;
}

static int64_t reach_ident_end(const LoadSource *src, int64_t pos)
{
    const char *t = src->text;
    int64_t len = (int64_t)src->len;
    if (pos >= len) return -1;
    unsigned char ch = (unsigned char)t[pos];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')) {
        return -1;
    }
    while (pos < len) {
        ch = (unsigned char)t[pos];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            break;
        }
        pos++;
    }
    return pos;
}

/* The "fn <name>" header span for E0416: from the start of the `fn`
 * keyword through the end of the function name identifier
 * (corpus-pinned). Returns NULL on defensive failure (missing source);
 * the caller falls back to the decl span. */
static DiagSpan *reach_fn_header_span(const NameModule *module,
                                      const AstNode *decl)
{
    int64_t start, pos, end;
    if (!module || !module->src || !decl || !decl->span) return NULL;
    start = decl->span->start.offset;
    if (start < 0 || start + 2 > (int64_t)module->src->len ||
        memcmp(module->src->text + start, "fn", 2) != 0) {
        return NULL;
    }
    pos = reach_skip_trivia(module->src, start + 2);
    end = reach_ident_end(module->src, pos);
    if (end < 0 || end > (int64_t)module->src->len) return NULL;
    return load_span_range(module->src, start, end);
}

/* ---------------------------------------------------------------------------
 * Fall-through analysis (E0416)
 * ------------------------------------------------------------------------- */

/* True when the loop condition is the literal `true` keyword (after
 * unwrapping parens) or absent (for (;;)); used for the
 * while(true)/for(;;) non-termination rule. Only the literal keyword
 * counts; other constant conditions are not const-evaluated here. */
static bool reach_cond_literal_true(const AstNode *cond)
{
    while (cond && cond->kind == AST_EXPR_PAREN) {
        cond = cond->u.paren.expr;
    }
    return cond && cond->kind == AST_EXPR_BOOL_LITERAL &&
           cond->u.bool_literal.value;
}

/* True when the loop body contains a `break` that exits THIS loop: a
 * break not nested inside a switch and not nested inside an inner loop.
 * Breaks inside a switch exit the switch (sec. 13.2); breaks inside an
 * inner loop exit the inner loop. */
static bool reach_loop_has_exiting_break(const AstNode *body)
{
    size_t i;
    if (!body || body->kind != AST_BLOCK) return false;
    for (i = 0; i < body->u.list.count; i++) {
        const AstNode *s = body->u.list.items[i];
        if (!s) continue;
        switch (s->kind) {
        case AST_BREAK:
            return true;
        case AST_BLOCK:
            if (reach_loop_has_exiting_break(s)) return true;
            break;
        case AST_IF:
            if (reach_loop_has_exiting_break(s->u.branch.then)) return true;
            if (reach_loop_has_exiting_break(s->u.branch.els)) return true;
            break;
        case AST_WHILE:
        case AST_FOR:
        case AST_SWITCH:
            /* breaks inside belong to the inner construct */
            break;
        default:
            break;
        }
    }
    return false;
}

/* Forward declarations for the mutual recursion. */
static bool reach_stmt_may_fall_through(const NameModule *module,
                                        const AstNode *s,
                                        int loop_depth, int switch_depth);

/* Whether a statement list may fall off its end: there is a path from
 * the start of the list through the end. */
static bool reach_list_may_fall_off(const NameModule *module,
                                    const AstNode *block,
                                    int loop_depth, int switch_depth)
{
    size_t i;
    bool reachable = true;
    if (!block || block->kind != AST_BLOCK) return true;
    for (i = 0; i < block->u.list.count && reachable; i++) {
        reachable = reach_stmt_may_fall_through(module,
                                                block->u.list.items[i],
                                                loop_depth, switch_depth);
    }
    return reachable;
}

/* Whether a `switch` may fall past to the statement after it: no
 * default clause (a selector with no matching case reaches the tail),
 * or some case body ends in a valid break (the break path reaches the
 * statement after the switch), or some case body may fall through its
 * own end (defensive; 13c1's E0412 owns that record, E0416 still
 * applies - corpus 18-5-semantic-case-no-terminate). */
static bool reach_switch_may_fall_past(const NameModule *module,
                                       const AstNode *sw,
                                       int loop_depth, int switch_depth)
{
    size_t i;
    bool has_default = false;
    for (i = 0; i < sw->u.switch_stmt.ncases; i++) {
        const AstNode *cl = sw->u.switch_stmt.cases[i];
        if (!cl) continue;
        if (cl->kind == AST_DEFAULT_CLAUSE) has_default = true;
    }
    if (!has_default) return true;
    for (i = 0; i < sw->u.switch_stmt.ncases; i++) {
        const AstNode *cl = sw->u.switch_stmt.cases[i];
        const AstNode *body, *final_stmt = NULL;
        if (!cl || !cl->u.clause.body) continue;
        body = cl->u.clause.body;
        if (body->kind != AST_BLOCK) continue;
        if (body->u.list.count > 0) {
            final_stmt = body->u.list.items[body->u.list.count - 1];
        }
        if (!final_stmt) return true;          /* empty body: falls through */
        if (final_stmt->kind == AST_BREAK) {
            /* valid break inside the case body (switch_depth + 1) */
            return true;
        }
        if (reach_list_may_fall_off(module, body,
                                    loop_depth, switch_depth + 1)) {
            return true;   /* body lacks a terminating final statement */
        }
    }
    return false;
}

/* Whether a loop may fall past to the statement after it: its
 * condition is not the literal `true` (it may run zero times), or a
 * break that exits this loop exists (the break path reaches the tail).
 * A `while (true)` / `for (;;)` with no such break is non-terminating
 * and control cannot reach the tail (sec. 13.5). */
static bool reach_loop_may_fall_past(const AstNode *loop)
{
    const AstNode *cond, *body;
    if (loop->kind == AST_WHILE) {
        cond = loop->u.while_loop.cond;
        body = loop->u.while_loop.body;
    } else {
        cond = loop->u.for_loop.cond;
        body = loop->u.for_loop.body;
    }
    /* `for (;;)` has an absent condition; it is always-true for the
     * non-termination rule (spec sec. 13.5). */
    if (cond == NULL) {
        return reach_loop_has_exiting_break(body);
    }
    if (!reach_cond_literal_true(cond)) return true;
    return reach_loop_has_exiting_break(body);
}

/* Whether executing `s` may let control reach the next statement in the
 * same block. This is the conservative fall-through predicate used for
 * E0416's tail analysis. */
static bool reach_stmt_may_fall_through(const NameModule *module,
                                        const AstNode *s,
                                        int loop_depth, int switch_depth)
{
    if (!s) return true;
    switch (s->kind) {
    case AST_RETURN:
        return false;
    case AST_EXPR_STMT:
        return !reach_is_noreturn_call(module, s);
    case AST_BREAK:
        /* a VALID break leaves the block; an invalid break (outside any
         * loop/switch - 13c1's E0414) is treated as ordinary flow so
         * the corpus derived-semantic-break-outside-loop matches */
        return !(loop_depth > 0 || switch_depth > 0);
    case AST_CONTINUE:
        return !(loop_depth > 0);
    case AST_BLOCK:
        return reach_list_may_fall_off(module, s, loop_depth, switch_depth);
    case AST_IF:
        if (!s->u.branch.els) return true;   /* conservative: missing else */
        return reach_stmt_may_fall_through(module, s->u.branch.then,
                                           loop_depth, switch_depth) ||
               reach_stmt_may_fall_through(module, s->u.branch.els,
                                           loop_depth, switch_depth);
    case AST_WHILE:
    case AST_FOR:
        return reach_loop_may_fall_past(s);
    case AST_SWITCH:
        return reach_switch_may_fall_past(module, s, loop_depth,
                                          switch_depth);
    case AST_VAR_DECL:
    case AST_CONST_DECL:
    case AST_EMPTY_STMT:
        return true;
    default:
        /* defensive: unknown statement kind (never reached on a clean
         * build; statements never nest inside expressions). Do not
         * claim the tail is reachable through unknown code - the
         * E0417 walker flags the build unsupported instead. */
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Unreachable-statement walk (E0417)
 * ------------------------------------------------------------------------- */

static bool reach_walk_block(ReachCtx *c, const NameModule *module,
                             const AstNode *block,
                             int loop_depth, int switch_depth);

/* Walk one function body block: per-block, after a terminator (return,
 * valid break/continue, or noreturn call), the first following
 * statement is unreachable -> one E0417 record; the remainder of the
 * block is not walked. */
static bool reach_walk_stmt(ReachCtx *c, const NameModule *module,
                            const AstNode *s,
                            int loop_depth, int switch_depth)
{
    size_t i;
    if (!s || c->oom) return true;
    switch (s->kind) {
    case AST_BLOCK:
        return reach_walk_block(c, module, s, loop_depth, switch_depth);
    case AST_IF:
        if (!reach_walk_stmt(c, module, s->u.branch.then,
                             loop_depth, switch_depth)) {
            return false;
        }
        return reach_walk_stmt(c, module, s->u.branch.els,
                               loop_depth, switch_depth);
    case AST_WHILE:
        return reach_walk_stmt(c, module, s->u.while_loop.body,
                               loop_depth + 1, switch_depth);
    case AST_FOR:
        return reach_walk_stmt(c, module, s->u.for_loop.body,
                               loop_depth + 1, switch_depth);
    case AST_SWITCH:
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (!cl || !cl->u.clause.body) continue;
            if (!reach_walk_stmt(c, module, cl->u.clause.body,
                                 loop_depth, switch_depth + 1)) {
                return false;
            }
            if (c->oom) return false;
        }
        return true;
    case AST_RETURN:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_EXPR_STMT:
    case AST_VAR_DECL:
    case AST_CONST_DECL:
    case AST_EMPTY_STMT:
        return true;
    default:
        c->unsupported = true;
        return false;
    }
}

static bool reach_walk_block(ReachCtx *c, const NameModule *module,
                             const AstNode *block,
                             int loop_depth, int switch_depth)
{
    size_t i;
    bool terminated = false;
    if (!block || block->kind != AST_BLOCK) {
        c->unsupported = true;
        return false;
    }
    for (i = 0; i < block->u.list.count; i++) {
        const AstNode *s = block->u.list.items[i];
        if (!s) continue;
        if (terminated) {
            /* first statement after a terminator in this block:
             * unreachable -> one E0417 record, then stop the block */
            DiagRecord *r = reach_new_record(c, "AIC-E0417",
                                             "unreachable statement",
                                             s->span);
            if (!reach_push_record(c, r)) return false;
            return true;
        }
        switch (s->kind) {
        case AST_RETURN:
            terminated = true;
            break;
        case AST_EXPR_STMT:
            if (reach_is_noreturn_call(module, s)) terminated = true;
            break;
        case AST_BREAK:
            if (loop_depth > 0 || switch_depth > 0) terminated = true;
            break;
        case AST_CONTINUE:
            if (loop_depth > 0) terminated = true;
            break;
        default:
            break;
        }
        if (!reach_walk_stmt(c, module, s, loop_depth, switch_depth)) {
            return false;
        }
        if (c->oom) return false;
    }
    return true;
}

/* Walk one module-scope declaration. Only function bodies contain
 * statements; global var/const initializers, enum member values, and
 * struct fields are expressions owned by other packages. */
static bool reach_walk_decl(ReachCtx *c, const NameModule *module,
                            const AstNode *decl)
{
    if (!decl || c->oom) return true;
    if (decl->kind == AST_FN_DECL) {
        return reach_walk_block(c, module, decl->u.fn_decl.body, 0, 0);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * E0416 per-function tail check
 * ------------------------------------------------------------------------- */

static void reach_check_fn_tail(ReachCtx *c, const NameModule *module,
                                const AstNode *decl)
{
    const AstNode *ret_type, *body;
    DiagSpan *hspan;
    DiagRecord *r;
    char msg[256];

    if (!decl || decl->kind != AST_FN_DECL || c->oom) return;
    ret_type = decl->u.fn_decl.ret_type;
    if (!ret_type) { c->unsupported = true; return; }
    /* void functions may fall off the end (sec. 13.4) */
    if (ret_type->kind == AST_TYPE_PRIM &&
        ret_type->u.type_prim.prim == AST_PRIM_VOID) {
        return;
    }
    body = decl->u.fn_decl.body;
    if (!body || body->kind != AST_BLOCK) { c->unsupported = true; return; }
    if (!reach_list_may_fall_off(module, body, 0, 0)) return;

    snprintf(msg, sizeof(msg),
             "non-void function '%s' has a path without return",
             decl->u.fn_decl.name ? decl->u.fn_decl.name : "?");
    hspan = reach_fn_header_span(module, decl);
    r = reach_new_record(c, "AIC-E0416", msg,
                         hspan ? hspan : (decl->span ? decl->span : NULL));
    if (r) reach_push_record(c, r);
    diag_span_free(hspan);
}

/* ---------------------------------------------------------------------------
 * Build-level entry
 * ------------------------------------------------------------------------- */

StmtReachStatus stmt_reach_check(const NameResult *result,
                                 const LayoutBuild *layout,
                                 DiagRecord ***out_records,
                                 size_t *out_record_count)
{
    ReachCtx c;
    size_t m;
    if (!result || !layout) return STMT_REACH_UNSUPPORTED;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    memset(&c, 0, sizeof(c));
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *mod = result->modules[m];
        size_t d;
        if (!mod) continue;
        for (d = 0; d < mod->nmodule_scope; d++) {
            const NameSymbol *sym = mod->module_scope[d];
            if (!sym || !sym->decl) continue;
            reach_check_fn_tail(&c, mod, sym->decl);
            if (c.oom || c.unsupported) break;
            if (!reach_walk_decl(&c, mod, sym->decl)) break;
        }
        if (c.oom || c.unsupported) break;
    }
    if (c.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        return STMT_REACH_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (c.nrecords) return STMT_REACH_DIAG_ERROR;
    if (c.unsupported) return STMT_REACH_UNSUPPORTED;
    return STMT_REACH_OK;
}
