/* bootstrap/src/types/optype.h
 *
 * AI-Co Stage-0 explicit cast/wrap matrix and operator typing (WP-M0-11d).
 *
 * Implements, over the resolved build (WP-M0-10 NameResult) and the type
 * descriptors of WP-M0-11a:
 *   - the explicit cast matrix of spec sec. 11.2 and the wrap<T> rules of
 *     spec sec. 11.5 (AIC-T0308);
 *   - binary/unary/assignment operator applicability per spec sec. 10.2
 *     with the rejections of sec. 11.2/11.4 (AIC-T0306), equality on
 *     array/struct (AIC-T0304), chained comparisons (AIC-T0305), void
 *     misuse at cast/wrap targets;
 *   - conditions (sec. 13.1 AIC-T0310), switch selectors (sec. 13.2
 *     AIC-T0311), call argument counts (sec. 11.4 AIC-T0312);
 *   - array literal element counts in typed contexts (sec. 7.3
 *     AIC-T0309) and struct literal field checks (sec. 12.7 AIC-T0313).
 *
 * Boundary with WP-M0-11c: 11c decides whether an implicit conversion is
 * permitted (the whitelist) and computes common types; 11d never checks
 * conversions and never duplicates 11c's AIC-T0307 records. Integer
 * operand pairs whose common type is missing are 11c's rejection; 11d
 * treats every integer/integer pair as an applicable operator pair.
 * Non-integer operand pairs (bool/bool, str/str, enum/enum, pointer
 * pairs, mismatched pairs) carry no implicit conversion, so their
 * validity is decided here. Ternary then/else branches with no common
 * type are rejected here with AIC-T0307 (11c marks them unknown, see
 * convert.h "mismatched non-integer: 11d").
 *
 * Bound with the later packages: lvalue/mutability/const checks
 * (AIC-E0402/E0404/E0419), index/slice-bound to-usize conversions
 * (sec. 12.1/12.4), array/struct assignment semantics, and the runtime
 * built-in signatures are owned by later packages. This package only
 * checks operator-applicability types, never evaluation semantics.
 *
 * Diagnostics: records carry phase "type", severity "error", recovery
 * "authoritative" and are returned sorted with the contract sec. 9
 * comparator (diag_sort_records). The corpus
 * (tests/negative/cases/derived-type-*) pins these message shapes and
 * primary spans:
 *   - AIC-T0304: "'==' operator not applicable to struct type 'Point'"
 *     (or "array type 'i32[3]'"); span: the ==/!= operator token.
 *   - AIC-T0305: "chained comparison is not allowed"; span: the
 *     leftmost comparison operator of the chain (corpus-pinned;
 *     DIAGNOSTIC-CONTRACT sec. 11.4 text says "the second comparison
 *     operator" - the accepted corpus pins the first/leftmost operator
 *     and is the stable oracle here; documented discrepancy).
 *   - AIC-T0306: "'+' operator not applicable to operand type 'bool'";
 *     span: the operator token (recovered from the source text between
 *     the operand spans - the AST does not store operator token spans).
 *   - AIC-T0308: "invalid explicit cast pair: str to i32"; span: the
 *     whole cast/wrap expression.
 *   - AIC-T0309: "array literal element count mismatch: expected 3,
 *     found 2"; span: the whole literal.
 *   - AIC-T0310: "condition must be bool, found i32"; span: the
 *     condition expression.
 *   - AIC-T0311: "switch selector must be integer or enum type, found
 *     str"; span: the selector expression.
 *   - AIC-T0312: "call argument count mismatch: expected 2, found 1";
 *     span: the whole call.
 *   - AIC-T0313: "unknown field 'z' in struct literal of type 'Point'";
 *     span: the offending field name.
 * Unpinned shapes chosen deterministically (no corpus fixture):
 *   - wrap pairs: "invalid explicit wrap pair: <from> to <to>";
 *   - T0306 for non-pinned operators/operands: "'<op>' operator not
 *     applicable to operand type '<desc>'"; for compound assignment the
 *     operator spelling is the compound form ("+=", ...); for member
 *     access on a non-struct base the same shape; for a struct base
 *     without the field "'<op>' operator not applicable to struct type
 *     '<desc>' (no field '<name>')"; for len/ptr "'len' ..."; for
 *     struct literals on a non-struct base "'{}' operator not
 *     applicable to operand type '<desc>'"; void cast/wrap targets use
 *     "'cast'/'wrap' operator not applicable to operand type 'void'";
 *   - T0313 duplicate/missing fields: "duplicate field '<name>' in
 *     struct literal of type '<desc>'" (span: the repeated field name)
 *     and "missing field '<name>' in struct literal of type '<desc>'"
 *     (span: the whole literal).
 * The <desc> renderings are type_describe() output; the null literal
 * renders as "null".
 *
 * Ownership:
 *   - On OPTYPE_OK / OPTYPE_DIAG_ERROR / OPTYPE_UNKNOWN, records (when
 *     non-empty) are owned by the caller via types_records_free. The
 *     NameResult is borrowed and never modified.
 *   - On OPTYPE_UNSUPPORTED / OPTYPE_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_TYPES_OPTYPE_H
#define AICO_BOOTSTRAP_SRC_TYPES_OPTYPE_H

#include "layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Explicit cast/wrap pair predicates (spec sec. 11.2, sec. 11.5)
 * ------------------------------------------------------------------------- */

/* True when the explicit cast `cast<to>(from)` is a valid pair per the
 * sec. 11.2 matrix. `from_is_null` marks a null-literal source (which
 * carries no Type; `from` must then be NULL). NULL types are never a
 * valid pair (false). Identity (cast<T>(e) where typeof(e) == T) is
 * valid for all types: sec. 11.6 treats identical types as the
 * identity mechanism (the matrix's bool/bool and slice/slice identity
 * rows confirm identity is a conversion mechanism; documented decision).
 * The valid rows:
 *   - identity (any kind);
 *   - integer -> integer (cast and wrap both per sec. 11.5);
 *   - bool -> integer and integer -> bool (cast only);
 *   - enum -> its underlying integer type (cast only; checked
 *     identity, not a re-interpretation);
 *   - integer -> enum (cast only; value checked at const/runtime);
 *   - enum -> enum (different enum type; cast only);
 *   - pointer <-> usize/u64/isize/i64 and integer -> pointer
 *     (cast only; raw-pointer re-interpretation is permitted only
 *     through the explicit cast per ADR-004);
 *   - T* -> U* (different pointee; cast only);
 *   - str <-> u8[] (cast only);
 *   - null -> any T* (cast only).
 * Everything else is false (the operand pair of AIC-T0308). */
bool optype_cast_pair_valid(const Type *from, bool from_is_null,
                            const Type *to);

/* True when the explicit wrap `wrap<to>(from)` is valid per sec. 11.5:
 * the target is an integer primitive and the source is an integer
 * primitive or an enum type (identity included; null is never a wrap
 * source). */
bool optype_wrap_pair_valid(const Type *from, bool from_is_null,
                            const Type *to);

/* ---------------------------------------------------------------------------
 * Build-level operator typing check (spec sec. 10.2 / 11.2)
 * ------------------------------------------------------------------------- */

typedef enum OptypeStatus {
    OPTYPE_OK = 0,          /* all reachable operator sites checked; no records */
    OPTYPE_DIAG_ERROR,      /* AIC-T0304/05/06/08/09/10/11/12/13 records produced */
    OPTYPE_UNKNOWN,         /* some site's type was outside the 11d expression
                             * subset; no record for those sites (later packages
                             * own them); records may also be present alongside
                             * UNKNOWN (in which case OPTYPE_DIAG_ERROR wins) */
    OPTYPE_UNSUPPORTED,     /* defensive: malformed input; nothing owned */
    OPTYPE_OOM              /* allocation failure; nothing owned */
} OptypeStatus;

/* Check every operator/typing site in the resolved build and produce the
 * AIC-T03xx records above, with the source spans pinned by the corpus.
 * Callers are expected to run completeness, layout, and the 11c
 * conversion check first and stop on their diagnostics, as the pipeline
 * does.
 *
 * Returns:
 *   OPTYPE_OK            no records.
 *   OPTYPE_DIAG_ERROR    records exist (*out_records set).
 *   OPTYPE_UNKNOWN       no records were produced, but at least one site
 *                        could not be typed; records may also be present
 *                        alongside UNKNOWN.
 *   OPTYPE_UNSUPPORTED   defensive; nothing owned.
 *   OPTYPE_OOM           nothing owned.
 */
OptypeStatus types_optype_check(const NameResult *result,
                                DiagRecord ***out_records,
                                size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_TYPES_OPTYPE_H */
