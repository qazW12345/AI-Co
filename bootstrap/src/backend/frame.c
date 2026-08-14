/* bootstrap/src/backend/frame.c
 *
 * AI-Co Stage-0 x86-64 stack frame layout and function prologue/epilogue
 * (WP-M0-17b1).
 *
 * Implements the frame obligations of frame.h: deterministic per-function
 * slot layout below RBP, the frame-pointer prologue/epilogue instruction
 * sequences, `main` entry setup, and noreturn handling. See frame.h for
 * the normative rules (1-6), the entry convention, and the scope boundary
 * (register allocation/call emission 17b2, trap branches 17c, COFF 18).
 *
 * Determinism is structural: the pass walks the selection in emission
 * order and the IR's deterministic arrays (function comments carry the
 * construction-order function node id; slot tables are in slot-index
 * order); offsets and frame sizes are pure functions of the IR slot
 * types. Identical inputs always yield identical framed streams and
 * identical dump bytes (spec sec. 14.2).
 *
 * The frame dump renders body lines in the same format as isel_core's
 * assembly dump (isel_asm_dump) with two frame-specific changes: symbolic
 * slot operands are already rewritten to frame-base memory operands
 * ([rbp-<off>]), and the prologue/epilogue steps render as push rbp /
 * mov rbp, rsp / sub rsp, $N / mov rsp, rbp / pop rbp.
 */
#include "frame.h"

#include "../diag/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

static const char *const kFrameOpNames[] = {
    "body", "push rbp", "mov rbp, rsp", "sub rsp",
    "mov rsp, rbp", "pop rbp"
};

const char *frame_op_text(FrameOp op)
{
    if (op < 0 || (size_t)op >= sizeof(kFrameOpNames) / sizeof(kFrameOpNames[0])) {
        return "?";
    }
    return kFrameOpNames[op];
}

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

/* Look up the IR function node for a function-comment ir_node_id.
 * Returns NULL when the id is out of range or not an IR_FUNCTION
 * (defensive; verified IR always resolves). */
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

/* Find the entry function: the function named `main` declared in the
 * entry module (build->modules[0], the entry module per the IR
 * contract). Returns its node id, or -1 when not found (defensive;
 * verified IR always declares main per spec sec. 15.3). */
static int64_t find_entry_function(const IrBuild *build)
{
    size_t i;
    if (build == NULL || build->nmodules == 0 || build->modules[0] == NULL) {
        return -1;
    }
    {
        const IrNode *entry = build->modules[0];
        for (i = 0; i < entry->u.module.ndecls; i++) {
            const IrNode *d = entry->u.module.decls[i];
            if (d->kind == IR_FUNCTION && d->u.function.name != NULL &&
                strcmp(d->u.function.name, "main") == 0) {
                return d->id;
            }
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Layout computation
 * ------------------------------------------------------------------------- */

/* Compute the frame layout for one IR function. Appends one FrameLayout
 * to the output. Returns false on allocation failure. */
static bool layout_function(FrameOutput *fr, const IrNode *fn)
{
    FrameLayout layout;
    FrameSlotLayout *slot_array = NULL;
    int64_t cursor = 0;
    size_t i;
    size_t n = fn->u.function.nslots;
    if (fn->u.function.slots == NULL) {
        n = 0;   /* defensive: no slot table, no slots */
    }
    memset(&layout, 0, sizeof(layout));
    layout.function_id = fn->id;
    layout.noreturn = fn->u.function.noreturn;
    layout.has_body = fn->u.function.body != NULL;
    layout.is_entry = (fr->entry_function_id >= 0 &&
                       fn->id == fr->entry_function_id);
    layout.frame_size = 0;
    layout.slots = NULL;

    /* Bodyless external declarations (rt.proc.exit / rt.trap.report)
     * carry no frame: there is no body code to protect, so no slots and
     * no reservation (normative rule 6). */
    if (!layout.has_body) {
        layout.nslots = 0;
    } else {
        layout.nslots = n;
        if (n > 0) {
            slot_array = (FrameSlotLayout *)calloc(n, sizeof(FrameSlotLayout));
            if (slot_array == NULL) {
                return false;
            }
            layout.slots = slot_array;
        }
        for (i = 0; i < n; i++) {
            const IrSlot *slot = fn->u.function.slots[i];
            int64_t size = 8;
            int64_t align = 8;
            int64_t pad;
            if (slot != NULL && slot->type != NULL) {
                size = slot->type->size > 0 ? slot->type->size : 8;
                align = slot->type->align > 0 ? slot->type->align : 8;
            }
            /* pad so the slot's base (rbp + offset) is aligned; rbp is
             * 16-aligned after the prologue, so (cursor + size) % align
             * must be 0 */
            pad = (align - (cursor + size) % align) % align;
            cursor += pad;
            slot_array[i].slot_index = slot != NULL ? slot->index
                                                    : (int64_t)i;
            slot_array[i].offset = -(cursor + size);
            slot_array[i].size = size;
            slot_array[i].align = align;
            cursor += size;
        }
        layout.frame_size = align_up_i64(cursor, 16);
    }

    /* append to the layouts array */
    if (fr->nlayouts == fr->layouts_cap) {
        size_t ncap = fr->layouts_cap == 0 ? 8 : fr->layouts_cap * 2;
        FrameLayout *p = (FrameLayout *)realloc(
            fr->layouts, ncap * sizeof(FrameLayout));
        if (p == NULL) {
            free(layout.slots);
            return false;
        }
        fr->layouts = p;
        fr->layouts_cap = ncap;
    }
    fr->layouts[fr->nlayouts] = layout;
    fr->nlayouts++;
    return true;
}

/* ---------------------------------------------------------------------------
 * Stream emission
 * ------------------------------------------------------------------------- */

static bool emit_step(FrameOutput *fr, FrameOp op, int64_t imm)
{
    FrameInsn *p;
    if (fr->count == fr->cap) {
        size_t ncap = fr->cap == 0 ? 32 : fr->cap * 2;
        FrameInsn *q = (FrameInsn *)realloc(fr->insns, ncap * sizeof(FrameInsn));
        if (q == NULL) {
            fr->oom = true;
            return false;
        }
        fr->insns = q;
        fr->cap = ncap;
    }
    p = &fr->insns[fr->count];
    memset(p, 0, sizeof(*p));
    p->op = op;
    p->imm = imm;
    fr->count++;
    return true;
}

static bool emit_body(FrameOutput *fr, const IselInsn *insn)
{
    FrameInsn *p;
    if (fr->count == fr->cap) {
        size_t ncap = fr->cap == 0 ? 32 : fr->cap * 2;
        FrameInsn *q = (FrameInsn *)realloc(fr->insns, ncap * sizeof(FrameInsn));
        if (q == NULL) {
            fr->oom = true;
            return false;
        }
        fr->insns = q;
        fr->cap = ncap;
    }
    p = &fr->insns[fr->count];
    memset(p, 0, sizeof(*p));
    p->op = FRAME_OP_BODY;
    p->body = *insn;   /* shallow copy; note/trap pointers stay borrowed */
    fr->count++;
    return true;
}

/* Rewrite one operand: ISEL_OP_SLOT becomes a frame-base memory operand
 * (ISEL_OP_MEM, base FRAME_BASE_VREG, displacement = the slot's RBP
 * offset). The width is preserved. */
static void rewrite_operand(IselOperand *op, const FrameLayout *layout)
{
    const FrameSlotLayout *sl;
    if (op->kind != ISEL_OP_SLOT || layout == NULL) {
        return;
    }
    sl = frame_layout_slot(layout, op->id);
    if (sl == NULL) {
        /* defensive: an unknown slot id stays symbolic (cannot happen
         * on verified IR; a visible slotN in the dump is a defect
         * signal, not silent corruption) */
        return;
    }
    op->kind = ISEL_OP_MEM;
    op->vreg = FRAME_BASE_VREG;
    op->imm = sl->offset;
    /* id and width are left: width is the operand width, id is unused
     * for MEM */
}

/* Rewrite every slot operand of one body instruction in place (the
 * instruction lives in the stream as a shallow copy; the original
 * selection is not modified). */
static void rewrite_slots(IselInsn *insn, const FrameLayout *layout)
{
    rewrite_operand(&insn->dst, layout);
    rewrite_operand(&insn->src1, layout);
    rewrite_operand(&insn->src2, layout);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

FrameStatus frame_build(const IrBuild *build, const IselOutput *sel,
                        FrameOutput **out)
{
    FrameOutput *fr;
    const FrameLayout *cur = NULL;   /* current function layout, or NULL */
    size_t i;
    if (build == NULL || sel == NULL || out == NULL) {
        return FRAME_OOM;
    }
    fr = (FrameOutput *)calloc(1, sizeof(*fr));
    if (fr == NULL) {
        return FRAME_OOM;
    }
    fr->entry_function_id = find_entry_function(build);

    for (i = 0; i < sel->count; i++) {
        const IselInsn *insn = &sel->insns[i];
        bool is_function_marker = false;
        if (insn->op == ISEL_COMMENT && insn->note != NULL &&
            strncmp(insn->note, "function ", 9) == 0) {
            is_function_marker = true;
        }
        if (is_function_marker) {
            const IrNode *fn = lookup_function(build, insn->ir_node_id);
            if (fn != NULL) {
                if (!layout_function(fr, fn)) {
                    fr->oom = true;
                    break;
                }
                cur = &fr->layouts[fr->nlayouts - 1];
            } else {
                cur = NULL;   /* defensive: unknown function marker */
            }
            /* the marker comment itself passes through */
            if (!emit_body(fr, insn)) {
                break;
            }
            if (fn != NULL && cur != NULL && cur->has_body) {
                /* prologue: push rbp; mov rbp, rsp; [sub rsp, frame_size] */
                if (!emit_step(fr, FRAME_OP_PUSH_RBP, 0) ||
                    !emit_step(fr, FRAME_OP_MOV_RBP_RSP, 0)) {
                    break;
                }
                if (cur->frame_size > 0) {
                    if (!emit_step(fr, FRAME_OP_SUB_RSP, cur->frame_size)) {
                        break;
                    }
                }
            }
            continue;
        }
        /* body instruction */
        if (cur != NULL && cur->has_body && !cur->noreturn &&
            insn->op == ISEL_RET) {
            /* epilogue restore before every RET of a returning function */
            if (!emit_step(fr, FRAME_OP_MOV_RSP_RBP, 0) ||
                !emit_step(fr, FRAME_OP_POP_RBP, 0)) {
                break;
            }
        }
        {
            IselInsn copy = *insn;
            if (cur != NULL) {
                rewrite_slots(&copy, cur);
            }
            if (!emit_body(fr, &copy)) {
                break;
            }
        }
    }

    if (fr->oom) {
        frame_output_free(fr);
        *out = NULL;
        return FRAME_OOM;
    }
    *out = fr;
    return FRAME_OK;
}

void frame_output_free(FrameOutput *fr)
{
    size_t i;
    if (fr == NULL) {
        return;
    }
    for (i = 0; i < fr->nlayouts; i++) {
        free(fr->layouts[i].slots);
    }
    free(fr->layouts);
    free(fr->insns);
    free(fr);
}

size_t frame_output_count(const FrameOutput *fr)
{
    return fr != NULL ? fr->count : 0;
}

const FrameInsn *frame_output_insn(const FrameOutput *fr, size_t i)
{
    if (fr == NULL || i >= fr->count) {
        return NULL;
    }
    return &fr->insns[i];
}

size_t frame_layout_count(const FrameOutput *fr)
{
    return fr != NULL ? fr->nlayouts : 0;
}

const FrameLayout *frame_layout_at(const FrameOutput *fr, size_t i)
{
    if (fr == NULL || i >= fr->nlayouts) {
        return NULL;
    }
    return &fr->layouts[i];
}

const FrameLayout *frame_layout_for_function(const FrameOutput *fr,
                                             int64_t function_id)
{
    size_t i;
    if (fr == NULL) {
        return NULL;
    }
    for (i = 0; i < fr->nlayouts; i++) {
        if (fr->layouts[i].function_id == function_id) {
            return &fr->layouts[i];
        }
    }
    return NULL;
}

const FrameSlotLayout *frame_layout_slot(const FrameLayout *layout,
                                         int64_t slot_index)
{
    size_t i;
    if (layout == NULL || layout->slots == NULL) {
        return NULL;
    }
    for (i = 0; i < layout->nslots; i++) {
        if (layout->slots[i].slot_index == slot_index) {
            return &layout->slots[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Deterministic framed assembly dump
 * ------------------------------------------------------------------------- */

/* DiagBuf growth helpers (same shape as isel_core.c / ir_dump.c). */

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

/* Opcodes whose mnemonic carries a width suffix (same set as isel_core's
 * dump). */
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

/* Operand rendering. Frame-base memory operands (vreg == FRAME_BASE_VREG)
 * render as [rbp-<off>] / [rbp]; other memory operands as [rN] /
 * [rN+disp]. */
static bool dump_operand(DiagBuf *b, const IselOperand *op)
{
    switch (op->kind) {
    case ISEL_OP_NONE:
        return true;
    case ISEL_OP_VREG:
        return s_printf(b, "r%lld", (long long)op->vreg);
    case ISEL_OP_IMM:
        if (op->is_unsigned) {
            return s_printf(b, "$%llu", (unsigned long long)op->imm);
        }
        return s_printf(b, "$%lld", (long long)op->imm);
    case ISEL_OP_SLOT:
        /* symbolic slot after framing is a defect signal (rewritten on
         * verified IR); render it distinctly */
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
        if (op->vreg == FRAME_BASE_VREG) {
            if (op->imm == 0) {
                return s_append_cstr(b, "[rbp]");
            }
            return s_printf(b, "[rbp%+lld]", (long long)op->imm);
        }
        if (op->imm == 0) {
            return s_printf(b, "[r%lld]", (long long)op->vreg);
        }
        return s_printf(b, "[r%lld%+lld]", (long long)op->vreg,
                        (long long)op->imm);
    default:
        return false;
    }
}

/* One body instruction line, in isel_core's dump format. */
static bool dump_body_line(DiagBuf *b, const IselInsn *insn)
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
    if (insn->op == ISEL_LEA && insn->scale > 0) {
        if (!s_append_cstr(b, " (scaled)")) {
            return false;
        }
    }
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

bool frame_asm_dump(const FrameOutput *fr, DiagBuf *out)
{
    size_t i;
    if (fr == NULL || out == NULL) {
        return false;
    }
    if (!s_append_cstr(out, "; AI-Co frame layout dump "
                            "(WP-M0-17b1, deterministic)\n")) {
        return false;
    }
    if (!s_printf(out, "; functions=%zu insns=%zu entry=%lld\n",
                  fr->nlayouts, fr->count, (long long)fr->entry_function_id)) {
        return false;
    }
    for (i = 0; i < fr->count; i++) {
        const FrameInsn *fi = &fr->insns[i];
        switch (fi->op) {
        case FRAME_OP_BODY:
            if (!dump_body_line(out, &fi->body)) {
                return false;
            }
            break;
        case FRAME_OP_PUSH_RBP:
            if (!s_append_cstr(out, "  push rbp\n")) {
                return false;
            }
            break;
        case FRAME_OP_MOV_RBP_RSP:
            if (!s_append_cstr(out, "  mov rbp, rsp\n")) {
                return false;
            }
            break;
        case FRAME_OP_SUB_RSP:
            if (!s_printf(out, "  sub rsp, $%lld\n", (long long)fi->imm)) {
                return false;
            }
            break;
        case FRAME_OP_MOV_RSP_RBP:
            if (!s_append_cstr(out, "  mov rsp, rbp\n")) {
                return false;
            }
            break;
        case FRAME_OP_POP_RBP:
            if (!s_append_cstr(out, "  pop rbp\n")) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return true;
}
