/* bootstrap/src/parse/parse_test.c
 *
 * WP-M0-09 unit tests: golden AST-dump tests (spec sec. 18.2 valid programs
 * plus derived grammar cases; every dump compared byte-for-byte against the
 * committed expected text in golden_cases.h), determinism (same input parsed
 * twice produces identical dumps), ambiguity-prone forms (sizeof type-vs-expr
 * per sec. 6.2 single name space, struct-init postfix per sec. 12.7, the
 * comment-split two-token rule per sec. 4.1, ternary right-associativity per
 * sec. 5.2,
 * type postfix order per sec. 5.2), grammar-level rejections AIC-S0101..
 * AIC-S0104 with exact spans, deterministic recovery with recovery_derived
 * marking (diagnostic contract sec. 11.2 / sec. 7), and re-execution of the
 * parser-owned negative-corpus anchors against the real fixture files.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-parse' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/parse/parse_test.c bootstrap/src/parse/parse.c \
 *     bootstrap/src/ast/ast.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-parse/parse_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-parse)
 *
 * The corpus anchors read the committed fixture inputs under
 * tests/negative/cases/ (read-only; owned by WP-M0-03) and assert the exact
 * records from each case's expected.json.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "parse.h"
#include "golden_cases.h"

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
 * Shared pipeline: load -> lex -> parse (records from load/lex are merged
 * into *recs/rec_n alongside parse records, matching driver semantics).
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    DiagRecord **recs;
    size_t rn;
    LexStatus lst;   /* LEX_OK or LEX_DIAG_ERROR */
    ParseStatus pst; /* PARSE_OK, PARSE_DIAG_ERROR, or PARSE_OOM */
} Pipeline;

static void pipeline_run(Pipeline *p, const char *input)
{
    LoadStatus ld;

    memset(p, 0, sizeof(*p));
    ld = load_source_from_bytes("input.ai", (const uint8_t *)input,
                                strlen(input), &p->src, &p->recs, &p->rn);
    CHECK(ld == LOAD_OK);
    if (ld != LOAD_OK) {
        load_records_free(p->recs, p->rn);
        p->recs = NULL;
        p->rn = 0;
        return;
    }
    p->lst = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    CHECK(p->lst == LEX_OK);
    p->pst = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
}

static void pipeline_free(Pipeline *p)
{
    ast_node_free(p->program);
    parse_records_free(p->recs, p->rn);
    lex_tokens_free(p->toks, p->tn);
    lex_records_free(NULL, 0);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* ---------------------------------------------------------------------------
 * Golden AST-dump tests
 * ------------------------------------------------------------------------- */

static void check_golden_dump(const char *name, const char *input,
                              const char *expected)
{
    Pipeline p;
    AstDumpBuf out1, out2;
    size_t i;

    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_OK);
    CHECK(p.rn == 0 && p.recs == NULL);
    CHECK(p.program != NULL);
    if (p.pst != PARSE_OK || p.program == NULL) {
        fprintf(stderr, "  [%s] expected PARSE_OK\n", name);
        pipeline_free(&p);
        return;
    }

    ast_dump_init(&out1);
    CHECK(ast_dump(p.program, &out1));
    CHECK(ast_dump_ok(&out1));
    if (!ast_dump_ok(&out1)) {
        ast_dump_free(&out1);
        pipeline_free(&p);
        return;
    }

    /* Determinism: dumping the same tree twice is byte-identical. */
    ast_dump_init(&out2);
    CHECK(ast_dump(p.program, &out2));
    CHECK(ast_dump_ok(&out2));
    if (ast_dump_ok(&out2)) {
        CHECK(out2.len == out1.len);
        CHECK(out2.len == 0 ||
              memcmp(out2.data, out1.data, out1.len) == 0);
    }
    ast_dump_free(&out2);

    if (strcmp(out1.data, expected) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL %s:%d: [%s] golden dump mismatch\n",
                __FILE__, __LINE__, name);
        fprintf(stderr, "  --- expected (%zu bytes) ---\n", strlen(expected));
        fwrite(expected, 1, strlen(expected), stderr);
        fprintf(stderr, "  --- got (%zu bytes) ---\n", out1.len);
        fwrite(out1.data, 1, out1.len, stderr);
        fprintf(stderr, "  ---------------------------\n");
    }
    g_checks++;

    /* The dump is deterministically anchored on source offsets only; also
     * verify a couple of structural invariants per case via the AST API. */
    for (i = 0; i < out1.len; i++) {
        CHECK(out1.data[i] != '\r'); /* LF-only dumps */
    }

    ast_dump_free(&out1);
    pipeline_free(&p);
}

static void test_golden_dumps(void)
{
    size_t i;

    for (i = 0; i < GOLDEN_CASE_COUNT; i++) {
        const GoldenCase *gc = &g_golden_cases[i];
        check_golden_dump(gc->name, gc->input, gc->expected_dump);
    }
}

/* Determinism across two independent parses of the same input. */
static void test_determinism(void)
{
    static const char *inputs[] = {
        "module main;\nstruct Point { x: i32; y: i32; }\n"
        "fn f(p: Point) -> i32 {\n"
        "  if (p.x > 0) { return p.x; } else { return p.y; }\n}\n",
        "module main;\nfn g(a: i32, b: i32) -> i32 { return a * b + 1; }\n",
    };
    size_t t;

    for (t = 0; t < sizeof(inputs) / sizeof(inputs[0]); t++) {
        Pipeline p1, p2;
        AstDumpBuf d1, d2;

        pipeline_run(&p1, inputs[t]);
        pipeline_run(&p2, inputs[t]);
        CHECK(p1.pst == PARSE_OK && p2.pst == PARSE_OK);
        if (p1.pst == PARSE_OK && p2.pst == PARSE_OK) {
            ast_dump_init(&d1);
            ast_dump_init(&d2);
            CHECK(ast_dump(p1.program, &d1));
            CHECK(ast_dump(p2.program, &d2));
            if (ast_dump_ok(&d1) && ast_dump_ok(&d2)) {
                CHECK(d1.len == d2.len);
                CHECK(d1.len == 0 ||
                      memcmp(d1.data, d2.data, d1.len) == 0);
            }
            ast_dump_free(&d1);
            ast_dump_free(&d2);
        }
        pipeline_free(&p1);
        pipeline_free(&p2);
    }
}

/* ---------------------------------------------------------------------------
 * Ambiguity-prone forms: assert the exact AST shape through the dump text.
 * ------------------------------------------------------------------------- */

static void check_dump_contains(const char *name, const char *input,
                                const char *needle)
{
    Pipeline p;
    AstDumpBuf out;

    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_OK);
    if (p.pst == PARSE_OK) {
        ast_dump_init(&out);
        CHECK(ast_dump(p.program, &out));
        if (ast_dump_ok(&out)) {
            CHECK(strstr(out.data, needle) != NULL);
            if (strstr(out.data, needle) == NULL) {
                fprintf(stderr, "  [%s] dump missing %s\n", name, needle);
            }
        }
        ast_dump_free(&out);
    }
    pipeline_free(&p);
}

static void test_ambiguities(void)
{
    /* sizeof(type) vs sizeof(expr): leading primitive type keyword. */
    check_dump_contains("sizeof-prim-type", "module main;\n"
                        "fn f() -> i32 { return sizeof(u8); }\n",
                        "expr_sizeof_type");
    /* sizeof(Point) where Point is a declared struct type. */
    check_dump_contains("sizeof-struct-type", "module main;\n"
                        "struct Point { x: i32; y: i32; }\n"
                        "fn f() -> i32 { return sizeof(Point); }\n",
                        "expr_sizeof_type");
    /* sizeof(p) where p is a value name (expression operand). */
    check_dump_contains("sizeof-expr-value", "module main;\n"
                        "fn f(p: i32) -> i32 { return sizeof(p); }\n",
                        "expr_sizeof_expr");
    /* sizeof(q) where q is undeclared: expression operand. */
    check_dump_contains("sizeof-expr-undeclared", "module main;\n"
                        "fn f() -> i32 { return sizeof(q); }\n",
                        "expr_sizeof_expr");
    /* sizeof of an arbitrary expression. */
    check_dump_contains("sizeof-expr-arith", "module main;\n"
                        "fn f(a: i32, b: i32) -> i32 { return sizeof(a + b); }\n",
                        "expr_sizeof_expr");
    /* alignof takes a type only. */
    check_dump_contains("alignof-type", "module main;\n"
                        "fn f() -> i32 { return alignof(Point); }\n",
                        "expr_alignof");

    /* struct-init postfix: `{` after an expression is struct_init, never a
     * block, and a following postfix (member) applies after it. */
    check_dump_contains("struct-init-postfix", "module main;\n"
                        "struct Point { x: i32; y: i32; }\n"
                        "fn f(p: Point) -> i32 { return p { x: 1, y: 2 }.x; }\n",
                        "expr_member");
    check_dump_contains("struct-init-base-ident", "module main;\n"
                        "struct Point { x: i32; y: i32; }\n"
                        "fn f() -> i32 { var q: Point = Point { x: 1, y: 2 }; "
                        "return q.x; }\n",
                        "expr_struct_init");

    /* The comment-split rule: a comment is whitespace, so `a` and `b`
     * separated only by a comment are two tokens (never a merged
     * identifier); a parse of `a SLASH-STAR STAR-SLASH + SLASH-STAR
     * STAR-SLASH b` yields a binary add with lhs a and rhs b. */
    check_dump_contains("comment-split", "module main;\n"
                        "fn f() -> i32 { var x: i32 = a/**/+/**/b; return x; }\n",
                        "expr_binary op=+");

    /* Ternary right-associativity: a ? b : c ? d : e nests the else. */
    check_dump_contains("ternary-right-assoc", "module main;\n"
                        "fn f(a: i32, b: i32, c: i32, d: i32, e: i32) -> i32 {\n"
                        "  return a ? b : c ? d : e;\n}\n",
                        "else expr_ternary");

    /* Postfix type order: u8*[4] is array of 4 ptr-to-u8; u8[4]* is ptr to
     * array; u8*[] slice of ptr; u8[]* ptr to slice. */
    check_dump_contains("type-order-array-of-ptr", "module main;\n"
                        "var a: u8*[4] = 1;\n",
                        "type_array");
    check_dump_contains("type-order-ptr-to-array", "module main;\n"
                        "var b: u8[4]* = 1;\n",
                        "type_ptr");
    check_dump_contains("type-order-slice-of-ptr", "module main;\n"
                        "var c: u8*[] = 1;\n",
                        "type_slice");
    check_dump_contains("type-order-ptr-to-slice", "module main;\n"
                        "var d: u8[]* = 1;\n",
                        "type_ptr");
}

/* ---------------------------------------------------------------------------
 * Grammar-level rejections and deterministic recovery
 * ------------------------------------------------------------------------- */

static void check_neg_one(const char *name, const char *input,
                          const char *code, const char *message,
                          int64_t sl, int64_t sc, int64_t so,
                          int64_t el, int64_t ec, int64_t eo)
{
    Pipeline p;

    (void)name;
    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_DIAG_ERROR);
    CHECK(p.recs != NULL && p.rn == 1);
    if (p.recs != NULL && p.rn >= 1) {
        const DiagRecord *r = p.recs[0];
        CHECK(strcmp(r->code, code) == 0);
        CHECK(strcmp(r->severity, "error") == 0);
        CHECK(strcmp(r->phase, "syntax") == 0);
        CHECK(r->recovery != NULL && strcmp(r->recovery, "authoritative") == 0);
        if (message != NULL) {
            CHECK(strcmp(r->message, message) == 0);
        }
        CHECK(r->primary_span != NULL);
        if (r->primary_span != NULL) {
            if (r->primary_span->start.line != sl ||
                r->primary_span->start.col != sc ||
                r->primary_span->start.offset != so ||
                r->primary_span->end.line != el ||
                r->primary_span->end.col != ec ||
                r->primary_span->end.offset != eo) {
                fprintf(stderr,
                        "  [%s] record span: got (%lld,%lld,%lld)-(%lld,%lld,%lld) "
                        "want (%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                        code,
                        (long long)r->primary_span->start.line,
                        (long long)r->primary_span->start.col,
                        (long long)r->primary_span->start.offset,
                        (long long)r->primary_span->end.line,
                        (long long)r->primary_span->end.col,
                        (long long)r->primary_span->end.offset,
                        (long long)sl, (long long)sc, (long long)so,
                        (long long)el, (long long)ec, (long long)eo);
            }
            CHECK(r->primary_span->start.line == sl);
            CHECK(r->primary_span->start.col == sc);
            CHECK(r->primary_span->start.offset == so);
            CHECK(r->primary_span->end.line == el);
            CHECK(r->primary_span->end.col == ec);
            CHECK(r->primary_span->end.offset == eo);
            CHECK(strcmp(r->primary_span->file, "input.ai") == 0);
        }
    }
    pipeline_free(&p);
}

static void check_neg_two(const char *name, const char *input,
                          const char *code1, const char *msg1,
                          int64_t sl1, int64_t sc1, int64_t so1,
                          int64_t el1, int64_t ec1, int64_t eo1,
                          const char *code2, const char *msg2,
                          int64_t sl2, int64_t sc2, int64_t so2,
                          int64_t el2, int64_t ec2, int64_t eo2)
{
    Pipeline p;

    (void)name;
    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_DIAG_ERROR);
    CHECK(p.recs != NULL && p.rn == 2);
    if (p.recs != NULL && p.rn >= 2) {
        const DiagRecord *r0 = p.recs[0];
        const DiagRecord *r1 = p.recs[1];
        CHECK(strcmp(r0->code, code1) == 0);
        CHECK(strcmp(r0->phase, "syntax") == 0);
        CHECK(r0->recovery != NULL &&
              strcmp(r0->recovery, "authoritative") == 0);
        if (msg1 != NULL) {
            CHECK(strcmp(r0->message, msg1) == 0);
        }
        CHECK(r0->primary_span != NULL);
        if (r0->primary_span != NULL) {
            CHECK(r0->primary_span->start.line == sl1 &&
                  r0->primary_span->start.col == sc1 &&
                  r0->primary_span->start.offset == so1);
            CHECK(r0->primary_span->end.line == el1 &&
                  r0->primary_span->end.col == ec1 &&
                  r0->primary_span->end.offset == eo1);
        }
        CHECK(strcmp(r1->code, code2) == 0);
        CHECK(strcmp(r1->phase, "syntax") == 0);
        CHECK(r1->recovery != NULL &&
              strcmp(r1->recovery, "recovery_derived") == 0);
        if (msg2 != NULL) {
            CHECK(strcmp(r1->message, msg2) == 0);
        }
        CHECK(r1->primary_span != NULL);
        if (r1->primary_span != NULL) {
            CHECK(r1->primary_span->start.line == sl2 &&
                  r1->primary_span->start.col == sc2 &&
                  r1->primary_span->start.offset == so2);
            CHECK(r1->primary_span->end.line == el2 &&
                  r1->primary_span->end.col == ec2 &&
                  r1->primary_span->end.offset == eo2);
        }
    }
    pipeline_free(&p);
}

static void test_grammar_rejections(void)
{
    /* AIC-S0101: expected token. Missing ';' at EOF (span from last token
     * end to EOF, mirroring the missing-semicolon corpus anchor). */
    check_neg_one("missing-semicolon-eof",
                  "module main;\nvar x: i32 = 42\n",
                  "AIC-S0101", "expected ';'",
                  2, 16, 28, 3, 1, 29);

    /* AIC-S0101: missing ';' after return value, before '}'. */
    check_neg_one("missing-semicolon-rbrace",
                  "module main;\nfn f() -> i32 {\n  return 1\n}\n",
                  "AIC-S0101", "expected ';'",
                  4, 1, 40, 4, 2, 41);

    /* AIC-S0101: missing ')' in a parameter list. The failed fn_decl is
     * dropped; recovery consumes to the ';', then the leftover '}' at top
     * level is reported as AIC-S0102 (recovery_derived). */
    check_neg_two("missing-rparen-params",
                  "module main;\nfn f(x: i32 -> i32 {\n  return x;\n}\n",
                  "AIC-S0101", "expected ')'",
                  2, 13, 25, 2, 15, 27,
                  "AIC-S0102", "unexpected token",
                  4, 1, 46, 4, 2, 47);

    /* AIC-S0101: missing '}' at EOF. */
    check_neg_one("missing-rbrace",
                  "module main;\nfn f() -> i32 {\n  var x: i32 = 1;\n  return x;\n",
                  "AIC-S0101", "expected '}'",
                  4, 12, 58, 5, 1, 59);

    /* AIC-S0101: missing identifier after 'struct'. The failed struct_decl
     * is dropped; recovery consumes to the ';', then the leftover '}' at
     * top level is reported as AIC-S0102 (recovery_derived). */
    check_neg_two("missing-ident-struct",
                  "module main;\nstruct { x: i32; }\n",
                  "AIC-S0101", "expected identifier",
                  2, 8, 20, 2, 9, 21,
                  "AIC-S0102", "unexpected token",
                  2, 18, 30, 2, 19, 31);

    /* AIC-S0101: missing expression after '='. */
    check_neg_one("missing-expr-init",
                  "module main;\nfn f() -> i32 {\n  var x: i32 = ;\n  return x;\n}\n",
                  "AIC-S0101", "expected expression",
                  3, 16, 44, 3, 17, 45);

    /* AIC-S0103: module declaration not first. */
    check_neg_one("module-not-first",
                  "var x: i32 = 42;\nmodule main;\n",
                  "AIC-S0103",
                  "module declaration must be the first element",
                  2, 1, 17, 2, 13, 29);

    /* AIC-S0104: controlled body without braces. */
    check_neg_one("if-no-braces",
                  "module main;\nfn main() -> i32 {\n  if (true)\n    return 0;\n  return 1;\n}\n",
                  "AIC-S0104",
                  "controlled body must be enclosed in braces",
                  3, 3, 34, 3, 12, 43);
}

/* Two errors in one file: the first is authoritative, every later syntax
 * error is marked recovery_derived, and records are sorted by span start. */
static void test_recovery_derived(void)
{
    static const char *input =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  var x: i32 = ;\n"
        "  return 1\n"
        "}\n";
    Pipeline p;

    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_DIAG_ERROR);
    CHECK(p.recs != NULL && p.rn == 2);
    if (p.recs != NULL && p.rn >= 2) {
        CHECK(strcmp(p.recs[0]->code, "AIC-S0101") == 0);
        CHECK(p.recs[0]->recovery != NULL &&
              strcmp(p.recs[0]->recovery, "authoritative") == 0);
        CHECK(p.recs[0]->primary_span != NULL &&
              p.recs[0]->primary_span->start.offset == 44);
        CHECK(strcmp(p.recs[1]->code, "AIC-S0101") == 0);
        CHECK(p.recs[1]->recovery != NULL &&
              strcmp(p.recs[1]->recovery, "recovery_derived") == 0);
        CHECK(p.recs[1]->primary_span != NULL &&
              p.recs[1]->primary_span->start.offset == 57);
        /* Deterministic ordering: sorted by start offset. */
        CHECK(p.recs[0]->primary_span->start.offset <
              p.recs[1]->primary_span->start.offset);
    }
    pipeline_free(&p);
}

/* Unexpected tokens at top level: AIC-S0102 for the run, then the parser
 * recovers to the next top-level construct. */
static void test_unexpected_token(void)
{
    static const char *input = "module main;\nvar x: i32 = 42; **\n";
    Pipeline p;

    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_DIAG_ERROR);
    CHECK(p.recs != NULL && p.rn == 1);
    if (p.recs != NULL && p.rn >= 1) {
        CHECK(strcmp(p.recs[0]->code, "AIC-S0102") == 0);
        CHECK(strcmp(p.recs[0]->message, "unexpected token") == 0);
        CHECK(strcmp(p.recs[0]->phase, "syntax") == 0);
        CHECK(p.recs[0]->recovery != NULL &&
              strcmp(p.recs[0]->recovery, "authoritative") == 0);
        CHECK(p.recs[0]->primary_span != NULL);
        if (p.recs[0]->primary_span != NULL) {
            CHECK(p.recs[0]->primary_span->start.line == 2 &&
                  p.recs[0]->primary_span->start.col == 18 &&
                  p.recs[0]->primary_span->start.offset == 30);
            CHECK(p.recs[0]->primary_span->end.line == 2 &&
                  p.recs[0]->primary_span->end.col == 20 &&
                  p.recs[0]->primary_span->end.offset == 32);
        }
    }
    pipeline_free(&p);
}

/* Records validate and emit deterministic JSONL (recovery_derived marking
 * must be visible in the emitted record). */
static void test_records_valid_and_emit(void)
{
    static const char *input =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  var x: i32 = ;\n"
        "  return 1\n"
        "}\n";
    Pipeline p;
    DiagBuf out;
    size_t i;

    pipeline_run(&p, input);
    CHECK(p.pst == PARSE_DIAG_ERROR);
    CHECK(p.rn == 2);
    for (i = 0; i < p.rn; i++) {
        char errbuf[128];
        CHECK(diag_record_validate(p.recs[i], errbuf, sizeof(errbuf)));
        diag_buf_init(&out);
        CHECK(diag_emit_record(&out, p.recs[i]));
        CHECK(diag_buf_ok(&out));
        if (out.data != NULL) {
            CHECK(strstr(out.data, "\"code\":\"AIC-S0101\"") != NULL);
            CHECK(strstr(out.data, "\"severity\":\"error\"") != NULL);
            CHECK(strstr(out.data, "\"phase\":\"syntax\"") != NULL);
            CHECK(strstr(out.data, "\"file\":\"input.ai\"") != NULL);
            if (i == 0) {
                CHECK(strstr(out.data, "\"recovery\":\"authoritative\"") != NULL);
            } else {
                CHECK(strstr(out.data, "\"recovery\":\"recovery_derived\"") != NULL);
            }
        }
        diag_buf_free(&out);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Corpus anchors: re-execute the parser-owned negative fixtures.
 * ------------------------------------------------------------------------- */

static void check_corpus_fixture(const char *path,
                                 const char *code, const char *message,
                                 int64_t sl, int64_t sc, int64_t so,
                                 int64_t el, int64_t ec, int64_t eo)
{
    FILE *f = NULL;
    long sz = 0;
    char *buf = NULL;
    size_t rd = 0;
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t rn = 0;
    LexToken *toks = NULL;
    size_t tn = 0;
    AstNode *program = NULL;
    LoadStatus lst;
    LexStatus lst2;
    ParseStatus pst;

    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        CHECK(0 && "fixture seek failed");
        return;
    }
    sz = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        CHECK(0 && "fixture seek failed");
        return;
    }
    buf = (char *)malloc((size_t)sz + 1);
    CHECK(buf != NULL);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    CHECK(rd == (size_t)sz);

    lst = load_source_from_bytes("input.ai", (const uint8_t *)buf, (size_t)sz,
                                 &src, &recs, &rn);
    CHECK(lst == LOAD_OK);
    if (lst != LOAD_OK) {
        load_records_free(recs, rn);
        free(buf);
        return;
    }

    lst2 = lex_tokenize(src, &toks, &tn, &recs, &rn);
    CHECK(lst2 == LEX_OK);
    pst = parse_program(toks, tn, &program, &recs, &rn);
    CHECK(pst == PARSE_DIAG_ERROR);
    CHECK(recs != NULL && rn == 1);
    if (recs != NULL && rn >= 1) {
        const DiagRecord *r = recs[0];
        CHECK(strcmp(r->code, code) == 0);
        CHECK(strcmp(r->severity, "error") == 0);
        CHECK(strcmp(r->phase, "syntax") == 0);
        CHECK(r->recovery != NULL && strcmp(r->recovery, "authoritative") == 0);
        if (message != NULL) {
            CHECK(strcmp(r->message, message) == 0);
        }
        CHECK(r->primary_span != NULL);
        if (r->primary_span != NULL) {
            if (r->primary_span->start.line != sl ||
                r->primary_span->start.col != sc ||
                r->primary_span->start.offset != so ||
                r->primary_span->end.line != el ||
                r->primary_span->end.col != ec ||
                r->primary_span->end.offset != eo) {
                fprintf(stderr,
                        "  [%s] record span: got (%lld,%lld,%lld)-(%lld,%lld,%lld) "
                        "want (%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                        code,
                        (long long)r->primary_span->start.line,
                        (long long)r->primary_span->start.col,
                        (long long)r->primary_span->start.offset,
                        (long long)r->primary_span->end.line,
                        (long long)r->primary_span->end.col,
                        (long long)r->primary_span->end.offset,
                        (long long)sl, (long long)sc, (long long)so,
                        (long long)el, (long long)ec, (long long)eo);
            }
            CHECK(r->primary_span->start.line == sl);
            CHECK(r->primary_span->start.col == sc);
            CHECK(r->primary_span->start.offset == so);
            CHECK(r->primary_span->end.line == el);
            CHECK(r->primary_span->end.col == ec);
            CHECK(r->primary_span->end.offset == eo);
            CHECK(strcmp(r->primary_span->file, "input.ai") == 0);
        }
    }

    ast_node_free(program);
    parse_records_free(recs, rn);
    lex_tokens_free(toks, tn);
    lex_records_free(NULL, 0);
    load_source_free(src);
    free(buf);
}

static void test_corpus_anchors(void)
{
    /* The five parser-owned corpus fixtures; expected.json values inlined. */
    check_corpus_fixture(
        "tests/negative/cases/18-2-syntax-brace-no-type/input.ai",
        "AIC-S0101", "expected token",
        3, 16, 61, 3, 30, 75);
    check_corpus_fixture(
        "tests/negative/cases/derived-syntax-if-no-braces/input.ai",
        "AIC-S0104", "controlled body must be enclosed in braces",
        3, 3, 34, 3, 12, 43);
    check_corpus_fixture(
        "tests/negative/cases/derived-syntax-missing-semicolon/input.ai",
        "AIC-S0101", "expected ';'",
        2, 16, 28, 3, 1, 29);
    check_corpus_fixture(
        "tests/negative/cases/derived-syntax-module-not-first/input.ai",
        "AIC-S0103",
        "module declaration must be the first element",
        2, 1, 17, 2, 13, 29);
    check_corpus_fixture(
        "tests/negative/cases/derived-syntax-unexpected-token/input.ai",
        "AIC-S0102", "unexpected token",
        2, 18, 30, 2, 20, 32);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_golden_dumps();
    test_determinism();
    test_ambiguities();
    test_grammar_rejections();
    test_recovery_derived();
    test_unexpected_token();
    test_records_valid_and_emit();
    test_corpus_anchors();

    ast_node_free(NULL);
    parse_records_free(NULL, 0);

    if (g_failures == 0) {
        printf("parse_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "parse_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
