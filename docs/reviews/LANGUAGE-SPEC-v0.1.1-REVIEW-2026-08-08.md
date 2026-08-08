# AI-Co Language Specification v0.1.1 (Proposed) — Independent Conformance Review (Gate 2, Consolidated)

**Review date:** 2026-08-08
**Reviewer:** reviewer2 profile (Hermes Agent, Sneedworks organization)
**Task:** `t_b2c932bc` (consolidated gate-2 independent conformance review; supersedes `t_7fe50c06` and `t_cf703bf3`)
**Artifact version reviewed:** Proposed v0.1.1 specification set at **local HEAD `f589d0f0056dd628590aa77189c013092c875d70`** (main, ahead of origin/main by 2). The pre-correction public commit `dd58eea5d79d5074bfd5539879d478a0f25119ff` was **not** used as the review baseline, per the task mandate.
**Scope:** project AI-Co

## Independence declaration

This review was conducted by the reviewer2 profile with no prior material authorship of the reviewed artifacts (specification, diagnostic contract, open-questions record). The reviewer2 profile did not author, remediate, or correct any of the artifacts under review. No conflict of interest is present. The prior independent Reviewer Pass report cited in the Planner self-review (`docs/reviews/ADR-003-LANGUAGE-SPEC-REVIEW-2026-08-08.md`) is absent from the repository (never committed; see Planner addendum commit `f589d0f`); this review therefore verifies current sources independently and does not rely on that report. Runtime lane for this review: `deepseek-v4-flash` (opencode-go), the configured primary lane; no authorship or prior-review involvement exists to disclose beyond this statement.

## Governing baseline

- Sneedworks Constitution v1.0.0 (Accepted)
- Sneedworks Operations Manual v1.1.0 (Accepted)
- AI-Co PROJECT_CHARTER.md v0.1.0 (Accepted, 2026-08-08)
- ADR-001 Bootstrap Compiler and Initial Target Architecture (Accepted)
- ADR-002 Minimal Core Language Semantics (Accepted)
- ADR-003 Safety, Wrapping, and Host Boundaries (Superseded by ADR-004; historical evidence only)
- ADR-004 Human Sponsor Bootstrap Resolutions (Accepted; governing)
- `research/ENVIRONMENT_BASELINE_2026-08-08.md` (evidence)
- `docs/reviews/INITIAL-ARCHITECTURE-REVIEW-2026-08-08.md` (Approved with Minor findings; FIND-001..005)
- `docs/reviews/PLANNER-SELF-REVIEW-LANGUAGE-SPEC-2026-08-08.md` (Planner self-review; gate 1)

## Evidence inspected

| Artifact | Identity | Role |
|---|---|---|
| `spec/AI-CO-LANGUAGE-SPECIFICATION.md` | v0.1.1 Proposed at HEAD `f589d0f` | Primary artifact under review |
| `spec/DIAGNOSTIC-CONTRACT.md` | v0.1.1 Proposed at HEAD `f589d0f` | Normative companion under review |
| `spec/OPEN-QUESTIONS.md` | v0.1.1 at HEAD `f589d0f` | Resolution record; consistency check |
| `docs/adr/ADR-001..ADR-004` | Accepted / Superseded per headers | Governing requirements |
| `PROJECT_CHARTER.md` | v0.1.0 Accepted | Project purpose/constraints |
| `research/ENVIRONMENT_BASELINE_2026-08-08.md` | 2026-08-08 | Machine evidence (Haswell i7-4770, Windows 10 22H2 build 10.0.19045.6466) |
| `docs/reviews/INITIAL-ARCHITECTURE-REVIEW-2026-08-08.md` | committed | Prior review; FIND-001..005 |
| `docs/reviews/PLANNER-SELF-REVIEW-LANGUAGE-SPEC-2026-08-08.md` | committed (406238c, f589d0f) | Planner self-review; gate 1 |
| Git history | dd58eea, 406238c, f589d0f | Correction/commit verification |
| README.md, LICENSE, .gitignore | current | Public content / secret / license clarity |

All artifacts inspected on disk at `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`. Working tree for the three spec-set files is byte-identical to HEAD (verified: `git diff HEAD -- spec/` empty). The §14.3 Planner correction (runtime-facing Windows calls minimized/enumerated/documented) is present at HEAD (line 848) and in `git show HEAD:spec/...`.

## Scope and exclusions

**In scope:** authority/supersession; grammar uniqueness; static/dynamic semantic completeness; ADR-004 temporal/wrapping/Windows alignment (incl. spec §14.3); diagnostic schema/code registry/order; runtime/allocator/pointer rules; bootstrap byte identity; closure of Planner self-review focus areas; the required focus list from the task body.

**Excluded:** editing/remediation/implementation/risk acceptance/release. The specification and diagnostic contract were not modified. This report does not push to origin (downstream task handles push).

## Deterministic checks performed (recorded)

1. **Repository state:** HEAD `f589d0f` ahead of origin/main by 2 (406238c, f589d0f); working tree spec set identical to HEAD; the pre-correction commit `dd58eea` was excluded as baseline.
2. **Code registry consistency:** All 68 `AIC-*` codes referenced in the language specification exist in the DIAGNOSTIC-CONTRACT registry (76 codes defined including internal classes I/B/O/BL/U). Zero spec codes missing from the registry; zero duplicate registry rows; zero contract-prose codes outside the registry; all OPEN-QUESTIONS codes ⊆ registry.
3. **JSONL validity:** Both `jsonl` blocks in the diagnostic contract (4 compile-time records + 1 trap record) parse as valid JSON; all required fields present; `recovery` present on all error/trap records.
4. **Forbidden artifact references:** Zero matches for `MAIN-DESIGNER-DECISIONS` / `ADR-003-bootstrap-open-question-resolutions` / the deleted decision-note artifact across spec, contract, and OPEN-QUESTIONS.
5. **Placeholder/draft markers:** None (the only lexical hits are the §20 self-claim sentence "contains no unresolved placeholders or draft-decision markers").
6. **Secret scan (tracked files):** No API keys, tokens, passwords, or private-key material found. README license disclaimer present; `LICENSE` file with explicit "NO LICENSE GRANTED" present (closes initial-review SUGG-004); `.gitignore` covers `.env`, `*.key`, `*.token`, `GitHub-API.txt`.
7. **Grammar spot-checks:** `pub` on all top-level declaration kinds (grammar `top_level_decl` matches §6.3); `switch` case bodies are brace blocks with mandatory terminators; type postfix left-to-right parsing; struct-literal/block ambiguity resolution documented; negative-literal rule (§4.3) consistent with grammar note.
8. **Trap tables:** Spec §15.5 (16 rows) vs contract §11.8 (15 rows + AIC-U0000) — 1:1 consistent; trap exit code 70 consistent.
9. **ADR-004 alignment:** §14.3 Windows obligation present (Planner correction confirmed); §12.8/§15.1 temporal baseline matches ADR-004 §34–49; §11.3 wrapping baseline matches ADR-004 §51–57; OPEN-QUESTIONS OQ-001 (resolved), OQ-002 (deferred w/ trigger), OQ-003 (resolved) consistent.
10. **No-normalization:** §14.2, §16.2, §16.3 prohibit normalization of compiler-produced artifacts, consistent with ADR-001 §99–109.

**Not reproduced (disclosed):** No compiler implementation exists, so no executable verification of the semantic rules, trap behavior, bootstrap byte identity, or diagnostic emission was possible. All checks above are document-level determinism/consistency checks.

---

## Findings

### Critical Findings

None.

### Major Findings

#### FIND-G2-01 (Major): Normative examples contradict the implicit-conversion rules for integer literals

**Violated obligation:** Spec §18 is titled "Normative examples and counterexamples"; its conventions state `// valid` marks conforming examples and `// ERROR AIC-xxxx` marks invalid examples. §4.3 fixes unsuffixed integer literal types (fits i32 → i32). §11.1 permits implicit conversions only for the listed value-preserving widenings; there is no i32→i8 or i32→u8 row, and "No narrowing ... is ever implicit." §20 self-checklist claims examples cover all major rule families and the draft contains no contradictions.

**Evidence:**
- §18.4 line 1098: `var a: i8 = 100;` — shown as valid (no error marker; line 1099 then treats `a` as i8). `100` is an i32 literal, so `i32 → i8` is implicit narrowing, which §11.1 forbids. The very next line's comment (1100) states "200 is i32; implicit narrowing is absent" for `var c: i8 = 200;` — the identical construct. The two adjacent examples are mutually contradictory: `100` (i32 → i8) is accepted while `200` (i32 → i8) is rejected, with no stated value-based exception in §4.3 or §11.1.
- §18.6 line 1140: `var buf: u8[16] = [0; 16];` — shown as valid. `0` is an i32 literal; the array-literal element type is i32, giving `i32[16]`; assigning `i32[16]` to `u8[16]` requires an array conversion that does not exist (§7.3 type identity; §11.1 has no array rows; §11.2 matrix has no array→array conversion). No contextual typing of array literal elements is specified.
- §18.6 line 1142: `sl[0] = 65;` — shown as valid ("buffer element mutated"). `65` is an i32 literal assigned to a `u8` lvalue — implicit narrowing i32→u8, forbidden by §11.1.

**Impact:** A conformance-suite author or implementer following the normative rules must reject `var a: i8 = 100;`, `[0; 16]` for `u8[16]`, and `sl[0] = 65;`; an author following the examples accepts them. The example set, which §20 claims is normative and contradiction-free, contradicts the rules it is meant to illustrate. This breaks the conformance claim for the conversion rule family.

**Severity rationale:** Major — an acceptance/consistency failure within normative example content; must be corrected before approval so conformance expectations are unambiguous. Not Critical (no security/data-loss/unauthorized-action harm).

**Required resolution condition:** Planner must either (a) correct the examples to use explicit conversions (`cast`/`wrap`) or to show the correct rejection codes, or (b) add an explicit, stated rule for contextual typing of integer literals / array literal elements where the value fits the target range (with conformance coverage), and reconcile §4.3/§11.1 accordingly. The chosen resolution must be consistent across §18.4 and §18.6 and covered by the negative-diagnostic suite.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer re-inspects the corrected §18 examples against §4.3/§11.1; finding closes when every example's validity/error marker matches the normative conversion rules.

#### FIND-G2-02 (Major): Build manifest self-hash recursion is unspecified

**Violated obligation:** §14.4 requires the build manifest to record "output artifact paths (relative to project root) and SHA-256 hashes of each artifact". §16.2 makes the Stage 1 and Stage 2 build manifests primary identity artifacts that "must be byte-identical with no normalization step". §16.6 acceptance evidence includes byte-identity comparison of the manifests. The manifest is itself a build output artifact.

**Evidence:** §14.4 (lines 851–865) lists manifest fields including per-artifact SHA-256 hashes but does not state whether the manifest's own hash is included among "each artifact", nor the rule for computing a self-hash deterministically (e.g., hash over content with the self-hash field zeroed, or manifest excluded from its own hash list). §16.2 requires byte-identical manifests; a self-referential hash field without a defined computation rule cannot be computed deterministically as specified.

**Impact:** Implementers must guess: (a) manifest excludes itself (then "each artifact" is under-specified), or (b) manifest includes its own hash (then a fixed-point/exclusion rule is required but absent). Divergent choices produce divergent manifests, defeating the raw byte-identity gate or forcing an undocumented normalization — which §16.2 forbids.

**Severity rationale:** Major — acceptance-contract failure in the bootstrap identity gate; the manifest schema cannot be implemented deterministically as written.

**Required resolution condition:** §14.4 must explicitly state whether the manifest hashes itself and, if so, the exact deterministic self-hash computation rule (field excluded from hash input), or explicitly exclude the manifest from "each artifact" and enumerate exactly which artifacts are hashed.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected §14.4/§16.2 wording; finding closes when the manifest hash field set and self-hash rule are fully defined and consistent with byte-identity acceptance.

#### FIND-G2-03 (Major): Stage/host-specific provenance and output paths conflict with raw byte-identical manifest identity

**Violated obligation:** §14.4 requires the manifest to record "compiler version/identity and host compiler used for the executable if applicable" and "output artifact paths (relative to project root)". §16.2 requires Stage 1 and Stage 2 manifests to be byte-identical for the same source/inputs/options with no normalization. §16.3 requires the comparison evidence to record host compiler identity/version and linker flags.

**Evidence:** The Stage 1 manifest is emitted by the Stage 0 compiler (a C17 program built by MSVC or Clang); the Stage 2 manifest is emitted by the Stage 1 compiler (an AI-Co program). "Compiler version/identity" and "host compiler used for the executable" therefore differ by stage (Stage 0's host compiler MSVC/Clang vs Stage 1's AI-Co identity), and "output artifact paths" differ if the two stage builds write to different directories (`.gitignore` lists `bootstrap/stage0/`, `bootstrap/stage1/`, `bootstrap/stage2/` as separate trees). The spec does not state how these stage-dependent fields are made identical across the two manifests (e.g., by defining the fields as stage-invariant values, by excluding them from the identity comparison, or by requiring identical output paths). As written, the byte-identical-manifest requirement is unsatisfiable or forces hidden normalization.

**Impact:** The primary identity gate (M1 acceptance: raw byte identity of Stage 1/Stage 2 manifests) cannot be met as specified unless the spec defines stage-invariant manifest content; without correction, either the gate fails or implementers resort to prohibited normalization.

**Severity rationale:** Major — internal contradiction between §14.4 manifest schema and §16.2/§16.6 byte-identity acceptance; must be resolved before approval.

**Required resolution condition:** §14.4/§16.2/§16.6 must define exactly which manifest fields are stage-invariant (e.g., record only the AI-Co language/compiler version string common to both stages; carry host compiler identity only in the external comparison evidence per §16.3, not in the manifest), and require identical output paths for the identity comparison (or exclude path fields from identity with an explicit rule). No normalization may be introduced.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer re-checks the corrected manifest schema and bootstrap identity wording; finding closes when the fields required for byte-identity are provably stage-invariant without normalization.

#### FIND-G2-04 (Major): Deterministic diagnostic ordering is undefined for records with null primary span

**Violated obligation:** DIAGNOSTIC-CONTRACT §1 guarantees "a deterministic total ordering of emitted diagnostics". §9 defines ordering by phase → `primary_span.file` → `primary_span.start.offset` → `code`. §3 states `primary_span` is "null only when no source location exists (e.g., link-level or build-level records)". §11.6/§11.7 define codes `AIC-O0701`, `AIC-O0702`, `AIC-BL0801`, `AIC-BL0802`, `AIC-BL0803` whose primary span is "null" or "null or derived span". §13 conformance obligations assert the emitted record set "in the deterministic order".

**Evidence:** The §9 ordering keys (`primary_span.file`, `primary_span.start.offset`) do not exist for null-span records, yet the contract itself defines codes that mandate null spans. No rule states where null-span records sort within their phase (before file-bearing records? after? tie-break by code?).

**Impact:** Build-level and link-level diagnostics cannot be deterministically ordered as required; conformance tests asserting exact record sequences for those phases have no spec basis. The headline "deterministic total ordering" guarantee is not total.

**Severity rationale:** Major — acceptance-contract defect in the normative diagnostic contract's core ordering guarantee for a defined record class.

**Required resolution condition:** Add an explicit ordering rule for null-span records (e.g., null-span records sort before all file-bearing records in the same phase, tie-broken by `code`), or define a deterministic canonical sort key for null spans.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected §9 ordering rule covers null-span records and is consistent with §3 and §11.6/§11.7.

#### FIND-G2-05 (Major): Allocator zero-size and size-fit/reuse semantics are undefined

**Violated obligation:** ADR-004 §49: "The specification must define the allocator's deterministic reuse rule and observable resource-exhaustion behavior." §15.1 makes the reuse order "part of the observable contract". §2.2/§2.3 forbid undefined/unspecified behavior. The required focus for this review explicitly includes "allocator zero-size and size-fit/reuse semantics".

**Evidence:** §15.1 defines `rt.mem.alloc_bytes(count: usize)` ("returns null if allocation fails") and the reuse order (reverse order of release) but does not define: (a) `alloc_bytes(0)` — valid pointer, null, or trap? (b) size-fit eligibility — can a freed block of size N satisfy a request of size M≠N (split/coalesce), or only exact matches? What happens to a freed 64-byte block when the next request is 8 bytes or 128 bytes? These choices change observable stale-pointer/reuse outcomes, which §15.1 declares part of the observable contract.

**Impact:** Two conforming implementations can produce different reuse outcomes for the same allocation/release sequence, violating the deterministic-reuse observable contract and the ADR-004 requirement. Zero-size allocation is a reachable input with no defined behavior.

**Severity rationale:** Major — explicit ADR-004 requirement partially unmet; observable contract underdetermined for a documented runtime API.

**Required resolution condition:** §15.1 must define: zero-size allocation behavior (result value and deallocation semantics), and the size-fit/reuse rule (exact-fit vs any-fit with splitting/coalescing, or a stated minimal-fit rule), with the rule expressed so reuse outcomes are deterministic and testable.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected §15.1 (and §8.3/§12.8 consistency); finding closes when zero-size and size-fit reuse are fully specified.

#### FIND-G2-06 (Major): Pointer arithmetic overflow has no defined semantics or trap

**Violated obligation:** §2.2: "Undefined behavior: forbidden in this specification. Every operation either has fully defined observable behavior, is rejected at compile time, or traps deterministically at runtime." §12.8(1): "Every operation on a pointer (dereference, arithmetic, comparison, conversion) has defined semantics in this specification or a deterministic trap." The required focus for this review explicitly includes "pointer arithmetic overflow".

**Evidence:** §12.5 defines `p + i` / `p - i` "scaled by sizeof(T)" and `p - q` as `(byte_address(p) - byte_address(q)) / sizeof(T)` with divisibility checks, but does not define behavior when the scaled byte-address computation overflows (e.g., `p` near the top of the address space plus a large `i`, or `i * sizeof(T)` overflow in the scaling arithmetic), nor when the `p - q` byte difference overflows `isize`. No trap code is assigned (the trap table §15.5 has no pointer-arithmetic-overflow entry).

**Impact:** A reachable operation can produce a wrapped or otherwise unspecified address value; no rejection or deterministic trap is specified, violating the language's central no-UB promise and §12.8(1).

**Severity rationale:** Major — direct violation of the core no-UB acceptance claim for a supported operation.

**Required resolution condition:** §12.5 (or §11.3 as applicable) must define pointer-arithmetic address-computation overflow: either checked with a deterministic trap (a new/assigned stable trap code) or a defined wrapping/defined-value rule consistent with the controlled-region model; the trap table and diagnostic registry must be updated accordingly.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected §12.5/trap-table wording; finding closes when pointer arithmetic overflow has an explicit defined outcome or assigned trap.

### Minor Findings

#### FIND-G2-07 (Minor): `rt` import resolution — bare `rt` module and auto-availability are ambiguous

**Violated obligation:** §6.5 defines reserved runtime module resolution for the four submodules (`rt.mem`, `rt.io`, `rt.proc`, `rt.trap`) and rejection codes AIC-N0207/N0208, but does not define `import rt;` (bare) nor whether runtime members are usable without an explicit import. §4.5 reserves "the module name `rt` and its submodules".

**Evidence:** §6.5 (lines 351–363) says `import rt.mem;` etc. "bind to the project-owned runtime modules defined in Section 15"; it does not state whether bare `import rt;` is valid (and what it binds) or which code rejects it, nor whether the runtime module is automatically in scope.

**Impact:** Program authors and implementers cannot determine the correct spelling/behavior for using the runtime without importing each submodule, and bare `rt` imports have no defined acceptance/rejection.

**Required resolution condition:** §6.5 should state whether `import rt;` (bare) is accepted (and what it binds) or rejected with an explicit code, and confirm that runtime members require explicit submodule imports (or are auto-available, with a stated rule).

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected §6.5/§4.5 wording; finding closes when bare-`rt` and auto-availability are defined.

#### FIND-G2-08 (Minor): §18.5 example annotation is misplaced and incomplete

**Violated obligation:** §18 conventions require error annotations to state the correct code and span; §20 claims examples/counterexamples are coherent.

**Evidence:** §18.5 (lines 1119–1129): the comment `// ERROR AIC-E0416: span "fn bad" (non-void, path without return)` is placed above `fn missing`, not `fn bad`, so a reader cannot tell which function the annotation describes (both functions have a no-return path). Additionally, `fn bad` case 1 (`return x;`) references `x` declared inside case 0's block — out of scope per §6.1 — an unannotated `AIC-N0202` error that the example neither shows nor explains.

**Impact:** Example annotations are ambiguous and incomplete, weakening the normative example set for the reachability/scope rule family.

**Required resolution condition:** Reposition/re-label the E0416 annotation, add the missing N0202 annotation (or restructure the example to avoid the out-of-scope reference).

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected §18.5 annotations match the actual error set.

#### FIND-G2-09 (Minor): Diagnostic schema-versioning rule conflicts with the v0.1.1 "schema remains 1" note

**Violated obligation:** DIAGNOSTIC-CONTRACT §11.10 states "a change that alters the meaning of a required field, changes required-field structure, or changes the code allocation rules requires a new schema version (e.g., `"2"`)".

**Evidence:** The v0.1.1 note (same section) states `recovery` was made mandatory on error/trap records and `corrections` became structured objects, yet schema remains `"1"`. The rationale (v0.1.0 draft never accepted/implemented) is reasonable, but the rule text contains no draft-exemption clause; the two statements are in tension within the same section.

**Impact:** A future implementer/consumer cannot tell from the rule text alone when a schema version bump is actually required; the documented rule and the documented practice disagree.

**Required resolution condition:** Amend §11.10's versioning rule to include the draft-exemption (never-accepted/never-emitted drafts do not trigger a bump) or bump schema to `"2"`; keep the registry and example records consistent.

**Owner:** Planner (routes through Coordinator).

**Closure check:** Reviewer verifies the corrected versioning rule and records are mutually consistent.

### Suggestions

#### SUG-G2-01 (Suggestion): Timestamp wording could be tightened to match ADR-001

§16.2 says "compiler-controlled timestamps are zero (or derived solely from input content, recorded as such)". ADR-001 §107 states "Compiler-controlled timestamps are zero". The content-derived option is deterministic and within ADR-001's intent, but the wording extends the ADR literally; consider aligning the spec text to "zero" with an explicit note, or recording the elaboration in §21 history.

#### SUG-G2-02 (Suggestion): `AIC-U0000` code digits do not follow the documented phase-group mapping

DIAGNOSTIC-CONTRACT §5 says the first two digits of `<NNNN>` are a phase group with "09 user"; `AIC-U0000` uses `00` (lex group). This is cosmetic (the mapping is "for human readability") and does not affect stability, but the mapping table should note that the `U` class is exempt, or use `U09xx`.

#### SUG-G2-03 (Suggestion): Document the manifest "compiler version/identity" field semantics

To support FIND-G2-03 resolution, consider defining "compiler version/identity" as the AI-Co language/spec version string (identical across stages) rather than the producing executable's identity.

---

## FIND-001..005 closure assessment (from initial architecture review)

- **FIND-001 (normalization spec):** Claimed closed in §16.2–16.3. **Closure incomplete** — the byte-identity gate text exists, but FIND-G2-02 and FIND-G2-03 leave the manifest identity contract undefined.
- **FIND-002 (external linker time bound):** Closed via M1/M2 milestone boundaries in §16.5. ✓
- **FIND-003 (function-pointer necessity):** Closed via the recorded evaluation in §17.3 (static dispatch suffices; trigger preserved). ✓
- **FIND-004 (runtime contract):** Claimed closed in §15/§15.7–15.8. **Closure incomplete** — FIND-G2-05 (allocator zero-size/size-fit) and FIND-G2-06 (pointer arithmetic overflow) are runtime-contract gaps.
- **FIND-005 (diagnostic schema):** Claimed closed in the diagnostic contract. **Closure incomplete** — FIND-G2-04 (null-span ordering) and FIND-G2-09 (schema-version rule tension) remain.

## Planner self-review closure

The Planner self-review's single correction (ADR-004 §63 obligation added to spec §14.3) is verified present at HEAD. The self-review's other assertions (temporal/wrapping/Windows alignment, supersession, traceability) were re-verified independently and are accurate, except where the findings above identify additional gaps the self-review did not surface (notably FIND-G2-02/03/05/06, which sit in areas the self-review assessed as complete).

## Repository-state observations (not findings against the reviewed artifacts)

- Untracked stub `docs/reviews/REVIEWER-CONFORMANCE-REVIEW-LANGUAGE-SPEC-2026-08-08.md` (3 lines) is a leftover from a prior attempt on the superseded cards; it is superseded by this report. Coordinator may remove it or the Historian may archive it.
- Untracked stray file `C:tmpcheck_jsonl.py` at the repository root (likely an accidental Windows-path artifact of an earlier ad-hoc JSONL check) is not part of the spec set and should be removed or moved out of the repository root by the Coordinator; it contains no secrets.
- The missing prior independent Reviewer Pass report (`docs/reviews/ADR-003-LANGUAGE-SPEC-REVIEW-2026-08-08.md`) remains uncommitted; this review independently covers the same ground for the amended v0.1.1 set.

---

## Verdict

**Changes required.**

**Rationale:** The corrected v0.1.1 set is substantially coherent: ADR-004 alignment is accurate, the §14.3 Planner correction is present, supersession/authority handling is clean, the code registry is internally consistent, JSONL examples parse, no secrets/PII/license issues were found, and no forbidden artifact references exist. However, six Major findings (FIND-G2-01..06) block approval: normative examples contradict the conversion rules; the build-manifest schema has an unspecified self-hash rule and stage/host-provenance fields that conflict with raw byte identity; diagnostic ordering is undefined for null-span records; allocator zero-size and size-fit/reuse semantics are undefined; and pointer arithmetic overflow has no defined outcome or trap. These defects touch the specification's central claims (no undefined behavior, deterministic outputs, deterministic diagnostics, bootstrap byte identity) and the accepted ADR-004 §49 allocator requirement.

**Gate state:** Specification and diagnostic contract remain **Proposed**. **The Main Designer may NOT accept the specification set at this time, and implementation planning must NOT begin** until the Major findings are corrected and the amended set passes re-review. FIND-001/FIND-004/FIND-005 closure claims are incomplete pending the associated corrections.

**Limits of assurance:** This verdict covers the reviewed documents only. No compiler exists; behavioral conformance, trap behavior, and bootstrap byte identity are not executable yet and were not verified beyond document-level determinism/consistency checks (disclosed above). This review is not a warranty of defect absence.

**Residual risk owner:** Planner (specification corrections); Main Designer (architectural acceptance, deferred); Coordinator (routing and gate state); Marcel (Human Sponsor) for any purpose-affecting or risk-acceptance questions — none raised by this review.

**Remediation/re-review condition:** Planner to correct FIND-G2-01..09 (six Major, three Minor) and consider the Suggestions, updating §21 review history and the diagnostic contract version notes as appropriate; then the amended set must be re-reviewed by the Reviewer (gate 2) before Main Designer architectural acceptance (gate 3). Re-review is required because the Major findings affect acceptance-critical claims.

## Handoff to Coordinator

- **Verdict:** Changes required (6 Major, 3 Minor, 3 Suggestions).
- **Gate state:** Specification and diagnostic contract remain Proposed; implementation remains blocked; Main Designer acceptance and implementation planning must not proceed until Major findings are corrected and re-reviewed.
- **Remediation routing:** All findings → Planner (specification/diagnostic-contract corrections), routed through Coordinator. Repo-hygiene items (stray stub, stray `C:tmpcheck_jsonl.py`, missing prior review report) → Coordinator/Historian.
- **Documentation state:** This report delivered as `docs/reviews/LANGUAGE-SPEC-v0.1.1-REVIEW-2026-08-08.md` (replacing the stale 13-line stub that referenced the pre-correction commit) and committed locally on `main` (not pushed; push handled by downstream task).
- **Review evidence:** local commit of this report; deterministic checks recorded above; exact references to lines in the reviewed artifacts.

*End of review report*
