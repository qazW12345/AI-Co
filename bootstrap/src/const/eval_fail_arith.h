/* bootstrap/src/const/eval_fail_arith.h
 *
 * AI-Co Stage-0 checked-arithmetic failure evaluation (WP-M0-12b1).
 *
 * Implements compile-time checked-arithmetic failure evaluation for the
 * three arithmetic failure families of spec sec. 11.3 - checked
 * overflow, division/remainder by zero, and shift count out of range -
 * producing TYPED failure values (kind + diagnostic code + failing
 * operator node + operand facts) that WP-M0-12b2 consumes for
 * AIC-E0405 / AIC-E0406 / AIC-E0407 record emission.
 *
 * Composes with the WP-M0-12a evaluator: const-context site enumeration
 * and expression composition are 12a's (const_eval_check /
 * const_eval_expr). This package re-evaluates sub-expressions through
 * the 12a public API to locate the failing operator node and capture
 * operand facts; it never emits records, never modifies the evaluator,
 * and never performs a host-trapping operation itself.
 *
 * Scope (manifest WP-M0-12b1):
 *   - typed failure values for EVAL_FAIL_OVERFLOW / EVAL_FAIL_DIV_ZERO /
 *     EVAL_FAIL_SHIFT_RANGE (AIC-E0405 / AIC-E0406 / AIC-E0407);
 *   - single-site classification: arith_fail_eval (evaluate a
 *     const-context site and type its arithmetic failure) and
 *     arith_fail_classify (type an already-routed failure from 12a's
 *     const_eval_check);
 *   - a build-level walk over the const-context sites (the same sites
 *     12a's const_eval_check walks: global const/var initializers and
 *     enum member value expressions, modules in result order,
 *     declarations in source order) producing the typed arithmetic
 *     failures in deterministic order;
 *   - the never-trap guarantee: like 12a, no host trap / UB is ever
 *     executed; overflow, division by zero, and out-of-range shifts are
 *     detected and returned as typed failures (12a already checks
 *     before dividing/shifting/negating; this package only reads values
 *     and never performs the failing operation).
 *
 * Exclusions (12b2 owns these): cast-range, index/slice-range,
 * str-slice-boundary, and pointer-difference failures
 * (EVAL_FAIL_CAST_RANGE / EVAL_FAIL_INDEX_RANGE /
 * EVAL_FAIL_STR_BOUNDARY / EVAL_FAIL_PTR_DIFF), record emission for all
 * const failure codes, and negative-corpus anchors.
 *
 * Deterministic conventions (documented decisions; the spec does not
 * pin them):
 *   - The failing operator node (op_node) is located by re-evaluating
 *     sub-expressions through const_eval_expr in the evaluator's own
 *     order (left before right, children before the operation) and
 *     descending into the first sub-expression that fails with the
 *     routed kind; the operation whose operands evaluate but whose
 *     operator fails is the failing operator. For E0407 the
 *     diagnostic-contract primary span is "the shift": op_node IS the
 *     shift operator node. For E0405/E0406 the primary span is the
 *     whole constant expression: site is the whole initializer and
 *     op_node additionally identifies the failing operation for message
 *     facts.
 *   - A failure reached through a const reference (an identifier /
 *     member chain resolving to a const whose initializer fails) is
 *     typed at the reference node: op_node = the reference expression.
 *     The referenced const's own const-context site is walked separately
 *     (12a routes both sites) and carries the precise failing operator;
 *     op_node never crosses a site boundary.
 *   - Pointer-arithmetic overflow (sec. 12.5, e.g. p + huge) is routed
 *     by 12a as EVAL_FAIL_OVERFLOW and is typed here as AIC-E0405 with
 *     width 64 / signed (the byte-offset math is int64).
 *   - Operand facts: for binary integer ops a/b are the operand values
 *     in the operation's common type; width/is_signed are the common
 *     type's. For shifts, width/is_signed are the LEFT operand type's
 *     (the count is checked against it per sec. 11.3); a = left value,
 *     b = count value. For unary-negation overflow, a = the operand
 *     value. For pointer-arithmetic overflow, a = the integer operand,
 *     b = 0.
 *
 * Ownership:
 *   - ArithFailValue is a plain value: site/op_node are borrowed from
 *     the AST; code is a static string; no owned memory.
 *   - arith_fail_check's *out_fails (when non-empty) is a plain owned
 *     array the caller frees with free(); nothing else is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_CONST_EVAL_FAIL_ARITH_H
#define AICO_BOOTSTRAP_SRC_CONST_EVAL_FAIL_ARITH_H

#include "eval_core.h"

#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Typed arithmetic failure value
 * ------------------------------------------------------------------------- */

typedef struct ArithFailValue {
    EvalFailure kind;          /* EVAL_FAIL_OVERFLOW / DIV_ZERO / SHIFT_RANGE */
    const char *code;          /* "AIC-E0405" / "AIC-E0406" / "AIC-E0407" */
    const AstNode *site;       /* const-context site (whole initializer) */
    const AstNode *op_node;    /* failing operator node (the shift for E0407) */
    AstBinaryOp op;            /* failing binary operator (valid when !is_unary) */
    bool is_unary;             /* true: failing operator is AST_UN_NEG */
    EvalInt a;                 /* left (or only) operand value */
    EvalInt b;                 /* right operand value (unary: {0,false}) */
    int width;                 /* operation width in bits (0 when unknown) */
    bool is_signed;            /* operation signedness (false when unknown) */
} ArithFailValue;

/* Map an arithmetic failure kind to its diagnostic code, or NULL when the
 * kind is not one of the three arithmetic families (12b2 owns the
 * others). Static string; never freed. */
const char *arith_fail_code(EvalFailure kind);

/* ---------------------------------------------------------------------------
 * Classification status
 * ------------------------------------------------------------------------- */

typedef enum ArithFailStatus {
    ARITH_FAIL_OK = 0,         /* evaluated; no arithmetic failure */
    ARITH_FAIL_FAILURE,        /* arithmetic failure; *out set (typed value) */
    ARITH_FAIL_NOT_CONST,      /* expression outside the sec. 10.5 composition
                                * (12a owns AIC-E0401) */
    ARITH_FAIL_NOT_ARITH,      /* failure kind is not one of the three
                                * arithmetic families (12b2 owns it) */
    ARITH_FAIL_UNSUPPORTED,    /* defensive: malformed input; nothing owned */
    ARITH_FAIL_OOM             /* allocation failure; nothing owned */
} ArithFailStatus;

/* ---------------------------------------------------------------------------
 * Single-site classification
 * ------------------------------------------------------------------------- */

/* Evaluate `site` (a const-context site expression) in `ctx` and, when
 * its evaluation fails with an arithmetic failure kind, fill *out with
 * the typed failure value (kind, code, site, op_node, operator, operand
 * facts). The never-trap guarantee holds: the evaluation is 12a's and
 * this package performs no failing operation.
 *
 * Returns:
 *   ARITH_FAIL_OK          evaluated; no arithmetic failure.
 *   ARITH_FAIL_FAILURE     *out set (typed arithmetic failure).
 *   ARITH_FAIL_NOT_CONST   site is outside the sec. 10.5 composition.
 *   ARITH_FAIL_NOT_ARITH   the site failed with a non-arithmetic kind
 *                          (cast/index/slice/ptr-diff; 12b2 owns it).
 *   ARITH_FAIL_UNSUPPORTED defensive; nothing owned.
 *   ARITH_FAIL_OOM         nothing owned.
 */
ArithFailStatus arith_fail_eval(EvalCtx *ctx, const AstNode *site,
                                ArithFailValue *out);

/* Classify an already-routed failure: `site` + `kind` come from 12a's
 * const_eval_check (EvalFailureSite). Produces the typed failure value
 * exactly as arith_fail_eval does for arithmetic kinds; for any other
 * kind returns ARITH_FAIL_NOT_ARITH and leaves *out untouched. */
ArithFailStatus arith_fail_classify(EvalCtx *ctx, const AstNode *site,
                                    EvalFailure kind, ArithFailValue *out);

/* ---------------------------------------------------------------------------
 * Build-level walk (API for 12b2 record emission)
 * ------------------------------------------------------------------------- */

/* Walk the const-context sites (global const/var initializers and enum
 * member value expressions, modules in result order, declarations in
 * source order - the same sites 12a's const_eval_check walks) and
 * collect the typed arithmetic failures in deterministic order. Sites
 * that are not constant expressions (12a's E0401) and sites failing
 * with non-arithmetic kinds (12b2's) are skipped; this package produces
 * no records.
 *
 * Returns:
 *   ARITH_FAIL_OK          no arithmetic failures; *out_fails NULL.
 *   ARITH_FAIL_FAILURE     *out_fails set (owned by the caller; free with
 *                          free()) and *out_count set.
 *   ARITH_FAIL_UNSUPPORTED defensive; nothing owned.
 *   ARITH_FAIL_OOM         nothing owned.
 */
ArithFailStatus arith_fail_check(const NameResult *result,
                                 const LayoutBuild *layout,
                                 ArithFailValue **out_fails,
                                 size_t *out_count);

#endif /* AICO_BOOTSTRAP_SRC_CONST_EVAL_FAIL_ARITH_H */
