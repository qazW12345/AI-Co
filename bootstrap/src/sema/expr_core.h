/* bootstrap/src/sema/expr_core.h
 *
 * AI-Co Stage-0 expression core and evaluation-order model (WP-M0-13b1).
 *
 * Implements the expression semantics model of spec sec. 10.1-10.4 over
 * the resolved build (WP-M0-10 NameResult):
 *   - the precedence model (sec. 10.1): a deterministic precedence level
 *     for every expression node kind / operator, testable against the
 *     normative list;
 *   - the evaluation-order model (sec. 10.4): for any expression node, a
 *     deterministic ordered plan of evaluation steps (values, locations,
 *     reads, stores, conditional steps, and non-evaluated operands). The
 *     language has no unspecified evaluation order anywhere (sec. 10.4);
 *     the plan is total and deterministic for every expression form;
 *   - const-context use (AIC-E0401): the build-level check evaluates the
 *     const-context sites this package owns - array type extents (sec.
 *     12.1), switch case labels (sec. 13.2), and local const declaration
 *     initializers (sec. 8.1) - and rejects expressions outside the
 *     sec. 10.5 composition with AIC-E0401.
 *
 * Const-context ownership boundary (documented; the WP-M0-12a evaluator
 * owns the other const-context sites):
 *   - global const/var initializers and enum member value expressions
 *     belong to WP-M0-12a (const_eval_check in bootstrap/src/const/
 *     eval_core.c) and are NOT produced here;
 *   - array extents, case labels, and local const declarations are the
 *     const-context sites owned by this package (eval_core.h boundary
 *     note); const failures at these sites (AIC-E0405..E0411) are routed
 *     out as EvalFailureSite for the failure-record owner (the 12a/12b2
 *     handoff shape), never emitted here.
 *
 * Record conventions (phase "semantic", severity "error", recovery
 * "authoritative" - the code-registry defaults plus the explicit
 * recovery marking; primary span = the whole site expression, matching
 * the WP-M0-12a whole-initializer convention and DIAGNOSTIC-CONTRACT
 * sec. 11.5 "the expression"):
 *   - AIC-E0401  message "expression is not a constant expression";
 *                primary span = the whole array-extent expression, the
 *                whole case-label expression, or the whole local-const
 *                initializer expression (the site expression).
 *
 * The build-level walker visits every module in result order (entry
 * first, then imports depth-first) and within a module every declaration
 * in module-scope order, descending into struct field types, parameter
 * and return types, and function bodies (including `for` init
 * declarations, case clauses, and every type/expression reachable from
 * them). Records are returned sorted with the DIAGNOSTIC-CONTRACT sec. 9
 * comparator (diag_sort_records). Callers are expected to run the name,
 * type, layout, conversion, operator, and const-eval stages first and
 * stop on their diagnostics, as the pipeline does; on a clean build the
 * defensive EXPR_CORE_UNSUPPORTED paths are unreachable.
 *
 * Ownership:
 *   - On EXPR_CORE_OK / EXPR_CORE_DIAG_ERROR / EXPR_CORE_FAILURE,
 *     *out_records / *out_record_count (when non-empty) are owned by the
 *     caller via types_records_free, and *out_failures /
 *     *out_failure_count (when non-empty) is a plain owned array the
 *     caller frees with free(). The NameResult and LayoutBuild are
 *     borrowed and never modified.
 *   - On EXPR_CORE_UNSUPPORTED / EXPR_CORE_OOM nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_SEMA_EXPR_CORE_H
#define AICO_BOOTSTRAP_SRC_SEMA_EXPR_CORE_H

#include "../const/eval_core.h"
#include "../name/name.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Precedence model (spec sec. 10.1, highest to lowest)
 * ------------------------------------------------------------------------- */

typedef enum ExprPrecedence {
    EXPR_PREC_NONE = 0,        /* not an operator expression (leaf/name) */
    EXPR_PREC_POSTFIX,         /* [] slice[..] call() . -> struct-init{} */
    EXPR_PREC_UNARY,           /* - + ! ~ * & sizeof alignof cast wrap len ptr */
    EXPR_PREC_MULTIPLICATIVE,  /* * / % */
    EXPR_PREC_ADDITIVE,        /* + - */
    EXPR_PREC_SHIFT,           /* << >> */
    EXPR_PREC_RELATIONAL,      /* < <= > >= */
    EXPR_PREC_EQUALITY,        /* == != */
    EXPR_PREC_BAND,            /* & */
    EXPR_PREC_BXOR,            /* ^ */
    EXPR_PREC_BOR,             /* | */
    EXPR_PREC_LAND,            /* && */
    EXPR_PREC_LOR,             /* || */
    EXPR_PREC_CONDITIONAL,     /* ?: (right) */
    EXPR_PREC_ASSIGNMENT       /* = += -= *= /= %= <<= >>= &= |= ^= (right) */
} ExprPrecedence;

/* Precedence of the root operator of `expr` (sec. 10.1). Leaves,
 * identifiers, array literals, and parenthesized expressions return
 * EXPR_PREC_NONE (parens are transparent: they do not change the
 * evaluation order or the precedence class of the enclosed expression).
 * NULL returns EXPR_PREC_NONE. */
ExprPrecedence expr_precedence_of(const AstNode *expr);

/* ---------------------------------------------------------------------------
 * Evaluation-order model (spec sec. 10.4)
 * ------------------------------------------------------------------------- */

/* One elementary evaluation step of an expression. `node` is the AST node
 * the step evaluates; `conditional` marks steps that run only when a
 * preceding guard selects them (short-circuit && / || right operand, and
 * the chosen ?: branch). */
typedef enum ExprStepKind {
    EXPR_STEP_VALUE = 0,       /* evaluate the node for its value */
    EXPR_STEP_LOCATION,        /* evaluate the node to obtain a location */
    EXPR_STEP_READ,            /* read the value at a location (compound
                                * assignment destination read) */
    EXPR_STEP_STORE,           /* store a value into a location (assignment) */
    EXPR_STEP_NOT_EVAL         /* operand is not evaluated (sizeof/alignof) */
} ExprStepKind;

typedef struct ExprStep {
    ExprStepKind kind;
    const AstNode *node;       /* expression node this step concerns */
    bool conditional;          /* true: step may be skipped by short-circuit /
                                * ternary guard */
} ExprStep;

/* Produce the deterministic evaluation-order plan for `expr` per
 * sec. 10.4. `as_location` requests the location evaluation of the root
 * (assignment target / address-of operand context); it affects only the
 * terminal step kind of location-capable forms (ident, deref, index,
 * member, arrow). Every plan ends with a terminal step for `expr` itself
 * (VALUE for value evaluation, LOCATION for location evaluation of
 * location-capable forms, STORE for assignment, NOT_EVAL only when the
 * operand of sizeof/alignof is itself the planned node - defensive).
 *
 * Rules modeled (each testable):
 *   - binary operators: left operand fully before right operand;
 *   - call: callee (a name), then arguments left-to-right;
 *   - assignment `a = b`: destination location, then source value, then
 *     STORE; compound `a op= b`: destination location, then `b`, then
 *     READ the destination, then STORE (sec. 10.4 / sec. 11.6);
 *   - indexing `a[i]`: evaluate `a`, then `i`;
 *   - slice `a[x..y]`: evaluate `a`, then `x`, then `y`;
 *   - `&&`/`||`: left operand, then the right operand marked conditional
 *     (short-circuit);
 *   - `?:`: condition, then the then-branch and else-branch both marked
 *     conditional (exactly one executes);
 *   - member access `.`/`->`: the operand only; no member expression is
 *     evaluated;
 *   - `sizeof`/`alignof`: operand is NOT evaluated (NOT_EVAL step).
 * Returns EXPR_ORDER_OK with *out_steps set (owned by the caller; free
 * with expr_steps_free), EXPR_ORDER_UNSUPPORTED defensively for an
 * unknown/malformed node (nothing owned), or EXPR_ORDER_OOM (nothing
 * owned). */
typedef enum ExprOrderStatus {
    EXPR_ORDER_OK = 0,
    EXPR_ORDER_UNSUPPORTED,    /* defensive: unknown node kind */
    EXPR_ORDER_OOM             /* allocation failure; nothing owned */
} ExprOrderStatus;

ExprOrderStatus expr_order_plan(const AstNode *expr, bool as_location,
                                ExprStep **out_steps, size_t *out_count);

/* Free a step array from expr_order_plan (NULL accepted). */
void expr_steps_free(ExprStep *steps);

/* ---------------------------------------------------------------------------
 * Build-level const-context check (AIC-E0401)
 * ------------------------------------------------------------------------- */

typedef enum ExprCoreStatus {
    EXPR_CORE_OK = 0,          /* all owned const-context sites evaluated;
                                * no records, no failures */
    EXPR_CORE_DIAG_ERROR,      /* AIC-E0401 records produced */
    EXPR_CORE_FAILURE,         /* no records, but at least one site failed a
                                * checked-arithmetic check; *out_failures set
                                * (the failure-record owner emits AIC-E0405..
                                * E0411; this package never emits them) */
    EXPR_CORE_UNSUPPORTED,     /* defensive: malformed input; nothing owned */
    EXPR_CORE_OOM              /* allocation failure; nothing owned */
} ExprCoreStatus;

/* Evaluate every const-context site this package owns - array type
 * extents (sec. 12.1), switch case labels (sec. 13.2), and local const
 * declaration initializers (sec. 8.1) - and report composition violations
 * as AIC-E0401 (phase "semantic", severity "error", recovery
 * "authoritative", primary span = the whole site expression). Checked-
 * arithmetic failures at these sites are routed to *out_failures in
 * deterministic walk order (modules in result order, declarations in
 * source order); no failure record is emitted (the failure-record owner
 * emits AIC-E0405..E0411). Global const/var initializers and enum member
 * value expressions belong to WP-M0-12a and are NOT evaluated here.
 * Callers are expected to run completeness, layout, the 11c conversion
 * check, and the 11d operator check first and stop on their diagnostics,
 * as the pipeline does.
 *
 * Returns EXPR_CORE_OK / EXPR_CORE_DIAG_ERROR / EXPR_CORE_FAILURE with
 * *out_records and/or *out_failures set (when non-empty), or
 * EXPR_CORE_UNSUPPORTED / EXPR_CORE_OOM with nothing owned. */
ExprCoreStatus expr_core_check(const NameResult *result,
                               const LayoutBuild *layout,
                               DiagRecord ***out_records,
                               size_t *out_record_count,
                               EvalFailureSite **out_failures,
                               size_t *out_failure_count);

#endif /* AICO_BOOTSTRAP_SRC_SEMA_EXPR_CORE_H */
