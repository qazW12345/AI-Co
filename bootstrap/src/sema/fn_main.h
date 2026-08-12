/* bootstrap/src/sema/fn_main.h
 *
 * AI-Co Stage-0 entry-main validation and reserved-name enforcement
 * (WP-M0-13d2).
 *
 * Implements the entry-point contract of spec sec. 15.3 and the
 * reserved built-in name rule of spec sec. 4.5 over the resolved build
 * (WP-M0-10 NameResult):
 *
 *   - AIC-E0418 (entry `main` signature invalid / missing, sec. 15.3 /
 *     DIAGNOSTIC-CONTRACT sec. 11.5): the entry module must declare
 *     `fn main() -> i32` or `fn main() -> void` (the void form is
 *     equivalent to returning 0). A missing or mis-typed `main` is
 *     rejected with AIC-E0418 at entry validation. The check is
 *     applied to the entry module only; imported (non-entry) modules
 *     are not required to declare `main`.
 *
 *   - Reserved-name enforcement (spec sec. 4.5): the conversion
 *     operators `cast` and `wrap`, the built-in functions `len` and
 *     `ptr`, the module name `rt` and its submodules, and the
 *     trap-reporting function `rt.trap.report` may not be declared by
 *     user programs. All reachable spellings are already rejected
 *     BEFORE this semantic stage:
 *       - `cast`/`wrap`/`len`/`ptr` are lexer keywords (spec sec.
 *         4.2); the lexer never produces an identifier token for them,
 *         so a declaration spelling one of them fails in the parser
 *         with AIC-S0101 ("expected identifier") and never reaches
 *         name resolution or this package;
 *       - `rt`/`rt.*` module declarations and imports are rejected by
 *         the name phase (AIC-N0207 module declaration uses the
 *         reserved `rt` prefix; AIC-N0208 import of reserved runtime
 *         submodule not in the runtime surface; AIC-N0209 bare
 *         `import rt;`), so no user module with a reserved `rt` name
 *         can exist, and `rt.trap.report` can therefore never be
 *         declared by a user program.
 *     fn_main's residual role is a defensive guard: it scans every
 *     module-scope declaration of every module and, if a declaration
 *     name is one of the reserved built-in spellings that the lexer
 *     keywords cover (`cast`, `wrap`, `len`, `ptr`), it returns
 *     FN_MAIN_UNSUPPORTED rather than silently passing the program
 *     (mirroring the defensive contracts of fn_core/stmt_reach). On a
 *     clean pipeline this path is unreachable because the parser
 *     rejects those spellings first; the guard exists so that a
 *     malformed internal build can never be mis-reported as clean.
 *     A module-scope symbol literally named `rt` in a user module
 *     (e.g. `fn rt()`) is NOT a sec. 4.5 violation: the reserved name
 *     is the module `rt` (and rt.-qualified names), whose declaration
 *     and import sites are rejected by the name phase; the module-
 *     scope symbol's fully qualified name (e.g. `main.rt`) does not
 *     collide with the reserved module namespace.
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults plus the explicit
 * recovery marking):
 *   - AIC-E0418  message "entry 'main' signature invalid or missing"
 *                (corpus-pinned by tests/negative/cases/derived-
 *                semantic-main-missing). Primary span:
 *                - missing `main` (no module-scope declaration named
 *                  `main` in the entry module): the whole module
 *                  declaration (`module main;`), corpus-pinned
 *                  (offsets 0..12 for the corpus input; DIAGNOSTIC-
 *                  CONTRACT sec. 11.5 "the module / main decl");
 *                - mis-typed `main` (declared as a non-function, with
 *                  parameters, or with a return type other than `i32`
 *                  or `void`): the whole `main` declaration node's
 *                  span (documented decision; no corpus anchor; the
 *                  contract's "the module / main decl" is realized as
 *                  the main declaration, mirroring how stmt_reach
 *                  realizes "the function name").
 *
 * Ownership boundaries (never produced here):
 *   - AIC-E0415 return rules -> WP-M0-13d1 (fn_core);
 *   - AIC-E0416/E0417 reachability -> WP-M0-13c2 (stmt_reach);
 *   - AIC-E0412/E0413/E0414/E0420 statement/switch rules ->
 *     WP-M0-13c1 (stmt_core);
 *   - AIC-E0419 assignment to non-lvalue -> WP-M0-13a1 (decl_core);
 *   - AIC-N0207/N0208/N0209 reserved rt rules -> WP-M0-10 (name);
 *   - AIC-S0101 keyword-as-identifier rejections -> WP-M0-09 (parse).
 *
 * The build-level walker inspects the entry module's module scope for
 * the `main` declaration and scans all modules' module scopes for
 * reserved-name declaration spellings (defensive). Records are
 * returned sorted with the DIAGNOSTIC-CONTRACT sec. 9 comparator
 * (diag_sort_records). Callers are expected to run the name, type,
 * layout, conversion, operator, const-eval, 13b1/13b2, 13c1, 13c2,
 * and 13d1 stages first; on a clean build the defensive
 * FN_MAIN_UNSUPPORTED paths are unreachable.
 *
 * Ownership:
 *   - On FN_MAIN_OK / FN_MAIN_DIAG_ERROR / FN_MAIN_UNSUPPORTED,
 *     *out_records / *out_record_count (when non-empty) are owned by
 *     the caller via types_records_free. The NameResult and LayoutBuild
 *     are borrowed and never modified.
 *   - On FN_MAIN_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_FN_MAIN_H
#define AICO_BOOTSTRAP_SRC_SEMA_FN_MAIN_H

#include "../name/name.h"
#include "../types/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Build-level entry-main / reserved-name check (AIC-E0418)
 * ------------------------------------------------------------------------- */

typedef enum FnMainStatus {
    FN_MAIN_OK = 0,        /* entry main valid; no records */
    FN_MAIN_DIAG_ERROR,    /* AIC-E0418 record produced */
    FN_MAIN_UNSUPPORTED,   /* defensive: malformed input; nothing owned */
    FN_MAIN_OOM            /* allocation failure; nothing owned */
} FnMainStatus;

/* Validate the entry module's `main` declaration and guard the
 * reserved built-in names (spec sec. 4.5), emitting one authoritative
 * AIC-E0418 record when the entry module's `main` is missing or
 * mis-typed (spec sec. 15.3).
 *
 * `layout` is accepted for pipeline symmetry with the sibling sema
 * stages and is only NULL-checked; entry validation is purely
 * structural (module-scope symbol shape + the `main` declaration's
 * parameter/return-type form), no layout computation is consulted.
 *
 * Returns:
 *   FN_MAIN_OK            entry main valid; *out_records NULL.
 *   FN_MAIN_DIAG_ERROR    *out_records set (owned by the caller; free
 *                         with types_records_free) and *out_count set.
 *   FN_MAIN_UNSUPPORTED   defensive; nothing owned.
 *   FN_MAIN_OOM           nothing owned.
 */
FnMainStatus fn_main_check(const NameResult *result,
                           const LayoutBuild *layout,
                           DiagRecord ***out_records,
                           size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_FN_MAIN_H */
