/* bootstrap/src/lex/lex_test.c
 *
 * WP-M0-08 unit tests: golden token-stream tests (exact token kinds,
 * payloads, and spans for every token family of spec sec. 4), negative
 * tests per spec sec. 18.1 and the diagnostic contract sec. 11.1 (AIC-L0001,
 * AIC-L0004..AIC-L0009 with corpus-exact spans), the unary-minus
 * minimum-value rule (spec sec. 4.3), string escape / UTF-8-after-expansion
 * rules (spec sec. 4.4), and re-execution of the lexer-owned negative-corpus
 * anchors against the real fixture files.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-lex' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/lex/lex_test.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-lex/lex_test.exe
 *
 * The corpus anchors read the committed fixture inputs under
 * tests/negative/cases/ (read-only; owned by WP-M0-03) and assert the exact
 * records from each case's expected.json.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "lex.h"

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
 * Expected-token model for golden tests
 * ------------------------------------------------------------------------- */

typedef struct ExpTok {
    LexTokenKind kind;
    LexKeyword kw;
    LexPunct punct;
    LexIntType itype;
    uint64_t ivalue;
    bool is_min;
    const char *ident;  /* TOK_IDENT */
    const char *sbytes; /* TOK_STR_LITERAL decoded bytes; slen == SIZE_MAX means strlen */
    size_t slen;
    int64_t so;         /* span start offset */
    int64_t eo;         /* span end offset */
} ExpTok;

#define STR_EXP(s) (s), (sizeof(s) - 1)

/* Lex `input` (through the loader, file name "input.ai") and compare the
 * token stream against the expected table. `*pos_out` (optional) receives
 * the EOF token span start (= input length). */
static void check_golden(const char *name, const char *input,
                         const ExpTok *exps, size_t n)
{
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t rn = 0;
    LexToken *toks = NULL;
    size_t tn = 0;
    LoadStatus lst;
    LexStatus st;
    size_t i;

    lst = load_source_from_bytes("input.ai", (const uint8_t *)input,
                                 strlen(input), &src, &recs, &rn);
    CHECK(lst == LOAD_OK);
    if (lst != LOAD_OK) {
        load_records_free(recs, rn);
        return;
    }

    st = lex_tokenize(src, &toks, &tn, &recs, &rn);
    CHECK(st == LEX_OK);
    CHECK(recs == NULL && rn == 0);
    CHECK(toks != NULL && tn == n + 1); /* expected tokens + EOF */

    for (i = 0; i < n; ++i) {
        const LexToken *t = &toks[i];
        const ExpTok *e = &exps[i];
        if (i >= tn) {
            break;
        }
        CHECK(t->kind == e->kind);
        CHECK(t->span != NULL);
        if (t->span != NULL) {
            if (strcmp(t->span->file, "input.ai") != 0 ||
                t->span->start.offset != e->so ||
                t->span->end.offset != e->eo) {
                fprintf(stderr,
                        "  [%s] token %zu span: got file=%s (%lld..%lld) "
                        "want (%lld..%lld)\n",
                        name, i, t->span->file,
                        (long long)t->span->start.offset,
                        (long long)t->span->end.offset,
                        (long long)e->so, (long long)e->eo);
            }
            CHECK(strcmp(t->span->file, "input.ai") == 0);
            CHECK(t->span->start.offset == e->so);
            CHECK(t->span->end.offset == e->eo);
        }
        switch (e->kind) {
        case TOK_IDENT:
            CHECK(t->u.ident != NULL &&
                  strcmp(t->u.ident, e->ident) == 0);
            break;
        case TOK_KEYWORD:
            CHECK(t->u.keyword == e->kw);
            break;
        case TOK_INT_LITERAL:
            CHECK(t->u.integer.type == e->itype);
            CHECK(t->u.integer.value == e->ivalue);
            CHECK(t->u.integer.is_min == e->is_min);
            break;
        case TOK_STR_LITERAL: {
            size_t want = e->slen == SIZE_MAX ? strlen(e->sbytes) : e->slen;
            CHECK(t->u.string.len == want);
            CHECK(t->u.string.len == 0 ||
                  (t->u.string.bytes != NULL &&
                   memcmp(t->u.string.bytes, e->sbytes, want) == 0));
            break;
        }
        case TOK_PUNCT:
            CHECK(t->u.punct == e->punct);
            break;
        default:
            CHECK(0 && "unexpected expected kind");
            break;
        }
    }

    /* EOF token at the end. */
    CHECK(tn == n + 1);
    if (tn == n + 1) {
        CHECK(toks[n].kind == TOK_EOF);
        CHECK(toks[n].span != NULL);
        if (toks[n].span != NULL) {
            CHECK(toks[n].span->start.offset == (int64_t)strlen(input));
            CHECK(toks[n].span->end.offset == (int64_t)strlen(input));
        }
    }

    lex_tokens_free(toks, tn);
    lex_records_free(recs, rn);
    load_source_free(src);
}

/* Lex `input` and assert exactly one record with the given code, message,
 * and primary span. */
static void check_neg_one(const char *name, const char *input,
                          const char *code, const char *message,
                          int64_t sl, int64_t sc, int64_t so,
                          int64_t el, int64_t ec, int64_t eo)
{
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t rn = 0;
    LexToken *toks = NULL;
    size_t tn = 0;
    LoadStatus lst;
    LexStatus st;

    (void)name;
    lst = load_source_from_bytes("input.ai", (const uint8_t *)input,
                                 strlen(input), &src, &recs, &rn);
    CHECK(lst == LOAD_OK);
    if (lst != LOAD_OK) {
        load_records_free(recs, rn);
        return;
    }

    st = lex_tokenize(src, &toks, &tn, &recs, &rn);
    CHECK(st == LEX_DIAG_ERROR);
    CHECK(recs != NULL && rn == 1);
    if (recs != NULL && rn >= 1) {
        const DiagRecord *r = recs[0];
        CHECK(strcmp(r->code, code) == 0);
        CHECK(strcmp(r->severity, "error") == 0);
        CHECK(strcmp(r->phase, "lex") == 0);
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

    /* Tokens are still produced (best-effort stream; always ends in EOF). */
    CHECK(toks != NULL && tn >= 1);
    if (toks != NULL && tn >= 1) {
        CHECK(toks[tn - 1].kind == TOK_EOF);
    }

    lex_tokens_free(toks, tn);
    lex_records_free(recs, rn);
    load_source_free(src);
}

/* ---------------------------------------------------------------------------
 * Golden token-stream tests
 * ------------------------------------------------------------------------- */

static void test_golden_tokens(void)
{
    static const ExpTok empty[] = { { 0 } };
    check_golden("empty", "", empty, 0);

    {
        static const ExpTok ex[] = {
            { TOK_IDENT, 0, 0, 0, 0, 0, "foo", 0, 0, 0, 3 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "_bar", 0, 0, 4, 8 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "baz9", 0, 0, 9, 13 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "_", 0, 0, 14, 15 },
        };
        check_golden("identifiers", "foo _bar baz9 _", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_KEYWORD, KW_MODULE, 0, 0, 0, 0, 0, 0, 0, 0, 6 },
            { TOK_KEYWORD, KW_IMPORT, 0, 0, 0, 0, 0, 0, 0, 7, 13 },
            { TOK_KEYWORD, KW_PUB, 0, 0, 0, 0, 0, 0, 0, 14, 17 },
            { TOK_KEYWORD, KW_VAR, 0, 0, 0, 0, 0, 0, 0, 18, 21 },
            { TOK_KEYWORD, KW_CONST, 0, 0, 0, 0, 0, 0, 0, 22, 27 },
            { TOK_KEYWORD, KW_FN, 0, 0, 0, 0, 0, 0, 0, 28, 30 },
            { TOK_KEYWORD, KW_STRUCT, 0, 0, 0, 0, 0, 0, 0, 31, 37 },
            { TOK_KEYWORD, KW_ENUM, 0, 0, 0, 0, 0, 0, 0, 38, 42 },
            { TOK_KEYWORD, KW_IF, 0, 0, 0, 0, 0, 0, 0, 43, 45 },
            { TOK_KEYWORD, KW_ELSE, 0, 0, 0, 0, 0, 0, 0, 46, 50 },
            { TOK_KEYWORD, KW_WHILE, 0, 0, 0, 0, 0, 0, 0, 51, 56 },
            { TOK_KEYWORD, KW_FOR, 0, 0, 0, 0, 0, 0, 0, 57, 60 },
            { TOK_KEYWORD, KW_BREAK, 0, 0, 0, 0, 0, 0, 0, 61, 66 },
            { TOK_KEYWORD, KW_CONTINUE, 0, 0, 0, 0, 0, 0, 0, 67, 75 },
            { TOK_KEYWORD, KW_RETURN, 0, 0, 0, 0, 0, 0, 0, 76, 82 },
            { TOK_KEYWORD, KW_SWITCH, 0, 0, 0, 0, 0, 0, 0, 83, 89 },
            { TOK_KEYWORD, KW_CASE, 0, 0, 0, 0, 0, 0, 0, 90, 94 },
            { TOK_KEYWORD, KW_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 95, 102 },
            { TOK_KEYWORD, KW_TRUE, 0, 0, 0, 0, 0, 0, 0, 103, 107 },
            { TOK_KEYWORD, KW_FALSE, 0, 0, 0, 0, 0, 0, 0, 108, 113 },
            { TOK_KEYWORD, KW_NULL, 0, 0, 0, 0, 0, 0, 0, 114, 118 },
            { TOK_KEYWORD, KW_VOID, 0, 0, 0, 0, 0, 0, 0, 119, 123 },
            { TOK_KEYWORD, KW_BOOL, 0, 0, 0, 0, 0, 0, 0, 124, 128 },
            { TOK_KEYWORD, KW_I8, 0, 0, 0, 0, 0, 0, 0, 129, 131 },
            { TOK_KEYWORD, KW_I16, 0, 0, 0, 0, 0, 0, 0, 132, 135 },
            { TOK_KEYWORD, KW_I32, 0, 0, 0, 0, 0, 0, 0, 136, 139 },
            { TOK_KEYWORD, KW_I64, 0, 0, 0, 0, 0, 0, 0, 140, 143 },
            { TOK_KEYWORD, KW_U8, 0, 0, 0, 0, 0, 0, 0, 144, 146 },
            { TOK_KEYWORD, KW_U16, 0, 0, 0, 0, 0, 0, 0, 147, 150 },
            { TOK_KEYWORD, KW_U32, 0, 0, 0, 0, 0, 0, 0, 151, 154 },
            { TOK_KEYWORD, KW_U64, 0, 0, 0, 0, 0, 0, 0, 155, 158 },
            { TOK_KEYWORD, KW_ISIZE, 0, 0, 0, 0, 0, 0, 0, 159, 164 },
            { TOK_KEYWORD, KW_USIZE, 0, 0, 0, 0, 0, 0, 0, 165, 170 },
            { TOK_KEYWORD, KW_STR, 0, 0, 0, 0, 0, 0, 0, 171, 174 },
            { TOK_KEYWORD, KW_SIZEOF, 0, 0, 0, 0, 0, 0, 0, 175, 181 },
            { TOK_KEYWORD, KW_ALIGNOF, 0, 0, 0, 0, 0, 0, 0, 182, 189 },
            { TOK_KEYWORD, KW_CAST, 0, 0, 0, 0, 0, 0, 0, 190, 194 },
            { TOK_KEYWORD, KW_WRAP, 0, 0, 0, 0, 0, 0, 0, 195, 199 },
            { TOK_KEYWORD, KW_LEN, 0, 0, 0, 0, 0, 0, 0, 200, 203 },
            { TOK_KEYWORD, KW_PTR, 0, 0, 0, 0, 0, 0, 0, 204, 207 },
        };
        check_golden("keywords",
                     "module import pub var const fn struct enum "
                     "if else while for break continue return switch case default "
                     "true false null void bool i8 i16 i32 i64 u8 u16 u32 u64 "
                     "isize usize str sizeof alignof cast wrap len ptr",
                     ex, sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_KEYWORD, KW_MODULE, 0, 0, 0, 0, 0, 0, 0, 0, 6 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "main", 0, 0, 7, 11 },
            { TOK_PUNCT, 0, PUNCT_SEMI, 0, 0, 0, 0, 0, 0, 11, 12 },
        };
        check_golden("module-decl", "module main;", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_IDENT, 0, 0, 0, 0, 0, "a", 0, 0, 0, 1 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "b", 0, 0, 5, 6 },
        };
        check_golden("comments-whitespace", "a/**/b", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_IDENT, 0, 0, 0, 0, 0, "a", 0, 0, 0, 1 },
            { TOK_PUNCT, 0, PUNCT_SLASH, 0, 0, 0, 0, 0, 0, 2, 3 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "b", 0, 0, 4, 5 },
        };
        check_golden("slash-not-comment", "a / b", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }
}

static void test_golden_punct(void)
{
    /* Each token separated by a single space; offsets computed below. */
    static const ExpTok ex[] = {
        { TOK_PUNCT, 0, PUNCT_LPAREN, 0, 0, 0, 0, 0, 0, 0, 1 },
        { TOK_PUNCT, 0, PUNCT_RPAREN, 0, 0, 0, 0, 0, 0, 2, 3 },
        { TOK_PUNCT, 0, PUNCT_LBRACE, 0, 0, 0, 0, 0, 0, 4, 5 },
        { TOK_PUNCT, 0, PUNCT_RBRACE, 0, 0, 0, 0, 0, 0, 6, 7 },
        { TOK_PUNCT, 0, PUNCT_LBRACKET, 0, 0, 0, 0, 0, 0, 8, 9 },
        { TOK_PUNCT, 0, PUNCT_RBRACKET, 0, 0, 0, 0, 0, 0, 10, 11 },
        { TOK_PUNCT, 0, PUNCT_SEMI, 0, 0, 0, 0, 0, 0, 12, 13 },
        { TOK_PUNCT, 0, PUNCT_COMMA, 0, 0, 0, 0, 0, 0, 14, 15 },
        { TOK_PUNCT, 0, PUNCT_COLON, 0, 0, 0, 0, 0, 0, 16, 17 },
        { TOK_PUNCT, 0, PUNCT_DOT, 0, 0, 0, 0, 0, 0, 18, 19 },
        { TOK_PUNCT, 0, PUNCT_ARROW, 0, 0, 0, 0, 0, 0, 20, 22 },
        { TOK_PUNCT, 0, PUNCT_STAR, 0, 0, 0, 0, 0, 0, 23, 24 },
        { TOK_PUNCT, 0, PUNCT_AMP, 0, 0, 0, 0, 0, 0, 25, 26 },
        { TOK_PUNCT, 0, PUNCT_PLUS, 0, 0, 0, 0, 0, 0, 27, 28 },
        { TOK_PUNCT, 0, PUNCT_MINUS, 0, 0, 0, 0, 0, 0, 29, 30 },
        { TOK_PUNCT, 0, PUNCT_TILDE, 0, 0, 0, 0, 0, 0, 31, 32 },
        { TOK_PUNCT, 0, PUNCT_BANG, 0, 0, 0, 0, 0, 0, 33, 34 },
        { TOK_PUNCT, 0, PUNCT_SLASH, 0, 0, 0, 0, 0, 0, 35, 36 },
        { TOK_PUNCT, 0, PUNCT_PERCENT, 0, 0, 0, 0, 0, 0, 37, 38 },
        { TOK_PUNCT, 0, PUNCT_LT, 0, 0, 0, 0, 0, 0, 39, 40 },
        { TOK_PUNCT, 0, PUNCT_LE, 0, 0, 0, 0, 0, 0, 41, 43 },
        { TOK_PUNCT, 0, PUNCT_GT, 0, 0, 0, 0, 0, 0, 44, 45 },
        { TOK_PUNCT, 0, PUNCT_GE, 0, 0, 0, 0, 0, 0, 46, 48 },
        { TOK_PUNCT, 0, PUNCT_EQ, 0, 0, 0, 0, 0, 0, 49, 51 },
        { TOK_PUNCT, 0, PUNCT_NE, 0, 0, 0, 0, 0, 0, 52, 54 },
        { TOK_PUNCT, 0, PUNCT_AND_AND, 0, 0, 0, 0, 0, 0, 55, 57 },
        { TOK_PUNCT, 0, PUNCT_OR_OR, 0, 0, 0, 0, 0, 0, 58, 60 },
        { TOK_PUNCT, 0, PUNCT_ASSIGN, 0, 0, 0, 0, 0, 0, 61, 62 },
        { TOK_PUNCT, 0, PUNCT_PLUS_ASSIGN, 0, 0, 0, 0, 0, 0, 63, 65 },
        { TOK_PUNCT, 0, PUNCT_MINUS_ASSIGN, 0, 0, 0, 0, 0, 0, 66, 68 },
        { TOK_PUNCT, 0, PUNCT_STAR_ASSIGN, 0, 0, 0, 0, 0, 0, 69, 71 },
        { TOK_PUNCT, 0, PUNCT_SLASH_ASSIGN, 0, 0, 0, 0, 0, 0, 72, 74 },
        { TOK_PUNCT, 0, PUNCT_PERCENT_ASSIGN, 0, 0, 0, 0, 0, 0, 75, 77 },
        { TOK_PUNCT, 0, PUNCT_SHL_ASSIGN, 0, 0, 0, 0, 0, 0, 78, 81 },
        { TOK_PUNCT, 0, PUNCT_SHR_ASSIGN, 0, 0, 0, 0, 0, 0, 82, 85 },
        { TOK_PUNCT, 0, PUNCT_AND_ASSIGN, 0, 0, 0, 0, 0, 0, 86, 88 },
        { TOK_PUNCT, 0, PUNCT_OR_ASSIGN, 0, 0, 0, 0, 0, 0, 89, 91 },
        { TOK_PUNCT, 0, PUNCT_XOR_ASSIGN, 0, 0, 0, 0, 0, 0, 92, 94 },
        { TOK_PUNCT, 0, PUNCT_SHL, 0, 0, 0, 0, 0, 0, 95, 97 },
        { TOK_PUNCT, 0, PUNCT_SHR, 0, 0, 0, 0, 0, 0, 98, 100 },
        { TOK_PUNCT, 0, PUNCT_QUESTION, 0, 0, 0, 0, 0, 0, 101, 102 },
        { TOK_PUNCT, 0, PUNCT_OR, 0, 0, 0, 0, 0, 0, 103, 104 },
        { TOK_PUNCT, 0, PUNCT_XOR, 0, 0, 0, 0, 0, 0, 105, 106 },
        { TOK_PUNCT, 0, PUNCT_DOT_DOT, 0, 0, 0, 0, 0, 0, 107, 109 },
    };
    check_golden("all-punct",
                 "( ) { } [ ] ; , : . -> * & + - ~ ! / % < <= > >= == != "
                 "&& || = += -= *= /= %= <<= >>= &= |= ^= << >> ? | ^ ..",
                 ex, sizeof(ex) / sizeof(ex[0]));
}

/* ---------------------------------------------------------------------------
 * Integer literals (spec sec. 4.3)
 * ------------------------------------------------------------------------- */

static void test_golden_ints(void)
{
    {
        static const ExpTok ex[] = {
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 0, 0, 0, 0, 0, 0, 1 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 42, 0, 0, 0, 0, 2, 4 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 255, 0, 0, 0, 0, 5, 9 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 10, 0, 0, 0, 0, 10, 16 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 15, 0, 0, 0, 0, 17, 21 },
        };
        check_golden("int-bases", "0 42 0xFF 0b1010 0o17", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 1000000, 0, 0, 0, 0, 0, 9 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 65535, 0, 0, 0, 0, 10, 17 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 2, 0, 0, 0, 0, 18, 23 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 63, 0, 0, 0, 0, 24, 29 },
        };
        check_golden("int-underscores", "1_000_000 0xFF_FF 0b1_0 0o7_7", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I8, 127, 0, 0, 0, 0, 0, 5 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I16, 32767, 0, 0, 0, 0, 6, 14 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 2147483647UL, 0, 0, 0, 0, 15, 28 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I64, 9223372036854775807ULL, 0, 0, 0, 0, 29, 51 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_U8, 255, 0, 0, 0, 0, 52, 57 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_U16, 65535, 0, 0, 0, 0, 58, 66 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_U32, 4294967295UL, 0, 0, 0, 0, 67, 80 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_U64, 18446744073709551615ULL, 0, 0, 0, 0, 81, 104 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_ISIZE, 42, 0, 0, 0, 0, 105, 112 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_USIZE, 42, 0, 0, 0, 0, 113, 120 },
        };
        check_golden("int-suffix-bounds",
                     "127i8 32767i16 2147483647i32 9223372036854775807i64 "
                     "255u8 65535u16 4294967295u32 18446744073709551615u64 "
                     "42isize 42usize",
                     ex, sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 2147483647UL, 0, 0, 0, 0, 0, 10 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I64, 2147483648ULL, 0, 0, 0, 0, 11, 21 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I64, 9223372036854775807ULL, 0, 0, 0, 0, 22, 41 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_U64, 9223372036854775808ULL, 0, 0, 0, 0, 42, 61 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_U64, 18446744073709551615ULL, 0, 0, 0, 0, 62, 82 },
        };
        check_golden("int-unsuffixed-typing",
                     "2147483647 2147483648 9223372036854775807 "
                     "9223372036854775808 18446744073709551615",
                     ex, sizeof(ex) / sizeof(ex[0]));
    }
}

/* The unary-minus minimum-value rule (spec sec. 4.3). */
static void test_unary_minus(void)
{
    {
        static const ExpTok ex[] = {
            { TOK_PUNCT, 0, PUNCT_MINUS, 0, 0, 0, 0, 0, 0, 0, 1 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I8, 128, 1, 0, 0, 0, 1, 6 },
        };
        check_golden("unary-minus-min", "-128i8", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_PUNCT, 0, PUNCT_MINUS, 0, 0, 0, 0, 0, 0, 0, 1 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I8, 128, 1, 0, 0, 0, 2, 7 },
        };
        check_golden("unary-minus-space", "- 128i8", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_PUNCT, 0, PUNCT_MINUS, 0, 0, 0, 0, 0, 0, 0, 1 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I8, 128, 1, 0, 0, 0, 6, 11 },
        };
        check_golden("unary-minus-comment", "-/*c*/128i8", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_PUNCT, 0, PUNCT_MINUS, 0, 0, 0, 0, 0, 0, 0, 1 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I64, 9223372036854775808ULL, 1,
              0, 0, 0, 1, 23 },
        };
        check_golden("unary-minus-i64-min", "-9223372036854775808i64", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    /* 128i8 alone: out of range -> AIC-L0006 (span the literal). */
    check_neg_one("int-overrun-i8-alone", "128i8",
                  "AIC-L0006", "integer literal 128 is not representable in type i8",
                  1, 1, 0, 1, 6, 5);

    /* x - 128i8: the minus is binary, literal is not the direct operand. */
    check_neg_one("int-overrun-binary-minus", "x - 128i8",
                  "AIC-L0006", "integer literal 128 is not representable in type i8",
                  1, 5, 4, 1, 10, 9);

    /* -(128i8): the operand of unary minus is the parenthesized expression. */
    check_neg_one("int-overrun-paren", "-(128i8)",
                  "AIC-L0006", "integer literal 128 is not representable in type i8",
                  1, 3, 2, 1, 8, 7);
}

static void test_neg_ints(void)
{
    /* 18-1-lex-int-overrun corpus anchor (expected.json: AIC-L0006 at
     * (2,15,27)-(2,20,32), message matches). */
    check_neg_one("corpus-int-overrun",
                  "module main;\nvar bad: u8 = 300u8;",
                  "AIC-L0006",
                  "integer literal 300 is not representable in type u8",
                  2, 15, 27, 2, 20, 32);

    /* Suffixed unsigned overflow. */
    check_neg_one("u8-overrun", "256u8",
                  "AIC-L0006",
                  "integer literal 256 is not representable in type u8",
                  1, 1, 0, 1, 6, 5);

    /* Unsuffixed overflow (does not fit u64). */
    check_neg_one("u64-overrun", "18446744073709551616",
                  "AIC-L0006", NULL, 1, 1, 0, 1, 21, 20);

    /* Misplaced underscores (spec sec. 4.3): doubled, trailing, after-prefix.
     * The corpus fixture `derived-lex-misplaced-underscore` expects AIC-L0005
     * for `1_2_3_4_5_6_7_8_9_0`, but per accepted spec sec. 4.3 every `_`
     * there sits between two digits, so that literal is VALID; the fixture is
     * defective and is routed to Planner (see README). We test genuine
     * misplacements here. */
    check_neg_one("underscore-doubled", "1__2",
                  "AIC-L0005", "misplaced '_' in integer literal",
                  1, 1, 0, 1, 5, 4);
    check_neg_one("underscore-trailing", "1_2_",
                  "AIC-L0005", "misplaced '_' in integer literal",
                  1, 1, 0, 1, 5, 4);
    check_neg_one("underscore-after-prefix", "0x_FF",
                  "AIC-L0005", "misplaced '_' in integer literal",
                  1, 1, 0, 1, 6, 5);
    check_neg_one("underscore-leading", "0x_1",
                  "AIC-L0005", "misplaced '_' in integer literal",
                  1, 1, 0, 1, 5, 4);

    /* The spec-valid underscore-separated literal (the corpus fixture's input
     * is valid; assert no diagnostic and a correct i32 token). */
    {
        static const ExpTok ex[] = {
            { TOK_KEYWORD, KW_MODULE, 0, 0, 0, 0, 0, 0, 0, 0, 6 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "main", 0, 0, 7, 11 },
            { TOK_PUNCT, 0, PUNCT_SEMI, 0, 0, 0, 0, 0, 0, 11, 12 },
            { TOK_KEYWORD, KW_VAR, 0, 0, 0, 0, 0, 0, 0, 13, 16 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "x", 0, 0, 17, 18 },
            { TOK_PUNCT, 0, PUNCT_COLON, 0, 0, 0, 0, 0, 0, 18, 19 },
            { TOK_KEYWORD, KW_I32, 0, 0, 0, 0, 0, 0, 0, 20, 23 },
            { TOK_PUNCT, 0, PUNCT_ASSIGN, 0, 0, 0, 0, 0, 0, 24, 25 },
            { TOK_INT_LITERAL, 0, 0, LEX_INT_I32, 1234567890UL, 0,
              0, 0, 0, 26, 45 },
            { TOK_PUNCT, 0, PUNCT_SEMI, 0, 0, 0, 0, 0, 0, 45, 46 },
        };
        check_golden("underscore-valid-corpus-input",
                     "module main;\nvar x: i32 = 1_2_3_4_5_6_7_8_9_0;",
                     ex, sizeof(ex) / sizeof(ex[0]));
    }

    /* Malformed literals: prefix with no digits. */
    check_neg_one("malformed-hex-prefix", "0x",
                  "AIC-L0001", "malformed integer literal",
                  1, 1, 0, 1, 3, 2);
    check_neg_one("malformed-bin-prefix", "0b",
                  "AIC-L0001", "malformed integer literal",
                  1, 1, 0, 1, 3, 2);
    check_neg_one("malformed-oct-prefix", "0o",
                  "AIC-L0001", "malformed integer literal",
                  1, 1, 0, 1, 3, 2);
    check_neg_one("malformed-prefix-underscore", "0x_",
                  "AIC-L0001", "malformed integer literal",
                  1, 1, 0, 1, 4, 3);

    /* Leading-zero decimal: `0` alone is the decimal zero (spec sec. 4.3). */
    check_neg_one("leading-zero-decimal", "0123",
                  "AIC-L0001", "malformed integer literal",
                  1, 1, 0, 1, 5, 4);
}

/* ---------------------------------------------------------------------------
 * String literals (spec sec. 4.4)
 * ------------------------------------------------------------------------- */

static void test_golden_strings(void)
{
    {
        static const ExpTok ex[] = {
            { TOK_STR_LITERAL, 0, 0, 0, 0, 0, 0, STR_EXP("hello"), 0, 7 },
        };
        check_golden("string-simple", "\"hello\"", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_STR_LITERAL, 0, 0, 0, 0, 0, 0,
              "a\nb\tc\\d\"e\0f", 11, 0, 18 },
        };
        /* "a\nb\tc\\d\"e\0f" -> bytes: a LF b TAB c BS d QUOTE e NUL f */
        check_golden("string-escapes", "\"a\\nb\\tc\\\\d\\\"e\\0f\"", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_STR_LITERAL, 0, 0, 0, 0, 0, 0,
              STR_EXP("caf\xC3\xA9"), 0, 13 },
        };
        /* "\"caf\\xC3\\xA9\"" -> bytes caf C3 A9 (U+00E9), valid UTF-8 */
        check_golden("string-hex-utf8", "\"caf\\xC3\\xA9\"", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_STR_LITERAL, 0, 0, 0, 0, 0, 0, STR_EXP("abcd"), 0, 9 },
        };
        check_golden("string-concat", "\"ab\" \"cd\"", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_STR_LITERAL, 0, 0, 0, 0, 0, 0, STR_EXP("abcd"), 0, 17 },
        };
        check_golden("string-concat-comment", "\"ab\" /* x */ \"cd\"", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_STR_LITERAL, 0, 0, 0, 0, 0, 0, "", 0, 0, 2 },
        };
        check_golden("string-empty", "\"\"", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }
}

static void test_neg_strings(void)
{
    /* 18-1-lex-invalid-escape corpus anchor (expected.json: AIC-L0008 at
     * (2,19,31)-(2,21,33)). */
    check_neg_one("corpus-invalid-escape",
                  "module main;\nvar s: str = \"bad \\q escape\";",
                  "AIC-L0008", "invalid escape sequence",
                  2, 19, 31, 2, 21, 33);

    /* derived-lex-string-newline corpus anchor (expected.json: AIC-L0007 at
     * (2,20,32)-(3,1,33)). */
    check_neg_one("corpus-string-newline",
                  "module main;\nvar s: str = \"hello\nworld\";",
                  "AIC-L0007", "line terminator inside string literal",
                  2, 20, 32, 3, 1, 33);

    /* derived-lex-string-invalid-utf8-escape corpus anchor (expected.json:
     * AIC-L0009 at (2,14,26)-(2,20,32)). */
    check_neg_one("corpus-string-invalid-utf8",
                  "module main;\nvar s: str = \"\\xff\";",
                  "AIC-L0009",
                  "string literal bytes not valid UTF-8 after escape expansion",
                  2, 14, 26, 2, 20, 32);

    /* Invalid \x escape: only one hex digit. Span covers `\x1` (offsets
     * 1..4). */
    check_neg_one("escape-x-one-digit", "\"\\x1\"",
                  "AIC-L0008", "invalid escape sequence",
                  1, 2, 1, 1, 5, 4);

    /* Unterminated string: span from opening quote to EOF. */
    check_neg_one("unterminated-string", "\"abc",
                  "AIC-L0001", "unterminated string literal",
                  1, 1, 0, 1, 5, 4);

    /* Invalid UTF-8 after expansion (raw byte 0xFF is invalid alone). */
    check_neg_one("utf8-single-ff", "\"\\xff\"",
                  "AIC-L0009",
                  "string literal bytes not valid UTF-8 after escape expansion",
                  1, 1, 0, 1, 7, 6);

    /* Raw LF followed by EOF without a closing quote: BOTH failures are
     * reported - the line terminator (AIC-L0007, span the LF) and the
     * unterminated literal (AIC-L0001, span the opening quote..EOF).
     * Sorted by span start offset, the unterminated record sorts first. */
    {
        LoadSource *src = NULL;
        DiagRecord **recs = NULL;
        size_t rn = 0;
        LexToken *toks = NULL;
        size_t tn = 0;
        const char *input = "\"hello\nworld";
        LoadStatus lst;

        lst = load_source_from_bytes("input.ai", (const uint8_t *)input,
                                     strlen(input), &src, &recs, &rn);
        CHECK(lst == LOAD_OK);
        if (lst != LOAD_OK) {
            load_records_free(recs, rn);
            return;
        }
        CHECK(lex_tokenize(src, &toks, &tn, &recs, &rn) == LEX_DIAG_ERROR);
        CHECK(recs != NULL && rn == 2);
        if (recs != NULL && rn >= 2) {
            CHECK(strcmp(recs[0]->code, "AIC-L0001") == 0);
            CHECK(recs[0]->primary_span != NULL);
            if (recs[0]->primary_span != NULL) {
                CHECK(recs[0]->primary_span->start.line == 1 &&
                      recs[0]->primary_span->start.col == 1 &&
                      recs[0]->primary_span->start.offset == 0);
                CHECK(recs[0]->primary_span->end.line == 2 &&
                      recs[0]->primary_span->end.col == 6 &&
                      recs[0]->primary_span->end.offset == 12);
            }
            CHECK(strcmp(recs[1]->code, "AIC-L0007") == 0);
            CHECK(recs[1]->primary_span != NULL);
            if (recs[1]->primary_span != NULL) {
                CHECK(recs[1]->primary_span->start.line == 1 &&
                      recs[1]->primary_span->start.col == 7 &&
                      recs[1]->primary_span->start.offset == 6);
                CHECK(recs[1]->primary_span->end.line == 2 &&
                      recs[1]->primary_span->end.col == 1 &&
                      recs[1]->primary_span->end.offset == 7);
            }
        }
        lex_tokens_free(toks, tn);
        lex_records_free(recs, rn);
        load_source_free(src);
    }
}

/* ---------------------------------------------------------------------------
 * Comments (spec sec. 4.1)
 * ------------------------------------------------------------------------- */

static void test_comments(void)
{
    {
        static const ExpTok ex[] = {
            { TOK_IDENT, 0, 0, 0, 0, 0, "a", 0, 0, 0, 1 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "b", 0, 0, 13, 14 },
        };
        check_golden("line-comment", "a // comment\nb", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    {
        static const ExpTok ex[] = {
            { TOK_IDENT, 0, 0, 0, 0, 0, "a", 0, 0, 0, 1 },
            { TOK_IDENT, 0, 0, 0, 0, 0, "b", 0, 0, 12, 13 },
        };
        check_golden("block-comment", "a /* x\ny */ b", ex,
                     sizeof(ex) / sizeof(ex[0]));
    }

    /* 18-1-lex-unterminated-comment corpus anchor (expected.json: AIC-L0004
     * at (2,1,13)-(2,24,36)). Inline copy uses the fixture's exact input
     * (slash-star space "unterminated comment": 23 chars on line 2 ->
     * exclusive end offset 13+23=36, col 1+23=24). */
    check_neg_one("corpus-unterminated-comment",
                  "module main;\n/* unterminated comment",
                  "AIC-L0004", "unterminated block comment",
                  2, 1, 13, 2, 24, 36);
}

/* ---------------------------------------------------------------------------
 * AIC-L0001 invalid characters (spec sec. 4.6)
 * ------------------------------------------------------------------------- */

static void test_invalid_chars(void)
{
    check_neg_one("invalid-at", "@",
                  "AIC-L0001", "invalid character in source",
                  1, 1, 0, 1, 2, 1);
    check_neg_one("invalid-hash", "#",
                  "AIC-L0001", "invalid character in source",
                  1, 1, 0, 1, 2, 1);
    check_neg_one("invalid-backtick", "`",
                  "AIC-L0001", "invalid character in source",
                  1, 1, 0, 1, 2, 1);
    /* A valid multi-byte code point in code position (not string/comment) is
     * not a token character. */
    check_neg_one("invalid-utf8-in-code", "caf\xC3\xA9",
                  "AIC-L0001", "invalid character in source",
                  1, 4, 3, 1, 6, 5);
}

/* ---------------------------------------------------------------------------
 * Corpus anchor re-execution against the real fixture files
 * ------------------------------------------------------------------------- */

/* Read a fixture file, lex it, and assert exactly one record matching
 * `expected` fields (file name "input.ai" as in the corpus). */
static void check_corpus_fixture(const char *path, const char *code,
                                 const char *message,
                                 int64_t sl, int64_t sc, int64_t so,
                                 int64_t el, int64_t ec, int64_t eo)
{
    FILE *f;
    long sz;
    char *buf;
    size_t rd;
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t rn = 0;
    LexToken *toks = NULL;
    size_t tn = 0;
    LoadStatus lst;
    LexStatus st;

    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
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

    st = lex_tokenize(src, &toks, &tn, &recs, &rn);
    CHECK(st == LEX_DIAG_ERROR);
    CHECK(recs != NULL && rn == 1);
    if (recs != NULL && rn >= 1) {
        const DiagRecord *r = recs[0];
        CHECK(strcmp(r->code, code) == 0);
        CHECK(strcmp(r->severity, "error") == 0);
        CHECK(strcmp(r->phase, "lex") == 0);
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

    lex_tokens_free(toks, tn);
    lex_records_free(recs, rn);
    load_source_free(src);
    free(buf);
}

static void test_corpus_anchors(void)
{
    /* The five lexer-owned corpus fixtures (each expects exactly one record;
     * expected.json values inlined below). derived-lex-misplaced-underscore
     * is intentionally NOT anchored: its input is a spec-valid literal (see
     * test_neg_ints and README) and the fixture is routed to Planner. */
    check_corpus_fixture(
        "tests/negative/cases/18-1-lex-int-overrun/input.ai",
        "AIC-L0006",
        "integer literal 300 is not representable in type u8",
        2, 15, 27, 2, 20, 32);
    check_corpus_fixture(
        "tests/negative/cases/18-1-lex-invalid-escape/input.ai",
        "AIC-L0008", "invalid escape sequence",
        2, 19, 31, 2, 21, 33);
    check_corpus_fixture(
        "tests/negative/cases/18-1-lex-unterminated-comment/input.ai",
        "AIC-L0004", "unterminated block comment",
        2, 1, 13, 2, 24, 36);
    check_corpus_fixture(
        "tests/negative/cases/derived-lex-string-invalid-utf8-escape/input.ai",
        "AIC-L0009",
        "string literal bytes not valid UTF-8 after escape expansion",
        2, 14, 26, 2, 20, 32);
    check_corpus_fixture(
        "tests/negative/cases/derived-lex-string-newline/input.ai",
        "AIC-L0007", "line terminator inside string literal",
        2, 20, 32, 3, 1, 33);
}

/* ---------------------------------------------------------------------------
 * Records validate and emit (contract sec. 3/4/12)
 * ------------------------------------------------------------------------- */

static void test_records_valid_and_emit(void)
{
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t rn = 0;
    LexToken *toks = NULL;
    size_t tn = 0;
    DiagBuf out;
    size_t i;

    CHECK(load_source_from_bytes("input.ai",
                                 (const uint8_t *)"module main;\nvar bad: u8 = 300u8;",
                                 33, &src, &recs, &rn) == LOAD_OK);
    CHECK(src != NULL);
    if (src == NULL) {
        load_records_free(recs, rn);
        return;
    }
    CHECK(lex_tokenize(src, &toks, &tn, &recs, &rn) == LEX_DIAG_ERROR);
    CHECK(recs != NULL && rn == 1);
    if (recs != NULL && rn >= 1) {
        char errbuf[128];
        CHECK(diag_record_validate(recs[0], errbuf, sizeof(errbuf)));
        diag_buf_init(&out);
        CHECK(diag_emit_record(&out, recs[0]));
        CHECK(diag_buf_ok(&out));
        if (out.data != NULL) {
            CHECK(strstr(out.data, "\"code\":\"AIC-L0006\"") != NULL);
            CHECK(strstr(out.data, "\"severity\":\"error\"") != NULL);
            CHECK(strstr(out.data, "\"phase\":\"lex\"") != NULL);
            CHECK(strstr(out.data, "\"recovery\":\"authoritative\"") != NULL);
            CHECK(strstr(out.data, "\"file\":\"input.ai\"") != NULL);
        }
        diag_buf_free(&out);
    }
    lex_tokens_free(toks, tn);
    lex_records_free(recs, rn);
    load_source_free(src);

    /* Deterministic ordering: two records sort by offset (contract sec. 9). */
    CHECK(load_source_from_bytes("input.ai",
                                 (const uint8_t *)"@ 300u8 #",
                                 9, &src, &recs, &rn) == LOAD_OK);
    CHECK(src != NULL);
    if (src == NULL) {
        load_records_free(recs, rn);
        return;
    }
    CHECK(lex_tokenize(src, &toks, &tn, &recs, &rn) == LEX_DIAG_ERROR);
    CHECK(recs != NULL && rn == 3);
    if (recs != NULL && rn >= 3) {
        CHECK(strcmp(recs[0]->code, "AIC-L0001") == 0);
        CHECK(recs[0]->primary_span != NULL &&
              recs[0]->primary_span->start.offset == 0);
        CHECK(strcmp(recs[1]->code, "AIC-L0006") == 0);
        CHECK(recs[1]->primary_span != NULL &&
              recs[1]->primary_span->start.offset == 2);
        CHECK(strcmp(recs[2]->code, "AIC-L0001") == 0);
        CHECK(recs[2]->primary_span != NULL &&
              recs[2]->primary_span->start.offset == 8);
        for (i = 0; i < rn; ++i) {
            char errbuf[128];
            CHECK(diag_record_validate(recs[i], errbuf, sizeof(errbuf)));
        }
    }
    lex_tokens_free(toks, tn);
    lex_records_free(recs, rn);
    load_source_free(src);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_golden_tokens();
    test_golden_punct();
    test_golden_ints();
    test_unary_minus();
    test_neg_ints();
    test_golden_strings();
    test_neg_strings();
    test_comments();
    test_invalid_chars();
    test_corpus_anchors();
    test_records_valid_and_emit();

    lex_tokens_free(NULL, 0);
    lex_records_free(NULL, 0);

    if (g_failures == 0) {
        printf("lex_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "lex_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
