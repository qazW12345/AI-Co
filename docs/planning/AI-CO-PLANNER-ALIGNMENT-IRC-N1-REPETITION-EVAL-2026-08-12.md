# Planner Alignment Record — IRC-N1 repetition-form array literal `[e; N]` evaluate-exactly-once

**Status:** Alignment decision + proposed spec amendment (pending Main Designer acceptance)
**Owner:** Planner
**Date:** 2026-08-12
**Card:** t_72c6d57b (MIN-2 follow-up, child of t_24eee034)
**Decision authority:** Main Designer accepted interpretation IRC-N1 (t_d7b7f8d6 comment 1212, architecture lane; recorded on t_24eee034 Main Designer gate comment and Coordinator gate record). The Main Designer approved the interpretation and required Planner-owned language-spec/conformance alignment. Evidence: t_d7b7f8d6 comment 1212; t_24eee034 Main Designer gate comment (2026-08-12); Coordinator gate record t_02f8b6ff.
**Governing sources:** AI-Co language specification §10.4 (evaluation order), §10.5 (constant expressions), §12.1 (arrays), §21 (review state/change record); IR contract `docs/contracts/IR-CONTRACT-2026-08-12.md` §9.12 IRC-N1 (line 394) and §13 acceptance-record placeholder; DIAGNOSTIC-CONTRACT §11.5 (AIC-T0309 row, line 183); Stage-0 Work-Package Manifest §2/WP-M0-02 (conformance corpus is spec-derived; capability junior_specialist); spec §17.2 (feature-deferral/escalation rule).

---

## 1. IRC-N1 spec-silence status (cross-referenced from t_24eee034)

- **Status: confirmed public-spec silence, durably recorded.** The acceptance record on t_24eee034 states MIN-2: "IRC-N1 `[e; N]` fills a public-spec silence; acceptance record must state this; Planner alignment routed structurally to t_72c6d57b" (Coordinator gate record; Main Designer gate comment: "MIN-2 record IRC-N1's spec-silence status and have Coordinator route Planner-owned language-spec/conformance alignment").
- **Spec evidence of the silence:**
  - Spec §10.5 (line 595) admits repetition-form array literals `[e; N]` into the constant-expression enumeration (compile-time form).
  - Spec §12.1 (line 699) defines the array-initializer syntax `[e; N]` (repetition form) and the count-mismatch rejection `AIC-T0309`, but states **no evaluation count** for `e` in a runtime (non-constant) context.
  - Spec §10.4 (lines 574–585) enumerates evaluation order for binary operators, calls, assignment, indexing, slicing, `&&`/`||`, `?:`, member access, `sizeof`/`alignof`, and closes with "There is no unspecified evaluation order anywhere in the language" — the repetition-form evaluation count is not listed, so the completeness claim is not literally satisfied for `[e; N]` at runtime.
- **IR contract record:** §9.12 IRC-N1 (line 394) adopts the deterministic reading (evaluate-exactly-once, then repeated `IR_STORE` of the single evaluated value) and explicitly notes "it does not change any accepted spec text" — that statement was true at drafting time and remains the record that the spec did **not** already state the count. The contract's §13 acceptance-record entry for IRC-N1/N2/N3 is updated by the owning Specialist in the MIN-1 acceptance-record update (t_2397b562); no IR contract change is in scope here (card exclusion).
- **Diagnostic evidence:** DIAGNOSTIC-CONTRACT §11.5 AIC-T0309 (line 183) governs count mismatch only; no code names an evaluation-count rule, so no diagnostic contract change is implicated by this alignment.

## 2. Decision: (a) amend the language specification — adopt the Main Designer's accepted interpretation as normative text

**Decision: amend.** Record the accepted Main Designer interpretation (evaluate-exactly-once, including one evaluation when `N == 0`) as explicit normative wording in spec §12.1, with the change gated on Main Designer acceptance before adoption per this card's VERIFICATION/REVIEW.

### 2.1 Rationale

1. **The accepted interpretation fills a public contract gap, not an implementation choice.** The Main Designer's verdict states this explicitly: "Because this fills a public-spec silence rather than merely choosing an implementation technique … the acceptance entry must identify it as a Main Designer interpretation and the Coordinator must route a Planner-owned specification-alignment follow-up; the IR contract must not claim that the accepted language specification already states the evaluation count." Leaving the spec silent would keep an observable semantic (side-effect count of `e`, including traps) outside the public contract.
2. **Spec §10.4's completeness claim requires it.** "There is no unspecified evaluation order anywhere in the language" is a normative claim in the Accepted spec. The repetition-form evaluation count is observable runtime behavior (side effects and traps in `e` occur once vs. N times); a silent spec contradicts its own completeness claim. Amending restores internal consistency without expanding the language.
3. **The conformance corpus is spec-derived.** WP-M0-02's acceptance criteria (manifest §2) require expected behavior "derived strictly from the accepted specification … as the normative acceptance oracle," with "no implementation-derived values." A conformance case asserting exactly-once evaluation (e.g., side-effect counter, `N == 0`) has no normative anchor while the spec is silent; amending makes such a case spec-derived rather than implementation-derived.
4. **Precedent for precision amendments under accepted direction.** The spec already records Main Designer-approved precision amendments applied by the Planner within accepted semantics (§21: v0.1.2 Main Designer-approved precision amendments from t_5f69de3e comment 216; v0.1.5 ADR-005 alignment). This alignment is the same class: a precision statement of an already-accepted interpretation, not a new feature.
5. **Not a §17.2 feature addition.** §17.2 governs adding *excluded* features via feature ADR; `[e; N]` is already in the minimal language (§10.5, §12.1). The amendment states semantics for an existing form, so no feature ADR, no ADR change, no charter change, and no Human Sponsor gate applies (Main Designer confirmed no Human Sponsor gate implicated).

### 2.2 Consequences

- **Spec revision:** one normative bullet in §12.1 (proposed wording in §3 below) plus a §21 change-record entry; version bump v0.1.6 → v0.1.7 on adoption. No grammar change, no new diagnostic, no §10.5 change.
- **IR contract:** no change (excluded by this card). IRC-N1's note remains the interpretation record; the spec change record will cite it.
- **DIAGNOSTIC-CONTRACT:** no change.
- **Conformance corpus:** new cases become authorable (see §4); routed via Coordinator after adoption.
- **Implementation:** the accepted interpretation is already binding on WP-M0-16c (IR builder lowers `[e; N]` via single evaluation + `IR_ZERO`/repeated `IR_STORE`) and WP-M0-17 (backend); the spec amendment makes it normative rather than contract-derived. No implementation re-plan needed.
- **Alternative rejected:** option (b), interpretation-only without spec change, would leave the public spec silent and §10.4's completeness claim unsatisfied, keep the conformance oracle without a normative anchor, and require future readers to discover the semantics in the IR contract and acceptance records. It was rejected because the Main Designer's required path is explicitly to "align the normative language specification and relevant conformance expectations."

## 3. Proposed spec wording (for Main Designer acceptance)

**Amendment target: spec §12.1, array-initializer bullet (line 699).** Append one normative sentence to the existing bullet (no grammar change, no new bullet numbering):

> - Array initializer: array literals use square brackets — `[e0, e1, ..., eN-1]` (element list; the count must equal `N`) or `[e; N]` (repetition form). A literal whose count does not equal the declared size is **rejected** with `AIC-T0309`. In the repetition form `[e; N]`, the element expression `e` is evaluated **exactly once** — including when `N == 0` — and the resulting value is stored into each of the `N` elements in index order; side effects of `e` (including traps) occur exactly once regardless of `N`.

**Proposed §21 change-record entry (v0.1.7, on adoption):**

> **v0.1.7 (2026-08-12):** recorded the Main Designer's accepted interpretation IRC-N1 (t_d7b7f8d6 comment 1212; acceptance record t_24eee034) as normative text in §12.1: the repetition-form array literal `[e; N]` evaluates the element expression `e` exactly once — including one evaluation when `N == 0` — and stores the resulting value into each of the `N` elements in index order; side effects (including traps) occur exactly once. This closes the evaluation-count silence in the runtime (non-constant) context (§10.4 completeness claim) without changing grammar, diagnostics, or the constant-expression enumeration. Decision record: `docs/planning/AI-CO-PLANNER-ALIGNMENT-IRC-N1-REPETITION-EVAL-2026-08-12.md`. Authority: Main Designer accepted interpretation IRC-N1; Planner-authored precision amendment within accepted direction — no new architecture decision, no ADR/charter change, no Human Sponsor gate. Conformance corpus additions and Reviewer conformance re-review pending per OM §6.1.

## 4. Conformance-test impact

- **No existing case changes.** `tests/conformance/cases/derived-array-literal/` (element-list form, spec_ref §7.2) is unaffected; no existing conformance/negative case exercises the repetition form (verified: no repetition-form case in `tests/conformance/cases/`).
- **New conformance cases (WP-M0-02 owned area; capability junior_specialist; spec_ref §12.1), authorable after adoption:**
  1. Repetition form, `N >= 2`, with a side-effecting element expression: observable stdout/exit that proves `e` ran exactly once (e.g., `e` increments a global and the program prints the counter after initialization).
  2. Repetition form, `N == 0`: proves `e` is still evaluated exactly once although no element is stored (empty array `T[0]` with a side-effecting `e`).
  3. (Optional, smoke/trap suite WP-M0-04 area) repetition form whose `e` traps: proves the trap occurs once.
- **Routing:** after Main Designer acceptance and spec adoption, Coordinator routes a follow-up card for the corpus additions (WP-M0-02 area, junior_specialist) and Reviewer conformance re-review per OM §6.1 (precedent: §21 revision pattern for v0.1.2/v0.1.5).
- **No diagnostic impact:** no new AIC codes; AIC-T0309 row unchanged; DIAGNOSTIC-CONTRACT untouched.

## 5. Routing recommendation

- **Main Designer acceptance is required before adoption** of the proposed §12.1 wording (this card's VERIFICATION/REVIEW: "Main Designer acceptance for any spec amendment"). Route via Coordinator per OM §6.1/ADR-004 gate pattern (precedent: designer lane t_d7b7f8d6 for the IR contract).
- **After acceptance:** Planner applies spec v0.1.7 (this alignment record is the change-authorization source); Reviewer conformance re-review per OM §6.1; Coordinator routes the conformance-corpus follow-up (WP-M0-02 area).
- **Adjacent observation (explicitly out of scope, no action taken):** §10.4 does not enumerate element-list (`[e0, e1, ...]`) literal evaluation order either. This is not part of IRC-N1, was not flagged by the Main Designer, and is deliberately excluded to avoid scope creep; if it matters, it is a separate spec-precision question for the Main Designer, not folded into this amendment.

## 6. Review and completion state

- **This record:** Planner-authored alignment record; no spec edit performed in this run (amendment pending acceptance). Committed locally on main, not pushed (standing push gate).
- **Completion protocol:** per card RULES 1, handoff evidence posted as a comment; blocked with `review-required:` because Main Designer acceptance is required for the proposed spec amendment; this card is NOT self-completed — completion is applied by the authorized owner after the acceptance gate passes (OM §6.1, ADR-004).
