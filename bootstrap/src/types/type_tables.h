/* bootstrap/src/types/type_tables.h
 *
 * AI-Co Stage-0 type tables and completeness rules (WP-M0-11a).
 *
 * Primitive/composite type tables transcribed from spec sec. 7.1-7.2, and
 * the completeness pass for spec sec. 7.6:
 *   - A struct type is incomplete until its closing brace. Forming a
 *     value, array, slice, field, or dereference of an incomplete struct
 *     type is rejected with AIC-T0302.
 *   - Struct recursion through pointers is permitted; struct recursion by
 *     value is rejected (infinite size) with AIC-T0303.
 *   - Enums are complete immediately after their declaration.
 *
 * The tables here are the normative catalog: the thirteen primitives with
 * the spec's recorded size/alignment facts, and the five composite forms
 * with their spec-recorded shape/layout descriptions. They are DATA (the
 * accepted spec tables), not layout computation: deriving composite sizes
 * and offsets is WP-M0-11b (bootstrap/src/types/layout.*). Enum member
 * value rules (AIC-T0301) belong to 11b as well (enum layout).
 *
 * Completeness diagnostics follow the accepted negative corpus exactly:
 *   - AIC-T0302 message: "use of incomplete struct type '<S>' as a value";
 *     primary span: the incomplete struct's declaration-name span.
 *   - AIC-T0303 message: "struct '<S>' has infinite size due to recursive
 *     by-value field '<f>'"; primary span: the struct's declaration-name
 *     span. (The corpus pins the declaration-name span for both codes;
 *     the field name is carried in the message.)
 * Records are returned sorted with the contract sec. 9 comparator and
 * carry phase "type", severity "error", recovery "authoritative".
 */
#ifndef AICO_BOOTSTRAP_SRC_TYPES_TYPE_TABLES_H
#define AICO_BOOTSTRAP_SRC_TYPES_TYPE_TABLES_H

#include "type_identity.h"

#include "../diag/diag.h"
#include "../name/name.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Primitive type table (spec sec. 7.1)
 * ------------------------------------------------------------------------- */

/* One row of the sec. 7.1 table. `name` is the source spelling; the
 * size/alignment values are the spec's recorded facts for the initial
 * target (little-endian Windows x86-64) and are used by the layout
 * package (WP-M0-11b) when deriving composite layout. */
typedef struct TypePrimInfo {
    AstPrimKind kind;
    const char *name;        /* "void", "bool", "str", "i8", ..., "usize" */
    bool is_integer;         /* the ten integer types (bool is not) */
    bool is_signed;          /* meaningful only when is_integer */
    bool is_pointer_sized;   /* isize/usize */
    int width_bits;          /* 0 for void/bool/str; 8/16/32/64 for ints */
    int size_bytes;          /* spec sec. 7.1 Size column */
    int align_bytes;         /* spec sec. 7.1 Alignment column */
} TypePrimInfo;

/* Look up a primitive by kind; NULL for an invalid kind. */
const TypePrimInfo *types_prim_info(AstPrimKind kind);
/* Look up a primitive by source name; NULL when the name is not a
 * primitive type name (also NULL for a future non-primitive). */
const TypePrimInfo *types_prim_by_name(const char *name);
/* Number of rows in the table (13). */
size_t types_prim_count(void);

/* ---------------------------------------------------------------------------
 * Composite type table (spec sec. 7.2)
 * ------------------------------------------------------------------------- */

typedef enum TypeCompositeKind {
    TY_COMPOSITE_ARRAY = 0,   /* T[N] */
    TY_COMPOSITE_SLICE,       /* T[] */
    TY_COMPOSITE_PTR,         /* T* */
    TY_COMPOSITE_STRUCT,      /* struct S { ... } */
    TY_COMPOSITE_ENUM         /* enum E: T { ... } */
} TypeCompositeKind;

/* One row of the sec. 7.2 table. `layout_note` is the spec's recorded
 * "Layout (initial target)" text; deriving those layouts is 11b. */
typedef struct TypeCompositeInfo {
    TypeCompositeKind kind;
    const char *name;         /* "array", "slice", "pointer", "struct", "enum" */
    const char *form;         /* "T[N]", "T[]", "T*", "struct S { ... }",
                               * "enum E: T { ... }" */
    const char *layout_note;  /* spec sec. 7.2 Layout column (recorded) */
} TypeCompositeInfo;

const TypeCompositeInfo *types_composite_info(TypeCompositeKind kind);
size_t types_composite_count(void);

/* ---------------------------------------------------------------------------
 * Completeness pass (spec sec. 7.6)
 * ------------------------------------------------------------------------- */

typedef enum TypeCheckStatus {
    TYPE_CHECK_OK = 0,      /* completeness holds; no diagnostics */
    TYPE_CHECK_DIAG_ERROR,  /* completeness diagnostics produced */
    TYPE_CHECK_OOM          /* allocation failure; nothing produced */
} TypeCheckStatus;

/* Check completeness of every struct/enum in the resolved build
 * (spec sec. 7.6):
 *   - by-value recursion inside a struct declaration (a field whose type
 *     reaches the struct itself through value positions: the field type,
 *     or an array/slice element; a pointer stops the walk) is rejected;
 *   - forming a value, array, slice, field, or dereference of an
 *     incomplete struct type is rejected;
 *   - enums are complete after their declaration (never rejected here).
 *
 * The by-value-recursion-vs-incomplete-use selection follows the accepted
 * negative corpus: when a struct that recurses by value is also used as a
 * value anywhere in the build (variable/constant declaration, parameter,
 * return type, struct literal, array/slice of the struct at a value
 * position), the use-of-incomplete-struct rule (AIC-T0302) is reported at
 * the struct's declaration-name span; when the recursive struct is not
 * used as a value anywhere, the infinite-size rule (AIC-T0303) is
 * reported at the same span.
 *
 * `result` is the resolved build (WP-M0-10 output). Records (when
 * non-empty) are owned by the caller via types_records_free; the
 * NameResult is borrowed and never modified.
 *
 * Returns TYPE_CHECK_OK / TYPE_CHECK_DIAG_ERROR with *out_records set
 * (and *out_record_count when non-empty), or TYPE_CHECK_OOM with nothing
 * owned.
 */
TypeCheckStatus types_check_completeness(const NameResult *result,
                                         DiagRecord ***out_records,
                                         size_t *out_record_count);

void types_records_free(DiagRecord **records, size_t count);

#endif /* AICO_BOOTSTRAP_SRC_TYPES_TYPE_TABLES_H */
