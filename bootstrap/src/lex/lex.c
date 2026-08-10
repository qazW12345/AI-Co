/* bootstrap/src/lex/lex.c
 *
 * WP-M0-08 - Deterministic lexer implementation.
 *
 * Pipeline for one loaded source:
 *   1. Skip whitespace and comments (spec sec. 4.1). Line comments run to
 *      the line terminator (not including it); block comments do not nest;
 *      an unterminated block comment is AIC-L0004 (span from the opening
 *      delimiter to end of file).
 *   2. Lex one token: identifier/keyword, integer literal, string literal,
 *      punctuation (longest match), or report AIC-L0001 for any other
 *      character / malformed token (spec sec. 4.6).
 *   3. Literal typing per spec sec. 4.3 (integer) and sec. 4.4 (string),
 *      including the unary-minus minimum-value rule and escape expansion.
 *   4. Records are sorted with the contract sec. 9 comparator; a best-effort
 *      token stream (always ending in one EOF token) is produced even when
 *      diagnostics exist, so the parser can apply contract sec. 7 recovery.
 *
 * Deterministic recovery choices (documented in README.md):
 *   - A rejected integer literal (L0005/L0006) emits no token; scanning
 *     resumes after the literal.
 *   - A string with an invalid escape (L0008) or invalid UTF-8 (L0009) emits
 *     no STR token; scanning resumes after the closing quote (or EOF).
 *   - A raw LF inside a string (L0007) emits no STR token; the lexer then
 *     scans to the closing quote (discard mode, no further diagnostics for
 *     that string) so the rest of the file lexes normally; EOF before the
 *     closing quote is additionally AIC-L0001 (unterminated).
 *   - A malformed character (L0001) emits no token; scanning resumes after
 *     the offending character / maximal malformed run.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Keyword table (spec sec. 4.2, in source order)
 * ------------------------------------------------------------------------- */

typedef struct KwEntry {
    const char *name;
    LexKeyword kw;
} KwEntry;

static const KwEntry kKeywords[] = {
    { "alignof", KW_ALIGNOF }, { "bool", KW_BOOL }, { "break", KW_BREAK },
    { "case", KW_CASE }, { "cast", KW_CAST }, { "const", KW_CONST },
    { "continue", KW_CONTINUE }, { "default", KW_DEFAULT },
    { "else", KW_ELSE }, { "enum", KW_ENUM }, { "false", KW_FALSE },
    { "fn", KW_FN }, { "for", KW_FOR }, { "i16", KW_I16 },
    { "i32", KW_I32 }, { "i64", KW_I64 }, { "i8", KW_I8 },
    { "if", KW_IF }, { "import", KW_IMPORT }, { "isize", KW_ISIZE },
    { "len", KW_LEN }, { "module", KW_MODULE }, { "null", KW_NULL },
    { "ptr", KW_PTR }, { "pub", KW_PUB }, { "return", KW_RETURN },
    { "sizeof", KW_SIZEOF }, { "str", KW_STR }, { "struct", KW_STRUCT },
    { "switch", KW_SWITCH }, { "true", KW_TRUE }, { "u16", KW_U16 },
    { "u32", KW_U32 }, { "u64", KW_U64 }, { "u8", KW_U8 },
    { "usize", KW_USIZE }, { "var", KW_VAR }, { "void", KW_VOID },
    { "while", KW_WHILE }, { "wrap", KW_WRAP }
};

#define KKEYWORD_COUNT (sizeof(kKeywords) / sizeof(kKeywords[0]))

/* ---------------------------------------------------------------------------
 * Scanner state
 * ------------------------------------------------------------------------- */

typedef struct ByteBuf {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} ByteBuf;

typedef struct LexScan {
    const LoadSource *src;
    const char *text;
    size_t len;

    LexToken *tokens;
    size_t tcount;
    size_t tcap;

    DiagRecord **records;
    size_t rcount;
    size_t rcap;

    bool oom;

    /* Previous significant token, for the unary-minus rule (spec sec. 4.3):
     * a `-` is in unary position iff the token before it cannot end an
     * expression. */
    LexTokenKind prev_kind;
    LexPunct prev_punct;
    LexKeyword prev_kw;
    bool prev_minus_unary;
} LexScan;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_cont(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

/* Digit predicate for a given base (10, 2, 8, 16). */
static bool is_base_digit(char c, int base)
{
    switch (base) {
    case 2:
        return c == '0' || c == '1';
    case 8:
        return c >= '0' && c <= '7';
    case 16:
        return is_hex(c);
    default:
        return is_digit(c);
    }
}

static int digit_value(char c, int base)
{
    (void)base;
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

/* Can `kind` (with punct/keyword detail) end an expression? Used to decide
 * whether a following `-` is unary. Only `true`/`false`/`null` among
 * keywords are primary expressions (spec sec. 5.2). RBRACE is deliberately
 * NOT expression-ending: `{block} -128i8;` is a valid block-statement
 * followed by a unary-minus expression statement, and a struct-init followed
 * by `- 128i8` is a type error the parser rejects anyway (so no valid program
 * is rejected at lex time; see README.md). */
static bool is_expr_ending(LexTokenKind kind, LexPunct punct, LexKeyword kw)
{
    switch (kind) {
    case TOK_IDENT:
    case TOK_INT_LITERAL:
    case TOK_STR_LITERAL:
        return true;
    case TOK_KEYWORD:
        return kw == KW_TRUE || kw == KW_FALSE || kw == KW_NULL;
    case TOK_PUNCT:
        return punct == PUNCT_RPAREN || punct == PUNCT_RBRACKET;
    default:
        return false;
    }
}

/* UTF-8 validation of a decoded byte sequence (spec sec. 3.1 rules: no
 * overlong, no surrogates, no out-of-range, no stray continuation, no
 * truncation). The loader already validated the source bytes; this applies
 * the same rules to bytes produced by `\xHH` escapes. */
static bool utf8_valid(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) {
            i += 1;
            continue;
        }
        if (b >= 0xC2 && b <= 0xDF) {
            if (i + 1 >= len) {
                return false;
            }
            if ((uint8_t)s[i + 1] < 0x80 || (uint8_t)s[i + 1] > 0xBF) {
                return false;
            }
            i += 2;
            continue;
        }
        if (b >= 0xE0 && b <= 0xEF) {
            uint8_t c1;
            uint8_t c2;
            if (i + 2 >= len) {
                return false;
            }
            c1 = (uint8_t)s[i + 1];
            c2 = (uint8_t)s[i + 2];
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF) {
                return false;
            }
            if (b == 0xE0 && c1 < 0xA0) {
                return false; /* overlong */
            }
            if (b == 0xED && c1 >= 0xA0) {
                return false; /* surrogate */
            }
            i += 3;
            continue;
        }
        if (b >= 0xF0 && b <= 0xF4) {
            uint8_t c1;
            uint8_t c2;
            uint8_t c3;
            if (i + 3 >= len) {
                return false;
            }
            c1 = (uint8_t)s[i + 1];
            c2 = (uint8_t)s[i + 2];
            c3 = (uint8_t)s[i + 3];
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF ||
                c3 < 0x80 || c3 > 0xBF) {
                return false;
            }
            if (b == 0xF0 && c1 < 0x90) {
                return false; /* overlong */
            }
            if (b == 0xF4 && c1 >= 0x90) {
                return false; /* out of range */
            }
            i += 4;
            continue;
        }
        return false; /* invalid lead or stray continuation */
    }
    return true;
}

/* Byte buffer (for decoded string bytes; may contain NULs). */
static void bb_init(ByteBuf *b)
{
    memset(b, 0, sizeof(*b));
}

static bool bb_append(ByteBuf *b, char c)
{
    char *p;
    if (b->oom) {
        return false;
    }
    if (b->len == b->cap) {
        size_t ncap = b->cap == 0 ? 16 : b->cap * 2;
        p = (char *)realloc(b->data, ncap);
        if (p == NULL) {
            b->oom = true;
            return false;
        }
        b->data = p;
        b->cap = ncap;
    }
    b->data[b->len++] = c;
    return true;
}

static void bb_free(ByteBuf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

/* ---------------------------------------------------------------------------
 * Record / token emission
 * ------------------------------------------------------------------------- */

static bool scan_add_record(LexScan *s, const char *code, const char *message,
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
    sp = load_span_range(s->src, (int64_t)start, (int64_t)end);
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

    if (s->rcount == s->rcap) {
        size_t ncap = s->rcap == 0 ? 8 : s->rcap * 2;
        arr = (DiagRecord **)realloc(s->records, ncap * sizeof(*arr));
        if (arr == NULL) {
            diag_record_free(r);
            s->oom = true;
            return false;
        }
        s->records = arr;
        s->rcap = ncap;
    }
    s->records[s->rcount++] = r;
    return true;
}

static bool scan_emit(LexScan *s, const LexToken *tok)
{
    LexToken *arr;
    bool minus_unary;

    if (s->oom) {
        return false;
    }

    /* Compute the unary-minus context for the NEXT token. */
    minus_unary = (tok->kind == TOK_PUNCT && tok->u.punct == PUNCT_MINUS &&
                   !is_expr_ending(s->prev_kind, s->prev_punct, s->prev_kw));
    s->prev_minus_unary = minus_unary;
    s->prev_kind = tok->kind;
    s->prev_punct = (tok->kind == TOK_PUNCT) ? tok->u.punct : PUNCT_NONE;
    s->prev_kw = (tok->kind == TOK_KEYWORD) ? tok->u.keyword : KW_NONE;

    if (s->tcount == s->tcap) {
        size_t ncap = s->tcap == 0 ? 16 : s->tcap * 2;
        arr = (LexToken *)realloc(s->tokens, ncap * sizeof(*arr));
        if (arr == NULL) {
            s->oom = true;
            return false;
        }
        s->tokens = arr;
        s->tcap = ncap;
    }
    s->tokens[s->tcount++] = *tok;
    return true;
}

static LexToken make_token(LexTokenKind kind, const LoadSource *src,
                           size_t start, size_t end)
{
    LexToken t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.span = load_span_range(src, (int64_t)start, (int64_t)end);
    return t;
}

/* ---------------------------------------------------------------------------
 * Whitespace and comments
 * ------------------------------------------------------------------------- */

/* Advance *pos past whitespace and comments. On an unterminated block
 * comment, report AIC-L0004 (span from the opening delimiter to EOF) and
 * advance to EOF. */
static void skip_trivia(LexScan *s, size_t *pos)
{
    while (*pos < s->len) {
        char c = s->text[*pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            *pos += 1;
            continue;
        }
        if (c == '/' && *pos + 1 < s->len) {
            char c1 = s->text[*pos + 1];
            if (c1 == '/') {
                /* line comment: through the end of the line, not including
                 * the terminator */
                *pos += 2;
                while (*pos < s->len && s->text[*pos] != '\n') {
                    *pos += 1;
                }
                continue;
            }
            if (c1 == '*') {
                size_t cstart = *pos;
                *pos += 2;
                while (*pos + 1 < s->len &&
                       !(s->text[*pos] == '*' && s->text[*pos + 1] == '/')) {
                    *pos += 1;
                }
                if (*pos + 1 < s->len) {
                    *pos += 2; /* consumed the closing */
                    continue;
                }
                /* unterminated: report and consume to EOF */
                scan_add_record(s, "AIC-L0004", "unterminated block comment",
                                cstart, s->len);
                *pos = s->len;
                continue;
            }
        }
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Identifiers and keywords
 * ------------------------------------------------------------------------- */

static void scan_ident(LexScan *s, size_t *pos)
{
    size_t start = *pos;
    LexKeyword kw = KW_NONE;
    size_t k;
    LexToken t;

    while (*pos < s->len && is_ident_cont(s->text[*pos])) {
        *pos += 1;
    }

    for (k = 0; k < KKEYWORD_COUNT; ++k) {
        size_t n = strlen(kKeywords[k].name);
        if (*pos - start == n &&
            memcmp(s->text + start, kKeywords[k].name, n) == 0) {
            kw = kKeywords[k].kw;
            break;
        }
    }

    if (kw != KW_NONE) {
        t = make_token(TOK_KEYWORD, s->src, start, *pos);
        t.u.keyword = kw;
    } else {
        char *copy = (char *)malloc(*pos - start + 1);
        if (copy == NULL) {
            s->oom = true;
            return;
        }
        memcpy(copy, s->text + start, *pos - start);
        copy[*pos - start] = '\0';
        t = make_token(TOK_IDENT, s->src, start, *pos);
        t.u.ident = copy;
    }
    scan_emit(s, &t);
}

/* ---------------------------------------------------------------------------
 * Integer literals (spec sec. 4.3)
 * ------------------------------------------------------------------------- */

typedef struct IntSuffix {
    const char *name;
    LexIntType type;
    int width;      /* bits */
    bool is_signed;
} IntSuffix;

static const IntSuffix kSuffixes[] = {
    { "i8", LEX_INT_I8, 8, true },
    { "i16", LEX_INT_I16, 16, true },
    { "i32", LEX_INT_I32, 32, true },
    { "i64", LEX_INT_I64, 64, true },
    { "u8", LEX_INT_U8, 8, false },
    { "u16", LEX_INT_U16, 16, false },
    { "u32", LEX_INT_U32, 32, false },
    { "u64", LEX_INT_U64, 64, false },
    { "isize", LEX_INT_ISIZE, 64, true },
    { "usize", LEX_INT_USIZE, 64, false }
};

#define KSUFFIX_COUNT (sizeof(kSuffixes) / sizeof(kSuffixes[0]))

/* The suffix must be followed by a non-identifier character (word boundary),
 * otherwise the letters are the start of an identifier (e.g. `123i8x` is
 * INT(123) IDENT(i8x), not a suffixed literal). */
static bool match_suffix(const char *text, size_t len, size_t i,
                         const IntSuffix **out)
{
    size_t k;
    for (k = 0; k < KSUFFIX_COUNT; ++k) {
        size_t n = strlen(kSuffixes[k].name);
        if (i + n <= len && memcmp(text + i, kSuffixes[k].name, n) == 0) {
            if (i + n < len && is_ident_cont(text[i + n])) {
                continue; /* not a word boundary */
            }
            *out = &kSuffixes[k];
            return true;
        }
    }
    return false;
}

/* Build the "literal text" (digits, underscores stripped) for messages. */
static void literal_text(const char *text, size_t start, size_t end,
                         char *out, size_t outcap)
{
    size_t i;
    size_t n = 0;
    for (i = start; i < end && n + 1 < outcap; ++i) {
        if (text[i] != '_') {
            out[n++] = text[i];
        }
    }
    out[n] = '\0';
}

/* Maximum magnitude for a signed type: normally 2^(width-1) - 1; with the
 * unary-minus exception, 2^(width-1). Returns false when value does not fit. */
static bool signed_magnitude_fits(const IntSuffix *sfx, uint64_t value,
                                  bool unary_minus)
{
    uint64_t limit;
    if (sfx->width >= 64) {
        limit = unary_minus ? UINT64_C(9223372036854775808)
                            : UINT64_C(9223372036854775807);
    } else {
        limit = unary_minus ? (UINT64_C(1) << (sfx->width - 1))
                            : ((UINT64_C(1) << (sfx->width - 1)) - 1);
    }
    return value <= limit;
}

static bool unsigned_magnitude_fits(const IntSuffix *sfx, uint64_t value)
{
    uint64_t limit;
    if (sfx->width >= 64) {
        return true; /* uint64 holds the value by construction */
    }
    limit = (UINT64_C(1) << sfx->width) - 1;
    return value <= limit;
}

static void scan_integer(LexScan *s, size_t *pos)
{
    size_t start = *pos;
    size_t i = start;
    int base = 10;
    size_t digit_start;
    size_t digit_end;
    const IntSuffix *sfx = NULL;
    uint64_t value = 0;
    bool overflow = false;
    bool has_underscore_error = false;
    LexToken t;
    char litbuf[160];
    char msgbuf[256];

    /* Prefix. */
    if (s->text[i] == '0' && i + 1 < s->len) {
        char p = s->text[i + 1];
        if (p == 'x' || p == 'X') {
            base = 16;
            i += 2;
        } else if (p == 'b' || p == 'B') {
            base = 2;
            i += 2;
        } else if (p == 'o' || p == 'O') {
            base = 8;
            i += 2;
        }
    }

    digit_start = i;

    /* Digit / underscore run. */
    while (i < s->len && (is_base_digit(s->text[i], base) ||
                          s->text[i] == '_')) {
        i += 1;
    }
    digit_end = i;

    /* The literal must contain at least one actual base digit (`0x`, `0b`,
     * `0o` with no digits, or `0x_` with only underscores, are malformed
     * tokens, not misplaced-underscore cases). */
    {
        size_t k;
        bool has_digit = false;
        for (k = digit_start; k < digit_end; ++k) {
            if (s->text[k] != '_') {
                has_digit = true;
                break;
            }
        }
        if (!has_digit) {
            scan_add_record(s, "AIC-L0001", "malformed integer literal",
                            start, digit_end);
            *pos = digit_end;
            return;
        }
    }

    /* Decimal: `0` alone is the decimal zero (spec sec. 4.3). A bare decimal
     * starting with `0` and containing any further digit or underscore is a
     * malformed token, never a valid literal. */
    if (base == 10 && s->text[start] == '0' &&
        (digit_end - digit_start) > 1) {
        scan_add_record(s, "AIC-L0001", "malformed integer literal",
                        start, digit_end);
        *pos = digit_end;
        return;
    }

    /* Underscore placement: `_` must sit between two digits. */
    {
        size_t k;
        for (k = digit_start; k < digit_end; ++k) {
            if (s->text[k] == '_') {
                bool left_ok = k > digit_start &&
                               is_base_digit(s->text[k - 1], base);
                bool right_ok = k + 1 < digit_end &&
                                is_base_digit(s->text[k + 1], base);
                if (!left_ok || !right_ok) {
                    has_underscore_error = true;
                    break;
                }
            }
        }
    }

    /* Optional suffix (word boundary enforced). */
    if (i < s->len && (s->text[i] == 'i' || s->text[i] == 'u')) {
        const IntSuffix *cand = NULL;
        if (match_suffix(s->text, s->len, i, &cand)) {
            sfx = cand;
            i += strlen(sfx->name);
        }
    }

    if (has_underscore_error) {
        scan_add_record(s, "AIC-L0005", "misplaced '_' in integer literal",
                        start, i);
        *pos = i;
        return;
    }

    /* Value. */
    {
        size_t k;
        for (k = digit_start; k < digit_end; ++k) {
            int d;
            if (s->text[k] == '_') {
                continue;
            }
            d = digit_value(s->text[k], base);
            if (value > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
                overflow = true;
                break;
            }
            value = value * (uint64_t)base + (uint64_t)d;
        }
    }

    /* Typing. */
    if (sfx == NULL) {
        LexIntType type;
        if (!overflow && value <= (uint64_t)INT32_MAX) {
            type = LEX_INT_I32;
        } else if (!overflow && value <= (uint64_t)INT64_MAX) {
            type = LEX_INT_I64;
        } else if (!overflow) {
            type = LEX_INT_U64;
        } else {
            literal_text(s->text, start, digit_end, litbuf, sizeof(litbuf));
            snprintf(msgbuf, sizeof(msgbuf),
                     "integer literal %s is not representable in any integer type",
                     litbuf);
            scan_add_record(s, "AIC-L0006", msgbuf, start, i);
            *pos = i;
            return;
        }
        t = make_token(TOK_INT_LITERAL, s->src, start, i);
        t.u.integer.type = type;
        t.u.integer.value = value;
        t.u.integer.is_min = false;
        scan_emit(s, &t);
        *pos = i;
        return;
    }

    /* Suffixed. */
    {
        bool fits;
        bool unary_minus = s->prev_minus_unary;
        if (sfx->is_signed) {
            fits = !overflow && signed_magnitude_fits(sfx, value, unary_minus);
        } else {
            fits = !overflow && unsigned_magnitude_fits(sfx, value);
        }
        if (!fits) {
            literal_text(s->text, start, digit_end, litbuf, sizeof(litbuf));
            snprintf(msgbuf, sizeof(msgbuf),
                     "integer literal %s is not representable in type %s",
                     litbuf, lex_int_type_text(sfx->type));
            scan_add_record(s, "AIC-L0006", msgbuf, start, i);
            *pos = i;
            return;
        }
        t = make_token(TOK_INT_LITERAL, s->src, start, i);
        t.u.integer.type = sfx->type;
        t.u.integer.value = value;
        t.u.integer.is_min = sfx->is_signed && unary_minus &&
                             (sfx->width >= 64
                                  ? value == UINT64_C(9223372036854775808)
                                  : value == (UINT64_C(1) << (sfx->width - 1)));
        scan_emit(s, &t);
    }
    *pos = i;
}

/* ---------------------------------------------------------------------------
 * String literals (spec sec. 4.4)
 * ------------------------------------------------------------------------- */

/* Scan one string literal starting at the opening quote at `start`.
 * Appends decoded bytes to `bytes` while the literal is still valid. Returns
 * true when the literal was valid and bytes are complete; on invalid
 * literals the diagnostics are already recorded and `*end` is the recovery
 * position (after the closing quote, or EOF). */
static bool scan_string_one(LexScan *s, size_t start, ByteBuf *bytes,
                            size_t *end)
{
    size_t i = start + 1;
    bool invalid = false;
    bool discard = false; /* after a raw LF: scan to closing quote silently */

    while (i < s->len) {
        char c = s->text[i];
        if (c == '"') {
            *end = i + 1;
            goto done;
        }
        if (c == '\n') {
            if (!discard) {
                scan_add_record(s, "AIC-L0007",
                                "line terminator inside string literal",
                                i, i + 1);
                invalid = true;
                discard = true;
            }
            i += 1;
            continue;
        }
        if (c == '\\') {
            size_t esc = i;
            if (i + 1 >= s->len) {
                /* trailing backslash at EOF: unterminated (reported even in
                 * discard mode; the header contract promises AIC-L0001 for
                 * EOF before the closing quote) */
                scan_add_record(s, "AIC-L0001",
                                "unterminated string literal",
                                start, s->len);
                *end = s->len;
                return false;
            }
            {
                char e = s->text[i + 1];
                switch (e) {
                case '0':
                    if (!discard) {
                        bb_append(bytes, '\0');
                    }
                    i += 2;
                    break;
                case 'n':
                    if (!discard) {
                        bb_append(bytes, '\n');
                    }
                    i += 2;
                    break;
                case 'r':
                    if (!discard) {
                        bb_append(bytes, '\r');
                    }
                    i += 2;
                    break;
                case 't':
                    if (!discard) {
                        bb_append(bytes, '\t');
                    }
                    i += 2;
                    break;
                case '\\':
                    if (!discard) {
                        bb_append(bytes, '\\');
                    }
                    i += 2;
                    break;
                case '"':
                    if (!discard) {
                        bb_append(bytes, '"');
                    }
                    i += 2;
                    break;
                case 'x': {
                    size_t h1 = i + 2;
                    size_t h2 = i + 3;
                    if (h2 < s->len && is_hex(s->text[h1]) &&
                        is_hex(s->text[h2])) {
                        if (!discard) {
                            int v = hex_value(s->text[h1]) * 16 +
                                    hex_value(s->text[h2]);
                            bb_append(bytes, (char)v);
                        }
                        i += 4;
                    } else {
                        /* invalid \x escape: span = `\x` + hex digits present */
                        size_t span_end = i + 2;
                        while (span_end < s->len && span_end < i + 4 &&
                               is_hex(s->text[span_end])) {
                            span_end += 1;
                        }
                        if (!discard) {
                            scan_add_record(s, "AIC-L0008",
                                            "invalid escape sequence",
                                            esc, span_end);
                            invalid = true;
                        }
                        i = span_end;
                    }
                    break;
                }
                default:
                    if (!discard) {
                        scan_add_record(s, "AIC-L0008",
                                        "invalid escape sequence", esc, i + 2);
                        invalid = true;
                    }
                    i += 2;
                    break;
                }
            }
            continue;
        }
        /* any other byte (valid UTF-8 code point byte, incl. NUL) */
        if (!discard) {
            bb_append(bytes, c);
        }
        i += 1;
    }

    /* EOF without closing quote: the literal is unterminated regardless of
     * whether a raw LF was already reported (header contract). */
    scan_add_record(s, "AIC-L0001", "unterminated string literal",
                    start, s->len);
    *end = s->len;
    return false;

done:
    if (bytes->oom) {
        s->oom = true;
        return false;
    }
    if (invalid) {
        return false;
    }
    /* Concatenation is handled by the caller; UTF-8 validity of THIS
     * literal's bytes is checked by the caller after each component. */
    return true;
}

static void scan_string(LexScan *s, size_t *pos)
{
    size_t start = *pos;
    size_t end;
    ByteBuf bytes;
    LexToken t;

    bb_init(&bytes);

    if (!scan_string_one(s, start, &bytes, &end)) {
        bb_free(&bytes);
        *pos = end;
        return;
    }

    /* Adjacent string literals separated only by whitespace/comments are
     * concatenated at compile time (spec sec. 4.4). */
    for (;;) {
        size_t p = end;
        size_t comp_end;
        ByteBuf comp;
        skip_trivia(s, &p);
        if (p >= s->len || s->text[p] != '"') {
            break;
        }
        bb_init(&comp);
        if (!scan_string_one(s, p, &comp, &comp_end)) {
            bb_free(&comp);
            bb_free(&bytes);
            *pos = comp_end;
            return;
        }
        /* append component bytes */
        {
            size_t k;
            for (k = 0; k < comp.len; ++k) {
                if (!bb_append(&bytes, comp.data[k])) {
                    bb_free(&comp);
                    bb_free(&bytes);
                    s->oom = true;
                    *pos = comp_end;
                    return;
                }
            }
        }
        bb_free(&comp);
        end = comp_end;
    }

    /* UTF-8 validity of the decoded (concatenated) bytes. */
    if (!utf8_valid(bytes.data, bytes.len)) {
        scan_add_record(s, "AIC-L0009",
                        "string literal bytes not valid UTF-8 after escape expansion",
                        start, end);
        bb_free(&bytes);
        *pos = end;
        return;
    }

    t = make_token(TOK_STR_LITERAL, s->src, start, end);
    t.u.string.bytes = bytes.data;
    t.u.string.len = bytes.len;
    scan_emit(s, &t);
    *pos = end;
}

/* ---------------------------------------------------------------------------
 * Punctuation (spec sec. 4.6; longest match)
 * ------------------------------------------------------------------------- */

typedef struct PunctEntry {
    const char *text;
    LexPunct punct;
} PunctEntry;

static const PunctEntry kPuncts[] = {
    { "<<=", PUNCT_SHL_ASSIGN }, { ">>=", PUNCT_SHR_ASSIGN },
    { "->", PUNCT_ARROW },       { "..", PUNCT_DOT_DOT },
    { "<=", PUNCT_LE },          { ">=", PUNCT_GE },
    { "==", PUNCT_EQ },          { "!=", PUNCT_NE },
    { "&&", PUNCT_AND_AND },     { "||", PUNCT_OR_OR },
    { "+=", PUNCT_PLUS_ASSIGN }, { "-=", PUNCT_MINUS_ASSIGN },
    { "*=", PUNCT_STAR_ASSIGN }, { "/=", PUNCT_SLASH_ASSIGN },
    { "%=", PUNCT_PERCENT_ASSIGN },
    { "<<", PUNCT_SHL },         { ">>", PUNCT_SHR },
    { "&=", PUNCT_AND_ASSIGN },  { "|=", PUNCT_OR_ASSIGN },
    { "^=", PUNCT_XOR_ASSIGN },
    { "(", PUNCT_LPAREN },       { ")", PUNCT_RPAREN },
    { "{", PUNCT_LBRACE },       { "}", PUNCT_RBRACE },
    { "[", PUNCT_LBRACKET },     { "]", PUNCT_RBRACKET },
    { ";", PUNCT_SEMI },         { ",", PUNCT_COMMA },
    { ":", PUNCT_COLON },        { ".", PUNCT_DOT },
    { "*", PUNCT_STAR },         { "&", PUNCT_AMP },
    { "+", PUNCT_PLUS },         { "-", PUNCT_MINUS },
    { "~", PUNCT_TILDE },        { "!", PUNCT_BANG },
    { "/", PUNCT_SLASH },        { "%", PUNCT_PERCENT },
    { "<", PUNCT_LT },           { ">", PUNCT_GT },
    { "=", PUNCT_ASSIGN },       { "?", PUNCT_QUESTION },
    { "|", PUNCT_OR },           { "^", PUNCT_XOR },
};

#define KPUNCT_COUNT (sizeof(kPuncts) / sizeof(kPuncts[0]))

static void scan_punct(LexScan *s, size_t *pos)
{
    size_t k;
    LexToken t;

    for (k = 0; k < KPUNCT_COUNT; ++k) {
        size_t n = strlen(kPuncts[k].text);
        if (*pos + n <= s->len &&
            memcmp(s->text + *pos, kPuncts[k].text, n) == 0) {
            t = make_token(TOK_PUNCT, s->src, *pos, *pos + n);
            t.u.punct = kPuncts[k].punct;
            scan_emit(s, &t);
            *pos += n;
            return;
        }
    }

    /* Any other character: AIC-L0001, span the offending character (the
     * loader guarantees valid UTF-8, so a non-ASCII code point spans its
     * full byte length; a stray byte spans one byte). */
    {
        size_t n = 1;
        uint8_t b = (uint8_t)s->text[*pos];
        if (b >= 0xC2 && b <= 0xDF) {
            n = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            n = 3;
        } else if (b >= 0xF0 && b <= 0xF4) {
            n = 4;
        }
        if (*pos + n > s->len) {
            n = s->len - *pos;
        }
        scan_add_record(s, "AIC-L0001", "invalid character in source",
                        *pos, *pos + n);
        *pos += n;
    }
}

/* ---------------------------------------------------------------------------
 * Main scan
 * ------------------------------------------------------------------------- */

static void lex_run(LexScan *s)
{
    size_t pos = 0;
    LexToken t;

    for (;;) {
        skip_trivia(s, &pos);
        if (s->oom) {
            return;
        }
        if (pos >= s->len) {
            break;
        }
        {
            char c = s->text[pos];
            if (is_ident_start(c)) {
                scan_ident(s, &pos);
            } else if (is_digit(c)) {
                scan_integer(s, &pos);
            } else if (c == '"') {
                scan_string(s, &pos);
            } else {
                scan_punct(s, &pos);
            }
        }
        if (s->oom) {
            return;
        }
    }

    t = make_token(TOK_EOF, s->src, pos, pos);
    scan_emit(s, &t);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

LexStatus lex_tokenize(const LoadSource *src,
                       LexToken **out_tokens, size_t *out_token_count,
                       DiagRecord ***out_records, size_t *out_record_count)
{
    LexScan s;

    if (src == NULL || out_tokens == NULL || out_token_count == NULL ||
        out_records == NULL || out_record_count == NULL) {
        return LEX_OOM;
    }
    *out_tokens = NULL;
    *out_token_count = 0;
    *out_records = NULL;
    *out_record_count = 0;

    memset(&s, 0, sizeof(s));
    s.src = src;
    s.text = src->text;
    s.len = src->len;
    s.prev_kind = TOK_EOF;

    lex_run(&s);

    if (s.oom) {
        lex_tokens_free(s.tokens, s.tcount);
        lex_records_free(s.records, s.rcount);
        return LEX_OOM;
    }

    if (s.rcount > 0) {
        diag_sort_records(s.records, s.rcount);
        *out_records = s.records;
        *out_record_count = s.rcount;
    } else {
        free(s.records);
    }

    *out_tokens = s.tokens;
    *out_token_count = s.tcount;
    return s.rcount > 0 ? LEX_DIAG_ERROR : LEX_OK;
}

void lex_tokens_free(LexToken *tokens, size_t count)
{
    size_t i;
    if (tokens == NULL) {
        return;
    }
    for (i = 0; i < count; ++i) {
        diag_span_free(tokens[i].span);
        if (tokens[i].kind == TOK_IDENT) {
            free(tokens[i].u.ident);
        } else if (tokens[i].kind == TOK_STR_LITERAL) {
            free(tokens[i].u.string.bytes);
        }
    }
    free(tokens);
}

void lex_records_free(DiagRecord **records, size_t count)
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

const char *lex_keyword_text(LexKeyword kw)
{
    size_t k;
    for (k = 0; k < KKEYWORD_COUNT; ++k) {
        if (kKeywords[k].kw == kw) {
            return kKeywords[k].name;
        }
    }
    return NULL;
}

const char *lex_punct_text(LexPunct p)
{
    size_t k;
    for (k = 0; k < KPUNCT_COUNT; ++k) {
        if (kPuncts[k].punct == p) {
            return kPuncts[k].text;
        }
    }
    return NULL;
}

const char *lex_int_type_text(LexIntType t)
{
    size_t k;
    for (k = 0; k < KSUFFIX_COUNT; ++k) {
        if (kSuffixes[k].type == t) {
            return kSuffixes[k].name;
        }
    }
    return NULL;
}
