# ADR-004: Apply Human Sponsor Resolutions to Bootstrap Open Questions

## Status

Accepted

## Supersedes

ADR-003's decisions on checked raw-pointer access, dedicated wrapping arithmetic built-ins, and release-baseline deferral. ADR-003 remains historical evidence of the Main Designer's interim containment response before Marcel clarified that the substantive alternatives had already received Human Sponsor approval.

This ADR also supersedes only these targeted parts of ADR-002:

- the unconditional requirement that every use of an invalidated allocation trap; and
- the requirement that same-width explicit wrapping arithmetic exist in the minimal language.

It pins the bootstrap operating-system baseline more narrowly than ADR-001's generic Windows x86-64 target. All other ADR-001 and ADR-002 decisions remain in force.

## Context

Decision owner for the three escalated choices: Marcel, Human Sponsor. Steward and architecture reconciler: Main Designer.

The Proposed v0.1.0 specification escalated three questions: the depth of use-after-free detection, the form of explicit wrapping arithmetic, and the Windows 10 end-of-support baseline. The Planner recorded substantive choices but incorrectly labeled them as Main Designer decisions and conflated Marcel with the Main Designer role. The Main Designer contained the apparent authority defect and issued ADR-003 with alternative decisions. Marcel then explicitly clarified that the Planner intended to record **Human Sponsor approval for the substantive choices**. That current Human Sponsor direction outranks the interim ADR-003 choices and is recorded here with corrected authority attribution.

No Planner artifact creates the approval. Marcel's explicit correction is the approval source. The Planner's analysis is evidence and specification input only.

Alternatives considered are preserved in ADR-003 and the Planner's Proposed open-question analysis. The Human Sponsor-approved selections are:

1. minimal allocator registry and deterministic poisoning without a guarantee that every stale access traps;
2. no same-width wrapping arithmetic operation in v0.1.0, with evidence-triggered reconsideration;
3. Windows 10 22H2 x64 as the bootstrap development and execution baseline, with its lack of ordinary OS updates disclosed.

## Decision

### Raw-pointer temporal baseline

AI-Co v0.1.0 guarantees deterministic allocator behavior but does not guarantee detection or a trap for every stale pointer read or write.

The normative direction is:

- the project-owned allocator tracks every allocation and deterministically traps duplicate release and release of a pointer that is not the start of a live allocation;
- deallocation overwrites the allocation with byte `0xDD` before the block becomes eligible for deterministic reuse;
- freed blocks remain under the project allocator's controlled address space until deterministic reuse or process exit; the allocator must not expose host-allocator or OS-dependent stale-access semantics as a language rule;
- before reuse, a stale access to still-accessible freed storage observes or modifies the poisoned bytes at that address;
- after deterministic reuse, the address denotes the new allocation and a stale pointer accesses that current allocation according to ordinary raw-pointer rules;
- an address that is not accessible according to the runtime's controlled region model traps as invalid address/access;
- the compiler may not assume that a stale pointer access is unreachable, optimize on C-style lifetime undefined behavior, or produce arbitrary behavior from it;
- the language explicitly does not claim complete temporal memory safety.

The specification must define the allocator's deterministic reuse rule and observable resource-exhaustion behavior. Stronger temporal checking remains a future safety feature requiring an ADR; it may strengthen detection but may not retroactively justify arbitrary v0.1.0 behavior.

### Wrapping arithmetic baseline

`wrap<T>(value)` remains an explicit modulo/truncating conversion after its operand has been evaluated under ordinary checked semantics. It does not establish a wrapping evaluation context.

AI-Co v0.1.0 provides no same-width wrapping addition, subtraction, multiplication, or negation operation. The misleading expression `wrap<T>(a op b)` must not be described as same-width wrapping arithmetic when `a op b` can reject or trap before conversion.

Dedicated wrapping arithmetic built-ins are deferred. The decision must be reopened if bootstrap or self-hosting work demonstrates a concrete need, and it may be reconsidered later independently. Adding such operations is a public language-surface change requiring an accepted ADR and conformance coverage.

### Windows 10 bootstrap baseline

Windows 10 22H2 x64 is the pinned development-host and execution baseline for the Stage 0/1/2 bootstrap line. The project explicitly records that this operating-system baseline no longer receives ordinary OS updates.

Runtime-facing Windows calls must be enumerated and documented against this baseline. Compatibility with Windows 11 or later is desirable but not a v0.1.0 conformance guarantee.

This bootstrap approval does not itself approve publication of executable releases, network-facing production deployment, or acceptance of additional material security risk. Those remain separate Human Sponsor gates. The current project scope remains public, non-sensitive hobby development on the observed machine.

## Consequences

### Positive

- Authority attribution now correctly distinguishes Marcel, Human Sponsor, from the Main Designer.
- The minimal allocator avoids mandatory per-dereference liveness instrumentation and lifetime checking.
- Stale-pointer behavior is constrained by a project-owned deterministic allocator model rather than inherited C undefined behavior.
- No speculative wrapping built-ins become permanent language surface before self-hosting evidence exists.
- The bootstrap baseline matches the available, verified machine.

### Negative

- AI-Co v0.1.0 does not detect every use-after-free and does not provide complete temporal memory safety.
- Deterministic poisoning/reuse can make stale pointer bugs produce plausible data rather than immediate diagnostics.
- Programs needing same-width wrapping arithmetic, especially `u64`/`usize` hash operations, have no direct operation in v0.1.0.
- Windows 10 22H2 is outside ordinary support and is not evidence of compatibility with newer Windows releases.
- ADR-003 was published before the Human Sponsor clarification and must remain as superseded history.

### Risks and controls

- **Raw-pointer defects are mistaken for safety:** documentation and diagnostics must state the temporal-safety limit prominently; no memory-safety claim is permitted.
- **Host allocator leaks nondeterminism:** the project-owned allocator defines poisoning, retention, reuse order, and failure behavior; host behavior is not normative.
- **Wrapping is smuggled through conversions:** `wrap<T>` evaluates its operand normally; same-width overflow still rejects or traps.
- **Windows baseline becomes a general release approval:** executable release and deployment remain Human Sponsor gates.
- **Role attribution fails again:** every claimed approval or decision must cite the actual role and directly traceable approving record; a role may not infer another role's decision from analysis text.

### Follow-up implications

The untracked duplicate `docs/adr/ADR-003-bootstrap-open-question-resolutions.md` must not be published because ADR-003 already exists. Its substantive Human Sponsor-approved choices are preserved here under ADR-004.

The Planner must revise the Proposed specification and diagnostic contract to align with this ADR, remove false Main Designer-decision attribution and nonexistent decision-note references, and correct the independent structural contradictions identified during Main Designer inspection. The corrected draft requires independent Reviewer conformance review before Main Designer acceptance. Implementation remains blocked.
