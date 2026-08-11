/* bootstrap/src/const/eval_fail_arith.c
 *
 * AI-Co Stage-0 checked-arithmetic failure evaluation (WP-M0-12b1).
 *
 * Produces the typed failure values for the three checked-arithmetic
 * failure families of spec sec. 11.3 (AIC-E0405 overflow, AIC-E0406
 * division/remainder by zero, AIC-E0407 shift count out of range), for
 * const-context sites (spec sec. 10.5), in the deterministic walk order
 * of the WP-M0-12a const-context check. WP-M0-12b2 consumes the typed
 * values for record emission; no record is produced here.
 *
 * The evaluation itself is 12a's (const_eval_expr): this package
 * re-evaluates the sub-expressions of a failing site through the 12a
 * public API only to locate the failing operator node and to capture
 * operand facts. It never performs the failing operation, so the
 * never-trap guarantee of 12a holds here as well.
 *
 * Deterministic conventions are documented in eval_fail_arith.h: the
 * failing operator is the first failure in the evaluator's order
 * (children before the operation, left before right), a const reference
 * is typed at the reference node (no cross-site descent), and
 * pointer-arithmetic overflow is typed as AIC-E0405 with width 64 /
 * signed (the byte-offset math is int64).
 */
#include "eval_fail_arith.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Kind -> code mapping (AIC-E0405..E0407 only)
 * ------------------------------------------------------------------------- */

const char *arith_fail_code(EvalFailure kind)
{
    switch (kind) {
    case EVAL_FAIL_OVERFLOW:    return "AIC-E0405";
    case EVAL_FAIL_DIV_ZERO:    return "AIC-E0406";
    case EVAL_FAIL_SHIFT_RANGE: return "AIC-E0407";
    default:
        return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Type helpers
 * ------------------------------------------------------------------------- */

/* Width/signedness of a primitive integer type; false when `t` is not a
 * primitive integer (the same convention as 12a's int_ty_of). */
static bool arith_int_ty(const Type *t, int *width, bool *is_signed)
{
    const TypePrimInfo *p;
    if (!t || t->kind != TYPE_PRIM) return false;
    p = types_prim_info(t->u.prim);
    if (!p || !p->is_integer) return false;
    *width = p->width_bits;
    *is_signed = p->is_signed;
    return true;
}

/* Common-type width/signedness for a binary integer operation. Returns
 * false when no common type exists (defensive; on a valid build 11c
 * already guaranteed one). */
static bool arith_common_ty(const Type *a, const Type *b,
                            int *width, bool *is_signed)
{
    Type *ct;
    bool ok;
    if (!a || !b) return false;
    ct = convert_common_type(a, b);
    if (!ct) return false;
    ok = arith_int_ty(ct, width, is_signed);
    type_free(ct);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Failure location (evaluator-order descent)
 * ------------------------------------------------------------------------- */

/* Map an EvalStatus from a sub-expression re-evaluation to the
 * classification status (defensive paths). */
static ArithFailStatus map_eval_status(EvalStatus st)
{
    switch (st) {
    case EVAL_OOM:         return ARITH_FAIL_OOM;
    case EVAL_NOT_CONST:   return ARITH_FAIL_NOT_CONST;
    default:               return ARITH_FAIL_UNSUPPORTED;
    }
}

/* Locate the failing operator node inside `node`, which is known to
 * evaluate to EVAL_FAILURE with `kind` (the routed kind of the site).
 * Follows the evaluator's order: children first (left before right),
 * then the node's own operator. On success fills out->op_node (and the
 * operand facts when the node itself is the failing operation) and
 * returns ARITH_FAIL_FAILURE. */
static ArithFailStatus locate_failure(EvalCtx *ctx, const AstNode *node,
                                      EvalFailure kind, ArithFailValue *out)
{
    EvalStatus st;
    if (!ctx || !node) return ARITH_FAIL_UNSUPPORTED;

    switch (node->kind) {
    case AST_EXPR_PAREN:
        return locate_failure(ctx, node->u.paren.expr, kind, out);

    case AST_EXPR_BINARY: {
        EvalValue lv, rv;
        EvalFailure lf = EVAL_FAIL_NONE, rf = EVAL_FAIL_NONE;
        st = const_eval_expr(ctx, node->u.binary.lhs, &lv, &lf);
        if (st == EVAL_FAILURE) {
            return locate_failure(ctx, node->u.binary.lhs, lf, out);
        }
        if (st != EVAL_OK) return map_eval_status(st);
        st = const_eval_expr(ctx, node->u.binary.rhs, &rv, &rf);
        if (st == EVAL_FAILURE) {
            eval_value_free(&lv);
            return locate_failure(ctx, node->u.binary.rhs, rf, out);
        }
        if (st != EVAL_OK) {
            eval_value_free(&lv);
            return map_eval_status(st);
        }
        /* both operands evaluate: this node's own operator failed */
        out->op_node = node;
        out->op = node->u.binary.op;
        out->is_unary = false;
        if (lv.kind == EVAL_VAL_INT && rv.kind == EVAL_VAL_INT) {
            out->a = lv.u.i;
            out->b = rv.u.i;
            if (node->u.binary.op == AST_BIN_SHL ||
                node->u.binary.op == AST_BIN_SHR) {
                /* shift width is the LEFT operand type (sec. 11.3) */
                if (!arith_int_ty(lv.type, &out->width, &out->is_signed)) {
                    out->width = 0;
                    out->is_signed = false;
                }
            } else {
                if (!arith_common_ty(lv.type, rv.type, &out->width,
                                     &out->is_signed)) {
                    out->width = 0;
                    out->is_signed = false;
                }
            }
        } else if (lv.kind == EVAL_VAL_ADDR && rv.kind == EVAL_VAL_INT) {
            /* pointer arithmetic overflow (sec. 12.5): the byte-offset
             * math is int64 (documented convention) */
            out->a = rv.u.i;
            out->b.v = 0;
            out->b.big = false;
            out->width = 64;
            out->is_signed = true;
        } else if (lv.kind == EVAL_VAL_INT && rv.kind == EVAL_VAL_ADDR) {
            out->a = lv.u.i;
            out->b.v = 0;
            out->b.big = false;
            out->width = 64;
            out->is_signed = true;
        } else {
            out->a.v = 0; out->a.big = false;
            out->b.v = 0; out->b.big = false;
            out->width = 0;
            out->is_signed = false;
        }
        eval_value_free(&lv);
        eval_value_free(&rv);
        return ARITH_FAIL_FAILURE;
    }

    case AST_EXPR_UNARY: {
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        if (node->u.unary.op == AST_UN_ADDR) {
            /* & of a static lvalue: the failure (if arithmetic) is
             * inside the index expression, not in the address form
             * itself (address failures are INDEX_RANGE/PTR_DIFF, 12b2) */
            const AstNode *operand = node->u.unary.operand;
            if (operand && operand->kind == AST_EXPR_INDEX) {
                st = const_eval_expr(ctx, operand->u.index_slice.index,
                                     &v, &vf);
                if (st == EVAL_FAILURE) {
                    return locate_failure(ctx,
                                          operand->u.index_slice.index,
                                          vf, out);
                }
                if (st != EVAL_OK) return map_eval_status(st);
                eval_value_free(&v);
            }
            out->op_node = node;
            return ARITH_FAIL_FAILURE;
        }
        st = const_eval_expr(ctx, node->u.unary.operand, &v, &vf);
        if (st == EVAL_FAILURE) {
            return locate_failure(ctx, node->u.unary.operand, vf, out);
        }
        if (st != EVAL_OK) return map_eval_status(st);
        /* the unary operator itself failed (negation overflow) */
        out->op_node = node;
        out->is_unary = true;
        if (v.kind == EVAL_VAL_INT) {
            out->a = v.u.i;
            if (!arith_int_ty(v.type, &out->width, &out->is_signed)) {
                out->width = 0;
                out->is_signed = false;
            }
        } else {
            out->a.v = 0; out->a.big = false;
            out->width = 0;
            out->is_signed = false;
        }
        out->b.v = 0; out->b.big = false;
        eval_value_free(&v);
        return ARITH_FAIL_FAILURE;
    }

    case AST_EXPR_CAST:
    case AST_EXPR_WRAP: {
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        /* the cast's own failure is CAST_RANGE (12b2); an arithmetic
         * failure must be inside the source expression */
        st = const_eval_expr(ctx, node->u.cast_wrap.expr, &v, &vf);
        if (st == EVAL_FAILURE) {
            return locate_failure(ctx, node->u.cast_wrap.expr, vf, out);
        }
        if (st != EVAL_OK) return map_eval_status(st);
        eval_value_free(&v);
        out->op_node = node;
        return ARITH_FAIL_FAILURE;
    }

    case AST_EXPR_IDENT:
    case AST_EXPR_MEMBER:
        /* const reference: typed at the reference node (documented
         * convention; the referenced const's own site carries the
         * precise failing operator) */
        out->op_node = node;
        return ARITH_FAIL_FAILURE;

    case AST_EXPR_ARRAY_LITERAL: {
        size_t i;
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        if (node->u.array_literal.count) {
            st = const_eval_expr(ctx, node->u.array_literal.count, &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_failure(ctx, node->u.array_literal.count,
                                      vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        for (i = 0; i < node->u.array_literal.nelems; i++) {
            st = const_eval_expr(ctx, node->u.array_literal.elems[i], &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_failure(ctx, node->u.array_literal.elems[i],
                                      vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        out->op_node = node;
        return ARITH_FAIL_FAILURE;
    }

    case AST_EXPR_STRUCT_INIT: {
        size_t i;
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        for (i = 0; i < node->u.struct_init.nfields; i++) {
            const AstNode *fi = node->u.struct_init.fields[i];
            if (!fi || !fi->u.named.value) {
                out->op_node = node;
                return ARITH_FAIL_FAILURE;
            }
            st = const_eval_expr(ctx, fi->u.named.value, &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_failure(ctx, fi->u.named.value, vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        out->op_node = node;
        return ARITH_FAIL_FAILURE;
    }

    case AST_EXPR_SLICE: {
        EvalValue v;
        EvalFailure vf = EVAL_FAIL_NONE;
        if (node->u.index_slice.lo) {
            st = const_eval_expr(ctx, node->u.index_slice.lo, &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_failure(ctx, node->u.index_slice.lo, vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        if (node->u.index_slice.hi) {
            st = const_eval_expr(ctx, node->u.index_slice.hi, &v, &vf);
            if (st == EVAL_FAILURE) {
                return locate_failure(ctx, node->u.index_slice.hi, vf, out);
            }
            if (st != EVAL_OK) return map_eval_status(st);
            eval_value_free(&v);
        }
        out->op_node = node;
        return ARITH_FAIL_FAILURE;
    }

    default:
        /* literal / sizeof / alignof / other leaf: the failing node is
         * the site itself (defensive; these cannot fail arithmetically
         * on a valid build) */
        out->op_node = node;
        return ARITH_FAIL_FAILURE;
    }
}

/* ---------------------------------------------------------------------------
 * Classification
 * ------------------------------------------------------------------------- */

ArithFailStatus arith_fail_classify(EvalCtx *ctx, const AstNode *site,
                                    EvalFailure kind, ArithFailValue *out)
{
    const char *code;
    ArithFailStatus st;
    if (!ctx || !site || !out) return ARITH_FAIL_UNSUPPORTED;
    code = arith_fail_code(kind);
    if (!code) return ARITH_FAIL_NOT_ARITH;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->code = code;
    out->site = site;
    out->op_node = site;          /* default; locate may refine it */
    st = locate_failure(ctx, site, kind, out);
    if (st != ARITH_FAIL_FAILURE) return st;
    return ARITH_FAIL_FAILURE;
}

ArithFailStatus arith_fail_eval(EvalCtx *ctx, const AstNode *site,
                                ArithFailValue *out)
{
    EvalValue v;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    if (!ctx || !site || !out) return ARITH_FAIL_UNSUPPORTED;
    st = const_eval_expr(ctx, site, &v, &fail);
    if (st == EVAL_OK) {
        eval_value_free(&v);
        return ARITH_FAIL_OK;
    }
    if (st == EVAL_FAILURE) {
        return arith_fail_classify(ctx, site, fail, out);
    }
    return map_eval_status(st);
}

/* ---------------------------------------------------------------------------
 * Build-level walk (same sites and order as 12a's const_eval_check)
 * ------------------------------------------------------------------------- */

ArithFailStatus arith_fail_check(const NameResult *result,
                                 const LayoutBuild *layout,
                                 ArithFailValue **out_fails,
                                 size_t *out_count)
{
    EvalCtx ctx;
    ArithFailValue *fails = NULL;
    size_t nfails = 0, cap = 0;
    bool unsupported = false;
    size_t m;
    ArithFailStatus status = ARITH_FAIL_OK;

    if (!result || !layout) return ARITH_FAIL_UNSUPPORTED;
    if (out_fails) *out_fails = NULL;
    if (out_count) *out_count = 0;

    eval_ctx_init(&ctx, result, layout, NULL);
    for (m = 0; m < result->nmodules; m++) {
        const NameModule *mod = result->modules[m];
        size_t d;
        ctx.module = mod;
        for (d = 0; d < mod->nmodule_scope; d++) {
            const NameSymbol *sym = mod->module_scope[d];
            const AstNode *decl;
            ArithFailValue af;
            ArithFailStatus st;
            if (!sym || !sym->decl) continue;
            decl = sym->decl;
            switch (decl->kind) {
            case AST_GLOBAL_CONST_DECL:
            case AST_GLOBAL_VAR_DECL:
                st = arith_fail_eval(&ctx, decl->u.global_decl.init, &af);
                break;
            case AST_ENUM_DECL: {
                size_t i;
                st = ARITH_FAIL_OK;
                for (i = 0; i < decl->u.enum_decl.nmembers; i++) {
                    const AstNode *mem = decl->u.enum_decl.members[i];
                    if (!mem || !mem->u.named.value) continue;
                    st = arith_fail_eval(&ctx, mem->u.named.value, &af);
                    if (st == ARITH_FAIL_FAILURE) {
                        if (nfails == cap) {
                            size_t ncap = cap ? cap * 2 : 8;
                            ArithFailValue *nf = (ArithFailValue *)realloc(
                                fails, ncap * sizeof(ArithFailValue));
                            if (!nf) {
                                free(fails);
                                eval_ctx_cleanup(&ctx);
                                if (out_fails) *out_fails = NULL;
                                if (out_count) *out_count = 0;
                                return ARITH_FAIL_OOM;
                            }
                            fails = nf;
                            cap = ncap;
                        }
                        fails[nfails++] = af;
                    } else if (st == ARITH_FAIL_UNSUPPORTED) {
                        unsupported = true;
                    }
                    if (ctx.oom) break;
                }
                continue;
            }
            default:
                continue;
            }
            if (st == ARITH_FAIL_FAILURE) {
                if (nfails == cap) {
                    size_t ncap = cap ? cap * 2 : 8;
                    ArithFailValue *nf = (ArithFailValue *)realloc(
                        fails, ncap * sizeof(ArithFailValue));
                    if (!nf) {
                        free(fails);
                        eval_ctx_cleanup(&ctx);
                        if (out_fails) *out_fails = NULL;
                        if (out_count) *out_count = 0;
                        return ARITH_FAIL_OOM;
                    }
                    fails = nf;
                    cap = ncap;
                }
                fails[nfails++] = af;
            } else if (st == ARITH_FAIL_UNSUPPORTED) {
                unsupported = true;
            }
            if (ctx.oom) break;
        }
        if (ctx.oom) break;
    }
    eval_ctx_cleanup(&ctx);
    if (ctx.oom) {
        free(fails);
        if (out_fails) *out_fails = NULL;
        if (out_count) *out_count = 0;
        return ARITH_FAIL_OOM;
    }
    if (nfails) status = ARITH_FAIL_FAILURE;
    else if (unsupported) status = ARITH_FAIL_UNSUPPORTED;
    else status = ARITH_FAIL_OK;
    if (out_fails) *out_fails = fails;
    if (out_count) *out_count = nfails;
    return status;
}
