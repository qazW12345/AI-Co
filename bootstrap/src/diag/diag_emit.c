/* bootstrap/src/diag/diag_emit.c
 *
 * AI-Co Stage-0 diagnostic infrastructure (WP-M0-06): JSONL emitter.
 *
 * Emits one JSON object per line (LF-terminated, no trailing whitespace, no
 * embedded newlines) in the fixed canonical field order that byte-matches
 * the DIAGNOSTIC-CONTRACT §12 example records:
 *
 *   schema_version, code, severity, phase, message, primary_span,
 *   secondary_spans, recovery, causes, expected, actual, corrections,
 *   related, trap_code, exit_code
 *
 * primary_span is always emitted (object or null). Strings are escaped per
 * RFC 8259 (\" \\ \b \f \n \r \t, other control bytes as \u00XX lowercase
 * hex); valid UTF-8 bytes >= 0x20 pass through unchanged, so columns and
 * offsets stay byte-true to the source.
 *
 * The emitter is deterministic: it writes exactly the caller-supplied
 * strings and numbers, never timestamps or host paths.
 */
#include "diag.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Buffer
 * ------------------------------------------------------------------------- */

static bool buf_reserve(DiagBuf *buf, size_t extra)
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

static bool buf_append_n(DiagBuf *buf, const char *s, size_t n)
{
    if (!buf_reserve(buf, n)) {
        return false;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return true;
}

static bool buf_append_cstr(DiagBuf *buf, const char *s)
{
    return buf_append_n(buf, s, strlen(s));
}

static bool buf_append_char(DiagBuf *buf, char c)
{
    return buf_append_n(buf, &c, 1);
}

void diag_buf_init(DiagBuf *buf)
{
    if (buf == NULL) {
        return;
    }
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->oom = false;
}

void diag_buf_free(DiagBuf *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->oom = false;
}

bool diag_buf_ok(const DiagBuf *buf)
{
    return buf != NULL && !buf->oom;
}

/* ---------------------------------------------------------------------------
 * JSON primitives
 * ------------------------------------------------------------------------- */

static void append_hex2(DiagBuf *buf, unsigned int v)
{
    static const char kHex[] = "0123456789abcdef";
    buf_append_char(buf, kHex[(v >> 4) & 0xF]);
    buf_append_char(buf, kHex[v & 0xF]);
}

static bool append_json_string(DiagBuf *buf, const char *s)
{
    const unsigned char *p;
    if (!buf_append_char(buf, '"')) {
        return false;
    }
    for (p = (const unsigned char *)s; *p != '\0'; ++p) {
        unsigned char c = *p;
        switch (c) {
        case '"':
            if (!buf_append_n(buf, "\\\"", 2)) return false;
            break;
        case '\\':
            if (!buf_append_n(buf, "\\\\", 2)) return false;
            break;
        case '\b':
            if (!buf_append_n(buf, "\\b", 2)) return false;
            break;
        case '\f':
            if (!buf_append_n(buf, "\\f", 2)) return false;
            break;
        case '\n':
            if (!buf_append_n(buf, "\\n", 2)) return false;
            break;
        case '\r':
            if (!buf_append_n(buf, "\\r", 2)) return false;
            break;
        case '\t':
            if (!buf_append_n(buf, "\\t", 2)) return false;
            break;
        default:
            if (c < 0x20) {
                if (!buf_append_n(buf, "\\u00", 4)) return false;
                append_hex2(buf, c);
            } else {
                if (!buf_append_char(buf, (char)c)) return false;
            }
            break;
        }
    }
    return buf_append_char(buf, '"');
}

static bool append_i64(DiagBuf *buf, int64_t v)
{
    char tmp[24];
    size_t n = 0;
    uint64_t u;
    size_t i;

    if (v < 0) {
        if (!buf_append_char(buf, '-')) {
            return false;
        }
        u = (uint64_t)(-(v + 1)) + 1;
    } else {
        u = (uint64_t)v;
    }
    if (u == 0) {
        tmp[n++] = '0';
    } else {
        while (u != 0) {
            tmp[n++] = (char)('0' + (u % 10));
            u /= 10;
        }
    }
    for (i = n; i > 0; --i) {
        if (!buf_append_char(buf, tmp[i - 1])) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Struct emitters
 * ------------------------------------------------------------------------- */

static bool emit_position(DiagBuf *buf, const DiagPosition *pos)
{
    if (!buf_append_cstr(buf, "{\"line\":") || !append_i64(buf, pos->line)) {
        return false;
    }
    if (!buf_append_cstr(buf, ",\"col\":") || !append_i64(buf, pos->col)) {
        return false;
    }
    if (!buf_append_cstr(buf, ",\"offset\":") || !append_i64(buf, pos->offset)) {
        return false;
    }
    return buf_append_char(buf, '}');
}

static bool emit_span(DiagBuf *buf, const DiagSpan *span)
{
    if (!buf_append_cstr(buf, "{\"file\":") || !append_json_string(buf, span->file)) {
        return false;
    }
    if (!buf_append_cstr(buf, ",\"start\":") || !emit_position(buf, &span->start)) {
        return false;
    }
    if (!buf_append_cstr(buf, ",\"end\":") || !emit_position(buf, &span->end)) {
        return false;
    }
    return buf_append_char(buf, '}');
}

static bool emit_kv(DiagBuf *buf, const DiagKv *kv)
{
    if (!append_json_string(buf, kv->key) || !buf_append_char(buf, ':')) {
        return false;
    }
    if (kv->kind == DIAG_KV_STRING) {
        return append_json_string(buf, kv->str);
    }
    return append_i64(buf, kv->i);
}

static bool emit_kv_object(DiagBuf *buf, const DiagKv *kvs, size_t count)
{
    size_t i;
    if (!buf_append_char(buf, '{')) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        if (i > 0 && !buf_append_char(buf, ',')) {
            return false;
        }
        if (!emit_kv(buf, &kvs[i])) {
            return false;
        }
    }
    return buf_append_char(buf, '}');
}

static bool emit_cause(DiagBuf *buf, const DiagCause *cause)
{
    if (!buf_append_cstr(buf, "{\"code\":") ||
        !append_json_string(buf, cause->code)) {
        return false;
    }
    if (!buf_append_cstr(buf, ",\"message\":") ||
        !append_json_string(buf, cause->message)) {
        return false;
    }
    if (cause->span != NULL) {
        if (!buf_append_cstr(buf, ",\"primary_span\":") ||
            !emit_span(buf, cause->span)) {
            return false;
        }
    }
    return buf_append_char(buf, '}');
}

static bool emit_correction(DiagBuf *buf, const DiagCorrection *corr)
{
    if (!buf_append_cstr(buf, "{\"replacement\":") ||
        !append_json_string(buf, corr->replacement)) {
        return false;
    }
    if (corr->span != NULL) {
        if (!buf_append_cstr(buf, ",\"span\":") ||
            !emit_span(buf, corr->span)) {
            return false;
        }
    }
    return buf_append_char(buf, '}');
}

/* ---------------------------------------------------------------------------
 * Record emission
 * ------------------------------------------------------------------------- */

bool diag_emit_record(DiagBuf *out, const DiagRecord *rec)
{
    char errbuf[128];
    size_t i;

    if (out == NULL || !diag_record_validate(rec, errbuf, sizeof(errbuf))) {
        return false;
    }

    if (!buf_append_cstr(out, "{\"schema_version\":") ||
        !append_json_string(out, DIAG_SCHEMA_VERSION)) {
        return false;
    }
    if (!buf_append_cstr(out, ",\"code\":") || !append_json_string(out, rec->code)) {
        return false;
    }
    if (!buf_append_cstr(out, ",\"severity\":") ||
        !append_json_string(out, rec->severity)) {
        return false;
    }
    if (!buf_append_cstr(out, ",\"phase\":") || !append_json_string(out, rec->phase)) {
        return false;
    }
    if (!buf_append_cstr(out, ",\"message\":") ||
        !append_json_string(out, rec->message)) {
        return false;
    }
    if (!buf_append_cstr(out, ",\"primary_span\":")) {
        return false;
    }
    if (rec->primary_span != NULL) {
        if (!emit_span(out, rec->primary_span)) {
            return false;
        }
    } else if (!buf_append_cstr(out, "null")) {
        return false;
    }

    if (rec->secondary_count > 0) {
        if (!buf_append_cstr(out, ",\"secondary_spans\":[")) {
            return false;
        }
        for (i = 0; i < rec->secondary_count; ++i) {
            if (i > 0 && !buf_append_char(out, ',')) {
                return false;
            }
            if (!emit_span(out, rec->secondary_spans[i])) {
                return false;
            }
        }
        if (!buf_append_char(out, ']')) {
            return false;
        }
    }

    if (rec->recovery != NULL) {
        if (!buf_append_cstr(out, ",\"recovery\":") ||
            !append_json_string(out, rec->recovery)) {
            return false;
        }
    }

    if (rec->cause_count > 0) {
        if (!buf_append_cstr(out, ",\"causes\":[")) {
            return false;
        }
        for (i = 0; i < rec->cause_count; ++i) {
            if (i > 0 && !buf_append_char(out, ',')) {
                return false;
            }
            if (!emit_cause(out, &rec->causes[i])) {
                return false;
            }
        }
        if (!buf_append_char(out, ']')) {
            return false;
        }
    }

    if (rec->expected_count > 0) {
        if (!buf_append_cstr(out, ",\"expected\":") ||
            !emit_kv_object(out, rec->expected, rec->expected_count)) {
            return false;
        }
    }

    if (rec->actual_count > 0) {
        if (!buf_append_cstr(out, ",\"actual\":") ||
            !emit_kv_object(out, rec->actual, rec->actual_count)) {
            return false;
        }
    }

    if (rec->correction_count > 0) {
        if (!buf_append_cstr(out, ",\"corrections\":[")) {
            return false;
        }
        for (i = 0; i < rec->correction_count; ++i) {
            if (i > 0 && !buf_append_char(out, ',')) {
                return false;
            }
            if (!emit_correction(out, &rec->corrections[i])) {
                return false;
            }
        }
        if (!buf_append_char(out, ']')) {
            return false;
        }
    }

    if (rec->related_count > 0) {
        if (!buf_append_cstr(out, ",\"related\":") ||
            !emit_kv_object(out, rec->related, rec->related_count)) {
            return false;
        }
    }

    if (rec->has_trap_code) {
        if (!buf_append_cstr(out, ",\"trap_code\":") ||
            !append_i64(out, rec->trap_code)) {
            return false;
        }
    }

    if (rec->has_exit_code) {
        if (!buf_append_cstr(out, ",\"exit_code\":") ||
            !append_i64(out, rec->exit_code)) {
            return false;
        }
    }

    return buf_append_cstr(out, "}\n");
}

bool diag_emit_records_sorted(DiagBuf *out, DiagRecord **recs, size_t count)
{
    size_t i;
    if (out == NULL) {
        return false;
    }
    diag_sort_records(recs, count);
    for (i = 0; i < count; ++i) {
        if (!diag_emit_record(out, recs[i])) {
            return false;
        }
    }
    return true;
}

bool diag_buf_write_file(DiagBuf *buf, FILE *f)
{
    if (buf == NULL || f == NULL) {
        return false;
    }
    return fwrite(buf->data, 1, buf->len, f) == buf->len;
}
