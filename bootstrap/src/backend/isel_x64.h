/* bootstrap/src/backend/isel_x64.h
 *
 * AI-Co Stage-0 x86-64+SSE2 instruction coverage and backend constraint
 * enforcement (WP-M0-17a2).
 *
 * Implements the two WP-M0-17a2 obligations per spec sec. 14.1(7) and
 * 14.3 and ADR-001:
 *   1. instruction coverage - the closed set of x86-64 + SSE2 machine
 *      instructions the backend is licensed to generate, classified by
 *      target feature. The covered set contains no AVX2 or higher and no
 *      host-specific instruction (acceptance criterion 1: "generated
 *      instruction set uses only x86-64 + SSE2; no AVX2/host-specific
 *      instructions required");
 *   2. backend constraint enforcement - any request for an instruction
 *      outside the covered baseline is reported as AIC-B0601 (backend
 *      constraint violation; DIAGNOSTIC-CONTRACT sec. 11.6), with the
 *      failing IR node's span as the derived span (acceptance criterion
 *      2: "backend constraint violations (AIC-B0601) enforced").
 *
 * Scope (manifest WP-M0-17a2): instruction coverage and constraint
 * checks only. Excluded (owned by later packages): selection core/order
 * (17a1); frame layout/regalloc (17b); trap branches (17c); COFF
 * emission (WP-M0-18). This package consumes the 17a1 selection output
 * (IselOutput) and does not modify isel_core.*.
 *
 * Coverage model. The coverage registry (x64_insn_info / x64_insn_at /
 * x64_insn_count) is a single deterministic, closed table. Every entry
 * is a rendered mnemonic (the exact token isel_asm_dump prints, without
 * the width suffix b/w/l/q which is a rendering detail of the same
 * instruction) with a feature class:
 *   - ISEL_X64_BASE   - x86-64 baseline instruction (always available on
 *                       the target; spec sec. 14.3);
 *   - ISEL_X64_SSE2   - SSE2 instruction (permitted; the baseline
 *                       ceiling is "x86-64 + SSE2");
 *   - ISEL_X64_PSEUDO - selection pseudo-op of isel_core (17a1). Not a
 *                       machine instruction; its x86-64 lowering is
 *                       owned by later packages (17b/17c/18). Pseudo-ops
 *                       are covered entries and never violate the
 *                       baseline;
 *   - ISEL_X64_AVX2   - known out-of-baseline instruction requiring
 *                       AVX2 or higher. Registered so that requesting it
 *                       is a deterministic AIC-B0601 violation, not an
 *                       unknown lookup;
 *   - ISEL_X64_HOST   - known host-specific instruction (requires a CPU
 *                       feature flag beyond x86-64+SSE2, e.g. AES-NI,
 *                       SHA, RDRAND). Registered for the same reason;
 *   - ISEL_X64_UNKNOWN- no registry entry (outside the closed set).
 *
 * The registry also carries the rendered conditional mnemonics
 * (sete..setae, je..jae) exactly as the dump prints them, so a dump
 * token lookup needs no special casing beyond the width-suffix strip.
 * The SSE2 subset is the closed set the backend is licensed to emit;
 * extending it (or the covered set in general) is an instruction
 * coverage change and returns to the Planner per the manifest escalation
 * rules.
 *
 * Constraint enforcement surfaces:
 *   - x64_check_mnemonic / x64_mnemonic_within_baseline - the query a
 *     future encoder (WP-M0-18) or later package calls before emitting
 *     an instruction; out-of-baseline results are reported through
 *     x64_constraint_record;
 *   - x64_constraint_record - the AIC-B0601 record factory (phase
 *     "backend", severity "error", recovery "authoritative", primary
 *     span = the derived span supplied by the caller);
 *   - x64_verify_constraints - pipeline safety net over a complete
 *     selection: every selected opcode must be covered and within the
 *     baseline; violations produce AIC-B0601 records with the
 *     originating IR node's span, returned sorted with the
 *     DIAGNOSTIC-CONTRACT sec. 9 comparator (the same pattern as
 *     ir_core_verify).
 *
 * Determinism. The registry is a static, closed table with no duplicate
 * mnemonics; lookups are deterministic linear scans (the table is small
 * and static). Verification emits records in instruction order and sorts
 * them before returning; identical selections always yield identical
 * record sets (spec sec. 14.2).
 *
 * Ownership:
 *   - x64_verify_constraints returns an owned record array on
 *     ISEL_X64_VIOLATION (freed with x64_records_free); on
 *     ISEL_X64_OK *out_records is NULL and *out_record_count is 0; on
 *     ISEL_X64_OOM nothing is owned.
 *   - x64_constraint_record returns an owned record (diag_record_free).
 *   - The registry is static; all strings are borrowed literals.
 *
 * Build (from the repository root; MSVC example) for the unit test:
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17a2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/isel_x64_test.c \
 *     bootstrap/src/backend/isel_x64.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17a2)
 */
#ifndef AICO_BOOTSTRAP_SRC_BACKEND_ISEL_X64_H
#define AICO_BOOTSTRAP_SRC_BACKEND_ISEL_X64_H

#include "isel_core.h"

#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Target feature classes (spec sec. 14.3: baseline = x86-64 + SSE2; no
 * AVX2/host-specific instructions required).
 * ------------------------------------------------------------------------- */

typedef enum IselX64Feature {
    ISEL_X64_BASE = 0,  /* x86-64 baseline instruction */
    ISEL_X64_SSE2,      /* SSE2 instruction (permitted) */
    ISEL_X64_PSEUDO,    /* selection pseudo-op (not a machine instruction) */
    ISEL_X64_AVX2,      /* AVX2 or higher (outside the baseline) */
    ISEL_X64_HOST,      /* host-specific feature (outside the baseline) */
    ISEL_X64_UNKNOWN    /* not in the coverage registry */
} IselX64Feature;

/* Stable feature name for messages and tests ("x86-64 baseline",
 * "SSE2", "pseudo-op", "AVX2 or higher", "host-specific feature",
 * "unknown"). */
const char *x64_feature_text(IselX64Feature f);

/* ---------------------------------------------------------------------------
 * Coverage registry
 * ------------------------------------------------------------------------- */

typedef struct IselX64InsnInfo {
    const char *mnemonic;   /* rendered mnemonic (isel_asm_dump form,
                             * width suffix omitted) */
    IselX64Feature feature;
    bool is_real;           /* false for pseudo-ops */
} IselX64InsnInfo;

/* Registry lookup by mnemonic; returns NULL when the mnemonic has no
 * entry (ISEL_X64_UNKNOWN). */
const IselX64InsnInfo *x64_insn_info(const char *mnemonic);

/* Registry iteration (deterministic sorted table order). */
const IselX64InsnInfo *x64_insn_at(size_t index);
size_t x64_insn_count(void);

/* Feature class of a selected opcode (isel_core closed opcode set).
 * Out-of-range opcode values return ISEL_X64_UNKNOWN. */
IselX64Feature x64_opcode_feature(IselOpcode op);

/* True when the opcode is a pseudo-op whose machine expansion is owned
 * by a later package. */
bool x64_opcode_is_pseudo(IselOpcode op);

/* True when the opcode is covered and within the x86-64+SSE2 baseline
 * (BASE or SSE2; pseudo-ops count as covered non-machine entries). */
bool x64_opcode_within_baseline(IselOpcode op);

/* ---------------------------------------------------------------------------
 * Constraint enforcement
 * ------------------------------------------------------------------------- */

/* Feature class of a mnemonic from the coverage registry; ISEL_X64_UNKNOWN
 * when the mnemonic has no entry. This is the query a future encoder or
 * later package performs before emitting an instruction. */
IselX64Feature x64_check_mnemonic(const char *mnemonic);

/* True when the mnemonic is within the baseline (BASE/SSE2/PSEUDO). */
bool x64_mnemonic_within_baseline(const char *mnemonic);

/* AIC-B0601 backend-constraint-violation record factory (DIAGNOSTIC-
 * CONTRACT sec. 11.6): phase "backend", severity "error", recovery
 * "authoritative". The message names the offending instruction and its
 * feature. `span` is the derived span (the failing IR node's span, or
 * NULL for a null span) and is copied. Returns NULL on allocation
 * failure or when mnemonic is NULL. */
DiagRecord *x64_constraint_record(const char *mnemonic,
                                  IselX64Feature feature,
                                  const DiagSpan *span);

typedef enum IselX64Status {
    ISEL_X64_OK = 0,      /* selection is fully within the baseline */
    ISEL_X64_VIOLATION,   /* one or more AIC-B0601 records produced */
    ISEL_X64_OOM          /* allocation failure; nothing owned */
} IselX64Status;

/* Verify a complete selection against the x86-64+SSE2 baseline: every
 * selected opcode must be covered by the registry and within the
 * baseline. Each violation produces one AIC-B0601 record with the
 * originating IR node's span (build->nodes[ir_node_id]->span) as the
 * derived span, or a null span when the id is out of range; the record
 * set is returned sorted with the DIAGNOSTIC-CONTRACT sec. 9 comparator
 * (same shape as ir_core_verify). The build must outlive the records
 * (spans are copied, so only the lookup is borrowed). */
IselX64Status x64_verify_constraints(const IselOutput *sel,
                                     const IrBuild *build,
                                     DiagRecord ***out_records,
                                     size_t *out_record_count);

/* Free the record array returned by x64_verify_constraints. */
void x64_records_free(DiagRecord **records, size_t count);

#endif /* AICO_BOOTSTRAP_SRC_BACKEND_ISEL_X64_H */
