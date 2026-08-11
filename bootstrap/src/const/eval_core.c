/* bootstrap/src/const/eval_core.c
 *
 * AI-Co Stage-0 constant-expression evaluator core (WP-M0-12a).
 * See eval_core.h for the API contract and documented conventions.
 */
#include "eval_core.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

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

static bool type_is_prim_int(const Type *t)
{
    const TypePrimInfo *p = prim_of(t);
    return p != NULL && p->is_integer;
}

static bool type_is_bool(const Type *t)
{
    return t && t->kind == TYPE_PRIM && t->u.prim == AST_PRIM_BOOL;
}

/* Clone a type graph (fresh ownership). NULL on OOM (nothing owned). */
static Type *type_clone(const Type *t)
{
    Type *e, *r;
    if (!t) return NULL;
    switch (t->kind) {
    case TYPE_PRIM:
        return type_prim_new(t->u.prim);
    case TYPE_STRUCT:
        return type_struct_new(t->u.sym);
    case TYPE_ENUM:
        return type_enum_new(t->u.sym);
    case TYPE_PTR:
        e = t->u.ptr.elem ? type_clone(t->u.ptr.elem) : NULL;
        if (t->u.ptr.elem && !e) return NULL;
        r = type_ptr_new(e);
        if (!r) { type_free(e); return NULL; }
        return r;
    case TYPE_SLICE:
        e = t->u.slice.elem ? type_clone(t->u.slice.elem) : NULL;
        if (t->u.slice.elem && !e) return NULL;
        r = type_slice_new(e);
        if (!r) { type_free(e); return NULL; }
        return r;
    case TYPE_ARRAY:
        e = t->u.array.elem ? type_clone(t->u.array.elem) : NULL;
        if (t->u.array.elem && !e) return NULL;
        r = type_array_new(e, t->u.array.extent);
        if (!r) { type_free(e); return NULL; }
        return r;
    default:
        return NULL;
    }
}

/* The integer type description of a Type: width + signedness. Returns
 * false when the Type is not an integer primitive. */
static bool int_ty_of(const Type *t, int *width, bool *is_signed)
{
    const TypePrimInfo *p = prim_of(t);
    if (!p || !p->is_integer) return false;
    *width = p->width_bits;
    *is_signed = p->is_signed;
    return true;
}

/* The mathematical value of an unsigned-typed value as uint64 (valid
 * only for values of an unsigned type; negative values never occur). */
static uint64_t u64_of(const EvalInt *v)
{
    return (uint64_t)v->v;
}

/* Is the mathematical value representable in the target primitive? */
static bool int_fits(const EvalInt *v, const TypePrimInfo *t)
{
    uint64_t raw;
    int w = t->width_bits;
    if (t->is_signed) {
        if (v->big) return false;          /* >= 2^63 never fits a signed type */
        if (w >= 64) return true;          /* any int64 fits i64/isize */
        return v->v >= -(int64_t)((uint64_t)1 << (w - 1)) &&
               v->v <=  (int64_t)((uint64_t)1 << (w - 1)) - 1;
    }
    raw = (uint64_t)v->v;
    if (!v->big && v->v < 0) return false;
    if (w >= 64) return true;
    return raw <= ((uint64_t)1 << w) - 1;
}

/* The low `width` bits of the value's two's-complement pattern. */
static uint64_t int_raw(const EvalInt *v, int width)
{
    uint64_t raw = (uint64_t)v->v;
    if (width >= 64) return raw;
    return raw & (((uint64_t)1 << width) - 1);
}

/* Interpret a w-bit pattern as the target type. */
static EvalInt int_from_raw(uint64_t raw, int width, bool is_signed)
{
    EvalInt r;
    uint64_t mask = width >= 64 ? UINT64_MAX : (((uint64_t)1 << width) - 1);
    if (!is_signed) {
        uint64_t val = raw & mask;
        if (val >= ((uint64_t)1 << 63)) {
            r.v = (int64_t)val;
            r.big = true;
        } else {
            r.v = (int64_t)val;
            r.big = false;
        }
        return r;
    }
    if (raw & (((uint64_t)1) << (width - 1))) {
        /* negative: sign-extend the w-bit pattern */
        r.v = (int64_t)(raw | ~mask);
        r.big = false;
    } else {
        r.v = (int64_t)(raw & mask);
        r.big = false;
    }
    return r;
}

/* The integer value of an integer literal (spec sec. 4.3). */
static void int_from_literal(const AstNode *lit, EvalInt *out)
{
    LexIntType t = lit->u.int_literal.type;
    uint64_t mag = lit->u.int_literal.value;
    const TypePrimInfo *p = types_prim_info(prim_from_lex(t));
    int w = p ? p->width_bits : 32;
    if (lit->u.int_literal.is_min) {
        /* denotes -(2^(width-1)) (the grammar production "-" literal) */
        if (w >= 64) {
            out->v = INT64_MIN;
        } else {
            out->v = -(int64_t)((uint64_t)1 << (w - 1));
        }
        out->big = false;
        return;
    }
    if (p && !p->is_signed) {
        /* unsigned literal: magnitude up to 2^64-1 */
        if (mag > (uint64_t)INT64_MAX) {
            out->v = (int64_t)mag;
            out->big = true;
        } else {
            out->v = (int64_t)mag;
            out->big = false;
        }
    } else {
        out->v = (int64_t)mag;
        out->big = false;
    }
}

/* Mathematical comparison of two integer values (-1, 0, 1). */
static int int_cmp(const EvalInt *a, const EvalInt *b)
{
    if (a->big && b->big) {
        uint64_t av = (uint64_t)a->v, bv = (uint64_t)b->v;
        return av < bv ? -1 : (av > bv ? 1 : 0);
    }
    if (a->big) return 1;      /* >= 2^63 always above any int64 */
    if (b->big) return -1;
    return a->v < b->v ? -1 : (a->v > b->v ? 1 : 0);
}

/* Well-defined int64 add/sub/mul with overflow detection (C17 does not
 * define signed overflow). Same conventions as the WP-M0-11b subset
 * (layout.c). */
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

/* Arithmetic right shift, defined for negative values. */
static int64_t i64_ashr(int64_t a, int b)
{
    if (a >= 0) return a >> b;
    return ~(~a >> b);
}

/* Forward declaration (defined in the core section below; used by the
 * size/type helpers above it). */
EvalStatus const_eval_expr(EvalCtx *ctx, const AstNode *expr,
                           EvalValue *out, EvalFailure *out_failure);

/* ---------------------------------------------------------------------------
 * Context + value ownership
 * ------------------------------------------------------------------------- */

void eval_ctx_init(EvalCtx *ctx, const NameResult *result,
                   const LayoutBuild *layout, const NameModule *module)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->result = result;
    ctx->layout = layout;
    ctx->module = module;
}

void eval_ctx_cleanup(EvalCtx *ctx)
{
    if (!ctx) return;
    free((void *)ctx->in_progress);
    ctx->in_progress = NULL;
    ctx->n_in_progress = ctx->in_progress_cap = 0;
}

void eval_value_free(EvalValue *v)
{
    size_t i;
    if (!v) return;
    if (v->kind == EVAL_VAL_ARRAY) {
        for (i = 0; i < v->u.array.nelems; i++) eval_value_free(&v->u.array.elems[i]);
        free(v->u.array.elems);
    } else if (v->kind == EVAL_VAL_STRUCT) {
        for (i = 0; i < v->u.st.nfields; i++) eval_value_free(&v->u.st.fields[i]);
        free(v->u.st.fields);
    }
    type_free(v->type);
    memset(v, 0, sizeof(*v));
}

static bool ctx_push_const(EvalCtx *ctx, const NameSymbol *sym)
{
    const NameSymbol **ns;
    if (ctx->n_in_progress == ctx->in_progress_cap) {
        size_t ncap = ctx->in_progress_cap ? ctx->in_progress_cap * 2 : 8;
        ns = (const NameSymbol **)realloc((void *)ctx->in_progress,
                                          ncap * sizeof(const NameSymbol *));
        if (!ns) { ctx->oom = true; return false; }
        ctx->in_progress = ns;
        ctx->in_progress_cap = ncap;
    }
    ctx->in_progress[ctx->n_in_progress++] = sym;
    return true;
}

static bool ctx_const_in_progress(const EvalCtx *ctx, const NameSymbol *sym)
{
    size_t i;
    for (i = 0; i < ctx->n_in_progress; i++) {
        if (ctx->in_progress[i] == sym) return true;
    }
    return false;
}

static void ctx_pop_const(EvalCtx *ctx)
{
    if (ctx->n_in_progress) ctx->n_in_progress--;
}

/* ---------------------------------------------------------------------------
 * Value construction
 * ------------------------------------------------------------------------- */

static EvalValue val_int(EvalInt i, Type *t)
{
    EvalValue v;
    memset(&v, 0, sizeof(v));
    v.kind = EVAL_VAL_INT;
    v.type = t;
    v.u.i = i;
    return v;
}

static EvalValue val_bool(bool b, Type *t)
{
    EvalValue v;
    memset(&v, 0, sizeof(v));
    v.kind = EVAL_VAL_BOOL;
    v.type = t;
    v.u.b = b;
    return v;
}

static EvalValue val_str(const char *bytes, size_t len, Type *t)
{
    EvalValue v;
    memset(&v, 0, sizeof(v));
    v.kind = EVAL_VAL_STR;
    v.type = t;
    v.u.str.bytes = bytes;
    v.u.str.len = len;
    return v;
}

static EvalValue val_null(void)
{
    EvalValue v;
    memset(&v, 0, sizeof(v));
    v.kind = EVAL_VAL_NULL;
    return v;
}

static EvalValue val_addr(const NameSymbol *sym, int64_t byte_offset, Type *t)
{
    EvalValue v;
    memset(&v, 0, sizeof(v));
    v.kind = EVAL_VAL_ADDR;
    v.type = t;
    v.u.addr.sym = sym;
    v.u.addr.byte_offset = byte_offset;
    return v;
}

static EvalValue val_slice(const NameSymbol *sym, int64_t lo, int64_t hi, Type *t)
{
    EvalValue v;
    memset(&v, 0, sizeof(v));
    v.kind = EVAL_VAL_SLICE;
    v.type = t;
    v.u.slice.sym = sym;
    v.u.slice.lo = lo;
    v.u.slice.hi = hi;
    return v;
}

/* ---------------------------------------------------------------------------
 * Size/alignment
 * ------------------------------------------------------------------------- */

static EvalStatus size_align_of_type(EvalCtx *ctx, const Type *t,
                                     int64_t *size, int64_t *align,
                                     EvalFailure *fail);

/* Byte offset is a UTF-8 code point boundary iff it is 0 or len, or the
 * byte there is not a continuation byte (validated UTF-8 input). */
static bool utf8_boundary(const char *bytes, size_t len, int64_t off)
{
    unsigned char c;
    if (off == 0) return true;
    if (off == (int64_t)len) return true;
    if (off < 0 || off > (int64_t)len) return false;
    c = (unsigned char)bytes[off];
    return (c & 0xC0) != 0x80;
}

/* Size/alignment of an AST type node. Array extents are evaluated with
 * full composition (the WP-M0-12 superset of the 11b subset), so
 * sizeof of an array whose extent was outside the 11b subset still
 * evaluates. Returns EVAL_NOT_CONST when an extent is not a constant
 * integer expression (a non-const array extent makes the type invalid
 * and therefore the sizeof/alignof not a constant expression), and
 * EVAL_UNSUPPORTED for missing named layouts (defensive). */
static EvalStatus size_align_of_type_node(EvalCtx *ctx,
                                          const NameModule *module,
                                          const AstNode *tn,
                                          int64_t *size, int64_t *align,
                                          EvalFailure *fail)
{
    if (fail) *fail = EVAL_FAIL_NONE;
    if (!tn || !module) return EVAL_UNSUPPORTED;
    switch (tn->kind) {
    case AST_TYPE_PRIM: {
        const TypePrimInfo *p = types_prim_info(tn->u.type_prim.prim);
        if (!p || p->size_bytes <= 0) return EVAL_UNSUPPORTED;
        if (size) *size = p->size_bytes;
        if (align) *align = p->align_bytes;
        return EVAL_OK;
    }
    case AST_TYPE_PTR:
        if (size) *size = 8;
        if (align) *align = 8;
        return EVAL_OK;
    case AST_TYPE_SLICE:
        if (size) *size = 16;
        if (align) *align = 8;
        return EVAL_OK;
    case AST_TYPE_ARRAY: {
        EvalValue ev;
        int64_t esize, ealign, extent;
        EvalStatus st;
        const NameModule *saved;
        if (!tn->u.type_derived.len) return EVAL_UNSUPPORTED;
        st = size_align_of_type_node(ctx, module, tn->u.type_derived.base,
                                     &esize, &ealign, fail);
        if (st != EVAL_OK) return st;
        saved = ctx->module;
        ctx->module = module;
        st = const_eval_expr(ctx, tn->u.type_derived.len, &ev, fail);
        ctx->module = saved;
        if (st == EVAL_NOT_CONST) return EVAL_NOT_CONST;
        if (st != EVAL_OK) return st;
        if (ev.kind != EVAL_VAL_INT || ev.u.i.big || ev.u.i.v < 0) {
            eval_value_free(&ev);
            return EVAL_NOT_CONST;
        }
        extent = ev.u.i.v;
        eval_value_free(&ev);
        if (size) {
            if (extent != 0 && i64_mul_ovf(extent, esize, size)) {
                if (fail) *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            if (extent == 0) *size = 0;
        }
        if (align) *align = ealign;
        return EVAL_OK;
    }
    case AST_TYPE_NAMED: {
        const NameSymbol *sym = name_symbol_for_node(module, tn);
        if (!sym) return EVAL_UNSUPPORTED;
        if (sym->kind == NAME_SYM_STRUCT) {
            const LayoutStruct *ls = layout_build_struct(ctx->layout, sym);
            if (!ls) return EVAL_UNSUPPORTED;
            if (size) *size = ls->size;
            if (align) *align = ls->align;
            return EVAL_OK;
        }
        if (sym->kind == NAME_SYM_ENUM) {
            const LayoutEnum *le = layout_build_enum(ctx->layout, sym);
            if (!le) return EVAL_UNSUPPORTED;
            if (size) *size = le->size;
            if (align) *align = le->align;
            return EVAL_OK;
        }
        return EVAL_UNSUPPORTED;
    }
    default:
        return EVAL_UNSUPPORTED;
    }
}

static EvalStatus size_align_of_type(EvalCtx *ctx, const Type *t,
                                     int64_t *size, int64_t *align,
                                     EvalFailure *fail)
{
    if (fail) *fail = EVAL_FAIL_NONE;
    if (!t) return EVAL_UNSUPPORTED;
    switch (t->kind) {
    case TYPE_PRIM: {
        const TypePrimInfo *p = prim_of(t);
        if (!p || p->size_bytes <= 0) return EVAL_UNSUPPORTED;
        if (size) *size = p->size_bytes;
        if (align) *align = p->align_bytes;
        return EVAL_OK;
    }
    case TYPE_PTR:
        if (size) *size = 8;
        if (align) *align = 8;
        return EVAL_OK;
    case TYPE_SLICE:
        if (size) *size = 16;
        if (align) *align = 8;
        return EVAL_OK;
    case TYPE_ARRAY: {
        int64_t esize, ealign;
        EvalStatus st = size_align_of_type(ctx, t->u.array.elem,
                                           &esize, &ealign, fail);
        if (st != EVAL_OK) return st;
        if (size) {
            if (t->u.array.extent != 0 &&
                i64_mul_ovf(t->u.array.extent, esize, size)) {
                if (fail) *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            if (t->u.array.extent == 0) *size = 0;
        }
        if (align) *align = ealign;
        return EVAL_OK;
    }
    case TYPE_STRUCT: {
        const LayoutStruct *ls = layout_build_struct(ctx->layout, t->u.sym);
        if (!ls) return EVAL_UNSUPPORTED;
        if (size) *size = ls->size;
        if (align) *align = ls->align;
        return EVAL_OK;
    }
    case TYPE_ENUM: {
        const LayoutEnum *le = layout_build_enum(ctx->layout, t->u.sym);
        if (!le) return EVAL_UNSUPPORTED;
        if (size) *size = le->size;
        if (align) *align = le->align;
        return EVAL_OK;
    }
    default:
        return EVAL_UNSUPPORTED;
    }
}

/* ---------------------------------------------------------------------------
 * Bounded expression typer (for sizeof(expr) operands and type helpers)
 * ------------------------------------------------------------------------- */

typedef enum TyperStatus {
    TYP_OK = 0,
    TYP_UNKNOWN,     /* form outside the subset / untypeable; no record */
    TYP_OOM
} TyperStatus;

static TyperStatus type_from_type_node(EvalCtx *ctx, const NameModule *module,
                                       const AstNode *tn, Type **out)
{
    Type *e;
    *out = NULL;
    if (!tn || !module) return TYP_UNKNOWN;
    switch (tn->kind) {
    case AST_TYPE_PRIM:
        *out = type_prim_new(tn->u.type_prim.prim);
        break;
    case AST_TYPE_NAMED: {
        const NameSymbol *sym = name_symbol_for_node(module, tn);
        if (!sym) return TYP_UNKNOWN;
        if (sym->kind == NAME_SYM_STRUCT) {
            *out = type_struct_new(sym);
        } else if (sym->kind == NAME_SYM_ENUM) {
            *out = type_enum_new(sym);
        } else {
            return TYP_UNKNOWN;
        }
        break;
    }
    case AST_TYPE_PTR:
        e = NULL;
        {
            TyperStatus st = type_from_type_node(ctx, module,
                                                 tn->u.type_derived.base, &e);
            if (st != TYP_OK) return st;
        }
        *out = type_ptr_new(e);
        if (!*out) { type_free(e); return TYP_OOM; }
        break;
    case AST_TYPE_SLICE:
        e = NULL;
        {
            TyperStatus st = type_from_type_node(ctx, module,
                                                 tn->u.type_derived.base, &e);
            if (st != TYP_OK) return st;
        }
        *out = type_slice_new(e);
        if (!*out) { type_free(e); return TYP_OOM; }
        break;
    case AST_TYPE_ARRAY: {
        EvalValue ev;
        EvalFailure fail = EVAL_FAIL_NONE;
        TyperStatus st = type_from_type_node(ctx, module,
                                             tn->u.type_derived.base, &e);
        EvalStatus est;
        const NameModule *saved;
        if (st != TYP_OK) return st;
        if (!tn->u.type_derived.len) { type_free(e); return TYP_UNKNOWN; }
        saved = ctx->module;
        ctx->module = module;
        est = const_eval_expr(ctx, tn->u.type_derived.len, &ev, &fail);
        ctx->module = saved;
        if (est != EVAL_OK || ev.kind != EVAL_VAL_INT ||
            ev.u.i.big || ev.u.i.v < 0) {
            eval_value_free(&ev);
            type_free(e);
            return TYP_UNKNOWN;
        }
        *out = type_array_new(e, ev.u.i.v);
        eval_value_free(&ev);
        if (!*out) { type_free(e); return TYP_OOM; }
        break;
    }
    default:
        return TYP_UNKNOWN;
    }
    if (!*out) return TYP_OOM;
    return TYP_OK;
}

static TyperStatus type_of_symbol(EvalCtx *ctx, const NameSymbol *sym,
                                  Type **out)
{
    *out = NULL;
    if (!sym || !sym->decl) return TYP_UNKNOWN;
    switch (sym->kind) {
    case NAME_SYM_GLOBAL_VAR:
    case NAME_SYM_GLOBAL_CONST:
        return type_from_type_node(ctx, sym->module,
                                   sym->decl->u.global_decl.type, out);
    case NAME_SYM_LOCAL_VAR:
    case NAME_SYM_LOCAL_CONST:
        return type_from_type_node(ctx, sym->module,
                                   sym->decl->u.local_decl.type, out);
    case NAME_SYM_PARAM:
    case NAME_SYM_FIELD:
        return type_from_type_node(ctx, sym->module,
                                   sym->decl->u.named.type, out);
    case NAME_SYM_STRUCT:
        *out = type_struct_new(sym);
        return *out ? TYP_OK : TYP_OOM;
    case NAME_SYM_ENUM:
        *out = type_enum_new(sym);
        return *out ? TYP_OK : TYP_OOM;
    case NAME_SYM_ENUM_MEMBER:
        *out = type_enum_new(sym->owner);
        return *out ? TYP_OK : TYP_OOM;
    default:
        return TYP_UNKNOWN;
    }
}

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

static TyperStatus type_of_expr(EvalCtx *ctx, const NameModule *module,
                                const AstNode *e, Type **out, bool *out_is_null);

/* Result type of a binary operator over applicable operands (spec
 * sec. 10.2): integer arithmetic/bitwise -> common type; shifts -> left
 * type; comparisons/logical -> bool; pointer arithmetic -> pointer or
 * isize. Returns TYP_UNKNOWN when an integer pair has no common type
 * (the 11c record already exists) or the operands were not typable. */
static TyperStatus binary_result_type(EvalCtx *ctx, AstBinaryOp op,
                                      const Type *lt, const Type *rt,
                                      Type **out)
{
    (void)ctx;
    *out = NULL;
    if (!lt || !rt) return TYP_UNKNOWN;
    if (op == AST_BIN_LAND || op == AST_BIN_LOR ||
        op == AST_BIN_LT || op == AST_BIN_LE || op == AST_BIN_GT ||
        op == AST_BIN_GE || op == AST_BIN_EQ || op == AST_BIN_NE) {
        *out = type_prim_new(AST_PRIM_BOOL);
        return *out ? TYP_OK : TYP_OOM;
    }
    if (op == AST_BIN_SHL || op == AST_BIN_SHR) {
        if (!type_is_prim_int(lt)) return TYP_UNKNOWN;
        *out = type_clone(lt);
        return *out ? TYP_OK : TYP_OOM;
    }
    if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
        if (lt->kind == TYPE_PTR && type_is_prim_int(rt)) {
            *out = type_clone(lt);
            return *out ? TYP_OK : TYP_OOM;
        }
        if (op == AST_BIN_ADD && rt->kind == TYPE_PTR &&
            type_is_prim_int(lt)) {
            *out = type_clone(rt);
            return *out ? TYP_OK : TYP_OOM;
        }
        if (op == AST_BIN_SUB && lt->kind == TYPE_PTR &&
            rt->kind == TYPE_PTR) {
            *out = type_prim_new(AST_PRIM_ISIZE);
            return *out ? TYP_OK : TYP_OOM;
        }
    }
    if (type_is_prim_int(lt) && type_is_prim_int(rt)) {
        *out = convert_common_type(lt, rt);
        return *out ? TYP_OK : TYP_UNKNOWN;
    }
    return TYP_UNKNOWN;
}

static TyperStatus type_of_expr(EvalCtx *ctx, const NameModule *module,
                                const AstNode *e, Type **out, bool *out_is_null)
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
        if (!sym) return TYP_UNKNOWN;
        return type_of_symbol(ctx, sym, out);
    }
    case AST_EXPR_PAREN:
        return type_of_expr(ctx, module, e->u.paren.expr, out, out_is_null);
    case AST_EXPR_UNARY: {
        Type *ot = NULL;
        bool onull = false;
        TyperStatus ost = type_of_expr(ctx, module, e->u.unary.operand,
                                       &ot, &onull);
        Type *r = NULL;
        if (ost != TYP_OK) { type_free(ot); return TYP_UNKNOWN; }
        if (onull) { type_free(ot); return TYP_UNKNOWN; }
        switch (e->u.unary.op) {
        case AST_UN_PLUS:
        case AST_UN_NEG:
        case AST_UN_BNOT:
            if (type_is_prim_int(ot)) r = type_clone(ot);
            break;
        case AST_UN_NOT:
            if (type_is_bool(ot)) r = type_prim_new(AST_PRIM_BOOL);
            break;
        case AST_UN_DEREF:
            if (ot && ot->kind == TYPE_PTR) r = type_clone(ot->u.ptr.elem);
            break;
        case AST_UN_ADDR:
            if (ot) r = type_ptr_new(type_clone(ot));
            break;
        default:
            break;
        }
        type_free(ot);
        if (!r) return TYP_UNKNOWN;
        *out = r;
        return TYP_OK;
    }
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
    case AST_EXPR_LEN:
        *out = type_prim_new(AST_PRIM_USIZE);
        break;
    case AST_EXPR_PTR: {
        Type *ot = NULL;
        bool onull = false;
        TyperStatus ost = type_of_expr(ctx, module, e->u.size_op.operand,
                                       &ot, &onull);
        Type *r = NULL;
        if (ost != TYP_OK || onull) { type_free(ot); return TYP_UNKNOWN; }
        if (ot->kind == TYPE_PRIM && ot->u.prim == AST_PRIM_STR) {
            r = type_ptr_new(type_prim_new(AST_PRIM_U8));
        } else if (ot->kind == TYPE_ARRAY || ot->kind == TYPE_SLICE) {
            const Type *elem = ot->kind == TYPE_ARRAY ? ot->u.array.elem
                                                      : ot->u.slice.elem;
            r = type_ptr_new(type_clone(elem));
        }
        type_free(ot);
        if (!r) return TYP_UNKNOWN;
        *out = r;
        return TYP_OK;
    }
    case AST_EXPR_INDEX: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr(ctx, module, e->u.index_slice.base,
                                       &bt, &bnull);
        Type *r = NULL;
        if (bst != TYP_OK || bnull) { type_free(bt); return TYP_UNKNOWN; }
        if (bt->kind == TYPE_ARRAY) {
            r = type_clone(bt->u.array.elem);
        } else if (bt->kind == TYPE_SLICE) {
            r = type_clone(bt->u.slice.elem);
        } else if (bt->kind == TYPE_PRIM && bt->u.prim == AST_PRIM_STR) {
            r = type_prim_new(AST_PRIM_U8);
        }
        type_free(bt);
        if (!r) return TYP_UNKNOWN;
        *out = r;
        return TYP_OK;
    }
    case AST_EXPR_SLICE: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr(ctx, module, e->u.index_slice.base,
                                       &bt, &bnull);
        Type *r = NULL;
        if (bst != TYP_OK || bnull) { type_free(bt); return TYP_UNKNOWN; }
        if (bt->kind == TYPE_PRIM && bt->u.prim == AST_PRIM_STR) {
            r = type_clone(bt);
        } else if (bt->kind == TYPE_ARRAY || bt->kind == TYPE_SLICE) {
            const Type *elem = bt->kind == TYPE_ARRAY ? bt->u.array.elem
                                                      : bt->u.slice.elem;
            r = type_slice_new(type_clone(elem));
        }
        type_free(bt);
        if (!r) return TYP_UNKNOWN;
        *out = r;
        return TYP_OK;
    }
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP: {
        Type *tt = NULL;
        TyperStatus tst = type_from_type_node(ctx, module,
                                              e->u.cast_wrap.type, &tt);
        if (tst != TYP_OK) return TYP_UNKNOWN;
        *out = tt;
        return TYP_OK;
    }
    case AST_EXPR_BINARY: {
        Type *lt = NULL, *rt = NULL;
        bool lnull = false, rnull = false;
        TyperStatus lst = type_of_expr(ctx, module, e->u.binary.lhs,
                                       &lt, &lnull);
        TyperStatus rst = type_of_expr(ctx, module, e->u.binary.rhs,
                                       &rt, &rnull);
        TyperStatus bst;
        if (lst == TYP_OOM || rst == TYP_OOM) {
            type_free(lt);
            type_free(rt);
            return TYP_OOM;
        }
        if (lst != TYP_OK || rst != TYP_OK) {
            type_free(lt);
            type_free(rt);
            return TYP_UNKNOWN;
        }
        bst = binary_result_type(ctx, e->u.binary.op, lt, rt, out);
        type_free(lt);
        type_free(rt);
        return bst;
    }
    case AST_EXPR_TERNARY: {
        Type *tt = NULL, *et = NULL;
        bool tnull = false, enull = false;
        TyperStatus tst = type_of_expr(ctx, module, e->u.branch.then,
                                       &tt, &tnull);
        TyperStatus est = type_of_expr(ctx, module, e->u.branch.els,
                                       &et, &enull);
        if (tst == TYP_OOM || est == TYP_OOM) {
            type_free(tt);
            type_free(et);
            return TYP_OOM;
        }
        if (tst != TYP_OK || est != TYP_OK) {
            type_free(tt);
            type_free(et);
            return TYP_UNKNOWN;
        }
        if (!tnull && !enull) {
            if (type_is_prim_int(tt) && type_is_prim_int(et)) {
                *out = convert_common_type(tt, et);
                type_free(tt);
                type_free(et);
                return *out ? TYP_OK : TYP_UNKNOWN;
            }
            if (type_identical(tt, et)) {
                *out = type_clone(tt);
                type_free(tt);
                type_free(et);
                return *out ? TYP_OK : TYP_OOM;
            }
        } else if (tnull && et && et->kind == TYPE_PTR) {
            *out = type_clone(et);
            type_free(tt);
            type_free(et);
            return *out ? TYP_OK : TYP_OOM;
        } else if (enull && tt && tt->kind == TYPE_PTR) {
            *out = type_clone(tt);
            type_free(tt);
            type_free(et);
            return *out ? TYP_OK : TYP_OOM;
        }
        type_free(tt);
        type_free(et);
        return TYP_UNKNOWN;
    }
    case AST_EXPR_MEMBER: {
        const NameSymbol *sym = name_symbol_for_node(module, e);
        if (sym && (sym->kind == NAME_SYM_ENUM_MEMBER ||
                    sym->kind == NAME_SYM_GLOBAL_CONST ||
                    sym->kind == NAME_SYM_LOCAL_CONST)) {
            return type_of_symbol(ctx, sym, out);
        }
        /* struct field access: type the base, find the field */
        {
            Type *bt = NULL;
            bool bnull = false;
            TyperStatus bst = type_of_expr(ctx, module, e->u.member.base,
                                           &bt, &bnull);
            const AstNode *f;
            if (bst != TYP_OK || bnull) { type_free(bt); return TYP_UNKNOWN; }
            f = struct_field_decl(bt, e->u.member.name);
            type_free(bt);
            if (!f) return TYP_UNKNOWN;
            return type_from_type_node(ctx, module, f->u.named.type, out);
        }
    }
    case AST_EXPR_ARROW: {
        Type *bt = NULL;
        bool bnull = false;
        TyperStatus bst = type_of_expr(ctx, module, e->u.member.base,
                                       &bt, &bnull);
        const AstNode *f;
        Type *pointee;
        if (bst != TYP_OK || bnull || !bt || bt->kind != TYPE_PTR) {
            type_free(bt);
            return TYP_UNKNOWN;
        }
        pointee = bt->u.ptr.elem;
        f = struct_field_decl(pointee, e->u.member.name);
        type_free(bt);
        if (!f) return TYP_UNKNOWN;
        return type_from_type_node(ctx, module, f->u.named.type, out);
    }
    case AST_EXPR_STRUCT_INIT: {
        const NameSymbol *sym = name_symbol_for_node(ctx->module,
                                                     e->u.struct_init.base);
        if (!sym || sym->kind != NAME_SYM_STRUCT) return TYP_UNKNOWN;
        *out = type_struct_new(sym);
        return *out ? TYP_OK : TYP_OOM;
    }
    case AST_EXPR_CALL: {
        const NameSymbol *sym = NULL;
        if (e->u.call.callee &&
            e->u.call.callee->kind == AST_EXPR_IDENT) {
            sym = name_symbol_for_node(module, e->u.call.callee);
        }
        if (sym && sym->kind == NAME_SYM_FN && sym->decl &&
            sym->decl->u.fn_decl.ret_type) {
            return type_from_type_node(ctx, module,
                                       sym->decl->u.fn_decl.ret_type, out);
        }
        return TYP_UNKNOWN;
    }
    case AST_EXPR_ASSIGN:
        return type_of_expr(ctx, module, e->u.assign.target, out, out_is_null);
    default:
        return TYP_UNKNOWN;
    }
    if (!*out) return TYP_OOM;
    return TYP_OK;
}

/* ---------------------------------------------------------------------------
 * Checked typed integer arithmetic (spec sec. 11.3)
 * ------------------------------------------------------------------------- */

/* Arithmetic over the common type `ty` (width/signed). Operands are the
 * promoted values. Result representability is checked against `ty`
 * (AIC-E0405). Division/remainder by zero -> EVAL_FAIL_DIV_ZERO. The
 * quotient/remainder semantics are C17 truncation toward zero (the
 * initial target's x86 behavior; the language defines no other). */
static EvalStatus int_arith_op(const EvalInt *a, const EvalInt *b,
                               AstBinaryOp op, int width, bool is_signed,
                               EvalInt *out, EvalFailure *fail)
{
    int64_t minv;
    if (fail) *fail = EVAL_FAIL_NONE;
    if (width < 64) {
        int64_t r64;
        uint64_t ur64;
        minv = -(int64_t)((uint64_t)1 << (width - 1));
        switch (op) {
        case AST_BIN_ADD:
            if (is_signed) {
                if (i64_add_ovf(a->v, b->v, &r64) ||
                    r64 < minv || r64 > (int64_t)(((uint64_t)1 << (width - 1)) - 1)) {
                    goto overflow;
                }
                *out = int_from_raw((uint64_t)r64, width, true);
                return EVAL_OK;
            }
            ur64 = u64_of(a) + u64_of(b);
            if (ur64 > ((uint64_t)1 << width) - 1) goto overflow;
            *out = int_from_raw(ur64, width, false);
            return EVAL_OK;
        case AST_BIN_SUB:
            if (is_signed) {
                if (i64_sub_ovf(a->v, b->v, &r64) ||
                    r64 < minv || r64 > (int64_t)(((uint64_t)1 << (width - 1)) - 1)) {
                    goto overflow;
                }
                *out = int_from_raw((uint64_t)r64, width, true);
                return EVAL_OK;
            }
            if (u64_of(b) > u64_of(a)) goto overflow;
            ur64 = u64_of(a) - u64_of(b);
            *out = int_from_raw(ur64, width, false);
            return EVAL_OK;
        case AST_BIN_MUL:
            if (is_signed) {
                if (i64_mul_ovf(a->v, b->v, &r64) ||
                    r64 < minv || r64 > (int64_t)(((uint64_t)1 << (width - 1)) - 1)) {
                    goto overflow;
                }
                *out = int_from_raw((uint64_t)r64, width, true);
                return EVAL_OK;
            }
            ur64 = u64_of(a) * u64_of(b);
            if (ur64 > ((uint64_t)1 << width) - 1) goto overflow;
            *out = int_from_raw(ur64, width, false);
            return EVAL_OK;
        case AST_BIN_DIV:
            if (is_signed) {
                if (b->v == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
                if (a->v == minv && b->v == -1) goto overflow;
                *out = int_from_raw((uint64_t)(a->v / b->v), width, true);
                return EVAL_OK;
            }
            if (u64_of(b) == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
            *out = int_from_raw(u64_of(a) / u64_of(b), width, false);
            return EVAL_OK;
        case AST_BIN_MOD:
            if (is_signed) {
                if (b->v == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
                if (a->v == minv && b->v == -1) goto overflow;
                *out = int_from_raw((uint64_t)(a->v % b->v), width, true);
                return EVAL_OK;
            }
            if (u64_of(b) == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
            *out = int_from_raw(u64_of(a) % u64_of(b), width, false);
            return EVAL_OK;
        case AST_BIN_BAND:
        case AST_BIN_BXOR:
        case AST_BIN_BOR: {
            uint64_t av = int_raw(a, width), bv = int_raw(b, width), rv;
            if (op == AST_BIN_BAND) rv = av & bv;
            else if (op == AST_BIN_BXOR) rv = av ^ bv;
            else rv = av | bv;
            *out = int_from_raw(rv, width, is_signed);
            return EVAL_OK;
        }
        default:
            return EVAL_UNSUPPORTED;
        }
    overflow:
        *fail = EVAL_FAIL_OVERFLOW;
        return EVAL_FAILURE;
    }

    /* width == 64: wrap arithmetic with explicit checks */
    switch (op) {
    case AST_BIN_ADD:
        if (is_signed) {
            int64_t r;
            if (i64_add_ovf(a->v, b->v, &r)) goto overflow64;
            *out = int_from_raw((uint64_t)r, 64, true);
            return EVAL_OK;
        }
        {
            uint64_t av = u64_of(a), bv = u64_of(b), r = av + bv;
            if (r < av) goto overflow64;
            *out = int_from_raw(r, 64, false);
            return EVAL_OK;
        }
    case AST_BIN_SUB:
        if (is_signed) {
            int64_t r;
            if (i64_sub_ovf(a->v, b->v, &r)) goto overflow64;
            *out = int_from_raw((uint64_t)r, 64, true);
            return EVAL_OK;
        }
        {
            uint64_t av = u64_of(a), bv = u64_of(b);
            if (bv > av) goto overflow64;
            *out = int_from_raw(av - bv, 64, false);
            return EVAL_OK;
        }
    case AST_BIN_MUL:
        if (is_signed) {
            int64_t r;
            if (i64_mul_ovf(a->v, b->v, &r)) goto overflow64;
            *out = int_from_raw((uint64_t)r, 64, true);
            return EVAL_OK;
        }
        {
            uint64_t av = u64_of(a), bv = u64_of(b);
            if (bv != 0 && av > UINT64_MAX / bv) goto overflow64;
            *out = int_from_raw(av * bv, 64, false);
            return EVAL_OK;
        }
    case AST_BIN_DIV:
        if (is_signed) {
            if (b->v == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
            if (a->v == INT64_MIN && b->v == -1) goto overflow64;
            *out = int_from_raw((uint64_t)(a->v / b->v), 64, true);
            return EVAL_OK;
        }
        if (u64_of(b) == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
        *out = int_from_raw(u64_of(a) / u64_of(b), 64, false);
        return EVAL_OK;
    case AST_BIN_MOD:
        if (is_signed) {
            if (b->v == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
            if (a->v == INT64_MIN && b->v == -1) goto overflow64;
            *out = int_from_raw((uint64_t)(a->v % b->v), 64, true);
            return EVAL_OK;
        }
        if (u64_of(b) == 0) { *fail = EVAL_FAIL_DIV_ZERO; return EVAL_FAILURE; }
        *out = int_from_raw(u64_of(a) % u64_of(b), 64, false);
        return EVAL_OK;
    case AST_BIN_BAND:
    case AST_BIN_BXOR:
    case AST_BIN_BOR: {
        uint64_t av = int_raw(a, 64), bv = int_raw(b, 64), rv;
        if (op == AST_BIN_BAND) rv = av & bv;
        else if (op == AST_BIN_BXOR) rv = av ^ bv;
        else rv = av | bv;
        *out = int_from_raw(rv, 64, is_signed);
        return EVAL_OK;
    }
    default:
        return EVAL_UNSUPPORTED;
    }
overflow64:
    *fail = EVAL_FAIL_OVERFLOW;
    return EVAL_FAILURE;
}

/* Shift: count must be in [0, width-1] (AIC-E0407); the result is the
 * left operand's pattern shifted, re-read as the left type (sec. 11.3:
 * left shift of signed values is defined on the two's-complement bit
 * pattern; right shift of signed values is arithmetic). */
static EvalStatus int_shift_op(const EvalInt *a, int width, bool is_signed,
                               const EvalInt *count, AstBinaryOp op,
                               EvalInt *out, EvalFailure *fail)
{
    int64_t c;
    uint64_t raw, shifted;
    if (fail) *fail = EVAL_FAIL_NONE;
    if (count->big) { *fail = EVAL_FAIL_SHIFT_RANGE; return EVAL_FAILURE; }
    c = count->v;
    if (c < 0 || c >= width) {
        *fail = EVAL_FAIL_SHIFT_RANGE;
        return EVAL_FAILURE;
    }
    raw = int_raw(a, width);
    if (op == AST_BIN_SHL) {
        shifted = raw << c;
        if (width < 64) shifted &= (((uint64_t)1 << width) - 1);
        *out = int_from_raw(shifted, width, is_signed);
        return EVAL_OK;
    }
    if (is_signed) {
        EvalInt sx = int_from_raw(raw, width, true);
        shifted = (uint64_t)i64_ashr(sx.v, (int)c);
        if (width < 64) shifted &= (((uint64_t)1 << width) - 1);
        *out = int_from_raw(shifted, width, is_signed);
        return EVAL_OK;
    }
    shifted = raw >> c;
    *out = int_from_raw(shifted, width, false);
    return EVAL_OK;
}

/* Unary minus / complement / plus (sec. 10.2). Unsigned negation and
 * `!` on integers are rejected by WP-M0-11d (AIC-T0306); on a valid
 * build they never reach here (defensive EVAL_UNSUPPORTED). */
static EvalStatus int_unary_op(AstUnaryOp op, const EvalInt *a,
                               int width, bool is_signed,
                               EvalInt *out, EvalFailure *fail)
{
    int64_t minv;
    if (fail) *fail = EVAL_FAIL_NONE;
    switch (op) {
    case AST_UN_PLUS:
        *out = *a;
        return EVAL_OK;
    case AST_UN_BNOT: {
        uint64_t raw = int_raw(a, width);
        uint64_t r = ~raw;
        if (width < 64) r &= (((uint64_t)1 << width) - 1);
        *out = int_from_raw(r, width, is_signed);
        return EVAL_OK;
    }
    case AST_UN_NEG:
        if (!is_signed) return EVAL_UNSUPPORTED;
        if (a->big) return EVAL_UNSUPPORTED;
        minv = width >= 64 ? INT64_MIN : -(int64_t)((uint64_t)1 << (width - 1));
        if (a->v == minv) { *fail = EVAL_FAIL_OVERFLOW; return EVAL_FAILURE; }
        *out = int_from_raw((uint64_t)(-a->v), width, true);
        return EVAL_OK;
    default:
        return EVAL_UNSUPPORTED;
    }
}

/* ---------------------------------------------------------------------------
 * Const-name evaluation (module or enclosing scopes; recursion guarded)
 * ------------------------------------------------------------------------- */

static EvalStatus eval_const_initializer(EvalCtx *ctx, const NameSymbol *sym,
                                         EvalValue *out, EvalFailure *fail);

/* Evaluate an identifier (or a module-qualified member chain) that
 * resolved to a const symbol and wrap the value to the declared type
 * (a const's value has the declared type; WP-M0-11c already checked the
 * initializer conversion). */
static EvalStatus eval_const_ref(EvalCtx *ctx, const NameSymbol *sym,
                                 EvalValue *out, EvalFailure *fail)
{
    EvalValue val;
    Type *dt = NULL;
    EvalStatus st;
    if (ctx_const_in_progress(ctx, sym)) {
        /* const cycle (e.g. A = B, B = A): neither is a constant
         * expression (sec. 10.5 requires the value to be computable) */
        return EVAL_NOT_CONST;
    }
    if (!ctx_push_const(ctx, sym)) return EVAL_OOM;
    st = eval_const_initializer(ctx, sym, &val, fail);
    ctx_pop_const(ctx);
    if (st != EVAL_OK) return st;
    if (type_of_symbol(ctx, sym, &dt) != TYP_OK) {
        eval_value_free(&val);
        return EVAL_UNSUPPORTED;
    }
    type_free(val.type);
    val.type = dt;
    *out = val;
    return EVAL_OK;
}

static EvalStatus eval_const_initializer(EvalCtx *ctx, const NameSymbol *sym,
                                         EvalValue *out, EvalFailure *fail)
{
    const AstNode *init;
    const NameModule *saved;
    EvalStatus st;
    if (!sym || !sym->decl) return EVAL_UNSUPPORTED;
    switch (sym->kind) {
    case NAME_SYM_GLOBAL_CONST:
    case NAME_SYM_GLOBAL_VAR:
        init = sym->decl->u.global_decl.init;
        break;
    case NAME_SYM_LOCAL_CONST:
    case NAME_SYM_LOCAL_VAR:
        init = sym->decl->u.local_decl.init;
        break;
    default:
        return EVAL_UNSUPPORTED;
    }
    if (!init) return EVAL_UNSUPPORTED;
    saved = ctx->module;
    ctx->module = sym->module;
    st = const_eval_expr(ctx, init, out, fail);
    ctx->module = saved;
    return st;
}

/* ---------------------------------------------------------------------------
 * Cast / wrap (spec sec. 11.2 / 11.5)
 * ------------------------------------------------------------------------- */

/* wrap<T>: reduce the value modulo 2^width and re-read as the target
 * (never checked, never fails; source is an integer or enum). */
static EvalStatus wrap_into(const EvalInt *v, const TypePrimInfo *tp,
                            EvalInt *out)
{
    if (!tp || !tp->is_integer) return EVAL_UNSUPPORTED;
    *out = int_from_raw(int_raw(v, tp->width_bits),
                        tp->width_bits, tp->is_signed);
    return EVAL_OK;
}

static EvalStatus eval_convert(EvalCtx *ctx, const AstNode *e, bool is_cast,
                               EvalValue *out, EvalFailure *fail)
{
    const AstNode *type_node = e->u.cast_wrap.type;
    const AstNode *src_node = e->u.cast_wrap.expr;
    Type *to = NULL;
    EvalValue val;
    EvalStatus st;
    if (fail) *fail = EVAL_FAIL_NONE;
    if (type_from_type_node(ctx, ctx->module, type_node, &to) != TYP_OK) {
        return EVAL_UNSUPPORTED;
    }
    st = const_eval_expr(ctx, src_node, &val, fail);
    if (st != EVAL_OK) { type_free(to); return st; }

    /* identity: cast<T>(e) where typeof(e) == T is the identity
     * mechanism (optype.h); keep the value, retype */
    if (val.type && type_identical(val.type, to)) {
        type_free(val.type);
        val.type = to;
        *out = val;
        return EVAL_OK;
    }

    if (val.kind == EVAL_VAL_NULL) {
        /* null -> any T* (cast only; wrap never accepts null) */
        if (!is_cast || !to || to->kind != TYPE_PTR) {
            eval_value_free(&val);
            type_free(to);
            return EVAL_UNSUPPORTED;
        }
        eval_value_free(&val);
        *out = val_addr(NULL, 0, to);
        return EVAL_OK;
    }

    if (val.kind == EVAL_VAL_ADDR) {
        /* T* -> U*: bit-preserving; ptr -> integer is link-time
         * (EVAL_NOT_CONST, documented) */
        if (to && to->kind == TYPE_PTR) {
            eval_value_free(&val);
            *out = val_addr(val.u.addr.sym, val.u.addr.byte_offset, to);
            return EVAL_OK;
        }
        if (to && type_is_prim_int(to)) {
            eval_value_free(&val);
            type_free(to);
            return EVAL_NOT_CONST;
        }
        eval_value_free(&val);
        type_free(to);
        return EVAL_UNSUPPORTED;
    }

    if (val.kind == EVAL_VAL_STR || val.kind == EVAL_VAL_SLICE) {
        /* str <-> u8[] casts produce a (data, length) view whose data
         * pointer is link-time; not evaluable at compile time */
        eval_value_free(&val);
        type_free(to);
        return EVAL_NOT_CONST;
    }

    if (val.kind == EVAL_VAL_ARRAY || val.kind == EVAL_VAL_STRUCT) {
        eval_value_free(&val);
        type_free(to);
        return EVAL_UNSUPPORTED;
    }

    if (val.kind == EVAL_VAL_BOOL) {
        const TypePrimInfo *tp = prim_of(to);
        if (!is_cast) {
            eval_value_free(&val);
            type_free(to);
            return EVAL_UNSUPPORTED;
        }
        if (tp && tp->is_integer) {
            /* bool -> integer: false -> 0, true -> 1 */
            EvalInt r = { val.u.b ? 1 : 0, false };
            eval_value_free(&val);
            *out = val_int(r, to);
            return EVAL_OK;
        }
        if (type_is_bool(to)) {
            eval_value_free(&val);
            *out = val_bool(val.u.b, to);
            return EVAL_OK;
        }
        eval_value_free(&val);
        type_free(to);
        return EVAL_UNSUPPORTED;
    }

    if (val.kind == EVAL_VAL_INT) {
        const TypePrimInfo *tp = prim_of(to);
        Type *src_type = val.type;
        if (src_type && src_type->kind == TYPE_ENUM) {
            /* enum source: the value is the underlying integer */
            EvalInt uv = val.u.i;
            if (to->kind == TYPE_ENUM) {
                /* enum -> enum (different): via integer semantics;
                 * the value must equal a declared member of `to` */
                const LayoutEnum *le = layout_build_enum(ctx->layout, to->u.sym);
                size_t i;
                bool found = false;
                if (!le) {
                    eval_value_free(&val);
                    type_free(to);
                    return EVAL_UNSUPPORTED;
                }
                for (i = 0; i < le->nmembers; i++) {
                    if (le->members[i].domain_overflow) continue;
                    if (le->members[i].big_unsigned == uv.big &&
                        le->members[i].value == uv.v) { found = true; break; }
                }
                if (!found) {
                    eval_value_free(&val);
                    type_free(to);
                    *fail = EVAL_FAIL_CAST_RANGE;
                    return EVAL_FAILURE;
                }
                type_free(val.type);
                val.type = to;
                *out = val;
                return EVAL_OK;
            }
            if (!is_cast) {
                /* wrap with an enum source: reduce the underlying value */
                if (tp && tp->is_integer) {
                    EvalInt r;
                    EvalStatus ws = wrap_into(&uv, tp, &r);
                    if (ws != EVAL_OK) {
                        eval_value_free(&val);
                        type_free(to);
                        return ws;
                    }
                    eval_value_free(&val);
                    *out = val_int(r, to);
                    return EVAL_OK;
                }
                eval_value_free(&val);
                type_free(to);
                return EVAL_UNSUPPORTED;
            }
            /* cast enum -> underlying integer: checked identity */
            if (tp && tp->is_integer) {
                if (!int_fits(&uv, tp)) {
                    eval_value_free(&val);
                    type_free(to);
                    *fail = EVAL_FAIL_CAST_RANGE;
                    return EVAL_FAILURE;
                }
                eval_value_free(&val);
                *out = val_int(uv, to);
                return EVAL_OK;
            }
            eval_value_free(&val);
            type_free(to);
            return EVAL_UNSUPPORTED;
        }
        if (!tp) {
            /* non-primitive targets: enum and pointer casts only (the
             * enum-source branch above handled enum sources; wrap never
             * accepts a non-primitive target) */
            if (to->kind == TYPE_ENUM) {
                if (!is_cast) {
                    eval_value_free(&val);
                    type_free(to);
                    return EVAL_UNSUPPORTED;
                }
                /* integer -> enum: value must equal a declared member */
                {
                    const LayoutEnum *le = layout_build_enum(ctx->layout,
                                                             to->u.sym);
                    size_t i;
                    bool found = false;
                    if (!le) {
                        eval_value_free(&val);
                        type_free(to);
                        return EVAL_UNSUPPORTED;
                    }
                    for (i = 0; i < le->nmembers; i++) {
                        if (le->members[i].domain_overflow) continue;
                        if (le->members[i].big_unsigned == val.u.i.big &&
                            le->members[i].value == val.u.i.v) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        eval_value_free(&val);
                        type_free(to);
                        *fail = EVAL_FAIL_CAST_RANGE;
                        return EVAL_FAILURE;
                    }
                    type_free(val.type);
                    val.type = to;
                    *out = val;
                    return EVAL_OK;
                }
            }
            if (to->kind == TYPE_PTR) {
                if (!is_cast) {
                    eval_value_free(&val);
                    type_free(to);
                    return EVAL_UNSUPPORTED;
                }
                /* integer -> pointer: value preserved (a raw address) */
                if (val.u.i.big) {
                    eval_value_free(&val);
                    type_free(to);
                    return EVAL_UNSUPPORTED;
                }
                *out = val_addr(NULL, val.u.i.v, to);
                eval_value_free(&val);
                return EVAL_OK;
            }
            eval_value_free(&val);
            type_free(to);
            return EVAL_UNSUPPORTED;
        }
        if (type_is_bool(to)) {
            if (!is_cast) {
                eval_value_free(&val);
                type_free(to);
                return EVAL_UNSUPPORTED;
            }
            /* integer -> bool: 0 -> false, 1 -> true, other out of range */
            if (val.u.i.big || (val.u.i.v != 0 && val.u.i.v != 1)) {
                eval_value_free(&val);
                type_free(to);
                *fail = EVAL_FAIL_CAST_RANGE;
                return EVAL_FAILURE;
            }
            {
                bool b = (val.u.i.v == 1);
                eval_value_free(&val);
                *out = val_bool(b, to);
            }
            return EVAL_OK;
        }
        if (tp->is_integer) {
            if (is_cast) {
                if (!int_fits(&val.u.i, tp)) {
                    eval_value_free(&val);
                    type_free(to);
                    *fail = EVAL_FAIL_CAST_RANGE;
                    return EVAL_FAILURE;
                }
                /* reinterpret at the target width (value-preserving:
                 * int_fits guarantees the mathematical value is in
                 * range) */
                {
                    EvalInt r = int_from_raw(int_raw(&val.u.i, tp->width_bits),
                                             tp->width_bits, tp->is_signed);
                    eval_value_free(&val);
                    *out = val_int(r, to);
                    return EVAL_OK;
                }
            } else {
                /* wrap: modulo 2^width, never checked */
                EvalInt r = int_from_raw(int_raw(&val.u.i, tp->width_bits),
                                         tp->width_bits, tp->is_signed);
                eval_value_free(&val);
                *out = val_int(r, to);
                return EVAL_OK;
            }
        }
        if (to->kind == TYPE_ENUM && is_cast) {
            /* unreachable: a non-primitive target was handled by the
             * `!tp` branch above */
            eval_value_free(&val);
            type_free(to);
            return EVAL_UNSUPPORTED;
        }
        if (to->kind == TYPE_PTR && is_cast) {
            /* unreachable: a non-primitive target was handled by the
             * `!tp` branch above */
            eval_value_free(&val);
            type_free(to);
            return EVAL_UNSUPPORTED;
        }
        eval_value_free(&val);
        type_free(to);
        return EVAL_UNSUPPORTED;
    }

    eval_value_free(&val);
    type_free(to);
    return EVAL_UNSUPPORTED;
}

/* ---------------------------------------------------------------------------
 * Static addresses and slices (spec sec. 10.5 / sec. 12.5)
 * ------------------------------------------------------------------------- */

static EvalStatus eval_static_addr(EvalCtx *ctx, const AstNode *operand,
                                   EvalValue *out, EvalFailure *fail)
{
    const NameModule *saved;
    /* & of a global var: IDENT -> NAME_SYM_GLOBAL_VAR */
    if (operand->kind == AST_EXPR_IDENT) {
        const NameSymbol *sym = name_symbol_for_node(ctx->module, operand);
        Type *t = NULL, *pt;
        if (!sym) return EVAL_UNSUPPORTED;
        if (sym->kind == NAME_SYM_GLOBAL_VAR) {
            if (type_of_symbol(ctx, sym, &t) != TYP_OK) return EVAL_UNSUPPORTED;
            pt = type_ptr_new(t);
            if (!pt) { type_free(t); return EVAL_OOM; }
            *out = val_addr(sym, 0, pt);
            return EVAL_OK;
        }
        if (sym->kind == NAME_SYM_GLOBAL_CONST ||
            sym->kind == NAME_SYM_LOCAL_CONST) {
            /* address of a const is AIC-E0402, owned by a later
             * package; do not emit E0401 here */
            return EVAL_UNSUPPORTED;
        }
        /* local var / param / fn / ... : not a static-storage lvalue */
        return EVAL_NOT_CONST;
    }
    /* &arr[i] of a static array: unary & over an index whose base is a
     * global var of array type */
    if (operand->kind == AST_EXPR_INDEX) {
        const AstNode *base = operand->u.index_slice.base;
        const AstNode *idx = operand->u.index_slice.index;
        const NameSymbol *sym;
        Type *vt = NULL, *et = NULL, *pt;
        int64_t extent = -1, esize = -1, off;
        EvalValue iv;
        EvalStatus st;
        if (base->kind != AST_EXPR_IDENT) return EVAL_NOT_CONST;
        sym = name_symbol_for_node(ctx->module, base);
        if (!sym || sym->kind != NAME_SYM_GLOBAL_VAR) return EVAL_NOT_CONST;
        if (type_of_symbol(ctx, sym, &vt) != TYP_OK || !vt ||
            vt->kind != TYPE_ARRAY) {
            type_free(vt);
            return EVAL_UNSUPPORTED;
        }
        et = vt->u.array.elem;
        vt->u.array.elem = NULL;
        type_free(vt);
        saved = ctx->module;
        ctx->module = sym->module;
        if (sym->decl && sym->decl->u.global_decl.type &&
            sym->decl->u.global_decl.type->kind == AST_TYPE_ARRAY &&
            sym->decl->u.global_decl.type->u.type_derived.len) {
            EvalValue ev;
            EvalStatus est = const_eval_expr(ctx,
                    sym->decl->u.global_decl.type->u.type_derived.len, &ev, fail);
            if (est == EVAL_OK && ev.kind == EVAL_VAL_INT &&
                !ev.u.i.big && ev.u.i.v >= 0) {
                extent = ev.u.i.v;
            }
            eval_value_free(&ev);
        }
        if (extent >= 0 && sym->decl &&
            sym->decl->u.global_decl.type &&
            sym->decl->u.global_decl.type->kind == AST_TYPE_ARRAY) {
            st = size_align_of_type_node(ctx, sym->module,
                    sym->decl->u.global_decl.type->u.type_derived.base,
                    &esize, NULL, fail);
        } else {
            st = EVAL_UNSUPPORTED;
        }
        ctx->module = saved;
        if (extent < 0 || st != EVAL_OK) {
            type_free(et);
            return st == EVAL_OK ? EVAL_UNSUPPORTED : st;
        }
        st = const_eval_expr(ctx, idx, &iv, fail);
        if (st == EVAL_NOT_CONST) { type_free(et); return EVAL_NOT_CONST; }
        if (st != EVAL_OK) { type_free(et); return st; }
        if (iv.kind != EVAL_VAL_INT) {
            eval_value_free(&iv);
            type_free(et);
            return EVAL_UNSUPPORTED;
        }
        if (iv.u.i.big || iv.u.i.v < 0 || iv.u.i.v >= extent) {
            eval_value_free(&iv);
            type_free(et);
            *fail = EVAL_FAIL_INDEX_RANGE;
            return EVAL_FAILURE;
        }
        if (i64_mul_ovf(iv.u.i.v, esize, &off)) {
            eval_value_free(&iv);
            type_free(et);
            *fail = EVAL_FAIL_OVERFLOW;
            return EVAL_FAILURE;
        }
        eval_value_free(&iv);
        pt = type_ptr_new(et);
        if (!pt) { type_free(et); return EVAL_OOM; }
        *out = val_addr(sym, off, pt);
        return EVAL_OK;
    }
    return EVAL_NOT_CONST;
}

/* Slice of a static array with constant bounds, or of a str value
 * (str literal / str const) with the code point boundary rule
 * (sec. 12.2; constant failure AIC-E0410). */
static EvalStatus eval_slice_expr(EvalCtx *ctx, const AstNode *e,
                                  EvalValue *out, EvalFailure *fail)
{
    const AstNode *base = e->u.index_slice.base;
    const AstNode *lo = e->u.index_slice.lo;
    const AstNode *hi = e->u.index_slice.hi;
    const NameModule *saved;
    EvalValue bv;
    EvalStatus bst;

    /* a str base (literal or const): byte slice */
    if (base->kind == AST_EXPR_STR_LITERAL) {
        const char *bytes = base->u.str_literal.bytes;
        size_t len = base->u.str_literal.len;
        EvalValue lv, hv;
        int64_t lo64 = 0, hi64 = (int64_t)len;
        EvalStatus st;
        if (lo) {
            st = const_eval_expr(ctx, lo, &lv, fail);
            if (st == EVAL_NOT_CONST) return EVAL_NOT_CONST;
            if (st != EVAL_OK) return st;
            if (lv.kind != EVAL_VAL_INT) { eval_value_free(&lv); return EVAL_UNSUPPORTED; }
            if (lv.u.i.big || lv.u.i.v < 0) {
                eval_value_free(&lv);
                *fail = EVAL_FAIL_INDEX_RANGE;
                return EVAL_FAILURE;
            }
            lo64 = lv.u.i.v;
            eval_value_free(&lv);
        }
        if (hi) {
            st = const_eval_expr(ctx, hi, &hv, fail);
            if (st == EVAL_NOT_CONST) return EVAL_NOT_CONST;
            if (st != EVAL_OK) return st;
            if (hv.kind != EVAL_VAL_INT) { eval_value_free(&hv); return EVAL_UNSUPPORTED; }
            if (hv.u.i.big || hv.u.i.v < 0) {
                eval_value_free(&hv);
                *fail = EVAL_FAIL_INDEX_RANGE;
                return EVAL_FAILURE;
            }
            hi64 = hv.u.i.v;
            eval_value_free(&hv);
        }
        if (lo64 > hi64 || hi64 > (int64_t)len) {
            *fail = EVAL_FAIL_INDEX_RANGE;
            return EVAL_FAILURE;
        }
        if (!utf8_boundary(bytes, len, lo64) || !utf8_boundary(bytes, len, hi64)) {
            *fail = EVAL_FAIL_STR_BOUNDARY;
            return EVAL_FAILURE;
        }
        *out = val_str(bytes + lo64, (size_t)(hi64 - lo64),
                       type_prim_new(AST_PRIM_STR));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    }

    /* a str const base: evaluate it, then slice the bytes */
    if (base->kind == AST_EXPR_IDENT) {
        const NameSymbol *sym = name_symbol_for_node(ctx->module, base);
        if (sym && (sym->kind == NAME_SYM_GLOBAL_CONST ||
                    sym->kind == NAME_SYM_LOCAL_CONST)) {
            bst = const_eval_expr(ctx, base, &bv, fail);
            if (bst != EVAL_OK) return bst;
            if (bv.kind == EVAL_VAL_STR) {
                const char *bytes = bv.u.str.bytes;
                size_t len = bv.u.str.len;
                EvalValue lv, hv;
                int64_t lo64 = 0, hi64 = (int64_t)len;
                EvalStatus st;
                if (lo) {
                    st = const_eval_expr(ctx, lo, &lv, fail);
                    if (st != EVAL_OK) { eval_value_free(&bv); return st; }
                    if (lv.kind != EVAL_VAL_INT) {
                        eval_value_free(&lv); eval_value_free(&bv);
                        return EVAL_UNSUPPORTED;
                    }
                    if (lv.u.i.big || lv.u.i.v < 0) {
                        eval_value_free(&lv); eval_value_free(&bv);
                        *fail = EVAL_FAIL_INDEX_RANGE;
                        return EVAL_FAILURE;
                    }
                    lo64 = lv.u.i.v;
                    eval_value_free(&lv);
                }
                if (hi) {
                    st = const_eval_expr(ctx, hi, &hv, fail);
                    if (st != EVAL_OK) { eval_value_free(&bv); return st; }
                    if (hv.kind != EVAL_VAL_INT) {
                        eval_value_free(&hv); eval_value_free(&bv);
                        return EVAL_UNSUPPORTED;
                    }
                    if (hv.u.i.big || hv.u.i.v < 0) {
                        eval_value_free(&hv); eval_value_free(&bv);
                        *fail = EVAL_FAIL_INDEX_RANGE;
                        return EVAL_FAILURE;
                    }
                    hi64 = hv.u.i.v;
                    eval_value_free(&hv);
                }
                if (lo64 > hi64 || hi64 > (int64_t)len) {
                    eval_value_free(&bv);
                    *fail = EVAL_FAIL_INDEX_RANGE;
                    return EVAL_FAILURE;
                }
                if (!utf8_boundary(bytes, len, lo64) ||
                    !utf8_boundary(bytes, len, hi64)) {
                    eval_value_free(&bv);
                    *fail = EVAL_FAIL_STR_BOUNDARY;
                    return EVAL_FAILURE;
                }
                eval_value_free(&bv);
                *out = val_str(bytes + lo64, (size_t)(hi64 - lo64),
                               type_prim_new(AST_PRIM_STR));
                if (!out->type) return EVAL_OOM;
                return EVAL_OK;
            }
            {
                bool bv_was_array = (bv.kind == EVAL_VAL_ARRAY);
                eval_value_free(&bv);
                if (bv_was_array) return EVAL_NOT_CONST;
            }
            return EVAL_UNSUPPORTED;
        }
    }

    /* slice of a static array with constant bounds */
    {
        const NameSymbol *sym;
        Type *vt = NULL, *st2 = NULL;
        int64_t extent = -1;
        EvalValue lv, hv;
        int64_t lo64 = 0, hi64 = -1;
        EvalStatus st;
        if (base->kind != AST_EXPR_IDENT) return EVAL_NOT_CONST;
        sym = name_symbol_for_node(ctx->module, base);
        if (!sym || sym->kind != NAME_SYM_GLOBAL_VAR) return EVAL_NOT_CONST;
        if (type_of_symbol(ctx, sym, &vt) != TYP_OK || !vt ||
            vt->kind != TYPE_ARRAY) {
            type_free(vt);
            return EVAL_UNSUPPORTED;
        }
        saved = ctx->module;
        ctx->module = sym->module;
        if (sym->decl && sym->decl->u.global_decl.type &&
            sym->decl->u.global_decl.type->kind == AST_TYPE_ARRAY &&
            sym->decl->u.global_decl.type->u.type_derived.len) {
            EvalValue ev;
            EvalStatus est = const_eval_expr(ctx,
                    sym->decl->u.global_decl.type->u.type_derived.len, &ev, fail);
            if (est == EVAL_OK && ev.kind == EVAL_VAL_INT &&
                !ev.u.i.big && ev.u.i.v >= 0) {
                extent = ev.u.i.v;
            }
            eval_value_free(&ev);
        }
        ctx->module = saved;
        if (extent < 0) {
            type_free(vt);
            return EVAL_UNSUPPORTED;
        }
        if (lo) {
            st = const_eval_expr(ctx, lo, &lv, fail);
            if (st == EVAL_NOT_CONST) { type_free(vt); return EVAL_NOT_CONST; }
            if (st != EVAL_OK) { type_free(vt); return st; }
            if (lv.kind != EVAL_VAL_INT) {
                eval_value_free(&lv); type_free(vt); return EVAL_UNSUPPORTED;
            }
            if (lv.u.i.big || lv.u.i.v < 0) {
                eval_value_free(&lv); type_free(vt);
                *fail = EVAL_FAIL_INDEX_RANGE;
                return EVAL_FAILURE;
            }
            lo64 = lv.u.i.v;
            eval_value_free(&lv);
        }
        hi64 = extent;
        if (hi) {
            st = const_eval_expr(ctx, hi, &hv, fail);
            if (st == EVAL_NOT_CONST) { type_free(vt); return EVAL_NOT_CONST; }
            if (st != EVAL_OK) { type_free(vt); return st; }
            if (hv.kind != EVAL_VAL_INT) {
                eval_value_free(&hv); type_free(vt); return EVAL_UNSUPPORTED;
            }
            if (hv.u.i.big || hv.u.i.v < 0) {
                eval_value_free(&hv); type_free(vt);
                *fail = EVAL_FAIL_INDEX_RANGE;
                return EVAL_FAILURE;
            }
            hi64 = hv.u.i.v;
            eval_value_free(&hv);
        }
        if (lo64 > hi64 || hi64 > extent) {
            type_free(vt);
            *fail = EVAL_FAIL_INDEX_RANGE;
            return EVAL_FAILURE;
        }
        st2 = type_slice_new(vt->u.array.elem);
        vt->u.array.elem = NULL;
        type_free(vt);
        if (!st2) return EVAL_OOM;
        *out = val_slice(sym, lo64, hi64, st2);
        return EVAL_OK;
    }
}

/* ---------------------------------------------------------------------------
 * Binary operators (spec sec. 10.2 / 11.3 / 11.4 / 12.5)
 * ------------------------------------------------------------------------- */

static EvalStatus eval_binary(EvalCtx *ctx, const AstNode *e,
                              EvalValue *out, EvalFailure *fail)
{
    const AstNode *lhs = e->u.binary.lhs;
    const AstNode *rhs = e->u.binary.rhs;
    AstBinaryOp op = e->u.binary.op;
    EvalValue lv, rv;
    EvalStatus st;
    if (fail) *fail = EVAL_FAIL_NONE;
    st = const_eval_expr(ctx, lhs, &lv, fail);
    if (st != EVAL_OK) return st;
    st = const_eval_expr(ctx, rhs, &rv, fail);
    if (st != EVAL_OK) { eval_value_free(&lv); return st; }

    if (op == AST_BIN_LAND || op == AST_BIN_LOR) {
        bool a, b, r;
        if (!type_is_bool(lv.type) || !type_is_bool(rv.type)) {
            eval_value_free(&lv);
            eval_value_free(&rv);
            return EVAL_UNSUPPORTED;
        }
        a = lv.u.b;
        b = rv.u.b;
        r = op == AST_BIN_LAND ? (a && b) : (a || b);
        eval_value_free(&lv);
        eval_value_free(&rv);
        *out = val_bool(r, type_prim_new(AST_PRIM_BOOL));
        return out->type ? EVAL_OK : EVAL_OOM;
    }

    /* pointer arithmetic (sec. 12.5): p + i, i + p, p - i, p - q */
    if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
        if (lv.kind == EVAL_VAL_ADDR && rv.kind == EVAL_VAL_INT) {
            int64_t esize, noff, nb;
            EvalStatus sst;
            if (rv.u.i.big || rv.u.i.v == INT64_MIN) {
                eval_value_free(&lv); eval_value_free(&rv);
                *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            sst = size_align_of_type(ctx, lv.type ? lv.type->u.ptr.elem : NULL,
                                     &esize, NULL, fail);
            if (sst != EVAL_OK) {
                eval_value_free(&lv); eval_value_free(&rv);
                return sst;
            }
            if (i64_mul_ovf(rv.u.i.v, esize, &noff)) {
                eval_value_free(&lv); eval_value_free(&rv);
                *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            if (op == AST_BIN_SUB) {
                if (i64_sub_ovf(lv.u.addr.byte_offset, noff, &nb)) {
                    eval_value_free(&lv); eval_value_free(&rv);
                    *fail = EVAL_FAIL_OVERFLOW;
                    return EVAL_FAILURE;
                }
            } else {
                if (i64_add_ovf(lv.u.addr.byte_offset, noff, &nb)) {
                    eval_value_free(&lv); eval_value_free(&rv);
                    *fail = EVAL_FAIL_OVERFLOW;
                    return EVAL_FAILURE;
                }
            }
            lv.u.addr.byte_offset = nb;
            eval_value_free(&rv);
            *out = lv;
            return EVAL_OK;
        }
        if (op == AST_BIN_ADD && lv.kind == EVAL_VAL_INT &&
            rv.kind == EVAL_VAL_ADDR) {
            int64_t esize, noff, nb;
            EvalStatus sst;
            if (lv.u.i.big || lv.u.i.v == INT64_MIN) {
                eval_value_free(&lv); eval_value_free(&rv);
                *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            sst = size_align_of_type(ctx, rv.type ? rv.type->u.ptr.elem : NULL,
                                     &esize, NULL, fail);
            if (sst != EVAL_OK) {
                eval_value_free(&lv); eval_value_free(&rv);
                return sst;
            }
            if (i64_mul_ovf(lv.u.i.v, esize, &noff) ||
                i64_add_ovf(rv.u.addr.byte_offset, noff, &nb)) {
                eval_value_free(&lv); eval_value_free(&rv);
                *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            rv.u.addr.byte_offset = nb;
            eval_value_free(&lv);
            *out = rv;
            return EVAL_OK;
        }
        if (op == AST_BIN_SUB && lv.kind == EVAL_VAL_ADDR &&
            rv.kind == EVAL_VAL_ADDR) {
            /* p - q: same object required for a compile-time value;
             * different objects -> link-time (not a constant
             * expression). The byte difference must be a multiple of
             * sizeof(T) (AIC-E0411 -> EVAL_FAIL_PTR_DIFF). */
            int64_t esize, diff, q;
            EvalStatus sst;
            if (lv.u.addr.sym != rv.u.addr.sym) {
                eval_value_free(&lv); eval_value_free(&rv);
                return EVAL_NOT_CONST;
            }
            sst = size_align_of_type(ctx, lv.type ? lv.type->u.ptr.elem : NULL,
                                     &esize, NULL, fail);
            if (sst != EVAL_OK) {
                eval_value_free(&lv); eval_value_free(&rv);
                return sst;
            }
            if (esize == 0) {
                eval_value_free(&lv); eval_value_free(&rv);
                return EVAL_UNSUPPORTED;
            }
            if (i64_sub_ovf(lv.u.addr.byte_offset, rv.u.addr.byte_offset,
                            &diff)) {
                eval_value_free(&lv); eval_value_free(&rv);
                *fail = EVAL_FAIL_OVERFLOW;
                return EVAL_FAILURE;
            }
            if (diff % esize != 0) {
                eval_value_free(&lv); eval_value_free(&rv);
                *fail = EVAL_FAIL_PTR_DIFF;
                return EVAL_FAILURE;
            }
            q = diff / esize;
            eval_value_free(&lv);
            eval_value_free(&rv);
            *out = val_int((EvalInt){ q, false }, type_prim_new(AST_PRIM_ISIZE));
            return out->type ? EVAL_OK : EVAL_OOM;
        }
    }

    /* comparisons (sec. 11.4) */
    if (op == AST_BIN_LT || op == AST_BIN_LE || op == AST_BIN_GT ||
        op == AST_BIN_GE || op == AST_BIN_EQ || op == AST_BIN_NE) {
        bool result;
        bool equality = (op == AST_BIN_EQ || op == AST_BIN_NE);
        bool ordered = !equality;
        int c;
        if (lv.kind == EVAL_VAL_NULL || rv.kind == EVAL_VAL_NULL) {
            if (lv.kind == EVAL_VAL_NULL && rv.kind == EVAL_VAL_NULL) {
                result = (op == AST_BIN_EQ);   /* defensive; 11d rejects */
            } else {
                const EvalValue *other = lv.kind == EVAL_VAL_NULL ? &rv : &lv;
                bool other_null;
                if (other->kind != EVAL_VAL_ADDR) {
                    eval_value_free(&lv); eval_value_free(&rv);
                    return EVAL_UNSUPPORTED;
                }
                other_null = (other->u.addr.sym == NULL &&
                              other->u.addr.byte_offset == 0);
                if (ordered) {
                    /* null vs a static address / raw address ordering
                     * is link-time; not a constant expression */
                    eval_value_free(&lv); eval_value_free(&rv);
                    return EVAL_NOT_CONST;
                }
                result = (op == AST_BIN_EQ) ? other_null : !other_null;
            }
            eval_value_free(&lv);
            eval_value_free(&rv);
            *out = val_bool(result, type_prim_new(AST_PRIM_BOOL));
            return out->type ? EVAL_OK : EVAL_OOM;
        }
        if (lv.kind == EVAL_VAL_ADDR && rv.kind == EVAL_VAL_ADDR) {
            if (lv.u.addr.sym == rv.u.addr.sym &&
                (lv.u.addr.sym != NULL || ordered)) {
                int64_t a = lv.u.addr.byte_offset, b = rv.u.addr.byte_offset;
                c = a < b ? -1 : (a > b ? 1 : 0);
            } else if (equality) {
                c = (lv.u.addr.sym == rv.u.addr.sym &&
                     lv.u.addr.byte_offset == rv.u.addr.byte_offset) ? 0 : 1;
            } else {
                eval_value_free(&lv); eval_value_free(&rv);
                return EVAL_NOT_CONST;
            }
            eval_value_free(&lv);
            eval_value_free(&rv);
            switch (op) {
            case AST_BIN_LT: result = c < 0; break;
            case AST_BIN_LE: result = c <= 0; break;
            case AST_BIN_GT: result = c > 0; break;
            case AST_BIN_GE: result = c >= 0; break;
            case AST_BIN_EQ: result = c == 0; break;
            default:         result = c != 0; break;
            }
            *out = val_bool(result, type_prim_new(AST_PRIM_BOOL));
            return out->type ? EVAL_OK : EVAL_OOM;
        }
        if (lv.kind == EVAL_VAL_BOOL && rv.kind == EVAL_VAL_BOOL) {
            if (!equality) {
                eval_value_free(&lv); eval_value_free(&rv);
                return EVAL_UNSUPPORTED;
            }
            result = (lv.u.b == rv.u.b) == (op == AST_BIN_EQ);
            eval_value_free(&lv);
            eval_value_free(&rv);
            *out = val_bool(result, type_prim_new(AST_PRIM_BOOL));
            return out->type ? EVAL_OK : EVAL_OOM;
        }
        if (lv.kind == EVAL_VAL_STR && rv.kind == EVAL_VAL_STR) {
            size_t n = lv.u.str.len < rv.u.str.len ? lv.u.str.len : rv.u.str.len;
            int mc = n ? memcmp(lv.u.str.bytes, rv.u.str.bytes, n) : 0;
            c = mc != 0 ? mc : (lv.u.str.len < rv.u.str.len ? -1 :
                                (lv.u.str.len > rv.u.str.len ? 1 : 0));
            eval_value_free(&lv);
            eval_value_free(&rv);
            switch (op) {
            case AST_BIN_LT: result = c < 0; break;
            case AST_BIN_LE: result = c <= 0; break;
            case AST_BIN_GT: result = c > 0; break;
            case AST_BIN_GE: result = c >= 0; break;
            case AST_BIN_EQ: result = c == 0; break;
            default:         result = c != 0; break;
            }
            *out = val_bool(result, type_prim_new(AST_PRIM_BOOL));
            return out->type ? EVAL_OK : EVAL_OOM;
        }
        if (lv.kind == EVAL_VAL_INT && rv.kind == EVAL_VAL_INT) {
            /* integer/enum comparison: mathematical value on the actual
             * type's range (sec. 11.4). Enum operands compare by
             * underlying value; 11d already required same enum type. */
            c = int_cmp(&lv.u.i, &rv.u.i);
            eval_value_free(&lv);
            eval_value_free(&rv);
            switch (op) {
            case AST_BIN_LT: result = c < 0; break;
            case AST_BIN_LE: result = c <= 0; break;
            case AST_BIN_GT: result = c > 0; break;
            case AST_BIN_GE: result = c >= 0; break;
            case AST_BIN_EQ: result = c == 0; break;
            default:         result = c != 0; break;
            }
            *out = val_bool(result, type_prim_new(AST_PRIM_BOOL));
            return out->type ? EVAL_OK : EVAL_OOM;
        }
        eval_value_free(&lv);
        eval_value_free(&rv);
        return EVAL_UNSUPPORTED;
    }

    /* shifts: result is the left operand type; the count is the right
     * operand's mathematical value (11c already checked the conversion) */
    if (op == AST_BIN_SHL || op == AST_BIN_SHR) {
        int w;
        bool s;
        EvalInt r;
        Type *lt;
        if (!int_ty_of(lv.type, &w, &s) || rv.kind != EVAL_VAL_INT) {
            eval_value_free(&lv); eval_value_free(&rv);
            return EVAL_UNSUPPORTED;
        }
        st = int_shift_op(&lv.u.i, w, s, &rv.u.i, op, &r, fail);
        lt = lv.type;
        lv.type = NULL;
        eval_value_free(&lv);
        eval_value_free(&rv);
        if (st != EVAL_OK) { type_free(lt); return st; }
        *out = val_int(r, lt);
        return EVAL_OK;
    }

    /* integer arithmetic/bitwise: common type, checked */
    {
        int w;
        bool s;
        Type *ct = NULL;
        EvalInt a, b, r;
        const TypePrimInfo *ctp;
        if (lv.kind != EVAL_VAL_INT || rv.kind != EVAL_VAL_INT) {
            eval_value_free(&lv); eval_value_free(&rv);
            return EVAL_UNSUPPORTED;
        }
        if (lv.type && rv.type) {
            ct = convert_common_type(lv.type, rv.type);
        }
        if (!ct || !int_ty_of(ct, &w, &s)) {
            type_free(ct);
            eval_value_free(&lv); eval_value_free(&rv);
            return EVAL_UNSUPPORTED;
        }
        a = lv.u.i;
        b = rv.u.i;
        eval_value_free(&lv);
        eval_value_free(&rv);
        /* value-preserving widening to the common type is guaranteed by
         * convert_common_type (Table 11.1 rows); defensively check */
        ctp = types_prim_info(ct->u.prim);
        if (!int_fits(&a, ctp) || !int_fits(&b, ctp)) {
            type_free(ct);
            return EVAL_UNSUPPORTED;
        }
        st = int_arith_op(&a, &b, op, w, s, &r, fail);
        if (st != EVAL_OK) { type_free(ct); return st; }
        *out = val_int(r, ct);
        return EVAL_OK;
    }
}

/* ---------------------------------------------------------------------------
 * Core evaluation entry point
 * ------------------------------------------------------------------------- */

EvalStatus const_eval_expr(EvalCtx *ctx, const AstNode *e,
                           EvalValue *out, EvalFailure *out_failure)
{
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    if (!ctx || !e || !out) return EVAL_UNSUPPORTED;
    if (ctx->oom) return EVAL_OOM;
    switch (e->kind) {
    case AST_EXPR_INT_LITERAL: {
        EvalInt v;
        int_from_literal(e, &v);
        *out = val_int(v, type_prim_new(prim_from_lex(e->u.int_literal.type)));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    }
    case AST_EXPR_BOOL_LITERAL:
        *out = val_bool(e->u.bool_literal.value,
                        type_prim_new(AST_PRIM_BOOL));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    case AST_EXPR_STR_LITERAL:
        *out = val_str(e->u.str_literal.bytes, e->u.str_literal.len,
                       type_prim_new(AST_PRIM_STR));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    case AST_EXPR_NULL_LITERAL:
        *out = val_null();
        return EVAL_OK;
    case AST_EXPR_PAREN:
        return const_eval_expr(ctx, e->u.paren.expr, out, out_failure);
    case AST_EXPR_IDENT: {
        const NameSymbol *sym = name_symbol_for_node(ctx->module, e);
        if (!sym) return EVAL_UNSUPPORTED;
        if (sym->kind == NAME_SYM_GLOBAL_CONST ||
            sym->kind == NAME_SYM_LOCAL_CONST) {
            return eval_const_ref(ctx, sym, out, out_failure);
        }
        /* a var name, function name, struct/enum type name, or field is
         * not a const form (only const names and enum members are) */
        return EVAL_NOT_CONST;
    }
    case AST_EXPR_MEMBER: {
        const NameSymbol *sym = name_symbol_for_node(ctx->module, e);
        if (!sym) return EVAL_UNSUPPORTED;
        if (sym->kind == NAME_SYM_ENUM_MEMBER) {
            /* enum member: read the member value from the WP-M0-11b
             * LayoutBuild (the authoritative source) */
            const LayoutEnum *le = layout_build_enum(ctx->layout, sym->owner);
            Type *t;
            EvalInt v = { 0, false };
            size_t i;
            bool found = false;
            if (!le) return EVAL_UNSUPPORTED;
            for (i = 0; i < le->nmembers; i++) {
                if (le->members[i].name &&
                    strcmp(le->members[i].name, sym->name) == 0) {
                    if (le->members[i].domain_overflow) {
                        return EVAL_UNSUPPORTED;
                    }
                    v.v = le->members[i].value;
                    v.big = le->members[i].big_unsigned;
                    found = true;
                    break;
                }
            }
            if (!found) return EVAL_UNSUPPORTED;
            t = type_enum_new(sym->owner);
            if (!t) return EVAL_OOM;
            *out = val_int(v, t);
            return EVAL_OK;
        }
        if (sym->kind == NAME_SYM_GLOBAL_CONST ||
            sym->kind == NAME_SYM_LOCAL_CONST) {
            /* module-qualified const reference (e.g. a.X) */
            return eval_const_ref(ctx, sym, out, out_failure);
        }
        /* struct field access is not a const form (sec. 10.5 lists
         * only enum members) */
        return EVAL_NOT_CONST;
    }
    case AST_EXPR_ARRAY_LITERAL: {
        size_t n = e->u.array_literal.nelems;
        size_t total = n;
        EvalValue *arr = NULL;
        size_t i;
        if (e->u.array_literal.count) {
            EvalValue cv;
            EvalStatus cst = const_eval_expr(ctx, e->u.array_literal.count,
                                             &cv, out_failure);
            if (cst == EVAL_NOT_CONST) return EVAL_NOT_CONST;
            if (cst != EVAL_OK) return cst;
            if (cv.kind != EVAL_VAL_INT) {
                eval_value_free(&cv);
                return EVAL_UNSUPPORTED;
            }
            if (cv.u.i.big || cv.u.i.v < 0) {
                eval_value_free(&cv);
                if (out_failure) *out_failure = EVAL_FAIL_INDEX_RANGE;
                return EVAL_FAILURE;
            }
            total = (size_t)cv.u.i.v;
            eval_value_free(&cv);
        }
        arr = (EvalValue *)calloc(total ? total : 1, sizeof(EvalValue));
        if (!arr) return EVAL_OOM;
        for (i = 0; i < total; i++) {
            const AstNode *el = e->u.array_literal.elems[
                e->u.array_literal.count ? 0 : i];
            st = const_eval_expr(ctx, el, &arr[i], out_failure);
            if (st != EVAL_OK) {
                size_t j;
                for (j = 0; j < i; j++) eval_value_free(&arr[j]);
                free(arr);
                return st;
            }
        }
        out->kind = EVAL_VAL_ARRAY;
        out->type = NULL;   /* an array literal has no standalone type */
        out->u.array.elems = arr;
        out->u.array.nelems = total;
        return EVAL_OK;
    }
    case AST_EXPR_STRUCT_INIT: {
        size_t n = e->u.struct_init.nfields;
        EvalValue *fields = NULL;
        const NameSymbol *sym = name_symbol_for_node(ctx->module,
                                                     e->u.struct_init.base);
        Type *t;
        size_t i;
        if (!sym || sym->kind != NAME_SYM_STRUCT) return EVAL_UNSUPPORTED;
        t = type_struct_new(sym);
        if (!t) return EVAL_OOM;
        fields = (EvalValue *)calloc(n ? n : 1, sizeof(EvalValue));
        if (!fields) { type_free(t); return EVAL_OOM; }
        for (i = 0; i < n; i++) {
            const AstNode *fi = e->u.struct_init.fields[i];
            if (!fi->u.named.value) {
                size_t j;
                for (j = 0; j < i; j++) eval_value_free(&fields[j]);
                free(fields);
                type_free(t);
                return EVAL_UNSUPPORTED;
            }
            st = const_eval_expr(ctx, fi->u.named.value, &fields[i],
                                 out_failure);
            if (st != EVAL_OK) {
                size_t j;
                for (j = 0; j < i; j++) eval_value_free(&fields[j]);
                free(fields);
                type_free(t);
                return st;
            }
        }
        out->kind = EVAL_VAL_STRUCT;
        out->type = t;
        out->u.st.fields = fields;
        out->u.st.nfields = n;
        return EVAL_OK;
    }
    case AST_EXPR_SIZEOF_TYPE: {
        int64_t size, align;
        EvalStatus sst = size_align_of_type_node(ctx, ctx->module,
                                                 e->u.size_op.operand,
                                                 &size, &align, &fail);
        if (sst != EVAL_OK) {
            if (out_failure) *out_failure = fail;
            return sst;
        }
        *out = val_int((EvalInt){ size, false }, type_prim_new(AST_PRIM_USIZE));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    }
    case AST_EXPR_SIZEOF_EXPR: {
        Type *t = NULL;
        bool isnull = false;
        int64_t size, align;
        EvalStatus sst;
        if (type_of_expr(ctx, ctx->module, e->u.size_op.operand, &t,
                         &isnull) != TYP_OK || isnull || !t) {
            type_free(t);
            return EVAL_NOT_CONST;
        }
        sst = size_align_of_type(ctx, t, &size, &align, &fail);
        type_free(t);
        if (sst != EVAL_OK) {
            if (out_failure) *out_failure = fail;
            return sst;
        }
        *out = val_int((EvalInt){ size, false }, type_prim_new(AST_PRIM_USIZE));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    }
    case AST_EXPR_ALIGNOF: {
        int64_t size, align;
        EvalStatus sst = size_align_of_type_node(ctx, ctx->module,
                                                 e->u.size_op.operand,
                                                 &size, &align, &fail);
        if (sst != EVAL_OK) {
            if (out_failure) *out_failure = fail;
            return sst;
        }
        *out = val_int((EvalInt){ align, false }, type_prim_new(AST_PRIM_USIZE));
        if (!out->type) return EVAL_OOM;
        return EVAL_OK;
    }
    case AST_EXPR_UNARY: {
        AstUnaryOp op = e->u.unary.op;
        const AstNode *operand = e->u.unary.operand;
        EvalValue v;
        if (op == AST_UN_ADDR) {
            return eval_static_addr(ctx, operand, out, out_failure);
        }
        if (op == AST_UN_DEREF) {
            /* dereference of a constant address reads runtime storage;
             * not a const form */
            return EVAL_NOT_CONST;
        }
        if (op == AST_UN_NEG && operand &&
            operand->kind == AST_EXPR_INT_LITERAL &&
            operand->u.int_literal.is_min) {
            /* "-" directly over a min-magnitude literal denotes the
             * type minimum (spec sec. 4.3); the literal already
             * evaluated to it */
            return const_eval_expr(ctx, operand, out, out_failure);
        }
        st = const_eval_expr(ctx, operand, &v, out_failure);
        if (st != EVAL_OK) return st;
        if (v.kind == EVAL_VAL_INT && v.type) {
            int w;
            bool s;
            EvalInt r;
            if (!int_ty_of(v.type, &w, &s)) {
                eval_value_free(&v);
                return EVAL_UNSUPPORTED;
            }
            st = int_unary_op(op, &v.u.i, w, s, &r, &fail);
            if (st != EVAL_OK) {
                eval_value_free(&v);
                if (out_failure) *out_failure = fail;
                return st;
            }
            v.u.i = r;
            *out = v;
            return EVAL_OK;
        }
        if (op == AST_UN_NOT && v.kind == EVAL_VAL_BOOL) {
            bool b = !v.u.b;
            eval_value_free(&v);
            *out = val_bool(b, type_prim_new(AST_PRIM_BOOL));
            if (!out->type) return EVAL_OOM;
            return EVAL_OK;
        }
        eval_value_free(&v);
        return EVAL_UNSUPPORTED;
    }
    case AST_EXPR_BINARY:
        return eval_binary(ctx, e, out, out_failure);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        return eval_convert(ctx, e, e->kind == AST_EXPR_CAST, out,
                            out_failure);
    case AST_EXPR_SLICE:
        return eval_slice_expr(ctx, e, out, out_failure);
    case AST_EXPR_TERNARY:
    case AST_EXPR_INDEX:
    case AST_EXPR_CALL:
    case AST_EXPR_ASSIGN:
    case AST_EXPR_ARROW:
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        /* forms outside the sec. 10.5 composition */
        return EVAL_NOT_CONST;
    default:
        return EVAL_UNSUPPORTED;
    }
}

/* ---------------------------------------------------------------------------
 * Build-level const-context check (AIC-E0401)
 * ------------------------------------------------------------------------- */

static DiagRecord *new_semantic_record(EvalCtx *ctx, const char *code,
                                       const char *message,
                                       const DiagSpan *primary)
{
    DiagRecord *r = diag_record_new();
    if (!r) { ctx->oom = true; return NULL; }
    if (!diag_record_set_code(r, code) ||
        !diag_record_set_message(r, message) ||
        !diag_record_set_primary_span(r, primary) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        ctx->oom = true;
        return NULL;
    }
    return r;
}

typedef struct ConstCheckCtx {
    EvalCtx ev;
    DiagRecord **records;
    size_t nrecords, records_cap;
    EvalFailureSite *failures;
    size_t nfailures, failures_cap;
    bool unsupported;
} ConstCheckCtx;

static bool cc_push_record(ConstCheckCtx *c, DiagRecord *r)
{
    if (c->nrecords == c->records_cap) {
        size_t ncap = c->records_cap ? c->records_cap * 2 : 8;
        DiagRecord **nr = (DiagRecord **)realloc(
            c->records, ncap * sizeof(DiagRecord *));
        if (!nr) { c->ev.oom = true; return false; }
        c->records = nr;
        c->records_cap = ncap;
    }
    c->records[c->nrecords++] = r;
    return true;
}

static bool cc_push_failure(ConstCheckCtx *c, const AstNode *node,
                            EvalFailure kind)
{
    EvalFailureSite *nf;
    if (c->nfailures == c->failures_cap) {
        size_t ncap = c->failures_cap ? c->failures_cap * 2 : 8;
        nf = (EvalFailureSite *)realloc(
            c->failures, ncap * sizeof(EvalFailureSite));
        if (!nf) { c->ev.oom = true; return false; }
        c->failures = nf;
        c->failures_cap = ncap;
    }
    c->failures[c->nfailures].node = node;
    c->failures[c->nfailures].kind = kind;
    c->nfailures++;
    return true;
}

/* Evaluate one const-context site: on EVAL_NOT_CONST emit AIC-E0401 at
 * the site expression; on EVAL_FAILURE route the failure (the site node
 * is the whole initializer expression - the WP-M0-12b record span
 * convention). */
static void cc_check_site(ConstCheckCtx *c, const AstNode *expr)
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
        DiagRecord *r = new_semantic_record(&c->ev, "AIC-E0401",
                                            "expression is not a constant expression",
                                            expr->span);
        if (r) cc_push_record(c, r);
        return;
    }
    if (st == EVAL_FAILURE) {
        cc_push_failure(c, expr, fail);
        return;
    }
    if (st == EVAL_OOM) return;
    /* EVAL_UNSUPPORTED: defensive; no record */
    c->unsupported = true;
}

ConstEvalStatus const_eval_check(const NameResult *result,
                                 const LayoutBuild *layout,
                                 DiagRecord ***out_records,
                                 size_t *out_record_count,
                                 EvalFailureSite **out_failures,
                                 size_t *out_failure_count)
{
    ConstCheckCtx c;
    size_t m;
    if (!result || !layout) return CONST_EVAL_UNSUPPORTED;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    if (out_failures) *out_failures = NULL;
    if (out_failure_count) *out_failure_count = 0;
    memset(&c, 0, sizeof(c));
    eval_ctx_init(&c.ev, result, layout, NULL);
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *mod = result->modules[m];
        size_t d;
        c.ev.module = mod;
        for (d = 0; d < mod->nmodule_scope; d++) {
            const NameSymbol *sym = mod->module_scope[d];
            const AstNode *decl;
            if (!sym || !sym->decl) continue;
            decl = sym->decl;
            switch (decl->kind) {
            case AST_GLOBAL_CONST_DECL:
            case AST_GLOBAL_VAR_DECL:
                cc_check_site(&c, decl->u.global_decl.init);
                break;
            case AST_ENUM_DECL: {
                size_t i;
                for (i = 0; i < decl->u.enum_decl.nmembers; i++) {
                    const AstNode *mem = decl->u.enum_decl.members[i];
                    if (mem->u.named.value) {
                        cc_check_site(&c, mem->u.named.value);
                    }
                }
                break;
            }
            default:
                break;
            }
            if (c.ev.oom) break;
        }
        if (c.ev.oom) break;
    }
    if (c.ev.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        free(c.failures);
        eval_ctx_cleanup(&c.ev);
        return CONST_EVAL_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    if (out_failures) *out_failures = c.failures;
    if (out_failure_count) *out_failure_count = c.nfailures;
    eval_ctx_cleanup(&c.ev);
    if (c.nrecords) return CONST_EVAL_DIAG_ERROR;
    if (c.nfailures) return CONST_EVAL_FAILURE;
    if (c.unsupported) return CONST_EVAL_UNSUPPORTED;
    return CONST_EVAL_OK;
}
