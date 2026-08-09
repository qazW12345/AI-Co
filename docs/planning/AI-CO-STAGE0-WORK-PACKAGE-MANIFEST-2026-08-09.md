# AI-Co Stage-0 Work-Package Manifest

> **TRIAL — Constitution §8 temporary exception (2026-08-08).** This attribution header is a trial addition per [TRIAL-APPROVAL-authority-attribution-control-2026-08-08.md](../../../governance/runtime/TRIAL-APPROVAL-authority-attribution-control-2026-08-08.md) and proposal v0.3.1 (Controls 1–5). Expires 30 days from trial start or 3 ADRs + 5 reports, whichever comes first. Rollback by the Main Designer through the authorizing approval path.

- **Recorded by:** Planner
- **Decision authority (if applicable):** Main Designer (accepted architecture: ADR-001, ADR-002, ADR-004); Marcel, Human Sponsor (ADR-004 resolutions; Stage-0 direction and working philosophy, 2026-08-08)
- **Human approver (if a gate applies):** Not applicable — this manifest routes implementation; it introduces no new approval gate.

**Status:** Accepted
**Owner:** Planner
**Date:** 2026-08-09
**Scope:** project AI-Co, milestone M0 (Stage-0 bootstrap compiler and verification infrastructure); M1 shown as milestone-level streams with a Planner refinement checkpoint
**Accepted:** 2026-08-10 — Main Designer (decision owner for plan acceptance; decision in session 2026-08-10, routing task t_5bcee55d). Scope: M0 work-package manifest only; M1 remains milestone-level streams; the M1 refinement checkpoint is unchanged. No new architecture decisions; no new Human Sponsor claims.

Companion: `docs/planning/AI-CO-STAGE0-MILESTONE-PLAN-2026-08-09.md` (milestone structure, build conventions, harness design, serial discipline). This manifest is the routing surface: every package below contains the fields required by Operations Manual §7 and the Coordinator profile so cards can be created structurally.

## 1. Routing rules (binding on the Coordinator)

1. Dispatch **strictly serially** in the order below. Do not start WP-M0-N+1 until WP-M0-N is verified (dependency edges on each card enforce this).
2. Assign to the stated profile only (`senior_specialist` / `junior_specialist` per package). No parallel lanes within M0.
3. Every card carries: scope identity = project AI-Co; workspace = `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co` (dir kind); delivery destination = repository path named in the package.
4. Package N+1 must not modify files owned by package N. Reads of previously owned artifacts are permitted; modifications are not. Gaps requiring an earlier package's artifact return to the Planner via the Coordinator (downstream gap rule, milestone plan §7).
5. Corpus packages (WP-M0-02..04) may be authored before the compiler exists; they encode the specification, not the implementation. Their acceptance does not require execution. All three corpus packages and the harness (WP-M0-05) implement **the single case schema defined in §1a** below; no package invents its own format.
6. Commit locally on `main`; do **not** push (push is handled separately per project practice). Commits must carry clear messages naming the package.
7. No package may write to the C: drive, create secrets, modify the specification/ADRs, or publish anything (private repo).

## 1a. Corpus and harness case schema (Planner-defined interface)

One machine-readable schema binds the corpora and the harness. The harness (WP-M0-05) implements this schema; the corpus packages (WP-M0-02..04) author cases to it.

- Each corpus case is a directory under `tests/<corpus>/cases/<case-name>/` containing:
  - `input.ai` — the AI-Co source program;
  - `expected.json` — expected result record:
    - conformance: `{"kind": "run", "stdout": "<exact bytes, base64 or escaped>", "exit_code": N}`;
    - negative: `{"kind": "diagnostics", "records": [<expected JSONL records or partial matchers>]}` — ordering per DIAGNOSTIC-CONTRACT §9; span assertions may be exact or structural (`"primary_span": {"file": ..., "line": L, "col": C}` or `"span_mode": "any"`);
    - smoke: `{"kind": "run", "stdout": ..., "exit_code": N, "stderr_contains": [<substrings or JSONL fragments>], "trap": <true|false>}`.
  - `meta.json` (optional) — `{"spec_ref": "<spec section or §18 example id>", "codes": ["AIC-..."], "deferral_reason": "<string>"}`.
- A corpus-level `manifest.json` lists all case names plus `{"schema_version": 1}`.
- The harness treats missing/extra fields as format errors, not silent passes; harness output is deterministic (no timestamps, repository-relative paths, sorted iteration).
- The exact record shapes above are normative for WP-M0-02..05. If a corpus author finds a shape inadequate for a case class, the gap returns to the Planner (via Coordinator), not to the harness or corpus author's own invention.

## 2. File-ownership matrix (disjoint areas)

| Package | Owned repository area |
|---|---|
| WP-M0-01 | `bootstrap/build/` top-level entry points + toolchain init scripts + `CONVENTIONS.md` + the empty `bootstrap/build/` directory convention; `bootstrap/README.md`; `.gitignore` (tests/artifacts only). Per-area fragment files (`bootstrap/build/<area>.txt`) belong to their area packages, not to WP-M0-01. |
| WP-M0-02 | `tests/conformance/**` |
| WP-M0-03 | `tests/negative/**` |
| WP-M0-04 | `tests/smoke/**` |
| WP-M0-05 | `tests/harness/**`, `tests/README.md` |
| WP-M0-06 | `bootstrap/src/diag/**`, `bootstrap/build/diag.txt` |
| WP-M0-07 | `bootstrap/src/load/**`, `bootstrap/build/load.txt` |
| WP-M0-08 | `bootstrap/src/lex/**`, `bootstrap/build/lex.txt` |
| WP-M0-09 | `bootstrap/src/ast/**`, `bootstrap/src/parse/**`, `bootstrap/build/ast.txt`, `bootstrap/build/parse.txt` |
| WP-M0-10 | `bootstrap/src/name/**`, `bootstrap/build/name.txt` |
| WP-M0-11 | `bootstrap/src/types/**`, `bootstrap/build/types.txt` |
| WP-M0-12 | `bootstrap/src/const/**`, `bootstrap/build/const.txt` |
| WP-M0-13 | `bootstrap/src/sema/**`, `bootstrap/build/sema.txt` |
| WP-M0-14 | `bootstrap/runtime/rt_mem/**`, `bootstrap/build/rt_mem.txt` |
| WP-M0-15 | `bootstrap/runtime/rt_io/**`, `bootstrap/runtime/rt_proc/**`, `bootstrap/runtime/rt_trap/**`, `bootstrap/runtime/README.md`, `bootstrap/build/rt_io.txt`, `bootstrap/build/rt_proc.txt`, `bootstrap/build/rt_trap.txt` |
| WP-M0-16 | `docs/contracts/IR-CONTRACT-*.md`, `bootstrap/src/ir/**`, `bootstrap/build/ir.txt` |
| WP-M0-17 | `bootstrap/src/backend/**`, `bootstrap/build/backend.txt` |
| WP-M0-18 | `bootstrap/src/coff/**`, `bootstrap/build/coff.txt` |
| WP-M0-19 | `bootstrap/src/driver/**`, `bootstrap/build/driver.txt` |
| WP-M0-20 | `docs/verification/STAGE0-INTEGRATION-*.md` (writes evidence; modifies no source) |
| WP-M1-01..04 | `bootstrap/selfhost/**`, `docs/verification/STAGE1-2-IDENTITY-*.md` (refined at M1 planning) |

## 3. Work packages — M0 (Stage-0 bootstrap compiler and verification infrastructure)

---

### WP-M0-01 — Bootstrap scaffolding and build conventions

- **Objective:** establish the `bootstrap/` repository layout, deterministic build entry points for both accepted host compilers, per-area build-fragment convention, and environment-hazard handling so every later package builds and verifies identically.
- **Scope:** `bootstrap/build/` scripts and documentation; `bootstrap/README.md`; `.gitignore` addition for `tests/artifacts/`; a trivial C17 hello-world compile through each entry point as entry verification. No compiler logic.
- **Exclusions:** any `src/` code; any per-area build fragment beyond the base convention (area fragments are owned by their packages); test corpus content; runtime code.
- **Dependencies / inputs:** none (first package). Inputs: environment baseline (MSVC path/vcvarsall, LLVM 22.1.8 off PATH, `link` collision), milestone plan §5.
- **Expected artifacts:** `bootstrap/build/build-stage0-msvc.*`, `bootstrap/build/build-stage0-clang.*`, `bootstrap/build/CONVENTIONS.md`, `bootstrap/README.md`, `.gitignore` update, verified hello-world artifacts under `bootstrap/stage0/`.
- **Capability:** `senior_specialist` (environment-sensitive; establishes conventions every later package depends on).
- **Host toolchain required:** yes (MSVC `cl` and/or LLVM Clang; verifies both entry points).
- **Acceptance criteria:**
  1. Both entry points compile and run a trivial C17 program with artifacts under `bootstrap/stage0/` on the E: workspace.
  2. No bare `link` is invoked; the linker is called by explicit path or inside an initialized developer environment.
  3. Building twice yields deterministic artifacts (reproducible flags; no timestamps affecting output).
  4. `CONVENTIONS.md` documents the per-area fragment convention, deterministic flags, and the never-bare-`link` rule.
  5. `tests/artifacts/` is gitignored; `bootstrap/stage0|1|2/` remain gitignored.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** PATH/`link` collision; C: pressure; LLVM off PATH. **Escalation:** environment deviation → Coordinator; convention ambiguity → Planner.

---

### WP-M0-02 — Conformance suite corpus (spec-derived)

- **Objective:** author `tests/conformance/` — valid AI-Co programs with expected observable behavior (stdout bytes, exit code), derived strictly from the accepted specification (§18 valid examples plus broader spec-derived cases) as the normative acceptance oracle.
- **Scope:** corpus only, including expected-output records, spec-section traceability per file, and a case manifest (format consumed by the harness). Program coverage: lexer-valid forms, declarations, types, operators, conversions, control flow, functions, modules/imports, arrays/slices/str/pointers, const expressions, runtime calls.
- **Exclusions:** negative programs (WP-M0-03); trap/smoke programs (WP-M0-04); compiler implementation; harness code.
- **Dependencies / inputs:** spec §18 + §19 traceability; corpus format defined with WP-M0-05's manifest contract (authors coordinate format via the harness package's documented case schema; if the schema is not yet committed, WP-M0-02 records the expected schema and WP-M0-05 implements it).
- **Expected artifacts:** `tests/conformance/**` (programs + `.expected` records + manifest).
- **Capability:** `junior_specialist` (spec-bounded authoring).
- **Host toolchain required:** no (pure scaffolding).
- **Acceptance criteria:**
  1. Every valid program in spec §18 (marked `// valid`) appears with a spec-section reference.
  2. Expected stdout/exit are derived from spec semantics (deterministic), with no implementation-derived values.
  3. Each case file names its spec section; the manifest is machine-readable per the harness schema.
  4. No invalid program is present in this corpus.
- **Verification / review class:** self-review + Reviewer conformance review against the spec (class: Reviewer).
- **Risks:** corpus reflects implementation rather than spec. **Escalation:** ambiguous spec semantics → Planner; schema conflict with harness → Planner via Coordinator.

---

### WP-M0-03 — Negative-diagnostic suite corpus (spec-derived)

- **Objective:** author `tests/negative/` — invalid AI-Co programs with the exact expected diagnostic records: stable codes, primary/secondary spans, ordering per DIAGNOSTIC-CONTRACT §9, and recovery/cascade marking where asserted.
- **Scope:** all spec §18 `// ERROR` examples plus derived cases for every diagnostic code in the registry (DIAGNOSTIC-CONTRACT §11.1–11.8); span rules per contract §6; ordering rules per §9; null-span build/link cases where applicable.
- **Exclusions:** valid programs; smoke/trap programs; compiler implementation.
- **Dependencies / inputs:** spec §18 annotations; DIAGNOSTIC-CONTRACT code registry and span/ordering rules.
- **Expected artifacts:** `tests/negative/**` (programs + expected JSONL records + manifest).
- **Capability:** `junior_specialist`.
- **Host toolchain required:** no.
- **Acceptance criteria:**
  1. Every `// ERROR AIC-xxxx` annotation in spec §18 has a corresponding negative case with that code and the annotated span semantics.
  2. Every registry code in classes L/S/N/T/E/O/BL (and R where static) has at least one case or an explicit recorded deferral.
  3. Expected records follow the contract's deterministic ordering rule.
- **Verification / review class:** self-review + Reviewer conformance review (class: Reviewer).
- **Risks:** span drift; incomplete code coverage. **Escalation:** code/span ambiguity → Planner; contract conflict → Planner.

---

### WP-M0-04 — Executable smoke suite corpus (spec-derived)

- **Objective:** author `tests/smoke/` — representative programs that exercise runtime behavior deterministically: normal execution, runtime traps (exit code 70, trap record on stderr), `rt.mem` allocator behavior (incl. 0xDD poisoning observability, duplicate/invalid release traps), `rt.io`, `rt.proc.args`/`exit`, `main` entry variants.
- **Scope:** small end-to-end programs; expected stdout/exit/trap-record records; a few host-compiler comparison cases.
- **Exclusions:** conformance corpus; negative corpus; harness.
- **Dependencies / inputs:** spec §15 (runtime), §15.5 trap table, §18.6 example.
- **Expected artifacts:** `tests/smoke/**` (programs + expected records + manifest).
- **Capability:** `junior_specialist`.
- **Host toolchain required:** no (executed later by harness).
- **Acceptance criteria:**
  1. Each trap code in §15.5 with a program-observable trigger has a smoke case (or recorded deferral).
  2. Expected records follow the trap-record contract (§10 of DIAGNOSTIC-CONTRACT).
  3. Programs are deterministic under identical inputs.
- **Verification / review class:** self-review + Reviewer conformance review (class: Reviewer).
- **Risks:** trap-trigger cases hard to write before runtime exists. **Escalation:** runtime ambiguity → Planner; trap contract conflict → Planner.

---

### WP-M0-05 — Verification harness

- **Objective:** implement `tests/harness/` — Python runner for conformance/negative/smoke suites, determinism runner, identity runner (M1), and manifest validator, per milestone plan §6, verifiable independently with fixture compiler output.
- **Scope:** harness modules + fixtures; `tests/README.md` documenting usage; deterministic report generation to `tests/artifacts/`.
- **Exclusions:** corpus content; compiler implementation; CI wiring (none exists).
- **Dependencies / inputs:** the corpus/harness case schema (manifest §1a, normative); corpus content is authored by WP-M0-02..04 but the harness implements §1a directly; Python 3.11.15 (`python`).
- **Expected artifacts:** `tests/harness/**`, `tests/README.md`.
- **Capability:** `junior_specialist` (bounded, fixture-verifiable).
- **Host toolchain required:** no (Python only; C17 not needed for harness verification).
- **Acceptance criteria:**
  1. Conformance/negative/smoke runners execute fixture compiler output (fixtures follow the §1a schema) and produce correct pass/fail per expected records.
  2. Determinism runner byte-compares two artifact sets and reports differences deterministically.
  3. Identity runner compares two object/manifest sets and two PE files byte-for-byte (M1-ready).
  4. Manifest validator checks spec §14.4 fields (no self-hash; stage-invariant version; relative paths; sorted options).
  5. Harness output contains no absolute host paths or timestamps that break comparisons.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** harness nondeterminism; schema mismatch with corpora. **Escalation:** schema conflict → Planner; Python env issue → Coordinator.

---

### WP-M0-06 — Diagnostic infrastructure

- **Objective:** implement `bootstrap/src/diag/` — diagnostic record model, JSONL emitter, span model, deterministic ordering, code registry table, recovery/cascade marking, trap-record emission, per DIAGNOSTIC-CONTRACT.
- **Scope:** record structs; span (file/line/col/offset) computation support; emitter producing the exact JSONL shape (schema_version "1", required fields, optional fields, no embedded newlines); ordering comparator per contract §9; code tables for the full registry; severity/phase/recovery semantics.
- **Exclusions:** source loading (WP-M0-07); any compiler stage logic; human-readable renderer (secondary, may be deferred with a recorded note).
- **Dependencies / inputs:** DIAGNOSTIC-CONTRACT v0.1.1 (schema, registry, ordering); WP-M0-01 build conventions.
- **Expected artifacts:** `bootstrap/src/diag/**` (headers/sources), `bootstrap/build/diag.txt`, unit tests, golden JSONL fixtures.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Emits records byte-matching the contract §12 example records (modulo span content) for equivalent inputs.
  2. Ordering matches contract §9 for mixed null-span/file-bearing records, ties by offset then code.
  3. Registry tables cover every code in contract §11.1–11.8; unknown codes rejected as defects.
  4. Trap records carry `phase="trap"`, `severity="error"`, `recovery="authoritative"`, `exit_code=70` where applicable.
  5. Unit tests pass under both MSVC and Clang builds.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** JSONL encoding drift; ordering edge cases (null spans). **Escalation:** contract contradiction → Planner; code registry change → Planner (registry is Planner-owned).

---

### WP-M0-07 — Source loader and UTF-8 validation

- **Objective:** implement `bootstrap/src/load/` — read source files as bytes, validate UTF-8 per spec §3.1 (reject BOM `AIC-L0002`, NUL `AIC-L0003`, invalid sequences `AIC-L0001`), normalize line terminators for span computation (LF/CRLF; lone CR = whitespace), compute 1-based line/col and 0-based byte offsets.
- **Scope:** file reading; UTF-8 validation state machine; span computation; error records via WP-M0-06.
- **Exclusions:** tokenization (WP-M0-08); module-to-file resolution (WP-M0-10).
- **Dependencies / inputs:** WP-M0-06 diag API; spec §3, §4.1.
- **Expected artifacts:** `bootstrap/src/load/**`, `bootstrap/build/load.txt`, unit tests incl. byte-level UTF-8 vectors.
- **Capability:** `junior_specialist` (bounded, well-specified; byte-level test vectors make verification objective).
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Valid UTF-8 (incl. 4-byte code points) loads; BOM/NUL/invalid/overlong/surrogate sequences rejected with the correct codes.
  2. Spans are exact for CRLF and LF inputs (lone CR per §4.1).
  3. Errors are emitted as diag records with correct primary spans.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** UTF-8 edge cases; CRLF span drift. **Escalation:** spec ambiguity → Planner.

---

### WP-M0-08 — Lexer

- **Objective:** implement `bootstrap/src/lex/` — deterministic token stream per spec §4: identifiers/keywords, integer literals (all bases, `_` separators, suffixes, typing rules, unary-minus minimum-value rule), string literals (escapes, UTF-8 validity, concatenation), punctuation, comments (line/block, no nesting), and lexical rejections `AIC-L0001..L0009`.
- **Scope:** full tokenization; literal value computation and typing; escape handling; token-to-span mapping; comment-to-whitespace rule (`a/**/b` = two tokens).
- **Exclusions:** grammar/parsing (WP-M0-09); source loading (WP-M0-07).
- **Dependencies / inputs:** WP-M0-06 (diag), WP-M0-07 (source/span); spec §4, §18.1.
- **Expected artifacts:** `bootstrap/src/lex/**`, `bootstrap/build/lex.txt`, golden token-stream tests, negative tests per §18.1.
- **Capability:** `senior_specialist` (subtle literal/suffix rules).
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Every token family in spec §4.6 produced correctly; comments handled per §4.1.
  2. Integer literal typing/suffix rules incl. `-128i8` vs `-(128i8)` and `_` placement match §4.3 exactly.
  3. String escapes and UTF-8-after-expansion validity match §4.4 (AIC-L0008/L0009).
  4. All lexical rejections carry the correct code/span; no silent recovery.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** unary-minus special rule; escape edge cases. **Escalation:** grammar ambiguity → Planner.

---

### WP-M0-09 — AST and parser

- **Objective:** implement `bootstrap/src/ast/` + `bootstrap/src/parse/` — single-meaning parser for the spec §5.2 EBNF producing a typed syntax representation; deterministic recovery per contract §7 (recovery_derived marking); grammar-level rejections `AIC-S0101..S0104`; disambiguation rules (sizeof type-vs-expr via §6.2 single name space; struct-init vs block per §12.7; type postfix order).
- **Scope:** AST node definitions; recursive-descent parser; deterministic recovery strategy; span preservation on every node; `pub` handling; complete coverage of the grammar productions.
- **Exclusions:** name resolution (WP-M0-10); type checking (WP-M0-11).
- **Dependencies / inputs:** WP-M0-06, WP-M0-07, WP-M0-08; spec §5, §12.7, §18.2.
- **Expected artifacts:** `bootstrap/src/ast/**`, `bootstrap/src/parse/**`, `bootstrap/build/ast.txt`, `bootstrap/build/parse.txt`, golden AST dump tests, recovery tests.
- **Capability:** `senior_specialist` (grammar is the largest single component).
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Every valid program in §18.2 (and derived grammar cases) parses to exactly one AST; AST dumps are deterministic.
  2. Ambiguity-prone forms (`sizeof`, struct-init postfix, `a/**/b`, ternary right-assoc, postfix type order) resolve exactly per the normative resolution rules.
  3. Malformed inputs produce `AIC-S01xx` with correct spans and deterministic recovery; recovery records are marked `recovery_derived`.
  4. Parser never silently accepts an invalid program.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** grammar corner cases; recovery determinism. **Escalation:** grammar conflict → Planner; missing grammar rule → Planner (spec is the authority).

---

### WP-M0-10 — Name resolution, modules, and imports

- **Objective:** implement `bootstrap/src/name/` — scopes (module/function/block), shadowing, single-name-space rule, visibility (`pub`/private), module declarations, import resolution to canonical paths from the build-manifest project root, cycle detection, reserved `rt` module rules, per spec §6.
- **Scope:** name tables; scope stack; module graph and cycle detection; canonical module-to-file mapping (`a.b.c` → `<root>/a/b/c.ai`); `rt.*` reserved handling; rejections `AIC-N0201..N0209`.
- **Exclusions:** type resolution/checking (WP-M0-11); semantic validation (WP-M0-13); build-manifest/project-root parsing (WP-M0-19 provides the root; WP-M0-10 defines the resolution API contract it consumes).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09 AST; spec §6, §18.3.
- **Expected artifacts:** `bootstrap/src/name/**`, `bootstrap/build/name.txt`, unit/integration tests incl. multi-module fixtures.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Scope/shadowing/visibility and duplicate/undeclared rules match §6.1–6.3 with correct codes/spans.
  2. Module-to-file mapping is canonical and cwd-independent; cycle detection names the closing import; `AIC-N0204/N0205/N0206` behavior per §6.5.
  3. `rt.*` reserved rules (`AIC-N0207/N0208/N0209`) and explicit-import requirement match §6.5 exactly.
  4. Same fully qualified name always resolves to the same declaration.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** import-cycle corner cases; module-graph determinism. **Escalation:** module semantics ambiguity → Planner; architecture question → Main Designer.

---

### WP-M0-11 — Type system, layout, and conversions

- **Objective:** implement `bootstrap/src/types/` — type identity, primitive/composite type tables (spec §7.1–7.2), struct layout (§7.4), enum rules (§7.5), completeness (§7.6), implicit-conversion table (§11.1), explicit `cast`/`wrap` matrix (§11.2), operator typing (§10.2), type-level rejections `AIC-T0301..T0313`.
- **Scope:** type representation; layout computation (sizes/alignments per §7); conversion checking; operator operand/result typing; common-type promotion; rejection records.
- **Exclusions:** const-evaluation of sizes (WP-M0-12 provides the evaluator API; WP-M0-11 defines what it needs); semantic validation of statements (WP-M0-13).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09 AST, WP-M0-10 names; spec §7, §10.2, §11.1–11.2.
- **Expected artifacts:** `bootstrap/src/types/**`, `bootstrap/build/types.txt`, unit tests incl. layout vectors and conversion-matrix cases.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Sizes/alignments match §7 tables exactly (incl. `str` 16/8, slice 16/8, pointer 8/8).
  2. Implicit conversions exactly the §11.1 whitelist; narrowing/sign-change/pointer-integer/implicit decay rejected (`AIC-T0307` etc.).
  3. `cast`/`wrap` matrix matches §11.2; invalid pairs rejected `AIC-T0308`; `void` misuse `AIC-T0306`.
  4. Struct layout (declaration order, alignment, padding-zero rule) matches §7.4; enum continuation/aliasing per §7.5.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** conversion-matrix subtlety; padding determinism. **Escalation:** layout/conversion ambiguity → Planner.

---

### WP-M0-12 — Constant-expression evaluator

- **Objective:** implement `bootstrap/src/const/` — compile-time evaluation of constant expressions per spec §10.5: composition rules, compile-time checked arithmetic, `sizeof`/`alignof`, static addresses (`&` of static storage, slice of static array), enum members, `cast`/`wrap` in constant contexts, and constant rejections `AIC-E0401`, `AIC-E0405..E0411`.
- **Scope:** evaluator over AST constant expressions; typed constant values; use by WP-M0-11 (array extents, enum values) and WP-M0-13 via a documented API.
- **Exclusions:** general expression semantics (WP-M0-13); runtime traps.
- **Dependencies / inputs:** WP-M0-06, WP-M0-09, WP-M0-11; spec §10.5, §11.3.
- **Expected artifacts:** `bootstrap/src/const/**`, `bootstrap/build/const.txt`, unit tests incl. constant-overflow/div-zero/shift/cast cases.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Const-expression composition per §10.5 exactly; non-const uses rejected `AIC-E0401`.
  2. Constant arithmetic/division/shift/cast failures emit `AIC-E0405..E0411` per spec (never traps).
  3. `sizeof`/`alignof` and static-address forms evaluate per §10.5; results are deterministic typed values.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** const-context edge cases. **Escalation:** const semantics ambiguity → Planner.

---

### WP-M0-13 — Semantic validation

- **Objective:** implement `bootstrap/src/sema/` — declarations/initialization (§8), expressions/operators/evaluation order (§10.1–10.4, §11.3–11.6), statements/control flow (§13: braces, switch no-fall-through, reachability, functions, return rules), entry `main` validation (`AIC-E0418`), and semantic rejections `AIC-E0401..E0419` plus remaining `AIC-T03xx` checks.
- **Scope:** full semantic pass over the resolved/typed AST; evaluation-order model; checked-arithmetic compile-time decisions; reachability analysis; switch/loop/break/continue rules; function return/reachability; reserved-name enforcement (§4.5).
- **Exclusions:** IR lowering (WP-M0-16); runtime; name binding (WP-M0-10) and type rules (WP-M0-11) already done.
- **Dependencies / inputs:** WP-M0-06, WP-M0-09, WP-M0-10, WP-M0-11, WP-M0-12; spec §8, §10–§13, §18.4–18.5.
- **Expected artifacts:** `bootstrap/src/sema/**`, `bootstrap/build/sema.txt`, unit/integration tests per §18.4–18.5 and §19 traceability rows.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Every §18.4/18.5 negative annotation is rejected with the exact code and span semantics.
  2. Evaluation-order rules (§10.4) are modeled and testable.
  3. Reachability per §13.5 (`AIC-E0416/E0417`), switch terminator rule (`AIC-E0412`), duplicate case (`AIC-E0413`), break/continue placement (`AIC-E0414`) match spec.
  4. No semantic rule silently passes an invalid program.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** reachability conservatism; interaction with const-eval. **Escalation:** semantic ambiguity → Planner; capability gap for self-hosting → Main Designer (spec §17.2).

---

### WP-M0-14 — Runtime allocator (rt.mem)

- **Objective:** implement `bootstrap/runtime/rt_mem/` — deterministic project-owned allocator per spec §15.1 and ADR-004: zero-initialized allocation, `null` on exhaustion, zero-size → `null` no-op, exact-fit reuse with 0xDD overwrite before reuse, reverse-order-of-release within a size class, no split/coalesce, controlled address region, duplicate/invalid-release traps `AIC-R0812/R0813`, alignment ≥16.
- **Scope:** allocator registry and reuse policy; `alloc_bytes`/`dealloc_bytes`/`copy`/`fill`; trap reporting integration via rt.trap API contract (WP-M0-15 provides the implementation; WP-M0-14 consumes the documented interface or coordinates the interface with WP-M0-15 through the Planner).
- **Exclusions:** file I/O, process, trap-report implementation (WP-M0-15); language semantics.
- **Dependencies / inputs:** spec §15.1, §15.5, §15.8; ADR-004 temporal baseline; WP-M0-06 diag record shape for trap records (via rt.trap contract).
- **Expected artifacts:** `bootstrap/runtime/rt_mem/**`, `bootstrap/build/rt_mem.txt`, allocator unit tests (reuse order, poisoning, traps, zero-size, exhaustion).
- **Capability:** `senior_specialist` (deterministic reuse semantics are subtle and normative).
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Reuse order is exactly reverse-of-release within a size class; identical allocation/release sequences yield identical addresses (observable contract).
  2. Deallocated memory is overwritten with 0xDD before reuse; no host-allocator stale-access semantics leak.
  3. Duplicate release → `AIC-R0812`; invalid release → `AIC-R0813`; `alloc_bytes(0)` → `null` without state change; exhaustion → `null`, never a trap.
  4. Traps are reported as records matching the trap contract (exit code 70).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** host-allocator nondeterminism leaking; reuse-rule subtlety. **Escalation:** allocator ambiguity → Planner; Windows behavior question → Researcher; architecture → Main Designer.

---

### WP-M0-15 — Runtime I/O, process, and trap (rt.io / rt.proc / rt.trap)

- **Objective:** implement `bootstrap/runtime/rt_io|rt_proc|rt_trap/` — file handles/open/read/write/close/stdio per spec §15.2; process args (UTF-16→UTF-8, U+FFFD replacement) and exit per §15.3; trap reporting (JSONL record to stderr, exit 70) per §15.4/§10 of the contract; document runtime-facing Windows calls against the pinned baseline per ADR-004.
- **Scope:** the three modules; `rt` module surface; Windows API enumeration doc (`bootstrap/runtime/README.md`); integration with rt.mem for buffer allocation where needed.
- **Exclusions:** allocator internals (WP-M0-14); compiler-emitted runtime calls beyond the §15.8 table.
- **Dependencies / inputs:** WP-M0-14 allocator API; spec §15.2–15.5, §15.7 (calling convention), ADR-004 Windows baseline.
- **Expected artifacts:** runtime module sources, `bootstrap/runtime/README.md` (Windows API list), build fragments, tests (I/O behavior, args conversion, trap record/exit).
- **Capability:** `senior_specialist` (Windows API surface; determinism obligations).
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. File open/read/write/close and stdio behavior match §15.2 (handles, `0` on failure, `AIC-R0814` on invalid handles).
  2. `rt.proc.args()` converts deterministically with U+FFFD replacement for invalid surrogates; `args()[0]` is the program path.
  3. `rt.trap.report` emits a JSONL record (`AIC-U0000`, caller `trap_code`) and exits 70; `rt.proc.exit` exits with the given code, no record.
  4. `README.md` enumerates every runtime-facing Windows call against Win10 22H2 x64 and notes the no-OS-updates baseline.
- **Verification / review class:** self-review + Reviewer independent review; Windows API doc reviewed by Main Designer for baseline conformance (class: Reviewer + Main Designer for the API doc).
- **Risks:** Windows API enumeration drift; args encoding edge cases. **Escalation:** API/baseline question → Main Designer; environment → Coordinator.

---

### WP-M0-16 — Canonical IR contract and implementation

- **Objective:** produce `docs/contracts/IR-CONTRACT-*.md` defining the canonical target-neutral IR (instruction set, node kinds, determinism, span/causal-chain preservation) satisfying the spec §14.1(6) boundary — every semantic rule representable and enforceable; then implement `bootstrap/src/ir/` per that contract.
- **Scope:** IR contract document (drafted by the Specialist, **reviewed and accepted by the Main Designer** before implementation proceeds); IR builder mapping typed AST → IR; IR invariants and deterministic printing/verification support.
- **Exclusions:** x86-64 codegen (WP-M0-17); optimizations (deferred per ADR-001); IR changes to the public language contract (none; IR is internal).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09, WP-M0-11, WP-M0-13; spec §14.1(6) boundary; ADR-001 pipeline stage 6.
- **Expected artifacts:** `docs/contracts/IR-CONTRACT-*.md` (accepted by Main Designer), `bootstrap/src/ir/**`, `bootstrap/build/ir.txt`, IR unit tests.
- **Capability:** `senior_specialist` (with Main Designer contract review gate).
- **Host toolchain required:** yes (implementation; contract drafting itself is spec work).
- **Acceptance criteria:**
  1. Contract states IR determinism, target-neutrality, span/cause preservation, and representation coverage for every semantic rule; Main Designer accepts it.
  2. Implementation matches the accepted contract; identical AST → identical IR.
  3. IR preserves source spans and causal chains for diagnostics/traps.
- **Verification / review class:** self-review + Reviewer independent review; Main Designer architecture review of the contract (class: Reviewer + Main Designer gate).
- **Risks:** IR design drift into architecture-by-specialist; contract/implementation mismatch. **Escalation:** IR boundary conflict → Main Designer; spec-representability gap → Main Designer (spec §17.2).

---

### WP-M0-17 — x86-64 backend

- **Objective:** implement `bootstrap/src/backend/` — deterministic code generation per spec §14.1(7)/§14.3: instruction baseline ≤ x86-64 + SSE2 (no AVX2 required), Microsoft x64 calling convention at the compiler-to-runtime boundary (§15.7), stack layout, checked-operation branches to trap reports with stable codes and source spans, `main` entry setup, noreturn handling.
- **Scope:** IR → x86-64 instruction selection; register allocation (simple/deterministic); function prologue/epilogue; call emission per §15.7; trap branches for checked arithmetic/bounds/null/pointer-arithmetic-overflow; deterministic output ordering.
- **Exclusions:** COFF emission (WP-M0-18); optimizations; linking (WP-M0-19).
- **Dependencies / inputs:** WP-M0-16 IR contract/impl; spec §14.1(7), §14.3, §15.5, §15.7.
- **Expected artifacts:** `bootstrap/src/backend/**`, `bootstrap/build/backend.txt`, codegen unit tests, assembly dump tests.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Generated instruction set uses only x86-64 + SSE2; no AVX2/host-specific instructions required.
  2. Runtime calls follow the §15.7 convention (RCX/RDX/R8/R9, shadow space, 16-byte alignment, RAX return).
  3. Every runtime-failable checked operation emits a deterministic trap branch with the correct `AIC-Rxxxx` code and source span.
  4. Output is deterministic: identical IR → identical assembly bytes.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** calling-convention errors; trap-branch determinism. **Escalation:** backend constraint → Planner; ABI question → Main Designer.

---

### WP-M0-18 — COFF object emission

- **Objective:** implement `bootstrap/src/coff/` — deterministic COFF object emission per spec §14.2/§14.3: zero compiler-controlled timestamps, canonical record/section order, repository-relative canonically separated paths, no random/host identifiers; objects link with accepted external linkers (`link.exe` / `lld-link`).
- **Scope:** section/symbol/relocation tables; emission of the backend's sections; byte-determinism machinery; inspection support (dumpbin/llvm-objdump readable).
- **Exclusions:** PE/linking (WP-M0-19); codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-17 backend output contract; spec §14.2, §14.3; baseline tooling (dumpbin on PATH, llvm-objdump off PATH).
- **Expected artifacts:** `bootstrap/src/coff/**`, `bootstrap/build/coff.txt`, byte-level determinism tests, object inspection tests.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes (MSVC and/or Clang/LLVM for verification; objects verified with dumpbin/llvm-objdump).
- **Acceptance criteria:**
  1. Identical inputs → byte-identical COFF objects; no normalization.
  2. Objects contain zero timestamps, canonical order, relative paths; no host/build-machine identity.
  3. Objects link with both `link.exe` (via initialized environment) and `lld-link`; inspection tools read them cleanly.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** COFF field nondeterminism; tool-order canonicalization. **Escalation:** COFF detail → Planner; linker incompatibility → Main Designer/Researcher.

---

### WP-M0-19 — Build driver, manifest, and link integration

- **Objective:** implement `bootstrap/src/driver/` — pipeline orchestration (load→lex→parse→name→types→const→sema→ir→backend→coff→link), CLI/options, build manifest per spec §14.4 (schema version, project root, entry module, module list, AI-Co language/spec version field, sorted options, linker flag set, relative artifact paths, SHA-256 of each artifact **excluding the manifest itself**, diagnostic summary, exit status), external linker invocation per §16.3 (never bare `link`; explicit path/initialized env), entry validation (`AIC-E0418`), diagnostics to stderr as JSONL.
- **Scope:** `main()`; pipeline driver; manifest emission with the hashed-artifact set and self-hash exclusion (spec §14.4 FIND-G2-02/03); deterministic option parsing; exit codes.
- **Exclusions:** language semantics; linker implementation (external for M0/M1); M2 project-owned linker (out of scope).
- **Dependencies / inputs:** WP-M0-06..18; spec §14.1, §14.4, §16.3; baseline linker paths.
- **Expected artifacts:** `bootstrap/src/driver/**`, `bootstrap/build/driver.txt`, end-to-end compile tests on corpus programs, manifest fixtures.
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes (MSVC and Clang; both linkers).
- **Acceptance criteria:**
  1. End-to-end compile of valid programs produces COFF + linked PE via the accepted linker modes; invalid programs produce JSONL diagnostics and non-zero exit.
  2. Manifest fields match §14.4 exactly: no self-hash; identical relative output paths across stage builds; stage-invariant version field; sorted build options; recorded linker flag set.
  3. Two identical builds → byte-identical objects + manifests.
  4. Never invokes bare `link`; linker identity/version recorded only in comparison evidence (spec §16.3), never in the manifest.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** manifest drift; linker-mode mismatch; exit-code ambiguity. **Escalation:** manifest/contract conflict → Planner; linker nondeterminism → Main Designer (linker unsuitability per §16.3).

---

### WP-M0-20 — Stage-0 integration verification

- **Objective:** execute the full M0 exit gate — build Stage 0 with both accepted host compilers, run conformance/negative/smoke suites via the harness, run determinism checks, record evidence in `docs/verification/STAGE0-INTEGRATION-*.md`, and produce the M0 completion claim for Reviewer/Main Designer.
- **Scope:** integration runs; evidence report; gap/deferral log; handoff to M1 planning.
- **Exclusions:** new source code (finds defects → returns to package owner via Coordinator); M1 execution.
- **Dependencies / inputs:** WP-M0-01..19; harness and corpora; both host compilers.
- **Expected artifacts:** `docs/verification/STAGE0-INTEGRATION-*.md` (suite results, determinism evidence, host-compiler comparison, deferred items).
- **Capability:** `senior_specialist`.
- **Host toolchain required:** yes (full M0 gate).
- **Acceptance criteria:**
  1. M0 exit gate (milestone plan §3) evidenced: suite pass under both compilers, determinism, manifest conformance, runtime checks.
  2. Any failing case is triaged to the owning package with evidence; no silent skip.
  3. Report is committed (not pushed) with clear message; Reviewer verifies; Main Designer accepts M0 gate.
- **Verification / review class:** Reviewer independent verification of the evidence; Main Designer accepts the milestone gate (class: Reviewer + Main Designer gate).
- **Risks:** integration reveals cross-package defects (routed back serially); evidence incomplete. **Escalation:** package defect → Coordinator routes back to owning package (serial); spec/architecture gap → Planner/Main Designer.

## 4. Work streams — M1 (first self-hosting proof; refinement checkpoint)

These are **milestone-level streams, not dispatch-ready packages**. Per the milestone plan, the Planner refreshes M1 into bite-sized packages with exact file ownership **after M0 verification**, when the C17 architecture, IR contract, and corpus are proven. The Coordinator must not dispatch M1 cards from this manifest alone.

| Stream | Scope (to be refined) | Dependency | Capability (planned) |
|---|---|---|---|
| WP-M1-01 | AI-Co self-hosted compiler source — front end + semantic layers (`bootstrap/selfhost/`) mirroring the verified C17 architecture, in the minimal AI-Co language | M0 verified; IR contract accepted | `senior_specialist` |
| WP-M1-02 | AI-Co self-hosted compiler source — IR, backend, COFF, driver, build conventions for self-host | WP-M1-01 | `senior_specialist` |
| WP-M1-03 | Stage 0 compiles the AI-Co compiler → Stage 1; Stage 1 recompiles same source → Stage 2 | WP-M1-01, WP-M1-02 | `senior_specialist` |
| WP-M1-04 | Byte-identity gates: Stage 1 vs Stage 2 COFF objects + build manifests byte-identical (no normalization); PE byte-identity under accepted deterministic linker modes; full suite pass on both; comparison evidence per spec §16.6; record `docs/verification/STAGE1-2-IDENTITY-*.md` | WP-M1-03 | `senior_specialist` (evidence), Reviewer verification, Main Designer M1 gate |

M1 acceptance is normative in spec §16.5/§16.6 and is not restated as a new decision here.

## 5. Coordinator checklist

For each WP-M0 card, verify before dispatch: assignee profile Active and capability matches; workspace `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`; parent dependency edges set (WP-M0-N depends on WP-M0-(N-1); WP-M0-05 also depends on WP-M0-02..04 schemas; WP-M0-14/WP-M0-15 coordinate the rt.trap interface — represent as a documented interface note, not a shared file); delivery destination = owned area in §2; acceptance criteria present; review class recorded; no human gate outstanding. If any package requires a Planner refinement (e.g., harness schema conflict), return to the Planner rather than inventing content.
