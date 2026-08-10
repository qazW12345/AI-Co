/* bootstrap/src/diag/diag.h
 *
 * AI-Co Stage-0 diagnostic infrastructure (WP-M0-06).
 *
 * Diagnostic record model, span model, deterministic ordering, code
 * registry, recovery/cascade marking, and the canonical JSONL emitter,
 * per DIAGNOSTIC-CONTRACT v0.1.1 (Accepted).
 *
 * Owned by WP-M0-06. Consumers (WP-M0-07 load, WP-M0-08 lex, ...) include
 * this header; the area source list is owned by this package via
 * bootstrap/build/diag.txt. Do not modify this file outside WP-M0-06's
 * package without a Planner re-planning decision.
 *
 * Emission guarantees:
 *  - one JSON object per line, LF-terminated, no embedded newlines;
 *  - fields emitted in the fixed canonical order that byte-matches the
 *    contract §12 example records (schema_version, code, severity, phase,
 *    message, primary_span, secondary_spans, recovery, causes, expected,
 *    actual, corrections, related, trap_code, exit_code);
 *  - primary_span is always emitted (object or null);
 *  - recovery is required (validated) on severity=error and trap records;
 *  - output contains no timestamps and no absolute host paths: file paths
 *    are exactly the strings stored in spans (repository-relative by
 *    contract §6) and messages are the strings supplied by the caller.
 */
#ifndef AICO_BOOTSTRAP_SRC_DIAG_DIAG_H
#define AICO_BOOTSTRAP_SRC_DIAG_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Diagnostic contract schema version emitted by this package (contract §2/§3). */
#define DIAG_SCHEMA_VERSION "1"

/* Default process exit code for runtime trap records (contract §10). */
#define DIAG_TRAP_EXIT_CODE 70

/* Severity values (contract §3). */
#define DIAG_SEVERITY_ERROR   "error"
#define DIAG_SEVERITY_WARNING "warning"
#define DIAG_SEVERITY_NOTE    "note"

/* Phase values (contract §4). */
#define DIAG_PHASE_LEX      "lex"
#define DIAG_PHASE_SYNTAX   "syntax"
#define DIAG_PHASE_NAME     "name"
#define DIAG_PHASE_TYPE     "type"
#define DIAG_PHASE_SEMANTIC "semantic"
#define DIAG_PHASE_IR       "ir"
#define DIAG_PHASE_BACKEND  "backend"
#define DIAG_PHASE_OBJECT   "object"
#define DIAG_PHASE_LINK     "link"
#define DIAG_PHASE_BUILD    "build"
#define DIAG_PHASE_TRAP     "trap"

/* Recovery markings (contract §7). */
#define DIAG_RECOVERY_AUTHORITATIVE    "authoritative"
#define DIAG_RECOVERY_CASCADING        "cascading"
#define DIAG_RECOVERY_RECOVERY_DERIVED "recovery_derived"

/* Position inside a file: 1-based line, 1-based UTF-8 byte column,
 * 0-based byte offset (contract §6). */
typedef struct DiagPosition {
    int64_t line;
    int64_t col;
    int64_t offset;
} DiagPosition;

/* Source span (contract §6). file is the path relative to the project root;
 * the span is empty (start == end) for a point location. */
typedef struct DiagSpan {
    char *file;
    DiagPosition start;
    DiagPosition end;
} DiagSpan;

/* One key/value member of the `related` / `expected` / `actual` objects
 * (contract §4). Values are either strings or signed 64-bit integers;
 * insertion order is preserved by the emitter. */
typedef enum DiagKvKind {
    DIAG_KV_STRING = 0,
    DIAG_KV_INT = 1
} DiagKvKind;

typedef struct DiagKv {
    char *key;
    DiagKvKind kind;
    char *str;   /* owned when kind == DIAG_KV_STRING */
    int64_t i;   /* used when kind == DIAG_KV_INT */
} DiagKv;

/* One entry of the `causes` causal chain (contract §4): a record-shaped
 * object with code, message, and optional primary_span. */
typedef struct DiagCause {
    char *code;
    char *message;
    DiagSpan *span;   /* owned or NULL */
} DiagCause;

/* One candidate correction (contract §8): replacement text plus an optional
 * span; an omitted span means insertion at the primary span's start. */
typedef struct DiagCorrection {
    char *replacement;
    DiagSpan *span;   /* owned or NULL */
} DiagCorrection;

/* A complete diagnostic record (contract §3/§4). */
typedef struct DiagRecord {
    char *code;               /* owned; must exist in the code registry */
    char *severity;           /* owned; error | warning | note */
    char *phase;              /* owned */
    char *message;            /* owned */
    DiagSpan *primary_span;   /* owned or NULL (null span) */
    char *recovery;           /* owned or NULL; required on error/trap */
    DiagSpan **secondary_spans;
    size_t secondary_count;
    DiagKv *expected;
    size_t expected_count;
    DiagKv *actual;
    size_t actual_count;
    DiagCause *causes;
    size_t cause_count;
    DiagCorrection *corrections;
    size_t correction_count;
    DiagKv *related;
    size_t related_count;
    int64_t trap_code;        /* valid when has_trap_code */
    bool has_trap_code;
    int64_t exit_code;        /* valid when has_exit_code */
    bool has_exit_code;
} DiagRecord;

/* ---------------------------------------------------------------------------
 * Code registry (contract §11.1-11.9)
 * ------------------------------------------------------------------------- */

typedef struct DiagCodeInfo {
    const char *code;       /* e.g. "AIC-L0006" */
    const char *phase;      /* default phase for records carrying this code */
    const char *severity;   /* default severity (error in v0.1.1) */
    const char *description; /* meaning from the contract registry tables */
} DiagCodeInfo;

/* Look up a code in the registry; returns NULL when the code is unknown.
 * Unknown codes are defects: record creation/emission rejects them. */
const DiagCodeInfo *diag_code_lookup(const char *code);

/* Registry iteration (deterministic table order, contract §11.1-11.8). */
const DiagCodeInfo *diag_code_at(size_t index);
size_t diag_code_count(void);

/* ---------------------------------------------------------------------------
 * Spans
 * ------------------------------------------------------------------------- */

/* Point span (start == end); file is duplicated. */
DiagSpan *diag_span_new_point(const char *file, int64_t line, int64_t col,
                              int64_t offset);
/* Range span; file is duplicated. */
DiagSpan *diag_span_new_range(const char *file,
                              int64_t start_line, int64_t start_col, int64_t start_offset,
                              int64_t end_line, int64_t end_col, int64_t end_offset);
/* Deep copy; returns NULL for NULL input. */
DiagSpan *diag_span_clone(const DiagSpan *span);
void diag_span_free(DiagSpan *span);

/* Compute a point span (file/line/col/offset) from a byte offset into
 * line-terminator-normalized source text (LF line endings; the loader
 * package WP-M0-07 performs terminator normalization). Lines are 1-based,
 * columns 1-based byte columns within the line, offsets 0-based.
 * Returns false when text is NULL, offset is negative, or offset > len. */
bool diag_span_from_offset(const char *file, const char *text, size_t len,
                           int64_t offset, DiagSpan **out);

/* ---------------------------------------------------------------------------
 * Records
 * ------------------------------------------------------------------------- */

/* Create an empty record (all fields unset); NULL on allocation failure. */
DiagRecord *diag_record_new(void);
void diag_record_free(DiagRecord *rec);

/* Set the stable diagnostic code. The code must exist in the registry;
 * unknown codes are rejected (returns false and leaves the record
 * unchanged). On success the record's phase and severity default to the
 * registry entry's values; callers may override them afterwards. */
bool diag_record_set_code(DiagRecord *rec, const char *code);
bool diag_record_set_severity(DiagRecord *rec, const char *severity);
bool diag_record_set_phase(DiagRecord *rec, const char *phase);
bool diag_record_set_message(DiagRecord *rec, const char *message);
/* NULL clears the primary span (null span). */
bool diag_record_set_primary_span(DiagRecord *rec, const DiagSpan *span);
/* NULL clears the recovery marking. */
bool diag_record_set_recovery(DiagRecord *rec, const char *recovery);

bool diag_record_add_secondary_span(DiagRecord *rec, const DiagSpan *span);
bool diag_record_add_related_str(DiagRecord *rec, const char *key, const char *value);
bool diag_record_add_related_int(DiagRecord *rec, const char *key, int64_t value);
bool diag_record_add_expected_str(DiagRecord *rec, const char *key, const char *value);
bool diag_record_add_expected_int(DiagRecord *rec, const char *key, int64_t value);
bool diag_record_add_actual_str(DiagRecord *rec, const char *key, const char *value);
bool diag_record_add_actual_int(DiagRecord *rec, const char *key, int64_t value);
bool diag_record_add_cause(DiagRecord *rec, const char *code, const char *message,
                           const DiagSpan *span);
bool diag_record_add_correction(DiagRecord *rec, const char *replacement,
                                const DiagSpan *span);
bool diag_record_set_trap_code(DiagRecord *rec, int64_t trap_code);
bool diag_record_set_exit_code(DiagRecord *rec, int64_t exit_code);

/* Validate a record against the contract: required fields present, code in
 * the registry, severity/phase/recovery values valid, phase matches the
 * code's registry phase, recovery present on error/trap records, trap_code
 * within u32 range. Returns true when valid; on false, errbuf (when
 * non-NULL) receives a short reason. */
bool diag_record_validate(const DiagRecord *rec, char *errbuf, size_t errbuf_size);

/* Trap-record factories (contract §10): phase="trap", severity="error",
 * recovery="authoritative". diag_trap_record sets exit_code=70;
 * diag_trap_record_ex accepts an explicit exit code (values <= 0 fall back
 * to 70). Only trap-phase codes (AIC-Rxxxx or AIC-U0000) are accepted; other
 * codes return NULL. The span may be NULL. */
DiagRecord *diag_trap_record(const char *code, const char *message,
                             const DiagSpan *span);
DiagRecord *diag_trap_record_ex(const char *code, const char *message,
                                const DiagSpan *span, int64_t exit_code);
/* User trap via rt.trap.report: code "AIC-U0000", trap_code = caller code
 * (u32 range 0..4294967295), exit_code 70. NULL when caller_code is out of
 * range or allocation fails. */
DiagRecord *diag_user_trap_record(int64_t caller_code, const char *message,
                                  const DiagSpan *span);

/* ---------------------------------------------------------------------------
 * Deterministic ordering (contract §9)
 * ------------------------------------------------------------------------- */

/* qsort-compatible comparator over DiagRecord* elements:
 *  1. phase rank (lex < syntax < name < type < semantic < ir < backend <
 *     object < link < build; "trap" is appended after build as the terminal
 *     phase - a bounded implementation extension needed for a total order;
 *     trap records are emitted singly at trap time and are not batch-ordered
 *     by the contract);
 *  2. within a phase, null primary_span records before file-bearing records,
 *     null-span ties by code lexicographically;
 *  3. file-bearing records by primary_span.file lexicographically, then
 *     start.offset, then code lexicographically. */
int diag_record_compare(const void *pa, const void *pb);
void diag_sort_records(DiagRecord **recs, size_t count);

/* ---------------------------------------------------------------------------
 * JSONL emitter
 * ------------------------------------------------------------------------- */

/* Growable byte buffer sink used by the emitter; deterministic (no
 * timestamps, no host paths). */
typedef struct DiagBuf {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} DiagBuf;

void diag_buf_init(DiagBuf *buf);
void diag_buf_free(DiagBuf *buf);
bool diag_buf_ok(const DiagBuf *buf);

/* Append one JSONL record line (LF-terminated). Validates the record first;
 * returns false (emitting nothing) for invalid records or OOM. */
bool diag_emit_record(DiagBuf *out, const DiagRecord *rec);
/* Sort records in place with the contract §9 comparator, then emit each.
 * Returns false when any record is invalid or on OOM. */
bool diag_emit_records_sorted(DiagBuf *out, DiagRecord **recs, size_t count);
/* Write the buffer contents to a stream (e.g. stderr for trap records). */
bool diag_buf_write_file(DiagBuf *buf, FILE *f);

#endif /* AICO_BOOTSTRAP_SRC_DIAG_DIAG_H */
