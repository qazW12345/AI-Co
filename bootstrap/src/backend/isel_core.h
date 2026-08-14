/* bootstrap/src/backend/isel_core.h
 *
 * AI-Co Stage-0 x86-64 instruction selection core (WP-M0-17a1).
 *
 * Implements the IR -> x86-64 instruction selection core of the accepted
 * canonical IR contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1,
 * WP-M0-16a) per spec sec. 14.1(7) and 14.3: deterministic code generation
 * with deterministic output ordering and register-usage determinism.
 *
 * Scope (manifest WP-M0-17a1): instruction selection; register-usage
 * determinism; deterministic output ordering. Excluded (owned by later
 * packages): x86-64+SSE2 coverage/constraint checks (17a2, AIC-B0601);
 * frame layout and register allocation (17b1/17b2); trap branch emission
 * (17c1); checked-operation emission with span/cause (17c2); COFF emission
 * (WP-M0-18).
 *
 * What this package delivers:
 *   - a deterministic traversal of the IR build (canonical order:
 *     modules in build order, declarations in source order, function
 *     bodies, statements in order, expression children in the contract
 *     sec. 5.3 evaluation order) that SELECTS one or more x86-64
 *     instructions per IR node into an owned, ordered instruction list
 *     (IselOutput);
 *   - deterministic virtual-register usage: every value-producing IR
 *     node is assigned a virtual register on first visit; vreg numbers
 *     are gapless and follow the canonical traversal, so identical IR
 *     always yields identical vreg numbers (register-usage determinism,
 *     spec sec. 14.1(7) "deterministic code generation");
 *   - deterministic control-flow labels: labels are allocated in
 *     creation order during the same traversal;
 *   - a byte-deterministic assembly dump (isel_asm_dump) that renders
 *     the selected instruction list as stable text. Identical IR
 *     produces byte-identical dump bytes; this is the observable
 *     artifact tested by the assembly dump tests (acceptance criterion 1).
 *
 * Normative rules (documented implementation detail within the contract):
 *   1. Canonical traversal order. Modules in build->modules order;
 *      declarations in module source order; function params/slots in
 *      slot order; body statements in order; expression children in
 *      sec. 5.3 evaluation order (left, then right; condition, then
 *      chosen branch). No reordering is representable.
 *   2. Register-usage determinism. A fresh vreg is allocated for every
 *      value-producing node at its first visit, in traversal order.
 *      vreg numbers are gapless from 0 and depend only on the IR (node
 *      ids are the contract's deterministic construction-order ids).
 *      No pointer address, hash order, environment value, or host
 *      identity influences vreg numbering or output order.
 *   3. Immediate folding. Integer/boolean/enum constants are folded into
 *      the instruction's immediate operand when the opcode supports an
 *      immediate form (mov, add, sub, imul, and, or, xor, cmp, test,
 *      shl, shr, sar, lea displacement). The fold decision is a pure
 *      function of the constant value and opcode, so it is
 *      deterministic.
 *   4. Trap-obligation preservation. Every selected instruction that
 *      performs a checked operation carries its originating IR node's
 *      declared trap code (e.g. AIC-R0802 on add/sub/mul, AIC-R0803 on
 *      div/mod, AIC-R0807 on index, AIC-R0809 on deref, AIC-R0805 on
 *      bool load). The trap code is preserved on the instruction and in
 *      the dump; the actual trap BRANCH is owned by 17c1 and is not
 *      emitted here.
 *   5. Dump determinism. The assembly dump is a stable textual rendering
 *      of the instruction list in emission order: fixed mnemonic set,
 *      decimal integers (signed for signed types, unsigned for unsigned
 *      types), symbolic slot/global/string/function references by
 *      deterministic id, no timestamps, no pointer addresses, no host
 *      paths. Identical instruction lists produce identical bytes.
 *
 * Pseudo-ops. A small closed set of pseudo-ops records operations whose
 * full x86-64 lowering belongs to later packages (composite object
 * copies ISEL_REP_MOVSB / zero-fills ISEL_REP_STOSB -> 17a2/17b; slice
 * equality ISEL_SLICEEQ -> 17a2; UTF-8 validation ISEL_UTF8 -> 17a2/17c;
 * pointer difference ISEL_PTRDIFF -> 17a2/17b; trap ISEL_TRAP -> 17c).
 * They are selected deterministically, carry their operands and trap
 * annotations, and are rendered as pseudo lines in the dump. They are
 * not instruction-selection gaps: the selection decision (opcode,
 * operand order, vregs, obligations) is complete; the encoding details
 * are owned by the excluded packages. Call emission (ISEL_CALL) selects
 * arguments in order and the call; argument/return register placement
 * (Microsoft x64 ABI, spec sec. 15.7) is owned by 17b2 and is annotated
 * only.
 *
 * The selection pass consumes a VERIFIED IR build (the driver only runs
 * the backend on accepted programs; contract sec. 1.3). It does not
 * re-verify invariants. An allocation failure is reported as ISEL_OOM;
 * no other failure mode exists (the closed IR node-kind set is fully
 * covered by the selection table).
 *
 * Ownership:
 *   - isel_select returns an owned IselOutput (isel_output_free).
 *   - The IselOutput borrows the IrBuild (node ids and names); the build
 *     must outlive the output.
 *   - Trap codes are borrowed literals from the IR nodes (registry
 *     strings), never owned.
 *
 * Build (from the repository root; MSVC example) for the unit test:
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17a1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/isel_core_test.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17a1)
 */
#ifndef AICO_BOOTSTRAP_SRC_BACKEND_ISEL_CORE_H
#define AICO_BOOTSTRAP_SRC_BACKEND_ISEL_CORE_H

#include "../ir/ir_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Selected x86-64 instruction opcodes (core set; closed for this package).
 * The mnemonic set is documented in isel_asm_dump; width suffixes are
 * derived from operand types (b/w/l/q for 1/2/4/8 bytes).
 * ------------------------------------------------------------------------- */

typedef enum IselOpcode {
    /* comments / markers (no machine code) */
    ISEL_COMMENT = 0,
    /* data movement */
    ISEL_MOV,        /* mov  dst, src   (dst-first dump form; width by
                      * type; memory form via ISEL_OP_MEM) */
    ISEL_LEA,        /* lea  dst, addr  (address computation) */
    ISEL_MOVZX,      /* movzx dst, src  (zero-extending widening cast) */
    ISEL_MOVSX,      /* movsx dst, src  (sign-extending widening cast) */
    /* integer arithmetic */
    ISEL_ADD, ISEL_SUB, ISEL_IMUL, ISEL_IDIV, ISEL_NEG,
    /* logic and shifts */
    ISEL_AND, ISEL_OR, ISEL_XOR, ISEL_NOT,
    ISEL_SHL, ISEL_SHR, ISEL_SAR,
    /* comparison */
    ISEL_CMP, ISEL_TEST, ISEL_SETCC,
    /* control flow */
    ISEL_JMP, ISEL_JCC, ISEL_LABEL, ISEL_CALL, ISEL_RET,
    /* composite object operations (pseudo; encoding owned by 17a2/17b) */
    ISEL_REP_MOVSB,  /* full-object copy: dst=[dst_addr], src1=[src_addr],
                      * src2=imm size (bytes) */
    ISEL_REP_STOSB,  /* zero-fill: dst=[dst_addr], src1=imm size (bytes) */
    ISEL_SLICEEQ,    /* slice element-wise equality (element size in src2) */
    ISEL_SLICE,      /* slice pair construction: src1=data ptr, src2=len;
                      * pair materialization owned by 17b */
    ISEL_STRCMP,     /* str byte-sequence comparison (element size 1;
                      * lexicographic sequence owned by 17a2) */
    ISEL_UTF8,       /* u8[] -> str UTF-8 validation (trap AIC-R0806) */
    ISEL_PTRDIFF,    /* byte difference / element scale check (AIC-R0810) */
    ISEL_TRAP        /* unconditional trap marker (17c lowers to the
                      * trap-report path) */
} IselOpcode;

const char *isel_opcode_text(IselOpcode op);

/* Condition codes used by ISEL_JCC and ISEL_SETCC. */
typedef enum IselCond {
    ISEL_COND_E = 0,   /* equal           je  / sete */
    ISEL_COND_NE,      /* not equal       jne / setne */
    ISEL_COND_L,       /* signed less     jl  / setl */
    ISEL_COND_LE,      /* signed <=       jle / setle */
    ISEL_COND_G,       /* signed greater  jg  / setg */
    ISEL_COND_GE,      /* signed >=       jge / setge */
    ISEL_COND_B,       /* unsigned below  jb  / setb */
    ISEL_COND_BE,      /* unsigned <=     jbe / setbe */
    ISEL_COND_A,       /* unsigned above  ja  / seta */
    ISEL_COND_AE       /* unsigned >=     jae / setae */
} IselCond;

const char *isel_cond_text(IselCond cond);

/* ---------------------------------------------------------------------------
 * Operands
 * ------------------------------------------------------------------------- */

typedef enum IselOperandKind {
    ISEL_OP_NONE = 0,
    ISEL_OP_VREG,      /* virtual register r<N> */
    ISEL_OP_IMM,       /* integer immediate (value, width from type) */
    ISEL_OP_SLOT,      /* symbolic function slot (index) */
    ISEL_OP_GLOBAL,    /* symbolic global (IR_GLOBAL_VAR node id) */
    ISEL_OP_STR,       /* string constant reference (const id) */
    ISEL_OP_FUNC,      /* function reference (IR_FUNCTION node id) */
    ISEL_OP_LABEL,     /* control-flow label id */
    ISEL_OP_MEM        /* memory reference: base vreg + displacement */
} IselOperandKind;

typedef struct IselOperand {
    IselOperandKind kind;
    int64_t vreg;        /* ISEL_OP_VREG; ISEL_OP_MEM base */
    int64_t imm;         /* ISEL_OP_IMM; ISEL_OP_MEM displacement */
    int64_t id;          /* SLOT index / GLOBAL node id / STR const id /
                          * FUNC node id / LABEL id */
    int width;           /* operand width in bytes (1/2/4/8), 0 when none */
    bool is_unsigned;    /* ISEL_OP_IMM: render as unsigned decimal */
} IselOperand;

/* An empty operand (kind ISEL_OP_NONE). */
IselOperand isel_operand_none(void);

/* ---------------------------------------------------------------------------
 * Instructions and output
 * ------------------------------------------------------------------------- */

typedef struct IselInsn {
    IselOpcode op;
    IselOperand dst;     /* result / destination */
    IselOperand src1;    /* first source */
    IselOperand src2;    /* second source (immediate for rep ops) */
    IselCond cond;       /* ISEL_JCC / ISEL_SETCC condition; else 0 */
    int scale;           /* ISEL_LEA index scale / ISEL_SLICEEQ element
                          * size (bytes); else 0 */
    bool mod;            /* ISEL_IDIV: true when lowering IR_MOD */
    const char *trap;    /* borrowed registry trap code, or NULL */
    const char *note;    /* borrowed free-form note (ISEL_COMMENT) */
    int64_t ir_node_id;  /* originating IR node id (deterministic trace) */
} IselInsn;

typedef enum IselStatus {
    ISEL_OK = 0,
    ISEL_OOM             /* allocation failure; nothing owned */
} IselStatus;

typedef struct IselOutput {
    IselInsn *insns;     /* selected instructions, emission order */
    size_t count;
    size_t cap;
    int64_t next_vreg;   /* next virtual register number (gapless) */
    int64_t next_label;  /* next label id (gapless) */
    bool oom;            /* sticky allocation-failure flag */
} IselOutput;

/* Select instructions for the whole build. The build must be a verified
 * IR build (contract sec. 1.3). Returns ISEL_OK with *out owned by the
 * caller, or ISEL_OOM with *out NULL. The output borrows `build`. */
IselStatus isel_select(const IrBuild *build, IselOutput **out);

void isel_output_free(IselOutput *out);

/* Accessors for tests. */
size_t isel_output_count(const IselOutput *out);
const IselInsn *isel_output_insn(const IselOutput *out, size_t i);

/* ---------------------------------------------------------------------------
 * Deterministic assembly dump
 * ------------------------------------------------------------------------- */

/* Render the selected instruction list as a stable textual assembly dump
 * appended to `out` (DiagBuf, e.g. diag_buf_init). Byte-deterministic:
 * identical IR produces identical dump bytes (acceptance criterion 1).
 * Returns false on allocation failure (out->oom set). */
bool isel_asm_dump(const IselOutput *sel, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_SRC_BACKEND_ISEL_CORE_H */
