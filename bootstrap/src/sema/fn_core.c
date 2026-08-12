/* bootstrap/src/sema/fn_core.c
 *
 * AI-Co Stage-0 function return rules (WP-M0-13d1).
 *
 * See fn_core.h for the contract and documented decisions. This file
 * implements AIC-E0415 (return value mismatch, spec sec. 13.4):
 *   - `return expr;` in a `void` function -> "return value in void
 *     function";
 *   - bare `return;` in a non-`void` function -> "return value missing
 *     in non-void function".
 *
 * The check is purely structural on statement shape plus the function's
 * return-type void-ness; no expression semantics are evaluated here and
 * no const evaluation is performed.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "fn_core.h"

#include "../diag/diag.h"
#include "../load/load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context and record plumbing
 * ------------------------------------------------------------------------- */

typedef struct FnCtx {
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unsupported;
} FnCtx;

static DiagRecord *fn_new_record(FnCtx *c, const char *code,
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

static bool fn_push_record(FnCtx *c, DiagRecord *r)
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
 * Return-value mismatch walk (E0415)
 * ------------------------------------------------------------------------- */

/* True when the function's declared return type is `void` (AST_TYPE_PRIM
 * with AST_PRIM_VOID). Returns false defensively when the type node is
 * absent or not a primitive; a NULL ret_type is treated as unsupported by
 * the caller (fn_walk_decl) rather than guessed, mirroring stmt_reach's
 * defensive contract. */
static bool fn_is_void_return(const AstNode *decl)
{
    const AstNode *ret_type;
    if (!decl || decl->kind != AST_FN_DECL) return false;
    ret_type = decl->u.fn_decl.ret_type;
    if (!ret_type) return false;
    return ret_type->kind == AST_TYPE_PRIM &&
           ret_type->u.type_prim.prim == AST_PRIM_VOID;
}

/* True when the function declaration lacks an explicit return type node.
 * The grammar (spec sec. 5.2 fn_decl) always produces one on a clean
 * parse, so this is defensive-only; treat it as unsupported input. */
static bool fn_missing_ret_type(const AstNode *decl)
{
    if (!decl || decl->kind != AST_FN_DECL) return true;
    return decl->u.fn_decl.ret_type == NULL;
}

/* Check one return statement against the enclosing function's void-ness
 * and emit one E0415 record when the presence/absence of the value
 * mismatches (spec sec. 13.4):
 *   - value present in a void function;
 *   - value absent in a non-void function.
 * The primary span is the whole return statement (corpus-pinned for the
 * value-in-void direction). */
static void fn_check_return(FnCtx *c, const AstNode *ret_stmt,
                            bool is_void)
{
    DiagRecord *r;
    if (!ret_stmt || c->oom) return;
    if (is_void && ret_stmt->u.ret.value != NULL) {
        r = fn_new_record(c, "AIC-E0415", "return value in void function",
                          ret_stmt->span);
        if (r) fn_push_record(c, r);
    } else if (!is_void && ret_stmt->u.ret.value == NULL) {
        r = fn_new_record(c, "AIC-E0415",
                          "return value missing in non-void function",
                          ret_stmt->span);
        if (r) fn_push_record(c, r);
    }
}

/* Walk one statement list, checking every return statement. Returns
 * false on OOM or defensive unsupported input. */
static bool fn_walk_block(FnCtx *c, const AstNode *block, bool is_void);

/* Walk one statement. Only return statements are owned here; all other
 * statement kinds are descended into (blocks, if/else, loops, switch
 * clause bodies) so returns at any nesting depth are checked. */
static bool fn_walk_stmt(FnCtx *c, const AstNode *s, bool is_void)
{
    size_t i;
    if (!s || c->oom) return true;
    switch (s->kind) {
    case AST_RETURN:
        fn_check_return(c, s, is_void);
        return !c->oom;
    case AST_BLOCK:
        return fn_walk_block(c, s, is_void);
    case AST_IF:
        if (!fn_walk_stmt(c, s->u.branch.then, is_void)) return false;
        return fn_walk_stmt(c, s->u.branch.els, is_void);
    case AST_WHILE:
        return fn_walk_stmt(c, s->u.while_loop.body, is_void);
    case AST_FOR:
        return fn_walk_stmt(c, s->u.for_loop.body, is_void);
    case AST_SWITCH:
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (!cl || !cl->u.clause.body) continue;
            if (!fn_walk_stmt(c, cl->u.clause.body, is_void)) return false;
            if (c->oom) return false;
        }
        return true;
    case AST_EXPR_STMT:
    case AST_VAR_DECL:
    case AST_CONST_DECL:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_EMPTY_STMT:
        /* no nested statements; nothing owned here */
        return true;
    default:
        /* defensive: unknown statement kind (never reached on a clean
         * build; statements never nest inside expressions) */
        c->unsupported = true;
        return false;
    }
}

static bool fn_walk_block(FnCtx *c, const AstNode *block, bool is_void)
{
    size_t i;
    if (!block || block->kind != AST_BLOCK) {
        c->unsupported = true;
        return false;
    }
    for (i = 0; i < block->u.list.count; i++) {
        const AstNode *s = block->u.list.items[i];
        if (!s) continue;
        if (!fn_walk_stmt(c, s, is_void)) return false;
        if (c->oom) return false;
    }
    return true;
}

/* Walk one module-scope declaration: only function bodies contain
 * return statements; global var/const initializers, enum member values,
 * and struct fields are expressions owned by other packages. */
static bool fn_walk_decl(FnCtx *c, const AstNode *decl)
{
    bool is_void;
    if (!decl || c->oom) return true;
    if (decl->kind != AST_FN_DECL) return true;
    /* The grammar always produces an explicit return type on a clean
     * parse; a missing one is defensive unsupported input (mirrors
     * stmt_reach), never guessed. */
    if (fn_missing_ret_type(decl)) {
        c->unsupported = true;
        return false;
    }
    is_void = fn_is_void_return(decl);
    return fn_walk_block(c, decl->u.fn_decl.body, is_void);
}

/* ---------------------------------------------------------------------------
 * Build-level entry
 * ------------------------------------------------------------------------- */

FnCoreStatus fn_core_check(const NameResult *result,
                           const LayoutBuild *layout,
                           DiagRecord ***out_records,
                           size_t *out_record_count)
{
    FnCtx c;
    size_t m;
    if (!result || !layout) return FN_CORE_UNSUPPORTED;
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
            if (!fn_walk_decl(&c, sym->decl)) break;
        }
        if (c.oom || c.unsupported) break;
    }
    if (c.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        return FN_CORE_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (c.nrecords) return FN_CORE_DIAG_ERROR;
    if (c.unsupported) return FN_CORE_UNSUPPORTED;
    return FN_CORE_OK;
}
