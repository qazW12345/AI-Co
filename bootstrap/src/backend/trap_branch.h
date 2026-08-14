/* bootstrap/src/backend/trap_branch.h
 *
 * AI-Co Stage-0 x86-64 trap branch emission (WP-M0-17c1).
 *
 * Implements the trap-branch obligations of spec sec. 14.3 and 15.5/15.8
 * (runtime-failable checked operations compile to a branch to a trap-report
 * call with the stable trap code and the failing operation's source span)
 * on top of the 17b2 physical stream (CallOutput): deterministic trap
 * branches with stable AIC-R codes and source spans.
 *
 * Scope (manifest WP-M0-17c1): trap branch emission (AIC-R0801..R0816 per
 * operation); stable codes and source spans. Excluded (owned by later
 * packages): checked-op emission details (17c2, trap_checked.* -- the
 * multi-instruction check sequences for the pseudo-ops ISEL_SLICE /
 * ISEL_UTF8 / ISEL_PTRDIFF / index bounds / null deref / cast range and the
 * span/cause preservation on trap records); instruction selection (17a1,
 * isel_core.*); x86-64 coverage/constraint checks (17a2, isel_x64.*);
 * frame layout and register allocation/call emission (17b, frame and
 * call packages); COFF emission (WP-M0-18).
 *
 * What this package delivers:
 *   - trap_branch_build, a deterministic pass over a 17b2 physical stream
 *     plus its 17b1 framed stream and IR build that computes, for every
 *     function in canonical order, a trap-site plan (one deterministic trap
 *     site per trap obligation in stream order: a label, the stable
 *     AIC-Rxxxx code, the numeric u32 code passed to rt.trap.report, and the
 *     failing operation's source span from the IR node) and emits a
 *     trap-branched physical stream (TrapOutput.insns): the original
 *     instructions with, at each obligation site, the deterministic trap
 *     branch (an unconditional jmp for the ISEL_TRAP marker, or the
 *     flag-direct conditional branch with the minimal flag-setting
 *     test/cmp for the simple checked ops: overflow jo/jc after
 *     add/sub/mul/neg, divisor-zero test+jz before idiv, shift-count
 *     cmp+jae before a variable-count shift, bool-byte cmp+ja after a bool
 *     load), and at the end of each function the trap paths: for each site,
 *     `.LtrapN:` then `sub rsp, $32` (the report call's shadow space),
 *     `mov rcx, $<code>`, `lea rdx, [.LmsgN]` (the message str pair image
 *     address), and `call rt.trap.report` (noreturn; nothing after);
 *   - a byte-deterministic trap assembly dump (trap_asm_dump) that renders
 *     the trap-branched stream and the per-function site plan as stable
 *     text. Identical inputs produce byte-identical dump bytes (acceptance
 *     criterion 1: "every runtime-failable checked operation emits a
 *     deterministic trap branch with the correct AIC-Rxxxx code and source
 *     span" is demonstrated by the emitted branches/paths and the site
 *     plan, which carries code + span for every obligation).
 *
 * Normative rules (documented implementation detail within the contract):
 *   1. Obligation discovery. A trap obligation is any body instruction in
 *      the framed stream whose IselInsn carries a non-NULL trap code (the
 *      registry string selected by isel_core: R0801..R0816, or "user" for
 *      rt.trap.report user traps) -- this includes the ISEL_TRAP pseudo
 *      marker (unconditional) and the checked ops. The physical stream
 *      counterpart is the first instruction with the same ir_node_id and
 *      the same opcode (pseudo markers match as CALL_OP_PSEUDO; a bool
 *      load is the REG-destination MOV). Every obligation gets exactly one
 *      trap site, in stream order.
 *   2. Trap-site plan. Per function, sites are numbered 0..n-1 in stream
 *      order; the site label is `.Ltrap<k>`. Each site records the stable
 *      code string (borrowed), the numeric u32 code (the AIC-R suffix as
 *      hex: AIC-R0801 -> 0x0801 = 2049; user traps pass the caller's u32),
 *      and the source span (borrowed from the IR node). The message text
 *      is deterministic: "<code> at <file>:<line>:<col>" (user traps:
 *      "user trap <code> at <file>:<line>:<col>").
 *   3. Branch emission. ISEL_TRAP markers become `jmp .LtrapN`. Checked
 *      ops whose failure condition is a direct flag result of the operation
 *      or a single minimal flag-setting test get the complete deterministic
 *      trap branch here: overflow (R0802) on add/sub/mul/neg -> jo (signed
 *      result type) or jc (unsigned) immediately after the operation;
 *      divisor zero (R0803) -> test <divisor>,<divisor> + jz immediately
 *      before idiv (a constant divisor is verified non-zero, so no test);
 *      shift count (R0804) -> cmp cl,$<width-bits> + jae before a
 *      variable-count shift (constant counts verified in range, so no
 *      test); invalid bool byte (R0805) -> cmp <byte>, $1 + ja after the
 *      bool load. All other obligations (ISEL_SLICE / ISEL_UTF8 /
 *      ISEL_PTRDIFF, index bounds, null deref, cast range) receive their
 *      trap site and path here and pass through the stream unchanged as
 *      annotated markers; their multi-instruction check sequences and the
 *      span/cause preservation on trap records are the checked-op emission
 *      details owned by WP-M0-17c2.
 *   4. Trap path. At the end of each function (after the last physical
 *      instruction of its region), each site emits: `.LtrapN:`, `sub rsp,
 *      $32` (the 32-byte shadow space the report call's caller must
 *      reserve, sec. 15.7; RSP is 16-byte aligned and never restored --
 *      rt.trap.report is noreturn), `mov rcx, $<code>` (stable numeric
 *      code), `lea rdx, [.LmsgN]` (address of the message str pair image,
 *      the composite-argument convention of 17b2), and `call rt.trap.report`
 *      with nothing after it (noreturn; no epilogue, frame is never
 *      corrupted). Trap paths are reachable only via the trap branches.
 *
 * Determinism. trap_branch_build iterates only the physical stream in
 * emission order, the framed stream in emission order, and the IR's
 * deterministic arrays (function node ids are the contract's
 * construction-order ids; spans are the nodes' owned spans; the message
 * text is a pure function of code + span). The dump renders the stream in
 * emission order with stable formatting (fixed mnemonics, decimal
 * integers, symbolic references by id, no timestamps/pointers/paths).
 * Identical inputs always yield byte-identical trap dumps (spec
 * sec. 14.2).
 *
 * Ownership:
 *   - trap_branch_build returns an owned TrapOutput (trap_output_free).
 *   - TrapOutput borrows `build`, `fr`, and `co`: all must outlive it.
 *     Pass-through instructions are shallow copies of the physical stream
 *     (note/trap code pointers remain borrowed from the selection/IR);
 *     the site plan borrows code strings and spans from the IR.
 *   - TrapOutput.msgs are owned strings (freed by trap_output_free).
 *   - TrapOutput.functions is in canonical function order: the order in
 *     which the physical stream emits "function <name>" comments, which is
 *     the IR canonical module/declaration order.
 *
 * Build (from the repository root; MSVC example) for the unit test:
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17c1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/trap_branch_test.c \
 *     bootstrap/src/backend/trap_branch.c \
 *     bootstrap/src/backend/call.c \
 *     bootstrap/src/backend/frame.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17c1)
 */
#ifndef AICO_BOOTSTRAP_SRC_BACKEND_TRAP_BRANCH_H
#define AICO_BOOTSTRAP_SRC_BACKEND_TRAP_BRANCH_H

#include "call.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Trap-branched instruction stream
 * ------------------------------------------------------------------------- */

typedef enum TrapOp {
    TRAP_OP_BODY = 0,    /* pass-through physical instruction (CallInsn copy) */
    TRAP_OP_LABEL,       /* .Ltrap<k>:  (imm = site index) */
    TRAP_OP_JMP,         /* jmp .Ltrap<k>  (imm = site index) */
    TRAP_OP_JCC,         /* j<cond> .Ltrap<k>  (imm = site index, cond) */
    TRAP_OP_TEST,        /* test <src1>, <src1>  (flag-setter; imm = 0) */
    TRAP_OP_CMP_IMM,     /* cmp <src1>, $<imm>   (flag-setter) */
    TRAP_OP_SUB_RSP,     /* sub rsp, $imm (imm = 32; report-call shadow) */
    TRAP_OP_MOV_CODE,    /* mov rcx, $imm (imm = numeric trap code) */
    TRAP_OP_LEA_MSG,     /* lea rdx, [.Lmsg<imm>] (imm = message index) */
    TRAP_OP_CALL_REPORT  /* call fn<report> (imm = report fn id) */
} TrapOp;

const char *trap_op_text(TrapOp op);

/* Branch conditions the trap branches use. IselCond does not cover the
 * overflow/carry tests, so trap_branch owns its closed set. */
typedef enum TrapCond {
    TRAP_COND_JZ = 0,    /* jump if zero        (divisor zero) */
    TRAP_COND_JA,        /* jump if unsigned >  (bool byte > 1) */
    TRAP_COND_JAE,       /* jump if unsigned >= (shift count out of range) */
    TRAP_COND_JO,        /* jump if overflow    (signed overflow) */
    TRAP_COND_JC,        /* jump if carry       (unsigned overflow) */
    TRAP_COND_JMP        /* unconditional       (ISEL_TRAP) */
} TrapCond;

const char *trap_cond_text(TrapCond cond);

typedef struct TrapInsn {
    TrapOp op;
    CallInsn call;       /* TRAP_OP_BODY: the pass-through instruction
                          * (shallow copy; note/trap pointers borrowed) */
    TrapCond cond;       /* TRAP_OP_JCC condition */
    CallOperand src;     /* TRAP_OP_TEST / TRAP_OP_CMP_IMM operand */
    int64_t imm;         /* site index / immediate / message index / bytes */
    int64_t ir_node_id;  /* originating IR node id (deterministic trace) */
} TrapInsn;

/* ---------------------------------------------------------------------------
 * Per-function trap-site plan
 * ------------------------------------------------------------------------- */

typedef struct TrapSite {
    int64_t site_index;      /* 0..n-1 within the function (stream order) */
    const char *code;        /* borrowed registry code ("AIC-R0802"/"user") */
    int64_t numeric_code;    /* u32 passed to rt.trap.report (hex AIC-R
                              * suffix, or the user code) */
    const DiagSpan *span;    /* borrowed failing-operation span */
    int64_t msg_index;       /* index into TrapOutput.msgs (owned text) */
    int64_t ir_node_id;      /* originating IR node id */
    bool unconditional;      /* ISEL_TRAP marker: branch is jmp */
} TrapSite;

typedef struct TrapFunction {
    int64_t function_id;     /* IR function node id */
    TrapSite *sites;         /* owned array; stream order */
    size_t nsites;
    size_t start;            /* index of the function's first TrapInsn
                              * (its "function <name>" marker) */
    size_t count;            /* number of TrapInsn in this function */
} TrapFunction;

/* ---------------------------------------------------------------------------
 * TrapOutput
 * ------------------------------------------------------------------------- */

typedef enum TrapStatus {
    TRAP_OK = 0,
    TRAP_OOM              /* allocation failure; nothing owned */
} TrapStatus;

typedef struct TrapOutput {
    TrapInsn *insns;       /* trap-branched stream, emission order */
    size_t count;
    size_t cap;
    TrapFunction *functions;  /* per function, canonical function order */
    size_t nfunctions;
    size_t functions_cap;
    char **msgs;           /* owned message texts (deterministic) */
    size_t nmsgs;
    size_t msgs_cap;
    int64_t entry_function_id;  /* main's IR node id, or -1 */
    int64_t report_function_id; /* rt.trap.report's IR node id, or -1 when
                                 * the build does not declare it (defensive;
                                 * verified IR always declares the runtime
                                 * module) */
    bool oom;              /* sticky allocation-failure flag */
} TrapOutput;

/* Emit trap branches for a whole physical stream. `build` must be the IR
 * build the selection/frame/call were made from; `fr` must be the 17b1
 * framed stream the physical stream was made from (it carries the trap
 * annotations the physical stream drops); `co` must be a valid 17b2
 * CallOutput. Returns TRAP_OK with *out owned by the caller, or TRAP_OOM
 * with *out NULL. The output borrows `build`, `fr`, and `co` (all must
 * outlive it). */
TrapStatus trap_branch_build(const IrBuild *build, const FrameOutput *fr,
                             const CallOutput *co, TrapOutput **out);

void trap_output_free(TrapOutput *out);

/* Accessors for tests and later packages. */
size_t trap_output_count(const TrapOutput *out);
const TrapInsn *trap_output_insn(const TrapOutput *out, size_t i);
size_t trap_function_count(const TrapOutput *out);
const TrapFunction *trap_function_at(const TrapOutput *out, size_t i);
const TrapFunction *trap_function_for(const TrapOutput *out,
                                      int64_t function_id);
const TrapSite *trap_function_site(const TrapFunction *tf, size_t i);
const char *trap_message(const TrapOutput *out, size_t msg_index);

/* ---------------------------------------------------------------------------
 * Deterministic trap assembly dump
 * ------------------------------------------------------------------------- */

/* Render the trap-branched stream as stable text appended to `out`
 * (DiagBuf, e.g. diag_buf_init): the trap plan header, then per function
 * the pass-through physical stream with the trap branches interleaved at
 * their obligation sites, the trap paths (`.LtrapN:` + shadow + code +
 * message + report call) after the function's last instruction, and a
 * message-constant section. Byte-deterministic: identical inputs produce
 * identical dump bytes. Returns false on allocation failure (out->oom
 * set). */
bool trap_asm_dump(const TrapOutput *to, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_SRC_BACKEND_TRAP_BRANCH_H */
