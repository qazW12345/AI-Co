/* bootstrap/src/backend/trap_checked.c
 *
 * AI-Co Stage-0 x86-64 checked-operation emission with span/cause
 * preservation on trap records (WP-M0-17c2).
 *
 * Implements the obligations of trap_checked.h on top of the 17c1
 * trap-branched stream (TrapOutput), its 17b1 framed stream (FrameOutput),
 * its 17b2 physical stream (CallOutput), and the IR build: the deterministic
 * multi-instruction check sequences for the complex checked ops that 17c1
 * passed through as annotated markers (null deref, index bounds, cast
 * range, slice bounds, UTF-8 validation, pointer difference, pointer
 * arithmetic overflow), and the span/cause preservation on trap records
 * (every site carries the failing IR node's cause chain, root cause first;
 * the emitted message text is derived from code + span + causes).
 * See trap_checked.h for the normative rules (1-4) and the ownership model.
 *
 * Determinism is structural: the pass walks the 17c1 stream in emission
 * order, the framed stream in emission order, and the IR's deterministic
 * arrays; every output is a pure function of those inputs. Identical inputs
 * always yield identical checked streams and identical dump bytes (spec
 * sec. 14.2).
 */
#include "trap_checked.h"

#include "../diag/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Buffer helpers (deterministic text sink; mirrors trap_branch.c)
 * ------------------------------------------------------------------------- */

static bool c_reserve(DiagBuf *buf, size_t extra)
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

static bool c_append_n(DiagBuf *buf, const char *s, size_t n)
{
    if (!c_reserve(buf, n)) {
        return false;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return true;
}

static bool c_append_cstr(DiagBuf *buf, const char *s)
{
    return c_append_n(buf, s, strlen(s));
}

static bool c_printf(DiagBuf *buf, const char *fmt, ...)
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
        return c_append_n(buf, tmp, (size_t)n);
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
        c_append_n(buf, big, (size_t)n);
        free(big);
        return !buf->oom;
    }
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static bool is_function_marker_pseudo(const CallInsn *ci)
{
    return ci->op == CALL_OP_PSEUDO && ci->isel == ISEL_COMMENT &&
           ci->pseudo.note != NULL &&
           strncmp(ci->pseudo.note, "function ", 9) == 0;
}

static bool is_function_marker_frame(const FrameInsn *fi)
{
    return fi->op == FRAME_OP_BODY && fi->body.op == ISEL_COMMENT &&
           fi->body.note != NULL &&
           strncmp(fi->body.note, "function ", 9) == 0;
}

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

static const IrNode *lookup_node(const IrBuild *build, int64_t node_id)
{
    if (build == NULL || node_id < 0 || (size_t)node_id >= build->nnodes) {
        return NULL;
    }
    return build->nodes[node_id];
}

/* The deterministic IR node -> vreg map (mirrors call.c build_vreg_map: a
 * node's own vreg is the minimum VREG-destination vreg among instructions
 * with that ir_node_id; isel_core allocates the node's vreg first). */
static int64_t *build_vreg_map(const FrameOutput *fr, const IrBuild *build,
                               size_t *out_cap)
{
    int64_t *map;
    size_t cap = build != NULL ? build->nnodes : 0;
    size_t i;
    map = (int64_t *)malloc((cap > 0 ? cap : 1) * sizeof(int64_t));
    if (map == NULL) {
        return NULL;
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
    *out_cap = cap;
    return map;
}

/* The spill slot for a vreg: [rbp - frame_size - 8*(vreg+1)] (call.h rule
 * 1). */
static CallOperand checked_spill_mem(int64_t frame_size, int64_t vreg)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = CALL_OPR_MEM;
    op.id = (int64_t)X64_REG_RBP;
    op.imm = -(frame_size + 8 * (vreg + 1));
    op.width = 8;
    return op;
}

/* Signedness of an IR type (mirrors isel_core/trap_branch helpers). */
static bool ir_type_is_signed(const IrType *t)
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
            return ir_type_is_signed(t->u.decl->u.enum_decl.underlying);
        }
        return false;
    default:
        return false;
    }
}

static int type_width(const IrType *t)
{
    if (t == NULL) {
        return 8;
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
    default:
        return 8;
    }
}

/* The numeric u32 code for a language trap (the AIC-R suffix as hex). */
static int64_t language_code_numeric(const char *code)
{
    const char *p;
    int64_t v = 0;
    int i;
    if (code == NULL || strncmp(code, "AIC-R", 5) != 0) {
        return 0;
    }
    p = code + 5;
    for (i = 0; i < 4 && p[i] != '\0'; i++) {
        int d;
        if (p[i] >= '0' && p[i] <= '9') {
            d = p[i] - '0';
        } else if (p[i] >= 'a' && p[i] <= 'f') {
            d = p[i] - 'a' + 10;
        } else if (p[i] >= 'A' && p[i] <= 'F') {
            d = p[i] - 'A' + 10;
        } else {
            d = 0;
        }
        v = v * 16 + d;
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * Output helpers
 * ------------------------------------------------------------------------- */

static bool emit_insn(CheckedOutput *co, CheckedOp op, const TrapInsn *trap,
                      const CallOperand *dst, const CallOperand *src,
                      const CallOperand *base, CheckedCond cond,
                      int64_t imm, int64_t ir_node_id)
{
    CheckedInsn *p;
    if (co->count == co->cap) {
        size_t ncap = co->cap == 0 ? 64 : co->cap * 2;
        CheckedInsn *q =
            (CheckedInsn *)realloc(co->insns, ncap * sizeof(CheckedInsn));
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
    if (trap != NULL) {
        p->trap = *trap;
    }
    if (dst != NULL) {
        p->dst = *dst;
    }
    if (src != NULL) {
        p->src = *src;
    }
    if (base != NULL) {
        p->base = *base;
    }
    p->cond = cond;
    p->imm = imm;
    p->ir_node_id = ir_node_id;
    co->count++;
    return true;
}

static bool emit_body(CheckedOutput *co, const TrapInsn *trap)
{
    return emit_insn(co, CHK_OP_BODY, trap, NULL, NULL, NULL, CHK_COND_JMP,
                     0, trap->ir_node_id);
}

static bool emit_site_ref(CheckedOutput *co, CheckedOp op, CheckedCond cond,
                          int64_t site_index, int64_t ir_node_id)
{
    return emit_insn(co, op, NULL, NULL, NULL, NULL, cond, site_index,
                     ir_node_id);
}

static bool emit_reg_reg(CheckedOutput *co, CheckedOp op,
                         const CallOperand *dst, const CallOperand *src,
                         int64_t ir_node_id)
{
    return emit_insn(co, op, NULL, dst, src, NULL, CHK_COND_JMP, 0,
                     ir_node_id);
}

static bool emit_reg_imm(CheckedOutput *co, CheckedOp op,
                         const CallOperand *dst, int64_t imm,
                         int64_t ir_node_id)
{
    return emit_insn(co, op, NULL, dst, NULL, NULL, CHK_COND_JMP, imm,
                     ir_node_id);
}

static bool emit_idiv(CheckedOutput *co, const CallOperand *src,
                      int64_t ir_node_id)
{
    return emit_insn(co, CHK_OP_IDIV, NULL, NULL, src, NULL, CHK_COND_JMP,
                     0, ir_node_id);
}

static char *c_strdup(const char *s)
{
    size_t n;
    char *copy;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, n + 1);
    return copy;
}

static bool append_message(CheckedOutput *co, const char *text)
{
    char *copy;
    if (co->nmsgs == co->msgs_cap) {
        size_t ncap = co->msgs_cap == 0 ? 8 : co->msgs_cap * 2;
        char **q = (char **)realloc(co->msgs, ncap * sizeof(char *));
        if (q == NULL) {
            co->oom = true;
            return false;
        }
        co->msgs = q;
        co->msgs_cap = ncap;
    }
    copy = c_strdup(text);
    if (copy == NULL) {
        co->oom = true;
        return false;
    }
    co->msgs[co->nmsgs] = copy;
    co->nmsgs++;
    return true;
}

static bool append_site(CheckedFunction *cf, CheckedOutput *co,
                        const char *code, int64_t numeric_code,
                        const DiagSpan *span, const IrCauseLink *causes,
                        size_t cause_count, int64_t ir_node_id,
                        bool unconditional)
{
    CheckedSite *p;
    char msg[2048];
    int n;
    size_t ci;
    if (cf->nsites % 8 == 0) {
        CheckedSite *q = (CheckedSite *)realloc(
            cf->sites, (cf->nsites + 8) * sizeof(CheckedSite));
        if (q == NULL) {
            co->oom = true;
            return false;
        }
        cf->sites = q;
    }
    p = &cf->sites[cf->nsites];
    memset(p, 0, sizeof(*p));
    p->site_index = (int64_t)cf->nsites;
    p->code = code;
    p->numeric_code = numeric_code;
    p->span = span;
    p->causes = causes;
    p->cause_count = cause_count;
    p->ir_node_id = ir_node_id;
    p->unconditional = unconditional;
    /* deterministic message: "<code> at <file>:<line>:<col>" then one
     * "; <kind> at <file>:<line>:<col>" per cause link, root cause first
     * (DIAGNOSTIC-CONTRACT sec. 4/10, IR contract sec. 8.4) */
    if (code != NULL && strcmp(code, "user") == 0) {
        if (span != NULL && span->file != NULL) {
            n = snprintf(msg, sizeof(msg), "user trap %lld at %s:%lld:%lld",
                         (long long)numeric_code, span->file,
                         (long long)span->start.line,
                         (long long)span->start.col);
        } else {
            n = snprintf(msg, sizeof(msg), "user trap %lld",
                         (long long)numeric_code);
        }
    } else if (span != NULL && span->file != NULL) {
        n = snprintf(msg, sizeof(msg), "%s at %s:%lld:%lld",
                     code != NULL ? code : "trap", span->file,
                     (long long)span->start.line,
                     (long long)span->start.col);
    } else {
        n = snprintf(msg, sizeof(msg), "%s",
                     code != NULL ? code : "trap");
    }
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        co->oom = true;
        return false;
    }
    for (ci = 0; ci < cause_count && causes != NULL; ci++) {
        const IrCauseLink *link = &causes[ci];
        const char *kind = link->construct_kind != NULL
                               ? link->construct_kind : "?";
        const DiagSpan *lspan = link->span;
        int m;
        if (lspan != NULL && lspan->file != NULL) {
            m = snprintf(msg + n, sizeof(msg) - (size_t)n,
                         "; %s at %s:%lld:%lld", kind, lspan->file,
                         (long long)lspan->start.line,
                         (long long)lspan->start.col);
        } else {
            m = snprintf(msg + n, sizeof(msg) - (size_t)n, "; %s", kind);
        }
        if (m < 0 || (size_t)n + (size_t)m >= sizeof(msg)) {
            co->oom = true;
            return false;
        }
        n += m;
    }
    p->msg_index = (int64_t)co->nmsgs;
    if (!append_message(co, msg)) {
        return false;
    }
    cf->nsites++;
    return true;
}

static bool append_function(CheckedOutput *co, const CheckedFunction *cf)
{
    CheckedFunction *p;
    if (co->nfunctions == co->functions_cap) {
        size_t ncap = co->functions_cap == 0 ? 8 : co->functions_cap * 2;
        CheckedFunction *q = (CheckedFunction *)realloc(
            co->functions, ncap * sizeof(CheckedFunction));
        if (q == NULL) {
            co->oom = true;
            return false;
        }
        co->functions = q;
        co->functions_cap = ncap;
    }
    p = &co->functions[co->nfunctions];
    *p = *cf;
    p->sites = cf->sites;
    co->nfunctions++;
    return true;
}

/* ---------------------------------------------------------------------------
 * Operand constructors
 * ------------------------------------------------------------------------- */

static CallOperand cop_reg(X64Reg reg, int width)
{
    CallOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = CALL_OPR_REG;
    op.id = (int64_t)reg;
    op.width = width;
    return op;
}

/* ---------------------------------------------------------------------------
 * Check-sequence emission
 *
 * Each complex obligation site gets its deterministic multi-instruction
 * check sequence at the obligation body instruction (before it for the
 * pre-body codes: null deref, index bounds, cast range, slice bounds; after
 * it for the post-body codes: UTF-8, ptrdiff, pointer-arith overflow),
 * branching to the site's existing .LtrapN path. The sequences use the
 * caller-saved scratch registers (R10/R11; the 17b2 model keeps values in
 * slots, so nothing is live across instructions) and reference the IR node
 * facts and vreg slots -- never host addresses or iteration order.
 * ------------------------------------------------------------------------- */

/* Load a value into a scratch register: constants via LOAD_IMM, other IR
 * nodes via their spill slot. Returns false on allocation failure. */
static bool load_node_value(CheckedOutput *co, const IrBuild *build,
                            const CallFunction *cff, const int64_t *vreg_map,
                            size_t vreg_cap, const IrNode *node,
                            const CallOperand *dst, int64_t ir_id)
{
    (void)build;
    if (node != NULL && node->kind == IR_INT &&
        node->u.constant.value != NULL) {
        return emit_reg_imm(co, CHK_OP_LOAD_IMM, dst,
                            (int64_t)node->u.constant.value->u.int_bits,
                            ir_id);
    }
    if (node != NULL && node->id >= 0 && (size_t)node->id < vreg_cap &&
        vreg_map[node->id] >= 0) {
        CallOperand slot =
            checked_spill_mem(cff->frame_size, vreg_map[node->id]);
        return emit_insn(co, CHK_OP_MOV_LOAD, NULL, dst, NULL, &slot,
                         CHK_COND_JMP, 0, ir_id);
    }
    return true;   /* defensive: no operand available */
}

/* R0809 null deref: load the pointer, test, jz .LtrapN BEFORE the deref
 * pointer copy (the obligation MOV). */
static bool emit_null_deref_check(CheckedOutput *co, const IrBuild *build,
                                  const CallFunction *cff,
                                  const int64_t *vreg_map, size_t vreg_cap,
                                  const IrNode *node, int64_t site_index)
{
    const IrNode *ptr = NULL;
    CallOperand r10 = cop_reg(X64_REG_R10, 8);
    int64_t ir_id = node != NULL ? node->id : -1;
    if (node != NULL && node->kind == IR_DEREF) {
        ptr = node->u.deref.ptr;
    }
    if (ptr == NULL) {
        return true;   /* defensive */
    }
    if (!load_node_value(co, build, cff, vreg_map, vreg_cap, ptr, &r10,
                         ir_id) ||
        !emit_insn(co, CHK_OP_TEST, NULL, NULL, &r10, NULL, CHK_COND_JMP,
                   0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JZ, site_index, ir_id)) {
        return false;
    }
    return true;
}

/* R0807 index bounds: index < len. The index and base come from the IR
 * node; the length is the compile-time array extent or the runtime
 * pair-image length field at [base+8]. Runs BEFORE the address
 * computation. */
static bool emit_index_bounds_check(CheckedOutput *co, const IrBuild *build,
                                    const CallFunction *cff,
                                    const int64_t *vreg_map, size_t vreg_cap,
                                    const IrNode *node, int64_t site_index)
{
    const IrNode *base_node = NULL;
    const IrNode *index_node = NULL;
    const IrType *base_type = NULL;
    CallOperand r10 = cop_reg(X64_REG_R10, 8);
    CallOperand r11 = cop_reg(X64_REG_R11, 8);
    int64_t ir_id = node != NULL ? node->id : -1;
    if (node != NULL && node->kind == IR_INDEX_ADDR) {
        base_node = node->u.index_addr.base;
        index_node = node->u.index_addr.index;
        if (base_node != NULL) {
            base_type = base_node->type;
        }
    }
    if (index_node == NULL) {
        return true;   /* defensive */
    }
    if (!load_node_value(co, build, cff, vreg_map, vreg_cap, index_node,
                         &r10, ir_id)) {
        return false;
    }
    if (base_type != NULL && base_type->kind == IRT_ARRAY) {
        if (!emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10, NULL,
                       CHK_COND_JMP, base_type->u.array.extent, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index,
                           ir_id)) {
            return false;
        }
        return true;
    }
    if (base_node != NULL) {
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, base_node,
                             &r11, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_MEM, NULL, NULL, &r10, &r11,
                       CHK_COND_JMP, 8, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index,
                           ir_id)) {
            return false;
        }
        return true;
    }
    return true;
}

/* R0801 cast range: value must be representable in the target type. The
 * source is extended to 64 bits by source signedness (sign- or
 * zero-extension from the source width), then compared against the target
 * range bounds. Runs BEFORE the truncating MOV. */
static bool emit_cast_range_check(CheckedOutput *co, const IrBuild *build,
                                  const CallFunction *cff,
                                  const int64_t *vreg_map, size_t vreg_cap,
                                  const IrNode *node, int64_t site_index)
{
    const IrType *st = NULL;
    const IrType *tt = NULL;
    const IrNode *src_node = NULL;
    int ws, wt;
    bool src_signed;
    bool dst_signed;
    int64_t min_val = 0;
    int64_t max_val = 0;
    CallOperand r10 = cop_reg(X64_REG_R10, 8);
    CallOperand r11 = cop_reg(X64_REG_R11, 8);
    int64_t ir_id = node != NULL ? node->id : -1;
    if (node != NULL && node->kind == IR_CAST) {
        src_node = node->u.cast_wrap.value;
        if (src_node != NULL) {
            st = src_node->type;
        }
        tt = node->type;
    }
    if (st == NULL || tt == NULL || src_node == NULL) {
        return true;   /* defensive */
    }
    ws = type_width(st);
    wt = type_width(tt);
    src_signed = ir_type_is_signed(st);
    dst_signed = ir_type_is_signed(tt);
    if (wt >= 8) {
        return true;   /* no runtime check for 64-bit targets */
    }
    if (dst_signed) {
        min_val = -(int64_t)1 << (8 * wt - 1);
        max_val = ((int64_t)1 << (8 * wt - 1)) - 1;
    } else {
        min_val = 0;
        max_val = ((int64_t)1 << (8 * wt)) - 1;
    }
    /* extend the source to 64 bits by source signedness */
    if (ws < 8) {
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, src_node,
                             &r10, ir_id)) {
            return false;
        }
        {
            CallOperand low = cop_reg(X64_REG_R10, ws);
            if (!emit_reg_reg(co, src_signed ? CHK_OP_MOVSX : CHK_OP_MOVZX,
                              &r10, &low, ir_id)) {
                return false;
            }
        }
    } else {
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, src_node,
                             &r10, ir_id)) {
            return false;
        }
    }
    if (dst_signed) {
        if (!emit_reg_imm(co, CHK_OP_LOAD_IMM, &r11, min_val, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r10, &r11,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JL, site_index, ir_id) ||
            !emit_reg_imm(co, CHK_OP_LOAD_IMM, &r11, max_val, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r10, &r11,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JG, site_index,
                           ir_id)) {
            return false;
        }
    } else {
        if (!emit_reg_imm(co, CHK_OP_LOAD_IMM, &r11, max_val, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r10, &r11,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index,
                           ir_id)) {
            return false;
        }
    }
    return true;
}

/* R0807/R0808 slice bounds: start <= end and end <= base_len. Runs BEFORE
 * the slice pair construction. For str slices (R0808) the byte offsets must
 * additionally fall on code point boundaries; the boundary check tests that
 * the byte at data+off is not a continuation byte (0x80-0xBF). */
static bool emit_slice_check(CheckedOutput *co, const IrBuild *build,
                             const CallFunction *cff,
                             const int64_t *vreg_map, size_t vreg_cap,
                             const IrNode *node, int64_t site_index,
                             const char *code, int64_t *next_label)
{
    const IrNode *base_node = NULL;
    const IrNode *start_node = NULL;
    const IrNode *end_node = NULL;
    const IrType *base_type = NULL;
    CallOperand r10 = cop_reg(X64_REG_R10, 8);
    CallOperand r11 = cop_reg(X64_REG_R11, 8);
    int64_t ir_id = node != NULL ? node->id : -1;
    if (node != NULL && node->kind == IR_SLICE) {
        base_node = node->u.slice.base;
        start_node = node->u.slice.start;
        end_node = node->u.slice.end;
        if (base_node != NULL) {
            base_type = base_node->type;
        }
    }
    if (base_node == NULL) {
        return true;
    }
    /* base length into R11: array extent constant or [base+8] runtime */
    if (base_type != NULL && base_type->kind == IRT_ARRAY) {
        if (!emit_reg_imm(co, CHK_OP_LOAD_IMM, &r11,
                          base_type->u.array.extent, ir_id)) {
            return false;
        }
    } else {
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, base_node,
                             &r10, ir_id) ||
            !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r11, NULL, &r10,
                       CHK_COND_JMP, 8, ir_id)) {
            return false;
        }
    }
    /* end <= base_len */
    if (end_node != NULL) {
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, end_node,
                             &r10, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r10, &r11,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id)) {
            return false;
        }
    }
    /* start <= end (and start <= base_len when end is absent) */
    if (start_node != NULL) {
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, start_node,
                             &r10, ir_id)) {
            return false;
        }
        if (end_node != NULL) {
            if (!load_node_value(co, build, cff, vreg_map, vreg_cap,
                                 end_node, &r11, ir_id)) {
                return false;
            }
        } else {
            /* end absent: reuse base_len (already in R11) */
        }
        if (!emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r10, &r11,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id)) {
            return false;
        }
    }
    /* R0808 code-point boundary: the byte at data+start must not be a
     * continuation byte (0x80-0xBF). data = [pair]; byte at [data+start].
     * The skip label is a loop-style label id (reused counter). */
    if (code != NULL && strcmp(code, "AIC-R0808") == 0 &&
        start_node != NULL) {
        int64_t l_skip = (*next_label)++;
        CallOperand r11b = cop_reg(X64_REG_R11, 1);
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, base_node,
                             &r10, ir_id) ||
            !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10, NULL, &r10,
                       CHK_COND_JMP, 0, ir_id)) {
            return false;
        }
        if (!load_node_value(co, build, cff, vreg_map, vreg_cap, start_node,
                             &r11, ir_id)) {
            return false;
        }
        /* byte at [data + start] (1-byte load into r11b) */
        if (!emit_insn(co, CHK_OP_ADD_REG, NULL, &r10, &r11, NULL,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r11b, NULL, &r10,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r11b, NULL,
                       CHK_COND_JMP, 0x80, ir_id) ||
            !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                       CHK_COND_JB, l_skip, ir_id) ||
            !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r11b, NULL,
                       CHK_COND_JMP, 0xBF, ir_id) ||
            !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                       CHK_COND_JA, l_skip, ir_id) ||
            !emit_site_ref(co, CHK_OP_JMP, CHK_COND_JMP, site_index,
                           ir_id) ||
            !emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                       CHK_COND_JMP, l_skip, ir_id)) {
            return false;
        }
    }
    return true;
}

/* R0806 UTF-8 validation: validate the byte slice [data, data+len) is
 * valid UTF-8. Runs AFTER the ISEL_UTF8 marker. Deterministic loop over the
 * bytes (leading byte classes, continuation bytes 0x80-0xBF, overlong /
 * surrogate / out-of-range rejections). The pair image for the u8[] slice
 * is at the marker's src1 vreg slot: [pair+0] = data ptr, [pair+8] = len.
 * Register use: r9 = p, r11 = end (data+len), r8 = current byte, r10 =
 * scratch byte. All caller-saved; the 17b2 model keeps values in slots, so
 * nothing is live across the marker. */
static bool emit_utf8_check(CheckedOutput *co, const IrBuild *build,
                            const CallFunction *cff,
                            const int64_t *vreg_map, size_t vreg_cap,
                            const IrNode *node, const CallInsn *body,
                            int64_t site_index, int64_t *next_label)
{
    int64_t src_vreg = body->pseudo.src1.vreg;
    (void)build;
    (void)vreg_map;
    (void)vreg_cap;
    (void)node;
    CallOperand r8b = cop_reg(X64_REG_R8, 1);
    CallOperand r9 = cop_reg(X64_REG_R9, 8);
    CallOperand r10 = cop_reg(X64_REG_R10, 8);
    CallOperand r10b = cop_reg(X64_REG_R10, 1);
    CallOperand r11 = cop_reg(X64_REG_R11, 8);
    int64_t ir_id = body->ir_node_id;
    int64_t l_loop = (*next_label)++;
    int64_t l_ascii = (*next_label)++;
    int64_t l_seq2 = (*next_label)++;
    int64_t l_seq3 = (*next_label)++;
    int64_t l_seq4 = (*next_label)++;
    int64_t l_sk1 = (*next_label)++;
    int64_t l_sk2 = (*next_label)++;
    int64_t l_sk3 = (*next_label)++;
    int64_t l_sk4 = (*next_label)++;
    int64_t l_done = (*next_label)++;
    CallOperand slot = checked_spill_mem(cff->frame_size, src_vreg);
    /* setup: r10 = pair addr; r11 = data; r10 = len; r9 = p (data);
     * r11 = end (data + len) */
    if (!emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10, NULL, &slot,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r11, NULL, &r10,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10, NULL, &r10,
                   CHK_COND_JMP, 8, ir_id) ||
        !emit_reg_reg(co, CHK_OP_MOV_REG, &r9, &r11, ir_id) ||
        !emit_insn(co, CHK_OP_ADD_REG, NULL, &r11, &r10, NULL,
                   CHK_COND_JMP, 0, ir_id)) {
        return false;
    }
    /* .loop: while (p < end) */
    if (!emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_loop, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JAE, l_done, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r8b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id)) {
        return false;
    }
    /* b0 in r8b: ASCII (<0x80); then 2/3/4-byte classes */
    if (!emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JB, l_ascii, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xC2, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xE0, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JB, l_seq2, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xF0, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JB, l_seq3, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xF5, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JB, l_seq4, ir_id) ||
        !emit_site_ref(co, CHK_OP_JMP, CHK_COND_JMP, site_index, ir_id)) {
        return false;
    }
    /* .ascii: p++ */
    if (!emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_ascii, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JMP, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_loop, ir_id)) {
        return false;
    }
    /* .seq2: 2-byte (b0 in 0xC2..0xDF): one continuation byte in
     * 0x80..0xBF at [p+1]; p += 2 */
    if (!emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_seq2, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xBF, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JMP, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_loop, ir_id)) {
        return false;
    }
    /* .seq3: 3-byte (b0 in 0xE0..0xEF): two continuation bytes; overlong
     * (b0==0xE0 && b1<0xA0) and surrogate (b0==0xED && b1>0x9F) rejected */
    if (!emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_seq3, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xBF, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        /* overlong: b0==0xE0 && b1<0xA0 */
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xE0, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JNZ, l_sk1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xA0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        /* surrogate: b0==0xED && b1>0x9F */
        !emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_sk1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xED, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JNZ, l_sk2, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x9F, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        /* second continuation byte at [p+2]; p += 3 */
        !emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_sk2, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xBF, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JMP, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_loop, ir_id)) {
        return false;
    }
    /* .seq4: 4-byte (b0 in 0xF0..0xF4): three continuation bytes; overlong
     * (b0==0xF0 && b1<0x90) and >U+10FFFF (b0==0xF4 && b1>0x8F) rejected */
    if (!emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_seq4, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xBF, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        /* overlong: b0==0xF0 && b1<0x90 */
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xF0, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JNZ, l_sk3, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x90, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        /* >U+10FFFF: b0==0xF4 && b1>0x8F */
        !emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_sk3, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r8b, NULL,
                   CHK_COND_JMP, 0xF4, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JCC, NULL, NULL, NULL, NULL,
                   CHK_COND_JNZ, l_sk4, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x8F, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        /* second continuation byte at [p+2] */
        !emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_sk4, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xBF, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        /* third continuation byte at [p+3]; p += 4 */
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_REG, NULL, NULL, &r9, &r11,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JAE, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10b, NULL, &r9,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0x80, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JB, site_index, ir_id) ||
        !emit_insn(co, CHK_OP_CMP_IMM, NULL, NULL, &r10b, NULL,
                   CHK_COND_JMP, 0xBF, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JA, site_index, ir_id) ||
        !emit_reg_imm(co, CHK_OP_ADD_IMM, &r9, 1, ir_id) ||
        !emit_insn(co, CHK_OP_LOOP_JMP, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_loop, ir_id) ||
        /* .done: valid; fall through */
        !emit_insn(co, CHK_OP_LOOP_LABEL, NULL, NULL, NULL, NULL,
                   CHK_COND_JMP, l_done, ir_id)) {
        return false;
    }
    return true;
}

/* R0810 ptrdiff: byte_diff % elem_size == 0. Power-of-two element sizes
 * use test-and-mask; other sizes use the idiv remainder check. Runs AFTER
 * the ISEL_PTRDIFF marker; the byte difference is in the marker's dst vreg
 * slot. */
static bool emit_ptrdiff_check(CheckedOutput *co, const CallFunction *cff,
                               const CallInsn *body, int64_t site_index)
{
    int64_t elem = body->pseudo.src2.kind == ISEL_OP_IMM
                       ? body->pseudo.src2.imm : 0;
    int64_t dvreg = body->pseudo.dst.vreg;
    CallOperand slot = checked_spill_mem(cff->frame_size, dvreg);
    CallOperand rax = cop_reg(X64_REG_RAX, 8);
    CallOperand rdx = cop_reg(X64_REG_RDX, 8);
    CallOperand r10 = cop_reg(X64_REG_R10, 8);
    CallOperand r11 = cop_reg(X64_REG_R11, 8);
    bool power_of_two;
    int64_t ir_id = body->ir_node_id;
    if (elem <= 1) {
        return true;   /* always divisible */
    }
    power_of_two = (elem & (elem - 1)) == 0;
    if (power_of_two) {
        if (!emit_insn(co, CHK_OP_MOV_LOAD, NULL, &r10, NULL, &slot,
                       CHK_COND_JMP, 0, ir_id) ||
            !emit_reg_imm(co, CHK_OP_AND_IMM, &r10, elem - 1, ir_id) ||
            !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JNZ, site_index,
                           ir_id)) {
            return false;
        }
        return true;
    }
    /* general element size: signed remainder via idiv (RDX:RAX / R11;
     * remainder in RDX) */
    if (!emit_insn(co, CHK_OP_MOV_LOAD, NULL, &rax, NULL, &slot,
                   CHK_COND_JMP, 0, ir_id) ||
        !emit_reg_reg(co, CHK_OP_MOV_REG, &rdx, &rax, ir_id) ||
        !emit_reg_imm(co, CHK_OP_SAR_IMM, &rdx, 63, ir_id) ||
        !emit_reg_imm(co, CHK_OP_LOAD_IMM, &r11, elem, ir_id) ||
        !emit_idiv(co, &r11, ir_id) ||
        !emit_insn(co, CHK_OP_TEST, NULL, NULL, &rdx, NULL, CHK_COND_JMP,
                   0, ir_id) ||
        !emit_site_ref(co, CHK_OP_JCC, CHK_COND_JNZ, site_index, ir_id)) {
        return false;
    }
    return true;
}

/* R0816 pointer arithmetic overflow: unsigned carry after the ADD/SUB
 * (addresses are unsigned). Runs AFTER the obligation body. */
static bool emit_ptr_arith_overflow_check(CheckedOutput *co,
                                          int64_t site_index,
                                          int64_t ir_node_id)
{
    return emit_site_ref(co, CHK_OP_JCC, CHK_COND_JC, site_index,
                         ir_node_id);
}

/* ---------------------------------------------------------------------------
 * Obligation scan over the framed stream (mirrors trap_branch.c)
 * ------------------------------------------------------------------------- */

typedef struct FrameObligation {
    IselOpcode opcode;
    int64_t ir_node_id;
    const char *code;
} FrameObligation;

typedef struct ObligationList {
    FrameObligation *items;
    size_t count;
    size_t cap;
} ObligationList;

static bool obl_push(ObligationList *l, FrameObligation o)
{
    if (l->count == l->cap) {
        size_t ncap = l->cap == 0 ? 8 : l->cap * 2;
        FrameObligation *q = (FrameObligation *)realloc(
            l->items, ncap * sizeof(FrameObligation));
        if (q == NULL) {
            return false;
        }
        l->items = q;
        l->cap = ncap;
    }
    l->items[l->count] = o;
    l->count++;
    return true;
}

static void obl_free(ObligationList *l)
{
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static bool collect_obligations(const FrameOutput *fr, size_t start,
                                size_t end, ObligationList *out)
{
    size_t i;
    out->count = 0;
    out->cap = 0;
    out->items = NULL;
    for (i = start; i < end; i++) {
        const FrameInsn *fi = &fr->insns[i];
        FrameObligation o;
        if (fi->op != FRAME_OP_BODY || fi->body.trap == NULL) {
            continue;
        }
        o.opcode = fi->body.op;
        o.ir_node_id = fi->body.ir_node_id;
        o.code = fi->body.trap;
        if (!obl_push(out, o)) {
            obl_free(out);
            return false;
        }
    }
    return true;
}

/* True when a site's code is a complex checked op that this package emits a
 * check sequence for (17c1 passed these through as annotated markers). */
static bool is_complex_code(const char *code)
{
    if (code == NULL) {
        return false;
    }
    return strcmp(code, "AIC-R0801") == 0 ||
           strcmp(code, "AIC-R0806") == 0 ||
           strcmp(code, "AIC-R0807") == 0 ||
           strcmp(code, "AIC-R0808") == 0 ||
           strcmp(code, "AIC-R0809") == 0 ||
           strcmp(code, "AIC-R0810") == 0 ||
           strcmp(code, "AIC-R0816") == 0;
}

/* The physical opcode family a site's obligation body belongs to, with the
 * same dst-kind rules 17c1 used to attach the site (mirrors trap_branch.c
 * lines 814-873). Returns true when `ci` is the obligation body for
 * `site`. Unknown codes (defensive) never match, so the cursor does not
 * advance past a site whose body was not seen. */
static bool site_matches_body(const CheckedSite *site, const CallInsn *ci)
{
    const char *code = site->code;
    if (site->unconditional) {
        return ci->op == CALL_OP_PSEUDO && ci->isel == ISEL_TRAP;
    }
    if (code == NULL) {
        return false;
    }
    if (strcmp(code, "AIC-R0802") == 0) {
        return ci->isel == ISEL_ADD || ci->isel == ISEL_SUB ||
               ci->isel == ISEL_IMUL || ci->isel == ISEL_NEG;
    }
    if (strcmp(code, "AIC-R0803") == 0) {
        return ci->isel == ISEL_IDIV;
    }
    if (strcmp(code, "AIC-R0804") == 0) {
        return ci->isel == ISEL_SHL || ci->isel == ISEL_SHR ||
               ci->isel == ISEL_SAR;
    }
    if (strcmp(code, "AIC-R0805") == 0) {
        return ci->isel == ISEL_MOV && ci->dst.kind == CALL_OPR_REG &&
               ci->dst.width == 1;
    }
    if (strcmp(code, "AIC-R0806") == 0) {
        return ci->isel == ISEL_UTF8;
    }
    if (strcmp(code, "AIC-R0807") == 0) {
        return ci->isel == ISEL_LEA || ci->isel == ISEL_ADD ||
               ci->isel == ISEL_SLICE;
    }
    if (strcmp(code, "AIC-R0808") == 0) {
        return ci->isel == ISEL_SLICE;
    }
    if (strcmp(code, "AIC-R0809") == 0) {
        return ci->isel == ISEL_MOV && ci->dst.kind == CALL_OPR_REG;
    }
    if (strcmp(code, "AIC-R0810") == 0) {
        return ci->isel == ISEL_PTRDIFF;
    }
    if (strcmp(code, "AIC-R0816") == 0) {
        return ci->isel == ISEL_ADD || ci->isel == ISEL_SUB;
    }
    return false;
}

/* True when the check sequence for `code` runs AFTER the obligation body
 * (the body executes first; the branch then tests its result/flag). All
 * other complex codes run BEFORE the body. */
static bool code_is_post_check(const char *code)
{
    if (code == NULL) {
        return false;
    }
    return strcmp(code, "AIC-R0806") == 0 ||   /* UTF-8 validation */
           strcmp(code, "AIC-R0810") == 0 ||   /* ptrdiff divisibility */
           strcmp(code, "AIC-R0816") == 0;     /* ptr-arith overflow (jc) */
}

/* ---------------------------------------------------------------------------
 * Cast-range discovery
 *
 * isel_core emits narrowing conversions as a plain MOV with no trap
 * annotation (the obligation lives on the IR_CAST node's trap_code), so
 * 17c1's framed-stream scan cannot discover them. This pass scans the IR
 * directly for IR_CAST nodes whose conversion can fail at runtime and whose
 * physical MOV appears in this function's region.
 * ------------------------------------------------------------------------- */

static bool cast_needs_check(const IrNode *node)
{
    const IrType *st;
    const IrType *tt;
    if (node == NULL || node->kind != IR_CAST) {
        return false;
    }
    if (node->trap_code == NULL ||
        strcmp(node->trap_code, "AIC-R0801") != 0) {
        return false;
    }
    st = node->u.cast_wrap.value != NULL ? node->u.cast_wrap.value->type
                                         : NULL;
    tt = node->type;
    if (st == NULL || tt == NULL || tt->kind == IRT_STR) {
        return false;
    }
    if (type_width(tt) < type_width(st)) {
        return true;    /* narrowing */
    }
    if (type_width(tt) == type_width(st) &&
        ir_type_is_signed(st) != ir_type_is_signed(tt)) {
        return true;    /* same-width signedness change */
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static int64_t find_report_function(const IrBuild *build)
{
    size_t i;
    if (build == NULL) {
        return -1;
    }
    for (i = 0; i < build->nnodes; i++) {
        const IrNode *n = build->nodes[i];
        if (n != NULL && n->kind == IR_FUNCTION &&
            n->u.function.name != NULL &&
            strcmp(n->u.function.name, "rt.trap.report") == 0) {
            return (int64_t)n->id;
        }
    }
    return -1;
}

/* Emit the trap path for one site (label + shadow + code + message +
 * report call) -- mirrors trap_branch.c's emit_trap_paths. */
static bool emit_site_path(CheckedOutput *co, const CheckedSite *site,
                           int64_t report_id)
{
    const char *msg;
    int64_t len;
    if (!emit_insn(co, CHK_OP_LABEL, NULL, NULL, NULL, NULL, CHK_COND_JMP,
                   site->site_index, site->ir_node_id)) {
        return false;
    }
    if (!emit_reg_imm(co, CHK_OP_SUB_RSP, NULL, 32, site->ir_node_id)) {
        return false;
    }
    if (!emit_reg_imm(co, CHK_OP_MOV_CODE, NULL, site->numeric_code,
                      site->ir_node_id)) {
        return false;
    }
    if (!emit_insn(co, CHK_OP_LEA_MSG, NULL, NULL, NULL, NULL, CHK_COND_JMP,
                   site->msg_index, site->ir_node_id)) {
        return false;
    }
    msg = (site->msg_index >= 0 && (size_t)site->msg_index < co->nmsgs)
              ? co->msgs[site->msg_index] : NULL;
    len = msg != NULL ? (int64_t)strlen(msg) : 0;
    if (!emit_reg_imm(co, CHK_OP_MOV_LEN, NULL, len, site->ir_node_id)) {
        return false;
    }
    return emit_insn(co, CHK_OP_CALL_REPORT, NULL, NULL, NULL, NULL,
                     CHK_COND_JMP, report_id, site->ir_node_id);
}

CheckedStatus trap_checked_build(const IrBuild *build, const FrameOutput *fr,
                                 const CallOutput *co, const TrapOutput *to,
                                 CheckedOutput **out)
{
    CheckedOutput *cho;
    size_t i;
    size_t fi;
    int64_t *vreg_map = NULL;
    size_t vreg_cap = 0;
    int64_t next_label = 0;
    if (build == NULL || fr == NULL || co == NULL || to == NULL ||
        out == NULL) {
        return CHK_OOM;
    }
    cho = (CheckedOutput *)calloc(1, sizeof(*cho));
    if (cho == NULL) {
        return CHK_OOM;
    }
    cho->entry_function_id = to->entry_function_id;
    cho->report_function_id = to->report_function_id;
    if (cho->report_function_id < 0) {
        cho->report_function_id = find_report_function(build);
    }
    vreg_map = build_vreg_map(fr, build, &vreg_cap);
    if (vreg_map == NULL && build != NULL && build->nnodes > 0) {
        checked_output_free(cho);
        return CHK_OOM;
    }

    fi = 0;
    i = 0;
    while (i < to->count) {
        const TrapInsn *ti = &to->insns[i];
        if (!(ti->op == TRAP_OP_BODY &&
              is_function_marker_pseudo(&ti->call))) {
            if (!emit_body(cho, ti)) {
                cho->oom = true;
                break;
            }
            i++;
            continue;
        }
        {
            const IrNode *fn = lookup_function(build, ti->call.ir_node_id);
            CheckedFunction cf;
            ObligationList obl;
            size_t region_start = i + 1;
            size_t region_end = region_start;
            size_t frame_region_start;
            size_t frame_region_end;
            size_t j;
            size_t site_cursor = 0;   /* next 17c1 site to process */
            const CallFunction *cff = NULL;
            if (cho->nfunctions > 0) {
                CheckedFunction *prev = &cho->functions[cho->nfunctions - 1];
                prev->count = cho->count - prev->start;
            }
            memset(&cf, 0, sizeof(cf));
            cf.function_id = fn != NULL ? fn->id : ti->call.ir_node_id;
            cf.start = cho->count;
            cf.count = 0;
            if (fn != NULL) {
                while (fi < fr->count) {
                    const FrameInsn *fmark = &fr->insns[fi];
                    if (is_function_marker_frame(fmark) &&
                        fmark->body.ir_node_id == fn->id) {
                        break;
                    }
                    fi++;
                }
                frame_region_start = fi + 1;
                frame_region_end = frame_region_start;
                while (frame_region_end < fr->count) {
                    const FrameInsn *nf = &fr->insns[frame_region_end];
                    if (is_function_marker_frame(nf)) {
                        break;
                    }
                    frame_region_end++;
                }
                if (!collect_obligations(fr, frame_region_start,
                                         frame_region_end, &obl)) {
                    cho->oom = true;
                    break;
                }
                fi = frame_region_end;
                cff = call_function_for(co, cf.function_id);
            } else {
                memset(&obl, 0, sizeof(obl));
            }
            if (!emit_body(cho, ti)) {
                cho->oom = true;
                break;
            }
            while (region_end < to->count &&
                   !(to->insns[region_end].op == TRAP_OP_BODY &&
                     is_function_marker_pseudo(&to->insns[region_end].call))) {
                region_end++;
            }
            /* copy the 17c1 site plan (span + causes) into the checked
             * plan; cast-range sites are appended as discovered below */
            {
                size_t tf_idx;
                for (tf_idx = 0; tf_idx < to->nfunctions; tf_idx++) {
                    if (to->functions[tf_idx].function_id == cf.function_id) {
                        break;
                    }
                }
                if (tf_idx < to->nfunctions) {
                    const TrapFunction *tf = &to->functions[tf_idx];
                    size_t s;
                    for (s = 0; s < tf->nsites; s++) {
                        const TrapSite *ts = &tf->sites[s];
                        const IrNode *n = lookup_node(build, ts->ir_node_id);
                        const IrCauseLink *causes =
                            n != NULL ? n->causes : NULL;
                        size_t cause_count = n != NULL ? n->cause_count : 0;
                        if (!append_site(&cf, cho, ts->code,
                                         ts->numeric_code, ts->span,
                                         causes, cause_count,
                                         ts->ir_node_id,
                                         ts->unconditional)) {
                            cho->oom = true;
                            break;
                        }
                    }
                }
            }
            if (cho->oom) {
                break;
            }
            /* Walk the region: copy bodies; emit check sequences at the
             * complex obligation bodies; drop 17c1's trap paths (re-emitted
             * below with cause-carrying messages). Cast-range sites are
             * discovered inline: a narrowing MOV whose IR node is an
             * IR_CAST with trap_code R0801. */
            for (j = region_start; j < region_end; j++) {
                const TrapInsn *pt = &to->insns[j];
                if (pt->op == TRAP_OP_BODY) {
                    const IrNode *n = lookup_node(build, pt->ir_node_id);
                    bool is_obligation = false;
                    bool cast_site = false;
                    if (site_cursor < cf.nsites) {
                        const CheckedSite *site = &cf.sites[site_cursor];
                        if (site->ir_node_id == pt->ir_node_id &&
                            site_matches_body(site, &pt->call)) {
                            is_obligation = true;
                        }
                    }
                    /* cast-range discovery (not in the 17c1 plan): the
                     * first MOV body whose IR node is a runtime-failable
                     * IR_CAST becomes a new site + check sequence */
                    if (!is_obligation && n != NULL &&
                        cast_needs_check(n) &&
                        pt->call.op == CALL_OP_BODY &&
                        pt->call.isel == ISEL_MOV) {
                        size_t s;
                        bool already = false;
                        for (s = 0; s < cf.nsites; s++) {
                            if (cf.sites[s].ir_node_id == pt->ir_node_id) {
                                already = true;
                                break;
                            }
                        }
                        if (!already) {
                            cast_site = true;
                        }
                    }
                    if (is_obligation || cast_site) {
                        const CheckedSite *site;
                        const char *code;
                        int64_t site_index;
                        if (is_obligation) {
                            site = &cf.sites[site_cursor];
                            site_index = site->site_index;
                        } else {
                            const IrCauseLink *causes = n->causes;
                            size_t cause_count = n->cause_count;
                            site_index = (int64_t)cf.nsites;
                            if (!append_site(&cf, cho, "AIC-R0801",
                                             language_code_numeric(
                                                 "AIC-R0801"),
                                             n->span, causes, cause_count,
                                             n->id, false)) {
                                cho->oom = true;
                                break;
                            }
                            site = &cf.sites[cf.nsites - 1];
                        }
                        code = site->code;
                        /* post-check codes: the body first, then the check
                         * (the branch tests the body's result/flag) */
                        if (code_is_post_check(code) && is_obligation) {
                            if (!emit_body(cho, pt)) {
                                cho->oom = true;
                                break;
                            }
                        }
                        if (is_complex_code(code) && !site->unconditional) {
                            if (strcmp(code, "AIC-R0801") == 0) {
                                if (cff != NULL &&
                                    !emit_cast_range_check(
                                        cho, build, cff, vreg_map,
                                        vreg_cap, n, site_index)) {
                                    cho->oom = true;
                                    break;
                                }
                            } else if (strcmp(code, "AIC-R0809") == 0) {
                                if (cff != NULL &&
                                    !emit_null_deref_check(
                                        cho, build, cff, vreg_map,
                                        vreg_cap, n, site_index)) {
                                    cho->oom = true;
                                    break;
                                }
                            } else if (strcmp(code, "AIC-R0807") == 0 &&
                                       n != NULL &&
                                       n->kind == IR_INDEX_ADDR) {
                                if (cff != NULL &&
                                    !emit_index_bounds_check(
                                        cho, build, cff, vreg_map,
                                        vreg_cap, n, site_index)) {
                                    cho->oom = true;
                                    break;
                                }
                            } else if (strcmp(code, "AIC-R0806") == 0) {
                                if (cff != NULL &&
                                    !emit_utf8_check(
                                        cho, build, cff, vreg_map,
                                        vreg_cap, n, &pt->call,
                                        site_index, &next_label)) {
                                    cho->oom = true;
                                    break;
                                }
                            } else if (strcmp(code, "AIC-R0810") == 0) {
                                if (cff != NULL &&
                                    !emit_ptrdiff_check(
                                        cho, cff, &pt->call,
                                        site_index)) {
                                    cho->oom = true;
                                    break;
                                }
                            } else if ((strcmp(code, "AIC-R0807") == 0 ||
                                        strcmp(code, "AIC-R0808") == 0) &&
                                       n != NULL && n->kind == IR_SLICE) {
                                if (cff != NULL &&
                                    !emit_slice_check(
                                        cho, build, cff, vreg_map,
                                        vreg_cap, n, site_index, code,
                                        &next_label)) {
                                    cho->oom = true;
                                    break;
                                }
                            } else if (strcmp(code, "AIC-R0816") == 0) {
                                if (!emit_ptr_arith_overflow_check(
                                        cho, site_index, pt->ir_node_id)) {
                                    cho->oom = true;
                                    break;
                                }
                            }
                        }
                        if (cho->oom) {
                            break;
                        }
                        if (is_obligation) {
                            site_cursor++;
                        }
                        /* cast-range: the check ran before the body (the
                         * narrowing MOV must not execute on an out-of-range
                         * value) */
                        if (!code_is_post_check(code) || cast_site) {
                            if (!emit_body(cho, pt)) {
                                cho->oom = true;
                                break;
                            }
                        }
                        continue;
                    }
                    if (cho->oom) {
                        break;
                    }
                    if (!emit_body(cho, pt)) {
                        cho->oom = true;
                        break;
                    }
                    continue;
                }
                /* trap-path entries are re-emitted from the site plan */
                if (pt->op == TRAP_OP_LABEL || pt->op == TRAP_OP_SUB_RSP ||
                    pt->op == TRAP_OP_MOV_CODE || pt->op == TRAP_OP_LEA_MSG ||
                    pt->op == TRAP_OP_MOV_LEN ||
                    pt->op == TRAP_OP_CALL_REPORT) {
                    continue;
                }
                if (!emit_body(cho, pt)) {
                    cho->oom = true;
                    break;
                }
            }
            if (cho->oom) {
                break;
            }
            /* trap paths for all sites (17c1 + cast-range), rebuilt with
             * the cause-carrying messages */
            for (j = 0; j < cf.nsites; j++) {
                if (!emit_site_path(cho, &cf.sites[j],
                                    cho->report_function_id)) {
                    cho->oom = true;
                    break;
                }
            }
            if (cho->oom) {
                break;
            }
            cf.count = cho->count - cf.start;
            if (!append_function(cho, &cf)) {
                cho->oom = true;
                break;
            }
            obl_free(&obl);
            i = region_end;
            continue;
        }
    }
    free(vreg_map);
    if (cho->nfunctions > 0) {
        cho->functions[cho->nfunctions - 1].count =
            cho->count - cho->functions[cho->nfunctions - 1].start;
    }
    if (cho->oom) {
        checked_output_free(cho);
        *out = NULL;
        return CHK_OOM;
    }
    *out = cho;
    return CHK_OK;
}

void checked_output_free(CheckedOutput *out)
{
    size_t i;
    size_t f;
    if (out == NULL) {
        return;
    }
    free(out->insns);
    for (f = 0; f < out->nfunctions; f++) {
        free(out->functions[f].sites);
    }
    free(out->functions);
    for (i = 0; i < out->nmsgs; i++) {
        free(out->msgs[i]);
    }
    free(out->msgs);
    free(out);
}

size_t checked_output_count(const CheckedOutput *out)
{
    return out != NULL ? out->count : 0;
}

const CheckedInsn *checked_output_insn(const CheckedOutput *out, size_t i)
{
    if (out == NULL || i >= out->count) {
        return NULL;
    }
    return &out->insns[i];
}

size_t checked_function_count(const CheckedOutput *out)
{
    return out != NULL ? out->nfunctions : 0;
}

const CheckedFunction *checked_function_at(const CheckedOutput *out, size_t i)
{
    if (out == NULL || i >= out->nfunctions) {
        return NULL;
    }
    return &out->functions[i];
}

const CheckedFunction *checked_function_for(const CheckedOutput *out,
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

const CheckedSite *checked_function_site(const CheckedFunction *cf, size_t i)
{
    if (cf == NULL || i >= cf->nsites) {
        return NULL;
    }
    return &cf->sites[i];
}

const char *checked_message(const CheckedOutput *out, size_t msg_index)
{
    if (out == NULL || msg_index >= out->nmsgs) {
        return NULL;
    }
    return out->msgs[msg_index];
}

const char *checked_op_text(CheckedOp op)
{
    switch (op) {
    case CHK_OP_BODY:        return "body";
    case CHK_OP_LABEL:       return "label";
    case CHK_OP_JMP:         return "jmp";
    case CHK_OP_JCC:         return "jcc";
    case CHK_OP_TEST:        return "test";
    case CHK_OP_CMP_IMM:     return "cmp";
    case CHK_OP_CMP_REG:     return "cmp";
    case CHK_OP_CMP_MEM:     return "cmp";
    case CHK_OP_MOV_LOAD:    return "mov";
    case CHK_OP_MOV_REG:     return "mov";
    case CHK_OP_LOAD_IMM:    return "mov";
    case CHK_OP_MOVSX:       return "movsx";
    case CHK_OP_MOVZX:       return "movzx";
    case CHK_OP_AND_IMM:     return "and";
    case CHK_OP_SHR_IMM:     return "shr";
    case CHK_OP_SAR_IMM:     return "sar";
    case CHK_OP_ADD_IMM:     return "add";
    case CHK_OP_ADD_REG:     return "add";
    case CHK_OP_IDIV:        return "idiv";
    case CHK_OP_LOOP_LABEL:  return "label";
    case CHK_OP_LOOP_JCC:    return "jcc";
    case CHK_OP_LOOP_JMP:    return "jmp";
    case CHK_OP_SUB_RSP:     return "sub";
    case CHK_OP_MOV_CODE:    return "mov";
    case CHK_OP_LEA_MSG:     return "lea";
    case CHK_OP_MOV_LEN:     return "mov";
    case CHK_OP_CALL_REPORT: return "call";
    default:                 return "?";
    }
}

const char *checked_cond_text(CheckedCond cond)
{
    switch (cond) {
    case CHK_COND_JZ:  return "jz";
    case CHK_COND_JNZ: return "jnz";
    case CHK_COND_JA:  return "ja";
    case CHK_COND_JAE: return "jae";
    case CHK_COND_JB:  return "jb";
    case CHK_COND_JC:  return "jc";
    case CHK_COND_JL:  return "jl";
    case CHK_COND_JG:  return "jg";
    case CHK_COND_JMP: return "jmp";
    default:           return "?";
    }
}

/* ---------------------------------------------------------------------------
 * Deterministic checked assembly dump
 *
 * Mirrors the 17c1 dump format (trap_branch.c): the plan header, then per
 * function the pass-through stream (bodies rendered in the 17b2 format)
 * with the check sequences interleaved and the trap paths, then the
 * per-site plan with span and cause chains, then the message constants.
 * Byte-deterministic: identical inputs produce identical dump bytes.
 * ------------------------------------------------------------------------- */

/* Register names (mirrors call.c / trap_branch.c). */
static const char *const kRegNames[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char *checked_reg_name(int64_t reg, int width)
{
    static const char *const kBytes[16] = {
        "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
        "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
    };
    static const char *const kWds[16] = {
        "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
        "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
    };
    static const char *const kDws[16] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
    };
    if (reg < 0 || (size_t)reg >= 16) {
        return "?";
    }
    switch (width) {
    case 1: return kBytes[reg];
    case 2: return kWds[reg];
    case 4: return kDws[reg];
    default: return kRegNames[reg];
    }
}

static bool dump_operand(DiagBuf *b, const CallOperand *op)
{
    switch (op->kind) {
    case CALL_OPR_NONE:
        return true;
    case CALL_OPR_REG:
        return c_printf(b, "%s", checked_reg_name(op->id, op->width));
    case CALL_OPR_IMM:
        if (op->is_unsigned) {
            return c_printf(b, "$%llu", (unsigned long long)op->imm);
        }
        return c_printf(b, "$%lld", (long long)op->imm);
    case CALL_OPR_MEM:
        if (op->imm == 0) {
            return c_printf(b, "[%s]", checked_reg_name(op->id, 8));
        }
        return c_printf(b, "[%s%+lld]", checked_reg_name(op->id, 8),
                        (long long)op->imm);
    case CALL_OPR_GLOBAL:
        return c_printf(b, "g%lld", (long long)op->id);
    case CALL_OPR_STR:
        return c_printf(b, ".Lstr%lld", (long long)op->id);
    case CALL_OPR_FUNC:
        return c_printf(b, "fn%lld", (long long)op->id);
    case CALL_OPR_LABEL:
        return c_printf(b, "L%lld", (long long)op->id);
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

/* Render a pass-through physical instruction in the 17b2 dump format
 * (mirrors trap_branch.c dump_body_line). */
static bool dump_body_line(DiagBuf *b, const CallInsn *ci)
{
    switch (ci->op) {
    case CALL_OP_PSEUDO: {
        const IselInsn *insn = &ci->pseudo;
        if (insn->op == ISEL_COMMENT) {
            return c_printf(b, "# %s\n",
                            insn->note != NULL ? insn->note : "");
        }
        if (insn->op == ISEL_LABEL) {
            return c_printf(b, "L%lld:\n", (long long)insn->dst.id);
        }
        if (!c_append_cstr(b, "  ")) {
            return false;
        }
        if (insn->op == ISEL_SETCC) {
            if (!c_append_cstr(b, "set") ||
                !c_append_cstr(b, isel_cond_text(insn->cond))) {
                return false;
            }
        } else if (insn->op == ISEL_JCC) {
            if (!c_append_cstr(b, "j") ||
                !c_append_cstr(b, isel_cond_text(insn->cond))) {
                return false;
            }
        } else if (!c_append_cstr(b, isel_opcode_text(insn->op))) {
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
            if (!c_append_cstr(b, suffix)) {
                return false;
            }
        }
        {
            const IselOperand *ops[3] = { &insn->dst, &insn->src1,
                                          &insn->src2 };
            size_t oi;
            bool need_sep = false;
            if (insn->dst.kind != ISEL_OP_NONE ||
                insn->src1.kind != ISEL_OP_NONE ||
                insn->src2.kind != ISEL_OP_NONE) {
                if (!c_append_cstr(b, " ")) {
                    return false;
                }
            }
            for (oi = 0; oi < 3; oi++) {
                const IselOperand *op = ops[oi];
                if (op->kind == ISEL_OP_NONE) {
                    continue;
                }
                if (need_sep && !c_append_cstr(b, ", ")) {
                    return false;
                }
                switch (op->kind) {
                case ISEL_OP_VREG:
                    if (!c_printf(b, "r%lld", (long long)op->vreg)) {
                        return false;
                    }
                    break;
                case ISEL_OP_IMM:
                    if (op->is_unsigned) {
                        if (!c_printf(b, "$%llu",
                                      (unsigned long long)op->imm)) {
                            return false;
                        }
                    } else if (!c_printf(b, "$%lld",
                                         (long long)op->imm)) {
                        return false;
                    }
                    break;
                case ISEL_OP_SLOT:
                    if (!c_printf(b, "slot%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_GLOBAL:
                    if (!c_printf(b, "g%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_STR:
                    if (!c_printf(b, ".Lstr%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_FUNC:
                    if (!c_printf(b, "fn%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_LABEL:
                    if (!c_printf(b, "L%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_MEM:
                    if (op->vreg == FRAME_BASE_VREG) {
                        if (!c_printf(b, "[rbp%+lld]",
                                      (long long)op->imm)) {
                            return false;
                        }
                    } else if (op->imm == 0) {
                        if (!c_printf(b, "[r%lld]",
                                      (long long)op->vreg)) {
                            return false;
                        }
                    } else if (!c_printf(b, "[r%lld%+lld]",
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
        if (!c_printf(b, "   # ir%lld", (long long)insn->ir_node_id)) {
            return false;
        }
        if (insn->trap != NULL) {
            if (!c_printf(b, " trap=%s", insn->trap)) {
                return false;
            }
        }
        if (insn->mod) {
            if (!c_append_cstr(b, " mod")) {
                return false;
            }
        }
        return c_append_cstr(b, "\n");
    }
    case CALL_OP_PUSH_RBP:
        return c_append_cstr(b, "  push rbp\n");
    case CALL_OP_MOV_RBP_RSP:
        return c_append_cstr(b, "  mov rbp, rsp\n");
    case CALL_OP_SUB_RSP:
        return c_printf(b, "  sub rsp, $%lld\n", (long long)ci->imm);
    case CALL_OP_ADD_RSP:
        return c_printf(b, "  add rsp, $%lld\n", (long long)ci->imm);
    case CALL_OP_MOV_RSP_RBP:
        return c_append_cstr(b, "  mov rsp, rbp\n");
    case CALL_OP_POP_RBP:
        return c_append_cstr(b, "  pop rbp\n");
    case CALL_OP_PUSH_REG:
        return c_printf(b, "  push %s\n",
                        x64_reg_text((X64Reg)ci->imm));
    case CALL_OP_POP_REG:
        return c_printf(b, "  pop %s\n",
                        x64_reg_text((X64Reg)ci->imm));
    case CALL_OP_BODY: {
        if (!c_append_cstr(b, "  ")) {
            return false;
        }
        if (ci->isel == ISEL_REP_MOVSB || ci->isel == ISEL_REP_STOSB) {
            /* composite ops pass through as pseudo markers in the 17b2
             * dump; mirror trap_branch.c */
            if (ci->isel == ISEL_REP_MOVSB) {
                if (!c_append_cstr(b, "rep movsb")) {
                    return false;
                }
            } else if (!c_append_cstr(b, "rep stosb")) {
                return false;
            }
            if (!c_printf(b, "   # ir%lld\n", (long long)ci->ir_node_id)) {
                return false;
            }
            return true;
        }
        if (ci->isel == ISEL_SETCC) {
            if (!c_append_cstr(b, "set") ||
                !c_append_cstr(b, isel_cond_text(ci->cond))) {
                return false;
            }
        } else if (ci->isel == ISEL_JCC) {
            if (!c_append_cstr(b, "j") ||
                !c_append_cstr(b, isel_cond_text(ci->cond))) {
                return false;
            }
        } else if (!c_append_cstr(b, isel_opcode_text(ci->isel))) {
            return false;
        }
        if (ci->isel == ISEL_LEA && ci->scale > 0) {
            if (!c_append_cstr(b, " (scaled)")) {
                return false;
            }
        }
        if (ci->isel == ISEL_IDIV) {
            if (ci->src1.kind != CALL_OPR_NONE) {
                if (!c_append_cstr(b, " ") ||
                    !dump_operand(b, &ci->src1)) {
                    return false;
                }
            }
            if (ci->mod) {
                if (!c_append_cstr(b, " mod")) {
                    return false;
                }
            }
        } else {
            if (opcode_has_width_suffix(ci->isel)) {
                int w = ci->dst.width > 0 ? ci->dst.width : ci->src1.width;
                const char *suffix = "?";
                switch (w) {
                case 1: suffix = "b"; break;
                case 2: suffix = "w"; break;
                case 4: suffix = "l"; break;
                case 8: suffix = "q"; break;
                default: break;
                }
                if (!c_append_cstr(b, suffix)) {
                    return false;
                }
            }
            if (ci->dst.kind != CALL_OPR_NONE ||
                ci->src1.kind != CALL_OPR_NONE ||
                ci->src2.kind != CALL_OPR_NONE) {
                const CallOperand *ops[3] = { &ci->dst, &ci->src1,
                                              &ci->src2 };
                size_t oi;
                bool need_sep = false;
                if (!c_append_cstr(b, " ")) {
                    return false;
                }
                for (oi = 0; oi < 3; oi++) {
                    if (ops[oi]->kind == CALL_OPR_NONE) {
                        continue;
                    }
                    if (need_sep && !c_append_cstr(b, ", ")) {
                        return false;
                    }
                    if (!dump_operand(b, ops[oi])) {
                        return false;
                    }
                    need_sep = true;
                }
            }
        }
        if (!c_printf(b, "   # ir%lld", (long long)ci->ir_node_id)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    }
    default:
        return c_append_cstr(b, "  ?\n");
    }
}

/* Render a pass-through TrapInsn in the 17c1 dump format. */
static bool dump_trap_line(DiagBuf *b, const TrapInsn *ti)
{
    switch (ti->op) {
    case TRAP_OP_BODY:
        return dump_body_line(b, &ti->call);
    case TRAP_OP_LABEL:
        return c_printf(b, ".Ltrap%lld:\n", (long long)ti->imm);
    case TRAP_OP_JMP:
        return c_printf(b, "  jmp .Ltrap%lld\n", (long long)ti->imm);
    case TRAP_OP_JCC:
        return c_printf(b, "  %s .Ltrap%lld\n",
                        trap_cond_text(ti->cond), (long long)ti->imm);
    case TRAP_OP_TEST:
        if (!c_append_cstr(b, "  test ") ||
            !dump_operand(b, &ti->src) ||
            !c_append_cstr(b, ", ") ||
            !dump_operand(b, &ti->src)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    case TRAP_OP_CMP_IMM:
        if (!c_append_cstr(b, "  cmp ") ||
            !dump_operand(b, &ti->src) ||
            !c_printf(b, ", $%lld\n", (long long)ti->imm)) {
            return false;
        }
        return true;
    case TRAP_OP_SUB_RSP:
        return c_printf(b, "  sub rsp, $%lld\n", (long long)ti->imm);
    case TRAP_OP_MOV_CODE:
        return c_printf(b, "  mov rcx, $%lld\n", (long long)ti->imm);
    case TRAP_OP_LEA_MSG:
        return c_printf(b, "  lea rdx, [.Lmsg%lld]\n",
                        (long long)ti->imm);
    case TRAP_OP_MOV_LEN:
        return c_printf(b, "  mov r8, $%lld\n", (long long)ti->imm);
    case TRAP_OP_CALL_REPORT:
        return c_printf(b, "  call fn%lld\n", (long long)ti->imm);
    default:
        return c_append_cstr(b, "  ?\n");
    }
}

/* Render one CheckedInsn. */
static bool dump_checked_line(DiagBuf *b, const CheckedInsn *ci)
{
    switch (ci->op) {
    case CHK_OP_BODY:
        return dump_trap_line(b, &ci->trap);
    case CHK_OP_LABEL:
        return c_printf(b, ".Ltrap%lld:\n", (long long)ci->imm);
    case CHK_OP_JMP:
        return c_printf(b, "  jmp .Ltrap%lld\n", (long long)ci->imm);
    case CHK_OP_JCC:
        return c_printf(b, "  %s .Ltrap%lld\n",
                        checked_cond_text(ci->cond), (long long)ci->imm);
    case CHK_OP_TEST:
        if (!c_append_cstr(b, "  test ") ||
            !dump_operand(b, &ci->src) ||
            !c_append_cstr(b, ", ") ||
            !dump_operand(b, &ci->src)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_CMP_IMM:
        if (!c_append_cstr(b, "  cmp ") ||
            !dump_operand(b, &ci->src) ||
            !c_printf(b, ", $%lld\n", (long long)ci->imm)) {
            return false;
        }
        return true;
    case CHK_OP_CMP_REG:
        if (!c_append_cstr(b, "  cmp ") ||
            !dump_operand(b, &ci->src) ||
            !c_append_cstr(b, ", ") ||
            !dump_operand(b, &ci->base)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_CMP_MEM:
        if (!c_append_cstr(b, "  cmp ") ||
            !dump_operand(b, &ci->src) ||
            !c_append_cstr(b, ", ")) {
            return false;
        }
        if (ci->base.kind == CALL_OPR_MEM) {
            if (!dump_operand(b, &ci->base)) {
                return false;
            }
        } else {
            if (!c_append_cstr(b, "[") ||
                !dump_operand(b, &ci->base) ||
                !c_printf(b, "%+lld]", (long long)ci->imm)) {
                return false;
            }
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_MOV_LOAD:
        if (!c_append_cstr(b, "  mov ") ||
            !dump_operand(b, &ci->dst) ||
            !c_append_cstr(b, ", ")) {
            return false;
        }
        if (ci->base.kind == CALL_OPR_MEM) {
            if (!dump_operand(b, &ci->base)) {
                return false;
            }
        } else {
            if (!c_append_cstr(b, "[") ||
                !dump_operand(b, &ci->base) ||
                !c_printf(b, "%+lld]", (long long)ci->imm)) {
                return false;
            }
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_MOV_REG:
    case CHK_OP_MOVSX:
    case CHK_OP_MOVZX:
        if (!c_append_cstr(b, "  ") ||
            !c_append_cstr(b, checked_op_text(ci->op)) ||
            !c_append_cstr(b, " ") ||
            !dump_operand(b, &ci->dst) ||
            !c_append_cstr(b, ", ") ||
            !dump_operand(b, &ci->src)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_LOAD_IMM:
        if (!c_append_cstr(b, "  mov ") ||
            !dump_operand(b, &ci->dst) ||
            !c_printf(b, ", $%lld\n", (long long)ci->imm)) {
            return false;
        }
        return true;
    case CHK_OP_AND_IMM:
    case CHK_OP_SHR_IMM:
    case CHK_OP_SAR_IMM:
    case CHK_OP_ADD_IMM:
        if (!c_append_cstr(b, "  ") ||
            !c_append_cstr(b, checked_op_text(ci->op)) ||
            !c_append_cstr(b, " ") ||
            !dump_operand(b, &ci->dst) ||
            !c_printf(b, ", $%lld\n", (long long)ci->imm)) {
            return false;
        }
        return true;
    case CHK_OP_ADD_REG:
        if (!c_append_cstr(b, "  add ") ||
            !dump_operand(b, &ci->dst) ||
            !c_append_cstr(b, ", ") ||
            !dump_operand(b, &ci->src)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_IDIV:
        if (!c_append_cstr(b, "  idiv ") ||
            !dump_operand(b, &ci->src)) {
            return false;
        }
        return c_append_cstr(b, "\n");
    case CHK_OP_LOOP_LABEL:
        return c_printf(b, ".Lchk%lld:\n", (long long)ci->imm);
    case CHK_OP_LOOP_JCC:
        return c_printf(b, "  %s .Lchk%lld\n",
                        checked_cond_text(ci->cond), (long long)ci->imm);
    case CHK_OP_LOOP_JMP:
        return c_printf(b, "  jmp .Lchk%lld\n", (long long)ci->imm);
    case CHK_OP_SUB_RSP:
        return c_printf(b, "  sub rsp, $%lld\n", (long long)ci->imm);
    case CHK_OP_MOV_CODE:
        return c_printf(b, "  mov rcx, $%lld\n", (long long)ci->imm);
    case CHK_OP_LEA_MSG:
        return c_printf(b, "  lea rdx, [.Lmsg%lld]\n",
                        (long long)ci->imm);
    case CHK_OP_MOV_LEN:
        return c_printf(b, "  mov r8, $%lld\n", (long long)ci->imm);
    case CHK_OP_CALL_REPORT:
        return c_printf(b, "  call fn%lld\n", (long long)ci->imm);
    default:
        return c_append_cstr(b, "  ?\n");
    }
}

bool checked_asm_dump(const CheckedOutput *co, DiagBuf *out)
{
    size_t i;
    size_t f;
    size_t m;
    if (co == NULL || out == NULL) {
        return false;
    }
    if (!c_append_cstr(out, "; AI-Co checked-op emission dump "
                            "(WP-M0-17c2, deterministic)\n")) {
        return false;
    }
    if (!c_printf(out, "; functions=%zu insns=%zu msgs=%zu entry=%lld "
                       "report=fn%lld\n",
                  co->nfunctions, co->count, co->nmsgs,
                  (long long)co->entry_function_id,
                  (long long)co->report_function_id)) {
        return false;
    }
    for (i = 0; i < co->count; i++) {
        if (!dump_checked_line(out, &co->insns[i])) {
            return false;
        }
    }
    /* per-function site plan */
    for (f = 0; f < co->nfunctions; f++) {
        const CheckedFunction *cf = &co->functions[f];
        if (!c_printf(out, "; function %lld sites=%zu\n",
                      (long long)cf->function_id, cf->nsites)) {
            return false;
        }
        for (i = 0; i < cf->nsites; i++) {
            const CheckedSite *site = &cf->sites[i];
            const char *file = site->span != NULL ? site->span->file : NULL;
            if (!c_printf(out, ";   .Ltrap%lld %s code=%lld span=%s:%lld:%lld\n",
                          (long long)site->site_index,
                          site->code != NULL ? site->code : "?",
                          (long long)site->numeric_code,
                          file != NULL ? file : "?",
                          site->span != NULL
                              ? (long long)site->span->start.line : 0,
                          site->span != NULL
                              ? (long long)site->span->start.col : 0)) {
                return false;
            }
            /* causal chain, root cause first */
            for (m = 0; m < site->cause_count && site->causes != NULL; m++) {
                const IrCauseLink *link = &site->causes[m];
                const char *kind = link->construct_kind != NULL
                                       ? link->construct_kind : "?";
                const DiagSpan *lspan = link->span;
                const char *lfile = lspan != NULL ? lspan->file : NULL;
                if (!c_printf(out, ";   cause %zu: %s at %s:%lld:%lld\n", m,
                              kind, lfile != NULL ? lfile : "?",
                              lspan != NULL
                                  ? (long long)lspan->start.line : 0,
                              lspan != NULL
                                  ? (long long)lspan->start.col : 0)) {
                    return false;
                }
            }
        }
    }
    /* message constants */
    for (m = 0; m < co->nmsgs; m++) {
        if (!c_printf(out, "; .Lmsg%zu = \"%s\"\n", m,
                      co->msgs[m] != NULL ? co->msgs[m] : "")) {
            return false;
        }
    }
    return true;
}
