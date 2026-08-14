/* bootstrap/src/backend/trap_branch.c
 *
 * AI-Co Stage-0 x86-64 trap branch emission (WP-M0-17c1).
 *
 * Implements the obligations of trap_branch.h on top of the 17b2 physical
 * stream (CallOutput), its 17b1 framed stream (FrameOutput), and the IR
 * build: a deterministic per-function trap-site plan (stable AIC-R code +
 * source span for every trap obligation), the trap branches at the
 * obligation sites, and the trap paths (the rt.trap.report call with the
 * stable code and a span-carrying message) at the end of each function.
 * See trap_branch.h for the normative rules (1-4), the scope boundary
 * (checked-op emission details are WP-M0-17c2), and the ownership model.
 *
 * Determinism is structural: the pass walks the framed stream and the
 * physical stream in emission order and the IR's deterministic arrays;
 * every output is a pure function of those inputs. Identical inputs always
 * yield identical trap-branched streams and identical dump bytes (spec
 * sec. 14.2).
 */
#include "trap_branch.h"

#include "../diag/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Buffer helpers (deterministic text sink; mirrors isel_core/call)
 * ------------------------------------------------------------------------- */

static bool t_reserve(DiagBuf *buf, size_t extra)
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

static bool t_append_n(DiagBuf *buf, const char *s, size_t n)
{
    if (!t_reserve(buf, n)) {
        return false;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return true;
}

static bool t_append_cstr(DiagBuf *buf, const char *s)
{
    return t_append_n(buf, s, strlen(s));
}

static bool t_printf(DiagBuf *buf, const char *fmt, ...)
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
        return t_append_n(buf, tmp, (size_t)n);
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
        t_append_n(buf, big, (size_t)n);
        free(big);
        return !buf->oom;
    }
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

/* Signedness of the IR result type (mirrors isel_core's helper; used to
 * choose jo vs jc for checked add/sub/mul overflow). */
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

/* The deterministic u32 code passed to rt.trap.report for a language trap:
 * the 4-digit AIC-R suffix (AIC-R0802 -> 0x0802 = 2050). */
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

const char *trap_op_text(TrapOp op)
{
    switch (op) {
    case TRAP_OP_BODY:        return "body";
    case TRAP_OP_LABEL:       return "label";
    case TRAP_OP_JMP:         return "jmp";
    case TRAP_OP_JCC:         return "jcc";
    case TRAP_OP_TEST:        return "test";
    case TRAP_OP_CMP_IMM:     return "cmp";
    case TRAP_OP_SUB_RSP:     return "sub";
    case TRAP_OP_MOV_CODE:    return "mov";
    case TRAP_OP_LEA_MSG:     return "lea";
    case TRAP_OP_CALL_REPORT: return "call";
    default:                  return "?";
    }
}

const char *trap_cond_text(TrapCond cond)
{
    switch (cond) {
    case TRAP_COND_JZ:  return "jz";
    case TRAP_COND_JA:  return "ja";
    case TRAP_COND_JAE: return "jae";
    case TRAP_COND_JO:  return "jo";
    case TRAP_COND_JC:  return "jc";
    case TRAP_COND_JMP: return "jmp";
    default:            return "?";
    }
}

/* ---------------------------------------------------------------------------
 * Function markers
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

/* ---------------------------------------------------------------------------
 * Output helpers
 * ------------------------------------------------------------------------- */

static bool emit_insn(TrapOutput *to, TrapOp op, const CallInsn *call,
                      const CallOperand *src, TrapCond cond, int64_t imm,
                      int64_t ir_node_id)
{
    TrapInsn *p;
    if (to->count == to->cap) {
        size_t ncap = to->cap == 0 ? 32 : to->cap * 2;
        TrapInsn *q = (TrapInsn *)realloc(to->insns, ncap * sizeof(TrapInsn));
        if (q == NULL) {
            to->oom = true;
            return false;
        }
        to->insns = q;
        to->cap = ncap;
    }
    p = &to->insns[to->count];
    memset(p, 0, sizeof(*p));
    p->op = op;
    if (call != NULL) {
        p->call = *call;
    }
    if (src != NULL) {
        p->src = *src;
    }
    p->cond = cond;
    p->imm = imm;
    p->ir_node_id = ir_node_id;
    to->count++;
    return true;
}

static bool emit_body(TrapOutput *to, const CallInsn *call)
{
    return emit_insn(to, TRAP_OP_BODY, call, NULL, TRAP_COND_JMP, 0,
                     call->ir_node_id);
}

static bool emit_site_ref(TrapOutput *to, TrapOp op, TrapCond cond,
                          int64_t site_index, int64_t ir_node_id)
{
    return emit_insn(to, op, NULL, NULL, cond, site_index, ir_node_id);
}

static char *t_strdup(const char *s)
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

static bool append_message(TrapOutput *to, const char *text)
{
    char *copy;
    if (to->nmsgs == to->msgs_cap) {
        size_t ncap = to->msgs_cap == 0 ? 4 : to->msgs_cap * 2;
        char **q = (char **)realloc(to->msgs, ncap * sizeof(char *));
        if (q == NULL) {
            to->oom = true;
            return false;
        }
        to->msgs = q;
        to->msgs_cap = ncap;
    }
    copy = t_strdup(text);
    if (copy == NULL) {
        to->oom = true;
        return false;
    }
    to->msgs[to->nmsgs] = copy;
    to->nmsgs++;
    return true;
}

/* ---------------------------------------------------------------------------
 * Site plan
 * ------------------------------------------------------------------------- */

static bool append_site(TrapFunction *tf, TrapOutput *to,
                        const char *code, int64_t numeric_code,
                        const DiagSpan *span, int64_t ir_node_id,
                        bool unconditional)
{
    TrapSite *p;
    char msg[512];
    int n;
    if (tf->nsites == 0) {
        tf->sites = (TrapSite *)malloc(sizeof(TrapSite));
        if (tf->sites == NULL) {
            to->oom = true;
            return false;
        }
    } else if (tf->nsites % 8 == 0) {
        TrapSite *q = (TrapSite *)realloc(
            tf->sites, (tf->nsites + 8) * sizeof(TrapSite));
        if (q == NULL) {
            to->oom = true;
            return false;
        }
        tf->sites = q;
    }
    p = &tf->sites[tf->nsites];
    memset(p, 0, sizeof(*p));
    p->site_index = (int64_t)tf->nsites;
    p->code = code;
    p->numeric_code = numeric_code;
    p->span = span;
    p->ir_node_id = ir_node_id;
    p->unconditional = unconditional;
    if (code != NULL && strcmp(code, "user") == 0) {
        /* user trap via rt.trap.report: the message names the caller's
         * u32 code */
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
                     code != NULL ? code : "trap",
                     span->file,
                     (long long)span->start.line,
                     (long long)span->start.col);
    } else {
        n = snprintf(msg, sizeof(msg), "%s",
                     code != NULL ? code : "trap");
    }
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        to->oom = true;
        return false;
    }
    p->msg_index = (int64_t)to->nmsgs;
    if (!append_message(to, msg)) {
        return false;
    }
    tf->nsites++;
    return true;
}

static bool append_function(TrapOutput *to, const TrapFunction *tf)
{
    TrapFunction *p;
    if (to->nfunctions == to->functions_cap) {
        size_t ncap = to->functions_cap == 0 ? 8 : to->functions_cap * 2;
        TrapFunction *q = (TrapFunction *)realloc(
            to->functions, ncap * sizeof(TrapFunction));
        if (q == NULL) {
            to->oom = true;
            return false;
        }
        to->functions = q;
        to->functions_cap = ncap;
    }
    p = &to->functions[to->nfunctions];
    *p = *tf;
    p->sites = tf->sites;
    to->nfunctions++;
    return true;
}

/* ---------------------------------------------------------------------------
 * Trap path emission (end of a function region)
 * ------------------------------------------------------------------------- */

static bool emit_trap_paths(TrapOutput *to, const TrapFunction *tf)
{
    size_t i;
    for (i = 0; i < tf->nsites; i++) {
        const TrapSite *site = &tf->sites[i];
        /* label */
        if (!emit_site_ref(to, TRAP_OP_LABEL, TRAP_COND_JMP,
                           site->site_index, site->ir_node_id)) {
            return false;
        }
        /* 32-byte shadow space for the report call (sec. 15.7); RSP is
         * 16-byte aligned and is never restored (rt.trap.report noreturn) */
        if (!emit_insn(to, TRAP_OP_SUB_RSP, NULL, NULL, TRAP_COND_JMP, 32,
                       site->ir_node_id)) {
            return false;
        }
        /* code -> RCX */
        if (!emit_insn(to, TRAP_OP_MOV_CODE, NULL, NULL, TRAP_COND_JMP,
                       site->numeric_code, site->ir_node_id)) {
            return false;
        }
        /* message str pair image address -> RDX (composite argument is
         * address-resident per the 17b2 convention) */
        if (!emit_insn(to, TRAP_OP_LEA_MSG, NULL, NULL, TRAP_COND_JMP,
                       site->msg_index, site->ir_node_id)) {
            return false;
        }
        /* call rt.trap.report; nothing after (noreturn) */
        if (!emit_insn(to, TRAP_OP_CALL_REPORT, NULL, NULL, TRAP_COND_JMP,
                       to->report_function_id, site->ir_node_id)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Branch emission at an obligation site
 *
 * The checked physical instruction (opcode + ir_node_id match) is `body`.
 * The deterministic trap branch per rule:
 *   R0802 overflow (add/sub/mul/neg) -> jo (signed) / jc (unsigned) AFTER
 *   R0803 divisor zero (idiv)        -> test <divisor>,<divisor>; jz BEFORE
 *   R0804 shift count (shl/shr/sar)  -> cmp <count>, $width; jae BEFORE
 *   R0805 invalid bool byte          -> cmp <byte>, $1; ja AFTER the load
 * All other obligations pass through unchanged (their check sequences are
 * 17c2's checked-op emission details).
 * ------------------------------------------------------------------------- */

typedef enum TrapRule {
    TRAP_RULE_NONE = 0,
    TRAP_RULE_OVERFLOW,   /* jo/jc after */
    TRAP_RULE_DIV_ZERO,   /* test+jz before (register divisor) */
    TRAP_RULE_SHIFT,      /* cmp+jae before (variable count) */
    TRAP_RULE_BOOL_BYTE   /* cmp+ja after (register byte load) */
} TrapRule;

static TrapRule rule_for(const char *code, IselOpcode op)
{
    if (code == NULL) {
        return TRAP_RULE_NONE;
    }
    if (strcmp(code, "AIC-R0802") == 0) {
        switch (op) {
        case ISEL_ADD:
        case ISEL_SUB:
        case ISEL_IMUL:
        case ISEL_NEG:
            return TRAP_RULE_OVERFLOW;
        default:
            return TRAP_RULE_NONE;
        }
    }
    if (strcmp(code, "AIC-R0803") == 0 && op == ISEL_IDIV) {
        return TRAP_RULE_DIV_ZERO;
    }
    if (strcmp(code, "AIC-R0804") == 0 &&
        (op == ISEL_SHL || op == ISEL_SHR || op == ISEL_SAR)) {
        return TRAP_RULE_SHIFT;
    }
    if (strcmp(code, "AIC-R0805") == 0 && op == ISEL_MOV) {
        return TRAP_RULE_BOOL_BYTE;
    }
    return TRAP_RULE_NONE;
}

/* The pre-body part of the trap branch (flag-setter + branch for the
 * before-rules). Returns true when a pre-body branch was emitted. */
static bool emit_pre_branch(TrapOutput *to, const CallInsn *body,
                            const char *code, int64_t site_index,
                            int64_t ir_node_id)
{
    TrapRule rule = rule_for(code, body->isel);
    switch (rule) {
    case TRAP_RULE_DIV_ZERO:
        /* divisor operand is the idiv's src1. A register divisor is
         * tested; a constant divisor is verified non-zero (AIC-E0406), so
         * no runtime test is needed. */
        if (body->src1.kind == CALL_OPR_REG) {
            if (!emit_insn(to, TRAP_OP_TEST, NULL, &body->src1,
                           TRAP_COND_JMP, 0, ir_node_id) ||
                !emit_site_ref(to, TRAP_OP_JCC, TRAP_COND_JZ,
                               site_index, ir_node_id)) {
                return false;
            }
        }
        return true;
    case TRAP_RULE_SHIFT:
        /* variable count is in CL (17b2 lowering); count must be in
         * 0..width-1. A constant count is verified in range (AIC-E0407),
         * so no runtime test is needed. */
        if (body->src1.kind == CALL_OPR_REG) {
            int64_t bits = (int64_t)body->dst.width * 8;
            if (!emit_insn(to, TRAP_OP_CMP_IMM, NULL, &body->src1,
                           TRAP_COND_JMP, bits, ir_node_id) ||
                !emit_site_ref(to, TRAP_OP_JCC, TRAP_COND_JAE,
                               site_index, ir_node_id)) {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}

/* The post-body part of the trap branch (for the after-rules). */
static bool emit_post_branch(TrapOutput *to, const IrBuild *build,
                             const CallInsn *body, const char *code,
                             int64_t site_index, int64_t ir_node_id)
{
    TrapRule rule = rule_for(code, body->isel);
    switch (rule) {
    case TRAP_RULE_OVERFLOW:
        /* signed result type: OF=1 -> jo; unsigned: CF=1 -> jc */
        if (ir_type_is_signed(
                build != NULL && ir_node_id >= 0 &&
                        (size_t)ir_node_id < build->nnodes
                    ? build->nodes[ir_node_id]->type : NULL)) {
            return emit_site_ref(to, TRAP_OP_JCC, TRAP_COND_JO,
                                 site_index, ir_node_id);
        }
        return emit_site_ref(to, TRAP_OP_JCC, TRAP_COND_JC,
                             site_index, ir_node_id);
    case TRAP_RULE_BOOL_BYTE:
        /* the loaded byte is in the load's destination register; a value
         * above 1 is an invalid bool byte (sec. 9.1) */
        if (body->dst.kind == CALL_OPR_REG) {
            if (!emit_insn(to, TRAP_OP_CMP_IMM, NULL, &body->dst,
                           TRAP_COND_JMP, 1, ir_node_id) ||
                !emit_site_ref(to, TRAP_OP_JCC, TRAP_COND_JA,
                               site_index, ir_node_id)) {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}

/* ---------------------------------------------------------------------------
 * Obligation scan over the framed stream
 *
 * The framed stream is the authoritative trap-annotation source: 17b2's
 * physical stream drops the IselInsn trap for lowered body instructions.
 * Each function region contributes obligations (opcode, ir_node_id, trap
 * code) for every body instruction with a non-NULL trap.
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

/* Collect obligations for the framed function region [start, end). */
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

TrapStatus trap_branch_build(const IrBuild *build, const FrameOutput *fr,
                             const CallOutput *co, TrapOutput **out)
{
    TrapOutput *to;
    size_t i;
    size_t fi;   /* framed stream cursor */
    if (build == NULL || fr == NULL || co == NULL || out == NULL) {
        return TRAP_OOM;
    }
    to = (TrapOutput *)calloc(1, sizeof(*to));
    if (to == NULL) {
        return TRAP_OOM;
    }
    to->entry_function_id = co->entry_function_id;
    to->report_function_id = find_report_function(build);

    fi = 0;
    i = 0;
    while (i < co->count) {
        const CallInsn *ci = &co->insns[i];
        if (!is_function_marker_pseudo(ci)) {
            /* non-marker instruction outside any function (defensive) */
            if (!emit_body(to, ci)) {
                to->oom = true;
                break;
            }
            i++;
            continue;
        }
        {
            const IrNode *fn = lookup_function(build, ci->ir_node_id);
            TrapFunction tf;
            ObligationList obl;
            size_t region_start = i + 1;
            size_t region_end = region_start;
            size_t j;
            size_t frame_region_start;
            size_t frame_region_end;
            /* finalize the previous function now that its span is known */
            if (to->nfunctions > 0) {
                TrapFunction *prev = &to->functions[to->nfunctions - 1];
                prev->count = to->count - prev->start;
            }
            memset(&tf, 0, sizeof(tf));
            tf.function_id = fn != NULL ? fn->id : ci->ir_node_id;
            tf.start = to->count;
            tf.count = 0;
            if (fn != NULL) {
                /* advance the framed cursor to this function's marker and
                 * collect its trap obligations */
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
                    to->oom = true;
                    break;
                }
                fi = frame_region_end;
            } else {
                memset(&obl, 0, sizeof(obl));
            }
            /* the marker comment passes through */
            if (!emit_body(to, ci)) {
                to->oom = true;
                break;
            }
            /* walk the physical region */
            while (region_end < co->count &&
                   !is_function_marker_pseudo(&co->insns[region_end])) {
                region_end++;
            }
            for (j = region_start; j < region_end; j++) {
                const CallInsn *pc = &co->insns[j];
                if (pc->op == CALL_OP_PSEUDO && pc->isel == ISEL_TRAP) {
                    /* unconditional trap marker: keep the marker as an
                     * annotation, then jmp to a fresh site */
                    const char *code = pc->pseudo.trap;
                    int64_t numeric =
                        (code != NULL && strcmp(code, "user") == 0)
                            ? pc->pseudo.src1.imm
                            : language_code_numeric(code);
                    const DiagSpan *span =
                        (pc->pseudo.ir_node_id >= 0 &&
                         (size_t)pc->pseudo.ir_node_id < build->nnodes)
                            ? build->nodes[pc->pseudo.ir_node_id]->span
                            : NULL;
                    if (!append_site(&tf, to, code, numeric, span,
                                     pc->pseudo.ir_node_id, true)) {
                        to->oom = true;
                        break;
                    }
                    if (!emit_body(to, pc) ||
                        !emit_site_ref(to, TRAP_OP_JMP, TRAP_COND_JMP,
                                       (int64_t)tf.nsites - 1,
                                       pc->pseudo.ir_node_id)) {
                        to->oom = true;
                        break;
                    }
                    continue;
                }
                if (pc->op == CALL_OP_PSEUDO) {
                    /* 17c pseudo markers with a trap obligation (utf8 /
                     * ptrdiff / slice) get a trap site and path here and
                     * pass through; their check sequences are 17c2. Other
                     * pseudos (strcmp/sliceeq) pass through unchanged. */
                    if (pc->pseudo.trap != NULL) {
                        const char *code = pc->pseudo.trap;
                        int64_t numeric = language_code_numeric(code);
                        const DiagSpan *span =
                            (pc->pseudo.ir_node_id >= 0 &&
                             (size_t)pc->pseudo.ir_node_id < build->nnodes)
                                ? build->nodes[pc->pseudo.ir_node_id]->span
                                : NULL;
                        if (!append_site(&tf, to, code, numeric, span,
                                         pc->pseudo.ir_node_id, false)) {
                            to->oom = true;
                            break;
                        }
                    }
                    if (!emit_body(to, pc)) {
                        to->oom = true;
                        break;
                    }
                    continue;
                }
                if (pc->op == CALL_OP_BODY) {
                    size_t k;
                    bool matched = false;
                    for (k = 0; k < obl.count; k++) {
                        const FrameObligation *o = &obl.items[k];
                        if (o->ir_node_id < 0) {
                            continue;   /* consumed */
                        }
                        if (o->ir_node_id != pc->ir_node_id ||
                            o->opcode != pc->isel) {
                            continue;
                        }
                        /* bool-load obligations attach to the byte load
                         * (REG destination, width 1), not the spill store
                         * or the base-address load; other ISEL_MOV
                         * obligations (e.g. the deref pointer copy)
                         * attach to the REG-destination copy */
                        if (o->opcode == ISEL_MOV) {
                            if (pc->dst.kind != CALL_OPR_REG) {
                                continue;
                            }
                            if (rule_for(o->code, o->opcode) ==
                                    TRAP_RULE_BOOL_BYTE &&
                                pc->dst.width != 1) {
                                continue;
                            }
                        }
                        {
                            const DiagSpan *span =
                                (pc->ir_node_id >= 0 &&
                                 (size_t)pc->ir_node_id < build->nnodes)
                                    ? build->nodes[pc->ir_node_id]->span
                                    : NULL;
                            int64_t numeric = language_code_numeric(o->code);
                            int64_t site = (int64_t)tf.nsites;
                            if (!append_site(&tf, to, o->code, numeric,
                                             span, o->ir_node_id, false)) {
                                to->oom = true;
                                break;
                            }
                            /* pre-body branch (div-zero / shift count) */
                            if (!emit_pre_branch(to, pc, o->code,
                                                 site, o->ir_node_id)) {
                                to->oom = true;
                                break;
                            }
                            if (!emit_body(to, pc)) {
                                to->oom = true;
                                break;
                            }
                            /* post-body branch (overflow / bool byte) */
                            if (!emit_post_branch(to, build, pc, o->code,
                                                  site, o->ir_node_id)) {
                                to->oom = true;
                                break;
                            }
                        }
                        obl.items[k].ir_node_id = -1;   /* consume */
                        matched = true;
                        break;
                    }
                    if (to->oom) {
                        break;
                    }
                    if (!matched && !emit_body(to, pc)) {
                        to->oom = true;
                        break;
                    }
                    continue;
                }
                /* defensive: other physical ops pass through */
                if (!emit_body(to, pc)) {
                    to->oom = true;
                    break;
                }
            }
            if (to->oom) {
                break;
            }
            /* trap paths at the end of the function */
            if (!emit_trap_paths(to, &tf)) {
                to->oom = true;
                break;
            }
            tf.count = to->count - tf.start;
            if (!append_function(to, &tf)) {
                to->oom = true;
                break;
            }
            obl_free(&obl);
            i = region_end;
            continue;
        }
    }
    if (to->nfunctions > 0) {
        to->functions[to->nfunctions - 1].count =
            to->count - to->functions[to->nfunctions - 1].start;
    }
    if (to->oom) {
        trap_output_free(to);
        *out = NULL;
        return TRAP_OOM;
    }
    *out = to;
    return TRAP_OK;
}

void trap_output_free(TrapOutput *out)
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

size_t trap_output_count(const TrapOutput *out)
{
    return out != NULL ? out->count : 0;
}

const TrapInsn *trap_output_insn(const TrapOutput *out, size_t i)
{
    if (out == NULL || i >= out->count) {
        return NULL;
    }
    return &out->insns[i];
}

size_t trap_function_count(const TrapOutput *out)
{
    return out != NULL ? out->nfunctions : 0;
}

const TrapFunction *trap_function_at(const TrapOutput *out, size_t i)
{
    if (out == NULL || i >= out->nfunctions) {
        return NULL;
    }
    return &out->functions[i];
}

const TrapFunction *trap_function_for(const TrapOutput *out,
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

const TrapSite *trap_function_site(const TrapFunction *tf, size_t i)
{
    if (tf == NULL || i >= tf->nsites) {
        return NULL;
    }
    return &tf->sites[i];
}

const char *trap_message(const TrapOutput *out, size_t msg_index)
{
    if (out == NULL || msg_index >= out->nmsgs) {
        return NULL;
    }
    return out->msgs[msg_index];
}

/* ---------------------------------------------------------------------------
 * Deterministic trap assembly dump
 * ------------------------------------------------------------------------- */

/* Register names (mirrors call.c). */
static const char *const kRegNames[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char *reg_name(int64_t reg, int width)
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

static bool dump_call_operand(DiagBuf *b, const CallOperand *op)
{
    switch (op->kind) {
    case CALL_OPR_NONE:
        return true;
    case CALL_OPR_REG:
        return t_printf(b, "%s", reg_name(op->id, op->width));
    case CALL_OPR_IMM:
        if (op->is_unsigned) {
            return t_printf(b, "$%llu", (unsigned long long)op->imm);
        }
        return t_printf(b, "$%lld", (long long)op->imm);
    case CALL_OPR_MEM:
        if (op->imm == 0) {
            return t_printf(b, "[%s]", reg_name(op->id, 8));
        }
        return t_printf(b, "[%s%+lld]", reg_name(op->id, 8),
                        (long long)op->imm);
    case CALL_OPR_GLOBAL:
        return t_printf(b, "g%lld", (long long)op->id);
    case CALL_OPR_STR:
        return t_printf(b, ".Lstr%lld", (long long)op->id);
    case CALL_OPR_FUNC:
        return t_printf(b, "fn%lld", (long long)op->id);
    case CALL_OPR_LABEL:
        return t_printf(b, "L%lld", (long long)op->id);
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

/* Render a pass-through physical instruction in the 17b2 dump format. */
static bool dump_body_line(DiagBuf *b, const CallInsn *ci)
{
    switch (ci->op) {
    case CALL_OP_PSEUDO: {
        const IselInsn *insn = &ci->pseudo;
        if (insn->op == ISEL_COMMENT) {
            return t_printf(b, "# %s\n",
                            insn->note != NULL ? insn->note : "");
        }
        if (insn->op == ISEL_LABEL) {
            return t_printf(b, "L%lld:\n", (long long)insn->dst.id);
        }
        if (!t_append_cstr(b, "  ")) {
            return false;
        }
        if (insn->op == ISEL_SETCC) {
            if (!t_append_cstr(b, "set") ||
                !t_append_cstr(b, isel_cond_text(insn->cond))) {
                return false;
            }
        } else if (insn->op == ISEL_JCC) {
            if (!t_append_cstr(b, "j") ||
                !t_append_cstr(b, isel_cond_text(insn->cond))) {
                return false;
            }
        } else if (!t_append_cstr(b, isel_opcode_text(insn->op))) {
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
            if (!t_append_cstr(b, suffix)) {
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
                if (!t_append_cstr(b, " ")) {
                    return false;
                }
            }
            for (oi = 0; oi < 3; oi++) {
                const IselOperand *op = ops[oi];
                if (op->kind == ISEL_OP_NONE) {
                    continue;
                }
                if (need_sep && !t_append_cstr(b, ", ")) {
                    return false;
                }
                switch (op->kind) {
                case ISEL_OP_VREG:
                    if (!t_printf(b, "r%lld", (long long)op->vreg)) {
                        return false;
                    }
                    break;
                case ISEL_OP_IMM:
                    if (op->is_unsigned) {
                        if (!t_printf(b, "$%llu",
                                      (unsigned long long)op->imm)) {
                            return false;
                        }
                    } else if (!t_printf(b, "$%lld",
                                         (long long)op->imm)) {
                        return false;
                    }
                    break;
                case ISEL_OP_SLOT:
                    if (!t_printf(b, "slot%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_GLOBAL:
                    if (!t_printf(b, "g%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_STR:
                    if (!t_printf(b, ".Lstr%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_FUNC:
                    if (!t_printf(b, "fn%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_LABEL:
                    if (!t_printf(b, "L%lld", (long long)op->id)) {
                        return false;
                    }
                    break;
                case ISEL_OP_MEM:
                    if (op->vreg == FRAME_BASE_VREG) {
                        if (!t_printf(b, "[rbp%+lld]",
                                      (long long)op->imm)) {
                            return false;
                        }
                    } else if (op->imm == 0) {
                        if (!t_printf(b, "[r%lld]",
                                      (long long)op->vreg)) {
                            return false;
                        }
                    } else if (!t_printf(b, "[r%lld%+lld]",
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
        if (!t_printf(b, "   # ir%lld", (long long)insn->ir_node_id)) {
            return false;
        }
        if (insn->trap != NULL) {
            if (!t_printf(b, " trap=%s", insn->trap)) {
                return false;
            }
        }
        if (insn->mod) {
            if (!t_append_cstr(b, " mod")) {
                return false;
            }
        }
        return t_append_cstr(b, "\n");
    }
    case CALL_OP_PUSH_RBP:
        return t_append_cstr(b, "  push rbp\n");
    case CALL_OP_MOV_RBP_RSP:
        return t_append_cstr(b, "  mov rbp, rsp\n");
    case CALL_OP_SUB_RSP:
        return t_printf(b, "  sub rsp, $%lld\n", (long long)ci->imm);
    case CALL_OP_ADD_RSP:
        return t_printf(b, "  add rsp, $%lld\n", (long long)ci->imm);
    case CALL_OP_MOV_RSP_RBP:
        return t_append_cstr(b, "  mov rsp, rbp\n");
    case CALL_OP_POP_RBP:
        return t_append_cstr(b, "  pop rbp\n");
    case CALL_OP_PUSH_REG:
        return t_printf(b, "  push %s\n",
                        x64_reg_text((X64Reg)ci->imm));
    case CALL_OP_POP_REG:
        return t_printf(b, "  pop %s\n",
                        x64_reg_text((X64Reg)ci->imm));
    case CALL_OP_BODY: {
        if (!t_append_cstr(b, "  ")) {
            return false;
        }
        if (ci->isel == ISEL_SETCC) {
            if (!t_append_cstr(b, "set") ||
                !t_append_cstr(b, isel_cond_text(ci->cond))) {
                return false;
            }
        } else if (ci->isel == ISEL_JCC) {
            if (!t_append_cstr(b, "j") ||
                !t_append_cstr(b, isel_cond_text(ci->cond))) {
                return false;
            }
        } else if (!t_append_cstr(b, isel_opcode_text(ci->isel))) {
            return false;
        }
        if (ci->isel == ISEL_LEA && ci->scale > 0) {
            if (!t_append_cstr(b, " (scaled)")) {
                return false;
            }
        }
        if (ci->isel == ISEL_IDIV) {
            if (ci->src1.kind != CALL_OPR_NONE) {
                if (!t_append_cstr(b, " ") ||
                    !dump_call_operand(b, &ci->src1)) {
                    return false;
                }
            }
            if (ci->mod) {
                if (!t_append_cstr(b, " mod")) {
                    return false;
                }
            }
            if (!t_printf(b, "   # ir%lld",
                          (long long)ci->ir_node_id)) {
                return false;
            }
            return t_append_cstr(b, "\n");
        }
        if (ci->isel == ISEL_REP_MOVSB || ci->isel == ISEL_REP_STOSB) {
            if (!t_printf(b, "   # ir%lld",
                          (long long)ci->ir_node_id)) {
                return false;
            }
            return t_append_cstr(b, "\n");
        }
        if (ci->dst.kind != CALL_OPR_NONE ||
            ci->src1.kind != CALL_OPR_NONE ||
            ci->src2.kind != CALL_OPR_NONE) {
            const CallOperand *ops[3] = { &ci->dst, &ci->src1, &ci->src2 };
            size_t oi;
            bool need_sep = false;
            if (!t_append_cstr(b, " ")) {
                return false;
            }
            for (oi = 0; oi < 3; oi++) {
                if (ops[oi]->kind == CALL_OPR_NONE) {
                    continue;
                }
                if (need_sep && !t_append_cstr(b, ", ")) {
                    return false;
                }
                if (!dump_call_operand(b, ops[oi])) {
                    return false;
                }
                need_sep = true;
            }
        }
        if (!t_printf(b, "   # ir%lld", (long long)ci->ir_node_id)) {
            return false;
        }
        return t_append_cstr(b, "\n");
    }
    default:
        return t_append_cstr(b, "  ?\n");
    }
}

/* Render the trap-specific additions. */
static bool dump_trap_insn(DiagBuf *b, const TrapInsn *ti)
{
    switch (ti->op) {
    case TRAP_OP_BODY:
        return dump_body_line(b, &ti->call);
    case TRAP_OP_LABEL:
        return t_printf(b, ".Ltrap%lld:\n", (long long)ti->imm);
    case TRAP_OP_JMP:
        return t_printf(b, "  jmp .Ltrap%lld\n", (long long)ti->imm);
    case TRAP_OP_JCC:
        return t_printf(b, "  %s .Ltrap%lld\n",
                        trap_cond_text(ti->cond), (long long)ti->imm);
    case TRAP_OP_TEST:
        if (!t_append_cstr(b, "  test ") ||
            !dump_call_operand(b, &ti->src) ||
            !t_append_cstr(b, ", ") ||
            !dump_call_operand(b, &ti->src)) {
            return false;
        }
        return t_append_cstr(b, "\n");
    case TRAP_OP_CMP_IMM:
        if (!t_append_cstr(b, "  cmp ") ||
            !dump_call_operand(b, &ti->src) ||
            !t_printf(b, ", $%lld\n", (long long)ti->imm)) {
            return false;
        }
        return true;
    case TRAP_OP_SUB_RSP:
        return t_printf(b, "  sub rsp, $%lld\n", (long long)ti->imm);
    case TRAP_OP_MOV_CODE:
        return t_printf(b, "  mov rcx, $%lld\n", (long long)ti->imm);
    case TRAP_OP_LEA_MSG:
        return t_printf(b, "  lea rdx, [.Lmsg%lld]\n",
                        (long long)ti->imm);
    case TRAP_OP_CALL_REPORT:
        return t_printf(b, "  call fn%lld\n", (long long)ti->imm);
    default:
        return t_append_cstr(b, "  ?\n");
    }
}

bool trap_asm_dump(const TrapOutput *to, DiagBuf *out)
{
    size_t i;
    size_t f;
    size_t m;
    if (to == NULL || out == NULL) {
        return false;
    }
    if (!t_append_cstr(out, "; AI-Co trap branch dump "
                            "(WP-M0-17c1, deterministic)\n")) {
        return false;
    }
    if (!t_printf(out, "; functions=%zu insns=%zu msgs=%zu entry=%lld "
                       "report=fn%lld\n",
                  to->nfunctions, to->count, to->nmsgs,
                  (long long)to->entry_function_id,
                  (long long)to->report_function_id)) {
        return false;
    }
    for (i = 0; i < to->count; i++) {
        if (!dump_trap_insn(out, &to->insns[i])) {
            return false;
        }
    }
    /* per-function site plan */
    for (f = 0; f < to->nfunctions; f++) {
        const TrapFunction *tf = &to->functions[f];
        if (!t_printf(out, "; function %lld sites=%zu\n",
                      (long long)tf->function_id, tf->nsites)) {
            return false;
        }
        for (i = 0; i < tf->nsites; i++) {
            const TrapSite *site = &tf->sites[i];
            const char *file = site->span != NULL ? site->span->file : NULL;
            if (!t_printf(out, ";   .Ltrap%lld %s code=%lld span=%s:%lld:%lld\n",
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
        }
    }
    /* message constants */
    for (m = 0; m < to->nmsgs; m++) {
        if (!t_printf(out, "; .Lmsg%zu = \"%s\"\n", m,
                      to->msgs[m] != NULL ? to->msgs[m] : "")) {
            return false;
        }
    }
    return true;
}
