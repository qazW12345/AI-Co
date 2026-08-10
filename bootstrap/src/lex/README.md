# bootstrap/src/lex — WP-M0-08 Deterministic lexer

Owned area: `bootstrap/src/lex/**` and `bootstrap/build/lex.txt` (work-package
manifest WP-M0-08). Consumes a WP-M0-07 `LoadSource` (line-terminator
normalized UTF-8 text) and produces the deterministic token stream plus
lex-level diagnostics (AIC-L0001, AIC-L0004..AIC-L0009) per spec §4 and the
diagnostic contract §11.1.

## Scope

- Identifiers and keywords (spec §4.2): ASCII `[A-Za-z_][A-Za-z0-9_]*`,
  case-sensitive, with the 40 reserved words recognized as keyword tokens.
- Integer literals (spec §4.3): decimal / hex / binary / octal, `_`
  separators (between two digits only), optional type suffix
  (`i8 i16 i32 i64 u8 u16 u32 u64 isize usize`), unsuffixed typing
  (fits `i32` → `i32`, else `i64`, else `u64`, else AIC-L0006), the
  unary-minus minimum-value rule (`-128i8` etc.), and per-literal rejection
  AIC-L0005/L0006.
- String literals (spec §4.4): escapes (`\0 \n \r \t \\ \" \xHH`),
  byte-level `\xHH` expansion, UTF-8 validity of the decoded (concatenated)
  bytes (AIC-L0009), raw LF rejection (AIC-L0007), invalid escape rejection
  (AIC-L0008), adjacent-literal concatenation, and AIC-L0001 for an
  unterminated literal.
- Punctuation (spec §4.6): longest-match single/multi-character tokens
  including `-> .. << >> <<= >>= += -= *= /= %= &= |= ^= && || == != <= >=`
  and the single `|` / `^` binary operators used by the §5.2 grammar
  (`bit_or_expr` / `bit_xor_expr`).
- Comments (spec §4.1): `//` line comments (through the line terminator,
  exclusive) and non-nesting `/* ... */` block comments; an unterminated
  block comment is AIC-L0004 (span `/*`..EOF). Comments are whitespace:
  `a/**/b` is two tokens.

Explicitly out of scope: grammar/parsing (WP-M0-09), name resolution
(WP-M0-10), type checking (WP-M0-11), and anything after the token stream.
Load-level rejections (AIC-L0001..L0003 from WP-M0-07) are not re-emitted.

## API

`bootstrap/src/lex/lex.h`:

- `lex_tokenize(src, &tokens, &count, &records, &record_count)` — lex one
  loaded source. Returns `LEX_OK` (no diagnostics), `LEX_DIAG_ERROR`
  (token stream AND diagnostics), or `LEX_OOM`.
- `lex_tokens_free` / `lex_records_free` — ownership (caller frees).
- `lex_keyword_text` / `lex_punct_text` / `lex_int_type_text` — exact source
  spellings for the token enums.

Token model: every token carries a `DiagSpan`; integer tokens carry the
resolved type, the magnitude (unsigned value), and `is_min` (accepted via
the unary-minus minimum-value rule; value == 2^(width-1)); string tokens
carry decoded bytes (escapes expanded, adjacent literals concatenated) and
their byte length (may contain NULs; length is authoritative).

## Design decisions

1. **Best-effort stream + authoritative diagnostics.** All lex failures are
   reported (never silent recovery, spec §4.6); a best-effort token stream
   (always ending in one EOF token) is still produced so the parser can apply
   contract §7 recovery. Deterministic recovery choices:
   - A rejected integer literal (L0005/L0006) emits no token; scanning
     resumes after the literal.
   - A string with an invalid escape (L0008) or invalid UTF-8 (L0009) emits
     no STR token; scanning resumes after the closing quote (or EOF).
   - A raw LF inside a string (L0007) emits no STR token; the lexer scans to
     the closing quote in discard mode (no further diagnostics for that
     string). EOF before the closing quote is ADDITIONALLY AIC-L0001
     (unterminated) — reported in discard mode too.
   - A malformed character (L0001) emits no token; scanning resumes after the
     offending character / maximal malformed run (for a valid UTF-8 code
     point in code position, the full code point bytes).
2. **Unary-minus minimum-value rule (spec §4.3).** The lexer decides whether
   a `-` is in unary position from the previous significant token:
   `-` is unary iff the preceding token cannot end an expression. A token
   ends an expression iff it is an identifier, an integer/string literal,
   `true`/`false`/`null`, `)`, or `]`. `}` is deliberately NOT
   expression-ending: `{block} -128i8;` (a block statement followed by a
   unary-minus expression statement) must not be rejected at lex time; a
   struct-init followed by `- 128i8` is a type error the parser rejects
   anyway (spec §12.7), so no valid program is misclassified. The rule
   depends only on the grammar relation between `-` and the literal token,
   never on whitespace/comments between them (`-128i8`, `- 128i8`,
   `-/*c*/128i8` all lex the same; `x - 128i8` and `-(128i8)` do not).
3. **Suffix word boundary.** A suffix is recognized only at a word boundary:
   `123i8x` lexes as INT(123) IDENT(i8x), not a suffixed literal. This is the
   deterministic reading of spec §4.3 ("optional suffix selects the
   literal's type" — the type suffixes are exactly the ten listed names).
4. **Malformed literal shapes.** `0x`/`0b`/`0o` with no digits, `0x_` with
   only underscores, and a bare decimal starting with `0` plus more content
   (`0123`) are malformed tokens → AIC-L0001 (not L0005). Leading, trailing,
   doubled, or between-non-digit `_` is AIC-L0005 with span the literal
   (including any suffix). One record per literal.
5. **UTF-8 revalidation.** The loader validated the source bytes; the lexer
   re-applies the same UTF-8 rules (no overlong, no surrogates, no
   out-of-range, no stray continuation, no truncation) to bytes produced by
   `\xHH` escapes in the decoded (concatenated) string, per spec §4.4
   AIC-L0009.
6. **NUL consistency with WP-M0-07.** The loader's NUL-context scanner
   (README in `bootstrap/src/load`) mirrors the spec's string/comment rules
   to classify NUL bytes; the lexer is consistent: a raw NUL inside a string
   is a legal string character (appended), a raw NUL inside a comment is
   consumed as trivia, and a NUL in code was already rejected by the loader
   (the pipeline stops on LOAD_VALIDATION_ERROR). `\0` escapes produce NUL
   bytes in decoded string data.

## Diagnostics emitted (contract §11.1; all phase `lex`, severity `error`,
recovery `authoritative`)

| Code | Condition | Primary span |
|---|---|---|
| AIC-L0001 | invalid character in source / malformed token (incl. unterminated string) | offending character / maximal malformed run / opening quote..EOF |
| AIC-L0004 | unterminated block comment | `/*`..EOF |
| AIC-L0005 | misplaced `_` in integer literal | the literal (incl. suffix) |
| AIC-L0006 | integer literal value not representable in its type | the literal |
| AIC-L0007 | line terminator inside string literal | the terminator |
| AIC-L0008 | invalid escape sequence | the escape (e.g. `\x` plus present hex digits) |
| AIC-L0009 | decoded string bytes not valid UTF-8 | the literal |

Records are sorted with the contract §9 comparator before return.

## Build and test

The area source list is aggregated by the stage-0 entry points via
`bootstrap/build/lex.txt`. Unit tests build and run from the repository root:

```sh
STAGE0_OUT_DIR='bootstrap\stage0\msvc-lex' ./bootstrap/build/build-stage0-msvc.cmd \
    bootstrap/src/lex/lex_test.c bootstrap/src/lex/lex.c \
    bootstrap/src/load/load.c \
    bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
    bootstrap/src/diag/diag_emit.c
./bootstrap/stage0/msvc-lex/lex_test.exe
```

Repeat with `build-stage0-clang.cmd` / `bootstrap\stage0\clang-lex` for the
Clang build. Expected: `lex_test: N checks, 0 failures`, exit 0.

`lex_test.c` covers: golden token-stream tests for every token family
(identifiers, all 40 keywords, all punctuation with longest-match spans,
integer literals across bases/suffixes/typing, string escapes/hex/UTF-8/
concatenation, comments), the unary-minus minimum-value rule (all four spec
forms plus the three rejected forms), negative tests for AIC-L0001/L0004..
L0009 with exact spans, record validation/emission and deterministic
ordering, and re-execution of the lexer-owned negative-corpus anchors
against the real fixture files under `tests/negative/cases/` (read-only).

## Corpus notes

The lexer-owned negative-corpus anchors are: `18-1-lex-int-overrun`
(AIC-L0006), `18-1-lex-invalid-escape` (AIC-L0008),
`18-1-lex-unterminated-comment` (AIC-L0004),
`derived-lex-string-invalid-utf8-escape` (AIC-L0009),
`derived-lex-string-newline` (AIC-L0007). Each expects exactly one record;
`lex_test.c` asserts the exact codes, messages, and spans from the fixture
`expected.json` files.

**Routed to Planner (downstream gap):** `derived-lex-misplaced-underscore`
(fixture under `tests/negative/cases/`, owned by WP-M0-03) expects AIC-L0005
for the input `var x: i32 = 1_2_3_4_5_6_7_8_9_0;`. Per accepted spec §4.3,
every `_` in that literal sits between two digits, so the literal is VALID
(meta.json also cites §4.2, which is the identifiers section, not the
integer-literal section). The lexer therefore does not anchor that fixture;
`lex_test.c` asserts the correct behavior instead (no diagnostic, value
1234567890 typed `i32`). The fixture needs a Planner re-planning decision
(correct the input or expectation, or amend the spec/corpus); it will fail
the end-to-end negative suite until resolved. No spec or corpus file was
modified by this package.

## Ownership

Owned by WP-M0-08: `bootstrap/src/lex/**` and `bootstrap/build/lex.txt`.
Consumers include `lex.h` but do not modify the lexer area. WP-M0-09
(parser) is the next consumer (AST/parse, `bootstrap/src/ast/**` +
`bootstrap/src/parse/**`). A needed change to the lexer's public contract is
a downstream gap → Planner via the Coordinator per milestone plan §7.
