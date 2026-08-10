/* bootstrap/src/types/layout.h
 *
 * AI-Co Stage-0 struct/enum layout and deterministic padding (WP-M0-11b).
 *
 * Computes, over a resolved build (WP-M0-10 NameResult):
 *   - struct layout per spec sec. 7.4: fields in declaration order, each
 *     field at the first offset that is a multiple of alignof(field); the
 *     struct's alignment is the maximum field alignment and its size is
 *     rounded up to that alignment; no reordering, bit-fields, packing
 *     controls, or unions;
 *   - deterministic padding per spec sec. 9.4: the padding byte ranges
 *     (inter-field gaps plus tail padding) are identified exactly so the
 *     semantic/IR layers can guarantee zero-on-initialization and
 *     preserved-on-assignment; the ranges are deterministic for a build;
 *   - enum layout per spec sec. 7.5: size/alignment equal to the
 *     underlying integer type T; member values continue the sequence
 *     (previous + 1, first member defaults to 0); aliasing constants are
 *     permitted; a member value not representable in T is rejected with
 *     AIC-T0301.
 *
 * Primitive sizes/alignments come from the spec sec. 7.1 table
 * (type_tables.h); composite sizes come from the sec. 7.2 table (slice
 * 16/8, pointer 8/8, array N * sizeof(T) with alignment alignof(T));
 * named struct/enum types use their declaration layout.
 *
 * Member-value and array-extent constant expressions: WP-M0-12 (the
 * constant-expression evaluator) owns full const_expr composition
 * (sec. 10.5). This package evaluates the bounded deterministic subset it
 * needs at this stage: integer literals, parenthesized expressions, unary
 * + - ~, and binary + - * / % << >> & | ^ over integer values (see the
 * "Bounded constant-integer evaluation" section below and layout.c).
 * Any other expression form yields LAYOUT_UNEVALUABLE and no record: the
 * const evaluator (WP-M0-12) is the owner of full composition, and the
 * const failure codes (AIC-E0401, AIC-E0405..E0411) belong to later
 * packages. LAYOUT_UNEVALUABLE is surfaced so the driver can route those
 * programs to the const stage; it is not a rejection by this package.
 *
 * Diagnostics: AIC-T0301 records carry phase "type", severity "error",
 * recovery "authoritative" and are returned sorted with the contract
 * sec. 9 comparator. The primary span is the member's value expression
 * span for explicit values (corpus-pinned by
 * tests/negative/cases/derived-type-enum-value-overflow) and the member
 * identifier span for implicit (continuation) failures.
 *
 * Ownership:
 *   - On LAYOUT_OK / LAYOUT_DIAG_ERROR / LAYOUT_UNEVALUABLE,
 *     *out_build (and, when non-empty, *out_records / *out_record_count)
 *     are owned by the caller: layout_build_free / types_records_free.
 *     The NameResult is borrowed and never modified.
 *   - On LAYOUT_OOM / LAYOUT_UNSUPPORTED nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_TYPES_LAYOUT_H
#define AICO_BOOTSTRAP_SRC_TYPES_LAYOUT_H

#include "type_tables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */

typedef enum LayoutStatus {
    LAYOUT_OK = 0,          /* all layouts computed; no records */
    LAYOUT_DIAG_ERROR,      /* layouts computed; AIC-T0301 records exist */
    LAYOUT_UNEVALUABLE,     /* a member-value/array-extent expression is
                             * outside the 11b subset (WP-M0-12 owns it);
                             * no record was emitted for that site */
    LAYOUT_UNSUPPORTED,     /* defensive: malformed input / layout cycle /
                             * unknown symbol; nothing owned */
    LAYOUT_OOM              /* allocation failure; nothing owned */
} LayoutStatus;

/* ---------------------------------------------------------------------------
 * Layout results
 * ------------------------------------------------------------------------- */

/* Size and alignment of one type. `align` is 0 only for void (defensive:
 * void cannot appear in a value/field position; the void-type rejection is
 * AIC-T0306, owned by WP-M0-11d). */
typedef struct LayoutSizeAlign {
    int64_t size;
    int64_t align;
} LayoutSizeAlign;

/* One padding byte range (offset, length) of a struct, computed from the
 * field offsets: inter-field gaps plus tail padding (spec sec. 9.4). */
typedef struct LayoutPadRange {
    int64_t offset;
    int64_t length;
} LayoutPadRange;

/* One laid-out struct field (spec sec. 7.4). `name` is borrowed from the
 * AST. `pad_before` is the number of padding bytes between the previous
 * field's end and this field's offset (0 for the first field). */
typedef struct LayoutField {
    const char *name;
    int64_t offset;
    LayoutSizeAlign type;   /* field type size/alignment */
    int64_t pad_before;
} LayoutField;

/* Layout of one struct declaration. `fields` is an owned array of
 * nfields entries in declaration order. `tail_padding` is
 * size - (end of last field). An empty struct has size 0, alignment 1,
 * no fields, no tail padding (documented decision: nothing to align). */
typedef struct LayoutStruct {
    int64_t size;
    int64_t align;
    size_t nfields;
    LayoutField *fields;
    int64_t tail_padding;
} LayoutStruct;

/* One enum member value (spec sec. 7.5). `name` is borrowed from the AST.
 * The mathematical value is:
 *   - `big_unsigned == false`: `value` is the exact int64 value;
 *   - `big_unsigned == true`: the value is in [2^63, 2^64-1] and `value`
 *     holds its two's-complement bit pattern (read as uint64_t);
 *   - `domain_overflow == true`: the value exceeds 2^64-1 (only reachable
 *     through implicit continuation, e.g. A = u64 max, then B). Such a
 *     value is not representable in any integer type and always fails the
 *     sec. 7.5 representability check.
 * `has_explicit` is true when the member carried `= expr`. */
typedef struct LayoutEnumMember {
    const char *name;
    int64_t value;
    bool has_explicit;
    bool big_unsigned;
    bool domain_overflow;
} LayoutEnumMember;

/* Layout of one enum declaration: member values plus size/alignment equal
 * to the underlying type (spec sec. 7.5). `members` is an owned array of
 * nmembers entries in declaration order. */
typedef struct LayoutEnum {
    LayoutSizeAlign underlying;
    int64_t size;
    int64_t align;
    size_t nmembers;
    LayoutEnumMember *members;
} LayoutEnum;

/* All layouts of a build, keyed by declaration symbol (the NameSymbol for
 * each struct/enum declaration). Struct layouts are computed in the same
 * deterministic order as the completeness pass (entry module first, then
 * imports depth-first; within a module, top-level declarations in source
 * order), so by-value struct references always resolve to an
 * already-computed layout in valid builds (the completeness pass rejects
 * forming a field of an incomplete struct). */
typedef struct LayoutBuild {
    const NameSymbol **struct_syms;
    LayoutStruct *struct_layouts;
    size_t nstructs, structs_cap;
    const NameSymbol **enum_syms;
    LayoutEnum *enum_layouts;
    size_t nenums, enums_cap;
} LayoutBuild;

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

/* Compute the layouts of every struct and enum in the resolved build
 * (spec sec. 7.4, 7.5, 9.4). `result` must be the resolved build; callers
 * are expected to run completeness first and stop on diagnostics, as the
 * pipeline does (layout on an invalid build is undefined defensively
 * guarded).
 *
 * Returns:
 *   LAYOUT_OK              no records; *out_build set.
 *   LAYOUT_DIAG_ERROR      *out_build set, *out_records set (AIC-T0301).
 *   LAYOUT_UNEVALUABLE     *out_build set; some member-value/array-extent
 *                          expression was outside the 11b subset; no
 *                          record was emitted for those sites (WP-M0-12
 *                          owns full const evaluation). Records from
 *                          representability failures may also be present.
 *   LAYOUT_UNSUPPORTED     defensive; nothing owned.
 *   LAYOUT_OOM             nothing owned.
 */
LayoutStatus types_layout_build(const NameResult *result,
                                LayoutBuild **out_build,
                                DiagRecord ***out_records,
                                size_t *out_record_count);

void layout_build_free(LayoutBuild *build);

/* ---------------------------------------------------------------------------
 * Query helpers (consumers: WP-M0-11c/11d, WP-M0-12)
 * ------------------------------------------------------------------------- */

/* The layout of a struct/enum declaration symbol, or NULL when the symbol
 * is not a struct/enum declaration or not present in the build. */
const LayoutStruct *layout_build_struct(const LayoutBuild *build,
                                        const NameSymbol *sym);
const LayoutEnum *layout_build_enum(const LayoutBuild *build,
                                    const NameSymbol *sym);

/* Size/alignment of an AST type node in `module` (recursive; struct/enum
 * references resolved through `build`). Returns LAYOUT_OK, LAYOUT_OOM, or
 * LAYOUT_UNSUPPORTED (defensive: unknown symbol/cycle). Never returns
 * LAYOUT_DIAG_ERROR / LAYOUT_UNEVALUABLE: size/alignment of an array type
 * whose extent expression is outside the 11b subset returns
 * LAYOUT_UNEVALUABLE. `out` is untouched unless LAYOUT_OK. */
LayoutStatus layout_build_type_info(const LayoutBuild *build,
                                    const NameModule *module,
                                    const AstNode *type_node,
                                    LayoutSizeAlign *out);

/* Fill `ranges` (capacity `cap`) with the padding byte ranges of `ls`:
 * every inter-field gap and the tail padding, in ascending offset order
 * (spec sec. 9.4). Returns the number of ranges (at most nfields + 1).
 * Pass NULL/0 to query the count. Deterministic for a build. */
size_t layout_struct_padding(const LayoutStruct *ls, LayoutPadRange *ranges,
                             size_t cap);

/* ---------------------------------------------------------------------------
 * Bounded constant-integer evaluation (documented 11b subset)
 * ------------------------------------------------------------------------- */

/* Status of evaluating one member-value / array-extent expression. */
typedef enum LayoutEvalStatus {
    LAYOUT_EVAL_OK = 0,
    LAYOUT_EVAL_UNEVALUABLE,  /* expression form outside the subset */
    LAYOUT_EVAL_DIV_ZERO,     /* division or modulo by zero */
    LAYOUT_EVAL_SHIFT_RANGE,  /* shift count outside [0, 63] */
    LAYOUT_EVAL_OVERFLOW      /* arithmetic outside the subset domain */
} LayoutEvalStatus;

/* Value shape (same convention as LayoutEnumMember: `big` marks a value in
 * [2^63, 2^64-1] stored two's-complement in `v`). */
typedef struct LayoutEvalValue {
    int64_t v;
    bool big;
} LayoutEvalValue;

/* Evaluate `expr` (an integer constant expression over the 11b subset:
 * integer literals, parentheses, unary + - ~, binary + - * / % << >> & |
 * ^). `out` is untouched unless LAYOUT_EVAL_OK. Exposed for consumers
 * (e.g. array extent queries) and tests. */
LayoutEvalStatus layout_eval_int_expr(const AstNode *expr,
                                      LayoutEvalValue *out);

#endif /* AICO_BOOTSTRAP_SRC_TYPES_LAYOUT_H */
