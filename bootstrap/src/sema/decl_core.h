/* bootstrap/src/sema/decl_core.h
 *
 * AI-Co Stage-0 declaration model and assignability (WP-M0-13a1).
 *
 * Implements the declaration model of spec sec. 8 (constants, variables,
 * storage duration, mutability) over the resolved build (WP-M0-10
 * NameResult), and the lvalue/mutability/assignability checks of
 * sec. 8.4 / 12.5 with the rejections the earlier packages defer:
 *   - AIC-E0402  address of const / address of non-lvalue (sec. 8.1,
 *                sec. 8.3, sec. 12.5);
 *   - AIC-E0404  assignment to const (sec. 8.4);
 *   - AIC-E0419  assignment to non-lvalue (sec. 8.4).
 * The missing-initializer rule (AIC-E0403) belongs to WP-M0-13a2 and the
 * const-expression composition rule (AIC-E0401) to WP-M0-12a / WP-M0-13b1;
 * neither is produced here.
 *
 * Declaration model (sec. 8.3):
 *   - module-scope `var` has static storage; function-local `var` and
 *     parameters have automatic storage; a `const` at any scope has no
 *     storage location (it is a compile-time value).
 *   - `var` names and parameters denote mutable lvalues; `const` names
 *     denote immutable values (sec. 8.4).
 *
 * Lvalue model (documented decisions; the spec pins names, dereference,
 * and the slice-index example, and is silent elsewhere - the following
 * is the coherent minimal reading):
 *   - mutable lvalues: var/param identifiers, dereference results
 *     (`*p` is always a mutable lvalue, sec. 12.5), array/slice element
 *     access on a mutable base (sec. 18.6 example `sl[0] = 65u8;` is
 *     normative "valid"), and struct field access on a mutable base
 *     (`p->f` == `(*p).f`, so arrow access is always mutable when it
 *     resolves to a field);
 *   - immutable objects (AIC-E0404 / E0402 class): const names and
 *     const-derived element/field access, and enum members (sec. 7.5
 *     constants);
 *   - non-lvalues (AIC-E0419 / E0402 class): literals, arithmetic and
 *     logic expressions, calls, casts/wraps, len/ptr, slice expressions,
 *     struct literals, ternary, assignment expressions, `&` results,
 *     and `str` element access - `str` is an immutable byte sequence and
 *     indexing it "yields u8 (byte value)" (sec. 12.2), i.e. a value,
 *     never an lvalue.
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults plus the explicit
 * recovery marking; primary spans corpus-pinned where the negative
 * corpus has anchors, else the same deterministic convention):
 *   - AIC-E0402  message "address of const is not allowed" (corpus-pinned
 *                by tests/negative/cases/derived-semantic-addr-of-const)
 *                for a const operand, "address of non-lvalue is not
 *                allowed" otherwise; primary span = the whole address-of
 *                expression node (corpus-pinned for the const case; the
 *                same convention for the non-lvalue case);
 *   - AIC-E0404  message "assignment to const '<name>'" (corpus-pinned by
 *                tests/negative/cases/derived-semantic-assign-to-const)
 *                where <name> is the immutable object's unqualified
 *                symbol name (the const name, or the enum member name);
 *                primary span = the immutable object's declaration
 *                identifier span (corpus-pinned; for a const-derived
 *                member/element target this is the const declaration's
 *                identifier);
 *   - AIC-E0419  message "assignment target is not a modifiable lvalue"
 *                (corpus-pinned by
 *                tests/negative/cases/derived-semantic-assign-non-lvalue);
 *                primary span = the assignment target expression node.
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every
 * declaration in source order: global var/const initializers, enum
 * member value expressions, struct field types (array extents), and
 * every function body (statements and all nested expressions). Records
 * are returned sorted with the DIAGNOSTIC-CONTRACT sec. 9 comparator
 * (diag_sort_records). Callers are expected to run the name, type,
 * layout, conversion, operator, and const-eval stages first and stop on
 * their diagnostics, as the pipeline does; on a clean build the
 * defensive DECL_UNSUPPORTED paths are unreachable.
 *
 * Ownership:
 *   - On DECL_OK / DECL_DIAG_ERROR, *out_records / *out_record_count
 *     (when non-empty) are owned by the caller via types_records_free.
 *     The NameResult is borrowed and never modified.
 *   - On DECL_UNSUPPORTED / DECL_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_DECL_CORE_H
#define AICO_BOOTSTRAP_SRC_SEMA_DECL_CORE_H

#include "../name/name.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Declaration model (spec sec. 8.3)
 * ------------------------------------------------------------------------- */

typedef enum DeclStorageKind {
    DECL_STORAGE_NONE = 0,   /* const: no storage location (sec. 8.1/8.3) */
    DECL_STORAGE_STATIC,     /* module-scope var: program lifetime (sec. 8.3) */
    DECL_STORAGE_AUTOMATIC   /* function-local var and parameters (sec. 8.3) */
} DeclStorageKind;

typedef enum DeclMutability {
    DECL_MUTABLE = 0,        /* var names and parameters (sec. 8.4) */
    DECL_IMMUTABLE           /* const names and enum members */
} DeclMutability;

/* Storage duration of a resolved symbol. Returns DECL_STORAGE_NONE for
 * every symbol kind without storage (consts, fields, enum members,
 * functions, struct/enum/module names - defensive for the non-declaration
 * kinds, which never reach a storage query in a valid pipeline). */
DeclStorageKind decl_storage_of_symbol(const NameSymbol *sym);

/* Mutability of a resolved symbol: var/param/field -> MUTABLE;
 * const/enum-member -> IMMUTABLE; other kinds IMMUTABLE (defensive). */
DeclMutability decl_mutability_of_symbol(const NameSymbol *sym);

/* ---------------------------------------------------------------------------
 * Lvalue / assignability analysis (spec sec. 8.4, sec. 10.2, sec. 12.5)
 * ------------------------------------------------------------------------- */

typedef enum DeclLvalueKind {
    DECL_LVALUE_NONE = 0,    /* not an lvalue (AIC-E0419 / E0402) */
    DECL_LVALUE_MUTABLE,     /* modifiable lvalue */
    DECL_LVALUE_CONST        /* immutable object; const_sym is set */
} DeclLvalueKind;

/* Classification of one expression as an assignment/address-of target.
 * `const_sym` is valid when kind == DECL_LVALUE_CONST and names the
 * immutable object (the const declaration symbol, or the enum-member
 * symbol) whose declaration identifier is the E0404 primary span. */
typedef struct DeclLvalue {
    DeclLvalueKind kind;
    const NameSymbol *const_sym;
} DeclLvalue;

/* Classify `expr` (an expression inside `module`) as a target:
 *   - DECL_LVALUE_MUTABLE  assignment and address-of are permitted;
 *   - DECL_LVALUE_CONST    assignment is AIC-E0404 and address-of is
 *                          AIC-E0402 (const names, const-derived
 *                          element/field access, enum members);
 *   - DECL_LVALUE_NONE     assignment is AIC-E0419 and address-of is
 *                          AIC-E0402 (values and non-lvalues).
 * The analysis is purely syntactic over the resolved name tables and the
 * AST; it assumes the build passed name/type/layout/conversion/operator
 * checks (a well-typed `*p` is a pointer dereference, a well-typed
 * `a[i]` indexes an array/slice/str, a well-typed `.f` is a struct
 * field or enum member). */
DeclLvalue decl_expr_lvalue(const NameModule *module, const AstNode *expr);

/* ---------------------------------------------------------------------------
 * Build-level check (AIC-E0402 / AIC-E0404 / AIC-E0419)
 * ------------------------------------------------------------------------- */

typedef enum DeclStatus {
    DECL_OK = 0,             /* all declaration/assignability sites checked;
                              * no records */
    DECL_DIAG_ERROR,         /* AIC-E0402/E0404/E0419 records produced */
    DECL_UNSUPPORTED,        /* defensive: malformed input; nothing owned */
    DECL_OOM                 /* allocation failure; nothing owned */
} DeclStatus;

/* Walk every declaration and expression site of the resolved build
 * (global initializers, enum member values, struct field types, and all
 * function bodies) and emit one authoritative record per violation of
 * the sec. 8.4 / sec. 12.5 lvalue, const, and assignability rules, in
 * the deterministic order of the DIAGNOSTIC-CONTRACT sec. 9 comparator
 * (records are sorted before return).
 *
 * Returns:
 *   DECL_OK            no violations; *out_records NULL.
 *   DECL_DIAG_ERROR    *out_records set (owned by the caller; free with
 *                      types_records_free) and *out_count set.
 *   DECL_UNSUPPORTED   defensive; nothing owned.
 *   DECL_OOM           nothing owned.
 */
DeclStatus decl_check(const NameResult *result,
                      DiagRecord ***out_records,
                      size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_DECL_CORE_H */
