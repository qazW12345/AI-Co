/* bootstrap/src/ir/ir_builder_core.h
 *
 * AI-Co Stage-0 IR builder foundation (WP-M0-16c1a).
 *
 * ADOPTED partial artifact from the superseded WP-M0-16c1 card
 * (t_804b7d94; untracked in the shared tree, Rule W4 provenance) and
 * adjusted to the 16c1a..16c1d sub-split scope per the work-package
 * manifest amendment (2026-08-12, commit 6b771c1, lines 49-55).
 *
 * This package owns the builder FOUNDATION only:
 *   - the entry surface `ir_builder_build` with the status/ownership
 *     contract (IR_BUILDER_OK with *out_build owned; IR_BUILDER_UNSUPPORTED
 *     / IR_BUILDER_OOM with nothing owned);
 *   - the builder context (BuilderCtx) and its allocation discipline
 *     (sticky OOM, mirroring ir_core's);
 *   - the canonical-order two-phase construction driver skeleton
 *     (builder_phase_a / builder_phase_b below).
 *
 * The per-construct mapping is implemented by the successor packages,
 * which register their mappers through the seam defined here:
 *   - WP-M0-16c1b (ir_builder_decl.*): module/import graph, top-level
 *     declarations, storage model, type/const interning -> module mapper
 *     + decl mapper;
 *   - WP-M0-16c1c (ir_builder_expr.*): expression mapping/lowering ->
 *     used by the body mapper;
 *   - WP-M0-16c1d (ir_builder_stmt.*): statement mapping/terminators and
 *     the full `ir_builder_build` wiring -> body mapper + integration;
 *   - WP-M0-16c2 (ir_builder_cause.*): full span/cause preservation.
 * The default mappers installed by this package are defensive stubs
 * returning IR_BUILDER_UNSUPPORTED (the representable surface at 16c1a
 * is an empty build); successors replace them via the setters below.
 *
 * The builder runs only on accepted (diagnostic-free) builds per the
 * IR contract sec. 1.3; the driver stops before IR when diagnostics
 * exist (the caller's responsibility).
 *
 * Determinism: the builder walks the resolved build in canonical order
 * and constructs nodes in a fixed two-phase order. Phase A creates all
 * module/import/declaration nodes in canonical order; phase B lowers
 * function bodies in canonical order. This is the minimal order that
 * makes cross-module reference edges (contract sec. 4.1) resolvable as
 * direct node pointers; identical inputs always produce identical node
 * ids, so the observable determinism obligation (identical AST ->
 * identical IR; dump byte-identity, WP-M0-16b2) is unaffected.
 *
 * Scope boundaries:
 *   - This package owns the structural foundation only. The deterministic
 *     dump / round-trip verification is WP-M0-16b2 (ir_dump.*, read-only
 *     here). Source-span and causal-chain preservation (full cause
 *     chains with resolved-reference facts, span/cause determinism
 *     tests) is WP-M0-16c2 (ir_builder_cause.*). This builder attaches
 *     the minimal structural span/cause every IR node must carry for
 *     ir_core_verify invariant 2 to pass: each node's primary span is
 *     copied from its AST construct and each node receives a single
 *     root cause link naming its AST construct kind and span. 16c2
 *     replaces these with the full preservation chains.
 *   - Defensive IR_BUILDER_UNSUPPORTED: the builder runs only on
 *     accepted (diagnostic-free) builds per contract sec. 1.3. Malformed
 *     input that cannot be mapped returns IR_BUILDER_UNSUPPORTED with
 *     nothing owned. Coverage gaps in the representable surface are
 *     disclosed in the header and the package completion report and are
 *     routed to the Main Designer per the work-package manifest
 *     (escalation: IR boundary conflict -> Main Designer).
 *
 * Known representable-surface gaps (disclosed; see the completion
 * report):
 *   - runtime address-of a plain scalar local/param/global (AST_UN_ADDR
 *     over an identifier): the closed IR instruction set has no
 *     address-of node and invariants 4/10 forbid pointer-typed
 *     IR_LOCAL/IR_GLOBAL nodes, so this form is unmappable and returns
 *     IR_BUILDER_UNSUPPORTED. Representable address forms (const
 *     context `&global` via IRConst_ADDR, `&arr[i]` via IR_INDEX_ADDR,
 *     `&s.f` via IR_FIELD_ADDR) are fully supported.
 *   - slice-typed global const/var initializers from static slices
 *     (EvalValue EVAL_VAL_SLICE): the closed IRConst set has no slice
 *     constant kind and IRConst_ADDR carries a pointer type only
 *     (invariant 10), so a global slice initializer is unmappable and
 *     returns IR_BUILDER_UNSUPPORTED.
 *   These gaps are anticipated by the manifest risk line ("builder
 *   coverage gaps") and the escalation path (IR boundary conflict ->
 *   Main Designer).
 *
 * Ownership:
 *   - On IR_BUILDER_OK, *out_build is owned by the caller
 *     (ir_build_free). On IR_BUILDER_UNSUPPORTED / IR_BUILDER_OOM
 *     nothing is owned. The NameResult and LayoutBuild are borrowed and
 *     never modified. The EvalCtx used for constant evaluation is
 *     internal to the mapping packages (16c1b).
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_CORE_H
#define AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_CORE_H

#include "ir_core.h"
#include "../name/name.h"
#include "../types/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */

typedef enum IrBuilderStatus {
    IR_BUILDER_OK = 0,          /* build produced; *out_build owned by caller */
    IR_BUILDER_UNSUPPORTED,     /* defensive: construct outside the representable
                                 * surface / malformed input; nothing owned */
    IR_BUILDER_OOM              /* allocation failure; nothing owned */
} IrBuilderStatus;

/* ---------------------------------------------------------------------------
 * Builder context (foundation; owned by 16c1a, read-only for successors)
 * ------------------------------------------------------------------------- */

typedef struct BuilderCtx {
    const NameResult *result;    /* borrowed; never modified */
    const LayoutBuild *layout;   /* borrowed; never modified */
    IrBuild *build;              /* owned during construction; freed on failure,
                                  * transferred to the caller on IR_BUILDER_OK */
    bool oom;                    /* sticky allocation-failure flag */

    /* two-phase driver state */
    int phase;                   /* 1 = Phase A, 2 = Phase B */
    size_t module_index;         /* current module in canonical order */
    size_t decl_index;           /* current declaration in source order */

    /* per-construct mapping hooks (installed by the successor packages) */
    IrBuilderStatus (*map_module)(struct BuilderCtx *ctx,
                                  const NameModule *module);
    IrBuilderStatus (*map_decl)(struct BuilderCtx *ctx,
                                const NameModule *module,
                                const NameSymbol *sym);
    IrBuilderStatus (*map_body)(struct BuilderCtx *ctx,
                                const NameModule *module,
                                const NameSymbol *fn_sym);
} BuilderCtx;

/* Allocate builder-scratch memory through the context; returns NULL and
 * sets ctx->oom on allocation failure (sticky, mirroring ir_core). */
void *ir_builder_ctx_alloc(BuilderCtx *ctx, size_t size);

/* ---------------------------------------------------------------------------
 * Mapping hook registration (the 16c1b..16c1d seam)
 * ------------------------------------------------------------------------- */

/* Install the per-construct mapping implementations. The defaults are
 * defensive stubs returning IR_BUILDER_UNSUPPORTED (nothing owned).
 * Pass NULL to restore the default stub. The setters are process-global
 * (single-build compiler); ir_builder_build snapshots the current hooks
 * into the context at entry. */
void ir_builder_set_module_mapper(
    IrBuilderStatus (*fn)(BuilderCtx *ctx, const NameModule *module));
void ir_builder_set_decl_mapper(
    IrBuilderStatus (*fn)(BuilderCtx *ctx, const NameModule *module,
                          const NameSymbol *sym));
void ir_builder_set_body_mapper(
    IrBuilderStatus (*fn)(BuilderCtx *ctx, const NameModule *module,
                          const NameSymbol *fn_sym));

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

/* Build the IR graph for the resolved, validated build.
 *
 * `result` must be the resolved build (WP-M0-10 output); `layout` the
 * struct/enum layout facts (WP-M0-11b output). Callers run the builder
 * only after the semantic pipeline produced no diagnostics (contract
 * sec. 1.3), as the driver does.
 *
 * Returns IR_BUILDER_OK with *out_build set (owned by the caller), or
 * IR_BUILDER_UNSUPPORTED / IR_BUILDER_OOM with nothing owned. The
 * produced graph is intended to pass ir_core_verify (invariant
 * violations are reported by that API, not by this builder).
 */
IrBuilderStatus ir_builder_build(const NameResult *result,
                                 const LayoutBuild *layout,
                                 IrBuild **out_build);

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_CORE_H */
