/* bootstrap/src/load/load_test.c
 *
 * WP-M0-07 unit tests: byte-level UTF-8 vectors, BOM / NUL / invalid
 * sequence rejection (AIC-L0001/0002/0003), line-terminator normalization
 * and exact spans (CRLF, LF, lone CR), diag record shape, and file reading.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-load' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/load/load_test.c bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-load/load_test.exe
 *
 * Span policy under test (see README.md): AIC-L0001's primary span is the
 * malformed run as consumed - the lead byte through the last byte of the
 * failed sequence (all bytes of a completed overlong/surrogate/out-of-range
 * sequence; lead + valid continuations for truncation; the lead byte alone
 * when the next byte is not a continuation; one byte for an invalid lead or
 * stray continuation). This matches the maximal-subpart behavior of the
 * Unicode standard (D92) and the negative corpus (derived-lex-invalid-utf8:
 * a single 0xFF at offset 26 spans exactly one byte).
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void check_record(const DiagRecord *r, const char *code,
                         const char *file,
                         int64_t sl, int64_t sc, int64_t so,
                         int64_t el, int64_t ec, int64_t eo)
{
    CHECK(r != NULL);
    if (r == NULL) {
        return;
    }
    CHECK(strcmp(r->code, code) == 0);
    CHECK(strcmp(r->severity, "error") == 0);
    CHECK(strcmp(r->phase, "lex") == 0);
    CHECK(r->recovery != NULL && strcmp(r->recovery, "authoritative") == 0);
    CHECK(r->primary_span != NULL);
    if (r->primary_span != NULL) {
        CHECK(strcmp(r->primary_span->file, file) == 0);
        CHECK(r->primary_span->start.line == sl);
        CHECK(r->primary_span->start.col == sc);
        CHECK(r->primary_span->start.offset == so);
        CHECK(r->primary_span->end.line == el);
        CHECK(r->primary_span->end.col == ec);
        CHECK(r->primary_span->end.offset == eo);
    }
}

/* ---------------------------------------------------------------------------
 * Valid UTF-8
 * ------------------------------------------------------------------------- */

static void test_valid_utf8(void)
{
    /* ASCII + 2-byte + 3-byte + 4-byte code points in one file. */
    {
        const uint8_t bytes[] =
            "module main;\n"
            "var s: str = \"caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x92\xA9\";\n";
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes) - 1,
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        if (src != NULL) {
            CHECK(src->len == sizeof(bytes) - 1);
            CHECK(src->line_count == 3);
            CHECK(strcmp(src->file, "input.ai") == 0);
        }
        load_source_free(src);
    }

    /* UTF-8 boundary code points: all valid. */
    {
        static const uint8_t u007f[] = { 0x7F };
        static const uint8_t u0080[] = { 0xC2, 0x80 };
        static const uint8_t u07ff[] = { 0xDF, 0xBF };
        static const uint8_t u0800[] = { 0xE0, 0xA0, 0x80 };
        static const uint8_t uffff[] = { 0xEF, 0xBF, 0xBF };
        static const uint8_t u10000[] = { 0xF0, 0x90, 0x80, 0x80 };
        static const uint8_t u10ffff[] = { 0xF4, 0x8F, 0xBF, 0xBF };
        static const uint8_t *valid[] = {
            u007f, u0080, u07ff, u0800, uffff, u10000, u10ffff
        };
        static const size_t valid_len[] = {
            1, 2, 2, 3, 3, 4, 4
        };
        size_t i;
        for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
            LoadSource *src = NULL;
            DiagRecord **recs = NULL;
            size_t n = 0;
            LoadStatus st = load_source_from_bytes("input.ai", valid[i],
                                                   valid_len[i],
                                                   &src, &recs, &n);
            CHECK(st == LOAD_OK);
            CHECK(src != NULL && src->len == valid_len[i]);
            CHECK(recs == NULL && n == 0);
            load_source_free(src);
        }
    }
}

/* ---------------------------------------------------------------------------
 * BOM (AIC-L0002)
 * ------------------------------------------------------------------------- */

static void test_bom(void)
{
    /* BOM at the start of the file: rejected, span is the BOM bytes
     * (corpus 18-1-lex-bom expects (1,1,0)-(1,4,3)). */
    {
        const uint8_t bytes[] = { 0xEF, 0xBB, 0xBF,
                                  'm', 'o', 'd', 'u', 'l', 'e', ';' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(src == NULL);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0002", "input.ai",
                         1, 1, 0, 1, 4, 3);
            CHECK(strcmp(recs[0]->message, "UTF-8 BOM present at start of file") == 0);
        }
        load_records_free(recs, n);
    }

    /* BOM alone. */
    {
        const uint8_t bytes[] = { 0xEF, 0xBB, 0xBF };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0002", "input.ai",
                         1, 1, 0, 1, 4, 3);
        }
        load_records_free(recs, n);
    }

    /* BOM mid-file is a valid U+FEFF code point, not a load error. */
    {
        const uint8_t bytes[] = { 'a', 0xEF, 0xBB, 0xBF, 'b' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 5);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* BOM plus a second load error: both reported, sorted by offset. */
    {
        const uint8_t bytes[] = { 0xEF, 0xBB, 0xBF, 0x00 };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 2);
        if (recs != NULL && n >= 2) {
            check_record(recs[0], "AIC-L0002", "input.ai",
                         1, 1, 0, 1, 4, 3);
            check_record(recs[1], "AIC-L0003", "input.ai",
                         1, 4, 3, 1, 5, 4);
        }
        load_records_free(recs, n);
    }
}

/* ---------------------------------------------------------------------------
 * NUL byte (AIC-L0003)
 * ------------------------------------------------------------------------- */

static void test_nul(void)
{
    /* Top-level NUL: rejected with the corpus-exact span
     * (derived-lex-nul-byte expects (2,1,13)-(2,2,14)). */
    {
        const uint8_t bytes[] = { 'm','o','d','u','l','e',' ','m','a','i','n',';',
                                  '\n', 0x00, 'v','a','r',' ','x',':',' ','i','3','2',';','\n' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(src == NULL);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0003", "input.ai",
                         2, 1, 13, 2, 2, 14);
            CHECK(strcmp(recs[0]->message, "NUL byte in source") == 0);
        }
        load_records_free(recs, n);
    }

    /* NUL inside a line comment: allowed (spec sec. 3.1 qualifier). */
    {
        const uint8_t bytes[] = { '/', '/', 'c', 0x00, '\n', 'x' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* NUL inside a block comment: allowed. */
    {
        const uint8_t bytes[] = { '/', '*', 'x', 0x00, '*', '/' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* NUL inside an unterminated block comment: allowed at load time (the
     * lexer reports AIC-L0004 later; the loader does not misclassify). */
    {
        const uint8_t bytes[] = { '/', '*', 0x00, 'x' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* NUL inside a string literal: allowed (spec sec. 4.4 permits any code
     * point except " \\ and line terminators; a raw U+0000 is a character). */
    {
        const uint8_t bytes[] = { '"', 'a', 0x00, 'b', '"' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* NUL after an escaped quote inside a string: still inside the string. */
    {
        const uint8_t bytes[] = { '"', 'a', '\\', '"', 0x00, 'b', '"' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* String/comment markers inside a string do not change context. */
    {
        const uint8_t bytes[] = { '"', '/', '/', '*', 0x00, '"' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        CHECK(recs == NULL && n == 0);
        load_source_free(src);
    }

    /* Raw LF inside a string ends the string context (recovery); a NUL on a
     * later line is therefore code and is rejected. */
    {
        const uint8_t bytes[] = { '"', 'a', '\n', 0x00 };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0003", "input.ai",
                         2, 1, 3, 2, 2, 4);
        }
        load_records_free(recs, n);
    }

    /* Backslash in code does not create a string context. */
    {
        const uint8_t bytes[] = { '\\', 0x00 };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0003", "input.ai",
                         1, 2, 1, 1, 3, 2);
        }
        load_records_free(recs, n);
    }

    /* Two top-level NULs: two records, sorted by offset. */
    {
        const uint8_t bytes[] = { 'a', 0x00, 'b', 0x00, 'c' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 2);
        if (recs != NULL && n >= 2) {
            check_record(recs[0], "AIC-L0003", "input.ai",
                         1, 2, 1, 1, 3, 2);
            check_record(recs[1], "AIC-L0003", "input.ai",
                         1, 4, 3, 1, 5, 4);
        }
        load_records_free(recs, n);
    }
}

/* ---------------------------------------------------------------------------
 * Invalid UTF-8 (AIC-L0001)
 * ------------------------------------------------------------------------- */

typedef struct InvalidVec {
    const uint8_t *bytes;
    size_t len;
    size_t nrecords;
    size_t first_start;   /* span of the first record (by offset) */
    size_t first_end;
    size_t second_start;  /* span of the second record, or (size_t)-1 */
    size_t second_end;
    const char *label;
} InvalidVec;

static void test_invalid_utf8(void)
{
    static const uint8_t v_stray80[] = { 0x80 };
    static const uint8_t v_straybf[] = { 0xBF };
    static const uint8_t v_trunc_c2[] = { 0xC2 };
    static const uint8_t v_badcont_c220[] = { 0xC2, 0x20 };
    static const uint8_t v_trunc_e282[] = { 0xE2, 0x82 };
    static const uint8_t v_trunc_f09f92[] = { 0xF0, 0x9F, 0x92 };
    static const uint8_t v_ff[] = { 0xFF };
    static const uint8_t v_f5[] = { 0xF5 };
    static const uint8_t v_over_e08080[] = { 0xE0, 0x80, 0x80 };
    static const uint8_t v_over_e09fbf[] = { 0xE0, 0x9F, 0xBF };
    static const uint8_t v_surr_eda080[] = { 0xED, 0xA0, 0x80 };
    static const uint8_t v_surr_edbfbf[] = { 0xED, 0xBF, 0xBF };
    static const uint8_t v_over_f08fbfbf[] = { 0xF0, 0x8F, 0xBF, 0xBF };
    static const uint8_t v_range_f4908080[] = { 0xF4, 0x90, 0x80, 0x80 };
    static const uint8_t v_c2c3a9[] = { 0xC2, 0xC3, 0xA9 };
    static const uint8_t v_c080[] = { 0xC0, 0x80 };
    static const uint8_t v_c1bf[] = { 0xC1, 0xBF };
    static const uint8_t v_feff[] = { 0xFE, 0xFF };
    static const uint8_t v_e228a1[] = { 0xE2, 0x28, 0xA1 };

    static const InvalidVec vecs[] = {
        { v_stray80,    1, 1, 0, 1, (size_t)-1, 0, "stray continuation 0x80" },
        { v_straybf,    1, 1, 0, 1, (size_t)-1, 0, "stray continuation 0xBF" },
        { v_trunc_c2,   1, 1, 0, 1, (size_t)-1, 0, "truncated 2-byte at EOF" },
        { v_badcont_c220, 2, 1, 0, 1, (size_t)-1, 0, "lead then non-continuation" },
        { v_trunc_e282, 2, 1, 0, 2, (size_t)-1, 0, "truncated 3-byte at EOF" },
        { v_trunc_f09f92, 3, 1, 0, 3, (size_t)-1, 0, "truncated 4-byte at EOF" },
        { v_ff,         1, 1, 0, 1, (size_t)-1, 0, "invalid lead 0xFF" },
        { v_f5,         1, 1, 0, 1, (size_t)-1, 0, "invalid lead 0xF5" },
        { v_over_e08080, 3, 1, 0, 3, (size_t)-1, 0, "overlong U+0000" },
        { v_over_e09fbf, 3, 1, 0, 3, (size_t)-1, 0, "overlong U+07FF" },
        { v_surr_eda080, 3, 1, 0, 3, (size_t)-1, 0, "surrogate U+D800" },
        { v_surr_edbfbf, 3, 1, 0, 3, (size_t)-1, 0, "surrogate U+DFFF" },
        { v_over_f08fbfbf, 4, 1, 0, 4, (size_t)-1, 0, "overlong U+FFFF" },
        { v_range_f4908080, 4, 1, 0, 4, (size_t)-1, 0, "out of range U+110000" },
        { v_c2c3a9,     3, 1, 0, 1, (size_t)-1, 0, "lead then valid 2-byte lead" },
        { v_c080,       2, 2, 0, 1, 1, 2, "invalid lead 0xC0 then stray" },
        { v_c1bf,       2, 2, 0, 1, 1, 2, "invalid lead 0xC1 then stray" },
        { v_feff,       2, 2, 0, 1, 1, 2, "invalid leads 0xFE 0xFF" },
        { v_e228a1,     3, 2, 0, 1, 2, 3, "truncated then stray" },
    };

    size_t i;
    for (i = 0; i < sizeof(vecs) / sizeof(vecs[0]); ++i) {
        const InvalidVec *v = &vecs[i];
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", v->bytes,
                                               v->len, &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(src == NULL);
        CHECK(recs != NULL && n == v->nrecords);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0001", "input.ai",
                         1, (int64_t)v->first_start + 1, (int64_t)v->first_start,
                         1, (int64_t)v->first_end + 1, (int64_t)v->first_end);
        }
        if (recs != NULL && n >= 2 && v->second_start != (size_t)-1) {
            check_record(recs[1], "AIC-L0001", "input.ai",
                         1, (int64_t)v->second_start + 1, (int64_t)v->second_start,
                         1, (int64_t)v->second_end + 1, (int64_t)v->second_end);
        }
        load_records_free(recs, n);
    }

    /* A run of continuation bytes after one invalid lead produces one
     * record per malformed subpart (maximal-subpart semantics). */
    {
        const uint8_t bytes[] = { 0xF5, 0x80, 0x80, 0x80 };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 4);
        if (recs != NULL && n >= 4) {
            size_t k;
            for (k = 0; k < 4; ++k) {
                check_record(recs[k], "AIC-L0001", "input.ai",
                             1, (int64_t)k + 1, (int64_t)k,
                             1, (int64_t)k + 2, (int64_t)k + 1);
            }
        }
        load_records_free(recs, n);
    }

    /* Message text matches the corpus/contract wording. */
    {
        const uint8_t bytes[] = { 'm', 'o', 'd', 'u', 'l', 'e', ';', 0xFF };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        if (recs != NULL && n >= 1) {
            CHECK(strcmp(recs[0]->message, "invalid UTF-8 sequence") == 0);
        }
        load_records_free(recs, n);
    }
}

/* ---------------------------------------------------------------------------
 * Line terminators, spans, byte columns
 * ------------------------------------------------------------------------- */

static void test_line_terminators(void)
{
    /* LF-only file. */
    {
        const uint8_t bytes[] = "ab\ncd\nefgh";
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        int64_t line = 0;
        int64_t col = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes) - 1,
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 10 && src->line_count == 3);
        if (src != NULL) {
            CHECK(load_position(src, 0, &line, &col) && line == 1 && col == 1);
            CHECK(load_position(src, 1, &line, &col) && line == 1 && col == 2);
            /* the LF at offset 2 is the last byte of line 1 */
            CHECK(load_position(src, 2, &line, &col) && line == 1 && col == 3);
            CHECK(load_position(src, 3, &line, &col) && line == 2 && col == 1);
            CHECK(load_position(src, 5, &line, &col) && line == 2 && col == 3);
            CHECK(load_position(src, 9, &line, &col) && line == 3 && col == 4);
            CHECK(load_position(src, 10, &line, &col) && line == 3 && col == 5);
            CHECK(!load_position(src, -1, &line, &col));
            CHECK(!load_position(src, 11, &line, &col));
        }
        load_source_free(src);
    }

    /* CRLF normalizes to a single LF; spans are exact in normalized space. */
    {
        const uint8_t bytes[] = { 'a', 'b', '\r', '\n', 'c', 'd' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        int64_t line = 0;
        int64_t col = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 5 && src->line_count == 2);
        if (src != NULL) {
            CHECK(memcmp(src->text, "ab\ncd", 5) == 0);
            /* normalized text: a0 b1 \n2 c3 d4 (len 5) */
            CHECK(load_position(src, 2, &line, &col) && line == 1 && col == 3);
            CHECK(load_position(src, 3, &line, &col) && line == 2 && col == 1);
            CHECK(load_position(src, 5, &line, &col) && line == 2 && col == 3);
        }
        load_source_free(src);
    }

    /* Lone CR is whitespace, never a line break. */
    {
        const uint8_t bytes[] = { 'a', '\r', 'b' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        int64_t line = 0;
        int64_t col = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 3 && src->line_count == 1);
        if (src != NULL) {
            CHECK(load_position(src, 1, &line, &col) && line == 1 && col == 2);
            CHECK(load_position(src, 2, &line, &col) && line == 1 && col == 3);
        }
        load_source_free(src);
    }

    /* Empty file. */
    {
        static const uint8_t empty_file[] = { 0 };
        const uint8_t *bytes = empty_file;
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        int64_t line = 0;
        int64_t col = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes, 0,
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 0 && src->line_count == 1);
        if (src != NULL) {
            CHECK(load_position(src, 0, &line, &col) && line == 1 && col == 1);
        }
        load_source_free(src);
    }

    /* Trailing LF: EOF is on the next line at column 1. */
    {
        const uint8_t bytes[] = { 'a', '\n' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        int64_t line = 0;
        int64_t col = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 2 && src->line_count == 2);
        if (src != NULL) {
            CHECK(load_position(src, 2, &line, &col) && line == 2 && col == 1);
        }
        load_source_free(src);
    }

    /* Error span after a CRLF line break uses normalized offsets. */
    {
        const uint8_t bytes[] = { 'a', 'b', '\r', '\n', 'c', 'd', 0x00, 'e', 'f' };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0003", "input.ai",
                         2, 3, 5, 2, 4, 6);
        }
        load_records_free(recs, n);
    }

    /* Invalid UTF-8 after a CRLF line break. */
    {
        const uint8_t bytes[] = { 'a', '\r', '\n', 0xFF };
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0001", "input.ai",
                         2, 1, 2, 2, 2, 3);
        }
        load_records_free(recs, n);
    }
}

static void test_byte_columns(void)
{
    /* Columns are UTF-8 BYTE columns: a 2-byte char occupies 2 columns. */
    {
        const uint8_t bytes[] = { 0xC3, 0xA9, 'x' }; /* U+00E9, 'x' */
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        int64_t line = 0;
        int64_t col = 0;
        LoadStatus st = load_source_from_bytes("input.ai", bytes,
                                               sizeof(bytes),
                                               &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL);
        if (src != NULL) {
            CHECK(load_position(src, 0, &line, &col) && line == 1 && col == 1);
            CHECK(load_position(src, 1, &line, &col) && line == 1 && col == 2);
            CHECK(load_position(src, 2, &line, &col) && line == 1 && col == 3);
        }
        load_source_free(src);
    }
}

/* ---------------------------------------------------------------------------
 * Record validity and emission
 * ------------------------------------------------------------------------- */

static void test_records_valid_and_emit(void)
{
    const uint8_t bytes[] = { 0xEF, 0xBB, 0xBF, 'x' };
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t n = 0;
    char errbuf[256];
    DiagBuf out;
    size_t i;
    size_t newlines = 0;

    CHECK(load_source_from_bytes("input.ai", bytes, sizeof(bytes),
                                 &src, &recs, &n) == LOAD_VALIDATION_ERROR);
    CHECK(recs != NULL && n == 1);
    if (recs == NULL || n == 0) {
        load_records_free(recs, n);
        return;
    }

    /* Every record passes contract validation. */
    for (i = 0; i < n; ++i) {
        CHECK(diag_record_validate(recs[i], errbuf, sizeof(errbuf)));
    }

    /* Emission produces exactly one JSONL line with the expected fields. */
    diag_buf_init(&out);
    CHECK(diag_emit_record(&out, recs[0]));
    CHECK(diag_buf_ok(&out));
    if (out.data != NULL) {
        CHECK(strstr(out.data, "\"code\":\"AIC-L0002\"") != NULL);
        CHECK(strstr(out.data, "\"severity\":\"error\"") != NULL);
        CHECK(strstr(out.data, "\"phase\":\"lex\"") != NULL);
        CHECK(strstr(out.data, "\"recovery\":\"authoritative\"") != NULL);
        CHECK(strstr(out.data, "\"file\":\"input.ai\"") != NULL);
        for (i = 0; i < out.len; ++i) {
            if (out.data[i] == '\n') {
                newlines++;
            }
        }
        CHECK(newlines == 1);
    }
    diag_buf_free(&out);
    load_records_free(recs, n);
}

/* ---------------------------------------------------------------------------
 * File reading
 * ------------------------------------------------------------------------- */

static void test_file_reading(void)
{
    const char *path_bom = "bootstrap/stage0/load-test-bom.ai";
    const char *path_ok = "bootstrap/stage0/load-test-ok.ai";
    const char *path_missing = "bootstrap/stage0/load-test-missing.ai";
    FILE *f;

    /* BOM file read from disk. */
    f = fopen(path_bom, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fputc(0xEF, f);
        fputc(0xBB, f);
        fputc(0xBF, f);
        fputs("module main;", f);
        fclose(f);
    }
    {
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_file(path_bom, &src, &recs, &n);
        CHECK(st == LOAD_VALIDATION_ERROR);
        CHECK(src == NULL);
        CHECK(recs != NULL && n == 1);
        if (recs != NULL && n >= 1) {
            check_record(recs[0], "AIC-L0002", path_bom,
                         1, 1, 0, 1, 4, 3);
        }
        load_records_free(recs, n);
    }

    /* Valid file read from disk. */
    f = fopen(path_ok, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("module main;\n", f);
        fclose(f);
    }
    {
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_file(path_ok, &src, &recs, &n);
        CHECK(st == LOAD_OK);
        CHECK(src != NULL && src->len == 13 && src->line_count == 2);
        CHECK(recs == NULL && n == 0);
        if (src != NULL) {
            CHECK(strcmp(src->file, path_ok) == 0);
        }
        load_source_free(src);
    }

    /* Missing file: I/O failure, no diag record, no source. */
    {
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t n = 0;
        LoadStatus st = load_source_from_file(path_missing, &src, &recs, &n);
        CHECK(st == LOAD_IO_ERROR);
        CHECK(src == NULL);
        CHECK(recs == NULL && n == 0);
    }

    /* NULL-safety of frees. */
    load_source_free(NULL);
    load_records_free(NULL, 0);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_valid_utf8();
    test_bom();
    test_nul();
    test_invalid_utf8();
    test_line_terminators();
    test_byte_columns();
    test_records_valid_and_emit();
    test_file_reading();

    if (g_failures == 0) {
        printf("load_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "load_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
