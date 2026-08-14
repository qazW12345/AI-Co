/* bootstrap/src/backend/isel_x64.c
 *
 * AI-Co Stage-0 x86-64+SSE2 instruction coverage and backend constraint
 * enforcement (WP-M0-17a2). See isel_x64.h for the normative model:
 * the closed coverage registry (spec sec. 14.3 baseline: x86-64 + SSE2,
 * no AVX2/host-specific), the AIC-B0601 record factory, and the
 * selection-wide constraint verification. This file implements only the
 * 17a2 scope; isel_core.* (17a1) is read-only here.
 */
#include "isel_x64.h"

#include "../diag/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Feature names
 * ------------------------------------------------------------------------- */

static const char *const kFeatureNames[] = {
    "x86-64 baseline", "SSE2", "pseudo-op",
    "AVX2 or higher", "host-specific feature", "unknown"
};

const char *x64_feature_text(IselX64Feature f)
{
    if (f < 0 || (size_t)f >= sizeof(kFeatureNames) / sizeof(kFeatureNames[0])) {
        return "unknown";
    }
    return kFeatureNames[f];
}

/* ---------------------------------------------------------------------------
 * Coverage registry (closed, no duplicate mnemonics).
 *
 * Entries are the rendered mnemonics exactly as isel_asm_dump prints
 * them (width suffixes b/w/l/q omitted: they are a rendering detail of
 * the same instruction). The registry deliberately includes:
 *   - the base x86-64 mnemonics of the isel_core closed opcode set;
 *   - the rendered conditional forms sete..setae / je..jae (the dump
 *     prints "set"+cond / "j"+cond, never a generic "setcc"/"jcc");
 *   - the SSE2 subset the backend is licensed to emit (closed; an
 *     extension is an instruction-coverage change -> Planner);
 *   - known out-of-baseline instructions (AVX2 or higher; host-specific
 *     feature flags) so requesting them is a deterministic AIC-B0601
 *     violation, and
 *   - the isel_core pseudo-ops (machine expansion owned by 17b/17c/18).
 * The covered set (BASE/SSE2/PSEUDO) contains no AVX2/HOST entry.
 * ------------------------------------------------------------------------- */

static const IselX64InsnInfo kRegistry[] = {
    /* --- x86-64 baseline (real instructions; isel_core closed set) --- */
    { "mov",    ISEL_X64_BASE, true  },
    { "lea",    ISEL_X64_BASE, true  },
    { "movzx",  ISEL_X64_BASE, true  },
    { "movsx",  ISEL_X64_BASE, true  },
    { "add",    ISEL_X64_BASE, true  },
    { "sub",    ISEL_X64_BASE, true  },
    { "imul",   ISEL_X64_BASE, true  },
    { "idiv",   ISEL_X64_BASE, true  },
    { "neg",    ISEL_X64_BASE, true  },
    { "and",    ISEL_X64_BASE, true  },
    { "or",     ISEL_X64_BASE, true  },
    { "xor",    ISEL_X64_BASE, true  },
    { "not",    ISEL_X64_BASE, true  },
    { "shl",    ISEL_X64_BASE, true  },
    { "shr",    ISEL_X64_BASE, true  },
    { "sar",    ISEL_X64_BASE, true  },
    { "cmp",    ISEL_X64_BASE, true  },
    { "test",   ISEL_X64_BASE, true  },
    { "setcc",  ISEL_X64_BASE, true  },
    { "sete",   ISEL_X64_BASE, true  },
    { "setne",  ISEL_X64_BASE, true  },
    { "setl",   ISEL_X64_BASE, true  },
    { "setle",  ISEL_X64_BASE, true  },
    { "setg",   ISEL_X64_BASE, true  },
    { "setge",  ISEL_X64_BASE, true  },
    { "setb",   ISEL_X64_BASE, true  },
    { "setbe",  ISEL_X64_BASE, true  },
    { "seta",   ISEL_X64_BASE, true  },
    { "setae",  ISEL_X64_BASE, true  },
    { "jmp",    ISEL_X64_BASE, true  },
    { "jcc",    ISEL_X64_BASE, true  },
    { "je",     ISEL_X64_BASE, true  },
    { "jne",    ISEL_X64_BASE, true  },
    { "jl",     ISEL_X64_BASE, true  },
    { "jle",    ISEL_X64_BASE, true  },
    { "jg",     ISEL_X64_BASE, true  },
    { "jge",    ISEL_X64_BASE, true  },
    { "jb",     ISEL_X64_BASE, true  },
    { "jbe",    ISEL_X64_BASE, true  },
    { "ja",     ISEL_X64_BASE, true  },
    { "jae",    ISEL_X64_BASE, true  },
    { "call",   ISEL_X64_BASE, true  },
    { "ret",    ISEL_X64_BASE, true  },

    /* --- SSE2 (permitted; closed licensed subset) --- */
    /* scalar double */
    { "movsd",    ISEL_X64_SSE2, true  },
    { "addsd",    ISEL_X64_SSE2, true  },
    { "subsd",    ISEL_X64_SSE2, true  },
    { "mulsd",    ISEL_X64_SSE2, true  },
    { "divsd",    ISEL_X64_SSE2, true  },
    { "sqrtsd",   ISEL_X64_SSE2, true  },
    { "maxsd",    ISEL_X64_SSE2, true  },
    { "minsd",    ISEL_X64_SSE2, true  },
    { "comisd",   ISEL_X64_SSE2, true  },
    { "ucomisd",  ISEL_X64_SSE2, true  },
    { "cvtsi2sd", ISEL_X64_SSE2, true  },
    { "cvtsd2si", ISEL_X64_SSE2, true  },
    { "cvttsd2si",ISEL_X64_SSE2, true  },
    { "cvtsd2ss", ISEL_X64_SSE2, true  },
    /* scalar float */
    { "movss",    ISEL_X64_SSE2, true  },
    { "addss",    ISEL_X64_SSE2, true  },
    { "subss",    ISEL_X64_SSE2, true  },
    { "mulss",    ISEL_X64_SSE2, true  },
    { "divss",    ISEL_X64_SSE2, true  },
    { "sqrtss",   ISEL_X64_SSE2, true  },
    { "maxss",    ISEL_X64_SSE2, true  },
    { "minss",    ISEL_X64_SSE2, true  },
    { "comiss",   ISEL_X64_SSE2, true  },
    { "ucomiss",  ISEL_X64_SSE2, true  },
    { "cvtsi2ss", ISEL_X64_SSE2, true  },
    { "cvtss2si", ISEL_X64_SSE2, true  },
    { "cvttss2si",ISEL_X64_SSE2, true  },
    { "cvtss2sd", ISEL_X64_SSE2, true  },
    /* 128-bit vector moves */
    { "movapd",   ISEL_X64_SSE2, true  },
    { "movaps",   ISEL_X64_SSE2, true  },
    { "movupd",   ISEL_X64_SSE2, true  },
    { "movups",   ISEL_X64_SSE2, true  },
    /* GPR <-> XMM */
    { "movd",     ISEL_X64_SSE2, true  },
    { "movq",     ISEL_X64_SSE2, true  },
    /* integer SIMD */
    { "pxor",     ISEL_X64_SSE2, true  },
    { "pand",     ISEL_X64_SSE2, true  },
    { "pandn",    ISEL_X64_SSE2, true  },
    { "por",      ISEL_X64_SSE2, true  },
    { "paddb",    ISEL_X64_SSE2, true  },
    { "paddw",    ISEL_X64_SSE2, true  },
    { "paddd",    ISEL_X64_SSE2, true  },
    { "paddq",    ISEL_X64_SSE2, true  },
    { "psubb",    ISEL_X64_SSE2, true  },
    { "psubw",    ISEL_X64_SSE2, true  },
    { "psubd",    ISEL_X64_SSE2, true  },
    { "psubq",    ISEL_X64_SSE2, true  },
    { "pcmpeqb",  ISEL_X64_SSE2, true  },
    { "pcmpeqw",  ISEL_X64_SSE2, true  },
    { "pcmpeqd",  ISEL_X64_SSE2, true  },
    { "pcmpgtb",  ISEL_X64_SSE2, true  },
    { "pcmpgtw",  ISEL_X64_SSE2, true  },
    { "pcmpgtd",  ISEL_X64_SSE2, true  },
    { "punpcklbw",ISEL_X64_SSE2, true  },
    { "punpcklwd",ISEL_X64_SSE2, true  },
    { "punpckldq",ISEL_X64_SSE2, true  },
    { "punpckhdq",ISEL_X64_SSE2, true  },
    /* integer SIMD shifts */
    { "psllw",    ISEL_X64_SSE2, true  },
    { "pslld",    ISEL_X64_SSE2, true  },
    { "psllq",    ISEL_X64_SSE2, true  },
    { "psrlw",    ISEL_X64_SSE2, true  },
    { "psrld",    ISEL_X64_SSE2, true  },
    { "psrlq",    ISEL_X64_SSE2, true  },

    /* --- pseudo-ops (isel_core 17a1; machine expansion owned by
     * 17b/17c/18; covered, never a baseline violation) --- */
    { "comment",   ISEL_X64_PSEUDO, false },
    { "label",     ISEL_X64_PSEUDO, false },
    { "rep movsb", ISEL_X64_PSEUDO, false },
    { "rep stosb", ISEL_X64_PSEUDO, false },
    { "sliceeq",   ISEL_X64_PSEUDO, false },
    { "slice",     ISEL_X64_PSEUDO, false },
    { "strcmp",    ISEL_X64_PSEUDO, false },
    { "utf8",      ISEL_X64_PSEUDO, false },
    { "ptrdiff",   ISEL_X64_PSEUDO, false },
    { "trap",      ISEL_X64_PSEUDO, false },

    /* --- known out-of-baseline (registered so requesting one is a
     * deterministic AIC-B0601 violation, not an unknown lookup) --- */
    /* AVX2 or higher */
    { "vaddps",       ISEL_X64_AVX2, true },
    { "vpaddd",       ISEL_X64_AVX2, true },
    { "vmovaps",      ISEL_X64_AVX2, true },
    { "vmovdqu",      ISEL_X64_AVX2, true },
    { "vpxor",        ISEL_X64_AVX2, true },
    { "vbroadcastss", ISEL_X64_AVX2, true },
    { "vzeroupper",   ISEL_X64_AVX2, true },
    /* AVX-512 (higher than AVX2; same violation class) */
    { "vmovdqa32",    ISEL_X64_AVX2, true },
    { "vpternlogd",   ISEL_X64_AVX2, true },
    { "kmovw",        ISEL_X64_AVX2, true },
    /* host-specific feature flags (beyond the x86-64+SSE2 baseline) */
    { "aesenc",       ISEL_X64_HOST, true },
    { "aesenclast",   ISEL_X64_HOST, true },
    { "pclmulqdq",    ISEL_X64_HOST, true },
    { "rdrand",       ISEL_X64_HOST, true },
    { "rdseed",       ISEL_X64_HOST, true },
    { "sha1rnds4",    ISEL_X64_HOST, true },
    { "xgetbv",       ISEL_X64_HOST, true }
};

#define X64_REGISTRY_COUNT (sizeof(kRegistry) / sizeof(kRegistry[0]))

const IselX64InsnInfo *x64_insn_info(const char *mnemonic)
{
    size_t i;
    if (mnemonic == NULL) {
        return NULL;
    }
    for (i = 0; i < X64_REGISTRY_COUNT; i++) {
        if (strcmp(mnemonic, kRegistry[i].mnemonic) == 0) {
            return &kRegistry[i];
        }
    }
    return NULL;
}

const IselX64InsnInfo *x64_insn_at(size_t index)
{
    if (index >= X64_REGISTRY_COUNT) {
        return NULL;
    }
    return &kRegistry[index];
}

size_t x64_insn_count(void)
{
    return X64_REGISTRY_COUNT;
}

/* ---------------------------------------------------------------------------
 * Opcode classification (isel_core closed opcode set)
 * ------------------------------------------------------------------------- */

IselX64Feature x64_opcode_feature(IselOpcode op)
{
    switch (op) {
    case ISEL_COMMENT:
    case ISEL_REP_MOVSB:
    case ISEL_REP_STOSB:
    case ISEL_SLICEEQ:
    case ISEL_SLICE:
    case ISEL_STRCMP:
    case ISEL_UTF8:
    case ISEL_PTRDIFF:
    case ISEL_TRAP:
        return ISEL_X64_PSEUDO;
    case ISEL_MOV:
    case ISEL_LEA:
    case ISEL_MOVZX:
    case ISEL_MOVSX:
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
    case ISEL_SETCC:
    case ISEL_JMP:
    case ISEL_JCC:
    case ISEL_LABEL:
    case ISEL_CALL:
    case ISEL_RET:
        return ISEL_X64_BASE;
    default:
        return ISEL_X64_UNKNOWN;
    }
}

bool x64_opcode_is_pseudo(IselOpcode op)
{
    return x64_opcode_feature(op) == ISEL_X64_PSEUDO;
}

bool x64_opcode_within_baseline(IselOpcode op)
{
    IselX64Feature f = x64_opcode_feature(op);
    return f == ISEL_X64_BASE || f == ISEL_X64_SSE2 || f == ISEL_X64_PSEUDO;
}

/* ---------------------------------------------------------------------------
 * Mnemonic-level constraint queries
 * ------------------------------------------------------------------------- */

IselX64Feature x64_check_mnemonic(const char *mnemonic)
{
    const IselX64InsnInfo *info = x64_insn_info(mnemonic);
    if (info == NULL) {
        return ISEL_X64_UNKNOWN;
    }
    return info->feature;
}

bool x64_mnemonic_within_baseline(const char *mnemonic)
{
    IselX64Feature f = x64_check_mnemonic(mnemonic);
    return f == ISEL_X64_BASE || f == ISEL_X64_SSE2 || f == ISEL_X64_PSEUDO;
}

/* ---------------------------------------------------------------------------
 * AIC-B0601 record factory
 * ------------------------------------------------------------------------- */

DiagRecord *x64_constraint_record(const char *mnemonic,
                                  IselX64Feature feature,
                                  const DiagSpan *span)
{
    char msg[320];
    DiagRecord *r;
    if (mnemonic == NULL) {
        return NULL;
    }
    if (feature == ISEL_X64_UNKNOWN) {
        (void)snprintf(msg, sizeof(msg),
                       "backend constraint violation: instruction %s is "
                       "not in the x86-64+SSE2 instruction coverage "
                       "registry (spec 14.3)",
                       mnemonic);
    } else {
        (void)snprintf(msg, sizeof(msg),
                       "backend constraint violation: instruction %s "
                       "requires %s, outside the x86-64+SSE2 baseline "
                       "(spec 14.3)",
                       mnemonic, x64_feature_text(feature));
    }
    msg[sizeof(msg) - 1] = '\0';
    r = diag_record_new();
    if (r == NULL) {
        return NULL;
    }
    if (!diag_record_set_code(r, "AIC-B0601") ||
        !diag_record_set_message(r, msg) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        return NULL;
    }
    if (span != NULL) {
        if (!diag_record_set_primary_span(r, span)) {
            diag_record_free(r);
            return NULL;
        }
    }
    if (!diag_record_add_related_str(r, "mnemonic", mnemonic) ||
        !diag_record_add_related_str(r, "feature",
                                     x64_feature_text(feature))) {
        diag_record_free(r);
        return NULL;
    }
    return r;
}

/* ---------------------------------------------------------------------------
 * Selection-wide constraint verification
 * ------------------------------------------------------------------------- */

/* Growable pointer-array append used by the verification record list
 * (mirrors ir_core.c's private helper; deterministic order = append
 * order, sorted afterwards with the contract sec. 9 comparator). */
static bool x64_ptr_append(void ***arr, size_t *count, void *item)
{
    void **p = (void **)realloc(*arr, (*count + 1) * sizeof(void *));
    if (p == NULL) {
        return false;
    }
    *arr = p;
    p[*count] = item;
    (*count)++;
    return true;
}

/* Append one violation record for instruction i. Returns false on
 * allocation failure (the caller sets the sticky OOM flag). */
static bool verify_append_violation(const IselOutput *sel,
                                    const IrBuild *build,
                                    size_t i,
                                    DiagRecord ***out_records,
                                    size_t *out_record_count)
{
    const IselInsn *insn = &sel->insns[i];
    IselX64Feature f = x64_opcode_feature(insn->op);
    const DiagSpan *span = NULL;
    char mnem[64];
    DiagRecord *r;

    /* derive the primary span from the originating IR node (DIAGNOSTIC-
     * CONTRACT sec. 11.6: derived span). build->nodes is indexed by node
     * id (id == index), so an in-range id with a node gives its span. */
    if (build != NULL && insn->ir_node_id >= 0 &&
        (size_t)insn->ir_node_id < build->nnodes) {
        IrNode *node = build->nodes[insn->ir_node_id];
        if (node != NULL && node->span != NULL) {
            span = node->span;
        }
    }

    if (f == ISEL_X64_UNKNOWN) {
        (void)snprintf(mnem, sizeof(mnem), "<opcode %d>", (int)insn->op);
        r = x64_constraint_record(mnem, f, span);
    } else {
        /* a registered but out-of-baseline opcode (not reachable with
         * the current closed enum; kept as defense in depth) */
        r = x64_constraint_record(isel_opcode_text(insn->op), f, span);
    }
    if (r == NULL) {
        return false;
    }
    if (!diag_record_add_related_int(r, "opcode", (int64_t)insn->op) ||
        !diag_record_add_related_int(r, "node_id", insn->ir_node_id) ||
        !x64_ptr_append((void ***)out_records, out_record_count, r)) {
        diag_record_free(r);
        return false;
    }
    return true;
}

IselX64Status x64_verify_constraints(const IselOutput *sel,
                                     const IrBuild *build,
                                     DiagRecord ***out_records,
                                     size_t *out_record_count)
{
    size_t i;
    size_t count = 0;
    DiagRecord **records = NULL;
    bool oom = false;
    if (sel == NULL || out_records == NULL || out_record_count == NULL) {
        return ISEL_X64_OOM;
    }
    *out_records = NULL;
    *out_record_count = 0;
    if (sel->count == 0) {
        return ISEL_X64_OK;
    }
    if (sel->insns == NULL) {
        /* caller defect: a non-empty selection must carry an instruction
         * array; nothing is owned */
        return ISEL_X64_OOM;
    }
    for (i = 0; i < sel->count; i++) {
        const IselInsn *insn = &sel->insns[i];
        IselX64Feature f;
        if (insn->op == ISEL_COMMENT || insn->op == ISEL_LABEL) {
            continue;   /* covered pseudo-ops (markers) */
        }
        f = x64_opcode_feature(insn->op);
        if (f == ISEL_X64_BASE || f == ISEL_X64_SSE2 ||
            f == ISEL_X64_PSEUDO) {
            continue;   /* within the baseline */
        }
        if (!verify_append_violation(sel, build, i, &records, &count)) {
            oom = true;
            break;
        }
    }
    if (oom) {
        if (records != NULL) {
            size_t k;
            for (k = 0; k < count; k++) {
                diag_record_free(records[k]);
            }
            free(records);
        }
        return ISEL_X64_OOM;
    }
    if (count == 0) {
        return ISEL_X64_OK;
    }
    diag_sort_records(records, count);
    *out_records = records;
    *out_record_count = count;
    return ISEL_X64_VIOLATION;
}

void x64_records_free(DiagRecord **records, size_t count)
{
    size_t i;
    if (records == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        diag_record_free(records[i]);
    }
    free(records);
}
