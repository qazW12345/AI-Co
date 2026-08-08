# AI-Co Specification Open Questions and Resolution Record (Proposed v0.1.1)

**Status:** No implementation-blocking open questions remain. All items escalated during v0.1.0 drafting are resolved or deferred by Human Sponsor approval recorded in ADR-004; the entries below are the monitored resolution record, not an open-question list.
**Owner:** Planner
**Decision owner per item:** Marcel, Human Sponsor (as recorded in ADR-004)
**Date:** 2026-08-08
**Companion to:** `spec/AI-CO-LANGUAGE-SPECIFICATION.md` v0.1.1 (Proposed) and `spec/DIAGNOSTIC-CONTRACT.md` v0.1.1 (Proposed)

**Authority note:** the substantive choices below are Human Sponsor approvals recorded in `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (Accepted). ADR-003 is retained as superseded history only. No Planner artifact creates an approval; ADR-004 is the authoritative record of these resolutions (ADR-004, Context; Constitution: silence is not approval).

---

## OQ-001: Depth of use-after-free / invalidated-allocation detection

**Status:** Resolved — ADR-004 "Raw-pointer temporal baseline" (Human Sponsor approval, 2026-08-08).
**Recorded in:** Spec §12.8 (raw-pointer contract) and §15.1 (allocator reuse rule).
**Supersedes:** ADR-003's checked raw-pointer boundary alternative and the targeted part of ADR-002 requiring every use of an invalidated allocation to trap.

**Question:** How far must the minimal runtime go to guarantee that use of an invalidated allocation is a deterministic trap?

**Resolution (ADR-004):** the minimal language guarantees deterministic allocator behavior but does not guarantee detection or a trap for every stale pointer read or write. The project-owned allocator tracks every allocation and deterministically traps duplicate release and release of a pointer that is not the start of a live allocation; deallocation overwrites the allocation with byte `0xDD` before the block becomes eligible for deterministic reuse; freed blocks remain under the project allocator's controlled address space until deterministic reuse or process exit; before reuse a stale access observes or modifies the poisoned bytes, after reuse it accesses the current allocation under ordinary raw-pointer rules, and an inaccessible address traps as invalid address/access. The compiler may not exploit C-style lifetime undefined behavior. The language explicitly does not claim complete temporal memory safety.

**Specification consequence:** §12.8 states the accepted temporal baseline and compiler obligations exactly; §15.1 defines the deterministic reuse rule (reverse order of release) and resource-exhaustion behavior (`alloc_bytes` returns `null`).

**Monitor:** no follow-up item. Stronger temporal checking is a future safety feature that requires a new accepted ADR; it may strengthen detection but may not retroactively justify arbitrary behavior in the minimal language.

---

## OQ-002: Explicit wrapping-arithmetic built-in form

**Status:** Deferred — ADR-004 "Wrapping arithmetic baseline" (Human Sponsor approval, 2026-08-08).
**Recorded in:** Spec §11.3.
**Supersedes:** ADR-003's dedicated wrapping built-ins and the targeted part of ADR-002 requiring same-width explicit wrapping arithmetic in the minimal language.

**Question:** Is `wrap<T>(expr)` applied to a checked operator result sufficient as the named explicit wrapping operation, or must dedicated wrapping arithmetic built-ins exist?

**Resolution (ADR-004):** `wrap<T>(value)` remains an explicit modulo/truncating conversion after its operand has been evaluated under ordinary checked semantics; it does not establish a wrapping evaluation context. AI-Co v0.1.0 provides no same-width wrapping addition, subtraction, multiplication, or negation operation. Dedicated wrapping arithmetic built-ins are deferred; the decision must be reopened if bootstrap or self-hosting work demonstrates a concrete need, and it may be reconsidered later independently. Adding such operations is a public language-surface change requiring an accepted ADR and conformance coverage.

**Correction applied in v0.1.1:** the v0.1.0 draft's framing that `wrap<T>(a op b)` expresses wrapping arithmetic is removed. Spec §11.3 now states that `a op b` is a checked operation and rejects or traps before the conversion applies, so `wrap<T>(a op b)` is not same-width wrapping arithmetic.

**Monitor:** deferred, not excluded. The revisit trigger is a concrete bootstrap or self-hosting need (notably `u64`/`usize` hash arithmetic). Any future addition is tracked as a public language-surface change requiring an accepted ADR and conformance coverage.

---

## OQ-003: Target-platform OS version (Windows 10 end-of-support)

**Status:** Resolved — ADR-004 "Windows 10 bootstrap baseline" (Human Sponsor approval, 2026-08-08).
**Recorded in:** Spec §14.3; runtime-facing Windows calls remain bounded by Spec §15.
**Supersedes:** ADR-003's development-host-versus-release distinction to the extent that the bootstrap baseline is now pinned.

**Question:** Does the initial target remain Windows 10 22H2 (past end of support as of 2026-08-08), or move to a supported Windows version before the first release gate?

**Resolution (ADR-004):** Windows 10 22H2 x64 is the pinned development-host and execution baseline for the Stage 0/1/2 bootstrap line. The project explicitly records that this baseline no longer receives ordinary OS updates. Runtime-facing Windows calls must be enumerated and documented against this baseline. Compatibility with Windows 11 or later is desirable but not a v0.1.0 conformance guarantee. This bootstrap approval does not itself approve publication of executable releases, network-facing production deployment, or acceptance of additional material security risk; those remain separate Human Sponsor gates.

**Specification consequence:** §14.3 states the pinned baseline, the no-updates disclosure, and the non-guarantee of newer-Windows compatibility; §15.7 binds runtime-facing calls to the internal Microsoft x64 convention; release remains a separate Human Sponsor gate.

**Monitor:** no follow-up item within the specification. The executable-release gate remains a separate Human Sponsor decision.

---

## Resolved-and-monitored items (not open)

- **Function pointers (review FIND-003):** Evaluated in Spec §17.3; not necessary for the minimal self-hosting compiler (static dispatch design basis documented); the trigger to return the exact capability gap to the Main Designer is preserved if implementation planning proves otherwise. Not an open question.
- **Bootstrap normalization (review FIND-001):** Per ADR-001 §99–109 and Spec §16.2–16.3: no normalization; raw byte identity of COFF objects and build manifests; deterministic-linker PE identity; zero/canonical metadata rules; full comparison-input evidence. Not an open question.
- **External-linker time bound (review FIND-002):** Per ADR-001 and Spec §16.5 (milestones M1/M2). Not an open question.
- **Diagnostic schema/code registry (review FIND-005):** `DIAGNOSTIC-CONTRACT.md` v0.1.1 (schema version 1, code registry, compatibility rules). Not an open question.
- **Runtime contract completeness (review FIND-004):** Spec §15, §15.7–15.8 (calling convention, ABI obligations, complete compiler-emitted runtime call list, source-visible versus compiler-emitted distinction). Not an open question.

---

## Monitored resolutions (deferred by ADR-004)

- **Wrapping arithmetic built-ins (OQ-002):** deferred with an evidence-triggered revisit, tracked above. The deferral is not an open question and not a permanent exclusion; it is a monitored resolution that will be reopened only through the trigger or an accepted ADR.

---

## Closure discipline

- Each item closes only by an explicit decision recorded in an accepted record; silence is not approval (Constitution §3.1). For these items, the accepted record is ADR-004 (Human Sponsor approval), and the decisions are attributed only to Marcel, Human Sponsor, and ADR-004 — never to the Main Designer or to any nonexistent decision note.
- The deleted, never-committed Main Designer decision-note artifact is not referenced anywhere in the v0.1.1 specification set and is not a source of any resolution.
- Deferred items are monitored here until reopened or closed; they do not block the v0.1.1 Proposed specification's remaining gates (Planner self-review, independent Reviewer conformance review, Main Designer acceptance). Implementation remains blocked until those gates pass.
