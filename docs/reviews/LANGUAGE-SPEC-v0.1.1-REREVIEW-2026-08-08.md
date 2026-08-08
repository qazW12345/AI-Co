# AI-Co Language Specification v0.1.1 (Proposed) — Re-review of Remediated Set (Gate 2, Round 1)

**Review date:** 2026-08-08
**Reviewer:** reviewer2 profile (Hermes Agent, Sneedworks organization)
**Task:** `t_b157b2bd` (re-review round 1 of the Planner's remediation against the gate-2 findings)
**Artifact version reviewed:** Post-remediation Proposed v0.1.1 specification set at **local HEAD `f366f0dc31eff4fe1ac782bcd9d03f5d4c7ff8f8`** (main, ahead of origin/main by 4: `406238c`, `f589d0f`, `2b79fb5`, `f366f0d`; not pushed).
**Scope:** project AI-Co

## Independence declaration

This re-review was conducted by the reviewer2 profile against the Planner's remediation commit `f366f0d`. The reviewer2 profile authored the gate-2 conformance review report being re-verified (`docs/reviews/LANGUAGE-SPEC-v0.1.1-REVIEW-2026-08-08.md`) but did not author or remediate the specification, diagnostic contract, or open-questions record; the remediation under review is Planner work (task `t_8e7b999d`). The reviewed artifacts (spec set) were not modified by this reviewer. This is a re-review of the same artifact line the reviewer previously assessed at gate 2; that prior review is the normative correction baseline, and this re-review independently re-inspects the post-correction state rather than relying on the remediation summary. Runtime lane: `deepseek-v4-flash` (opencode-go), the configured primary lane.

## Governing baseline (unchanged)

- Sneedworks Constitution v1.0.0 (Accepted)
- Sneedworks Operations Manual v1.1.0 (Accepted)
- AI-Co PROJECT_CHARTER.md v0.1.0 (Accepted, 2026-08-08)
- ADR-001 Bootstrap Compiler and Initial Target Architecture (Accepted)
- ADR-002 Minimal Core Language Semantics (Accepted)
- ADR-003 Safety, Wrapping, and Host Boundaries (Superseded by ADR-004; historical evidence only)
- ADR-004 Human Sponsor Bootstrap Resolutions (Accepted; governing)
- `research/ENVIRONMENT_BASELINE_2026-08-08.md` (evidence)
- `docs/reviews/LANGUAGE-SPEC-v0.1.1-REVIEW-2026-08-08.md` (gate-2 report; FIND-G2-01..09 Required resolution conditions are the normative correction spec; SUG-G2-01..03 consideration)

## Evidence inspected

| Artifact | Identity | Role |
|---|---|---|
| `spec/AI-CO-LANGUAGE-SPECIFICATION.md` | v0.1.1 at HEAD `f366f0d` | Primary artifact under re-review |
| `spec/DIAGNOSTIC-CONTRACT.md` | v0.1.1 at HEAD `f366f0d` | Normative companion under re-review |
| `spec/OPEN-QUESTIONS.md` | v0.1.1 at HEAD `f366f0d` | Resolution record; consistency check |
| Remediation diff | `git show f366f0d` (3 files; spec only) | Exact correction set |
| Gate-2 report | `docs/reviews/LANGUAGE-SPEC-v0.1.1-REVIEW-2026-08-08.md` | Normative correction spec (FIND-G2-01..09) |
| ADR-001 §107 | Accepted | Timestamp obligation (SUG-G2-01 vehicle) |
| ADR-004 §49 | Accepted | Allocator reuse/exhaustion obligation (FIND-G2-05) |
| Git history | `dd58eea`..`f366f0d` | Baseline and correction verification |

Working tree for the three spec-set files is byte-identical to HEAD (`git diff HEAD -- spec/` empty). No governance document was modified by the remediation commit (stat: 3 spec files only).

## Scope and exclusions

**In scope:** substantive closure of FIND-G2-01..09 per each Required resolution condition; internal consistency across spec/contract/registry/trap table/OPEN-QUESTIONS; new contradictions introduced by the correction; §21 review-history recording; the deterministic document-level checks listed below; consideration of SUG-G2-01..03 adoption.

**Excluded:** editing/remediation/implementation/risk acceptance/release; executable verification (no compiler exists). This report does not push to origin and does not modify the reviewed artifacts.

## Deterministic checks performed (recorded)

Run from a scratch script outside the repository (no repo-root pollution), reading the files at HEAD:

1. **Repository state:** HEAD `f366f0d`; working tree spec set identical to HEAD; remediation commit touches only the three spec-set files; LF-only line endings (0 CRLF matches).
2. **Code registry consistency:** All `AIC-*` codes referenced in the language specification, diagnostic contract, and OPEN-QUESTIONS exist in the DIAGNOSTIC-CONTRACT registry; **78 unique codes**, zero duplicate registry rows; `AIC-N0209` and `AIC-R0816` present in the registry; no spec/contract/OQ code missing from the registry.
3. **JSONL validity:** Both `jsonl` blocks in the diagnostic contract (4 compile-time records + 1 trap record) parse as valid JSON; all required fields (`schema_version`, `code`, `severity`, `phase`, `message`, `recovery`) present on every record.
4. **Forbidden artifact references:** Zero matches for `MAIN-DESIGNER-DECISIONS` / `ADR-003-bootstrap-open-question-resolutions` across spec, contract, and OPEN-QUESTIONS.
5. **Placeholder/draft markers:** None beyond the §20 self-claim sentence ("contains no unresolved placeholders or draft-decision markers") and the §5 `U`-class technical sentence ("its `00` digits are a fixed placeholder"); both are legitimate, not draft markers.
6. **Secret scan (spec set):** No API keys, tokens, passwords, or private-key material. (Initial scan hits were the lexical word "token" and the two legitimate "placeholder" sentences above; refined scan clean.)
7. **Grammar spot-checks:** `pub` on top-level declarations consistent with §6.3; switch case bodies are brace blocks with mandatory terminators (§13.2); negative-literal direct-operand rule present (§4.3); struct-literal/block ambiguity resolution present (§5.2); type postfix parsing wording present.
8. **Trap tables:** Spec §15.5 vs contract §11.8 — identical 16 `AIC-R` code sets plus user-trap row; all AIC-R trap exit codes `70`; `AIC-R0816` present in both tables and in the §12.8(2) trap list and §15.8 trap-report note.
9. **Null-span ordering:** Contract §9 now orders null-span records before file-bearing records within a phase, tie-broken by `code`, and explicitly names `AIC-O0701`/`AIC-O0702`/`AIC-BL0801`–`AIC-BL0803` (consistent with §3 and §11.6/§11.7).
10. **Example-vs-rule consistency (§18):** §18.4 uses explicit `cast<i8>(100)` and correct rejection codes (`200`→T0307, `cast<i8>(200)`→E0408, i8 overflow→E0405, `5/0`→E0406); §18.6 uses suffixed literals (`[0u8; 16]`, `65u8`) with negative coverage (`[0; 16]`→T0307, implicit array→slice→T0307) — all consistent with §4.3/§11.1/§11.2/§11.3; §18.5 annotations for `fn missing` (E0416) and `fn bad` (N0202, E0412) match the annotated constructs; **one residual gap found — see RER-G2-01**.
11. **Manifest/identity wording:** §14.4 hashed artifact set explicitly excludes the manifest; exact set enumerated (COFF objects + linked executable); no fixed-point rule needed; stage-invariant version field = AI-Co language/spec version string only; host/linker identity in §16.3 evidence only; identical relative output paths required; §16.2/16.3/16.5/16.6 aligned; no normalization introduced.
12. **Allocator rules:** §15.1 defines `alloc_bytes(0)` → `null` (no allocation, no trap, no state), exact-fit reuse (no split/coalesce; reverse order of release within a size class; fresh block when no exact size), exhaustion → `null`; consistent with §12.8(3) and ADR-004 §49; OPEN-QUESTIONS OQ-001 consequence line updated.
13. **Pointer-arithmetic trap:** §12.5 checked scaling product and byte-difference overflow; constant → `AIC-E0405`, runtime → new stable trap `AIC-R0816`; trap table §15.5, registry §11.8, §12.8(2), and §15.8 all updated; §21 history and contract version notes record the correction round.
14. **Timestamps (SUG-G2-01):** §16.2 now states "compiler-controlled timestamps are zero (ADR-001 §107)" — matches ADR-001 §107 verbatim ("Compiler-controlled timestamps are zero").

**Not reproduced (disclosed):** No compiler implementation exists, so no executable verification of semantic rules, trap behavior, diagnostic emission, or byte identity was possible. All checks are document-level determinism/consistency checks.

---

## Findings-closure table (FIND-G2-01..09)

| Finding | Severity (gate-2) | Closure | Assessment |
|---|---|---|---|
| FIND-G2-01 (examples contradict conversion rules) | Major | **Closed** | §18.4/§18.6 corrected to explicit `cast`/suffixed literals with negative coverage; annotations match §4.3/§11.1/§11.2/§11.3. |
| FIND-G2-02 (manifest self-hash unspecified) | Major | **Closed** | §14.4 explicitly excludes manifest from its own hash list; hashed set enumerated (COFF objects + linked executable); no fixed-point rule needed. |
| FIND-G2-03 (stage/host provenance conflicts with byte identity) | Major | **Closed** | Manifest's only version/identity field is the stage-invariant AI-Co language/spec version; host/linker identity moved to §16.3 evidence; identical relative output paths required for identity comparison; §16.2/16.5/16.6 aligned; no normalization. |
| FIND-G2-04 (null-span ordering undefined) | Major | **Closed** | Contract §9 defines null-span-before-file-bearing ordering within phase, tie-break by `code`, naming the affected classes. |
| FIND-G2-05 (allocator zero-size / size-fit undefined) | Major | **Closed** | §15.1 defines `alloc_bytes(0)`→`null` (no allocation/no trap/no state), exact-fit reuse (no split/coalesce, reverse release order within size class, fresh block otherwise), exhaustion→`null`; consistent with §12.8(3), ADR-004 §49, OQ-001. |
| FIND-G2-06 (pointer-arithmetic overflow undefined) | Major | **Closed** | §12.5 checked scaling/byte-difference overflow; constant→E0405, runtime→new stable trap AIC-R0816; registry §11.8, trap table §15.5, §12.8(2), §15.8 updated. |
| FIND-G2-07 (bare `rt` import / auto-availability ambiguous) | Minor | **Closed** | §6.5 rejects bare `import rt;` with new code AIC-N0209; runtime members not auto-available; unimported reference→AIC-N0202; registry updated. |
| FIND-G2-08 (§18.5 annotation misplaced/incomplete) | Minor | **Not fully closed — residual RER-G2-01** | N0202 annotation added and E0416 relocated to `fn missing` (both correct), but `fn bad` still has a reachable no-return path (selector matches no case; `default` is optional per §13.2) that is **not** annotated with E0416 — the gate-2 evidence itself stated "both functions have a no-return path". |
| FIND-G2-09 (schema-versioning rule tension) | Minor | **Closed** | Contract §11.10 gained the draft-exemption clause; consistent with the v0.1.1 "schema remains 1" note. |

### Suggestions (SUG-G2-01..03)

- **SUG-G2-01** (timestamp wording): **adopted** — §16.2 now reads "compiler-controlled timestamps are zero (ADR-001 §107)". ✓
- **SUG-G2-02** (`U` class phase-group exemption): **adopted** — contract §5 notes `AIC-U0000`'s `00` digits are a fixed placeholder, not a phase group. ✓
- **SUG-G2-03** (manifest version/identity semantics): **adopted** — §14.4 defines the field as the AI-Co language/spec version string (the FIND-G2-03 vehicle). ✓

---

## New findings introduced by the remediation (or residual)

### RER-G2-01 (Minor): §18.5 `fn bad` example still lacks the required `AIC-E0416` annotation for its reachable no-return path

**Violated obligation:** Spec §13.4 — "every reachable path in a non-`void` function must end in `return`; otherwise **rejected** with `AIC-E0416`." §13.2 — "`default` is optional". §13.5 reachability rules treat only loops as potentially non-terminating; a `switch` with no matching case and no `default` completes normally and the function tail is reachable. §18 conventions require error annotations to state the correct code and span; §20 claims the example set is coherent and covers all rule families.

**Evidence:** Current §18.5 (lines 1129–1140 at HEAD `f366f0d`):
```
// ERROR AIC-N0202: span "x" (undeclared; x is declared inside case 0's block, out of scope)
// ERROR AIC-E0412: span "case 0" (case body lacks a terminating statement)
fn bad(n: i32) -> i32 {
  switch (n) {
    case 0: { var x: i32 = 1; }
    case 1: { return x; }
  }
}
// ERROR AIC-E0416: span "fn missing" (non-void, path without return)
fn missing(n: i32) -> i32 {
  if (n > 0) { return n; }
}
```
For `fn bad`, when `n` matches neither `0` nor `1`, no case matches, `default` is absent (and optional per §13.2), the `switch` completes, and control reaches the end of the non-`void` function without a `return` — so a conforming compiler must reject `fn bad` with `AIC-E0416` (span: the function name, per contract §11.5) **in addition to** the annotated `AIC-N0202` and `AIC-E0412`. The remediation added the N0202 annotation and relocated E0416 to `fn missing` (correct for `fn missing`), but did **not** add an E0416 annotation for `fn bad`. The gate-2 report's own FIND-G2-08 evidence explicitly stated "both functions have a no-return path"; the remediation therefore resolved the annotation-placement ambiguity for `fn missing` but left `fn bad`'s E0416 unannotated — the same class of annotation incompleteness FIND-G2-08 was raised to fix.

**Impact:** A conformance-suite author reading the §18.5 example will not assert E0416 for `fn bad`, while the negative-diagnostic suite (contract §13) requires the emitted record set to contain the required root-cause code; the example set (which §20 claims is coherent and contradiction-free) is incomplete for the reachability rule family. The normative rules themselves are correct and consistent; the defect is bounded to one example's annotation set.

**Severity rationale:** Minor — a bounded example-annotation completeness defect in normative example content; it does not invalidate the corrected rules, the registry/trap-table/JSONL consistency, or the manifest/allocator/pointer contracts. Same severity class as FIND-G2-08 (Minor).

**Required resolution condition (for round 2 or a follow-up correction):** Either (a) add `// ERROR AIC-E0416: span "fn bad"` to the §18.5 `fn bad` example (consistent with §13.4/13.5), or (b) restructure the example so `fn bad` has no reachable no-return path (e.g., add a `default` clause that returns), keeping the annotations matching the actual error set; verify the negative-coverage claim in §20/§18 conventions remains accurate.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer re-inspects §18.5; finding closes when `fn bad`'s annotation set matches the actual error set (N0202 + E0412 + E0416, or the example restructured so E0416 is not expected).

---

## FIND-001..005 closure assessment (from initial architecture review)

- FIND-001 (normalization spec): closure now **complete** — §16.2/16.3 byte-identity gate is well-defined with the FIND-G2-02/03 manifest corrections (self-hash exclusion, stage-invariant fields, identical output paths, host identity in evidence only).
- FIND-002 (external linker time bound): Closed via M1/M2 boundaries in §16.5. ✓ (unchanged)
- FIND-003 (function-pointer necessity): Closed via recorded evaluation in §17.3. ✓ (unchanged)
- FIND-004 (runtime contract): closure now **complete** — FIND-G2-05 (allocator zero-size/exact-fit) and FIND-G2-06 (pointer arithmetic overflow trap) close the runtime-contract gaps.
- FIND-005 (diagnostic schema): closure now **complete** — FIND-G2-04 (null-span ordering) and FIND-G2-09 (schema-version draft exemption) close the diagnostic-contract gaps.

## Repository-state observations (unchanged, not findings)

- Untracked stub `docs/reviews/REVIEWER-CONFORMANCE-REVIEW-LANGUAGE-SPEC-2026-08-08.md` (3 lines) and stray `C:tmpcheck_jsonl.py` remain at the repository; both were noted in the gate-2 report as Coordinator/Historian hygiene items. Neither is part of the spec set; neither affects this verdict.

---

## Verdict

**Approved with Minor findings.**

**Rationale:** All six Major gate-2 findings (FIND-G2-01..06) and two of the three Minor findings (FIND-G2-07, FIND-G2-09) are substantively closed per their Required resolution conditions; the three Suggestions are adopted coherently; deterministic document-level checks all pass (registry 78 codes/no duplicates, JSONL valid, trap tables 1:1, secret scan clean, no forbidden references, no stray placeholders, examples consistent with conversion rules except the single §18.5 gap, manifest/identity wording stage-invariant without normalization, allocator and pointer rules fully specified); §21 review history records the correction round; OPEN-QUESTIONS OQ-001 consequence line updated; no governance document modified. One residual Minor finding (RER-G2-01) remains: the §18.5 `fn bad` example lacks the E0416 annotation for its reachable no-return path, which the gate-2 evidence itself identified ("both functions have a no-return path"). This finding does not invalidate the corrected rules or the acceptance/conformance claim of the specification set; it must be corrected or explicitly deferred before the negative-diagnostic suite is authored.

**Gate state:** Specification and diagnostic contract remain **Proposed**. **The Main Designer may proceed to gate-3 architectural acceptance** of the amended v0.1.1 specification set, with RER-G2-01 routed to the Planner for correction (or an explicit deferral recorded by the Coordinator). The push card `t_8179c45a` is released by this verdict (no Major/Critical findings).

**Limits of assurance:** This verdict covers the reviewed documents only. No compiler exists; behavioral conformance, trap behavior, and bootstrap byte identity are not executable yet and were not verified beyond document-level determinism/consistency checks (disclosed above). This review is not a warranty of defect absence.

**Residual-risk owner:** Planner (RER-G2-01 example correction); Main Designer (architectural acceptance, deferred to gate 3); Coordinator (routing, gate state, push release); Marcel (Human Sponsor) for any purpose-affecting or risk-acceptance questions — none raised by this re-review.

**Remediation/re-review condition:** RER-G2-01 (Minor) should be corrected in a small follow-up (or explicitly deferred with Coordinator authorization) before negative-diagnostic-suite authoring; because it is Minor and does not affect the rules or acceptance-critical claims, no full round-2 re-review is required for this finding alone. The push card may be released per the verdict.

## Handoff to Coordinator

- **Verdict:** Approved with Minor findings (0 Critical, 0 Major, 1 Minor residual: RER-G2-01).
- **One-line Main Designer statement:** **The Main Designer may proceed to gate-3 architectural acceptance** of the amended Proposed v0.1.1 specification set; the push card `t_8179c45a` is released by this verdict (no Major/Critical findings).
- **Findings status:** FIND-G2-01..07, FIND-G2-09 closed; FIND-G2-08 substantially addressed but not fully closed (residual RER-G2-01); SUG-G2-01..03 adopted.
- **Remediation routing:** RER-G2-01 → Planner (routes through Coordinator) for the small §18.5 annotation fix or explicit deferral; Coordinator applies gate state and may release push.
- **Documentation state:** This report delivered as `docs/reviews/LANGUAGE-SPEC-v0.1.1-REREVIEW-2026-08-08.md` and committed locally on `main` (not pushed; push handled by downstream task).
- **Review evidence:** local commit of this report; deterministic checks recorded above; exact references to lines in the reviewed artifacts; working tree of spec set verified identical to HEAD.

*End of re-review report (round 1)*
