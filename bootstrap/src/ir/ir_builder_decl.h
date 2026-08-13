/* bootstrap/src/ir/ir_builder_decl.h
 *
 * AI-Co Stage-0 IR builder Phase A mapping (WP-M0-16c1b).
 *
 * Implements the Phase A mapping of the accepted canonical IR contract
 * (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) sections 4.1-4.5 and
 * 6 over the resolved, validated build:
 *
 *   - module units in canonical order (entry module first, then imports
 *     depth-first in import order - the NameResult.modules array order),
 *     each with its import list (IR_IMPORT) and top-level declarations
 *     in source order (contract 4.1);
 *   - top-level declarations (contract 4.2): IR_STRUCT_DECL with layout
 *     facts (offsets/size/align from the WP-M0-11b LayoutBuild),
 *     IR_ENUM_DECL with continuation values (from LayoutBuild),
 *     IR_GLOBAL_CONST without storage, IR_GLOBAL_VAR with its IRConst
 *     initializer, IR_FUNCTION with parameter slots and a body
 *     placeholder (the empty IR_BLOCK Phase B fills);
 *   - the deterministic storage model (contract 4.3): parameter slots
 *     first (in parameter order), then local slots in first-declaration
 *     order, then compiler temporaries with deterministic slot indices
 *     (ir_builder_add_slot);
 *   - type interning (contract 4.4/6.3) and constant deduplication
 *     (contract 4.5/6.4, EvalValue -> IRConst, incl. sizeof/alignof as
 *     IRConst_INT of type usize). Composite (struct/array-of-struct)
 *     const references map by reusing the referenced const's IRConst
 *     (AC3 dedup) or, for forward/cross-module references, by
 *     recovering the field/element names from the referenced const's
 *     own initializer AST (see ir_builder_const_from_eval).
 *
 * This package owns the Phase A mappers installed through the 16c1a seam
 * (ir_builder_set_module_mapper / ir_builder_set_decl_mapper). It also
 * installs a Phase B body mapper that is a no-op at 16c1b: function
 * bodies are the Phase A placeholder blocks until WP-M0-16c1c/16c1d
 * install the expression/statement mappers.
 *
 * Construction order (determinism, contract 6.1): the module mapper runs
 * a creation pre-pass on its first invocation for a build, creating ALL
 * module/import/declaration nodes in canonical order (module order, then
 * per module source order) with their identity facts (names, spans,
 * struct size/align, enum underlying). The skeleton then calls the decl
 * mapper per symbol in canonical order, which fills each declaration's
 * detail (struct fields, enum members, const values, var initializers,
 * function params/slots/body). Creating every declaration node before
 * any type/value mapping is what makes by-value and pointer references
 * to declarations in ANY module resolvable as direct node pointers
 * without a deferral table (contract 4.1 "the same fully qualified name
 * always denotes the same IR node within a build").
 *
 * Runtime modules (rt.mem/rt.io/rt.proc/rt.trap) are mapped like any
 * module: IR_MODULE + IR_IMPORT + IR_FUNCTION nodes. Runtime functions
 * have no source (NameSymbol.decl == NULL), so their nodes carry a
 * deterministic synthetic point span whose file is the module/function
 * fqn, and no params (runtime signatures are not part of the resolved
 * build; the IR_CALL sites carry the spec signatures - contract 9.10,
 * owned by 16c1c/16c1d). The noreturn flag is set on rt.proc.exit and
 * rt.trap.report only (contract 4.2); noreturn runtime functions carry
 * no body placeholder (ir_core_verify invariant 3 allows body == NULL
 * for noreturn functions).
 *
 * Known representable-surface gaps (disclosed; see the package
 * completion report):
 *   - slice-typed global const/var initializers (EvalValue EVAL_VAL_SLICE
 *     from a static-slice const expression): the closed IRConst set has
 *     no slice constant kind (contract 4.5), so such a declaration is
 *     unmappable and the build returns IR_BUILDER_UNSUPPORTED with
 *     nothing owned. Inherited from the ir_builder_core.h gap list.
 *
 * Representation notes (documented conventions; no normalization here):
 *   - MINOR-1 (reviewer2 t_e1758837): enum member values are stored in
 *     IrEnumMember.value (int64_t, ir_core.h, 16b-owned) as the layout's
 *     two's-complement bit pattern via (int64_t)(uint64_t)layout value.
 *     For a u64 enum whose member value is >= 2^63 (big_unsigned) the
 *     dump prints that member as a negative number (%lld) while the same
 *     member's IRC_ENUM constant prints unsigned (%llu, uint64). The
 *     round-trip is byte-identical (determinism preserved) and the
 *     underlying u64 type is recorded on IR_ENUM_DECL.underlying, so a
 *     correct consumer can reinterpret; the dual representation is a
 *     documented wart. Normalization (e.g. a uint64 member value or an
 *     underlying-type-aware dump) would require touching 16b-owned
 *     ir_core.h / ir_dump.*, which are READ-ONLY for this package, so it
 *     is deferred and routed for 16b awareness via the Coordinator.
 *   - SUG-1 (reviewer2 t_e1758837, forward note): runtime function spans
 *     are synthetic (file = fqn), deviating from contract 8.1 "spans are
 *     never synthesized"; disclosed here and deterministic for
 *     sourceless runtime built-ins (invariant 2 requires a non-null
 *     span). Re-examine in WP-M0-16c2 when span/cause preservation is
 *     implemented. No action on this package.
 *
 * Ownership:
 *   - The mappers allocate build-owned memory (node payloads, names,
 *     spans, slot structs) directly; on failure they set build->oom and
 *     return IR_BUILDER_OOM. ir_build_free releases everything.
 *   - Mapper-scratch state (the per-build symbol -> node table) is
 *     process-global (single-build compiler, mirroring ir_builder_core.c
 *     hook table) and is released at the start of the next build.
 *   - The NameResult and LayoutBuild are borrowed and never modified.
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_DECL_H
#define AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_DECL_H

#include "ir_builder_core.h"
#include "../const/eval_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------- */

/* Install the Phase A mappers (module mapper + declaration mapper) and
 * the 16c1b Phase B no-op body mapper through the ir_builder_core.h
 * seam, and reset the package's per-build scratch state. Call before
 * ir_builder_build when the Phase A mapping should run (tests and the
 * 16c1d integration wiring do this). */
void ir_builder_decl_install(void);

/* The installed mapper entry points (also exposed for tests that verify
 * the seam directly). */
IrBuilderStatus ir_builder_decl_module(BuilderCtx *ctx,
                                       const NameModule *module);
IrBuilderStatus ir_builder_decl_decl(BuilderCtx *ctx,
                                     const NameModule *module,
                                     const NameSymbol *sym);

/* ---------------------------------------------------------------------------
 * Type mapping (contract 4.4; interning is delegated to ir_core)
 * ------------------------------------------------------------------------- */

/* Map an AST type node to its interned IrType descriptor. Every named
 * struct/enum reference resolves to the declaration IR node created by
 * the Phase A pre-pass (same fully qualified name -> same node, contract
 * 4.1). Returns NULL on allocation failure (build->oom set) or on
 * malformed/unrepresentable input (build->oom untouched); callers
 * distinguish with ctx->build->oom. `module` is the module whose name
 * table resolves the type node. */
IrType *ir_builder_type_from_ast(BuilderCtx *ctx, const NameModule *module,
                                 const AstNode *type_node);

/* Map a resolved Type (WP-M0-11a / EvalValue.type) to its interned IrType
 * descriptor. Same NULL semantics as ir_builder_type_from_ast. */
IrType *ir_builder_type_from_type(BuilderCtx *ctx, const Type *type);

/* ---------------------------------------------------------------------------
 * Constant mapping (contract 4.5/6.4; dedup is delegated to ir_core)
 * ------------------------------------------------------------------------- */

/* Map one constant-evaluated value (WP-M0-12 EvalValue) to its interned
 * IRConst representative.
 *
 *   `expected` is the declared type of the value position (an interned
 *   IrType): used for EVAL_VAL_NULL (must be a pointer type) and as the
 *   composite type for untyped array literals. For EVAL_VAL_INT the
 *   value's own type is authoritative (it carries enum vs integer).
 *   `expr` is the AST expression at the value position (the initializer
 *   or the corresponding sub-expression): used ONLY to recover struct
 *   literal field names so IRConst_STRUCT field values are emitted in
 *   declaration order (contract 4.5; spec 12.7 allows any literal
 *   order). May be NULL when `ev` contains no struct literals.
 *
 * Const references (spec 10.5: const names are constant expressions):
 * when the value position is a reference to a module-scope const
 * (AST_EXPR_IDENT or AST_EXPR_MEMBER), the referenced GLOBAL_CONST's
 * already-interned IRConst is reused directly when it has been mapped
 * (AC3 dedup: identical constants share one IRConst). This is what
 * makes composite (struct/array-of-struct) const references
 * representable: the composite branches need the literal AST at the
 * position for field-name recovery (contract 4.5), which a bare
 * reference does not provide. For a forward or cross-module reference
 * whose referenced const has not been filled yet, the composite
 * branches recover the field/element names from the referenced const's
 * own initializer AST (a struct-init / array-literal in the referenced
 * const's module; the EvalValue fields are in that literal's order, so
 * the same name-based reordering applies). For scalar forms the reuse
 * is equivalent to interning the per-kind branch would perform.
 *
 * Mapping summary: EVAL_VAL_INT -> IRConst_INT (bit pattern normalized
 * to the type's width) or IRConst_ENUM (enum type); EVAL_VAL_BOOL ->
 * IRConst_BOOL; EVAL_VAL_STR -> IRConst_STR; EVAL_VAL_NULL ->
 * IRConst_NULL (expected pointer type); EVAL_VAL_ADDR -> IRConst_ADDR
 * (target = the referenced IR_GLOBAL_VAR node); EVAL_VAL_ARRAY ->
 * IRConst_ARRAY (elements in index order; repetition form uses the
 * single evaluated element for every index); EVAL_VAL_STRUCT ->
 * IRConst_STRUCT (field values reordered to declaration order).
 * sizeof/alignof values arrive as EVAL_VAL_INT of type usize and map to
 * IRConst_INT (contract 4.5).
 *
 * Returns the interned representative, or NULL. On NULL, *out_supported
 * is set false when the form is unmappable (EVAL_VAL_SLICE - the
 * disclosed gap - or a malformed reference) and true otherwise (an
 * allocation failure; build->oom is set). */
IrConst *ir_builder_const_from_eval(BuilderCtx *ctx, const NameModule *module,
                                    IrType *expected, const AstNode *expr,
                                    const EvalValue *ev, bool *out_supported);

/* ---------------------------------------------------------------------------
 * Storage model (contract 4.3)
 * ------------------------------------------------------------------------- */

/* Append one slot to the function's slot table with the next
 * deterministic index (parameter slots first, then locals in
 * first-declaration order, then temporaries - ir_core.h IrSlotKind).
 * `name` is copied (NULL for compiler temporaries); `span` is cloned.
 * Returns the new slot, or NULL on allocation failure (build->oom set).
 * 16c1b creates the parameter slots while mapping a function; 16c1c/16c1d
 * add local and temporary slots through this API. Local slots are always
 * declared with an initializer via IR_LOCAL_DECL (contract 5.2: no
 * uninitialized local state; ir_core_verify invariant 4 rejects an
 * IR_LOCAL_DECL without an initializer). */
IrSlot *ir_builder_add_slot(IrBuild *b, IrNode *fn_node, IrSlotKind kind,
                            const char *name, IrType *type,
                            const DiagSpan *span);

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_DECL_H */
