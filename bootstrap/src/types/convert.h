/* bootstrap/src/types/convert.h
 *
 * AI-Co Stage-0 implicit conversions and common type (WP-M0-11c).
 *
 * Implements the implicit-conversion whitelist of spec sec. 11.1 (exactly
 * the table rows; anything else is rejected with AIC-T0307) and the
 * binary-operator common-type promotion of sec. 11.1/11.4, over the
 * resolved build (WP-M0-10 NameResult) and the type descriptors of
 * WP-M0-11a.
 *
 * Boundary with WP-M0-11d: this package decides whether an implicit
 * conversion is permitted (the whitelist) and computes the common type;
 * it never validates operator applicability (AIC-T0306), the explicit
 * cast/wrap matrix (AIC-T0308), void misuse, or the comparison/equality
 * pair rules. Operand pairs that involve no implicit conversion at all
 * (e.g. mismatched enum/enum or str/str comparisons) are not this
 * package's rejections: the conversion whitelist only ever applies to
 * integers, identical types, same-pointee pointers, and null -> T*.
 *
 * Conversion sites checked by types_convert_check (spec sec. 11.1 bullet:
 * initializers, assignments, argument passing, return values, and binary
 * operator common-type promotion):
 *   - global/local var and const declarations (initializer -> declared
 *     type);
 *   - plain assignment `a = v` (v -> typeof(a)) and compound assignment
 *     `a op= v` (a op v per sec. 11.6: the integer op's common type must
 *     exist and must be assignable back to typeof(a));
 *   - call arguments (arg -> parameter type; skipped when the callee is a
 *     runtime built-in whose signature is not in the resolved build, and
 *     when the argument count differs from the parameter count - the
 *     count rule AIC-T0312 belongs to WP-M0-11d);
 *   - return values (value -> declared return type; skipped for void
 *     returns - the value-in-void rejection belongs to a later package);
 *   - binary integer operators (sec. 10.2 result column): + - * / %
 *     and & | ^ use common-type promotion; << >> convert the right
 *     operand to the left operand's type (the shift result is the left
 *     operand type per sec. 10.2, so the right operand is assignment-
 *     converted, not common-type-promoted); comparisons < <= > >= and
 *     equality == != on integers use common-type promotion (sec. 11.4);
 *     logical && || are bool/bool and produce no conversion checks here;
 *   - ternary ?: (then/else must have a common type; the ternary's type
 *     is that common type, sec. 10.2);
 *   - array-literal elements in a typed array context (each element is
 *     checked against the array's element type; the record span is the
 *     whole array literal, corpus-pinned);
 *   - struct-literal field values (each value against the field type).
 *
 * Bounded expression typing: to reach the conversion sites above the
 * package derives the type of an expression over a documented subset
 * (integer/str/bool/null literals; identifiers through the name tables;
 * parentheses; unary + - ~ ! * &; binary arithmetic/bitwise/comparison/
 * logical operators; shifts; index (element type); slice expression;
 * calls to functions in the build (return type); member/arrow access
 * (field or enum-member type); sizeof/alignof (usize); len/ptr builtins;
 * cast/wrap (the target type - validity is 11d's); struct literals;
 * assignments (lvalue type); ternary). Expressions outside the subset,
 * or whose type depends on constructs this package does not type (e.g.
 * runtime built-in calls), yield CONVERT_UNKNOWN and produce no record:
 * the same discipline as 11b's LAYOUT_UNEVALUABLE - the owning package
 * (WP-M0-11d operator typing / WP-M0-13 expressions) reports those.
 * Array literals and null literals are typed only at a conversion site
 * through their destination (no type inference in the language); a
 * standalone array literal has no type and yields UNKNOWN.
 *
 * Indexing and slice-bound operands (spec sec. 12.1/12.4 index-to-usize
 * conversion) are out of scope: they are sec. 12 rules owned by later
 * packages, and this package only types the index expression's result
 * (the element type) when needed for a surrounding conversion site.
 *
 * Diagnostics: AIC-T0307 records carry phase "type", severity "error",
 * recovery "authoritative" and are returned sorted with the contract
 * sec. 9 comparator. The corpus pins three message shapes:
 *   - integer narrowing at a site: "no common type: <from> cannot be
 *     implicitly narrowed to <to>" (span: the source expression; corpus
 *     tests/negative/cases/18-4-type-implicit-narrowing), with " in
 *     array literal" appended for array-literal elements (corpus
 *     tests/negative/cases/18-6-type-array-literal-narrowing; span: the
 *     whole literal);
 *   - array to slice: "implicit array-to-slice conversion is absent"
 *     (corpus tests/negative/cases/18-6-type-array-slice-implicit; span:
 *     the source expression).
 * Unpinned shapes chosen deterministically:
 *   - other non-convertible pairs: "no common type: <from> is not
 *     implicitly convertible to <to>";
 *   - binary operator with no common type: "no common type: <a> and <b>".
 * The <from>/<a>/<b> descriptions are type_describe() renderings, with
 * literal expressions additionally carrying the literal value ("i32
 * literal 200", corpus-pinned) and identifiers carrying the name
 * ("u8[] value 'buf'").
 *
 * Ownership:
 *   - On CONVERT_OK / CONVERT_DIAG_ERROR / CONVERT_UNKNOWN, records (when
 *     non-empty) are owned by the caller via types_records_free. The
 *     NameResult is borrowed and never modified.
 *   - On CONVERT_UNSUPPORTED / CONVERT_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_TYPES_CONVERT_H
#define AICO_BOOTSTRAP_SRC_TYPES_CONVERT_H

#include "layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Implicit-conversion whitelist (spec sec. 11.1)
 * ------------------------------------------------------------------------- */

/* True when `from` may be implicitly converted to `to`, exactly the
 * sec. 11.1 table plus identity (sec. 11.6: "identical type or per
 * Table 11.1"):
 *   - identical types (any kind);
 *   - the ten integer widening rows of Table 11.1 (value-preserving
 *     widening; same-sign widening, and unsigned -> signed only when the
 *     target is strictly wider; identical-width identical-sign pairs
 *     i64<->isize and u64<->usize);
 *   - any T* -> T* (same pointee type);
 *   - null -> any T* is permitted at the expression level (the check API
 *     accepts an explicit null-source flag; there is no "null type" in
 *     the Type model).
 * Everything else (bool<->integer, enum<->integer, array->slice, slice->
 * pointer, str<->other, pointer<->integer, void, ...) is false.
 * NULL operands are never allowed (false). */
bool convert_implicit_allowed(const Type *from, const Type *to);

/* True when `from` (an expression's derived type) may be implicitly
 * converted to `to`, treating a null literal source specially: when
 * `from_is_null` is true, the conversion is allowed iff `to` is a
 * pointer type (null -> any T*, Table 11.1). When `from_is_null` is
 * true, `from` must be NULL. When `from` is NULL and `from_is_null` is
 * false, the pair is not checkable and false is returned (callers use
 * CONVERT_UNKNOWN instead of emitting a record). */
bool convert_implicit_allowed_ex(const Type *from, bool from_is_null,
                                 const Type *to);

/* ---------------------------------------------------------------------------
 * Common type (spec sec. 11.1 bullet / sec. 11.4)
 * ------------------------------------------------------------------------- */

/* Compute the common type of two integer binary operands (spec sec. 11.1
 * bullet): when both operands have the same type, the common type is that
 * type; when they differ, both must be implicitly convertible to the
 * common type - the wider of the two by bit width, with both conversions
 * present in Table 11.1 - else there is no common type (NULL; the caller
 * rejects with AIC-T0307). The result is a NEW Type owned by the caller
 * (type_free), or NULL when no common type exists, either operand is not
 * an integer, or allocation fails (callers must treat allocation failure
 * and no-common-type identically only through the status-level API; the
 * build-level check reports OOM separately).
 *
 * Equal-width identical-sign pairs (i64/isize, u64/usize): both
 * conversions are in the table (identical width/sign) and the value sets
 * are identical, so a common type exists; the deterministic tie-break
 * prefers the fixed-width type (i64 over isize, u64 over usize) so the
 * common type is commutative and reproducible (documented decision; the
 * choice is unobservable in validity, only in the reported type name).
 * Equal-width different-sign pairs (i32/u32, i64/u64, ...) have no
 * common type. */
Type *convert_common_type(const Type *a, const Type *b);

/* ---------------------------------------------------------------------------
 * Build-level conversion check (spec sec. 11.1/11.6)
 * ------------------------------------------------------------------------- */

typedef enum ConvertStatus {
    CONVERT_OK = 0,         /* all reachable sites checked; no records */
    CONVERT_DIAG_ERROR,     /* AIC-T0307 records produced */
    CONVERT_UNKNOWN,        /* some conversion site's type was outside the
                             * 11c expression subset; no record for those
                             * sites (later packages own them) */
    CONVERT_UNSUPPORTED,    /* defensive: malformed input; nothing owned */
    CONVERT_OOM             /* allocation failure; nothing owned */
} ConvertStatus;

/* Check every implicit-conversion site in the resolved build (see the
 * header block for the site list) and produce AIC-T0307 records for
 * conversions outside the sec. 11.1 whitelist, with the conversion
 * site's source span. Callers are expected to run completeness and
 * layout first and stop on diagnostics, as the pipeline does.
 *
 * Returns:
 *   CONVERT_OK            no records.
 *   CONVERT_DIAG_ERROR    records exist (*out_records set).
 *   CONVERT_UNKNOWN       no records were produced, but at least one
 *                         conversion site could not be typed (expression
 *                         outside the 11c subset); records may also be
 *                         present alongside UNKNOWN (in which case
 *                         CONVERT_DIAG_ERROR wins and the caller treats
 *                         the result as diagnostic-bearing).
 *   CONVERT_UNSUPPORTED   defensive; nothing owned.
 *   CONVERT_OOM           nothing owned.
 */
ConvertStatus types_convert_check(const NameResult *result,
                                  DiagRecord ***out_records,
                                  size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_TYPES_CONVERT_H */
