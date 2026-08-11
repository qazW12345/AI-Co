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

**Amendment (2026-08-10):** explicit completion protocol added — §1 routing rule 8 (binding on every implementing worker) and a §5 Coordinator checklist item (card bodies must carry the rule). Source: Main Designer direction, Marcel-approved (2026-08-10), following the WP-M0-03/04 protocol-slip corrective round (OM §6.1 watchdog audit). Decision authority for this planning-artifact amendment: Main Designer (OM §8); no new Human Sponsor claims. Existing M0 cards 05–20 keep their current card bodies and are covered operationally by the OM §6.1 watchdog audit (premature-completion auto-route); no board edits are required. The companion milestone plan §7 carries a one-line reference; the protocol is not duplicated there.

**Amendment (2026-08-10):** task-sizing discipline added — §1 routing rule 9 and §1b sizing rule (primary metric: estimated agent-turn effort; threshold formula tied to lane budgets; splitting rule; calibration loop), a per-package **Sizing** field for every remaining package (WP-M0-11..20), recorded **Actuals** notes for completed packages (WP-M0-01..10), and retroactive re-estimation with in-manifest splits for every remaining package estimated above the threshold.
- Source: Human Sponsor direction (Marcel, 2026-08-10): "from now on, the planner should think about each task and how much work in terms of amount of code (or some other, better metric) each 'bite-sized' task is likely to require, and if a singular task is too large, divide it into separate cards." Sizing metric (estimated agent-turn effort as the binding constraint) recommended by the Main Designer per the Human Sponsor's "better metric" ask.
- Decision authority for this planning-artifact amendment: Main Designer (OM §8). No new Human Sponsor claims.
- Evidence: Kanban task t_a38a2060 (2026-08-10) — Human Sponsor direction and Main Designer metric recommendation recorded in the task body; run/turn actuals from the Kanban run history (runs 89–144) and repository evidence (commit log, LOC counts, test counts).
- Board-state scope: no board edits here. The Coordinator handoff note (§5) states that split cards replace the original oversized cards for unrun packages; existing already-created M0 cards keep their bodies (no board edits for cards already dispatched or in-flight).

**Amendment (2026-08-11):** Minor-1 citation correction in WP-M0-11c — the three common-type-promotion citations previously citing §11.4 (Objective, Dependencies, AC 2) now cite §11.1 bullet 2 (the normative common-type-promotion rule; §11.4 is "Comparison semantics"). Source: reviewer2 watchdog re-verification finding Minor-1 (review card t_879bda8d, 2026-08-11), routed via Coordinator gate t_ddc8f6e3 as correction task t_2dd05335. Comment/prose only; no behavioral, source, or spec change. Decision authority: Main Designer (OM §8); authorized deferral with no re-review required (reviewer2). Card t_2e52b09e body keeps its existing text per §1 rule 9 (no board edits for already-created/dispatched cards); this amendment is the durable correction record.

Companion: `docs/planning/AI-CO-STAGE0-MILESTONE-PLAN-2026-08-09.md` (milestone structure, build conventions, harness design, serial discipline). This manifest is the routing surface: every package below contains the fields required by Operations Manual §7 and the Coordinator profile so cards can be created structurally.

## 1. Routing rules (binding on the Coordinator)

1. Dispatch **strictly serially** in the order below. Do not start WP-M0-N+1 until WP-M0-N is verified (dependency edges on each card enforce this).
2. Assign to the stated profile only (`senior_specialist` / `junior_specialist` per package). No parallel lanes within M0.
3. Every card carries: scope identity = project AI-Co; workspace = `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co` (dir kind); delivery destination = repository path named in the package.
4. Package N+1 must not modify files owned by package N. Reads of previously owned artifacts are permitted; modifications are not. Gaps requiring an earlier package's artifact return to the Planner via the Coordinator (downstream gap rule, milestone plan §7).
5. Corpus packages (WP-M0-02..04) may be authored before the compiler exists; they encode the specification, not the implementation. Their acceptance does not require execution. All three corpus packages and the harness (WP-M0-05) implement **the single case schema defined in §1a** below; no package invents its own format.
6. Commit locally on `main`; do **not** push (push is handled separately per project practice). Commits must carry clear messages naming the package.
7. No package may write to the C: drive, create secrets, modify the specification/ADRs, or publish anything (private repo).
8. **Completion protocol (binding on every implementing worker):** on completing bounded work whose review class requires independent review, post the handoff evidence as a comment, then call `kanban_block(reason='review-required: <one-line summary>')`. Do **NOT** call `kanban_complete` on your own card — completion is applied by the authorized owner only after the review gate passes (OM §6.1, org ADR-004 §54-8).
9. **Task-sizing discipline (binding on the Coordinator):** every card created from this manifest must carry its package's **Sizing** estimate (§1b, per-package field in §3). A package whose estimate exceeds the threshold MUST NOT be dispatched as one card; it MUST be represented as multiple cards per the §3 split (each with its own owned area in §2, its own acceptance criteria, review class, and serial edges). When the amended manifest reaches the Coordinator, split cards replace the original oversized cards for unrun packages — create the new cards per the manifest and supersede the originals (supersession pattern already used for push cards); existing already-created M0 cards keep their bodies (no board edits for cards already dispatched or in-flight).

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

## 1b. Task-sizing discipline (Planner-defined rule, calibrated to actuals)

**Metric.** The binding constraint for a card is the lane's budgeted run: `senior_specialist` = 90 agent turns per run; `junior_specialist` = 60. A package is **bite-sized** iff it fits comfortably within ONE budgeted run: estimate ≤ ~70% of the lane budget (senior ≤ ~63 turns; junior ≤ ~42 turns), leaving room for verification, commits, and handoff.

**Estimation method.** Estimate using, in order of weight: (1) expected LOC (source + tests), (2) expected test-case count, (3) spec-rule complexity — the number of distinct normative rules to implement/verify — all calibrated against recorded actuals (per-package **Actuals** notes in §3 and the anchors below). State the estimate as a turn range plus its basis so the next calibration can compare.

**Splitting rule.** Any package estimated above the threshold MUST be split into multiple cards in this manifest before dispatch. Each split card must have: its own owned area (new §2 matrix entry), its own acceptance criteria, its own review class, serial edges to its siblings and the next package, and its own **Sizing** estimate. A split must keep each card independently reviewable; no split card may depend on files owned by a later sibling.

**Calibration loop.** After each package completes, the Planner (or the executing worker via the completion handoff) records actuals: runs consumed, agent turns consumed, LOC delivered, test cases authored. The manifest gains an **Actuals** note per package so future estimates are calibrated from evidence, not guesses.

**Calibration anchors (recorded 2026-08-10).**
- WP-M0-02..04 (junior corpus; 55/68/25 cases): each completed in 1–2 runs, well within the 60-turn junior budget. Corpus authoring is not the risk class.
- WP-M0-05 (junior harness): 2 runs (1st hit the 60/60 junior budget and timed out; 2nd completed) — borderline for the junior lane.
- WP-M0-06 (diag): 2 runs, ~2,500 LOC total (~1,670 source + ~850 test), 20 test functions.
- WP-M0-07 (loader): ~2 runs, ~1,520 LOC total (~694 source + ~829 test), 16 test functions — fits but consumes most of a senior run.
- WP-M0-08 (lexer): ~250–300 agent turns across 5 runs (1 crashed, 1 hit 90/90 budget, 2 watchdog continuations), ~2,370 LOC total (~1,333 source + ~1,034 test), 23 test functions — the canonical oversized example.
- WP-M0-09 (parser): 4 runs + 2 continuations (2 crashes, 1 hit 90/90), ~4,850 LOC total (AST ~1,350 + parser ~3,500), 16 test functions + golden cases — oversized.
- WP-M0-10 (name resolution): in-flight at recording time; 2 runs crashed after 30–90 min each, resumed after flag review; ~1,440 LOC source, tests pending — oversized.

**Lesson for remaining packages.** Compiler-core packages are the risk class: conceptually small, but large raw effort (many tests, subtle spec rules). Remaining packages WP-M0-11..20 are re-estimated against these anchors below; every package estimated above ~63 senior turns is split.

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
| WP-M0-11a | `bootstrap/src/types/type_identity.*`, `bootstrap/src/types/type_tables.*` (identity, primitive/composite tables, completeness), `bootstrap/build/types_a.txt` |
| WP-M0-11b | `bootstrap/src/types/layout.*` (struct/enum layout, padding), `bootstrap/build/types_b.txt` |
| WP-M0-11c | `bootstrap/src/types/convert.*` (implicit conversions, common-type promotion), `bootstrap/build/types_c.txt` |
| WP-M0-11d | `bootstrap/src/types/optype.*` (explicit cast/wrap matrix, operator typing), `bootstrap/build/types_d.txt` |
| WP-M0-12a | `bootstrap/src/const/eval_core.*`, `bootstrap/build/const_a.txt` |
| WP-M0-12b | `bootstrap/src/const/eval_fail.*`, `bootstrap/build/const_b.txt` |
| WP-M0-13a | `bootstrap/src/sema/decl.*`, `bootstrap/build/sema_a.txt` |
| WP-M0-13b | `bootstrap/src/sema/expr.*`, `bootstrap/build/sema_b.txt` |
| WP-M0-13c | `bootstrap/src/sema/stmt.*`, `bootstrap/build/sema_c.txt` |
| WP-M0-13d | `bootstrap/src/sema/fn.*`, `bootstrap/build/sema_d.txt` |
| WP-M0-14a | `bootstrap/runtime/rt_mem/rt_mem_core.*`, `bootstrap/build/rt_mem_a.txt` |
| WP-M0-14b | `bootstrap/runtime/rt_mem/rt_mem_reuse.*`, `bootstrap/build/rt_mem_b.txt` |
| WP-M0-15a | `bootstrap/runtime/rt_io/**`, `bootstrap/build/rt_io.txt` |
| WP-M0-15b | `bootstrap/runtime/rt_proc/**`, `bootstrap/build/rt_proc.txt` |
| WP-M0-15c | `bootstrap/runtime/rt_trap/**`, `bootstrap/runtime/README.md`, `bootstrap/build/rt_trap.txt` |
| WP-M0-16a | `docs/contracts/IR-CONTRACT-*.md` (draft + acceptance record) |
| WP-M0-16b | `bootstrap/src/ir/ir_core.*`, `bootstrap/src/ir/ir_dump.*`, `bootstrap/build/ir.txt` |
| WP-M0-16c | `bootstrap/src/ir/ir_builder.*`, `bootstrap/build/ir_builder.txt` |
| WP-M0-17a | `bootstrap/src/backend/isel.*`, `bootstrap/build/backend_a.txt` |
| WP-M0-17b | `bootstrap/src/backend/frame.*`, `bootstrap/build/backend_b.txt` |
| WP-M0-17c | `bootstrap/src/backend/trap.*`, `bootstrap/build/backend_c.txt` |
| WP-M0-18a | `bootstrap/src/coff/coff_emit.*`, `bootstrap/build/coff_a.txt` |
| WP-M0-18b | `bootstrap/src/coff/determinism.*`, `bootstrap/build/coff_b.txt` |
| WP-M0-19a | `bootstrap/src/driver/main.*`, `bootstrap/src/driver/cli.*`, `bootstrap/build/driver_a.txt` |
| WP-M0-19b | `bootstrap/src/driver/manifest.*`, `bootstrap/build/driver_b.txt` |
| WP-M0-19c | `bootstrap/src/driver/link.*`, `bootstrap/build/driver_c.txt` |
| WP-M0-20 | `docs/verification/STAGE0-INTEGRATION-*.md` (writes evidence; modifies no source) |
| WP-M1-01..04 | `bootstrap/selfhost/**`, `docs/verification/STAGE1-2-IDENTITY-*.md` (refined at M1 planning) |

Split packages own sub-area fragments (`<area>_<suffix>.txt`, e.g. `types_a.txt`) per the WP-M0-01 per-area fragment convention; the original single-fragment entry (e.g. `types.txt`) is superseded by the split entries above.

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
- **Actuals (2026-08-10):** 2 runs (1 flag/review block, 1 final); small scope — fits one senior run comfortably.

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
- **Actuals (2026-08-10):** 2 runs; 55 cases; fits junior lane (60-turn budget) well.

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
- **Actuals (2026-08-10):** 1 run; 68 cases; fits junior lane (60-turn budget) well.

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
- **Actuals (2026-08-10):** 1 run; 25 cases; fits junior lane (60-turn budget) well.

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
- **Actuals (2026-08-10):** 2 runs (1st hit the 60/60 junior budget and timed out; 2nd completed) — borderline for the junior lane.

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
- **Actuals (2026-08-10):** 2 runs; ~2,500 LOC total (~1,670 source + ~850 test); 20 test functions — fits a senior run.

---

### WP-M0-07 — Source loader and UTF-8 validation

- **Objective:** implement `bootstrap/src/load/` — read source files as bytes, validate UTF-8 per spec §3.1 (reject BOM `AIC-L0002`, NUL `AIC-L0003`, invalid sequences `AIC-L0001`), normalize line terminators for span computation (LF/CRLF; lone CR = whitespace), compute 1-based line/col and 0-based byte offsets.
- **Scope:** file reading; UTF-8 validation state machine; span computation; error records via WP-M0-06.
- **Exclusions:** tokenization (WP-M0-08); module-to-file resolution (WP-M0-10).
- **Dependencies / inputs:** WP-M0-06 diag API; spec §3, §4.1.
- **Expected artifacts:** `bootstrap/src/load/**`, `bootstrap/build/load.txt`, unit tests incl. byte-level UTF-8 vectors.
- **Capability:** `senior_specialist` (C17 host-toolchain implementation package per milestone plan §8; byte-level UTF-8 validation and span computation are pipeline-critical).
- **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Valid UTF-8 (incl. 4-byte code points) loads; BOM/NUL/invalid/overlong/surrogate sequences rejected with the correct codes.
  2. Spans are exact for CRLF and LF inputs (lone CR per §4.1).
  3. Errors are emitted as diag records with correct primary spans.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** UTF-8 edge cases; CRLF span drift. **Escalation:** spec ambiguity → Planner.
- **Actuals (2026-08-10):** ~2 runs; ~1,520 LOC total (~694 source + ~829 test); 16 test functions — fits but consumes most of a senior run.

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
- **Actuals (2026-08-10):** ~250–300 agent turns across 5 runs (1 crashed, 1 hit the 90/90 budget, 2 watchdog continuations); ~2,370 LOC total (~1,333 source + ~1,034 test); 23 test functions — the canonical oversized example.

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
- **Actuals (2026-08-10):** 4 runs + 2 continuations (2 crashes, 1 hit the 90/90 budget); ~4,850 LOC total (AST ~1,350 + parser ~3,500); 16 test functions + golden cases — oversized.

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
- **Actuals (2026-08-10, in-flight at recording time):** 2 runs crashed after 30–90 min each, resumed after flag review; ~1,440 LOC source, tests pending — oversized.

---

### WP-M0-11 — Type system, layout, and conversions (split: WP-M0-11a..11d)

- **Sizing estimate (whole):** ~250–330 senior turns. Above threshold → split below per §1b. Basis: 13 type codes (`AIC-T0301..T0313`), ~7.1–7.6 + §10.2 + §11.1–11.2 rule surface, comparable to lexer/parser actuals.

#### WP-M0-11a — Type identity and type tables

- **Objective:** implement type representation, identity, primitive/composite type tables (spec §7.1–7.3), and completeness rules (§7.6).
- **Scope:** type identity; primitive/composite tables; completeness; type-level rejections for incomplete/recursive structs (`AIC-T0302/T0303`).
- **Exclusions:** layout computation (11b); conversions (11c); operator typing (11d).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09 AST, WP-M0-10 names; spec §7.1–7.3, §7.6.
- **Expected artifacts:** `bootstrap/src/types/type_identity.*`, `type_tables.*`, `bootstrap/build/types_a.txt`, unit tests (identity, tables, completeness).
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Type identity per §7.3 (structural, not name-based); tables match §7.1–7.2.
  2. Completeness rules §7.6 enforced; incomplete/recursive struct usage rejected `AIC-T0302/T0303`.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** identity edge cases. **Escalation:** type ambiguity → Planner.

---

#### WP-M0-11b — Struct/enum layout and padding

- **Objective:** implement struct layout (§7.4), enum rules (§7.5), and deterministic padding (§9.4).
- **Scope:** sizes/alignments per §7 tables (incl. `str` 16/8, slice 16/8, pointer 8/8); padding-zero rule; enum continuation/aliasing; `AIC-T0301`.
- **Exclusions:** type identity (11a); conversions (11c); operator typing (11d).
- **Dependencies / inputs:** WP-M0-11a; spec §7.4–7.5, §9.4.
- **Expected artifacts:** `bootstrap/src/types/layout.*`, `bootstrap/build/types_b.txt`, layout vector tests.
- **Sizing estimate:** 45–55 senior turns. Basis: ~500–700 LOC incl. tests, ~8 test functions, ~5 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Sizes/alignments match §7 tables exactly; struct layout declaration-order/alignment/padding-zero per §7.4/§9.4.
  2. Enum continuation/aliasing per §7.5; unrepresentable member value rejected `AIC-T0301`.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** padding determinism. **Escalation:** layout ambiguity → Planner.

---

#### WP-M0-11c — Implicit conversions and common type

- **Objective:** implement the implicit-conversion whitelist (§11.1) and common-type promotion (§11.1 bullet 2).
- **Scope:** conversion whitelist; narrowing/sign-change/pointer-integer/implicit-decay rejections (`AIC-T0307` etc.); common-type promotion.
- **Exclusions:** explicit cast/wrap matrix (11d); layout (11b).
- **Dependencies / inputs:** WP-M0-11a/b; spec §11.1 (incl. bullet 2: common-type promotion).
- **Expected artifacts:** `bootstrap/src/types/convert.*`, `bootstrap/build/types_c.txt`, conversion-matrix tests.
- **Sizing estimate:** 45–55 senior turns. Basis: ~500–700 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Implicit conversions exactly the §11.1 whitelist; anything else rejected `AIC-T0307` with correct spans.
  2. Common-type promotion per §11.1 bullet 2.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** conversion-matrix subtlety. **Escalation:** conversion ambiguity → Planner.

---

#### WP-M0-11d — Explicit cast/wrap matrix and operator typing

- **Objective:** implement the explicit `cast`/`wrap` matrix (§11.2) and operator typing (§10.2).
- **Scope:** cast/wrap pair validity (`AIC-T0308`); `void` misuse (`AIC-T0306`); operator operand/result typing incl. `==`/`!=` on array/struct (`AIC-T0304`), chained comparison (`AIC-T0305`), condition-not-bool (`AIC-T0310`), switch selector (`AIC-T0311`), call arg count (`AIC-T0312`), struct-literal fields (`AIC-T0313`), array-literal count (`AIC-T0309`).
- **Exclusions:** implicit conversions (11c); layout (11b).
- **Dependencies / inputs:** WP-M0-11a/c; spec §10.2, §11.2.
- **Expected artifacts:** `bootstrap/src/types/optype.*`, `bootstrap/build/types_d.txt`, operator/cast typing tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~12 test functions, ~8 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. `cast`/`wrap` matrix matches §11.2; invalid pairs rejected `AIC-T0308`; `void` misuse `AIC-T0306`.
  2. Operator typing per §10.2 with the listed rejections; no silent acceptance.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** operator edge cases. **Escalation:** operator ambiguity → Planner.

---

### WP-M0-12 — Constant-expression evaluator (split: WP-M0-12a..12b)

- **Sizing estimate (whole):** ~130–180 senior turns. Above threshold → split below per §1b. Basis: 8 const codes (`AIC-E0401`, `AIC-E0405..E0411`), §10.5/§11.3 rule surface.

#### WP-M0-12a — Evaluator core

- **Objective:** implement compile-time evaluation of constant expressions per spec §10.5: composition rules, typed constant values, `sizeof`/`alignof`, static addresses, enum members.
- **Scope:** evaluator over AST constant expressions; typed constant values; API for WP-M0-11 (array extents, enum values) and WP-M0-13.
- **Exclusions:** const failure semantics (12b); runtime traps.
- **Dependencies / inputs:** WP-M0-06, WP-M0-09, WP-M0-11; spec §10.5, §11.3.
- **Expected artifacts:** `bootstrap/src/const/eval_core.*`, `bootstrap/build/const_a.txt`, unit tests (composition, sizeof/alignof, static addresses, enum members).
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~7 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Const-expression composition per §10.5 exactly; non-const uses rejected `AIC-E0401`.
  2. `sizeof`/`alignof` and static-address forms evaluate per §10.5; results deterministic typed values.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** const-context edge cases. **Escalation:** const semantics ambiguity → Planner.

---

#### WP-M0-12b — Constant arithmetic failure semantics

- **Objective:** implement compile-time checked arithmetic and const failure records per spec §10.5/§11.3.
- **Scope:** overflow/div-zero/shift/cast/index/slice-boundary/pointer-difference failures (`AIC-E0405..E0411`); never-trap guarantee for constant failures.
- **Exclusions:** evaluator composition (12a); runtime traps.
- **Dependencies / inputs:** WP-M0-12a; spec §10.5, §11.3.
- **Expected artifacts:** `bootstrap/src/const/eval_fail.*`, `bootstrap/build/const_b.txt`, overflow/div-zero/shift/cast failure tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~500–700 LOC incl. tests, ~10 test functions, ~7 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Constant arithmetic/division/shift/cast failures emit `AIC-E0405..E0411` per spec (never traps).
  2. Failure records carry correct spans and deterministic order.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** failure-path gaps. **Escalation:** const semantics ambiguity → Planner.

---

### WP-M0-13 — Semantic validation (split: WP-M0-13a..13d)

- **Sizing estimate (whole):** ~280–360 senior turns. Above threshold → split below per §1b. Basis: 19 semantic codes (`AIC-E0401..E0419`) + remaining `AIC-T03xx` checks, §8/§10–§13 rule surface, comparable to parser actuals.

#### WP-M0-13a — Declarations and initialization

- **Objective:** implement declaration/initialization semantics per spec §8 and §9: constants, variables, storage, mutability/assignability, initializers.
- **Scope:** §8 declaration rules; §9 initialization; assignment-to-const/non-lvalue checks (`AIC-E0403/E0404/E0419`); missing-initializer rules (`AIC-E0403`).
- **Exclusions:** expressions/operators (13b); statements/control flow (13c); functions/reachability (13d).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09, WP-M0-10, WP-M0-11, WP-M0-12; spec §8, §9.
- **Expected artifacts:** `bootstrap/src/sema/decl.*`, `bootstrap/build/sema_a.txt`, unit/integration tests per §18.4.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~12 test functions, ~8 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Declaration/initialization rules match §8/§9 exactly; rejections carry correct codes/spans.
  2. Const/assignability rules (`AIC-E0404`, `AIC-E0419`) enforced.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** init edge cases. **Escalation:** semantic ambiguity → Planner.

---

#### WP-M0-13b — Expressions, operators, and evaluation order

- **Objective:** implement expression/operator semantics per spec §10.1–10.4 and §11.3–11.6: precedence, evaluation order, checked-arithmetic decisions, comparison semantics.
- **Scope:** expression validation; evaluation-order model; checked-arithmetic compile-time decisions; `AIC-E0401` const-context use.
- **Exclusions:** declarations (13a); statements (13c); functions (13d).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09..12; spec §10.1–10.4, §11.3–11.6.
- **Expected artifacts:** `bootstrap/src/sema/expr.*`, `bootstrap/build/sema_b.txt`, evaluation-order and operator tests per §18.4.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~12 test functions, ~8 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Evaluation-order rules (§10.4) modeled and testable.
  2. No expression semantic rule silently passes an invalid program.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** evaluation-order subtlety. **Escalation:** semantic ambiguity → Planner.

---

#### WP-M0-13c — Statements and control flow

- **Objective:** implement statement/control-flow semantics per spec §13: braces-only blocks, switch no-fall-through, loops, break/continue placement, reachability.
- **Scope:** §13 statement rules; switch terminator (`AIC-E0412`), duplicate case (`AIC-E0413`), break/continue placement (`AIC-E0414`); reachability (`AIC-E0416/E0417`).
- **Exclusions:** declarations (13a); expressions (13b); functions (13d).
- **Dependencies / inputs:** WP-M0-13a/b; spec §13, §18.4–18.5.
- **Expected artifacts:** `bootstrap/src/sema/stmt.*`, `bootstrap/build/sema_c.txt`, control-flow/reachability tests per §18.4.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~12 test functions, ~8 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Switch terminator rule (`AIC-E0412`), duplicate case (`AIC-E0413`), break/continue placement (`AIC-E0414`) match spec.
  2. Reachability per §13.5 (`AIC-E0416/E0417`).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** reachability conservatism. **Escalation:** semantic ambiguity → Planner.

---

#### WP-M0-13d — Functions, returns, and reserved names

- **Objective:** implement function-level semantics per spec §8/§13.5: return rules, non-void path without return, entry `main` validation, reserved-name enforcement (§4.5).
- **Scope:** return value mismatch (`AIC-E0415`); missing return on non-void path (`AIC-E0416`); `main` validation (`AIC-E0418`); reserved-name enforcement (`AIC-E0419` where applicable).
- **Exclusions:** statements (13c); IR lowering (WP-M0-16).
- **Dependencies / inputs:** WP-M0-13a..c; spec §4.5, §8, §13.5, §18.5.
- **Expected artifacts:** `bootstrap/src/sema/fn.*`, `bootstrap/build/sema_d.txt`, function/return/main tests per §18.5.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Return mismatch and missing-return rules match §13.5 (`AIC-E0415/E0416`).
  2. Entry `main` validation per §18.5 (`AIC-E0418`); reserved-name enforcement per §4.5.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** return-rule edge cases. **Escalation:** semantic ambiguity → Planner.

---

### WP-M0-14 — Runtime allocator (rt.mem) (split: WP-M0-14a..14b)

- **Sizing estimate (whole):** ~110–150 senior turns. Above threshold → split below per §1b. Basis: allocator determinism + reuse policy + trap integration, §15.1/ADR-004 rule surface.

#### WP-M0-14a — Allocator core

- **Objective:** implement the deterministic project-owned allocator core per spec §15.1 and ADR-004: zero-initialized allocation, `null` on exhaustion, zero-size → `null` no-op, alignment ≥16, controlled address region, `alloc_bytes`/`dealloc_bytes`/`copy`/`fill`.
- **Scope:** allocator registry; allocation semantics; exhaustion/zero-size behavior.
- **Exclusions:** reuse policy/0xDD poisoning/traps (14b); file I/O/process/trap implementation (WP-M0-15).
- **Dependencies / inputs:** spec §15.1, §15.5, §15.8; ADR-004; WP-M0-06 diag record shape via rt.trap contract.
- **Expected artifacts:** `bootstrap/runtime/rt_mem/rt_mem_core.*`, `bootstrap/build/rt_mem_a.txt`, allocator core unit tests (zero-init, exhaustion, zero-size, alignment).
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Zero-initialized allocation; `alloc_bytes(0)` → `null` without state change; exhaustion → `null`, never a trap.
  2. Alignment ≥16; addresses within the controlled region.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** host-allocator nondeterminism leaking. **Escalation:** allocator ambiguity → Planner; Windows behavior → Researcher.

---

#### WP-M0-14b — Reuse policy and release traps

- **Objective:** implement exact-fit reuse with 0xDD overwrite before reuse, reverse-order-of-release within a size class, no split/coalesce, and duplicate/invalid-release traps.
- **Scope:** reuse registry and policy; 0xDD poisoning; duplicate release (`AIC-R0812`); invalid release (`AIC-R0813`); trap-record integration (exit 70).
- **Exclusions:** allocator core (14a); trap implementation (WP-M0-15c).
- **Dependencies / inputs:** WP-M0-14a; spec §15.1, §15.5, §15.8; ADR-004.
- **Expected artifacts:** `bootstrap/runtime/rt_mem/rt_mem_reuse.*`, `bootstrap/build/rt_mem_b.txt`, reuse-order/poisoning/trap tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Reuse order is exactly reverse-of-release within a size class; identical allocation/release sequences yield identical addresses (observable contract).
  2. Deallocated memory overwritten 0xDD before reuse; duplicate release → `AIC-R0812`; invalid release → `AIC-R0813`; traps report exit 70.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** reuse-rule subtlety. **Escalation:** allocator ambiguity → Planner.

---

### WP-M0-15 — Runtime I/O, process, and trap (split: WP-M0-15a..15c)

- **Sizing estimate (whole):** ~160–210 senior turns. Above threshold → split below per §1b. Basis: three modules + Windows API baseline doc, §15.2–15.5/§15.7 rule surface.

#### WP-M0-15a — rt.io

- **Objective:** implement `bootstrap/runtime/rt_io/` per spec §15.2: file handles/open/read/write/close/stdio; invalid-handle failures `AIC-R0814`.
- **Scope:** rt_io module; handle model; `0` on failure; stdio behavior.
- **Exclusions:** process args/exit (15b); trap reporting (15c); allocator internals (WP-M0-14).
- **Dependencies / inputs:** WP-M0-14 allocator API; spec §15.2, §15.5; ADR-004 Windows baseline.
- **Expected artifacts:** `bootstrap/runtime/rt_io/**`, `bootstrap/build/rt_io.txt`, I/O behavior tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~8 test functions, ~5 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. File open/read/write/close and stdio behavior match §15.2 (handles, `0` on failure, `AIC-R0814` on invalid handles).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** Windows API surface drift. **Escalation:** API/baseline question → Main Designer; environment → Coordinator.

---

#### WP-M0-15b — rt.proc

- **Objective:** implement `bootstrap/runtime/rt_proc/` per spec §15.3: process args (UTF-16→UTF-8, U+FFFD replacement) and exit.
- **Scope:** rt_proc module; args conversion determinism; `args()[0]` program path; `rt.proc.exit`.
- **Exclusions:** I/O (15a); trap reporting (15c).
- **Dependencies / inputs:** spec §15.3, §15.5; ADR-004 Windows baseline.
- **Expected artifacts:** `bootstrap/runtime/rt_proc/**`, `bootstrap/build/rt_proc.txt`, args-conversion/exit tests.
- **Sizing estimate:** 45–55 senior turns. Basis: ~500–700 LOC incl. tests, ~8 test functions, ~4 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. `rt.proc.args()` converts deterministically with U+FFFD replacement for invalid surrogates; `args()[0]` is the program path.
  2. `rt.proc.exit` exits with the given code, no record.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** UTF-16 edge cases. **Escalation:** args ambiguity → Planner.

---

#### WP-M0-15c — rt.trap and runtime Windows API baseline doc

- **Objective:** implement `bootstrap/runtime/rt_trap/` per spec §15.4/contract §10: trap reporting (JSONL record to stderr, exit 70), and produce `bootstrap/runtime/README.md` enumerating every runtime-facing Windows call against the pinned baseline (ADR-004).
- **Scope:** rt_trap module; trap record shape (`AIC-U0000`, caller `trap_code`); Windows API enumeration doc.
- **Exclusions:** allocator internals (WP-M0-14); rt_io (15a); rt_proc (15b).
- **Dependencies / inputs:** WP-M0-14 allocator API; spec §15.4–15.5, §15.7 (calling convention); ADR-004 Windows baseline.
- **Expected artifacts:** `bootstrap/runtime/rt_trap/**`, `bootstrap/runtime/README.md`, `bootstrap/build/rt_trap.txt`, trap record/exit tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests + doc, ~8 test functions, ~5 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. `rt.trap.report` emits a JSONL record (`AIC-U0000`, caller `trap_code`) and exits 70.
  2. `README.md` enumerates every runtime-facing Windows call against Win10 22H2 x64 and notes the no-OS-updates baseline.
- **Verification / review class:** self-review + Reviewer independent review; Windows API doc reviewed by Main Designer for baseline conformance (class: Reviewer + Main Designer for the API doc).
- **Risks:** Windows API enumeration drift. **Escalation:** API/baseline question → Main Designer.

---

### WP-M0-16 — Canonical IR contract and implementation (split: WP-M0-16a..16c)

- **Sizing estimate (whole):** ~170–220 senior turns. Above threshold → split below per §1b. Basis: contract drafting (Main Designer gate) + core implementation + builder, §14.1(6)/ADR-001 rule surface.

#### WP-M0-16a — IR contract document

- **Objective:** produce `docs/contracts/IR-CONTRACT-*.md` defining the canonical target-neutral IR (instruction set, node kinds, determinism, span/causal-chain preservation) satisfying spec §14.1(6) — every semantic rule representable and enforceable; obtain Main Designer acceptance before implementation proceeds.
- **Scope:** IR contract document; representation coverage; determinism statement.
- **Exclusions:** IR implementation (16b/16c); optimizations (deferred per ADR-001); IR changes to the public language contract (none; IR is internal).
- **Dependencies / inputs:** spec §14.1(6) boundary; ADR-001 pipeline stage 6; WP-M0-09/11/13 shapes.
- **Expected artifacts:** `docs/contracts/IR-CONTRACT-*.md` (accepted by Main Designer) + acceptance record.
- **Sizing estimate:** 45–55 senior turns. Basis: document + review iterations, ~5 normative sections; no code.
- **Capability:** `senior_specialist` (contract drafting; Main Designer review gate). **Host toolchain required:** no (spec work).
- **Acceptance criteria:**
  1. Contract states IR determinism, target-neutrality, span/cause preservation, and representation coverage for every semantic rule; Main Designer accepts it.
- **Verification / review class:** Main Designer architecture review (class: Reviewer + Main Designer gate).
- **Risks:** IR design drift into architecture-by-specialist. **Escalation:** IR boundary conflict → Main Designer; spec-representability gap → Main Designer (spec §17.2).

---

#### WP-M0-16b — IR core: node model, invariants, deterministic dump

- **Objective:** implement the IR node model and core machinery per the accepted IR contract: node kinds, invariants, deterministic printing/verification support.
- **Scope:** IR node model; invariant enforcement (`AIC-I0501`); deterministic dump/verification.
- **Exclusions:** AST→IR builder (16c); contract drafting (16a); x86-64 codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-16a (accepted contract); WP-M0-06; spec §14.1(6).
- **Expected artifacts:** `bootstrap/src/ir/ir_core.*`, `bootstrap/src/ir/ir_dump.*`, `bootstrap/build/ir.txt`, IR core unit tests (invariants, deterministic dump).
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Implementation matches the accepted contract; invariant violations reported `AIC-I0501`.
  2. Dump/verification output deterministic.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** contract/implementation mismatch. **Escalation:** IR boundary conflict → Main Designer.

---

#### WP-M0-16c — IR builder (typed AST → IR)

- **Objective:** implement the IR builder mapping typed/resolved AST → IR per the accepted contract, preserving source spans and causal chains.
- **Scope:** builder over typed AST; span/cause preservation; AST→IR mapping tests.
- **Exclusions:** IR core (16b); contract (16a); codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-16b; WP-M0-09, WP-M0-11, WP-M0-13; spec §14.1(6).
- **Expected artifacts:** `bootstrap/src/ir/ir_builder.*`, `bootstrap/build/ir_builder.txt`, IR builder unit tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Identical AST → identical IR; IR preserves source spans and causal chains for diagnostics/traps.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** builder coverage gaps. **Escalation:** IR boundary conflict → Main Designer.

---

### WP-M0-17 — x86-64 backend (split: WP-M0-17a..17c)

- **Sizing estimate (whole):** ~230–290 senior turns. Above threshold → split below per §1b. Basis: instruction selection + frame/regalloc + trap branches, §14.1(7)/§14.3/§15.7 rule surface; comparable to parser actuals.

#### WP-M0-17a — Instruction selection and deterministic output

- **Objective:** implement IR → x86-64 instruction selection per spec §14.1(7)/§14.3: baseline ≤ x86-64 + SSE2 (no AVX2 required), deterministic output ordering.
- **Scope:** instruction selection; register-usage determinism; backend constraint violations (`AIC-B0601`); deterministic output ordering.
- **Exclusions:** frame layout/regalloc (17b); trap branches (17c); COFF emission (WP-M0-18).
- **Dependencies / inputs:** WP-M0-16 IR contract/impl; spec §14.1(7), §14.3.
- **Expected artifacts:** `bootstrap/src/backend/isel.*`, `bootstrap/build/backend_a.txt`, codegen unit tests, assembly dump tests.
- **Sizing estimate:** 55–60 senior turns. Basis: ~700–900 LOC incl. tests, ~12 test functions, ~8 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Generated instruction set uses only x86-64 + SSE2; no AVX2/host-specific instructions required.
  2. Output is deterministic: identical IR → identical assembly bytes.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** instruction-selection gaps. **Escalation:** backend constraint → Planner; ABI question → Main Designer.

---

#### WP-M0-17b — Frame layout, register allocation, and calls

- **Objective:** implement stack layout, simple/deterministic register allocation, function prologue/epilogue, and call emission per spec §15.7 (Microsoft x64 convention).
- **Scope:** frame layout; register allocation (simple/deterministic); prologue/epilogue; call emission (RCX/RDX/R8/R9, shadow space, 16-byte alignment, RAX return); `main` entry setup; noreturn handling.
- **Exclusions:** instruction selection (17a); trap branches (17c); COFF (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17a; spec §14.3, §15.5, §15.7.
- **Expected artifacts:** `bootstrap/src/backend/frame.*`, `bootstrap/build/backend_b.txt`, calling-convention and frame tests.
- **Sizing estimate:** 55–60 senior turns. Basis: ~700–900 LOC incl. tests, ~12 test functions, ~8 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Runtime calls follow the §15.7 convention (RCX/RDX/R8/R9, shadow space, 16-byte alignment, RAX return).
  2. Prologue/epilogue and `main` entry setup correct; noreturn handled without corrupting the frame.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** calling-convention errors. **Escalation:** ABI question → Main Designer.

---

#### WP-M0-17c — Trap branches and checked-operation emission

- **Objective:** implement deterministic trap branches for every runtime-failable checked operation (checked arithmetic, bounds, null deref, pointer-arithmetic overflow) with stable codes and source spans.
- **Scope:** trap branch emission (`AIC-R0801..R0816` per operation); span/cause preservation on trap records.
- **Exclusions:** instruction selection (17a); frame/regalloc (17b); COFF (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17a/b; spec §14.3, §15.5, §15.7.
- **Expected artifacts:** `bootstrap/src/backend/trap.*`, `bootstrap/build/backend_c.txt`, trap-branch unit tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~7 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Every runtime-failable checked operation emits a deterministic trap branch with the correct `AIC-Rxxxx` code and source span.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** trap-branch determinism. **Escalation:** backend constraint → Planner.

---

### WP-M0-18 — COFF object emission (split: WP-M0-18a..18b)

- **Sizing estimate (whole):** ~140–180 senior turns. Above threshold → split below per §1b. Basis: section/symbol/relocation tables + byte-determinism + link verification, §14.2/§14.3 rule surface.

#### WP-M0-18a — COFF writer core

- **Objective:** implement deterministic COFF object emission per spec §14.2/§14.3: section/symbol/relocation tables, canonical record/section order, zero compiler-controlled timestamps, repository-relative canonically separated paths, no random/host identifiers.
- **Scope:** COFF writer; canonical ordering; object emission of the backend's sections.
- **Exclusions:** link verification (18b); codegen (WP-M0-17); PE/linking (WP-M0-19).
- **Dependencies / inputs:** WP-M0-17 backend output contract; spec §14.2, §14.3.
- **Expected artifacts:** `bootstrap/src/coff/coff_emit.*`, `bootstrap/build/coff_a.txt`, byte-level determinism tests, object inspection tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Identical inputs → byte-identical COFF objects; zero timestamps, canonical order, relative paths; no host/build-machine identity.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** COFF field nondeterminism. **Escalation:** COFF detail → Planner.

---

#### WP-M0-18b — COFF verification and linker smoke

- **Objective:** verify emitted objects link with accepted external linkers (`link.exe` / `lld-link`) and are readable by inspection tools (dumpbin/llvm-objdump); maintain byte-determinism machinery.
- **Scope:** link smoke tests; object inspection tests; determinism verification.
- **Exclusions:** COFF writer core (18a); PE/linking (WP-M0-19).
- **Dependencies / inputs:** WP-M0-18a; baseline tooling (dumpbin on PATH, llvm-objdump off PATH).
- **Expected artifacts:** `bootstrap/src/coff/determinism.*`, `bootstrap/build/coff_b.txt`, link/inspection verification tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~8 test functions, ~5 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes (MSVC and/or Clang/LLVM for verification).
- **Acceptance criteria:**
  1. Objects link with both `link.exe` (via initialized environment) and `lld-link`; inspection tools read them cleanly.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** tool-order canonicalization. **Escalation:** linker incompatibility → Main Designer/Researcher.

---

### WP-M0-19 — Build driver, manifest, and link integration (split: WP-M0-19a..19c)

- **Sizing estimate (whole):** ~190–240 senior turns. Above threshold → split below per §1b. Basis: pipeline orchestration + manifest emission + linker integration, §14.1/§14.4/§16.3 rule surface.

#### WP-M0-19a — Pipeline orchestration and CLI

- **Objective:** implement `main()` and pipeline orchestration (load→lex→parse→name→types→const→sema→ir→backend→coff→link), deterministic CLI/options, diagnostics to stderr, and exit codes.
- **Scope:** driver `main()`; pipeline driver; deterministic option parsing; exit codes.
- **Exclusions:** build manifest emission (19b); linker invocation (19c); language semantics.
- **Dependencies / inputs:** WP-M0-06..18; spec §14.1.
- **Expected artifacts:** `bootstrap/src/driver/main.*`, `bootstrap/src/driver/cli.*`, `bootstrap/build/driver_a.txt`, pipeline unit tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Pipeline orchestration runs all stages in order; invalid programs produce JSONL diagnostics and non-zero exit.
  2. Option parsing is deterministic (sorted options).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** exit-code ambiguity. **Escalation:** manifest/contract conflict → Planner.

---

#### WP-M0-19b — Build manifest emission

- **Objective:** implement build-manifest emission per spec §14.4: schema version, project root, entry module, module list, AI-Co language/spec version field, sorted options, linker flag set, relative artifact paths, SHA-256 of each artifact excluding the manifest, diagnostic summary, exit status.
- **Scope:** manifest writer; hashed-artifact set; self-hash exclusion (FIND-G2-02/03); stage-invariant version.
- **Exclusions:** pipeline (19a); linker invocation (19c).
- **Dependencies / inputs:** WP-M0-19a; spec §14.4.
- **Expected artifacts:** `bootstrap/src/driver/manifest.*`, `bootstrap/build/driver_b.txt`, manifest fixture tests.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~7 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Manifest fields match §14.4 exactly: no self-hash; identical relative output paths across stage builds; stage-invariant version field; sorted build options; recorded linker flag set.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** manifest drift. **Escalation:** manifest/contract conflict → Planner.

---

#### WP-M0-19c — External linker integration and end-to-end compile

- **Objective:** implement external linker invocation per spec §16.3 (never bare `link`; explicit path/initialized env), entry validation (`AIC-E0418`), and end-to-end compile tests on corpus programs.
- **Scope:** linker invocation; entry validation; end-to-end compile tests; link-failure reporting (`AIC-O0702`).
- **Exclusions:** pipeline (19a); manifest (19b); M2 project-owned linker (out of scope).
- **Dependencies / inputs:** WP-M0-19a/b; spec §14.1, §16.3; baseline linker paths.
- **Expected artifacts:** `bootstrap/src/driver/link.*`, `bootstrap/build/driver_c.txt`, end-to-end compile tests on corpus programs, manifest fixtures.
- **Sizing estimate:** 50–60 senior turns. Basis: ~600–800 LOC incl. tests, ~10 test functions, ~6 normative rules.
- **Capability:** `senior_specialist`. **Host toolchain required:** yes (MSVC and Clang; both linkers).
- **Acceptance criteria:**
  1. End-to-end compile of valid programs produces COFF + linked PE via the accepted linker modes; invalid programs produce JSONL diagnostics and non-zero exit.
  2. Never invokes bare `link`; linker identity/version recorded only in comparison evidence (spec §16.3), never in the manifest.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** linker-mode mismatch. **Escalation:** linker nondeterminism → Main Designer.

---

### WP-M0-20 — Stage-0 integration verification

- **Sizing estimate:** 40–55 senior turns — within threshold, no split. Basis: execution/evidence work, no new source; ~4 normative checks (both compilers, suite pass, determinism, manifest conformance).
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

For each WP-M0 card, verify before dispatch: assignee profile Active and capability matches; workspace `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`; parent dependency edges set (WP-M0-N depends on WP-M0-(N-1); WP-M0-05 also depends on WP-M0-02..04 schemas; WP-M0-14/WP-M0-15 coordinate the rt.trap interface — represent as a documented interface note, not a shared file); delivery destination = owned area in §2; acceptance criteria present; review class recorded; card body carries the completion protocol rule (manifest §1.8 — worker must block `review-required:` and must not `kanban_complete` own review-class work); no human gate outstanding. If any package requires a Planner refinement (e.g., harness schema conflict), return to the Planner rather than inventing content.

**Task-sizing and supersession (binding, from the 2026-08-10 amendment):** for the remaining unrun packages (WP-M0-11..20), dispatch per the §3 split cards, not the original oversized packages. When the amended manifest reaches the Coordinator, create the new split cards per the manifest (each with its own owned area, acceptance criteria, review class, serial edges, and sizing estimate) and supersede the original oversized card for that package (supersession pattern already used for push cards). Existing already-created M0 cards keep their bodies — no board edits for cards already dispatched or in-flight. Each split card's body must carry its §1b sizing estimate; dispatch split siblings strictly serially (e.g., WP-M0-11a → 11b → 11c → 11d) and do not start a split group until the previous group's cards are verified.
