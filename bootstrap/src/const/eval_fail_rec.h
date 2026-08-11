/* bootstrap/src/const/eval_fail_rec.h
 *
 * AI-Co Stage-0 const-failure record emission and remaining failure
 * sites (WP-M0-12b2).
 *
 * Implements deterministic failure-record emission for all seven
 * constant-failure codes of spec sec. 11.3 / 11.5 / 12.1 / 12.2 /
 * 12.4 / 12.5 (AIC-E0405 .. AIC-E0411) over the const-context sites
 * of the WP-M0-12a evaluator (global const/var initializers and enum
 * member value expressions), and the remaining four failure SITES the
 * WP-M0-12b1 arithmetic package excluded: cast-range (AIC-E0408),
 * index/slice-bound (AIC-E0409), str-slice code-point-boundary
 * (AIC-E0410), and pointer-difference divisibility (AIC-E0411).
 *
 * The arithmetic kinds (overflow / division-by-zero / shift-range)
 * are typed by the WP-M0-12b1 package (arith_fail_classify); this
 * package consumes those typed values for record emission and never
 * re-implements arithmetic failure detection. The never-trap
 * guarantee holds here exactly as in 12a/12b1: every failure is
 * detected and returned as a typed value / record; no host trap or
 * UB operation is ever executed (extreme values are covered by the
 * test program).
 *
 * Record conventions (documented decisions; the spec/contract does
 * not pin every message string - DIAGNOSTIC-CONTRACT sec. 11.10):
 *   - Every record is phase "semantic", severity "error", recovery
 *     "authoritative" (the code-registry defaults plus the explicit
 *     recovery marking), primary span per the contract table
 *     (DIAGNOSTIC-CONTRACT sec. 11.5):
 *       E0405 / E0406  the constant expression (the whole site
 *                      initializer - matches the WP-M0-12b corpus
 *                      anchors and the 12a whole-expression span
 *                      convention);
 *       E0407          the shift (op_node = the shift operator node,
 *                      the WP-M0-12b1 convention);
 *       E0408          the cast;
 *       E0409          the bound (the index expression of &arr[i], or
 *                      the offending slice bound expression);
 *       E0410          the slice;
 *       E0411          the subtraction.
 *   - Messages: registry description for E0405/E0406/E0410/E0411;
 *     descriptive suffixes for E0407 ("...: 33 exceeds i32 bit
 *     width"), E0408 ("...: 200 does not fit in i8"), and E0409
 *     ("...: 10 out of range for array of length 3" / slice-bound
 *     wording) matching the negative-corpus anchors.
 *   - The failing node is located by re-evaluating sub-expressions
 *     through const_eval_expr in the evaluator's own order (sec.
 *     10.4: slice base, then x, then y; binary left before right;
 *     children before the operation) and descending into the first
 *     sub-expression that fails with the routed kind. A failure
 *     reached through a const reference is typed at the reference
 *     node (op_node = the reference expression), the WP-M0-12b1
 *     convention; op_node never crosses a site boundary.
 *   - For E0409 slice-bound failures the offending bound is chosen
 *     deterministically: a negative explicit lo bound -> lo; a
 *     negative explicit hi bound -> hi; lo > hi (ordering violation)
 *     -> lo when explicit, else hi; hi > extent -> hi when explicit,
 *     else the slice node. The bound value and the extent (array
 *     length / str byte length) are captured as facts for the
 *     message.
 *
 * Ownership:
 *   - RecFailValue is a plain value: site/op_node are borrowed from
 *     the AST; code is a static string; the type-name buffers are
 *     inline (no owned memory).
 *   - rec_fail_emit's *out_records (when non-empty) is an owned array
 *     of DiagRecord* the caller frees with types_records_free.
 */
#ifndef AICO_BOOTSTRAP_SRC_CONST_EVAL_FAIL_REC_H
#define AICO_BOOTSTRAP_SRC_CONST_EVAL_FAIL_REC_H

#include "eval_fail_arith.h"

#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Kind -> code mapping (all seven const failure codes)
 * ------------------------------------------------------------------------- */

/* Map a const failure kind to its diagnostic code AIC-E0405 .. E0411,
 * or NULL for a kind this package does not emit (EVAL_FAIL_NONE and
 * defensive/unknown kinds). Static string; never freed. */
const char *rec_fail_code(EvalFailure kind);

/* ---------------------------------------------------------------------------
 * Typed failure value (all const failure kinds)
 * ------------------------------------------------------------------------- */

typedef struct RecFailValue {
    EvalFailure kind;          /* any of the seven const failure kinds */
    const char *code;          /* "AIC-E0405" .. "AIC-E0411" */
    const AstNode *site;       /* const-context site (whole initializer) */
    const AstNode *op_node;    /* failing node per the span convention above */

    /* operator facts (E0405 / E0406 / E0407 / E0411; from 12b1) */
    AstBinaryOp op;            /* failing binary operator (valid when !is_unary) */
    bool is_unary;             /* true: failing operator is AST_UN_NEG */
    EvalInt a;                 /* left (or only) operand value */
    EvalInt b;                 /* right operand value (shift: the count) */
    int width;                 /* operation width in bits (0 when unknown) */
    bool is_signed;            /* operation signedness (false when unknown) */
    char op_type[64];          /* E0407: shift left operand type name
                                * ("i32", "u64", ...); empty when unknown */

    /* cast facts (E0408) */
    EvalInt cast_value;        /* the source value that did not fit */
    char cast_target[64];      /* target type name ("i8", "Color", ...) */

    /* bound facts (E0409) */
    EvalInt bound_value;       /* the offending index/bound value */
    int64_t extent;            /* array length / str byte length; -1 unknown */
    bool is_slice_bound;       /* true: slice-bound failure; false: array index */

    /* slice facts (E0410) */
    int64_t slice_lo;          /* evaluated lo offset (default 0) */
    int64_t slice_hi;          /* evaluated hi offset (default len) */
} RecFailValue;

/* ---------------------------------------------------------------------------
 * Classification status
 * ------------------------------------------------------------------------- */

typedef enum RecFailStatus {
    REC_FAIL_OK = 0,           /* evaluated; no const failure */
    REC_FAIL_FAILURE,          /* const failure; *out set (typed value) */
    REC_FAIL_NOT_CONST,        /* expression outside the sec. 10.5 composition
                                * (12a owns AIC-E0401) */
    REC_FAIL_UNSUPPORTED,      /* defensive: malformed input; nothing owned */
    REC_FAIL_OOM               /* allocation failure; nothing owned */
} RecFailStatus;

/* ---------------------------------------------------------------------------
 * Single-site classification (all const failure kinds)
 * ------------------------------------------------------------------------- */

/* Classify an already-routed failure: `site` + `kind` come from 12a's
 * const_eval_check (EvalFailureSite). For the three arithmetic kinds
 * this delegates to 12b1's arith_fail_classify and copies the typed
 * value; for the four remaining kinds (cast/index/slice/ptr-diff) it
 * locates the failing node and captures the kind-specific facts.
 *
 * Returns:
 *   REC_FAIL_FAILURE     *out set (typed failure value).
 *   REC_FAIL_NOT_CONST   the site is outside the sec. 10.5 composition.
 *   REC_FAIL_UNSUPPORTED defensive; nothing owned.
 *   REC_FAIL_OOM         nothing owned.
 */
RecFailStatus rec_fail_classify(EvalCtx *ctx, const AstNode *site,
                                EvalFailure kind, RecFailValue *out);

/* ---------------------------------------------------------------------------
 * Build-level record emission (AIC-E0405 .. E0411)
 * ------------------------------------------------------------------------- */

/* Walk the const-context sites (global const/var initializers and enum
 * member value expressions, modules in result order, declarations in
 * source order - the same sites 12a's const_eval_check walks) and
 * emit one authoritative DiagRecord per site whose evaluation fails
 * with a const failure kind, in the deterministic order of the
 * DIAGNOSTIC-CONTRACT sec. 9 comparator (records are sorted before
 * return). Sites that are not constant expressions (12a's E0401) are
 * skipped: this package never emits E0401 (12a owns it) and never
 * emits records for defensive EVAL_UNSUPPORTED results.
 *
 * Returns:
 *   REC_FAIL_OK          no const failures; *out_records NULL.
 *   REC_FAIL_FAILURE     *out_records set (owned by the caller; free with
 *                        types_records_free) and *out_count set.
 *   REC_FAIL_UNSUPPORTED defensive; nothing owned.
 *   REC_FAIL_OOM         nothing owned.
 */
RecFailStatus rec_fail_emit(const NameResult *result,
                            const LayoutBuild *layout,
                            DiagRecord ***out_records,
                            size_t *out_count);

#endif /* AICO_BOOTSTRAP_SRC_CONST_EVAL_FAIL_REC_H */
