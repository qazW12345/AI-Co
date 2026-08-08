# AI-Co Diagnostic Contract v0.1.1 (Proposed)

**Status:** Proposed
**Owner:** Planner
**Decision owner:** Main Designer (architecture); decisions recorded in ADR-004 are Human Sponsor approvals and are applied here as governing direction.
**Approver:** Main Designer (architectural acceptance); Reviewer (independent conformance review).
**Version:** 0.1.1
**Schema version:** 1
**Date:** 2026-08-08
**Scope:** project AI-Co
**Companion to:** `spec/AI-CO-LANGUAGE-SPECIFICATION.md` (v0.1.1, Proposed)

This document defines the normative, versioned JSON Lines diagnostic contract for the AI-Co minimal language and compiler, and the proposed stable diagnostic-code and runtime-trap-code namespace.

Governing sources: `docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md` (Accepted), `docs/adr/ADR-002-minimal-core-language-semantics.md` (Accepted), `docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md` (Superseded by ADR-004), `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (Accepted), `spec/AI-CO-LANGUAGE-SPECIFICATION.md` (Proposed v0.1.1).

---

## 1. Purpose

Structured diagnostics are a normative compiler output, not an optional presentation feature (ADR-001, ADR-002). The canonical representation is a stream of JSON objects, one per line (JSON Lines, RFC 7464-safe encoding: UTF-8, LF-terminated, no embedded newlines). A human-readable renderer is secondary and derived from the same records; it never substitutes for the structured record.

The contract guarantees:

- stable, machine-addressable diagnostic codes;
- a deterministic total ordering of emitted diagnostics;
- causal information across compilation stages (the record carries the cause chain, not only the final symptom);
- recovery/cascade marking so agents can prioritize root causes;
- candidate corrections only when they can be generated without guessing intent;
- a runtime trap contract with the same schema.

---

## 2. Encoding and record shape

- Encoding: UTF-8 without BOM.
- One JSON object per line; LF line terminator; no trailing whitespace.
- Unknown fields in a record must be preserved by consumers and ignored by the compiler's own tooling; the schema is extensible forward-only.
- A record must contain the required fields below; optional fields may be omitted or null.

## 3. Required fields

| Field | Type | Meaning |
|---|---|---|
| `schema_version` | string | diagnostic contract schema version: `"1"` |
| `code` | string | stable diagnostic code, e.g. `"AIC-T0307"` (Section 5) |
| `severity` | string | `"error"` \| `"warning"` \| `"note"` |
| `phase` | string | compilation phase or `"trap"` (Section 4) |
| `message` | string | human-readable description (may improve between versions without changing the code) |
| `primary_span` | object \| null | primary source span (Section 6); null only when no source location exists (e.g., link-level or build-level records) |
| `recovery` | string | deterministic recovery/continuation marking (Section 7); **required** on `severity = "error"` records and on all trap records (`phase = "trap"`); `warning`/`note` records may omit it, and a `note` inherits the marking of its parent error |

## 4. Optional fields

| Field | Type | Meaning |
|---|---|---|
| `secondary_spans` | array of span | related locations (e.g., the earlier declaration of a duplicate, the other side of a mismatch, the import that closes a cycle) |
| `expected` | object \| null | structured expectation: what the rule required (e.g., `{"type": "bool"}`) |
| `actual` | object \| null | structured observation (e.g., `{"type": "i32"}`) |
| `causes` | array of cause | causal chain, root cause first; each cause is a record-shaped object with `code`, `message`, and optional `primary_span` |
| `corrections` | array of correction object | candidate corrections (Section 8); each is `{"replacement": string, "span": span \| null}`; omitted when none can be generated deterministically |
| `related` | object | declaration/scope/module/type context: e.g., `{"module": "a.b", "declaration": "f", "scope": "block", "type": "i32"}` |
| `trap_code` | number | for user traps via `rt.trap.report`: the caller-supplied `u32` code |
| `exit_code` | number | for trap records: the process exit code (default `70`) |

Phase values: `"lex"`, `"syntax"`, `"name"`, `"type"`, `"semantic"`, `"ir"`, `"backend"`, `"object"`, `"link"`, `"build"`, `"trap"`.

Severity semantics:

- `error`: the build is rejected / the process terminates.
- `warning`: does not reject the build; reserved for future diagnostic families. In v0.1.0 the minimal compiler emits no warnings by default; any future warning must be specified in a feature ADR before use.
- `note`: additional structured context attached to an error (e.g., the "declared here" span for a duplicate). Notes are always emitted immediately after their parent error and share its ordering position.

## 5. Stable code namespace

Code format: `AIC-<CLASS><NNNN>`, where `<CLASS>` is one of `L` (lex), `S` (syntax), `N` (name), `T` (type), `E` (semantic), `I` (IR), `B` (backend), `O` (object/link), `BL` (build), `R` (runtime trap), `U` (user trap), and `<NNNN>` is a four-digit sequence. The first two digits of `<NNNN>` are a phase group (00 lex, 01 syntax, 02 name, 03 type, 04 semantic, 05 IR, 06 backend, 07 object/link, 08 build/trap, 09 user) for human readability; the last two digits are the rule's sequence within that group. The `U` class is exempt from the phase-group mapping: `AIC-U0000` is the single user-trap marker, and its `00` digits are a fixed placeholder rather than a phase group (user traps are caller-supplied and not phase-scoped). Codes are stable: their meaning never changes; a code is never reused for a different rule; deprecated codes are retained in the namespace table and marked deprecated, never deleted.

## 6. Spans

A span object: `{"file": "<path relative to project root>", "start": {"line": L, "col": C, "offset": O}, "end": {"line": L2, "col": C2, "offset": O2}}`.

- Lines are 1-based; columns are 1-based UTF-8 byte columns within the line; offsets are 0-based byte offsets from the start of the file.
- The primary span is the smallest source region responsible for the problem (e.g., the offending literal, operator, identifier, or the case label lacking a terminator).
- A span may be empty (`start == end`) for a point location.

## 7. Recovery marking

- `authoritative`: the root-cause diagnostic; the compiler can continue deterministically but this record explains the primary failure.
- `cascading`: produced because an earlier error made the construct unusable; agents should fix authoritative errors first.
- `recovery_derived`: produced by the parser's deterministic recovery strategy (e.g., after a syntax error, the parser resynchronizes and reports a related problem). Recovery must be deterministic: the same input yields the same recovery records.
- Parser recovery is required to be deterministic; cascading diagnostics must be marked so agents can prioritize root causes (ADR-001).

## 8. Candidate corrections

A correction is a structured source-text suggestion, emitted only when it can be generated deterministically without guessing intent. Each correction object has:

| Field | Type | Meaning |
|---|---|---|
| `replacement` | string | the exact source text to insert or replace |
| `span` | span \| null | the source region the replacement applies to; when omitted, the correction is an insertion at the primary span's start |

Examples permitted in v0.1.1:

- missing `;` → `{"replacement": ";"}` (insertion at the primary span's start);
- undeclared name where exactly one candidate exists in scope → `{"replacement": "candidate_name", "span": {primary_span of the identifier}}`;
- `u8` assignment from an `i32` literal constant → `{"replacement": "cast<u8>(...)", "span": {...}}` or `{"replacement": "wrap<u8>(...)", "span": {...}}`.

When no deterministic correction exists, `corrections` is omitted.

## 9. Deterministic ordering

Records are emitted in a deterministic total order:

1. by `phase` in a fixed phase order (`lex`, `syntax`, `name`, `type`, `semantic`, `ir`, `backend`, `object`, `link`, `build`);
2. within a phase, records whose `primary_span` is `null` sort **before** all file-bearing records (records with a non-null `primary_span`); among null-span records, order by `code` lexicographically;
3. within a phase, file-bearing records sort by `primary_span.file` (lexicographic by relative path);
4. then by `primary_span.start.offset`;
5. then by `code` lexicographically.

This rule covers the null-span record classes of Sections 11.6 and 11.7 (`AIC-O0701`, `AIC-O0702`, `AIC-BL0801`–`AIC-BL0803`): within their phase they sort before file-bearing records, tie-broken by `code`.

Notes are emitted immediately after their parent error, in the same position. Cascading/recovery-derived records follow their triggering authoritative record. The ordering rule is part of the contract; tests may assert exact record sequences.

## 10. Runtime trap records

Runtime traps use the same schema with `phase = "trap"`, `severity = "error"`, `recovery = "authoritative"`, and `exit_code = 70` (or the `rt.proc.exit` code for explicit exits, which are not traps and emit no record). A trap record's `message` states the failing operation and values; `related` carries operation/type/value facts; `primary_span` carries the source location of the failing operation where available (the compiler must preserve source mapping for all checked operations, §14.1 of the language spec).

The process writes the trap record to `stderr` as one JSON line, then terminates with the trap exit code.

User traps via `rt.trap.report(code: u32, message: str)` emit a record with `code = "AIC-U0000"` and `trap_code = <caller code>`; exit code `70`.

## 11. Proposed diagnostic-code table

### 11.1 Lexical (`AIC-L`)

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-L0001` | invalid byte / invalid UTF-8 sequence / malformed character or token | error | offending byte(s) or maximal malformed run |
| `AIC-L0002` | UTF-8 BOM present | error | BOM bytes |
| `AIC-L0003` | NUL byte in source (outside literal/comment) | error | the NUL byte |
| `AIC-L0004` | unterminated block comment | error | from `/*` to EOF |
| `AIC-L0005` | misplaced `_` in integer literal | error | the literal |
| `AIC-L0006` | integer literal value not representable in its type | error | the literal |
| `AIC-L0007` | line terminator inside string literal | error | the terminator |
| `AIC-L0008` | invalid escape sequence | error | the escape |
| `AIC-L0009` | string literal bytes not valid UTF-8 after escape expansion | error | the literal |

### 11.2 Syntax (`AIC-S`)

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-S0101` | expected token (details in message) | error | offending token |
| `AIC-S0102` | unexpected token / excluded construct | error | offending token |
| `AIC-S0103` | module declaration not first element | error | the module decl |
| `AIC-S0104` | controlled body without braces | error | the statement head |

### 11.3 Name binding (`AIC-N`)

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-N0201` | duplicate declaration in same scope | error | later identifier; secondary span: earlier declaration |
| `AIC-N0202` | undeclared name | error | the identifier |
| `AIC-N0203` | access to private item from another module | error | the reference |
| `AIC-N0204` | imported module not found at canonical path | error | import qualified name |
| `AIC-N0205` | module declaration name does not match canonical path | error | module declaration |
| `AIC-N0206` | import cycle | error | the import closing the cycle; secondary spans: cycle members |
| `AIC-N0207` | module declaration uses the reserved `rt` prefix | error | the module declaration |
| `AIC-N0208` | import of reserved runtime submodule not in the runtime surface | error | the import qualified name |
| `AIC-N0209` | bare `import rt;` (import a specific runtime submodule instead) | error | the import qualified name |

### 11.4 Type (`AIC-T`)

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-T0301` | enum member value not representable in underlying type | error | the member |
| `AIC-T0302` | use of incomplete struct type as a value | error | the type reference |
| `AIC-T0303` | struct recursion by value (infinite size) | error | the recursive field |
| `AIC-T0304` | `==`/`!=` on array or struct type | error | the operator |
| `AIC-T0305` | chained comparison | error | the second comparison operator |
| `AIC-T0306` | operator not applicable to operand type | error | the operator |
| `AIC-T0307` | no common type / type mismatch (implicit conversion absent) | error | the mismatched operand/expression |
| `AIC-T0308` | invalid explicit cast pair | error | the cast expression |
| `AIC-T0309` | array literal element count mismatch | error | the literal |
| `AIC-T0310` | condition is not `bool` | error | the condition |
| `AIC-T0311` | switch selector is not integer/enum | error | the selector |
| `AIC-T0312` | call argument count mismatch | error | the call |
| `AIC-T0313` | struct literal field error (missing, duplicate, or unknown field) | error | the offending literal/field |

### 11.5 Semantic (`AIC-E`)

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-E0401` | expression is not a constant expression | error | the expression |
| `AIC-E0402` | address of const / address of non-lvalue | error | the operand |
| `AIC-E0403` | missing initializer on variable declaration | error | the declaration |
| `AIC-E0404` | assignment to const | error | the assignment |
| `AIC-E0405` | constant expression overflow (checked arithmetic) | error | the constant expression |
| `AIC-E0406` | constant division by zero | error | the constant expression |
| `AIC-E0407` | constant shift count out of range | error | the shift |
| `AIC-E0408` | constant cast out of range | error | the cast |
| `AIC-E0409` | constant index/slice bound out of range | error | the bound |
| `AIC-E0410` | constant str slice not on code point boundary | error | the slice |
| `AIC-E0411` | constant pointer difference not divisible by element size | error | the subtraction |
| `AIC-E0412` | switch case body missing terminating statement (no fall-through) | error | the case label/body |
| `AIC-E0413` | duplicate switch case value | error | the duplicate case label |
| `AIC-E0414` | break/continue outside loop (or break outside switch) | error | the statement |
| `AIC-E0415` | return value mismatch (value in void, or missing value in non-void) | error | the return |
| `AIC-E0416` | non-void function path without return | error | the function name |
| `AIC-E0417` | unreachable statement | error | the statement |
| `AIC-E0418` | entry `main` signature invalid / missing | error | the module / main decl |
| `AIC-E0419` | assignment to non-lvalue | error | the assignment target |

### 11.6 IR / backend / object / link (`AIC-I`, `AIC-B`, `AIC-O`)

These classes are reserved for internal invariant and artifact failures. In v0.1.0 a conforming compiler should never emit them for valid inputs; they exist so any internal failure is itself a stable diagnostic rather than silence.

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-I0501` | IR invariant violation (compiler internal) | error | derived span |
| `AIC-B0601` | backend constraint violation (e.g., unsupported target feature) | error | derived span |
| `AIC-O0701` | object emission failure / deterministic-artifact failure | error | null or derived span |
| `AIC-O0702` | link failure reported by external linker | error | null or derived span |

### 11.7 Build (`AIC-BL`)

| Code | Meaning | Severity | Primary span |
|---|---|---|---|
| `AIC-BL0801` | build manifest schema error | error | null |
| `AIC-BL0802` | project root missing/invalid | error | null |
| `AIC-BL0803` | entry module not found | error | null |

### 11.8 Runtime traps (`AIC-R`)

| Code | Meaning | Primary span |
|---|---|---|
| `AIC-R0801` | checked conversion out of range | failing operation |
| `AIC-R0802` | arithmetic overflow (checked) | failing operation |
| `AIC-R0803` | division by zero | failing operation |
| `AIC-R0804` | shift count out of range | failing operation |
| `AIC-R0805` | invalid `bool` byte value | byte access |
| `AIC-R0806` | invalid UTF-8 → `str` cast | failing operation |
| `AIC-R0807` | index/slice span out of bounds | failing operation |
| `AIC-R0808` | str slice not on code point boundary | failing operation |
| `AIC-R0809` | null pointer dereference | failing operation |
| `AIC-R0810` | pointer difference not divisible by element size | failing operation |
| `AIC-R0811` | invalid address/alignment memory access | failing operation |
| `AIC-R0812` | double release | failing operation |
| `AIC-R0813` | invalid release (pointer not from allocator) | failing operation |
| `AIC-R0814` | invalid/closed file handle | failing operation |
| `AIC-R0815` | stack exhaustion | failing operation |
| `AIC-R0816` | pointer arithmetic overflow (checked scaling / byte-difference overflow) | failing operation |
| `AIC-U0000` | user trap via `rt.trap.report` (numeric code in `trap_code`) | call site |

### 11.9 Code registry and allocation policy

- The table in Sections 11.1–11.8 is the **authoritative code registry** for schema version 1.
- **Allocation:** codes are allocated sequentially within each class's reserved range. A new rule requiring a diagnostic or trap receives the next unused code in the appropriate class (`L`, `S`, `N`, `T`, `E`, `I`, `B`, `O`, `BL`, `R`). Code allocation is a Planner-owned change to this document and requires review with the specification.
- **Stability:** once allocated, a code's meaning is immutable. A code is never deleted, reused, or re-scoped. If a rule is removed, its code is marked `deprecated` and retained in the registry with a note; `deprecated` codes are never re-emitted by conforming compilers.
- **Reserved ranges:** the numeric ranges within each class are allocated contiguously; gaps are reserved for future rules and may not be used out of sequence. User-defined trap codes never collide with `AIC-` codes: user codes are raw `u32` values carried in `trap_code`, outside the `AIC-` namespace.
- **Registry updates:** any addition, deprecation, or schema change must be recorded in this document with a version note and reviewed; the registry is the durable record for `FIND-005` closure.
- **v0.1.1 additions:** `AIC-N0207` and `AIC-N0208` were added for the reserved runtime-module rules of the language specification §6.5 (next unused codes in class `N`). `AIC-E0412`'s meaning text was narrowed to the brace-delimited case-body rule; the code, severity, and primary-span semantics are unchanged. In the gate-2 correction round, `AIC-N0209` (bare `import rt;`, language spec §6.5) and `AIC-R0816` (pointer arithmetic overflow, language spec §12.5) were added as the next unused codes in classes `N` and `R` respectively.

### 11.10 Compatibility rules

- **Forward compatibility:** consumers must tolerate unknown codes and unknown optional fields within a known `schema_version`. An unknown code with severity `error` must be treated as a build failure; its meaning may not be guessed.
- **Backward compatibility:** schema version 1 records remain valid for consumers written against schema version 1. Required fields are never renamed, removed, or re-typed within a schema version. New optional fields may be added within a schema version.
- **Schema versioning:** a change that alters the meaning of a required field, changes required-field structure, or changes the code allocation rules requires a new schema version (e.g., `"2"`) and a migration note. **Draft exemption:** a change made while the contract is still Proposed — before any conforming record of the affected shape has ever been emitted — does not trigger a version bump; the first accepted, emitted schema is the baseline that later changes must version. A conforming compiler emits exactly one `schema_version` for the whole build (except historical records replayed by tools, which preserve their original version).
- **Wording:** `message` text may change without a schema or code change; the stable code, structure, and facts are the contract.
- **Trap compatibility:** trap codes follow the same immutability rules; trap exit code is defined per Section 10 of this contract and Section 15.5 of `AI-CO-LANGUAGE-SPECIFICATION.md`.
- **v0.1.1 compatibility note:** this revision makes two structural corrections to the Proposed v0.1.0 draft: `recovery` is now required on `error` and trap records (Section 3), and `corrections` elements are structured objects (Sections 4 and 8). Schema version remains `"1"` because the v0.1.0 draft was never accepted or implemented, no record in the old shape was ever emitted by a conforming compiler, and both changes align the contract with its governing ADR-001 requirement (every diagnostic record carries a deterministic recovery/continuation status) and with this document's own prior prose (§8 already described corrections as replacement text plus optional span). Consumers of the corrected contract are the only conforming consumers; the required-field semantics and code registry defined here are the v1 baseline for implementation.

## 12. Example records

Valid program behavior:

```jsonl
{"schema_version":"1","code":"AIC-L0006","severity":"error","phase":"lex","message":"integer literal 300 is not representable in type u8","primary_span":{"file":"main.ai","start":{"line":4,"col":12,"offset":88},"end":{"line":4,"col":16,"offset":92}},"recovery":"authoritative","related":{"module":"main"}}
{"schema_version":"1","code":"AIC-N0203","severity":"error","phase":"name","message":"access to private item g in module a.b","primary_span":{"file":"main.ai","start":{"line":9,"col":14,"offset":210},"end":{"line":9,"col":17,"offset":213}},"secondary_spans":[{"file":"a/b.ai","start":{"line":5,"col":1,"offset":60},"end":{"line":5,"col":2,"offset":61}}],"recovery":"authoritative","related":{"module":"a.b","declaration":"g"}}
{"schema_version":"1","code":"AIC-S0101","severity":"error","phase":"syntax","message":"expected ';' after expression","primary_span":{"file":"main.ai","start":{"line":7,"col":19,"offset":120},"end":{"line":7,"col":20,"offset":121}},"recovery":"authoritative","corrections":[{"replacement":";"}]}
{"schema_version":"1","code":"AIC-E0412","severity":"error","phase":"semantic","message":"switch case 0 body lacks a terminating statement; fall-through is prohibited","primary_span":{"file":"main.ai","start":{"line":12,"col":7,"offset":300},"end":{"line":12,"col":12,"offset":305}},"recovery":"authoritative"}
```

Runtime trap record (on stderr at trap time):

```jsonl
{"schema_version":"1","code":"AIC-R0807","severity":"error","phase":"trap","message":"slice index 16 out of bounds (len 16)","primary_span":{"file":"main.ai","start":{"line":30,"col":14,"offset":740},"end":{"line":30,"col":19,"offset":745}},"recovery":"authoritative","related":{"operation":"index","type":"u8[]","index":16,"len":16},"exit_code":70}
```

## 13. Conformance obligations

- The negative-diagnostic suite asserts: for each invalid program, the emitted record set contains the required root-cause code at the required primary span (plus required secondary spans), in the deterministic order.
- Cascade/recovery records are asserted separately and must be deterministic.
- No two records in one build may have the same primary span and code unless they are distinct causal failures (a code must be emitted at most once per root cause).
- Diagnostics wording may improve without changing the stable code and structured meaning (ADR-001).

## 14. Review state

Status: **Proposed**. This contract is reviewed with the language specification: Planner self-review, then Reviewer conformance review and Main Designer architectural acceptance. Code additions/changes require a new schema version or documented deprecation; codes never change meaning after acceptance. This document closes review finding FIND-005 (diagnostic schema version 1, stable code-allocation policy and registry, compatibility rules, deterministic ordering, and runtime-trap code contract).

**v0.1.1 (2026-08-08):** made `recovery` mandatory on error/trap records, defined `corrections` as structured objects, and added `AIC-N0207`/`AIC-N0208` for the reserved runtime-module rules of the language specification §6.5. Schema version remains `"1"` (see §11.10 compatibility note). Status remains Proposed.

**v0.1.1 gate-2 correction round (2026-08-08):** per the gate-2 conformance review (`docs/reviews/LANGUAGE-SPEC-v0.1.1-REVIEW-2026-08-08.md`): §9 now defines deterministic ordering for null-span records (FIND-G2-04), §11.10 now states the draft-exemption clause (FIND-G2-09), the registry gained `AIC-N0209` (bare `import rt;`; language spec §6.5) and `AIC-R0816` (pointer arithmetic overflow; language spec §12.5) (FIND-G2-07, FIND-G2-06), and §5 notes the `U` class exemption from the phase-group digit mapping (SUG-G2-02). Schema version remains `"1"` (the additions follow the class allocation policy; no required-field structure changed). Status remains Proposed.
