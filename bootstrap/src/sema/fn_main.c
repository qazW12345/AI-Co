/* bootstrap/src/sema/fn_main.c
 *
 * AI-Co Stage-0 entry-main validation and reserved-name enforcement
 * (WP-M0-13d2).
 *
 * See fn_main.h for the contract and documented decisions. This file
 * implements:
 *   - AIC-E0418 (entry `main` signature invalid / missing, spec
 *     sec. 15.3): the entry module must declare `fn main() -> i32` or
 *     `fn main() -> void`; missing or mis-typed `main` is rejected.
 *     The check is applied to the entry module only.
 *   - the sec. 4.5 reserved-name guard (defensive; all reachable
 *     spellings are rejected earlier - see the header).
 *
 * The checks are purely structural on module-scope symbol shape and
 * the `main` declaration's parameter/return-type form; no expression
 * semantics or const evaluation are performed here.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "fn_main.h"

#include "../diag/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context and record plumbing
 * ------------------------------------------------------------------------- */

typedef struct FnMainCtx {
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unsupported;
} FnMainCtx;

static void fnm_free_records(FnMainCtx *c)
{
    size_t i;
    for (i = 0; i < c->nrecords; i++) diag_record_free(c->records[i]);
    free(c->records);
    c->records = NULL;
    c->nrecords = 0;
    c->records_cap = 0;
}

static DiagRecord *fnm_new_record(FnMainCtx *c, const char *code,
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

static bool fnm_push_record(FnMainCtx *c, DiagRecord *r)
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
 * Entry-module lookup and helpers
 * ------------------------------------------------------------------------- */

/* The entry module: modules[0] when present and marked entry (the name
 * package guarantees the entry is first), with a defensive scan for
 * the is_entry flag. Returns NULL on malformed results (defensive). */
static const NameModule *fnm_entry_module(const NameResult *result)
{
    size_t m;
    if (!result) return NULL;
    if (result->nmodules > 0 && result->modules[0] &&
        result->modules[0]->is_entry) {
        return result->modules[0];
    }
    for (m = 0; m < result->nmodules; m++) {
        if (result->modules[m] && result->modules[m]->is_entry) {
            return result->modules[m];
        }
    }
    return NULL;
}

/* Module-scope lookup by unqualified name; NULL when absent. */
static const NameSymbol *fnm_module_lookup(const NameModule *module,
                                           const char *name)
{
    size_t i;
    if (!module || !name) return NULL;
    for (i = 0; i < module->nmodule_scope; i++) {
        const NameSymbol *sym = module->module_scope[i];
        if (sym && sym->name && strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}

static bool fnm_is_prim(const AstNode *type, AstPrimKind prim)
{
    return type && type->kind == AST_TYPE_PRIM &&
           type->u.type_prim.prim == prim;
}

/* Emit the one authoritative E0418 record (message corpus-pinned). */
static bool fnm_emit_e0418(FnMainCtx *c, const DiagSpan *primary)
{
    DiagRecord *r = fnm_new_record(c, "AIC-E0418",
                                   "entry 'main' signature invalid or missing",
                                   primary);
    if (r) fnm_push_record(c, r);
    return !c->oom;
}

/* ---------------------------------------------------------------------------
 * Reserved-name guard (spec sec. 4.5)
 * ------------------------------------------------------------------------- */

/* Scan every module's module-scope declarations for a declaration name
 * that is one of the reserved built-in spellings covered by the lexer
 * keywords (`cast`, `wrap`, `len`, `ptr`). On a clean pipeline these
 * spellings can never appear: the lexer produces a keyword token, not
 * an identifier, so the parser rejects the declaration with AIC-S0101
 * before name resolution. The guard marks the build unsupported
 * (rather than silently passing) if such a name is nevertheless seen -
 * the defensive contract documented in the header. `rt`/`rt.*` names
 * are NOT in this set: the reserved module names are rejected by the
 * name phase (AIC-N0207/N0208/N0209), and a module-scope symbol
 * literally named `rt` in a user module is not a sec. 4.5 violation
 * (its FQN, e.g. `main.rt`, does not collide with the reserved module
 * namespace). Returns false on OOM or unsupported. */
static bool fnm_check_reserved_names(FnMainCtx *c, const NameResult *result)
{
    static const char *const kReserved[] = { "cast", "wrap", "len", "ptr" };
    size_t m, d, k;

    if (!result) return false;
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *module = result->modules[m];
        if (!module) continue;
        for (d = 0; d < module->nmodule_scope; d++) {
            const NameSymbol *sym = module->module_scope[d];
            if (!sym || !sym->name || !sym->decl) continue;
            for (k = 0; k < sizeof(kReserved) / sizeof(kReserved[0]); k++) {
                if (strcmp(sym->name, kReserved[k]) == 0) {
                    c->unsupported = true;
                    return false;
                }
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Entry main validation (AIC-E0418)
 * ------------------------------------------------------------------------- */

/* Validate the entry module's `main` declaration per spec sec. 15.3:
 * exactly `fn main() -> i32` or `fn main() -> void` (the void form is
 * equivalent to returning 0). Emits at most one E0418 record:
 *   - no module-scope declaration named `main` (or a main-named import
 *     binding, defensive): primary span = the module declaration
 *     (corpus-pinned by derived-semantic-main-missing);
 *   - `main` declared as a non-function (global var/const, struct,
 *     enum): primary span = the whole main-named declaration node;
 *   - `fn main` with parameters: primary span = the fn declaration;
 *   - `fn main` whose return type is neither `i32` nor `void`:
 *     primary span = the fn declaration.
 * A clean parse always yields an explicit return type; a missing one
 * is defensive unsupported input (mirrors fn_core), never guessed.
 * Returns false on OOM or unsupported. */
static bool fnm_check_entry_main(FnMainCtx *c, const NameModule *entry)
{
    const NameSymbol *main_sym;
    const AstNode *decl;
    const AstNode *mdecl;

    if (!entry) return true;
    if (c->oom) return false;   /* caller maps to FN_MAIN_OOM */

    main_sym = fnm_module_lookup(entry, "main");

    if (!main_sym || !main_sym->decl) {
        /* missing main: primary span = the module declaration
         * (corpus-pinned by derived-semantic-main-missing) */
        mdecl = (entry->program && entry->program->kind == AST_PROGRAM)
                    ? entry->program->u.program.module_decl
                    : NULL;
        return fnm_emit_e0418(c, mdecl ? mdecl->span
                                       : (entry->program ? entry->program->span
                                                         : NULL));
    }

    decl = main_sym->decl;

    if (decl->kind != AST_FN_DECL) {
        /* main declared as a non-function: mis-typed */
        return fnm_emit_e0418(c, decl->span);
    }

    if (decl->u.fn_decl.nparams != 0) {
        /* fn main with parameters: mis-typed */
        return fnm_emit_e0418(c, decl->span);
    }

    if (!decl->u.fn_decl.ret_type) {
        /* the grammar always produces an explicit return type on a
         * clean parse; a missing one is defensive unsupported input
         * (mirrors fn_core), never guessed */
        c->unsupported = true;
        return false;
    }

    if (!fnm_is_prim(decl->u.fn_decl.ret_type, AST_PRIM_I32) &&
        !fnm_is_prim(decl->u.fn_decl.ret_type, AST_PRIM_VOID)) {
        /* fn main returning anything other than i32 or void:
         * mis-typed */
        return fnm_emit_e0418(c, decl->span);
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * Build-level entry
 * ------------------------------------------------------------------------- */

FnMainStatus fn_main_check(const NameResult *result,
                           const LayoutBuild *layout,
                           DiagRecord ***out_records,
                           size_t *out_record_count)
{
    FnMainCtx c;
    const NameModule *entry;

    if (!result || !layout) return FN_MAIN_UNSUPPORTED;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    memset(&c, 0, sizeof(c));

    /* reserved-name guard first (defensive; see header) */
    if (!fnm_check_reserved_names(&c, result)) {
        if (c.oom) {
            fnm_free_records(&c);
            return FN_MAIN_OOM;
        }
        /* unsupported: nothing owned */
        return FN_MAIN_UNSUPPORTED;
    }

    entry = fnm_entry_module(result);
    if (!entry) {
        /* no entry module: malformed result (defensive) */
        return FN_MAIN_UNSUPPORTED;
    }

    if (!fnm_check_entry_main(&c, entry)) {
        if (c.oom) {
            fnm_free_records(&c);
            return FN_MAIN_OOM;
        }
        /* unsupported: nothing owned */
        return FN_MAIN_UNSUPPORTED;
    }

    if (c.oom) {
        /* defensive: a helper returned true with oom set (unreachable
         * on the current paths; kept for robustness) */
        fnm_free_records(&c);
        return FN_MAIN_OOM;
    }

    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (c.nrecords) return FN_MAIN_DIAG_ERROR;
    return FN_MAIN_OK;
}
