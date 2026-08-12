/* bootstrap/src/sema/expr_ops.h
 *
 * AI-Co Stage-0 operator semantics (WP-M0-13b2).
 *
 * Implements the operator-semantics model of spec sec. 11.3-11.6 over
 * the resolved build (WP-M0-10 NameResult), the layouts of WP-M0-11b,
 * and the const-evaluation / failure-classification facilities of
 * WP-M0-12a/12b:
 *
 *   - checked-arithmetic compile-time decisions (sec. 11.3): a pure,
 *     site-agnostic decision API that classifies an integer operation
 *     as valid / checked-overflow (AIC-E0405) / division-by-zero
 *     (AIC-E0406) / shift-count-out-of-range (AIC-E0407). It encodes
 *     the exact rules of sec. 11.3: signed add/sub/mul overflow,
 *     unsigned wrap-around rejection (unsigned arithmetic does not
 *     silently wrap), division/remainder by zero, signed min / -1,
 *     unary negation of the signed minimum, shift counts in 0..width-1,
 *     and the defined shift result semantics (left shift of signed
 *     values is defined on the two's-complement bit pattern; right
 *     shift of signed values is arithmetic, of unsigned logical);
 *   - comparison semantics (sec. 11.4): the comparison mechanism per
 *     operand type (integer mathematical value on the actual type's
 *     range, enum by underlying integer value, pointer byte addresses
 *     as unsigned integers, str lexicographic byte-by-byte, bool by
 *     value, slice length-then-element-wise equality) plus pure
 *     comparators for the constant-evaluable forms (integers, enums,
 *     bools, str bytes, slices, same-object addresses);
 *   - operator-site checks (sec. 11.3-11.6): the build-level
 *     failure-record emission (AIC-E0405..E0411) for the const-context
 *     sites owned by WP-M0-13b1 - array type extents (sec. 12.1),
 *     switch case labels (sec. 13.2), and local const declaration
 *     initializers (sec. 8.1). This closes the routing gap left by the
 *     13b1 package, which routes failures out as EvalFailureSite but
 *     never emits records: without this stage an overflow, division by
 *     zero, out-of-range shift, or out-of-range cast in a local const
 *     initializer, case label, or array extent would silently pass
 *     (acceptance criterion: no expression semantic rule silently
 *     passes an invalid program).
 *
 * Ownership boundaries:
 *   - Global const/var initializers and enum member value expressions
 *     are WP-M0-12a const-context sites; their failure records
 *     (AIC-E0405..E0411) are emitted by WP-M0-12b2 (rec_fail_emit) and
 *     are NEVER produced here (boundary test in expr_ops_test.c).
 *   - AIC-E0401 (const-context composition) at the 13b1-owned sites is
 *     owned by WP-M0-13b1 (expr_core_check) and is never produced
 *     here; a non-const site yields no record from this package.
 *   - Operator APPLICABILITY typing (AIC-T0304..T0313, sec. 10.2) is
 *     owned by WP-M0-11d (optype) and is not repeated here; the
 *     lvalue/mutability/assignability checks of sec. 11.6
 *     (AIC-E0402/E0404/E0419) are owned by WP-M0-13a1 (decl_core); the
 *     compound-assignment evaluation order (destination location, then
 *     the right operand, then read, then store) is modeled by
 *     WP-M0-13b1 (expr_order_plan). This package adds the semantic
 *     decisions and failure records those packages route onward.
 *
 * Record conventions (mirroring the WP-M0-12b2 conventions documented
 * in eval_fail_rec.h and the DIAGNOSTIC-CONTRACT sec. 11.5 span table,
 * so records at the 13b1-owned sites are byte-identical in shape to
 * 12b2's at the 12a-owned sites; the negative-corpus anchors pin the
 * message strings):
 *   - Every record is phase "semantic", severity "error", recovery
 *     "authoritative" (the code-registry defaults plus the explicit
 *     recovery marking), primary span per the contract table:
 *       E0405 / E0406  the constant expression (the whole site
 *                      expression - the array extent, case label, or
 *                      local-const initializer);
 *       E0407          the shift (op_node = the shift operator node);
 *       E0408          the cast; E0409 the bound; E0410 the slice;
 *       E0411          the subtraction.
 *   - Messages: "constant expression overflow", "constant division by
 *     zero", "constant shift count out of range: <count> exceeds <ty>
 *     bit width", "constant cast out of range: <value> does not fit in
 *     <ty>", the bound wording for E0409, and the registry descriptions
 *     for E0410/E0411 - exactly the 12b2 strings.
 *   - The failing node and message facts come from the public
 *     WP-M0-12b2 classifier (rec_fail_classify); this package only
 *     renders the documented message text and builds the record. It
 *     never re-implements arithmetic failure detection.
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every
 * declaration in module-scope order, descending into struct field
 * types, parameter and return types, and function bodies (including
 * `for` init declarations, case clauses, and every type/expression
 * reachable from them) - the same walk order as expr_core_check
 * (WP-M0-13b1), so the two stages' records compose deterministically.
 * Records are returned sorted with the DIAGNOSTIC-CONTRACT sec. 9
 * comparator (diag_sort_records). Callers are expected to run the
 * name, type, layout, conversion, operator, const-eval, and 13b1
 * const-context stages first and stop on their diagnostics, as the
 * pipeline does; on a clean build the defensive EXPR_OPS_UNSUPPORTED
 * paths are unreachable.
 *
 * Ownership:
 *   - On EXPR_OPS_OK / EXPR_OPS_DIAG_ERROR / EXPR_OPS_UNSUPPORTED,
 *     *out_records / *out_record_count (when non-empty) are owned by
 *     the caller via types_records_free. The NameResult and LayoutBuild
 *     are borrowed and never modified.
 *   - On EXPR_OPS_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_EXPR_OPS_H
#define AICO_BOOTSTRAP_SRC_SEMA_EXPR_OPS_H

#include "../const/eval_fail_rec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Checked-arithmetic compile-time decisions (spec sec. 11.3)
 * ------------------------------------------------------------------------- */

/* The compile-time decision for one checked integer operation. */
typedef enum ExprOpDecision {
    EXPR_OP_OK = 0,          /* operation is valid for the given operands */
    EXPR_OP_OVERFLOW,        /* AIC-E0405: signed overflow / unsigned wrap /
                              * min / -1 / negation of the signed minimum */
    EXPR_OP_DIV_ZERO,        /* AIC-E0406: division/remainder by zero */
    EXPR_OP_SHIFT_RANGE      /* AIC-E0407: shift count outside 0..width-1 */
} ExprOpDecision;

/* Map a decision to its diagnostic code ("AIC-E0405" / "AIC-E0406" /
 * "AIC-E0407"), or NULL for EXPR_OP_OK. Static string; never freed. */
const char *expr_op_decision_code(ExprOpDecision d);

/* Decide a binary integer operation per sec. 11.3:
 *   - add/sub/mul: signed -> overflow when the mathematical result is
 *     outside the type's range; unsigned -> overflow when the result
 *     wraps modulo 2^width (unsigned arithmetic does not silently
 *     wrap);
 *   - div/mod: divisor zero -> DIV_ZERO; signed min / -1 (and
 *     min % -1) -> OVERFLOW;
 *   - shl/shr: count outside 0..width-1 -> SHIFT_RANGE; otherwise OK
 *     (the shift result is defined by expr_op_shift_value; shifts
 *     themselves never overflow);
 *   - any other operator (bitwise, comparisons, logical) never fails
 *     per sec. 11.3 -> EXPR_OP_OK.
 * `width` is the operation width in bits (8/16/32/64 per the
 * sec. 7.1 table; 1..64 handled generically). Defensive: width <= 0 or
 * width > 64 returns EXPR_OP_OK (no decision; a valid pipeline never
 * reaches the decision API with an unknown width). The never-trap
 * guarantee holds: no host overflow/UB operation is ever performed;
 * detection uses unsigned/division-based checks. */
ExprOpDecision expr_op_binary_decision(AstBinaryOp op, EvalInt a, EvalInt b,
                                       int width, bool is_signed);

/* Decide unary negation of an integer value per sec. 11.3: negation of
 * the signed minimum is checked overflow (AIC-E0405); every other
 * signed value negates within range -> OK. Unsigned negation is
 * rejected earlier by WP-M0-11d (AIC-T0306) and never reaches this
 * API on a valid build; defensively it returns EXPR_OP_OK. */
ExprOpDecision expr_op_neg_decision(EvalInt a, int width, bool is_signed);

/* The defined result of a VALID shift (sec. 11.3): left shift of
 * signed values is defined on the two's-complement bit pattern (the
 * resulting bits are the mathematical value of (unsigned)x << n
 * re-read as the signed type; no overflow trap for shifts); right
 * shift of signed values is arithmetic (sign-extending), of unsigned
 * logical. `count` must satisfy 0 <= count < width (callers check with
 * expr_op_binary_decision first); a defensive out-of-range count
 * returns `a` unchanged. */
EvalInt expr_op_shift_value(AstBinaryOp op, EvalInt a, EvalInt count,
                            int width, bool is_signed);

/* ---------------------------------------------------------------------------
 * Comparison semantics (spec sec. 11.4)
 * ------------------------------------------------------------------------- */

/* The comparison mechanism for one operand type (sec. 11.4). */
typedef enum ExprCmpKind {
    EXPR_CMP_INT_MATH = 0,       /* integer: mathematical value comparison on
                                  * the actual type's range */
    EXPR_CMP_ENUM_UNDERLYING,    /* enum: by underlying integer value; mixed
                                  * enum types need an explicit cast (11d) */
    EXPR_CMP_BOOL_VALUE,         /* bool: == / != by value */
    EXPR_CMP_PTR_ADDR,           /* pointer: byte addresses compared as
                                  * unsigned integers; relational between
                                  * distinct objects is defined (address
                                  * ordering) though only ==/!= between
                                  * pointers into the same object is
                                  * meaningful (sec. 12.8 caveat) */
    EXPR_CMP_STR_BYTES,          /* str: lexicographic byte-by-byte of the
                                  * UTF-8 byte sequences */
    EXPR_CMP_SLICE_ELEMS         /* slice: == / != element-wise, using the
                                  * element type's equality; length mismatch
                                  * -> not equal */
} ExprCmpKind;

/* The sec. 11.4 mechanism for a type: integer primitives -> INT_MATH;
 * enum -> ENUM_UNDERLYING; bool -> BOOL_VALUE; pointer -> PTR_ADDR;
 * str -> STR_BYTES; slice (any element) -> SLICE_ELEMS. Other kinds
 * (void, arrays, structs) return EXPR_CMP_INT_MATH defensively (a
 * valid pipeline never compares them - 11d rejects with AIC-T0304/
 * T0306). NULL type -> EXPR_CMP_INT_MATH. */
ExprCmpKind expr_cmp_kind_of(const Type *type);

/* Compare two integer values on the mathematical value of the actual
 * type's range (sec. 11.4): returns < 0, 0, > 0. `width`/`is_signed`
 * describe the operand type; big values (two's complement in the
 * int64 field, [2^63, 2^64-1]) are compared as unsigned. For signed
 * types big is never set. Defensive width handling: width >= 64
 * compares on the full 64-bit range. */
int expr_cmp_ints(EvalInt a, EvalInt b, int width, bool is_signed);

/* Lexicographic byte-by-byte comparison (sec. 11.4 str semantics):
 * returns < 0, 0, > 0 comparing the byte sequences up to the shorter
 * length, then by length (memcmp semantics; NULL/empty handled). */
int expr_cmp_bytes(const void *a, size_t an, const void *b, size_t bn);

/* Slice equality (sec. 11.4): equal lengths AND element-wise equality
 * of every element using the element type's equality; length mismatch
 * -> false. `elem_size` is the byte stride of one element (the caller
 * knows the element type); `elem_eq` receives the two element pointers
 * for the current index and `ud`; `a`/`b` are the element arrays. */
bool expr_cmp_slice_equal(int64_t alen, const void *a, size_t elem_size,
                          int64_t blen, const void *b,
                          bool (*elem_eq)(const void *x, const void *y,
                                          void *ud),
                          void *ud);

/* Pointer comparison model (sec. 11.4): byte addresses compared as
 * unsigned integers. For two addresses into the SAME static object,
 * the byte-offset ordering equals the address ordering and is decided
 * here (out = -1/0/1); for addresses into DISTINCT objects the final
 * addresses are link-time values, so the comparison is runtime-only
 * (the function returns false and leaves *out untouched - the
 * semantics are still defined: compare the final byte addresses as
 * unsigned integers at runtime). */
bool expr_cmp_addr(const NameSymbol *a_sym, int64_t a_off,
                   const NameSymbol *b_sym, int64_t b_off, int *out);

/* ---------------------------------------------------------------------------
 * Build-level operator-site check (AIC-E0405..E0411 at the 13b1-owned
 * const-context sites)
 * ------------------------------------------------------------------------- */

typedef enum ExprOpsStatus {
    EXPR_OPS_OK = 0,         /* all owned sites evaluated; no records */
    EXPR_OPS_DIAG_ERROR,     /* AIC-E0405..E0411 records produced */
    EXPR_OPS_UNSUPPORTED,    /* defensive: malformed input; nothing owned */
    EXPR_OPS_OOM             /* allocation failure; nothing owned */
} ExprOpsStatus;

/* Walk the const-context sites owned by WP-M0-13b1 - array type
 * extents (sec. 12.1), switch case labels (sec. 13.2), and local const
 * declaration initializers (sec. 8.1) - and emit one authoritative
 * record per site whose evaluation fails with a const failure kind
 * (AIC-E0405..E0411), in the deterministic order of the
 * DIAGNOSTIC-CONTRACT sec. 9 comparator (records are sorted before
 * return). Sites that are not constant expressions (13b1's AIC-E0401)
 * yield no record here; global const/var initializers and enum member
 * value expressions (12b2's sites) are never walked. Classification
 * delegates to the public WP-M0-12b2 classifier (rec_fail_classify),
 * which types the arithmetic kinds through WP-M0-12b1.
 *
 * Returns:
 *   EXPR_OPS_OK            no const failures; *out_records NULL.
 *   EXPR_OPS_DIAG_ERROR    *out_records set (owned by the caller; free
 *                          with types_records_free) and *out_count set.
 *   EXPR_OPS_UNSUPPORTED   defensive; nothing owned.
 *   EXPR_OPS_OOM           nothing owned.
 */
ExprOpsStatus expr_ops_check(const NameResult *result,
                             const LayoutBuild *layout,
                             DiagRecord ***out_records,
                             size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_EXPR_OPS_H */
