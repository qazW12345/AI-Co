/* bootstrap/src/ir/ir_builder_stmt.h
 *
 * AI-Co Stage-0 IR builder Phase B statement mapping and terminators
 * (WP-M0-16c1d).
 *
 * Implements the statement lowering of the accepted canonical IR
 * contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) sections
 * 5.2, 5.6, 6.1 and 12.1 over the resolved, validated build:
 *
 *   - statements of contract 5.2: IR_BLOCK (block scoping), IR_LOCAL_DECL
 *     (local variable declarations with value initializers), IR_IF /
 *     IR_WHILE / IR_FOR (first-class; IR_FOR carries its init/cond/step
 *     with the init declaration scoped to the for statement, contract
 *     4.3 + spec 13.3), IR_SWITCH / IR_CASE / IR_DEFAULT (selector
 *     evaluated once; case values are resolved constants of the selector
 *     type), IR_BREAK / IR_CONTINUE with enclosing-stack target
 *     resolution (break targets an enclosing switch or loop; continue
 *     targets an enclosing loop, skipping switches - spec 13.2),
 *     IR_RETURN (optional value; void functions return none), IR_EXPR_STMT
 *     (effect expressions), IR_EMPTY (the empty statement), and
 *     IR_CALL_TERM for expression statements that are calls to a
 *     noreturn runtime function (rt.proc.exit / rt.trap.report, contract
 *     4.2/5.2). IR_TRAP has no source construct (the language has no
 *     trap statement); it is builder-emitted only by the 16c1c runtime
 *     signature patch (ir_builder_expr.c gap note 7), and the statement
 *     mapper recognizes it as a terminator kind in its defensive checks.
 *
 *   - terminator rules of contract 5.6: every switch case/default body
 *     ends in a terminator (IR_RETURN / IR_BREAK / IR_CONTINUE /
 *     IR_CALL_TERM / IR_TRAP); a terminator is the last statement of its
 *     block (no statement after a terminator); non-void function tails
 *     terminate (the ir_core_verify invariant-5 analysis is the
 *     structural authority - the mapper maps accepted builds whose
 *     reachability was already verified pre-IR, AIC-E0416, and refuses,
 *     with IR_BUILDER_UNSUPPORTED and nothing owned, the tails the
 *     invariant-5 analysis cannot certify, see gap note 6); a void
 *     function tail may fall off the end. The mapper runs only on
 *     accepted builds (contract 1.3), so the source-level rules are
 *     already enforced; the mapper additionally defends against
 *     malformed input (a case body that does not end in a terminator, or
 *     a statement after a terminator) by returning IR_BUILDER_UNSUPPORTED
 *     with nothing owned instead of silently producing an invalid graph.
 *
 * The mapper installs the Phase B body mapper through the 16c1a seam
 * (ir_builder_set_body_mapper) and consumes the Phase B expression
 * lowerer of 16c1c (ir_builder_expr_lower and its convenience wrappers)
 * for every expression position: conditions, selectors, case values are
 * lowered to constants, initializers, return values, and effect
 * expressions. The expression lowerer appends its intermediate nodes
 * (IR_ZERO/IR_STORE materializations, compound-assignment source
 * temps) directly to the current block, so the block's statement order
 * is the evaluation order (16c1c block-appending convention); the
 * statement mapper appends its statement nodes to the same block in the
 * same order.
 *
 * Local symbol handling (contract 4.3 storage model + the 16c1c
 * registration API): local variables get slots in first-declaration
 * order (source order) via ir_builder_add_slot and are registered with
 * ir_builder_expr_register_local BEFORE any statement that references
 * them is lowered (name resolution guarantees declaration-before-use,
 * so walking statements in source order suffices). Local const
 * declarations emit NO IR node: consts have no storage (contract 4.3)
 * and the expression lowerer resolves NAME_SYM_LOCAL_CONST references
 * by evaluating the initializer at each reference site. The local
 * symbol for a declaration node is found by symbol identity
 * (NameResult.syms scan for sym->decl == the declaration AST node), so
 * shadowing resolves correctly.
 *
 * Runtime noreturn signatures (spec 15.1-15.4, contract 4.2/5.2): a
 * call to rt.proc.exit / rt.trap.report in expression-statement
 * position lowers to IR_CALL_TERM. The 16c1b placeholder callee has no
 * parameters; ir_core_verify (invariant 4) demands the CALL_TERM
 * argument count/types match the callee parameters. The mapper
 * therefore attaches the spec signature (parameters + parameter slots;
 * void return) to the two noreturn runtime functions on first use as a
 * CALL_TERM callee, mirroring ir_builder_expr.c's ensure_runtime_signature
 * for exactly these two functions (the only noreturn functions in the
 * build; the 16c1c static helper is not reachable from this package).
 * The patch is idempotent (only when the callee still has no
 * parameters).
 *
 * Ownership and OOM discipline mirror the 16c1a/16c1b/16c1c packages:
 * all produced nodes are build-owned (ir_build_free releases the build);
 * on failure the mapper returns IR_BUILDER_UNSUPPORTED / IR_BUILDER_OOM
 * with nothing owned (the driver frees the partial build). The
 * NameResult and LayoutBuild are borrowed and never modified. No
 * process-global mapper-scratch state is needed by this package: the
 * enclosing-construct stack lives in the per-body mapping call.
 *
 * Representable-surface gaps (defensive IR_BUILDER_UNSUPPORTED with
 * nothing owned; disclosed; routed for Main Designer awareness per the
 * work-package manifest escalation line "IR boundary conflict ->
 * Main Designer"):
 *
 *   1. IR_TRAP has no source construct (no trap statement in the
 *      language; lexer keyword set has no trap keyword). It is produced
 *      only by the 16c1c runtime-signature patch for non-void runtime
 *      function bodies. The mapper's terminator-kind checks recognize
 *      it so a builder-emitted IR_TRAP is treated correctly.
 *
 *   2. Local const declarations emit no IR node (compile-time bindings
 *      with no storage; contract 4.3). This is the faithful mapping,
 *      not an unmappable construct: references resolve through the
 *      const evaluator at reference sites.
 *
 *   3. The two noreturn runtime signatures are re-declared here
 *      (mirroring ir_builder_expr.c's spec 15.1-15.4 table for
 *      rt.proc.exit / rt.trap.report) because the 16c1c signature
 *      patch is static and read-only for this package. The data is
 *      spec-fixed; a spec change would need synchronized updates in
 *      both packages (Coordinator-routed awareness note).
 *
 *   4. Case/default bodies whose termination is compound - a trailing
 *      statement that is not itself a terminator node but whose
 *      branches all terminate (e.g. `case 0: { if (c) { break; }
 *      else { break; } }`), or a trailing loop with a terminator in
 *      every path - are refused with IR_BUILDER_UNSUPPORTED. The
 *      mapper's 5.6 check requires the body's LAST statement to be a
 *      terminator node (IR_RETURN / IR_BREAK / IR_CONTINUE /
 *      IR_CALL_TERM / IR_TRAP). Source-level reachability analysis
 *      (stmt_reach, WP-M0-13c2) may accept such bodies, but the closed
 *      IR has no structural "if-branch terminators" encoding for this
 *      position, so the conservative refusal is the faithful mapper
 *      boundary (disclosed; routed for Main Designer awareness).
 *
 *   5. Expressions in per-iteration positions (a while/for condition,
 *      the for step) whose lowering appends intermediate effect nodes
 *      to the enclosing block (compound-assignment source temps,
 *      struct/array literal ZERO+store materializations, spec 10.4
 *      side effects) are refused with IR_BUILDER_UNSUPPORTED. The
 *      closed IR has no per-iteration statement list: intermediates
 *      appended to the enclosing block would be hoisted out of the
 *      loop, changing observable behavior. Simple pure value
 *      expressions (a load, comparison, call, arithmetic) lower
 *      normally; only intermediate-appending lowerings are refused.
 *
 *   6. Non-void function tails whose last mapped statement is an
 *      always-true loop (while(true) / for(;;)) whose body the
 *      ir_core_verify invariant-5 analysis cannot certify as
 *      never-exiting (an empty body; a body ending in an if without
 *      else; any other body shape whose every path does not provably
 *      end in a never-exiting form) are refused with
 *      IR_BUILDER_UNSUPPORTED and nothing owned. The source-level
 *      reachability analysis (stmt_reach, AIC-E0416; spec sec. 13.5)
 *      accepts these tails - control cannot reach the statement after
 *      the loop when no break exits it - but the closed IR's
 *      invariant-5 certification requires the loop body to provably
 *      never exit the loop, which the mapper cannot guarantee
 *      structurally for the listed shapes; a faithful mapping would
 *      produce a graph that fails ir_core_verify (AIC-I0501). The
 *      mapper therefore enforces the invariant-5 tail rule itself (a
 *      local mirror of the read-only ir_core.c analysis, see
 *      ir_builder_stmt.c) and refuses any non-void tail the analysis
 *      does not certify (disclosed; routed for Main Designer
 *      awareness). Certified tails - e.g. `while (true) { continue; }`
 *      - map and verify normally.
 *
 * Everything else in the accepted surface maps; constructs outside it
 * (malformed case bodies, statements after terminators, break/continue
 * outside any enclosing construct, a non-void return missing its value,
 * a void return carrying a value) return IR_BUILDER_UNSUPPORTED with
 * nothing owned - never a silent acceptance.
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_STMT_H
#define AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_STMT_H

#include "ir_builder_expr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------- */

/* Install the Phase B body mapper (ir_builder_stmt_body) through the
 * ir_builder_core.h seam. Call before ir_builder_build when statement
 * mapping should run (the 16c1d integration wiring and tests do this
 * after ir_builder_decl_install() + ir_builder_expr_install()). */
void ir_builder_stmt_install(void);

/* The installed body mapper entry (also exposed for tests that verify
 * the seam directly). Maps the function body of `fn_sym` per contract
 * 5.2/5.6 into the function's existing body IR_BLOCK (created by 16c1b
 * as the placeholder). Runtime functions (fn_sym->decl == NULL) have no
 * source body and return IR_BUILDER_OK without touching their
 * placeholder/NULL bodies (the 16c1c signature patch owns them). */
IrBuilderStatus ir_builder_stmt_body(BuilderCtx *ctx,
                                     const NameModule *module,
                                     const NameSymbol *fn_sym);

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_STMT_H */
