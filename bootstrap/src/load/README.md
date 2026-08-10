# bootstrap/src/load — WP-M0-07 Source loader / UTF-8 validation

Owned area: `bootstrap/src/load/**` and `bootstrap/build/load.txt` (work-package
manifest WP-M0-07, milestone plan line 66: "load/ # source loader, UTF-8
validation").

The loader reads a source file as bytes, validates the encoding per spec §3.1,
normalizes line terminators, and produces the canonical text plus a span index
that every later stage uses for 1-based line/col and 0-based byte offsets.
This package is the single place that guarantees offset→(line,col) consistency
(see diag.h: "the loader package WP-M0-07 performs terminator normalization").

## Scope

- File reading (binary mode; CRLF is handled by this package, never by the
  CRT text-mode layer).
- UTF-8 validation state machine (spec §3.1): no overlong encodings, no
  surrogate code points, no out-of-range code points, no invalid lead or
  stray continuation bytes, no truncated sequences at EOF.
- BOM (EF BB BF) at the start of a file → `AIC-L0002` (primary span: the BOM
  bytes).
- NUL byte (U+0000) outside a string literal or comment → `AIC-L0003`
  (primary span: the NUL byte).
- Line terminator normalization: CRLF → LF; a lone CR is whitespace (spec
  §3.1/§4.1) and never starts a line.
- Span computation: 1-based lines, 1-based UTF-8 byte columns, 0-based byte
  offsets in the normalized text (contract §6).
- Error records via WP-M0-06 (`bootstrap/src/diag`).

Explicitly out of scope: tokenization, escape processing, literal values,
comment/string *diagnostics* (AIC-L0004..L0009), module resolution, and any
driver behavior (all later work packages).

## Design decisions

1. **NUL context (spec §3.1 qualifier).** The spec rejects a NUL "outside a
   string literal or comment"; spec §4.4 makes a raw NUL a legal string
   character. Honoring both requires lexical context, which the loader does
   not have (tokenization is WP-M0-08). The validation pass therefore tracks
   line-comment / block-comment / string-literal boundaries **only** to
   classify NUL bytes. It produces no tokens, validates no escapes, and
   computes no literal values. Recovery for malformed constructs is
   deterministic:
   - a line comment ends at LF (a lone CR is whitespace and does not end it);
   - a block comment runs to `*/` or EOF (an unterminated block comment is
     the lexer's AIC-L0004; the loader stays in comment context, so NULs
     inside it are not misclassified);
   - a string ends at an unescaped `"`; `\` skips the next byte; a raw LF
     inside a string ends the string context (the lexer reports AIC-L0007
     later), so NULs on later lines are classified as code.
2. **AIC-L0001 span policy.** The primary span is the malformed run as
   consumed, following the Unicode standard's maximal-subpart behavior
   (Unicode D92):
   - invalid lead byte / stray continuation: the single byte;
   - truncated sequence at EOF: the lead byte plus the valid continuations
     consumed;
   - lead byte followed by a non-continuation byte: the lead byte alone (the
     following byte is a fresh character and is not swallowed);
   - completed overlong / surrogate / out-of-range sequence: all bytes of the
     sequence.
   This matches the negative corpus (`derived-lex-invalid-utf8`: a single
   0xFF at offset 26 spans exactly one byte).
3. **Offset space.** All spans/offsets are in the line-terminator-normalized
   text (CRLF counted as one LF; the CR byte is not part of any offset). This
   is the canonical text every downstream stage consumes.
4. **All load-level errors are reported.** A file with several load failures
   yields several records, sorted with the contract §9 comparator.
5. **I/O failures carry no diag record.** The registry has no file-read code
   (AIC-BL0802/0803 are build-phase module-resolution codes owned by
   WP-M0-10/driver). `load_source_from_file` returns `LOAD_IO_ERROR` and the
   driver decides how to report it.
6. **Embedded NULs.** The normalized text may contain NUL bytes (inside
   strings/comments); `len` is authoritative. The buffer is not a C string.

## API

`bootstrap/src/load/load.h`:

- `load_source_from_bytes(file, bytes, len, &src, &recs, &n)` — validate a
  byte buffer; `file` is the span file name (repository-relative by
  convention).
- `load_source_from_file(path, &src, &recs, &n)` — read + validate; `path` is
  used verbatim as the span file name; `LOAD_IO_ERROR` on unreadable file.
- `load_position(src, offset, &line, &col)` — 1-based line/col for an offset
  (offset == `len` is the EOF position).
- `load_span_point` / `load_span_range` — point / half-open range spans.
- Ownership: on `LOAD_OK` the caller frees the source with `load_source_free`;
  on `LOAD_VALIDATION_ERROR` the caller frees records with `load_records_free`.

## Build and test

MSVC (from the repository root):

```
STAGE0_OUT_DIR='bootstrap\stage0\msvc-load' \
  ./bootstrap/build/build-stage0-msvc.cmd \
  bootstrap/src/load/load_test.c bootstrap/src/load/load.c \
  bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
  bootstrap/src/diag/diag_emit.c
./bootstrap/stage0/msvc-load/load_test.exe
```

Clang-cl:

```
STAGE0_OUT_DIR='bootstrap\stage0\clang-load' \
  ./bootstrap/build/build-stage0-clang.cmd \
  bootstrap/src/load/load_test.c bootstrap/src/load/load.c \
  bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
  bootstrap/src/diag/diag_emit.c
./bootstrap/stage0/clang-load/load_test.exe
```

The test program writes scratch files under `bootstrap/stage0/` (gitignored);
build the target first so the directory exists.

`load_test.c` covers: valid ASCII + 2/3/4-byte code points; UTF-8 boundary
code points (U+007F, U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF);
BOM at start / mid-file / with a second error; NUL in code, line comment,
block comment, unterminated block comment, string, string with escapes, and
recovery after a raw LF in a string; a table of invalid sequences with exact
expected spans (overlong, surrogate, out-of-range, stray continuation,
truncated, invalid lead, lead-then-non-continuation); CRLF / LF / lone-CR /
empty / trailing-LF positions; byte columns for multi-byte characters;
record validation + single-line JSONL emission; file reading including a
missing file.

## Known limitations / residual risk

- The NUL-context scanner deliberately mirrors the spec's comment/string
  rules at a minimum level. If WP-M0-08's lexer later implements different
  recovery for malformed strings/comments, the two must stay consistent;
  the loader's recovery choices are documented above and deterministic.
- The no-argument aggregate build (which links `diag.txt` + `load.txt`)
  still fails at LINK until a driver/program fragment with `main` lands
  (recorded residual risk for WP-M0-06; the aggregate compiles cleanly).
