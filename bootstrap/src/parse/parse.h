/* bootstrap/src/parse/parse.h
 *
 * AI-Co Stage-0 recursive-descent parser (WP-M0-09).
 *
 * Consumes the deterministic token stream (WP-M0-08) and produces the
 * single-meaning AST (WP-M0-09 ast package) plus syntax-level diagnostics
 * (AIC-S0101..AIC-S0104) per the diagnostic contract sec. 11.2.
 *
 * Behavior contract (documented in README.md):
 *  - The parser assumes a token stream ending in exactly one EOF token
 *    (the lexer guarantees this even when lex diagnostics exist).
 *  - Grammar-level rejections: AIC-S0101 (expected token), AIC-S0102
 *    (unexpected token), AIC-S0103 (module declaration not first),
 *    AIC-S0104 (controlled body without braces). All phase "syntax",
 *    severity "error".
 *  - Deterministic recovery: the first syntax error in a file is marked
 *    "authoritative"; every later syntax error in the same parse is marked
 *    "recovery_derived" (the parser is in recovery state after the first
 *    failure). Recovery skips are silent (no records during a skip).
 *  - The AST contains only fully-parsed constructs; constructs that fail
 *    are dropped (with their partial children freed). A tree returned with
 *    diagnostics is best-effort and must not be processed by downstream
 *    stages (the driver stops when diagnostics exist).
 *  - Records are sorted with the contract sec. 9 comparator before being
 *    returned.
 *
 * Ownership:
 *  - On PARSE_OK / PARSE_DIAG_ERROR, *out_program and (when non-empty)
 *    *out_records / *out_record_count are owned by the caller
 *    (ast_node_free / parse_records_free).
 *  - On PARSE_OOM nothing is allocated.
 */
#ifndef AICO_BOOTSTRAP_SRC_PARSE_PARSE_H
#define AICO_BOOTSTRAP_SRC_PARSE_PARSE_H

#include "../ast/ast.h"
#include "../lex/lex.h"

#include <stddef.h>

typedef enum ParseStatus {
    PARSE_OK = 0,            /* AST produced, no diagnostics */
    PARSE_DIAG_ERROR,        /* AST produced AND diagnostics exist */
    PARSE_OOM                /* allocation failure; nothing produced */
} ParseStatus;

/* Parse a token stream (must end with one EOF token). */
ParseStatus parse_program(const LexToken *tokens, size_t token_count,
                          AstNode **out_program,
                          DiagRecord ***out_records, size_t *out_record_count);

void parse_records_free(DiagRecord **records, size_t count);

#endif /* AICO_BOOTSTRAP_SRC_PARSE_PARSE_H */
