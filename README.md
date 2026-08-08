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
- [`docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md`](docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md)
- [`docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md`](docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md)
- [`research/ENVIRONMENT_BASELINE_2026-08-08.md`](research/ENVIRONMENT_BASELINE_2026-08-08.md)

## Status

Architecture establishment and specification drafting. No compiler implementation has begun.

## Governance

AI-Co is a Sneedworks project. Project work follows the Sneedworks Constitution and Operations Manual while keeping project architecture, specifications, source, tests, and release records in this repository.

## License

No license has been selected yet. The repository is publicly visible, but no permission grant should be inferred until Marcel approves a license.
