/* bootstrap/src/sema/stmt_core.h
 *
 * AI-Co Stage-0 statement rules and switch/break/continue (WP-M0-13c1).
 *
 * Implements the statement-semantics model of spec sec. 13 over the
 * resolved build (WP-M0-10 NameResult) and the layouts of WP-M0-11b:
 *   - switch no-fall-through (AIC-E0412): every case/default body is a
 *     brace-delimited block (grammar case_clause/default_clause, and the
 *     parser already rejects non-braced bodies with AIC-S0104) whose
 *     FINAL statement must be a terminator - break, return, continue
 *     (only when the switch is inside a loop), or a call to a noreturn
 *     function (rt.proc.exit, rt.trap.report) - else AIC-E0412. An
 *     empty body {} is rejected too (sec. 13.2);
 *   - duplicate case values (AIC-E0413): case labels are constant
 *     expressions of the selector's type (sec. 13.2); two labels with
 *     equal evaluated values in the same switch are rejected;
 *   - duplicate default clauses (AIC-E0420): `default` is optional
 *     and may appear at most once (sec. 13.2); every default clause
 *     after the first is rejected;
 *   - break/continue placement (AIC-E0414): break is valid inside a
 *     loop or a switch; continue is valid only inside a loop (sec.
 *     13.2: "continue inside a switch that is itself inside a loop
 *     continues the loop"). A break outside loop and switch, or a
 *     continue outside any loop, is rejected.
 *
 * Reachability (AIC-E0416/E0417), declarations (sec. 8), expressions
 * (sec. 10), and functions/returns (sec. 13.4-13.5) are owned by other
 * packages (WP-M0-13c2/13a/13b/13d) and are NEVER produced here.
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults plus the explicit
 * recovery marking):
 *   - AIC-E0412  message "switch case <label> body lacks a terminating
 *                statement; fall-through is prohibited" where <label>
 *                is the source text of the case value expression
 *                (corpus-pinned by
 *                tests/negative/cases/18-5-semantic-case-no-terminate);
 *                for a default clause the message is "switch default
 *                body lacks a terminating statement; fall-through is
 *                prohibited" (documented decision; no corpus anchor);
 *                primary span = the case label: from the start of the
 *                `case` keyword through the end of the value
 *                expression (corpus-pinned), or the `default` keyword
 *                for a default clause (documented decision).
 *   - AIC-E0413  message "duplicate switch case value: <value>" where
 *                <value> is the evaluated case value rendered as a
 *                decimal integer (big values, [2^63, 2^64-1], as
 *                unsigned - the WP-M0-12b2 render_eval_int convention;
 *                corpus-pinned by
 *                tests/negative/cases/derived-semantic-duplicate-case);
 *                primary span = the first case label (from the start of
 *                the `case` keyword through the end of its value
 *                expression; corpus-pinned). One record is emitted per
 *                later label that duplicates an earlier one, each
 *                pointing at the first occurrence's label (documented
 *                decision; the corpus pins the single-pair case).
 *   - AIC-E0420  message "duplicate switch default clause" (corpus-
 *                pinned by tests/negative/cases/derived-semantic-
 *                duplicate-default); primary span = the first default
 *                clause's `default` keyword (7 bytes, the stmt_label_span
 *                default-clause convention), mirroring the E0413
 *                first-occurrence pin. One record is emitted per
 *                default clause after the first (N defaults -> N-1
 *                records), each pointing at the first default's keyword
 *                (documented decision; the corpus pins the two-default
 *                single-pair case).
 *   - AIC-E0414  message "break outside loop or switch" (corpus-pinned
 *                by tests/negative/cases/derived-semantic-break-outside-
 *                loop) or "continue outside loop" (documented decision;
 *                no corpus anchor); primary span = the whole
 *                break/continue statement (corpus-pinned).
 *
 * Terminator check (documented decisions; the spec text is the
 * authority and the corpus has no anchors for these edges):
 *   - The check is syntactic on the case body block's final statement:
 *     a final statement that is a nested block, an `if`, a loop, a
 *     declaration, or an expression statement that is not a noreturn
 *     call is not a terminator, even if control-flow analysis could
 *     prove it always exits (that deeper analysis is WP-M0-13c2
 *     reachability). "case x: { break; }" is valid (sec. 13.2).
 *   - A final `continue` is a valid terminator only when the switch is
 *     inside a loop (loop depth > 0 at the switch). A final `continue`
 *     outside a loop therefore produces BOTH AIC-E0412 (the body's
 *     final statement is not a valid terminator) and AIC-E0414 (the
 *     continue statement is outside a loop): two distinct rule
 *     violations with distinct primary spans.
 *   - A noreturn call is an expression statement whose expression is a
 *     call whose callee resolves (name_symbol_for_node) to the
 *     compiler-provided rt.proc.exit or rt.trap.report NAME_SYM_FN
 *     (fqn "rt.proc.exit" / "rt.trap.report"). Any other callee -
 *     user function, non-function member - is not noreturn.
 *
 * Duplicate detection uses the WP-M0-12a public evaluator
 * (const_eval_expr) on each case label; labels that do not evaluate
 * (EVAL_NOT_CONST / EVAL_FAILURE / EVAL_UNSUPPORTED) are skipped - the
 * const-context stages (WP-M0-13b1/13b2) own those records - and
 * labels of non-integer kind are skipped defensively (a valid pipeline
 * never reaches them: the selector is int/enum by AIC-T0311). Values
 * are compared by (v, big) on EVAL_VAL_INT (enums evaluate to their
 * underlying integer), i.e. by mathematical value after promotion.
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every
 * declaration in module-scope order, descending into function bodies
 * (blocks, if/else, while/for with their bodies, switch clauses and
 * their bodies, break/continue). Statements never nest inside
 * expressions in this language, so no expression is descended into.
 * Records are returned sorted with the DIAGNOSTIC-CONTRACT sec. 9
 * comparator (diag_sort_records). Callers are expected to run the
 * name, type, layout, conversion, operator, const-eval, and 13b1/13b2
 * stages first and stop on their diagnostics, as the pipeline does; on
 * a clean build the defensive STMT_CORE_UNSUPPORTED paths are
 * unreachable.
 *
 * Ownership:
 *   - On STMT_CORE_OK / STMT_CORE_DIAG_ERROR / STMT_CORE_UNSUPPORTED,
 *     *out_records / *out_record_count (when non-empty) are owned by
 *     the caller via types_records_free. The NameResult and LayoutBuild
 *     are borrowed and never modified.
 *   - On STMT_CORE_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_STMT_CORE_H
#define AICO_BOOTSTRAP_SRC_SEMA_STMT_CORE_H

#include "../const/eval_core.h"
#include "../name/name.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Build-level statement-rule check (AIC-E0412 / AIC-E0413 / AIC-E0414 /
 * AIC-E0420)
 * ------------------------------------------------------------------------- */

typedef enum StmtCoreStatus {
    STMT_CORE_OK = 0,        /* all owned statement rules checked; no records */
    STMT_CORE_DIAG_ERROR,    /* AIC-E0412..E0414/E0420 records produced */
    STMT_CORE_UNSUPPORTED,   /* defensive: malformed input; nothing owned */
    STMT_CORE_OOM            /* allocation failure; nothing owned */
} StmtCoreStatus;

/* Walk every function body of the resolved build and emit one
 * authoritative record per owned violation: switch case/default body
 * without a terminating final statement (AIC-E0412), duplicate case
 * value (AIC-E0413), duplicate default clause (AIC-E0420),
 * break/continue outside loop (or break outside switch) (AIC-E0414).
 * The check is purely syntactic on statement
 * structure (plus loop/switch depth context and noreturn-call
 * resolution); no expression semantics are evaluated here.
 *
 * Returns:
 *   STMT_CORE_OK            no violations; *out_records NULL.
 *   STMT_CORE_DIAG_ERROR    *out_records set (owned by the caller; free
 *                           with types_records_free) and *out_count set.
 *   STMT_CORE_UNSUPPORTED   defensive; nothing owned.
 *   STMT_CORE_OOM           nothing owned.
 */
StmtCoreStatus stmt_core_check(const NameResult *result,
                               const LayoutBuild *layout,
                               DiagRecord ***out_records,
                               size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_STMT_CORE_H */
