/* bootstrap/src/backend/call.c
 *
 * AI-Co Stage-0 x86-64 register allocation and call emission
 * (WP-M0-17b2).
 *
 * Implements the obligations of call.h on top of the 17b1 framed
 * instruction stream: deterministic simple register allocation
 * (spill-everything with a two-scratch-register lowering) and call
 * emission per spec sec. 15.7 (RCX/RDX/R8/R9, shadow space, 16-byte
 * alignment, RAX return). See call.h for the normative rules (1-5),
 * the scope boundary, and the ownership model.
 *
 * Determinism is structural: the pass walks the framed stream in
 * emission order and the IR's deterministic arrays; every output is a
 * pure function of the framed stream and the IR. Identical inputs
 * always yield identical physical streams and identical dump bytes
 * (spec sec. 14.2).
 */
#include "call.h"

#include "../diag/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Microsoft x64 convention's caller-reserved shadow space. */
#define CALL_SHADOW_BYTES 32

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static int64_t align_up_i64(int64_t v, int64_t a)
{
    if (a <= 1) {
        return v;
    }
    return (v + a - 1) / a * a;
}

/* Composite types are copied by value into parameter slots (address-
 * resident arguments; IR contract sec. 5.3 / spec sec. 7.2, 12.1). */
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

/* Look up the IR function node for a function-comment ir_node_id. */
static const IrNode *lookup_function(const IrBuild *build, int64_t node_id)
{
    if (build == NULL || node_id < 0 || (size_t)node_id >= build->nnodes) {
        return NULL;
    }
    if (build->nodes[node_id]->kind != IR_FUNCTION) {
        return NULL;
    }
    return build->nodes[node_id];
}

/* ---------------------------------------------------------------------------
 * Register names
 * ------------------------------------------------------------------------- */

static const char *const kX64RegNames[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

const char *x64_reg_text(X64Reg reg)
{
    if (reg < 0 || (size_t)reg >= sizeof(kX64RegNames) / sizeof(kX64RegNames[0])) {
        return "?";
    }
    return kX64RegNames[reg];
}

/* Width-qualified register name (al/ax/eax/rax, r10b/r10w/r10d/r10, ...). */
static const char *reg_name(X64Reg reg, int width)
{
    static const char *const kBytes[16] = {
        "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
        "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
    };
    static const char *const kWords[16] = {
        "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
        "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
    };
    static const char *const kDwords[16] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
    };
    if (reg < 0 || (size_t)reg >= 16) {
        return "?";
    }
    switch (width) {
    case 1: return kBytes[reg];
    case 2: return kWords[reg];
    case 4: return kDwords[reg];
    default: return kX64RegNames[reg];
    }
}

/* ---------------------------------------------------------------------------
 * Operand builders
 * ------------------------------------------------------------------------- */

CallOperand call_operand_none(void)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = CALL_OPR_NONE;
    return op;
}

static CallOperand cop_reg(X64Reg reg, int width)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = CALL_OPR_REG;
    op.id = (int64_t)reg;
    op.width = width;
    return op;
}

static CallOperand cop_imm(int64_t v, int width, bool is_unsigned)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = CALL_OPR_IMM;
    op.imm = v;
    op.width = width;
    op.is_unsigned = is_unsigned;
    return op;
}

static CallOperand cop_mem(X64Reg base, int64_t disp, int width)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = CALL_OPR_MEM;
    op.id = (int64_t)base;
    op.imm = disp;
    op.width = width;
    return op;
}

static CallOperand cop_symbol(CallOperandKind kind, int64_t id)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = kind;
    op.id = id;
    op.width = 8;
    return op;
}

/* ---------------------------------------------------------------------------
 * Output helpers
 * ------------------------------------------------------------------------- */

static bool emit_insn(CallOutput *co, CallOp op, IselOpcode isel,
                      CallOperand dst, CallOperand src1, CallOperand src2,
                      IselCond cond, int scale, bool mod, int64_t imm,
                      int64_t ir_node_id)
{
    CallInsn *p;
    if (co->count == co->cap) {
        size_t ncap = co->cap == 0 ? 32 : co->cap * 2;
        CallInsn *q = (CallInsn *)realloc(co->insns, ncap * sizeof(CallInsn));
        if (q == NULL) {
            co->oom = true;
            return false;
        }
        co->insns = q;
        co->cap = ncap;
    }
    p = &co->insns[co->count];
    memset(p, 0, sizeof(*p));
    p->op = op;
    p->isel = isel;
    p->dst = dst;
    p->src1 = src1;
    p->src2 = src2;
    p->cond = cond;
    p->scale = scale;
    p->mod = mod;
    p->imm = imm;
    p->ir_node_id = ir_node_id;
    co->count++;
    return true;
}

static bool emit_body_insn(CallOutput *co, IselOpcode isel,
                           CallOperand dst, CallOperand src1,
                           CallOperand src2, IselCond cond, int scale,
                           bool mod, int64_t ir_node_id)
{
    return emit_insn(co, CALL_OP_BODY, isel, dst, src1, src2, cond, scale,
                     mod, 0, ir_node_id);
}

static bool emit_pseudo(CallOutput *co, const IselInsn *body)
{
    CallInsn *p;
    if (co->count == co->cap) {
        size_t ncap = co->cap == 0 ? 32 : co->cap * 2;
        CallInsn *q = (CallInsn *)realloc(co->insns, ncap * sizeof(CallInsn));
        if (q == NULL) {
            co->oom = true;
            return false;
        }
        co->insns = q;
        co->cap = ncap;
    }
    p = &co->insns[co->count];
    memset(p, 0, sizeof(*p));
    p->op = CALL_OP_PSEUDO;
    p->isel = body->op;
    p->pseudo = *body;   /* shallow copy; note/trap pointers stay borrowed */
    p->ir_node_id = body->ir_node_id;
    co->count++;
    return true;
}

/* ---------------------------------------------------------------------------
 * The vreg -> node map
 *
 * isel_core assigns each value node exactly one virtual register (the
 * first vreg allocated for that node). Instructions written by a node
 * carry the node's ir_node_id; the node's own vreg is the minimum vreg
 * number among instructions with a VREG destination for that id,
 * because the node's own vreg is always allocated before any anonymous
 * intermediate vregs of the same node (the only node that allocates
 * anonymous vregs is IR_SLICE, whose own vreg is allocated first).
 * ------------------------------------------------------------------------- */

static bool build_vreg_map(const FrameOutput *fr, const IrBuild *build,
                           int64_t **out_map, size_t *out_cap)
{
    int64_t *map;
    size_t cap = build != NULL ? build->nnodes : 0;
    size_t i;
    map = (int64_t *)malloc((cap > 0 ? cap : 1) * sizeof(int64_t));
    if (map == NULL) {
        return false;
    }
    for (i = 0; i < cap; i++) {
        map[i] = -1;
    }
    for (i = 0; i < fr->count; i++) {
        const FrameInsn *fi = &fr->insns[i];
        if (fi->op != FRAME_OP_BODY) {
            continue;
        }
        if (fi->body.dst.kind == ISEL_OP_VREG &&
            fi->body.ir_node_id >= 0 &&
            (size_t)fi->body.ir_node_id < cap) {
            int64_t v = fi->body.dst.vreg;
            if (map[fi->body.ir_node_id] < 0 ||
                v < map[fi->body.ir_node_id]) {
                map[fi->body.ir_node_id] = v;
            }
        }
    }
    *out_map = map;
    *out_cap = cap;
    return true;
}

/* ---------------------------------------------------------------------------
 * Region analysis
 * ------------------------------------------------------------------------- */

typedef struct RegionStats {
    int64_t max_vreg;        /* highest vreg number seen, or -1 */
    size_t nslice;           /* ISEL_SLICE count (16-byte pair temps) */
    bool has_calls;
    size_t max_stack_args;   /* max args beyond the 4 register args */
    bool sees_rep_movsb;
    bool sees_rep_stosb;
    bool has_composite_param;
} RegionStats;

static void stats_reset(RegionStats *st)
{
    memset(st, 0, sizeof(*st));
    st->max_vreg = -1;
}

static void stats_operand(RegionStats *st, const IselOperand *op)
{
    if (op == NULL) {
        return;
    }
    if (op->kind == ISEL_OP_VREG && op->vreg > st->max_vreg) {
        st->max_vreg = op->vreg;
    }
    if (op->kind == ISEL_OP_MEM && op->vreg >= 0 && op->vreg > st->max_vreg) {
        st->max_vreg = op->vreg;
    }
}

/* Scan the framed region [start, end) and the IR for the call plan
 * inputs. `fn` is the region's IR function node (non-NULL). */
static void analyze_region(const FrameOutput *fr, size_t start, size_t end,
                           const IrNode *fn, const IrBuild *build,
                           RegionStats *st)
{
    size_t i;
    size_t p;
    stats_reset(st);
    for (p = 0; p < fn->u.function.nparams; p++) {
        if (type_is_composite(fn->u.function.slots[p]->type)) {
            st->has_composite_param = true;
        }
    }
    for (i = start; i < end; i++) {
        const FrameInsn *fi = &fr->insns[i];
        const IrNode *call_node;
        if (fi->op != FRAME_OP_BODY) {
            continue;
        }
        stats_operand(st, &fi->body.dst);
        stats_operand(st, &fi->body.src1);
        stats_operand(st, &fi->body.src2);
        if (fi->body.op == ISEL_SLICE) {
            st->nslice++;
        }
        if (fi->body.op == ISEL_REP_MOVSB) {
            st->sees_rep_movsb = true;
        }
        if (fi->body.op == ISEL_REP_STOSB) {
            st->sees_rep_stosb = true;
        }
        if (fi->body.op != ISEL_CALL) {
            continue;
        }
        st->has_calls = true;
        call_node = (fi->body.ir_node_id >= 0 &&
                     (size_t)fi->body.ir_node_id < build->nnodes)
                        ? build->nodes[fi->body.ir_node_id] : NULL;
        if (call_node == NULL) {
            continue;
        }
        if (call_node->kind == IR_CALL) {
            if (call_node->u.call.nargs > 4 &&
                call_node->u.call.nargs - 4 > st->max_stack_args) {
                st->max_stack_args = call_node->u.call.nargs - 4;
            }
        } else if (call_node->kind == IR_CALL_TERM) {
            if (call_node->u.call_term.nargs > 4 &&
                call_node->u.call_term.nargs - 4 > st->max_stack_args) {
                st->max_stack_args = call_node->u.call_term.nargs - 4;
            }
        }
    }
}

/* Compute the per-function plan from the region stats and the 17b1
 * layout. `layout` may be NULL for a bodyless function. */
static void compute_plan(CallFunction *cf, const FrameLayout *layout,
                         const RegionStats *st)
{
    int64_t base;
    memset(cf, 0, sizeof(*cf));
    cf->frame_size = layout != NULL ? layout->frame_size : 0;
    cf->nvregs = st->max_vreg >= 0 ? st->max_vreg + 1 : 0;
    cf->nslice = st->nslice;
    cf->has_calls = st->has_calls;
    cf->max_stack_args = st->max_stack_args;
    cf->saves_rdi = st->sees_rep_movsb || st->sees_rep_stosb ||
                    st->has_composite_param;
    cf->saves_rsi = st->sees_rep_movsb || st->has_composite_param;
    cf->spill_bytes = 8 * cf->nvregs;
    cf->pair_bytes = 16 * (int64_t)cf->nslice;
    cf->shadow_bytes = cf->has_calls ? CALL_SHADOW_BYTES : 0;
    cf->stackarg_bytes = 8 * (int64_t)cf->max_stack_args;
    cf->saved_bytes = 8 * (int64_t)(cf->saves_rdi + cf->saves_rsi);
    base = cf->frame_size + cf->spill_bytes + cf->pair_bytes +
           cf->shadow_bytes + cf->stackarg_bytes;
    cf->total = align_up_i64(base, 16);
    /* keep RSP 16-aligned at call sites after the callee-saved pushes
     * (an odd push count needs 8 extra bytes of reservation) */
    if ((cf->total + cf->saved_bytes) % 16 != 0) {
        cf->total += 8;
    }
}

/* ---------------------------------------------------------------------------
 * Lowering context
 * ------------------------------------------------------------------------- */

typedef struct CallCtx {
    const IrBuild *build;
    const FrameOutput *fr;
    CallOutput *out;
    const int64_t *vreg_of;    /* node id -> vreg, or -1 */
    size_t vreg_cap;
    const FrameLayout *layout; /* current function's 17b1 layout */
    const IrNode *fn;          /* current IR function node */
    CallFunction cf_storage;   /* current plan (owned by ctx, outlives
                                * the marker block) */
    const CallFunction *cf;    /* == &cf_storage */
    size_t slice_count;        /* ISEL_SLICE counter for pair temps */
    bool epilogue_pending;     /* the framed stream's MOV_RSP_RBP /
                                * POP_RBP epilogue steps were seen and
                                * deferred until the ISEL_RET body */
} CallCtx;

/* The spill slot for vreg v: [rbp - frame_size - 8*(v+1)]. */
static CallOperand spill_mem(const CallCtx *ctx, int64_t vreg)
{
    return cop_mem(X64_REG_RBP,
                   -(ctx->cf->frame_size + 8 * (vreg + 1)), 8);
}

/* The pair-image temp for the k-th ISEL_SLICE of the current function:
 * [rbp - frame_size - spill_bytes - 16*(k+1)]. */
static CallOperand pair_mem(const CallCtx *ctx, size_t k, int64_t disp)
{
    int64_t off = ctx->cf->frame_size + ctx->cf->spill_bytes +
                  16 * (int64_t)(k + 1);
    return cop_mem(X64_REG_RBP, -(off) + disp, 8);
}

/* Look up the vreg for a call argument node. Defensive fallback: the
 * node id itself (deterministic; unreachable on verified IR). */
static int64_t vreg_of_node(const CallCtx *ctx, const IrNode *node)
{
    if (node != NULL && node->id >= 0 && (size_t)node->id < ctx->vreg_cap &&
        ctx->vreg_of[node->id] >= 0) {
        return ctx->vreg_of[node->id];
    }
    return node != NULL ? node->id : -1;
}

/* ---------------------------------------------------------------------------
 * Operand lowering
 * ------------------------------------------------------------------------- */

/* Convert a source operand to its physical form, loading vreg values /
 * memory bases into `scratch` first. Returns false on allocation
 * failure. `ir_id` is the owning instruction's IR node id (trace). */
static bool lower_source(CallCtx *ctx, const IselOperand *op,
                         X64Reg scratch, CallOperand *out, int64_t ir_id)
{
    if (op == NULL) {
        *out = call_operand_none();
        return true;
    }
    switch (op->kind) {
    case ISEL_OP_NONE:
        *out = call_operand_none();
        return true;
    case ISEL_OP_VREG:
        if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(scratch, 8),
                            spill_mem(ctx, op->vreg), call_operand_none(),
                            ISEL_COND_E, 0, false, ir_id)) {
            return false;
        }
        *out = cop_reg(scratch, op->width > 0 ? op->width : 8);
        return true;
    case ISEL_OP_IMM:
        *out = cop_imm(op->imm, op->width, op->is_unsigned);
        return true;
    case ISEL_OP_MEM:
        if (op->vreg == FRAME_BASE_VREG) {
            *out = cop_mem(X64_REG_RBP, op->imm, op->width);
            return true;
        }
        if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(scratch, 8),
                            spill_mem(ctx, op->vreg), call_operand_none(),
                            ISEL_COND_E, 0, false, ir_id)) {
            return false;
        }
        *out = cop_mem(scratch, op->imm, op->width);
        return true;
    case ISEL_OP_SLOT:
        /* defensive: frame_build rewrites every slot on verified IR; a
         * residual slot resolves against the current layout */
        if (ctx->layout != NULL) {
            const FrameSlotLayout *sl =
                frame_layout_slot(ctx->layout, op->id);
            if (sl != NULL) {
                *out = cop_mem(X64_REG_RBP, sl->offset, op->width);
                return true;
            }
        }
        *out = cop_mem(X64_REG_RBP, 0, op->width);
        return true;
    case ISEL_OP_GLOBAL:
        *out = cop_symbol(CALL_OPR_GLOBAL, op->id);
        return true;
    case ISEL_OP_STR:
        *out = cop_symbol(CALL_OPR_STR, op->id);
        return true;
    case ISEL_OP_FUNC:
        *out = cop_symbol(CALL_OPR_FUNC, op->id);
        return true;
    case ISEL_OP_LABEL:
        *out = cop_symbol(CALL_OPR_LABEL, op->id);
        return true;
    default:
        *out = call_operand_none();
        return true;
    }
}

/* Convert a destination operand to its physical form. Returns the
 * physical destination and, when the destination is a vreg, the scratch
 * register the result is computed in (via *dst_reg), plus the vreg to
 * store back (*dst_vreg, -1 when none). `ir_id` traces the loads. */
static bool lower_dest(CallCtx *ctx, const IselOperand *op,
                       X64Reg *dst_reg, int64_t *dst_vreg,
                       CallOperand *out, int64_t ir_id)
{
    *dst_vreg = -1;
    *dst_reg = X64_REG_R10;
    if (op == NULL || op->kind == ISEL_OP_NONE) {
        *out = call_operand_none();
        return true;
    }
    if (op->kind == ISEL_OP_VREG) {
        *dst_vreg = op->vreg;
        *out = cop_reg(X64_REG_R10, op->width > 0 ? op->width : 8);
        return true;
    }
    if (op->kind == ISEL_OP_MEM) {
        if (op->vreg == FRAME_BASE_VREG) {
            *out = cop_mem(X64_REG_RBP, op->imm, op->width);
            return true;
        }
        /* dst base register is loaded into R10 (no result scratch) */
        if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_R10, 8),
                            spill_mem(ctx, op->vreg), call_operand_none(),
                            ISEL_COND_E, 0, false, ir_id)) {
            return false;
        }
        *out = cop_mem(X64_REG_R10, op->imm, op->width);
        return true;
    }
    if (op->kind == ISEL_OP_LABEL) {
        *out = cop_symbol(CALL_OPR_LABEL, op->id);
        return true;
    }
    *out = call_operand_none();
    return true;
}

/* Emit the spill store for a result computed in R10 (widening narrow
 * results to 8 bytes so slots always hold zero-extended images). */
static bool store_result(CallCtx *ctx, int64_t dst_vreg, int width,
                         int64_t ir_id)
{
    if (dst_vreg < 0) {
        return true;
    }
    if (width < 4) {
        if (!emit_body_insn(ctx->out, ISEL_MOVZX,
                            cop_reg(X64_REG_R10, 8),
                            cop_reg(X64_REG_R10, width),
                            call_operand_none(), ISEL_COND_E, 0, false,
                            ir_id)) {
            return false;
        }
    }
    return emit_body_insn(ctx->out, ISEL_MOV, spill_mem(ctx, dst_vreg),
                          cop_reg(X64_REG_R10, 8), call_operand_none(),
                          ISEL_COND_E, 0, false, ir_id);
}

/* ---------------------------------------------------------------------------
 * Instruction lowering
 * ------------------------------------------------------------------------- */

/* Opcodes whose destination is also a source (two-address x86 form). */
static bool opcode_is_two_address(IselOpcode op)
{
    switch (op) {
    case ISEL_ADD:
    case ISEL_SUB:
    case ISEL_IMUL:
    case ISEL_AND:
    case ISEL_OR:
    case ISEL_XOR:
    case ISEL_NEG:
    case ISEL_NOT:
        return true;
    default:
        return false;
    }
}

static bool lower_idiv(CallCtx *ctx, const IselInsn *insn)
{
    int w = insn->dst.width > 0 ? insn->dst.width : 8;
    int64_t dv = insn->dst.vreg;
    CallOperand divisor = call_operand_none();
    bool is_mod = insn->mod;
    /* dividend -> RAX */
    if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_RAX, w),
                        spill_mem(ctx, dv), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id)) {
        return false;
    }
    /* sign-extend the dividend into RDX:RAX (cdq/cwd/cbw/cqo
     * equivalents with the existing opcode set) */
    if (w == 8) {
        if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_RDX, 8),
                            cop_reg(X64_REG_RAX, 8), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id) ||
            !emit_body_insn(ctx->out, ISEL_SAR, cop_reg(X64_REG_RDX, 8),
                            cop_imm(63, 8, false), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id)) {
            return false;
        }
    } else if (w == 4) {
        if (!emit_body_insn(ctx->out, ISEL_MOVSX,
                            cop_reg(X64_REG_RDX, 8),
                            cop_reg(X64_REG_RAX, 4), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id)) {
            return false;
        }
    } else if (w == 2) {
        if (!emit_body_insn(ctx->out, ISEL_MOVSX,
                            cop_reg(X64_REG_RDX, 8),
                            cop_reg(X64_REG_RAX, 2), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id)) {
            return false;
        }
    } else {
        if (!emit_body_insn(ctx->out, ISEL_MOVSX,
                            cop_reg(X64_REG_RAX, 2),
                            cop_reg(X64_REG_RAX, 1), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id)) {
            return false;
        }
    }
    /* divisor -> R10 */
    if (!lower_source(ctx, &insn->src1, X64_REG_R10, &divisor,
                        insn->ir_node_id)) {
        return false;
    }
    if (!emit_body_insn(ctx->out, ISEL_IDIV, cop_reg(X64_REG_RAX, w),
                        divisor, call_operand_none(), ISEL_COND_E, 0,
                        is_mod, insn->ir_node_id)) {
        return false;
    }
    /* quotient in RAX, remainder in RDX */
    if (!emit_body_insn(ctx->out, ISEL_MOV,
                        cop_reg(X64_REG_R10, w),
                        cop_reg(is_mod ? X64_REG_RDX : X64_REG_RAX, w),
                        call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id)) {
        return false;
    }
    return store_result(ctx, dv, w, insn->ir_node_id);
}

static bool lower_shift(CallCtx *ctx, const IselInsn *insn)
{
    int w = insn->dst.width > 0 ? insn->dst.width : 8;
    int64_t dv = insn->dst.vreg;
    CallOperand count = call_operand_none();
    /* value (two-address) -> R10 */
    if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_R10, 8),
                        spill_mem(ctx, dv), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id)) {
        return false;
    }
    if (insn->src1.kind == ISEL_OP_IMM) {
        count = cop_imm(insn->src1.imm, insn->src1.width,
                        insn->src1.is_unsigned);
    } else {
        /* variable count -> CL */
        if (!lower_source(ctx, &insn->src1, X64_REG_RCX, &count,
                            insn->ir_node_id)) {
            return false;
        }
        count.width = 1;
        count.id = (int64_t)X64_REG_RCX;
    }
    if (!emit_body_insn(ctx->out, insn->op, cop_reg(X64_REG_R10, w),
                        count, call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id)) {
        return false;
    }
    return store_result(ctx, dv, w, insn->ir_node_id);
}

static bool lower_generic(CallCtx *ctx, const IselInsn *insn)
{
    bool two_addr = opcode_is_two_address(insn->op);
    bool dst_is_vreg = insn->dst.kind == ISEL_OP_VREG;
    /* a memory destination whose base is a vreg consumes R10 for the
     * base load, so sources must use R11 */
    bool dst_base_uses_r10 = insn->dst.kind == ISEL_OP_MEM &&
                             insn->dst.vreg != FRAME_BASE_VREG;
    X64Reg dst_reg;
    int64_t dst_vreg;
    CallOperand dst = call_operand_none();
    CallOperand src1 = call_operand_none();
    CallOperand src2 = call_operand_none();
    X64Reg src_scratch1 =
        (dst_is_vreg || dst_base_uses_r10) ? X64_REG_R11 : X64_REG_R10;
    X64Reg src_scratch2 =
        (dst_is_vreg || dst_base_uses_r10) ? X64_REG_R10 : X64_REG_R11;
    int w = insn->dst.width > 0 ? insn->dst.width : 8;

    if (two_addr) {
        /* load the destination (also a source) into R10 first */
        if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_R10, 8),
                            spill_mem(ctx, insn->dst.vreg),
                            call_operand_none(), ISEL_COND_E, 0, false,
                            insn->ir_node_id)) {
            return false;
        }
        dst = cop_reg(X64_REG_R10, w);
        dst_vreg = insn->dst.vreg;
    } else {
        if (!lower_dest(ctx, &insn->dst, &dst_reg, &dst_vreg, &dst,
                         insn->ir_node_id)) {
            return false;
        }
    }
    if (!lower_source(ctx, &insn->src1, src_scratch1, &src1,
                      insn->ir_node_id)) {
        return false;
    }
    if (!lower_source(ctx, &insn->src2, src_scratch2, &src2,
                      insn->ir_node_id)) {
        return false;
    }
    /* identical vreg source used twice (e.g. test r, r): load once */
    if (insn->src1.kind == ISEL_OP_VREG &&
        insn->src2.kind == ISEL_OP_VREG &&
        insn->src1.vreg == insn->src2.vreg &&
        src1.kind == CALL_OPR_REG) {
        src2 = src1;
    }
    if (!emit_body_insn(ctx->out, insn->op, dst, src1, src2, insn->cond,
                        insn->scale, insn->mod, insn->ir_node_id)) {
        return false;
    }
    return store_result(ctx, dst_vreg, w, insn->ir_node_id);
}

static bool lower_setcc(CallCtx *ctx, const IselInsn *insn)
{
    int64_t dv = insn->dst.vreg;
    if (!emit_body_insn(ctx->out, ISEL_SETCC, cop_reg(X64_REG_R10, 1),
                        call_operand_none(), call_operand_none(),
                        insn->cond, 0, false, dv)) {
        return false;
    }
    return store_result(ctx, dv, 1, insn->ir_node_id);
}

/* ---------------------------------------------------------------------------
 * Rep-op lowering (special registers: RDI/RSI/RCX, RAX=0 for stosb)
 * ------------------------------------------------------------------------- */

/* Set RDI to the address of a memory operand (base vreg or frame). */
static bool lower_addr_to_rdi(CallCtx *ctx, const IselOperand *op,
                              int64_t ir_id)
{
    if (op->vreg == FRAME_BASE_VREG) {
        return emit_body_insn(ctx->out, ISEL_LEA, cop_reg(X64_REG_RDI, 8),
                              cop_mem(X64_REG_RBP, op->imm, 8),
                              call_operand_none(), ISEL_COND_E, 0, false,
                              ir_id);
    }
    if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_RDI, 8),
                        spill_mem(ctx, op->vreg), call_operand_none(),
                        ISEL_COND_E, 0, false, ir_id)) {
        return false;
    }
    if (op->imm != 0) {
        if (!emit_body_insn(ctx->out, ISEL_ADD, cop_reg(X64_REG_RDI, 8),
                            cop_imm(op->imm, 8, false),
                            call_operand_none(), ISEL_COND_E, 0, false,
                            ir_id)) {
            return false;
        }
    }
    return true;
}

static bool lower_addr_to_rsi(CallCtx *ctx, const IselOperand *op,
                              int64_t ir_id)
{
    if (op->vreg == FRAME_BASE_VREG) {
        return emit_body_insn(ctx->out, ISEL_LEA, cop_reg(X64_REG_RSI, 8),
                              cop_mem(X64_REG_RBP, op->imm, 8),
                              call_operand_none(), ISEL_COND_E, 0, false,
                              ir_id);
    }
    if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_RSI, 8),
                        spill_mem(ctx, op->vreg), call_operand_none(),
                        ISEL_COND_E, 0, false, ir_id)) {
        return false;
    }
    if (op->imm != 0) {
        if (!emit_body_insn(ctx->out, ISEL_ADD, cop_reg(X64_REG_RSI, 8),
                            cop_imm(op->imm, 8, false),
                            call_operand_none(), ISEL_COND_E, 0, false,
                            ir_id)) {
            return false;
        }
    }
    return true;
}

static bool lower_rep_movsb(CallCtx *ctx, const IselInsn *insn)
{
    if (!lower_addr_to_rdi(ctx, &insn->dst, insn->ir_node_id) ||
        !lower_addr_to_rsi(ctx, &insn->src1, insn->ir_node_id)) {
        return false;
    }
    if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_RCX, 8),
                        cop_imm(insn->src2.imm, 8, true),
                        call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id)) {
        return false;
    }
    return emit_body_insn(ctx->out, ISEL_REP_MOVSB,
                          cop_reg(X64_REG_RDI, 8),
                          cop_reg(X64_REG_RSI, 8),
                          cop_reg(X64_REG_RCX, 8), ISEL_COND_E, 0, false,
                          insn->ir_node_id);
}

static bool lower_rep_stosb(CallCtx *ctx, const IselInsn *insn)
{
    if (!lower_addr_to_rdi(ctx, &insn->dst, insn->ir_node_id)) {
        return false;
    }
    if (!emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_RCX, 8),
                        cop_imm(insn->src1.imm, 8, true),
                        call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id)) {
        return false;
    }
    if (!emit_body_insn(ctx->out, ISEL_XOR, cop_reg(X64_REG_RAX, 4),
                        cop_reg(X64_REG_RAX, 4), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id)) {
        return false;
    }
    return emit_body_insn(ctx->out, ISEL_REP_STOSB,
                          cop_reg(X64_REG_RDI, 8),
                          cop_reg(X64_REG_RCX, 8),
                          cop_reg(X64_REG_RAX, 8), ISEL_COND_E, 0, false,
                          insn->ir_node_id);
}

static bool lower_slice(CallCtx *ctx, const IselInsn *insn)
{
    size_t k = ctx->slice_count++;
    int64_t dv = insn->dst.vreg;
    /* the pair-image materialization; the pseudo marker (emitted by the
     * caller) preserves the bounds-trap annotation for 17c */
    if (!emit_body_insn(ctx->out, ISEL_LEA, cop_reg(X64_REG_R10, 8),
                        pair_mem(ctx, k, 0), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id) ||
        !emit_body_insn(ctx->out, ISEL_MOV, spill_mem(ctx, dv),
                        cop_reg(X64_REG_R10, 8), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id) ||
        !emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_R11, 8),
                        spill_mem(ctx, insn->src1.vreg),
                        call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id) ||
        !emit_body_insn(ctx->out, ISEL_MOV, pair_mem(ctx, k, 0),
                        cop_reg(X64_REG_R11, 8), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id) ||
        !emit_body_insn(ctx->out, ISEL_MOV, cop_reg(X64_REG_R11, 8),
                        spill_mem(ctx, insn->src2.vreg),
                        call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id) ||
        !emit_body_insn(ctx->out, ISEL_MOV, pair_mem(ctx, k, 8),
                        cop_reg(X64_REG_R11, 8), call_operand_none(),
                        ISEL_COND_E, 0, false, insn->ir_node_id)) {
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Call emission (acceptance criterion)
 * ------------------------------------------------------------------------- */

/* The four register-argument registers of sec. 15.7. */
static X64Reg arg_register(size_t i)
{
    switch (i) {
    case 0: return X64_REG_RCX;
    case 1: return X64_REG_RDX;
    case 2: return X64_REG_R8;
    default: return X64_REG_R9;
    }
}

static bool lower_call(CallCtx *ctx, const IselInsn *insn)
{
    const IrNode *call_node = NULL;
    IrNode **args = NULL;
    size_t nargs = 0;
    size_t i;
    if (insn->ir_node_id >= 0 &&
        (size_t)insn->ir_node_id < ctx->vreg_cap) {
        call_node = ctx->build->nodes[insn->ir_node_id];
    }
    if (call_node != NULL && call_node->kind == IR_CALL) {
        args = call_node->u.call.args;
        nargs = call_node->u.call.nargs;
    } else if (call_node != NULL && call_node->kind == IR_CALL_TERM) {
        args = call_node->u.call_term.args;
        nargs = call_node->u.call_term.nargs;
    }
    for (i = 0; i < nargs; i++) {
        int64_t av = vreg_of_node(ctx, args[i]);
        if (av < 0) {
            continue;   /* defensive: no value (unreachable on verified IR) */
        }
        if (i < 4) {
            if (!emit_body_insn(ctx->out, ISEL_MOV,
                                cop_reg(arg_register(i), 8),
                                spill_mem(ctx, av), call_operand_none(),
                                ISEL_COND_E, 0, false,
                                insn->ir_node_id)) {
                return false;
            }
        } else {
            /* stack argument above the shadow space: [rsp + 32 + 8*(i-4)] */
            if (!emit_body_insn(ctx->out, ISEL_MOV,
                                cop_reg(X64_REG_R10, 8),
                                spill_mem(ctx, av), call_operand_none(),
                                ISEL_COND_E, 0, false,
                                insn->ir_node_id) ||
                !emit_body_insn(ctx->out, ISEL_MOV,
                                cop_mem(X64_REG_RSP,
                                        CALL_SHADOW_BYTES +
                                            8 * (int64_t)(i - 4), 8),
                                cop_reg(X64_REG_R10, 8),
                                call_operand_none(), ISEL_COND_E, 0, false,
                                insn->ir_node_id)) {
                return false;
            }
        }
    }
    /* the call itself */
    if (!emit_body_insn(ctx->out, ISEL_CALL, call_operand_none(),
                        cop_symbol(CALL_OPR_FUNC, insn->src1.id),
                        call_operand_none(), ISEL_COND_E, 0, false,
                        insn->ir_node_id)) {
        return false;
    }
    /* capture the return value from RAX (void calls have no dst) */
    if (insn->dst.kind == ISEL_OP_VREG) {
        if (!emit_body_insn(ctx->out, ISEL_MOV,
                            cop_reg(X64_REG_R10, 8),
                            cop_reg(X64_REG_RAX, 8), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id) ||
            !emit_body_insn(ctx->out, ISEL_MOV,
                            spill_mem(ctx, insn->dst.vreg),
                            cop_reg(X64_REG_R10, 8), call_operand_none(),
                            ISEL_COND_E, 0, false, insn->ir_node_id)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Parameter copies (incoming arguments -> parameter slots)
 * ------------------------------------------------------------------------- */

static bool emit_param_copies(CallCtx *ctx)
{
    const IrNode *fn = ctx->fn;
    size_t i;
    for (i = 0; i < fn->u.function.nparams; i++) {
        const IrSlot *slot = fn->u.function.slots[i];
        const FrameSlotLayout *sl;
        int w;
        bool comp;
        if (slot == NULL || ctx->layout == NULL) {
            continue;
        }
        sl = frame_layout_slot(ctx->layout, slot->index);
        if (sl == NULL) {
            continue;
        }
        w = (int)sl->size;
        if (w <= 0) {
            w = 8;
        }
        comp = type_is_composite(slot->type);
        if (comp) {
            /* copy by value from the incoming address (address-resident
             * argument; IR contract sec. 5.3) */
            if (i < 4) {
                if (!emit_body_insn(ctx->out, ISEL_LEA,
                                    cop_reg(X64_REG_RDI, 8),
                                    cop_mem(X64_REG_RBP, sl->offset, 8),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_MOV,
                                    cop_reg(X64_REG_RSI, 8),
                                    cop_reg(arg_register(i), 8),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_MOV,
                                    cop_reg(X64_REG_RCX, 8),
                                    cop_imm(sl->size, 8, true),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_REP_MOVSB,
                                    cop_reg(X64_REG_RDI, 8),
                                    cop_reg(X64_REG_RSI, 8),
                                    cop_reg(X64_REG_RCX, 8), ISEL_COND_E,
                                    0, false, slot->index)) {
                    return false;
                }
            } else {
                if (!emit_body_insn(ctx->out, ISEL_MOV,
                                    cop_reg(X64_REG_R10, 8),
                                    cop_mem(X64_REG_RBP,
                                            8 * (int64_t)i + 16, 8),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_LEA,
                                    cop_reg(X64_REG_RDI, 8),
                                    cop_mem(X64_REG_RBP, sl->offset, 8),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_MOV,
                                    cop_reg(X64_REG_RSI, 8),
                                    cop_reg(X64_REG_R10, 8),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_MOV,
                                    cop_reg(X64_REG_RCX, 8),
                                    cop_imm(sl->size, 8, true),
                                    call_operand_none(), ISEL_COND_E, 0,
                                    false, slot->index) ||
                    !emit_body_insn(ctx->out, ISEL_REP_MOVSB,
                                    cop_reg(X64_REG_RDI, 8),
                                    cop_reg(X64_REG_RSI, 8),
                                    cop_reg(X64_REG_RCX, 8), ISEL_COND_E,
                                    0, false, slot->index)) {
                    return false;
                }
            }
        } else if (i < 4) {
            /* scalar register parameter: store the incoming register */
            if (!emit_body_insn(ctx->out, ISEL_MOV,
                                cop_mem(X64_REG_RBP, sl->offset, w),
                                cop_reg(arg_register(i), w),
                                call_operand_none(), ISEL_COND_E, 0, false,
                                slot->index)) {
                return false;
            }
        } else {
            /* scalar stack parameter: [rbp + 8*i + 16] */
            if (!emit_body_insn(ctx->out, ISEL_MOV,
                                cop_reg(X64_REG_R10, 8),
                                cop_mem(X64_REG_RBP, 8 * (int64_t)i + 16, 8),
                                call_operand_none(), ISEL_COND_E, 0, false,
                                slot->index) ||
                !emit_body_insn(ctx->out, ISEL_MOV,
                                cop_mem(X64_REG_RBP, sl->offset, w),
                                cop_reg(X64_REG_R10, w),
                                call_operand_none(), ISEL_COND_E, 0, false,
                                slot->index)) {
                return false;
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Epilogue restore
 *
 * frame.c emits the epilogue steps (FRAME_OP_MOV_RSP_RBP followed by
 * FRAME_OP_POP_RBP) BEFORE the ISEL_RET body in the framed stream.
 * call_build defers them (ctx->epilogue_pending) so the ISEL_RET
 * handler can load the return value into RAX first, while RBP still
 * addresses the current frame (spec sec. 15.7 "RAX return"), and
 * only then restore the frame:
 *
 *   mov rsp, rbp
 *   [sub rsp, total+saved_bytes]   (only when callee-saved saved)
 *   [pop rdi; pop rsi]             (only the saved registers)
 *   [add rsp, total]               (RSP back to RBP before pop rbp)
 *   pop rbp
 *
 * The add rsp,total is what lets `pop rbp` read [rbp] (the saved
 * caller rbp) instead of [rbp-total] (inside the reservation), so
 * `ret` pops the correct return address (callee-saved contract).
 * ------------------------------------------------------------------------- */

static bool emit_epilogue_restore(CallCtx *ctx)
{
    const CallFunction *cf = ctx->cf;
    int64_t fn_id = ctx->fn != NULL ? ctx->fn->id : 0;
    if (!emit_insn(ctx->out, CALL_OP_MOV_RSP_RBP, ISEL_COMMENT,
                   call_operand_none(), call_operand_none(),
                   call_operand_none(), ISEL_COND_E, 0, false,
                   0, fn_id)) {
        return false;
    }
    if (cf->saved_bytes > 0) {
        if (!emit_insn(ctx->out, CALL_OP_SUB_RSP, ISEL_COMMENT,
                       call_operand_none(), call_operand_none(),
                       call_operand_none(), ISEL_COND_E, 0, false,
                       cf->total + cf->saved_bytes, fn_id)) {
            return false;
        }
        if (cf->saves_rdi) {
            if (!emit_insn(ctx->out, CALL_OP_POP_REG, ISEL_COMMENT,
                           call_operand_none(), call_operand_none(),
                           call_operand_none(), ISEL_COND_E, 0, false,
                           X64_REG_RDI, fn_id)) {
                return false;
            }
        }
        if (cf->saves_rsi) {
            if (!emit_insn(ctx->out, CALL_OP_POP_REG, ISEL_COMMENT,
                           call_operand_none(), call_operand_none(),
                           call_operand_none(), ISEL_COND_E, 0, false,
                           X64_REG_RSI, fn_id)) {
                return false;
            }
        }
        /* RSP back to RBP before pop rbp (CRIT-2 resolution) */
        if (!emit_insn(ctx->out, CALL_OP_ADD_RSP, ISEL_COMMENT,
                       call_operand_none(), call_operand_none(),
                       call_operand_none(), ISEL_COND_E, 0, false,
                       cf->total, fn_id)) {
            return false;
        }
    }
    return emit_insn(ctx->out, CALL_OP_POP_RBP, ISEL_COMMENT,
                     call_operand_none(), call_operand_none(),
                     call_operand_none(), ISEL_COND_E, 0, false,
                     0, fn_id);
}

/* ---------------------------------------------------------------------------
 * Body instruction dispatch
 * ------------------------------------------------------------------------- */

static bool lower_body_insn(CallCtx *ctx, const IselInsn *insn)
{
    switch (insn->op) {
    case ISEL_COMMENT:
    case ISEL_STRCMP:
    case ISEL_SLICEEQ:
    case ISEL_UTF8:
    case ISEL_PTRDIFF:
    case ISEL_TRAP:
        /* 17c-owned pseudo-ops pass through as annotated markers */
        return emit_pseudo(ctx->out, insn);
    case ISEL_LABEL:
        return emit_body_insn(ctx->out, ISEL_LABEL,
                              cop_symbol(CALL_OPR_LABEL, insn->dst.id),
                              call_operand_none(), call_operand_none(),
                              ISEL_COND_E, 0, false, insn->ir_node_id);
    case ISEL_JMP:
        return emit_body_insn(ctx->out, ISEL_JMP, call_operand_none(),
                              cop_symbol(CALL_OPR_LABEL, insn->src1.id),
                              call_operand_none(), ISEL_COND_E, 0, false,
                              insn->ir_node_id);
    case ISEL_JCC:
        return emit_body_insn(ctx->out, ISEL_JCC, call_operand_none(),
                              cop_symbol(CALL_OPR_LABEL, insn->src1.id),
                              call_operand_none(), insn->cond, 0, false,
                              insn->ir_node_id);
    case ISEL_CALL:
        return lower_call(ctx, insn);
    case ISEL_RET:
        /* the return value is placed in RAX BEFORE the epilogue frame
         * restore (spec sec. 15.7 "RAX return"): the framed stream
         * carries the epilogue steps (MOV_RSP_RBP / POP_RBP) before
         * this body, so call_build deferred them (ctx->epilogue_pending)
         * and they are emitted here after the load, while RBP still
         * addresses the current frame */
        if (insn->src1.kind == ISEL_OP_VREG) {
            if (!emit_body_insn(ctx->out, ISEL_MOV,
                                cop_reg(X64_REG_RAX, 8),
                                spill_mem(ctx, insn->src1.vreg),
                                call_operand_none(), ISEL_COND_E, 0, false,
                                insn->ir_node_id)) {
                return false;
            }
        }
        if (ctx->epilogue_pending) {
            if (!emit_epilogue_restore(ctx)) {
                return false;
            }
            ctx->epilogue_pending = false;
        }
        return emit_body_insn(ctx->out, ISEL_RET, call_operand_none(),
                              call_operand_none(), call_operand_none(),
                              ISEL_COND_E, 0, false, insn->ir_node_id);
    case ISEL_REP_MOVSB:
        return lower_rep_movsb(ctx, insn);
    case ISEL_REP_STOSB:
        return lower_rep_stosb(ctx, insn);
    case ISEL_SLICE:
        if (!emit_pseudo(ctx->out, insn)) {
            return false;
        }
        return lower_slice(ctx, insn);
    case ISEL_IDIV:
        return lower_idiv(ctx, insn);
    case ISEL_SHL:
    case ISEL_SHR:
    case ISEL_SAR:
        return lower_shift(ctx, insn);
    case ISEL_SETCC:
        return lower_setcc(ctx, insn);
    default:
        return lower_generic(ctx, insn);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static bool append_function(CallOutput *co, const CallFunction *cf)
{
    if (co->nfunctions == co->functions_cap) {
        size_t ncap = co->functions_cap == 0 ? 8 : co->functions_cap * 2;
        CallFunction *p = (CallFunction *)realloc(
            co->functions, ncap * sizeof(CallFunction));
        if (p == NULL) {
            co->oom = true;
            return false;
        }
        co->functions = p;
        co->functions_cap = ncap;
    }
    co->functions[co->nfunctions] = *cf;
    co->nfunctions++;
    return true;
}

/* Append the pending function with its final [start, count) span. */
static bool finalize_function(CallOutput *co, const CallFunction *cf,
                              size_t start)
{
    CallFunction c = *cf;
    c.start = start;
    c.count = co->count - start;
    return append_function(co, &c);
}

CallStatus call_build(const IrBuild *build, const FrameOutput *fr,
                      CallOutput **out)
{
    CallOutput *co;
    int64_t *vreg_map = NULL;
    size_t vreg_cap = 0;
    CallCtx ctx;
    size_t i;
    if (build == NULL || fr == NULL || out == NULL) {
        return CALL_OOM;
    }
    co = (CallOutput *)calloc(1, sizeof(*co));
    if (co == NULL) {
        return CALL_OOM;
    }
    co->entry_function_id = fr->entry_function_id;
    if (!build_vreg_map(fr, build, &vreg_map, &vreg_cap)) {
        call_output_free(co);
        return CALL_OOM;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.build = build;
    ctx.fr = fr;
    ctx.out = co;
    ctx.vreg_of = vreg_map;
    ctx.vreg_cap = vreg_cap;

    i = 0;
    while (i < fr->count) {
        const FrameInsn *fi = &fr->insns[i];
        bool is_function_marker = false;
        if (fi->op == FRAME_OP_BODY && fi->body.op == ISEL_COMMENT &&
            fi->body.note != NULL &&
            strncmp(fi->body.note, "function ", 9) == 0) {
            is_function_marker = true;
        }
        if (is_function_marker) {
            const IrNode *fn = lookup_function(build, fi->body.ir_node_id);
            RegionStats st;
            const FrameLayout *layout = NULL;
            size_t region_start = i + 1;
            size_t region_end = region_start;
            /* finalize the previous function now that its span is known */
            if (ctx.fn != NULL && ctx.cf == &ctx.cf_storage) {
                if (!finalize_function(co, &ctx.cf_storage,
                                       ctx.cf_storage.start)) {
                    co->oom = true;
                    break;
                }
            }
            if (fn == NULL) {
                if (!emit_pseudo(co, &fi->body)) {
                    co->oom = true;
                    break;
                }
                ctx.fn = NULL;
                ctx.cf = NULL;
                ctx.layout = NULL;
                i++;
                continue;
            }
            /* scan to the end of this function's region */
            while (region_end < fr->count) {
                const FrameInsn *nf = &fr->insns[region_end];
                if (nf->op == FRAME_OP_BODY &&
                    nf->body.op == ISEL_COMMENT && nf->body.note != NULL &&
                    strncmp(nf->body.note, "function ", 9) == 0) {
                    break;
                }
                region_end++;
            }
            analyze_region(fr, region_start, region_end, fn, build, &st);
            layout = frame_layout_for_function(fr, fn->id);
            compute_plan(&ctx.cf_storage, layout, &st);
            ctx.cf_storage.function_id = fn->id;
            ctx.cf_storage.start = co->count;
            ctx.cf_storage.count = 0;
            /* the marker comment passes through */
            if (!emit_pseudo(co, &fi->body)) {
                co->oom = true;
                break;
            }
            ctx.layout = layout;
            ctx.fn = fn;
            ctx.cf = &ctx.cf_storage;
            ctx.slice_count = 0;
            ctx.epilogue_pending = false;
            if (fn->u.function.body != NULL) {
                /* final prologue: push rbp; mov rbp, rsp; sub rsp, total;
                 * [save callee-saved] */
                if (!emit_insn(co, CALL_OP_PUSH_RBP, ISEL_COMMENT,
                               call_operand_none(), call_operand_none(),
                               call_operand_none(), ISEL_COND_E, 0, false,
                               0, fn->id) ||
                    !emit_insn(co, CALL_OP_MOV_RBP_RSP, ISEL_COMMENT,
                               call_operand_none(), call_operand_none(),
                               call_operand_none(), ISEL_COND_E, 0, false,
                               0, fn->id)) {
                    co->oom = true;
                    break;
                }
                if (ctx.cf_storage.total > 0) {
                    if (!emit_insn(co, CALL_OP_SUB_RSP, ISEL_COMMENT,
                                   call_operand_none(), call_operand_none(),
                                   call_operand_none(), ISEL_COND_E, 0,
                                   false, ctx.cf_storage.total, fn->id)) {
                        co->oom = true;
                        break;
                    }
                }
                if (ctx.cf_storage.saves_rsi) {
                    if (!emit_insn(co, CALL_OP_PUSH_REG, ISEL_COMMENT,
                                   call_operand_none(), call_operand_none(),
                                   call_operand_none(), ISEL_COND_E, 0,
                                   false, X64_REG_RSI, fn->id)) {
                        co->oom = true;
                        break;
                    }
                }
                if (ctx.cf_storage.saves_rdi) {
                    if (!emit_insn(co, CALL_OP_PUSH_REG, ISEL_COMMENT,
                                   call_operand_none(), call_operand_none(),
                                   call_operand_none(), ISEL_COND_E, 0,
                                   false, X64_REG_RDI, fn->id)) {
                        co->oom = true;
                        break;
                    }
                }
                if (!emit_param_copies(&ctx)) {
                    co->oom = true;
                    break;
                }
            }
            /* continue the outer loop into the body region */
            i = region_start;
            continue;
        }
        /* body instructions of the current function */
        if (ctx.fn != NULL && ctx.cf == &ctx.cf_storage &&
            ctx.fn->u.function.body != NULL) {
            switch (fi->op) {
            case FRAME_OP_BODY:
                if (!lower_body_insn(&ctx, &fi->body)) {
                    co->oom = true;
                    i = fr->count;
                }
                break;
            case FRAME_OP_MOV_RSP_RBP:
                /* epilogue restore: frame.c emits these steps before
                 * the ISEL_RET body. They are deferred
                 * (ctx->epilogue_pending) and emitted by the ISEL_RET
                 * handler AFTER the return value is loaded into RAX
                 * (CRIT-1 resolution: [rbp-off] must be read while
                 * RBP still addresses the current frame). */
                ctx.epilogue_pending = true;
                break;
            case FRAME_OP_POP_RBP:
                /* part of the deferred epilogue restore (always paired
                 * with FRAME_OP_MOV_RSP_RBP by frame.c); emitted with
                 * it by the ISEL_RET handler. Defensive: a lone pop
                 * rbp (unreachable on verified framed streams) is
                 * still emitted so the restore never gets lost. */
                if (!ctx.epilogue_pending) {
                    if (!emit_insn(co, CALL_OP_POP_RBP, ISEL_COMMENT,
                                   call_operand_none(), call_operand_none(),
                                   call_operand_none(), ISEL_COND_E, 0,
                                   false, 0, ctx.fn->id)) {
                        co->oom = true;
                        i = fr->count;
                    }
                }
                break;
            default:
                /* defensive: no other framed step appears in a body
                 * (the 17b1 prologue steps were replaced by our own) */
                break;
            }
        }
        i++;
    }
    /* finalize the last function */
    if (ctx.fn != NULL && ctx.cf == &ctx.cf_storage) {
        if (!finalize_function(co, &ctx.cf_storage, ctx.cf_storage.start)) {
            co->oom = true;
        }
    }

    if (co->oom) {
        free(vreg_map);
        call_output_free(co);
        *out = NULL;
        return CALL_OOM;
    }
    free(vreg_map);
    *out = co;
    return CALL_OK;
}

void call_output_free(CallOutput *out)
{
    if (out == NULL) {
        return;
    }
    /* per-function plans carry no owned data */
    free(out->functions);
    free(out->insns);
    free(out);
}

size_t call_output_count(const CallOutput *out)
{
    return out != NULL ? out->count : 0;
}

const CallInsn *call_output_insn(const CallOutput *out, size_t i)
{
    if (out == NULL || i >= out->count) {
        return NULL;
    }
    return &out->insns[i];
}

size_t call_function_count(const CallOutput *out)
{
    return out != NULL ? out->nfunctions : 0;
}

const CallFunction *call_function_at(const CallOutput *out, size_t i)
{
    if (out == NULL || i >= out->nfunctions) {
        return NULL;
    }
    return &out->functions[i];
}

const CallFunction *call_function_for(const CallOutput *out,
                                      int64_t function_id)
{
    size_t i;
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < out->nfunctions; i++) {
        if (out->functions[i].function_id == function_id) {
            return &out->functions[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Deterministic physical assembly dump
 * ------------------------------------------------------------------------- */

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

static bool dump_call_operand(DiagBuf *b, const CallOperand *op)
{
    switch (op->kind) {
    case CALL_OPR_NONE:
        return true;
    case CALL_OPR_REG:
        return s_append_cstr(b, reg_name((X64Reg)op->id, op->width));
    case CALL_OPR_IMM:
        if (op->is_unsigned) {
            return s_printf(b, "$%llu", (unsigned long long)op->imm);
        }
        return s_printf(b, "$%lld", (long long)op->imm);
    case CALL_OPR_MEM:
        if (op->imm == 0) {
            return s_printf(b, "[%s]", reg_name((X64Reg)op->id, 8));
        }
        return s_printf(b, "[%s%+lld]", reg_name((X64Reg)op->id, 8),
                        (long long)op->imm);
    case CALL_OPR_GLOBAL:
        return s_printf(b, "g%lld", (long long)op->id);
    case CALL_OPR_STR:
        return s_printf(b, ".Lstr%lld", (long long)op->id);
    case CALL_OPR_FUNC:
        return s_printf(b, "fn%lld", (long long)op->id);
    case CALL_OPR_LABEL:
        return s_printf(b, "L%lld", (long long)op->id);
    default:
        return false;
    }
}

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

/* Render a pass-through pseudo/comment line in isel_core's dump
 * format (vreg operands stay symbolic). */
static bool dump_pseudo_line(DiagBuf *b, const IselInsn *insn)
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
    if (insn->dst.kind != ISEL_OP_NONE ||
        insn->src1.kind != ISEL_OP_NONE ||
        insn->src2.kind != ISEL_OP_NONE) {
        const IselOperand *ops[3] = { &insn->dst, &insn->src1, &insn->src2 };
        size_t oi;
        bool need_sep = false;
        if (!s_append_cstr(b, " ")) {
            return false;
        }
        for (oi = 0; oi < 3; oi++) {
            const IselOperand *op = ops[oi];
            if (op->kind == ISEL_OP_NONE) {
                continue;
            }
            if (need_sep) {
                if (!s_append_cstr(b, ", ")) {
                    return false;
                }
            }
            switch (op->kind) {
            case ISEL_OP_VREG:
                if (!s_printf(b, "r%lld", (long long)op->vreg)) {
                    return false;
                }
                break;
            case ISEL_OP_IMM:
                if (op->is_unsigned) {
                    if (!s_printf(b, "$%llu",
                                  (unsigned long long)op->imm)) {
                        return false;
                    }
                } else if (!s_printf(b, "$%lld", (long long)op->imm)) {
                    return false;
                }
                break;
            case ISEL_OP_SLOT:
                if (!s_printf(b, "slot%lld", (long long)op->id)) {
                    return false;
                }
                break;
            case ISEL_OP_GLOBAL:
                if (!s_printf(b, "g%lld", (long long)op->id)) {
                    return false;
                }
                break;
            case ISEL_OP_STR:
                if (!s_printf(b, ".Lstr%lld", (long long)op->id)) {
                    return false;
                }
                break;
            case ISEL_OP_FUNC:
                if (!s_printf(b, "fn%lld", (long long)op->id)) {
                    return false;
                }
                break;
            case ISEL_OP_LABEL:
                if (!s_printf(b, "L%lld", (long long)op->id)) {
                    return false;
                }
                break;
            case ISEL_OP_MEM:
                if (op->vreg == FRAME_BASE_VREG) {
                    if (!s_printf(b, "[rbp%+lld]", (long long)op->imm)) {
                        return false;
                    }
                } else if (op->imm == 0) {
                    if (!s_printf(b, "[r%lld]", (long long)op->vreg)) {
                        return false;
                    }
                } else if (!s_printf(b, "[r%lld%+lld]",
                                      (long long)op->vreg,
                                      (long long)op->imm)) {
                    return false;
                }
                break;
            default:
                break;
            }
            need_sep = true;
        }
    }
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

/* Render one physical instruction line (no leading indent, so the
 * prologue/epilogue and body read like assembly). */
static bool dump_phys_line(DiagBuf *b, const CallInsn *ci)
{
    switch (ci->op) {
    case CALL_OP_PSEUDO:
        return dump_pseudo_line(b, &ci->pseudo);
    case CALL_OP_PUSH_RBP:
        return s_append_cstr(b, "  push rbp\n");
    case CALL_OP_MOV_RBP_RSP:
        return s_append_cstr(b, "  mov rbp, rsp\n");
    case CALL_OP_SUB_RSP:
        return s_printf(b, "  sub rsp, $%lld\n", (long long)ci->imm);
    case CALL_OP_ADD_RSP:
        return s_printf(b, "  add rsp, $%lld\n", (long long)ci->imm);
    case CALL_OP_MOV_RSP_RBP:
        return s_append_cstr(b, "  mov rsp, rbp\n");
    case CALL_OP_POP_RBP:
        return s_append_cstr(b, "  pop rbp\n");
    case CALL_OP_PUSH_REG:
        return s_printf(b, "  push %s\n",
                        x64_reg_text((X64Reg)ci->imm));
    case CALL_OP_POP_REG:
        return s_printf(b, "  pop %s\n",
                        x64_reg_text((X64Reg)ci->imm));
    case CALL_OP_BODY:
        break;
    default:
        return false;
    }
    if (!s_append_cstr(b, "  ")) {
        return false;
    }
    if (ci->isel == ISEL_SETCC) {
        if (!s_append_cstr(b, "set") ||
            !s_append_cstr(b, isel_cond_text(ci->cond))) {
            return false;
        }
    } else if (ci->isel == ISEL_JCC) {
        if (!s_append_cstr(b, "j") ||
            !s_append_cstr(b, isel_cond_text(ci->cond))) {
            return false;
        }
    } else if (!s_append_cstr(b, isel_opcode_text(ci->isel))) {
        return false;
    }
    if (ci->isel == ISEL_LEA && ci->scale > 0) {
        if (!s_append_cstr(b, " (scaled)")) {
            return false;
        }
    }
    /* idiv has an implicit RAX/RDX dividend; the divisor is the only
     * rendered operand (the mod flag marks a remainder result) */
    if (ci->isel == ISEL_IDIV) {
        if (ci->src1.kind != CALL_OPR_NONE) {
            if (!s_append_cstr(b, " ") || !dump_call_operand(b, &ci->src1)) {
                return false;
            }
        }
        if (ci->mod) {
            if (!s_append_cstr(b, " mod")) {
                return false;
            }
        }
        if (!s_printf(b, "   # ir%lld", (long long)ci->ir_node_id)) {
            return false;
        }
        return s_append_cstr(b, "\n");
    }
    /* rep movsb/stosb use their implicit RDI/RSI/RCX/RAX operands */
    if (ci->isel == ISEL_REP_MOVSB || ci->isel == ISEL_REP_STOSB) {
        if (!s_printf(b, "   # ir%lld", (long long)ci->ir_node_id)) {
            return false;
        }
        return s_append_cstr(b, "\n");
    }
    if (ci->dst.kind != CALL_OPR_NONE ||
        ci->src1.kind != CALL_OPR_NONE ||
        ci->src2.kind != CALL_OPR_NONE) {
        const CallOperand *ops[3] = { &ci->dst, &ci->src1, &ci->src2 };
        size_t oi;
        bool need_sep = false;
        if (!s_append_cstr(b, " ")) {
            return false;
        }
        for (oi = 0; oi < 3; oi++) {
            if (ops[oi]->kind == CALL_OPR_NONE) {
                continue;
            }
            if (need_sep) {
                if (!s_append_cstr(b, ", ")) {
                    return false;
                }
            }
            if (!dump_call_operand(b, ops[oi])) {
                return false;
            }
            need_sep = true;
        }
    }
    if (!s_printf(b, "   # ir%lld", (long long)ci->ir_node_id)) {
        return false;
    }
    return s_append_cstr(b, "\n");
}

bool call_asm_dump(const CallOutput *co, DiagBuf *out)
{
    size_t i;
    if (co == NULL || out == NULL) {
        return false;
    }
    if (!s_append_cstr(out, "; AI-Co call emission dump "
                            "(WP-M0-17b2, deterministic)\n")) {
        return false;
    }
    if (!s_printf(out, "; functions=%zu insns=%zu entry=%lld\n",
                  co->nfunctions, co->count,
                  (long long)co->entry_function_id)) {
        return false;
    }
    for (i = 0; i < co->count; i++) {
        if (!dump_phys_line(out, &co->insns[i])) {
            return false;
        }
    }
    return true;
}
