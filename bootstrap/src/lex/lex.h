/* bootstrap/src/lex/lex.h
 *
 * WP-M0-08 - Deterministic lexer.
 *
 * Consumes a WP-M0-07 LoadSource (line-terminator-normalized UTF-8 text) and
 * produces the deterministic token stream plus lex-level diagnostics
 * (AIC-L0001, AIC-L0004..AIC-L0009) per spec sec. 4 and the diagnostic
 * contract sec. 11.1.
 *
 * Token model (spec sec. 4.6): identifiers, keywords, integer literals,
 * string literals, and punctuation. Every token carries its source span.
 * Integer literal tokens carry the resolved type and the magnitude (unsigned
 * value); when the literal was accepted via the unary-minus minimum-value
 * rule (spec sec. 4.3) the `is_min` flag is set and `value` equals
 * 2^(width-1). String literal tokens carry the decoded bytes (escape
 * expansion applied; adjacent literals already concatenated per spec
 * sec. 4.4) and their byte length.
 *
 * Diagnostics (spec sec. 4, contract sec. 11.1; all phase "lex", severity
 * "error", recovery "authoritative"):
 *   AIC-L0001  any other character or malformed token. Primary span: the
 *              offending character or maximal malformed run.
 *   AIC-L0004  unterminated block comment. Primary span: from the opening
 *              delimiter to end of file.
 *   AIC-L0005  misplaced `_` in an integer literal. Primary span: the
 *              literal.
 *   AIC-L0006  integer literal value not representable in its type. Primary
 *              span: the literal.
 *   AIC-L0007  line terminator inside a string literal. Primary span: the
 *              terminator.
 *   AIC-L0008  invalid escape sequence. Primary span: the escape.
 *   AIC-L0009  string literal bytes not valid UTF-8 after escape expansion.
 *              Primary span: the literal.
 *
 * Behavior contract (documented in README.md):
 *   - The lexer assumes a LOAD_OK source (the pipeline runs load first and
 *     stops on LOAD_VALIDATION_ERROR). BOM / NUL-in-code / invalid-UTF-8
 *     rejections (AIC-L0001..L0003) are the loader's; they are not re-emitted
 *     here.
 *   - All lex-level failures in a file are reported; records are sorted with
 *     the contract sec. 9 comparator before being returned.
 *   - Even when diagnostics exist, a best-effort token stream is produced
 *     (malformed constructs emit no token and scanning continues
 *     deterministically) so the parser can apply contract sec. 7 recovery.
 *     The token stream always ends with one EOF token.
 *   - Comments are whitespace (spec sec. 4.1): an empty block comment
 *     between two identifiers yields two tokens (`a`, `b`).
 *
 * Ownership:
 *   - On LEX_OK / LEX_DIAG_ERROR, *out_tokens / *out_token_count and
 *     *out_records / *out_record_count are owned by the caller
 *     (lex_tokens_free / lex_records_free; records is NULL when count is 0).
 *   - On LEX_OOM nothing is allocated.
 */
#ifndef AICO_BOOTSTRAP_SRC_LEX_LEX_H
#define AICO_BOOTSTRAP_SRC_LEX_LEX_H

#include "../diag/diag.h"
#include "../load/load.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum LexStatus {
    LEX_OK = 0,            /* token stream produced, no diagnostics */
    LEX_DIAG_ERROR,        /* token stream produced AND diagnostics exist */
    LEX_OOM                /* allocation failure; nothing produced */
} LexStatus;

typedef enum LexTokenKind {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_KEYWORD,
    TOK_INT_LITERAL,
    TOK_STR_LITERAL,
    TOK_PUNCT
} LexTokenKind;

/* Reserved words (spec sec. 4.2). */
typedef enum LexKeyword {
    KW_NONE = 0,
    KW_MODULE, KW_IMPORT, KW_PUB,
    KW_VAR, KW_CONST, KW_FN, KW_STRUCT, KW_ENUM,
    KW_IF, KW_ELSE, KW_WHILE, KW_FOR,
    KW_BREAK, KW_CONTINUE, KW_RETURN,
    KW_SWITCH, KW_CASE, KW_DEFAULT,
    KW_TRUE, KW_FALSE, KW_NULL,
    KW_VOID, KW_BOOL,
    KW_I8, KW_I16, KW_I32, KW_I64,
    KW_U8, KW_U16, KW_U32, KW_U64,
    KW_ISIZE, KW_USIZE, KW_STR,
    KW_SIZEOF, KW_ALIGNOF, KW_CAST, KW_WRAP, KW_LEN, KW_PTR
} LexKeyword;

/* Punctuation (spec sec. 4.6). */
typedef enum LexPunct {
    PUNCT_NONE = 0,
    PUNCT_LPAREN, PUNCT_RPAREN, PUNCT_LBRACE, PUNCT_RBRACE,
    PUNCT_LBRACKET, PUNCT_RBRACKET,
    PUNCT_SEMI, PUNCT_COMMA, PUNCT_COLON, PUNCT_DOT,
    PUNCT_ARROW,                      /* -> */
    PUNCT_STAR, PUNCT_AMP, PUNCT_PLUS, PUNCT_MINUS,
    PUNCT_TILDE, PUNCT_BANG,
    PUNCT_SLASH, PUNCT_PERCENT,
    PUNCT_LT, PUNCT_LE, PUNCT_GT, PUNCT_GE, PUNCT_EQ, PUNCT_NE,
    PUNCT_AND_AND, PUNCT_OR_OR,
    PUNCT_ASSIGN,
    PUNCT_PLUS_ASSIGN, PUNCT_MINUS_ASSIGN, PUNCT_STAR_ASSIGN,
    PUNCT_SLASH_ASSIGN, PUNCT_PERCENT_ASSIGN,
    PUNCT_SHL_ASSIGN, PUNCT_SHR_ASSIGN, PUNCT_AND_ASSIGN, PUNCT_OR_ASSIGN,
    PUNCT_XOR_ASSIGN,
    PUNCT_SHL, PUNCT_SHR,
    PUNCT_OR, PUNCT_XOR,             /* | ^ (bitwise; spec sec. 5.2 grammar) */
    PUNCT_QUESTION, PUNCT_DOT_DOT       /* .. (slice expressions) */
} LexPunct;

/* Integer literal types (spec sec. 4.3 / sec. 7.1; isize/usize are 64-bit
 * on the initial target). */
typedef enum LexIntType {
    LEX_INT_I8 = 0, LEX_INT_I16, LEX_INT_I32, LEX_INT_I64,
    LEX_INT_U8, LEX_INT_U16, LEX_INT_U32, LEX_INT_U64,
    LEX_INT_ISIZE, LEX_INT_USIZE
} LexIntType;

typedef struct LexToken {
    LexTokenKind kind;
    DiagSpan *span;                 /* owned */
    union {
        char *ident;                /* TOK_IDENT: owned copy of the identifier */
        LexKeyword keyword;         /* TOK_KEYWORD */
        struct {
            LexIntType type;        /* resolved literal type */
            uint64_t value;         /* magnitude (unsigned value) */
            bool is_min;            /* accepted via unary-minus rule;
                                     * value == 2^(width-1) */
        } integer;                  /* TOK_INT_LITERAL */
        struct {
            char *bytes;            /* decoded bytes, owned; may contain NULs */
            size_t len;             /* byte length of bytes */
        } string;                   /* TOK_STR_LITERAL */
        LexPunct punct;             /* TOK_PUNCT */
    } u;
} LexToken;

/* Tokenize a loaded source. src must be a LOAD_OK LoadSource. */
LexStatus lex_tokenize(const LoadSource *src,
                       LexToken **out_tokens, size_t *out_token_count,
                       DiagRecord ***out_records, size_t *out_record_count);

void lex_tokens_free(LexToken *tokens, size_t count);
void lex_records_free(DiagRecord **records, size_t count);

/* Text lookup helpers (keywords/punctuation/type names). Return the exact
 * source spelling, or NULL for the NONE sentinels. */
const char *lex_keyword_text(LexKeyword kw);
const char *lex_punct_text(LexPunct p);
const char *lex_int_type_text(LexIntType t);

#endif /* AICO_BOOTSTRAP_SRC_LEX_LEX_H */
