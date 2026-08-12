# bootstrap/src/parse — WP-M0-09 Recursive-descent parser

Owned area: `bootstrap/src/parse/**`, `bootstrap/src/ast/**`, and
`bootstrap/build/parse.txt` / `bootstrap/build/ast.txt` (work-package manifest
WP-M0-09). Consumes the deterministic token stream from WP-M0-08 and produces
the single-meaning AST plus grammar-level diagnostics `AIC-S0101..S0104` per
spec §5.2 and the diagnostic contract §11.2 / §7.

## Scope

- Complete coverage of the spec §5.2 grammar productions: module
  declaration, imports (dotted names), struct / enum / fn declarations,
  global `var`/`const` declarations, `pub` handling, parameters, fields,
  enum members (with optional values), statements (block, local var/const,
  if/else, while, for (all three clause forms), switch/case/default,
  break, continue, return, expression statement, empty statement), types
  (primitives, named, pointer, array with const length, slice), and
  expressions (literals, identifiers, array literals incl. repeat form,
  parens, unary, binary per §10.1 precedence, assignment, ternary,
  postfix index/slice/call/member/arrow, struct-init postfix, sizeof
  (type and expr), alignof, cast, wrap, len, ptr).
- Span preservation on every node: each `AstNode` carries an owned
  `DiagSpan` covering the exact source range of the construct.
- Grammar-level rejections only (`AIC-S0101` expected token, `AIC-S0102`
  unexpected token, `AIC-S0103` module declaration not first, `AIC-S0104`
  controlled body without braces). Name/type diagnostics are later
  packages (WP-M0-10 / WP-M0-11).
- Deterministic recovery per contract §7 with `recovery_derived` marking.

Explicitly out of scope: name resolution (WP-M0-10), type checking
(WP-M0-11), and anything after the AST. Lex/load-level rejections are not
re-emitted here.

## API

`bootstrap/src/parse/parse.h`:

- `parse_program(tokens, token_count, &program, &records, &record_count)` —
  parse one token stream (must end in exactly one EOF token). Returns
  `PARSE_OK` (AST, no diagnostics), `PARSE_DIAG_ERROR` (AST AND
  diagnostics), or `PARSE_OOM`.
- `parse_records_free` — ownership (caller frees).

AST ownership: `bootstrap/src/ast/ast.h` — `ast_node_free` releases a whole
subtree; `ast_dump` renders the deterministic text dump. Node definitions,
the dump format, and the ownership helpers live in the ast package.

## Design decisions

1. **Left-to-right, single-pass, no backtracking.** The parser is
   deterministic by construction: it never speculates, so every input has
   exactly one parse (acceptance criterion 1).
2. **Deterministic recovery.** The first syntax record in a file is
   `recovery=authoritative`; every later syntax record is
   `recovery=recovery_derived` (the parser is in recovery state after the
   first failure). Recovery itself never emits records; it consumes tokens
   to a deterministic resync point (statement boundary, top-level
   boundary, field boundary, or list separator). A construct that fails to
   parse is dropped from the AST entirely (its partial children are freed);
   the AST therefore contains only fully-parsed constructs and a parse with
   diagnostics must not be processed downstream.
3. **`sizeof` type-vs-expression disambiguation** (spec §5.2 note, §6.2
   single name space). A primitive type keyword → type operand. A
   top-level struct/enum name (pre-scanned, order-independent) that is not
   shadowed by a visible local value at the decision point → type operand.
   Everything else — including undeclared identifiers — → expression
   operand. So `sizeof(Point)` is `expr_sizeof_type` when `Point` is a
   declared struct, and `sizeof(p)` / `sizeof(q)` (undeclared) are
   `expr_sizeof_expr` (the golden dumps in g26/g28 pin these).
4. **Struct-init postfix vs block** (spec §12.7). A `{` following a
   postfix expression binds as struct-init postfix (`p { x: 1, y: 2 }`),
   never as a block; a `{` in expression primary position has no leading
   type and is rejected (AIC-S0101). Postfix operators apply after
   struct-init (`p { ... }.x` is member-on-struct-init, g29).
5. **Comments are whitespace** (spec §4.1). `a/**/b` is two identifiers
   with no operator between them — the comment never merges or separates
   tokens (g30; the lexer already guarantees two tokens).
6. **Precedence and associativity** (spec §5.2 / §10.1). Binary levels are
   left-associative in the §10.1 order (multiplicative, additive, shift,
   relational, equality, bit-and, bit-xor, bit-or, logical-and,
   logical-or). Assignment and ternary are right-associative; a
   right-nested ternary `a ? b : c ? d : e` nests the else branch (g24).
7. **Type postfix order** (spec §5.2). Postfixes bind tighter than
   array-element pointers: `u8*[4]` is array of pointer-to-u8,
   `u8[4]*` is pointer to array, `u8*[]` is slice of pointer,
   `u8[]*` is pointer to slice (g27). This is implemented by parsing the
   base type, then consuming `*`/`[]`/`[len]` postfixes from right to
   left relative to the base.
8. **`pub` handling.** `pub` is accepted only on top-level declarations
   (struct/enum/fn/global var/const); it is recorded per-declaration
   (`is_pub`). No `pub` inside blocks or on parameters/fields — those are
   AIC-S0102 unexpected-token cases.
9. **Module declaration.** A missing leading `module` is NOT a parse
   error (an absent module declaration is accepted; a `module` appearing
   after other top-level elements is AIC-S0103). This matches the accepted
   corpus fixture `derived-syntax-module-not-first`, which expects exactly
   one record.
10. **NUL/EOF safety.** The parser trusts the loader/lexer guarantees
    (valid UTF-8 text, token stream ending in exactly one EOF token) and
    always terminates on EOF; the `cur_is_eof` guard is checked at every
    loop and entry point.
11. **Missing initializer on `var` declarations** (spec §5.2 v0.1.3,
    Planner ruling t_dcb5540e). `var_decl` and `global_var_decl` accept an
    optional `"=" expr`; a missing initializer leaves `init == NULL` with
    **no** syntax record and the declaration is kept in the AST (it is not
    dropped by recovery). The rejection is the semantic rule `AIC-E0403`
    (spec §8.2), emitted later in the pipeline. `const_decl` /
    `global_const_decl` keep the strict grammar: a missing `=` is still
    `AIC-S0101` and the declaration is dropped by recovery (unchanged).
    If both `=` and `;` are missing (for example `var x: i32` at EOF), the
    lenient grammar skips the absent initializer and the single diagnostic
    becomes `AIC-S0101 "expected ';'"` (previously `"expected '='"`); still
    exactly one syntax record, no pinned test changes.
    Note: `ast_dump()` in the ast package does not yet render a declaration
    with `init == NULL` (`dump_node` requires a non-NULL child), so the
    parse tests assert the lenient form structurally on the AST rather
    than via golden dumps. That dump limitation is tracked to its follow-up
    owner WP-M0-13a2 (task t_b174081d).

## Diagnostics emitted (contract §11.2; all phase `syntax`, severity
`error`)

| Code | Condition | Primary span |
|---|---|---|
| AIC-S0101 | expected token (e.g. `';'`, `')'`, `'}'`, identifier, expression) | the offending token / gap at the expected position |
| AIC-S0102 | unexpected token (e.g. `pub` misuse, trailing garbage, stray `}`) | the unexpected token |
| AIC-S0103 | module declaration not the first element | the `module` keyword..end of declaration |
| AIC-S0104 | controlled body (if/while/for) without braces | the first statement of the controlled body |

The first syntax record in a file is `recovery=authoritative`; every later
syntax record is `recovery=recovery_derived`. Records are sorted with the
contract §9 comparator before return.

## Build and test

The area source lists are aggregated by the stage-0 entry points via
`bootstrap/build/parse.txt` (parser) and `bootstrap/build/ast.txt` (ast).
Unit tests build and run from the repository root:

```sh
STAGE0_OUT_DIR='bootstrap\stage0\msvc-parse' ./bootstrap/build/build-stage0-msvc.cmd \
    bootstrap/src/parse/parse_test.c bootstrap/src/parse/parse.c \
    bootstrap/src/ast/ast.c bootstrap/src/lex/lex.c \
    bootstrap/src/load/load.c \
    bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
    bootstrap/src/diag/diag_emit.c
./bootstrap/stage0/msvc-parse/parse_test.exe
```

Repeat with `build-stage0-clang.cmd` / `bootstrap\stage0\clang-parse` for the
Clang build. Expected: `parse_test: N checks, 0 failures`, exit 0.

`parse_test.c` covers:

- Golden AST-dump tests: every spec §18.2 valid program and the derived
  grammar cases in `golden_cases.h` (g01..g30) parsed and dumped
  byte-for-byte against the committed expected text.
- Determinism: the same tree dumped twice is byte-identical, and two
  independent parses of the same input produce identical dumps.
- Ambiguity-prone forms asserted by exact dump shape: sizeof type-vs-expr
  (declared type, prim type, value, undeclared, arithmetic expression),
  struct-init postfix (incl. member-after-struct-init), the comment-split
  two-token rule, ternary right-associativity, and all four postfix-type
  orders.
- Grammar-level rejections AIC-S0101..S0104 with exact codes, messages,
  phases, recovery flags, and spans (single- and two-error files), plus
  record validation and deterministic JSONL emission with
  `recovery_derived` visible.
- Missing-initializer leniency (Planner ruling t_dcb5540e): local and
  module-scope `var` declarations without `=` parse with `init == NULL`
  and no syntax record (asserted structurally on the AST, incl. for-init
  reuse); `const` forms with a missing `=` still report `AIC-S0101
  "expected '='"` and are dropped by recovery.
- Re-execution of the parser-owned negative-corpus anchors against the
  real fixture files under `tests/negative/cases/` (read-only).

## Corpus notes

The parser-owned negative-corpus anchors are: `18-2-syntax-brace-no-type`
(AIC-S0101), `derived-syntax-if-no-braces` (AIC-S0104),
`derived-syntax-missing-semicolon` (AIC-S0101),
`derived-syntax-module-not-first` (AIC-S0103),
`derived-syntax-unexpected-token` (AIC-S0102). Each expects exactly one
record; `parse_test.c` asserts the exact codes, messages, and spans from the
fixture `expected.json` files.

## Ownership

Owned by WP-M0-09: `bootstrap/src/ast/**`, `bootstrap/src/parse/**`,
`bootstrap/build/ast.txt`, `bootstrap/build/parse.txt`. Consumers include
`parse.h`/`ast.h` but do not modify this area. WP-M0-10 (name resolution) is
the next consumer. A needed change to the parser's public contract is a
downstream gap → Planner via the Coordinator per milestone plan §7.
