/* bootstrap/src/diag/diag.c
 *
 * AI-Co Stage-0 diagnostic infrastructure (WP-M0-06): record model, span
 * helpers, and the contract §9 deterministic ordering comparator.
 *
 * All strings and arrays owned by records are heap-allocated and released by
 * diag_record_free. Setters copy their inputs; failure (allocation or
 * validation) leaves the record unchanged where documented.
 */
#include "diag.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    size_t n;
    char *p;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

static void set_err(char *errbuf, size_t size, const char *msg)
{
    size_t i = 0;
    if (errbuf == NULL || size == 0) {
        return;
    }
    while (i + 1 < size && msg[i] != '\0') {
        errbuf[i] = msg[i];
        ++i;
    }
    errbuf[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * Spans
 * ------------------------------------------------------------------------- */

DiagSpan *diag_span_new_range(const char *file,
                              int64_t start_line, int64_t start_col, int64_t start_offset,
                              int64_t end_line, int64_t end_col, int64_t end_offset)
{
    DiagSpan *s = (DiagSpan *)malloc(sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->file = dup_str(file);
    if (s->file == NULL) {
        free(s);
        return NULL;
    }
    s->start.line = start_line;
    s->start.col = start_col;
    s->start.offset = start_offset;
    s->end.line = end_line;
    s->end.col = end_col;
    s->end.offset = end_offset;
    return s;
}

DiagSpan *diag_span_new_point(const char *file, int64_t line, int64_t col,
                              int64_t offset)
{
    return diag_span_new_range(file, line, col, offset, line, col, offset);
}

DiagSpan *diag_span_clone(const DiagSpan *span)
{
    if (span == NULL) {
        return NULL;
    }
    return diag_span_new_range(span->file,
                               span->start.line, span->start.col, span->start.offset,
                               span->end.line, span->end.col, span->end.offset);
}

void diag_span_free(DiagSpan *span)
{
    if (span == NULL) {
        return;
    }
    free(span->file);
    free(span);
}

bool diag_span_from_offset(const char *file, const char *text, size_t len,
                           int64_t offset, DiagSpan **out)
{
    int64_t line;
    int64_t line_start;
    int64_t i;
    int64_t col;
    DiagSpan *s;

    if (text == NULL || out == NULL || offset < 0 || (uint64_t)offset > len) {
        return false;
    }
    line = 1;
    line_start = 0;
    for (i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    col = offset - line_start + 1;
    s = diag_span_new_point(file, line, col, offset);
    if (s == NULL) {
        return false;
    }
    *out = s;
    return true;
}

/* ---------------------------------------------------------------------------
 * Records
 * ------------------------------------------------------------------------- */

DiagRecord *diag_record_new(void)
{
    return (DiagRecord *)calloc(1, sizeof(DiagRecord));
}

static void free_kvs(DiagKv *kvs, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        free(kvs[i].key);
        free(kvs[i].str);
    }
    free(kvs);
}

void diag_record_free(DiagRecord *rec)
{
    size_t i;
    if (rec == NULL) {
        return;
    }
    free(rec->code);
    free(rec->severity);
    free(rec->phase);
    free(rec->message);
    diag_span_free(rec->primary_span);
    for (i = 0; i < rec->secondary_count; ++i) {
        diag_span_free(rec->secondary_spans[i]);
    }
    free(rec->secondary_spans);
    free_kvs(rec->expected, rec->expected_count);
    free_kvs(rec->actual, rec->actual_count);
    for (i = 0; i < rec->cause_count; ++i) {
        free(rec->causes[i].code);
        free(rec->causes[i].message);
        diag_span_free(rec->causes[i].span);
    }
    free(rec->causes);
    for (i = 0; i < rec->correction_count; ++i) {
        free(rec->corrections[i].replacement);
        diag_span_free(rec->corrections[i].span);
    }
    free(rec->corrections);
    free_kvs(rec->related, rec->related_count);
    free(rec);
}

bool diag_record_set_code(DiagRecord *rec, const char *code)
{
    const DiagCodeInfo *info;
    char *new_code;
    char *new_phase;
    char *new_severity;

    if (rec == NULL || code == NULL) {
        return false;
    }
    info = diag_code_lookup(code);
    if (info == NULL) {
        /* Unknown code: defect (contract §11.9). Record unchanged. */
        return false;
    }
    new_code = dup_str(code);
    new_phase = dup_str(info->phase);
    new_severity = dup_str(info->severity);
    if (new_code == NULL || new_phase == NULL || new_severity == NULL) {
        free(new_code);
        free(new_phase);
        free(new_severity);
        return false;
    }
    free(rec->code);
    free(rec->phase);
    free(rec->severity);
    rec->code = new_code;
    rec->phase = new_phase;
    rec->severity = new_severity;
    return true;
}

static bool is_one_of(const char *value, const char *a, const char *b, const char *c)
{
    if (value == NULL) {
        return false;
    }
    if (a != NULL && strcmp(value, a) == 0) {
        return true;
    }
    if (b != NULL && strcmp(value, b) == 0) {
        return true;
    }
    if (c != NULL && strcmp(value, c) == 0) {
        return true;
    }
    return false;
}

bool diag_record_set_severity(DiagRecord *rec, const char *severity)
{
    char *p;
    if (rec == NULL || !is_one_of(severity, DIAG_SEVERITY_ERROR,
                                  DIAG_SEVERITY_WARNING, DIAG_SEVERITY_NOTE)) {
        return false;
    }
    p = dup_str(severity);
    if (p == NULL) {
        return false;
    }
    free(rec->severity);
    rec->severity = p;
    return true;
}

static bool phase_is_valid(const char *phase)
{
    static const char *const kPhases[] = {
        DIAG_PHASE_LEX, DIAG_PHASE_SYNTAX, DIAG_PHASE_NAME, DIAG_PHASE_TYPE,
        DIAG_PHASE_SEMANTIC, DIAG_PHASE_IR, DIAG_PHASE_BACKEND,
        DIAG_PHASE_OBJECT, DIAG_PHASE_LINK, DIAG_PHASE_BUILD, DIAG_PHASE_TRAP
    };
    size_t i;
    if (phase == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(kPhases) / sizeof(kPhases[0]); ++i) {
        if (strcmp(phase, kPhases[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool diag_record_set_phase(DiagRecord *rec, const char *phase)
{
    char *p;
    if (rec == NULL || !phase_is_valid(phase)) {
        return false;
    }
    p = dup_str(phase);
    if (p == NULL) {
        return false;
    }
    free(rec->phase);
    rec->phase = p;
    return true;
}

bool diag_record_set_message(DiagRecord *rec, const char *message)
{
    char *p;
    if (rec == NULL) {
        return false;
    }
    p = dup_str(message == NULL ? "" : message);
    if (p == NULL) {
        return false;
    }
    free(rec->message);
    rec->message = p;
    return true;
}

bool diag_record_set_primary_span(DiagRecord *rec, const DiagSpan *span)
{
    DiagSpan *clone;
    if (rec == NULL) {
        return false;
    }
    if (span == NULL) {
        diag_span_free(rec->primary_span);
        rec->primary_span = NULL;
        return true;
    }
    clone = diag_span_clone(span);
    if (clone == NULL) {
        return false;
    }
    diag_span_free(rec->primary_span);
    rec->primary_span = clone;
    return true;
}

bool diag_record_set_recovery(DiagRecord *rec, const char *recovery)
{
    char *p;
    if (rec == NULL) {
        return false;
    }
    if (recovery == NULL) {
        free(rec->recovery);
        rec->recovery = NULL;
        return true;
    }
    if (!is_one_of(recovery, DIAG_RECOVERY_AUTHORITATIVE,
                   DIAG_RECOVERY_CASCADING, DIAG_RECOVERY_RECOVERY_DERIVED)) {
        return false;
    }
    p = dup_str(recovery);
    if (p == NULL) {
        return false;
    }
    free(rec->recovery);
    rec->recovery = p;
    return true;
}

bool diag_record_add_secondary_span(DiagRecord *rec, const DiagSpan *span)
{
    DiagSpan *clone;
    DiagSpan **arr;
    if (rec == NULL || span == NULL) {
        return false;
    }
    clone = diag_span_clone(span);
    if (clone == NULL) {
        return false;
    }
    arr = (DiagSpan **)realloc(rec->secondary_spans,
                               (rec->secondary_count + 1) * sizeof(*arr));
    if (arr == NULL) {
        diag_span_free(clone);
        return false;
    }
    rec->secondary_spans = arr;
    rec->secondary_spans[rec->secondary_count] = clone;
    ++rec->secondary_count;
    return true;
}

static bool rec_add_kv(DiagKv **arr, size_t *count,
                       const char *key, DiagKvKind kind,
                       const char *str_value, int64_t int_value)
{
    char *k;
    char *s;
    DiagKv *p;

    if (key == NULL || (kind == DIAG_KV_STRING && str_value == NULL)) {
        return false;
    }
    k = dup_str(key);
    s = (kind == DIAG_KV_STRING) ? dup_str(str_value) : NULL;
    if (k == NULL || (kind == DIAG_KV_STRING && s == NULL)) {
        free(k);
        free(s);
        return false;
    }
    p = (DiagKv *)realloc(*arr, (*count + 1) * sizeof(**arr));
    if (p == NULL) {
        free(k);
        free(s);
        return false;
    }
    *arr = p;
    p = &(*arr)[*count];
    ++*count;
    p->key = k;
    p->kind = kind;
    p->str = s;
    p->i = int_value;
    return true;
}

bool diag_record_add_related_str(DiagRecord *rec, const char *key, const char *value)
{
    return rec != NULL &&
           rec_add_kv(&rec->related, &rec->related_count, key, DIAG_KV_STRING, value, 0);
}

bool diag_record_add_related_int(DiagRecord *rec, const char *key, int64_t value)
{
    return rec != NULL &&
           rec_add_kv(&rec->related, &rec->related_count, key, DIAG_KV_INT, NULL, value);
}

bool diag_record_add_expected_str(DiagRecord *rec, const char *key, const char *value)
{
    return rec != NULL &&
           rec_add_kv(&rec->expected, &rec->expected_count, key, DIAG_KV_STRING, value, 0);
}

bool diag_record_add_expected_int(DiagRecord *rec, const char *key, int64_t value)
{
    return rec != NULL &&
           rec_add_kv(&rec->expected, &rec->expected_count, key, DIAG_KV_INT, NULL, value);
}

bool diag_record_add_actual_str(DiagRecord *rec, const char *key, const char *value)
{
    return rec != NULL &&
           rec_add_kv(&rec->actual, &rec->actual_count, key, DIAG_KV_STRING, value, 0);
}

bool diag_record_add_actual_int(DiagRecord *rec, const char *key, int64_t value)
{
    return rec != NULL &&
           rec_add_kv(&rec->actual, &rec->actual_count, key, DIAG_KV_INT, NULL, value);
}

bool diag_record_add_cause(DiagRecord *rec, const char *code, const char *message,
                           const DiagSpan *span)
{
    DiagCause *arr;
    DiagCause *cause;
    if (rec == NULL || code == NULL || message == NULL) {
        return false;
    }
    arr = (DiagCause *)realloc(rec->causes, (rec->cause_count + 1) * sizeof(*arr));
    if (arr == NULL) {
        return false;
    }
    rec->causes = arr;
    cause = &rec->causes[rec->cause_count];
    cause->code = NULL;
    cause->message = NULL;
    cause->span = NULL;
    cause->code = dup_str(code);
    cause->message = dup_str(message);
    cause->span = diag_span_clone(span);
    if (cause->code == NULL || cause->message == NULL) {
        /* span clone failure with NULL span input is not an error, so only
         * the string allocations can fail here; roll back the slot. */
        free(cause->code);
        free(cause->message);
        diag_span_free(cause->span);
        return false;
    }
    ++rec->cause_count;
    return true;
}

bool diag_record_add_correction(DiagRecord *rec, const char *replacement,
                                const DiagSpan *span)
{
    DiagCorrection *arr;
    DiagCorrection *corr;
    if (rec == NULL || replacement == NULL) {
        return false;
    }
    arr = (DiagCorrection *)realloc(rec->corrections,
                                    (rec->correction_count + 1) * sizeof(*arr));
    if (arr == NULL) {
        return false;
    }
    rec->corrections = arr;
    corr = &rec->corrections[rec->correction_count];
    corr->replacement = NULL;
    corr->span = NULL;
    corr->replacement = dup_str(replacement);
    corr->span = diag_span_clone(span);
    if (corr->replacement == NULL) {
        free(corr->replacement);
        diag_span_free(corr->span);
        return false;
    }
    ++rec->correction_count;
    return true;
}

bool diag_record_set_trap_code(DiagRecord *rec, int64_t trap_code)
{
    if (rec == NULL || trap_code < 0 || trap_code > (int64_t)UINT32_MAX) {
        return false;
    }
    rec->trap_code = trap_code;
    rec->has_trap_code = true;
    return true;
}

bool diag_record_set_exit_code(DiagRecord *rec, int64_t exit_code)
{
    if (rec == NULL) {
        return false;
    }
    rec->exit_code = exit_code;
    rec->has_exit_code = true;
    return true;
}

bool diag_record_validate(const DiagRecord *rec, char *errbuf, size_t errbuf_size)
{
    const DiagCodeInfo *info;
    bool is_error;
    bool is_trap;

    if (errbuf != NULL && errbuf_size > 0) {
        errbuf[0] = '\0';
    }
    if (rec == NULL) {
        set_err(errbuf, errbuf_size, "record is NULL");
        return false;
    }
    if (rec->code == NULL) {
        set_err(errbuf, errbuf_size, "code is required");
        return false;
    }
    info = diag_code_lookup(rec->code);
    if (info == NULL) {
        set_err(errbuf, errbuf_size, "unknown diagnostic code");
        return false;
    }
    if (rec->severity == NULL) {
        set_err(errbuf, errbuf_size, "severity is required");
        return false;
    }
    if (!is_one_of(rec->severity, DIAG_SEVERITY_ERROR,
                   DIAG_SEVERITY_WARNING, DIAG_SEVERITY_NOTE)) {
        set_err(errbuf, errbuf_size, "invalid severity");
        return false;
    }
    if (rec->phase == NULL) {
        set_err(errbuf, errbuf_size, "phase is required");
        return false;
    }
    if (strcmp(rec->phase, info->phase) != 0) {
        set_err(errbuf, errbuf_size, "phase does not match the code registry");
        return false;
    }
    if (rec->message == NULL) {
        set_err(errbuf, errbuf_size, "message is required");
        return false;
    }
    is_error = (strcmp(rec->severity, DIAG_SEVERITY_ERROR) == 0);
    is_trap = (strcmp(rec->phase, DIAG_PHASE_TRAP) == 0);
    if ((is_error || is_trap) && rec->recovery == NULL) {
        set_err(errbuf, errbuf_size, "recovery is required on error and trap records");
        return false;
    }
    if (rec->recovery != NULL &&
        !is_one_of(rec->recovery, DIAG_RECOVERY_AUTHORITATIVE,
                   DIAG_RECOVERY_CASCADING, DIAG_RECOVERY_RECOVERY_DERIVED)) {
        set_err(errbuf, errbuf_size, "invalid recovery marking");
        return false;
    }
    if (rec->has_trap_code &&
        (rec->trap_code < 0 || rec->trap_code > (int64_t)UINT32_MAX)) {
        set_err(errbuf, errbuf_size, "trap_code is outside u32 range");
        return false;
    }
    return true;
}

DiagRecord *diag_trap_record_ex(const char *code, const char *message,
                                const DiagSpan *span, int64_t exit_code)
{
    DiagRecord *rec;
    const DiagCodeInfo *info;

    if (code == NULL) {
        return NULL;
    }
    info = diag_code_lookup(code);
    if (info == NULL || strcmp(info->phase, DIAG_PHASE_TRAP) != 0) {
        /* Trap factory accepts only trap-phase codes (AIC-R* / AIC-U0000). */
        return NULL;
    }
    rec = diag_record_new();
    if (rec == NULL) {
        return NULL;
    }
    if (!diag_record_set_code(rec, code) ||
        !diag_record_set_phase(rec, DIAG_PHASE_TRAP) ||
        !diag_record_set_severity(rec, DIAG_SEVERITY_ERROR) ||
        !diag_record_set_message(rec, message == NULL ? "" : message) ||
        !diag_record_set_primary_span(rec, span) ||
        !diag_record_set_recovery(rec, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(rec);
        return NULL;
    }
    if (exit_code <= 0) {
        exit_code = DIAG_TRAP_EXIT_CODE;
    }
    if (!diag_record_set_exit_code(rec, exit_code)) {
        diag_record_free(rec);
        return NULL;
    }
    return rec;
}

DiagRecord *diag_trap_record(const char *code, const char *message,
                             const DiagSpan *span)
{
    return diag_trap_record_ex(code, message, span, DIAG_TRAP_EXIT_CODE);
}

DiagRecord *diag_user_trap_record(int64_t caller_code, const char *message,
                                  const DiagSpan *span)
{
    DiagRecord *rec;
    if (caller_code < 0 || caller_code > (int64_t)UINT32_MAX) {
        return NULL;
    }
    rec = diag_trap_record("AIC-U0000", message, span);
    if (rec == NULL) {
        return NULL;
    }
    rec->trap_code = caller_code;
    rec->has_trap_code = true;
    return rec;
}

/* ---------------------------------------------------------------------------
 * Deterministic ordering (contract §9)
 * ------------------------------------------------------------------------- */

/* Phase rank. The contract §9 fixed phase order is lex, syntax, name, type,
 * semantic, ir, backend, object, link, build. "trap" is appended as the
 * terminal rank: the contract does not batch-order trap records (they are
 * emitted singly at trap time), but the comparator needs a total order, and
 * the terminal position is the least surprising deterministic extension.
 * Unknown phases (defensive; validation rejects them) sort after all known
 * phases, tie-broken by the remaining ordering keys. */
static int phase_rank(const char *phase)
{
    static const char *const kOrder[] = {
        DIAG_PHASE_LEX, DIAG_PHASE_SYNTAX, DIAG_PHASE_NAME, DIAG_PHASE_TYPE,
        DIAG_PHASE_SEMANTIC, DIAG_PHASE_IR, DIAG_PHASE_BACKEND,
        DIAG_PHASE_OBJECT, DIAG_PHASE_LINK, DIAG_PHASE_BUILD, DIAG_PHASE_TRAP
    };
    size_t i;
    if (phase == NULL) {
        return -1;
    }
    for (i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); ++i) {
        if (strcmp(phase, kOrder[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int diag_record_compare(const void *pa, const void *pb)
{
    const DiagRecord *a = *(const DiagRecord *const *)pa;
    const DiagRecord *b = *(const DiagRecord *const *)pb;
    int ra;
    int rb;
    int c;

    if (a == NULL && b == NULL) {
        return 0;
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }

    /* 1. phase, fixed order (contract §9 rule 1). */
    ra = phase_rank(a->phase);
    rb = phase_rank(b->phase);
    if (ra < 0) {
        ra = 11;
    }
    if (rb < 0) {
        rb = 11;
    }
    if (ra != rb) {
        return ra < rb ? -1 : 1;
    }

    /* 2. within a phase, null primary_span sorts before file-bearing;
     *    null-span ties break by code (rule 2). */
    if (a->primary_span == NULL && b->primary_span == NULL) {
        return strcmp(a->code, b->code);
    }
    if (a->primary_span == NULL) {
        return -1;
    }
    if (b->primary_span == NULL) {
        return 1;
    }

    /* 3. file-bearing: file, then start.offset, then code (rules 3-5). */
    c = strcmp(a->primary_span->file, b->primary_span->file);
    if (c != 0) {
        return c;
    }
    if (a->primary_span->start.offset != b->primary_span->start.offset) {
        return a->primary_span->start.offset < b->primary_span->start.offset ? -1 : 1;
    }
    return strcmp(a->code, b->code);
}

void diag_sort_records(DiagRecord **recs, size_t count)
{
    if (recs == NULL || count <= 1) {
        return;
    }
    qsort(recs, count, sizeof(DiagRecord *), diag_record_compare);
}
