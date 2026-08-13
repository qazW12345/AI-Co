# Planner Work-Package Record — WP-M0-S2a-IR SPLIT: bounded serial sub-packages after 2 failed runs

**Status:** Implementation-ready revised package (Planner split decision 2026-08-13, routing task t_35739131; supersedes execution of the single-package card t_afb33982)
**Owner:** Planner
**Date:** 2026-08-13
**Supersedes for execution:** `docs/planning/AI-CO-PLANNER-S2A-IR-PACKAGE-2026-08-13.md` (commit 113e6db) — the original single-package sizing (§6) and structural-wiring (§11) are superseded by this record; the original record remains the historical baseline.
**Routing source:** Coordinator triage t_4aa1c339 (OM §6.1 watchdog escalation; t_afb33982 blocked after 2 failed runs). Precedent: t_befbdc86 (WP-M0-16c1 split into bounded sub-packages after 2 failed runs, commit 6b771c1).
**Decision authority:** Planner (work-package decomposition within accepted direction; PLANNER_PROFILE independent decisions; precedent t_befbdc86). No new architecture decision, no spec/ADR/contract/governance change, no Human Sponsor gate. Routing decision authority: Coordinator (t_4aa1c339). If the Main Designer disagrees it may reject or revise this split (manifest is Planner-owned).

---

## 1. Failure basis (durable evidence, Coordinator-verified 2026-08-13 ~19:50)

- **Run 882** (18:12–18:50): `timed_out`, "Iteration budget exhausted (150/150)". Original card sizing estimate 45–70 senior turns (1,300–2,200 LOC) at 25–30 LOC/turn; 150 iterations exhausted with the package **incomplete** → estimate materially understated.
- **Run 883** (18:51–19:38): `crashed`, "pid 21412 not alive". Crash-pair → OM §6.1 no auto-continue; Coordinator decision required.
- **Real progress exists (W4 provenance):** `bootstrap/src/ir/ir_s2a_features_test.c` — untracked, 2,015 lines / 89,677 B, mtime 19:35 (inside run 883 window; ownership t_afb33982). MSVC build succeeded (gitignored `bootstrap/stage0/msvc-s2a-ir/`); exe ran: `ir_s2a_features_test: 63 checks, 0 failures` exit 0.
- **Coverage present:** Feature 1 compound assignment — cases C1d/C1a/C1b/C1c incl. the three failed-review shapes (`*gp += bump()`, `a[giu] += bump()`, `*getp() += bump()`); Feature 2 repetition `[e; N]` incl. N==0 — C2a/C2b; Feature 3 cast/wrap — C3a. Each case has a provenance comment block (hand-computed oracle, spans from loader position_at math, construction order from contract §6.1).
- **Coverage MISSING:** Feature 4 (pointer arithmetic / value categories); Feature 5 (struct/array literal lowering); Feature 6 (trap obligations); composition subset; `bootstrap/build/ir_s2a_features.txt`; local commit; completion report; review-required block.
- **Scratch helpers** `s2a_oracle.py` / `s2a_render.py` exist under gitignored `bootstrap/stage0/` (outside owned files; not committed; not a repo-tree scope breach per Coordinator).

**Classification (per OM §13 / §8):** execution failure (run 882: sizing estimate 45–70 turns exceeded ~2–3× with scope incomplete) + environment/host failure (run 883 crash). NOT a review request (no review-required block posted) and NOT a human decision. Materially different strategy required: split the package (precedent t_befbdc86), not a blind retry with more budget.

## 2. Calibration and split rationale

- **Observed throughput:** 2,015 LOC delivered / 150 iterations ≈ **13.4 LOC/turn** for oracle-heavy work (hand-computed dump strings dominate). This matches the manifest §1b **high-complexity** rate (13 LOC/turn), not the 25–30 calibration used in the original estimate. The original estimate used the moderate-rate calibration on a high-rate workload → the sizing failure.
- **Revised rate for all remaining sub-cards: 13 LOC/turn**, with per-card targets ≤ 55 turns (well under the 150-turn lane budget and the §1b ~105-turn threshold with margin).
- **Remaining scope (LOC estimate at delivered case density):** Feature 4 ≈ 2–3 cases × ~230 LOC/case ≈ 460–690; Feature 5 ≈ 2 cases ≈ 460; Feature 6 ≈ 2–3 cases ≈ 460–690; composition ≈ 2–3 cases ≈ 300–450; build fragment ≈ 60–100. Features 1–3 already delivered (2,015 LOC) — finalization is verification/cleanup, not authoring.
- **Split decision:** 5 serial sub-cards (A, B1, B2, B3, C), each 20–55 turns, strictly serial per manifest §1 rule 1. Same pattern as t_befbdc86. One file (`ir_s2a_features_test.c`) grows through the chain — each card extends the owned program and commits its increment; no restart from scratch.

## 3. Sub-package table (ready for card creation — created by Planner per routing task action 3)

| id | title | owned files (ONLY) | dep | sizing (13 LOC/turn) | AC core |
|---|---|---|---|---|---|
| S2a-IR-A | Finalize features 1-3 partial artifact + build fragment (adopts t_afb33982 partial) | `bootstrap/src/ir/ir_s2a_features_test.c` (adopt W4 partial; audit oracle discipline; clean dev markers; commit), `bootstrap/build/ir_s2a_features.txt` (author) | 16c2 done (t_07aacd82) + tree quiescent | 20–30 turns | Features 1-3 verified on both toolchains /W4; fragment authored; oracle provenance audited; debug markers cleaned; no case removal |
| S2a-IR-B1 | Feature 4: pointer arithmetic / value categories | `bootstrap/src/ir/ir_s2a_features_test.c` (extend), `bootstrap/build/ir_s2a_features.txt` (update expected output) | S2a-IR-A | 35–55 turns | PTR_ADD/SUB/DIFF scaled offsets + AIC-R0816/R0810; §5.4 value categories in dumps; boundary values mandatory; regression green |
| S2a-IR-B2 | Feature 5: struct/array literal lowering | `bootstrap/src/ir/ir_s2a_features_test.c` (extend), `bootstrap/build/ir_s2a_features.txt` (update expected output) | S2a-IR-B1 | 30–45 turns | IR_ZERO + field/element stores; padding zeroing; composites address-resident; regression green |
| S2a-IR-B3 | Feature 6: trap obligations | `bootstrap/src/ir/ir_s2a_features_test.c` (extend), `bootstrap/build/ir_s2a_features.txt` (update expected output) | S2a-IR-B2 | 35–55 turns | Every lowered node carries AIC-R0802..R0816 enforcement; boundary values; regression green |
| S2a-IR-C | Composition subset (2-3 programs combining 2-3 features) | `bootstrap/src/ir/ir_s2a_features_test.c` (extend), `bootstrap/build/ir_s2a_features.txt` (update expected output) | S2a-IR-B3 | 25–40 turns | 2-3 composition cases with hand-computed oracles; interaction coverage; full-program regression green; **C's review PASS is the structural gate for 17a1 + push wave** |

Serial chain: **A → B1 → B2 → B3 → C**. Each card's body carries the parseable **Owned files (ONLY)** list (watchdog W3 form), §1b Sizing, review-required completion protocol (rule 8), Reviewer review class, escalation (IR boundary → Main Designer), rule 2 (local commit on main, no push).

## 4. Partial artifact preservation (routing requirement 4 — binding)

- **No destructive cleanup:** the untracked `bootstrap/src/ir/ir_s2a_features_test.c` stays exactly where it is in the shared tree; the tree remains quiescent until S2a-IR-A dispatches.
- **Structural ownership transfer (W2/W4):** ownership of the untracked file remains t_afb33982 (W4 provenance) until S2a-IR-A claims it structurally via its **Owned files (ONLY)** list in the card body. S2a-IR-A adopts, audits, finalizes, and commits it. No card before A may touch it; the Coordinator must NOT sweep it into another commit or delete it.
- **Scratch helpers** `s2a_oracle.py` / `s2a_render.py` (gitignored `bootstrap/stage0/`) are NOT owned files and MUST NOT be committed. A may decide to keep or remove them as part of finalization; no other actor touches them before A runs.
- **Gitignored build outputs** (`bootstrap/stage0/msvc-s2a-ir/`, `clang-s2a-ir/`) are outside the owned file set; they may stay (build evidence) or be regenerated by each card's verification runs.

## 5. Gate chain wiring (routing requirement 2 — binding)

- **Old edges to remove (Coordinator action — Planner tool surface is additive-only):**
  - `t_afb33982 → t_e66fb023` (WP-M0-17a1) — MUST be removed. A blocked parent is non-terminal; `recompute_ready` requires **all** parents `done`/`archived`, so leaving t_afb33982 as a parent deadlocks 17a1 promotion forever.
  - `t_afb33982 → t_bc4c6088` (16-section push wave) — MUST be removed, same deadlock.
- **New edges (Planner adds now via `kanban_link`):**
  - `S2a-IR-C → t_e66fb023` — 17a1 promotes only when 16c2 done AND S2a-IR-C (last sub-card, review-PASSed) done → "only after S2a-IR passes review".
  - `S2a-IR-C → t_bc4c6088` — push wave promotes only after 16c2 done AND S2a-IR-C done.
- **Fallback if edges cannot be removed (precedent t_befbdc86):** create replacement gate cards parented on S2a-IR-C, supersede t_e66fb023 (blocked origin) and re-point its child t_d6b8e5b3 (17a2); same replacement pattern for t_bc4c6088.
- **t_afb33982 stays blocked** as superseded origin (OM §5, same pattern as t_804b7d94/t_eeb8e27b); no retry of the old card.

## 6. Coordinator actions required (manifest §5 binding note)

1. **Unlink old edges** `t_afb33982 → t_e66fb023` and `t_afb33982 → t_bc4c6088` (or apply the replacement-card fallback in §5 if the runtime cannot unlink).
2. **Verify final gate edges:** t_e66fb023 parents = [t_07aacd82, S2a-IR-C]; t_bc4c6088 parents = [t_07aacd82, S2a-IR-C].
3. **Supersede t_afb33982** — stays blocked as superseded origin; record on the board; no retry.
4. **Dispatch serially A → B1 → B2 → B3 → C** (parent edges enforce); do NOT parallelize within the chain (manifest §1 rule 1).
5. **W2a check before A dispatch:** shared tree quiescent (no running pair on the AI-Co dir; t_afb33982 blocked, not running); untracked file still present for adoption.
6. Do NOT create duplicate implementation cards — the Planner has created them per routing task action 3 (ids in the handoff comment).

## 7. Confidence

- **Requirements confidence: High** — design record §S2a-IR and original package record are explicit; split preserves scope, oracle discipline, boundary-value mandate, and completion protocol unchanged.
- **Architecture confidence: High** — no new architecture surface (same IR boundary, same oracle machinery, same owned file); sub-cards are pure work decomposition.
- **Execution-planning confidence: Medium (corrected)** — sizing is now calibrated from observed throughput (13 LOC/turn) with each sub-card ≤ 55 turns vs the 150-turn lane budget; S2a-IR-A is intentionally small (adoption + finalization) so the rate is re-validated on real work before B-cards dispatch; if A's actuals show a materially different rate, B-cards return to the Planner for re-sizing (downstream gap rule, milestone plan §7).

## 8. Evidence

- t_35739131 routing body (Coordinator triage t_4aa1c339; W4 evidence; classification; action items).
- t_afb33982 run history (runs 882/883), watchdog comments, Coordinator comment 1489.
- Partial artifact `bootstrap/src/ir/ir_s2a_features_test.c` (2,015 lines; 7 cases C1d/C1a/C1b/C1c/C2a/C2b/C3a; provenance comments; build+run evidence at gitignored `bootstrap/stage0/msvc-s2a-ir/`).
- Precedent t_befbdc86 (commit 6b771c1) — split pattern, W3 parseable forms, parent re-pointing fallback.
- `kanban_review_router.py` `owned_paths_from_body` (lines 683-752) — accepted W3 forms verified.
- Manifest §1 rule 1/4/6/8/9, §1b (2026-08-11/12 calibration anchors; high-complexity rate 13 LOC/turn), §3 package-entry format, §5.
- Design record `docs/decisions/AI-CO-POST-MILESTONE-TESTING-STRATEGY-2026-08-12.md` §S2a-IR (commit 2136e3a).
- Original package record `docs/planning/AI-CO-PLANNER-S2A-IR-PACKAGE-2026-08-13.md` (commit 113e6db) — superseded for execution by this record.
