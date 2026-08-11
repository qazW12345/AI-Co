/* bootstrap/src/const/eval_core.h
 *
 * AI-Co Stage-0 constant-expression evaluator core (WP-M0-12a).
 *
 * Implements compile-time evaluation of constant expressions per spec
 * sec. 10.5 (composition rules, typed constant values) and the value
 * semantics of sec. 11.3 (checked arithmetic) / sec. 11.5 (cast and
 * wrap) / sec. 12.5 (static addresses and pointer arithmetic), over the
 * resolved build (WP-M0-10 NameResult), the layouts of WP-M0-11b, and
 * the type/conversion/operator facilities of WP-M0-11a/11c/11d.
 *
 * Scope (manifest WP-M0-12a):
 *   - const-expression composition per sec. 10.5 exactly; a violation
 *     of const-ness is reported by the caller as AIC-E0401 (the
 *     evaluator returns EVAL_NOT_CONST; the build-level check emits the
 *     record);
 *   - typed constant values: every evaluated value carries its Type;
 *   - sizeof/alignof (sec. 10.2) and static-address forms (& of a
 *     global var, &arr[i], slice expressions of static arrays with
 *     constant bounds) evaluate to deterministic typed values;
 *   - enum members (sec. 7.5): member values are read from the
 *     WP-M0-11b LayoutBuild (the authoritative member-value source);
 *   - checked arithmetic per sec. 11.3 with *typed* widths: failures
 *     (overflow/div-zero/shift-range/cast-range/index-range/ptr-diff,
 *     AIC-E0405..E0411) are DETECTED and routed as EvalFailure kinds,
 *     but no failure record is emitted here - WP-M0-12b owns the
 *     failure records (exclusion: const failure semantics).
 *
 * Deterministic conventions (documented decisions; the spec does not
 * pin them and the negative corpus has no reachable E0401 anchor):
 *   - The build-level check reports one AIC-E0401 per const-context
 *     site whose initializer is not a constant expression, primary
 *     span = the whole initializer expression (matches the
 *     whole-expression span convention of the WP-M0-12b failure
 *     corpus, e.g. `5 / 0`). Message: "expression is not a constant
 *     expression" (registry description), phase "semantic" (registry
 *     default), recovery "authoritative".
 *   - Composed forms exactly per sec. 10.5: integer/str/bool/null
 *     literals; const names (module and enclosing scopes); enum
 *     members; struct/array literals (repeat form included); sizeof/
 *     alignof; & of a global var, &arr[i] of a static array, slices of
 *     static arrays with constant bounds; unary and binary operators
 *     on constant expressions; cast/wrap. Everything else is NOT
 *     const: calls, len/ptr built-ins, assignments, ternary ?:, bare
 *     var names, indexing that is not part of an address/slice form,
 *     struct-field member access, dereference, non-static addresses,
 *     and str/pointer forms whose value depends on link-time
 *     addresses (pointer relational comparison between distinct
 *     objects, ptr->integer cast, str<->u8[] cast).
 *   - sizeof(expr) does not evaluate its operand (sec. 10.4): the
 *     operand only needs a derivable type, so sizeof of a non-const
 *     expression (e.g. a runtime var) is still a constant expression.
 *   - Shift counts are checked against the *typed* width (sec. 11.3:
 *     count in 0 .. width-1). min % -1 is an overflow failure
 *     (sec. 11.3), unlike the WP-M0-11b bounded subset which returned
 *     0 (11b's subset is superseded for full composition).
 *   - Array sizes are computed by this package from the type node
 *     (evaluating the extent with full composition), so sizeof of an
 *     array whose extent was outside the 11b subset still evaluates.
 *
 * Boundary notes:
 *   - The evaluator assumes the build passed name/completeness/layout/
 *     convert/optype with no diagnostics (the pipeline order). Records
 *     for operator misuse (AIC-T030x) and conversions (AIC-T0307)
 *     belong to WP-M0-11c/11d and are never produced here; on a valid
 *     build the defensive EVAL_UNSUPPORTED paths are unreachable.
 *   - `&const` is NOT reported here as E0401: the address-of-const
 *     rejection is AIC-E0402, owned by a later package (spec sec. 8.1,
 *     sec. 12.5); the evaluator returns EVAL_UNSUPPORTED (no record).
 *   - Array extents, case labels, and local const declarations are
 *     const-context sites owned by later packages; this package
 *     exposes the evaluation API for them (manifest: "API for
 *     WP-M0-11 (array extents, enum values) and WP-M0-13") and the
 *     build-level check covers global const/var initializers and enum
 *     member value expressions.
 *
 * Ownership:
 *   - On EVAL_OK, *out is owned by the caller (eval_value_free). The
 *     Type inside is owned; str bytes are borrowed from the AST;
 *     NameSymbol pointers are borrowed from the NameResult.
 *   - On any other EvalStatus *out is untouched.
 *   - On CONST_EVAL_OK / CONST_EVAL_DIAG_ERROR / CONST_EVAL_FAILURE,
 *     *out_records / *out_record_count (when non-empty) are owned by
 *     the caller via types_records_free, and *out_failures /
 *     *out_failure_count (when non-empty) is a plain owned array the
 *     caller frees with free(). The NameResult and LayoutBuild are
 *     borrowed and never modified.
 *   - On CONST_EVAL_UNSUPPORTED / CONST_EVAL_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_CONST_EVAL_CORE_H
#define AICO_BOOTSTRAP_SRC_CONST_EVAL_CORE_H

#include "../types/convert.h"
#include "../types/optype.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Status and failure kinds
 * ------------------------------------------------------------------------- */

typedef enum EvalStatus {
    EVAL_OK = 0,          /* evaluated; *out owned by the caller */
    EVAL_NOT_CONST,       /* expression outside the sec. 10.5 composition;
                           * caller reports AIC-E0401 when it owns the site */
    EVAL_FAILURE,         /* checked-arithmetic failure (sec. 11.3); the
                           * kind is *out_failure; WP-M0-12b owns records */
    EVAL_UNSUPPORTED,     /* defensive: malformed input / unknown symbol /
                           * missing layout / form whose evaluation belongs
                           * to a later package; no record is produced */
    EVAL_OOM              /* allocation failure; nothing owned */
} EvalStatus;

typedef enum EvalFailure {
    EVAL_FAIL_NONE = 0,
    EVAL_FAIL_OVERFLOW,      /* sec. 11.3 checked overflow -> AIC-E0405 (12b) */
    EVAL_FAIL_DIV_ZERO,      /* division/remainder by zero -> AIC-E0406 (12b) */
    EVAL_FAIL_SHIFT_RANGE,   /* shift count outside 0..width-1 -> AIC-E0407 */
    EVAL_FAIL_CAST_RANGE,    /* cast target representability -> AIC-E0408 */
    EVAL_FAIL_INDEX_RANGE,   /* index/slice bound out of range -> AIC-E0409 */
    EVAL_FAIL_STR_BOUNDARY,  /* str slice not on a code point boundary
                              * -> AIC-E0410 */
    EVAL_FAIL_PTR_DIFF       /* pointer difference not divisible by element
                              * size -> AIC-E0411 */
} EvalFailure;

/* ---------------------------------------------------------------------------
 * Evaluation context
 * ------------------------------------------------------------------------- */

typedef struct EvalCtx {
    const NameResult *result;
    const LayoutBuild *layout;
    const NameModule *module;      /* current module for name resolution;
                                    * the walk (and const references) switch
                                    * it per site */
    const NameSymbol **in_progress; /* const-recursion guard (owned) */
    size_t n_in_progress, in_progress_cap;
    bool oom;
} EvalCtx;

void eval_ctx_init(EvalCtx *ctx, const NameResult *result,
                   const LayoutBuild *layout, const NameModule *module);
void eval_ctx_cleanup(EvalCtx *ctx);

/* ---------------------------------------------------------------------------
 * Typed constant values
 * ------------------------------------------------------------------------- */

typedef enum EvalValueKind {
    EVAL_VAL_INT = 0,      /* integer (or enum; value = underlying integer) */
    EVAL_VAL_BOOL,
    EVAL_VAL_STR,          /* bytes/len borrowed from the AST */
    EVAL_VAL_NULL,         /* null literal; type is NULL */
    EVAL_VAL_ADDR,         /* static address; see addr */
    EVAL_VAL_SLICE,        /* slice of a static array; see slice */
    EVAL_VAL_ARRAY,        /* array literal value */
    EVAL_VAL_STRUCT        /* struct literal value */
} EvalValueKind;

/* Forward declaration so the composite value arrays can reference the
 * value type by name inside its own definition. */
typedef struct EvalValue EvalValue;

/* Integer value: `big` marks a value in [2^63, 2^64-1] stored two's
 * complement in `v` (the LayoutEnumMember convention). For signed
 * integer types `big` is always false. */
typedef struct EvalInt {
    int64_t v;
    bool big;
} EvalInt;

struct EvalValue {
    Type *type;             /* owned; NULL only for a standalone null literal
                             * or an untyped array literal */
    EvalValueKind kind;
    union {
        EvalInt i;                       /* EVAL_VAL_INT */
        bool b;                          /* EVAL_VAL_BOOL */
        struct {
            const char *bytes;           /* borrowed from the AST */
            size_t len;
        } str;                           /* EVAL_VAL_STR */
        struct {
            const NameSymbol *sym;       /* static object, or NULL for a raw
                                          * address / null pointer */
            int64_t byte_offset;         /* byte offset into the object (or
                                          * the raw address when sym is NULL) */
        } addr;                          /* EVAL_VAL_ADDR */
        struct {
            const NameSymbol *sym;       /* static array */
            int64_t lo;                  /* element index range [lo, hi) */
            int64_t hi;
        } slice;                         /* EVAL_VAL_SLICE */
        struct {
            EvalValue *elems;            /* owned; one per element */
            size_t nelems;
        } array;                         /* EVAL_VAL_ARRAY */
        struct {
            EvalValue *fields;           /* owned; one per literal field, in
                                          * literal order */
            size_t nfields;
        } st;                            /* EVAL_VAL_STRUCT */
    } u;
};

/* Free a value graph (type + composite children). NULL accepted. */
void eval_value_free(EvalValue *v);

/* ---------------------------------------------------------------------------
 * Core evaluation
 * ------------------------------------------------------------------------- */

/* Evaluate `expr` as a constant expression in `ctx` (sec. 10.5). The
 * module used for name resolution is ctx->module (callers set it before
 * the call; const references switch it internally while evaluating the
 * referenced initializer).
 *
 * Returns:
 *   EVAL_OK          *out is set (owned by the caller).
 *   EVAL_NOT_CONST   the expression is outside the sec. 10.5 composition;
 *                    *out untouched. The caller emits AIC-E0401 when it
 *                    owns the const-context site.
 *   EVAL_FAILURE     a checked-arithmetic failure was detected; the kind
 *                    is written to *out_failure (when non-NULL). No
 *                    record is produced.
 *   EVAL_UNSUPPORTED defensive; nothing owned.
 *   EVAL_OOM         nothing owned.
 */
EvalStatus const_eval_expr(EvalCtx *ctx, const AstNode *expr,
                           EvalValue *out, EvalFailure *out_failure);

/* ---------------------------------------------------------------------------
 * Build-level const-context check (AIC-E0401)
 * ------------------------------------------------------------------------- */

typedef enum ConstEvalStatus {
    CONST_EVAL_OK = 0,          /* all const-context sites evaluated; no records */
    CONST_EVAL_DIAG_ERROR,      /* AIC-E0401 records produced (composition
                                 * violations) */
    CONST_EVAL_FAILURE,         /* no records, but at least one site failed a
                                 * checked-arithmetic check; *out_failures set
                                 * (WP-M0-12b consumes them) */
    CONST_EVAL_UNSUPPORTED,     /* defensive; nothing owned */
    CONST_EVAL_OOM              /* nothing owned */
} ConstEvalStatus;

/* One routed arithmetic failure: `node` is the const-context site
 * expression whose evaluation failed (the whole initializer / enum
 * member value expression; the WP-M0-12b record span convention),
 * `kind` the failure kind. */
typedef struct EvalFailureSite {
    const AstNode *node;
    EvalFailure kind;
} EvalFailureSite;

/* Evaluate every const-context site of the resolved build that this
 * package owns - global const initializers (spec sec. 8.1), global var
 * initializers (sec. 8.3: static storage is initialized by a constant
 * initializer), and enum member value expressions (sec. 7.5) - and
 * report composition violations as AIC-E0401 (phase "semantic",
 * recovery "authoritative", primary span = the whole initializer
 * expression). Arithmetic failures are routed to *out_failures in
 * deterministic walk order (modules in result order, declarations in
 * source order); no failure record is emitted (WP-M0-12b owns the
 * records). Callers are expected to run completeness, layout, the 11c
 * conversion check, and the 11d operator check first and stop on their
 * diagnostics, as the pipeline does.
 *
 * Returns CONST_EVAL_OK / CONST_EVAL_DIAG_ERROR / CONST_EVAL_FAILURE
 * with *out_records and/or *out_failures set (when non-empty), or
 * CONST_EVAL_UNSUPPORTED / CONST_EVAL_OOM with nothing owned. */
ConstEvalStatus const_eval_check(const NameResult *result,
                                 const LayoutBuild *layout,
                                 DiagRecord ***out_records,
                                 size_t *out_record_count,
                                 EvalFailureSite **out_failures,
                                 size_t *out_failure_count);

#endif /* AICO_BOOTSTRAP_SRC_CONST_EVAL_CORE_H */
