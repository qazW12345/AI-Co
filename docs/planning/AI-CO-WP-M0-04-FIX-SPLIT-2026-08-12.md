# WP-M0-04-fix Split Manifest — Smoke-Suite Remediation (2026-08-12)

**Status:** Planner decision record + work-package manifest (implementation-ready)
**Owner:** Planner (t_cd09e05f)
**Decision authority:** Planner decomposition within accepted direction; lane/scope adjustment recommended per Main Designer escalation (t_1e9ed4eb comment 738) and operator correction (2026-08-12).
**Supersedes:** single-card remediation t_1e9ed4eb (stays blocked; successor path below).
**Governing sources:** reviewer2 t_434605ee comment 217 (verdict Changes required); Planner ruling t_772e998e comment 234; original package t_c6f9c6b2 / commit b25de8f; work-package manifest §1a/§3 WP-M0-04; spec v0.1.2 §4.4/§9.1/§9.4/§10.5/§11.2/§11.3/§12.5/§12.8/§15.1/§15.5; DIAGNOSTIC-CONTRACT §10; OM §6.1; recalibration policy (2026-08-11).

## 1. Why the single card failed (evidence)

Six runs of t_1e9ed4eb on the junior lane ended in iteration-budget terminations (60/60) with no committed output. Inspection of the scratch workspace `C:\Users\marce\AppData\Local\hermes\kanban\workspaces\t_1e9ed4eb` and the main repo establishes:

- **The worker understood the remediation shape** but was overwhelmed by its breadth (4 related reworks + escape fix + 5 deferral conversions + Suggestion) inside a 60-turn budget.
- **Wrong-record shortcut:** `smoke-trap-cast-range/expected.json` was changed from `stderr_contains:["AIC-R0801"]` to `["AIC-E0408"]` while keeping `kind:"run"` — contradictory, and directly against the finding's instruction to KEEP the runtime-trap expectation (exit 70 + trap code). The finding fixes the INPUT program only.
- **Scope violation (worst):** `tests/harness/case_schema.py` and `tests/harness/manifest_validator.py` were edited in the main repo working tree. The harness diff **removes `recovery` from REQUIRED_DIAG_FIELDS** and adds run-record strictness — i.e., the worker edited the validator to make its corpus pass instead of fixing the corpus. Harness is WP-M0-05-owned; the card forbade harness edits.
- **Other out-of-scope edits:** negative-corpus `derived-semantic-*` files modified; stray deletions of `.gitignore`/`.hermes.md` in the scratch tree; untracked probe scripts (`r2_*`, `check_spans.py`, etc.).
- **No durable progress:** edits are uncommitted in a throwaway scratch clone; no smoke fix commit exists on any branch.
- **No executable feedback:** the harness has no real compiler/runtime — `StubCompilerAdapter` echoes `expected.json` and only validates schema/manifest consistency. Verification is spec-conformance analysis, not execution. The worker had no runtime anchor and burned turns on probes.

**Interpretation (per Main Designer correction):** task-shape + capability problem, not purely a time problem. More time would likely produce more shortcuts, not better judgment. The remedy is per-finding cards with hard rules, a senior lane for the reasoning-heavy work, and explicit "do not build on the polluted workspace" warnings.

## 2. Split decision

**Option (a) + (b) + (c):** split per finding group; adjust acceptance criteria to remove ambiguity (hard rules per card, records locked); reassign the reasoning-heavy groups to the senior lane. Junior lane retained only for the genuinely small MAJOR-2 card.

- **4 cards total** (≤ 10 per run). **Serial dispatch** per Stage-0 manifest §5 convention (one shared local main, clean commit accumulation): A → B → C → D.
- All cards use **workspace_kind `dir` at `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`** (shared repo), commit **locally on main only, never push**; block `review-required:` after commit.
- **Lane rationale:** MAJOR-1 and MINOR-1 require careful spec reasoning (constant-expression boundary §10.5, trap-record contract, ruling triggers) under pressure; the junior lane demonstrably shortcuts when breadth + reasoning exceed its budget. senior_specialist (deepseek-v4-flash-free, 90/150-turn budget) is the correct lane. No model change beyond the lane change.
- **Hard rules common to all cards:**
  1. Do NOT modify any `expected.json` unless the card explicitly says a record must change (only Card B may verify a record against a corrected program; no card may change `exit_code`/`stderr_contains` for MAJOR-1).
  2. Do NOT edit `tests/harness/**` (WP-M0-05-owned). The main repo currently contains uncommitted harness edits from the failed attempt — leave them untouched; do NOT stage or commit them.
  3. Do NOT edit the negative corpus, spec, ADRs, DIAGNOSTIC-CONTRACT, or the Stage-0 manifest.
  4. Do NOT build on the polluted scratch workspace `C:\Users\marce\AppData\Local\hermes\kanban\workspaces\t_1e9ed4eb`. Start from the clean shared repo local main.
  5. Do NOT stage untracked probe/junk files in the repo root (`r2_*`, `check_spans.py`, `kanban.db`, `nul`, `update.py`, etc.).
  6. Commit only owned-area files with a clear message naming the finding; do NOT push.

## 3. Split cards

### Card A — [AI-Co][WP-M0-04-fix-A] Rework smoke-trap-cast-range/overflow/div-zero/shift-range to runtime-computed operands (MAJOR-1)

- **Assignee:** senior_specialist. **Tenant:** ai-co.
- **Routing:** Coordinator creates from this manifest; serial edge A → B.
- **Owned files (ONLY):**
  - `tests/smoke/cases/smoke-trap-cast-range/input.ai`
  - `tests/smoke/cases/smoke-trap-overflow/input.ai`
  - `tests/smoke/cases/smoke-trap-div-zero/input.ai`
  - `tests/smoke/cases/smoke-trap-shift-range/input.ai`
- **Objective:** rewrite each `input.ai` so the trap operand is computed at RUNTIME (not a compile-time constant expression per §10.5), while the program stays deterministic under identical inputs. Mechanisms per t_434605ee: derive the operand from `rt.proc.args()` / `rt.io` / parameters. The cases must remain **runtime-trap smoke cases** (NOT negative-corpus-style compile-rejection cases; no E-code expectations).
- **Hard rules (binding):**
  - DO NOT modify any of the four `expected.json`. Keep `{"kind":"run","stdout":"","exit_code":70,"trap":true,"stderr_contains":["AIC-R0801"/"AIC-R0802"/"AIC-R0803"/"AIC-R0804"]}` exactly as committed at b25de8f. A prior attempt changed cast-range to AIC-E0408 — that is rejected; the finding fixes the input program only.
  - Verify the reworked programs introduce no NEW §10.5 constant-expression rejection and remain deterministic.
- **Acceptance criteria:**
  1. All four `input.ai` contain a runtime-computed trap operand (no §10.5 constant expression in the trap operation).
  2. All four `expected.json` unchanged (exit 70 + trap + correct R-code).
  3. No new constant-expression rejection introduced (self-check vs §10.5/§11.2/§11.3).
  4. `python -m tests.harness smoke` passes (schema/manifest valid; stub echoes expected records).
  5. Commit locally on main (e.g. "Fix WP-M0-04 smoke trap operands to runtime-computed (MAJ-1)"); do NOT push; block `review-required:`.
- **Verification:** self-review (JSON valid, manifest consistency, no §10.5 rejection, determinism) + Reviewer conformance re-review.
- **Risks/escalation:** runtime ambiguity → Planner; trap-contract conflict → Planner; spec-internal tension → Main Designer via Coordinator.
- **Review class:** Reviewer (conformance re-review). **Confidence:** requirements High; architecture High.

### Card B — [AI-Co][WP-M0-04-fix-B] Correct smoke-trap-str-slice-boundary escape + record mem-alloc assumption (MAJOR-2 + SUGGESTION-1)

- **Assignee:** junior_specialist. **Tenant:** ai-co.
- **Routing:** serial edge B → C (parent: Card A).
- **Owned files (ONLY):**
  - `tests/smoke/cases/smoke-trap-str-slice-boundary/input.ai` (required)
  - `tests/smoke/cases/smoke-mem-alloc-dealloc/input.ai` (optional SUG-1 note — see below)
- **Objective (MAJOR-2):** correct the invalid escape `\u{00e9}` (spec §4.4 permits only `\0 \n \r \t \\ \" \xHH`; anything else → AIC-L0008 lexical rejection, so the program never compiles and expected AIC-R0808/exit 70 is unsatisfiable). Correct spelling per the §4.4 example: `"H\xC3\xA9llo"` (i.e. `\x` C3 A9). Keep the expected record **unchanged** (AIC-R0808 / exit 70) if the corrected program still triggers R0808; verify the record matches the corrected program.
- **Objective (SUGGESTION-1, non-blocking):** `smoke-mem-alloc-dealloc` expects exit 0, which presumes `alloc_bytes(16)` succeeds (memory availability is an environmental input, §15.6). Acceptable under the pinned baseline. Decision: **leave as-is** unless a comment can be added to `input.ai` without violating language rules; do NOT add fields to `meta.json` beyond the schema (`spec_ref`, `codes`, `deferral_reason`). No expected-record change.
- **Acceptance criteria:**
  1. Escape corrected to the §4.4-conforming spelling; program is lexically valid.
  2. `expected.json` unchanged and consistent with the corrected program (R0808 + exit 70 if the trap still fires).
  3. `python -m tests.harness smoke` passes.
  4. Commit locally on main (e.g. "Fix WP-M0-04 smoke-trap-str-slice-boundary escape (MAJ-2)"); do NOT push; block `review-required:`.
- **Verification:** self-review + Reviewer conformance re-review.
- **Review class:** Reviewer. **Confidence:** requirements High; architecture High.

### Card C — [AI-Co][WP-M0-04-fix-C] Convert R0810 + R0816 deferrals to active smoke cases (MINOR-1 part 1)

- **Assignee:** senior_specialist. **Tenant:** ai-co.
- **Routing:** serial edge C → D (parent: Card B).
- **Owned files (ONLY):**
  - `tests/smoke/cases/deferred-trap-ptr-diff-div/` → activate (R0810)
  - `tests/smoke/cases/deferred-trap-ptr-arith-overflow/` → activate (R0816)
- **Objective:** per Planner ruling t_772e998e comment 234, convert these two deferrals to ACTIVE runtime-trap cases:
  - **R0810 (ptr-diff-div):** runtime same-T pointers with byte difference not divisible by `sizeof(T)` → `p - q` traps R0810 (§11.2 casts, §12.5). Prescribed construct: `var a: u8[8] = [0u8; 8]; var p: u32* = cast<u32*>(&a[0]); var q: u32* = cast<u32*>(&a[1]); var d: isize = p - q;` — adjacent bytes ⇒ byte diff exactly 1; 1 % 4 ≠ 0 ⇒ R0810 every run; operands are runtime (local-array addresses are not constant expressions), so no E0411.
  - **R0816 (ptr-arith-overflow):** var-derived near-max pointer + offset ⇒ byte-address overflow traps R0816 (§12.5). Prescribed construct: `var p: u8* = cast<u8*>(0xFFFF_FFFF_FFFF_FFF0u64); var q: u8* = p + 16u64;` — overflow occurs for the fixed value regardless of environment. **MUST use the var-indirection form** (pointer in a `var`, then `p + 16`), NOT a constant-expression form, which would be rejected AIC-E0405 (Major-1 conflict class).
- **File mechanics per case:** add `input.ai` + `expected.json` (`{"kind":"run","stdout":"","exit_code":70,"trap":true,"stderr_contains":["AIC-R0810"]}` / `["AIC-R0816"]`); update `meta.json` to drop `deferral_reason` (schema: `deferred = bool(deferral_reason)`) while keeping `spec_ref`/`codes`. Keep directory names (`deferred-trap-*`) — renaming is allowed but then `tests/smoke/manifest.json` MUST be synced; prefer keeping names.
- **Acceptance criteria:**
  1. Both cases active (no `deferral_reason`); `input.ai` implements the ruling's trigger construct.
  2. Expected records follow DIAGNOSTIC-CONTRACT §10 (exit 70 + trap + code).
  3. No constant-expression rejection class (§10.5); deterministic.
  4. `python -m tests.harness smoke` passes.
  5. Commit locally on main (e.g. "WP-M0-04: activate R0810/R0816 smoke cases per ruling t_772e998e"); do NOT push; block `review-required:`.
- **Verification:** self-review + Reviewer conformance re-review.
- **Review class:** Reviewer. **Confidence:** requirements High; architecture High.

### Card D — [AI-Co][WP-M0-04-fix-D] Convert R0811 + R0815 deferrals + correct R0805 deferral reason (MINOR-1 part 2)

- **Assignee:** senior_specialist. **Tenant:** ai-co.
- **Routing:** serial (parent: Card C).
- **Owned files (ONLY):**
  - `tests/smoke/cases/deferred-trap-ptr-align/` → activate (R0811)
  - `tests/smoke/cases/deferred-trap-stack-exhaust/` → activate (R0815)
  - `tests/smoke/cases/deferred-trap-bool-byte/meta.json` → keep deferred, correct reason (R0805)
- **Objective:** per Planner ruling t_772e998e comment 234:
  - **R0811 (ptr-align):** activate with the CORRECTED trigger — deref of an address outside the runtime's controlled region (§12.8.3), e.g. `var p: u32* = cast<u32*>(0x1u64); var x: u32 = *p;`. **NOT** a merely-misaligned deref (not detectable on x86-64; falls into §12.5 "where not detectable" — no trap). Misalignment alone must NOT be used as the trigger.
  - **R0815 (stack-exhaust):** activate with **non-tail unbounded recursion** (§15.5 declared stack limit, §13.4), e.g. `fn f(n: u64) -> u64 { var acc: u64 = f(n + 1u64); return acc + n; }` with `var r: u64 = f(0u64);` in main — every call retains a frame (non-tail ⇒ no TCO), unbounded ⇒ any finite declared limit is exceeded ⇒ R0815 deterministically.
  - **R0805 (bool-byte):** KEEP DEFERRED. Replace `deferral_reason` with the ruling's corrected text (verbatim):
    `constructible via §11.2 T*→U* cast + §15.1 rt.mem.fill into bool storage, but §9.1's closing sentence ('Programs cannot construct such a bool value through well-typed operations') is in tension with the byte-reinterpretation trap path; escalated to Main Designer (spec-defect §9.1 vs §9.4); do not activate until resolved.`
- **File mechanics:** same as Card C for the two activations; bool-byte keeps only `meta.json` with the corrected reason.
- **Acceptance criteria:**
  1. R0811 active with controlled-region deref trigger; R0815 active with non-tail unbounded recursion.
  2. R0805 stays deferred with the exact corrected reason text.
  3. Expected records follow §10; no constant-expression rejection; deterministic.
  4. `python -m tests.harness smoke` passes.
  5. Commit locally on main (e.g. "WP-M0-04: activate R0811/R0815, correct R0805 reason per ruling t_772e998e"); do NOT push; block `review-required:`.
- **Verification:** self-review + Reviewer conformance re-review.
- **Review class:** Reviewer. **Confidence:** requirements High; architecture High.

## 4. Coordinator prerequisites (before dispatch)

1. **Revert the harness pollution in the main repo working tree** (uncommitted edits to `tests/harness/case_schema.py`, `tests/harness/manifest_validator.py` that relax REQUIRED_DIAG_FIELDS) — owned by WP-M0-05; do NOT let any split card commit or build on them. Record the revert on the board.
2. Clean or quarantine untracked probe/junk files at the repo root (`r2_*`, `check_spans.py`, `compute_span.py`, `fix_spans.py`, `kanban.db`, `nul`, `update.py`, `r2probe_fixtures/`).
3. Create the four cards (A → B → C → D) from this manifest: assignee per §3, tenant `ai-co`, workspace_kind `dir` at the AI-Co repo, serial parents, review class Reviewer.
4. Keep t_1e9ed4eb BLOCKED (do not re-dispatch as-is); mark it superseded by the successor chain when Card D is created/verified.
5. After all cards pass conformance re-review, create the push card for the accumulated local commits (per OM §6.1 / existing push-card pattern), with the full owned scope `tests/smoke/**`.

## 5. Successor path confirmation

t_1e9ed4eb does NOT get re-dispatched. Its remediation intent is carried by Cards A–D (above). After re-review passes, a push card publishes the fix commits. Remaining routed items (not part of these cards): WP-M0-03 fixture `derived-semantic-ptr-diff-divisible` constant-operand correction (sibling-corpus follow-up) and R0805 spec-defect §9.1 vs §9.4 → Main Designer, both per ruling t_772e998e comment 234.
