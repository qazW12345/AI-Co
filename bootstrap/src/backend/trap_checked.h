/* bootstrap/src/backend/trap_checked.h
 *
 * AI-Co Stage-0 x86-64 checked-operation emission with span/cause
 * preservation on trap records (WP-M0-17c2).
 *
 * Implements the checked-op emission obligations of spec sec. 14.3/15.5/15.8
 * on top of the 17c1 trap-branched stream (TrapOutput): the multi-instruction
 * check sequences for the complex checked ops that 17c1 passed through as
 * annotated markers, and the span/cause preservation on trap records
 * (DIAGNOSTIC-CONTRACT sec. 10 causes, IR contract sec. 8.4 consumption).
 *
 * Scope (manifest WP-M0-17c2): checked-op emission (the multi-instruction
 * check sequences for ISEL_SLICE / ISEL_UTF8 / ISEL_PTRDIFF / index bounds /
 * null deref / cast range) and span/cause preservation on trap records.
 * Excluded (owned by other packages): trap branch structure (17c1,
 * trap_branch.*); instruction selection (17a, isel_core.* and isel_x64.*);
 * frame
 * layout and register allocation/call emission (17b, frame.* and call.*);
 * COFF emission (WP-M0-18). The 17c1 boundary note (trap_branch.h) is the
 * binding handoff: 17c1 creates the trap site and trap path for every
 * runtime-failable checked operation and passes the complex obligations
 * through unchanged as annotated markers; this package emits the check
 * sequences that make those sites reachable, and extends each trap record
 * with the failing IR node's causal chain (root cause first).
 *
 * What this package delivers:
 *   - trap_checked_build, a deterministic pass over the 17c1 TrapOutput
 *     (plus the IR build, framed stream, and call output it was made from)
 *     that produces a CheckedOutput:
 *       * the trap-branched stream with the multi-instruction check sequence
 *         emitted at every complex obligation site, branching to the site's
 *         existing trap path:
 *           - null deref (AIC-R0809): test <ptr>,<ptr>; jz .LtrapN BEFORE the
 *             deref pointer copy (the obligation MOV);
 *           - index bounds (AIC-R0807): cmp <index>,<len>; jae .LtrapN BEFORE
 *             the address LEA/ADD. The length is the compile-time array
 *             extent (from the IR base type) or, for slice/str bases, the
 *             pair-image length field at [base+8] (runtime);
 *           - cast range (AIC-R0801): the narrowing conversion range check
 *             (sign/zero-extension round trip) BEFORE the truncating MOV.
 *             17c1 cannot discover cast-range obligations (isel_core emits
 *             the narrowing MOV without a trap annotation; the obligation
 *             lives on the IR_CAST node's trap_code), so this package
 *             discovers them from the IR and emits a complete site + path +
 *             check sequence (documented in the header notes);
 *           - slice bounds (AIC-R0807 / AIC-R0808): start<=end and
 *             end<=base_len checks BEFORE the ISEL_SLICE pair construction,
 *             using the IR node's start/end operands and the base length
 *             (array extent or [base+8]);
 *           - UTF-8 validation (AIC-R0806): the byte validation loop AFTER
 *             the ISEL_UTF8 marker (data/len from the pair image);
 *           - pointer difference (AIC-R0810): byte-difference divisibility
 *             check AFTER the ISEL_PTRDIFF marker (power-of-two element
 *             sizes use a test-and-mask; other sizes use the documented
 *             idiv remainder check);
 *       * trap records that preserve the failing IR node's primary span AND
 *         its full causal chain (root cause first) per IR contract sec. 8.4:
 *         every CheckedSite carries the borrowed IrCauseLink array, and the
 *         emitted message text (the .LmsgN constant passed to
 *         rt.trap.report) is derived from code + span + causes -- never from
 *         plain strings;
 *   - a byte-deterministic checked assembly dump (checked_asm_dump) that
 *     renders the checked stream and the per-function site plan with span
 *     and cause chains. Identical inputs produce byte-identical dump bytes
 *     (acceptance criterion 1: "Checked operations emit trap records
 *     preserving source spans and causal chains" is demonstrated by the
 *     records, which carry span + causes for every obligation).
 *
 * Normative rules (documented implementation detail within the contract):
 *   1. Check-sequence classification. Every trap site whose code is a
 *      complex checked op -- AIC-R0801 (cast range), AIC-R0806 (UTF-8),
 *      AIC-R0807 (index/slice bounds), AIC-R0808 (str slice boundary),
 *      AIC-R0809 (null deref), AIC-R0810 (ptrdiff divisibility), AIC-R0816
 *      (pointer arithmetic overflow) -- receives its deterministic
 *      multi-instruction check sequence at the obligation body instruction
 *      in the stream (matched by ir_node_id + opcode), branching to the
 *      site's existing .LtrapN path. Sites whose code is a simple op
 *      (R0802 overflow, R0803 divisor zero, R0804 shift count, R0805 bool
 *      byte, and the ISEL_TRAP marker) already carry their branch from 17c1
 *      and pass through unchanged.
 *   2. Cast-range discovery. A cast-range obligation is any IR_CAST node
 *      whose trap_code is "AIC-R0801" and whose conversion can fail at
 *      runtime (target width < source width, or same-width signedness
 *      change). isel_core emits such conversions as a plain MOV with no trap
 *      annotation, so 17c1's framed-stream scan cannot discover them; this
 *      package scans the IR directly, matches the physical MOV by
 *      ir_node_id, and emits a complete trap site + path + range-check
 *      sequence. (Disclosed: this is the checked-op emission detail for cast
 *      range; it is not a change to 17c1's trap branch structure.)
 *   3. Span/cause preservation. Every trap record (CheckedSite) carries the
 *      failing IR node's primary span (borrowed) and the node's full cause
 *      chain (borrowed IrCauseLink array, root cause first -- IR contract
 *      sec. 8.2/8.4). The emitted message text is the deterministic
 *      rendering "<code> at <file>:<line>:<col>; <kind> at
 *      <file>:<line>:<col>" per cause link, root cause first; user traps
 *      keep the "user trap <code>" prefix. Message byte lengths are
 *      recomputed so the trap path's R8 immediate stays exact.
 *   4. Determinism. The pass walks only the 17c1 stream in emission order,
 *      the framed stream in emission order, and the IR's deterministic
 *      arrays (node ids are construction-order ids; causes are the nodes'
 *      owned chains). Check sequences are pure functions of the site code,
 *      the obligation body instruction's operands, and IR type facts. No
 *      pointer address, hash order, environment value, or host identity
 *      influences the stream or the dump (spec sec. 14.2).
 *
 * Ownership:
 *   - trap_checked_build returns an owned CheckedOutput (checked_output_free).
 *   - CheckedOutput borrows `build`, `fr`, `co`, and `to` (all must outlive
 *     it). Pass-through instructions are shallow copies; site code/span/
 *     cause pointers are borrowed from the IR; msgs are owned strings.
 *   - CheckedOutput.functions is in canonical function order (the 17c1
 *     plan's order, which is the IR canonical module/declaration order).
 *
 * Build (from the repository root; MSVC example) for the unit test:
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17c2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/trap_checked_test.c \
 *     bootstrap/src/backend/trap_checked.c \
 *     bootstrap/src/backend/trap_branch.c \
 *     bootstrap/src/backend/call.c \
 *     bootstrap/src/backend/frame.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17c2)
 */
#ifndef AICO_BOOTSTRAP_SRC_BACKEND_TRAP_CHECKED_H
#define AICO_BOOTSTRAP_SRC_BACKEND_TRAP_CHECKED_H

#include "trap_branch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Checked instruction stream
 *
 * The checked stream is the 17c1 trap-branched stream plus the check
 * sequences at the complex obligation sites and the trap paths (re-emitted
 * with cause-carrying messages). Pass-through TrapInsn entries are carried
 * as CHK_OP_BODY; check-sequence entries use the ops below.
 * ------------------------------------------------------------------------- */

typedef enum CheckedOp {
    CHK_OP_BODY = 0,       /* pass-through TrapInsn (17c1 stream) */
    CHK_OP_LABEL,          /* .LtrapN:  (imm = site index) */
    CHK_OP_JMP,            /* jmp .LtrapN  (imm = site index) */
    CHK_OP_JCC,            /* j<cond> .LtrapN  (imm = site index, cond) */
    CHK_OP_TEST,           /* test <src>, <src>  (flag-setter) */
    CHK_OP_CMP_IMM,        /* cmp <src>, $<imm>  (flag-setter) */
    CHK_OP_CMP_REG,        /* cmp <src1>, <src2>  (flag-setter) */
    CHK_OP_CMP_MEM,        /* cmp <src>, [<base>+<imm>]  (flag-setter;
                            * base is a CallOperand reg; width from src) */
    CHK_OP_MOV_LOAD,       /* mov <dst>, [<base>+<imm>]  (load to reg) */
    CHK_OP_MOV_REG,        /* mov <dst>, <src>  (reg copy) */
    CHK_OP_LOAD_IMM,       /* mov <dst>, $<imm> */
    CHK_OP_MOVSX,          /* movsx <dst>, <src>  (sign-extend) */
    CHK_OP_MOVZX,          /* movzx <dst>, <src>  (zero-extend) */
    CHK_OP_AND_IMM,        /* and <dst>, $<imm>  (mask) */
    CHK_OP_SHR_IMM,        /* shr <dst>, $<imm> */
    CHK_OP_SAR_IMM,        /* sar <dst>, $<imm> */
    CHK_OP_ADD_IMM,        /* add <dst>, $<imm> */
    CHK_OP_ADD_REG,        /* add <dst>, <src>  (reg-reg add) */
    CHK_OP_IDIV,           /* idiv <src>  (RDX:RAX / src; RAX=quot, RDX=rem) */
    CHK_OP_LOOP_LABEL,     /* .Lchk<N>:  (imm = label id) */
    CHK_OP_LOOP_JCC,       /* j<cond> .Lchk<N>  (imm = label id, cond) */
    CHK_OP_LOOP_JMP,       /* jmp .Lchk<N>  (imm = label id) */
    CHK_OP_SUB_RSP,        /* sub rsp, $imm (imm = 32; report shadow) */
    CHK_OP_MOV_CODE,       /* mov rcx, $imm (imm = numeric trap code) */
    CHK_OP_LEA_MSG,        /* lea rdx, [.Lmsg<imm>] (imm = msg index) */
    CHK_OP_MOV_LEN,        /* mov r8, $imm (imm = message byte length) */
    CHK_OP_CALL_REPORT     /* call fn<imm> (imm = report fn id) */
} CheckedOp;

const char *checked_op_text(CheckedOp op);

/* Branch conditions used by CHK_OP_JCC / CHK_OP_LOOP_JCC. */
typedef enum CheckedCond {
    CHK_COND_JZ = 0,       /* jump if zero */
    CHK_COND_JNZ,          /* jump if not zero */
    CHK_COND_JA,           /* jump if unsigned > */
    CHK_COND_JAE,          /* jump if unsigned >= */
    CHK_COND_JB,           /* jump if unsigned < */
    CHK_COND_JC,           /* jump if carry (unsigned overflow) */
    CHK_COND_JL,           /* jump if signed < */
    CHK_COND_JG,           /* jump if signed > */
    CHK_COND_JMP           /* unconditional */
} CheckedCond;

const char *checked_cond_text(CheckedCond cond);

typedef struct CheckedInsn {
    CheckedOp op;
    TrapInsn trap;         /* CHK_OP_BODY: the pass-through 17c1 entry */
    CallOperand dst;       /* register operand (MOV_LOAD / LOAD_IMM /
                            * MOVSX / MOVZX / AND_IMM / SHR_IMM / ADD_IMM) */
    CallOperand src;       /* CMP_MEM / MOV_LOAD / MOVSX / MOVZX / TEST /
                            * CMP_IMM operand */
    CallOperand base;      /* CMP_MEM / MOV_LOAD base register */
    CheckedCond cond;      /* CHK_OP_JCC / CHK_OP_LOOP_JCC condition */
    int64_t imm;           /* site index / message index / label id /
                            * immediate / bytes */
    int64_t ir_node_id;    /* originating IR node id (deterministic trace) */
} CheckedInsn;

/* ---------------------------------------------------------------------------
 * Per-function trap record plan (span/cause preserved)
 * ------------------------------------------------------------------------- */

typedef struct CheckedSite {
    int64_t site_index;      /* 0..n-1 within the function (stream order) */
    const char *code;        /* borrowed registry code ("AIC-R0807"/"user") */
    int64_t numeric_code;    /* u32 passed to rt.trap.report */
    const DiagSpan *span;    /* borrowed failing-operation span */
    const IrCauseLink *causes;  /* borrowed IR node cause chain, root cause
                                 * first (IR contract sec. 8.2/8.4) */
    size_t cause_count;
    int64_t msg_index;       /* index into CheckedOutput.msgs (owned text) */
    int64_t ir_node_id;      /* originating IR node id */
    bool unconditional;      /* ISEL_TRAP marker / cast-range site: branch
                              * is jmp after the check sequence */
} CheckedSite;

typedef struct CheckedFunction {
    int64_t function_id;     /* IR function node id */
    CheckedSite *sites;      /* owned array; stream order */
    size_t nsites;
    size_t start;            /* index of the function's first CheckedInsn */
    size_t count;            /* number of CheckedInsn in this function */
} CheckedFunction;

/* ---------------------------------------------------------------------------
 * CheckedOutput
 * ------------------------------------------------------------------------- */

typedef enum CheckedStatus {
    CHK_OK = 0,
    CHK_OOM              /* allocation failure; nothing owned */
} CheckedStatus;

typedef struct CheckedOutput {
    CheckedInsn *insns;    /* checked stream, emission order */
    size_t count;
    size_t cap;
    CheckedFunction *functions;  /* per function, canonical function order */
    size_t nfunctions;
    size_t functions_cap;
    char **msgs;           /* owned message texts (deterministic) */
    size_t nmsgs;
    size_t msgs_cap;
    int64_t entry_function_id;   /* main's IR node id, or -1 */
    int64_t report_function_id;  /* rt.trap.report's IR node id, or -1 */
    bool oom;              /* sticky allocation-failure flag */
} CheckedOutput;

/* Build the checked stream from the 17c1 trap-branched stream. `build` must
 * be the IR build the pipeline was made from; `fr`, `co`, and `to` the 17b1
 * framed stream, 17b2 call output, and 17c1 TrapOutput respectively (all
 * must outlive the output). Returns CHK_OK with *out owned by the caller,
 * or CHK_OOM with *out NULL. */
CheckedStatus trap_checked_build(const IrBuild *build, const FrameOutput *fr,
                                 const CallOutput *co, const TrapOutput *to,
                                 CheckedOutput **out);

void checked_output_free(CheckedOutput *out);

/* Accessors for tests and later packages. */
size_t checked_output_count(const CheckedOutput *out);
const CheckedInsn *checked_output_insn(const CheckedOutput *out, size_t i);
size_t checked_function_count(const CheckedOutput *out);
const CheckedFunction *checked_function_at(const CheckedOutput *out, size_t i);
const CheckedFunction *checked_function_for(const CheckedOutput *out,
                                            int64_t function_id);
const CheckedSite *checked_function_site(const CheckedFunction *cf, size_t i);
const char *checked_message(const CheckedOutput *out, size_t msg_index);

/* ---------------------------------------------------------------------------
 * Deterministic checked assembly dump
 * ------------------------------------------------------------------------- */

/* Render the checked stream as stable text appended to `out` (DiagBuf, e.g.
 * diag_buf_init): the plan header, then per function the stream with the
 * check sequences interleaved and the trap paths, then the per-site plan
 * with span and cause chains, then the message constants. Byte-
 * deterministic: identical inputs produce identical dump bytes. Returns
 * false on allocation failure (out->oom set). */
bool checked_asm_dump(const CheckedOutput *co, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_SRC_BACKEND_TRAP_CHECKED_H */
