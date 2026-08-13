/* bootstrap/src/ir/ir_builder_cause.h
 *
 * AI-Co Stage-0 IR span/cause preservation (WP-M0-16c2).
 *
 * Implements the full source-span and causal-chain preservation of the
 * accepted canonical IR contract (docs/contracts/IR-CONTRACT-2026-08-12.md,
 * v0.1.1) section 8 over the completed IR builder output:
 *
 *   - every IR node carries a primary span: the DIAGNOSTIC-CONTRACT sec. 6
 *     span object of the smallest source construct it derives from. The
 *     16c1a..16c1d builders already copy the primary span from the AST
 *     construct at construction time; this package does not modify primary
 *     spans and proves byte-identity to the AST spans (sec. 8.5);
 *   - every IR node carries a cause chain: the ordered, parent-linked chain
 *     from the node's source construct to its module root, root cause first
 *     (the source construct), matching the DIAGNOSTIC-CONTRACT sec. 4
 *     causes convention (contract sec. 8.2, 8.3). The 16c1a..16c1d builders
 *     attach only the minimal single root cause link (construct kind +
 *     span) that ir_core_verify invariant 2 requires; this package REPLACES
 *     that single link with the full preservation chain, one link per AST
 *     ancestor construct, each link carrying the construct kind (the AST
 *     node kind text, e.g. "AST_EXPR_BINARY"), that construct's primary
 *     span (cloned byte-identically to the AST span), and resolved-
 *     reference facts (declaration/type/constant IR ids);
 *   - lowering preserves causality (contract sec. 8.3): when one source
 *     construct produces several IR nodes (compound assignment lowers to
 *     destination-location + source + operation + store; a checked
 *     operation to operation + failure path), every produced node carries
 *     the source construct's span in its cause chain and shares the same
 *     full chain root. Derived nodes whose construct-kind text is coarse
 *     ("AST_EXPR" for materialization temporaries / constant folding) are
 *     matched to their source construct by span alone (the smallest AST
 *     construct with that exact span), so their chains are identical to
 *     the source construct's chain;
 *   - consumption (contract sec. 8.4): the backend emits trap records and
 *     backend diagnostics from the failing IR node's span, cause chain, and
 *     type/value facts - never from plain strings. This package makes the
 *     cause chain complete so a failing node can cite every enclosing
 *     source construct up to the module root;
 *   - determinism of spans (contract sec. 8.5): spans are byte-identical to
 *     the AST spans produced by the parser; the enrichment copies them
 *     without modification, and two builds of identical ASTs produce
 *     byte-identical span and cause data (identical AST -> byte-identical
 *     IR; verified by the package tests).
 *
 * Scope boundaries:
 *   - This package owns the enrichment pass and its tests ONLY
 *     (bootstrap/src/ir/ir_builder_cause.*, bootstrap/build/ir_builder2.txt,
 *     span/cause tests). The structural mapping (16c1a..16c1d), the IR node
 *     model and invariants (16b1, ir_core.*), and the deterministic dump
 *     (16b2, ir_dump.*) are read-only inputs.
 *   - Runtime modules (rt.mem/rt.io/rt.proc/rt.trap) have no source AST
 *     (NameModule.program == NULL); their IR nodes carry deterministic
 *     synthetic spans (file = fqn, disclosed in ir_builder_decl.h). They
 *     have no AST ancestor chain, so their single-link cause chains are
 *     left unchanged (deterministic; invariant 2 is satisfied because the
 *     link span is the node's own synthetic span). This mirrors the
 *     documented 16c1b synthetic-span convention and is disclosed here.
 *   - resolved-reference facts are filled only where determinable from the
 *     resolved build and the built graph (see ir_builder_cause.c):
 *       - module-scope declarations (FN/STRUCT/ENUM/GLOBAL_VAR/
 *         GLOBAL_CONST): ref_decl = the IR declaration node id (by fqn);
 *       - local variable references: ref_decl = the IR_LOCAL_DECL node id
 *         (matched by the declaring AST node's span);
 *       - named type references (AST_TYPE_NAMED) to struct/enum:
 *         ref_decl = the decl node id, ref_type = the interned type id;
 *       - enum member references (AST_EXPR_MEMBER resolving to an enum
 *         member): ref_decl = the owner enum decl node id, ref_type = the
 *         enum type id, ref_const = the IRC_ENUM constant id when the
 *         member's value constant is interned;
 *       - global const references: ref_const = the referenced const's value
 *         IRConst id;
 *       - call expressions: ref_decl = the callee declaration node id.
 *     Value member (field) accesses (p.x) are deferred by name resolution
 *     to the types phase (name.c resolve_member_chain records only the
 *     base identifier), so their cause links carry ref_decl/ref_type/
 *     ref_const = -1 (disclosed). Parameters and local consts have no IR
 *     declaration node (params are slots; local consts fold at use
 *     sites), so their references carry -1 as well (disclosed).
 *
 * Ownership:
 *   - ir_builder_cause_finalize mutates the completed IrBuild in place:
 *     each node's owned cause array is replaced (old links and the array
 *     are freed exactly as ir_build_free frees them). On IR_BUILDER_OOM
 *     build->oom is set and the caller frees the whole build (nothing is
 *     owned beyond what the caller already owns); nodes are only replaced
 *     after their full new chain has been built, so the build is always
 *     freeable. The NameResult is borrowed and never modified.
 *
 * Call contract:
 *   - The driver (WP-M0-19) calls ir_builder_cause_finalize AFTER
 *     ir_builder_build and BEFORE ir_core_verify / codegen so the IR
 *     consumed by later stages carries the full preservation chains.
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_CAUSE_H
#define AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_CAUSE_H

#include "ir_builder_core.h"
#include "../name/name.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enrich the completed builder output with the full span/cause
 * preservation chains (contract sec. 8).
 *
 * `build` must be a completed IrBuild (ir_builder_build output); `result`
 * the resolved build used to construct it (borrowed, never modified).
 *
 * For every node with a source construct found in the module AST (matched
 * by the node's primary span, preferring the exact AST construct kind
 * text), the node's single minimal cause link is replaced by the ordered
 * parent-linked chain from the source construct to the module root
 * (AST_PROGRAM), each link carrying the construct kind text, a byte-
 * identical clone of that construct's AST span, and resolved-reference
 * facts. Nodes whose span has no AST source (runtime synthetic spans) keep
 * their existing single-link chain (disclosed above).
 *
 * Deterministic: identical AST -> identical chains -> byte-identical IR
 * (span and cause data identical across builds; the pass walks modules in
 * NameResult order, the AST in source order, and build nodes in id order,
 * with no pointer-address ordering and no hash iteration).
 *
 * Returns IR_BUILDER_OK on success; IR_BUILDER_OOM with build->oom set on
 * allocation failure (caller frees the build); IR_BUILDER_UNSUPPORTED on
 * malformed input (NULL build/result) with nothing changed.
 */
IrBuilderStatus ir_builder_cause_finalize(IrBuild *build,
                                          const NameResult *result);

#ifdef __cplusplus
}
#endif

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_CAUSE_H */
