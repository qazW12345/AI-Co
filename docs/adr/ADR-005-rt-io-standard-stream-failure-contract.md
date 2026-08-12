# ADR-005: Standard Stream Handles Use Explicit Failure Values

## Status

Accepted

Decision owner: Main Designer.

## Supersedes

The parenthetical `never 0` guarantee for `rt.io.stdin()`, `rt.io.stdout()`, and `rt.io.stderr()` in AI-Co Language Specification §15.2. All other §15.2 requirements remain in force.

## Context

AI-Co Language Specification §15.2 says that file-handle value `0` is invalid, that a successful operation never returns it, and that `rt.io.open` returns `0` on failure. The same section describes the three standard-stream accessors as returning their handles “never `0`.”

WP-M0-15a2 exposed an environmental case that the last phrase does not define: Windows may provide no standard handle to a detached or windowed process, and the finite runtime handle table may be full when an accessor first registers a stream. Commit `a5921a3` returns `0` in those cases, while returning stable nonzero handles for available standard streams. The behavior is documented and tested. Independent review `t_4e93c316` found the mismatch as MIN-1 and verified the implementation under MSVC and Clang.

ADR-002 permits resource exhaustion and external I/O failure to be represented either by explicit result values or by named runtime traps, but forbids undefined behavior. It also treats environmental values as inputs rather than language ambiguity. The current standard-stream function signatures return only `usize`; they have no separate result type through which to report absence.

Alternatives considered:

1. **Make `never 0` absolute.** Reserve runtime-table entries and synthesize a nonzero stream handle even when the OS supplies no corresponding stream. This preserves the literal sentence but requires new semantics for reads, writes, and closes on a synthetic handle. Reserving entries addresses table exhaustion but not stream absence; mapping an absent stream to a null device would silently change environmental failure into apparent success.
2. **Trap when a standard stream is absent or cannot be registered.** This is deterministic, but makes ordinary environmental absence or resource exhaustion process-fatal without an established trap code or demonstrated self-hosting need.
3. **Use `0` as the explicit failure value.** This matches the existing handle sentinel and `rt.io.open` model, preserves the distinction between success and environmental failure, and requires no fabricated stream semantics.
4. **Add a richer result type.** This could distinguish absence from table exhaustion, but the minimal language has no such established runtime result type and the current self-hosting requirement does not justify expanding the language surface.

## Decision

Select alternative 3.

For each of `rt.io.stdin()`, `rt.io.stdout()`, and `rt.io.stderr()`:

- a nonzero return value denotes a successfully registered, usable runtime-managed handle for the corresponding OS standard stream;
- `0` is returned when the process environment supplies no valid corresponding standard handle or when the runtime cannot register that handle because its handle table is exhausted;
- returning `0` for either failure is deterministic and is an explicit failure value, not a trap;
- an available stream successfully registered by the runtime must never return `0`, and repeated successful calls return the cached handle;
- no conforming implementation may fabricate a successful stream, silently redirect an absent stream to a null device, or expose an invalid OS handle as a nonzero successful runtime handle.

Accordingly, the normative interpretation of Specification §15.2 is “nonzero on successful standard-stream registration; `0` on environmental absence or registration resource exhaustion.” The literal absolute `never 0` parenthetical is superseded.

The implementation in commit `a5921a3` conforms to this decision. Reviewer finding MIN-1 is closed by this ruling; no production-source remediation is required.

The Planner should align the normative specification text with this ADR. That editorial alignment must not reopen or vary the architecture decision without a new architecture escalation.

## Consequences

### Positive

- Every observable outcome has an explicit meaning at the exact environmental failure boundary.
- Standard-stream access follows the established `usize` handle sentinel and ADR-002 failure model.
- Detached/windowed processes and finite-table exhaustion do not require synthetic resources or unexplained process termination.
- Callers can detect absence using the same `0` failure convention as `rt.io.open`.

### Negative

- Callers that require a standard stream must check for `0` before use.
- The three accessors do not distinguish absent OS streams from handle-table exhaustion.
- The accepted specification requires a narrow Planner-owned wording correction.

### Risks and controls

- **Failure is mistaken for EOF or a zero-byte write:** `0` is a handle value here, not an I/O byte count; tests and API documentation must keep that distinction explicit.
- **Implementations fabricate success:** conformance tests should include a process with an absent standard handle and a full-table case, both requiring `0`.
- **Specification drift persists:** a Planner-owned follow-up must replace the superseded absolute phrase and cite this ADR.

### Approval and follow-up

This is a bounded project-architecture decision within the Main Designer's delegated authority. It does not change project purpose, accept material risk, or cross a Human Sponsor gate. No Human Sponsor approval is required.

Follow-up: Planner-owned specification alignment; no Specialist remediation.
