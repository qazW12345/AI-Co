/* bootstrap/src/sema/stmt_reach.h
 *
 * AI-Co Stage-0 reachability analysis (WP-M0-13c2).
 *
 * Implements the reachability rules of spec sec. 13.5 over the resolved
 * build (WP-M0-10 NameResult):
 *
 *   - AIC-E0416 (non-void function path without return, sec. 13.4-13.5):
 *     every reachable path in a non-void function must end in `return`
 *     (or a noreturn call - rt.proc.exit / rt.trap.report); otherwise
 *     rejected. A void function may fall off the end. The analysis is
 *     conservative per sec. 13.5: `if`/`else` merge paths
 *     conservatively (a missing else always leaves a path to the tail);
 *     loops are potentially non-terminating (a function whose only
 *     return is inside a loop still needs a terminating path), but
 *     `while (true)` / `for (;;)` with no `break` that exits the loop
 *     is non-terminating and satisfies the tail requirement if control
 *     cannot reach the tail;
 *   - AIC-E0417 (unreachable statement, sec. 13.5): a statement after
 *     `return`, `break`, `continue`, or a noreturn call in the same
 *     block is unreachable and rejected. The check is block-scoped and
 *     syntactic: only the direct terminator forms listed in the spec
 *     count; an `if`/`switch`/loop that always transfers control does
 *     NOT flag a following statement (conservative; that is E0416's
 *     tail analysis, not E0417's per-block rule).
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults plus the explicit
 * recovery marking):
 *   - AIC-E0416  message "non-void function '<name>' has a path without
 *                return" where <name> is the function name
 *                (corpus-pinned by tests/negative/cases/18-5-semantic-
 *                fn-no-return and derived-semantic-fn-missing-return);
 *                primary span = the "fn <name>" header: from the start
 *                of the `fn` keyword through the end of the function
 *                name identifier (corpus-pinned; the DIAGNOSTIC-CONTRACT
 *                sec. 11.5 "the function name" is realized as the
 *                keyword+name span the corpus pins).
 *   - AIC-E0417  message "unreachable statement" (corpus-pinned by
 *                tests/negative/cases/derived-semantic-unreachable);
 *                primary span = the whole unreachable statement
 *                (corpus-pinned).
 *
 * Reachability model (documented decisions; the spec text is the
 * authority and the corpus pins the central cases):
 *   - A statement "may fall through" when control can reach the
 *     statement after it in the same block: return and noreturn calls
 *     never fall through; a VALID break/continue (break inside a loop
 *     or switch; continue inside a loop) does not fall through to the
 *     next statement in its own block (control leaves the block), while
 *     an INVALID break/continue (outside any loop/switch - 13c1's
 *     AIC-E0414 record) is treated as ordinary flow so the corpus
 *     derived-semantic-break-outside-loop (E0414 only) matches: the
 *     following `return 0;` stays reachable and the function tail is
 *     not flagged E0416.
 *   - A block may fall off its end when control can reach the end of
 *     its statement list. An `if` may fall through when there is no
 *     else or either branch may fall through (conservative merge). A
 *     `switch` may fall past when it has no `default` (a selector with
 *     no matching case reaches the tail per sec. 13.2/13.4) or when
 *     some case body ends in a valid `break` (the break path reaches
 *     the statement after the switch) or when some case body lacks a
 *     terminating final statement (defensive; 13c1's E0412 owns that
 *     record, E0416 still applies - corpus 18-5-semantic-case-no-
 *     terminate pins E0416 + E0412 together). A case body ending in
 *     `return`/noreturn contributes no fall-past; a case body ending
 *     in `continue` leaves the fall-past decision to the enclosing
 *     loop.
 *   - A loop may fall past when its condition is not literally `true`
 *     (the loop may run zero times) or when a `break` that exits THIS
 *     loop exists (a break inside a nested switch or a nested loop
 *     belongs to that construct and does not exit this loop). For
 *     `for (;;)` the absent condition counts as literal `true`. Only
 *     the literal `true` keyword (or absent condition) is treated as
 *     always-true; other constant conditions are NOT const-evaluated
 *     here (conservative - reachability never proves a condition).
 *   - E0416 applies to every non-void function in every module; a
 *     function returning `void` (AST_TYPE_PRIM AST_PRIM_VOID) may fall
 *     off the end and never produces E0416. E0417 applies to all
 *     functions.
 *   - E0417 emits at most one record per block: the FIRST statement
 *     after a terminator in the block is flagged and the remainder of
 *     that block is not walked (statements after a terminator are all
 *     unreachable; one root-cause record per block, corpus-pinned by
 *     derived-semantic-unreachable which has two unreachable statements
 *     and pins one record).
 *
 * Ownership boundaries:
 *   - AIC-E0412/E0413/E0414/E0420 (switch terminators, duplicates,
 *     break/continue placement) are owned by WP-M0-13c1 (stmt_core) and
 *     are NEVER produced here. Return-value mismatch (AIC-E0415), entry
 *     main validation (AIC-E0418), and reserved-name rules are owned by
 *     WP-M0-13d; declarations (13a) and expressions (13b) are owned
 *     elsewhere. Statements never nest inside expressions in this
 *     language, so no expression is descended into.
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every module-
 * scope declaration, descending into function bodies (blocks, if/else,
 * while/for with their bodies, switch clauses and their bodies,
 * break/continue/return/expr statements). Records are returned sorted
 * with the DIAGNOSTIC-CONTRACT sec. 9 comparator (diag_sort_records).
 * Callers are expected to run the name, type, layout, conversion,
 * operator, const-eval, 13b1/13b2, and 13c1 stages first; on a clean
 * build the defensive STMT_REACH_UNSUPPORTED paths are unreachable.
 *
 * Ownership:
 *   - On STMT_REACH_OK / STMT_REACH_DIAG_ERROR / STMT_REACH_UNSUPPORTED,
 *     *out_records / *out_record_count (when non-empty) are owned by
 *     the caller via types_records_free. The NameResult and LayoutBuild
 *     are borrowed and never modified.
 *   - On STMT_REACH_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_STMT_REACH_H
#define AICO_BOOTSTRAP_SRC_SEMA_STMT_REACH_H

#include "../name/name.h"
#include "../types/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Build-level reachability check (AIC-E0416 / AIC-E0417)
 * ------------------------------------------------------------------------- */

typedef enum StmtReachStatus {
    STMT_REACH_OK = 0,        /* all owned reachability rules checked; no records */
    STMT_REACH_DIAG_ERROR,    /* AIC-E0416/E0417 records produced */
    STMT_REACH_UNSUPPORTED,   /* defensive: malformed input; nothing owned */
    STMT_REACH_OOM            /* allocation failure; nothing owned */
} StmtReachStatus;

/* Walk every function body of the resolved build and emit one
 * authoritative record per owned violation: non-void function with a
 * path without return (AIC-E0416), unreachable statement after a
 * terminator in the same block (AIC-E0417).
 *
 * `layout` is accepted for pipeline symmetry with the sibling sema
 * stages and is only NULL-checked; reachability itself is purely
 * structural (statement shape + name resolution for noreturn calls),
 * no layout computation is consulted.
 *
 * Returns:
 *   STMT_REACH_OK            no violations; *out_records NULL.
 *   STMT_REACH_DIAG_ERROR    *out_records set (owned by the caller; free
 *                            with types_records_free) and *out_count set.
 *   STMT_REACH_UNSUPPORTED   defensive; nothing owned.
 *   STMT_REACH_OOM           nothing owned.
 */
StmtReachStatus stmt_reach_check(const NameResult *result,
                                 const LayoutBuild *layout,
                                 DiagRecord ***out_records,
                                 size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_STMT_REACH_H */
