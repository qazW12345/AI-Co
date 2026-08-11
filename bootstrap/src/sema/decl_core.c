/* bootstrap/src/sema/decl_core.c
 *
 * AI-Co Stage-0 declaration model and assignability (WP-M0-13a1).
 *
 * See decl_core.h for the contract and documented decisions. The
 * declaration model (storage duration, mutability) is a pure function
 * of the resolved symbols; the lvalue analysis is a bounded syntactic
 * walk over the AST and name tables; the build-level walker visits all
 * declaration and expression sites and emits AIC-E0402 / AIC-E0404 /
 * AIC-E0419 records per spec sec. 8 and sec. 12.5.
 *
 * Span/message conventions (corpus-pinned where anchors exist):
 *   - E0402: "address of const is not allowed" (const operand) /
 *     "address of non-lvalue is not allowed" (other non-lvalue);
 *     span = the whole address-of expression node.
 *   - E0404: "assignment to const '<name>'"; span = the immutable
 *     object's declaration identifier (NameSymbol.span).
 *   - E0419: "assignment target is not a modifiable lvalue";
 *     span = the assignment target expression node.
 * All records: phase "semantic" (registry default), severity "error"
 * (registry default), recovery "authoritative".
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "decl_core.h"

#include "../diag/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Declaration model (spec sec. 8.3 / 8.4)
 * ------------------------------------------------------------------------- */

DeclStorageKind decl_storage_of_symbol(const NameSymbol *sym)
{
    if (!sym) return DECL_STORAGE_NONE;
    switch (sym->kind) {
    case NAME_SYM_GLOBAL_VAR:
        return DECL_STORAGE_STATIC;
    case NAME_SYM_LOCAL_VAR:
    case NAME_SYM_PARAM:
        return DECL_STORAGE_AUTOMATIC;
    case NAME_SYM_GLOBAL_CONST:
    case NAME_SYM_LOCAL_CONST:
    case NAME_SYM_FN:
    case NAME_SYM_STRUCT:
    case NAME_SYM_ENUM:
    case NAME_SYM_FIELD:
    case NAME_SYM_ENUM_MEMBER:
    case NAME_SYM_MODULE_IMPORT:
    default:
        /* consts have no storage (sec. 8.1/8.3); the remaining kinds
         * are not storage-bearing declarations (defensive). */
        return DECL_STORAGE_NONE;
    }
}

DeclMutability decl_mutability_of_symbol(const NameSymbol *sym)
{
    if (!sym) return DECL_IMMUTABLE;
    switch (sym->kind) {
    case NAME_SYM_GLOBAL_VAR:
    case NAME_SYM_LOCAL_VAR:
    case NAME_SYM_PARAM:
    case NAME_SYM_FIELD:
        return DECL_MUTABLE;
    case NAME_SYM_GLOBAL_CONST:
    case NAME_SYM_LOCAL_CONST:
    case NAME_SYM_ENUM_MEMBER:
        return DECL_IMMUTABLE;
    case NAME_SYM_FN:
    case NAME_SYM_STRUCT:
    case NAME_SYM_ENUM:
    case NAME_SYM_MODULE_IMPORT:
    default:
        /* not storage-bearing values; immutable is the defensive
         * classification (never reached in a valid pipeline). */
        return DECL_IMMUTABLE;
    }
}

/* ---------------------------------------------------------------------------
 * Bounded type probes (exact `str` detection for the sec. 12.2 index rule
 * and struct-field resolution for value member access)
 * ------------------------------------------------------------------------- */

/* True when an AST type node denotes exactly `str` (no container
 * recursion: str[2], str[], and str* are NOT str). */
static bool type_node_is_exact_str(const AstNode *type_node)
{
    return type_node && type_node->kind == AST_TYPE_PRIM &&
           type_node->u.type_prim.prim == AST_PRIM_STR;
}

/* The declared type node of a declaration symbol, or NULL when the
 * symbol has no declaration or its declaration carries no type. */
static const AstNode *symbol_decl_type_node(const NameSymbol *sym)
{
    const AstNode *d;
    if (!sym) return NULL;
    d = sym->decl;
    if (!d) return NULL;
    switch (d->kind) {
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        return d->u.global_decl.type;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        return d->u.local_decl.type;
    case AST_PARAM:
    case AST_FIELD_DECL:
        return d->u.named.type;
    default:
        return NULL;
    }
}

/* Resolve a named type node (AST_TYPE_NAMED) to its declaration symbol:
 * single-part names resolve in the module scope; qualified names match
 * the longest prefix that is the current module or a direct import, then
 * the remaining parts in that module's scope (spec sec. 6.6). Returns
 * NULL when the type is not named or cannot be resolved (the
 * completeness/type stages would have reported it upstream). */
static const NameSymbol *named_type_symbol(const NameModule *module,
                                           const AstNode *type_node)
{
    const AstName *nm;
    size_t k;
    if (!module || !type_node || type_node->kind != AST_TYPE_NAMED) return NULL;
    nm = type_node->u.type_named.name;
    if (!nm || nm->count == 0) return NULL;
    if (nm->count == 1) {
        return name_module_lookup(module, nm->parts[0]);
    }
    /* longest module prefix parts[0..k), then the type name parts[k] */
    for (k = nm->count - 1; k >= 1; k--) {
        const NameModule *target = NULL;
        size_t i, prefix_len = 0;
        for (i = 0; i < k; i++) prefix_len += strlen(nm->parts[i]) + 1;
        {
            char *prefix = (char *)malloc(prefix_len);
            if (!prefix) return NULL;
            prefix[0] = '\0';
            for (i = 0; i < k; i++) {
                if (i) strcat(prefix, ".");
                strcat(prefix, nm->parts[i]);
            }
            if (module->fqn && strcmp(module->fqn, prefix) == 0) {
                target = module;
            } else {
                for (i = 0; i < module->nimports; i++) {
                    if (module->imports[i] && module->imports[i]->fqn &&
                        strcmp(module->imports[i]->fqn, prefix) == 0) {
                        target = module->imports[i];
                        break;
                    }
                }
            }
            free(prefix);
        }
        if (!target) continue;
        return name_module_lookup(target, nm->parts[k]);
    }
    return NULL;
}

/* The field symbol of a member/arrow expression (forward). */
static const NameSymbol *field_sym_of(const NameModule *module,
                                      const AstNode *expr);

/* The AST type node denoting the value type of `expr`, over the
 * lvalue-forming subset: identifiers and struct fields use their
 * declared type; parentheses recurse; dereference peels `T*`; indexing
 * peels `T[N]`/`T[]` to the element type. Non-lvalue forms and
 * un-derivable types return NULL. */
static const AstNode *expr_type_node(const NameModule *module,
                                     const AstNode *e)
{
    const AstNode *t;
    if (!e) return NULL;
    switch (e->kind) {
    case AST_EXPR_IDENT:
        return symbol_decl_type_node(name_symbol_for_node(module, e));
    case AST_EXPR_PAREN:
        return expr_type_node(module, e->u.paren.expr);
    case AST_EXPR_UNARY:
        if (e->u.unary.op != AST_UN_DEREF) return NULL;
        t = expr_type_node(module, e->u.unary.operand);
        if (t && t->kind == AST_TYPE_PTR) return t->u.type_derived.base;
        return NULL;
    case AST_EXPR_INDEX:
        t = expr_type_node(module, e->u.index_slice.base);
        if (t && (t->kind == AST_TYPE_ARRAY || t->kind == AST_TYPE_SLICE)) {
            return t->u.type_derived.base;
        }
        return NULL;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        const NameSymbol *f = field_sym_of(module, e);
        return f ? symbol_decl_type_node(f) : NULL;
    }
    default:
        return NULL;
    }
}

/* The field symbol of a value member/arrow access, resolved through the
 * base expression's struct type (the name package defers field names to
 * the type layer; the field symbols live on the struct declaration's
 * member list). Returns NULL when the base is not a struct value or the
 * field is absent (upstream type checks own those rejections). */
static const NameSymbol *field_sym_of(const NameModule *module,
                                      const AstNode *expr)
{
    const AstNode *base, *bt;
    const NameSymbol *st;
    size_t i;
    if (!module || !expr) return NULL;
    if (expr->kind != AST_EXPR_MEMBER && expr->kind != AST_EXPR_ARROW) return NULL;
    base = expr->u.member.base;
    bt = expr_type_node(module, base);
    /* p->f == (*p).f: the base is a struct pointer; peel T*. */
    if (expr->kind == AST_EXPR_ARROW && bt && bt->kind == AST_TYPE_PTR) {
        bt = bt->u.type_derived.base;
    }
    st = named_type_symbol(module, bt);
    if (!st || st->kind != NAME_SYM_STRUCT) return NULL;
    for (i = 0; i < st->nmembers; i++) {
        const NameSymbol *f = st->members[i];
        if (f && f->name && strcmp(f->name, expr->u.member.name) == 0) return f;
    }
    return NULL;
}

/* True when `expr` has exactly `str` type (sec. 12.2: str is immutable
 * and its element access yields a u8 value, never an lvalue). */
static bool expr_is_str_type(const NameModule *module, const AstNode *e)
{
    return type_node_is_exact_str(expr_type_node(module, e));
}

/* ---------------------------------------------------------------------------
 * Lvalue analysis (spec sec. 8.4, sec. 10.2, sec. 12.5)
 * ------------------------------------------------------------------------- */

static DeclLvalue lvalue_of_ident(const NameModule *module,
                                  const AstNode *expr)
{
    DeclLvalue rv = { DECL_LVALUE_NONE, NULL };
    const NameSymbol *sym = name_symbol_for_node(module, expr);
    if (!sym) return rv;
    switch (sym->kind) {
    case NAME_SYM_GLOBAL_VAR:
    case NAME_SYM_LOCAL_VAR:
    case NAME_SYM_PARAM:
        /* sec. 8.4: var names and parameters are mutable lvalues. */
        rv.kind = DECL_LVALUE_MUTABLE;
        break;
    case NAME_SYM_GLOBAL_CONST:
    case NAME_SYM_LOCAL_CONST:
        /* sec. 8.4: const names denote immutable values. */
        rv.kind = DECL_LVALUE_CONST;
        rv.const_sym = sym;
        break;
    default:
        /* function / struct / enum / module names are not lvalues. */
        break;
    }
    return rv;
}

DeclLvalue decl_expr_lvalue(const NameModule *module, const AstNode *expr)
{
    DeclLvalue rv = { DECL_LVALUE_NONE, NULL };
    if (!module || !expr) return rv;
    switch (expr->kind) {
    case AST_EXPR_IDENT:
        return lvalue_of_ident(module, expr);
    case AST_EXPR_PAREN:
        return decl_expr_lvalue(module, expr->u.paren.expr);
    case AST_EXPR_UNARY:
        if (expr->u.unary.op == AST_UN_DEREF) {
            /* sec. 12.5: the dereference result is a mutable lvalue of
             * type T (always mutable; the pointee carries no const
             * qualifier in the minimal language). */
            rv.kind = DECL_LVALUE_MUTABLE;
            return rv;
        }
        /* &e, -e, +e, !e, ~e are values. */
        return rv;
    case AST_EXPR_INDEX: {
        const AstNode *base = expr->u.index_slice.base;
        DeclLvalue b = decl_expr_lvalue(module, base);
        if (b.kind == DECL_LVALUE_NONE) return rv;
        /* sec. 12.2: str indexing "yields u8 (byte value)" - a value,
         * never an lvalue (str is immutable). */
        if (expr_is_str_type(module, base)) return rv;
        /* array/slice element access inherits the base's kind. */
        return b;
    }
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        const NameSymbol *sym = name_symbol_for_node(module, expr);
        if (sym && sym->kind == NAME_SYM_ENUM_MEMBER) {
            /* sec. 7.5: enum members are constants - immutable. */
            rv.kind = DECL_LVALUE_CONST;
            rv.const_sym = sym;
            return rv;
        }
        /* Value member access: the name package defers the field name to
         * the type layer (no ref on the member node), so resolve the
         * field through the base's struct type. */
        if (!sym || sym->kind != NAME_SYM_FIELD) {
            sym = field_sym_of(module, expr);
        }
        if (!sym || sym->kind != NAME_SYM_FIELD) return rv;
        if (expr->kind == AST_EXPR_ARROW) {
            /* p->f == (*p).f (sec. 12.6); the dereference result is a
             * mutable lvalue (sec. 12.5), so arrow field access is
             * always mutable. */
            rv.kind = DECL_LVALUE_MUTABLE;
            return rv;
        }
        /* s.f inherits the base's lvalue kind and mutability. */
        return decl_expr_lvalue(module, expr->u.member.base);
    }
    default:
        /* literals, binary/unary values, calls, ternary, casts, wraps,
         * len/ptr, slice expressions, struct literals, assignments and
         * every other expression form are values, not lvalues. */
        return rv;
    }
}

/* ---------------------------------------------------------------------------
 * Record building and the check walker
 * ------------------------------------------------------------------------- */

typedef struct DeclCtx {
    DiagRecord **records;
    size_t nrecords, cap;
    bool oom;
} DeclCtx;

static DiagRecord *decl_new_record(DeclCtx *c, const char *code,
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

static bool decl_push_record(DeclCtx *c, DiagRecord *r)
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

/* AIC-E0404: assignment target resolves to an immutable object. */
static bool check_assign_to_const(DeclCtx *c, const NameSymbol *const_sym)
{
    char msg[128];
    DiagRecord *r;
    if (!const_sym || !const_sym->name || !const_sym->span) return true;
    snprintf(msg, sizeof msg, "assignment to const '%s'", const_sym->name);
    r = decl_new_record(c, "AIC-E0404", msg, const_sym->span);
    return decl_push_record(c, r);
}

/* AIC-E0419: assignment target is not an lvalue at all. */
static bool check_assign_non_lvalue(DeclCtx *c, const AstNode *target)
{
    DiagRecord *r = decl_new_record(c, "AIC-E0419",
                                    "assignment target is not a modifiable lvalue",
                                    target ? target->span : NULL);
    return decl_push_record(c, r);
}

/* Check one assignment target (plain or compound, sec. 11.6). */
static bool check_assign_target(DeclCtx *c, const NameModule *module,
                                const AstNode *target)
{
    DeclLvalue lv;
    if (!target) return true;
    lv = decl_expr_lvalue(module, target);
    if (lv.kind == DECL_LVALUE_MUTABLE) return true;
    if (lv.kind == DECL_LVALUE_CONST)
        return check_assign_to_const(c, lv.const_sym);
    return check_assign_non_lvalue(c, target);
}

/* AIC-E0402: address-of requires a mutable lvalue operand (sec. 12.5). */
static bool check_addr_operand(DeclCtx *c, const NameModule *module,
                               const AstNode *unary)
{
    const AstNode *operand = unary->u.unary.operand;
    DeclLvalue lv = decl_expr_lvalue(module, operand);
    const char *msg;
    DiagRecord *r;
    if (lv.kind == DECL_LVALUE_MUTABLE) return true;
    if (lv.kind == DECL_LVALUE_CONST)
        msg = "address of const is not allowed";
    else
        msg = "address of non-lvalue is not allowed";
    /* Primary span: the whole address-of expression (corpus-pinned). */
    r = decl_new_record(c, "AIC-E0402", msg, unary->span);
    return decl_push_record(c, r);
}

/* Walk an AST type node: array extents are constant expressions that can
 * syntactically contain address-of (e.g. T[&X]); walk them for E0402.
 * Other type forms carry no expressions. */
static bool walk_expr(DeclCtx *c, const NameModule *module,
                      const AstNode *e);
static bool walk_type(DeclCtx *c, const NameModule *module,
                      const AstNode *type_node)
{
    if (!type_node || c->oom) return true;
    if (type_node->kind == AST_TYPE_ARRAY) {
        if (!walk_expr(c, module, type_node->u.type_derived.len)) return false;
        return walk_type(c, module, type_node->u.type_derived.base);
    }
    if (type_node->kind == AST_TYPE_PTR ||
        type_node->kind == AST_TYPE_SLICE) {
        return walk_type(c, module, type_node->u.type_derived.base);
    }
    return true;
}

/* Recursively walk an expression, checking assignment targets and
 * address-of operands and descending into every sub-expression. */
static bool walk_expr(DeclCtx *c, const NameModule *module,
                      const AstNode *e)
{
    size_t i;
    if (!e || c->oom) return true;
    switch (e->kind) {
    case AST_EXPR_ASSIGN:
        if (!check_assign_target(c, module, e->u.assign.target)) return false;
        if (!walk_expr(c, module, e->u.assign.target)) return false;
        return walk_expr(c, module, e->u.assign.value);
    case AST_EXPR_UNARY:
        if (e->u.unary.op == AST_UN_ADDR) {
            if (!check_addr_operand(c, module, e)) return false;
        }
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
            if (!walk_expr(c, module, e->u.array_literal.elems[i])) return false;
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
        /* sizeof does not evaluate its operand, but the address-of
         * rejection is a syntactic rule (sec. 8.1 "taking its address is
         * rejected"), so the operand is still checked. */
        return walk_expr(c, module, e->u.size_op.operand);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        if (!walk_type(c, module, e->u.cast_wrap.type)) return false;
        return walk_expr(c, module, e->u.cast_wrap.expr);
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        return walk_expr(c, module, e->u.size_op.operand);
    case AST_EXPR_INT_LITERAL:
    case AST_EXPR_STR_LITERAL:
    case AST_EXPR_BOOL_LITERAL:
    case AST_EXPR_NULL_LITERAL:
    case AST_EXPR_IDENT:
    default:
        return true;
    }
}

/* Recursively walk a statement (13c owns statement semantics; this walk
 * only discovers assignment and address-of expressions). */
static bool walk_stmt(DeclCtx *c, const NameModule *module,
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
    case AST_CONST_DECL:
        if (!walk_type(c, module, s->u.local_decl.type)) return false;
        return walk_expr(c, module, s->u.local_decl.init);
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
                if (!walk_expr(c, module, cl->u.clause.value)) return false;
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
    default:
        return true;
    }
}

/* Walk one module-scope declaration (module_scope iteration, the same
 * deterministic order the const-eval stage uses). */
static bool walk_decl(DeclCtx *c, const NameModule *module,
                      const AstNode *decl)
{
    size_t i;
    if (!decl || c->oom) return true;
    switch (decl->kind) {
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        if (!walk_type(c, module, decl->u.global_decl.type)) return false;
        return walk_expr(c, module, decl->u.global_decl.init);
    case AST_ENUM_DECL:
        for (i = 0; i < decl->u.enum_decl.nmembers; i++) {
            const AstNode *mem = decl->u.enum_decl.members[i];
            if (mem && mem->u.named.value) {
                if (!walk_expr(c, module, mem->u.named.value)) return false;
            }
        }
        return true;
    case AST_STRUCT_DECL:
        for (i = 0; i < decl->u.struct_decl.nfields; i++) {
            const AstNode *f = decl->u.struct_decl.fields[i];
            if (f) {
                if (!walk_type(c, module, f->u.named.type)) return false;
            }
        }
        return true;
    case AST_FN_DECL:
        /* parameter types (array extents) then the body. */
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

DeclStatus decl_check(const NameResult *result,
                      DiagRecord ***out_records,
                      size_t *out_record_count)
{
    DeclCtx c;
    size_t m;
    if (!result) return DECL_UNSUPPORTED;
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
            if (!walk_decl(&c, mod, sym->decl)) break;
        }
        if (c.oom) break;
    }
    if (c.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        return DECL_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (c.nrecords) return DECL_DIAG_ERROR;
    return DECL_OK;
}
