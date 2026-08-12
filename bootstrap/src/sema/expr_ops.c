/* bootstrap/src/sema/expr_ops.c
 *
 * AI-Co Stage-0 operator semantics (WP-M0-13b2).
 *
 * See expr_ops.h for the contract and documented decisions. This file
 * implements:
 *   - the checked-arithmetic compile-time decision model (spec
 *     sec. 11.3): expr_op_binary_decision / expr_op_neg_decision /
 *     expr_op_shift_value - pure, site-agnostic, never performing a
 *     host-trapping operation (all overflow detection uses unsigned /
 *     division-based checks, mirroring the WP-M0-12a evaluator's
 *     conventions so the decisions agree with const evaluation);
 *   - the comparison semantics model (spec sec. 11.4):
 *     expr_cmp_kind_of (the mechanism per operand type),
 *     expr_cmp_ints (mathematical value comparison),
 *     expr_cmp_bytes (lexicographic byte comparison for str),
 *     expr_cmp_slice_equal (length-then-element-wise slice equality),
 *     expr_cmp_addr (same-object pointer address ordering);
 *   - the operator-site check (sec. 11.3-11.6): expr_ops_check emits
 *     the AIC-E0405..E0411 failure records for the const-context sites
 *     owned by WP-M0-13b1 (array type extents sec. 12.1, switch case
 *     labels sec. 13.2, local const initializers sec. 8.1), closing
 *     the 13b1 routing gap. Classification delegates to the public
 *     WP-M0-12b2 classifier (rec_fail_classify); the message/span
 *     rendering mirrors the documented 12b2 conventions
 *     (eval_fail_rec.h, DIAGNOSTIC-CONTRACT sec. 11.5) so records are
 *     byte-identical in shape to 12b2's at the 12a-owned sites.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "expr_ops.h"

#include "../diag/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Width helpers (sec. 7.1 primitive widths; 1..64 handled generically)
 * ------------------------------------------------------------------------- */

/* The signed minimum of a width (2's complement). width >= 64 -> INT64_MIN. */
static int64_t ops_min_signed(int width)
{
    if (width >= 64) return INT64_MIN;
    return -(int64_t)((uint64_t)1 << (width - 1));
}

/* The signed maximum of a width. width >= 64 -> INT64_MAX. */
static int64_t ops_max_signed(int width)
{
    if (width >= 64) return INT64_MAX;
    return (int64_t)(((uint64_t)1 << (width - 1)) - 1);
}

/* The unsigned maximum of a width. width >= 64 -> UINT64_MAX. */
static uint64_t ops_max_unsigned(int width)
{
    if (width >= 64) return UINT64_MAX;
    return ((uint64_t)1 << width) - 1;
}

/* The low `width` bits of the value's two's-complement pattern. */
static uint64_t ops_int_raw(const EvalInt *v, int width)
{
    uint64_t raw = (uint64_t)v->v;
    if (width >= 64) return raw;
    return raw & ops_max_unsigned(width);
}

/* Interpret a w-bit pattern as the target type (the WP-M0-12a
 * int_from_raw convention: unsigned values >= 2^63 carry big=true;
 * signed values are sign-extended, big always false). */
static EvalInt ops_int_from_raw(uint64_t raw, int width, bool is_signed)
{
    EvalInt r;
    uint64_t mask = ops_max_unsigned(width);
    if (!is_signed) {
        uint64_t val = raw & mask;
        r.v = (int64_t)val;
        r.big = val >= ((uint64_t)1 << 63);
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

/* Arithmetic right shift, defined for negative values (the WP-M0-12a
 * convention). */
static int64_t ops_i64_ashr(int64_t a, int b)
{
    if (a >= 0) return a >> b;
    return ~(~a >> b);
}

/* Magnitude of an int64 as uint64 (INT64_MIN -> 2^63). */
static uint64_t ops_mag64(int64_t v)
{
    if (v < 0) {
        if (v == INT64_MIN) return (uint64_t)1 << 63;
        return (uint64_t)(-v);
    }
    return (uint64_t)v;
}

/* ---------------------------------------------------------------------------
 * Checked-arithmetic compile-time decisions (spec sec. 11.3)
 * ------------------------------------------------------------------------- */

const char *expr_op_decision_code(ExprOpDecision d)
{
    switch (d) {
    case EXPR_OP_OVERFLOW:    return "AIC-E0405";
    case EXPR_OP_DIV_ZERO:    return "AIC-E0406";
    case EXPR_OP_SHIFT_RANGE: return "AIC-E0407";
    default:                  return NULL;
    }
}

ExprOpDecision expr_op_binary_decision(AstBinaryOp op, EvalInt a, EvalInt b,
                                       int width, bool is_signed)
{
    if (width <= 0 || width > 64) return EXPR_OP_OK;  /* defensive */
    switch (op) {
    case AST_BIN_ADD:
        if (is_signed) {
            int64_t minv = ops_min_signed(width), maxv = ops_max_signed(width);
            if ((b.v > 0 && a.v > maxv - b.v) ||
                (b.v < 0 && a.v < minv - b.v)) {
                return EXPR_OP_OVERFLOW;
            }
            return EXPR_OP_OK;
        }
        {
            uint64_t av = (uint64_t)a.v, bv = (uint64_t)b.v;
            uint64_t maxv = ops_max_unsigned(width);
            /* av + bv > maxv  <=>  bv != 0 && av > maxv - bv (no wrap) */
            if (bv != 0 && av > maxv - bv) return EXPR_OP_OVERFLOW;
            return EXPR_OP_OK;
        }
    case AST_BIN_SUB:
        if (is_signed) {
            int64_t minv = ops_min_signed(width), maxv = ops_max_signed(width);
            if ((b.v > 0 && a.v < minv + b.v) ||
                (b.v < 0 && a.v > maxv + b.v)) {
                return EXPR_OP_OVERFLOW;
            }
            return EXPR_OP_OK;
        }
        {
            uint64_t av = (uint64_t)a.v, bv = (uint64_t)b.v;
            if (bv > av) return EXPR_OP_OVERFLOW;  /* result would wrap */
            return EXPR_OP_OK;
        }
    case AST_BIN_MUL:
        if (is_signed) {
            uint64_t ma = ops_mag64(a.v), mb = ops_mag64(b.v);
            uint64_t limit;
            if (ma == 0 || mb == 0) return EXPR_OP_OK;
            limit = ((a.v < 0) != (b.v < 0))
                        ? (uint64_t)0 - (uint64_t)ops_min_signed(width)
                        : (uint64_t)ops_max_signed(width);
            /* |a| * |b| > limit  <=>  mb > limit / ma (ma != 0) */
            if (mb > limit / ma) return EXPR_OP_OVERFLOW;
            return EXPR_OP_OK;
        }
        {
            uint64_t av = (uint64_t)a.v, bv = (uint64_t)b.v;
            uint64_t maxv = ops_max_unsigned(width);
            if (av != 0 && bv > maxv / av) return EXPR_OP_OVERFLOW;
            return EXPR_OP_OK;
        }
    case AST_BIN_DIV:
    case AST_BIN_MOD:
        if (is_signed) {
            if (b.v == 0) return EXPR_OP_DIV_ZERO;
            if (a.v == ops_min_signed(width) && b.v == -1) {
                return EXPR_OP_OVERFLOW;   /* sec. 11.3: min / -1 */
            }
            return EXPR_OP_OK;
        }
        if ((uint64_t)b.v == 0) return EXPR_OP_DIV_ZERO;
        return EXPR_OP_OK;
    case AST_BIN_SHL:
    case AST_BIN_SHR:
        /* count must be in 0..width-1 (sec. 11.3) */
        if (b.big || b.v < 0 || b.v >= width) return EXPR_OP_SHIFT_RANGE;
        return EXPR_OP_OK;
    default:
        /* bitwise, comparisons, logical: never fail per sec. 11.3 */
        return EXPR_OP_OK;
    }
}

ExprOpDecision expr_op_neg_decision(EvalInt a, int width, bool is_signed)
{
    if (width <= 0 || width > 64) return EXPR_OP_OK;  /* defensive */
    if (!is_signed) return EXPR_OP_OK;  /* unsigned negation: 11d rejects */
    if (a.v == ops_min_signed(width)) return EXPR_OP_OVERFLOW;
    return EXPR_OP_OK;
}

EvalInt expr_op_shift_value(AstBinaryOp op, EvalInt a, EvalInt count,
                            int width, bool is_signed)
{
    uint64_t raw, shifted;
    int64_t c = count.v;
    if (width <= 0 || width > 64 || count.big || c < 0 || c >= width) {
        return a;   /* defensive: callers check validity first */
    }
    raw = ops_int_raw(&a, width);
    if (op == AST_BIN_SHL) {
        /* defined on the two's-complement bit pattern (sec. 11.3) */
        shifted = raw << c;
        if (width < 64) shifted &= ops_max_unsigned(width);
        return ops_int_from_raw(shifted, width, is_signed);
    }
    if (is_signed) {
        /* arithmetic (sign-extending) right shift (sec. 11.3) */
        EvalInt sx = ops_int_from_raw(raw, width, true);
        shifted = (uint64_t)ops_i64_ashr(sx.v, (int)c);
        if (width < 64) shifted &= ops_max_unsigned(width);
        return ops_int_from_raw(shifted, width, is_signed);
    }
    /* logical right shift */
    shifted = raw >> c;
    return ops_int_from_raw(shifted, width, false);
}

/* ---------------------------------------------------------------------------
 * Comparison semantics (spec sec. 11.4)
 * ------------------------------------------------------------------------- */

ExprCmpKind expr_cmp_kind_of(const Type *type)
{
    if (!type) return EXPR_CMP_INT_MATH;
    switch (type->kind) {
    case TYPE_PRIM: {
        const TypePrimInfo *p = types_prim_info(type->u.prim);
        if (!p) return EXPR_CMP_INT_MATH;
        if (p->kind == AST_PRIM_BOOL) return EXPR_CMP_BOOL_VALUE;
        if (p->kind == AST_PRIM_STR) return EXPR_CMP_STR_BYTES;
        return EXPR_CMP_INT_MATH;   /* integers; void defensively */
    }
    case TYPE_ENUM:
        return EXPR_CMP_ENUM_UNDERLYING;
    case TYPE_PTR:
        return EXPR_CMP_PTR_ADDR;
    case TYPE_SLICE:
        return EXPR_CMP_SLICE_ELEMS;
    default:
        /* array / struct: comparisons are rejected by 11d (AIC-T0304);
         * defensive mechanism */
        return EXPR_CMP_INT_MATH;
    }
}

int expr_cmp_ints(EvalInt a, EvalInt b, int width, bool is_signed)
{
    (void)width;
    if (is_signed) {
        if (a.big) return 1;   /* >= 2^63 always above any int64 */
        if (b.big) return -1;
        return a.v < b.v ? -1 : (a.v > b.v ? 1 : 0);
    }
    {
        uint64_t av = (uint64_t)a.v, bv = (uint64_t)b.v;
        return av < bv ? -1 : (av > bv ? 1 : 0);
    }
}

int expr_cmp_bytes(const void *a, size_t an, const void *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    if (n) {
        int c = memcmp(a, b, n);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

bool expr_cmp_slice_equal(int64_t alen, const void *a, size_t elem_size,
                          int64_t blen, const void *b,
                          bool (*elem_eq)(const void *x, const void *y,
                                          void *ud),
                          void *ud)
{
    int64_t i;
    if (alen != blen) return false;   /* length mismatch -> not equal */
    if (!elem_eq) return alen == 0;   /* defensive */
    for (i = 0; i < alen; i++) {
        const void *x = (const uint8_t *)a + (size_t)i * elem_size;
        const void *y = (const uint8_t *)b + (size_t)i * elem_size;
        if (!elem_eq(x, y, ud)) return false;
    }
    return true;
}

bool expr_cmp_addr(const NameSymbol *a_sym, int64_t a_off,
                   const NameSymbol *b_sym, int64_t b_off, int *out)
{
    if (a_sym != b_sym) return false;  /* distinct objects: link-time */
    if (out) *out = a_off < b_off ? -1 : (a_off > b_off ? 1 : 0);
    return true;
}

/* ---------------------------------------------------------------------------
 * Operator-site check: failure-record emission at the 13b1-owned
 * const-context sites (array extents, case labels, local const
 * initializers)
 * ------------------------------------------------------------------------- */

typedef struct OpsCtx {
    EvalCtx ev;
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;
    bool unsupported;
} OpsCtx;

/* Render an EvalInt deterministically: big values (two's complement in
 * the int64 field, [2^63, 2^64-1]) as unsigned, others as signed
 * (the WP-M0-12b2 render_eval_int convention). */
static void ops_render_eval_int(const EvalInt *v, char *buf, size_t sz)
{
    if (v->big) {
        snprintf(buf, sz, "%llu", (unsigned long long)(uint64_t)v->v);
    } else {
        snprintf(buf, sz, "%lld", (long long)v->v);
    }
}

/* The message text for a typed const failure (the WP-M0-12b2 strings,
 * pinned by the negative-corpus anchors). */
static const char *ops_message(const RecFailValue *v, char *buf, size_t sz)
{
    char nbuf[32];
    switch (v->kind) {
    case EVAL_FAIL_OVERFLOW:
        return "constant expression overflow";
    case EVAL_FAIL_DIV_ZERO:
        return "constant division by zero";
    case EVAL_FAIL_SHIFT_RANGE:
        ops_render_eval_int(&v->b, nbuf, sizeof(nbuf));
        snprintf(buf, sz, "constant shift count out of range: %s exceeds "
                          "%s bit width",
                 nbuf, v->op_type[0] ? v->op_type : "int");
        return buf;
    case EVAL_FAIL_CAST_RANGE:
        ops_render_eval_int(&v->cast_value, nbuf, sizeof(nbuf));
        snprintf(buf, sz, "constant cast out of range: %s does not fit in %s",
                 nbuf, v->cast_target[0] ? v->cast_target : "?");
        return buf;
    case EVAL_FAIL_INDEX_RANGE:
        ops_render_eval_int(&v->bound_value, nbuf, sizeof(nbuf));
        if (v->is_slice_bound) {
            snprintf(buf, sz, "constant slice bound %s out of range for "
                              "extent %lld",
                     nbuf, (long long)v->extent);
        } else if (v->extent >= 0) {
            snprintf(buf, sz, "constant index %s out of range for array of "
                              "length %lld",
                     nbuf, (long long)v->extent);
        } else {
            snprintf(buf, sz, "constant index %s out of range", nbuf);
        }
        return buf;
    case EVAL_FAIL_STR_BOUNDARY:
        return "constant str slice not on code point boundary";
    case EVAL_FAIL_PTR_DIFF:
        return "constant pointer difference not divisible by element size";
    default:
        snprintf(buf, sz, "constant expression failure");
        return buf;
    }
}

/* One authoritative semantic record for a typed failure. E0405/E0406
 * primary span = the whole constant expression (site); the other codes
 * primary span = the located node per the contract table
 * (DIAGNOSTIC-CONTRACT sec. 11.5). */
static DiagRecord *ops_build_record(OpsCtx *c, const RecFailValue *v)
{
    char msg[256];
    const char *text = ops_message(v, msg, sizeof(msg));
    const AstNode *span_node;
    DiagRecord *r;
    if (v->kind == EVAL_FAIL_OVERFLOW || v->kind == EVAL_FAIL_DIV_ZERO) {
        span_node = v->site;
    } else {
        span_node = v->op_node ? v->op_node : v->site;
    }
    r = diag_record_new();
    if (!r) { c->oom = true; return NULL; }
    if (!diag_record_set_code(r, v->code) ||
        !diag_record_set_message(r, text) ||
        !diag_record_set_primary_span(r, span_node ? span_node->span : NULL) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        c->oom = true;
        return NULL;
    }
    return r;
}

static bool ops_push_record(OpsCtx *c, DiagRecord *r)
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

/* Evaluate one const-context site owned by 13b1: EVAL_FAILURE sites
 * are classified (12b2 public API) and emitted as AIC-E0405..E0411;
 * EVAL_NOT_CONST sites yield no record (13b1 owns AIC-E0401). */
static void ops_check_site(OpsCtx *c, const AstNode *expr)
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
    if (st == EVAL_FAILURE) {
        RecFailValue rf;
        RecFailStatus rst = rec_fail_classify(&c->ev, expr, fail, &rf);
        if (rst == REC_FAIL_FAILURE) {
            DiagRecord *r = ops_build_record(c, &rf);
            if (r) ops_push_record(c, r);
        } else if (rst == REC_FAIL_OOM) {
            c->ev.oom = true;
        } else if (rst == REC_FAIL_UNSUPPORTED) {
            c->unsupported = true;
        }
        /* REC_FAIL_NOT_CONST: defensive; no record */
        return;
    }
    if (st == EVAL_NOT_CONST) {
        /* 13b1 owns AIC-E0401; no record here */
        return;
    }
    if (st == EVAL_OOM) return;   /* eval_core set ctx->oom */
    /* EVAL_UNSUPPORTED: defensive; no record */
    c->unsupported = true;
}

static bool ops_walk_expr(OpsCtx *c, const NameModule *module,
                          const AstNode *e);
static bool ops_walk_type(OpsCtx *c, const NameModule *module,
                          const AstNode *type_node);

/* Recursively walk an expression, descending into sub-expressions and
 * type operands (cast/wrap targets, sizeof/alignof type operands). No
 * expression itself is a const-context site of this package; the sites
 * are discovered by the statement walker (case labels, local const
 * initializers) and the type walker (array extents). Mirrors the
 * WP-M0-13b1 walk so the two stages visit the same sites in the same
 * order. */
static bool ops_walk_expr(OpsCtx *c, const NameModule *module,
                          const AstNode *e)
{
    size_t i;
    if (!e || c->oom) return true;
    switch (e->kind) {
    case AST_EXPR_ASSIGN:
        if (!ops_walk_expr(c, module, e->u.assign.target)) return false;
        return ops_walk_expr(c, module, e->u.assign.value);
    case AST_EXPR_UNARY:
        return ops_walk_expr(c, module, e->u.unary.operand);
    case AST_EXPR_BINARY:
        if (!ops_walk_expr(c, module, e->u.binary.lhs)) return false;
        return ops_walk_expr(c, module, e->u.binary.rhs);
    case AST_EXPR_TERNARY:
        if (!ops_walk_expr(c, module, e->u.branch.cond)) return false;
        if (!ops_walk_expr(c, module, e->u.branch.then)) return false;
        return ops_walk_expr(c, module, e->u.branch.els);
    case AST_EXPR_INDEX:
        if (!ops_walk_expr(c, module, e->u.index_slice.base)) return false;
        return ops_walk_expr(c, module, e->u.index_slice.index);
    case AST_EXPR_SLICE:
        if (!ops_walk_expr(c, module, e->u.index_slice.base)) return false;
        if (!ops_walk_expr(c, module, e->u.index_slice.lo)) return false;
        return ops_walk_expr(c, module, e->u.index_slice.hi);
    case AST_EXPR_CALL:
        if (!ops_walk_expr(c, module, e->u.call.callee)) return false;
        for (i = 0; i < e->u.call.nargs; i++) {
            if (!ops_walk_expr(c, module, e->u.call.args[i])) return false;
        }
        return true;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        return ops_walk_expr(c, module, e->u.member.base);
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < e->u.array_literal.nelems; i++) {
            if (!ops_walk_expr(c, module, e->u.array_literal.elems[i])) {
                return false;
            }
        }
        return ops_walk_expr(c, module, e->u.array_literal.count);
    case AST_EXPR_STRUCT_INIT:
        if (!ops_walk_expr(c, module, e->u.struct_init.base)) return false;
        for (i = 0; i < e->u.struct_init.nfields; i++) {
            const AstNode *fi = e->u.struct_init.fields[i];
            if (fi && fi->kind == AST_FIELD_INIT) {
                if (!ops_walk_expr(c, module, fi->u.named.value)) return false;
            }
        }
        return true;
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF:
        return ops_walk_type(c, module, e->u.size_op.operand);
    case AST_EXPR_SIZEOF_EXPR:
        return ops_walk_expr(c, module, e->u.size_op.operand);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        if (!ops_walk_type(c, module, e->u.cast_wrap.type)) return false;
        return ops_walk_expr(c, module, e->u.cast_wrap.expr);
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        return ops_walk_expr(c, module, e->u.size_op.operand);
    case AST_EXPR_PAREN:
        return ops_walk_expr(c, module, e->u.paren.expr);
    case AST_EXPR_INT_LITERAL:
    case AST_EXPR_STR_LITERAL:
    case AST_EXPR_BOOL_LITERAL:
    case AST_EXPR_NULL_LITERAL:
    case AST_EXPR_IDENT:
    default:
        return true;
    }
}

/* Walk an AST type node: every array extent is a const-context site
 * (sec. 12.1). */
static bool ops_walk_type(OpsCtx *c, const NameModule *module,
                          const AstNode *type_node)
{
    if (!type_node || c->oom) return true;
    switch (type_node->kind) {
    case AST_TYPE_ARRAY:
        if (type_node->u.type_derived.len) {
            ops_check_site(c, type_node->u.type_derived.len);
        }
        return ops_walk_type(c, module, type_node->u.type_derived.base);
    case AST_TYPE_PTR:
    case AST_TYPE_SLICE:
        return ops_walk_type(c, module, type_node->u.type_derived.base);
    default:
        return true;
    }
}

/* Recursively walk a statement, checking local const initializers and
 * case labels (the const-context sites of this package that live in
 * function bodies). */
static bool ops_walk_stmt(OpsCtx *c, const NameModule *module,
                          const AstNode *s)
{
    size_t i;
    if (!s || c->oom) return true;
    switch (s->kind) {
    case AST_BLOCK:
        for (i = 0; i < s->u.list.count; i++) {
            if (!ops_walk_stmt(c, module, s->u.list.items[i])) return false;
        }
        return true;
    case AST_VAR_DECL:
        if (!ops_walk_type(c, module, s->u.local_decl.type)) return false;
        return ops_walk_expr(c, module, s->u.local_decl.init);
    case AST_CONST_DECL:
        /* local const initializer must be a constant expression
         * (sec. 8.1); failures are this package's records */
        if (!ops_walk_type(c, module, s->u.local_decl.type)) return false;
        if (s->u.local_decl.init) ops_check_site(c, s->u.local_decl.init);
        return true;
    case AST_IF:
        if (!ops_walk_expr(c, module, s->u.branch.cond)) return false;
        if (!ops_walk_stmt(c, module, s->u.branch.then)) return false;
        return ops_walk_stmt(c, module, s->u.branch.els);
    case AST_WHILE:
        if (!ops_walk_expr(c, module, s->u.while_loop.cond)) return false;
        return ops_walk_stmt(c, module, s->u.while_loop.body);
    case AST_FOR:
        if (s->u.for_loop.init) {
            if (s->u.for_loop.init->kind == AST_VAR_DECL ||
                s->u.for_loop.init->kind == AST_CONST_DECL) {
                if (!ops_walk_stmt(c, module, s->u.for_loop.init)) return false;
            } else {
                if (!ops_walk_expr(c, module, s->u.for_loop.init)) return false;
            }
        }
        if (!ops_walk_expr(c, module, s->u.for_loop.cond)) return false;
        if (!ops_walk_expr(c, module, s->u.for_loop.step)) return false;
        return ops_walk_stmt(c, module, s->u.for_loop.body);
    case AST_SWITCH:
        if (!ops_walk_expr(c, module, s->u.switch_stmt.selector)) return false;
        for (i = 0; i < s->u.switch_stmt.ncases; i++) {
            const AstNode *cl = s->u.switch_stmt.cases[i];
            if (!cl) continue;
            if (cl->kind == AST_CASE_CLAUSE) {
                /* each case label is a constant expression (sec. 13.2);
                 * the whole-label check subsumes nested array extents
                 * (a nested non-const extent makes the whole label
                 * non-const), so the label expression is not descended
                 * into separately - that would duplicate records */
                if (cl->u.clause.value) {
                    ops_check_site(c, cl->u.clause.value);
                }
            }
            if (!ops_walk_stmt(c, module, cl->u.clause.body)) return false;
        }
        return true;
    case AST_RETURN:
        return ops_walk_expr(c, module, s->u.ret.value);
    case AST_EXPR_STMT:
        return ops_walk_expr(c, module, s->u.expr_stmt.expr);
    case AST_EMPTY_STMT:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_CASE_CLAUSE:
    case AST_DEFAULT_CLAUSE:
    default:
        return true;
    }
}

/* Walk one module-scope declaration. Global var/const initializers and
 * enum member value expressions are WP-M0-12a sites and are NOT walked
 * here; this package walks only the type positions (array extents) and
 * function bodies (local consts, case labels, nested types). */
static bool ops_walk_decl(OpsCtx *c, const NameModule *module,
                          const AstNode *decl)
{
    size_t i;
    if (!decl || c->oom) return true;
    switch (decl->kind) {
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        return ops_walk_type(c, module, decl->u.global_decl.type);
    case AST_ENUM_DECL:
        return true;   /* member values: 12a's site */
    case AST_STRUCT_DECL:
        for (i = 0; i < decl->u.struct_decl.nfields; i++) {
            const AstNode *f = decl->u.struct_decl.fields[i];
            if (f) {
                if (!ops_walk_type(c, module, f->u.named.type)) return false;
            }
        }
        return true;
    case AST_FN_DECL:
        for (i = 0; i < decl->u.fn_decl.nparams; i++) {
            const AstNode *p = decl->u.fn_decl.params[i];
            if (p) {
                if (!ops_walk_type(c, module, p->u.named.type)) return false;
            }
        }
        if (!ops_walk_type(c, module, decl->u.fn_decl.ret_type)) return false;
        return ops_walk_stmt(c, module, decl->u.fn_decl.body);
    default:
        return true;
    }
}

ExprOpsStatus expr_ops_check(const NameResult *result,
                             const LayoutBuild *layout,
                             DiagRecord ***out_records,
                             size_t *out_record_count)
{
    OpsCtx c;
    size_t m;
    if (!result || !layout) return EXPR_OPS_UNSUPPORTED;
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
            if (!ops_walk_decl(&c, mod, sym->decl)) break;
        }
        if (c.ev.oom || c.oom) break;
    }
    if (c.ev.oom || c.oom) {
        size_t i;
        for (i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
        free(c.records);
        eval_ctx_cleanup(&c.ev);
        return EXPR_OPS_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    eval_ctx_cleanup(&c.ev);
    if (c.nrecords) return EXPR_OPS_DIAG_ERROR;
    if (c.unsupported) return EXPR_OPS_UNSUPPORTED;
    return EXPR_OPS_OK;
}
