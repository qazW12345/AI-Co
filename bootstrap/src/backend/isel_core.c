/* bootstrap/src/backend/isel_core.c
 *
 * AI-Co Stage-0 x86-64 instruction selection core (WP-M0-17a1).
 *
 * Implements the deterministic IR -> x86-64 instruction selection of
 * isel_core.h over the closed IR node-kind set (contract sec. 5): a
 * canonical traversal (modules -> decls -> function bodies -> statements
 * -> expressions in evaluation order) selects instructions into an
 * ordered list with gapless deterministic virtual registers and labels,
 * preserving every node's declared trap obligation. See isel_core.h for
 * the normative rules (1-5), the pseudo-op set, and the scope boundary
 * (frame/regalloc 17b, trap branches 17c, coverage 17a2, COFF 18).
 *
 * Determinism is structural: the traversal iterates only the IR's
 * deterministic arrays (build->modules, module decls, block stmts, call
 * args, expression children) in their stored order; vregs and labels are
 * assigned from counters in that traversal order; no pointer address,
 * hash iteration, environment value, or host identity is ever consulted.
 * The assembly dump (isel_asm_dump) renders the instruction list in
 * emission order with stable formatting, so identical IR yields
 * byte-identical dump bytes (acceptance criterion 1).
 */
#include "isel_core.h"

#include "../diag/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

static const char *const kOpcodeNames[] = {
    "comment", "mov", "lea", "movzx", "movsx",
    "add", "sub", "imul", "idiv", "neg",
    "and", "or", "xor", "not",
    "shl", "shr", "sar",
    "cmp", "test", "setcc",
    "jmp", "jcc", "label", "call", "ret",
    "rep movsb", "rep stosb", "sliceeq", "slice", "strcmp", "utf8",
    "ptrdiff", "trap"
};

const char *isel_opcode_text(IselOpcode op)
{
    if (op < 0 || (size_t)op >= sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0])) {
        return "?";
    }
    return kOpcodeNames[op];
}

static const char *const kCondNames[] = {
    "e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae"
};

const char *isel_cond_text(IselCond cond)
{
    if (cond < 0 || (size_t)cond >= sizeof(kCondNames) / sizeof(kCondNames[0])) {
        return "?";
    }
    return kCondNames[cond];
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

IselOperand isel_operand_none(void)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_NONE;
    return op;
}

static IselOperand make_vreg(int64_t vreg, int width)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_VREG;
    op.vreg = vreg;
    op.width = width;
    return op;
}

static IselOperand make_imm(int64_t value, int width, bool is_unsigned)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_IMM;
    op.imm = value;
    op.width = width;
    op.is_unsigned = is_unsigned;
    return op;
}

static IselOperand make_slot(int64_t index, int width)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_SLOT;
    op.id = index;
    op.width = width;
    return op;
}

static IselOperand make_global(int64_t node_id)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_GLOBAL;
    op.id = node_id;
    op.width = 8;
    return op;
}

static IselOperand make_str(int64_t const_id)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_STR;
    op.id = const_id;
    op.width = 8;
    return op;
}

static IselOperand make_func(int64_t node_id)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_FUNC;
    op.id = node_id;
    op.width = 8;
    return op;
}

static IselOperand make_label(int64_t label)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_LABEL;
    op.id = label;
    return op;
}

static IselOperand make_mem(int64_t base_vreg, int64_t disp, int width)
{
    IselOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = ISEL_OP_MEM;
    op.vreg = base_vreg;
    op.imm = disp;
    op.width = width;
    return op;
}

/* Operand width in bytes from an IR type (1/2/4/8). Composite types
 * (str/slice/array/struct) are address-resident: their address operand is
 * 8 bytes. */
static int type_operand_width(const IrType *t)
{
    if (t == NULL) {
        return 0;
    }
    switch (t->kind) {
    case IRT_BOOL:
    case IRT_I8:
    case IRT_U8:
        return 1;
    case IRT_I16:
    case IRT_U16:
        return 2;
    case IRT_I32:
    case IRT_U32:
        return 4;
    case IRT_I64:
    case IRT_U64:
    case IRT_ISIZE:
    case IRT_USIZE:
        return 8;
    case IRT_STR:
    case IRT_ARRAY:
    case IRT_SLICE:
        return 8;   /* address of the object image */
    case IRT_PTR:
        return 8;
    case IRT_ENUM:
        /* underlying integer width */
        if (t->u.decl != NULL && t->u.decl->kind == IR_ENUM_DECL) {
            return type_operand_width(t->u.decl->u.enum_decl.underlying);
        }
        return 4;
    case IRT_STRUCT:
        return 8;   /* address of the object image */
    default:
        return 0;
    }
}

/* Signedness of an IR type for comparison-condition selection. */
static bool type_is_signed(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    switch (t->kind) {
    case IRT_I8:
    case IRT_I16:
    case IRT_I32:
    case IRT_I64:
    case IRT_ISIZE:
        return true;
    case IRT_ENUM:
        if (t->u.decl != NULL && t->u.decl->kind == IR_ENUM_DECL) {
            return type_is_signed(t->u.decl->u.enum_decl.underlying);
        }
        return false;
    default:
        return false;
    }
}

static bool type_is_composite(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    switch (t->kind) {
    case IRT_STR:
    case IRT_ARRAY:
    case IRT_SLICE:
    case IRT_STRUCT:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Selection context
 * ------------------------------------------------------------------------- */

typedef struct SelCtx {
    const IrBuild *build;
    IselOutput *out;
    /* node id -> assigned vreg, or -1 (allocated in first-visit order) */
    int64_t *vreg_of;
    /* construct node id -> break/continue label ids, or -1 */
    int64_t *break_label_of;
    int64_t *continue_label_of;
} SelCtx;

static bool sel_emit(SelCtx *ctx, IselOpcode op,
                     IselOperand dst, IselOperand src1, IselOperand src2,
                     IselCond cond, int scale, bool mod,
                     const char *trap, int64_t ir_node_id)
{
    IselOutput *out = ctx->out;
    if (out->count == out->cap) {
        size_t ncap = out->cap == 0 ? 16 : out->cap * 2;
        IselInsn *p = (IselInsn *)realloc(out->insns, ncap * sizeof(IselInsn));
        if (p == NULL) {
            out->oom = true;
            return false;
        }
        out->insns = p;
        out->cap = ncap;
    }
    {
        IselInsn *insn = &out->insns[out->count];
        memset(insn, 0, sizeof(*insn));
        insn->op = op;
        insn->dst = dst;
        insn->src1 = src1;
        insn->src2 = src2;
        insn->cond = cond;
        insn->scale = scale;
        insn->mod = mod;
        insn->trap = trap;
        insn->ir_node_id = ir_node_id;
    }
    out->count++;
    return true;
}

static bool sel_comment(SelCtx *ctx, const char *note, int64_t ir_node_id)
{
    IselInsn insn;
    char *owned = NULL;
    IselOutput *out = ctx->out;
    if (note != NULL) {
        owned = (char *)malloc(strlen(note) + 1);
        if (owned == NULL) {
            out->oom = true;
            return false;
        }
        memcpy(owned, note, strlen(note) + 1);
    }
    if (out->count == out->cap) {
        size_t ncap = out->cap == 0 ? 16 : out->cap * 2;
        IselInsn *p = (IselInsn *)realloc(out->insns, ncap * sizeof(IselInsn));
        if (p == NULL) {
            free(owned);
            out->oom = true;
            return false;
        }
        out->insns = p;
        out->cap = ncap;
    }
    memset(&insn, 0, sizeof(insn));
    insn.op = ISEL_COMMENT;
    insn.note = owned;   /* owned copy; freed by isel_output_free */
    insn.ir_node_id = ir_node_id;
    out->insns[out->count++] = insn;
    return true;
}

static int64_t sel_new_label(SelCtx *ctx)
{
    return ctx->out->next_label++;
}

static int64_t sel_vreg_for_node(SelCtx *ctx, int64_t node_id)
{
    if (node_id >= 0 && ctx->vreg_of[node_id] >= 0) {
        return ctx->vreg_of[node_id];
    }
    {
        int64_t v = ctx->out->next_vreg++;
        if (node_id >= 0) {
            ctx->vreg_of[node_id] = v;
        }
        return v;
    }
}

/* Condition code for a comparison IR node: EQ/NE are the same for signed
 * and unsigned; LT/LE/GT/GE select the signed or unsigned form from the
 * operand type. */
static IselCond cond_for_ir_kind(IrNodeKind kind, const IrType *t)
{
    bool s = type_is_signed(t);
    switch (kind) {
    case IR_EQ: return ISEL_COND_E;
    case IR_NE: return ISEL_COND_NE;
    case IR_LT: return s ? ISEL_COND_L : ISEL_COND_B;
    case IR_LE: return s ? ISEL_COND_LE : ISEL_COND_BE;
    case IR_GT: return s ? ISEL_COND_G : ISEL_COND_A;
    case IR_GE: return s ? ISEL_COND_GE : ISEL_COND_AE;
    default:    return ISEL_COND_E;
    }
}

/* ---------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

static bool select_stmt(SelCtx *ctx, const IrNode *fn, IrNode *stmt);
static bool select_value(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg);

/* ---------------------------------------------------------------------------
 * Constant emission (data-image references; materialization owned by
 * 18/17b). These emit no instructions in the core; global consts are
 * folded at use sites by the value selector.
 * ------------------------------------------------------------------------- */

static bool select_global_const(SelCtx *ctx, IrNode *node)
{
    char buf[256];
    const char *name = node->u.global_const.name != NULL
                           ? node->u.global_const.name : "?";
    (void)snprintf(buf, sizeof(buf), "const %s", name);
    return sel_comment(ctx, buf, node->id);
}

static bool select_global_var(SelCtx *ctx, IrNode *node)
{
    char buf[256];
    const char *name = node->u.global_var.name != NULL
                           ? node->u.global_var.name : "?";
    (void)snprintf(buf, sizeof(buf), "global %s", name);
    return sel_comment(ctx, buf, node->id);
}

static bool select_struct_decl(SelCtx *ctx, IrNode *node)
{
    char buf[256];
    const char *name = node->u.struct_decl.name != NULL
                           ? node->u.struct_decl.name : "?";
    (void)snprintf(buf, sizeof(buf), "struct %s (%lld bytes)",
                   name, (long long)node->u.struct_decl.size);
    return sel_comment(ctx, buf, node->id);
}

static bool select_enum_decl(SelCtx *ctx, IrNode *node)
{
    char buf[256];
    const char *name = node->u.enum_decl.name != NULL
                           ? node->u.enum_decl.name : "?";
    (void)snprintf(buf, sizeof(buf), "enum %s", name);
    return sel_comment(ctx, buf, node->id);
}

/* ---------------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------------- */

static bool select_block(SelCtx *ctx, const IrNode *fn, IrNode *block)
{
    size_t i;
    for (i = 0; i < block->u.block.nstmts; i++) {
        if (!select_stmt(ctx, fn, block->u.block.stmts[i])) {
            return false;
        }
    }
    return true;
}

static bool select_local_decl(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    IrNode *init = node->u.local_decl.init;
    int64_t v = -1;
    IrSlot *slot = NULL;
    int64_t slot_index = node->u.local_decl.slot_index;
    size_t i;
    const IrNode *f = fn;
    /* resolve the slot table entry for the width / composite decision */
    if (f != NULL && f->kind == IR_FUNCTION) {
        for (i = 0; i < f->u.function.nslots; i++) {
            if (f->u.function.slots[i]->index == slot_index) {
                slot = f->u.function.slots[i];
                break;
            }
        }
    }
    if (init == NULL) {
        return true;   /* defensive: builder always supplies an init */
    }
    if (!select_value(ctx, fn, init, &v)) {
        return false;
    }
    if (slot != NULL && type_is_composite(slot->type)) {
        /* full-object copy from the init image to the slot image */
        if (!sel_emit(ctx, ISEL_REP_MOVSB, make_slot(slot_index, 8),
                      make_vreg(v, 8), make_imm(slot->type->size, 8, true),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
    } else {
        int w = slot != NULL ? type_operand_width(slot->type) : 8;
        if (!sel_emit(ctx, ISEL_MOV, make_slot(slot_index, w),
                      make_vreg(v, w), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
    }
    return true;
}

static bool select_if(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t vc = -1;
    int64_t l_else = sel_new_label(ctx);
    int64_t l_end = sel_new_label(ctx);
    if (!select_value(ctx, fn, node->u.if_stmt.cond, &vc)) {
        return false;
    }
    /* test the bool condition */
    if (!sel_emit(ctx, ISEL_TEST, make_vreg(vc, 1), make_vreg(vc, 1),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JCC, isel_operand_none(), make_label(l_else),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!select_block(ctx, fn, node->u.if_stmt.then_block)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(), make_label(l_end),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_else), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (node->u.if_stmt.else_block != NULL) {
        if (!select_block(ctx, fn, node->u.if_stmt.else_block)) {
            return false;
        }
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_end), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return true;
}

static bool select_while(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t vc = -1;
    int64_t l_loop = sel_new_label(ctx);
    int64_t l_exit = sel_new_label(ctx);
    /* register break/continue targets for this construct */
    if (node->id >= 0) {
        ctx->break_label_of[node->id] = l_exit;
        ctx->continue_label_of[node->id] = l_loop;
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_loop), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.while_stmt.cond, &vc)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_TEST, make_vreg(vc, 1), make_vreg(vc, 1),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JCC, isel_operand_none(), make_label(l_exit),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!select_block(ctx, fn, node->u.while_stmt.body)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(), make_label(l_loop),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_exit), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return true;
}

static bool select_for(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t vc = -1;
    int64_t l_loop = sel_new_label(ctx);
    int64_t l_continue = sel_new_label(ctx);
    int64_t l_exit = sel_new_label(ctx);
    if (node->id >= 0) {
        ctx->break_label_of[node->id] = l_exit;
        /* continue targets the step; absent step, the condition */
        ctx->continue_label_of[node->id] =
            node->u.for_stmt.step != NULL ? l_continue : l_loop;
    }
    if (node->u.for_stmt.init != NULL) {
        if (!select_stmt(ctx, fn, node->u.for_stmt.init)) {
            return false;
        }
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_loop), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (node->u.for_stmt.cond != NULL) {
        if (!select_value(ctx, fn, node->u.for_stmt.cond, &vc)) {
            return false;
        }
        if (!sel_emit(ctx, ISEL_TEST, make_vreg(vc, 1), make_vreg(vc, 1),
                      isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                      node->id)) {
            return false;
        }
        if (!sel_emit(ctx, ISEL_JCC, isel_operand_none(), make_label(l_exit),
                      isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                      node->id)) {
            return false;
        }
    }
    if (!select_block(ctx, fn, node->u.for_stmt.body)) {
        return false;
    }
    if (node->u.for_stmt.step != NULL) {
        if (!sel_emit(ctx, ISEL_LABEL, make_label(l_continue),
                      isel_operand_none(), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
        if (!select_stmt(ctx, fn, node->u.for_stmt.step)) {
            return false;
        }
    }
    if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(), make_label(l_loop),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_exit), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return true;
}

static bool select_case_body(SelCtx *ctx, const IrNode *fn, IrNode *body)
{
    return select_block(ctx, fn, body);
}

static bool select_switch(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t vs = -1;
    int64_t l_end = sel_new_label(ctx);
    int64_t l_default = sel_new_label(ctx);
    int64_t *l_cases = NULL;
    size_t i;
    if (node->id >= 0) {
        ctx->break_label_of[node->id] = l_end;
        ctx->continue_label_of[node->id] = -1;
    }
    if (!select_value(ctx, fn, node->u.switch_stmt.selector, &vs)) {
        return false;
    }
    /* allocate one label per case, in source order (deterministic) */
    if (node->u.switch_stmt.ncases > 0) {
        l_cases = (int64_t *)malloc(node->u.switch_stmt.ncases *
                                    sizeof(int64_t));
        if (l_cases == NULL) {
            ctx->out->oom = true;
            return false;
        }
        for (i = 0; i < node->u.switch_stmt.ncases; i++) {
            l_cases[i] = sel_new_label(ctx);
        }
    }
    /* pass 1: compare selector against each case value in source order
     * and jump to the case label on equality; fall through to the
     * default (or the end label) when nothing matches */
    for (i = 0; i < node->u.switch_stmt.ncases; i++) {
        IrNode *c = node->u.switch_stmt.cases[i];
        const IrConst *val = c->u.case_clause.value;
        const IrType *sel_type = node->u.switch_stmt.selector != NULL
                                     ? node->u.switch_stmt.selector->type
                                     : NULL;
        int w = type_operand_width(sel_type != NULL ? sel_type
                                                    : node->type);
        if (val != NULL && val->kind == IRC_INT) {
            bool uns = !type_is_signed(val->type);
            if (!sel_emit(ctx, ISEL_CMP, isel_operand_none(),
                          make_vreg(vs, w), make_imm((int64_t)val->u.int_bits,
                                                     w, uns),
                          ISEL_COND_E, 0, false, NULL, c->id)) {
                free(l_cases);
                return false;
            }
        } else if (val != NULL && val->kind == IRC_ENUM) {
            if (!sel_emit(ctx, ISEL_CMP, isel_operand_none(),
                          make_vreg(vs, w),
                          make_imm((int64_t)val->u.en.value, w, false),
                          ISEL_COND_E, 0, false, NULL, c->id)) {
                free(l_cases);
                return false;
            }
        } else {
            /* defensive: per the IR contract case values are resolved
             * constants, so this path is unreachable on verified IR; a
             * comment keeps the selection deterministic */
            if (!sel_comment(ctx, "case (non-constant value)", c->id)) {
                free(l_cases);
                return false;
            }
        }
        if (!sel_emit(ctx, ISEL_JCC, isel_operand_none(),
                      make_label(l_cases[i]), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, c->id)) {
            free(l_cases);
            return false;
        }
    }
    if (node->u.switch_stmt.default_clause != NULL) {
        if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(),
                      make_label(l_default), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL,
                      node->u.switch_stmt.default_clause->id)) {
            free(l_cases);
            return false;
        }
    } else {
        /* no default: the compare chain falls through to the end label
         * (spec sec. 13.2: no statement executes) */
        if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(),
                      make_label(l_end), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            free(l_cases);
            return false;
        }
    }
    /* pass 2: case bodies in source order (each ends in a terminator;
     * no fall-through is possible per contract sec. 5.6) */
    for (i = 0; i < node->u.switch_stmt.ncases; i++) {
        IrNode *c = node->u.switch_stmt.cases[i];
        if (!sel_emit(ctx, ISEL_LABEL, make_label(l_cases[i]),
                      isel_operand_none(), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, c->id)) {
            free(l_cases);
            return false;
        }
        if (!select_case_body(ctx, fn, c->u.case_clause.body)) {
            free(l_cases);
            return false;
        }
    }
    if (node->u.switch_stmt.default_clause != NULL) {
        if (!sel_emit(ctx, ISEL_LABEL, make_label(l_default),
                      isel_operand_none(), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL,
                      node->u.switch_stmt.default_clause->id)) {
            free(l_cases);
            return false;
        }
        if (!select_block(ctx, fn,
                          node->u.switch_stmt.default_clause->u.default_clause.body)) {
            free(l_cases);
            return false;
        }
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_end), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        free(l_cases);
        return false;
    }
    free(l_cases);
    return true;
}

static bool select_break(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t target = -1;
    (void)fn;
    if (node->u.break_stmt.target != NULL) {
        target = node->u.break_stmt.target->id;
    }
    if (target >= 0 && ctx->break_label_of[target] >= 0) {
        return sel_emit(ctx, ISEL_JMP, isel_operand_none(),
                        make_label(ctx->break_label_of[target]),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    }
    return true;   /* defensive: placement invariant is pre-IR */
}

static bool select_continue(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t target = -1;
    (void)fn;
    if (node->u.continue_stmt.target != NULL) {
        target = node->u.continue_stmt.target->id;
    }
    if (target >= 0 && ctx->continue_label_of[target] >= 0) {
        return sel_emit(ctx, ISEL_JMP, isel_operand_none(),
                        make_label(ctx->continue_label_of[target]),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    }
    return true;   /* defensive */
}

static bool select_return(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    (void)fn;
    if (node->u.return_stmt.value != NULL) {
        int64_t v = -1;
        if (!select_value(ctx, fn, node->u.return_stmt.value, &v)) {
            return false;
        }
        /* return value register placement is ABI-owned (17b2); the core
         * records the value vreg and emits the terminator */
        return sel_emit(ctx, ISEL_RET, isel_operand_none(),
                        make_vreg(v, 8), isel_operand_none(),
                        ISEL_COND_E, 0, false, NULL, node->id);
    }
    return sel_emit(ctx, ISEL_RET, isel_operand_none(), isel_operand_none(),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

static bool select_expr_stmt(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t v = -1;
    if (node->u.expr_stmt.expr == NULL) {
        return true;
    }
    return select_value(ctx, fn, node->u.expr_stmt.expr, &v);
}

static bool select_call_term(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    int64_t callee_id = -1;
    size_t i;
    (void)fn;
    if (node->u.call_term.callee != NULL) {
        callee_id = node->u.call_term.callee->id;
    }
    for (i = 0; i < node->u.call_term.nargs; i++) {
        int64_t v = -1;
        if (!select_value(ctx, fn, node->u.call_term.args[i], &v)) {
            return false;
        }
    }
    /* noreturn call terminates; no RET after (spec sec. 15.7) */
    return sel_emit(ctx, ISEL_CALL, isel_operand_none(),
                    make_func(callee_id), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_trap(SelCtx *ctx, const IrNode *fn, IrNode *node)
{
    (void)fn;
    {
        const char *code = node->u.trap.code;
        if (code == NULL && node->u.trap.has_user_code) {
            code = "user";
        }
        return sel_emit(ctx, ISEL_TRAP, isel_operand_none(),
                        make_imm(node->u.trap.has_user_code
                                     ? node->u.trap.user_code : 0,
                                 8, true),
                        isel_operand_none(), ISEL_COND_E, 0, false, code,
                        node->id);
    }
}

static bool select_stmt(SelCtx *ctx, const IrNode *fn, IrNode *stmt)
{
    if (stmt == NULL) {
        return true;
    }
    switch (stmt->kind) {
    case IR_BLOCK:
        return select_block(ctx, fn, stmt);
    case IR_LOCAL_DECL:
        return select_local_decl(ctx, fn, stmt);
    case IR_IF:
        return select_if(ctx, fn, stmt);
    case IR_WHILE:
        return select_while(ctx, fn, stmt);
    case IR_FOR:
        return select_for(ctx, fn, stmt);
    case IR_SWITCH:
        return select_switch(ctx, fn, stmt);
    case IR_BREAK:
        return select_break(ctx, fn, stmt);
    case IR_CONTINUE:
        return select_continue(ctx, fn, stmt);
    case IR_RETURN:
        return select_return(ctx, fn, stmt);
    case IR_EXPR_STMT:
        return select_expr_stmt(ctx, fn, stmt);
    case IR_EMPTY:
        return sel_comment(ctx, "empty", stmt->id);
    case IR_CALL_TERM:
        return select_call_term(ctx, fn, stmt);
    case IR_TRAP:
        return select_trap(ctx, fn, stmt);
    default:
        /* statement kinds appearing inside constructs (CASE/DEFAULT) are
         * selected by their construct; a stray occurrence is defensively
         * ignored (invariants forbid it). */
        return true;
    }
}

/* ---------------------------------------------------------------------------
 * Value-producing nodes (instructions)
 * ------------------------------------------------------------------------- */

static bool select_const_value(SelCtx *ctx, const IrNode *fn, IrNode *node,
                               int64_t *out_vreg)
{
    int64_t v;
    const IrConst *c = node->u.constant.value;
    int w;
    (void)fn;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    switch (node->kind) {
    case IR_INT:
        w = type_operand_width(node->type);
        return sel_emit(ctx, ISEL_MOV, make_vreg(v, w),
                        make_imm((int64_t)c->u.int_bits, w,
                                 !type_is_signed(node->type)),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    case IR_BOOL:
        return sel_emit(ctx, ISEL_MOV, make_vreg(v, 1),
                        make_imm(c->u.b ? 1 : 0, 1, false),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    case IR_NULL:
        return sel_emit(ctx, ISEL_MOV, make_vreg(v, 8), make_imm(0, 8, true),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    case IR_STR:
        /* the str value is the address of its read-only pair image */
        return sel_emit(ctx, ISEL_LEA, make_vreg(v, 8),
                        make_str(c->id), isel_operand_none(),
                        ISEL_COND_E, 0, false, NULL, node->id);
    case IR_ENUM_VAL:
        w = type_operand_width(node->type);
        return sel_emit(ctx, ISEL_MOV, make_vreg(v, w),
                        make_imm((int64_t)c->u.en.value, w,
                                 !type_is_signed(node->type)),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    default:
        return false;
    }
}

static bool select_local(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg)
{
    int64_t v;
    (void)fn;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    return sel_emit(ctx, ISEL_LEA, make_vreg(v, 8),
                    make_slot(node->u.local.slot_index, 8), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_global(SelCtx *ctx, const IrNode *fn, IrNode *node,
                          int64_t *out_vreg)
{
    int64_t v;
    int64_t target_id = -1;
    (void)fn;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (node->u.global.target != NULL) {
        target_id = node->u.global.target->id;
    }
    return sel_emit(ctx, ISEL_LEA, make_vreg(v, 8),
                    make_global(target_id), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_field_addr(SelCtx *ctx, const IrNode *fn, IrNode *node,
                              int64_t *out_vreg)
{
    int64_t vb = -1;
    int64_t v;
    int64_t offset = 0;
    const IrType *base_type = NULL;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.field_addr.base, &vb)) {
        return false;
    }
    /* byte offset from the struct decl layout facts (spec sec. 7.4) */
    if (node->u.field_addr.base != NULL) {
        base_type = node->u.field_addr.base->type;
    }
    if (base_type != NULL && base_type->kind == IRT_STRUCT &&
        base_type->u.decl != NULL &&
        base_type->u.decl->kind == IR_STRUCT_DECL) {
        const IrNode *decl = base_type->u.decl;
        int64_t fi = node->u.field_addr.field_index;
        if (fi >= 0 && (size_t)fi < decl->u.struct_decl.nfields) {
            offset = decl->u.struct_decl.fields[fi].byte_offset;
        }
    }
    return sel_emit(ctx, ISEL_LEA, make_vreg(v, 8),
                    make_mem(vb, offset, 8), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_index_addr(SelCtx *ctx, const IrNode *fn, IrNode *node,
                              int64_t *out_vreg)
{
    int64_t vb = -1;
    int64_t vi = -1;
    int64_t v;
    int elem_size = 1;
    const IrType *res = node->type;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.index_addr.base, &vb)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.index_addr.index, &vi)) {
        return false;
    }
    if (res != NULL && res->kind == IRT_PTR && res->u.ptr.elem != NULL) {
        elem_size = (int)res->u.ptr.elem->size;
        if (elem_size <= 0) {
            elem_size = 1;
        }
    }
    if (elem_size == 1 || elem_size == 2 || elem_size == 4 || elem_size == 8) {
        /* scaled index in a single lea */
        return sel_emit(ctx, ISEL_LEA, make_vreg(v, 8),
                        make_vreg(vb, 8), make_vreg(vi, 8),
                        ISEL_COND_E, elem_size, false, "AIC-R0807",
                        node->id);
    }
    /* general element size: v = base; vi *= scale; v += vi
     * (deterministic three-step sequence) */
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, 8), make_vreg(vb, 8),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_IMUL, make_vreg(vi, 8),
                  make_imm(elem_size, 8, false), isel_operand_none(),
                  ISEL_COND_E, 0, false, NULL, node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_ADD, make_vreg(v, 8), make_vreg(vi, 8),
                  isel_operand_none(), ISEL_COND_E, 0, false,
                  "AIC-R0807", node->id)) {
        return false;
    }
    return true;
}

static bool select_deref(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg)
{
    int64_t vp = -1;
    int64_t v;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.deref.ptr, &vp)) {
        return false;
    }
    /* the lvalue address is the pointer value; null check is 17c, the
     * obligation is preserved here */
    return sel_emit(ctx, ISEL_MOV, make_vreg(v, 8), make_vreg(vp, 8),
                    isel_operand_none(), ISEL_COND_E, 0, false,
                    "AIC-R0809", node->id);
}

static bool select_load(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t va = -1;
    int64_t v;
    int w;
    const char *trap = NULL;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.load.lvalue, &va)) {
        return false;
    }
    w = type_operand_width(node->type);
    if (node->type != NULL && node->type->kind == IRT_BOOL) {
        trap = "AIC-R0805";   /* bool load must be 0 or 1 */
    }
    return sel_emit(ctx, ISEL_MOV, make_vreg(v, w),
                    make_mem(va, 0, w), isel_operand_none(),
                    ISEL_COND_E, 0, false, trap, node->id);
}

static bool select_store(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg)
{
    int64_t vd = -1;
    int64_t vv = -1;
    int w;
    (void)out_vreg;   /* IR_STORE produces no value */
    if (!select_value(ctx, fn, node->u.store.dest, &vd)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.store.value, &vv)) {
        return false;
    }
    {
        const IrType *dt = node->u.store.dest != NULL
                               ? node->u.store.dest->type : NULL;
        if (dt != NULL && type_is_composite(dt)) {
            /* complete object representation copy (spec sec. 9.1/9.3) */
            return sel_emit(ctx, ISEL_REP_MOVSB, make_mem(vd, 0, 8),
                            make_mem(vv, 0, 8),
                            make_imm(dt->size, 8, true), ISEL_COND_E, 0,
                            false, NULL, node->id);
        }
        w = type_operand_width(dt);
        return sel_emit(ctx, ISEL_MOV, make_mem(vd, 0, w),
                        make_vreg(vv, w), isel_operand_none(),
                        ISEL_COND_E, 0, false, NULL, node->id);
    }
}

/* Binary integer arithmetic: add/sub/mul/shifts/bitwise. */
static bool select_binary_arith(SelCtx *ctx, const IrNode *fn, IrNode *node,
                                int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int w;
    IselOpcode op;
    const char *trap = NULL;
    switch (node->kind) {
    case IR_ADD: op = ISEL_ADD; trap = "AIC-R0802"; break;
    case IR_SUB: op = ISEL_SUB; trap = "AIC-R0802"; break;
    case IR_MUL: op = ISEL_IMUL; trap = "AIC-R0802"; break;
    case IR_BAND: op = ISEL_AND; break;
    case IR_BOR:  op = ISEL_OR;  break;
    case IR_BXOR: op = ISEL_XOR; break;
    default: return false;
    }
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    w = type_operand_width(node->type);
    /* dst = left op right (two-address form) */
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(vl, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, op, make_vreg(v, w), make_vreg(vr, w),
                    isel_operand_none(), ISEL_COND_E, 0, false, trap,
                    node->id);
}

static bool select_div_mod(SelCtx *ctx, const IrNode *fn, IrNode *node,
                           int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int w;
    bool is_mod = node->kind == IR_MOD;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    w = type_operand_width(node->type);
    /* dividend -> dst; idiv divisor (quotient in dst, remainder in
     * mod); the rax/rdx register pair placement is owned by 17b2 */
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(vl, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_IDIV, make_vreg(v, w), make_vreg(vr, w),
                    isel_operand_none(), ISEL_COND_E, 0, is_mod,
                    "AIC-R0803", node->id);
}

static bool select_neg(SelCtx *ctx, const IrNode *fn, IrNode *node,
                       int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    int w;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.unary.operand, &vo)) {
        return false;
    }
    w = type_operand_width(node->type);
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(vo, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_NEG, make_vreg(v, w), isel_operand_none(),
                    isel_operand_none(), ISEL_COND_E, 0, false,
                    "AIC-R0802", node->id);
}

static bool select_shift(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int w;
    IselOpcode op;
    bool s = type_is_signed(node->type);
    if (node->kind == IR_SHL) {
        op = ISEL_SHL;
    } else if (node->kind == IR_SHR) {
        op = s ? ISEL_SAR : ISEL_SHR;   /* arithmetic vs logical (spec
                                         * sec. 11.3) */
    } else {
        return false;
    }
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    w = type_operand_width(node->type);
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(vl, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    /* count in a vreg; the %cl placement is owned by 17b2/17a2 */
    return sel_emit(ctx, op, make_vreg(v, w), make_vreg(vr, 1),
                    isel_operand_none(), ISEL_COND_E, 0, false,
                    "AIC-R0804", node->id);
}

static bool select_bnot(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    int w;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.unary.operand, &vo)) {
        return false;
    }
    w = type_operand_width(node->type);
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(vo, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_NOT, make_vreg(v, w), isel_operand_none(),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

static bool select_lnot(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.unary.operand, &vo)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, 1), make_vreg(vo, 1),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_XOR, make_vreg(v, 1), make_imm(1, 1, false),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

/* Short-circuit boolean: IR_LAND / IR_LOR. */
static bool select_land_lor(SelCtx *ctx, const IrNode *fn, IrNode *node,
                            int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int64_t l_right = sel_new_label(ctx);
    int64_t l_end = sel_new_label(ctx);
    bool is_lor = node->kind == IR_LOR;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_TEST, make_vreg(vl, 1), make_vreg(vl, 1),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    /* LAND: left false -> result false (jump over right). LOR: left
     * true -> result true. */
    if (!sel_emit(ctx, ISEL_JCC, isel_operand_none(), make_label(l_right),
                  isel_operand_none(),
                  is_lor ? ISEL_COND_E : ISEL_COND_NE, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, 1),
                  make_imm(is_lor ? 1 : 0, 1, false), isel_operand_none(),
                  ISEL_COND_E, 0, false, NULL, node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(), make_label(l_end),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_right), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, 1), make_vreg(vr, 1),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_LABEL, make_label(l_end), isel_operand_none(),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

/* Scalar comparisons. str/str comparisons are selected as a documented
 * strcmp pseudo (byte-sequence lowering owned by 17a2); scalar types use
 * cmp + setcc with the signed/unsigned condition from the operand type. */
static bool select_compare(SelCtx *ctx, const IrNode *fn, IrNode *node,
                           int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int w;
    IselCond cond;
    const IrType *lt = node->u.binary.left != NULL
                           ? node->u.binary.left->type : NULL;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    if (lt != NULL && lt->kind == IRT_STR) {
        /* str comparison: the byte-sequence comparison pseudo; the
         * condition records the relational operator */
        return sel_emit(ctx, ISEL_STRCMP, make_vreg(v, 1),
                        make_vreg(vl, 8), make_vreg(vr, 8),
                        cond_for_ir_kind(node->kind, lt), 0, false, NULL,
                        node->id);
    }
    w = type_operand_width(lt);
    cond = cond_for_ir_kind(node->kind, lt);
    if (!sel_emit(ctx, ISEL_CMP, isel_operand_none(), make_vreg(vl, w),
                  make_vreg(vr, w), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_SETCC, make_vreg(v, 1), isel_operand_none(),
                    isel_operand_none(), cond, 0, false, NULL, node->id);
}

/* IR_SLICE_EQ: element-wise equality (pseudo; sequence owned by 17a2). */
static bool select_slice_eq(SelCtx *ctx, const IrNode *fn, IrNode *node,
                            int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int elem_size = 1;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    {
        const IrType *lt = node->u.binary.left != NULL
                               ? node->u.binary.left->type : NULL;
        if (lt != NULL && lt->kind == IRT_SLICE && lt->u.slice.elem != NULL) {
            elem_size = (int)lt->u.slice.elem->size;
            if (elem_size <= 0) {
                elem_size = 1;
            }
        }
    }
    return sel_emit(ctx, ISEL_SLICEEQ, make_vreg(v, 1), make_vreg(vl, 8),
                    make_vreg(vr, 8), ISEL_COND_E, elem_size, false, NULL,
                    node->id);
}

static bool select_select(SelCtx *ctx, const IrNode *fn, IrNode *node,
                          int64_t *out_vreg)
{
    int64_t vc = -1;
    int64_t vt = -1;
    int64_t ve = -1;
    int64_t v;
    int w;
    int64_t l_else = sel_new_label(ctx);
    int64_t l_end = sel_new_label(ctx);
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.select.cond, &vc)) {
        return false;
    }
    w = type_operand_width(node->type);
    if (!sel_emit(ctx, ISEL_TEST, make_vreg(vc, 1), make_vreg(vc, 1),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JCC, isel_operand_none(), make_label(l_else),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.select.then_value, &vt)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(vt, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_JMP, isel_operand_none(), make_label(l_end),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_LABEL, make_label(l_else), isel_operand_none(),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.select.else_value, &ve)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, w), make_vreg(ve, w),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_LABEL, make_label(l_end), isel_operand_none(),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

static bool select_call(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t callee_id = -1;
    int64_t v = -1;
    size_t i;
    bool void_ret = node->type != NULL && node->type->kind == IRT_VOID;
    if (node->u.call.callee != NULL) {
        callee_id = node->u.call.callee->id;
    }
    for (i = 0; i < node->u.call.nargs; i++) {
        int64_t va = -1;
        if (!select_value(ctx, fn, node->u.call.args[i], &va)) {
            return false;
        }
    }
    if (!void_ret) {
        v = sel_vreg_for_node(ctx, node->id);
        *out_vreg = v;
    }
    /* argument/return register placement is ABI-owned (17b2); the core
     * selects the call and records the result vreg when non-void */
    return sel_emit(ctx, ISEL_CALL,
                    void_ret ? isel_operand_none() : make_vreg(v, 8),
                    make_func(callee_id), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_len(SelCtx *ctx, const IrNode *fn, IrNode *node,
                       int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    const IrType *ot;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.unary.operand, &vo)) {
        return false;
    }
    ot = node->u.unary.operand != NULL
             ? node->u.unary.operand->type : NULL;
    if (ot != NULL && ot->kind == IRT_ARRAY) {
        /* constant extent */
        return sel_emit(ctx, ISEL_MOV, make_vreg(v, 8),
                        make_imm(ot->u.array.extent, 8, true),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    }
    /* slice/str: length field at offset 8 of the pair image */
    return sel_emit(ctx, ISEL_MOV, make_vreg(v, 8),
                    make_mem(vo, 8, 8), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_ptr(SelCtx *ctx, const IrNode *fn, IrNode *node,
                       int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    const IrType *ot;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.unary.operand, &vo)) {
        return false;
    }
    ot = node->u.unary.operand != NULL
             ? node->u.unary.operand->type : NULL;
    if (ot != NULL && ot->kind == IRT_ARRAY) {
        /* first element address; empty array -> null */
        if (ot->u.array.extent == 0) {
            return sel_emit(ctx, ISEL_MOV, make_vreg(v, 8),
                            make_imm(0, 8, true), isel_operand_none(),
                            ISEL_COND_E, 0, false, NULL, node->id);
        }
        return sel_emit(ctx, ISEL_LEA, make_vreg(v, 8),
                        make_mem(vo, 0, 8), isel_operand_none(),
                        ISEL_COND_E, 0, false, NULL, node->id);
    }
    /* slice/str: data field at offset 0 (empty -> null is a runtime
     * check, owned by 17c; the load is selected here) */
    return sel_emit(ctx, ISEL_MOV, make_vreg(v, 8),
                    make_mem(vo, 0, 8), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

static bool select_slice(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg)
{
    int64_t vb = -1;
    int64_t vs = -1;       /* start vreg, or -1 when omitted */
    int64_t ve = -1;       /* end vreg, or -1 when omitted */
    int64_t vdata = -1;
    int64_t vlen = -1;
    int64_t v;
    int elem_size = 1;
    const IrType *bt;
    const char *trap;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    bt = node->u.slice.base != NULL ? node->u.slice.base->type : NULL;
    if (bt != NULL && (bt->kind == IRT_SLICE || bt->kind == IRT_ARRAY)) {
        const IrType *elem = bt->kind == IRT_SLICE ? bt->u.slice.elem
                                                   : bt->u.array.elem;
        if (elem != NULL) {
            elem_size = (int)elem->size;
            if (elem_size <= 0) {
                elem_size = 1;
            }
        }
    }
    trap = bt != NULL && bt->kind == IRT_STR ? "AIC-R0808" : "AIC-R0807";
    /* evaluation order: base, then start, then end (contract sec. 5.3) */
    if (!select_value(ctx, fn, node->u.slice.base, &vb)) {
        return false;
    }
    if (node->u.slice.start != NULL) {
        if (!select_value(ctx, fn, node->u.slice.start, &vs)) {
            return false;
        }
    }
    if (node->u.slice.end != NULL) {
        if (!select_value(ctx, fn, node->u.slice.end, &ve)) {
            return false;
        }
    }
    /* data pointer from the pair image (offset 0) */
    vdata = sel_vreg_for_node(ctx, -1);
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(vdata, 8),
                  make_mem(vb, 0, 8), isel_operand_none(),
                  ISEL_COND_E, 0, false, NULL, node->id)) {
        return false;
    }
    if (vs >= 0) {
        /* data += start * elem_size (scale the offset in place) */
        if (elem_size != 1) {
            if (!sel_emit(ctx, ISEL_IMUL, make_vreg(vs, 8),
                          make_imm(elem_size, 8, false), isel_operand_none(),
                          ISEL_COND_E, 0, false, NULL, node->id)) {
                return false;
            }
        }
        if (!sel_emit(ctx, ISEL_ADD, make_vreg(vdata, 8),
                      make_vreg(vs, 8), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
    }
    if (ve >= 0) {
        vlen = sel_vreg_for_node(ctx, -1);
        if (!sel_emit(ctx, ISEL_MOV, make_vreg(vlen, 8),
                      make_vreg(ve, 8), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
        if (vs >= 0) {
            /* len = end - start */
            if (!sel_emit(ctx, ISEL_SUB, make_vreg(vlen, 8),
                          make_vreg(vs, 8), isel_operand_none(),
                          ISEL_COND_E, 0, false, NULL, node->id)) {
                return false;
            }
        }
    } else {
        /* end omitted -> the pair's length field at offset 8 */
        vlen = sel_vreg_for_node(ctx, -1);
        if (!sel_emit(ctx, ISEL_MOV, make_vreg(vlen, 8),
                      make_mem(vb, 8, 8), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
    }
    /* slice pair construction pseudo (pair image materialization owned
     * by 17b); bounds checks preserved */
    return sel_emit(ctx, ISEL_SLICE, make_vreg(v, 8),
                    make_vreg(vdata, 8), make_vreg(vlen, 8),
                    ISEL_COND_E, elem_size, false, trap, node->id);
}

static bool select_cast(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    int ws, wt;
    const IrType *st;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.cast_wrap.value, &vo)) {
        return false;
    }
    st = node->u.cast_wrap.value != NULL
             ? node->u.cast_wrap.value->type : NULL;
    ws = type_operand_width(st);
    wt = type_operand_width(node->type);
    if (node->type != NULL && node->type->kind == IRT_STR &&
        st != NULL && st->kind == IRT_SLICE) {
        /* u8[] -> str: UTF-8 validation pseudo (trap AIC-R0806);
         * the validated pair address is the result */
        return sel_emit(ctx, ISEL_UTF8, make_vreg(v, 8),
                        make_vreg(vo, 8), isel_operand_none(),
                        ISEL_COND_E, 0, false, "AIC-R0806", node->id);
    }
    if (wt > ws && ws > 0) {
        /* widening: sign- or zero-extend by source signedness */
        IselOpcode op = type_is_signed(st) ? ISEL_MOVSX : ISEL_MOVZX;
        return sel_emit(ctx, op, make_vreg(v, wt), make_vreg(vo, ws),
                        isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                        node->id);
    }
    if (ws == 0) {
        ws = 8;
    }
    if (wt == 0) {
        wt = 8;
    }
    /* same-width or narrowing: bit-preserving move; narrowing takes the
     * low bits (checked conversion obligations remain on the IR node and
     * are carried by 17c2) */
    return sel_emit(ctx, ISEL_MOV, make_vreg(v, wt), make_vreg(vo, ws),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

static bool select_wrap(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t vo = -1;
    int64_t v;
    int ws, wt;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.cast_wrap.value, &vo)) {
        return false;
    }
    ws = type_operand_width(node->u.cast_wrap.value != NULL
                                ? node->u.cast_wrap.value->type : NULL);
    wt = type_operand_width(node->type);
    if (ws == 0) {
        ws = 8;
    }
    if (wt == 0) {
        wt = 8;
    }
    /* wrapping/truncating conversion: low bits (modulo 2^width,
     * never checked, never traps) */
    return sel_emit(ctx, ISEL_MOV, make_vreg(v, wt), make_vreg(vo, ws),
                    isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                    node->id);
}

static bool select_ptr_arith(SelCtx *ctx, const IrNode *fn, IrNode *node,
                             int64_t *out_vreg)
{
    int64_t vp = -1;
    int64_t vo = -1;
    int64_t v;
    int elem_size = 1;
    const IrType *pt;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.ptr_arith.ptr, &vp)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.ptr_arith.offset, &vo)) {
        return false;
    }
    pt = node->u.ptr_arith.ptr != NULL
             ? node->u.ptr_arith.ptr->type : NULL;
    if (pt != NULL && pt->kind == IRT_PTR && pt->u.ptr.elem != NULL) {
        elem_size = (int)pt->u.ptr.elem->size;
        if (elem_size <= 0) {
            elem_size = 1;
        }
    }
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, 8), make_vreg(vp, 8),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (elem_size != 1) {
        /* offset *= scale (in place), then v += offset */
        if (!sel_emit(ctx, ISEL_IMUL, make_vreg(vo, 8),
                      make_imm(elem_size, 8, false), isel_operand_none(),
                      ISEL_COND_E, 0, false, NULL, node->id)) {
            return false;
        }
    }
    return sel_emit(ctx, node->kind == IR_PTR_ADD ? ISEL_ADD : ISEL_SUB,
                    make_vreg(v, 8), make_vreg(vo, 8), isel_operand_none(),
                    ISEL_COND_E, 0, false, "AIC-R0816", node->id);
}

static bool select_ptr_diff(SelCtx *ctx, const IrNode *fn, IrNode *node,
                            int64_t *out_vreg)
{
    int64_t vl = -1;
    int64_t vr = -1;
    int64_t v;
    int elem_size = 1;
    const IrType *lt;
    v = sel_vreg_for_node(ctx, node->id);
    *out_vreg = v;
    if (!select_value(ctx, fn, node->u.binary.left, &vl)) {
        return false;
    }
    if (!select_value(ctx, fn, node->u.binary.right, &vr)) {
        return false;
    }
    lt = node->u.binary.left != NULL ? node->u.binary.left->type : NULL;
    if (lt != NULL && lt->kind == IRT_PTR && lt->u.ptr.elem != NULL) {
        elem_size = (int)lt->u.ptr.elem->size;
        if (elem_size <= 0) {
            elem_size = 1;
        }
    }
    /* byte difference: v = left - right; the divisibility/scale pseudo
     * carries the element size and the AIC-R0810 obligation */
    if (!sel_emit(ctx, ISEL_MOV, make_vreg(v, 8), make_vreg(vl, 8),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    if (!sel_emit(ctx, ISEL_SUB, make_vreg(v, 8), make_vreg(vr, 8),
                  isel_operand_none(), ISEL_COND_E, 0, false, NULL,
                  node->id)) {
        return false;
    }
    return sel_emit(ctx, ISEL_PTRDIFF, make_vreg(v, 8),
                    isel_operand_none(), make_imm(elem_size, 8, false),
                    ISEL_COND_E, 0, false, "AIC-R0810", node->id);
}

static bool select_zero(SelCtx *ctx, const IrNode *fn, IrNode *node,
                        int64_t *out_vreg)
{
    int64_t va = -1;
    int64_t size = 1;
    const IrType *ot;
    (void)out_vreg;   /* IR_ZERO produces no value */
    if (!select_value(ctx, fn, node->u.unary.operand, &va)) {
        return false;
    }
    ot = node->u.unary.operand != NULL
             ? node->u.unary.operand->type : NULL;
    if (ot != NULL && ot->size > 0) {
        size = ot->size;
    }
    return sel_emit(ctx, ISEL_REP_STOSB, make_mem(va, 0, 8),
                    make_imm(size, 8, true), isel_operand_none(),
                    ISEL_COND_E, 0, false, NULL, node->id);
}

/* IR_STORE handled as a statement-like value node with no result vreg. */
static bool select_store_as_value(SelCtx *ctx, const IrNode *fn, IrNode *node,
                                  int64_t *out_vreg)
{
    (void)out_vreg;
    return select_store(ctx, fn, node, out_vreg);
}

static bool select_zero_as_value(SelCtx *ctx, const IrNode *fn, IrNode *node,
                                 int64_t *out_vreg)
{
    (void)out_vreg;
    return select_zero(ctx, fn, node, out_vreg);
}

static bool select_value(SelCtx *ctx, const IrNode *fn, IrNode *node,
                         int64_t *out_vreg)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
    case IR_INT:
    case IR_BOOL:
    case IR_NULL:
    case IR_STR:
    case IR_ENUM_VAL:
        return select_const_value(ctx, fn, node, out_vreg);
    case IR_LOCAL:
        return select_local(ctx, fn, node, out_vreg);
    case IR_GLOBAL:
        return select_global(ctx, fn, node, out_vreg);
    case IR_FIELD_ADDR:
        return select_field_addr(ctx, fn, node, out_vreg);
    case IR_INDEX_ADDR:
        return select_index_addr(ctx, fn, node, out_vreg);
    case IR_DEREF:
        return select_deref(ctx, fn, node, out_vreg);
    case IR_LOAD:
        return select_load(ctx, fn, node, out_vreg);
    case IR_STORE:
        return select_store_as_value(ctx, fn, node, out_vreg);
    case IR_ADD:
    case IR_SUB:
    case IR_MUL:
    case IR_BAND:
    case IR_BOR:
    case IR_BXOR:
        return select_binary_arith(ctx, fn, node, out_vreg);
    case IR_DIV:
    case IR_MOD:
        return select_div_mod(ctx, fn, node, out_vreg);
    case IR_NEG:
        return select_neg(ctx, fn, node, out_vreg);
    case IR_SHL:
    case IR_SHR:
        return select_shift(ctx, fn, node, out_vreg);
    case IR_BNOT:
        return select_bnot(ctx, fn, node, out_vreg);
    case IR_LNOT:
        return select_lnot(ctx, fn, node, out_vreg);
    case IR_LAND:
    case IR_LOR:
        return select_land_lor(ctx, fn, node, out_vreg);
    case IR_EQ:
    case IR_NE:
    case IR_LT:
    case IR_LE:
    case IR_GT:
    case IR_GE:
        return select_compare(ctx, fn, node, out_vreg);
    case IR_SLICE_EQ:
        return select_slice_eq(ctx, fn, node, out_vreg);
    case IR_SELECT:
        return select_select(ctx, fn, node, out_vreg);
    case IR_CALL:
        return select_call(ctx, fn, node, out_vreg);
    case IR_LEN:
        return select_len(ctx, fn, node, out_vreg);
    case IR_PTR:
        return select_ptr(ctx, fn, node, out_vreg);
    case IR_SLICE:
        return select_slice(ctx, fn, node, out_vreg);
    case IR_CAST:
        return select_cast(ctx, fn, node, out_vreg);
    case IR_WRAP:
        return select_wrap(ctx, fn, node, out_vreg);
    case IR_PTR_ADD:
    case IR_PTR_SUB:
        return select_ptr_arith(ctx, fn, node, out_vreg);
    case IR_PTR_DIFF:
        return select_ptr_diff(ctx, fn, node, out_vreg);
    case IR_ZERO:
        return select_zero_as_value(ctx, fn, node, out_vreg);
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Function selection
 * ------------------------------------------------------------------------- */

static bool select_function(SelCtx *ctx, IrNode *fn)
{
    char buf[512];
    size_t n = 0;
    size_t i;
    const char *name = fn->u.function.name != NULL
                           ? fn->u.function.name : "?";
    n = (size_t)snprintf(buf, sizeof(buf), "function %s", name);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    for (i = 0; i < fn->u.function.nparams; i++) {
        const IrParam *p = &fn->u.function.params[i];
        const char *pn = p->name != NULL ? p->name : "?";
        if (n < sizeof(buf) - 1) {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %s", pn);
            if (n >= sizeof(buf)) {
                n = sizeof(buf) - 1;
            }
        }
    }
    if (!sel_comment(ctx, buf, fn->id)) {
        return false;
    }
    if (fn->u.function.body != NULL) {
        if (!select_block(ctx, fn, fn->u.function.body)) {
            return false;
        }
    }
    return true;
}

static bool select_module(SelCtx *ctx, IrNode *module)
{
    char buf[256];
    size_t i;
    const char *name = module->u.module.name != NULL
                           ? module->u.module.name : "?";
    (void)snprintf(buf, sizeof(buf), "module %s", name);
    if (!sel_comment(ctx, buf, module->id)) {
        return false;
    }
    for (i = 0; i < module->u.module.ndecls; i++) {
        IrNode *decl = module->u.module.decls[i];
        bool ok = true;
        switch (decl->kind) {
        case IR_GLOBAL_CONST:
            ok = select_global_const(ctx, decl);
            break;
        case IR_GLOBAL_VAR:
            ok = select_global_var(ctx, decl);
            break;
        case IR_STRUCT_DECL:
            ok = select_struct_decl(ctx, decl);
            break;
        case IR_ENUM_DECL:
            ok = select_enum_decl(ctx, decl);
            break;
        case IR_FUNCTION:
            ok = select_function(ctx, decl);
            break;
        default:
            ok = true;
            break;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

IselStatus isel_select(const IrBuild *build, IselOutput **out)
{
    SelCtx ctx;
    IselOutput *o;
    size_t i;
    int64_t nnodes = build != NULL ? (int64_t)build->nnodes : 0;
    if (build == NULL || out == NULL) {
        return ISEL_OOM;
    }
    o = (IselOutput *)calloc(1, sizeof(*o));
    if (o == NULL) {
        return ISEL_OOM;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.build = build;
    ctx.out = o;
    ctx.vreg_of = (int64_t *)malloc(
        (size_t)(nnodes > 0 ? nnodes : 1) * sizeof(int64_t));
    ctx.break_label_of = (int64_t *)malloc(
        (size_t)(nnodes > 0 ? nnodes : 1) * sizeof(int64_t));
    ctx.continue_label_of = (int64_t *)malloc(
        (size_t)(nnodes > 0 ? nnodes : 1) * sizeof(int64_t));
    if (ctx.vreg_of == NULL || ctx.break_label_of == NULL ||
        ctx.continue_label_of == NULL) {
        free(ctx.vreg_of);
        free(ctx.break_label_of);
        free(ctx.continue_label_of);
        free(o);
        return ISEL_OOM;
    }
    for (i = 0; i < (size_t)nnodes; i++) {
        ctx.vreg_of[i] = -1;
        ctx.break_label_of[i] = -1;
        ctx.continue_label_of[i] = -1;
    }
    for (i = 0; i < build->nmodules; i++) {
        if (!select_module(&ctx, build->modules[i])) {
            /* selection failure: OOM, or an internal defect (a node
             * kind the table does not cover). Both are surfaced as
             * ISEL_OOM with nothing owned: a truncated instruction
             * stream must never be returned. */
            if (!o->oom) {
                o->oom = true;
            }
            break;
        }
    }
    free(ctx.vreg_of);
    free(ctx.break_label_of);
    free(ctx.continue_label_of);
    if (o->oom) {
        isel_output_free(o);
        *out = NULL;
        return ISEL_OOM;
    }
    *out = o;
    return ISEL_OK;
}

void isel_output_free(IselOutput *out)
{
    size_t i;
    if (out == NULL) {
        return;
    }
    for (i = 0; i < out->count; i++) {
        if (out->insns[i].op == ISEL_COMMENT) {
            free((void *)out->insns[i].note);
        }
    }
    free(out->insns);
    free(out);
}

size_t isel_output_count(const IselOutput *out)
{
    return out != NULL ? out->count : 0;
}

const IselInsn *isel_output_insn(const IselOutput *out, size_t i)
{
    if (out == NULL || i >= out->count) {
        return NULL;
    }
    return &out->insns[i];
}

/* ---------------------------------------------------------------------------
 * Assembly dump (byte-deterministic)
 * ------------------------------------------------------------------------- */

/* DiagBuf growth helpers (DiagBuf is the public growable byte buffer of
 * the diag package; ir_dump.c grows it the same way). */

static bool s_reserve(DiagBuf *buf, size_t extra)
{
    size_t need;
    size_t cap;
    if (buf->oom) {
        return false;
    }
    if (extra > (size_t)-1 - buf->len - 1) {
        buf->oom = true;
        return false;
    }
    need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return true;
    }
    cap = buf->cap ? buf->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    {
        char *p = (char *)realloc(buf->data, cap);
        if (p == NULL) {
            buf->oom = true;
            return false;
        }
        buf->data = p;
        buf->cap = cap;
    }
    return true;
}

static bool s_append_n(DiagBuf *buf, const char *s, size_t n)
{
    if (!s_reserve(buf, n)) {
        return false;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return true;
}

static bool s_append_cstr(DiagBuf *buf, const char *s)
{
    return s_append_n(buf, s, strlen(s));
}

static bool s_printf(DiagBuf *buf, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf->oom = true;
        return false;
    }
    if ((size_t)n < sizeof(tmp)) {
        return s_append_n(buf, tmp, (size_t)n);
    }
    {
        char *big = (char *)malloc((size_t)n + 1);
        if (big == NULL) {
            buf->oom = true;
            return false;
        }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        s_append_n(buf, big, (size_t)n);
        free(big);
        return !buf->oom;
    }
}

/* Opcodes whose mnemonic carries a width suffix (b/w/l/q). LEA, MOVZX,
 * MOVSX, SETCC, JCC, and the composite pseudo-ops have fixed or no
 * suffixes. */
static bool opcode_has_width_suffix(IselOpcode op)
{
    switch (op) {
    case ISEL_MOV:
    case ISEL_ADD:
    case ISEL_SUB:
    case ISEL_IMUL:
    case ISEL_IDIV:
    case ISEL_NEG:
    case ISEL_AND:
    case ISEL_OR:
    case ISEL_XOR:
    case ISEL_NOT:
    case ISEL_SHL:
    case ISEL_SHR:
    case ISEL_SAR:
    case ISEL_CMP:
    case ISEL_TEST:
        return true;
    default:
        return false;
    }
}

static bool dump_operand(DiagBuf *b, const IselOperand *op)
{
    switch (op->kind) {
    case ISEL_OP_NONE:
        return true;
    case ISEL_OP_VREG:
        return s_printf(b, "r%lld", (long long)op->vreg);
    case ISEL_OP_IMM:
        if (op->is_unsigned) {
            return s_printf(b, "$%llu",
                            (unsigned long long)op->imm);
        }
        return s_printf(b, "$%lld", (long long)op->imm);
    case ISEL_OP_SLOT:
        return s_printf(b, "slot%lld", (long long)op->id);
    case ISEL_OP_GLOBAL:
        return s_printf(b, "g%lld", (long long)op->id);
    case ISEL_OP_STR:
        return s_printf(b, ".Lstr%lld", (long long)op->id);
    case ISEL_OP_FUNC:
        return s_printf(b, "fn%lld", (long long)op->id);
    case ISEL_OP_LABEL:
        return s_printf(b, "L%lld", (long long)op->id);
    case ISEL_OP_MEM:
        if (op->imm == 0) {
            return s_printf(b, "[r%lld]", (long long)op->vreg);
        }
        return s_printf(b, "[r%lld%+lld]", (long long)op->vreg,
                        (long long)op->imm);
    default:
        return false;
    }
}

static bool dump_insn_line(DiagBuf *b, const IselInsn *insn)
{
    if (insn->op == ISEL_COMMENT) {
        return s_printf(b, "# %s\n",
                        insn->note != NULL ? insn->note : "");
    }
    if (insn->op == ISEL_LABEL) {
        return s_printf(b, "L%lld:\n", (long long)insn->dst.id);
    }
    if (!s_append_cstr(b, "  ")) {
        return false;
    }
    /* SETCC/JCC render as "set"+cond / "j"+cond directly (no generic
     * "setcc"/"jcc" prefix) */
    if (insn->op == ISEL_SETCC) {
        if (!s_append_cstr(b, "set") ||
            !s_append_cstr(b, isel_cond_text(insn->cond))) {
            return false;
        }
    } else if (insn->op == ISEL_JCC) {
        if (!s_append_cstr(b, "j") ||
            !s_append_cstr(b, isel_cond_text(insn->cond))) {
            return false;
        }
    } else if (!s_append_cstr(b, isel_opcode_text(insn->op))) {
        return false;
    }
    /* mnemonic suffix from the destination operand width where the
     * opcode has width-specific forms */
    if (opcode_has_width_suffix(insn->op)) {
        int w = insn->dst.width > 0 ? insn->dst.width : insn->src1.width;
        const char *suffix = "?";
        switch (w) {
        case 1: suffix = "b"; break;
        case 2: suffix = "w"; break;
        case 4: suffix = "l"; break;
        case 8: suffix = "q"; break;
        default: break;
        }
        if (!s_append_cstr(b, suffix)) {
            return false;
        }
    }
    if (insn->op == ISEL_LEA && insn->scale > 0) {
        if (!s_append_cstr(b, " (scaled)")) {
            return false;
        }
    }
    /* calls print the callee symbol; the result vreg is a trailing
     * annotation ("-> rN") so the line reads as assembly */
    if (insn->op == ISEL_CALL) {
        if (!s_append_cstr(b, " ")) {
            return false;
        }
        if (!dump_operand(b, &insn->src1)) {
            return false;
        }
        if (insn->dst.kind == ISEL_OP_VREG) {
            if (!s_printf(b, " -> r%lld", (long long)insn->dst.vreg)) {
                return false;
            }
        }
        if (!s_printf(b, "   # ir%lld", (long long)insn->ir_node_id)) {
            return false;
        }
        return s_append_cstr(b, "\n");
    }
    /* operands: print the non-NONE operands in order, comma-separated
     * (no leading/trailing separators) */
    if (insn->dst.kind != ISEL_OP_NONE ||
        insn->src1.kind != ISEL_OP_NONE ||
        insn->src2.kind != ISEL_OP_NONE) {
        bool need_sep = false;
        const IselOperand *ops[3] = { &insn->dst, &insn->src1, &insn->src2 };
        size_t oi;
        if (!s_append_cstr(b, " ")) {
            return false;
        }
        for (oi = 0; oi < 3; oi++) {
            if (ops[oi]->kind == ISEL_OP_NONE) {
                continue;
            }
            if (need_sep) {
                if (!s_append_cstr(b, ", ")) {
                    return false;
                }
            }
            if (!dump_operand(b, ops[oi])) {
                return false;
            }
            need_sep = true;
        }
    }
    /* trace + obligation annotation */
    if (!s_printf(b, "   # ir%lld", (long long)insn->ir_node_id)) {
        return false;
    }
    if (insn->trap != NULL) {
        if (!s_printf(b, " trap=%s", insn->trap)) {
            return false;
        }
    }
    if (insn->mod) {
        if (!s_append_cstr(b, " mod")) {
            return false;
        }
    }
    return s_append_cstr(b, "\n");
}

bool isel_asm_dump(const IselOutput *sel, DiagBuf *out)
{
    size_t i;
    if (sel == NULL || out == NULL) {
        return false;
    }
    if (!s_append_cstr(out, "; AI-Co isel_core assembly dump "
                            "(WP-M0-17a1, deterministic)\n")) {
        return false;
    }
    if (!s_printf(out, "; insns=%zu vregs=%lld labels=%lld\n",
                  sel->count, (long long)sel->next_vreg,
                  (long long)sel->next_label)) {
        return false;
    }
    for (i = 0; i < sel->count; i++) {
        if (!dump_insn_line(out, &sel->insns[i])) {
            return false;
        }
    }
    return true;
}
