/* bootstrap/src/sema/fn_core.h
 *
 * AI-Co Stage-0 function return rules (WP-M0-13d1).
 *
 * Implements the return-value-mismatch rule of spec sec. 13.4-13.5 over
 * the resolved build (WP-M0-10 NameResult):
 *
 *   - AIC-E0415 (return value mismatch, sec. 13.4 / DIAGNOSTIC-CONTRACT
 *     sec. 11.5): `return expr;` in a `void` function (a value in a void
 *     function) and bare `return;` in a non-`void` function (a missing
 *     value in a non-void function) are rejected. `return;` or a bare
 *     block end in a `void` function is valid; `return expr;` in a
 *     non-`void` function is valid (its type compatibility is owned by
 *     the WP-M0-11c conversion check - see below).
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults plus the explicit
 * recovery marking):
 *   - AIC-E0415  message "return value in void function" (corpus-pinned
 *                by tests/negative/cases/derived-semantic-return-value-
 *                in-void); primary span = the whole return statement
 *                (from the start of the `return` keyword through the
 *                terminating `;`, corpus-pinned). For the missing-value
 *                direction the message is "return value missing in
 *                non-void function" (documented decision; no corpus
 *                anchor) and the primary span is likewise the whole
 *                return statement (documented decision; mirrors the
 *                value-in-void convention).
 *
 * Ownership boundaries:
 *   - AIC-E0412/E0413/E0414/E0420 (switch terminators, duplicates,
 *     break/continue placement) are owned by WP-M0-13c1 (stmt_core);
 *     AIC-E0416/E0417 (reachability) are owned by WP-M0-13c2
 *     (stmt_reach); entry main validation (AIC-E0418) and reserved-name
 *     rules are owned by WP-M0-13d2 (fn_main). None of these are EVER
 *     produced here. Return-value TYPE compatibility (`return expr;`
 *     assignable to the declared non-void return type) is owned by the
 *     WP-M0-11c conversion check (types_convert_check) and is NEVER
 *     produced here: this package checks only presence/absence of the
 *     value against the function's return type void-ness.
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every module-
 * scope declaration, descending into function bodies (blocks, if/else,
 * while/for with their bodies, switch clauses and their bodies) and
 * checking every `return` statement against its enclosing function's
 * return type. Statements never nest inside expressions in this
 * language, so no expression is descended into. Records are returned
 * sorted with the DIAGNOSTIC-CONTRACT sec. 9 comparator
 * (diag_sort_records). Callers are expected to run the name, type,
 * layout, conversion, operator, const-eval, 13b1/13b2, 13c1, and 13c2
 * stages first; on a clean build the defensive FN_CORE_UNSUPPORTED
 * paths are unreachable.
 *
 * Ownership:
 *   - On FN_CORE_OK / FN_CORE_DIAG_ERROR / FN_CORE_UNSUPPORTED,
 *     *out_records / *out_record_count (when non-empty) are owned by
 *     the caller via types_records_free. The NameResult and LayoutBuild
 *     are borrowed and never modified.
 *   - On FN_CORE_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_FN_CORE_H
#define AICO_BOOTSTRAP_SRC_SEMA_FN_CORE_H

#include "../name/name.h"
#include "../types/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Build-level return-rule check (AIC-E0415)
 * ------------------------------------------------------------------------- */

typedef enum FnCoreStatus {
    FN_CORE_OK = 0,        /* all owned return rules checked; no records */
    FN_CORE_DIAG_ERROR,    /* AIC-E0415 records produced */
    FN_CORE_UNSUPPORTED,   /* defensive: malformed input; nothing owned */
    FN_CORE_OOM            /* allocation failure; nothing owned */
} FnCoreStatus;

/* Walk every function body of the resolved build and emit one
 * authoritative record per owned violation: a return statement with a
 * value in a `void` function, or a bare return statement in a non-`void`
 * function (AIC-E0415, spec sec. 13.4). The check is purely syntactic on
 * statement shape plus the function's return-type void-ness; no
 * expression semantics are evaluated here (return-value type
 * compatibility is owned by the WP-M0-11c conversion check).
 *
 * `layout` is accepted for pipeline symmetry with the sibling sema
 * stages and is only NULL-checked; return rules are purely structural,
 * no layout computation is consulted.
 *
 * Returns:
 *   FN_CORE_OK            no violations; *out_records NULL.
 *   FN_CORE_DIAG_ERROR    *out_records set (owned by the caller; free
 *                         with types_records_free) and *out_count set.
 *   FN_CORE_UNSUPPORTED   defensive; nothing owned.
 *   FN_CORE_OOM           nothing owned.
 */
FnCoreStatus fn_core_check(const NameResult *result,
                           const LayoutBuild *layout,
                           DiagRecord ***out_records,
                           size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_FN_CORE_H */
