# bootstrap/src/diag — Stage-0 diagnostic infrastructure (WP-M0-06)

Implementation of the canonical diagnostics subsystem per
[DIAGNOSTIC-CONTRACT v0.1.1](../../../spec/DIAGNOSTIC-CONTRACT.md) (Accepted).

## Files

| File | Purpose |
|------|---------|
| `diag.h` | Public API: record/span/KV/cause/correction model, code registry, deterministic comparator, JSONL emitter. Consumers (`WP-M0-07` load, `WP-M0-08` lex, ...) include this header. |
| `diag_codes.c` | Code registry table: every code in contract §11.1–11.8, exactly once. Registry changes are contract changes (Planner-owned); this package rejects unknown codes as defects. |
| `diag.c` | Record model (owned strings/arrays, atomic setters), span helpers (`diag_span_new_*`, `diag_span_from_offset`), validation, trap factories, §9 comparator. |
| `diag_emit.c` | Growable byte buffer + JSONL emitter. Deterministic; canonical field order byte-matches the contract §12 example records. |
| `diag_test.c` | Unit test program (registry coverage, spans, validation, traps, escaping, ordering, golden fixture byte-comparison). |
| `golden/*.jsonl` | Golden fixtures: the five contract §12 example records byte-for-byte, plus `record-full-options.jsonl` (all optional fields) and `user-trap-null-span.jsonl` (null primary span). |
| `README.md` | This file. |

## Build and test

The area source list is aggregated by the stage-0 entry points via
`bootstrap/build/diag.txt`. Unit tests build and run from the repository
root (golden paths are repository-relative):

```sh
STAGE0_OUT_DIR='bootstrap\stage0\msvc-diag' ./bootstrap/build/build-stage0-msvc.cmd \
    bootstrap/src/diag/diag_test.c bootstrap/src/diag/diag.c \
    bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
./bootstrap/stage0/msvc-diag/diag_test.exe
```

Repeat with `build-stage0-clang.cmd` / `bootstrap\stage0\clang-diag` for the
Clang build. Expected: `diag_test: N checks, 0 failures`, exit 0.

## Emission contract (summary)

- One JSON object per line, LF-terminated, no embedded newlines, no
  trailing whitespace.
- Canonical field order: `schema_version`, `code`, `severity`, `phase`,
  `message`, `primary_span` (always present, object or `null`),
  `secondary_spans`, `recovery`, `causes`, `expected`, `actual`,
  `corrections`, `related`, `trap_code`, `exit_code`.
- `recovery` is required (validated) on `severity: "error"` records and on
  all trap records; optional on warnings and notes (a note inherits its
  parent error's marking, contract §7).
- Strings are RFC 8259 escaped (`\" \\ \b \f \n \r \t`, other control bytes
  as `\u00XX` lowercase hex); valid UTF-8 bytes ≥ 0x20 pass through raw.
- Output contains no timestamps and no absolute host paths.
- Unknown codes are rejected by `diag_record_set_code` /
  `diag_record_validate` / `diag_emit_record` (defect, contract §11.9).

## Deterministic ordering (contract §9)

`diag_record_compare` / `diag_sort_records` / `diag_emit_records_sorted`
order records by:

1. phase in the fixed order `lex < syntax < name < type < semantic < ir <
   backend < object < link < build` (trap records are emitted singly at
   trap time and are not batch-ordered; `trap` is appended as the terminal
   rank so the comparator is a total order — bounded implementation
   extension, see `diag.h`);
2. within a phase, records with a null `primary_span` before file-bearing
   records; null-span ties by code lexicographically;
3. file-bearing records by `primary_span.file` lexicographically, then
   `start.offset`, then code lexicographically.

## Ownership and deferrals

- Owned by WP-M0-06. The area source list (`bootstrap/build/diag.txt`) is
  maintained from this package; consumers include `diag.h` but do not add
  or reorder sources in `diag.txt`.
- **Recorded deferral (contract §1)**: the human-readable diagnostic
  renderer (secondary presentation) is not part of this package; the JSONL
  record is the canonical output. A text renderer will be a separate,
  later work package when the compiler driver needs it.
- **Recorded deferral (contract §6)**: byte columns are emitted as stored.
  Source loading (WP-M0-07) performs line-terminator and UTF-8
  normalization before lexing, so the loader is the single place that
  guarantees offset→(line,col) consistency.
