# AI-Co Stage-0 Planning Handoff and Completion Report

> **TRIAL — Constitution §8 temporary exception (2026-08-08).** This attribution header is a trial addition per [TRIAL-APPROVAL-authority-attribution-control-2026-08-08.md](../../../governance/runtime/TRIAL-APPROVAL-authority-attribution-control-2026-08-08.md) and proposal v0.3.1 (Controls 1–5). Expires 30 days from trial start or 3 ADRs + 5 reports, whichever comes first. Rollback by the Main Designer through the authorizing approval path.

- **Recorded by:** Planner
- **Decision authority (if applicable):** Main Designer (accepted architecture: ADR-001, ADR-002, ADR-004); Marcel, Human Sponsor (ADR-004 resolutions; Stage-0 direction and working philosophy, 2026-08-08)
- **Human approver (if a gate applies):** Not applicable — this is a planning handoff; no new approval gate is claimed. M0/M1 milestone gates remain with the Main Designer; human gates remain with Marcel per the Constitution.

**Status:** Complete for handoff (planning artifact; not an implementation or acceptance claim)
**Owner:** Planner
**Date:** 2026-08-09
**Scope:** project AI-Co, task t_dd420fb9
**Evidence inspected:** see §3 below.

## 1. Requested outcome and governing decisions

Transform the accepted AI-Co specification set (spec v0.1.1 Accepted set, ADR-001/002/004, charter, environment baseline) into an implementation-ready milestone plan and work-package manifest for the Coordinator, focused on the first serious proof: a conservative C17 Stage-0 compiler that compiles the AI-Co compiler, then Stage 1/Stage 2 byte-identity under the deterministic bootstrap contract — serial, bite-sized, one code area at a time, consistency over speed, per Marcel's working philosophy (task t_dd420fb9).

Governing decisions relied upon (attribution):
- [Main Designer] approved the bootstrap compiler and initial target architecture (C17 Stage-0; Windows x86-64; project-owned COFF; external linking time-bounded to M1; deterministic bootstrap contract). Evidence: `docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md` (Accepted).
- [Main Designer] approved minimal core language semantics (canonical declarations, fixed-width types, no implicit narrowing, checked arithmetic, explicit modules). Evidence: `docs/adr/ADR-002-minimal-core-language-semantics.md` (Accepted).
- [Marcel, Human Sponsor] approved the bootstrap resolutions: allocator temporal baseline, wrapping-as-conversion-only, and Windows 10 22H2 x64 pinned baseline. Evidence: `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (Accepted); superseded ADR-003 retained as history only.
- [Main Designer] accepted the language specification set at gate 3. Evidence: spec §21 gate-3 entry; task t_b157b2bd; spec Status flip commit 928ea39 (parent task t_b80286c0).
- [Marcel, Human Sponsor] set the Stage-0 working philosophy (slow, serial, no pressure, consistency over speed). Evidence: task t_dd420fb9 body (2026-08-08 direction).

## 2. Artifacts created

1. `docs/planning/AI-CO-STAGE0-MILESTONE-PLAN-2026-08-09.md` — milestone plan: M0/M1/M2 interpretation, M0 exit gate, repository layout, build conventions, verification harness design, serial/file-ownership discipline, host-toolchain notes, confidence/assumptions/risks, review plan, Coordinator handoff.
2. `docs/planning/AI-CO-STAGE0-WORK-PACKAGE-MANIFEST-2026-08-09.md` — 20 M0 work packages (WP-M0-01..20) with all Coordinator-routing fields; file-ownership matrix; serial dependency chain; M1 milestone-level streams with refinement checkpoint.
3. `docs/planning/AI-CO-STAGE0-PLANNING-HANDOFF-2026-08-09.md` — this report.

Commit: `docs/planning/` committed locally on `main`; **not pushed** (per task constraint; push handled separately). Commit message: "Add AI-Co Stage-0 milestone plan and work-package manifest (planning kickoff)".

## 3. Evidence and sources inspected

- Planner Profile v1.1.0 (`governance/profiles/PLANNER_PROFILE.md`); Coordinator Profile v1.0.0; Senior/Junior Specialist Profiles v1.0.1; Report Templates (Attribution header); Operations Manual §5–§13; authority-attribution trial record.
- `PROJECT_CHARTER.md` v0.1.0; `spec/AI-CO-LANGUAGE-SPECIFICATION.md` v0.1.1 (Accepted; full read); `spec/DIAGNOSTIC-CONTRACT.md` v0.1.1 (Accepted; full read); `spec/OPEN-QUESTIONS.md` v0.1.1 (Accepted).
- `docs/adr/ADR-001`, `ADR-002`, `ADR-003` (superseded), `ADR-004`.
- `research/ENVIRONMENT_BASELINE_2026-08-08.md` (toolchain, `link` collision, E:/C: constraint, CPU/OS baseline).
- Repository state: branch `main`, ahead of origin by the spec-acceptance commit; no implementation source exists yet; `.gitignore` already covers `bootstrap/stage0|1|2/`.
- Parent handoff t_b80286c0 (spec Status flip to Accepted).

## 4. Requirements and architecture confidence

- **Requirements confidence: High.** The accepted specification is normative, gate-reviewed, and has no implementation-blocking open questions (spec §21; OPEN-QUESTIONS.md).
- **Architecture confidence: High.** ADR-001/002/004 are Accepted and define the pipeline, semantics, runtime, and bootstrap contract. The single delegated design item — the IR instruction set (spec §14.1(6)) — is contained in WP-M0-16 with an explicit Main Designer review gate, so it does not lower M0 architecture confidence.

## 5. Assumptions and unresolved uncertainty

Assumptions (recorded in milestone plan §9, with owners): host toolchain per baseline; C17 runtime serves M0 and M1 (runtime-in-AI-Co not required until M2 planning); corpora are the normative oracle authored from spec; gitignored stage dirs remain build output only; no wrapping-arithmetic/function-pointer need in the C17 compiler (triggers preserved); private repo, no publication in M0.

Unresolved (visible, not hidden): exact IR instruction set (delegated to WP-M0-16 + Main Designer); corpus/harness schema binding (WP-M0-02..05 coordinate via documented schema; harmonize through Planner if they drift); M1 package granularity (Planner refinement checkpoint after M0 verification).

## 6. Milestones, dependencies, and critical sequencing

- **M0** — Stage-0 bootstrap compiler + verification infrastructure (20 serial packages).
- **M1** — first self-hosting proof per spec §16.5 (milestone-level streams, refined after M0).
- **M2** — self-sufficient development baseline per spec §16.5 (recorded as boundary only; out of scope).

Critical path: WP-M0-01 (conventions) → 06 (diag) → 07 (load) → 08 (lex) → 09 (parse) → 10 (name) → 11 (types) → 12 (const) → 13 (sema) → 16 (IR, requires Main Designer contract gate) → 17 (backend) → 18 (COFF) → 19 (driver/manifest/link) → 20 (integration gate). Corpora (02–04) and harness (05) are spec-derived scaffolding that may be authored early; they serialize before the compiler stages in dispatch order but do not block later stages' unit verification. Runtime packages (14, 15) fit after sema (13) before IR/backend (16/17) in the serial order, with the rt.trap interface coordinated between 14 and 15 as a documented interface note.

## 7. Work packages and intended capabilities

20 M0 packages; 16 `senior_specialist`, 4 `junior_specialist` (corpora 02–04, harness 05). 4 M1 milestone-level streams (all senior, refined later). No package overlaps another's file ownership (§2 matrix of the manifest).

## 8. Acceptance and verification coverage

- Every M0 package has testable acceptance criteria tied to spec sections/diagnostic codes, a named review class (Reviewer independent review for implementation; Reviewer + Main Designer for WP-M0-16 contract and WP-M0-20 gate; Reviewer + Main Designer for WP-M0-15 Windows API doc), and escalation owners.
- M0 exit gate (milestone plan §3) maps to the accepted spec's conformance obligations (§2.3), determinism (§14.2), manifest (§14.4), runtime (§15), and bootstrap (§16) requirements.
- M1 acceptance is left normative per spec §16.5/§16.6; not restated as new decisions.

## 9. Review and approval state

- Planning self-review: performed by the Planner over the two artifacts (scope, ownership disjointness, serial chain, traceability, attribution). No independent Reviewer review was requested for this planning handoff; the Coordinator may route a Reviewer conformance check of the manifest before card creation if assurance policy requires it.
- No approval is claimed in this report. M0 gate acceptance and IR contract acceptance remain with the Main Designer; any human gate remains with Marcel.

## 10. Coordinator handoff and follow-up decision owners

- **Handoff:** `docs/planning/AI-CO-STAGE0-WORK-PACKAGE-MANIFEST-2026-08-09.md` is the routing surface. The Coordinator creates cards serially per the manifest, enforces dependency edges and file-ownership, and returns gaps to the Planner.
- **Follow-up decision owners:** Main Designer — IR contract acceptance (WP-M0-16 gate), Windows API baseline doc (WP-M0-15), M0 milestone gate (WP-M0-20); Reviewer — independent verification per package and M0 evidence; Planner — M1 refinement after M0 verification, registry/contract changes; Coordinator — serial dispatch and board administration; Marcel — any human-gated choice (none currently).
