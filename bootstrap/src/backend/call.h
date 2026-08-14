/* bootstrap/src/backend/call.h
 *
 * AI-Co Stage-0 x86-64 register allocation and call emission
 * (WP-M0-17b2).
 *
 * Implements the register-allocation and call-emission obligations of
 * spec sec. 15.7 (Microsoft x64 convention, internal compiler-to-runtime
 * contract) on top of the 17b1 framed instruction stream (FrameOutput):
 * deterministic simple register allocation, and call sites that follow
 * the convention (integer/pointer args in RCX/RDX/R8/R9, stack args
 * right-to-left above the 32-byte shadow space, RSP 16-byte aligned at
 * every call site, return value in RAX).
 *
 * Scope (manifest WP-M0-17b2): register allocation (simple/deterministic)
 * and call emission. Excluded (owned by later packages): frame layout
 * (17b1, frame.*); instruction selection (17a1); x86-64 coverage
 * (17a2); trap branch emission (17c1); checked-operation emission (17c2);
 * COFF emission (WP-M0-18).
 *
 * What this package delivers:
 *   - call_build, a deterministic pass over a 17b1 FrameOutput plus the
 *     IR build that computes, for every function in canonical order, a
 *     CallFunction plan (spill area, slice-pair temps, shadow space,
 *     stack-argument area, callee-saved register set, total stack
 *     reservation) and emits a register-allocated physical instruction
 *     stream (CallOutput.insns): the final prologue (push rbp; mov
 *     rbp,rsp; [save callee-saved]; sub rsp,total), incoming-parameter
 *     copies (registers 0-3 and stack params 4+ into their frame slots;
 *     composite params copied by value via REP MOVSB), the body lowered
 *     to physical registers with values spilled to call-owned slots,
 *     call sequences (arg moves, call, RAX result capture), and the
 *     epilogue (RAX return-value load while RBP still addresses the
 *     current frame; mov rsp,rbp; [restore callee-saved and RSP back
 *     to RBP]; pop rbp; ret);
 *   - a byte-deterministic physical assembly dump (call_asm_dump) that
 *     renders the plan and the stream as stable text. Identical inputs
 *     produce byte-identical dump bytes (acceptance criterion 1:
 *     "Runtime calls follow the sec. 15.7 convention" is demonstrated by
 *     the emitted call sequences and their deterministic dump).
 *
 * Normative rules (documented implementation detail within the
 * contract; the exact x64 convention is spec sec. 15.7):
 *   1. Value placement. Every virtual register v is materialized in an
 *      8-byte call-owned spill slot below the function's 17b1 frame:
 *      slot(v) = [rbp - frame_size - 8*(v+1)]. Slots always hold the
 *      value's bits zero-extended to 8 bytes (narrow results are
 *      widened before the store), so loads from slots are unconditional
 *      64-bit moves.
 *   2. Instruction lowering. Every machine instruction is lowered to a
 *      two-scratch-register form (R10, R11): source vregs are loaded
 *      64-bit from their slots into scratch registers, the instruction
 *      executes on physical registers (immediates, frame/symbolic
 *      operands kept), and the destination is stored back. R10/R11 are
 *      caller-saved and never live across instructions (values live in
 *      slots), so no liveness analysis is needed.
 *   3. Special-register obligations. ISEL_IDIV uses the RAX/RDX pair
 *      (dividend sign-extended into RDX:RAX; quotient in RAX,
 *      remainder in RDX; the mod result is read from RDX); variable
 *      shifts put the count in CL; REP MOVSB/STOSB use RDI/RSI/RCX
 *      (REP STOSB uses RDI/RCX and zeroes RAX). A function that
 *      executes a REP op saves the callee-saved registers it clobbers
 *      (RDI for any rep op, RSI for REP MOVSB and composite-parameter
 *      copies) in its prologue and restores them in its epilogue, so
 *      the sec. 15.7 callee-saved contract holds.
 *   4. Call emission (acceptance criterion). Args 0-3 move (64-bit)
 *      into RCX/RDX/R8/R9; args 4+ store into the call-owned
 *      stack-argument area above the 32-byte shadow space ([rsp+32],
 *      [rsp+40], ...); the shadow space, stack-argument area, spill
 *      slots, and pair temps are reserved in the function's prologue
 *      (total is 16-aligned and adjusted for any callee-saved pushes),
 *      so RSP is 16-byte aligned at every call site and never moves in
 *      the body; after `call`, a non-void result is captured from RAX
 *      into the result vreg's slot. Composite args are passed
 *      address-resident (their vreg holds the composite's address); the
 *      callee copies into its parameter slots (IR contract sec. 5.3).
 *   5. Determinism and pseudo pass-through. call_build iterates only
 *      the framed stream in emission order and the IR's deterministic
 *      arrays; every output is a pure function of the framed stream and
 *      the IR (vreg numbers are the contract's deterministic
 *      first-visit order; slot offsets are 17b1's). Pseudo-ops whose
 *      machine expansion is owned by 17c (ISEL_STRCMP, ISEL_SLICEEQ,
 *      ISEL_UTF8, ISEL_PTRDIFF, ISEL_TRAP) pass through verbatim as
 *      annotated markers (their vreg operands stay symbolic); the
 *      call-owned lowerings (REP MOVSB, REP STOSB, ISEL_SLICE pair
 *      materialization) are emitted as physical instructions. Identical
 *      inputs produce byte-identical streams and dumps (spec sec. 14.2).
 *
 * Determinism. The vreg -> slot mapping is a pure function of the vreg
 * number; per-function vreg counts come from the maximum vreg number
 * observed in the function's region of the framed stream; pair temps are
 * allocated per ISEL_SLICE in stream order; the callee-saved set and
 * stack-arg area come from scanning the function's calls. No pointer
 * address, hash order, environment value, or host identity influences
 * the stream or the dump.
 *
 * Ownership:
 *   - call_build returns an owned CallOutput (call_output_free).
 *   - CallOutput borrows `build` and `fr`: both must outlive it. Body
 *     instructions and pseudo markers are shallow copies; note and trap
 *     code pointers remain borrowed from the selection/IR.
 *   - CallFunction arrays are owned by the CallOutput and freed by
 *     call_output_free.
 *   - CallOutput.functions is in canonical function order: the order in
 *     which the framed stream emits "function <name>" comments, which is
 *     the IR canonical module/declaration order.
 *
 * Build (from the repository root; MSVC example) for the unit test:
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17b2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/call_test.c \
 *     bootstrap/src/backend/call.c \
 *     bootstrap/src/backend/frame.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17b2)
 */
#ifndef AICO_BOOTSTRAP_SRC_BACKEND_CALL_H
#define AICO_BOOTSTRAP_SRC_BACKEND_CALL_H

#include "frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Physical x86-64 registers (standard x86-64 encoding order)
 * ------------------------------------------------------------------------- */

typedef enum X64Reg {
    X64_REG_RAX = 0,
    X64_REG_RCX,
    X64_REG_RDX,
    X64_REG_RBX,
    X64_REG_RSP,
    X64_REG_RBP,
    X64_REG_RSI,
    X64_REG_RDI,
    X64_REG_R8,
    X64_REG_R9,
    X64_REG_R10,
    X64_REG_R11,
    X64_REG_R12,
    X64_REG_R13,
    X64_REG_R14,
    X64_REG_R15
} X64Reg;

const char *x64_reg_text(X64Reg reg);

/* ---------------------------------------------------------------------------
 * Physical operands
 * ------------------------------------------------------------------------- */

typedef enum CallOperandKind {
    CALL_OPR_NONE = 0,
    CALL_OPR_REG,       /* id = X64Reg */
    CALL_OPR_IMM,       /* imm value; width; is_unsigned for rendering */
    CALL_OPR_MEM,       /* base register (id = X64Reg) + displacement (imm);
                         * width; base X64_REG_RBP is the frame pointer */
    CALL_OPR_GLOBAL,    /* id = IR_GLOBAL_VAR node id (symbolic) */
    CALL_OPR_STR,       /* id = const id (symbolic string) */
    CALL_OPR_FUNC,      /* id = IR_FUNCTION node id (symbolic) */
    CALL_OPR_LABEL      /* id = control-flow label id */
} CallOperandKind;

typedef struct CallOperand {
    CallOperandKind kind;
    int64_t id;         /* REG/GLOBAL/STR/FUNC/LABEL id */
    int64_t imm;        /* IMM value; MEM displacement */
    int width;          /* operand width in bytes (1/2/4/8), 0 when none */
    bool is_unsigned;   /* IMM: render as unsigned decimal */
} CallOperand;

CallOperand call_operand_none(void);

/* ---------------------------------------------------------------------------
 * Physical instruction stream
 * ------------------------------------------------------------------------- */

typedef enum CallOp {
    CALL_OP_BODY = 0,      /* physical machine instruction (isel opcode,
                            * physical operands) */
    CALL_OP_PSEUDO,        /* pass-through pseudo-op / comment marker
                            * (isel body verbatim, vregs symbolic) */
    CALL_OP_PUSH_RBP,      /* push rbp */
    CALL_OP_MOV_RBP_RSP,   /* mov rbp, rsp */
    CALL_OP_SUB_RSP,       /* sub rsp, imm  (imm = reservation size) */
    CALL_OP_ADD_RSP,       /* add rsp, imm  (imm = reservation size) */
    CALL_OP_MOV_RSP_RBP,   /* mov rsp, rbp */
    CALL_OP_POP_RBP,       /* pop rbp */
    CALL_OP_PUSH_REG,      /* save a callee-saved register (imm = X64Reg) */
    CALL_OP_POP_REG        /* restore a callee-saved register (imm = X64Reg) */
} CallOp;

typedef struct CallInsn {
    CallOp op;
    IselOpcode isel;   /* CALL_OP_BODY: machine opcode; CALL_OP_PSEUDO:
                        * the pseudo/comment opcode */
    CallOperand dst;
    CallOperand src1;
    CallOperand src2;
    IselCond cond;     /* ISEL_JCC / ISEL_SETCC condition; else 0 */
    int scale;         /* ISEL_LEA scale / ISEL_SLICEEQ element size */
    bool mod;          /* ISEL_IDIV: true when lowering IR_MOD */
    int64_t imm;       /* CALL_OP_SUB_RSP / CALL_OP_ADD_RSP: bytes;
                        * CALL_OP_PUSH/POP_REG: X64Reg value */
    IselInsn pseudo;   /* CALL_OP_PSEUDO: the original (vreg) instruction
                        * (shallow copy; note/trap pointers borrowed) */
    int64_t ir_node_id;/* originating IR node id (deterministic trace) */
} CallInsn;

/* ---------------------------------------------------------------------------
 * Per-function plan
 * ------------------------------------------------------------------------- */

typedef struct CallFunction {
    int64_t function_id;     /* IR function node id */
    int64_t frame_size;      /* 17b1 frame reservation (bytes) */
    int64_t nvregs;          /* spill slot count (max vreg in region + 1) */
    size_t nslice;           /* ISEL_SLICE count (16-byte pair temps) */
    bool has_calls;          /* region contains ISEL_CALL / ISEL_CALL_TERM */
    size_t max_stack_args;   /* max args beyond the 4 register args */
    bool saves_rdi;          /* prologue saves RDI (rep ops / param copies) */
    bool saves_rsi;          /* prologue saves RSI (REP MOVSB / param copies) */
    int64_t spill_bytes;     /* 8 * nvregs */
    int64_t pair_bytes;      /* 16 * nslice */
    int64_t shadow_bytes;    /* 32 when has_calls */
    int64_t stackarg_bytes;  /* 8 * max_stack_args */
    int64_t saved_bytes;     /* 8 * (saves_rdi + saves_rsi) */
    int64_t total;           /* align16(base + saved_bytes) adjusted so
                              * (total + saved_bytes) % 16 == 0 */
    size_t start;            /* index of the function's first CallInsn */
    size_t count;            /* number of CallInsn in this function */
} CallFunction;

/* ---------------------------------------------------------------------------
 * CallOutput
 * ------------------------------------------------------------------------- */

typedef enum CallStatus {
    CALL_OK = 0,
    CALL_OOM              /* allocation failure; nothing owned */
} CallStatus;

typedef struct CallOutput {
    CallInsn *insns;       /* physical stream, emission order */
    size_t count;
    size_t cap;
    CallFunction *functions;  /* per function, canonical function order */
    size_t nfunctions;
    size_t functions_cap;
    int64_t entry_function_id;  /* main's IR node id, or -1 */
    bool oom;              /* sticky allocation-failure flag */
} CallOutput;

/* Register-allocate and emit calls for a whole framed stream. `build`
 * must be the IR build the selection/frame were made from; `fr` must be
 * a valid 17b1 FrameOutput. Returns CALL_OK with *out owned by the
 * caller, or CALL_OOM with *out NULL. The output borrows `build` and
 * `fr` (both must outlive it). */
CallStatus call_build(const IrBuild *build, const FrameOutput *fr,
                      CallOutput **out);

void call_output_free(CallOutput *out);

/* Accessors for tests and later packages. */
size_t call_output_count(const CallOutput *out);
const CallInsn *call_output_insn(const CallOutput *out, size_t i);
size_t call_function_count(const CallOutput *out);
const CallFunction *call_function_at(const CallOutput *out, size_t i);
const CallFunction *call_function_for(const CallOutput *out,
                                      int64_t function_id);

/* ---------------------------------------------------------------------------
 * Deterministic physical assembly dump
 * ------------------------------------------------------------------------- */

/* Render the physical stream as stable text appended to `out` (DiagBuf,
 * e.g. diag_buf_init): the call plan header, then per function the final
 * prologue, incoming-parameter copies, the lowered body (physical
 * registers, [rbp-<off>] spill/frame operands, call sequences), and the
 * epilogue restore before each ret. Byte-deterministic: identical
 * inputs produce identical dump bytes. Returns false on allocation
 * failure (out->oom set). */
bool call_asm_dump(const CallOutput *co, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_SRC_BACKEND_CALL_H */
