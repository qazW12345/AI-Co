/* bootstrap/src/parse/parse.c
 *
 * AI-Co Stage-0 recursive-descent parser (WP-M0-09).
 *
 * Parses the deterministic token stream (WP-M0-08) against the EBNF of
 * spec sec. 5.2 into the single-meaning AST (bootstrap/src/ast/ast.h).
 *
 * Determinism and recovery
 *   - Left-to-right, single-pass, no backtracking.
 *   - Every parse error produces an AIC-S01xx record (phase=syntax) with an
 *     exact span. The FIRST syntax record in a file is recovery=authoritative;
 *     every later syntax record is recovery=recovery_derived (the parser is
 *     in a recovery state after the first failure). Recovery itself never
 *     emits records; it consumes tokens to a deterministic resync point.
 *   - A construct that fails to parse is dropped from the AST entirely; the
 *     AST contains only fully-parsed constructs. The driver stops on parse
 *     errors, so a partial AST is only used for recovery/dump testing.
 *
 * Ambiguity resolution (acceptance: "resolve exactly per the normative
 * resolution rules")
 *   - `sizeof` operand type-vs-expression: per spec sec. 5.2 note + sec. 6.2
 *     single name space. A primitive type keyword -> type. An identifier that
 *     names a module-level struct/enum (pre-scanned, order-independent) and is
 *     not shadowed by a visible local value at the decision point -> type.
 *     Everything else (including undeclared identifiers, which the literal
 *     rule classifies as non-types) -> expression.
 *   - struct-init postfix `{...}` binds as a postfix operator (spec sec. 5.2);
 *     a `{` in expression primary position is a brace-initializer without a
 *     type name (AIC-S0101).
 *   - Two identifiers separated only by a block comment (for example `a`
 *     followed by a comment then `b`) parse as two identifiers; comments
 *     are whitespace (lexer trivia).
 *   - Ternary is right-associative; assignment is right-associative;
 *     binary levels are left-associative per spec sec. 10.1.
 *   - Type postfixes bind tighter than array-element pointers: u8*[4] is
 *     array-of-pointer, u8[4]* is pointer-to-array.
 */

#include "parse.h"

#include "../ast/ast.h"
#include "../diag/diag.h"
#include "../lex/lex.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* context                                                             */
/* ------------------------------------------------------------------ */

typedef struct ScopeEntry {
    char *name;          /* owned */
    size_t decl_index;   /* token index at which the name becomes visible */
} ScopeEntry;

typedef struct Scope {
    ScopeEntry *entries;
    size_t count, cap;
} Scope;

typedef struct ParseCtx {
    const LexToken *tokens;
    size_t count;        /* includes the EOF token */
    size_t pos;          /* current token index */
    const char *file;    /* span file (from tokens[0]) */

    DiagRecord **records;
    size_t rcount, rcap;
    bool oom;
    bool seen_syntax_error;

    /* module-level struct/enum type names (prescan) */
    char **type_names;
    size_t ntype_names, type_names_cap;

    /* local value-name scopes (sizeof disambiguation) */
    Scope *scopes;
    size_t nscopes, scopes_cap;
} ParseCtx;

static char *dup_str(ParseCtx *c, const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) {
        c->oom = true;
        return NULL;
    }
    memcpy(d, s, n);
    return d;
}

/* ------------------------------------------------------------------ */
/* token access                                                        */
/* ------------------------------------------------------------------ */

static const LexToken *cur(const ParseCtx *c) { return &c->tokens[c->pos]; }

static bool cur_is_eof(const ParseCtx *c) { return c->tokens[c->pos].kind == TOK_EOF; }

static bool cur_is_ident(const ParseCtx *c) { return c->tokens[c->pos].kind == TOK_IDENT; }

static const char *cur_ident(const ParseCtx *c) { return c->tokens[c->pos].u.ident; }

static bool cur_is_kw(const ParseCtx *c, LexKeyword kw)
{
    return c->tokens[c->pos].kind == TOK_KEYWORD &&
           c->tokens[c->pos].u.keyword == kw;
}

static bool cur_is_punct(const ParseCtx *c, LexPunct p)
{
    return c->tokens[c->pos].kind == TOK_PUNCT &&
           c->tokens[c->pos].u.punct == p;
}

static bool cur_is_int_kw(const ParseCtx *c)
{
    const LexToken *t = &c->tokens[c->pos];
    if (t->kind != TOK_KEYWORD) return false;
    switch (t->u.keyword) {
    case KW_I8: case KW_I16: case KW_I32: case KW_I64:
    case KW_U8: case KW_U16: case KW_U32: case KW_U64:
    case KW_ISIZE: case KW_USIZE:
        return true;
    default:
        return false;
    }
}

/* Advance one token; never moves past the EOF token. */
static void advance(ParseCtx *c)
{
    if (c->pos + 1 < c->count) {
        c->pos++;
    } else {
        c->pos = c->count - 1;
    }
}

/* ------------------------------------------------------------------ */
/* spans                                                               */
/* ------------------------------------------------------------------ */

/* Range span from token start s (inclusive) to token end e (exclusive). */
static DiagSpan *span_from(const ParseCtx *c, size_t s, size_t e)
{
    if (s >= e) {
        return diag_span_clone(c->tokens[s].span);
    }
    const DiagSpan *a = c->tokens[s].span;
    const DiagSpan *b = c->tokens[e - 1].span;
    return diag_span_new_range(c->file,
                               a->start.line, a->start.col, a->start.offset,
                               b->end.line, b->end.col, b->end.offset);
}

/* The span to report for an expected-token error at the current position:
 * the current token itself, or at EOF the region from the end of the previous
 * token to the EOF position (covers the missing-token region). */
static DiagSpan *err_span(const ParseCtx *c)
{
    const DiagSpan *t = c->tokens[c->pos].span;
    if (!cur_is_eof(c) || c->pos == 0) {
        return diag_span_clone(t);
    }
    const DiagSpan *prev = c->tokens[c->pos - 1].span;
    return diag_span_new_range(c->file,
                               prev->end.line, prev->end.col, prev->end.offset,
                               t->end.line, t->end.col, t->end.offset);
}

/* ------------------------------------------------------------------ */
/* records                                                             */
/* ------------------------------------------------------------------ */

static void record_push(ParseCtx *c, DiagRecord *rec)
{
    if (c->rcount == c->rcap) {
        size_t ncap = c->rcap ? c->rcap * 2 : 8;
        DiagRecord **arr = (DiagRecord **)realloc(c->records, ncap * sizeof(*arr));
        if (!arr) {
            c->oom = true;
            diag_record_free(rec);
            return;
        }
        c->records = arr;
        c->rcap = ncap;
    }
    c->records[c->rcount++] = rec;
}

/* Report a syntax record. The span is cloned by the record. */
static void report_syntax(ParseCtx *c, const char *code, const char *message,
                          const DiagSpan *span)
{
    DiagRecord *rec = diag_record_new();
    if (!rec) { c->oom = true; return; }
    if (!diag_record_set_code(rec, code)) { diag_record_free(rec); c->oom = true; return; }
    if (!diag_record_set_message(rec, message)) { diag_record_free(rec); c->oom = true; return; }
    if (!diag_record_set_primary_span(rec, span)) { diag_record_free(rec); c->oom = true; return; }
    if (!diag_record_set_recovery(rec, c->seen_syntax_error
                                        ? DIAG_RECOVERY_RECOVERY_DERIVED
                                        : DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(rec);
        c->oom = true;
        return;
    }
    c->seen_syntax_error = true;
    record_push(c, rec);
}

static void err_expected(ParseCtx *c, const char *what)
{
    char msg[160];
    DiagSpan *span;
    snprintf(msg, sizeof(msg), "expected %s", what);
    span = err_span(c);
    report_syntax(c, "AIC-S0101", msg, span);
    diag_span_free(span);
}

static void err_unexpected(ParseCtx *c)
{
    DiagSpan *span = diag_span_clone(cur(c)->span);
    report_syntax(c, "AIC-S0102", "unexpected token", span);
    diag_span_free(span);
}

static const char *punct_expect_text(LexPunct p)
{
    switch (p) {
    case PUNCT_LPAREN: return "'('";
    case PUNCT_RPAREN: return "')'";
    case PUNCT_LBRACE: return "'{'";
    case PUNCT_RBRACE: return "'}'";
    case PUNCT_LBRACKET: return "'['";
    case PUNCT_RBRACKET: return "']'";
    case PUNCT_SEMI: return "';'";
    case PUNCT_COMMA: return "','";
    case PUNCT_COLON: return "':'";
    case PUNCT_DOT: return "'.'";
    case PUNCT_ARROW: return "'->'";
    case PUNCT_STAR: return "'*'";
    case PUNCT_ASSIGN: return "'='";
    case PUNCT_LT: return "'<'";
    case PUNCT_GT: return "'>'";
    case PUNCT_QUESTION: return "'?'";
    default: return "token";
    }
}

/* If the current token is p, consume it and return true; otherwise report
 * AIC-S0101 and return false (the caller performs recovery). */
static bool expect_punct(ParseCtx *c, LexPunct p)
{
    if (cur_is_punct(c, p)) {
        advance(c);
        return true;
    }
    err_expected(c, punct_expect_text(p));
    return false;
}

/* ------------------------------------------------------------------ */
/* scope stack and type-name table (sizeof disambiguation)             */
/* ------------------------------------------------------------------ */

static bool type_name_add(ParseCtx *c, const char *name)
{
    char *d = dup_str(c, name);
    if (!d) return false;
    if (c->ntype_names == c->type_names_cap) {
        size_t ncap = c->type_names_cap ? c->type_names_cap * 2 : 8;
        char **arr = (char **)realloc(c->type_names, ncap * sizeof(*arr));
        if (!arr) { free(d); c->oom = true; return false; }
        c->type_names = arr;
        c->type_names_cap = ncap;
    }
    c->type_names[c->ntype_names++] = d;
    return true;
}

static bool type_name_exists(const ParseCtx *c, const char *name)
{
    for (size_t i = 0; i < c->ntype_names; i++) {
        if (strcmp(c->type_names[i], name) == 0) return true;
    }
    return false;
}

/* Pre-scan the token stream for module-level struct/enum names so that
 * `sizeof(Point)` can be parsed as a type (spec sec. 5.2 note). Only
 * declarations at brace depth 0 count (struct/enum are top-level-only). */
static void prescan_type_names(ParseCtx *c)
{
    size_t depth = 0;
    for (size_t i = 0; i < c->count; i++) {
        const LexToken *t = &c->tokens[i];
        if (t->kind == TOK_PUNCT) {
            if (t->u.punct == PUNCT_LBRACE) depth++;
            else if (t->u.punct == PUNCT_RBRACE) { if (depth > 0) depth--; }
            continue;
        }
        if (depth == 0 && t->kind == TOK_KEYWORD &&
            (t->u.keyword == KW_STRUCT || t->u.keyword == KW_ENUM)) {
            if (i + 1 < c->count && c->tokens[i + 1].kind == TOK_IDENT) {
                type_name_add(c, c->tokens[i + 1].u.ident);
            }
        }
    }
}

static bool scope_push(ParseCtx *c)
{
    if (c->nscopes == c->scopes_cap) {
        size_t ncap = c->scopes_cap ? c->scopes_cap * 2 : 4;
        Scope *arr = (Scope *)realloc(c->scopes, ncap * sizeof(*arr));
        if (!arr) { c->oom = true; return false; }
        c->scopes = arr;
        c->scopes_cap = ncap;
    }
    memset(&c->scopes[c->nscopes], 0, sizeof(Scope));
    c->nscopes++;
    return true;
}

static void scope_pop(ParseCtx *c)
{
    if (c->nscopes == 0) return;
    Scope *sc = &c->scopes[c->nscopes - 1];
    for (size_t i = 0; i < sc->count; i++) free(sc->entries[i].name);
    free(sc->entries);
    c->nscopes--;
}

static bool scope_declare_value(ParseCtx *c, const char *name, size_t decl_index)
{
    if (c->nscopes == 0) return true;   /* no scope: nothing to track */
    Scope *sc = &c->scopes[c->nscopes - 1];
    if (sc->count == sc->cap) {
        size_t ncap = sc->cap ? sc->cap * 2 : 4;
        ScopeEntry *arr = (ScopeEntry *)realloc(sc->entries, ncap * sizeof(*arr));
        if (!arr) { c->oom = true; return false; }
        sc->entries = arr;
        sc->cap = ncap;
    }
    sc->entries[sc->count].name = dup_str(c, name);
    if (!sc->entries[sc->count].name) return false;
    sc->entries[sc->count].decl_index = decl_index;
    sc->count++;
    return true;
}

/* True when `name` is a local value visible at token index pos_index, i.e. a
 * declaration in an enclosing scope whose point of declaration precedes or
 * equals pos_index (spec sec. 6.1: visible from its point of declaration). */
static bool scope_is_visible_value(const ParseCtx *c, const char *name, size_t pos_index)
{
    for (size_t i = c->nscopes; i > 0; i--) {
        const Scope *sc = &c->scopes[i - 1];
        for (size_t j = 0; j < sc->count; j++) {
            if (sc->entries[j].decl_index <= pos_index &&
                strcmp(sc->entries[j].name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* token classes                                                       */
/* ------------------------------------------------------------------ */

static bool tok_starts_stmt(const ParseCtx *c)
{
    const LexToken *t = &c->tokens[c->pos];
    if (t->kind == TOK_PUNCT) return t->u.punct == PUNCT_LBRACE;
    if (t->kind != TOK_KEYWORD) return false;
    switch (t->u.keyword) {
    case KW_VAR: case KW_CONST: case KW_IF: case KW_WHILE: case KW_FOR:
    case KW_SWITCH: case KW_BREAK: case KW_CONTINUE: case KW_RETURN:
        return true;
    default:
        return false;
    }
}

static bool tok_starts_top_level(const ParseCtx *c)
{
    const LexToken *t = &c->tokens[c->pos];
    if (t->kind != TOK_KEYWORD) return false;
    switch (t->u.keyword) {
    case KW_PUB: case KW_STRUCT: case KW_ENUM: case KW_FN:
    case KW_VAR: case KW_CONST:
        return true;
    default:
        return false;
    }
}

static bool tok_starts_expr(const ParseCtx *c)
{
    const LexToken *t = &c->tokens[c->pos];
    switch (t->kind) {
    case TOK_IDENT: case TOK_INT_LITERAL: case TOK_STR_LITERAL:
        return true;
    case TOK_KEYWORD:
        switch (t->u.keyword) {
        case KW_TRUE: case KW_FALSE: case KW_NULL:
        case KW_SIZEOF: case KW_ALIGNOF: case KW_CAST: case KW_WRAP:
        case KW_LEN: case KW_PTR:
            return true;
        default:
            return false;
        }
    case TOK_PUNCT:
        switch (t->u.punct) {
        case PUNCT_LPAREN: case PUNCT_LBRACKET: case PUNCT_LBRACE:
        case PUNCT_MINUS: case PUNCT_PLUS: case PUNCT_BANG:
        case PUNCT_TILDE: case PUNCT_STAR: case PUNCT_AMP:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* recovery (deterministic resync; never emits records)                */
/* ------------------------------------------------------------------ */

/* Statement-level: consume until `;` (consumed), `}` (not consumed), a
 * statement starter (not consumed), or EOF. */
static void recover_statement(ParseCtx *c)
{
    while (!cur_is_eof(c)) {
        if (cur_is_punct(c, PUNCT_SEMI)) { advance(c); return; }
        if (cur_is_punct(c, PUNCT_RBRACE)) return;
        if (tok_starts_stmt(c)) return;
        advance(c);
    }
}

/* Top-level: consume until `;` (consumed), a top-level starter, `module`,
 * `import`, or EOF (none consumed). */
static void recover_top_level(ParseCtx *c)
{
    while (!cur_is_eof(c)) {
        if (cur_is_punct(c, PUNCT_SEMI)) { advance(c); return; }
        if (cur_is_kw(c, KW_MODULE) || cur_is_kw(c, KW_IMPORT)) return;
        if (tok_starts_top_level(c)) return;
        advance(c);
    }
}

/* Struct-field / enum-member separator: consume until `;` (consumed),
 * `}` (not consumed), or EOF. */
static void recover_field(ParseCtx *c)
{
    while (!cur_is_eof(c)) {
        if (cur_is_punct(c, PUNCT_SEMI)) { advance(c); return; }
        if (cur_is_punct(c, PUNCT_RBRACE)) return;
        advance(c);
    }
}

/* Param-list / enum-list separator: consume until `,` (not consumed),
 * `)` / `}` (not consumed), or EOF. */
static void recover_list_sep(ParseCtx *c, LexPunct closer)
{
    while (!cur_is_eof(c)) {
        if (cur_is_punct(c, PUNCT_COMMA)) return;
        if (cur_is_punct(c, closer)) return;
        advance(c);
    }
}

/* Switch body: consume until `;` (consumed), `}` (not consumed),
 * `case`/`default` (not consumed), or EOF. */
static void recover_switch_clause(ParseCtx *c)
{
    while (!cur_is_eof(c)) {
        if (cur_is_punct(c, PUNCT_SEMI)) { advance(c); return; }
        if (cur_is_punct(c, PUNCT_RBRACE)) return;
        if (cur_is_kw(c, KW_CASE) || cur_is_kw(c, KW_DEFAULT)) return;
        advance(c);
    }
}

/* ------------------------------------------------------------------ */
/* node helpers                                                        */
/* ------------------------------------------------------------------ */

static bool node_push(ParseCtx *c, AstNode ***arr, size_t *n, size_t *cap, AstNode *item)
{
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 4;
        AstNode **na = (AstNode **)realloc(*arr, ncap * sizeof(*na));
        if (!na) { c->oom = true; return false; }
        *arr = na;
        *cap = ncap;
    }
    (*arr)[(*n)++] = item;
    return true;
}

static AstNode *node_new(ParseCtx *c, AstNodeKind kind)
{
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    if (!n) {
        c->oom = true;
        return NULL;
    }
    n->kind = kind;
    return n;
}

/* ------------------------------------------------------------------ */
/* names                                                               */
/* ------------------------------------------------------------------ */

/* qualified_name = IDENT { "." IDENT } (current token must be IDENT) */
static AstName *parse_qualified_name(ParseCtx *c)
{
    char **parts = NULL;
    size_t n = 0, cap = 0;
    AstName *name;
    char *d;

    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    for (;;) {
        d = dup_str(c, cur_ident(c));
        if (!d) goto oom_parts;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            char **arr = (char **)realloc(parts, ncap * sizeof(*arr));
            if (!arr) {
                free(d);
                goto oom_parts;
            }
            parts = arr;
            cap = ncap;
        }
        parts[n++] = d;
        advance(c);
        if (!cur_is_punct(c, PUNCT_DOT)) break;
        advance(c);                    /* consume '.' */
        if (!cur_is_ident(c)) {
            err_expected(c, "identifier");
            goto free_parts;
        }
    }

    name = (AstName *)malloc(sizeof(AstName));
    if (!name) goto oom_parts;
    name->parts = parts;
    name->count = n;
    return name;

free_parts:
    for (size_t i = 0; i < n; i++) free(parts[i]);
    free(parts);
    return NULL;

oom_parts:
    c->oom = true;
    for (size_t i = 0; i < n; i++) free(parts[i]);
    free(parts);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* expressions (forward)                                               */
/* ------------------------------------------------------------------ */

static AstNode *parse_expr(ParseCtx *c);
static AstNode *parse_assignment(ParseCtx *c);
static AstNode *parse_conditional(ParseCtx *c);
static AstNode *parse_logical_or(ParseCtx *c);
static AstNode *parse_logical_and(ParseCtx *c);
static AstNode *parse_bit_or(ParseCtx *c);
static AstNode *parse_bit_xor(ParseCtx *c);
static AstNode *parse_bit_and(ParseCtx *c);
static AstNode *parse_equality(ParseCtx *c);
static AstNode *parse_relational(ParseCtx *c);
static AstNode *parse_shift(ParseCtx *c);
static AstNode *parse_additive(ParseCtx *c);
static AstNode *parse_multiplicative(ParseCtx *c);
static AstNode *parse_unary(ParseCtx *c);
static AstNode *parse_postfix(ParseCtx *c);
static AstNode *parse_primary(ParseCtx *c);

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

/* Whether the token after `sizeof (` is a type operand, per the spec
 * sec. 5.2 note resolved with the sec. 6.2 single name space:
 * primitive type keyword -> type; identifier naming a module-level
 * struct/enum that is not shadowed by a visible local value -> type;
 * everything else (including undeclared identifiers) -> expression. */
static bool sizeof_operand_is_type(const ParseCtx *c)
{
    const LexToken *t = &c->tokens[c->pos];
    if (t->kind == TOK_KEYWORD) {
        switch (t->u.keyword) {
        case KW_VOID: case KW_BOOL: case KW_STR:
        case KW_I8: case KW_I16: case KW_I32: case KW_I64:
        case KW_U8: case KW_U16: case KW_U32: case KW_U64:
        case KW_ISIZE: case KW_USIZE:
            return true;
        default:
            return false;
        }
    }
    if (t->kind == TOK_IDENT) {
        if (scope_is_visible_value(c, t->u.ident, c->pos)) return false;
        if (type_name_exists(c, t->u.ident)) return true;
        return false;
    }
    return false;
}

/* type = base_type { type_postfix }; type_postfix = "*" | "[" const_expr "]"
 * | "[]". Note the corpus rule: postfixes bind tighter than a leading
 * element pointer, so u8*[4] is array-of-pointer and u8[4]* is
 * pointer-to-array. */
static AstNode *parse_type(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *base = NULL;
    const LexToken *t = &c->tokens[c->pos];

    if (t->kind == TOK_IDENT) {
        AstName *name = parse_qualified_name(c);
        if (!name) return NULL;
        base = node_new(c, AST_TYPE_NAMED);
        if (!base) { ast_name_free(name); return NULL; }
        base->u.type_named.name = name;
    } else if (t->kind == TOK_KEYWORD) {
        int prim = ast_prim_from_keyword(t->u.keyword);
        if (prim >= 0) {
            base = node_new(c, AST_TYPE_PRIM);
            if (!base) return NULL;
            base->u.type_prim.prim = (AstPrimKind)prim;
            advance(c);
        }
    }
    if (base == NULL) {
        err_expected(c, "type");
        return NULL;
    }
    base->span = span_from(c, s, c->pos);
    for (;;) {
        if (cur_is_punct(c, PUNCT_STAR)) {
            advance(c);
            AstNode *ptr = node_new(c, AST_TYPE_PTR);
            if (!ptr) { ast_node_free(base); return NULL; }
            ptr->u.type_derived.base = base;
            ptr->span = span_from(c, s, c->pos);
            base = ptr;
        } else if (cur_is_punct(c, PUNCT_LBRACKET)) {
            advance(c);
            if (cur_is_punct(c, PUNCT_RBRACKET)) {
                advance(c);
                AstNode *sl = node_new(c, AST_TYPE_SLICE);
                if (!sl) { ast_node_free(base); return NULL; }
                sl->u.type_derived.base = base;
                sl->span = span_from(c, s, c->pos);
                base = sl;
            } else {
                AstNode *len = parse_expr(c);
                if (!len) { ast_node_free(base); return NULL; }
                if (!expect_punct(c, PUNCT_RBRACKET)) {
                    ast_node_free(base);
                    ast_node_free(len);
                    return NULL;
                }
                AstNode *arr = node_new(c, AST_TYPE_ARRAY);
                if (!arr) { ast_node_free(base); ast_node_free(len); return NULL; }
                arr->u.type_derived.base = base;
                arr->u.type_derived.len = len;
                arr->span = span_from(c, s, c->pos);
                base = arr;
            }
        } else {
            break;
        }
    }
    return base;
}

/* ------------------------------------------------------------------ */
/* expression nodes                                                    */
/* ------------------------------------------------------------------ */

static AstBinaryOp bin_op_of(LexPunct p)
{
    switch (p) {
    case PUNCT_STAR: return AST_BIN_MUL;
    case PUNCT_SLASH: return AST_BIN_DIV;
    case PUNCT_PERCENT: return AST_BIN_MOD;
    case PUNCT_PLUS: return AST_BIN_ADD;
    case PUNCT_MINUS: return AST_BIN_SUB;
    case PUNCT_SHL: return AST_BIN_SHL;
    case PUNCT_SHR: return AST_BIN_SHR;
    case PUNCT_LT: return AST_BIN_LT;
    case PUNCT_LE: return AST_BIN_LE;
    case PUNCT_GT: return AST_BIN_GT;
    case PUNCT_GE: return AST_BIN_GE;
    case PUNCT_EQ: return AST_BIN_EQ;
    case PUNCT_NE: return AST_BIN_NE;
    case PUNCT_AMP: return AST_BIN_BAND;
    case PUNCT_XOR: return AST_BIN_BXOR;
    case PUNCT_OR: return AST_BIN_BOR;
    case PUNCT_AND_AND: return AST_BIN_LAND;
    case PUNCT_OR_OR: return AST_BIN_LOR;
    default: return AST_BIN_ADD;   /* unreachable */
    }
}

static AstUnaryOp unary_op_of(LexPunct p)
{
    switch (p) {
    case PUNCT_MINUS: return AST_UN_NEG;
    case PUNCT_PLUS: return AST_UN_PLUS;
    case PUNCT_BANG: return AST_UN_NOT;
    case PUNCT_TILDE: return AST_UN_BNOT;
    case PUNCT_STAR: return AST_UN_DEREF;
    default: return AST_UN_ADDR;   /* PUNCT_AMP; unreachable otherwise */
    }
}

static AstAssignOp assign_op_of(LexPunct p)
{
    switch (p) {
    case PUNCT_ASSIGN: return AST_ASGN_ASSIGN;
    case PUNCT_PLUS_ASSIGN: return AST_ASGN_ADD;
    case PUNCT_MINUS_ASSIGN: return AST_ASGN_SUB;
    case PUNCT_STAR_ASSIGN: return AST_ASGN_MUL;
    case PUNCT_SLASH_ASSIGN: return AST_ASGN_DIV;
    case PUNCT_PERCENT_ASSIGN: return AST_ASGN_MOD;
    case PUNCT_SHL_ASSIGN: return AST_ASGN_SHL;
    case PUNCT_SHR_ASSIGN: return AST_ASGN_SHR;
    case PUNCT_AND_ASSIGN: return AST_ASGN_BAND;
    case PUNCT_OR_ASSIGN: return AST_ASGN_BOR;
    default: return AST_ASGN_BXOR; /* PUNCT_XOR_ASSIGN; unreachable otherwise */
    }
}

static bool cur_is_punct_in(const ParseCtx *c, const LexPunct *set, size_t nset)
{
    if (c->tokens[c->pos].kind != TOK_PUNCT) return false;
    LexPunct p = c->tokens[c->pos].u.punct;
    for (size_t i = 0; i < nset; i++) {
        if (p == set[i]) return true;
    }
    return false;
}

static AstNode *make_binary(ParseCtx *c, AstNode *lhs, AstNode *rhs,
                            AstBinaryOp op, size_t s, size_t e)
{
    AstNode *n = node_new(c, AST_EXPR_BINARY);
    if (!n) { ast_node_free(lhs); ast_node_free(rhs); return NULL; }
    n->u.binary.op = op;
    n->u.binary.lhs = lhs;
    n->u.binary.rhs = rhs;
    n->span = span_from(c, s, e);
    return n;
}

static AstNode *make_unary(ParseCtx *c, AstUnaryOp op, AstNode *operand,
                           size_t s, size_t e)
{
    AstNode *n = node_new(c, AST_EXPR_UNARY);
    if (!n) { ast_node_free(operand); return NULL; }
    n->u.unary.op = op;
    n->u.unary.operand = operand;
    n->span = span_from(c, s, e);
    return n;
}

/* ------------------------------------------------------------------ */
/* expressions: primary                                                */
/* ------------------------------------------------------------------ */

/* `{` in expression primary position is a brace-initializer without a type
 * name (spec sec. 12.7): report AIC-S0101 and consume the balanced brace
 * group so recovery is deterministic. */
static DiagSpan *consume_balanced_braces(ParseCtx *c, size_t s)
{
    int depth = 0;
    while (!cur_is_eof(c)) {
        if (cur_is_punct(c, PUNCT_LBRACE)) {
            depth++;
        } else if (cur_is_punct(c, PUNCT_RBRACE)) {
            depth--;
            advance(c);
            if (depth == 0) return span_from(c, s, c->pos);
            continue;
        }
        advance(c);
    }
    return span_from(c, s, c->pos);
}

/* array_literal = "[" [ expr { "," expr } [ "," ] ] "]" |
 *                 "[" expr ";" const_expr "]" */
static AstNode *parse_array_literal(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode **elems = NULL;
    size_t n = 0, cap = 0;
    AstNode *count = NULL;
    AstNode *first;
    AstNode *node;

    advance(c);   /* [ */
    if (!cur_is_punct(c, PUNCT_RBRACKET)) {
        first = parse_expr(c);
        if (!first) return NULL;
        if (cur_is_punct(c, PUNCT_SEMI)) {
            advance(c);   /* repeat form */
            if (!node_push(c, &elems, &n, &cap, first)) {
                ast_node_free(first);
                return NULL;
            }
            count = parse_expr(c);
            if (!count) { for (size_t i = 0; i < n; i++) ast_node_free(elems[i]); free(elems); return NULL; }
            if (!expect_punct(c, PUNCT_RBRACKET)) {
                for (size_t i = 0; i < n; i++) ast_node_free(elems[i]);
                free(elems);
                ast_node_free(count);
                return NULL;
            }
        } else {
            if (!node_push(c, &elems, &n, &cap, first)) {
                ast_node_free(first);
                return NULL;
            }
            while (cur_is_punct(c, PUNCT_COMMA)) {
                advance(c);
                if (cur_is_punct(c, PUNCT_RBRACKET)) break;   /* trailing comma */
                AstNode *e = parse_expr(c);
                if (!e) {
                    for (size_t i = 0; i < n; i++) ast_node_free(elems[i]);
                    free(elems);
                    return NULL;
                }
                if (!node_push(c, &elems, &n, &cap, e)) {
                    ast_node_free(e);
                    for (size_t i = 0; i < n; i++) ast_node_free(elems[i]);
                    free(elems);
                    return NULL;
                }
            }
            if (!expect_punct(c, PUNCT_RBRACKET)) {
                for (size_t i = 0; i < n; i++) ast_node_free(elems[i]);
                free(elems);
                return NULL;
            }
        }
    } else {
        advance(c);   /* empty list [] */
    }

    node = node_new(c, AST_EXPR_ARRAY_LITERAL);
    if (!node) {
        for (size_t i = 0; i < n; i++) ast_node_free(elems[i]);
        free(elems);
        return NULL;
    }
    node->u.array_literal.elems = elems;
    node->u.array_literal.nelems = n;
    node->u.array_literal.count = count;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* primary = literal | IDENT | "(" expr ")" | array_literal | null_literal */
static AstNode *parse_primary(ParseCtx *c)
{
    size_t s = c->pos;
    const LexToken *t = &c->tokens[c->pos];
    AstNode *node;

    switch (t->kind) {
    case TOK_INT_LITERAL:
        node = node_new(c, AST_EXPR_INT_LITERAL);
        if (!node) return NULL;
        node->u.int_literal.type = t->u.integer.type;
        node->u.int_literal.value = t->u.integer.value;
        node->u.int_literal.is_min = t->u.integer.is_min;
        advance(c);
        node->span = span_from(c, s, c->pos);
        return node;

    case TOK_STR_LITERAL:
        node = node_new(c, AST_EXPR_STR_LITERAL);
        if (!node) return NULL;
        node->u.str_literal.len = t->u.string.len;
        node->u.str_literal.bytes = (char *)malloc(node->u.str_literal.len);
        if (!node->u.str_literal.bytes && node->u.str_literal.len > 0) {
            ast_node_free(node);
            c->oom = true;
            return NULL;
        }
        if (node->u.str_literal.len > 0) {
            memcpy(node->u.str_literal.bytes, t->u.string.bytes, node->u.str_literal.len);
        }
        advance(c);
        node->span = span_from(c, s, c->pos);
        return node;

    case TOK_IDENT:
        node = node_new(c, AST_EXPR_IDENT);
        if (!node) return NULL;
        node->u.ident.name = dup_str(c, t->u.ident);
        if (!node->u.ident.name) { ast_node_free(node); return NULL; }
        advance(c);
        node->span = span_from(c, s, c->pos);
        return node;

    case TOK_KEYWORD:
        switch (t->u.keyword) {
        case KW_TRUE:
        case KW_FALSE:
            node = node_new(c, AST_EXPR_BOOL_LITERAL);
            if (!node) return NULL;
            node->u.bool_literal.value = (t->u.keyword == KW_TRUE);
            advance(c);
            node->span = span_from(c, s, c->pos);
            return node;
        case KW_NULL:
            node = node_new(c, AST_EXPR_NULL_LITERAL);
            if (!node) return NULL;
            advance(c);
            node->span = span_from(c, s, c->pos);
            return node;
        default:
            err_expected(c, "expression");
            return NULL;
        }

    case TOK_PUNCT:
        switch (t->u.punct) {
        case PUNCT_LPAREN: {
            advance(c);
            AstNode *e = parse_expr(c);
            if (!e) return NULL;
            if (!expect_punct(c, PUNCT_RPAREN)) {
                ast_node_free(e);
                return NULL;
            }
            node = node_new(c, AST_EXPR_PAREN);
            if (!node) { ast_node_free(e); return NULL; }
            node->u.paren.expr = e;
            node->span = span_from(c, s, c->pos);
            return node;
        }
        case PUNCT_LBRACKET:
            return parse_array_literal(c);
        case PUNCT_LBRACE: {
            DiagSpan *sp = consume_balanced_braces(c, s);
            report_syntax(c, "AIC-S0101", "expected token", sp);
            diag_span_free(sp);
            return NULL;
        }
        default:
            err_expected(c, "expression");
            return NULL;
        }

    default: /* TOK_EOF and anything unexpected */
        err_expected(c, "expression");
        return NULL;
    }
}

/* index/slice postfix: base "[" ... "]"; base may be a slice or an index. */
static AstNode *parse_index_slice(ParseCtx *c, AstNode *base, size_t s0)
{
    AstNode *lo = NULL, *hi = NULL;
    AstNodeKind kind;
    AstNode *node;

    advance(c);   /* [ */
    if (cur_is_punct(c, PUNCT_DOT_DOT)) {
        advance(c);   /* .. */
        if (!cur_is_punct(c, PUNCT_RBRACKET)) {
            hi = parse_expr(c);
            if (!hi) { ast_node_free(base); return NULL; }
        }
        if (!expect_punct(c, PUNCT_RBRACKET)) {
            ast_node_free(base);
            ast_node_free(hi);
            return NULL;
        }
        kind = AST_EXPR_SLICE;
    } else {
        lo = parse_expr(c);
        if (!lo) { ast_node_free(base); return NULL; }
        if (cur_is_punct(c, PUNCT_DOT_DOT)) {
            advance(c);   /* .. */
            if (!cur_is_punct(c, PUNCT_RBRACKET)) {
                hi = parse_expr(c);
                if (!hi) { ast_node_free(base); ast_node_free(lo); return NULL; }
            }
            if (!expect_punct(c, PUNCT_RBRACKET)) {
                ast_node_free(base);
                ast_node_free(lo);
                ast_node_free(hi);
                return NULL;
            }
            kind = AST_EXPR_SLICE;
        } else {
            if (!expect_punct(c, PUNCT_RBRACKET)) {
                ast_node_free(base);
                ast_node_free(lo);
                return NULL;
            }
            kind = AST_EXPR_INDEX;
        }
    }

    node = node_new(c, kind);
    if (!node) {
        ast_node_free(base);
        ast_node_free(lo);
        ast_node_free(hi);
        return NULL;
    }
    node->u.index_slice.base = base;
    if (kind == AST_EXPR_INDEX) {
        node->u.index_slice.index = lo;
    } else {
        node->u.index_slice.lo = lo;
        node->u.index_slice.hi = hi;
    }
    node->span = span_from(c, s0, c->pos);
    return node;
}

/* call postfix: callee "(" [ argument_list ] ")" (no trailing comma) */
static AstNode *parse_call(ParseCtx *c, AstNode *callee, size_t s0)
{
    AstNode **args = NULL;
    size_t n = 0, cap = 0;
    AstNode *node;

    advance(c);   /* ( */
    if (!cur_is_punct(c, PUNCT_RPAREN)) {
        AstNode *a = parse_expr(c);
        if (!a) { ast_node_free(callee); return NULL; }
        if (!node_push(c, &args, &n, &cap, a)) {
            ast_node_free(a);
            ast_node_free(callee);
            return NULL;
        }
        while (cur_is_punct(c, PUNCT_COMMA)) {
            advance(c);
            a = parse_expr(c);
            if (!a) {
                for (size_t i = 0; i < n; i++) ast_node_free(args[i]);
                free(args);
                ast_node_free(callee);
                return NULL;
            }
            if (!node_push(c, &args, &n, &cap, a)) {
                ast_node_free(a);
                for (size_t i = 0; i < n; i++) ast_node_free(args[i]);
                free(args);
                ast_node_free(callee);
                return NULL;
            }
        }
    }
    if (!expect_punct(c, PUNCT_RPAREN)) {
        for (size_t i = 0; i < n; i++) ast_node_free(args[i]);
        free(args);
        ast_node_free(callee);
        return NULL;
    }

    node = node_new(c, AST_EXPR_CALL);
    if (!node) {
        for (size_t i = 0; i < n; i++) ast_node_free(args[i]);
        free(args);
        ast_node_free(callee);
        return NULL;
    }
    node->u.call.callee = callee;
    node->u.call.args = args;
    node->u.call.nargs = n;
    node->span = span_from(c, s0, c->pos);
    return node;
}

/* member/arrow postfix: base ("." | "->") IDENT */
static AstNode *parse_member(ParseCtx *c, AstNode *base, size_t s0, bool arrow)
{
    AstNode *node;

    advance(c);   /* . or -> */
    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        ast_node_free(base);
        return NULL;
    }
    node = node_new(c, arrow ? AST_EXPR_ARROW : AST_EXPR_MEMBER);
    if (!node) { ast_node_free(base); return NULL; }
    node->u.member.base = base;
    node->u.member.name = dup_str(c, cur_ident(c));
    if (!node->u.member.name) { ast_node_free(node); return NULL; }
    advance(c);
    node->span = span_from(c, s0, c->pos);
    return node;
}

/* field_init = IDENT ":" expr */
static AstNode *parse_field_init(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *node;

    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_FIELD_INIT);
    if (!node) return NULL;
    node->u.named.name = dup_str(c, cur_ident(c));
    if (!node->u.named.name) { ast_node_free(node); return NULL; }
    advance(c);
    if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(node); return NULL; }
    node->u.named.value = parse_expr(c);
    if (!node->u.named.value) { ast_node_free(node); return NULL; }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* struct_init postfix (spec sec. 5.2): base "{" [ field_init { "," field_init }
 * [ "," ] ] "}" */
static AstNode *parse_struct_init(ParseCtx *c, AstNode *base, size_t s0)
{
    AstNode **fields = NULL;
    size_t n = 0, cap = 0;
    AstNode *node;

    advance(c);   /* { */
    if (!cur_is_punct(c, PUNCT_RBRACE)) {
        AstNode *f = parse_field_init(c);
        if (!f) { ast_node_free(base); return NULL; }
        if (!node_push(c, &fields, &n, &cap, f)) {
            ast_node_free(f);
            ast_node_free(base);
            return NULL;
        }
        while (cur_is_punct(c, PUNCT_COMMA)) {
            advance(c);
            if (cur_is_punct(c, PUNCT_RBRACE)) break;   /* trailing comma */
            f = parse_field_init(c);
            if (!f) {
                for (size_t i = 0; i < n; i++) ast_node_free(fields[i]);
                free(fields);
                ast_node_free(base);
                return NULL;
            }
            if (!node_push(c, &fields, &n, &cap, f)) {
                ast_node_free(f);
                for (size_t i = 0; i < n; i++) ast_node_free(fields[i]);
                free(fields);
                ast_node_free(base);
                return NULL;
            }
        }
    }
    if (!expect_punct(c, PUNCT_RBRACE)) {
        for (size_t i = 0; i < n; i++) ast_node_free(fields[i]);
        free(fields);
        ast_node_free(base);
        return NULL;
    }

    node = node_new(c, AST_EXPR_STRUCT_INIT);
    if (!node) {
        for (size_t i = 0; i < n; i++) ast_node_free(fields[i]);
        free(fields);
        ast_node_free(base);
        return NULL;
    }
    node->u.struct_init.base = base;
    node->u.struct_init.fields = fields;
    node->u.struct_init.nfields = n;
    node->span = span_from(c, s0, c->pos);
    return node;
}

/* postfix = primary { "[" ... "]" | "(" ... ")" | "." IDENT | "->" IDENT |
 *             "{" ... "}" } */
static AstNode *parse_postfix(ParseCtx *c)
{
    size_t s0 = c->pos;
    AstNode *e = parse_primary(c);
    if (!e) return NULL;

    for (;;) {
        const LexToken *t = &c->tokens[c->pos];
        if (t->kind != TOK_PUNCT) return e;
        switch (t->u.punct) {
        case PUNCT_LBRACKET:
            e = parse_index_slice(c, e, s0);
            break;
        case PUNCT_LPAREN:
            e = parse_call(c, e, s0);
            break;
        case PUNCT_DOT:
            e = parse_member(c, e, s0, false);
            break;
        case PUNCT_ARROW:
            e = parse_member(c, e, s0, true);
            break;
        case PUNCT_LBRACE:
            e = parse_struct_init(c, e, s0);
            break;
        default:
            return e;
        }
        if (!e) return NULL;
    }
}

/* sizeof/alignof/len/ptr: unary keywords with one parenthesized operand.
 * sizeof disambiguates type-vs-expression; alignof always takes a type;
 * len/ptr always take expressions. */
static AstNode *parse_sizeof(ParseCtx *c)
{
    size_t s = c->pos;
    bool is_type;
    AstNode *operand;
    AstNode *node;

    advance(c);   /* sizeof */
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    is_type = sizeof_operand_is_type(c);
    operand = is_type ? parse_type(c) : parse_expr(c);
    if (!operand) return NULL;
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(operand); return NULL; }

    node = node_new(c, is_type ? AST_EXPR_SIZEOF_TYPE : AST_EXPR_SIZEOF_EXPR);
    if (!node) { ast_node_free(operand); return NULL; }
    node->u.size_op.operand = operand;
    node->span = span_from(c, s, c->pos);
    return node;
}

static AstNode *parse_alignof(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *type;
    AstNode *node;

    advance(c);   /* alignof */
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    type = parse_type(c);
    if (!type) return NULL;
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(type); return NULL; }

    node = node_new(c, AST_EXPR_ALIGNOF);
    if (!node) { ast_node_free(type); return NULL; }
    node->u.size_op.operand = type;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* cast/wrap: ("cast"|"wrap") "<" type ">" "(" expr ")" */
static AstNode *parse_cast_wrap(ParseCtx *c, bool is_cast)
{
    size_t s = c->pos;
    AstNode *type;
    AstNode *e;
    AstNode *node;

    advance(c);
    if (!expect_punct(c, PUNCT_LT)) return NULL;
    type = parse_type(c);
    if (!type) return NULL;
    if (!expect_punct(c, PUNCT_GT)) { ast_node_free(type); return NULL; }
    if (!expect_punct(c, PUNCT_LPAREN)) { ast_node_free(type); return NULL; }
    e = parse_expr(c);
    if (!e) { ast_node_free(type); return NULL; }
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(type); ast_node_free(e); return NULL; }

    node = node_new(c, is_cast ? AST_EXPR_CAST : AST_EXPR_WRAP);
    if (!node) { ast_node_free(type); ast_node_free(e); return NULL; }
    node->u.cast_wrap.type = type;
    node->u.cast_wrap.expr = e;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* len/ptr: ("len"|"ptr") "(" expr ")" */
static AstNode *parse_len_ptr(ParseCtx *c, bool is_len)
{
    size_t s = c->pos;
    AstNode *e;
    AstNode *node;

    advance(c);
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    e = parse_expr(c);
    if (!e) return NULL;
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(e); return NULL; }

    node = node_new(c, is_len ? AST_EXPR_LEN : AST_EXPR_PTR);
    if (!node) { ast_node_free(e); return NULL; }
    node->u.size_op.operand = e;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* unary = [ ("-"|"+"|"!"|"~"|"*"|"&") ] postfix | sizeof_expr |
 *         alignof_expr | cast_expr | wrap_expr | len_expr | ptr_expr */
static AstNode *parse_unary(ParseCtx *c)
{
    size_t s = c->pos;
    const LexToken *t = &c->tokens[c->pos];

    if (t->kind == TOK_KEYWORD) {
        switch (t->u.keyword) {
        case KW_SIZEOF: return parse_sizeof(c);
        case KW_ALIGNOF: return parse_alignof(c);
        case KW_CAST: return parse_cast_wrap(c, true);
        case KW_WRAP: return parse_cast_wrap(c, false);
        case KW_LEN: return parse_len_ptr(c, true);
        case KW_PTR: return parse_len_ptr(c, false);
        default: break;
        }
    } else if (t->kind == TOK_PUNCT) {
        switch (t->u.punct) {
        case PUNCT_MINUS: case PUNCT_PLUS: case PUNCT_BANG:
        case PUNCT_TILDE: case PUNCT_STAR: case PUNCT_AMP: {
            AstNode *operand;
            advance(c);
            operand = parse_unary(c);
            if (!operand) return NULL;
            return make_unary(c, unary_op_of(t->u.punct), operand, s, c->pos);
        }
        default: break;
        }
    }
    return parse_postfix(c);
}

/* left-associative binary levels, spec sec. 10.1 (highest to lowest):
 * multiplicative, additive, shift, relational, equality, bit-and, bit-xor,
 * bit-or, logical-and, logical-or. */
static const LexPunct PUNCTS_MULT[] = { PUNCT_STAR, PUNCT_SLASH, PUNCT_PERCENT };
static const LexPunct PUNCTS_ADD[] = { PUNCT_PLUS, PUNCT_MINUS };
static const LexPunct PUNCTS_SHIFT[] = { PUNCT_SHL, PUNCT_SHR };
static const LexPunct PUNCTS_REL[] = { PUNCT_LT, PUNCT_LE, PUNCT_GT, PUNCT_GE };
static const LexPunct PUNCTS_EQ[] = { PUNCT_EQ, PUNCT_NE };

static AstNode *parse_multiplicative(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_unary(c);
    while (lhs && cur_is_punct_in(c, PUNCTS_MULT, 3)) {
        LexPunct op = c->tokens[c->pos].u.punct;
        AstNode *rhs;
        advance(c);
        rhs = parse_unary(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, bin_op_of(op), s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_additive(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_multiplicative(c);
    while (lhs && cur_is_punct_in(c, PUNCTS_ADD, 2)) {
        LexPunct op = c->tokens[c->pos].u.punct;
        AstNode *rhs;
        advance(c);
        rhs = parse_multiplicative(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, bin_op_of(op), s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_shift(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_additive(c);
    while (lhs && cur_is_punct_in(c, PUNCTS_SHIFT, 2)) {
        LexPunct op = c->tokens[c->pos].u.punct;
        AstNode *rhs;
        advance(c);
        rhs = parse_additive(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, bin_op_of(op), s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_relational(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_shift(c);
    while (lhs && cur_is_punct_in(c, PUNCTS_REL, 4)) {
        LexPunct op = c->tokens[c->pos].u.punct;
        AstNode *rhs;
        advance(c);
        rhs = parse_shift(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, bin_op_of(op), s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_equality(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_relational(c);
    while (lhs && cur_is_punct_in(c, PUNCTS_EQ, 2)) {
        LexPunct op = c->tokens[c->pos].u.punct;
        AstNode *rhs;
        advance(c);
        rhs = parse_relational(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, bin_op_of(op), s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_bit_and(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_equality(c);
    while (lhs && cur_is_punct(c, PUNCT_AMP)) {
        AstNode *rhs;
        advance(c);
        rhs = parse_equality(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, AST_BIN_BAND, s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_bit_xor(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_bit_and(c);
    while (lhs && cur_is_punct(c, PUNCT_XOR)) {
        AstNode *rhs;
        advance(c);
        rhs = parse_bit_and(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, AST_BIN_BXOR, s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_bit_or(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_bit_xor(c);
    while (lhs && cur_is_punct(c, PUNCT_OR)) {
        AstNode *rhs;
        advance(c);
        rhs = parse_bit_xor(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, AST_BIN_BOR, s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_logical_and(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_bit_or(c);
    while (lhs && cur_is_punct(c, PUNCT_AND_AND)) {
        AstNode *rhs;
        advance(c);
        rhs = parse_bit_or(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, AST_BIN_LAND, s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

static AstNode *parse_logical_or(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *lhs = parse_logical_and(c);
    while (lhs && cur_is_punct(c, PUNCT_OR_OR)) {
        AstNode *rhs;
        advance(c);
        rhs = parse_logical_and(c);
        if (!rhs) { ast_node_free(lhs); return NULL; }
        lhs = make_binary(c, lhs, rhs, AST_BIN_LOR, s, c->pos);
        if (!lhs) return NULL;
    }
    return lhs;
}

/* conditional_expr = logical_or_expr [ "?" expr ":" conditional_expr ]
 * (right-associative; the middle operand is a full expr, the else branch is
 * a conditional without assignment) */
static AstNode *parse_conditional(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *cond = parse_logical_or(c);
    AstNode *node;

    if (!cond) return NULL;
    if (!cur_is_punct(c, PUNCT_QUESTION)) return cond;
    advance(c);
    {
        AstNode *then = parse_expr(c);
        if (!then) { ast_node_free(cond); return NULL; }
        if (!expect_punct(c, PUNCT_COLON)) {
            ast_node_free(cond);
            ast_node_free(then);
            return NULL;
        }
        AstNode *els = parse_conditional(c);
        if (!els) {
            ast_node_free(cond);
            ast_node_free(then);
            return NULL;
        }
        node = node_new(c, AST_EXPR_TERNARY);
        if (!node) {
            ast_node_free(cond);
            ast_node_free(then);
            ast_node_free(els);
            return NULL;
        }
        node->u.branch.cond = cond;
        node->u.branch.then = then;
        node->u.branch.els = els;
    }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* assignment_expr = conditional_expr [ assign_op assignment_expr ]
 * (right-associative) */
static AstNode *parse_assignment(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *target = parse_conditional(c);
    LexPunct op;
    AstNode *value;
    AstNode *node;

    if (!target) return NULL;
    if (c->tokens[c->pos].kind != TOK_PUNCT) return target;
    op = c->tokens[c->pos].u.punct;
    switch (op) {
    case PUNCT_ASSIGN: case PUNCT_PLUS_ASSIGN: case PUNCT_MINUS_ASSIGN:
    case PUNCT_STAR_ASSIGN: case PUNCT_SLASH_ASSIGN: case PUNCT_PERCENT_ASSIGN:
    case PUNCT_SHL_ASSIGN: case PUNCT_SHR_ASSIGN:
    case PUNCT_AND_ASSIGN: case PUNCT_OR_ASSIGN: case PUNCT_XOR_ASSIGN:
        break;
    default:
        return target;
    }
    advance(c);
    value = parse_assignment(c);
    if (!value) { ast_node_free(target); return NULL; }

    node = node_new(c, AST_EXPR_ASSIGN);
    if (!node) { ast_node_free(target); ast_node_free(value); return NULL; }
    node->u.assign.op = assign_op_of(op);
    node->u.assign.target = target;
    node->u.assign.value = value;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* expr = assignment_expr */
static AstNode *parse_expr(ParseCtx *c)
{
    return parse_assignment(c);
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static AstNode *parse_block(ParseCtx *c);
static AstNode *parse_statement(ParseCtx *c);
static AstNode *parse_local_var_decl(ParseCtx *c, bool is_const);
static AstNode *parse_local_const_decl(ParseCtx *c);
static AstNode *parse_if(ParseCtx *c);
static AstNode *parse_while(ParseCtx *c);
static AstNode *parse_for(ParseCtx *c);
static AstNode *parse_switch(ParseCtx *c);
static AstNode *parse_switch_clause(ParseCtx *c);
static AstNode *parse_break_continue(ParseCtx *c, bool is_break);
static AstNode *parse_return(ParseCtx *c);
static AstNode *parse_expr_stmt(ParseCtx *c);

/* block = "{" { statement } "}" */
static AstNode *parse_block(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode **items = NULL;
    size_t n = 0, cap = 0;
    bool closed;
    AstNode *node;

    if (!cur_is_punct(c, PUNCT_LBRACE)) {
        err_expected(c, "'{'");
        return NULL;
    }
    if (!scope_push(c)) return NULL;
    advance(c);   /* { */
    while (!cur_is_punct(c, PUNCT_RBRACE) && !cur_is_eof(c)) {
        AstNode *stmt = parse_statement(c);
        if (stmt) {
            if (!node_push(c, &items, &n, &cap, stmt)) {
                ast_node_free(stmt);
                break;
            }
        } else {
            if (c->oom) break;
            recover_statement(c);
        }
    }
    closed = cur_is_punct(c, PUNCT_RBRACE);
    if (closed) advance(c);   /* } */
    else err_expected(c, "'}'");
    scope_pop(c);

    if (c->oom || !closed) {
        for (size_t i = 0; i < n; i++) ast_node_free(items[i]);
        free(items);
        return NULL;
    }
    node = node_new(c, AST_BLOCK);
    if (!node) {
        for (size_t i = 0; i < n; i++) ast_node_free(items[i]);
        free(items);
        return NULL;
    }
    node->u.list.items = items;
    node->u.list.count = n;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* statement = block | var_decl | const_decl | if_stmt | while_stmt |
 * for_stmt | switch_stmt | break_stmt | continue_stmt | return_stmt |
 * expr_stmt | empty_stmt */
static AstNode *parse_statement(ParseCtx *c)
{
    const LexToken *t = &c->tokens[c->pos];

    if (t->kind == TOK_PUNCT) {
        if (t->u.punct == PUNCT_LBRACE) return parse_block(c);
        if (t->u.punct == PUNCT_SEMI) {
            size_t s = c->pos;
            AstNode *node;
            advance(c);
            node = node_new(c, AST_EMPTY_STMT);
            if (!node) return NULL;
            node->span = span_from(c, s, c->pos);
            return node;
        }
        if (!tok_starts_expr(c)) {
            err_unexpected(c);
            return NULL;
        }
        return parse_expr_stmt(c);
    }

    if (t->kind == TOK_KEYWORD) {
        switch (t->u.keyword) {
        case KW_VAR: return parse_local_var_decl(c, false);
        case KW_CONST: return parse_local_const_decl(c);
        case KW_IF: return parse_if(c);
        case KW_WHILE: return parse_while(c);
        case KW_FOR: return parse_for(c);
        case KW_SWITCH: return parse_switch(c);
        case KW_BREAK: return parse_break_continue(c, true);
        case KW_CONTINUE: return parse_break_continue(c, false);
        case KW_RETURN: return parse_return(c);
        default: break;
        }
        if (!tok_starts_expr(c)) {
            err_unexpected(c);
            return NULL;
        }
        return parse_expr_stmt(c);
    }

    /* IDENT / INT / STR literals */
    return parse_expr_stmt(c);
}

/* local var/const decl: ("var"|"const") IDENT ":" type "=" expr ";" */
static AstNode *parse_local_var_decl(ParseCtx *c, bool is_const)
{
    size_t s = c->pos;
    size_t decl_index = c->pos;
    AstNode *type;
    AstNode *init;
    AstNode *node;

    advance(c);   /* var | const */
    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, is_const ? AST_CONST_DECL : AST_VAR_DECL);
    if (!node) return NULL;
    node->u.local_decl.name = dup_str(c, cur_ident(c));
    if (!node->u.local_decl.name) { ast_node_free(node); return NULL; }
    advance(c);
    /* The name is visible from its point of declaration (spec sec. 6.1),
     * including inside its own initializer; declare before parsing the
     * type/initializer so `var S: i32 = sizeof(S);` sees S as a value. */
    scope_declare_value(c, node->u.local_decl.name, decl_index);
    if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(node); return NULL; }
    type = parse_type(c);
    if (!type) { ast_node_free(node); return NULL; }
    node->u.local_decl.type = type;
    if (!expect_punct(c, PUNCT_ASSIGN)) { ast_node_free(node); return NULL; }
    init = parse_expr(c);
    if (!init) { ast_node_free(node); return NULL; }
    node->u.local_decl.init = init;
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(node); return NULL; }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* const_decl = \"const\" IDENT \":\" type \"=\" expr \";\" (same shape as var) */
static AstNode *parse_local_const_decl(ParseCtx *c)
{
    return parse_local_var_decl(c, true);
}

/* if_stmt = "if" "(" expr ")" block [ "else" ( if_stmt | block ) ] */
static AstNode *parse_if(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *cond;
    AstNode *then;
    AstNode *els = NULL;
    AstNode *node;

    advance(c);   /* if */
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    cond = parse_expr(c);
    if (!cond) return NULL;
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(cond); return NULL; }
    if (!cur_is_punct(c, PUNCT_LBRACE)) {
        DiagSpan *sp = span_from(c, s, c->pos);   /* the statement head */
        report_syntax(c, "AIC-S0104", "controlled body must be enclosed in braces", sp);
        diag_span_free(sp);
        ast_node_free(cond);
        return NULL;
    }
    then = parse_block(c);
    if (!then) { ast_node_free(cond); return NULL; }
    if (cur_is_kw(c, KW_ELSE)) {
        advance(c);
        if (cur_is_kw(c, KW_IF)) {
            els = parse_if(c);
            if (!els) { ast_node_free(cond); ast_node_free(then); return NULL; }
        } else if (cur_is_punct(c, PUNCT_LBRACE)) {
            els = parse_block(c);
            if (!els) { ast_node_free(cond); ast_node_free(then); return NULL; }
        } else {
            err_expected(c, "'if' or '{'");
            ast_node_free(cond);
            ast_node_free(then);
            return NULL;
        }
    }
    node = node_new(c, AST_IF);
    if (!node) { ast_node_free(cond); ast_node_free(then); ast_node_free(els); return NULL; }
    node->u.branch.cond = cond;
    node->u.branch.then = then;
    node->u.branch.els = els;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* while_stmt = "while" "(" expr ")" block */
static AstNode *parse_while(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *cond;
    AstNode *body;
    AstNode *node;

    advance(c);   /* while */
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    cond = parse_expr(c);
    if (!cond) return NULL;
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(cond); return NULL; }
    if (!cur_is_punct(c, PUNCT_LBRACE)) {
        DiagSpan *sp = span_from(c, s, c->pos);
        report_syntax(c, "AIC-S0104", "controlled body must be enclosed in braces", sp);
        diag_span_free(sp);
        ast_node_free(cond);
        return NULL;
    }
    body = parse_block(c);
    if (!body) { ast_node_free(cond); return NULL; }

    node = node_new(c, AST_WHILE);
    if (!node) { ast_node_free(cond); ast_node_free(body); return NULL; }
    node->u.while_loop.cond = cond;
    node->u.while_loop.body = body;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* for_stmt = "for" "(" ( var_decl | const_decl | [ expr ] ";" )
 *            [ expr ] ";" [ expr ] ")" block
 * The init declaration is scoped to the for statement (spec sec. 13.3). */
static AstNode *parse_for(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *init = NULL;
    AstNode *cond = NULL;
    AstNode *step = NULL;
    AstNode *body = NULL;
    AstNode *node;
    bool scoped = false;

    advance(c);   /* for */
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    if (!scope_push(c)) return NULL;
    scoped = true;

    if (cur_is_kw(c, KW_VAR)) {
        init = parse_local_var_decl(c, false);
        if (!init) goto fail;
    } else if (cur_is_kw(c, KW_CONST)) {
        init = parse_local_const_decl(c);
        if (!init) goto fail;
    } else if (cur_is_punct(c, PUNCT_SEMI)) {
        advance(c);
    } else {
        init = parse_expr(c);
        if (!init || !expect_punct(c, PUNCT_SEMI)) goto fail;
    }
    if (!cur_is_punct(c, PUNCT_SEMI)) {
        cond = parse_expr(c);
        if (!cond) goto fail;
    }
    if (!expect_punct(c, PUNCT_SEMI)) goto fail;
    if (!cur_is_punct(c, PUNCT_RPAREN)) {
        step = parse_expr(c);
        if (!step) goto fail;
    }
    if (!expect_punct(c, PUNCT_RPAREN)) goto fail;
    if (!cur_is_punct(c, PUNCT_LBRACE)) {
        DiagSpan *sp = span_from(c, s, c->pos);
        report_syntax(c, "AIC-S0104", "controlled body must be enclosed in braces", sp);
        diag_span_free(sp);
        goto fail;
    }
    body = parse_block(c);
    if (!body) goto fail;
    scope_pop(c);
    scoped = false;

    node = node_new(c, AST_FOR);
    if (!node) { ast_node_free(init); ast_node_free(cond); ast_node_free(step); ast_node_free(body); return NULL; }
    node->u.for_loop.init = init;
    node->u.for_loop.cond = cond;
    node->u.for_loop.step = step;
    node->u.for_loop.body = body;
    node->span = span_from(c, s, c->pos);
    return node;

fail:
    if (scoped) scope_pop(c);
    ast_node_free(init);
    ast_node_free(cond);
    ast_node_free(step);
    return NULL;
}

/* switch_stmt = "switch" "(" expr ")" switch_body ;
 * switch_body = "{" { case_clause | default_clause } "}" */
static AstNode *parse_switch(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *selector;
    AstNode **cases = NULL;
    size_t n = 0, cap = 0;
    bool closed;
    AstNode *node;

    advance(c);   /* switch */
    if (!expect_punct(c, PUNCT_LPAREN)) return NULL;
    selector = parse_expr(c);
    if (!selector) return NULL;
    if (!expect_punct(c, PUNCT_RPAREN)) { ast_node_free(selector); return NULL; }
    if (!expect_punct(c, PUNCT_LBRACE)) { ast_node_free(selector); return NULL; }

    while (!cur_is_punct(c, PUNCT_RBRACE) && !cur_is_eof(c)) {
        AstNode *cl = parse_switch_clause(c);
        if (cl) {
            if (!node_push(c, &cases, &n, &cap, cl)) {
                ast_node_free(cl);
                break;
            }
        } else {
            if (c->oom) break;
            recover_switch_clause(c);
        }
    }
    closed = cur_is_punct(c, PUNCT_RBRACE);
    if (closed) advance(c);   /* } */
    else err_expected(c, "'}'");

    if (c->oom || !closed) {
        for (size_t i = 0; i < n; i++) ast_node_free(cases[i]);
        free(cases);
        ast_node_free(selector);
        return NULL;
    }
    node = node_new(c, AST_SWITCH);
    if (!node) {
        for (size_t i = 0; i < n; i++) ast_node_free(cases[i]);
        free(cases);
        ast_node_free(selector);
        return NULL;
    }
    node->u.switch_stmt.selector = selector;
    node->u.switch_stmt.cases = cases;
    node->u.switch_stmt.ncases = n;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* case_clause = "case" const_expr ":" block ;
 * default_clause = "default" ":" block */
static AstNode *parse_switch_clause(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *value = NULL;
    AstNode *body;
    AstNode *node;
    bool is_case;

    if (cur_is_kw(c, KW_CASE)) {
        is_case = true;
        advance(c);
        value = parse_expr(c);
        if (!value) return NULL;
        if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(value); return NULL; }
    } else if (cur_is_kw(c, KW_DEFAULT)) {
        is_case = false;
        advance(c);
        if (!expect_punct(c, PUNCT_COLON)) return NULL;
    } else {
        err_expected(c, "'case' or 'default'");
        return NULL;
    }

    if (!cur_is_punct(c, PUNCT_LBRACE)) {
        DiagSpan *sp = span_from(c, s, c->pos);   /* the case head */
        report_syntax(c, "AIC-S0104", "controlled body must be enclosed in braces", sp);
        diag_span_free(sp);
        ast_node_free(value);
        return NULL;
    }
    body = parse_block(c);
    if (!body) { ast_node_free(value); return NULL; }

    node = node_new(c, is_case ? AST_CASE_CLAUSE : AST_DEFAULT_CLAUSE);
    if (!node) { ast_node_free(value); ast_node_free(body); return NULL; }
    node->u.clause.value = value;
    node->u.clause.body = body;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* break_stmt / continue_stmt = ("break"|"continue") ";" */
static AstNode *parse_break_continue(ParseCtx *c, bool is_break)
{
    size_t s = c->pos;
    AstNode *node;
    advance(c);
    if (!expect_punct(c, PUNCT_SEMI)) return NULL;
    node = node_new(c, is_break ? AST_BREAK : AST_CONTINUE);
    if (!node) return NULL;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* return_stmt = "return" [ expr ] ";" */
static AstNode *parse_return(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *value = NULL;
    AstNode *node;
    advance(c);
    if (tok_starts_expr(c)) {
        value = parse_expr(c);
        if (!value) return NULL;
    }
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(value); return NULL; }
    node = node_new(c, AST_RETURN);
    if (!node) { ast_node_free(value); return NULL; }
    node->u.ret.value = value;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* expr_stmt = expr ";" */
static AstNode *parse_expr_stmt(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *e;
    AstNode *node;

    e = parse_expr(c);
    if (!e) return NULL;
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(e); return NULL; }
    node = node_new(c, AST_EXPR_STMT);
    if (!node) { ast_node_free(e); return NULL; }
    node->u.expr_stmt.expr = e;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* ------------------------------------------------------------------ */
/* top-level declarations                                              */
/* ------------------------------------------------------------------ */

static AstNode *parse_struct_decl(ParseCtx *c, bool is_pub);
static AstNode *parse_enum_decl(ParseCtx *c, bool is_pub);
static AstNode *parse_fn_decl(ParseCtx *c, bool is_pub);
static AstNode *parse_global_var(ParseCtx *c, bool is_pub, bool is_const);

/* module_decl = "module" qualified_name ";" */
static AstNode *parse_module_decl(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *node;

    advance(c);   /* module */
    node = node_new(c, AST_MODULE_DECL);
    if (!node) return NULL;
    node->u.qname.name = parse_qualified_name(c);
    if (!node->u.qname.name) { ast_node_free(node); return NULL; }
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(node); return NULL; }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* import_decl = "import" qualified_name ";" */
static AstNode *parse_import(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *node;

    advance(c);   /* import */
    node = node_new(c, AST_IMPORT_DECL);
    if (!node) return NULL;
    node->u.qname.name = parse_qualified_name(c);
    if (!node->u.qname.name) { ast_node_free(node); return NULL; }
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(node); return NULL; }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* field_decl = IDENT ":" type ";" */
static AstNode *parse_field_decl(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *node;

    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_FIELD_DECL);
    if (!node) return NULL;
    node->u.named.name = dup_str(c, cur_ident(c));
    if (!node->u.named.name) { ast_node_free(node); return NULL; }
    advance(c);
    if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(node); return NULL; }
    node->u.named.type = parse_type(c);
    if (!node->u.named.type) { ast_node_free(node); return NULL; }
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(node); return NULL; }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* struct_decl = "struct" IDENT "{" { field_decl } "}" */
static AstNode *parse_struct_decl(ParseCtx *c, bool is_pub)
{
    size_t s = c->pos;
    AstNode **fields = NULL;
    size_t n = 0, cap = 0;
    bool closed;
    AstNode *node;

    advance(c);   /* struct */
    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_STRUCT_DECL);
    if (!node) return NULL;
    node->u.struct_decl.name = dup_str(c, cur_ident(c));
    if (!node->u.struct_decl.name) { ast_node_free(node); return NULL; }
    node->u.struct_decl.is_pub = is_pub;
    advance(c);
    if (!expect_punct(c, PUNCT_LBRACE)) { ast_node_free(node); return NULL; }

    while (!cur_is_punct(c, PUNCT_RBRACE) && !cur_is_eof(c)) {
        AstNode *f = parse_field_decl(c);
        if (f) {
            if (!node_push(c, &fields, &n, &cap, f)) {
                ast_node_free(f);
                break;
            }
        } else {
            if (c->oom) break;
            recover_field(c);
        }
    }
    closed = cur_is_punct(c, PUNCT_RBRACE);
    if (closed) advance(c);   /* } */
    else err_expected(c, "'}'");

    if (c->oom || !closed) {
        for (size_t i = 0; i < n; i++) ast_node_free(fields[i]);
        free(fields);
        ast_node_free(node);
        return NULL;
    }
    node->u.struct_decl.fields = fields;
    node->u.struct_decl.nfields = n;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* enum_member = IDENT [ "=" const_expr ] */
static AstNode *parse_enum_member(ParseCtx *c)
{
    size_t s = c->pos;
    AstNode *node;

    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_ENUM_MEMBER);
    if (!node) return NULL;
    node->u.named.name = dup_str(c, cur_ident(c));
    if (!node->u.named.name) { ast_node_free(node); return NULL; }
    advance(c);
    if (cur_is_punct(c, PUNCT_ASSIGN)) {
        advance(c);
        node->u.named.value = parse_expr(c);
        if (!node->u.named.value) { ast_node_free(node); return NULL; }
    }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* enum_decl = "enum" IDENT ":" int_type "{" enum_member_list "}" ;
 * enum_member_list = enum_member { "," enum_member } [ "," ] */
static AstNode *parse_enum_decl(ParseCtx *c, bool is_pub)
{
    size_t s = c->pos;
    AstNode **members = NULL;
    size_t n = 0, cap = 0;
    bool closed;
    AstNode *node;

    advance(c);   /* enum */
    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_ENUM_DECL);
    if (!node) return NULL;
    node->u.enum_decl.name = dup_str(c, cur_ident(c));
    if (!node->u.enum_decl.name) { ast_node_free(node); return NULL; }
    node->u.enum_decl.is_pub = is_pub;
    advance(c);
    if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(node); return NULL; }
    if (!cur_is_int_kw(c)) {
        err_expected(c, "integer type");
        ast_node_free(node);
        return NULL;
    }
    node->u.enum_decl.underlying = node_new(c, AST_TYPE_PRIM);
    if (!node->u.enum_decl.underlying) { ast_node_free(node); return NULL; }
    node->u.enum_decl.underlying->u.type_prim.prim =
        (AstPrimKind)ast_prim_from_keyword(cur(c)->u.keyword);
    node->u.enum_decl.underlying->span = diag_span_clone(cur(c)->span);
    advance(c);
    if (!expect_punct(c, PUNCT_LBRACE)) { ast_node_free(node); return NULL; }

    while (!cur_is_punct(c, PUNCT_RBRACE) && !cur_is_eof(c)) {
        AstNode *m = parse_enum_member(c);
        if (m) {
            if (!node_push(c, &members, &n, &cap, m)) {
                ast_node_free(m);
                break;
            }
        } else {
            if (c->oom) break;
            recover_list_sep(c, PUNCT_RBRACE);
            /* recover_list_sep stops at a `,` without consuming it; consume a
             * separator here so a repeated separator (e.g. `Red, , Blue`)
             * cannot resynchronize onto the same token forever. */
            if (cur_is_punct(c, PUNCT_COMMA)) {
                advance(c);
            }
            continue;
        }
        if (cur_is_punct(c, PUNCT_COMMA)) {
            advance(c);
            continue;
        }
        if (cur_is_punct(c, PUNCT_RBRACE)) break;
        err_expected(c, "',' or '}'");
        recover_list_sep(c, PUNCT_RBRACE);
    }
    closed = cur_is_punct(c, PUNCT_RBRACE);
    if (closed) advance(c);   /* } */
    else err_expected(c, "'}'");

    if (c->oom || !closed) {
        for (size_t i = 0; i < n; i++) ast_node_free(members[i]);
        free(members);
        ast_node_free(node);
        return NULL;
    }
    node->u.enum_decl.members = members;
    node->u.enum_decl.nmembers = n;
    node->span = span_from(c, s, c->pos);
    return node;
}

/* param = IDENT ":" type */
static AstNode *parse_param(ParseCtx *c)
{
    size_t s = c->pos;
    size_t decl_index = c->pos;
    AstNode *node;

    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_PARAM);
    if (!node) return NULL;
    node->u.named.name = dup_str(c, cur_ident(c));
    if (!node->u.named.name) { ast_node_free(node); return NULL; }
    advance(c);
    if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(node); return NULL; }
    node->u.named.type = parse_type(c);
    if (!node->u.named.type) { ast_node_free(node); return NULL; }
    scope_declare_value(c, node->u.named.name, decl_index);
    node->span = span_from(c, s, c->pos);
    return node;
}

/* fn_decl = "fn" IDENT "(" [ param_list ] ")" "->" type block */
static AstNode *parse_fn_decl(ParseCtx *c, bool is_pub)
{
    size_t s = c->pos;
    AstNode **params = NULL;
    size_t n = 0, cap = 0;
    AstNode *ret = NULL;
    AstNode *body = NULL;
    AstNode *node;
    bool scoped = false;

    advance(c);   /* fn */
    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, AST_FN_DECL);
    if (!node) return NULL;
    node->u.fn_decl.name = dup_str(c, cur_ident(c));
    if (!node->u.fn_decl.name) { ast_node_free(node); return NULL; }
    node->u.fn_decl.is_pub = is_pub;
    advance(c);
    if (!expect_punct(c, PUNCT_LPAREN)) { ast_node_free(node); return NULL; }

    if (!scope_push(c)) { ast_node_free(node); return NULL; }
    scoped = true;
    if (!cur_is_punct(c, PUNCT_RPAREN)) {
        for (;;) {
            AstNode *p = parse_param(c);
            if (p) {
                if (!node_push(c, &params, &n, &cap, p)) {
                    ast_node_free(p);
                    break;
                }
            } else {
                if (c->oom) break;
                recover_list_sep(c, PUNCT_RPAREN);
            }
            if (cur_is_punct(c, PUNCT_COMMA)) {
                advance(c);
                continue;
            }
            break;
        }
    }
    if (!expect_punct(c, PUNCT_RPAREN)) goto fail;
    if (!expect_punct(c, PUNCT_ARROW)) goto fail;
    ret = parse_type(c);
    if (!ret) goto fail;
    if (!cur_is_punct(c, PUNCT_LBRACE)) {
        err_expected(c, "'{'");
        goto fail;
    }
    body = parse_block(c);
    if (!body) goto fail;
    scope_pop(c);
    scoped = false;

    node->u.fn_decl.params = params;
    node->u.fn_decl.nparams = n;
    node->u.fn_decl.ret_type = ret;
    node->u.fn_decl.body = body;
    node->span = span_from(c, s, c->pos);
    return node;

fail:
    if (scoped) scope_pop(c);
    for (size_t i = 0; i < n; i++) ast_node_free(params[i]);
    free(params);
    ast_node_free(ret);
    ast_node_free(body);
    ast_node_free(node);
    return NULL;
}

/* global_var_decl / global_const_decl:
 * ("var"|"const") IDENT ":" type "=" const_expr ";" */
static AstNode *parse_global_var(ParseCtx *c, bool is_pub, bool is_const)
{
    size_t s = c->pos;
    AstNode *node;

    advance(c);   /* var | const */
    if (!cur_is_ident(c)) {
        err_expected(c, "identifier");
        return NULL;
    }
    node = node_new(c, is_const ? AST_GLOBAL_CONST_DECL : AST_GLOBAL_VAR_DECL);
    if (!node) return NULL;
    node->u.global_decl.name = dup_str(c, cur_ident(c));
    if (!node->u.global_decl.name) { ast_node_free(node); return NULL; }
    node->u.global_decl.is_pub = is_pub;
    advance(c);
    if (!expect_punct(c, PUNCT_COLON)) { ast_node_free(node); return NULL; }
    node->u.global_decl.type = parse_type(c);
    if (!node->u.global_decl.type) { ast_node_free(node); return NULL; }
    if (!expect_punct(c, PUNCT_ASSIGN)) { ast_node_free(node); return NULL; }
    node->u.global_decl.init = parse_expr(c);
    if (!node->u.global_decl.init) { ast_node_free(node); return NULL; }
    if (!expect_punct(c, PUNCT_SEMI)) { ast_node_free(node); return NULL; }
    node->span = span_from(c, s, c->pos);
    return node;
}

/* top_level_decl = [ "pub" ] ( struct_decl | enum_decl | fn_decl |
 *                            global_var_decl | global_const_decl ) */
static AstNode *parse_top_level_decl(ParseCtx *c)
{
    bool is_pub = false;
    if (cur_is_kw(c, KW_PUB)) {
        is_pub = true;
        advance(c);
    }
    if (cur_is_kw(c, KW_STRUCT)) return parse_struct_decl(c, is_pub);
    if (cur_is_kw(c, KW_ENUM)) return parse_enum_decl(c, is_pub);
    if (cur_is_kw(c, KW_FN)) return parse_fn_decl(c, is_pub);
    if (cur_is_kw(c, KW_VAR)) return parse_global_var(c, is_pub, false);
    if (cur_is_kw(c, KW_CONST)) return parse_global_var(c, is_pub, true);
    err_expected(c, "declaration");
    return NULL;
}

/* `module` in the decls phase: the module declaration must be the first
 * element (AIC-S0103); consume the whole declaration and span it. */
static void report_module_not_first(ParseCtx *c)
{
    size_t s = c->pos;
    DiagSpan *sp;
    advance(c);   /* module */
    if (cur_is_ident(c)) {
        advance(c);
        while (cur_is_punct(c, PUNCT_DOT)) {
            advance(c);
            if (cur_is_ident(c)) advance(c);
            else break;
        }
    }
    if (cur_is_punct(c, PUNCT_SEMI)) advance(c);
    sp = span_from(c, s, c->pos);
    report_syntax(c, "AIC-S0103", "module declaration must be the first element", sp);
    diag_span_free(sp);
}

/* `import` in the decls phase: imports must precede top-level declarations
 * (AIC-S0102 at the import keyword); consume the import declaration. */
static void report_import_after_decls(ParseCtx *c)
{
    err_unexpected(c);
    while (!cur_is_eof(c) && !cur_is_punct(c, PUNCT_SEMI)) advance(c);
    if (cur_is_punct(c, PUNCT_SEMI)) advance(c);
}

/* A run of tokens that cannot begin any top-level construct (AIC-S0102
 * spanning the maximal run). */
static void report_unexpected_run(ParseCtx *c)
{
    size_t s = c->pos;
    DiagSpan *sp;
    while (!cur_is_eof(c) && !tok_starts_top_level(c) &&
           !cur_is_kw(c, KW_MODULE) && !cur_is_kw(c, KW_IMPORT)) {
        advance(c);
    }
    sp = span_from(c, s, c->pos);
    report_syntax(c, "AIC-S0102", "unexpected token", sp);
    diag_span_free(sp);
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

static void parse_ctx_clear(ParseCtx *c)
{
    while (c->nscopes > 0) scope_pop(c);
    free(c->scopes);
    c->scopes = NULL;
    for (size_t i = 0; i < c->ntype_names; i++) free(c->type_names[i]);
    free(c->type_names);
    c->type_names = NULL;
}

ParseStatus parse_program(const LexToken *tokens, size_t token_count,
                          AstNode **out_program,
                          DiagRecord ***out_records, size_t *out_record_count)
{
    ParseCtx c;
    AstNode *module_decl = NULL;
    AstNode **imports = NULL;
    size_t nimports = 0, imports_cap = 0;
    AstNode **decls = NULL;
    size_t ndecls = 0, decls_cap = 0;
    AstNode *program = NULL;

    *out_program = NULL;
    *out_records = NULL;
    *out_record_count = 0;

    memset(&c, 0, sizeof(c));
    c.tokens = tokens;
    c.count = token_count;
    c.pos = 0;
    c.file = (tokens != NULL && tokens[0].span != NULL) ? tokens[0].span->file : "input.ai";

    prescan_type_names(&c);

    /* module declaration. A missing leading `module` is NOT a parse error:
     * the accepted corpus (derived-syntax-module-not-first) expects exactly
     * one record for a misplaced module and none for an absent one; a module
     * that appears after other elements is reported as AIC-S0103 below. */
    if (!c.oom && cur_is_kw(&c, KW_MODULE)) {
        module_decl = parse_module_decl(&c);
    }

    /* imports */
    while (!c.oom && cur_is_kw(&c, KW_IMPORT)) {
        AstNode *imp = parse_import(&c);
        if (imp) {
            if (!node_push(&c, &imports, &nimports, &imports_cap, imp)) {
                ast_node_free(imp);
                break;
            }
        } else {
            if (c.oom) break;
            recover_top_level(&c);
        }
    }

    /* top-level declarations */
    while (!c.oom && !cur_is_eof(&c)) {
        if (cur_is_kw(&c, KW_MODULE)) {
            report_module_not_first(&c);
            continue;
        }
        if (cur_is_kw(&c, KW_IMPORT)) {
            report_import_after_decls(&c);
            continue;
        }
        if (!tok_starts_top_level(&c)) {
            report_unexpected_run(&c);
            continue;
        }
        AstNode *decl = parse_top_level_decl(&c);
        if (decl) {
            if (!node_push(&c, &decls, &ndecls, &decls_cap, decl)) {
                ast_node_free(decl);
                break;
            }
        } else {
            if (c.oom) break;
            recover_top_level(&c);
        }
    }

    if (c.oom) {
        ast_node_free(module_decl);
        for (size_t i = 0; i < nimports; i++) ast_node_free(imports[i]);
        free(imports);
        for (size_t i = 0; i < ndecls; i++) ast_node_free(decls[i]);
        free(decls);
        for (size_t i = 0; i < c.rcount; i++) diag_record_free(c.records[i]);
        free(c.records);
        parse_ctx_clear(&c);
        return PARSE_OOM;
    }

    program = node_new(&c, AST_PROGRAM);
    if (!program) {
        ast_node_free(module_decl);
        for (size_t i = 0; i < nimports; i++) ast_node_free(imports[i]);
        free(imports);
        for (size_t i = 0; i < ndecls; i++) ast_node_free(decls[i]);
        free(decls);
        for (size_t i = 0; i < c.rcount; i++) diag_record_free(c.records[i]);
        free(c.records);
        parse_ctx_clear(&c);
        return PARSE_OOM;
    }
    program->u.program.module_decl = module_decl;
    program->u.program.imports = imports;
    program->u.program.nimports = nimports;
    program->u.program.decls = decls;
    program->u.program.ndecls = ndecls;
    program->span = span_from(&c, 0, c.count);

    diag_sort_records(c.records, c.rcount);

    *out_program = program;
    *out_records = c.rcount ? c.records : NULL;
    *out_record_count = c.rcount;

    parse_ctx_clear(&c);
    return c.rcount ? PARSE_DIAG_ERROR : PARSE_OK;
}

void parse_records_free(DiagRecord **records, size_t count)
{
    for (size_t i = 0; i < count; i++) diag_record_free(records[i]);
    free(records);
}
