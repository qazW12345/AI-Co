/* bootstrap/src/diag/diag_test.c
 *
 * AI-Co Stage-0 diagnostic infrastructure (WP-M0-06): unit tests.
 *
 * Exercises the record model, span helpers, code registry, deterministic
 * ordering comparator, trap factories, and the JSONL emitter against
 * golden fixtures (bootstrap/src/diag/golden/, the five contract §12
 * example records plus structural fixtures) byte-for-byte.
 *
 * Build and run (from the repository root, so the golden-relative paths
 * resolve; the build entry points cd to the repository root):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-diag' ./bootstrap/build/build-stage0-msvc.cmd \
 *       bootstrap/src/diag/diag_test.c bootstrap/src/diag/diag.c \
 *       bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-diag/diag_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-diag)
 *
 * Output is deterministic: relative __FILE__ paths, no timestamps, no
 * host identity, exit code 0 on success / 1 on failure.
 *
 * Golden fixtures are read in binary mode and CRLF-normalized before
 * comparison so the tests survive checkout line-ending conversion
 * (core.autocrlf=true on the baseline); the emitted records themselves
 * always use LF.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            ++g_failures; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_MSG(cond, fmt, ...) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            ++g_failures; \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

/* Line helpers for examining emitted JSONL buffers. */
static size_t line_count(const char *data, size_t len)
{
    size_t n = 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            ++n;
        }
    }
    return n;
}

static void line_at(const char *data, size_t len, size_t idx,
                    char *out, size_t outsz)
{
    size_t line = 0;
    size_t start = 0;
    size_t i;
    out[0] = '\0';
    for (i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            if (line == idx) {
                size_t n = i - start;
                if (n >= outsz) {
                    n = outsz - 1;
                }
                memcpy(out, data + start, n);
                out[n] = '\0';
                return;
            }
            ++line;
            start = i + 1;
        }
    }
    if (line == idx) {
        size_t n = len - start;
        if (n >= outsz) {
            n = outsz - 1;
        }
        memcpy(out, data + start, n);
        out[n] = '\0';
    }
}

static void line_code(const char *line, char *out, size_t outsz)
{
    const char *p = strstr(line, "\"code\":\"");
    size_t n = 0;
    out[0] = '\0';
    if (p == NULL) {
        return;
    }
    p += 8;
    while (p[n] != '\0' && p[n] != '"' && n + 1 < outsz) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
}

/* ---------------------------------------------------------------------------
 * Registry coverage: every code in DIAGNOSTIC-CONTRACT §11.1-11.8
 * ------------------------------------------------------------------------- */

static const char *const kAllCodes[] = {
    /* 11.1 lex */
    "AIC-L0001", "AIC-L0002", "AIC-L0003", "AIC-L0004", "AIC-L0005",
    "AIC-L0006", "AIC-L0007", "AIC-L0008", "AIC-L0009",
    /* 11.2 syntax */
    "AIC-S0101", "AIC-S0102", "AIC-S0103", "AIC-S0104",
    /* 11.3 name */
    "AIC-N0201", "AIC-N0202", "AIC-N0203", "AIC-N0204", "AIC-N0205",
    "AIC-N0206", "AIC-N0207", "AIC-N0208", "AIC-N0209",
    /* 11.4 type */
    "AIC-T0301", "AIC-T0302", "AIC-T0303", "AIC-T0304", "AIC-T0305",
    "AIC-T0306", "AIC-T0307", "AIC-T0308", "AIC-T0309", "AIC-T0310",
    "AIC-T0311", "AIC-T0312", "AIC-T0313",
    /* 11.5 semantic */
    "AIC-E0401", "AIC-E0402", "AIC-E0403", "AIC-E0404", "AIC-E0405",
    "AIC-E0406", "AIC-E0407", "AIC-E0408", "AIC-E0409", "AIC-E0410",
    "AIC-E0411", "AIC-E0412", "AIC-E0413", "AIC-E0414", "AIC-E0415",
    "AIC-E0416", "AIC-E0417", "AIC-E0418", "AIC-E0419", "AIC-E0420",
    /* 11.6 ir / backend / object / link */
    "AIC-I0501",
    "AIC-B0601",
    "AIC-O0701", "AIC-O0702",
    /* 11.7 build */
    "AIC-BL0801", "AIC-BL0802", "AIC-BL0803",
    /* 11.8 runtime traps and user trap */
    "AIC-R0801", "AIC-R0802", "AIC-R0803", "AIC-R0804", "AIC-R0805",
    "AIC-R0806", "AIC-R0807", "AIC-R0808", "AIC-R0809", "AIC-R0810",
    "AIC-R0811", "AIC-R0812", "AIC-R0813", "AIC-R0814", "AIC-R0815",
    "AIC-R0816",
    "AIC-U0000"
};

/* Expected phase for a code, derived from the contract's class/phase-group
 * mapping (§5, §11): used to cross-check every registry entry. */
static const char *expected_phase_for(const char *code)
{
    const char *cls = code + 4; /* after "AIC-" */
    if (strncmp(cls, "BL", 2) == 0) {
        return DIAG_PHASE_BUILD;
    }
    switch (cls[0]) {
    case 'L': return DIAG_PHASE_LEX;
    case 'S': return DIAG_PHASE_SYNTAX;
    case 'N': return DIAG_PHASE_NAME;
    case 'T': return DIAG_PHASE_TYPE;
    case 'E': return DIAG_PHASE_SEMANTIC;
    case 'I': return DIAG_PHASE_IR;
    case 'B': return DIAG_PHASE_BACKEND;
    case 'O': return (cls[4] == '1') ? DIAG_PHASE_OBJECT : DIAG_PHASE_LINK;
    case 'R': return DIAG_PHASE_TRAP;
    case 'U': return DIAG_PHASE_TRAP;
    default: return NULL;
    }
}

static void test_registry(void)
{
    size_t i;
    const size_t kExpectedCount = sizeof(kAllCodes) / sizeof(kAllCodes[0]);

    CHECK(diag_code_count() == kExpectedCount);
    for (i = 0; i < diag_code_count(); ++i) {
        const DiagCodeInfo *info = diag_code_at(i);
        const char *ephase;
        CHECK(info != NULL);
        if (info == NULL) {
            continue;
        }
        CHECK(info->code != NULL && info->code[0] != '\0');
        CHECK(info->description != NULL && info->description[0] != '\0');
        CHECK(strcmp(info->severity, DIAG_SEVERITY_ERROR) == 0);
        CHECK(diag_code_lookup(info->code) == info);
        ephase = expected_phase_for(info->code);
        CHECK(ephase != NULL);
        if (ephase != NULL) {
            CHECK(strcmp(info->phase, ephase) == 0);
        }
    }
    for (i = 0; i < kExpectedCount; ++i) {
        const DiagCodeInfo *info = diag_code_lookup(kAllCodes[i]);
        CHECK(info != NULL);
        if (info != NULL) {
            CHECK(strcmp(info->phase, expected_phase_for(kAllCodes[i])) == 0);
        }
    }
    /* next-unused / unknown codes are not in the registry */
    CHECK(diag_code_lookup("AIC-L0010") == NULL);
    CHECK(diag_code_lookup("AIC-E0421") == NULL);
    CHECK(diag_code_lookup("AIC-R0817") == NULL);
    CHECK(diag_code_lookup("AIC-U0001") == NULL);
    CHECK(diag_code_lookup("AIC-X0001") == NULL);
    CHECK(diag_code_lookup("") == NULL);
    CHECK(diag_code_lookup(NULL) == NULL);
    CHECK(diag_code_at(diag_code_count()) == NULL);
}

/* ---------------------------------------------------------------------------
 * Span helpers
 * ------------------------------------------------------------------------- */

static void test_spans(void)
{
    static const char kText[] = "ab\ncd\nefgh"; /* len 10 */
    const size_t kTextLen = sizeof(kText) - 1;
    DiagSpan *pt = diag_span_new_point("main.ai", 1, 1, 0);
    DiagSpan *rg;
    DiagSpan *cl;
    DiagSpan *so;

    CHECK(pt != NULL);
    if (pt != NULL) {
        CHECK(strcmp(pt->file, "main.ai") == 0);
        CHECK(pt->start.line == 1 && pt->start.col == 1 && pt->start.offset == 0);
        CHECK(pt->end.line == pt->start.line && pt->end.offset == pt->start.offset);
    }
    rg = diag_span_new_range("a/b.ai", 5, 1, 60, 5, 2, 61);
    CHECK(rg != NULL);
    if (rg != NULL) {
        CHECK(strcmp(rg->file, "a/b.ai") == 0);
        CHECK(rg->start.offset == 60 && rg->end.offset == 61 && rg->end.col == 2);
    }
    cl = diag_span_clone(rg);
    CHECK(cl != NULL);
    if (cl != NULL) {
        CHECK(strcmp(cl->file, "a/b.ai") == 0);
        CHECK(cl->start.line == 5 && cl->end.col == 2);
    }
    CHECK(diag_span_clone(NULL) == NULL);

    so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 0, &so));
    CHECK(so != NULL && so->start.line == 1 && so->start.col == 1);
    diag_span_free(so); so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 1, &so));
    CHECK(so != NULL && so->start.line == 1 && so->start.col == 2);
    diag_span_free(so); so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 2, &so));
    CHECK(so != NULL && so->start.line == 1 && so->start.col == 3);
    diag_span_free(so); so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 3, &so));
    CHECK(so != NULL && so->start.line == 2 && so->start.col == 1);
    diag_span_free(so); so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 4, &so));
    CHECK(so != NULL && so->start.line == 2 && so->start.col == 2);
    diag_span_free(so); so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 9, &so));
    CHECK(so != NULL && so->start.line == 3 && so->start.col == 4);
    diag_span_free(so); so = NULL;
    CHECK(diag_span_from_offset("f.ai", kText, kTextLen, 10, &so));
    CHECK(so != NULL && so->start.line == 3 && so->start.col == 5);
    diag_span_free(so); so = NULL;
    CHECK(!diag_span_from_offset("f.ai", kText, kTextLen, 11, &so));
    CHECK(!diag_span_from_offset("f.ai", NULL, 0, 0, &so));
    CHECK(!diag_span_from_offset("f.ai", kText, kTextLen, -1, &so));

    diag_span_free(pt);
    diag_span_free(rg);
    diag_span_free(cl);
}

/* ---------------------------------------------------------------------------
 * Record validation and trap factories
 * ------------------------------------------------------------------------- */

static void test_record_validation(void)
{
    DiagRecord *r = diag_record_new();
    char err[128];

    CHECK(r != NULL);
    CHECK(!diag_record_validate(r, err, sizeof(err)));            /* no code */
    CHECK(diag_record_set_code(r, "AIC-L0006"));
    CHECK(strcmp(r->phase, DIAG_PHASE_LEX) == 0);                 /* auto default */
    CHECK(strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(!diag_record_validate(r, err, sizeof(err)));            /* no message */
    CHECK(diag_record_set_message(r, "m"));
    CHECK(!diag_record_validate(r, err, sizeof(err)));            /* error w/o recovery */
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    CHECK(diag_record_validate(r, err, sizeof(err)));

    /* unknown codes rejected as defects, record unchanged */
    CHECK(!diag_record_set_code(r, "AIC-L0010"));
    CHECK(!diag_record_set_code(r, "AIC-X0001"));
    CHECK(strcmp(r->code, "AIC-L0006") == 0);

    /* invalid enum values rejected */
    CHECK(!diag_record_set_severity(r, "fatal"));
    CHECK(!diag_record_set_phase(r, "phasey"));
    CHECK(!diag_record_set_recovery(r, "maybe"));

    /* phase must match the code's registry phase */
    CHECK(diag_record_set_phase(r, DIAG_PHASE_TRAP));
    CHECK(!diag_record_validate(r, err, sizeof(err)));
    CHECK(diag_record_set_phase(r, DIAG_PHASE_LEX));
    CHECK(diag_record_validate(r, err, sizeof(err)));

    /* warning without recovery is valid */
    CHECK(diag_record_set_severity(r, DIAG_SEVERITY_WARNING));
    CHECK(diag_record_validate(r, err, sizeof(err)));
    CHECK(diag_record_set_severity(r, DIAG_SEVERITY_ERROR));

    /* trap_code is a u32 */
    CHECK(!diag_record_set_trap_code(r, -1));
    CHECK(!diag_record_set_trap_code(r, (int64_t)4294967296LL));
    CHECK(diag_record_set_trap_code(r, 7));
    CHECK(diag_record_validate(r, err, sizeof(err)));

    diag_record_free(r);
}

static void test_trap_factories(void)
{
    DiagSpan *sp = diag_span_new_point("main.ai", 30, 14, 740);
    DiagRecord *t;
    DiagRecord *t2;
    DiagRecord *t3;
    DiagRecord *u;
    DiagBuf b;
    char err[128];

    t = diag_trap_record("AIC-R0807", "slice index 16 out of bounds (len 16)", sp);
    CHECK(t != NULL);
    if (t != NULL) {
        CHECK(strcmp(t->phase, DIAG_PHASE_TRAP) == 0);
        CHECK(strcmp(t->severity, DIAG_SEVERITY_ERROR) == 0);
        CHECK(strcmp(t->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
        CHECK(t->has_exit_code && t->exit_code == DIAG_TRAP_EXIT_CODE);
        CHECK(!t->has_trap_code);
        CHECK(t->primary_span != NULL && t->primary_span->start.offset == 740);
        CHECK(diag_record_validate(t, err, sizeof(err)));
        diag_buf_init(&b);
        CHECK(diag_emit_record(&b, t));
        CHECK(strstr(b.data, "\"phase\":\"trap\"") != NULL);
        CHECK(strstr(b.data, "\"recovery\":\"authoritative\"") != NULL);
        CHECK(strstr(b.data, "\"exit_code\":70") != NULL);
        diag_buf_free(&b);
        diag_record_free(t);
    }

    /* non-trap code rejected by the trap factory */
    CHECK(diag_trap_record("AIC-L0001", "x", NULL) == NULL);
    CHECK(diag_trap_record("AIC-R9999", "x", NULL) == NULL);
    CHECK(diag_trap_record(NULL, "x", NULL) == NULL);

    t2 = diag_trap_record_ex("AIC-R0803", "division by zero", NULL, 0);
    CHECK(t2 != NULL && t2->exit_code == DIAG_TRAP_EXIT_CODE);
    t3 = diag_trap_record_ex("AIC-R0803", "division by zero", NULL, 99);
    CHECK(t3 != NULL && t3->exit_code == 99);
    diag_record_free(t2);
    diag_record_free(t3);

    u = diag_user_trap_record(7, "user trap", NULL);
    CHECK(u != NULL);
    if (u != NULL) {
        CHECK(strcmp(u->code, "AIC-U0000") == 0);
        CHECK(u->has_trap_code && u->trap_code == 7);
        CHECK(u->has_exit_code && u->exit_code == DIAG_TRAP_EXIT_CODE);
        CHECK(strcmp(u->phase, DIAG_PHASE_TRAP) == 0);
        CHECK(strcmp(u->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
        CHECK(u->primary_span == NULL);
        CHECK(diag_record_validate(u, err, sizeof(err)));
        diag_record_free(u);
    }
    CHECK(diag_user_trap_record(-1, "x", NULL) == NULL);
    CHECK(diag_user_trap_record(4294967296LL, "x", NULL) == NULL);
    u = diag_user_trap_record(4294967295LL, "x", NULL);
    CHECK(u != NULL && u->trap_code == 4294967295LL);
    diag_record_free(u);

    diag_span_free(sp);
}

/* ---------------------------------------------------------------------------
 * Golden fixture byte-comparison
 * ------------------------------------------------------------------------- */

static char *read_file_bin(const char *path, size_t *out_len)
{
    FILE *f;
    long n;
    char *buf;
    size_t rd;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) {
        free(buf);
        return NULL;
    }
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/* Collapse \r\n to \n in place so comparisons survive CRLF checkouts. */
static void normalize_crlf(char *s, size_t *len)
{
    size_t r = 0;
    size_t w = 0;
    while (r < *len) {
        if (s[r] == '\r' && r + 1 < *len && s[r + 1] == '\n') {
            ++r;
            continue;
        }
        s[w++] = s[r++];
    }
    *len = w;
    s[w] = '\0';
}

static const char kGoldenDir[] = "bootstrap/src/diag/golden";

static void golden_path(char *out, size_t outsz, const char *name)
{
    size_t i = 0;
    size_t n = sizeof(kGoldenDir) - 1;
    size_t k;
    for (k = 0; k < n && i + 1 < outsz; ++k) {
        out[i++] = kGoldenDir[k];
    }
    out[i++] = '/';
    for (k = 0; name[k] != '\0' && i + 1 < outsz; ++k) {
        out[i++] = name[k];
    }
    out[i] = '\0';
}

static void check_golden(const char *name, const DiagRecord *rec)
{
    char path[256];
    DiagBuf b;
    size_t glen = 0;
    char *g;

    golden_path(path, sizeof(path), name);
    diag_buf_init(&b);
    ++g_checks;
    if (!diag_emit_record(&b, rec)) {
        ++g_failures;
        printf("FAIL %s:%d: emit failed for golden %s\n", __FILE__, __LINE__, name);
        diag_buf_free(&b);
        return;
    }
    g = read_file_bin(path, &glen);
    ++g_checks;
    if (g == NULL) {
        ++g_failures;
        printf("FAIL %s:%d: cannot read golden fixture %s\n", __FILE__, __LINE__, path);
        diag_buf_free(&b);
        return;
    }
    normalize_crlf(g, &glen);
    ++g_checks;
    if (b.len != glen || memcmp(b.data, g, glen) != 0) {
        ++g_failures;
        printf("FAIL %s:%d: golden mismatch %s\n"
               "  expected(%zu): %s\n"
               "  actual  (%zu): %s\n",
               __FILE__, __LINE__, path, glen, g,
               b.len, b.data == NULL ? "(null)" : b.data);
    }
    free(g);
    diag_buf_free(&b);
}

static void test_golden_examples(void)
{
    DiagRecord *r;
    DiagSpan *s;

    /* Example 1: AIC-L0006 lex literal (contract §12). */
    r = diag_record_new();
    CHECK(r != NULL);
    CHECK(diag_record_set_code(r, "AIC-L0006"));
    CHECK(diag_record_set_message(r, "integer literal 300 is not representable in type u8"));
    s = diag_span_new_range("main.ai", 4, 12, 88, 4, 16, 92);
    CHECK(diag_record_set_primary_span(r, s));
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    CHECK(diag_record_add_related_str(r, "module", "main"));
    check_golden("example-01-lex-l0006.jsonl", r);
    diag_span_free(s);
    diag_record_free(r);

    /* Example 2: AIC-N0203 private access with a secondary span in the
     * declaration file a/b.ai. */
    r = diag_record_new();
    CHECK(diag_record_set_code(r, "AIC-N0203"));
    CHECK(diag_record_set_message(r, "access to private item g in module a.b"));
    s = diag_span_new_range("main.ai", 9, 14, 210, 9, 17, 213);
    CHECK(diag_record_set_primary_span(r, s));
    diag_span_free(s);
    s = diag_span_new_range("a/b.ai", 5, 1, 60, 5, 2, 61);
    CHECK(diag_record_add_secondary_span(r, s));
    diag_span_free(s);
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    CHECK(diag_record_add_related_str(r, "module", "a.b"));
    CHECK(diag_record_add_related_str(r, "declaration", "g"));
    check_golden("example-02-name-n0203.jsonl", r);
    diag_record_free(r);

    /* Example 3: AIC-S0101 expected token with an insertion correction. */
    r = diag_record_new();
    CHECK(diag_record_set_code(r, "AIC-S0101"));
    CHECK(diag_record_set_message(r, "expected ';' after expression"));
    s = diag_span_new_range("main.ai", 7, 19, 120, 7, 20, 121);
    CHECK(diag_record_set_primary_span(r, s));
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    CHECK(diag_record_add_correction(r, ";", NULL));
    check_golden("example-03-syntax-s0101.jsonl", r);
    diag_span_free(s);
    diag_record_free(r);

    /* Example 4: AIC-E0412 switch case terminator. */
    r = diag_record_new();
    CHECK(diag_record_set_code(r, "AIC-E0412"));
    CHECK(diag_record_set_message(r, "switch case 0 body lacks a terminating statement; fall-through is prohibited"));
    s = diag_span_new_range("main.ai", 12, 7, 300, 12, 12, 305);
    CHECK(diag_record_set_primary_span(r, s));
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    check_golden("example-04-semantic-e0412.jsonl", r);
    diag_span_free(s);
    diag_record_free(r);

    /* Example 5: AIC-R0807 runtime trap (contract §10). */
    s = diag_span_new_range("main.ai", 30, 14, 740, 30, 19, 745);
    r = diag_trap_record("AIC-R0807", "slice index 16 out of bounds (len 16)", s);
    diag_span_free(s);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK(diag_record_add_related_str(r, "operation", "index"));
        CHECK(diag_record_add_related_str(r, "type", "u8[]"));
        CHECK(diag_record_add_related_int(r, "index", 16));
        CHECK(diag_record_add_related_int(r, "len", 16));
        check_golden("example-05-trap-r0807.jsonl", r);
    }
    diag_record_free(r);
}

static void test_golden_full_options(void)
{
    DiagRecord *r = diag_record_new();
    DiagSpan *ps;
    DiagSpan *ss;

    CHECK(r != NULL);
    CHECK(diag_record_set_code(r, "AIC-E0404"));
    CHECK(diag_record_set_message(r, "assignment to const"));
    ps = diag_span_new_range("main.ai", 3, 5, 100, 3, 10, 105);
    CHECK(diag_record_set_primary_span(r, ps));
    ss = diag_span_new_point("main.ai", 1, 1, 0);
    CHECK(diag_record_add_secondary_span(r, ss));
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    CHECK(diag_record_add_cause(r, "AIC-E0402", "cause", ps));
    CHECK(diag_record_add_expected_str(r, "type", "bool"));
    CHECK(diag_record_add_actual_str(r, "type", "i32"));
    CHECK(diag_record_add_correction(r, "x", ps));
    CHECK(diag_record_add_related_str(r, "module", "main"));
    CHECK(diag_record_add_related_str(r, "declaration", "c"));
    check_golden("record-full-options.jsonl", r);
    diag_span_free(ps);
    diag_span_free(ss);
    diag_record_free(r);

    r = diag_user_trap_record(7, "user trap", NULL);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK(diag_record_add_related_str(r, "module", "main"));
        check_golden("user-trap-null-span.jsonl", r);
    }
    diag_record_free(r);
}

/* ---------------------------------------------------------------------------
 * JSON escaping / record shape
 * ------------------------------------------------------------------------- */

static void test_escaping(void)
{
    DiagRecord *r = diag_record_new();
    DiagSpan *sp;
    DiagBuf b;
    const char *raw = "q\"w\\e\x01r\x08t\x0cy\x0du\x0ai\x09o";

    CHECK(r != NULL);
    CHECK(diag_record_set_code(r, "AIC-L0006"));
    CHECK(diag_record_set_message(r, raw));
    sp = diag_span_new_point("main.ai", 1, 1, 0);
    CHECK(diag_record_set_primary_span(r, sp));
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    diag_buf_init(&b);
    CHECK(diag_emit_record(&b, r));
    CHECK(diag_buf_ok(&b));
    CHECK(b.len > 0 && b.data[b.len - 1] == '\n');
    /* exactly one LF: the record terminator, no embedded newlines */
    CHECK(line_count(b.data, b.len) == 1);
    CHECK(strstr(b.data, "q\\\"w\\\\e") != NULL);
    CHECK(strstr(b.data, "\\u0001r") != NULL);
    CHECK(strstr(b.data, "\\bt\\fy\\ru\\ni\\to") != NULL);
    diag_buf_free(&b);
    diag_span_free(sp);
    diag_record_free(r);
}

static void test_write_file(void)
{
    DiagRecord *r = diag_record_new();
    DiagBuf b;
    size_t glen = 0;
    char *g;
    FILE *f;

    CHECK(r != NULL);
    CHECK(diag_record_set_code(r, "AIC-L0006"));
    CHECK(diag_record_set_message(r, "write file test"));
    CHECK(diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE));
    diag_buf_init(&b);
    CHECK(diag_emit_record(&b, r));
    /* bootstrap/stage0/ is the gitignored build-output area; the build step
     * that runs this test creates it. */
    f = fopen("bootstrap/stage0/diag-write-test.jsonl", "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(diag_buf_write_file(&b, f));
        fclose(f);
        g = read_file_bin("bootstrap/stage0/diag-write-test.jsonl", &glen);
        CHECK(g != NULL);
        if (g != NULL) {
            normalize_crlf(g, &glen);
            CHECK(glen == b.len && memcmp(g, b.data, glen) == 0);
            free(g);
        }
    }
    diag_buf_free(&b);
    diag_record_free(r);
}

static void test_emit_rejects_invalid(void)
{
    DiagBuf b;
    DiagRecord *bad;

    diag_buf_init(&b);
    bad = diag_record_new();
    CHECK(!diag_emit_record(&b, bad));      /* no code */
    CHECK(b.len == 0);
    diag_record_free(bad);

    bad = diag_record_new();
    CHECK(diag_record_set_code(bad, "AIC-L0006"));
    CHECK(diag_record_set_message(bad, "m"));
    CHECK(!diag_emit_record(&b, bad));      /* error without recovery */
    CHECK(b.len == 0);
    diag_record_free(bad);
    diag_buf_free(&b);
}

/* ---------------------------------------------------------------------------
 * Deterministic ordering (contract §9)
 * ------------------------------------------------------------------------- */

static DiagRecord *mk(const char *code, const char *msg, const char *file,
                      int64_t line, int64_t col, int64_t off)
{
    DiagRecord *r = diag_record_new();
    DiagSpan *s;
    if (r == NULL) {
        return NULL;
    }
    if (!diag_record_set_code(r, code) || !diag_record_set_message(r, msg)) {
        diag_record_free(r);
        return NULL;
    }
    s = diag_span_new_range(file, line, col, off, line, col + 1, off + 1);
    if (s == NULL) {
        diag_record_free(r);
        return NULL;
    }
    if (!diag_record_set_primary_span(r, s) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_span_free(s);
        diag_record_free(r);
        return NULL;
    }
    diag_span_free(s);
    return r;
}

static DiagRecord *mk_null(const char *code, const char *msg)
{
    DiagRecord *r = diag_record_new();
    if (r == NULL) {
        return NULL;
    }
    if (!diag_record_set_code(r, code) || !diag_record_set_message(r, msg) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        return NULL;
    }
    return r;
}

static void test_ordering(void)
{
    DiagRecord *recs[14];
    static const char *const kExpected[14] = {
        "AIC-L0006", "AIC-S0101", "AIC-N0202", "AIC-T0307",
        "AIC-E0409", "AIC-E0410", "AIC-E0404",
        "AIC-I0501", "AIC-B0601",
        "AIC-O0701", "AIC-O0701", "AIC-O0702",
        "AIC-BL0801", "AIC-BL0803"
    };
    size_t i;
    DiagBuf b;
    char line[512];
    char code[64];

    /* Deliberately scrambled input; the comparator must produce the §9
     * order: phase, then null-span-before-file-bearing, then file, then
     * offset, then code. */
    recs[0] = mk_null("AIC-BL0801", "build manifest");
    recs[1] = mk("AIC-L0006", "lex literal", "main.ai", 1, 1, 10);
    recs[2] = mk("AIC-E0410", "semantic cast", "c.ai", 1, 1, 200);
    recs[3] = mk("AIC-O0701", "object failure 2", "z.ai", 1, 1, 5);
    recs[4] = mk("AIC-S0101", "syntax expected", "main.ai", 1, 1, 20);
    recs[5] = mk("AIC-E0404", "semantic assign", "main.ai", 1, 1, 50);
    recs[6] = mk("AIC-I0501", "ir invariant", "main.ai", 1, 1, 60);
    recs[7] = mk_null("AIC-O0701", "object failure");
    recs[8] = mk("AIC-N0202", "name undeclared", "main.ai", 1, 1, 30);
    recs[9] = mk("AIC-T0307", "type mismatch", "main.ai", 1, 1, 40);
    recs[10] = mk("AIC-E0409", "semantic shift", "c.ai", 1, 1, 200);
    recs[11] = mk("AIC-B0601", "backend constraint", "main.ai", 1, 1, 70);
    recs[12] = mk_null("AIC-BL0803", "build entry");
    recs[13] = mk("AIC-O0702", "link failure", "main.ai", 1, 1, 90);
    for (i = 0; i < 14; ++i) {
        CHECK(recs[i] != NULL);
    }

    diag_sort_records(recs, 14);
    for (i = 0; i < 14; ++i) {
        CHECK_MSG(strcmp(recs[i]->code, kExpected[i]) == 0,
                  "sorted position %zu: got %s expected %s",
                  i, recs[i]->code, kExpected[i]);
    }
    /* null-span O0701 sorts before the file-bearing O0701 in the object phase */
    CHECK(recs[9]->primary_span == NULL);
    CHECK(recs[10]->primary_span != NULL);

    /* the sorted batch emits in the same order, one JSONL line per record */
    diag_buf_init(&b);
    CHECK(diag_emit_records_sorted(&b, recs, 14));
    CHECK(line_count(b.data, b.len) == 14);
    for (i = 0; i < 14; ++i) {
        line_at(b.data, b.len, i, line, sizeof(line));
        line_code(line, code, sizeof(code));
        CHECK_MSG(strcmp(code, kExpected[i]) == 0,
                  "emitted line %zu: got %s expected %s", i, code, kExpected[i]);
    }
    diag_buf_free(&b);

    /* direct comparator checks */
    {
        DiagRecord *bn = mk_null("AIC-BL0801", "build manifest");
        DiagRecord *bf = mk("AIC-BL0803", "file-bearing build", "x.ai", 1, 1, 3);
        CHECK(diag_record_compare(&bn, &bf) < 0);  /* null-span before file-bearing */
        CHECK(diag_record_compare(&bf, &bn) > 0);
        diag_record_free(bn);
        diag_record_free(bf);
    }
    {
        DiagRecord *c1 = mk("AIC-E0409", "a", "t.ai", 1, 1, 5);
        DiagRecord *c2 = mk("AIC-E0410", "b", "t.ai", 1, 1, 5);
        CHECK(diag_record_compare(&c1, &c2) < 0);  /* same file+offset: tie by code */
        CHECK(diag_record_compare(&c2, &c1) > 0);
        diag_record_free(c1);
        diag_record_free(c2);
    }
    {
        DiagRecord *n1 = mk_null("AIC-BL0803", "x");
        DiagRecord *n2 = mk_null("AIC-BL0801", "y");
        CHECK(diag_record_compare(&n1, &n2) > 0);  /* null-span tie by code */
        diag_record_free(n1);
        diag_record_free(n2);
    }
    {
        DiagRecord *ph_l = mk("AIC-L0006", "", "f.ai", 1, 1, 1);
        DiagRecord *ph_b = mk_null("AIC-BL0801", "");
        DiagRecord *ph_o = mk("AIC-O0702", "", "f.ai", 1, 1, 1);
        CHECK(diag_record_compare(&ph_l, &ph_b) < 0);  /* lex before build */
        CHECK(diag_record_compare(&ph_o, &ph_b) < 0);  /* link before build */
        CHECK(diag_record_compare(&ph_b, &ph_l) > 0);
        diag_record_free(ph_l);
        diag_record_free(ph_b);
        diag_record_free(ph_o);
    }

    for (i = 0; i < 14; ++i) {
        diag_record_free(recs[i]);
    }
}

int main(void)
{
    test_registry();
    test_spans();
    test_record_validation();
    test_trap_factories();
    test_golden_examples();
    test_golden_full_options();
    test_escaping();
    test_write_file();
    test_emit_rejects_invalid();
    test_ordering();

    printf("diag_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
