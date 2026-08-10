# bootstrap/src/ast — WP-M0-09 Abstract syntax tree

Owned area: `bootstrap/src/ast/**`, `bootstrap/src/parse/**`, and
`bootstrap/build/ast.txt` / `bootstrap/build/parse.txt` (work-package
manifest WP-M0-09). Owns the node definitions consumed by the parser
(`bootstrap/src/parse/parse.c`) plus the deterministic dump and ownership
helpers. See `bootstrap/src/parse/README.md` for the parser's behavior
contract.

## Scope

- Node kind table covering every spec §5.2 grammar production (see
  `AstNodeKind` in `ast.h`): top-level declarations, statements, types,
  and expressions.
- Span preservation: every constructed node carries an owned `DiagSpan`
  (`ast.h` rule: spans are exact source ranges of the construct).
- Deterministic text dump (`ast_dump`): one line per node with indentation,
  field label, node kind, key attributes (names, operator spellings,
  literal types/values, counts), and the source span `[start,end)`.
  Deterministic for a given tree: no addresses, no host paths, no
  timestamps. LF-only output.
- Ownership helpers: `ast_node_free` releases a whole subtree; `ast_name_*`
  owns dotted names (module/import/named-type).

Explicitly out of scope: parsing (WP-M0-09 parse package), name resolution
(WP-M0-10), type checking (WP-M0-11), and anything after the tree. The AST
is a syntax-only representation; semantic validation happens downstream.

## Design decisions

1. **Every node has a span.** The dump anchors on 0-based byte offsets of
   the normalized text (contract §6); tests pin exact offsets.
2. **Children arrays are owned heap arrays.** `ast_node_free` recursively
   frees children, names, and spans; `NULL` is accepted everywhere.
3. **Strings are heap copies.** Callers that construct nodes manually
   (tests) must copy strings before attaching them.
4. **Tree contains only fully-parsed constructs.** When the parser reports
   a syntax error inside a construct, that construct is dropped; a tree
   returned with diagnostics is best-effort and must not be processed by
   downstream stages (the driver stops when diagnostics exist).
5. **Dump is the deterministic test oracle.** The golden AST-dump tests
   compare `ast_dump` output byte-for-byte against committed expected text
   (`bootstrap/src/parse/golden_cases.h`), and determinism tests dump the
   same tree twice / parse the same input twice.

## API

`bootstrap/src/ast/ast.h`:

- `ast_node_free(node)` — free a node and its entire subtree.
- `ast_name_new(parts, count)` / `ast_name_free` / `ast_name_to_string` —
  dotted-name helpers (module/import/named-type).
- `ast_prim_from_keyword(kw)` — primitive type kind from a lex keyword, or
  -1 when the keyword is not a primitive type keyword.
- `ast_dump_init` / `ast_dump_free` / `ast_dump_ok` / `ast_dump(node, out)`
  — deterministic dump (returns false only on OOM).

## Build and test

The area source list is aggregated by the stage-0 entry points via
`bootstrap/build/ast.txt`. Unit tests for both packages live in
`bootstrap/src/parse/parse_test.c` (the ast package has no standalone test
program; its behavior is exercised through the parser's golden-dump and
determinism tests). Build and run instructions: see
`bootstrap/src/parse/README.md`.

## Ownership

Owned by WP-M0-09: `bootstrap/src/ast/**`, `bootstrap/src/parse/**`,
`bootstrap/build/ast.txt`, `bootstrap/build/parse.txt`. It consumes the
lexer's `LexIntType` for integer-literal typing (the lexer resolves literal
types per spec §4.3); it must not be modified outside this package without a
Planner re-planning decision.
