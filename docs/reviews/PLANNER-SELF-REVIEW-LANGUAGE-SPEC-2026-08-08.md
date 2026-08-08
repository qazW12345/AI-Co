# Planner Self-Review Report: AI-Co Language Specification v0.1.1 (ADR-003/ADR-004 Amendments)

**Reviewer:** Planner (self-review; required gate 1 per Spec §21)
**Date:** 2026-08-08
**Task:** `t_a5cbebd6` (Planner self-review of ADR-003 language-spec amendments)
**Artifacts reviewed:**
- `projects/AI-Co/spec/AI-CO-LANGUAGE-SPECIFICATION.md` (v0.1.1 Proposed, amended per ADR-004)
- `projects/AI-Co/docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md` (Superseded by ADR-004; historical evidence)
- `projects/AI-Co/docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (Accepted; governing)
- `projects/AI-Co/spec/DIAGNOSTIC-CONTRACT.md` (v0.1.1 Proposed; normative companion)
- `projects/AI-Co/spec/OPEN-QUESTIONS.md` (v0.1.1; resolution record)
- `projects/AI-Co/docs/reviews/ADR-003-LANGUAGE-SPEC-REVIEW-2026-08-08.md` (independent Reviewer Pass, parent task `t_9c4abb9f`)
**Governing baseline:** ADR-001 (Accepted), ADR-002 (Accepted), ADR-004 (Accepted; supersedes the targeted ADR-002/ADR-003 parts identified therein), Charter v0.1.0, Constitution, Operations Manual, `governance/profiles/PLANNER_PROFILE.md` v1.1.0
**Scope:** the three Human Sponsor-approved decisions (temporal baseline, wrapping baseline, Windows baseline) as recorded in ADR-004; the specification's acceptance criteria (§20); internal coherence; implementation-readiness within those decisions. No new architecture decisions were made.

---

## Task-body path note (not a spec defect)

The task body references `projects/AI-Co/docs/adr/ADR-003-bootstrap-open-question-resolutions.md`. That file does not exist and must not be published — ADR-004 §95 records it as the untracked duplicate whose substantive choices are preserved under ADR-004. The correct governing records are `ADR-003-safety-wrapping-and-host-boundaries.md` (superseded) and `ADR-004-human-sponsor-bootstrap-resolutions.md` (accepted). This review was performed against the correct governing records and the current v0.1.1 draft; the path discrepancy did not change the outcome.

## Independent-review baseline

The parent independent Reviewer Pass (docs/reviews/ADR-003-LANGUAGE-SPEC-REVIEW-2026-08-08.md) reported no findings in all four focus areas. Its quoted normative text matches the current ADR-004-aligned wording (e.g., the §14.3 baseline sentence, §11.3 wrap semantics), although its header labels the reviewed draft "v0.1.0 / ADR-003 (Accepted)". The Planner self-review below confirms the same content against ADR-004 and closes the one requirements-completeness gap the independent pass did not cover.

---

## Findings by decision

### 1. Raw-pointer temporal baseline (ADR-004 §34-49) — Complete

- Spec §12.8 items 1-5 carry the full accepted baseline: allocator registry with deterministic duplicate/invalid-release traps (`AIC-R0812`/`AIC-R0813`), full `0xDD` overwrite before reuse, freed blocks under project-allocator control until deterministic reuse or exit, defined stale-access outcomes (poisoned bytes before reuse; current allocation after reuse; `AIC-R0811` for inaccessible addresses), compiler obligations (no C-style lifetime-UB assumption or optimization), and the explicit "no complete temporal memory safety" claim with a prominent documentation/diagnostic requirement.
- Spec §15.1 defines the deterministic reuse rule (reverse order of release, observable contract independent of host allocator/timing/environment) and resource-exhaustion behavior (`alloc_bytes` returns `null`; never traps; never violates the reuse rule) — satisfying ADR-004 §49's "must define the allocator's deterministic reuse rule and observable resource-exhaustion behavior."
- §15.5 trap table and `DIAGNOSTIC-CONTRACT.md` §11.8 consistently register `AIC-R0811`..`AIC-R0813`.
- Traceability: §19 rows "ADR-002 initialization/storage + ADR-004 temporal baseline" and "ADR-004 temporal baseline" point to §8, §9, §12, §15 and §12.8/§15.1 respectively; §20 checklist item covers it.

**Finding: no correction required.** No C-style UB is introduced; every operation has defined semantics or a deterministic trap.

### 2. Wrapping arithmetic baseline (ADR-004 §51-57) — Complete

- §11.3 states the conversion-only semantics exactly: `wrap<T>(expr)` is an explicit modulo/truncating conversion; its operand evaluates under ordinary checked arithmetic; `wrap<T>(a op b)` must not be described as supplying same-width wrapping arithmetic (the checked operation rejects/traps before conversion).
- §11.3 states the deferral exactly: no same-width wrapping addition/subtraction/multiplication/negation in v0.1.0; dedicated built-ins deferred with an evidence-triggered reopen (concrete bootstrap/self-hosting need); any addition is a public language-surface change requiring an accepted ADR and conformance coverage.
- §11.5 (`wrap` never checked, never traps; integer targets, integer/enum sources; modulo `2^width`), §10.2 operator table, §5.2 grammar (`wrap_expr` is a unary conversion), §4.5 reserved names, and §18.4 example (`wrap<u8>(300)` → 44, conversion-only) are mutually consistent.
- The v0.1.0 misleading framing is removed (confirmed by inspection and recorded in OPEN-QUESTIONS.md OQ-002 "Correction applied in v0.1.1").

**Finding: no correction required.**

### 3. Windows 10 bootstrap baseline (ADR-004 §59-65) — Complete after one correction

- §14.3 already pinned Windows 10 22H2 x64 as the bootstrap development-host and execution baseline, disclosed the absence of further OS updates, and stated that newer-Windows compatibility is desirable but not a v0.1.0 conformance guarantee; §7.1 fixes the initial target (little-endian Windows x86-64); §15.7 binds runtime-facing calls to the internal Microsoft x64 convention; release/deployment remain separate Human Sponsor gates (also recorded in §19 traceability row and OPEN-QUESTIONS.md OQ-003).
- **Correction applied (Planner-owned):** ADR-004 §63 requires "Runtime-facing Windows calls must be enumerated and documented against this baseline." The v0.1.1 draft did not carry that normative obligation. A §14.3 bullet was added: runtime-facing Windows calls used by the project-owned runtime must be minimized, enumerated, and documented against the pinned baseline; the Windows API dependency surface is part of the implementation contract but bounded and documented so later supported-version testing is possible; host- or OS-dependent behavior must not be exposed as a language rule.
- The correction is recorded in Spec §21 review-state history.

**Finding: one requirements-completeness correction, applied.** It elaborates an accepted ADR-004 decision; it does not change requirements or architecture.

### 4. Traceability, supersession, and acceptance criteria — Coherent

- §19 contains dedicated rows for each ADR-004 clarification (temporal, wrapping, Windows); all other rows remain consistent with ADR-001/ADR-002/Charter/review findings.
- ADR-003 is correctly labeled Superseded by ADR-004 in the spec header, §19, OPEN-QUESTIONS.md, and DIAGNOSTIC-CONTRACT.md; no artifact treats ADR-003 as governing.
- No references to the deleted, never-committed Main Designer decision-note artifact exist anywhere in the spec set (verified by grep: only the superseded ADR-003 and the explicit removal note in §21/OPEN-QUESTIONS.md mention it).
- No false decision attribution: all three resolutions are attributed to Marcel, Human Sponsor, via ADR-004.
- §20 acceptance checklist items were verified by inspection; no unchecked or falsely-checked item found.

**Finding: no correction required.**

---

## Additional observations (non-findings)

1. `DIAGNOSTIC-CONTRACT.md` v0.1.1 is coherent with the spec: `AIC-N0207`/`AIC-N0208` (reserved runtime modules, Spec §6.5), `AIC-E0412` narrowing, mandatory `recovery` on error/trap records, structured `corrections`, and the v0.1.1 compatibility note (schema version stays "1") are internally consistent.
2. `OPEN-QUESTIONS.md` v0.1.1 correctly converts the three OQs into a monitored resolution record (OQ-001 resolved, OQ-002 deferred with trigger, OQ-003 resolved); no implementation-blocking open questions remain.
3. The spec remains Proposed and §21 correctly blocks implementation until the remaining gates pass.

## Corrections applied (Planner-owned only)

1. `spec/AI-CO-LANGUAGE-SPECIFICATION.md` §14.3 — added the runtime-facing Windows call enumeration/documentation obligation (ADR-004 §63).
2. `spec/AI-CO-LANGUAGE-SPECIFICATION.md` §21 — recorded the self-review result and correction in the review-state history.

No other changes were made. No new architecture decisions were made.

## Verdict

**Pass with one correction applied.** The amended normative wording is requirements-complete, internally coherent, and implementation-ready within the three ADR-004 decisions; the only gap found (ADR-004 §63 Windows-call enumeration obligation) is now carried in §14.3. The specification remains **Proposed** pending: (2) independent Reviewer conformance review of the amended v0.1.1 draft and `DIAGNOSTIC-CONTRACT.md`, and (3) Main Designer architectural acceptance (Spec §21 gates; ADR-004 §97).

## Handoff to Coordinator

- Verdict: **Pass with one correction** (spec amended to v0.1.1 + Planner self-review corrections; committed).
- Gate state: Specification remains **Proposed**; implementation blocked.
- Next action: Coordinator to route the independent Reviewer conformance review of the amended v0.1.1 specification and `DIAGNOSTIC-CONTRACT.md` (gate 2), then Main Designer architectural acceptance (gate 3). The prior independent Pass covered the ADR-003/ADR-004-aligned content; the Reviewer should confirm the §14.3 addition as part of the gate-2 review of the amended draft.
- Documentation state: this report delivered as `docs/reviews/PLANNER-SELF-REVIEW-LANGUAGE-SPEC-2026-08-08.md`; spec corrections committed in the AI-Co repository.

---

## Addendum: repository-state observation (2026-08-08, during self-review)

The parent independent Reviewer Pass report at `docs/reviews/ADR-003-LANGUAGE-SPEC-REVIEW-2026-08-08.md` was read in full at the start of this self-review (content: verdict Pass, no findings, four focus areas). That file is currently **absent from the working tree** and was never committed to the AI-Co repository (verified via `git ls-files`, `git log --all`, `git reflog`, and `find`). It disappeared during the concurrent v0.1.1 publish commit (`dd58eea`, 17:10:19) and has no kanban attachment on the parent task (`t_9c4abb9f`).

Impact: the self-review inputs were complete (the report was inspected before it vanished), but the durable evidence record for the independent Pass is missing. This is a repository-state defect, not a specification defect, and is outside Planner-owned writable records. Recommended follow-up: the Reviewer (or Coordinator) should re-deliver the independent Pass report — restoring it from the Reviewer's own session record and committing it — so the review trail for gate 2 remains complete.
