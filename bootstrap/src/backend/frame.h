/* bootstrap/src/backend/frame.h
 *
 * AI-Co Stage-0 x86-64 stack frame layout and function prologue/epilogue
 * (WP-M0-17b1).
 *
 * Implements the frame obligations of spec sec. 15.7 (Microsoft x64
 * convention, internal compiler-to-runtime contract) and sec. 14.3
 * (x86-64 target): deterministic per-function stack frame layout, the
 * frame-pointer prologue/epilogue instruction sequences, `main` entry
 * setup, and noreturn handling.
 *
 * Scope (manifest WP-M0-17b1): frame layout; prologue/epilogue; `main`
 * entry setup; noreturn handling. Excluded (owned by later packages):
 * register allocation and call emission (17b2, call.*); instruction
 * selection (17a1, isel_core.*); x86-64 coverage/constraint checks
 * (17a2, isel_x64.*, AIC-B0601); trap branch emission (17c1);
 * checked-operation emission (17c2); COFF emission (WP-M0-18).
 *
 * What this package delivers:
 *   - frame_build, a deterministic pass over a 17a1 selection
 *     (IselOutput) plus its IR build that computes, for every function
 *     in canonical order, a FrameLayout: an RBP-relative stack offset
 *     for every function slot (params first, then locals, then temps,
 *     per the IR contract sec. 4.3), the total frame size (16-byte
 *     aligned so the standard prologue keeps RSP 16-byte aligned at
 *     call sites), the entry marking for `main`, and the noreturn
 *     marking;
 *   - a framed instruction stream (FrameOutput.insns): the original
 *     selection with the frame-pointer prologue emitted at each
 *     function start, the epilogue restore emitted before each RET,
 *     and every symbolic slot operand rewritten to an RBP-relative
 *     memory operand (ISEL_OP_MEM with base FRAME_BASE_VREG = -1);
 *   - a byte-deterministic framed assembly dump (frame_asm_dump) that
 *     renders prologue, body, and epilogue. Identical inputs produce
 *     byte-identical dump bytes (acceptance criterion 1: "prologue/
 *     epilogue and main entry setup correct; noreturn handled without
 *     corrupting the frame" is demonstrated by the framed stream and
 *     its deterministic dump).
 *
 * Normative rules (documented implementation detail within the
 * contract; the exact x64 convention is spec sec. 15.7):
 *   1. Slot layout. Function slots are laid out below RBP in slot-index
 *      order (params first, then locals, then temps; IR contract
 *      sec. 4.3). Each slot's base address (rbp + offset) is aligned to
 *      the slot type's alignment (spec sec. 7.1-7.5 fixed facts); the
 *      layout is computed from a single downward cursor so offsets are
 *      deterministic and gapless with no overlap. Offsets are negative
 *      (RBP-relative). Example (i32 at slot 0, i32 at slot 1): offsets
 *      -4 and -8; a following u8 gets offset -9.
 *   2. Frame size. frame_size = align_up(total_slot_bytes, 16). The
 *      prologue `sub rsp, frame_size` therefore keeps RSP 16-byte
 *      aligned at every call site in the body (entry convention:
 *      RSP == 8 mod 16 at function entry because the caller reserved a
 *      return slot and kept RSP 16-byte aligned before the call; after
 *      `push rbp` RSP is 16-byte aligned and frame_size preserves it).
 *      A function with no slots gets frame_size 0 and no SUB step.
 *   3. Prologue. Exactly: `push rbp; mov rbp, rsp; [sub rsp,
 *      frame_size]` (the SUB is emitted only when frame_size > 0).
 *      This is the frame-pointer convention: RBP is the fixed frame
 *      pointer; register allocation (17b2) assigns other registers.
 *   4. Epilogue. Before every ISEL_RET in a returning function,
 *      exactly: `mov rsp, rbp; pop rbp` followed by the RET itself
 *      (the RET stays in the stream with its return-value vreg). The
 *      restore is RBP-based so it does not depend on frame_size at the
 *      return site.
 *   5. main entry setup. The entry function is the function named
 *      `main` in the entry module (build->modules[0], the entry module
 *      per the IR contract); it is marked is_entry and recorded as
 *      FrameOutput.entry_function_id. main uses the same standard
 *      prologue/epilogue as every other function under the documented
 *      entry convention (RSP == 8 mod 16 at entry, return address at
 *      [rsp]); the PE-entry stub that reaches main under this
 *      convention is owned by the driver/link packages (WP-M0-18/19).
 *   6. Noreturn handling. A function whose IR node carries noreturn
 *      (only rt.proc.exit / rt.trap.report per ir_core invariant 3, and
 *      only as bodyless external declarations in verified IR) receives
 *      no frame at all: it has no body instructions, so no prologue/
 *      epilogue is emitted (the call to it is emitted by 17b2). A
 *      function whose body ends in a noreturn call terminator
 *      (IR_CALL_TERM) or a trap terminator receives the standard
 *      prologue but no epilogue after the terminator: the stream has no
 *      ISEL_RET after it, so the frame never emits restore/ret code
 *      after a never-returning call, and the frame is fully established
 *      (prologue) when the call executes - the frame is never
 *      corrupted. Defensive: a noreturn function with a body (not
 *      representable in verified IR) gets a prologue but no epilogue.
 *
 * Determinism. frame_build iterates only the selection in emission
 * order and the IR's deterministic arrays (function node ids are the
 * contract's construction-order ids); slot offsets and frame sizes are
 * pure functions of the IR slot types. The dump renders the stream in
 * emission order with stable formatting (fixed mnemonics, decimal
 * integers, symbolic references by id, no timestamps/pointers/paths).
 * Identical inputs always yield byte-identical framed dumps (spec
 * sec. 14.2).
 *
 * Ownership:
 *   - frame_build returns an owned FrameOutput (frame_output_free).
 *   - FrameOutput borrows `build` and `sel`: both must outlive it.
 *     Body instructions are shallow copies of the selection's
 *     instructions; comment note pointers and trap code pointers remain
 *     borrowed from the selection/IR.
 *   - FrameLayout.slots arrays are owned by the FrameOutput and freed
 *     by frame_output_free.
 *   - FrameOutput.layouts is in canonical function order: the order in
 *     which the selection emits "function <name>" comments, which is
 *     the IR canonical module/declaration order.
 *
 * Build (from the repository root; MSVC example) for the unit test:
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17b1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/frame_test.c \
 *     bootstrap/src/backend/frame.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17b1)
 */
#ifndef AICO_BOOTSTRAP_SRC_BACKEND_FRAME_H
#define AICO_BOOTSTRAP_SRC_BACKEND_FRAME_H

#include "isel_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The frame base in rewritten memory operands. After frame_build, every
 * ISEL_OP_MEM operand whose base vreg is FRAME_BASE_VREG means
 * "the frame pointer (RBP) plus the displacement field". Register
 * allocation (17b2) maps this sentinel to RBP. */
#define FRAME_BASE_VREG ((int64_t)-1)

/* ---------------------------------------------------------------------------
 * Framed instruction stream
 * ------------------------------------------------------------------------- */

typedef enum FrameOp {
    FRAME_OP_BODY = 0,   /* one (rewritten) body instruction from the selection */
    FRAME_OP_PUSH_RBP,   /* push rbp */
    FRAME_OP_MOV_RBP_RSP,/* mov rbp, rsp */
    FRAME_OP_SUB_RSP,    /* sub rsp, imm  (imm = frame_size) */
    FRAME_OP_MOV_RSP_RBP,/* mov rsp, rbp */
    FRAME_OP_POP_RBP     /* pop rbp */
} FrameOp;

const char *frame_op_text(FrameOp op);

typedef struct FrameInsn {
    FrameOp op;
    IselInsn body;   /* FRAME_OP_BODY: the (possibly rewritten) instruction */
    int64_t imm;     /* FRAME_OP_SUB_RSP: frame size in bytes */
} FrameInsn;

/* ---------------------------------------------------------------------------
 * Per-function frame layout
 * ------------------------------------------------------------------------- */

typedef struct FrameSlotLayout {
    int64_t slot_index;   /* IR slot table index */
    int64_t offset;       /* RBP-relative byte offset (<= 0) */
    int64_t size;         /* slot byte size (type size) */
    int64_t align;        /* slot alignment (type alignment) */
} FrameSlotLayout;

typedef struct FrameLayout {
    int64_t function_id;  /* IR function node id */
    bool is_entry;        /* main (entry module, name "main") */
    bool noreturn;        /* IR noreturn flag (rt.proc.exit / rt.trap.report) */
    bool has_body;        /* fn body != NULL (external decls have none) */
    FrameSlotLayout *slots;
    size_t nslots;
    int64_t frame_size;   /* bytes to reserve below RBP (16-byte aligned) */
} FrameLayout;

/* ---------------------------------------------------------------------------
 * FrameOutput
 * ------------------------------------------------------------------------- */

typedef enum FrameStatus {
    FRAME_OK = 0,
    FRAME_OOM              /* allocation failure; nothing owned */
} FrameStatus;

typedef struct FrameOutput {
    FrameInsn *insns;      /* framed instruction stream, emission order */
    size_t count;
    size_t cap;
    FrameLayout *layouts;  /* per function, canonical function order */
    size_t nlayouts;
    size_t layouts_cap;
    int64_t entry_function_id;  /* main's IR node id, or -1 when not found */
    bool oom;              /* sticky allocation-failure flag */
} FrameOutput;

/* Compute frame layouts and the framed instruction stream for a whole
 * selection. `build` must be the IR build the selection was made from
 * (the function-comment node ids are looked up in it); `sel` must be a
 * valid isel_core selection. Returns FRAME_OK with *out owned by the
 * caller, or FRAME_OOM with *out NULL. The output borrows `build` and
 * `sel` (both must outlive it). */
FrameStatus frame_build(const IrBuild *build, const IselOutput *sel,
                        FrameOutput **out);

void frame_output_free(FrameOutput *out);

/* Accessors for tests and later packages. */
size_t frame_output_count(const FrameOutput *out);
const FrameInsn *frame_output_insn(const FrameOutput *out, size_t i);
size_t frame_layout_count(const FrameOutput *out);
const FrameLayout *frame_layout_at(const FrameOutput *out, size_t i);
const FrameLayout *frame_layout_for_function(const FrameOutput *out,
                                             int64_t function_id);
const FrameSlotLayout *frame_layout_slot(const FrameLayout *layout,
                                         int64_t slot_index);

/* ---------------------------------------------------------------------------
 * Deterministic framed assembly dump
 * ------------------------------------------------------------------------- */

/* Render the framed stream as stable text appended to `out` (DiagBuf,
 * e.g. diag_buf_init): the frame-plan header, then per function the
 * prologue, the body (slot operands rendered as [rbp-<off>]), and the
 * epilogue restore before each ret. Byte-deterministic: identical
 * inputs produce identical dump bytes. Returns false on allocation
 * failure (out->oom set). */
bool frame_asm_dump(const FrameOutput *fr, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_SRC_BACKEND_FRAME_H */
