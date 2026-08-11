/* bootstrap/src/types/optype.c
 *
 * WP-M0-11d: explicit cast/wrap pair matrix (spec sec. 11.2 / sec. 11.5) and
 * build-level operator/typing checks (spec sec. 10.2, sec. 11.2, sec. 11.4,
 * sec. 12.7, sec. 13.1, sec. 13.2, sec. 7.3). See optype.h for ownership and
 * the exact message/span contract; see convert.c for the sibling package's
 * convention this file mirrors.
 *
 * Boundary with WP-M0-11c: 11c checks implicit conversions (AIC-T0307) and
 * computes common types; 11d never checks conversions and never re-derives
 * them. This walker decides operator applicability and site validity that 11c
 * leaves UNKNOWN by design (non-integer operand pairs, unknown/mismatched
 * non-integer branches, arg-count mismatch, unknown fields, standalone array
 * literals, cast/wrap/void misuse, condition/switch-selector types). Integer
 * operand pairs with a missing common type are 11c's AIC-T0307; 11d treats
 * every integer/integer pair as applicable.
 *
 * The expression type-derivation subset below (type_of_expr_od) exists
 * solely to feed the operator-applicability checks. It re-implements the
 * bare type derivation (no conversion records, no common-type records) that
 * convert.c computes internally, so that pairs 11c declines to type
 * (returning UNKNOWN) can still be inspected here. It is deliberately a
 * target of the same corpus and unit tests.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "optype.h"

#include "type_identity.h"
#include "type_tables.h"
#include "convert.h"

#include "../name/name.h"
#include "../ast/ast.h"
#include "../lex/lex.h"
#include "../parse/parse.h"
#include "../load/load.h"
#include "../diag/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context
 * ------------------------------------------------------------------------- */

typedef struct OptypeCtx {
    const NameResult *result;
    DiagRecord **records;
    size_t nrecords;
    size_t records_cap;
    bool oom;
    bool unknown;
} OptypeCtx;

static bool ctx_rec_push(OptypeCtx *c, DiagRecord *r)
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

static DiagRecord *new_type_record(OptypeCtx *c, const char *code,
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

static void emit_record(OptypeCtx *c, const char *code,
                        const char *message, const DiagSpan *span)
{
    DiagRecord *r = new_type_record(c, code, message, span);
    if (r && !ctx_rec_push(c, r)) diag_record_free(r);
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

/* Clone a type graph (fresh ownership). NULL on OOM (nothing owned). */
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
    return AST_PRIM_I32;   /* defensive */
}

static const TypePrimInfo *prim_of(const Type *t)
{
    if (!t || t->kind != TYPE_PRIM) return NULL;
    return types_prim_info(t->u.prim);
}

/* The underlying integer primitive of an enum type, per its declaration
 * (spec sec. 7.5; the AST enum decl records the underlying AST_TYPE_PRIM).
 * Returns false when `t` is not an enum with a resolved underlying int. */
static bool enum_underlying_prim(const Type *t, AstPrimKind *out)
{
    const AstNode *d;
    if (!t || t->kind != TYPE_ENUM || !t->u.sym || !t->u.sym->decl) {
        return false;
    }
    d = t->u.sym->decl;
    if (d->kind != AST_ENUM_DECL || !d->u.enum_decl.underlying) {
        return false;
    }
    if (d->u.enum_decl.underlying->kind != AST_TYPE_PRIM) return false;
    *out = d->u.enum_decl.underlying->u.type_prim.prim;
    return types_prim_info(*out) != NULL && types_prim_info(*out)->is_integer;
}

/* u8[] in the cast matrix is the slice u8[] (spec sec. 12.3: the (data,
 * length) view), not a fixed-length array. */
static bool type_is_u8_slice(const Type *t)
{
    return t && t->kind == TYPE_SLICE && t->u.slice.elem &&
           t->u.slice.elem->kind == TYPE_PRIM &&
           t->u.slice.elem->u.prim == AST_PRIM_U8;
}

/* ---------------------------------------------------------------------------
 * Explicit cast/wrap pair predicates (spec sec. 11.2 / sec. 11.5)
 * ------------------------------------------------------------------------- */

bool optype_cast_pair_valid(const Type *from, bool from_is_null,
                            const Type *to)
{
    AstPrimKind ek;
    const TypePrimInfo *pf, *pt;
    if (!to) return false;
    if (!from) return from_is_null && to->kind == TYPE_PTR;
    if (type_identical(from, to)) return true;

    pf = prim_of(from);
    pt = prim_of(to);

    /* integer -> integer (cast and wrap both per sec. 11.5) */
    if (pf && pf->is_integer && pt && pt->is_integer) return true;

    /* bool <-> integer (cast only) */
    if (from->kind == TYPE_PRIM && from->u.prim == AST_PRIM_BOOL &&
        pt && pt->is_integer) return true;
    if (to->kind == TYPE_PRIM && to->u.prim == AST_PRIM_BOOL &&
        pf && pf->is_integer) return true;

    /* enum -> underlying integer (cast only; checked identity) */
    if (from->kind == TYPE_ENUM && pt && pt->is_integer &&
        enum_underlying_prim(from, &ek) && to->u.prim == ek) return true;

    /* integer -> enum (cast only; value checked at const/runtime) */
    if (to->kind == TYPE_ENUM && pf && pf->is_integer) return true;

    /* enum -> enum (different enum type; cast only) */
    if (from->kind == TYPE_ENUM && to->kind == TYPE_ENUM) return true;

    /* pointer <-> usize/u64/isize/i64 (cast only) */
    if (from->kind == TYPE_PTR && pt && pt->is_integer &&
        (to->u.prim == AST_PRIM_USIZE || to->u.prim == AST_PRIM_U64 ||
         to->u.prim == AST_PRIM_ISIZE || to->u.prim == AST_PRIM_I64)) {
        return true;
    }

    /* integer -> pointer (cast only; any integer, ADR-004 raw-pointer
     * re-interpretation is explicit-cast only) */
    if (to->kind == TYPE_PTR && pf && pf->is_integer) return true;

    /* T* -> U* (different pointee; cast only; bit-preserving) */
    if (from->kind == TYPE_PTR && to->kind == TYPE_PTR) return true;

    /* str <-> u8[] (cast only) */
    if (from->kind == TYPE_PRIM && from->u.prim == AST_PRIM_STR &&
        type_is_u8_slice(to)) return true;
    if (to->kind == TYPE_PRIM && to->u.prim == AST_PRIM_STR &&
        type_is_u8_slice(from)) return true;

    return false;
}

bool optype_wrap_pair_valid(const Type *from, bool from_is_null,
                            const Type *to)
{
    const TypePrimInfo *pt;
    if (from_is_null || !to) return false;
    pt = prim_of(to);
    if (!pt || !pt->is_integer) return false;
    if (!from) return false;
    if (from->kind == TYPE_PRIM) {
        const TypePrimInfo *pf = prim_of(from);
        return pf != NULL && pf->is_integer;
    }
    if (from->kind == TYPE_ENUM) return true;
    return false;
}

/* ---------------------------------------------------------------------------
 * Type derivation from AST type nodes / symbols
 * ------------------------------------------------------------------------- */

typedef enum TyperStatus {
    TYP_OK = 0,       /* *out set (or *out_is_null set for null literals) */
    TYP_UNKNOWN,      /* outside the 11d subset; *out untouched */
    TYP_OOM           /* allocation failure; nothing owned */
} TyperStatus;

static TyperStatus unk(OptypeCtx *c)
{
    c->unknown = true;
    return TYP_UNKNOWN;
}

static TyperStatus oom(OptypeCtx *c)
{
    c->oom = true;
    return TYP_OOM;
}

/* Build a Type descriptor from an AST type node in `module` (spec
 * sec. 7.1-7.3). The array extent uses the 11b bounded constant-integer
 * subset; an extent outside it yields TYP_UNKNOWN (WP-M0-12 owns full
 * const evaluation). Mirrors convert.c's type_from_type_node. */
static TyperStatus type_from_type_node(OptypeCtx *c, const NameModule *module,
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
                LAYOUT_EVAL_OK || ev.big || ev.v < 0) {
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

static TyperStatus type_of_symbol_type(OptypeCtx *c, const NameSymbol *sym,
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
 * Operator token span recovery
 * ------------------------------------------------------------------------- */

static bool src_is_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* The operator token between two byte offsets, recovered from the module
 * source text; the non-whitespace run in [start_off, end_off). */
static DiagSpan *op_token_span_offsets(const NameModule *module,
                                       int64_t start_off, int64_t end_off)
{
    const LoadSource *src = module ? module->src : NULL;
    int64_t s, e;
    if (!src) return NULL;
    s = start_off;
    e = end_off;
    if (s < 0) s = 0;
    if (e > (int64_t)src->len) e = (int64_t)src->len;
    while (s < e && s < (int64_t)src->len && src_is_ws(src->text[s])) s++;
    while (e > s && src_is_ws(src->text[e - 1])) e--;
    if (s >= e) return load_span_point(src, s < (int64_t)src->len ? s : 0);
    return load_span_range(src, s, e);
}

/* The operator token between two child spans, recovered from the module
 * source text (the AST does not store operator token spans). Returns the
 * non-whitespace run strictly between `left->end.offset` and
 * `right->start.offset`; a point span at the left end when the region is
 * empty (defensive; a valid parse always has the operator token there). */
static DiagSpan *op_token_span(const NameModule *module,
                               const DiagSpan *left, const DiagSpan *right)
{
    if (!left || !right) return NULL;
    return op_token_span_offsets(module, left->end.offset,
                                 right->start.offset);
}

/* The unary operator token: the non-whitespace run between the unary
 * node's start and its operand's start (`-x`, `! b`, ...). */
static DiagSpan *unary_op_span(const NameModule *module, const AstNode *un,
                               const AstNode *operand)
{
    if (!un->span || !operand || !operand->span) return NULL;
    return op_token_span_offsets(module, un->span->start.offset,
                                 operand->span->start.offset);
}

/* The `.`/`->` token of a member/arrow node: between the base end and the
 * member name (the name is the trailing token of the node span). */
static DiagSpan *member_op_span(const NameModule *module, const AstNode *m,
                                const AstNode *base)
{
    DiagSpan *right;
    int64_t name_len;
    if (!m->span || !m->u.member.name || !base || !base->span) return NULL;
    name_len = (int64_t)strlen(m->u.member.name);
    right = load_span_point(module && module->src ? module->src : NULL,
                            m->span->end.offset - name_len);
    {
        DiagSpan *s = op_token_span(module, base->span, right);
        diag_span_free(right);
        return s;
    }
}

/* ---------------------------------------------------------------------------
 * Operator spelling
 * ------------------------------------------------------------------------- */

static const char *bin_op_text(AstBinaryOp op)
{
    switch (op) {
    case AST_BIN_MUL:  return "*";
    case AST_BIN_DIV:  return "/";
    case AST_BIN_MOD:  return "%";
    case AST_BIN_ADD:  return "+";
    case AST_BIN_SUB:  return "-";
    case AST_BIN_SHL:  return "<<";
    case AST_BIN_SHR:  return ">>";
    case AST_BIN_LT:   return "<";
    case AST_BIN_LE:   return "<=";
    case AST_BIN_GT:   return ">";
    case AST_BIN_GE:   return ">=";
    case AST_BIN_EQ:   return "==";
    case AST_BIN_NE:   return "!=";
    case AST_BIN_BAND: return "&";
    case AST_BIN_BXOR: return "^";
    case AST_BIN_BOR:  return "|";
    case AST_BIN_LAND: return "&&";
    case AST_BIN_LOR:  return "||";
    }
    return "?";
}

static const char *asgn_op_text(AstAssignOp op)
{
    switch (op) {
    case AST_ASGN_ASSIGN: return "=";
    case AST_ASGN_ADD:    return "+=";
    case AST_ASGN_SUB:    return "-=";
    case AST_ASGN_MUL:    return "*=";
    case AST_ASGN_DIV:    return "/=";
    case AST_ASGN_MOD:    return "%=";
    case AST_ASGN_SHL:    return "<<=";
    case AST_ASGN_SHR:    return ">>=";
    case AST_ASGN_BAND:   return "&=";
    case AST_ASGN_BOR:    return "|=";
    case AST_ASGN_BXOR:   return "^=";
    }
    return "?";
}

static const char *un_op_text(AstUnaryOp op)
{
    switch (op) {
    case AST_UN_NEG:   return "-";
    case AST_UN_PLUS:  return "+";
    case AST_UN_NOT:   return "!";
    case AST_UN_BNOT:  return "~";
    case AST_UN_DEREF: return "*";
    case AST_UN_ADDR:  return "&";
    }
    return "?";
}

/* ---------------------------------------------------------------------------
 * Record emission (AIC-T0304/05/06/08/09/10/11/12/13)
 * ------------------------------------------------------------------------- */

static void emit_t0306_op(OptypeCtx *c, const char *optext, const Type *t,
                          bool isnull, const DiagSpan *span)
{
    char msg[512];
    char *td = t ? type_describe(t) : NULL;
    snprintf(msg, sizeof(msg), "'%s' operator not applicable to operand "
             "type '%s'", optext, isnull ? "null" : (td ? td : "?"));
    free(td);
    emit_record(c, "AIC-T0306", msg, span);
}

/* "'<op>' operator not applicable to <kind> type '<desc>'" where kind is
 * "struct" or "array" - the AIC-T0304 equality shapes (spec sec. 10.2
 * last bullet: ==/!= on array or struct type is T0304, not T0306). */
static void emit_t0304_kind(OptypeCtx *c, const char *optext,
                            const char *kind, const Type *t,
                            const DiagSpan *span)
{
    char msg[512];
    char *td = t ? type_describe(t) : NULL;
    snprintf(msg, sizeof(msg), "'%s' operator not applicable to %s type "
             "'%s'", optext, kind, td ? td : "?");
    free(td);
    emit_record(c, "AIC-T0304", msg, span);
}

static void emit_t0306_no_field(OptypeCtx *c, const char *optext,
                                const Type *t, const char *field,
                                const DiagSpan *span)
{
    char msg[512];
    char *td = t ? type_describe(t) : NULL;
    snprintf(msg, sizeof(msg), "'%s' operator not applicable to struct type "
             "'%s' (no field '%s')", optext, td ? td : "?", field ? field : "?");
    free(td);
    emit_record(c, "AIC-T0306", msg, span);
}

static void emit_t0308(OptypeCtx *c, bool is_cast, const Type *from,
                       bool from_is_null, const Type *to, const DiagSpan *span)
{
    char msg[512];
    char *fd = from ? type_describe(from) : NULL;
    char *td = to ? type_describe(to) : NULL;
    snprintf(msg, sizeof(msg), "invalid explicit %s pair: %s to %s",
             is_cast ? "cast" : "wrap",
             from_is_null ? "null" : (fd ? fd : "?"),
             td ? td : "?");
    free(fd);
    free(td);
    emit_record(c, "AIC-T0308", msg, span);
}

/* ---------------------------------------------------------------------------
 * Shared expression typer (the 11d subset)
 * ------------------------------------------------------------------------- */

static TyperStatus type_of_expr_od(OptypeCtx *c, const NameModule *module,
                                   const AstNode *e, Type **out,
                                   bool *out_is_null);

/* Forward declaration: the primary span for condition/selector sites
 * (defined with the site walkers below; the expression typer uses it for
 * ternary conditions). */
static const DiagSpan *od_site_span(const NameModule *module,
                                    const AstNode *expr);

static void check_expr_od(OptypeCtx *c, const NameModule *module,
                          const AstNode *e)
{
    Type *t = NULL;
    bool isnull = false;
    TyperStatus st;
    if (!e) return;
    st = type_of_expr_od(c, module, e, &t, &isnull);
    if (st == TYP_OOM) c->oom = true;
    type_free(t);
}

/* The struct type named by a struct-literal base (mirrors convert.c). */
static TyperStatus type_of_struct_init_base(OptypeCtx *c,
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
        return unk(c);   /* non-struct literal: 11d T0306 */
    }
    return unk(c);
}

/* ---------------------------------------------------------------------------
 * Array literal element counts (spec sec. 12.1, AIC-T0309)
 * ------------------------------------------------------------------------- */

/* The count a literal claims: the repeat form's count value, or the list
 * form's element count. Returns false when the count cannot be determined
 * (repeat form outside the 11b const subset; the site is UNKNOWN). */
static bool array_literal_found_count(OptypeCtx *c, const AstNode *lit,
                                      int64_t *out)
{
    if (lit->u.array_literal.count) {
        LayoutEvalValue ev;
        if (layout_eval_int_expr(lit->u.array_literal.count, &ev) !=
                LAYOUT_EVAL_OK || ev.big || ev.v < 0) {
            c->unknown = true;
            return false;
        }
        *out = ev.v;
        return true;
    }
    *out = (int64_t)lit->u.array_literal.nelems;
    return true;
}

/* Check the element count of an array literal bound to a known array
 * destination type, and recurse into nested array literals. Elements are
 * also walked for inner operator sites. */
static void check_array_literal_typed(OptypeCtx *c, const NameModule *module,
                                      const AstNode *lit, const Type *dest)
{
    const Type *elem;
    int64_t found, expected;
    size_t i;
    if (!lit || !dest || dest->kind != TYPE_ARRAY) return;
    if (lit->kind != AST_EXPR_ARRAY_LITERAL) {
        check_expr_od(c, module, lit);
        return;
    }
    expected = dest->u.array.extent;
    if (array_literal_found_count(c, lit, &found)) {
        if (found != expected) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "array literal element count mismatch: expected %lld, "
                     "found %lld",
                     (long long)expected, (long long)found);
            emit_record(c, "AIC-T0309", msg, lit->span);
        }
    }
    elem = dest->u.array.elem;
    for (i = 0; i < lit->u.array_literal.nelems; i++) {
        const AstNode *el = lit->u.array_literal.elems[i];
        if (el && el->kind == AST_EXPR_ARRAY_LITERAL &&
            elem && elem->kind == TYPE_ARRAY) {
            check_array_literal_typed(c, module, el, elem);
        } else {
            check_expr_od(c, module, el);
        }
        if (c->oom) return;
    }
}

/* ---------------------------------------------------------------------------
 * Binary operator typing (spec sec. 10.2 / 11.4)
 * ------------------------------------------------------------------------- */

static bool type_is_prim_int(const Type *t)
{
    const TypePrimInfo *p = prim_of(t);
    return p != NULL && p->is_integer;
}

static bool type_is_bool(const Type *t)
{
    return t && t->kind == TYPE_PRIM && t->u.prim == AST_PRIM_BOOL;
}

/* A comparison operator (relational or equality). */
static bool is_comp_op(AstBinaryOp op)
{
    switch (op) {
    case AST_BIN_LT: case AST_BIN_LE: case AST_BIN_GT: case AST_BIN_GE:
    case AST_BIN_EQ: case AST_BIN_NE:
        return true;
    default:
        return false;
    }
}

/* The leftmost comparison operator of a chain: descend the lhs spine while
 * the lhs (parens unwrapped) is itself a comparison. */
static DiagSpan *chain_op_span(const NameModule *module, const AstNode *node)
{
    const AstNode *lhs = node->u.binary.lhs;
    while (lhs && lhs->kind == AST_EXPR_PAREN) lhs = lhs->u.paren.expr;
    if (lhs && lhs->kind == AST_EXPR_BINARY &&
        is_comp_op(lhs->u.binary.op)) {
        return chain_op_span(module, lhs);
    }
    return op_token_span(module, node->u.binary.lhs->span,
                         node->u.binary.rhs->span);
}

/* Emit AIC-T0305 for a chained comparison and return the bool result type
 * (one record per chain; the outer operand pair is not also reported). */
static TyperStatus emit_chain(OptypeCtx *c, const NameModule *module,
                              const AstNode *e, Type **out)
{
    DiagSpan *sp = chain_op_span(module, e);
    emit_record(c, "AIC-T0305", "chained comparison is not allowed",
                sp ? sp : e->span);
    diag_span_free(sp);
    *out = type_prim_new(AST_PRIM_BOOL);
    return *out ? TYP_OK : oom(c);
}

/* Equality/relational validity (spec sec. 10.2/11.4). Returns true when
 * the pair is applicable; on false the caller emits the record. */
static bool comp_pair_applicable(const Type *lt, bool lnull,
                                 const Type *rt, bool rnull,
                                 bool equality)
{
    if (lnull || rnull) {
        /* null vs T*: the null literal converts to the pointer type
         * (Table 11.1); the comparison is over the pointer value.
         * null vs non-pointer (or null vs null) is not applicable. */
        if (lnull && rnull) return false;
        {
            const Type *other = lnull ? rt : lt;
            return other != NULL && other->kind == TYPE_PTR;
        }
    }
    if (type_is_prim_int(lt) && type_is_prim_int(rt)) return true;
    if (type_is_bool(lt) && type_is_bool(rt)) return equality;
    if (lt->kind == TYPE_ENUM && rt->kind == TYPE_ENUM) {
        return type_identical(lt, rt);   /* mixed enums need a cast */
    }
    if (lt->kind == TYPE_PTR && rt->kind == TYPE_PTR) {
        return type_identical(lt, rt);   /* same pointee (T-star / T-star) */
    }
    if (lt->kind == TYPE_PRIM && lt->u.prim == AST_PRIM_STR &&
        rt->kind == TYPE_PRIM && rt->u.prim == AST_PRIM_STR) {
        return true;
    }
    if (equality && lt->kind == TYPE_SLICE && rt->kind == TYPE_SLICE) {
        return type_identical(lt, rt);   /* same element type */
    }
    return false;
}

/* The result type of a binary operator over applicable operands:
 *   - integer arithmetic/bitwise: common type (11c's rule; NULL when no
 *     common type exists - 11c owns that record);
 *   - shifts: the left operand type;
 *   - comparisons: bool;
 *   - pointer arithmetic (+/-): the pointer type / isize, per the closed
 *     sec. 12.5 enumeration (p + i, p - i, p - q with identical pointee;
 *     i + p and mixed-pointee p - q are not enumerated - their rejection
 *     is AIC-T0306 in check_binary_od; Planner ruling R-1);
 *   - logical: bool.
 * Returns TYP_OK with *out set, or TYP_UNKNOWN (no record; either the
 * operands were not typable or the integer pair lacks a common type). */
static TyperStatus binary_result_od(OptypeCtx *c, AstBinaryOp op,
                                    const Type *lt, const Type *rt,
                                    Type **out)
{
    *out = NULL;
    if (!lt || !rt) return unk(c);   /* null literal / untyped operand */
    if (op == AST_BIN_LAND || op == AST_BIN_LOR ||
        is_comp_op(op)) {
        *out = type_prim_new(AST_PRIM_BOOL);
        return *out ? TYP_OK : oom(c);
    }
    if (op == AST_BIN_SHL || op == AST_BIN_SHR) {
        *out = type_clone(lt);
        return *out ? TYP_OK : oom(c);
    }
    if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
        if (lt && lt->kind == TYPE_PTR && rt &&
            type_is_prim_int(rt)) {
            *out = type_clone(lt);
            return *out ? TYP_OK : oom(c);
        }
        if (op == AST_BIN_SUB && lt && lt->kind == TYPE_PTR &&
            rt && rt->kind == TYPE_PTR && type_identical(lt, rt)) {
            *out = type_prim_new(AST_PRIM_ISIZE);
            return *out ? TYP_OK : oom(c);
        }
    }
    if (type_is_prim_int(lt) && type_is_prim_int(rt)) {
        Type *ct = convert_common_type(lt, rt);
        if (!ct) {
            c->unknown = true;   /* 11c owns the T0307 record */
            return TYP_UNKNOWN;
        }
        *out = ct;
        return TYP_OK;
    }
    return unk(c);
}

/* Check one binary node: emit T0304/05/06 as needed and derive the result
 * type. `lt`/`rt` are the already-derived operand types. */
static TyperStatus check_binary_od(OptypeCtx *c, const NameModule *module,
                                   const AstNode *e, const Type *lt,
                                   bool lnull, const Type *rt, bool rnull,
                                   Type **out)
{
    AstBinaryOp op = e->u.binary.op;
    DiagSpan *osp = op_token_span(module, e->u.binary.lhs->span,
                                  e->u.binary.rhs->span);
    const DiagSpan *opsp = osp ? osp : e->span;
    *out = NULL;

    if (is_comp_op(op)) {
        /* chained comparisons are rejected before pair typing */
        {
            const AstNode *lhs = e->u.binary.lhs;
            while (lhs && lhs->kind == AST_EXPR_PAREN) {
                lhs = lhs->u.paren.expr;
            }
            if (lhs && lhs->kind == AST_EXPR_BINARY &&
                is_comp_op(lhs->u.binary.op)) {
                diag_span_free(osp);
                return emit_chain(c, module, e, out);
            }
        }
        if ((op == AST_BIN_EQ || op == AST_BIN_NE) &&
            ((lt && (lt->kind == TYPE_ARRAY || lt->kind == TYPE_STRUCT)) ||
             (rt && (rt->kind == TYPE_ARRAY || rt->kind == TYPE_STRUCT)))) {
            const Type *bad = (lt && (lt->kind == TYPE_ARRAY ||
                                      lt->kind == TYPE_STRUCT)) ? lt : rt;
            const char *kind = bad->kind == TYPE_ARRAY ? "array" : "struct";
            emit_t0304_kind(c, bin_op_text(op), kind, bad, opsp);
            diag_span_free(osp);
            goto result_bool;
        }
        if (!comp_pair_applicable(lt, lnull, rt, rnull,
                                  op == AST_BIN_EQ || op == AST_BIN_NE)) {
            emit_t0306_op(c, bin_op_text(op), lt, lnull, opsp);
            diag_span_free(osp);
            goto result_bool;
        }
        diag_span_free(osp);
        goto result_bool;
    }

    if (op == AST_BIN_LAND || op == AST_BIN_LOR) {
        if (!(type_is_bool(lt) && type_is_bool(rt))) {
            emit_t0306_op(c, bin_op_text(op), lt, lnull, opsp);
        }
        diag_span_free(osp);
        goto result_bool;
    }

    if (op == AST_BIN_SHL || op == AST_BIN_SHR) {
        if (!(type_is_prim_int(lt) && type_is_prim_int(rt))) {
            emit_t0306_op(c, bin_op_text(op), lt, lnull, opsp);
        }
        diag_span_free(osp);
        return binary_result_od(c, op, lt, rt, out);
    }

    /* arithmetic/bitwise: + - * / % & | ^ (pointer pairs per the closed
     * sec. 12.5 enumeration: p + i, p - i, p - q with identical pointee;
     * i + p and mixed-pointee p - q are not applicable - Planner ruling
     * R-1) */
    {
        bool ok = (type_is_prim_int(lt) && type_is_prim_int(rt)) ||
                  (lt && lt->kind == TYPE_PTR &&
                   ((op == AST_BIN_ADD || op == AST_BIN_SUB) &&
                    rt && type_is_prim_int(rt))) ||
                  (op == AST_BIN_SUB && lt && lt->kind == TYPE_PTR &&
                   rt && rt->kind == TYPE_PTR && type_identical(lt, rt));
        if (!ok) {
            emit_t0306_op(c, bin_op_text(op), lt, lnull, opsp);
        }
        diag_span_free(osp);
        return binary_result_od(c, op, lt, rt, out);
    }

result_bool:
    *out = type_prim_new(AST_PRIM_BOOL);
    return *out ? TYP_OK : oom(c);
}

/* ---------------------------------------------------------------------------
 * Unary operator typing (spec sec. 10.2)
 * ------------------------------------------------------------------------- */

static TyperStatus check_unary_od(OptypeCtx *c, const NameModule *module,
                                  const AstNode *e, const Type *ot,
                                  Type **out)
{
    AstUnaryOp op = e->u.unary.op;
    DiagSpan *osp = unary_op_span(module, e, e->u.unary.operand);
    const DiagSpan *opsp = osp ? osp : e->span;
    *out = NULL;
    switch (op) {
    case AST_UN_NEG:
        if (ot && ot->kind == TYPE_PRIM && type_is_prim_int(ot)) {
            const TypePrimInfo *p = prim_of(ot);
            if (p && p->is_signed) {
                diag_span_free(osp);
                *out = type_clone(ot);
                return *out ? TYP_OK : oom(c);
            }
        }
        emit_t0306_op(c, un_op_text(op), ot, false, opsp);
        break;
    case AST_UN_PLUS:
        if (!(ot && type_is_prim_int(ot))) {
            emit_t0306_op(c, un_op_text(op), ot, false, opsp);
        }
        diag_span_free(osp);
        *out = type_clone(ot);
        return *out ? TYP_OK : oom(c);
    case AST_UN_BNOT:
        if (!(ot && type_is_prim_int(ot))) {
            emit_t0306_op(c, un_op_text(op), ot, false, opsp);
        }
        diag_span_free(osp);
        *out = type_clone(ot);
        return *out ? TYP_OK : oom(c);
    case AST_UN_NOT:
        if (!type_is_bool(ot)) {
            emit_t0306_op(c, un_op_text(op), ot, false, opsp);
        }
        diag_span_free(osp);
        *out = type_prim_new(AST_PRIM_BOOL);
        return *out ? TYP_OK : oom(c);
    case AST_UN_DEREF:
        if (ot && ot->kind == TYPE_PTR) {
            Type *elem = type_clone(ot->u.ptr.elem);
            diag_span_free(osp);
            if (!elem) return oom(c);
            *out = elem;
            return TYP_OK;
        }
        emit_t0306_op(c, un_op_text(op), ot, false, opsp);
        break;
    case AST_UN_ADDR:
        /* address-of: lvalue/mutability checks are later packages
         * (AIC-E0402/E0404); the result type is T* */
        {
            Type *pt = type_ptr_new(type_clone(ot));
            diag_span_free(osp);
            if (!pt) return oom(c);
            *out = pt;
            return TYP_OK;
        }
    default:
        break;
    }
    diag_span_free(osp);
    return unk(c);
}

/* ---------------------------------------------------------------------------
 * Full expression typer (the 11d subset)
 * ------------------------------------------------------------------------- */

static TyperStatus type_of_expr_od(OptypeCtx *c, const NameModule *module,
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
        return type_of_expr_od(c, module, e->u.paren.expr, out, out_is_null);
    case AST_EXPR_UNARY: {
        Type *ot = NULL;
        bool onull = false;
        TyperStatus ost = type_of_expr_od(c, module, e->u.unary.operand,
                                          &ot, &onull);
        TyperStatus rst;
        if (ost == TYP_OOM) { c->oom = true; return TYP_OOM; }
        if (ost != TYP_OK || onull) {
            /* operand untypable or null: still report operator misuse when
             * the operand carries a type; otherwise unknown */
            if (ost == TYP_OK && onull) {
                DiagSpan *osp = unary_op_span(module, e,
                                              e->u.unary.operand);
                emit_t0306_op(c, un_op_text(e->u.unary.op), NULL, true,
                              osp ? osp : e->span);
                diag_span_free(osp);
                type_free(ot);
                return unk(c);
            }
            type_free(ot);
            return unk(c);
        }
        rst = check_unary_od(c, module, e, ot, out);
        type_free(ot);
        return rst;
    }
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
        /* sizeof/alignof: the operand is not evaluated (sec. 10.4); no
         * inner operator sites. Result usize (sec. 10.2). */
        *out = type_prim_new(AST_PRIM_USIZE);
        break;
    case AST_EXPR_LEN:
    case AST_EXPR_PTR: {
        Type *ot = NULL;
        bool onull = false;
        TyperStatus ost = type_of_expr_od(c, module, e->u.size_op.operand,
                                          &ot, &onull);
        Type *result = NULL;
        const char *kw = e->kind == AST_EXPR_LEN ? "len" : "ptr";
        DiagSpan *kwsp;
        if (ost == TYP_OOM) { c->oom = true; type_free(ot); return TYP_OOM; }
        if (ost != TYP_OK) { type_free(ot); return TYP_UNKNOWN; }
        if (ot && (ot->kind == TYPE_ARRAY || ot->kind == TYPE_SLICE ||
                   (ot->kind == TYPE_PRIM && ot->u.prim == AST_PRIM_STR))) {
            if (e->kind == AST_EXPR_LEN) {
                result = type_prim_new(AST_PRIM_USIZE);
            } else if (ot->kind == TYPE_PRIM && ot->u.prim == AST_PRIM_STR) {
                result = type_ptr_new(type_prim_new(AST_PRIM_U8));
            } else {
                result = type_ptr_new(type_clone(ot->u.array.elem));
            }
        }
        if (!result) {
            kwsp = load_span_range(module ? module->src : NULL,
                                   e->span->start.offset,
                                   e->span->start.offset + 3);
            emit_t0306_op(c, kw, ot, onull, kwsp ? kwsp : e->span);
            diag_span_free(kwsp);
        }
        type_free(ot);
        if (!result) return unk(c);
        *out = result;
        return TYP_OK;
    }
    case AST_EXPR_INDEX: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr_od(c, module, e->u.index_slice.base,
                                          &bt, &bnull);
        Type *et = NULL;
        DiagSpan *opsp;
        if (bst == TYP_OOM) { c->oom = true; type_free(bt); return TYP_OOM; }
        check_expr_od(c, module, e->u.index_slice.index);
        if (bst == TYP_OK && bt &&
            (bt->kind == TYPE_ARRAY || bt->kind == TYPE_SLICE)) {
            et = bt->u.array.elem;
            bt->u.array.elem = NULL;
        } else if (bst == TYP_OK && bt && bt->kind == TYPE_PRIM &&
                   bt->u.prim == AST_PRIM_STR) {
            et = type_prim_new(AST_PRIM_U8);
        } else if (bst == TYP_OK && bt) {
            opsp = op_token_span(module, e->u.index_slice.base->span,
                                 e->u.index_slice.index ?
                                     e->u.index_slice.index->span :
                                     e->span);
            emit_t0306_op(c, "[]", bt, bnull, opsp ? opsp : e->span);
            diag_span_free(opsp);
        }
        type_free(bt);
        if (!et) return unk(c);
        *out = et;
        return TYP_OK;
    }
    case AST_EXPR_SLICE: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr_od(c, module, e->u.index_slice.base,
                                          &bt, &bnull);
        Type *st = NULL;
        DiagSpan *opsp;
        if (bst == TYP_OOM) { c->oom = true; type_free(bt); return TYP_OOM; }
        check_expr_od(c, module, e->u.index_slice.lo);
        check_expr_od(c, module, e->u.index_slice.hi);
        if (bst == TYP_OK && bt && bt->kind == TYPE_PRIM &&
            bt->u.prim == AST_PRIM_STR) {
            st = bt;
            bt = NULL;
        } else if (bst == TYP_OK && bt &&
                   (bt->kind == TYPE_ARRAY || bt->kind == TYPE_SLICE)) {
            st = type_slice_new(type_clone(bt->u.array.elem));
        } else if (bst == TYP_OK && bt) {
            opsp = op_token_span(module, e->u.index_slice.base->span,
                                 e->u.index_slice.lo ?
                                     e->u.index_slice.lo->span :
                                     (e->u.index_slice.hi ?
                                          e->u.index_slice.hi->span :
                                          e->span));
            emit_t0306_op(c, "[..]", bt, bnull, opsp ? opsp : e->span);
            diag_span_free(opsp);
        }
        type_free(bt);
        if (!st) return unk(c);
        *out = st;
        return TYP_OK;
    }
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP: {
        bool is_cast = e->kind == AST_EXPR_CAST;
        Type *tt = NULL;
        Type *ot = NULL;
        bool onull = false;
        TyperStatus tst = type_from_type_node(c, module, e->u.cast_wrap.type,
                                              &tt);
        TyperStatus ost = type_of_expr_od(c, module, e->u.cast_wrap.expr,
                                          &ot, &onull);
        if (tst == TYP_OOM || ost == TYP_OOM) {
            c->oom = true;
            type_free(tt);
            type_free(ot);
            return TYP_OOM;
        }
        if (tst != TYP_OK) { type_free(ot); return TYP_UNKNOWN; }
        if (tt->kind == TYPE_PRIM && tt->u.prim == AST_PRIM_VOID) {
            /* void misuse at a cast/wrap target */
            char msg[512];
            snprintf(msg, sizeof(msg), "'%s' operator not applicable to "
                     "operand type 'void'", is_cast ? "cast" : "wrap");
            emit_record(c, "AIC-T0306", msg, e->span);
            type_free(ot);
            *out = tt;
            return TYP_OK;
        }
        if (ost == TYP_OK) {
            bool valid = is_cast
                ? optype_cast_pair_valid(ot, onull, tt)
                : optype_wrap_pair_valid(ot, onull, tt);
            if (!valid) emit_t0308(c, is_cast, ot, onull, tt, e->span);
        } else {
            c->unknown = true;
        }
        type_free(ot);
        *out = tt;
        return TYP_OK;
    }
    case AST_EXPR_BINARY: {
        Type *lt = NULL, *rt = NULL;
        bool lnull = false, rnull = false;
        TyperStatus lst = type_of_expr_od(c, module, e->u.binary.lhs,
                                          &lt, &lnull);
        TyperStatus rst = type_of_expr_od(c, module, e->u.binary.rhs,
                                          &rt, &rnull);
        TyperStatus bst;
        if (lst == TYP_OOM || rst == TYP_OOM) {
            c->oom = true;
            type_free(lt);
            type_free(rt);
            return TYP_OOM;
        }
        if (lst != TYP_OK || rst != TYP_OK) {
            /* an operand was untypable: operator validity cannot be
             * decided here; the inner site already reported (or later
             * packages own it) */
            type_free(lt);
            type_free(rt);
            return unk(c);
        }
        bst = check_binary_od(c, module, e, lt, lnull, rt, rnull, out);
        type_free(lt);
        type_free(rt);
        if (bst == TYP_UNKNOWN && *out == NULL) return TYP_UNKNOWN;
        return bst;
    }
    case AST_EXPR_TERNARY: {
        Type *ct = NULL, *tt = NULL, *et = NULL;
        bool cnull = false, tnull = false, enull = false;
        TyperStatus cst = type_of_expr_od(c, module, e->u.branch.cond,
                                          &ct, &cnull);
        TyperStatus tst = type_of_expr_od(c, module, e->u.branch.then,
                                          &tt, &tnull);
        TyperStatus est = type_of_expr_od(c, module, e->u.branch.els,
                                          &et, &enull);
        if (cst == TYP_OOM || tst == TYP_OOM || est == TYP_OOM) {
            c->oom = true;
            type_free(ct); type_free(tt); type_free(et);
            return TYP_OOM;
        }
        if (cst == TYP_OK && !type_is_bool(ct)) {
            char msg[512];
            char *cd = ct ? type_describe(ct) : NULL;
            const DiagSpan *sp = od_site_span(module, e->u.branch.cond);
            snprintf(msg, sizeof(msg), "condition must be bool, found %s",
                     cnull ? "null" : (cd ? cd : "?"));
            free(cd);
            emit_record(c, "AIC-T0310", msg,
                        sp ? sp : e->u.branch.cond->span);
        }
        type_free(ct);
        if (tst == TYP_OK && est == TYP_OK && !tnull && !enull) {
            if (type_is_prim_int(tt) && type_is_prim_int(et)) {
                Type *common = convert_common_type(tt, et);
                if (common) {
                    *out = common;
                    type_free(tt); type_free(et);
                    return TYP_OK;
                }
                /* no common integer type: 11c's record already exists */
                type_free(tt); type_free(et);
                return unk(c);
            }
            if (type_identical(tt, et)) {
                *out = type_clone(tt);
                type_free(tt); type_free(et);
                return *out ? TYP_OK : oom(c);
            }
            /* null branch against a pointer: null -> T* */
            if (tnull && et && et->kind == TYPE_PTR) {
                *out = type_clone(et);
                type_free(tt); type_free(et);
                return *out ? TYP_OK : oom(c);
            }
            if (enull && tt && tt->kind == TYPE_PTR) {
                *out = type_clone(tt);
                type_free(tt); type_free(et);
                return *out ? TYP_OK : oom(c);
            }
            /* mismatched non-integer branches: rejected here with
             * AIC-T0307 (11c marks them unknown) */
            {
                char msg[512];
                char *td1 = tt ? type_describe(tt) : NULL;
                char *td2 = et ? type_describe(et) : NULL;
                snprintf(msg, sizeof(msg), "no common type: %s and %s",
                         tnull ? "null" : (td1 ? td1 : "?"),
                         enull ? "null" : (td2 ? td2 : "?"));
                free(td1);
                free(td2);
                emit_record(c, "AIC-T0307", msg, e->span);
            }
            type_free(tt); type_free(et);
            return unk(c);
        }
        type_free(tt); type_free(et);
        return unk(c);
    }
    case AST_EXPR_CALL: {
        const AstNode *callee = e->u.call.callee;
        const NameSymbol *fsym = NULL;
        size_t i;
        if (callee && callee->kind == AST_EXPR_IDENT) {
            fsym = name_symbol_for_node(module, callee);
        }
        if (!fsym || fsym->kind != NAME_SYM_FN || !fsym->decl) {
            /* runtime built-in or non-function callee: signatures are not
             * in the build; arguments are still evaluated */
            for (i = 0; i < e->u.call.nargs; i++) {
                check_expr_od(c, module, e->u.call.args[i]);
                if (c->oom) return TYP_OOM;
            }
            return unk(c);
        }
        {
            const AstNode *fdecl = fsym->decl;
            size_t n = e->u.call.nargs;
            size_t np = fdecl->u.fn_decl.nparams;
            if (n != np) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "call argument count mismatch: expected %zu, "
                         "found %zu", np, n);
                emit_record(c, "AIC-T0312", msg, e->span);
            }
            for (i = 0; i < n && i < np; i++) {
                const AstNode *param = fdecl->u.fn_decl.params[i];
                const AstNode *arg = e->u.call.args[i];
                Type *pt = NULL;
                TyperStatus pst = type_from_type_node(c, fsym->module,
                                                      param->u.named.type,
                                                      &pt);
                if (pst == TYP_OOM) { c->oom = true; type_free(pt); return TYP_OOM; }
                if (pst == TYP_OK && pt && arg &&
                    arg->kind == AST_EXPR_ARRAY_LITERAL &&
                    pt->kind == TYPE_ARRAY) {
                    check_array_literal_typed(c, module, arg, pt);
                } else {
                    check_expr_od(c, module, arg);
                }
                type_free(pt);
                if (c->oom) return TYP_OOM;
            }
            for (; i < n; i++) check_expr_od(c, module, e->u.call.args[i]);
            {
                Type *rt = NULL;
                TyperStatus rst = type_from_type_node(c, fsym->module,
                                                      fdecl->u.fn_decl.ret_type,
                                                      &rt);
                if (rst == TYP_OOM) return TYP_OOM;
                if (rst != TYP_OK) return unk(c);
                *out = rt;
                return TYP_OK;
            }
        }
    }
    case AST_EXPR_ASSIGN: {
        Type *tt = NULL, *vt = NULL;
        bool tnull = false, vnull = false;
        TyperStatus tst = type_of_expr_od(c, module, e->u.assign.target,
                                          &tt, &tnull);
        TyperStatus vst = type_of_expr_od(c, module, e->u.assign.value,
                                          &vt, &vnull);
        if (tst == TYP_OOM || vst == TYP_OOM) {
            c->oom = true;
            type_free(tt); type_free(vt);
            return TYP_OOM;
        }
        if (e->u.assign.op == AST_ASGN_ASSIGN) {
            /* plain assignment: conversion is 11c's; array-literal counts
             * are checked against the lvalue's array type */
            if (tst == TYP_OK && vst == TYP_OK && tt && vt &&
                tt->kind == TYPE_ARRAY &&
                e->u.assign.value->kind == AST_EXPR_ARRAY_LITERAL) {
                check_array_literal_typed(c, module, e->u.assign.value, tt);
            }
        } else if (tst == TYP_OK && vst == TYP_OK && tt && vt) {
            /* compound assignment: the compound operator must be valid for
             * the lvalue type (sec. 11.6 / 10.2) */
            AstAssignOp aop = e->u.assign.op;
            bool ok;
            DiagSpan *osp = op_token_span(module, e->u.assign.target->span,
                                          e->u.assign.value->span);
            if (aop == AST_ASGN_ADD || aop == AST_ASGN_SUB) {
                ok = (type_is_prim_int(tt) && type_is_prim_int(vt)) ||
                     (tt && tt->kind == TYPE_PTR && type_is_prim_int(vt));
            } else {
                ok = type_is_prim_int(tt) && type_is_prim_int(vt);
            }
            if (!ok) {
                emit_t0306_op(c, asgn_op_text(aop), tt, tnull,
                              osp ? osp : e->span);
            }
            diag_span_free(osp);
        } else {
            c->unknown = true;
        }
        type_free(vt);
        if (!tt) return TYP_UNKNOWN;
        *out = type_clone(tt);
        type_free(tt);
        return *out ? TYP_OK : oom(c);
    }
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        const NameSymbol *msym = name_symbol_for_node(module, e);
        if (msym && msym->kind == NAME_SYM_ENUM_MEMBER) {
            *out = type_enum_new(msym->owner);
            return *out ? TYP_OK : oom(c);
        }
        {
            Type *bt = NULL;
            bool bnull = false;
            TyperStatus bst = type_of_expr_od(c, module, e->u.member.base,
                                              &bt, &bnull);
            const Type *struct_t = NULL;
            const NameModule *fmod;
            const AstNode *fdecl;
            Type *ft = NULL;
            TyperStatus fst;
            DiagSpan *opsp;
            if (bst == TYP_OOM) { c->oom = true; type_free(bt); return TYP_OOM; }
            if (bst != TYP_OK) { type_free(bt); return TYP_UNKNOWN; }
            opsp = member_op_span(module, e, e->u.member.base);
            if (e->kind == AST_EXPR_ARROW) {
                if (bt && bt->kind == TYPE_PTR) struct_t = bt->u.ptr.elem;
                if (!struct_t || (struct_t && struct_t->kind != TYPE_STRUCT)) {
                    emit_t0306_op(c, "->", bt, bnull,
                                  opsp ? opsp : e->span);
                    diag_span_free(opsp);
                    type_free(bt);
                    return unk(c);
                }
            } else {
                if (bt && bt->kind == TYPE_STRUCT) struct_t = bt;
                if (!struct_t) {
                    emit_t0306_op(c, ".", bt, bnull,
                                  opsp ? opsp : e->span);
                    diag_span_free(opsp);
                    type_free(bt);
                    return unk(c);
                }
            }
            fdecl = struct_field_decl(struct_t, e->u.member.name);
            if (!fdecl) {
                emit_t0306_no_field(c, e->kind == AST_EXPR_ARROW ? "->" : ".",
                                    struct_t, e->u.member.name,
                                    opsp ? opsp : e->span);
                diag_span_free(opsp);
                type_free(bt);
                return unk(c);
            }
            diag_span_free(opsp);
            fmod = struct_t->u.sym->module;
            fst = type_from_type_node(c, fmod, fdecl->u.named.type, &ft);
            type_free(bt);
            if (fst == TYP_OOM) return TYP_OOM;
            if (fst != TYP_OK) return unk(c);
            *out = ft;
            return TYP_OK;
        }
    }
    case AST_EXPR_ARRAY_LITERAL: {
        size_t i;
        for (i = 0; i < e->u.array_literal.nelems; i++) {
            check_expr_od(c, module, e->u.array_literal.elems[i]);
            if (c->oom) return TYP_OOM;
        }
        return unk(c);
    }
    case AST_EXPR_STRUCT_INIT: {
        Type *st = NULL;
        TyperStatus sst = type_of_struct_init_base(c, module,
                                                   e->u.struct_init.base,
                                                   &st);
        size_t i;
        bool have_unknown = false;
        bool *seen = NULL;
        if (sst == TYP_OOM) return TYP_OOM;
        if (sst != TYP_OK || !st || st->kind != TYPE_STRUCT) {
            /* the base does not denote a struct type: "'{}' operator not
             * applicable to operand type '<base type>'" */
            Type *bt = st;
            bool bnull = false;
            TyperStatus bst;
            if (!bt || bt->kind != TYPE_STRUCT) {
                bst = type_of_expr_od(c, module, e->u.struct_init.base,
                                      &bt, &bnull);
                if (bst == TYP_OOM) { type_free(st); return TYP_OOM; }
            }
            emit_t0306_op(c, "{}", bt, bnull, e->span);
            type_free(bt);
            for (i = 0; i < e->u.struct_init.nfields; i++) {
                const AstNode *fi = e->u.struct_init.fields[i];
                if (fi->u.named.value) check_expr_od(c, module,
                                                     fi->u.named.value);
            }
            type_free(st);
            return unk(c);
        }
        seen = (bool *)calloc(st->u.sym->decl->u.struct_decl.nfields ?
                              st->u.sym->decl->u.struct_decl.nfields : 1,
                              sizeof(bool));
        if (!seen) { type_free(st); return oom(c); }
        for (i = 0; i < e->u.struct_init.nfields; i++) {
            const AstNode *fi = e->u.struct_init.fields[i];
            const AstNode *fd = struct_field_decl(st, fi->u.named.name);
            size_t fi_idx;
            char msg[512];
            DiagSpan *nsp;
            if (fi->u.named.value) {
                Type *ft = NULL;
                TyperStatus fst = fd ? type_from_type_node(
                    c, st->u.sym->module, fd->u.named.type, &ft) : TYP_UNKNOWN;
                if (fst == TYP_OOM) { free(seen); type_free(st); return TYP_OOM; }
                if (fst == TYP_OK && ft && ft->kind == TYPE_ARRAY &&
                    fi->u.named.value->kind == AST_EXPR_ARRAY_LITERAL) {
                    check_array_literal_typed(c, module, fi->u.named.value, ft);
                } else {
                    check_expr_od(c, module, fi->u.named.value);
                }
                type_free(ft);
            }
            if (!fd) {
                have_unknown = true;
                nsp = load_span_range(module ? module->src : NULL,
                                      fi->span->start.offset,
                                      fi->span->start.offset +
                                          (int64_t)strlen(fi->u.named.name));
                snprintf(msg, sizeof(msg),
                         "unknown field '%s' in struct literal of type '%s'",
                         fi->u.named.name,
                         st->u.sym->name ? st->u.sym->name : "?");
                emit_record(c, "AIC-T0313", msg, nsp ? nsp : fi->span);
                diag_span_free(nsp);
                continue;
            }
            /* find the declared field's index for duplicate/missing
             * bookkeeping */
            for (fi_idx = 0;
                 fi_idx < st->u.sym->decl->u.struct_decl.nfields; fi_idx++) {
                if (st->u.sym->decl->u.struct_decl.fields[fi_idx] == fd) break;
            }
            if (fi_idx < st->u.sym->decl->u.struct_decl.nfields) {
                if (seen[fi_idx]) {
                    have_unknown = true;
                    nsp = load_span_range(module ? module->src : NULL,
                                          fi->span->start.offset,
                                          fi->span->start.offset +
                                              (int64_t)strlen(fi->u.named.name));
                    snprintf(msg, sizeof(msg),
                             "duplicate field '%s' in struct literal of "
                             "type '%s'",
                             fi->u.named.name,
                             st->u.sym->name ? st->u.sym->name : "?");
                    emit_record(c, "AIC-T0313", msg,
                                nsp ? nsp : fi->span);
                    diag_span_free(nsp);
                    continue;
                }
                seen[fi_idx] = true;
            }
        }
        if (!have_unknown) {
            for (i = 0; i < st->u.sym->decl->u.struct_decl.nfields; i++) {
                if (seen[i]) continue;
                {
                    char msg[512];
                    const AstNode *fd = st->u.sym->decl->u.struct_decl.fields[i];
                    snprintf(msg, sizeof(msg),
                             "missing field '%s' in struct literal of "
                             "type '%s'",
                             fd->u.named.name,
                             st->u.sym->name ? st->u.sym->name : "?");
                    emit_record(c, "AIC-T0313", msg, e->span);
                }
            }
        }
        free(seen);
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
 * Site walkers: declarations, statements, functions
 * ------------------------------------------------------------------------- */

static void check_decl_init_od(OptypeCtx *c, const NameModule *module,
                               const AstNode *type_node, const AstNode *init)
{
    Type *tt = NULL;
    TyperStatus tst;
    if (!init) return;
    tst = type_from_type_node(c, module, type_node, &tt);
    if (tst == TYP_OOM) { c->oom = true; return; }
    if (tst == TYP_OK && tt && init->kind == AST_EXPR_ARRAY_LITERAL &&
        tt->kind == TYPE_ARRAY) {
        check_array_literal_typed(c, module, init, tt);
        type_free(tt);
        return;
    }
    type_free(tt);
    check_expr_od(c, module, init);
}

static void check_block_od(OptypeCtx *c, const NameModule *module,
                           const AstNode *block, const Type *ret_type);

/* The primary span for a condition/selector: the corpus pins identifier
 * sources to their declaration (tests/negative/cases/derived-type-
 * condition-not-bool and derived-type-switch-selector-type expect the
 * declaration identifier of `x`/`s`, not the reference). */
static const DiagSpan *od_site_span(const NameModule *module,
                                    const AstNode *expr)
{
    const NameSymbol *sym;
    if (!expr) return NULL;
    if (expr->kind == AST_EXPR_IDENT ||
        expr->kind == AST_EXPR_MEMBER ||
        expr->kind == AST_EXPR_ARROW) {
        sym = name_symbol_for_node(module, expr);
        if (sym && sym->span) return sym->span;
    }
    return expr->span;
}

static void check_condition_od(OptypeCtx *c, const NameModule *module,
                               const AstNode *cond)
{
    Type *t = NULL;
    bool isnull = false;
    TyperStatus st = type_of_expr_od(c, module, cond, &t, &isnull);
    if (st == TYP_OOM) { c->oom = true; type_free(t); return; }
    if (st == TYP_OK && !type_is_bool(t)) {
        char msg[512];
        char *td = t ? type_describe(t) : NULL;
        const DiagSpan *sp = od_site_span(module, cond);
        snprintf(msg, sizeof(msg), "condition must be bool, found %s",
                 isnull ? "null" : (td ? td : "?"));
        free(td);
        emit_record(c, "AIC-T0310", msg, sp ? sp : cond->span);
    }
    type_free(t);
}

static void check_selector_od(OptypeCtx *c, const NameModule *module,
                              const AstNode *sel)
{
    Type *t = NULL;
    bool isnull = false;
    TyperStatus st = type_of_expr_od(c, module, sel, &t, &isnull);
    if (st == TYP_OOM) { c->oom = true; type_free(t); return; }
    if (st == TYP_OK) {
        const TypePrimInfo *p = t ? prim_of(t) : NULL;
        bool ok = !isnull && ((p && p->is_integer) ||
                              (t && t->kind == TYPE_ENUM));
        if (!ok) {
            char msg[512];
            char *td = t ? type_describe(t) : NULL;
            const DiagSpan *sp = od_site_span(module, sel);
            snprintf(msg, sizeof(msg),
                     "switch selector must be integer or enum type, found %s",
                     isnull ? "null" : (td ? td : "?"));
            free(td);
            emit_record(c, "AIC-T0311", msg, sp ? sp : sel->span);
        }
    }
    type_free(t);
}

static void check_stmt_od(OptypeCtx *c, const NameModule *module,
                          const AstNode *s, const Type *ret_type)
{
    if (!s) return;
    switch (s->kind) {
    case AST_BLOCK:
        check_block_od(c, module, s, ret_type);
        break;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        check_decl_init_od(c, module, s->u.local_decl.type,
                           s->u.local_decl.init);
        break;
    case AST_IF:
        check_condition_od(c, module, s->u.branch.cond);
        check_block_od(c, module, s->u.branch.then, ret_type);
        if (s->u.branch.els) {
            if (s->u.branch.els->kind == AST_IF) {
                check_stmt_od(c, module, s->u.branch.els, ret_type);
            } else {
                check_block_od(c, module, s->u.branch.els, ret_type);
            }
        }
        break;
    case AST_WHILE:
        check_condition_od(c, module, s->u.while_loop.cond);
        check_block_od(c, module, s->u.while_loop.body, ret_type);
        break;
    case AST_FOR: {
        const AstNode *init = s->u.for_loop.init;
        if (init && (init->kind == AST_VAR_DECL ||
                     init->kind == AST_CONST_DECL)) {
            check_decl_init_od(c, module, init->u.local_decl.type,
                               init->u.local_decl.init);
        } else if (init) {
            check_expr_od(c, module, init);
        }
        if (s->u.for_loop.cond) {
            check_condition_od(c, module, s->u.for_loop.cond);
        }
        check_expr_od(c, module, s->u.for_loop.step);
        check_block_od(c, module, s->u.for_loop.body, ret_type);
        break;
    }
    case AST_SWITCH: {
        size_t i;
        check_selector_od(c, module, s->u.switch_stmt.selector);
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (cl->u.clause.value) {
                check_expr_od(c, module, cl->u.clause.value);
            }
            check_block_od(c, module, cl->u.clause.body, ret_type);
            if (c->oom) return;
        }
        break;
    }
    case AST_RETURN: {
        const AstNode *v = s->u.ret.value;
        if (!v) break;
        if (ret_type && ret_type->kind == TYPE_ARRAY &&
            v->kind == AST_EXPR_ARRAY_LITERAL) {
            check_array_literal_typed(c, module, v, ret_type);
        } else {
            check_expr_od(c, module, v);
        }
        break;
    }
    case AST_EXPR_STMT:
        check_expr_od(c, module, s->u.expr_stmt.expr);
        break;
    case AST_EMPTY_STMT:
    case AST_BREAK:
    case AST_CONTINUE:
    default:
        break;
    }
}

static void check_block_od(OptypeCtx *c, const NameModule *module,
                           const AstNode *block, const Type *ret_type)
{
    size_t i;
    if (!block) return;
    for (i = 0; i < block->u.list.count; i++) {
        check_stmt_od(c, module, block->u.list.items[i], ret_type);
        if (c->oom) return;
    }
}

static void check_fn_od(OptypeCtx *c, const NameModule *module,
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
    if (fn->u.fn_decl.body) {
        check_block_od(c, module, fn->u.fn_decl.body, rt);
    }
    type_free(rt);
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

OptypeStatus types_optype_check(const NameResult *result,
                                DiagRecord ***out_records,
                                size_t *out_record_count)
{
    OptypeCtx c;
    size_t m;
    memset(&c, 0, sizeof(c));
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    if (!result) return OPTYPE_OK;

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
                check_decl_init_od(&c, module, decl->u.global_decl.type,
                                   decl->u.global_decl.init);
                break;
            case AST_FN_DECL:
                check_fn_od(&c, module, decl);
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
    if (c.nrecords > 0) return OPTYPE_DIAG_ERROR;
    return c.unknown ? OPTYPE_UNKNOWN : OPTYPE_OK;

oom:
    for (m = 0; m < c.nrecords; m++) diag_record_free(c.records[m]);
    free(c.records);
    return OPTYPE_OOM;
}

