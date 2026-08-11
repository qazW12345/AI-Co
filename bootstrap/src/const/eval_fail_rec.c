/* bootstrap/src/const/eval_fail_rec.c
 *
 * AI-Co Stage-0 const-failure record emission and remaining failure
 * sites (WP-M0-12b2).
 *
 * Emits the deterministic failure records AIC-E0405 .. AIC-E0411 for
 * const-context sites (spec sec. 10.5), in the walk order of the
 * WP-M0-12a const-context check, sorted per the DIAGNOSTIC-CONTRACT
 * sec. 9 comparator. The three arithmetic kinds are typed by the
 * WP-M0-12b1 package (arith_fail_classify); this package locates the
 * failing node for the remaining four kinds (cast-range, index/slice
 * bound, str-slice code-point boundary, pointer-difference
 * divisibility), captures the message facts, and builds the records.
 *
 * The never-trap guarantee of 12a/12b1 holds here: sub-expressions
 * are re-evaluated through const_eval_expr (which never performs the
 * failing operation) and this package performs no failing operation
 * itself (no division by a value that could be zero, no unchecked
 * shift, no negation of a minimum, no trap-casting).
 *
 * Span/message conventions are documented in eval_fail_rec.h; the
 * failing node is located by descending in the evaluator's own order
 * (sec. 10.4) into the first sub-expression that fails with the
 * routed kind.
 */
#include "eval_fail_rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Kind -> code mapping (AIC-E0405 .. E0411)
 * ------------------------------------------------------------------------- */

const char *rec_fail_code(EvalFailure kind)
{
    switch (kind) {
    case EVAL_FAIL_OVERFLOW:    return "AIC-E0405";
    case EVAL_FAIL_DIV_ZERO:    return "AIC-E0406";
    case EVAL_FAIL_SHIFT_RANGE: return "AIC-E0407";
    case EVAL_FAIL_CAST_RANGE:  return "AIC-E0408";
    case EVAL_FAIL_INDEX_RANGE: return "AIC-E0409";
    case EVAL_FAIL_STR_BOUNDARY:return "AIC-E0410";
    case EVAL_FAIL_PTR_DIFF:    return "AIC-E0411";
    default:
        return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

/* Map an EvalStatus from a sub-expression re-evaluation to the
 * classification status (defensive paths). */
static RecFailStatus map_eval_status(EvalStatus st)
{
    switch (st) {
    case EVAL_OOM:       return REC_FAIL_OOM;
    case EVAL_NOT_CONST: return REC_FAIL_NOT_CONST;
    default:             return REC_FAIL_UNSUPPORTED;
    }
}

/* Render an EvalInt deterministically: big values (two's complement in
 * the int64 field, [2^63, 2^64-1]) as unsigned, others as signed. */
static void render_eval_int(const EvalInt *v, char *buf, size_t sz)
{
    if (v->big) {
        snprintf(buf, sz, "%llu", (unsigned long long)(uint64_t)v->v);
    } else {
        snprintf(buf, sz, "%lld", (long long)v->v);
    }
}

/* Fallback type-name rendering from width/signedness when the actual
 * type is unavailable (defensive; the prim path is authoritative). */
static void render_width_type(int width, bool is_signed, char *buf, size_t sz)
{
    const char *base = is_signed ? "i" : "u";
    if (width == 8 || width == 16 || width == 32 || width == 64) {
        snprintf(buf, sz, "%s%d", base, width);
    } else {
        snprintf(buf, sz, "int");
    }
}

/* Render a target type name from an AST type node ("i8", "Color",
 * "i32*", "u8[]", "i32[N]"); "?" for unrenderable/unknown nodes. */
static void render_ast_type_name(const AstNode *tn, char *buf, size_t sz)
{
    char tmp[64];
    if (!tn) {
        snprintf(buf, sz, "?");
        return;
    }
    switch (tn->kind) {
    case AST_TYPE_PRIM: {
        const TypePrimInfo *p = types_prim_info(tn->u.type_prim.prim);
        snprintf(buf, sz, "%s", p && p->name ? p->name : "?");
        return;
    }
    case AST_TYPE_PTR:
        render_ast_type_name(tn->u.type_derived.base, tmp, sizeof(tmp));
        snprintf(buf, sz, "%s*", tmp);
        return;
    case AST_TYPE_SLICE:
        render_ast_type_name(tn->u.type_derived.base, tmp, sizeof(tmp));
        snprintf(buf, sz, "%s[]", tmp);
        return;
    case AST_TYPE_ARRAY:
        render_ast_type_name(tn->u.type_derived.base, tmp, sizeof(tmp));
        snprintf(buf, sz, "%s[N]", tmp);
        return;
    case AST_TYPE_NAMED: {
        char *s = tn->u.type_named.name
                      ? ast_name_to_string(tn->u.type_named.name)
                      : NULL;
        snprintf(buf, sz, "%s", s ? s : "?");
        free(s);
        return;
    }
    default:
        snprintf(buf, sz, "?");
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Extent helpers (facts for E0409 messages)
 * ------------------------------------------------------------------------- */

/* Array extent (length) of a global-var ident of array type, derived
 * from the declaration's type-length expression (the same source the
 * evaluator uses); -1 when not derivable. */
static int64_t array_extent_of_base(EvalCtx *ctx, const AstNode *base)
{
    const NameSymbol *sym;
    const NameModule *saved;
    EvalValue ev;
    EvalFailure ef = EVAL_FAIL_NONE;
    EvalStatus est;
    if (!base || base->kind != AST_EXPR_IDENT) return -1;
    sym = name_symbol_for_node(ctx->module, base);
    if (!sym || sym->kind != NAME_SYM_GLOBAL_VAR || !sym->decl ||
        sym->decl->kind != AST_GLOBAL_VAR_DECL ||
        !sym->decl->u.global_decl.type ||
        sym->decl->u.global_decl.type->kind != AST_TYPE_ARRAY ||
        !sym->decl->u.global_decl.type->u.type_derived.len) {
        return -1;
    }
    saved = ctx->module;
    ctx->module = sym->module;
    est = const_eval_expr(ctx,
                          sym->decl->u.global_decl.type->u.type_derived.len,
                          &ev, &ef);
    ctx->module = saved;
    if (est == EVAL_OK && ev.kind == EVAL_VAL_INT &&
        !ev.u.i.big && ev.u.i.v >= 0) {
        int64_t ext = ev.u.i.v;
        eval_value_free(&ev);
        return ext;
    }
    if (est == EVAL_OK) eval_value_free(&ev);
    return -1;
}

/* Extent of a slice expression's base: the byte length for a str base
 * (literal or const reference) or the array length for a static array
 * base; -1 when not derivable. */
static int64_t slice_extent(EvalCtx *ctx, const AstNode *slice)
{
    const AstNode *base;
    if (!slice) return -1;
    base = slice->u.index_slice.base;
    if (!base) return -1;
    if (base->kind == AST_EXPR_STR_LITERAL) {
        return (int64_t)base->u.str_literal.len;
    }
    if (base->kind == AST_EXPR_IDENT) {
        const NameSymbol *sym = name_symbol_for_node(ctx->module, base);
        if (sym && (sym->kind == NAME_SYM_GLOBAL_CONST ||
                    sym->kind == NAME_SYM_LOCAL_CONST)) {
            EvalValue bv;
            EvalFailure bf = EVAL_FAIL_NONE;
            EvalStatus bst = const_eval_expr(ctx, base, &bv, &bf);
            if (bst == EVAL_OK && bv.kind == EVAL_VAL_STR) {
                int64_t len = (int64_t)bv.u.str.len;
                eval_value_free(&bv);
                return len;
            }
            if (bst == EVAL_OK) eval_value_free(&bv);
            return -1;
        }
        return array_extent_of_base(ctx, base);
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Failure location (evaluator-order descent)
 * ------------------------------------------------------------------------- */

/* Locate the failing node inside `node`, which is known to evaluate
 * to EVAL_FAILURE with `kind` (the routed kind of the site). Follows
 * the evaluator's order (children before the operation, left before
 * right; slice base then lo then hi; the index expression inside the
 * &arr[i] address form). On success fills out->op_node (and the
 * kind-specific facts when the located node carries them) and returns
 * REC_FAIL_FAILURE. */
static RecFailStatus locate_rec_failure(EvalCtx *ctx, const AstNode *node,
                                        EvalFailure kind, RecFailValue *out)
{
    EvalStatus st;
    if (!ctx || !node) return REC_FAIL_UNSUPPORTED;

    switch (node->kind) {
    case AST_EXPR_PAREN:
        return locate_rec_failure(ctx, node->u.paren.expr, kind, out);

    case AST_EXPR_BINARY: {
        EvalValue lv, rv;
        EvalFailure lf = EVAL_FAIL_NONE, rf = EVAL_FAIL_NONE;
        st = const_eval_expr(ctx, node->u.binary.lhs, &lv, &lf);
        if (st == EVAL_FAILURE) {
            return locate_rec_failure(ctx, node->u.binary.lhs, lf, out);
        }
        if (st != EVAL_OK) return map_eval_status(st);
        st = const_eval_expr(ctx, node->u.binary.rhs, &rv, &rf);
        if (st == EVAL_FAILURE) {
            eval_value_free(&lv);
            return locate_rec_failure(ctx, node->u.binary.rhs, rf, out);
        }
        if (st != EVAL_OK) {
            eval_value_free(&lv);
            return map_eval_status(st);
        }
        /* both operands evaluate: this node's own operator failed
         * (pointer-difference divisibility for E0411) */
        eval_value_free(&lv);
        eval_value_free(&rv);
        out->op_node = node;
        out->op = node->u.binary.op;
        out->is_unary = false;
        return REC_FAIL_FAILURE;
    }

    case AST_EXPR_UNARY: {
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        if (node->u.unary.op == AST_UN_ADDR) {
            const AstNode *operand = node->u.unary.operand;
            if (operand && operand->kind == AST_EXPR_INDEX) {
                st = const_eval_expr(ctx, operand->u.index_slice.index,
                                     &v, &vf);
                if (st == EVAL_FAILURE) {
                    return locate_rec_failure(ctx,
                                              operand->u.index_slice.index,
                                              vf, out);
                }
                if (st == EVAL_OK) {
                    if (kind == EVAL_FAIL_INDEX_RANGE) {
                        /* the bounds check on the index failed: the
                         * bound is the index expression (contract
                         * primary span "the bound") */
                        out->op_node = operand->u.index_slice.index;
                        out->is_slice_bound = false;
                        if (v.kind == EVAL_VAL_INT) out->bound_value = v.u.i;
                        out->extent = array_extent_of_base(
                            ctx, operand->u.index_slice.base);
                        eval_value_free(&v);
                        return REC_FAIL_FAILURE;
                    }
                    eval_value_free(&v);
                } else if (st == EVAL_OOM) {
                    return REC_FAIL_OOM;
                } else {
                    return map_eval_status(st);
                }
            }
            /* address form without an index (or a non-index-routed
             * failure inside the address): the failing node is the
             * address expression itself */
            out->op_node = node;
            return REC_FAIL_FAILURE;
        }
        st = const_eval_expr(ctx, node->u.unary.operand, &v, &vf);
        if (st == EVAL_FAILURE) {
            return locate_rec_failure(ctx, node->u.unary.operand, vf, out);
        }
        if (st != EVAL_OK) return map_eval_status(st);
        eval_value_free(&v);
        out->op_node = node;
        out->is_unary = true;
        return REC_FAIL_FAILURE;
    }

    case AST_EXPR_CAST:
    case AST_EXPR_WRAP: {
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        /* the cast's own failure is CAST_RANGE; a different kind must
         * be inside the source expression */
        st = const_eval_expr(ctx, node->u.cast_wrap.expr, &v, &vf);
        if (st == EVAL_FAILURE) {
            return locate_rec_failure(ctx, node->u.cast_wrap.expr, vf, out);
        }
        if (st != EVAL_OK) return map_eval_status(st);
        if (kind == EVAL_FAIL_CAST_RANGE) {
            if (v.kind == EVAL_VAL_INT) out->cast_value = v.u.i;
            render_ast_type_name(node->u.cast_wrap.type, out->cast_target,
                                 sizeof(out->cast_target));
        }
        eval_value_free(&v);
        out->op_node = node;
        return REC_FAIL_FAILURE;
    }

    case AST_EXPR_IDENT:
    case AST_EXPR_MEMBER:
        /* const reference: typed at the reference node (documented
         * convention; the referenced const's own site carries the
         * precise failing operator) */
        out->op_node = node;
        return REC_FAIL_FAILURE;

    case AST_EXPR_ARRAY_LITERAL: {
        size_t i;
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        if (node->u.array_literal.count) {
            st = const_eval_expr(ctx, node->u.array_literal.count, &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_rec_failure(ctx, node->u.array_literal.count,
                                          vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        for (i = 0; i < node->u.array_literal.nelems; i++) {
            st = const_eval_expr(ctx, node->u.array_literal.elems[i], &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_rec_failure(ctx, node->u.array_literal.elems[i],
                                          vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        out->op_node = node;
        return REC_FAIL_FAILURE;
    }

    case AST_EXPR_STRUCT_INIT: {
        size_t i;
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        for (i = 0; i < node->u.struct_init.nfields; i++) {
            const AstNode *fi = node->u.struct_init.fields[i];
            if (!fi || !fi->u.named.value) {
                out->op_node = node;
                return REC_FAIL_FAILURE;
            }
            st = const_eval_expr(ctx, fi->u.named.value, &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_rec_failure(ctx, fi->u.named.value, vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        out->op_node = node;
        return REC_FAIL_FAILURE;
    }

    case AST_EXPR_SLICE: {
        const AstNode *base = node->u.index_slice.base;
        EvalValue lv = { 0 }, hv = { 0 };
        EvalFailure lf = EVAL_FAIL_NONE, hf = EVAL_FAIL_NONE;
        bool lo_ok = false, hi_ok = false;
        /* evaluator order (sec. 10.4): base, then lo, then hi. A const
         * reference base is the only base form whose evaluation can
         * fail with a routed kind. */
        if (base && base->kind == AST_EXPR_IDENT) {
            const NameSymbol *bsym = name_symbol_for_node(ctx->module, base);
            if (bsym && (bsym->kind == NAME_SYM_GLOBAL_CONST ||
                         bsym->kind == NAME_SYM_LOCAL_CONST)) {
                EvalValue bv;
                EvalFailure bf = EVAL_FAIL_NONE;
                st = const_eval_expr(ctx, base, &bv, &bf);
                if (st == EVAL_FAILURE) {
                    return locate_rec_failure(ctx, base, bf, out);
                }
                if (st == EVAL_OK) eval_value_free(&bv);
                else if (st == EVAL_OOM) return REC_FAIL_OOM;
                else return map_eval_status(st);
            }
        }
        if (node->u.index_slice.lo) {
            st = const_eval_expr(ctx, node->u.index_slice.lo, &lv, &lf);
            if (st == EVAL_FAILURE) {
                return locate_rec_failure(ctx, node->u.index_slice.lo, lf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            lo_ok = true;
        }
        if (node->u.index_slice.hi) {
            st = const_eval_expr(ctx, node->u.index_slice.hi, &hv, &hf);
            if (st == EVAL_FAILURE) {
                if (lo_ok) eval_value_free(&lv);
                return locate_rec_failure(ctx, node->u.index_slice.hi, hf, out);
            }
            if (st != EVAL_OK) {
                if (lo_ok) eval_value_free(&lv);
                return map_eval_status(st);
            }
            hi_ok = true;
        }
        /* both bounds evaluate: the slice's own check failed */
        out->op_node = node;
        if (kind == EVAL_FAIL_INDEX_RANGE) {
            /* offending bound, deterministic priority (header
             * convention): negative lo -> lo; negative hi -> hi;
             * lo > hi -> lo when explicit else hi; hi > extent -> hi
             * when explicit else the slice node; else lo when
             * explicit */
            const AstNode *lo = node->u.index_slice.lo;
            const AstNode *hi = node->u.index_slice.hi;
            const AstNode *bound = node;
            const EvalInt *bval = NULL;
            int64_t extent = slice_extent(ctx, node);
            int64_t lo64 = 0, hi64 = extent;
            if (lo_ok && lv.kind == EVAL_VAL_INT && !lv.u.i.big) lo64 = lv.u.i.v;
            if (hi_ok && hv.kind == EVAL_VAL_INT && !hv.u.i.big) hi64 = hv.u.i.v;
            if (lo && lo_ok && lv.kind == EVAL_VAL_INT &&
                (lv.u.i.big || lv.u.i.v < 0)) {
                bound = lo;
                bval = &lv.u.i;
            } else if (hi && hi_ok && hv.kind == EVAL_VAL_INT &&
                       (hv.u.i.big || hv.u.i.v < 0)) {
                bound = hi;
                bval = &hv.u.i;
            } else if (lo64 > hi64) {
                bound = lo ? lo : (hi ? hi : node);
                if (lo && lo_ok && lv.kind == EVAL_VAL_INT) bval = &lv.u.i;
                else if (hi && hi_ok && hv.kind == EVAL_VAL_INT) bval = &hv.u.i;
            } else if (hi64 > extent) {
                bound = hi ? hi : node;
                if (hi && hi_ok && hv.kind == EVAL_VAL_INT) bval = &hv.u.i;
            } else if (lo) {
                bound = lo;
                if (lo_ok && lv.kind == EVAL_VAL_INT) bval = &lv.u.i;
            }
            out->op_node = bound;
            out->is_slice_bound = true;
            if (bval) out->bound_value = *bval;
            out->extent = extent;
        }
        if (kind == EVAL_FAIL_STR_BOUNDARY) {
            if (lo_ok && lv.kind == EVAL_VAL_INT) out->slice_lo = lv.u.i.v;
            if (hi_ok && hv.kind == EVAL_VAL_INT) out->slice_hi = hv.u.i.v;
        }
        if (lo_ok) eval_value_free(&lv);
        if (hi_ok) eval_value_free(&hv);
        return REC_FAIL_FAILURE;
    }

    default:
        /* literal / sizeof / alignof / other leaf: the failing node is
         * the site itself (defensive; these cannot fail on a valid
         * build) */
        out->op_node = node;
        return REC_FAIL_FAILURE;
    }
}

/* ---------------------------------------------------------------------------
 * Classification
 * ------------------------------------------------------------------------- */

/* E0407 message fact: the shift left operand's type name. Re-evaluates
 * the shift's left operand (it already evaluated OK during location)
 * and renders its primitive name; falls back to width/signedness. */
static void rec_fill_shift_type(EvalCtx *ctx, const RecFailValue *v,
                                char *buf, size_t sz)
{
    const AstNode *opn = v->op_node;
    if (opn && opn->kind == AST_EXPR_BINARY &&
        (opn->u.binary.op == AST_BIN_SHL ||
         opn->u.binary.op == AST_BIN_SHR)) {
        EvalValue lv;
        EvalFailure lf = EVAL_FAIL_NONE;
        EvalStatus st = const_eval_expr(ctx, opn->u.binary.lhs, &lv, &lf);
        if (st == EVAL_OK && lv.type && lv.type->kind == TYPE_PRIM) {
            const TypePrimInfo *p = types_prim_info(lv.type->u.prim);
            if (p && p->name) {
                snprintf(buf, sz, "%s", p->name);
                eval_value_free(&lv);
                return;
            }
        }
        if (st == EVAL_OK) eval_value_free(&lv);
    }
    render_width_type(v->width, v->is_signed, buf, sz);
}

RecFailStatus rec_fail_classify(EvalCtx *ctx, const AstNode *site,
                                EvalFailure kind, RecFailValue *out)
{
    const char *code;
    RecFailStatus st;
    if (!ctx || !site || !out) return REC_FAIL_UNSUPPORTED;
    code = rec_fail_code(kind);
    if (!code) return REC_FAIL_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->code = code;
    out->site = site;
    out->op_node = site;          /* default; locate may refine it */

    if (kind == EVAL_FAIL_OVERFLOW || kind == EVAL_FAIL_DIV_ZERO ||
        kind == EVAL_FAIL_SHIFT_RANGE) {
        /* arithmetic families: 12b1 types them */
        ArithFailValue af;
        ArithFailStatus ast = arith_fail_classify(ctx, site, kind, &af);
        if (ast == ARITH_FAIL_OOM) { ctx->oom = true; return REC_FAIL_OOM; }
        if (ast != ARITH_FAIL_FAILURE) {
            return ast == ARITH_FAIL_NOT_CONST ? REC_FAIL_NOT_CONST
                                               : REC_FAIL_UNSUPPORTED;
        }
        out->op_node = af.op_node;
        out->op = af.op;
        out->is_unary = af.is_unary;
        out->a = af.a;
        out->b = af.b;
        out->width = af.width;
        out->is_signed = af.is_signed;
        if (kind == EVAL_FAIL_SHIFT_RANGE) {
            rec_fill_shift_type(ctx, out, out->op_type, sizeof(out->op_type));
        }
        return REC_FAIL_FAILURE;
    }

    st = locate_rec_failure(ctx, site, kind, out);
    if (st == REC_FAIL_OOM) ctx->oom = true;
    if (st != REC_FAIL_FAILURE) return st;
    return REC_FAIL_FAILURE;
}

/* ---------------------------------------------------------------------------
 * Record building
 * ------------------------------------------------------------------------- */

static const char *rec_message(const RecFailValue *v, char *buf, size_t sz)
{
    char nbuf[32];
    switch (v->kind) {
    case EVAL_FAIL_OVERFLOW:
        return "constant expression overflow";
    case EVAL_FAIL_DIV_ZERO:
        return "constant division by zero";
    case EVAL_FAIL_SHIFT_RANGE:
        render_eval_int(&v->b, nbuf, sizeof(nbuf));
        snprintf(buf, sz, "constant shift count out of range: %s exceeds "
                          "%s bit width",
                 nbuf, v->op_type[0] ? v->op_type : "int");
        return buf;
    case EVAL_FAIL_CAST_RANGE:
        render_eval_int(&v->cast_value, nbuf, sizeof(nbuf));
        snprintf(buf, sz, "constant cast out of range: %s does not fit in %s",
                 nbuf, v->cast_target[0] ? v->cast_target : "?");
        return buf;
    case EVAL_FAIL_INDEX_RANGE:
        render_eval_int(&v->bound_value, nbuf, sizeof(nbuf));
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
 * primary span = the located node per the contract table. */
static DiagRecord *rec_build_record(EvalCtx *ctx, const RecFailValue *v)
{
    char msg[256];
    const char *text = rec_message(v, msg, sizeof(msg));
    const AstNode *span_node;
    DiagRecord *r;
    if (v->kind == EVAL_FAIL_OVERFLOW || v->kind == EVAL_FAIL_DIV_ZERO) {
        span_node = v->site;
    } else {
        span_node = v->op_node ? v->op_node : v->site;
    }
    r = diag_record_new();
    if (!r) { ctx->oom = true; return NULL; }
    if (!diag_record_set_code(r, v->code) ||
        !diag_record_set_message(r, text) ||
        !diag_record_set_primary_span(r, span_node ? span_node->span : NULL) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        ctx->oom = true;
        return NULL;
    }
    return r;
}

/* ---------------------------------------------------------------------------
 * Build-level record emission (same sites and order as 12a/12b1)
 * ------------------------------------------------------------------------- */

typedef struct RecEmitCtx {
    EvalCtx ev;
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool unsupported;
} RecEmitCtx;

static bool rec_push_record(RecEmitCtx *c, DiagRecord *r)
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

static void rec_emit_site(RecEmitCtx *c, const AstNode *expr)
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
            DiagRecord *r = rec_build_record(&c->ev, &rf);
            if (r) rec_push_record(c, r);
        } else if (rst == REC_FAIL_UNSUPPORTED) {
            c->unsupported = true;
        }
        return;
    }
    if (st == EVAL_NOT_CONST) {
        /* 12a owns AIC-E0401 for non-const sites; no record here */
        return;
    }
    if (st == EVAL_OOM) return;
    /* EVAL_UNSUPPORTED: defensive; no record */
    c->unsupported = true;
}

RecFailStatus rec_fail_emit(const NameResult *result,
                            const LayoutBuild *layout,
                            DiagRecord ***out_records,
                            size_t *out_count)
{
    RecEmitCtx c;
    size_t m;
    RecFailStatus status = REC_FAIL_OK;

    if (!result || !layout) return REC_FAIL_UNSUPPORTED;
    if (out_records) *out_records = NULL;
    if (out_count) *out_count = 0;

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
                rec_emit_site(&c, decl->u.global_decl.init);
                break;
            case AST_ENUM_DECL: {
                size_t i;
                for (i = 0; i < decl->u.enum_decl.nmembers; i++) {
                    const AstNode *mem = decl->u.enum_decl.members[i];
                    if (mem->u.named.value) {
                        rec_emit_site(&c, mem->u.named.value);
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
        eval_ctx_cleanup(&c.ev);
        return REC_FAIL_OOM;
    }
    if (c.nrecords) diag_sort_records(c.records, c.nrecords);
    if (out_records) *out_records = c.records;
    if (out_count) *out_count = c.nrecords;
    eval_ctx_cleanup(&c.ev);
    if (c.nrecords) status = REC_FAIL_FAILURE;
    else if (c.unsupported) status = REC_FAIL_UNSUPPORTED;
    else status = REC_FAIL_OK;
    return status;
}
