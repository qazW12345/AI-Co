/* bootstrap/src/ir/ir_builder_expr.h
 *
 * AI-Co Stage-0 IR builder Phase B expression mapping and lowering
 * (WP-M0-16c1c).
 *
 * Implements the expression lowering of the accepted canonical IR
 * contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) sections
 * 5.3-5.4, 6, 9.6-9.8 and 12.1 over the resolved, validated build:
 *
 *   - every value-producing node of contract 5.3 (constants, IR_LOCAL /
 *     IR_GLOBAL refs, FIELD_ADDR / INDEX_ADDR / DEREF / LOAD / STORE,
 *     arithmetic / bitwise / logical / comparison incl. SLICE_EQ,
 *     SELECT, CALL, LEN / PTR / SLICE, CAST / WRAP, PTR_ADD / SUB /
 *     DIFF, ZERO) with its children, result type, evaluation order, and
 *     trap/enforcement obligations (declared trap codes) carried on the
 *     node;
 *   - value categories (contract 5.4): scalar direct values vs
 *     composite address-resident values, and the lvalue rules (no
 *     non-lvalue store, no addressable const, str/slice indexing yields
 *     a value address never an lvalue);
 *   - evaluation order (contract 6.1 = spec 10.4): the builder emits
 *     nodes in spec order; intermediate nodes (IR_ZERO, IR_STORE,
 *     materialization copies) are appended to the current block so the
 *     block's statement order is the evaluation order;
 *   - compound assignment lowered to destination-location + source +
 *     operation + store (contract 9.7 / 12.1) in the spec 10.4 order:
 *     the source is materialized into a temporary (a preceding
 *     IR_STORE statement) so the destination read (LOAD) executes
 *     AFTER the source evaluation - the IR's fixed per-node child
 *     order cannot express that ordering inside one STORE tree;
 *   - struct literals lowered to IR_ZERO + field stores; array literals
 *     lowered to IR_ZERO + element stores, with the repetition form
 *     `[e; N]` evaluating e exactly once (IRC-N1, Main Designer
 *     interpretation; planner alignment record
 *     docs/planning/AI-CO-PLANNER-ALIGNMENT-IRC-N1-REPETITION-EVAL-2026-08-12.md);
 *   - defensive IR_BUILDER_UNSUPPORTED (nothing owned) on the disclosed
 *     representable-surface gaps below.
 *
 * The statement mapper (WP-M0-16c1d, ir_builder_stmt.*) consumes this
 * API: it lowers a function body's statements into the function body's
 * IR_BLOCK, and for each expression position calls ir_builder_expr_lower
 * (conditions, selectors, arguments, return values, initializers,
 * assignment targets). Intermediate expression nodes are appended to
 * the same block (see the "block appending" convention below).
 *
 * Ownership and OOM discipline mirror the 16c1a/16c1b packages:
 *   - All nodes are build-owned (ir_build_free releases the build);
 *     payload strings/spans are copied at construction; build->oom is
 *     set on allocation failure and the caller returns IR_BUILDER_OOM.
 *   - The NameResult and LayoutBuild are borrowed and never modified.
 *   - Mapper-scratch state (the per-build local symbol -> slot table
 *     and the installed-runtime-signature marker) is process-global
 *     (single-build compiler) and is released at the start of the next
 *     build (ir_builder_expr_install).
 *
 * Representable-surface gaps (defensive IR_BUILDER_UNSUPPORTED with
 * nothing owned; disclosed; routed for Main Designer awareness per the
 * work-package manifest escalation line "IR boundary conflict ->
 * Main Designer"):
 *
 *   1. Runtime address-of a plain scalar local/param/global and
 *      address-of a dereference (`&*p`) or whole composite object:
 *      the closed IR instruction set has no address-of node and
 *      invariants 4/10 forbid pointer-typed IR_LOCAL/IR_GLOBAL nodes,
 *      so AST_UN_ADDR is representable only over AST_EXPR_INDEX
 *      (lowers to IR_INDEX_ADDR whose IR type is T*) and over
 *      AST_EXPR_MEMBER field access on a struct base (lowers to
 *      IR_FIELD_ADDR whose IR type is U*). This is the disclosed gap
 *      of ir_builder_core.h; const-context `&global` / `&arr[i]` /
 *      `&s.f` are handled at global-initializer sites by 16c1b
 *      (IRConst_ADDR) and are not reached here.
 *
 *   2. Slice-typed global const/var initializers (EvalValue
 *      EVAL_VAL_SLICE): the closed IRConst set has no slice constant
 *      kind; inherited from ir_builder_core.h/ir_builder_decl.h.
 *
 *   3. Assignment expressions in value-required positions (e.g.
 *      `f(x = 5)`, `var z = (x = 5)`): IR_STORE is an effect node with
 *      no value (contract 5.3 "void (effect)"), and the closed IR has
 *      no expression-level sequencing node, so the stored value cannot
 *      be produced in a value position. Assignment expressions are
 *      representable in effect positions (expression statements, for
 *      step expressions) and lower to the IR_STORE node.
 *
 *   4. Non-usize runtime index / slice-bound values: IR_INDEX_ADDR and
 *      IR_SLICE demand usize-typed index/bound operands (invariant 4),
 *      and the IR has no implicit-widening instruction (contract 9.7:
 *      widening is represented by the typed-node model, which applies
 *      to constants only). Constant integer indices/bounds are
 *      re-typed directly to usize (typed-node model); a non-usize
 *      runtime value in those positions is unmappable and returns
 *      IR_BUILDER_UNSUPPORTED. Spec 12.1/12.4 admits only indices
 *      implicitly convertible to usize per Table 11.1, so this is
 *      outside the intended accepted surface.
 *
 *   5. Mixed-width runtime comparisons / ternary branches: IR_EQ..GE
 *      and IR_SELECT demand identical operand types (invariant 4), and
 *      no widening instruction exists; constant operands are re-typed
 *      to the common type, runtime operands of different widths are
 *      unmappable. Same-type runtime operands are fully supported.
 *
 *   6. Struct/array literal branches of a `?:` expression: a literal
 *      object image is constructed eagerly (IR_ZERO + stores appended
 *      to the block before the IR_SELECT), so field/element side
 *      effects of a literal branch run before the selection. The
 *      closed IR has no lazy literal construction; non-literal
 *      composite branches (lvalues, calls, slices) are selected lazily
 *      by the IR_SELECT semantics. Disclosed for Main Designer
 *      awareness; the observable deviation requires a side-effecting
 *      field/element expression inside a `?:` literal branch.
 *
 *   7. Runtime-call signatures: 16c1b maps runtime functions
 *      (rt.mem/rt.io/rt.proc/rt.trap) as IR_FUNCTION nodes with no
 *      parameters and void return (runtime signatures are not part of
 *      the resolved build), while ir_core_verify demands
 *      IR_CALL/IR_CALL_TERM argument counts and types match the callee
 *      parameters (invariant 4). Per the 16c1b header's delegation
 *      ("the IR_CALL sites carry the spec signatures - contract 9.10,
 *      owned by 16c1c/16c1d"), this package attaches the spec
 *      signatures (params + return type + param slots) from spec 15.1
 *      -15.4 to runtime IR_FUNCTION nodes on first use as a call
 *      callee. The patch is idempotent, happens in Phase B after all
 *      Phase A nodes exist, and never alters a build that contains no
 *      runtime calls (16c1b's own tests are unaffected). For a patched
 *      NON-VOID runtime function (e.g. rt.io.write returning usize),
 *      the 16c1b placeholder body is empty, so invariant 5 (non-void
 *      function tails terminate) would reject the build; the patch
 *      appends an unreachable IR_TRAP terminator (user code 0) to that
 *      body - the runtime implementation is external and the IR body
 *      is never executed. This also keeps the whole build verifiable
 *      for 16c1d integration.
 *
 *   8. Composite `?:` / `&&`-`||` with composite... not applicable:
 *      logical operands are bool by spec 10.2, so IR_LAND/IR_LOR never
 *      carry composite operands.
 *
 * Block-appending convention (binding on the statement mapper 16c1d):
 *   The expression lowerer appends intermediate nodes that must execute
 *   before the produced value is consumed (IR_ZERO and field/element
 *   IR_STORE for struct/array literal images, materialization IR_STORE
 *   for composite values used where an lvalue is required, the
 *   compound-assignment source-materialization IR_STORE, and the
 *   single evaluated element of a repetition-form array literal when
 *   N == 0) directly to `block`'s statement list via ir_block_add_stmt.
 *   IR_STORE/IR_ZERO/IR_CALL and other expression-kind nodes are valid
 *   block children (ir_core_verify's block check verifies them as
 *   children; only terminator placement is restricted), so the block's
 *   statement order is the evaluation order. The statement mapper
 *   appends its own statement nodes (IR_EXPR_STMT, IR_LOCAL_DECL, ...)
 *   to the same block in the same order.
 *
 * Runtime signature table (spec 15.1-15.4) and the local symbol ->
 * slot registration API are part of this header so 16c1d can create
 * local slots in first-declaration order (contract 4.3) and register
 * them before lowering any statement that references them.
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_EXPR_H
#define AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_EXPR_H

#include "ir_builder_decl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Expression result model (contract 5.4 value categories)
 * ------------------------------------------------------------------------- */

/* The value category of a lowered expression result:
 *   IR_EXPR_SCALAR    - a direct scalar value node (IR_INT, IR_ADD,
 *                       IR_LOAD, IR_CALL with scalar return, ...). The
 *                       result node's type is the scalar type.
 *   IR_EXPR_LVALUE    - a storage location node (IR_LOCAL, IR_GLOBAL,
 *                       IR_FIELD_ADDR, IR_INDEX_ADDR on a mutable
 *                       array/slice base, IR_DEREF). `type` is the
 *                       value-at-location type (the pointee type for
 *                       FIELD_ADDR/INDEX_ADDR, whose IR node type is
 *                       U* / T*).
 *   IR_EXPR_COMPOSITE - a composite value: an address of an object
 *                       image (an lvalue-shaped node of composite type,
 *                       e.g. IR_LOCAL of a temporary), or a composite
 *                       value node (IR_STR / IR_SLICE / IR_CALL with
 *                       composite return / IR_SELECT of composites).
 *                       `type` is the composite type.
 *   IR_EXPR_EFFECT    - an effect-only node (IR_STORE produced by an
 *                       assignment expression). No value.
 */
typedef enum IrExprCat {
    IR_EXPR_SCALAR = 0,
    IR_EXPR_LVALUE,
    IR_EXPR_COMPOSITE,
    IR_EXPR_EFFECT
} IrExprCat;

typedef struct IrExprResult {
    IrExprCat cat;
    IrNode *node;   /* the result node (never NULL on IR_BUILDER_OK) */
    IrType *type;   /* the expression's result type (never NULL on
                     * IR_BUILDER_OK; value-at-location type for
                     * lvalues) */
} IrExprResult;

/* The value category required by the caller:
 *   IR_EXPR_WANT_ANY    - lower naturally; the caller inspects the
 *                         result category.
 *   IR_EXPR_WANT_VALUE  - a scalar value node (lvalues are loaded), or
 *                         the object-image address for composite
 *                         expressions. Assignment expressions (effect
 *                         only) are rejected (gap 3).
 *   IR_EXPR_WANT_LVALUE - the storage location node; any non-lvalue
 *                         expression is rejected (IR_BUILDER_UNSUPPORTED).
 */
typedef enum IrExprWant {
    IR_EXPR_WANT_ANY = 0,
    IR_EXPR_WANT_VALUE,
    IR_EXPR_WANT_LVALUE
} IrExprWant;

/* ---------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------- */

/* Reset the package's per-build scratch state (local-symbol registration
 * and the runtime-signature-installed marker) and prepare for a new
 * ir_builder_build. Call before ir_builder_build when expression
 * lowering should run (the 16c1d integration wiring and tests do this;
 * 16c1b's Phase A tests do not need it). */
void ir_builder_expr_install(void);

/* ---------------------------------------------------------------------------
 * Local-symbol slot registration (contract 4.3 storage model)
 * ------------------------------------------------------------------------- */

/* Register the slot index of one local variable/const declaration symbol
 * for the current build. The statement mapper (16c1d) creates local
 * slots in first-declaration order via ir_builder_add_slot and registers
 * each symbol before lowering statements that reference it; identifier
 * lowering resolves NAME_SYM_LOCAL_VAR / NAME_SYM_LOCAL_CONST
 * references through this table (symbol identity, not name, so shadowed
 * names resolve correctly). Parameter symbols resolve by declaration
 * position and are never registered here. No-op on NULL sym. */
void ir_builder_expr_register_local(IrBuild *b, const NameSymbol *fn_sym,
                                    const NameSymbol *local_sym,
                                    int64_t slot_index);

/* ---------------------------------------------------------------------------
 * Expression lowering (the 16c1c entry surface)
 * ------------------------------------------------------------------------- */

/* Lower one expression of the body of `fn_sym` in `module`.
 *
 *   fn_sym    the current function symbol (must be the NAME_SYM_FN
 *             whose body is being mapped; its IR_FUNCTION node is
 *             located by fully qualified name in the build).
 *   block     the current IR_BLOCK; intermediate nodes are appended to
 *             its statement list (block-appending convention above).
 *   expr      the AST expression to lower (accepted-build surface).
 *   want      the required value category.
 *   expected  the declared type at the expression position, or NULL
 *             (used for null literals - the pointer type; array
 *             literals - the array type; and constant re-typing of
 *             integer literals in widening positions).
 *
 * Returns IR_BUILDER_OK with *out set; IR_BUILDER_UNSUPPORTED for a
 * construct outside the representable surface (nothing owned); or
 * IR_BUILDER_OOM (nothing owned). The produced nodes are build-owned
 * and any intermediate nodes are already appended to `block`.
 */
IrBuilderStatus ir_builder_expr_lower(BuilderCtx *ctx,
                                      const NameModule *module,
                                      const NameSymbol *fn_sym,
                                      IrNode *block,
                                      const AstNode *expr,
                                      IrExprWant want,
                                      IrType *expected,
                                      IrExprResult *out);

/* Convenience wrappers for the statement mapper. Each returns
 * IR_BUILDER_OK with *out set, or an error status with *out untouched.
 *
 *   ir_builder_expr_to_value  lowers in IR_EXPR_WANT_VALUE mode and
 *                             returns the value node (scalar value, or
 *                             composite image address/value node).
 *   ir_builder_expr_to_lvalue lowers in IR_EXPR_WANT_LVALUE mode and
 *                             returns the storage-location node.
 *   ir_builder_expr_to_any    lowers in IR_EXPR_WANT_ANY mode and
 *                             returns the result node (used for
 *                             expression statements: the IR_STORE of an
 *                             assignment, the IR_CALL of a call, or the
 *                             natural node otherwise).
 */
IrBuilderStatus ir_builder_expr_to_value(BuilderCtx *ctx,
                                         const NameModule *module,
                                         const NameSymbol *fn_sym,
                                         IrNode *block,
                                         const AstNode *expr,
                                         IrType *expected,
                                         IrNode **out_value);
IrBuilderStatus ir_builder_expr_to_lvalue(BuilderCtx *ctx,
                                          const NameModule *module,
                                          const NameSymbol *fn_sym,
                                          IrNode *block,
                                          const AstNode *expr,
                                          IrNode **out_lvalue);
IrBuilderStatus ir_builder_expr_to_any(BuilderCtx *ctx,
                                       const NameModule *module,
                                       const NameSymbol *fn_sym,
                                       IrNode *block,
                                       const AstNode *expr,
                                       IrType *expected,
                                       IrNode **out_node);

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_BUILDER_EXPR_H */
