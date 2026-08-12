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

**Amendment (2026-08-11):** sizing-estimate recalibration against actuals + senior lane-budget recommendation — §1b gains the 2026-08-11 calibration anchors (WP-M0-11a..11d, WP-M0-12a), a calibrated per-area LOC/turn multiplier rule, and a recommended senior-lane budget change; every remaining package (WP-M0-12b..19c) is re-estimated under the multiplier and pre-split where the realistic estimate exceeds the recomputed threshold. WP-M0-12b, 13a..13d, 14a..14b, 15a, 15c, 16b..16c, 17a..17c, 18a..18b, 19a..19c are sub-split into two second-level cards each (12b1/12b2 … 19c1/19c2) in §2/§3; WP-M0-15b, 16a, and 20 are retained single with revised Sizing. The original first-level split rows for those packages are superseded per the existing split convention (no board edits here; dispatch remains the Coordinator's job after this amendment is accepted, per §5).
- Source: Main Designer evidence (task t_d54d7e23, 2026-08-11) — WP-M0-11c (~2,625 LOC vs 500–700 estimate), WP-M0-11d (~3,597 vs 600–800), WP-M0-12a (~4,058 vs 600–800) each exhausted the 90-iteration senior budget and needed 2–4 runs plus watchdog continuations; residual risk documented on t_2e52b09e / t_5988c42e / t_9a70ee96 (comment 510). Actuals: Kanban run records (runs 154–367) + repository LOC counts (git HEAD 8ba332b).
- Decision authority for this planning-artifact amendment: Main Designer (OM §8). No new Human Sponsor claims. The lane-budget change itself is a **recommendation** routed to the Coordinator for execution and recording (owner named in §1b); this document changes no runtime configuration.
- Board-state scope: no board edits here. Existing already-created/dispatched cards keep their bodies per §1 rule 9; the Coordinator supersedes the original over-threshold cards for unrun packages per §5 when the amended manifest reaches it.
- Evidence: Main Designer evidence note (2026-08-11); Kanban run histories for t_518e9bbe (11a), t_3656a3a8 (11b), t_2e52b09e (11c), t_5988c42e (11d), t_9a70ee96 (12a); repo `wc -l` counts of the delivered owned areas at HEAD 8ba332b; milestone-plan §7 one-line reference updated in the companion document.

**Amendment (2026-08-12):** file-ownership carve-out recorded — WP-M0-12b2 (t_159df8e5, commit e688c6e) converted four `derived-semantic-*` negative-corpus cases to const-context sites per its expected artifacts ("failure-record tests + negative-corpus anchors"). §2 now carves those four case directories out of WP-M0-03's `tests/negative/**` ownership into co-ownership with WP-M0-12b2 so future WP-M0-03 work does not conflict over them.
- Source: Reviewer2 verdict t_ca8bf4d5 (comment 712), Suggestion (non-blocking), routed via Coordinator as t_38568fc1; commit e688c6e (WP-M0-12b2) converted the four anchors; Coordinator gate record t_7b943196.
- Decision authority: Planner-owned bookkeeping amendment per task t_38568fc1 — records ownership consistent with the already-accepted WP-M0-12b2 scope; no new architecture decision; no governance change. Escalation on file-ownership ambiguity → Main Designer.
- Board-state scope: no board edits here. Future WP-M0-03 cards must respect the §2 carve-out; the WP-M0-03 owned area is `tests/negative/**` minus the four carved-out case directories.
- Evidence: `git show --stat e688c6e` (four case dirs `input.ai`/`expected.json`/`meta.json` modified); parent task t_159df8e5 completion metadata; §2 matrix below.

**Amendment (2026-08-12):** E0403 missing-initializer reachability ruling — grammar + parser leniency for `var` declarations. Spec §5.2 amended (v0.1.3): `var_decl`/`global_var_decl` initializer now syntactically optional; missing initializer is the semantic rule AIC-E0403 (spec §8.2, DIAGNOSTIC-CONTRACT §11.5, corpus anchor `derived-semantic-missing-init`); `const` forms unchanged (missing `=` stays AIC-S0101). A parser-leniency fix card (WP-M0-09-fix, senior_specialist) is added by the Planner from `docs/planning/AI-CO-PLANNER-RULING-E0403-MISSING-INIT-2026-08-12.md` §5 and is the serial prerequisite for WP-M0-13a2 (t_b174081d) resumption.
- Source: WP-M0-13a2 specialist evidence t_b174081d comment 744 (AC2 unsatisfiable: accepted parser rejects missing initializer as AIC-S0101 and drops the declaration, making E0403 unreachable); watchdog escalation t_dcb5540e; independent verification by the Planner against spec/DIAGNOSTIC-CONTRACT/corpus/parse.c in the ruling.
- Decision authority: Planner per milestone plan §9 (spec/contract ambiguity → Planner) and §7 downstream-gap re-planning/scope-amendment rule; ambiguity ruling within accepted direction — no new architecture decision, no ADR/contract/governance change, no Human Sponsor gate.
- Board-state scope: new WP-M0-09-fix card created by the Planner (assignee senior_specialist; WP-M0-09 owned area `bootstrap/src/parse/**`); WP-M0-13a2 stays blocked until the fix card's review PASS; Coordinator creates/dispatches the card and gates 13a2 resumption per OM §6.1. No existing card bodies edited.
- Evidence: `docs/planning/AI-CO-PLANNER-RULING-E0403-MISSING-INIT-2026-08-12.md`; spec v0.1.3 revision-log entry; commit(s) for this ruling.

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

**Calibration anchors (recorded 2026-08-11, from Kanban run records runs 154–367 + repo LOC counts at HEAD 8ba332b).**
- WP-M0-11a (type identity/tables): 1 implementation run, no budget hit; ~1,950 LOC total (source ~914 + tests ~650 + headers ~245 + README ~129 + fragment ~11), 14 test functions / 132 checks. Estimate was 600–800 LOC / 50–60 turns → delivered 2.4–3.3× the LOC estimate.
- WP-M0-11b (layout): 1 implementation run, no budget hit; ~2,374 LOC total (layout.c ~994 + layout_test.c ~1,110 + layout.h ~270), 17 test functions / 202 checks. Estimate was 500–700 / 45–55 → 3.4–4.7×.
- WP-M0-11c (conversions): 2–3 runs (one run killed at the 90/90 budget), ~2,625 LOC total (convert.c ~1,371 + convert_test.c ~1,046 + convert.h ~199), 19 test functions / 440 checks. Estimate was 500–700 / 45–55 → 3.8–5.3×.
- WP-M0-11d (optype): 6+ runs (two explicit 90/90 budget hits plus budget-killed crashes; reached the AIC-WATCHDOG-CONTINUE-FLAG human-decision cap and finished only after a Marcel-authorized RESUME), ~3,597 LOC total (optype.c ~1,814 + optype_test.c ~1,712 + optype.h ~167), 13 test functions / 657 checks. Estimate was 600–800 / 50–60 → 4.5–6.0×.
- WP-M0-12a (const eval core): 4 runs (three consecutive 90/90 budget hits; completed on the 4th; review pending at recording time), ~4,058 LOC total (eval_core.c ~2,673 + eval_core_test.c ~1,092 + eval_core.h ~282), 13 test functions / 516 checks. Estimate was 600–800 / 50–60 → 5.1–6.8×.

**Multiplier rule (calibrated 2026-08-11).** For compiler-core C packages (source + tests + headers + fragment + README):
- **Delivered LOC ≈ 3–5× the naive component-scope estimate (point 4×; observed range 2.4–6.8×, mean ≈ 4.4×).** A "~600–800 LOC incl. tests" estimate has historically delivered ~1,950–4,100 LOC. Corpus/junior packages are NOT covered by this multiplier (they fit the junior lane); the multiplier applies to senior compiler/runtime implementation packages.
- **Agent turns ≈ delivered total LOC ÷ rate**, where rate by spec-rule complexity: low ≈ 25 LOC/turn (tables/layout — 11a/11b each fit one run), moderate ≈ 17 (conversions/optype — 11c needed 2 runs), high ≈ 13 (const evaluator — 12a needed 4 runs). For planning, use 17 LOC/turn (moderate) and 13 (high); do not assume better than the complexity-adjusted rate.
- **One-run envelope at the current 90-turn budget:** ≈ ≤2,300 LOC (low complexity), ≈ ≤1,500 (moderate), ≈ ≤1,200 (high). At the recommended 150-turn budget: ≈ ≤3,800 / ≤2,500 / ≤2,000.

**Lane-budget recommendation (2026-08-11).** Raise the senior lane budget from **90 → 150** agent turns (`agent.max_turns` on the `senior_specialist` profile).
- Basis: the proven one-run envelope at 90 turns is ≈2,300–2,600 LOC (11a/11b fit; 11c at 2,625 hit the wall once). Packages at 3,600–4,100 LOC (11d/12a) needed 2–4 runs and watchdog continuations, and 11d reached the human-decision cap (AIC-WATCHDOG-CONTINUE-FLAG). At 150 turns the observed 2,625–4,058 LOC packages complete in 1–2 runs instead of 2–4, roughly halving per-package run fragmentation and watchdog overhead, while staying within the observed 19–87 min run-duration range (scaled ≈30–145 min) and the 200K context window.
- Threshold: the §1b formula (estimate ≤ ~70% of lane budget) is unchanged; at the recommended budget the senior threshold becomes **≈105 turns** (was ≈63). Junior lane unchanged (60 turns; no junior packages remain in M0).
- Owner of the config change: **Coordinator** — executes `hermes -p senior_specialist config set agent.max_turns 150` via the Deployment Guide §15 supported configuration surface, records the change on the board, and verifies it takes effect before dispatch of WP-M0-12b onward. Decision authority for raising the turn budget after the continuation cap is Coordinator/human per OM §6.1; the Planner makes no config change itself. If the budget raise is declined, the Coordinator returns over-threshold cards to the Planner for re-splitting per §1b rather than dispatching them over-threshold.

**Lesson for remaining packages.** Compiler-core packages are the risk class: conceptually small, but large raw effort (many tests, subtle spec rules). Remaining packages WP-M0-12b..19c below are re-estimated under the 2026-08-11 multiplier (3–5× LOC, 13–17 LOC/turn); every package whose realistic estimate exceeds the recomputed threshold (≈105 turns at the recommended budget) is pre-split into second-level cards (12b1/12b2 … 19c1/19c2). Estimates are stated as turn ranges with their LOC basis so the next calibration can compare.

## 2. File-ownership matrix (disjoint areas)

| Package | Owned repository area |
|---|---|
| WP-M0-01 | `bootstrap/build/` top-level entry points + toolchain init scripts + `CONVENTIONS.md` + the empty `bootstrap/build/` directory convention; `bootstrap/README.md`; `.gitignore` (tests/artifacts only). Per-area fragment files (`bootstrap/build/<area>.txt`) belong to their area packages, not to WP-M0-01. |
| WP-M0-02 | `tests/conformance/**` |
| WP-M0-03 | `tests/negative/**` (except the four const-context anchor cases carved out to WP-M0-12b2 per the 2026-08-12 amendment: `tests/negative/cases/derived-semantic-{shift-out-of-range,index-out-of-range,str-slice-boundary,ptr-diff-divisible}/**`) |
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
| WP-M0-12b1 | `bootstrap/src/const/eval_fail_arith.*` (checked arithmetic/div-zero/shift failure evaluation), `bootstrap/build/const_b1.txt` |
| WP-M0-12b2 | `bootstrap/src/const/eval_fail_rec.*` (cast/index/slice/ptr-diff failure sites + record emission), `bootstrap/build/const_b2.txt`, plus const-context negative-corpus anchors `tests/negative/cases/derived-semantic-{shift-out-of-range,index-out-of-range,str-slice-boundary,ptr-diff-divisible}/**` (co-owned with WP-M0-03 per the 2026-08-12 amendment) |
| WP-M0-13a1 | `bootstrap/src/sema/decl_core.*` (declaration model, storage/mutability/assignability), `bootstrap/build/sema_a1.txt` |
| WP-M0-13a2 | `bootstrap/src/sema/decl_init.*` (initializers, missing-initializer rules), `bootstrap/build/sema_a2.txt` |
| WP-M0-13b1 | `bootstrap/src/sema/expr_core.*` (precedence, evaluation-order, const-context), `bootstrap/build/sema_b1.txt` |
| WP-M0-13b2 | `bootstrap/src/sema/expr_ops.*` (checked-arithmetic decisions, comparison semantics, operator sites), `bootstrap/build/sema_b2.txt` |
| WP-M0-13c1 | `bootstrap/src/sema/stmt_core.*` (statement rules, switch/break/continue), `bootstrap/build/sema_c1.txt` |
| WP-M0-13c2 | `bootstrap/src/sema/stmt_reach.*` (reachability), `bootstrap/build/sema_c2.txt` |
| WP-M0-13d1 | `bootstrap/src/sema/fn_core.*` (return rules, return-value mismatch), `bootstrap/build/sema_d1.txt` |
| WP-M0-13d2 | `bootstrap/src/sema/fn_main.*` (main validation, reserved names), `bootstrap/build/sema_d2.txt` |
| WP-M0-14a1 | `bootstrap/runtime/rt_mem/rt_mem_alloc.*` (allocation registry + semantics), `bootstrap/build/rt_mem_a1.txt` |
| WP-M0-14a2 | `bootstrap/runtime/rt_mem/rt_mem_api.*` (public alloc/dealloc/copy/fill API + integration tests), `bootstrap/build/rt_mem_a2.txt` |
| WP-M0-14b1 | `bootstrap/runtime/rt_mem/rt_mem_reuse.*` (exact-fit reuse, 0xDD, reverse-order), `bootstrap/build/rt_mem_b1.txt` |
| WP-M0-14b2 | `bootstrap/runtime/rt_mem/rt_mem_trap.*` (duplicate/invalid release traps), `bootstrap/build/rt_mem_b2.txt` |
| WP-M0-15a1 | `bootstrap/runtime/rt_io/rt_io_core.*` (handle model, open/read/write/close), `bootstrap/build/rt_io1.txt` |
| WP-M0-15a2 | `bootstrap/runtime/rt_io/rt_io_stdio.*` (stdio behavior, failure paths), `bootstrap/build/rt_io2.txt` |
| WP-M0-15b | `bootstrap/runtime/rt_proc/**`, `bootstrap/build/rt_proc.txt` |
| WP-M0-15c1 | `bootstrap/runtime/rt_trap/rt_trap.*`, `bootstrap/build/rt_trap1.txt` |
| WP-M0-15c2 | `bootstrap/runtime/README.md` (Windows API baseline doc), `bootstrap/build/rt_trap2.txt` |
| WP-M0-16a | `docs/contracts/IR-CONTRACT-*.md` (draft + acceptance record) |
| WP-M0-16b1 | `bootstrap/src/ir/ir_core.*` (node model, invariants), `bootstrap/build/ir1.txt` |
| WP-M0-16b2 | `bootstrap/src/ir/ir_dump.*` (deterministic dump/verification), `bootstrap/build/ir2.txt` |
| WP-M0-16c1 | `bootstrap/src/ir/ir_builder_core.*` (typed AST → IR mapping), `bootstrap/build/ir_builder1.txt` |
| WP-M0-16c2 | `bootstrap/src/ir/ir_builder_cause.*` (span/cause preservation + determinism tests), `bootstrap/build/ir_builder2.txt` |
| WP-M0-17a1 | `bootstrap/src/backend/isel_core.*` (instruction selection, deterministic output), `bootstrap/build/backend_a1.txt` |
| WP-M0-17a2 | `bootstrap/src/backend/isel_x64.*` (x86-64+SSE2 coverage, AIC-B0601), `bootstrap/build/backend_a2.txt` |
| WP-M0-17b1 | `bootstrap/src/backend/frame.*` (stack layout, prologue/epilogue), `bootstrap/build/backend_b1.txt` |
| WP-M0-17b2 | `bootstrap/src/backend/call.*` (register allocation, call emission), `bootstrap/build/backend_b2.txt` |
| WP-M0-17c1 | `bootstrap/src/backend/trap_branch.*` (trap branch emission), `bootstrap/build/backend_c1.txt` |
| WP-M0-17c2 | `bootstrap/src/backend/trap_checked.*` (checked-op emission, span/cause), `bootstrap/build/backend_c2.txt` |
| WP-M0-18a1 | `bootstrap/src/coff/coff_sections.*` (section/symbol/relocation tables, canonical order), `bootstrap/build/coff_a1.txt` |
| WP-M0-18a2 | `bootstrap/src/coff/coff_determinism.*` (byte-determinism, timestamps, paths), `bootstrap/build/coff_a2.txt` |
| WP-M0-18b1 | `bootstrap/src/coff/coff_verify.*` (link smoke, object inspection), `bootstrap/build/coff_b1.txt` |
| WP-M0-18b2 | `bootstrap/src/coff/coff_detmach.*` (determinism machinery, tool-order canonicalization), `bootstrap/build/coff_b2.txt` |
| WP-M0-19a1 | `bootstrap/src/driver/main.*` (pipeline orchestration), `bootstrap/build/driver_a1.txt` |
| WP-M0-19a2 | `bootstrap/src/driver/cli.*` (deterministic CLI/options, exit codes), `bootstrap/build/driver_a2.txt` |
| WP-M0-19b1 | `bootstrap/src/driver/manifest_writer.*` (manifest emission §14.4), `bootstrap/build/driver_b1.txt` |
| WP-M0-19b2 | `bootstrap/src/driver/manifest_hash.*` (hashed artifact set, self-hash exclusion), `bootstrap/build/driver_b2.txt` |
| WP-M0-19c1 | `bootstrap/src/driver/link_invoke.*` (linker invocation §16.3), `bootstrap/build/driver_c1.txt` |
| WP-M0-19c2 | `bootstrap/src/driver/link_e2e.*` (end-to-end compile tests), `bootstrap/build/driver_c2.txt` |
| WP-M0-20 | `docs/verification/STAGE0-INTEGRATION-*.md` (writes evidence; modifies no source) |
| WP-M1-01..04 | `bootstrap/selfhost/**`, `docs/verification/STAGE1-2-IDENTITY-*.md` (refined at M1 planning) |

Split packages own sub-area fragments (`<area>_<suffix>.txt`, e.g. `types_a.txt`) per the WP-M0-01 per-area fragment convention; the original single-fragment entry (e.g. `types.txt`) is superseded by the split entries above. Second-level sub-splits (2026-08-11 recalibration) follow the same convention with a digit suffix (e.g. `const_b1.txt`/`const_b2.txt` supersede `const_b.txt`); the first-level split entries for WP-M0-12b, 13a..13d, 14a..14b, 15a, 15c, 16b..16c, 17a..17c, 18a..18b, and 19a..19c are superseded by the second-level entries above. WP-M0-15b, 16a, and 20 keep their single-fragment entries.

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
- **Actuals (2026-08-11):** 1 implementation run (no budget hit); ~1,950 LOC total; 14 test functions / 132 checks; Reviewer PASS (t_5ca13527) + watchdog re-verifications PASS.

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
- **Actuals (2026-08-11):** 1 implementation run (no budget hit); ~2,374 LOC total; 17 test functions / 202 checks; Reviewer PASS (t_33e5e9df); MIN-11B-01 remediation closed (6b248f5).

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
- **Actuals (2026-08-11):** 2–3 runs (one run killed at the 90/90 budget); ~2,625 LOC total; 19 test functions / 440 checks; Reviewer PASS (t_f79f4280) + watchdog re-verifications PASS; Minor-1/Minor-2 citation fixes closed (914a8e4/c0d5f2e).

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
- **Actuals (2026-08-11):** 6+ runs incl. two 90/90 budget hits and AIC-WATCHDOG-CONTINUE-FLAG (Marcel-authorized RESUME); ~3,597 LOC total; 13 test functions / 657 checks; reviewer2 PASS with Minor findings (t_0db758e5); Minor-1 (pointer-arithmetic §12.5) + Suggestion-1 (T0305 prose) remediated (b575102/a03db88).

---

### WP-M0-12 — Constant-expression evaluator (split: WP-M0-12a..12b; 12b sub-split 12b1..12b2)

- **Sizing estimate (whole):** ~275–370 senior turns (12a actual ~270–360 at the 90-turn budget; 12b re-estimated ~135–190). Above threshold → split below per §1b. Basis: 8 const codes (`AIC-E0401`, `AIC-E0405..E0411`), §10.5/§11.3 rule surface; recalibrated 2026-08-11.

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
- **Actuals (2026-08-11):** 4 runs (three consecutive 90/90 budget hits; completed on the 4th); ~4,058 LOC total; 13 test functions / 516 checks; review pending at recording time (t_d6414279).

---

#### WP-M0-12b — Constant arithmetic failure semantics (sub-split: WP-M0-12b1..12b2)

- **Sizing estimate (whole):** ~135–190 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold (≈105 at the recommended budget) → sub-split below per §1b. Basis: sibling 12a actual 4,058 LOC after 3×90-turn budget hits; 12b adds failure evaluation + record emission, estimated ~2,200–3,200 LOC delivered at ~13–17 LOC/turn.

##### WP-M0-12b1 — Checked-arithmetic failure evaluation

- **Objective:** implement compile-time checked-arithmetic failure evaluation (overflow, division-by-zero, shift-range) per spec §10.5/§11.3, producing typed failure kinds consumed by 12b2.
- **Scope:** overflow/div-zero/shift failure evaluation (`AIC-E0405..E0407` kinds); typed EvalFailure values; API for 12b2 record emission.
- **Exclusions:** cast/index/slice/ptr-diff failures and record emission (12b2); evaluator composition (12a).
- **Dependencies / inputs:** WP-M0-12a; spec §10.5, §11.3.
- **Expected artifacts:** `bootstrap/src/const/eval_fail_arith.*`, `bootstrap/build/const_b1.txt`, checked-arithmetic failure tests.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~6 test functions, ~4 normative rules, high complexity (~13–17 LOC/turn). Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Constant overflow/div-zero/shift failures produce the correct typed failure kinds (never traps) per §11.3.
  2. Failure kinds are deterministic and consumable by 12b2 for record emission.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** failure-kind gaps. **Escalation:** const semantics ambiguity → Planner.

##### WP-M0-12b2 — Failure record emission and remaining sites

- **Objective:** implement the remaining const failure sites (cast-range, index-range, slice-boundary, pointer-difference) and deterministic failure-record emission `AIC-E0405..E0411` per spec §10.5/§11.3; enforce the never-trap guarantee.
- **Scope:** cast/index/slice/ptr-diff failure evaluation; record emission for all 7 const failure codes; correct spans + deterministic order; never-trap guarantee.
- **Exclusions:** checked-arithmetic failure kinds (12b1); runtime traps.
- **Dependencies / inputs:** WP-M0-12a, WP-M0-12b1; spec §10.5, §11.3.
- **Expected artifacts:** `bootstrap/src/const/eval_fail_rec.*`, `bootstrap/build/const_b2.txt`, failure-record tests + negative-corpus anchors.
- **Sizing estimate:** 70–100 senior turns. Basis: ~1,200–1,700 LOC incl. tests, ~7 test functions, ~4 normative rules, high complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. All constant failures emit `AIC-E0405..E0411` per spec with correct spans and deterministic order (never traps).
  2. Negative-corpus anchors for const failures pass with exact records.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** failure-path gaps; corpus anchor gaps. **Escalation:** const semantics ambiguity → Planner.

---

### WP-M0-13 — Semantic validation (split: WP-M0-13a..13d; each sub-split 13a1/13a2 … 13d1/13d2)

- **Sizing estimate (whole):** ~540–730 senior turns (recalibrated 2026-08-11; was 280–360). Above threshold → split below per §1b. Basis: 19 semantic codes (`AIC-E0401..E0419`) + remaining `AIC-T03xx` checks, §8/§10–§13 rule surface; each 13x sub-package is estimated at optype scale (3,597 LOC actual) → two cards each.

#### WP-M0-13a — Declarations and initialization (sub-split: WP-M0-13a1..13a2)

- **Sizing estimate (whole):** ~135–185 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b. Basis: optype-scale walker (3,597 LOC actual at ~17 LOC/turn) over all declaration/initializer sites, §8/§9 rule surface.

##### WP-M0-13a1 — Declaration model and assignability

- **Objective:** implement the declaration model and storage/mutability/assignability checks per spec §8: constants, variables, storage, assignment-to-const/non-lvalue rules.
- **Scope:** §8 declaration rules; const/assignability checks (`AIC-E0404/E0419`); declaration-site walker.
- **Exclusions:** initializers/§9 (13a2); expressions (13b); statements (13c); functions (13d).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09, WP-M0-10, WP-M0-11, WP-M0-12; spec §8.
- **Expected artifacts:** `bootstrap/src/sema/decl_core.*`, `bootstrap/build/sema_a1.txt`, declaration/assignability tests.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~7 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Declaration rules match §8 exactly; rejections carry correct codes/spans.
  2. Const/assignability rules (`AIC-E0404`, `AIC-E0419`) enforced.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** assignability edge cases. **Escalation:** semantic ambiguity → Planner.

##### WP-M0-13a2 — Initializers

- **Objective:** implement initialization semantics per spec §9: initializer forms, missing-initializer rules.
- **Scope:** §9 initialization; missing-initializer rules (`AIC-E0403`); initializer-site walker.
- **Exclusions:** declaration model/assignability (13a1); expressions (13b); statements (13c); functions (13d).
- **Dependencies / inputs:** WP-M0-13a1; WP-M0-06, WP-M0-09..12; spec §9.
- **Expected artifacts:** `bootstrap/src/sema/decl_init.*`, `bootstrap/build/sema_a2.txt`, initializer tests per §18.4.
- **Sizing estimate:** 70–95 senior turns. Basis: ~1,200–1,600 LOC incl. tests, ~7 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Initialization rules match §9 exactly; rejections carry correct codes/spans.
  2. Missing-initializer rules (`AIC-E0403`) enforced.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** init edge cases. **Escalation:** semantic ambiguity → Planner.

---

#### WP-M0-13b — Expressions, operators, and evaluation order (sub-split: WP-M0-13b1..13b2)

- **Sizing estimate (whole):** ~145–195 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b. Basis: expression/operator surface comparable to optype (3,597 LOC actual), §10.1–10.4/§11.3–11.6 rule surface.

##### WP-M0-13b1 — Expression core and evaluation order

- **Objective:** implement expression semantics and the evaluation-order model per spec §10.1–10.4: precedence, evaluation order, const-context use.
- **Scope:** expression validation; precedence; evaluation-order model; `AIC-E0401` const-context use.
- **Exclusions:** checked-arithmetic decisions/comparison semantics (13b2); declarations (13a); statements (13c); functions (13d).
- **Dependencies / inputs:** WP-M0-06, WP-M0-09..12, WP-M0-13a; spec §10.1–10.4.
- **Expected artifacts:** `bootstrap/src/sema/expr_core.*`, `bootstrap/build/sema_b1.txt`, expression/evaluation-order tests per §18.4.
- **Sizing estimate:** 70–95 senior turns. Basis: ~1,200–1,600 LOC incl. tests, ~7 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Evaluation-order rules (§10.4) modeled and testable.
  2. Const-context use (`AIC-E0401`) rejected at the correct sites.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** evaluation-order subtlety. **Escalation:** semantic ambiguity → Planner.

##### WP-M0-13b2 — Operator semantics

- **Objective:** implement operator semantics per spec §11.3–11.6: checked-arithmetic compile-time decisions, comparison semantics.
- **Scope:** checked-arithmetic decisions; comparison semantics; operator-site checks.
- **Exclusions:** precedence/evaluation-order (13b1); declarations (13a); statements (13c); functions (13d).
- **Dependencies / inputs:** WP-M0-13b1; WP-M0-11/12; spec §11.3–11.6.
- **Expected artifacts:** `bootstrap/src/sema/expr_ops.*`, `bootstrap/build/sema_b2.txt`, operator/comparison tests per §18.4.
- **Sizing estimate:** 75–100 senior turns. Basis: ~1,300–1,700 LOC incl. tests, ~8 test functions, ~5 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Checked-arithmetic compile-time decisions and comparison semantics match §11.3–11.6.
  2. No expression semantic rule silently passes an invalid program.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** operator subtlety. **Escalation:** semantic ambiguity → Planner.

---

#### WP-M0-13c — Statements and control flow (sub-split: WP-M0-13c1..13c2)

- **Sizing estimate (whole):** ~130–180 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b. Basis: statement/control-flow walker, §13 rule surface.

##### WP-M0-13c1 — Statement rules and switch/break/continue

- **Objective:** implement statement/control-flow semantics per spec §13: braces-only blocks, switch no-fall-through, loops, break/continue placement.
- **Scope:** §13 statement rules; switch terminator (`AIC-E0412`), duplicate case (`AIC-E0413`), break/continue placement (`AIC-E0414`).
- **Exclusions:** reachability (13c2); declarations (13a); expressions (13b); functions (13d).
- **Dependencies / inputs:** WP-M0-13a/b; spec §13, §18.4–18.5.
- **Expected artifacts:** `bootstrap/src/sema/stmt_core.*`, `bootstrap/build/sema_c1.txt`, statement/switch tests per §18.4.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~7 test functions, ~5 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Switch terminator (`AIC-E0412`), duplicate case (`AIC-E0413`), break/continue placement (`AIC-E0414`) match spec.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** control-flow edge cases. **Escalation:** semantic ambiguity → Planner.

##### WP-M0-13c2 — Reachability

- **Objective:** implement reachability analysis per spec §13.5.
- **Scope:** reachability (`AIC-E0416/E0417`).
- **Exclusions:** statement/switch rules (13c1); declarations (13a); expressions (13b); functions (13d).
- **Dependencies / inputs:** WP-M0-13c1; spec §13.5.
- **Expected artifacts:** `bootstrap/src/sema/stmt_reach.*`, `bootstrap/build/sema_c2.txt`, reachability tests per §18.4.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~6 test functions, ~2 normative rules, moderate complexity (reachability is subtle). Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Reachability per §13.5 (`AIC-E0416/E0417`).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** reachability conservatism. **Escalation:** semantic ambiguity → Planner.

---

#### WP-M0-13d — Functions, returns, and reserved names (sub-split: WP-M0-13d1..13d2)

- **Sizing estimate (whole):** ~125–175 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b. Basis: function-level walker, §8/§13.5/§4.5 rule surface.

##### WP-M0-13d1 — Return rules

- **Objective:** implement function-level return semantics per spec §8/§13.5.
- **Scope:** return value mismatch (`AIC-E0415`); missing return on non-void path (`AIC-E0416`).
- **Exclusions:** main validation/reserved names (13d2); statements (13c); IR lowering (WP-M0-16).
- **Dependencies / inputs:** WP-M0-13a..c; spec §8, §13.5.
- **Expected artifacts:** `bootstrap/src/sema/fn_core.*`, `bootstrap/build/sema_d1.txt`, return/function tests per §18.5.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Return mismatch and missing-return rules match §13.5 (`AIC-E0415/E0416`).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** return-rule edge cases. **Escalation:** semantic ambiguity → Planner.

##### WP-M0-13d2 — Entry main and reserved names

- **Objective:** implement entry `main` validation and reserved-name enforcement.
- **Scope:** `main` validation (`AIC-E0418`); reserved-name enforcement (`AIC-E0419` where applicable, §4.5).
- **Exclusions:** return rules (13d1); statements (13c); IR lowering (WP-M0-16).
- **Dependencies / inputs:** WP-M0-13d1; spec §4.5, §18.5.
- **Expected artifacts:** `bootstrap/src/sema/fn_main.*`, `bootstrap/build/sema_d2.txt`, main/reserved-name tests per §18.5.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Entry `main` validation per §18.5 (`AIC-E0418`); reserved-name enforcement per §4.5.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** reserved-name edge cases. **Escalation:** semantic ambiguity → Planner.

---

### WP-M0-14 — Runtime allocator (rt.mem) (split: WP-M0-14a..14b; each sub-split 14a1/14a2, 14b1/14b2)

- **Sizing estimate (whole):** ~220–320 senior turns (recalibrated 2026-08-11; was 110–150). Above threshold → split below per §1b. Basis: allocator determinism + reuse policy + trap integration, §15.1/ADR-004 rule surface; runtime C packages estimated at ~17 LOC/turn moderate complexity.

#### WP-M0-14a — Allocator core (sub-split: WP-M0-14a1..14a2)

- **Sizing estimate (whole):** ~110–160 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-14a1 — Allocation registry and semantics

- **Objective:** implement the deterministic project-owned allocator core per spec §15.1 and ADR-004: zero-initialized allocation, `null` on exhaustion, zero-size → `null` no-op, alignment ≥16, controlled address region.
- **Scope:** allocator registry; allocation semantics; exhaustion/zero-size behavior.
- **Exclusions:** public alloc/dealloc/copy/fill API and integration (14a2); reuse policy/0xDD/traps (14b); file I/O/process/trap implementation (WP-M0-15).
- **Dependencies / inputs:** spec §15.1, §15.5, §15.8; ADR-004; WP-M0-06 diag record shape via rt.trap contract.
- **Expected artifacts:** `bootstrap/runtime/rt_mem/rt_mem_alloc.*`, `bootstrap/build/rt_mem_a1.txt`, allocator core unit tests (zero-init, exhaustion, zero-size, alignment).
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~6 test functions, ~5 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Zero-initialized allocation; `alloc_bytes(0)` → `null` without state change; exhaustion → `null`, never a trap.
  2. Alignment ≥16; addresses within the controlled region.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** host-allocator nondeterminism leaking. **Escalation:** allocator ambiguity → Planner; Windows behavior → Researcher.

##### WP-M0-14a2 — Public allocator API and integration

- **Objective:** implement the public allocator API (`alloc_bytes`/`dealloc_bytes`/`copy`/`fill`) and registry integration per spec §15.1/ADR-004.
- **Scope:** public API surface; registry wiring; integration tests.
- **Exclusions:** allocation semantics (14a1); reuse policy/traps (14b).
- **Dependencies / inputs:** WP-M0-14a1; spec §15.1.
- **Expected artifacts:** `bootstrap/runtime/rt_mem/rt_mem_api.*`, `bootstrap/build/rt_mem_a2.txt`, API/integration tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. `alloc_bytes`/`dealloc_bytes`/`copy`/`fill` behave per §15.1 with deterministic observable behavior.
  2. Registry integration complete; no host-allocator identity leaks into behavior.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** API drift. **Escalation:** allocator ambiguity → Planner.

---

#### WP-M0-14b — Reuse policy and release traps (sub-split: WP-M0-14b1..14b2)

- **Sizing estimate (whole):** ~110–160 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-14b1 — Reuse policy and poisoning

- **Objective:** implement exact-fit reuse with 0xDD overwrite before reuse and reverse-order-of-release within a size class per spec §15.1/ADR-004; no split/coalesce.
- **Scope:** reuse registry and policy; 0xDD poisoning; reverse-order determinism (observable contract).
- **Exclusions:** duplicate/invalid-release traps (14b2); allocator core (14a).
- **Dependencies / inputs:** WP-M0-14a; spec §15.1, §15.8; ADR-004.
- **Expected artifacts:** `bootstrap/runtime/rt_mem/rt_mem_reuse.*`, `bootstrap/build/rt_mem_b1.txt`, reuse-order/poisoning tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Reuse order is exactly reverse-of-release within a size class; identical allocation/release sequences yield identical addresses (observable contract).
  2. Deallocated memory overwritten 0xDD before reuse.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** reuse-rule subtlety. **Escalation:** allocator ambiguity → Planner.

##### WP-M0-14b2 — Release traps

- **Objective:** implement duplicate/invalid-release trap integration per spec §15.5/§15.8.
- **Scope:** duplicate release (`AIC-R0812`); invalid release (`AIC-R0813`); trap-record integration (exit 70).
- **Exclusions:** reuse policy (14b1); allocator core (14a); trap implementation (WP-M0-15c).
- **Dependencies / inputs:** WP-M0-14b1; spec §15.5, §15.8; ADR-004.
- **Expected artifacts:** `bootstrap/runtime/rt_mem/rt_mem_trap.*`, `bootstrap/build/rt_mem_b2.txt`, release-trap tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Duplicate release → `AIC-R0812`; invalid release → `AIC-R0813`; traps report exit 70.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** trap integration drift. **Escalation:** allocator ambiguity → Planner.

---

### WP-M0-15 — Runtime I/O, process, and trap (split: WP-M0-15a..15c; 15a/15c sub-split)

- **Sizing estimate (whole):** ~265–395 senior turns (recalibrated 2026-08-11; was 160–210). Above threshold → split below per §1b. Basis: three modules + Windows API baseline doc, §15.2–15.5/§15.7 rule surface; 15a and 15c sub-split, 15b retained single (within threshold).

#### WP-M0-15a — rt.io (sub-split: WP-M0-15a1..15a2)

- **Sizing estimate (whole):** ~100–150 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-15a1 — rt.io core: handle model and file operations

- **Objective:** implement the rt_io handle model and file operations per spec §15.2: file handles/open/read/write/close.
- **Scope:** rt_io handle model; open/read/write/close; `0` on failure.
- **Exclusions:** stdio behavior/failure paths (15a2); process args/exit (15b); trap reporting (15c); allocator internals (WP-M0-14).
- **Dependencies / inputs:** WP-M0-14 allocator API; spec §15.2, §15.5; ADR-004 Windows baseline.
- **Expected artifacts:** `bootstrap/runtime/rt_io/rt_io_core.*`, `bootstrap/build/rt_io1.txt`, file-operation tests.
- **Sizing estimate:** 50–75 senior turns. Basis: ~800–1,200 LOC incl. tests, ~5 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. File open/read/write/close match §15.2 (handles, `0` on failure).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** Windows API surface drift. **Escalation:** API/baseline question → Main Designer; environment → Coordinator.

##### WP-M0-15a2 — rt.io stdio and failure paths

- **Objective:** implement stdio behavior and invalid-handle failure paths per spec §15.2.
- **Scope:** stdio behavior; invalid-handle failures (`AIC-R0814`).
- **Exclusions:** handle model/open/read/write/close (15a1); process args/exit (15b); trap reporting (15c).
- **Dependencies / inputs:** WP-M0-15a1; spec §15.2, §15.5.
- **Expected artifacts:** `bootstrap/runtime/rt_io/rt_io_stdio.*`, `bootstrap/build/rt_io2.txt`, stdio/failure tests.
- **Sizing estimate:** 50–75 senior turns. Basis: ~800–1,200 LOC incl. tests, ~5 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. stdio behavior per §15.2; invalid handles → `AIC-R0814`; `0` on failure.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** failure-path gaps. **Escalation:** API/baseline question → Main Designer.

---

#### WP-M0-15b — rt.proc

- **Objective:** implement `bootstrap/runtime/rt_proc/` per spec §15.3: process args (UTF-16→UTF-8, U+FFFD replacement) and exit.
- **Scope:** rt_proc module; args conversion determinism; `args()[0]` program path; `rt.proc.exit`.
- **Exclusions:** I/O (15a); trap reporting (15c).
- **Dependencies / inputs:** spec §15.3, §15.5; ADR-004 Windows baseline.
- **Expected artifacts:** `bootstrap/runtime/rt_proc/**`, `bootstrap/build/rt_proc.txt`, args-conversion/exit tests.
- **Sizing estimate:** 60–90 senior turns (recalibrated 2026-08-11; was 45–55). Basis: ~1,000–1,500 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity — within threshold (≈105), retained single; smallest runtime module (args conversion + exit only).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. `rt.proc.args()` converts deterministically with U+FFFD replacement for invalid surrogates; `args()[0]` is the program path.
  2. `rt.proc.exit` exits with the given code, no record.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** UTF-16 edge cases. **Escalation:** args ambiguity → Planner.

---

#### WP-M0-15c — rt.trap and runtime Windows API baseline doc (sub-split: WP-M0-15c1..15c2)

- **Sizing estimate (whole):** ~105–155 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-15c1 — rt.trap module

- **Objective:** implement `bootstrap/runtime/rt_trap/` per spec §15.4/contract §10: trap reporting (JSONL record to stderr, exit 70).
- **Scope:** rt_trap module; trap record shape (`AIC-U0000`, caller `trap_code`).
- **Exclusions:** runtime README/Windows API doc (15c2); allocator internals (WP-M0-14); rt_io (15a); rt_proc (15b).
- **Dependencies / inputs:** WP-M0-14 allocator API; spec §15.4–15.5, §15.7 (calling convention); ADR-004 Windows baseline.
- **Expected artifacts:** `bootstrap/runtime/rt_trap/rt_trap.*`, `bootstrap/build/rt_trap1.txt`, trap record/exit tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~5 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. `rt.trap.report` emits a JSONL record (`AIC-U0000`, caller `trap_code`) and exits 70.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** trap record shape drift. **Escalation:** contract conflict → Planner.

##### WP-M0-15c2 — Runtime Windows API baseline doc

- **Objective:** produce `bootstrap/runtime/README.md` enumerating every runtime-facing Windows call against the pinned baseline (ADR-004).
- **Scope:** Windows API enumeration doc (Win10 22H2 x64; no-OS-updates baseline); no code.
- **Exclusions:** rt_trap module (15c1); allocator internals (WP-M0-14); rt_io (15a); rt_proc (15b).
- **Dependencies / inputs:** ADR-004 Windows baseline; WP-M0-14/15 modules' call surface.
- **Expected artifacts:** `bootstrap/runtime/README.md`, `bootstrap/build/rt_trap2.txt`.
- **Sizing estimate:** 50–75 senior turns (recalibrated 2026-08-11; was 50–60). Basis: ~500–800 lines doc + review iterations, no code, low complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** no (doc; verifies against pinned baseline).
- **Acceptance criteria:**
  1. `README.md` enumerates every runtime-facing Windows call against Win10 22H2 x64 and notes the no-OS-updates baseline.
- **Verification / review class:** self-review + Reviewer independent review; Windows API doc reviewed by Main Designer for baseline conformance (class: Reviewer + Main Designer for the API doc).
- **Risks:** Windows API enumeration drift. **Escalation:** API/baseline question → Main Designer.

---

### WP-M0-16 — Canonical IR contract and implementation (split: WP-M0-16a..16c; 16b/16c sub-split)

- **Sizing estimate (whole):** ~290–405 senior turns (recalibrated 2026-08-11; was 170–220). Above threshold → split below per §1b. Basis: contract drafting (Main Designer gate) + core implementation + builder, §14.1(6)/ADR-001 rule surface; 16a retained single (doc), 16b/16c sub-split.

#### WP-M0-16a — IR contract document

- **Objective:** produce `docs/contracts/IR-CONTRACT-*.md` defining the canonical target-neutral IR (instruction set, node kinds, determinism, span/causal-chain preservation) satisfying spec §14.1(6) — every semantic rule representable and enforceable; obtain Main Designer acceptance before implementation proceeds.
- **Scope:** IR contract document; representation coverage; determinism statement.
- **Exclusions:** IR implementation (16b/16c); optimizations (deferred per ADR-001); IR changes to the public language contract (none; IR is internal).
- **Dependencies / inputs:** spec §14.1(6) boundary; ADR-001 pipeline stage 6; WP-M0-09/11/13 shapes.
- **Expected artifacts:** `docs/contracts/IR-CONTRACT-*.md` (accepted by Main Designer) + acceptance record.
- **Sizing estimate:** 45–60 senior turns (recalibrated 2026-08-11; was 45–55). Basis: document + review iterations, ~5 normative sections; no code — doc work is not covered by the compiler-core multiplier; within threshold (≈105), retained single.
- **Capability:** `senior_specialist` (contract drafting; Main Designer review gate). **Host toolchain required:** no (spec work).
- **Acceptance criteria:**
  1. Contract states IR determinism, target-neutrality, span/cause preservation, and representation coverage for every semantic rule; Main Designer accepts it.
- **Verification / review class:** Main Designer architecture review (class: Reviewer + Main Designer gate).
- **Risks:** IR design drift into architecture-by-specialist. **Escalation:** IR boundary conflict → Main Designer; spec-representability gap → Main Designer (spec §17.2).

---

#### WP-M0-16b — IR core: node model, invariants, deterministic dump (sub-split: WP-M0-16b1..16b2)

- **Sizing estimate (whole):** ~120–170 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-16b1 — IR node model and invariants

- **Objective:** implement the IR node model and invariant enforcement per the accepted IR contract.
- **Scope:** IR node kinds; invariant enforcement (`AIC-I0501`).
- **Exclusions:** deterministic dump (16b2); AST→IR builder (16c); contract drafting (16a); x86-64 codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-16a (accepted contract); WP-M0-06; spec §14.1(6).
- **Expected artifacts:** `bootstrap/src/ir/ir_core.*`, `bootstrap/build/ir1.txt`, IR core unit tests (invariants).
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Implementation matches the accepted contract; invariant violations reported `AIC-I0501`.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** contract/implementation mismatch. **Escalation:** IR boundary conflict → Main Designer.

##### WP-M0-16b2 — IR deterministic dump and verification

- **Objective:** implement deterministic IR printing/verification support per the accepted IR contract.
- **Scope:** deterministic dump; verification support; dump determinism tests.
- **Exclusions:** node model/invariants (16b1); AST→IR builder (16c); x86-64 codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-16b1; spec §14.1(6).
- **Expected artifacts:** `bootstrap/src/ir/ir_dump.*`, `bootstrap/build/ir2.txt`, dump determinism tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Dump/verification output deterministic (identical IR → identical dump bytes).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** dump nondeterminism. **Escalation:** IR boundary conflict → Main Designer.

---

#### WP-M0-16c — IR builder (typed AST → IR) (sub-split: WP-M0-16c1..16c2)

- **Sizing estimate (whole):** ~130–180 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-16c1 — IR builder core mapping

- **Objective:** implement the IR builder mapping typed/resolved AST → IR per the accepted contract.
- **Scope:** builder over typed AST; AST→IR mapping tests.
- **Exclusions:** span/cause preservation (16c2); IR core (16b); contract (16a); codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-16b; WP-M0-09, WP-M0-11, WP-M0-13; spec §14.1(6).
- **Expected artifacts:** `bootstrap/src/ir/ir_builder_core.*`, `bootstrap/build/ir_builder1.txt`, IR builder unit tests.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~7 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Identical AST → identical IR (structural mapping per the contract).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** builder coverage gaps. **Escalation:** IR boundary conflict → Main Designer.

##### WP-M0-16c2 — IR span/cause preservation

- **Objective:** implement source-span and causal-chain preservation on the IR builder output.
- **Scope:** span/cause preservation; determinism tests.
- **Exclusions:** structural mapping (16c1); IR core (16b); codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-16c1; WP-M0-09/11/13; spec §14.1(6).
- **Expected artifacts:** `bootstrap/src/ir/ir_builder_cause.*`, `bootstrap/build/ir_builder2.txt`, span/cause tests.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. IR preserves source spans and causal chains for diagnostics/traps; identical AST → byte-identical IR.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** span drift. **Escalation:** IR boundary conflict → Main Designer.

---

### WP-M0-17 — x86-64 backend (split: WP-M0-17a..17c; each sub-split)

- **Sizing estimate (whole):** ~425–570 senior turns (recalibrated 2026-08-11; was 230–290). Above threshold → split below per §1b. Basis: instruction selection + frame/regalloc + trap branches, §14.1(7)/§14.3/§15.7 rule surface; each 17x sub-package sub-split into two cards.

#### WP-M0-17a — Instruction selection and deterministic output (sub-split: WP-M0-17a1..17a2)

- **Sizing estimate (whole):** ~145–195 senior turns (recalibrated 2026-08-11; was 55–60). Above threshold → sub-split below per §1b.

##### WP-M0-17a1 — Instruction selection core

- **Objective:** implement IR → x86-64 instruction selection core per spec §14.1(7)/§14.3: deterministic output ordering, register-usage determinism.
- **Scope:** instruction selection; register-usage determinism; deterministic output ordering.
- **Exclusions:** x86-64+SSE2 coverage/constraint checks (17a2); frame layout/regalloc (17b); trap branches (17c); COFF emission (WP-M0-18).
- **Dependencies / inputs:** WP-M0-16 IR contract/impl; spec §14.1(7), §14.3.
- **Expected artifacts:** `bootstrap/src/backend/isel_core.*`, `bootstrap/build/backend_a1.txt`, codegen unit tests, assembly dump tests.
- **Sizing estimate:** 70–95 senior turns. Basis: ~1,200–1,600 LOC incl. tests, ~7 test functions, ~5 normative rules, moderate-high complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Instruction selection is deterministic: identical IR → identical assembly bytes.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** instruction-selection gaps. **Escalation:** backend constraint → Planner; ABI question → Main Designer.

##### WP-M0-17a2 — x86-64 instruction coverage and constraints

- **Objective:** implement full x86-64+SSE2 instruction coverage and backend constraint enforcement per spec §14.3.
- **Scope:** instruction coverage (baseline ≤ x86-64 + SSE2, no AVX2); backend constraint violations (`AIC-B0601`).
- **Exclusions:** selection core/order (17a1); frame layout/regalloc (17b); trap branches (17c); COFF emission (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17a1; spec §14.1(7), §14.3.
- **Expected artifacts:** `bootstrap/src/backend/isel_x64.*`, `bootstrap/build/backend_a2.txt`, assembly dump tests.
- **Sizing estimate:** 75–100 senior turns. Basis: ~1,300–1,700 LOC incl. tests, ~7 test functions, ~4 normative rules, moderate-high complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Generated instruction set uses only x86-64 + SSE2; no AVX2/host-specific instructions required.
  2. Backend constraint violations (`AIC-B0601`) enforced.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** instruction-set gaps. **Escalation:** backend constraint → Planner; ABI question → Main Designer.

---

#### WP-M0-17b — Frame layout, register allocation, and calls (sub-split: WP-M0-17b1..17b2)

- **Sizing estimate (whole):** ~140–190 senior turns (recalibrated 2026-08-11; was 55–60). Above threshold → sub-split below per §1b.

##### WP-M0-17b1 — Frame layout and prologue/epilogue

- **Objective:** implement stack layout and function prologue/epilogue per spec §15.7 (Microsoft x64 convention).
- **Scope:** frame layout; prologue/epilogue; `main` entry setup; noreturn handling.
- **Exclusions:** register allocation/call emission (17b2); instruction selection (17a); trap branches (17c); COFF (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17a; spec §14.3, §15.5, §15.7.
- **Expected artifacts:** `bootstrap/src/backend/frame.*`, `bootstrap/build/backend_b1.txt`, frame tests.
- **Sizing estimate:** 70–95 senior turns. Basis: ~1,200–1,600 LOC incl. tests, ~7 test functions, ~5 normative rules, moderate-high complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Prologue/epilogue and `main` entry setup correct; noreturn handled without corrupting the frame.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** frame layout errors. **Escalation:** ABI question → Main Designer.

##### WP-M0-17b2 — Register allocation and call emission

- **Objective:** implement simple/deterministic register allocation and call emission per spec §15.7.
- **Scope:** register allocation (simple/deterministic); call emission (RCX/RDX/R8/R9, shadow space, 16-byte alignment, RAX return).
- **Exclusions:** frame layout (17b1); instruction selection (17a); trap branches (17c); COFF (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17b1; spec §14.3, §15.7.
- **Expected artifacts:** `bootstrap/src/backend/call.*`, `bootstrap/build/backend_b2.txt`, calling-convention tests.
- **Sizing estimate:** 70–95 senior turns. Basis: ~1,200–1,600 LOC incl. tests, ~7 test functions, ~5 normative rules, moderate-high complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Runtime calls follow the §15.7 convention (RCX/RDX/R8/R9, shadow space, 16-byte alignment, RAX return).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** calling-convention errors. **Escalation:** ABI question → Main Designer.

---

#### WP-M0-17c — Trap branches and checked-operation emission (sub-split: WP-M0-17c1..17c2)

- **Sizing estimate (whole):** ~130–180 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-17c1 — Trap branch emission

- **Objective:** implement deterministic trap branches for runtime-failable checked operations per spec §14.3/§15.5 with stable codes and source spans.
- **Scope:** trap branch emission (`AIC-R0801..R0816` per operation); stable codes and source spans.
- **Exclusions:** checked-op emission details (17c2); instruction selection (17a); frame/regalloc (17b); COFF (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17a/b; spec §14.3, §15.5, §15.7.
- **Expected artifacts:** `bootstrap/src/backend/trap_branch.*`, `bootstrap/build/backend_c1.txt`, trap-branch unit tests.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Every runtime-failable checked operation emits a deterministic trap branch with the correct `AIC-Rxxxx` code and source span.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** trap-branch determinism. **Escalation:** backend constraint → Planner.

##### WP-M0-17c2 — Checked-operation emission with span/cause preservation

- **Objective:** implement checked-operation trap emission with span/cause preservation on trap records.
- **Scope:** checked-op emission; span/cause preservation on trap records.
- **Exclusions:** trap branch structure (17c1); instruction selection (17a); frame/regalloc (17b); COFF (WP-M0-18).
- **Dependencies / inputs:** WP-M0-17c1; spec §14.3, §15.5.
- **Expected artifacts:** `bootstrap/src/backend/trap_checked.*`, `bootstrap/build/backend_c2.txt`, checked-op emission tests.
- **Sizing estimate:** 65–90 senior turns. Basis: ~1,100–1,500 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Checked operations emit trap records preserving source spans and causal chains.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** span drift on trap records. **Escalation:** backend constraint → Planner.

---

### WP-M0-18 — COFF object emission (split: WP-M0-18a..18b; each sub-split)

- **Sizing estimate (whole):** ~235–335 senior turns (recalibrated 2026-08-11; was 140–180). Above threshold → split below per §1b. Basis: section/symbol/relocation tables + byte-determinism + link verification, §14.2/§14.3 rule surface; 18a/18b each sub-split into two cards.

#### WP-M0-18a — COFF writer core (sub-split: WP-M0-18a1..18a2)

- **Sizing estimate (whole):** ~120–170 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-18a1 — COFF tables and canonical ordering

- **Objective:** implement deterministic COFF object emission core per spec §14.2/§14.3: section/symbol/relocation tables, canonical record/section order.
- **Scope:** COFF writer; section/symbol/relocation tables; canonical ordering; object emission of the backend's sections.
- **Exclusions:** byte-determinism/timestamps/paths (18a2); link verification (18b); codegen (WP-M0-17); PE/linking (WP-M0-19).
- **Dependencies / inputs:** WP-M0-17 backend output contract; spec §14.2, §14.3.
- **Expected artifacts:** `bootstrap/src/coff/coff_sections.*`, `bootstrap/build/coff_a1.txt`, object inspection tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Section/symbol/relocation tables emitted in canonical order per §14.2/§14.3.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** COFF field ordering. **Escalation:** COFF detail → Planner.

##### WP-M0-18a2 — COFF byte-determinism

- **Objective:** implement byte-determinism for COFF emission per spec §14.3: zero compiler-controlled timestamps, repository-relative canonically separated paths, no random/host identifiers.
- **Scope:** determinism machinery; timestamps/paths; byte-level determinism tests.
- **Exclusions:** tables/canonical order (18a1); link verification (18b); codegen (WP-M0-17).
- **Dependencies / inputs:** WP-M0-18a1; spec §14.2, §14.3.
- **Expected artifacts:** `bootstrap/src/coff/coff_determinism.*`, `bootstrap/build/coff_a2.txt`, byte-level determinism tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Identical inputs → byte-identical COFF objects; zero timestamps, canonical order, relative paths; no host/build-machine identity.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** COFF field nondeterminism. **Escalation:** COFF detail → Planner.

---

#### WP-M0-18b — COFF verification and linker smoke (sub-split: WP-M0-18b1..18b2)

- **Sizing estimate (whole):** ~110–160 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-18b1 — Link smoke and object inspection

- **Objective:** verify emitted objects link with accepted external linkers (`link.exe` / `lld-link`) and are readable by inspection tools (dumpbin/llvm-objdump).
- **Scope:** link smoke tests; object inspection tests.
- **Exclusions:** determinism machinery (18b2); COFF writer core (18a); PE/linking (WP-M0-19).
- **Dependencies / inputs:** WP-M0-18a; baseline tooling (dumpbin on PATH, llvm-objdump off PATH).
- **Expected artifacts:** `bootstrap/src/coff/coff_verify.*`, `bootstrap/build/coff_b1.txt`, link/inspection verification tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~5 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes (MSVC and/or Clang/LLVM for verification).
- **Acceptance criteria:**
  1. Objects link with both `link.exe` (via initialized environment) and `lld-link`; inspection tools read them cleanly.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** tool-order canonicalization. **Escalation:** linker incompatibility → Main Designer/Researcher.

##### WP-M0-18b2 — Determinism machinery maintenance

- **Objective:** maintain the byte-determinism machinery and tool-order canonicalization for the COFF pipeline.
- **Scope:** determinism verification; tool-order canonicalization; regression tests.
- **Exclusions:** link smoke (18b1); COFF writer core (18a); PE/linking (WP-M0-19).
- **Dependencies / inputs:** WP-M0-18a, WP-M0-18b1.
- **Expected artifacts:** `bootstrap/src/coff/coff_detmach.*`, `bootstrap/build/coff_b2.txt`, determinism regression tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~5 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Determinism machinery maintained: repeated builds byte-identical; tool order canonical.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** tool-order drift. **Escalation:** linker nondeterminism → Main Designer.

---

### WP-M0-19 — Build driver, manifest, and link integration (split: WP-M0-19a..19c; each sub-split)

- **Sizing estimate (whole):** ~355–495 senior turns (recalibrated 2026-08-11; was 190–240). Above threshold → split below per §1b. Basis: pipeline orchestration + manifest emission + linker integration, §14.1/§14.4/§16.3 rule surface; 19a/19b/19c each sub-split into two cards.

#### WP-M0-19a — Pipeline orchestration and CLI (sub-split: WP-M0-19a1..19a2)

- **Sizing estimate (whole):** ~120–170 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-19a1 — Pipeline orchestration

- **Objective:** implement `main()` and pipeline orchestration (load→lex→parse→name→types→const→sema→ir→backend→coff→link) per spec §14.1.
- **Scope:** driver `main()`; pipeline driver; exit codes.
- **Exclusions:** CLI/options (19a2); build manifest emission (19b); linker invocation (19c); language semantics.
- **Dependencies / inputs:** WP-M0-06..18; spec §14.1.
- **Expected artifacts:** `bootstrap/src/driver/main.*`, `bootstrap/build/driver_a1.txt`, pipeline unit tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~5 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Pipeline orchestration runs all stages in order; invalid programs produce JSONL diagnostics and non-zero exit.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** exit-code ambiguity. **Escalation:** manifest/contract conflict → Planner.

##### WP-M0-19a2 — Deterministic CLI and options

- **Objective:** implement deterministic CLI/options per spec §14.1: deterministic option parsing, diagnostics to stderr.
- **Scope:** deterministic option parsing; sorted options; diagnostics to stderr.
- **Exclusions:** pipeline (19a1); build manifest emission (19b); linker invocation (19c).
- **Dependencies / inputs:** WP-M0-19a1; spec §14.1.
- **Expected artifacts:** `bootstrap/src/driver/cli.*`, `bootstrap/build/driver_a2.txt`, CLI option tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Option parsing is deterministic (sorted options).
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** option drift. **Escalation:** manifest/contract conflict → Planner.

---

#### WP-M0-19b — Build manifest emission (sub-split: WP-M0-19b1..19b2)

- **Sizing estimate (whole):** ~120–170 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-19b1 — Manifest writer

- **Objective:** implement build-manifest emission per spec §14.4: schema version, project root, entry module, module list, AI-Co language/spec version field, sorted options, linker flag set, relative artifact paths, diagnostic summary, exit status.
- **Scope:** manifest writer; §14.4 fields; stage-invariant version.
- **Exclusions:** hashed-artifact set/self-hash exclusion (19b2); pipeline (19a); linker invocation (19c).
- **Dependencies / inputs:** WP-M0-19a; spec §14.4.
- **Expected artifacts:** `bootstrap/src/driver/manifest_writer.*`, `bootstrap/build/driver_b1.txt`, manifest fixture tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~5 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Manifest fields match §14.4 exactly: schema version, project root, module list, stage-invariant version, sorted options, linker flag set, relative artifact paths, diagnostic summary, exit status.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** manifest drift. **Escalation:** manifest/contract conflict → Planner.

##### WP-M0-19b2 — Hashed artifact set and self-hash exclusion

- **Objective:** implement the hashed-artifact set and self-hash exclusion per spec §14.4 (FIND-G2-02/03).
- **Scope:** SHA-256 of each artifact excluding the manifest; self-hash exclusion; fixture tests.
- **Exclusions:** manifest writer fields (19b1); pipeline (19a); linker invocation (19c).
- **Dependencies / inputs:** WP-M0-19b1; spec §14.4.
- **Expected artifacts:** `bootstrap/src/driver/manifest_hash.*`, `bootstrap/build/driver_b2.txt`, hash/self-hash fixture tests.
- **Sizing estimate:** 60–85 senior turns. Basis: ~1,000–1,400 LOC incl. tests, ~6 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes.
- **Acceptance criteria:**
  1. Artifact hashes correct; manifest never self-hashes; identical relative output paths across stage builds.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** self-hash regression. **Escalation:** manifest/contract conflict → Planner.

---

#### WP-M0-19c — External linker integration and end-to-end compile (sub-split: WP-M0-19c1..19c2)

- **Sizing estimate (whole):** ~110–160 senior turns (recalibrated 2026-08-11; was 50–60). Above threshold → sub-split below per §1b.

##### WP-M0-19c1 — External linker invocation

- **Objective:** implement external linker invocation per spec §16.3 (never bare `link`; explicit path/initialized env) and link-failure reporting.
- **Scope:** linker invocation; link-failure reporting (`AIC-O0702`); linker identity/version recorded only in comparison evidence.
- **Exclusions:** end-to-end compile tests (19c2); pipeline (19a); manifest (19b); M2 project-owned linker (out of scope).
- **Dependencies / inputs:** WP-M0-19a/b; spec §14.1, §16.3; baseline linker paths.
- **Expected artifacts:** `bootstrap/src/driver/link_invoke.*`, `bootstrap/build/driver_c1.txt`, linker-invocation tests.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~5 test functions, ~4 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes (MSVC and Clang; both linkers).
- **Acceptance criteria:**
  1. Never invokes bare `link`; linker identity/version recorded only in comparison evidence (spec §16.3), never in the manifest.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** linker-mode mismatch. **Escalation:** linker nondeterminism → Main Designer.

##### WP-M0-19c2 — End-to-end compile

- **Objective:** implement end-to-end compile tests on corpus programs per spec §16.3 and entry validation (`AIC-E0418`).
- **Scope:** end-to-end compile tests; entry validation.
- **Exclusions:** linker invocation mechanics (19c1); pipeline (19a); manifest (19b).
- **Dependencies / inputs:** WP-M0-19c1; spec §14.1, §16.3.
- **Expected artifacts:** `bootstrap/src/driver/link_e2e.*`, `bootstrap/build/driver_c2.txt`, end-to-end compile tests on corpus programs.
- **Sizing estimate:** 55–80 senior turns. Basis: ~900–1,300 LOC incl. tests, ~5 test functions, ~3 normative rules, moderate complexity. Within threshold (≈105).
- **Capability:** `senior_specialist`. **Host toolchain required:** yes (MSVC and Clang; both linkers).
- **Acceptance criteria:**
  1. End-to-end compile of valid programs produces COFF + linked PE via the accepted linker modes; invalid programs produce JSONL diagnostics and non-zero exit.
- **Verification / review class:** self-review + Reviewer independent review (class: Reviewer).
- **Risks:** fixture drift. **Escalation:** linker nondeterminism → Main Designer.

---

### WP-M0-20 — Stage-0 integration verification

- **Sizing estimate:** 40–55 senior turns (recalibrated 2026-08-11; unchanged) — within threshold, no split. Basis: execution/evidence work, no new source; ~4 normative checks (both compilers, suite pass, determinism, manifest conformance); evidence work is not covered by the compiler-core multiplier.
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

**Recalibration supersession (binding, from the 2026-08-11 amendment):** before dispatching any card from WP-M0-12b onward, the Coordinator must (1) execute or decline the recommended senior-lane budget change (§1b: `hermes -p senior_specialist config set agent.max_turns 150`), recording it on the board and verifying it takes effect — if declined, return over-threshold cards to the Planner for re-splitting rather than dispatching them over-threshold; and (2) supersede the first-level split cards (WP-M0-12b, 13a..13d, 14a..14b, 15a, 15c, 16b..16c, 17a..17c, 18a..18b, 19a..19c) with the §3 second-level sub-split cards (12b1/12b2 … 19c1/19c2), each with its own owned area (§2), acceptance criteria, review class, serial edges, and recalibrated §1b sizing estimate. Dispatch sub-split siblings strictly serially (e.g., 12b1 → 12b2, then 13a1 → 13a2 → 13b1 → 13b2 → …); do not start a split group until the previous group's cards are verified. WP-M0-15b, 16a, and 20 are dispatched as single cards with their revised estimates.
