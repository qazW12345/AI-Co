/* bootstrap/src/sema/decl_init.h
 *
 * AI-Co Stage-0 initialization semantics (WP-M0-13a2).
 *
 * Implements the initialization rules of spec sec. 9 over the resolved
 * build (WP-M0-10 NameResult): the initializer-site walker visits every
 * declaration site at which an initializer may appear and enforces the
 * missing-initializer rule of sec. 8.2:
 *   - AIC-E0403  every variable declaration must have an initializer;
 *                there is no uninitialized-variable state in the
 *                language (sec. 8.2). Primary span = the whole
 *                declaration (corpus-pinned by
 *                tests/negative/cases/derived-semantic-missing-init).
 *
 * The initializer forms this package checks are exactly the variable
 * declaration forms of sec. 5.2 (as amended v0.1.3, Planner ruling
 * t_dcb5540e): `var_decl`, `global_var_decl`, and the `var` form of a
 * `for` statement init. The `const` forms keep the strict
 * required-initializer grammar (missing "=" is AIC-S0101 in the parser)
 * and are not variable declarations, so no semantic record exists for
 * them here; a const with NULL init cannot reach the semantic stage
 * through the accepted pipeline.
 *
 * Documented boundaries (other packages own these):
 *   - the constant-expression requirement on global var and const
 *     initializers (AIC-E0401) belongs to WP-M0-12a / WP-M0-13b1 and
 *     is NOT produced here;
 *   - the sec. 8.4 assignability rules (AIC-E0402/E0404/E0419) belong
 *     to WP-M0-13a1 (bootstrap/src/sema/decl_core.*;
 *   - expression semantics of initializer expressions (sec. 10, 11)
 *     belong to WP-M0-13b;
 *   - sec. 9.1 object representation, sec. 9.3 assignment semantics,
 *     and sec. 9.4 deterministic padding are layout/IR/backend rules
 *     with no semantic-stage rejection here.
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults for AIC-E0403 plus the
 * explicit recovery marking; primary span corpus-pinned):
 *   - AIC-E0403  message "missing initializer on variable declaration",
 *                primary span = the whole declaration node span
 *                (the declaration including the terminating ';').
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every top-level
 * declaration in module-scope order, descending into function bodies
 * (including `for` init declarations). Records are returned sorted with
 * the DIAGNOSTIC-CONTRACT sec. 9 comparator (diag_sort_records).
 * Callers are expected to run the name, type, layout, conversion,
 * operator, and const-eval stages first and stop on their diagnostics,
 * as the pipeline does; on a clean build the defensive DECLINIT_*
 * paths are unreachable.
 *
 * Ownership:
 *   - On DECLINIT_OK / DECLINIT_DIAG_ERROR, *out_records /
 *     *out_record_count (when non-empty) are owned by the caller via
 *     types_records_free. The NameResult is borrowed and never
 *     modified.
 *   - On DECLINIT_UNSUPPORTED / DECLINIT_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_DECL_INIT_H
#define AICO_BOOTSTRAP_SRC_SEMA_DECL_INIT_H

#include "../name/name.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Build-level check (AIC-E0403)
 * ------------------------------------------------------------------------- */

typedef enum DeclInitStatus {
    DECLINIT_OK = 0,       /* all declaration/initializer sites checked;
                            * no records */
    DECLINIT_DIAG_ERROR,   /* AIC-E0403 records produced */
    DECLINIT_UNSUPPORTED,  /* defensive: malformed input; nothing owned */
    DECLINIT_OOM           /* allocation failure; nothing owned */
} DeclInitStatus;

/* Walk every declaration site of the resolved build (global var
 * declarations, function bodies incl. local var declarations and
 * `for` init var declarations) and emit one authoritative AIC-E0403
 * record per variable declaration whose initializer is NULL, in the
 * deterministic order of the DIAGNOSTIC-CONTRACT sec. 9 comparator
 * (records are sorted before return).
 *
 * Returns:
 *   DECLINIT_OK            no violations; *out_records NULL.
 *   DECLINIT_DIAG_ERROR    *out_records set (owned by the caller; free
 *                          with types_records_free) and *out_count set.
 *   DECLINIT_UNSUPPORTED   defensive; nothing owned.
 *   DECLINIT_OOM           nothing owned.
 */
DeclInitStatus declinit_check(const NameResult *result,
                              DiagRecord ***out_records,
                              size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_DECL_INIT_H */