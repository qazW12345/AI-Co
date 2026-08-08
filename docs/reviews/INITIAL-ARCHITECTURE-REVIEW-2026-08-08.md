# AI-Co Initial Architecture and Language Semantics Review

**Review date:** 2026-08-08
**Reviewer:** reviewer profile
**Artifact version reviewed:** Git commit eac06de (HEAD)
**Governing baseline:**
- Sneedworks Constitution v1.0.0
- Sneedworks Operations Manual v1.1.0
- AI-Co PROJECT_CHARTER.md v0.1.0 (accepted 2026-08-08)
- ADR-001: Bootstrap Compiler and Initial Target Architecture (Accepted)
- ADR-002: Minimal Core Language Semantics (Accepted)
- research/ENVIRONMENT_BASELINE_2026-08-08.md (Complete, Researcher self-review passed)
- Marcel's accepted base direction as captured in the charter

**Independence declaration:** This review was conducted by the reviewer profile with no prior material authorship of the reviewed artifacts. No conflict of interest is present.

**Scope:** Independent assessment of AI-Co's initial architecture and minimal core semantic decisions before implementation can be made ready. Decision owner: Main Designer.

**Exclusions:** Authoring remediation, changing architecture/specification, implementing code, routine board administration, accepting material risk, or treating public visibility as a software release approval.

---

## 1. Evidence Inspected

| Artifact | Version/Identity | Role |
|---|---|---|
| PROJECT_CHARTER.md | v0.1.0, accepted 2026-08-08 | Project purpose, governing direction, constraints |
| docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md | Accepted | Architecture decisions: bootstrap, compiler stages, target, diagnostics, bootstrap contract |
| docs/adr/ADR-002-minimal-core-language-semantics.md | Accepted | Language semantics: types, declarations, expressions, memory, modules, runtime |
| research/ENVIRONMENT_BASELINE_2026-08-08.md | Complete, 2026-08-08 | Machine evidence: OS, CPU, RAM, storage, toolchains, PATH hazards, smoke tests |
| .hermes.md | Current | Project context summary |
| README.md | Current | Public project description, license disclaimer |
| .gitignore | Current | Repository exclusion rules |
| Git history (3 commits) | eac06de, e971c44, 32e66fa | Repository state verification |

All artifacts were inspected on-disk at `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`. Claims were traced to the Researcher report (ENVIRONMENT_BASELINE) and the accepted ADRs. No reliance on chat summaries.

---

## 2. Findings

### 2.1 Critical Findings

None.

### 2.2 Major Findings

None.

### 2.3 Minor Findings

#### FIND-001: ADR-001 §99–109 — Bootstrap equivalence contract normalization underspecified
**Violated obligation:** ADR-001 states "Stage 1 and Stage 2 outputs must be byte-identical after any explicitly specified normalization. The preferred design is to eliminate timestamps, random identifiers, absolute paths, unstable iteration order, and other nondeterministic fields so normalization is empty or mechanically trivial." However, no normalization specification exists, and the Planner has not yet produced one.
**Evidence:** ADR-001 lines 99–109; PROJECT_CHARTER.md lines 30–36 (requires deterministic equivalence); ADR-002 does not address compiler-output normalization.
**Impact:** Without an explicit, accepted normalization spec, the byte-identity gate is not falsifiable. Implementation could produce outputs that differ in acceptable ways (e.g., COFF timestamps) and be incorrectly rejected, or differ in unacceptable ways and be incorrectly accepted.
**Severity rationale:** Major defect in the acceptance contract; however, the Planner's specification work is explicitly pending (ADR-001 §148, ADR-002 §131). This finding tracks the gap so it is not overlooked when the specification is drafted.
**Required resolution condition:** Planner must produce a normative normalization specification as part of the language specification or a dedicated bootstrap-contract document, listing every field that may differ between Stage 1 and Stage 2 outputs and the exact normalization rule for each. The spec must be reviewed and accepted before implementation readiness.
**Owner:** Planner (routes through Coordinator).
**Closure check:** Reviewer re-reviews the normalization spec when produced; finding closes when spec is accepted and covers all COFF/PE fields that can vary non-deterministically.

#### FIND-002: ADR-001 §56–62 — External linker dependency not time-bounded
**Violated obligation:** ADR-001 permits Microsoft `link.exe` or LLVM `lld-link` for final linking during early bootstrap and states "A later AI-Co utility may replace the external linker, but that replacement is not required before the first self-hosting proof unless the Planner determines it is necessary."
**Evidence:** ADR-001 lines 56–62; PROJECT_CHARTER.md line 25 (self-sufficiency); ADR-002 §112–116 (runtime boundary).
**Impact:** The self-hosting proof relies on an external linker that is not part of the AI-Co compiler core. If the linker's behavior changes or becomes unavailable, the bootstrap line breaks. The "not required before first self-hosting proof" language leaves the dependency unbounded in time.
**Severity rationale:** This is a known and accepted bootstrap constraint (ADR-001 §133), but the lack of a time bound or explicit acceptance criterion for linker replacement creates a latent architectural debt. Minor because the ADR discloses it and the charter accepts temporary external linking.
**Required resolution condition:** Planner must either (a) define an acceptance criterion for when the self-hosted linker becomes required, or (b) record an explicit Main Designer decision that the external linker remains acceptable for the defined initial milestone, with a follow-up ADR for replacement.
**Owner:** Planner / Main Designer.
**Closure check:** Explicit milestone criterion or follow-up ADR recorded and accepted.

#### FIND-003: ADR-002 §71 — Function-pointer types deferred without decision trigger
**Violated obligation:** ADR-002 states "Function-pointer types are deferred unless the Planner demonstrates they are essential for the minimal self-hosting compiler; such a finding returns to the Main Designer as an architecture question."
**Evidence:** ADR-002 line 71; PROJECT_CHARTER.md lines 30–36 (self-hosting compiler requirements).
**Impact:** A self-hosting compiler typically requires function pointers for vtables, callbacks, or plugin-style architectures. Deferring without a clear trigger risks a late architecture change if the Planner discovers they are essential.
**Severity rationale:** The deferral is explicit and has a return path, but the trigger ("Planner demonstrates they are essential") is passive. A self-hosting compiler in C-like languages almost always needs indirect calls. Minor because the return path exists.
**Required resolution condition:** Planner must explicitly evaluate function-pointer necessity during specification drafting and either (a) include them in the minimal language with a justification, or (b) demonstrate a viable self-hosting compiler design without them. The evaluation must be recorded in the specification or a Planner note.
**Owner:** Planner.
**Closure check:** Specification includes function-pointer decision with rationale; or Planner note records the evaluation outcome.

#### FIND-004: ADR-002 §113–116 — Runtime/platform boundary not fully specified
**Violated obligation:** ADR-002 states "Platform-specific operating-system calls remain behind explicit runtime modules. The initial language does not promise a stable foreign-function interface or Windows ABI compatibility. The compiler and runtime may use accepted Windows conventions internally without making them source-language guarantees."
**Evidence:** ADR-002 lines 112–116; ADR-001 lines 56–62 (Windows x86-64 PE/COFF target).
**Impact:** The runtime boundary is described in principle but the exact set of runtime capabilities, their signatures, and the calling convention used by the compiler for runtime calls are not specified. This affects the compiler's code generation for runtime calls and the bootstrap contract.
**Severity rationale:** The ADR acknowledges this is a specification task (ADR-002 §129–131). Minor because the gap is acknowledged and the Planner owns it.
**Required resolution condition:** Planner must specify the minimal runtime API (functions, signatures, calling convention, ABI) as part of the normative specification. The spec must be sufficient for the compiler to emit correct calls and for the Stage 0/1/2 equivalence test to be meaningful.
**Owner:** Planner.
**Closure check:** Normative specification includes complete runtime contract; Reviewer verifies it covers all compiler-emitted runtime calls.

#### FIND-005: ADR-001 §81–97 — Diagnostic schema versioning and stability not yet defined
**Violated obligation:** ADR-001 requires "stable diagnostic code and schema version" in every diagnostic record (§87) but no schema version or code registry exists yet.
**Evidence:** ADR-001 lines 81–97; ADR-002 §117–122 (compile-time/runtime failure model).
**Impact:** Structured diagnostics are a normative compiler output (ADR-001 §83). Without a versioned schema and stable code assignments, agent consumers cannot rely on diagnostic codes across compiler versions, defeating a core project goal.
**Severity rationale:** The ADR establishes the requirement; the schema is Planner work. Minor because the requirement is explicit and the Planner owns the specification.
**Required resolution condition:** Planner must define the initial diagnostic schema (version 1), stable code allocation policy, and code registry as part of the normative specification or a companion diagnostic contract document.
**Owner:** Planner.
**Closure check:** Schema v1 and code registry accepted and referenced in the specification.

### 2.4 Suggestions

#### SUGG-001: ADR-001 §21–29 — Document rejected alternatives more completely
The alternatives analysis (C++, Python/TypeScript prototype, Rust/Zig/Go, LLVM backend, C backend) is adequate but could explicitly note why each was rejected relative to the charter's six governing directions. This would strengthen the ADR's traceability to Marcel's direction.

#### SUGG-002: ADR-002 §123–128 — Feature exclusions list could reference the charter non-goals explicitly
The minimal-feature exclusions list (floating point, unions, generics, etc.) aligns with PROJECT_CHARTER.md §53–61 but cross-references would improve auditability.

#### SUGG-003: ENVIRONMENT_BASELINE §206 — Windows 10 end-of-support risk
The baseline notes Windows 10 is past end-of-support as of the evidence date. The project should record a Main Designer decision on whether the initial target remains Windows 10 22H2 or moves to a supported Windows version (11, Server 2025) before the first release gate. This is a strategic target-platform decision, not an architecture defect.

#### SUGG-004: Repository license clarity
README.md line 46 states "No license has been selected yet. The repository is publicly visible, but no permission grant should be inferred until Marcel approves a license." This is clear and correct. Consider adding a `LICENSE` file with a "NO LICENSE GRANTED" header to make the absence explicit in file listings and GitHub UI.

---

## 3. Architecture Gap Assessment

**Question:** Does any finding make Planner requirements confidence Low or self-hosting implausible?

**Answer:** No. The Minor findings identify specification gaps that the Planner must close, but they do not represent fundamental architecture flaws. The architecture is coherent:

- **Bootstrap determinism:** The Stage 0→1→2 contract is well-defined and falsifiable once the normalization spec exists (FIND-001).
- **Diagnostic architecture:** JSON Lines with causal chains is appropriate for agent consumers; schema versioning is a specification task (FIND-005).
- **Dependency posture:** No LLVM library, parser generator, or third-party runtime in the compiler core — consistent with self-sufficiency. The external linker is a disclosed, bounded bootstrap dependency (FIND-002).
- **Target choice:** Windows x86-64 PE/COFF with SSE2 baseline is evidenced by the Researcher's smoke tests (ENVIRONMENT_BASELINE §8, tests 1–4). AVX2 is available but not required.
- **C-similarity trade-offs:** ADR-002's departures (canonical declarations, explicit conversions, checked arithmetic, defined evaluation order, no preprocessor, ASCII identifiers) are each justified by ambiguity elimination or diagnostic goals. No departure appears gratuitous.

**Self-hosting plausibility:** The minimal feature set (ADR-002) appears sufficient for a C-like compiler: functions, structs, arrays, slices, pointers, explicit memory, modules, I/O runtime. The main risk is function-pointer necessity (FIND-003), which the Planner must evaluate. If function pointers are essential, adding them is a localized change that does not cascade into other semantics.

---

## 4. Public Repository Content Check

| Check | Result |
|---|---|
| Secrets (API keys, tokens, passwords) | None found. `.gitignore` excludes `.env`, `*.key`, `*.token`, `GitHub-API.txt`. Researcher baseline explicitly sanitized auth output and did not read a credential-bearing file in workspace root. |
| Unnecessary PII | None found. No author names, emails, or personal identifiers beyond the generic "Sneedworks" git author. |
| License grant clarity | README.md line 46 explicitly states no license selected and no permission grant inferred. No LICENSE file exists. This is correct for a pre-license repository. |

---

## 5. Verdict

**Verdict: Approved with Minor findings**

**Rationale:** The reviewed architecture (ADR-001, ADR-002) is internally consistent, faithful to Marcel's direction as captured in the charter, and sufficiently evidenced by the Researcher's environment baseline. No Critical or Major findings block approval. The five Minor findings (FIND-001 through FIND-005) identify specification gaps that the Planner must close before implementation readiness, but they do not invalidate the architecture decisions themselves. Each has a clear resolution path and owner.

**Limits of assurance:** This verdict covers the architecture and minimal semantic decisions only. It does not cover:
- The normative language specification (not yet drafted)
- Implementation correctness (not yet begun)
- The normalization specification for bootstrap equivalence (FIND-001)
- The runtime contract (FIND-004)
- The diagnostic schema (FIND-005)

**Residual risk owner:** Main Designer (architecture), Planner (specification gaps).

**Remediation/re-review condition:** The Minor findings do not require re-review of this architecture review. They will be verified when the Planner produces the normative specification and bootstrap-contract documents. The Reviewer will review those artifacts separately per the Planner's task routing.

---

## 6. Handoff to Coordinator

- **Verdict:** Approved with Minor findings
- **Gate state:** Architecture review passed; implementation remains blocked pending Planner specification and implementation-ready acceptance criteria
- **Remediation routing:**
  - FIND-001, FIND-004, FIND-005 → Planner (specification work)
  - FIND-002 → Planner / Main Designer (milestone criterion or follow-up ADR)
  - FIND-003 → Planner (function-pointer evaluation)
- **Documentation state:** This review report is the durable record. Findings are recorded herein and should be linked from the Planner's specification tasks.

---

*End of review report*