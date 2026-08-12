/* bootstrap/src/sema/decl_init_test.c
 *
 * WP-M0-13a2 initialization semantics tests: the missing-initializer
 * rule of spec sec. 8.2 / sec. 9 (AIC-E0403) at every variable
 * declaration form (local var, global var, `for` init var), the const
 * exemption boundary (consts keep the strict required-initializer
 * grammar; missing "=" is a parser AIC-S0101 and is NOT a semantic
 * record), the deterministic record output, and exact re-execution of
 * the negative-corpus anchor owned by WP-M0-03
 * (tests/negative/cases/derived-semantic-missing-init; read-only).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-sema-a2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/decl_init_test.c \
 *     bootstrap/src/sema/decl_init.c bootstrap/src/name/name.c \
 *     bootstrap/src/ast/ast.c bootstrap/src/parse/parse.c \
 *     bootstrap/src/lex/lex.c bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c bootstrap/src/types/type_tables.c
 *   ./bootstrap/stage0/msvc-sema-a2/decl_init_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-sema-a2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "decl_init.h"

#include "../parse/parse.h"
#include "../types/type_tables.h"

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

/* ---------------------------------------------------------------------------
 * Shared pipeline: bytes -> load -> lex -> parse -> name_resolve ->
 * declinit_check. decl_init consumes only the resolved build (name
 * tables + AST); the completeness/layout/convert/optype/const stages
 * are not needed for the E0403 check (documented boundary).
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    NameResult *result;
    DiagRecord **nrecs;     /* name-phase records */
    size_t nrn;
    NameStatus st;
    DiagRecord **irecs;     /* declinit records */
    size_t irn;
    DeclInitStatus ist;
} Pipeline;

static void pipeline_run_mem(Pipeline *p, const char *src_text)
{
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;

    memset(p, 0, sizeof(*p));
    ld = load_source_from_bytes("input.ai", (const uint8_t *)src_text,
                                strlen(src_text), &p->src, &p->nrecs, &p->nrn);
    CHECK(ld == LOAD_OK);
    if (ld != LOAD_OK) return;
    lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->nrecs, &p->nrn);
    CHECK(lx == LEX_OK);
    if (lx != LEX_OK) return;
    ps = parse_program(p->toks, p->tn, &p->program, &p->nrecs, &p->nrn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
                         &p->result, &p->nrecs, &p->nrn);
    if (p->st != NAME_OK) return;
    p->ist = declinit_check(p->result, &p->irecs, &p->irn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->nrecs, p->nrn);
    types_records_free(p->irecs, p->irn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* ---------------------------------------------------------------------------
 * Record inspection helpers
 * ------------------------------------------------------------------------- */

static const DiagRecord *find_record(const Pipeline *p, const char *code)
{
    size_t i;
    for (i = 0; i < p->irn; i++) {
        if (p->irecs[i] && p->irecs[i]->code &&
            strcmp(p->irecs[i]->code, code) == 0) return p->irecs[i];
    }
    return NULL;
}

static void check_record_shape(const DiagRecord *r, const char *code,
                               const char *message)
{
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->code && strcmp(r->code, code) == 0);
    CHECK(r->severity && strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(r->phase && strcmp(r->phase, DIAG_PHASE_SEMANTIC) == 0);
    CHECK(r->recovery &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    CHECK(r->message && strcmp(r->message, message) == 0);
    CHECK(r->primary_span != NULL);
}

/* Line/col of a byte offset inside `src` (LF-normalized, 1-based
 * line/col, 0-based offset - the DIAGNOSTIC-CONTRACT sec. 6 model). */
static void line_col(const char *src, int64_t offset,
                     int64_t *line, int64_t *col)
{
    int64_t l = 1, ls = 0, i;
    for (i = 0; i < offset && src[i] != '\0'; i++) {
        if (src[i] == '\n') { l++; ls = i + 1; }
    }
    *line = l;
    *col = offset - ls + 1;
}

/* Check that the record's primary span covers exactly [marker_start,
 * marker_start + len) inside `src` (offsets and line/col). */
static void check_record_span(const DiagRecord *r, const char *src,
                              const char *marker, size_t len)
{
    const char *hit;
    int64_t off, el, ec;
    CHECK(r != NULL && r->primary_span != NULL);
    if (!r || !r->primary_span) return;
    hit = strstr(src, marker);
    CHECK(hit != NULL);
    if (!hit) return;
    off = (int64_t)(hit - src);
    line_col(src, off, &el, &ec);
    CHECK(r->primary_span->start.line == el);
    CHECK(r->primary_span->start.col == ec);
    CHECK(r->primary_span->start.offset == off);
    line_col(src, off + (int64_t)len, &el, &ec);
    CHECK(r->primary_span->end.line == el);
    CHECK(r->primary_span->end.col == ec);
    CHECK(r->primary_span->end.offset == off + (int64_t)len);
}

/* ---------------------------------------------------------------------------
 * 1. AIC-E0403 local var declaration without initializer (sec. 8.2)
 * ------------------------------------------------------------------------- */

static void test_local_var_missing_init(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32;\n"
        "  return x;\n"
        "}\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_DIAG_ERROR);
    CHECK(p.irn == 1);
    r = find_record(&p, "AIC-E0403");
    check_record_shape(r, "AIC-E0403",
                       "missing initializer on variable declaration");
    /* corpus-pinned span: the whole declaration `var x: i32;` */
    check_record_span(r, src, "var x: i32;", 11);
    pipeline_free(&p);
}

static void test_local_var_missing_init_nested(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  if (true) {\n"
        "    var a: i32;\n"
        "  }\n"
        "  while (false) {\n"
        "    var b: i32;\n"
        "  }\n"
        "  switch (1) {\n"
        "    case 1: { var c: i32; break; }\n"
        "    default: { var d: i32; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_DIAG_ERROR);
    CHECK(p.irn == 4);
    CHECK(find_record(&p, "AIC-E0403") != NULL);
    /* each nested declaration is reported exactly once, its own span */
    {
        const char *markers[4] = { "var a: i32;", "var b: i32;",
                                   "var c: i32;", "var d: i32;" };
        size_t i;
        for (i = 0; i < 4; i++) {
            size_t k;
            int found = 0;
            for (k = 0; k < p.irn; k++) {
                const DiagRecord *rec = p.irecs[k];
                if (rec && rec->primary_span) {
                    const char *hit = strstr(src, markers[i]);
                    if (hit && rec->primary_span->start.offset ==
                                   (int64_t)(hit - src)) {
                        found = 1;
                        check_record_span(rec, src, markers[i],
                                          strlen(markers[i]));
                        break;
                    }
                }
            }
            CHECK(found);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 2. AIC-E0403 global var declaration without initializer (sec. 8.2)
 * ------------------------------------------------------------------------- */

static void test_global_var_missing_init(void)
{
    static const char src[] =
        "module main;\n"
        "var g: i32;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_DIAG_ERROR);
    CHECK(p.irn == 1);
    r = find_record(&p, "AIC-E0403");
    check_record_shape(r, "AIC-E0403",
                       "missing initializer on variable declaration");
    check_record_span(r, src, "var g: i32;", 11);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. AIC-E0403 for-init var declaration without initializer (sec. 13.3)
 * ------------------------------------------------------------------------- */

static void test_for_init_var_missing_init(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var acc: i32 = 0;\n"
        "  for (var i: i32; i < 3; i += 1) { acc = acc + i; }\n"
        "  return acc;\n"
        "}\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_DIAG_ERROR);
    CHECK(p.irn == 1);
    r = find_record(&p, "AIC-E0403");
    check_record_shape(r, "AIC-E0403",
                       "missing initializer on variable declaration");
    /* the for-init declaration span is exactly `var i: i32;` */
    check_record_span(r, src, "var i: i32;", 11);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. Multiple sites: deterministic count and span order
 * ------------------------------------------------------------------------- */

static void test_multi_site_ordering(void)
{
    static const char src[] =
        "module main;\n"
        "var g1: i32;\n"
        "var g2: i32 = 1;\n"
        "fn main() -> i32 {\n"
        "  var a: i32;\n"
        "  var b: i32 = 2;\n"
        "  for (var i: i32; i < 3; i += 1) { }\n"
        "  var c: i32;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_DIAG_ERROR);
    CHECK(p.irn == 4);
    /* exactly the three missing-init sites plus the for-init */
    CHECK(find_record(&p, "AIC-E0403") != NULL);
    /* span offsets must be non-decreasing (contract sec. 9 order) */
    for (i = 1; i < p.irn; i++) {
        CHECK(p.irecs[i - 1]->primary_span->start.offset <=
              p.irecs[i]->primary_span->start.offset);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. Valid initializers: every var form supplies its initializer
 * ------------------------------------------------------------------------- */

static void test_valid_initializers(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "var g: i32 = 0;\n"
        "var gp: Point = Point { x: 1, y: 2 };\n"
        "const GC: i32 = 5;\n"
        "fn f(p: Point) -> i32 {\n"
        "  var v: i32 = 1;\n"
        "  var arr: i32[3] = [1, 2, 3];\n"
        "  var sl: i32[] = arr[..];\n"
        "  const LC: i32 = 2;\n"
        "  for (var i: i32 = 0; i < 3; i += 1) { v = v + i; }\n"
        "  return v;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_OK);
    CHECK(p.irn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. const boundary: const declarations are not variables; missing "="
 *    is a parser AIC-S0101, never a semantic record here
 * ------------------------------------------------------------------------- */

static void test_const_sites_boundary(void)
{
    static const char src[] =
        "module main;\n"
        "const GC: i32 = 5;\n"
        "fn main() -> i32 {\n"
        "  const LC: i32 = 2;\n"
        "  for (const C: i32 = 3; C < 3; C += 1) { }\n"
        "  return LC;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_OK);
    CHECK(p.irn == 0);
    /* a const without "=" is rejected by the parser (S0101) and is not
     * decl_init's contract; the parser owns that strictness. */
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 7. Deterministic record output (contract sec. 9 / sec. 14.2)
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "var g1: i32;\n"
        "fn f() -> i32 {\n"
        "  var a: i32;\n"
        "  var b: i32;\n"
        "  return 0;\n"
        "}\n"
        "fn main() -> i32 {\n"
        "  var c: i32;\n"
        "  for (var i: i32; i < 3; i += 1) { }\n"
        "  return 0;\n"
        "}\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    CHECK(p1.ist == p2.ist);
    CHECK(p1.irn == p2.irn);
    CHECK(p1.irn == 5);
    if (p1.irn != p2.irn || p1.irn != 5) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.irn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.irecs[i]));
        CHECK(diag_emit_record(&b2, p2.irecs[i]));
        CHECK(b1.len == b2.len);
        CHECK(b1.len == 0 || memcmp(b1.data, b2.data, b1.len) == 0);
        diag_buf_free(&b1);
        diag_buf_free(&b2);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* ---------------------------------------------------------------------------
 * 8. Negative-corpus anchor (exact record; fixture read-only)
 * ------------------------------------------------------------------------- */

static char *read_file_bytes(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return buf;
}

/* The anchor expects exactly one E0403 record with the whole-declaration
 * span `var x: i32;` (line 3 col 3 offset 34 -> col 14 offset 45). The
 * input's `return x;` keeps the name stage clean; ast_dump is NOT used
 * (parse README design decision 11: it does not render a no-init decl). */
static void test_corpus_anchor_missing_init(void)
{
    static const char path[] =
        "tests/negative/cases/derived-semantic-missing-init/input.ai";
    char *src;
    size_t srclen;
    Pipeline p;
    const DiagRecord *r;

    src = read_file_bytes(path, &srclen);
    CHECK(src != NULL);
    if (!src) return;
    pipeline_run_mem(&p, src);
    free(src);
    CHECK(p.st == NAME_OK);
    CHECK(p.ist == DECLINIT_DIAG_ERROR);
    CHECK(p.irn == 1);
    if (p.irn == 1) {
        r = p.irecs[0];
        check_record_shape(r, "AIC-E0403",
                           "missing initializer on variable declaration");
        if (r && r->primary_span) {
            CHECK(r->primary_span->start.line == 3);
            CHECK(r->primary_span->start.col == 3);
            CHECK(r->primary_span->start.offset == 34);
            CHECK(r->primary_span->end.line == 3);
            CHECK(r->primary_span->end.col == 14);
            CHECK(r->primary_span->end.offset == 45);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_local_var_missing_init();
    fprintf(stderr, "after test_local_var_missing_init\n");
    test_local_var_missing_init_nested();
    fprintf(stderr, "after test_local_var_missing_init_nested\n");
    test_global_var_missing_init();
    fprintf(stderr, "after test_global_var_missing_init\n");
    test_for_init_var_missing_init();
    fprintf(stderr, "after test_for_init_var_missing_init\n");
    test_multi_site_ordering();
    fprintf(stderr, "after test_multi_site_ordering\n");
    test_valid_initializers();
    fprintf(stderr, "after test_valid_initializers\n");
    test_const_sites_boundary();
    fprintf(stderr, "after test_const_sites_boundary\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_corpus_anchor_missing_init();
    fprintf(stderr, "after test_corpus_anchor_missing_init\n");

    if (g_failures) {
        fprintf(stderr, "decl_init_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("decl_init_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
