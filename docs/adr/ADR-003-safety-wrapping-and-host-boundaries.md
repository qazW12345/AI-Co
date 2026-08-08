# ADR-003: Resolve Minimal Safety, Wrapping, and Host-Baseline Boundaries

## Status

Superseded by ADR-004

## Supersedes

The unresolved or incorrectly attributed positions recorded in the unapproved Planner artifacts `spec/OPEN-QUESTIONS.md` and `docs/decisions/MAIN-DESIGNER-DECISIONS-2026-08-08.md`. It refines, but does not otherwise supersede, ADR-001 or ADR-002.

## Context

Decision owner: Main Designer under Marcel's delegated technical authority. No decision in this ADR changes project purpose, authorizes a public software release, or accepts material security risk.

The Planner's Proposed v0.1.0 specification correctly surfaced three architecture questions, but then incorrectly marked them as resolved by the Main Designer and created a purported Main Designer decision note without an actual Main Designer decision. That attribution is invalid. The note also conflated Marcel, the Human Sponsor, with the Main Designer role and claimed direction that was not given in the cited form. The invalid note was never committed or published and is removed rather than accepted as organizational history; the durable task record preserves the evidence of the error.

The underlying questions still require real decisions before the Proposed specification can be reviewed for acceptance:

1. whether the minimal pointer model guarantees deterministic detection of invalidated allocation use;
2. how explicit wrapping arithmetic is expressed without context-sensitive evaluation;
3. whether the development host's Windows 10 version becomes a supported release target or accepted security baseline.

Alternatives considered for pointer safety:

1. **Allocator registry and poison only.** Low implementation cost, but stale accesses may silently observe poisoned or reused storage and directly contradict the deterministic-trap architecture.
2. **Never reuse released heap addresses, without per-access checks.** Avoids some reuse ambiguity but still cannot guarantee that each invalid access traps.
3. **Checked raw-pointer boundary.** Compiler-inserted access validation plus a live-region registry, heap quarantine, and restricted automatic-storage pointer escape. This adds runtime and compiler work but makes the supported operation deterministic and diagnostic.
4. **Remove raw pointers.** Stronger safety but makes a C-based self-hosting systems language substantially less direct and conflicts with the accepted minimal feature set.

Alternatives considered for wrapping arithmetic:

1. make operators inside `wrap<T>(...)` context-sensitive;
2. use dedicated named wrapping arithmetic built-ins;
3. require widening followed by `wrap<T>`;
4. omit same-width wrapping until later.

Alternatives considered for the operating-system baseline:

1. accept Windows 10 22H2 as the supported release baseline despite end of support;
2. require a supported Windows release immediately;
3. continue using the observed Windows 10 machine as a development host while keeping the release contract at the Windows x86-64/PE/COFF and explicitly documented runtime-API level until a later release decision.

## Decision

### Deterministic checked raw-pointer boundary

AI-Co v0.1.0 retains explicit raw pointers, but every source-level dereference and raw-pointer memory access is checked against a project-owned live-region model before the access occurs.

The minimal contract requires:

- compiler-inserted validation of address, extent, alignment, mutability where applicable, and liveness for every raw-pointer load, store, and byte-range runtime operation;
- a runtime registry for static, automatic, and allocated storage regions;
- released heap regions marked dead and quarantined from address reuse for the lifetime of the process in the minimal runtime;
- a distinct stable runtime trap for use-after-release or access to an invalidated region;
- automatic-storage pointers may be used only within a statically bounded non-escaping lifetime: they may be passed to direct calls for the duration of the call, but may not be returned, stored in static or heap storage, or otherwise outlive the declaring frame;
- a compiler rejection for a statically detectable automatic-storage pointer escape;
- frame-region registration/unregistration sufficient to validate allowed automatic-storage accesses;
- arrays, slices, and `str` retain their stronger bounds and UTF-8 contracts.

This is not a claim of general memory safety. Pointer-to-integer conversion and arbitrary integer-to-pointer construction remain explicit hazardous operations, but any attempted memory access through the resulting pointer still passes the live-region check and traps if it does not name a valid live region of sufficient extent and alignment.

The exact internal registry data structure and check-lowering strategy are Specialist decisions. The observable checks, rejection conditions, trap codes, and source diagnostics are normative specification work.

### Dedicated wrapping arithmetic built-ins

`wrap<T>(value)` remains an explicit truncating/conversion operation. It does not alter evaluation rules inside its operand.

Same-width wrapping arithmetic uses dedicated, reserved, generic built-ins:

- `wrapping_add<T>(a, b) -> T`;
- `wrapping_sub<T>(a, b) -> T`;
- `wrapping_mul<T>(a, b) -> T`;
- `wrapping_neg<T>(a) -> T` for signed integer types.

`T` must be an integer type and operands must be exactly `T` after only value-preserving implicit widening. Results use modulo `2^width` arithmetic and never trap for overflow. Division, remainder, and shifts retain their existing checked/defined rules and do not receive wrapping variants in v0.1.0.

This visible, named surface is selected over context-sensitive `wrap` semantics because local explicitness and one evaluation rule are more important than minimizing keyword count.

### Development host is not the release-support baseline

The observed Windows 10 22H2 machine remains the authorized development and bootstrap host because it is the available foreseeable environment and the required compiler/linker smoke tests passed there.

This does **not** accept Windows 10's end-of-support security posture as a supported release baseline and does not promise Windows 10 compatibility as a product contract. The initial target remains Windows x86-64 PE/COFF with a minimal, enumerated runtime-facing Win32 API contract. Before the first public executable release, the Main Designer must recommend and Marcel must approve the supported Windows version, security posture, and any residual risk. Until then:

- development remains local and limited to the public, non-sensitive hobby-project scope;
- no network-facing production deployment is authorized;
- release-support claims are prohibited;
- the runtime must minimize and document every Windows API dependency so later supported-version testing is bounded.

## Consequences

### Positive

- The pointer contract now matches AI-Co's deterministic-failure and detailed-diagnostic goals.
- Heap address reuse cannot make stale pointers silently appear valid during the minimal runtime.
- Automatic-storage pointer escape has an explicit, testable boundary.
- Wrapping arithmetic is visible, local, unambiguous, and usable for `u64`/`usize` hash and compiler workloads.
- Development can continue on the actual machine without silently converting an obsolete OS into a release or risk-acceptance decision.
- The invalid attributed decision record is explicitly rejected before publication.

### Negative

- Pointer checks, region tracking, heap quarantine, and frame registration increase compiler/runtime complexity and execution overhead.
- Quarantine can increase memory use; the specification must define resource-exhaustion behavior.
- Restrictions on automatic-storage pointer escape reject some familiar C patterns.
- Four wrapping built-ins permanently enlarge the minimal language surface and diagnostic/test matrix.
- A later Windows release-support decision and environment will still be required.

### Risks and controls

- **Pointer instrumentation omitted for an access path:** conformance tests must cover every pointer-producing and pointer-consuming form; uninstrumented access is a compiler defect.
- **Quarantine causes excessive memory pressure:** resource exhaustion returns or traps through the accepted runtime contract; it never permits address reuse that breaks the minimal temporal check.
- **Escape analysis becomes an implementation policy:** the specification must define the accepted and rejected source patterns conservatively; ambiguous cases reject with a stable diagnostic.
- **Wrapping built-ins become implicit promotion loopholes:** operands and result type are explicit and conversions remain governed by the value-preserving matrix.
- **Development-host wording becomes a support promise:** project documents must distinguish observed host evidence, target ABI, and approved release-support baseline.
- **Authority attribution recurs:** Planner and other roles may propose options but may not mark Main Designer or Marcel decisions as made without a directly traceable accepted record.

### Follow-up implications

The Planner must revise the Proposed specification, diagnostic contract, and open-question document to implement this ADR, remove false decision attribution, and restore all status fields to accurate Proposed/resolved-by-ADR language. The invalid `docs/decisions/MAIN-DESIGNER-DECISIONS-2026-08-08.md` artifact is deleted.

The revised specification requires independent Reviewer conformance review. Implementation remains blocked until that review passes and the Main Designer explicitly accepts the corrected specification.
