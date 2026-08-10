/* bootstrap/src/load/load.c
 *
 * WP-M0-07 - Source loader / UTF-8 validation implementation.
 *
 * Pipeline for one source file:
 *   1. Read bytes (binary mode; CRLF is preserved in the raw bytes and
 *      normalized here - never by the CRT text-mode layer).
 *   2. Normalize line terminators: CRLF -> LF; a lone CR stays as a
 *      whitespace byte (spec sec. 3.1/sec. 4.1). Build the line-start index.
 *   3. Single validation pass over the normalized text:
 *        - BOM (EF BB BF) at offset 0                     -> AIC-L0002
 *        - invalid UTF-8 sequence (incl. overlong,
 *          surrogate, out-of-range, stray continuation,
 *          truncated at EOF, invalid lead)                -> AIC-L0001
 *        - NUL byte (U+0000) outside a string literal or
 *          comment (spec sec. 3.1 qualifier; sec. 4.4 allows a raw
 *          NUL as a string character)                     -> AIC-L0003
 *   4. Errors are wrapped in diag records (AIC-L0001/0002/0003, phase lex,
 *      severity error, recovery authoritative) and sorted deterministically.
 *
 * Design decisions (also recorded in README.md):
 *   - NUL context: the spec's "outside a string literal or comment" qualifier
 *     cannot be honored without knowing lexical context, so the loader's
 *     validation pass tracks line-comment / block-comment / string-literal
 *     boundaries *only* to classify NUL bytes. It does not produce tokens,
 *     validate escapes, or compute literal values; full tokenization is
 *     WP-M0-08. Recovery for malformed constructs is deterministic:
 *       * a line comment ends at LF (not at a lone CR);
 *       * a block comment runs to its closing marker (star then slash) or
 *         to EOF (unterminated -> the lexer
 *         reports AIC-L0004 later; the loader stays in comment context);
 *       * a string ends at an unescaped `"`; `\` skips the next byte; a raw
 *         LF inside a string ends the string context (the lexer reports
 *         AIC-L0007 later) so NULs on later lines are not misclassified.
 *   - AIC-L0001 span: the malformed run as consumed - the lead byte through
 *     the last byte that belonged to the failed sequence (all bytes of a
 *     completed overlong/surrogate/out-of-range sequence; the lead byte plus
 *     valid continuations for truncation; the lead byte alone when the next
 *     byte is not a continuation; one byte for an invalid lead / stray
 *     continuation). A valid following byte is never swallowed.
 *   - I/O failures carry no diag record: the registry has no file-read code
 *     (AIC-BL0802/0803 are build-phase module-resolution concerns owned by
 *     WP-M0-10/driver). The driver decides how to report LOAD_IO_ERROR.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    size_t n;
    char *out;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    out = (char *)malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

/* ---------------------------------------------------------------------------
 * Normalization
 * ------------------------------------------------------------------------- */

/* Normalize raw source bytes to LF line endings and build the line-start
 * index. CRLF is converted to a single LF; a lone CR is kept as a whitespace
 * byte (spec sec. 3.1/sec. 4.1). Returns 0 on success, -1 on allocation failure. */
static int normalize_bytes(const uint8_t *in, size_t len,
                           char **out_text, size_t *out_len,
                           int64_t **out_line_starts, int64_t *out_line_count)
{
    char *text;
    int64_t *starts;
    size_t o = 0;
    size_t i = 0;
    int64_t lines = 1;

    text = (char *)malloc(len + 1);
    starts = (int64_t *)malloc((len + 2) * sizeof(int64_t));
    if (text == NULL || starts == NULL) {
        free(text);
        free(starts);
        return -1;
    }

    starts[0] = 0;
    while (i < len) {
        if (in[i] == '\r' && i + 1 < len && in[i + 1] == '\n') {
            text[o++] = '\n';
            starts[lines++] = (int64_t)o;
            i += 2;
        } else {
            text[o++] = (char)in[i];
            if (in[i] == '\n') {
                starts[lines++] = (int64_t)o;
            }
            i += 1;
        }
    }
    text[o] = '\0';

    *out_text = text;
    *out_len = o;
    *out_line_starts = starts;
    *out_line_count = lines;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Validation pass
 * ------------------------------------------------------------------------- */

/* Lexical context tracked ONLY to classify NUL bytes (spec sec. 3.1). */
typedef enum ScanCtx {
    SCAN_CODE = 0,
    SCAN_LINE_COMMENT,
    SCAN_BLOCK_COMMENT,
    SCAN_STRING
} ScanCtx;

typedef struct Scan {
    const char *file;
    const char *text;
    size_t len;
    const int64_t *line_starts;
    int64_t line_count;
    DiagRecord **records;
    size_t count;
    size_t cap;
    bool oom;
} Scan;

/* Rightmost line_starts[i] <= offset; offset is assumed in [0, len]. */
static void position_at(const int64_t *starts, int64_t count, int64_t offset,
                        int64_t *line, int64_t *col)
{
    int64_t lo = 0;
    int64_t hi = count - 1;
    int64_t ans = 0;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (starts[mid] <= offset) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    *line = ans + 1;
    *col = offset - starts[ans] + 1;
}

static DiagSpan *build_span(const Scan *s, size_t start, size_t end)
{
    int64_t sl;
    int64_t sc;
    int64_t el;
    int64_t ec;
    position_at(s->line_starts, s->line_count, (int64_t)start, &sl, &sc);
    position_at(s->line_starts, s->line_count, (int64_t)end, &el, &ec);
    return diag_span_new_range(s->file, sl, sc, (int64_t)start,
                               el, ec, (int64_t)end);
}

static bool scan_add(Scan *s, const char *code, const char *message,
                     size_t start, size_t end)
{
    DiagRecord *r;
    DiagSpan *sp;
    DiagRecord **arr;

    if (s->oom) {
        return false;
    }
    r = diag_record_new();
    if (r == NULL) {
        s->oom = true;
        return false;
    }
    if (!diag_record_set_code(r, code) || !diag_record_set_message(r, message)) {
        diag_record_free(r);
        s->oom = true;
        return false;
    }
    sp = build_span(s, start, end);
    if (sp == NULL) {
        diag_record_free(r);
        s->oom = true;
        return false;
    }
    if (!diag_record_set_primary_span(r, sp) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_span_free(sp);
        diag_record_free(r);
        s->oom = true;
        return false;
    }
    diag_span_free(sp);

    if (s->count == s->cap) {
        size_t ncap = s->cap == 0 ? 8 : s->cap * 2;
        arr = (DiagRecord **)realloc(s->records, ncap * sizeof(*arr));
        if (arr == NULL) {
            diag_record_free(r);
            s->oom = true;
            return false;
        }
        s->records = arr;
        s->cap = ncap;
    }
    s->records[s->count++] = r;
    return true;
}

/* Add an AIC-L0001 record for the malformed run [start, end). */
static void scan_add_invalid(Scan *s, size_t start, size_t end)
{
    scan_add(s, "AIC-L0001", "invalid UTF-8 sequence", start, end);
}

/* Collect continuation bytes for a sequence whose lead byte is at `i`.
 * `need` is the number of continuation bytes required. Returns the first
 * byte index NOT consumed: on success i + need + 1; on truncation/bad
 * continuation, the index of the first byte that broke the sequence (the
 * breaking byte itself is NOT consumed and will be re-examined). */
static size_t consume_continuations(const char *text, size_t len, size_t i,
                                    size_t need, bool *ok)
{
    size_t k;
    *ok = true;
    for (k = 1; k <= need; ++k) {
        uint8_t c;
        if (i + k >= len) {
            *ok = false;
            return i + k;
        }
        c = (uint8_t)text[i + k];
        if (c < 0x80 || c > 0xBF) {
            *ok = false;
            return i + k;
        }
    }
    return i + need + 1;
}

static void scan_text(Scan *s)
{
    size_t i = 0;
    ScanCtx ctx = SCAN_CODE;

    /* BOM at the very start (spec sec. 3.1). The BOM bytes are valid UTF-8
     * (U+FEFF), so scanning continues over them without a second record. */
    if (s->len >= 3 && (uint8_t)s->text[0] == 0xEF &&
        (uint8_t)s->text[1] == 0xBB && (uint8_t)s->text[2] == 0xBF) {
        scan_add(s, "AIC-L0002", "UTF-8 BOM present at start of file", 0, 3);
    }

    while (i < s->len) {
        uint8_t b = (uint8_t)s->text[i];

        switch (ctx) {
        case SCAN_CODE:
            if (b == '/' && i + 1 < s->len && s->text[i + 1] == '/') {
                ctx = SCAN_LINE_COMMENT;
                i += 2;
                continue;
            }
            if (b == '/' && i + 1 < s->len && s->text[i + 1] == '*') {
                ctx = SCAN_BLOCK_COMMENT;
                i += 2;
                continue;
            }
            if (b == '"') {
                ctx = SCAN_STRING;
                i += 1;
                continue;
            }
            if (b == 0x00) {
                scan_add(s, "AIC-L0003", "NUL byte in source", i, i + 1);
                i += 1;
                continue;
            }
            if (b < 0x80) {
                i += 1;
                continue;
            }
            if (b >= 0xC2 && b <= 0xDF) {
                size_t k;
                bool ok;
                k = consume_continuations(s->text, s->len, i, 1, &ok);
                if (!ok) {
                    scan_add_invalid(s, i, k);
                    i = k;
                    continue;
                }
                /* 2-byte sequences never encode overlong/surrogate values
                 * (leads C2..DF => U+0080..U+07FF). */
                i += 2;
                continue;
            }
            if (b >= 0xE0 && b <= 0xEF) {
                size_t k;
                bool ok;
                int64_t cp;
                k = consume_continuations(s->text, s->len, i, 2, &ok);
                if (!ok) {
                    scan_add_invalid(s, i, k);
                    i = k;
                    continue;
                }
                cp = ((int64_t)(b & 0x0F) << 12) |
                     ((int64_t)((uint8_t)s->text[i + 1] & 0x3F) << 6) |
                     (int64_t)((uint8_t)s->text[i + 2] & 0x3F);
                if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    /* overlong (U+0000..U+07FF in 3 bytes) or surrogate */
                    scan_add_invalid(s, i, i + 3);
                    i += 3;
                    continue;
                }
                i += 3;
                continue;
            }
            if (b >= 0xF0 && b <= 0xF4) {
                size_t k;
                bool ok;
                int64_t cp;
                k = consume_continuations(s->text, s->len, i, 3, &ok);
                if (!ok) {
                    scan_add_invalid(s, i, k);
                    i = k;
                    continue;
                }
                cp = ((int64_t)(b & 0x07) << 18) |
                     ((int64_t)((uint8_t)s->text[i + 1] & 0x3F) << 12) |
                     ((int64_t)((uint8_t)s->text[i + 2] & 0x3F) << 6) |
                     (int64_t)((uint8_t)s->text[i + 3] & 0x3F);
                if (cp < 0x10000 || cp > 0x10FFFF) {
                    /* overlong (U+0000..U+FFFF in 4 bytes) or out of range */
                    scan_add_invalid(s, i, i + 4);
                    i += 4;
                    continue;
                }
                i += 4;
                continue;
            }
            /* Invalid lead: 0x80-0xC1 (stray continuation / C0 C1) and
             * 0xF5-0xFF. Single-byte malformed run. */
            scan_add_invalid(s, i, i + 1);
            i += 1;
            continue;

        case SCAN_LINE_COMMENT:
            if (b == '\n') {
                ctx = SCAN_CODE;
            }
            i += 1;
            continue;

        case SCAN_BLOCK_COMMENT:
            if (b == '*' && i + 1 < s->len && s->text[i + 1] == '/') {
                ctx = SCAN_CODE;
                i += 2;
                continue;
            }
            i += 1;
            continue;

        case SCAN_STRING:
            if (b == '\\') {
                if (i + 1 < s->len) {
                    if (s->text[i + 1] == '\n') {
                        /* backslash before a line terminator: the string is
                         * malformed (lexer reports later); recover at LF */
                        ctx = SCAN_CODE;
                    }
                    i += 2;
                } else {
                    /* trailing backslash at EOF */
                    i += 1;
                }
                continue;
            }
            if (b == '"') {
                ctx = SCAN_CODE;
                i += 1;
                continue;
            }
            if (b == '\n') {
                /* raw LF inside a string: malformed (lexer reports AIC-L0007
                 * later); end the string context so NULs on later lines are
                 * classified as code */
                ctx = SCAN_CODE;
            }
            i += 1;
            continue;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

LoadStatus load_source_from_bytes(const char *file,
                                  const uint8_t *bytes, size_t len,
                                  LoadSource **out_src,
                                  DiagRecord ***out_records,
                                  size_t *out_record_count)
{
    LoadSource *src;
    char *text;
    size_t olen;
    int64_t *starts;
    int64_t lines;
    Scan s;

    if (out_src == NULL || out_records == NULL || out_record_count == NULL) {
        return LOAD_OOM;
    }
    *out_src = NULL;
    *out_records = NULL;
    *out_record_count = 0;

    if (normalize_bytes(bytes, len, &text, &olen, &starts, &lines) != 0) {
        return LOAD_OOM;
    }

    memset(&s, 0, sizeof(s));
    s.file = file;
    s.text = text;
    s.len = olen;
    s.line_starts = starts;
    s.line_count = lines;

    scan_text(&s);

    if (s.oom) {
        free(text);
        free(starts);
        load_records_free(s.records, s.count);
        return LOAD_OOM;
    }

    if (s.count > 0) {
        diag_sort_records(s.records, s.count);
        *out_records = s.records;
        *out_record_count = s.count;
        free(text);
        free(starts);
        return LOAD_VALIDATION_ERROR;
    }

    free(s.records);

    src = (LoadSource *)malloc(sizeof(*src));
    if (src == NULL) {
        free(text);
        free(starts);
        return LOAD_OOM;
    }
    src->file = dup_str(file);
    if (src->file == NULL) {
        free(src);
        free(text);
        free(starts);
        return LOAD_OOM;
    }
    src->text = text;
    src->len = olen;
    src->line_starts = starts;
    src->line_count = lines;
    *out_src = src;
    return LOAD_OK;
}

LoadStatus load_source_from_file(const char *path,
                                 LoadSource **out_src,
                                 DiagRecord ***out_records,
                                 size_t *out_record_count)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    size_t rd;
    LoadStatus st;

    if (path == NULL || out_src == NULL || out_records == NULL ||
        out_record_count == NULL) {
        return LOAD_OOM;
    }
    *out_src = NULL;
    *out_records = NULL;
    *out_record_count = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return LOAD_IO_ERROR;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return LOAD_IO_ERROR;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return LOAD_IO_ERROR;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return LOAD_IO_ERROR;
    }
    buf = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (buf == NULL) {
        fclose(f);
        return LOAD_OOM;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return LOAD_IO_ERROR;
    }

    st = load_source_from_bytes(path, buf, (size_t)sz,
                                out_src, out_records, out_record_count);
    free(buf);
    return st;
}

void load_source_free(LoadSource *src)
{
    if (src == NULL) {
        return;
    }
    free(src->file);
    free(src->text);
    free(src->line_starts);
    free(src);
}

void load_records_free(DiagRecord **records, size_t count)
{
    size_t i;
    if (records == NULL) {
        return;
    }
    for (i = 0; i < count; ++i) {
        diag_record_free(records[i]);
    }
    free(records);
}

bool load_position(const LoadSource *src, int64_t offset,
                   int64_t *line, int64_t *col)
{
    if (src == NULL || line == NULL || col == NULL ||
        offset < 0 || (uint64_t)offset > src->len) {
        return false;
    }
    position_at(src->line_starts, src->line_count, offset, line, col);
    return true;
}

DiagSpan *load_span_point(const LoadSource *src, int64_t offset)
{
    int64_t line;
    int64_t col;
    if (src == NULL || !load_position(src, offset, &line, &col)) {
        return NULL;
    }
    return diag_span_new_point(src->file, line, col, offset);
}

DiagSpan *load_span_range(const LoadSource *src, int64_t start_offset,
                          int64_t end_offset)
{
    int64_t sl;
    int64_t sc;
    int64_t el;
    int64_t ec;
    if (src == NULL || start_offset < 0 || end_offset < start_offset ||
        (uint64_t)end_offset > src->len) {
        return NULL;
    }
    position_at(src->line_starts, src->line_count, start_offset, &sl, &sc);
    position_at(src->line_starts, src->line_count, end_offset, &el, &ec);
    return diag_span_new_range(src->file, sl, sc, start_offset,
                               el, ec, end_offset);
}
