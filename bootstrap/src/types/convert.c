/* bootstrap/src/types/convert.c
 *
 * AI-Co Stage-0 implicit conversions and common type (WP-M0-11c).
 * See convert.h for the contract and design decisions.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "convert.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Implicit-conversion whitelist (spec sec. 11.1)
 * ------------------------------------------------------------------------- */

static const TypePrimInfo *prim_of(const Type *t)
{
    if (!t || t->kind != TYPE_PRIM) return NULL;
    return types_prim_info(t->u.prim);
}

/* The ten integer rows of Table 11.1, computed from the prim table facts
 * (width/sign) so the predicate matches the spec exactly:
 *   - different sign: only unsigned -> signed with a strictly wider
 *     target (u8->i16 yes, u8->i8 no, i8->u16 no);
 *   - same sign: the target must be at least as wide (identical-width
 *     identical-sign pairs i64<->isize and u64<->usize are allowed;
 *     narrowing never is). */
static bool int_implicit(const TypePrimInfo *pf, const TypePrimInfo *pt)
{
    if (pf->is_signed != pt->is_signed) {
        return !pf->is_signed && pt->is_signed &&
               pt->width_bits > pf->width_bits;
    }
    return pt->width_bits >= pf->width_bits;
}

bool convert_implicit_allowed(const Type *from, const Type *to)
{
    const TypePrimInfo *pf, *pt;
    if (!from || !to) return false;
    /* Identity (sec. 11.6: "identical type or per Table 11.1"). */
    if (type_identical(from, to)) return true;
    /* any T* -> T* (same type): same pointee type. */
    if (from->kind == TYPE_PTR && to->kind == TYPE_PTR) {
        return type_identical(from->u.ptr.elem, to->u.ptr.elem);
    }
    pf = prim_of(from);
    pt = prim_of(to);
    if (!pf || !pt || !pf->is_integer || !pt->is_integer) return false;
    return int_implicit(pf, pt);
}

bool convert_implicit_allowed_ex(const Type *from, bool from_is_null,
                                 const Type *to)
{
    if (from_is_null) {
        /* Caller contract: a null source carries no Type. */
        if (from != NULL) return false;
        return to != NULL && to->kind == TYPE_PTR;
    }
    if (from == NULL) return false;
    return convert_implicit_allowed(from, to);
}

/* The common type of two integer types, without allocation: writes the
 * kind into *out and returns true, or returns false when no common type
 * exists (non-integer operands, equal-width different-sign pairs, or
 * narrower-not-convertible pairs). See convert_common_type for the
 * rules and the equal-width tie-break. */
static bool common_type_kind(const Type *a, const Type *b, AstPrimKind *out)
{
    const TypePrimInfo *pa, *pb;
    if (!a || !b) return false;
    pa = prim_of(a);
    pb = prim_of(b);
    if (!pa || !pb || !pa->is_integer || !pb->is_integer) return false;
    if (type_identical(a, b)) {
        *out = a->u.prim;
        return true;
    }
    if (pa->width_bits > pb->width_bits) {
        if (!int_implicit(pb, pa)) return false;
        *out = a->u.prim;
        return true;
    }
    if (pb->width_bits > pa->width_bits) {
        if (!int_implicit(pa, pb)) return false;
        *out = b->u.prim;
        return true;
    }
    /* Equal width, different types: different sign -> no common type
     * (neither value set contains the other). Identical sign is the
     * i64<->isize / u64<->usize pair: both conversions are in the
     * table; the deterministic tie-break prefers the fixed-width type
     * (commutative). */
    if (pa->is_signed != pb->is_signed) return false;
    *out = pa->is_pointer_sized ? b->u.prim : a->u.prim;
    return true;
}

Type *convert_common_type(const Type *a, const Type *b)
{
    AstPrimKind common;
    if (!common_type_kind(a, b, &common)) return NULL;
    return type_prim_new(common);
}

/* ---------------------------------------------------------------------------
 * Build-level conversion check (spec sec. 11.1 bullet / sec. 11.6)
 * ------------------------------------------------------------------------- */

typedef struct ConvertCtx {
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unknown;      /* some expression was outside the 11c subset */
} ConvertCtx;

static bool ctx_rec_push(ConvertCtx *c, DiagRecord *r)
{
    if (c->nrecords == c->records_cap) {
        size_t ncap = c->records_cap ? c->records_cap * 2 : 8;
        DiagRecord **nr = (DiagRecord **)realloc(
            c->records, ncap * sizeof(DiagRecord *));
        if (!nr) { c->oom = true; return false; }
        c->records = nr;
        c->records_cap = ncap;
    }
    c->records[c->nrecords++] = r;
    return true;
}

static DiagRecord *new_type_record(ConvertCtx *c, const char *code,
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

static void emit_record(ConvertCtx *c, const char *message,
                        const DiagSpan *span)
{
    DiagRecord *r = new_type_record(c, "AIC-T0307", message, span);
    if (r && !ctx_rec_push(c, r)) diag_record_free(r);
}

/* ---------------------------------------------------------------------------
 * Type construction from AST type nodes / symbols
 * ------------------------------------------------------------------------- */

typedef enum TyperStatus {
    TYP_OK = 0,       /* *out set (or *out_is_null for null literals) */
    TYP_UNKNOWN,      /* outside the 11c subset; *out untouched */
    TYP_OOM           /* allocation failure; nothing owned */
} TyperStatus;

static TyperStatus unk(ConvertCtx *c)
{
    c->unknown = true;
    return TYP_UNKNOWN;
}

static TyperStatus oom(ConvertCtx *c)
{
    c->oom = true;
    return TYP_OOM;
}

/* Recursive copy of a Type graph (the expression typer needs fresh
 * ownership for results derived from borrowed descriptors). NULL on
 * allocation failure (nothing owned). */
static Type *type_clone(const Type *t)
{
    Type *e, *r;
    if (!t) return NULL;
    switch (t->kind) {
    case TYPE_PRIM:
        return type_prim_new(t->u.prim);
    case TYPE_PTR:
        e = type_clone(t->u.ptr.elem);
        if (!e) return NULL;
        r = type_ptr_new(e);
        if (!r) { type_free(e); return NULL; }
        return r;
    case TYPE_SLICE:
        e = type_clone(t->u.slice.elem);
        if (!e) return NULL;
        r = type_slice_new(e);
        if (!r) { type_free(e); return NULL; }
        return r;
    case TYPE_ARRAY:
        e = type_clone(t->u.array.elem);
        if (!e) return NULL;
        r = type_array_new(e, t->u.array.extent);
        if (!r) { type_free(e); return NULL; }
        return r;
    case TYPE_STRUCT:
        return type_struct_new(t->u.sym);
    case TYPE_ENUM:
        return type_enum_new(t->u.sym);
    }
    return NULL;
}

static AstPrimKind prim_from_lex(LexIntType t)
{
    switch (t) {
    case LEX_INT_I8:    return AST_PRIM_I8;
    case LEX_INT_I16:   return AST_PRIM_I16;
    case LEX_INT_I32:   return AST_PRIM_I32;
    case LEX_INT_I64:   return AST_PRIM_I64;
    case LEX_INT_U8:    return AST_PRIM_U8;
    case LEX_INT_U16:   return AST_PRIM_U16;
    case LEX_INT_U32:   return AST_PRIM_U32;
    case LEX_INT_U64:   return AST_PRIM_U64;
    case LEX_INT_ISIZE: return AST_PRIM_ISIZE;
    case LEX_INT_USIZE: return AST_PRIM_USIZE;
    }
    return AST_PRIM_I32;   /* defensive; lexer always sets a valid kind */
}

/* Build a Type descriptor from an AST type node in `module`. The array
 * extent uses the 11b bounded constant-integer subset; an extent outside
 * it yields TYP_UNKNOWN (WP-M0-12 owns full const evaluation). */
static TyperStatus type_from_type_node(ConvertCtx *c, const NameModule *module,
                                       const AstNode *tn, Type **out)
{
    Type *e;
    *out = NULL;
    if (!tn || !module) return unk(c);
    switch (tn->kind) {
    case AST_TYPE_PRIM:
        *out = type_prim_new(tn->u.type_prim.prim);
        break;
    case AST_TYPE_NAMED: {
        const NameSymbol *sym = name_symbol_for_node(module, tn);
        if (!sym) return unk(c);
        if (sym->kind == NAME_SYM_STRUCT) {
            *out = type_struct_new(sym);
        } else if (sym->kind == NAME_SYM_ENUM) {
            *out = type_enum_new(sym);
        } else {
            return unk(c);
        }
        break;
    }
    case AST_TYPE_PTR:
        e = NULL;
        {
            TyperStatus st = type_from_type_node(c, module,
                                                 tn->u.type_derived.base, &e);
            if (st != TYP_OK) return st;
        }
        *out = type_ptr_new(e);
        if (!*out) { type_free(e); return oom(c); }
        break;
    case AST_TYPE_SLICE:
        e = NULL;
        {
            TyperStatus st = type_from_type_node(c, module,
                                                 tn->u.type_derived.base, &e);
            if (st != TYP_OK) return st;
        }
        *out = type_slice_new(e);
        if (!*out) { type_free(e); return oom(c); }
        break;
    case AST_TYPE_ARRAY: {
        LayoutEvalValue ev;
        TyperStatus st = type_from_type_node(c, module,
                                             tn->u.type_derived.base, &e);
        if (st != TYP_OK) return st;
        if (layout_eval_int_expr(tn->u.type_derived.len, &ev) !=
                LAYOUT_EVAL_OK ||
            ev.big || ev.v < 0) {
            type_free(e);
            return unk(c);
        }
        *out = type_array_new(e, ev.v);
        if (!*out) { type_free(e); return oom(c); }
        break;
    }
    default:
        return unk(c);
    }
    if (!*out) return oom(c);
    return TYP_OK;
}

/* The value type of a resolved symbol (variable/constant/parameter/
 * field/enum member). A bare function name or type name is not a value:
 * TYP_UNKNOWN (operator typing, WP-M0-11d, rejects its misuse). */
static TyperStatus type_of_symbol_type(ConvertCtx *c, const NameSymbol *sym,
                                       Type **out)
{
    *out = NULL;
    if (!sym || !sym->decl) return unk(c);
    switch (sym->kind) {
    case NAME_SYM_GLOBAL_VAR:
    case NAME_SYM_GLOBAL_CONST:
        return type_from_type_node(c, sym->module,
                                   sym->decl->u.global_decl.type, out);
    case NAME_SYM_LOCAL_VAR:
    case NAME_SYM_LOCAL_CONST:
        return type_from_type_node(c, sym->module,
                                   sym->decl->u.local_decl.type, out);
    case NAME_SYM_PARAM:
    case NAME_SYM_FIELD:
        return type_from_type_node(c, sym->module,
                                   sym->decl->u.named.type, out);
    case NAME_SYM_ENUM_MEMBER:
        *out = type_enum_new(sym->owner);
        return *out ? TYP_OK : oom(c);
    default:
        return unk(c);
    }
}

/* The field declaration of `name` in a struct type, or NULL. */
static const AstNode *struct_field_decl(const Type *t, const char *name)
{
    const AstNode *decl;
    size_t i;
    if (!t || t->kind != TYPE_STRUCT || !t->u.sym || !t->u.sym->decl) {
        return NULL;
    }
    decl = t->u.sym->decl;
    for (i = 0; i < decl->u.struct_decl.nfields; i++) {
        const AstNode *f = decl->u.struct_decl.fields[i];
        if (f->u.named.name && strcmp(f->u.named.name, name) == 0) {
            return f;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Diagnostic descriptions and AIC-T0307 emission
 * ------------------------------------------------------------------------- */

/* Render a source-expression description for messages: literals carry
 * their value ("i32 literal 200", corpus-pinned), identifiers/members
 * carry the name ("u8[] value 'buf'"), everything else is the type
 * rendering. Writes into `buf` (bounded, deterministic). `t` must be
 * non-NULL for non-null sources. */
static void desc_from_expr(const AstNode *e, const Type *t, char *buf,
                           size_t bufsize)
{
    const TypePrimInfo *p;
    char *td;
    if (!e) {
        td = type_describe(t);
        if (td) {
            snprintf(buf, bufsize, "%s", td);
            free(td);
        } else {
            snprintf(buf, bufsize, "?");
        }
        return;
    }
    switch (e->kind) {
    case AST_EXPR_INT_LITERAL: {
        p = t ? prim_of(t) : NULL;
        if (e->u.int_literal.is_min) {
            snprintf(buf, bufsize, "%s literal -%llu",
                     p ? p->name : "int",
                     (unsigned long long)e->u.int_literal.value);
        } else {
            snprintf(buf, bufsize, "%s literal %llu",
                     p ? p->name : "int",
                     (unsigned long long)e->u.int_literal.value);
        }
        return;
    }
    case AST_EXPR_BOOL_LITERAL:
        snprintf(buf, bufsize, "bool literal %s",
                 e->u.bool_literal.value ? "true" : "false");
        return;
    case AST_EXPR_STR_LITERAL:
        snprintf(buf, bufsize, "str literal");
        return;
    case AST_EXPR_NULL_LITERAL:
        snprintf(buf, bufsize, "null");
        return;
    case AST_EXPR_IDENT:
    case AST_EXPR_MEMBER: {
        const char *name = (e->kind == AST_EXPR_IDENT)
            ? e->u.ident.name : e->u.member.name;
        td = type_describe(t);
        if (td) {
            snprintf(buf, bufsize, "%s value '%s'", td, name ? name : "?");
            free(td);
        } else {
            snprintf(buf, bufsize, "value '%s'", name ? name : "?");
        }
        return;
    }
    default:
        td = type_describe(t);
        if (td) {
            snprintf(buf, bufsize, "%s", td);
            free(td);
        } else {
            snprintf(buf, bufsize, "?");
        }
        return;
    }
}

/* The primary span for a rejected conversion: the corpus pins identifier
 * sources to their declaration (tests/negative/cases/18-6-type-array-
 * slice-implicit expects the declaration identifier of `buf`, not the
 * reference); literals and other expressions use their own span. */
static const DiagSpan *conversion_span(const NameModule *module,
                                       const AstNode *from_expr,
                                       const DiagSpan *fallback)
{
    const NameSymbol *sym;
    if (!from_expr) return fallback;
    if (from_expr->kind == AST_EXPR_IDENT ||
        from_expr->kind == AST_EXPR_MEMBER ||
        from_expr->kind == AST_EXPR_ARROW) {
        sym = name_symbol_for_node(module, from_expr);
        if (sym && sym->span) return sym->span;
    }
    return fallback;
}

/* Emit the AIC-T0307 record for a rejected conversion at `span`, choosing
 * the corpus-pinned message shapes: integer narrowing ("cannot be
 * implicitly narrowed to", with " in array literal" for array-literal
 * elements), array-to-slice ("implicit array-to-slice conversion is
 * absent"), and the deterministic general shape ("is not implicitly
 * convertible to"). */
static void emit_conversion_fail(ConvertCtx *c, const NameModule *module,
                                 const AstNode *from_expr,
                                 const Type *from, bool from_is_null,
                                 const Type *to, const DiagSpan *span,
                                 bool in_array_literal)
{
    char frombuf[256];
    char *td;
    char msg[512];
    bool from_int, to_int;
    const TypePrimInfo *pf, *pt;

    if (from_is_null) {
        snprintf(frombuf, sizeof(frombuf), "null");
    } else {
        desc_from_expr(from_expr, from, frombuf, sizeof(frombuf));
    }
    pf = from ? prim_of(from) : NULL;
    pt = to ? prim_of(to) : NULL;
    from_int = from != NULL && pf != NULL && pf->is_integer;
    to_int = to != NULL && pt != NULL && pt->is_integer;

    td = type_describe(to);
    if (from && from->kind == TYPE_ARRAY && to && to->kind == TYPE_SLICE) {
        snprintf(msg, sizeof(msg),
                 "implicit array-to-slice conversion is absent");
    } else if (from_int && to_int) {
        if (in_array_literal) {
            snprintf(msg, sizeof(msg),
                     "no common type: %s cannot be implicitly narrowed to "
                     "%s in array literal",
                     frombuf, td ? td : "?");
        } else {
            snprintf(msg, sizeof(msg),
                     "no common type: %s cannot be implicitly narrowed to %s",
                     frombuf, td ? td : "?");
        }
    } else {
        snprintf(msg, sizeof(msg),
                 "no common type: %s is not implicitly convertible to %s",
                 frombuf, td ? td : "?");
    }
    free(td);
    emit_record(c, msg, conversion_span(module, from_expr, span));
}

/* Check one conversion site: `from` (derived type of from_expr, or a null
 * literal when from_is_null) must be implicitly convertible to `to`
 * (sec. 11.1 whitelist). On failure an AIC-T0307 record is emitted with
 * the corpus-pinned span (identifier sources: their declaration) and
 * message shape. */
static void check_conversion(ConvertCtx *c, const NameModule *module,
                             const AstNode *from_expr, const Type *from,
                             bool from_is_null, const Type *to,
                             const DiagSpan *span, bool in_array_literal)
{
    if (convert_implicit_allowed_ex(from, from_is_null, to)) return;
    emit_conversion_fail(c, module, from_expr, from, from_is_null, to, span,
                         in_array_literal);
}

/* Binary/common-type failure: "no common type: <a> and <b>" (type
 * renderings; deterministic, unpinned). */
static void emit_no_common_at(ConvertCtx *c, const Type *a, const Type *b,
                              const DiagSpan *span)
{
    char msg[512];
    char *ad = type_describe(a);
    char *bd = type_describe(b);
    snprintf(msg, sizeof(msg), "no common type: %s and %s",
             ad ? ad : "?", bd ? bd : "?");
    free(ad);
    free(bd);
    emit_record(c, msg, span);
}

static void emit_no_common(ConvertCtx *c, const AstNode *node,
                           const Type *a, const Type *b)
{
    emit_no_common_at(c, a, b, node->span);
}

/* ---------------------------------------------------------------------------
 * Bounded expression typer (the 11c subset; see convert.h)
 * ------------------------------------------------------------------------- */

/* The struct type named by a struct-literal base: the parser represents
 * `P { ... }` with P as an identifier expression (or a named type node
 * for qualified forms); resolve it to the struct declaration symbol. */
static TyperStatus type_of_struct_init_base(ConvertCtx *c,
                                            const NameModule *module,
                                            const AstNode *base, Type **out)
{
    const NameSymbol *sym;
    if (!base) return unk(c);
    if (base->kind == AST_TYPE_NAMED) {
        return type_from_type_node(c, module, base, out);
    }
    if (base->kind == AST_EXPR_IDENT) {
        sym = name_symbol_for_node(module, base);
        if (sym && sym->kind == NAME_SYM_STRUCT) {
            *out = type_struct_new(sym);
            return *out ? TYP_OK : oom(c);
        }
        return unk(c);   /* non-struct literal: 11d AIC-T0306 */
    }
    return unk(c);
}

static TyperStatus type_of_expr(ConvertCtx *c, const NameModule *module,
                                const AstNode *e, Type **out,
                                bool *out_is_null);

/* Type one expression that carries no conversion obligation of its own
 * (conditions, selectors, statements) so inner conversion sites are
 * checked. */
static void check_expr(ConvertCtx *c, const NameModule *module,
                       const AstNode *e)
{
    Type *t = NULL;
    bool isnull = false;
    TyperStatus st;
    if (!e) return;
    st = type_of_expr(c, module, e, &t, &isnull);
    if (st == TYP_OOM) c->oom = true;
    type_free(t);
}

static TyperStatus type_of_expr(ConvertCtx *c, const NameModule *module,
                                const AstNode *e, Type **out,
                                bool *out_is_null)
{
    *out = NULL;
    if (out_is_null) *out_is_null = false;
    if (!e) return TYP_UNKNOWN;

    switch (e->kind) {
    case AST_EXPR_INT_LITERAL:
        *out = type_prim_new(prim_from_lex(e->u.int_literal.type));
        break;
    case AST_EXPR_BOOL_LITERAL:
        *out = type_prim_new(AST_PRIM_BOOL);
        break;
    case AST_EXPR_STR_LITERAL:
        *out = type_prim_new(AST_PRIM_STR);
        break;
    case AST_EXPR_NULL_LITERAL:
        if (out_is_null) *out_is_null = true;
        return TYP_OK;
    case AST_EXPR_IDENT: {
        const NameSymbol *sym = name_symbol_for_node(module, e);
        if (!sym) return unk(c);
        return type_of_symbol_type(c, sym, out);
    }
    case AST_EXPR_PAREN:
        return type_of_expr(c, module, e->u.paren.expr, out, out_is_null);
    case AST_EXPR_UNARY: {
        Type *ot = NULL;
        bool onull = false;
        TyperStatus ost = type_of_expr(c, module, e->u.unary.operand,
                                       &ot, &onull);
        if (ost != TYP_OK) return ost;
        switch (e->u.unary.op) {
        case AST_UN_NEG:
        case AST_UN_PLUS:
        case AST_UN_BNOT:
            /* operand integer (validity, incl. unsigned - , is 11d's) */
            *out = ot;
            return TYP_OK;
        case AST_UN_NOT:
            type_free(ot);
            ot = NULL;   /* avoid double-free on the OOM tail below */
            *out = type_prim_new(AST_PRIM_BOOL);
            break;
        case AST_UN_DEREF:
            if (ot && ot->kind == TYPE_PTR) {
                Type *elem = ot->u.ptr.elem;
                /* transfer the element; drop the pointer shell */
                ot->u.ptr.elem = NULL;
                type_free(ot);
                *out = elem;
                return TYP_OK;
            }
            type_free(ot);
            return unk(c);
        case AST_UN_ADDR: {
            Type *pt = type_ptr_new(ot);
            if (!pt) { type_free(ot); return oom(c); }
            *out = pt;
            return TYP_OK;
        }
        default:
            type_free(ot);
            return unk(c);
        }
        if (!*out) { type_free(ot); return oom(c); }
        return TYP_OK;
    }
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
        /* sizeof/alignof: the operand is not evaluated (sec. 10.4); no
         * inner conversion sites. Result usize (sec. 10.2). */
        *out = type_prim_new(AST_PRIM_USIZE);
        break;
    case AST_EXPR_LEN:
    case AST_EXPR_PTR: {
        Type *ot = NULL;
        bool onull = false;
        TyperStatus ost = type_of_expr(c, module, e->u.size_op.operand,
                                       &ot, &onull);
        Type *result = NULL;
        if (ost != TYP_OK) return ost;
        if (e->kind == AST_EXPR_LEN) {
            if (ot && (ot->kind == TYPE_ARRAY || ot->kind == TYPE_SLICE ||
                       (ot->kind == TYPE_PRIM &&
                        ot->u.prim == AST_PRIM_STR))) {
                result = type_prim_new(AST_PRIM_USIZE);
            }
        } else {
            if (ot && (ot->kind == TYPE_ARRAY || ot->kind == TYPE_SLICE)) {
                result = type_ptr_new(type_clone(ot->u.array.elem));
            } else if (ot && ot->kind == TYPE_PRIM &&
                       ot->u.prim == AST_PRIM_STR) {
                result = type_ptr_new(type_prim_new(AST_PRIM_U8));
            }
        }
        type_free(ot);
        if (!result) return unk(c);   /* operand not array/slice/str: 11d */
        *out = result;
        return TYP_OK;
    }
    case AST_EXPR_INDEX: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr(c, module, e->u.index_slice.base,
                                       &bt, &bnull);
        Type *et = NULL;
        if (bst != TYP_OK) return bst;
        /* the index is evaluated (inner sites checked); its conversion to
         * usize is a sec. 12 rule owned by later packages */
        check_expr(c, module, e->u.index_slice.index);
        if (bt && bt->kind == TYPE_ARRAY) {
            et = bt->u.array.elem;
            bt->u.array.elem = NULL;
        } else if (bt && bt->kind == TYPE_SLICE) {
            et = bt->u.slice.elem;
            bt->u.slice.elem = NULL;
        } else if (bt && bt->kind == TYPE_PRIM &&
                   bt->u.prim == AST_PRIM_STR) {
            et = type_prim_new(AST_PRIM_U8);
        }
        type_free(bt);
        if (!et) return unk(c);
        *out = et;
        return TYP_OK;
    }
    case AST_EXPR_SLICE: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr(c, module, e->u.index_slice.base,
                                       &bt, &bnull);
        Type *st = NULL;
        if (bst != TYP_OK) return bst;
        check_expr(c, module, e->u.index_slice.lo);
        check_expr(c, module, e->u.index_slice.hi);
        if (bt && bt->kind == TYPE_PRIM && bt->u.prim == AST_PRIM_STR) {
            /* str slicing yields str (sec. 12.4) */
            st = bt;
            bt = NULL;
        } else if (bt && (bt->kind == TYPE_ARRAY || bt->kind == TYPE_SLICE)) {
            st = type_slice_new(type_clone(bt->u.array.elem));
        }
        type_free(bt);
        if (!st) return unk(c);
        *out = st;
        return TYP_OK;
    }
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        /* the target type (validity of the pair is 11d's AIC-T0308); the
         * operand is evaluated, so its inner sites are checked */
        check_expr(c, module, e->u.cast_wrap.expr);
        return type_from_type_node(c, module, e->u.cast_wrap.type, out);
    case AST_EXPR_BINARY: {
        AstBinaryOp op = e->u.binary.op;
        Type *lt = NULL, *rt = NULL;
        bool lnull = false, rnull = false;
        TyperStatus lst = type_of_expr(c, module, e->u.binary.lhs,
                                       &lt, &lnull);
        TyperStatus rst = type_of_expr(c, module, e->u.binary.rhs,
                                       &rt, &rnull);
        const TypePrimInfo *pl, *pr;
        AstPrimKind ck;
        if (lst == TYP_OOM || rst == TYP_OOM) {
            c->oom = true;
            type_free(lt);
            type_free(rt);
            return TYP_OOM;
        }
        pl = lt ? prim_of(lt) : NULL;
        pr = rt ? prim_of(rt) : NULL;
        if (op == AST_BIN_LAND || op == AST_BIN_LOR) {
            /* bool/bool (validity is 11d's); result bool */
            type_free(lt);
            type_free(rt);
            *out = type_prim_new(AST_PRIM_BOOL);
            return *out ? TYP_OK : oom(c);
        }
        if (op == AST_BIN_SHL || op == AST_BIN_SHR) {
            /* result = left operand type (sec. 10.2); the right operand
             * is assignment-converted to it (whitelist), not common-type
             * promoted (documented 11c decision) */
            if (lt && rt && !lnull && !rnull &&
                lt->kind == TYPE_PRIM && rt->kind == TYPE_PRIM &&
                pl && pl->is_integer && pr && pr->is_integer) {
                check_conversion(c, module, e->u.binary.rhs, rt, false, lt,
                                 e->u.binary.rhs->span, false);
            } else {
                c->unknown = true;
            }
            type_free(rt);
            *out = lt;
            return lt ? TYP_OK : TYP_UNKNOWN;
        }
        if (op == AST_BIN_LT || op == AST_BIN_LE || op == AST_BIN_GT ||
            op == AST_BIN_GE || op == AST_BIN_EQ || op == AST_BIN_NE) {
            /* comparisons: integer operands use common-type promotion
             * (sec. 11.1 bullet 2); result bool. Non-integer pairs (bool/bool,
             * enum/enum, str/str, T* vs T*, slice/slice, mismatched)
             * involve no implicit conversion; their validity is 11d's. */
            if (lt && rt && !lnull && !rnull &&
                lt->kind == TYPE_PRIM && rt->kind == TYPE_PRIM &&
                pl && pl->is_integer && pr && pr->is_integer) {
                if (!common_type_kind(lt, rt, &ck)) {
                    emit_no_common(c, e, lt, rt);
                }
            } else if (lt == NULL || rt == NULL || lnull || rnull) {
                c->unknown = true;
            }
            type_free(lt);
            type_free(rt);
            *out = type_prim_new(AST_PRIM_BOOL);
            return *out ? TYP_OK : oom(c);
        }
        if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
            /* pointer arithmetic (sec. 12.5): p +/- i -> T*; p - q -> isize.
             * The index is "any integer type" (no conversion). */
            if (lt && rt && !lnull && !rnull) {
                if (lt->kind == TYPE_PTR && rt->kind == TYPE_PRIM &&
                    pr && pr->is_integer) {
                    type_free(rt);
                    *out = lt;
                    return TYP_OK;
                }
                if (lt->kind == TYPE_PRIM && pl && pl->is_integer &&
                    rt->kind == TYPE_PTR) {
                    type_free(lt);
                    *out = rt;
                    return TYP_OK;
                }
                if (op == AST_BIN_SUB && lt->kind == TYPE_PTR &&
                    rt->kind == TYPE_PTR) {
                    type_free(lt);
                    type_free(rt);
                    *out = type_prim_new(AST_PRIM_ISIZE);
                    return *out ? TYP_OK : oom(c);
                }
            } else {
                c->unknown = true;
            }
        }
        /* arithmetic + - * / % and bitwise & | ^: common type */
        if (lt && rt && !lnull && !rnull &&
            lt->kind == TYPE_PRIM && rt->kind == TYPE_PRIM &&
            pl && pl->is_integer && pr && pr->is_integer) {
            if (!common_type_kind(lt, rt, &ck)) {
                emit_no_common(c, e, lt, rt);
                type_free(lt);
                type_free(rt);
                return TYP_UNKNOWN;   /* record emitted; no site type */
            }
            type_free(lt);
            type_free(rt);
            *out = type_prim_new(ck);
            return *out ? TYP_OK : oom(c);
        }
        /* non-integer operand pairs: operator validity is 11d's */
        type_free(lt);
        type_free(rt);
        return unk(c);
    }
    case AST_EXPR_TERNARY: {
        /* then/else must have a common type (sec. 10.2 "same assignable
         * result type"); the ternary's type is that common type. */
        Type *ct = NULL, *tt = NULL, *et = NULL;
        bool cnull = false, tnull = false, enull = false;
        TyperStatus cst = type_of_expr(c, module, e->u.branch.cond,
                                       &ct, &cnull);
        TyperStatus tst = type_of_expr(c, module, e->u.branch.then,
                                       &tt, &tnull);
        TyperStatus est = type_of_expr(c, module, e->u.branch.els,
                                       &et, &enull);
        const TypePrimInfo *ptt, *pet;
        AstPrimKind ck;
        if (cst == TYP_OOM || tst == TYP_OOM || est == TYP_OOM) {
            c->oom = true;
            type_free(ct);
            type_free(tt);
            type_free(et);
            return TYP_OOM;
        }
        type_free(ct);   /* condition must be bool: 11d AIC-T0310 */
        ptt = tt ? prim_of(tt) : NULL;
        pet = et ? prim_of(et) : NULL;
        if (tt && et && !tnull && !enull) {
            if (tt->kind == TYPE_PRIM && et->kind == TYPE_PRIM &&
                ptt && ptt->is_integer && pet && pet->is_integer) {
                if (!common_type_kind(tt, et, &ck)) {
                    emit_no_common(c, e, tt, et);
                    type_free(tt);
                    type_free(et);
                    return TYP_UNKNOWN;
                }
                type_free(tt);
                type_free(et);
                *out = type_prim_new(ck);
                return *out ? TYP_OK : oom(c);
            }
            if (type_identical(tt, et)) {
                /* identical non-integer branches: same assignable type */
                *out = type_clone(tt);
                type_free(tt);
                type_free(et);
                return *out ? TYP_OK : oom(c);
            }
            c->unknown = true;   /* mismatched non-integer: 11d */
        } else {
            c->unknown = true;
        }
        type_free(tt);
        type_free(et);
        return TYP_UNKNOWN;
    }
    case AST_EXPR_CALL: {
        const AstNode *callee = e->u.call.callee;
        const NameSymbol *fsym = NULL;
        size_t i;
        if (callee && callee->kind == AST_EXPR_IDENT) {
            fsym = name_symbol_for_node(module, callee);
        }
        if (!fsym || fsym->kind != NAME_SYM_FN || !fsym->decl) {
            /* unresolvable callee (runtime built-in, non-function name):
             * arguments are still evaluated (inner sites checked); the
             * arg-to-parameter conversions cannot be checked */
            c->unknown = true;
            for (i = 0; i < e->u.call.nargs; i++) {
                Type *at = NULL;
                bool anull = false;
                TyperStatus ast = type_of_expr(c, module, e->u.call.args[i],
                                               &at, &anull);
                if (ast == TYP_OOM) { c->oom = true; type_free(at); return TYP_OOM; }
                type_free(at);
            }
            return TYP_UNKNOWN;
        }
        {
            const AstNode *fdecl = fsym->decl;
            size_t n = e->u.call.nargs;
            size_t np = fdecl->u.fn_decl.nparams;
            Type *rt = NULL;
            TyperStatus rst;
            if (n == np) {
                for (i = 0; i < n; i++) {
                    const AstNode *param = fdecl->u.fn_decl.params[i];
                    const AstNode *arg = e->u.call.args[i];
                    Type *pt = NULL, *at = NULL;
                    bool anull = false;
                    TyperStatus pst = type_from_type_node(c, fsym->module,
                                                          param->u.named.type,
                                                          &pt);
                    TyperStatus ast = type_of_expr(c, module, arg,
                                                   &at, &anull);
                    if (pst == TYP_OK && ast == TYP_OK) {
                        check_conversion(c, module, arg, at, anull, pt, arg->span,
                                         false);
                    } else if (pst == TYP_OOM || ast == TYP_OOM) {
                        c->oom = true;
                        type_free(pt);
                        type_free(at);
                        type_free(rt);
                        return TYP_OOM;
                    } else {
                        c->unknown = true;
                    }
                    type_free(pt);
                    type_free(at);
                }
            } else {
                /* argument count mismatch: AIC-T0312 is 11d's; the
                 * conversions of a prefix are not checked */
                for (i = 0; i < n; i++) {
                    Type *at = NULL;
                    bool anull = false;
                    TyperStatus ast = type_of_expr(c, module, e->u.call.args[i],
                                                   &at, &anull);
                    if (ast == TYP_OOM) { c->oom = true; type_free(at); return TYP_OOM; }
                    type_free(at);
                }
            }
            rst = type_from_type_node(c, fsym->module,
                                      fdecl->u.fn_decl.ret_type, &rt);
            if (rst != TYP_OK) {
                if (rst == TYP_OOM) return TYP_OOM;
                return unk(c);
            }
            *out = rt;
            return TYP_OK;
        }
    }
    case AST_EXPR_ASSIGN: {
        Type *tt = NULL, *vt = NULL;
        bool tnull = false, vnull = false;
        TyperStatus tst = type_of_expr(c, module, e->u.assign.target,
                                       &tt, &tnull);
        TyperStatus vst = type_of_expr(c, module, e->u.assign.value,
                                       &vt, &vnull);
        const TypePrimInfo *ptt, *pvt;
        AstPrimKind ck;
        if (tst == TYP_OOM || vst == TYP_OOM) {
            c->oom = true;
            type_free(tt);
            type_free(vt);
            return TYP_OOM;
        }
        ptt = tt ? prim_of(tt) : NULL;
        pvt = vt ? prim_of(vt) : NULL;
        if (e->u.assign.op == AST_ASGN_ASSIGN) {
            if (tst == TYP_OK && vst == TYP_OK) {
                check_conversion(c, module, e->u.assign.value, vt, vnull, tt,
                                 e->u.assign.value->span, false);
            } else {
                c->unknown = true;
            }
        } else if (tst == TYP_OK && vst == TYP_OK && !tnull && !vnull &&
                   tt && vt && tt->kind == TYPE_PRIM && vt->kind == TYPE_PRIM &&
                   ptt && ptt->is_integer && pvt && pvt->is_integer) {
            /* compound: a op= v == a = a op v (sec. 11.6). The integer op
             * needs a common type, and the result must be assignable back
             * to typeof(a). */
            if (common_type_kind(tt, vt, &ck)) {
                Type *res = type_prim_new(ck);
                if (!res) {
                    c->oom = true;
                    type_free(tt);
                    type_free(vt);
                    return TYP_OOM;
                }
                if (!convert_implicit_allowed(res, tt)) {
                    check_conversion(c, module, e->u.assign.value, res, false, tt,
                                     e->u.assign.value->span, false);
                }
                type_free(res);
            } else {
                emit_no_common_at(c, tt, vt,
                                  e->u.assign.value->span);
            }
        } else if (tst == TYP_OK && vst == TYP_OK && tt && vt &&
                   tt->kind == TYPE_PTR && vt->kind == TYPE_PRIM &&
                   pvt && pvt->is_integer) {
            /* pointer += / -= (sec. 12.5): index is any integer type */
        } else {
            c->unknown = true;
        }
        type_free(vt);
        /* the assignment expression's type is the target's type */
        if (!tt) return TYP_UNKNOWN;
        *out = type_clone(tt);
        type_free(tt);
        return *out ? TYP_OK : oom(c);
    }
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        /* E.Member: the member node resolves to the enum-member symbol;
         * the expression's type is the enum type. */
        const NameSymbol *msym = name_symbol_for_node(module, e);
        if (msym && msym->kind == NAME_SYM_ENUM_MEMBER) {
            *out = type_enum_new(msym->owner);
            return *out ? TYP_OK : oom(c);
        }
        {
            Type *bt = NULL;
            bool bnull = false;
            TyperStatus bst = type_of_expr(c, module, e->u.member.base,
                                           &bt, &bnull);
            const Type *struct_t = NULL;
            const NameModule *fmod = NULL;
            const AstNode *fdecl;
            Type *ft = NULL;
            TyperStatus fst;
            if (bst != TYP_OK) return bst;
            if (e->kind == AST_EXPR_ARROW) {
                if (bt && bt->kind == TYPE_PTR) struct_t = bt->u.ptr.elem;
            } else {
                struct_t = bt;
            }
            fdecl = struct_field_decl(struct_t, e->u.member.name);
            if (!fdecl) {
                /* unknown field / non-struct base: 11d AIC-T0306/T0313 */
                type_free(bt);
                return unk(c);
            }
            fmod = struct_t->u.sym->module;
            fst = type_from_type_node(c, fmod, fdecl->u.named.type, &ft);
            type_free(bt);
            if (fst != TYP_OK) return fst;
            *out = ft;
            return TYP_OK;
        }
    }
    case AST_EXPR_ARRAY_LITERAL:
        /* No type inference in the language: a standalone array literal
         * has no type; it is typed only at a conversion site through its
         * destination. Elements are evaluated (inner sites checked). */
        {
            size_t i;
            for (i = 0; i < e->u.array_literal.nelems; i++) {
                Type *et = NULL;
                bool enull = false;
                TyperStatus est = type_of_expr(c, module,
                                               e->u.array_literal.elems[i],
                                               &et, &enull);
                if (est == TYP_OOM) { c->oom = true; return TYP_OOM; }
                type_free(et);
            }
        }
        return unk(c);
    case AST_EXPR_STRUCT_INIT: {
        Type *st = NULL;
        TyperStatus sst = type_of_struct_init_base(c, module,
                                                   e->u.struct_init.base,
                                                   &st);
        size_t i;
        if (sst == TYP_OOM) return TYP_OOM;
        if (sst != TYP_OK || (st && st->kind != TYPE_STRUCT)) {
            /* unknown or non-struct literal type: 11d AIC-T0306; still
             * check field-value inner sites */
            for (i = 0; i < e->u.struct_init.nfields; i++) {
                const AstNode *fi = e->u.struct_init.fields[i];
                Type *vt = NULL;
                bool vnull = false;
                TyperStatus vst;
                if (!fi->u.named.value) continue;
                vst = type_of_expr(c, module, fi->u.named.value, &vt, &vnull);
                if (vst == TYP_OOM) { type_free(st); return TYP_OOM; }
                type_free(vt);
            }
            type_free(st);
            return unk(c);
        }
        for (i = 0; i < e->u.struct_init.nfields; i++) {
            const AstNode *fi = e->u.struct_init.fields[i];
            const AstNode *fd;
            Type *ft = NULL, *vt = NULL;
            bool vnull = false;
            TyperStatus fst, vst;
            if (!fi->u.named.value) continue;
            fd = struct_field_decl(st, fi->u.named.name);
            if (!fd) continue;   /* unknown field: 11d AIC-T0313 */
            fst = type_from_type_node(c, st->u.sym->module,
                                      fd->u.named.type, &ft);
            vst = type_of_expr(c, module, fi->u.named.value, &vt, &vnull);
            if (fst == TYP_OK && vst == TYP_OK) {
                check_conversion(c, module, fi->u.named.value, vt, vnull, ft,
                                 fi->u.named.value->span, false);
            } else if (fst == TYP_OOM || vst == TYP_OOM) {
                c->oom = true;
                type_free(ft);
                type_free(vt);
                type_free(st);
                return TYP_OOM;
            } else {
                c->unknown = true;
            }
            type_free(ft);
            type_free(vt);
        }
        *out = st;
        return TYP_OK;
    }
    default:
        return unk(c);
    }
    if (!*out) return oom(c);
    return TYP_OK;
}

/* ---------------------------------------------------------------------------
 * Site walkers: declarations, statements, array literals
 * ------------------------------------------------------------------------- */

/* Check every element of an array literal (in a typed array context)
 * against the element type; records carry the whole literal's span with
 * the " in array literal" message suffix (corpus-pinned). */
static void check_array_literal_elements(ConvertCtx *c,
                                         const NameModule *module,
                                         const AstNode *lit,
                                         const Type *elem_type)
{
    size_t i;
    for (i = 0; i < lit->u.array_literal.nelems; i++) {
        const AstNode *el = lit->u.array_literal.elems[i];
        Type *et = NULL;
        bool enull = false;
        TyperStatus est;
        if (el && el->kind == AST_EXPR_ARRAY_LITERAL &&
            elem_type->kind == TYPE_ARRAY) {
            check_array_literal_elements(c, module, el,
                                         elem_type->u.array.elem);
            est = type_of_expr(c, module, el, &et, &enull);
            if (est == TYP_OOM) { c->oom = true; return; }
            type_free(et);
            continue;
        }
        est = type_of_expr(c, module, el, &et, &enull);
        if (est == TYP_OOM) { c->oom = true; type_free(et); return; }
        if (est == TYP_OK) {
            check_conversion(c, module, el, et, enull, elem_type, lit->span, true);
        } else {
            c->unknown = true;
        }
        type_free(et);
    }
}

/* Check one declaration initializer against the declared type. Array
 * literals are typed through their destination: elements are checked
 * against the array's element type. */
static void check_decl_init(ConvertCtx *c, const NameModule *module,
                            const AstNode *type_node, const AstNode *init)
{
    Type *tt = NULL;
    TyperStatus tst;
    Type *it = NULL;
    bool inull = false;
    TyperStatus ist;
    if (!init) return;
    tst = type_from_type_node(c, module, type_node, &tt);
    if (tst == TYP_OOM) { c->oom = true; return; }
    if (tst != TYP_OK) {
        /* declared type unresolvable: still check inner sites of init */
        ist = type_of_expr(c, module, init, &it, &inull);
        if (ist == TYP_OOM) { c->oom = true; type_free(it); return; }
        type_free(it);
        c->unknown = true;
        return;
    }
    if (init->kind == AST_EXPR_ARRAY_LITERAL && tt->kind == TYPE_ARRAY) {
        check_array_literal_elements(c, module, init, tt->u.array.elem);
        type_free(tt);
        return;
    }
    ist = type_of_expr(c, module, init, &it, &inull);
    if (ist == TYP_OOM) { c->oom = true; type_free(tt); type_free(it); return; }
    if (ist == TYP_OK) {
        check_conversion(c, module, init, it, inull, tt, init->span, false);
    } else {
        c->unknown = true;
    }
    type_free(tt);
    type_free(it);
}

static void check_block(ConvertCtx *c, const NameModule *module,
                        const AstNode *block, const Type *ret_type);

static void check_stmt(ConvertCtx *c, const NameModule *module,
                       const AstNode *s, const Type *ret_type)
{
    if (!s) return;
    switch (s->kind) {
    case AST_BLOCK:
        check_block(c, module, s, ret_type);
        break;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        check_decl_init(c, module, s->u.local_decl.type,
                        s->u.local_decl.init);
        break;
    case AST_IF:
        check_expr(c, module, s->u.branch.cond);
        check_block(c, module, s->u.branch.then, ret_type);
        check_block(c, module, s->u.branch.els, ret_type);
        break;
    case AST_WHILE:
        check_expr(c, module, s->u.while_loop.cond);
        check_block(c, module, s->u.while_loop.body, ret_type);
        break;
    case AST_FOR: {
        const AstNode *init = s->u.for_loop.init;
        if (init && (init->kind == AST_VAR_DECL ||
                     init->kind == AST_CONST_DECL)) {
            check_decl_init(c, module, init->u.local_decl.type,
                            init->u.local_decl.init);
        } else if (init) {
            check_expr(c, module, init);
        }
        check_expr(c, module, s->u.for_loop.cond);
        check_expr(c, module, s->u.for_loop.step);
        check_block(c, module, s->u.for_loop.body, ret_type);
        break;
    }
    case AST_SWITCH: {
        size_t i;
        check_expr(c, module, s->u.switch_stmt.selector);
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (cl->u.clause.value) check_expr(c, module, cl->u.clause.value);
            check_block(c, module, cl->u.clause.body, ret_type);
        }
        break;
    }
    case AST_RETURN: {
        const AstNode *v = s->u.ret.value;
        Type *vt = NULL;
        bool vnull = false;
        TyperStatus vst;
        if (!v) break;
        if (!ret_type || (ret_type->kind == TYPE_PRIM &&
                          ret_type->u.prim == AST_PRIM_VOID)) {
            /* value-in-void return: later package (WP-M0-13d) */
            check_expr(c, module, v);
            break;
        }
        vst = type_of_expr(c, module, v, &vt, &vnull);
        if (vst == TYP_OOM) { c->oom = true; type_free(vt); return; }
        if (vst == TYP_OK) {
            check_conversion(c, module, v, vt, vnull, ret_type, v->span, false);
        } else {
            c->unknown = true;
        }
        type_free(vt);
        break;
    }
    case AST_EXPR_STMT:
        check_expr(c, module, s->u.expr_stmt.expr);
        break;
    case AST_EMPTY_STMT:
    default:
        break;
    }
}

static void check_block(ConvertCtx *c, const NameModule *module,
                        const AstNode *block, const Type *ret_type)
{
    size_t i;
    if (!block) return;
    for (i = 0; i < block->u.list.count; i++) {
        check_stmt(c, module, block->u.list.items[i], ret_type);
        if (c->oom) return;
    }
}

static void check_fn(ConvertCtx *c, const NameModule *module,
                     const AstNode *fn)
{
    Type *rt = NULL;
    TyperStatus rst;
    if (fn->u.fn_decl.ret_type) {
        rst = type_from_type_node(c, module, fn->u.fn_decl.ret_type, &rt);
        if (rst == TYP_OOM) { c->oom = true; return; }
        if (rst != TYP_OK) {
            c->unknown = true;
            rt = NULL;
        }
    }
    if (fn->u.fn_decl.body) check_block(c, module, fn->u.fn_decl.body, rt);
    type_free(rt);
}

ConvertStatus types_convert_check(const NameResult *result,
                                  DiagRecord ***out_records,
                                  size_t *out_record_count)
{
    ConvertCtx c;
    size_t m;
    memset(&c, 0, sizeof(c));
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    if (!result) return CONVERT_OK;

    for (m = 0; m < result->nmodules; m++) {
        const NameModule *module = result->modules[m];
        const AstNode *program = module->program;
        size_t i;
        if (!program) continue;
        for (i = 0; i < program->u.program.ndecls; i++) {
            const AstNode *decl = program->u.program.decls[i];
            switch (decl->kind) {
            case AST_GLOBAL_VAR_DECL:
            case AST_GLOBAL_CONST_DECL:
                check_decl_init(&c, module, decl->u.global_decl.type,
                                decl->u.global_decl.init);
                break;
            case AST_FN_DECL:
                check_fn(&c, module, decl);
                break;
            default:
                break;
            }
            if (c.oom) goto oom;
        }
        if (c.oom) goto oom;
    }

    if (c.nrecords > 0) diag_sort_records(c.records, c.nrecords);
    if (out_records && c.nrecords > 0) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (c.nrecords > 0) return CONVERT_DIAG_ERROR;
    return c.unknown ? CONVERT_UNKNOWN : CONVERT_OK;

oom:
    for (m = 0; m < c.nrecords; m++) diag_record_free(c.records[m]);
    free(c.records);
    return CONVERT_OOM;
}
