/* bootstrap/src/sema/decl_init.c
 *
 * AI-Co Stage-0 initialization semantics (WP-M0-13a2).
 *
 * See decl_init.h for the contract and documented decisions. The
 * initializer-site walker visits every declaration site at which an
 * initializer may appear (global var declarations, function bodies,
 * `for` init declarations) and emits AIC-E0403 records for variable
 * declarations with a NULL initializer per spec sec. 8.2 and sec. 9.
 *
 * Span/message conventions (corpus-pinned):
 *   - E0403: "missing initializer on variable declaration"; span = the
 *     whole declaration node (the declaration including the terminating
 *     ';'), matching tests/negative/cases/derived-semantic-missing-init.
 * All records: phase "semantic" (registry default), severity "error"
 * (registry default), recovery "authoritative".
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "decl_init.h"

#include "../diag/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Record building and the check walker
 * ------------------------------------------------------------------------- */

typedef struct DeclInitCtx {
    DiagRecord **records;
    size_t nrecords, cap;
    bool oom;
} DeclInitCtx;

static DiagRecord *declinit_new_record(DeclInitCtx *c, const char *code,
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

static bool declinit_push_record(DeclInitCtx *c, DiagRecord *r)
{
    DiagRecord **grown;
    if (c->oom) { if (r) diag_record_free(r); return false; }
    if (c->nrecords == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        grown = (DiagRecord **)realloc(c->records,
                                       ncap * sizeof(DiagRecord *));
        if (!grown) {
            c->oom = true;
            if (r) diag_record_free(r);
            return false;
        }
        c->records = grown;
        c->cap = ncap;
    }
    c->records[c->nrecords++] = r;
    return true;
}

/* AIC-E0403: a variable declaration without an initializer (sec. 8.2,
 * sec. 9.2). Primary span = the whole declaration node. */
static bool check_missing_init(DeclInitCtx *c, const AstNode *decl)
{
    DiagRecord *r = declinit_new_record(c, "AIC-E0403",
                                        "missing initializer on variable declaration",
                                        decl ? decl->span : NULL);
    return declinit_push_record(c, r);
}

/* The initializer of a local declaration statement (AST_VAR_DECL /
 * AST_CONST_DECL / `for` init). Returns non-NULL when the proposed
 * declaration is a variable with a NULL initializer. */
static bool local_decl_missing_init(const AstNode *s)
{
    return s && s->kind == AST_VAR_DECL &&
           !s->u.local_decl.init;
}

/* Recursively walk a statement, checking every variable declaration
 * site for the missing-initializer rule. Expression sub-trees are not
 * walked: declarations appear only as statements, `for` inits, and
 * module-scope declarations, and expression semantics belong to
 * WP-M0-13b. */
static bool walk_stmt(DeclInitCtx *c, const AstNode *s)
{
    size_t i;
    if (!s || c->oom) return true;
    switch (s->kind) {
    case AST_BLOCK:
        for (i = 0; i < s->u.list.count; i++) {
            if (!walk_stmt(c, s->u.list.items[i])) return false;
        }
        return true;
    case AST_VAR_DECL:
        /* var_decl initializer is optional in the grammar (v0.1.3);
         * a NULL initializer is AIC-E0403 here. */
        if (local_decl_missing_init(s)) return check_missing_init(c, s);
        return true;
    case AST_CONST_DECL:
        /* const_decl keeps the strict required-initializer grammar; a
         * NULL initializer cannot reach the semantic stage through the
         * accepted parser (missing "=" is AIC-S0101 and the declaration
         * is dropped by recovery). Nothing to check. */
        return true;
    case AST_IF:
        if (!walk_stmt(c, s->u.branch.then)) return false;
        return walk_stmt(c, s->u.branch.els);
    case AST_WHILE:
        return walk_stmt(c, s->u.while_loop.body);
    case AST_FOR:
        if (local_decl_missing_init(s->u.for_loop.init)) {
            if (!check_missing_init(c, s->u.for_loop.init)) return false;
        }
        return walk_stmt(c, s->u.for_loop.body);
    case AST_SWITCH:
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (!cl) continue;
            if (!walk_stmt(c, cl->u.clause.body)) return false;
        }
        return true;
    case AST_RETURN:
    case AST_EXPR_STMT:
    case AST_EMPTY_STMT:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_CASE_CLAUSE:
    case AST_DEFAULT_CLAUSE:
    default:
        return true;
    }
}

/* Walk one module-scope declaration (module_scope iteration, the same
 * deterministic order the decl_core and const-eval stages use). */
static bool walk_decl(DeclInitCtx *c, const AstNode *decl)
{
    if (!decl || c->oom) return true;
    switch (decl->kind) {
    case AST_GLOBAL_VAR_DECL:
        /* global_var_decl initializer is optional in the grammar
         * (v0.1.3); a NULL initializer is AIC-E0403 here. */
        if (!decl->u.global_decl.init) return check_missing_init(c, decl);
        return true;
    case AST_GLOBAL_CONST_DECL:
        /* const form keeps the strict required-initializer grammar;
         * see AST_CONST_DECL above. */
        return true;
    case AST_FN_DECL:
        /* parameter and return types carry no initializers; the body
         * may contain local var declarations and `for` inits. */
        return walk_stmt(c, decl->u.fn_decl.body);
    case AST_ENUM_DECL:
    case AST_STRUCT_DECL:
    default:
        return true;
    }
}

DeclInitStatus declinit_check(const NameResult *result,
                              DiagRecord ***out_records,
                              size_t *out_record_count)
{
    DeclInitCtx c;
    size_t m;
    if (!result) return DECLINIT_UNSUPPORTED;
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
            if (!walk_decl(&c, sym->decl)) break;
        }
        if (c.oom) break;
    }
    if (c.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        return DECLINIT_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (c.nrecords) return DECLINIT_DIAG_ERROR;
    return DECLINIT_OK;
}