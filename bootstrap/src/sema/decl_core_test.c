/* bootstrap/src/sema/decl_core_test.c
 *
 * WP-M0-13a1 declaration model and assignability tests: the
 * declaration model (storage duration, mutability), the lvalue/
 * assignability checks of spec sec. 8.4 and sec. 12.5 (AIC-E0402 /
 * AIC-E0404 / AIC-E0419), the deterministic record output, and exact
 * re-execution of the three negative-corpus anchors owned by WP-M0-03
 * (tests/negative/cases/derived-semantic-addr-of-const,
 * derived-semantic-assign-to-const, derived-semantic-assign-non-lvalue;
 * read-only).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-sema-a1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/decl_core_test.c \
 *     bootstrap/src/sema/decl_core.c bootstrap/src/name/name.c \
 *     bootstrap/src/ast/ast.c bootstrap/src/parse/parse.c \
 *     bootstrap/src/lex/lex.c bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c bootstrap/src/types/type_tables.c
 *   ./bootstrap/stage0/msvc-sema-a1/decl_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-sema-a1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "decl_core.h"

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
 * decl_check. decl_core consumes only the resolved build (name tables +
 * AST); the completeness/layout/convert/optype/const stages are not
 * needed for the E0402/E0404/E0419 checks (documented boundary).
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
    DiagRecord **drecs;     /* decl-check records */
    size_t drn;
    DeclStatus dst;
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
    p->dst = decl_check(p->result, &p->drecs, &p->drn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->nrecs, p->nrn);
    types_records_free(p->drecs, p->drn);
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
    for (i = 0; i < p->drn; i++) {
        if (p->drecs[i] && p->drecs[i]->code &&
            strcmp(p->drecs[i]->code, code) == 0) return p->drecs[i];
    }
    return NULL;
}

static const DiagRecord *find_record_msg(const Pipeline *p, const char *code,
                                         const char *message)
{
    size_t i;
    for (i = 0; i < p->drn; i++) {
        if (p->drecs[i] && p->drecs[i]->code && p->drecs[i]->message &&
            strcmp(p->drecs[i]->code, code) == 0 &&
            strcmp(p->drecs[i]->message, message) == 0) return p->drecs[i];
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
 * 1. Declaration model: storage duration and mutability (sec. 8.3/8.4)
 * ------------------------------------------------------------------------- */

static void test_storage_mutability_model(void)
{
    NameSymbol s;
    memset(&s, 0, sizeof(s));

    s.kind = NAME_SYM_GLOBAL_VAR;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_STATIC);
    CHECK(decl_mutability_of_symbol(&s) == DECL_MUTABLE);

    s.kind = NAME_SYM_LOCAL_VAR;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_AUTOMATIC);
    CHECK(decl_mutability_of_symbol(&s) == DECL_MUTABLE);

    s.kind = NAME_SYM_PARAM;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_AUTOMATIC);
    CHECK(decl_mutability_of_symbol(&s) == DECL_MUTABLE);

    s.kind = NAME_SYM_GLOBAL_CONST;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    CHECK(decl_mutability_of_symbol(&s) == DECL_IMMUTABLE);

    s.kind = NAME_SYM_LOCAL_CONST;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    CHECK(decl_mutability_of_symbol(&s) == DECL_IMMUTABLE);

    s.kind = NAME_SYM_FIELD;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    CHECK(decl_mutability_of_symbol(&s) == DECL_MUTABLE);

    s.kind = NAME_SYM_ENUM_MEMBER;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    CHECK(decl_mutability_of_symbol(&s) == DECL_IMMUTABLE);

    s.kind = NAME_SYM_FN;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    s.kind = NAME_SYM_STRUCT;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    s.kind = NAME_SYM_ENUM;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);
    s.kind = NAME_SYM_MODULE_IMPORT;
    CHECK(decl_storage_of_symbol(&s) == DECL_STORAGE_NONE);

    CHECK(decl_storage_of_symbol(NULL) == DECL_STORAGE_NONE);
    CHECK(decl_mutability_of_symbol(NULL) == DECL_IMMUTABLE);
}

/* ---------------------------------------------------------------------------
 * 2. AIC-E0404 assignment to const (sec. 8.4)
 * ------------------------------------------------------------------------- */

static void test_assign_to_const_local(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  const X: i32 = 5;\n"
        "  X = 10;\n"
        "  return X;\n"
        "}\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 1);
    r = find_record(&p, "AIC-E0404");
    check_record_shape(r, "AIC-E0404", "assignment to const 'X'");
    /* corpus-pinned span: the const's declaration identifier `X` */
    check_record_span(r, src, "X", 1);
    pipeline_free(&p);
}

static void test_assign_to_global_const_and_compound(void)
{
    static const char src[] =
        "module main;\n"
        "const G: i32 = 5;\n"
        "fn main() -> i32 {\n"
        "  G = 1;\n"
        "  G += 2;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 2);
    /* both records are E0404 on the same const; message repeats */
    CHECK(find_record(&p, "AIC-E0404") != NULL);
    CHECK(find_record(&p, "AIC-E0419") == NULL);
    pipeline_free(&p);
}

static void test_assign_to_const_derived(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "enum Color: u8 { Red, Green, Blue }\n"
        "fn main() -> i32 {\n"
        "  const PP: Point = Point { x: 1, y: 2 };\n"
        "  const AA: i32[2] = [1, 2];\n"
        "  PP.x = 5;\n"
        "  AA[0] = 5;\n"
        "  Color.Red = 5;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 3);
    /* records are span-sorted: Color.Red's member decl precedes PP and AA,
     * so find the const-struct record by message */
    r = find_record_msg(&p, "AIC-E0404", "assignment to const 'PP'");
    check_record_shape(r, "AIC-E0404", "assignment to const 'PP'");
    /* the const's declaration identifier is the span, not the target */
    check_record_span(r, src, "PP", 2);
    CHECK(find_record_msg(&p, "AIC-E0404", "assignment to const 'AA'") != NULL);
    CHECK(find_record_msg(&p, "AIC-E0404", "assignment to const 'Red'") != NULL);
    CHECK(find_record(&p, "AIC-E0419") == NULL);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. AIC-E0419 assignment to non-lvalue (sec. 8.4)
 * ------------------------------------------------------------------------- */

static void test_assign_non_lvalue(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> i32 { return 1; }\n"
        "fn main() -> i32 {\n"
        "  5 = 10;\n"
        "  var a: i32 = 1;\n"
        "  var b: i32 = 2;\n"
        "  (a + b) = 3;\n"
        "  f() = 4;\n"
        "  (a = 1) = 2;\n"
        "  return a;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 4);
    CHECK(find_record(&p, "AIC-E0404") == NULL);
    CHECK(find_record(&p, "AIC-E0419") != NULL);
    /* the nested assignment target `a = 1` inside `(a = 1) = 2` is a
     * valid mutable assignment; only the outer target is a non-lvalue */
    pipeline_free(&p);
}

static void test_assign_non_lvalue_span(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  5 = 10;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 1);
    r = find_record(&p, "AIC-E0419");
    check_record_shape(r, "AIC-E0419",
                       "assignment target is not a modifiable lvalue");
    /* corpus-pinned span: the assignment target expression `5` */
    check_record_span(r, src, "5", 1);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. Valid mutable assignments (no records)
 * ------------------------------------------------------------------------- */

static void test_valid_assignments(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "var g: i32 = 0;\n"
        "fn set(p: Point) -> i32 {\n"
        "  var v: i32 = 1;\n"
        "  var arr: i32[3] = [1, 2, 3];\n"
        "  var sl: i32[] = arr[..];\n"
        "  var ps: Point* = &p;\n"
        "  v = 2;\n"
        "  p = Point { x: 3, y: 4 };\n"
        "  g = 5;\n"
        "  arr[1] = 6;\n"
        "  sl[0] = 7;\n"
        "  p.x = 8;\n"
        "  ps->y = 9;\n"
        "  *(&v) = 10;\n"
        "  (v) = 11;\n"
        "  return v;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_OK);
    CHECK(p.drn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. AIC-E0402 address of const / non-lvalue (sec. 8.1, sec. 12.5)
 * ------------------------------------------------------------------------- */

static void test_addr_of_const(void)
{
    static const char src[] =
        "module main;\n"
        "import rt.mem;\n"
        "fn main() -> i32 {\n"
        "  const X: i32 = 5;\n"
        "  var p: i32* = &X;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    const DiagRecord *r;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 1);
    r = find_record(&p, "AIC-E0402");
    check_record_shape(r, "AIC-E0402", "address of const is not allowed");
    /* corpus-pinned span: the whole address-of expression `&X` */
    check_record_span(r, src, "&X", 2);
    pipeline_free(&p);
}

static void test_addr_of_non_lvalue(void)
{
    static const char src[] =
        "module main;\n"
        "enum Color: u8 { Red, Green, Blue }\n"
        "fn main() -> i32 {\n"
        "  var a: i32 = 1;\n"
        "  var b: i32 = 2;\n"
        "  var p: i32* = &(a + b);\n"
        "  var q: i32* = &(5);\n"
        "  var r: u8* = &Color.Red;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 3);
    CHECK(find_record(&p, "AIC-E0402") != NULL);
    /* enum members are constants: address-of is the const flavor */
    pipeline_free(&p);
}

static void test_addr_of_valid(void)
{
    static const char src[] =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "var g: i32 = 0;\n"
        "const PG: i32* = &g;\n"
        "fn main() -> i32 {\n"
        "  var v: i32 = 1;\n"
        "  var arr: i32[3] = [1, 2, 3];\n"
        "  var s: Point = Point { x: 1, y: 2 };\n"
        "  var p: i32* = &v;\n"
        "  var q: i32* = &arr[0];\n"
        "  var r: i32* = &s.x;\n"
        "  var t: i32* = &*(&v);\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_OK);
    CHECK(p.drn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. str element access is a value, not an lvalue (sec. 12.2)
 * ------------------------------------------------------------------------- */

static void test_str_index_not_lvalue(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var s: str = \"abc\";\n"
        "  s[0] = 65u8;\n"
        "  \"abc\"[0] = 65u8;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 2);
    CHECK(find_record(&p, "AIC-E0419") != NULL);
    CHECK(find_record(&p, "AIC-E0404") == NULL);
    pipeline_free(&p);
}

static void test_str_array_element_assign_ok(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var a: str[2] = [\"x\", \"y\"];\n"
        "  a[0] = \"z\";\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_OK);
    CHECK(p.drn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 7. Nested sites: statements, control flow, globals, enum members
 * ------------------------------------------------------------------------- */

static void test_nested_sites(void)
{
    static const char src[] =
        "module main;\n"
        "const BAD: i32 = 5;\n"
        "enum E: i32 { A = 0, B = 1 }\n"
        "const P: i32* = &BAD;\n"
        "fn main() -> i32 {\n"
        "  var acc: i32 = 0;\n"
        "  if (true) { acc = 1; }\n"
        "  while (acc < 3) { acc += 1; }\n"
        "  for (var i: i32 = 0; i < 3; i += 1) { acc = acc + i; }\n"
        "  switch (acc) {\n"
        "    case 0: { acc = 5; break; }\n"
        "    default: { acc = 6; }\n"
        "  }\n"
        "  acc = acc > 0 ? acc : (acc = 9);\n"
        "  return acc;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    /* E0402 from the global initializer `&BAD`; the body assignments
     * are all to a mutable local and produce no records. */
    CHECK(p.drn == 1);
    CHECK(find_record(&p, "AIC-E0402") != NULL);
    CHECK(find_record(&p, "AIC-E0404") == NULL);
    CHECK(find_record(&p, "AIC-E0419") == NULL);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 8. Deterministic ordering (contract sec. 9: span order)
 * ------------------------------------------------------------------------- */

static void test_ordering(void)
{
    static const char src[] =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  const X: i32 = 1;\n"
        "  const Y: i32 = 2;\n"
        "  5 = 1;\n"
        "  X = 2;\n"
        "  6 = 3;\n"
        "  Y = 4;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    /* E0404 spans point at the const declaration identifiers (X, Y),
     * which precede the bad statements in source; E0419 spans point at
     * the targets. Sorted by span offset: X-decl, Y-decl, 5, 6. */
    const char *want_codes[4] = { "AIC-E0404", "AIC-E0404", "AIC-E0419",
                                  "AIC-E0419" };
    size_t i;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.dst == DECL_DIAG_ERROR);
    CHECK(p.drn == 4);
    for (i = 0; i < p.drn && i < 4; i++) {
        CHECK(p.drecs[i] && p.drecs[i]->code &&
              strcmp(p.drecs[i]->code, want_codes[i]) == 0);
    }
    /* span offsets must be non-decreasing */
    for (i = 1; i < p.drn; i++) {
        CHECK(p.drecs[i - 1]->primary_span->start.offset <=
              p.drecs[i]->primary_span->start.offset);
    }
    pipeline_free(&p);
}

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "const A: i32 = 1;\n"
        "fn f() -> i32 { return 1; }\n"
        "fn main() -> i32 {\n"
        "  const B: i32 = 2;\n"
        "  var p: i32* = &A;\n"
        "  B = 3;\n"
        "  f() = 4;\n"
        "  5 = 6;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    CHECK(p1.dst == p2.dst);
    CHECK(p1.drn == p2.drn);
    CHECK(p1.drn == 4);
    if (p1.drn != p2.drn || p1.drn != 4) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.drn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.drecs[i]));
        CHECK(diag_emit_record(&b2, p2.drecs[i]));
        CHECK(b1.len == b2.len);
        CHECK(b1.len == 0 || memcmp(b1.data, b2.data, b1.len) == 0);
        diag_buf_free(&b1);
        diag_buf_free(&b2);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* ---------------------------------------------------------------------------
 * 9. Negative-corpus anchors (exact records; fixtures read-only)
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

typedef struct AnchorExpect {
    const char *dir;     /* tests/negative/cases/<dir>/ */
    const char *code;
    const char *message;
    int64_t sl, sc, so, el, ec, eo;
} AnchorExpect;

static const AnchorExpect kAnchors[] = {
    { "derived-semantic-addr-of-const", "AIC-E0402",
      "address of const is not allowed",
      5, 17, 83, 5, 19, 85 },
    { "derived-semantic-assign-to-const", "AIC-E0404",
      "assignment to const 'X'",
      3, 9, 40, 3, 10, 41 },
    { "derived-semantic-assign-non-lvalue", "AIC-E0419",
      "assignment target is not a modifiable lvalue",
      3, 3, 34, 3, 4, 35 },
};

static void test_corpus_anchors(void)
{
    size_t k;
    for (k = 0; k < sizeof(kAnchors) / sizeof(kAnchors[0]); k++) {
        const AnchorExpect *a = &kAnchors[k];
        char path[256];
        char *src;
        size_t srclen;
        Pipeline p;

        snprintf(path, sizeof(path), "tests/negative/cases/%s/input.ai",
                 a->dir);
        src = read_file_bytes(path, &srclen);
        CHECK(src != NULL);
        if (!src) continue;
        pipeline_run_mem(&p, src);
        free(src);
        CHECK(p.st == NAME_OK);
        CHECK(p.dst == DECL_DIAG_ERROR);
        CHECK(p.drn == 1);
        if (p.drn == 1) {
            const DiagRecord *r = p.drecs[0];
            check_record_shape(r, a->code, a->message);
            if (r && r->primary_span) {
                CHECK(r->primary_span->start.line == a->sl);
                CHECK(r->primary_span->start.col == a->sc);
                CHECK(r->primary_span->start.offset == a->so);
                CHECK(r->primary_span->end.line == a->el);
                CHECK(r->primary_span->end.col == a->ec);
                CHECK(r->primary_span->end.offset == a->eo);
            }
        }
        pipeline_free(&p);
    }
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_storage_mutability_model();
    fprintf(stderr, "after test_storage_mutability_model\n");
    test_assign_to_const_local();
    fprintf(stderr, "after test_assign_to_const_local\n");
    test_assign_to_global_const_and_compound();
    fprintf(stderr, "after test_assign_to_global_const_and_compound\n");
    test_assign_to_const_derived();
    fprintf(stderr, "after test_assign_to_const_derived\n");
    test_assign_non_lvalue();
    fprintf(stderr, "after test_assign_non_lvalue\n");
    test_assign_non_lvalue_span();
    fprintf(stderr, "after test_assign_non_lvalue_span\n");
    test_valid_assignments();
    fprintf(stderr, "after test_valid_assignments\n");
    test_addr_of_const();
    fprintf(stderr, "after test_addr_of_const\n");
    test_addr_of_non_lvalue();
    fprintf(stderr, "after test_addr_of_non_lvalue\n");
    test_addr_of_valid();
    fprintf(stderr, "after test_addr_of_valid\n");
    test_str_index_not_lvalue();
    fprintf(stderr, "after test_str_index_not_lvalue\n");
    test_str_array_element_assign_ok();
    fprintf(stderr, "after test_str_array_element_assign_ok\n");
    test_nested_sites();
    fprintf(stderr, "after test_nested_sites\n");
    test_ordering();
    fprintf(stderr, "after test_ordering\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_corpus_anchors();
    fprintf(stderr, "after test_corpus_anchors\n");

    if (g_failures) {
        fprintf(stderr, "decl_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("decl_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
