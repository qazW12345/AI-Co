/* bootstrap/src/types/layout.c
 *
 * AI-Co Stage-0 struct/enum layout and deterministic padding (WP-M0-11b).
 * See layout.h for the model and the bounded constant-integer subset;
 * design decisions in the README (11b section of layout.h header block).
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "layout.h"

#include "../lex/lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Bounded constant-integer evaluation (documented 11b subset)
 * ------------------------------------------------------------------------- */

static int lex_int_width(LexIntType t)
{
    switch (t) {
    case LEX_INT_I8:  case LEX_INT_U8:   return 8;
    case LEX_INT_I16: case LEX_INT_U16:  return 16;
    case LEX_INT_I32: case LEX_INT_U32:  return 32;
    case LEX_INT_I64: case LEX_INT_U64:
    case LEX_INT_ISIZE: case LEX_INT_USIZE: return 64;
    }
    return 0;
}

static bool lex_int_signed(LexIntType t)
{
    switch (t) {
    case LEX_INT_I8: case LEX_INT_I16: case LEX_INT_I32:
    case LEX_INT_I64: case LEX_INT_ISIZE:
        return true;
    default:
        return false;
    }
}

/* Well-defined int64 add/sub/mul with overflow detection (C17 does not
 * define signed overflow). */
static bool i64_add_ovf(int64_t a, int64_t b, int64_t *out)
{
    if (b > 0 && a > INT64_MAX - b) return true;
    if (b < 0 && a < INT64_MIN - b) return true;
    *out = a + b;
    return false;
}

static bool i64_sub_ovf(int64_t a, int64_t b, int64_t *out)
{
    if (b < 0 && a > INT64_MAX + b) return true;
    if (b > 0 && a < INT64_MIN + b) return true;
    *out = a - b;
    return false;
}

static bool i64_mul_ovf(int64_t a, int64_t b, int64_t *out)
{
    if (a == 0 || b == 0) { *out = 0; return false; }
    if (a == -1) {
        if (b == INT64_MIN) return true;
        *out = -b;
        return false;
    }
    if (b == -1) {
        if (a == INT64_MIN) return true;
        *out = -a;
        return false;
    }
    /* |a|, |b| >= 2; divisions below are defined (no INT64_MIN / -1). */
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) return true;
        } else {
            if (b < INT64_MIN / a) return true;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) return true;
        } else {
            if (b < INT64_MAX / a) return true;
        }
    }
    *out = a * b;
    return false;
}

/* Arithmetic right shift, defined for negative values (C17 leaves signed
 * right shift implementation-defined). */
static int64_t i64_ashr(int64_t a, int b)
{
    if (a >= 0) return a >> b;
    return ~(~a >> b);
}

LayoutEvalStatus layout_eval_int_expr(const AstNode *e, LayoutEvalValue *out)
{
    if (!e) return LAYOUT_EVAL_UNEVALUABLE;
    switch (e->kind) {
    case AST_EXPR_INT_LITERAL: {
        LexIntType t = e->u.int_literal.type;
        uint64_t mag = e->u.int_literal.value;
        if (e->u.int_literal.is_min) {
            /* Denotes -(2^(width-1)) (the grammar production "-" literal). */
            int w = lex_int_width(t);
            if (w >= 64) {
                out->v = INT64_MIN;
            } else {
                out->v = -(int64_t)((uint64_t)1 << (w - 1));
            }
            out->big = false;
            return LAYOUT_EVAL_OK;
        }
        if (lex_int_signed(t)) {
            /* magnitude <= 2^(w-1)-1 <= INT64_MAX for non-min signed. */
            out->v = (int64_t)mag;
            out->big = false;
        } else if (mag > (uint64_t)INT64_MAX) {
            /* Unsigned literal in [2^63, 2^64-1]: two's complement. */
            out->v = (int64_t)mag;
            out->big = true;
        } else {
            out->v = (int64_t)mag;
            out->big = false;
        }
        return LAYOUT_EVAL_OK;
    }
    case AST_EXPR_PAREN:
        return layout_eval_int_expr(e->u.paren.expr, out);
    case AST_EXPR_UNARY: {
        const AstNode *op = e->u.unary.operand;
        if (e->u.unary.op == AST_UN_PLUS) {
            return layout_eval_int_expr(op, out);
        }
        if (e->u.unary.op == AST_UN_NEG) {
            /* "-" directly over a min-magnitude literal denotes the type
             * minimum; the literal already evaluated to it. */
            if (op && op->kind == AST_EXPR_INT_LITERAL &&
                op->u.int_literal.is_min) {
                return layout_eval_int_expr(op, out);
            }
            {
                LayoutEvalValue a;
                LayoutEvalStatus st = layout_eval_int_expr(op, &a);
                if (st != LAYOUT_EVAL_OK) return st;
                if (a.big || a.v == INT64_MIN) return LAYOUT_EVAL_OVERFLOW;
                out->v = -a.v;
                out->big = false;
                return LAYOUT_EVAL_OK;
            }
        }
        if (e->u.unary.op == AST_UN_BNOT) {
            LayoutEvalValue a;
            LayoutEvalStatus st = layout_eval_int_expr(op, &a);
            if (st != LAYOUT_EVAL_OK) return st;
            if (a.big) return LAYOUT_EVAL_OVERFLOW;
            out->v = ~a.v;
            out->big = false;
            return LAYOUT_EVAL_OK;
        }
        return LAYOUT_EVAL_UNEVALUABLE;
    }
    case AST_EXPR_BINARY: {
        LayoutEvalValue a, b;
        LayoutEvalStatus st = layout_eval_int_expr(e->u.binary.lhs, &a);
        if (st != LAYOUT_EVAL_OK) return st;
        st = layout_eval_int_expr(e->u.binary.rhs, &b);
        if (st != LAYOUT_EVAL_OK) return st;
        /* 64-bit wrap arithmetic on big-unsigned operands is outside the
         * 11b subset (WP-M0-12 owns it). */
        if (a.big || b.big) return LAYOUT_EVAL_OVERFLOW;
        switch (e->u.binary.op) {
        case AST_BIN_ADD:
            if (i64_add_ovf(a.v, b.v, &out->v)) return LAYOUT_EVAL_OVERFLOW;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_SUB:
            if (i64_sub_ovf(a.v, b.v, &out->v)) return LAYOUT_EVAL_OVERFLOW;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_MUL:
            if (i64_mul_ovf(a.v, b.v, &out->v)) return LAYOUT_EVAL_OVERFLOW;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_DIV:
            if (b.v == 0) return LAYOUT_EVAL_DIV_ZERO;
            if (a.v == INT64_MIN && b.v == -1) return LAYOUT_EVAL_OVERFLOW;
            out->v = a.v / b.v;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_MOD:
            if (b.v == 0) return LAYOUT_EVAL_DIV_ZERO;
            out->v = (a.v == INT64_MIN && b.v == -1) ? 0 : a.v % b.v;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_SHL: {
            uint64_t ua;
            if (b.v < 0 || b.v >= 64) return LAYOUT_EVAL_SHIFT_RANGE;
            if (b.v == 0) { out->v = a.v; out->big = false; return LAYOUT_EVAL_OK; }
            if (a.v > 0 && a.v > (INT64_MAX >> b.v)) return LAYOUT_EVAL_OVERFLOW;
            if (a.v < 0) {
                int64_t floor_min = (b.v == 0)
                    ? INT64_MIN
                    : -(int64_t)((uint64_t)1 << (63 - (int)b.v));
                if (a.v < floor_min) return LAYOUT_EVAL_OVERFLOW;
            }
            ua = (uint64_t)a.v;
            out->v = (int64_t)(ua << b.v);
            out->big = false;
            return LAYOUT_EVAL_OK;
        }
        case AST_BIN_SHR:
            if (b.v < 0 || b.v >= 64) return LAYOUT_EVAL_SHIFT_RANGE;
            out->v = i64_ashr(a.v, (int)b.v);
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_BAND:
            out->v = a.v & b.v;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_BXOR:
            out->v = a.v ^ b.v;
            out->big = false;
            return LAYOUT_EVAL_OK;
        case AST_BIN_BOR:
            out->v = a.v | b.v;
            out->big = false;
            return LAYOUT_EVAL_OK;
        default:
            return LAYOUT_EVAL_UNEVALUABLE;
        }
    }
    default:
        return LAYOUT_EVAL_UNEVALUABLE;
    }
}

/* ---------------------------------------------------------------------------
 * Build context
 * ------------------------------------------------------------------------- */

typedef struct LayoutCtx {
    LayoutBuild *build;
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unevaluable;
} LayoutCtx;

static bool ctx_rec_push(LayoutCtx *c, DiagRecord *r)
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

static DiagRecord *new_type_record(LayoutCtx *c, const char *code,
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

/* ---------------------------------------------------------------------------
 * Build table helpers
 * ------------------------------------------------------------------------- */

static bool build_push_struct(LayoutBuild *b, const NameSymbol *sym,
                              const LayoutStruct *ls)
{
    if (b->nstructs == b->structs_cap) {
        size_t ncap = b->structs_cap ? b->structs_cap * 2 : 16;
        const NameSymbol **ns = (const NameSymbol **)realloc(
            (void *)b->struct_syms, ncap * sizeof(const NameSymbol *));
        if (!ns) return false;
        b->struct_syms = ns;
        LayoutStruct *nl = (LayoutStruct *)realloc(
            b->struct_layouts, ncap * sizeof(LayoutStruct));
        if (!nl) return false;
        b->struct_layouts = nl;
        b->structs_cap = ncap;
    }
    b->struct_syms[b->nstructs] = sym;
    b->struct_layouts[b->nstructs] = *ls;
    b->nstructs++;
    return true;
}

static bool build_push_enum(LayoutBuild *b, const NameSymbol *sym,
                            const LayoutEnum *le)
{
    if (b->nenums == b->enums_cap) {
        size_t ncap = b->enums_cap ? b->enums_cap * 2 : 16;
        const NameSymbol **ns = (const NameSymbol **)realloc(
            (void *)b->enum_syms, ncap * sizeof(const NameSymbol *));
        if (!ns) return false;
        b->enum_syms = ns;
        LayoutEnum *nl = (LayoutEnum *)realloc(
            b->enum_layouts, ncap * sizeof(LayoutEnum));
        if (!nl) return false;
        b->enum_layouts = nl;
        b->enums_cap = ncap;
    }
    b->enum_syms[b->nenums] = sym;
    b->enum_layouts[b->nenums] = *le;
    b->nenums++;
    return true;
}

/* Record a declaration whose layout was skipped as unevaluable (a
 * member-value/array-extent expression outside the 11b subset; WP-M0-12
 * owns full composition). The declaration's layout is not published;
 * the marker lets a later by-value reference route to
 * LAYOUT_UNEVALUABLE instead of the defensive LAYOUT_UNSUPPORTED, so
 * the driver can send the program to the const stage. */
static bool build_mark_struct_unevaluable(LayoutBuild *b,
                                          const NameSymbol *sym)
{
    if (b->nstruct_unevaluable == b->struct_unevaluable_cap) {
        size_t ncap = b->struct_unevaluable_cap
                          ? b->struct_unevaluable_cap * 2
                          : 16;
        const NameSymbol **ns = (const NameSymbol **)realloc(
            (void *)b->struct_unevaluable,
            ncap * sizeof(const NameSymbol *));
        if (!ns) return false;
        b->struct_unevaluable = ns;
        b->struct_unevaluable_cap = ncap;
    }
    b->struct_unevaluable[b->nstruct_unevaluable++] = sym;
    return true;
}

static bool build_mark_enum_unevaluable(LayoutBuild *b,
                                        const NameSymbol *sym)
{
    if (b->nenum_unevaluable == b->enum_unevaluable_cap) {
        size_t ncap = b->enum_unevaluable_cap ? b->enum_unevaluable_cap * 2
                                              : 16;
        const NameSymbol **ns = (const NameSymbol **)realloc(
            (void *)b->enum_unevaluable,
            ncap * sizeof(const NameSymbol *));
        if (!ns) return false;
        b->enum_unevaluable = ns;
        b->enum_unevaluable_cap = ncap;
    }
    b->enum_unevaluable[b->nenum_unevaluable++] = sym;
    return true;
}

static bool build_struct_unevaluable(const LayoutBuild *b,
                                     const NameSymbol *sym)
{
    size_t i;
    if (!b || !sym) return false;
    for (i = 0; i < b->nstruct_unevaluable; i++) {
        if (b->struct_unevaluable[i] == sym) return true;
    }
    return false;
}

static bool build_enum_unevaluable(const LayoutBuild *b,
                                   const NameSymbol *sym)
{
    size_t i;
    if (!b || !sym) return false;
    for (i = 0; i < b->nenum_unevaluable; i++) {
        if (b->enum_unevaluable[i] == sym) return true;
    }
    return false;
}

const LayoutStruct *layout_build_struct(const LayoutBuild *build,
                                        const NameSymbol *sym)
{
    size_t i;
    if (!build || !sym) return NULL;
    for (i = 0; i < build->nstructs; i++) {
        if (build->struct_syms[i] == sym) return &build->struct_layouts[i];
    }
    return NULL;
}

const LayoutEnum *layout_build_enum(const LayoutBuild *build,
                                    const NameSymbol *sym)
{
    size_t i;
    if (!build || !sym) return NULL;
    for (i = 0; i < build->nenums; i++) {
        if (build->enum_syms[i] == sym) return &build->enum_layouts[i];
    }
    return NULL;
}

void layout_build_free(LayoutBuild *build)
{
    size_t i;
    if (!build) return;
    for (i = 0; i < build->nstructs; i++) {
        free(build->struct_layouts[i].fields);
    }
    for (i = 0; i < build->nenums; i++) {
        free(build->enum_layouts[i].members);
    }
    free(build->struct_syms);
    free(build->struct_layouts);
    free(build->enum_syms);
    free(build->enum_layouts);
    free(build->struct_unevaluable);
    free(build->enum_unevaluable);
    free(build);
}

/* ---------------------------------------------------------------------------
 * Type size/alignment from an AST type node
 * ------------------------------------------------------------------------- */

static int64_t align_up(int64_t value, int64_t align, bool *ovf)
{
    int64_t pad;
    *ovf = false;
    if (align <= 1) return value;
    pad = align - 1;
    if (value > INT64_MAX - pad) { *ovf = true; return value; }
    return ((value + pad) / align) * align;
}

static LayoutStatus type_info(LayoutCtx *c, const NameModule *module,
                              const AstNode *type_node,
                              LayoutSizeAlign *out);

/* The layout of a named struct/enum referenced by a type node, resolved
 * through the build table. NULL when unknown (defensive: malformed input,
 * or a by-value reference to a struct not yet laid out, which completeness
 * rejects in valid builds). */
static const LayoutStruct *lookup_struct_sym(LayoutCtx *c,
                                             const NameSymbol *sym)
{
    if (!c || !c->build) return NULL;
    return layout_build_struct(c->build, sym);
}

static const LayoutEnum *lookup_enum_sym(LayoutCtx *c, const NameSymbol *sym)
{
    if (!c || !c->build) return NULL;
    return layout_build_enum(c->build, sym);
}

static LayoutStatus type_info(LayoutCtx *c, const NameModule *module,
                              const AstNode *type_node,
                              LayoutSizeAlign *out)
{
    if (!type_node || !module) return LAYOUT_UNSUPPORTED;
    switch (type_node->kind) {
    case AST_TYPE_PRIM: {
        const TypePrimInfo *p = types_prim_info(type_node->u.type_prim.prim);
        if (!p) return LAYOUT_UNSUPPORTED;
        out->size = p->size_bytes;
        out->align = p->align_bytes;
        return LAYOUT_OK;
    }
    case AST_TYPE_NAMED: {
        const NameSymbol *sym = name_symbol_for_node(module, type_node);
        if (!sym) return LAYOUT_UNSUPPORTED;
        if (sym->kind == NAME_SYM_STRUCT) {
            const LayoutStruct *ls = lookup_struct_sym(c, sym);
            if (!ls) {
                /* A declaration whose layout was skipped as unevaluable
                 * (11b subset boundary, WP-M0-12 territory) routes to
                 * LAYOUT_UNEVALUABLE so a future driver can send the
                 * program to the const stage; an unknown symbol stays
                 * defensive LAYOUT_UNSUPPORTED. */
                if (build_struct_unevaluable(c->build, sym)) {
                    c->unevaluable = true;
                    return LAYOUT_UNEVALUABLE;
                }
                return LAYOUT_UNSUPPORTED;
            }
            out->size = ls->size;
            out->align = ls->align;
            return LAYOUT_OK;
        }
        if (sym->kind == NAME_SYM_ENUM) {
            const LayoutEnum *le = lookup_enum_sym(c, sym);
            if (!le) {
                if (build_enum_unevaluable(c->build, sym)) {
                    c->unevaluable = true;
                    return LAYOUT_UNEVALUABLE;
                }
                return LAYOUT_UNSUPPORTED;
            }
            out->size = le->size;
            out->align = le->align;
            return LAYOUT_OK;
        }
        return LAYOUT_UNSUPPORTED;
    }
    case AST_TYPE_PTR:
        out->size = 8;
        out->align = 8;
        return LAYOUT_OK;
    case AST_TYPE_SLICE:
        out->size = 16;
        out->align = 8;
        return LAYOUT_OK;
    case AST_TYPE_ARRAY: {
        LayoutSizeAlign e;
        LayoutEvalValue ev;
        LayoutEvalStatus es;
        LayoutStatus st = type_info(c, module, type_node->u.type_derived.base,
                                    &e);
        if (st != LAYOUT_OK) return st;
        es = layout_eval_int_expr(type_node->u.type_derived.len, &ev);
        if (es != LAYOUT_EVAL_OK) {
            /* Unsupported form or const failure (div-zero etc.): both are
             * WP-M0-12 territory; no type-phase code exists. Surface as
             * unevaluable, no record. */
            c->unevaluable = true;
            return LAYOUT_UNEVALUABLE;
        }
        if (ev.big || ev.v < 0) {
            /* Extent must be a non-negative usize-compatible constant;
             * negative extents are rejected by a later package (no
             * type-phase code). */
            c->unevaluable = true;
            return LAYOUT_UNEVALUABLE;
        }
        if (e.size != 0 && ev.v > INT64_MAX / e.size) {
            /* Size would exceed the target addressable range; defensive. */
            return LAYOUT_UNSUPPORTED;
        }
        out->size = e.size * ev.v;
        out->align = e.align;
        return LAYOUT_OK;
    }
    default:
        return LAYOUT_UNSUPPORTED;
    }
}

LayoutStatus layout_build_type_info(const LayoutBuild *build,
                                    const NameModule *module,
                                    const AstNode *type_node,
                                    LayoutSizeAlign *out)
{
    LayoutCtx tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.build = (LayoutBuild *)build;
    return type_info(&tmp, module, type_node, out);
}

/* ---------------------------------------------------------------------------
 * Struct layout (spec sec. 7.4)
 * ------------------------------------------------------------------------- */

static LayoutStatus compute_struct(LayoutCtx *c, const NameModule *module,
                                   const AstNode *decl, LayoutStruct *out)
{
    size_t n = decl->u.struct_decl.nfields;
    size_t i;
    int64_t align = 1;
    int64_t offset = 0;
    int64_t end = 0;
    bool ovf;

    memset(out, 0, sizeof(*out));
    if (n > 0) {
        out->fields = (LayoutField *)calloc(n, sizeof(LayoutField));
        if (!out->fields) { c->oom = true; return LAYOUT_OOM; }
    }
    for (i = 0; i < n; i++) {
        const AstNode *f = decl->u.struct_decl.fields[i];
        LayoutSizeAlign ta;
        LayoutStatus st = type_info(c, module, f->u.named.type, &ta);
        int64_t na, fend;
        if (st != LAYOUT_OK) {
            free(out->fields);
            memset(out, 0, sizeof(*out));
            return st;
        }
        na = align_up(end, ta.align, &ovf);
        if (ovf) {
            free(out->fields);
            memset(out, 0, sizeof(*out));
            return LAYOUT_UNSUPPORTED;
        }
        offset = na;
        out->fields[i].name = f->u.named.name;
        out->fields[i].offset = offset;
        out->fields[i].type = ta;
        out->fields[i].pad_before = offset - end;
        if (ta.size > INT64_MAX - offset) {
            free(out->fields);
            memset(out, 0, sizeof(*out));
            return LAYOUT_UNSUPPORTED;
        }
        fend = offset + ta.size;
        end = fend;
        if (ta.align > align) align = ta.align;
    }
    {
        int64_t size = align_up(end, align, &ovf);
        if (ovf) {
            free(out->fields);
            memset(out, 0, sizeof(*out));
            return LAYOUT_UNSUPPORTED;
        }
        out->size = size;
        out->align = align;
        out->nfields = n;
        out->tail_padding = size - end;
    }
    return LAYOUT_OK;
}

size_t layout_struct_padding(const LayoutStruct *ls, LayoutPadRange *ranges,
                             size_t cap)
{
    size_t n = 0;
    size_t i;
    if (!ls) return 0;
    for (i = 0; i < ls->nfields; i++) {
        if (ls->fields[i].pad_before > 0) {
            if (ranges && n < cap) {
                ranges[n].offset = ls->fields[i].offset -
                                   ls->fields[i].pad_before;
                ranges[n].length = ls->fields[i].pad_before;
            }
            n++;
        }
    }
    if (ls->tail_padding > 0) {
        if (ranges && n < cap) {
            ranges[n].offset = ls->size - ls->tail_padding;
            ranges[n].length = ls->tail_padding;
        }
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Enum layout (spec sec. 7.5)
 * ------------------------------------------------------------------------- */

/* Ranges of the underlying integer type T. */
static void int_range(const TypePrimInfo *p, int64_t *signed_lo,
                      int64_t *signed_hi, uint64_t *unsigned_hi,
                      bool *is_signed)
{
    uint64_t umax;
    if (p->width_bits >= 64) {
        umax = UINT64_MAX;
    } else {
        umax = ((uint64_t)1 << p->width_bits) - 1;
    }
    *is_signed = p->is_signed;
    *signed_lo = (int64_t)(UINT64_MAX -
                           ((uint64_t)1 << (p->width_bits - 1)) + 1);
    *signed_hi = (int64_t)(umax >> 1);
    *unsigned_hi = umax;
}

static bool member_representable(const LayoutEnumMember *mm,
                                 const TypePrimInfo *p)
{
    int64_t lo, hi;
    uint64_t uhi;
    bool is_signed;
    if (mm->domain_overflow) return false;
    int_range(p, &lo, &hi, &uhi, &is_signed);
    if (is_signed) {
        if (mm->big_unsigned) return false;
        return mm->value >= lo && mm->value <= hi;
    }
    if (mm->big_unsigned) {
        return (uint64_t)mm->value <= uhi;
    }
    return mm->value >= 0 && (uint64_t)mm->value <= uhi;
}

static void render_member_value(const LayoutEnumMember *mm, char *buf,
                                size_t bufsize)
{
    if (mm->domain_overflow) {
        snprintf(buf, bufsize, "18446744073709551616");
        return;
    }
    if (mm->big_unsigned) {
        snprintf(buf, bufsize, "%llu",
                 (unsigned long long)(uint64_t)mm->value);
        return;
    }
    snprintf(buf, bufsize, "%lld", (long long)mm->value);
}

static void emit_t0301(LayoutCtx *c, const AstNode *member,
                       const LayoutEnumMember *mm, const TypePrimInfo *p)
{
    char vbuf[32];
    char msg[192];
    const DiagSpan *span;
    DiagRecord *r;

    render_member_value(mm, vbuf, sizeof(vbuf));
    snprintf(msg, sizeof(msg),
             "enum member value %s is not representable in underlying type %s",
             vbuf, p->name);
    /* Explicit values pin the value-expression span (corpus); implicit
     * (continuation) failures use the member identifier span. */
    span = member->u.named.value ? member->u.named.value->span
                                 : member->span;
    r = new_type_record(c, "AIC-T0301", msg, span);
    if (r && !ctx_rec_push(c, r)) diag_record_free(r);
}

static LayoutStatus compute_enum(LayoutCtx *c, const NameModule *module,
                                 const AstNode *decl, LayoutEnum *out)
{
    const AstNode *under = decl->u.enum_decl.underlying;
    const TypePrimInfo *p;
    size_t n = decl->u.enum_decl.nmembers;
    size_t i;
    int64_t prev = 0;
    bool prev_big = false;
    bool prev_overflow = false;   /* previous continuation exceeded 2^64-1 */
    bool prev_valid = true;
    bool was_unevaluable;

    (void)module;
    memset(out, 0, sizeof(*out));
    if (!under || under->kind != AST_TYPE_PRIM) return LAYOUT_UNSUPPORTED;
    p = types_prim_info(under->u.type_prim.prim);
    if (!p || !p->is_integer) return LAYOUT_UNSUPPORTED;
    was_unevaluable = c->unevaluable;

    if (n > 0) {
        out->members = (LayoutEnumMember *)calloc(n,
                                                  sizeof(LayoutEnumMember));
        if (!out->members) { c->oom = true; return LAYOUT_OOM; }
    }
    for (i = 0; i < n; i++) {
        const AstNode *m = decl->u.enum_decl.members[i];
        LayoutEnumMember *mm = &out->members[i];
        if (!m) {
            free(out->members);
            memset(out, 0, sizeof(*out));
            return LAYOUT_UNSUPPORTED;
        }
        mm->name = m->u.named.name;
        if (m->u.named.value) {
            LayoutEvalValue ev;
            LayoutEvalStatus es = layout_eval_int_expr(m->u.named.value, &ev);
            if (es != LAYOUT_EVAL_OK) {
                /* Unsupported form or const failure: WP-M0-12 territory;
                 * no record (the value is not known to be unrepresentable). */
                c->unevaluable = true;
                prev_valid = false;
                continue;
            }
            mm->has_explicit = true;
            mm->value = ev.v;
            mm->big_unsigned = ev.big;
            prev = ev.v;
            prev_big = ev.big;
            prev_overflow = false;
            prev_valid = true;
        } else {
            if (i == 0) {
                mm->value = 0;
                mm->big_unsigned = false;
                prev = 0;
                prev_big = false;
                prev_overflow = false;
                prev_valid = true;
            } else if (!prev_valid) {
                /* Cannot continue from an unknown explicit value. */
                c->unevaluable = true;
                continue;
            } else if (prev_overflow) {
                /* Every continuation past 2^64-1 exceeds it again. */
                mm->domain_overflow = true;
            } else if (prev_big) {
                if ((uint64_t)prev == UINT64_MAX) {
                    mm->domain_overflow = true;
                    prev_overflow = true;
                } else {
                    mm->big_unsigned = true;
                    mm->value = prev + 1;   /* still in [2^63, 2^64-1] */
                    prev = mm->value;
                }
            } else if (prev == INT64_MAX) {
                /* prev + 1 == 2^63: enters the big-unsigned range. */
                mm->big_unsigned = true;
                mm->value = INT64_MIN;      /* two's complement of 2^63 */
                prev = mm->value;
                prev_big = true;
            } else {
                mm->value = prev + 1;
                prev = mm->value;
            }
        }
        if (!member_representable(mm, p)) {
            emit_t0301(c, m, mm, p);
            if (c->oom) {
                free(out->members);
                memset(out, 0, sizeof(*out));
                return LAYOUT_OOM;
            }
        }
    }
    out->underlying.size = p->size_bytes;
    out->underlying.align = p->align_bytes;
    out->size = p->size_bytes;
    out->align = p->align_bytes;
    out->nmembers = n;
    if (c->unevaluable != was_unevaluable) {
        /* This enum contains a member-value expression outside the 11b
         * subset; its layout is partial and must not be published. */
        free(out->members);
        memset(out, 0, sizeof(*out));
        return LAYOUT_UNEVALUABLE;
    }
    return LAYOUT_OK;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

/* The module-scope type symbol (struct/enum) whose declaration node is
 * `decl`, or NULL. */
static const NameSymbol *type_symbol_for_decl(const NameModule *module,
                                              const AstNode *decl,
                                              NameSymbolKind kind)
{
    size_t i;
    if (!module) return NULL;
    for (i = 0; i < module->nmodule_scope; i++) {
        const NameSymbol *s = module->module_scope[i];
        if (s->kind == kind && s->decl == decl) return s;
    }
    return NULL;
}

LayoutStatus types_layout_build(const NameResult *result,
                                LayoutBuild **out_build,
                                DiagRecord ***out_records,
                                size_t *out_record_count)
{
    LayoutCtx c;
    LayoutBuild *b;
    LayoutStatus worst = LAYOUT_OK;
    size_t m;

    memset(&c, 0, sizeof(c));
    if (out_build) *out_build = NULL;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    if (!result) return LAYOUT_OK;

    b = (LayoutBuild *)calloc(1, sizeof(LayoutBuild));
    if (!b) return LAYOUT_OOM;
    c.build = b;

    /* Phase 1: enums. Enums are complete immediately after their
     * declaration (spec sec. 7.6) and their member values depend only on
     * their own members, so every enum can be laid out up front; structs
     * referencing an enum declared anywhere in the build resolve through
     * phase 1. Deterministic order: modules in result order (entry first,
     * imports depth-first), declarations in source order. */
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *module = result->modules[m];
        const AstNode *program = module->program;
        size_t i;
        if (!program) continue;
        for (i = 0; i < program->u.program.ndecls; i++) {
            const AstNode *decl = program->u.program.decls[i];
            if (decl->kind != AST_ENUM_DECL) continue;
            {
                const NameSymbol *sym = type_symbol_for_decl(
                    module, decl, NAME_SYM_ENUM);
                LayoutEnum le;
                LayoutStatus st;
                if (!sym) goto unsupported;
                if (layout_build_enum(b, sym)) continue;  /* defensive */
                st = compute_enum(&c, module, decl, &le);
                if (st == LAYOUT_UNEVALUABLE) {
                    /* Skip the layout (not published), but record the
                     * skip so a by-value reference from a struct routes
                     * LAYOUT_UNEVALUABLE instead of UNSUPPORTED. */
                    if (!build_mark_enum_unevaluable(b, sym)) goto oom;
                    worst = LAYOUT_UNEVALUABLE;
                    continue;
                }
                if (st != LAYOUT_OK) {
                    if (st == LAYOUT_OOM) goto oom;
                    goto unsupported;
                }
                if (!build_push_enum(b, sym, &le)) {
                    free(le.members);
                    goto oom;
                }
            }
        }
    }
    if (c.oom) goto oom;

    /* Phase 2: structs. By-value struct references are only to structs
     * already closed in valid builds (completeness rejects forming a
     * field of an incomplete struct), so the table lookup in type_info
     * always hits; enum fields resolve through phase 1. */
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *module = result->modules[m];
        const AstNode *program = module->program;
        size_t i;
        if (!program) continue;
        for (i = 0; i < program->u.program.ndecls; i++) {
            const AstNode *decl = program->u.program.decls[i];
            if (decl->kind != AST_STRUCT_DECL) continue;
            {
                const NameSymbol *sym = type_symbol_for_decl(
                    module, decl, NAME_SYM_STRUCT);
                LayoutStruct ls;
                LayoutStatus st;
                if (!sym) goto unsupported;
                if (layout_build_struct(b, sym)) continue;  /* defensive */
                st = compute_struct(&c, module, decl, &ls);
                if (st == LAYOUT_UNEVALUABLE) {
                    /* Same routing bookkeeping as the enum phase: the
                     * struct's own layout is skipped and recorded, so a
                     * later struct referencing it by value routes
                     * LAYOUT_UNEVALUABLE (MIN-11B-01). */
                    if (!build_mark_struct_unevaluable(b, sym)) goto oom;
                    worst = LAYOUT_UNEVALUABLE;
                    continue;
                }
                if (st != LAYOUT_OK) {
                    if (st == LAYOUT_OOM) goto oom;
                    goto unsupported;
                }
                if (!build_push_struct(b, sym, &ls)) {
                    free(ls.fields);
                    goto oom;
                }
            }
        }
    }
    if (c.oom) goto oom;

    if (c.nrecords > 0) {
        diag_sort_records(c.records, c.nrecords);
    }
    if (out_build) *out_build = b;
    if (c.nrecords > 0) {
        if (out_records) *out_records = c.records;
        if (out_record_count) *out_record_count = c.nrecords;
    }
    if (worst == LAYOUT_UNEVALUABLE) return LAYOUT_UNEVALUABLE;
    return c.nrecords > 0 ? LAYOUT_DIAG_ERROR : LAYOUT_OK;

unsupported:
    for (m = 0; m < c.nrecords; m++) diag_record_free(c.records[m]);
    free(c.records);
    layout_build_free(b);
    return LAYOUT_UNSUPPORTED;

oom:
    for (m = 0; m < c.nrecords; m++) diag_record_free(c.records[m]);
    free(c.records);
    layout_build_free(b);
    return LAYOUT_OOM;
}
