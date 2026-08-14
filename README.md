# AI-Co

AI-Co is an independent systems programming language based on C and designed primarily for AI coding agents.

Its defining priorities are:

- deterministic, unambiguous syntax and semantics;
- detailed, structured, causal diagnostics suitable for automated repair;
- C-like concepts and surface form where they do not conflict with agent usability;
- self-hosting and long-term local self-sufficiency;
- minimal external dependencies;
- AI-agent usability over human convenience when the two conflict.

Compatibility with C source, ABIs, existing ecosystems, or third-party adoption is not a project objective.

## Initial milestone

The first complete milestone is a minimal compiler that can compile an AI-Co implementation of itself and demonstrate deterministic Stage 1/Stage 2 equivalence with passing conformance and diagnostic suites.

## Current architecture

- Stage-0 compiler: conservative C17.
- Initial host: native Windows x86-64.
- Initial output: deterministic x86-64 COFF objects linked into PE executables.
- Compiler pipeline: source → lexer → parser → typed representation → semantic validation → canonical IR → x86-64 backend → COFF.
- Diagnostics: versioned structured JSON Lines as the canonical interface, with secondary human-readable rendering.
- Core dependency posture: no LLVM library, parser-generator, package-manager, network-service, or third-party runtime dependency.

See:

- [`PROJECT_CHARTER.md`](PROJECT_CHARTER.md)
- [`docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md`](docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md)
- [`docs/adr/ADR-002-minimal-core-language-semantics.md`](docs/adr/ADR-002-minimal-core-language-semantics.md)
- [`docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md`](docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md) — superseded history
- [`docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md`](docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md)
- [`docs/adr/ADR-005-rt-io-standard-stream-failure-contract.md`](docs/adr/ADR-005-rt-io-standard-stream-failure-contract.md)
- [`spec/AI-CO-LANGUAGE-SPECIFICATION.md`](spec/AI-CO-LANGUAGE-SPECIFICATION.md) — Accepted v0.1.6
- [`spec/DIAGNOSTIC-CONTRACT.md`](spec/DIAGNOSTIC-CONTRACT.md) — Accepted v0.1.1
- [`spec/OPEN-QUESTIONS.md`](spec/OPEN-QUESTIONS.md) — resolution and monitoring record
- [`research/ENVIRONMENT_BASELINE_2026-08-08.md`](research/ENVIRONMENT_BASELINE_2026-08-08.md)

Repository layout: [`bootstrap/src/`](bootstrap/src) (Stage-0 compiler source), [`tests/`](tests) (conformance/negative/smoke harness and corpora), [`spec/`](spec) (normative specifications and contracts), [`docs/adr/`](docs/adr) (accepted architecture decision records).

## Status

Architecture establishment and specification drafting are complete; the language specification is Accepted. Stage-0 implementation is in progress: the compiler front end, semantic layers, canonical IR, and IR validation are complete; x86-64 backend work is underway (WP-M0-17), followed by COFF emission, driver/linker integration, and the Stage-0 integration gate (WP-M0-20). Milestone acceptance is pending WP-M0-20.

## Governance

AI-Co is a Sneedworks project. Project work follows the Sneedworks Constitution and Operations Manual while keeping project architecture, specifications, source, tests, and release records in this repository.

## License

Licensed under the GNU General Public License v3.0 (GPL-3.0) — see [LICENSE](LICENSE).

The GPL covers the compiler implementation, specifications, tests, and documentation in this repository. AI-Co programs compiled by the compiler are the user's own copyright; GPL does not extend to compiler output.
