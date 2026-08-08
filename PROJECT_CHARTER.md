# AI-Co Project Charter

**Status:** Accepted

**Owner:** Main Designer

**Approver:** Marcel, Human Sponsor

**Version:** 0.1.0

**Accepted:** 2026-08-08

## Purpose

AI-Co is an independent systems programming language based on C and optimized primarily for use by AI coding agents. It preserves C-like concepts and syntax wherever doing so does not conflict with agent-oriented clarity, deterministic interpretation, actionable diagnostics, self-sufficiency, or the project's explicit goals.

Human convenience is secondary when it conflicts with AI-agent usability. Compatibility with C, existing source code, ABIs, ecosystems, or third-party tools is not a project objective.

## Governing direction

1. Remain as similar to C as practical outside deliberate changes that improve AI-agent generation, interpretation, debugging, or collaboration.
2. Eliminate ambiguous, unspecified, implementation-defined, and undefined language behavior from the supported language wherever a precise rule can be provided.
3. Make detailed, structured, location-precise error reporting a normative language-system capability rather than an optional compiler presentation feature.
4. Prefer agent-oriented decisions over human-oriented conventions when the two materially conflict.
5. Build toward local self-sufficiency and minimal reliance on third-party dependencies.
6. Treat interoperability and adoption by third parties as non-goals unless Marcel later changes project purpose.

## Initial outcome

The first complete language line must be sufficient to:

1. implement an AI-Co compiler in AI-Co;
2. compile that compiler through a bootstrap compiler written in an existing language;
3. use the resulting AI-Co compiler to compile itself again;
4. demonstrate deterministic equivalence between the self-hosting stages under the accepted bootstrap contract;
5. develop subsequent AI-Co language features and supporting utilities primarily in AI-Co.

## Initial scope

The minimal language is expected to include only the facilities required for deterministic native programs and compiler self-hosting:

- unambiguous lexical and grammatical rules;
- fixed and explicit primitive data semantics;
- functions, local and global declarations, control flow, aggregates, arrays, pointers, and explicit memory operations;
- a minimal module/import mechanism without a textual preprocessor;
- deterministic evaluation and conversion rules;
- a small runtime and system interface sufficient for compiler I/O, allocation, and process termination;
- a canonical structured diagnostic contract;
- a deterministic compiler pipeline and bootstrap verification contract.

The exact syntax and behavioral requirements belong in the Planner-owned language specification derived from accepted architecture decisions.

## Explicit non-goals for the minimal language

- C source, header, preprocessor, ABI, or binary compatibility;
- broad standard-library coverage;
- package registries or online package resolution;
- object orientation, inheritance, exceptions, garbage collection, reflection, generics, metaprogramming, concurrency, GPU programming, or distributed execution;
- optimization beyond what is needed to produce correct, inspectable native programs;
- Linux, macOS, web, mobile, embedded, or multi-architecture support in the first bootstrap line;
- convenience features that introduce hidden control flow, implicit conversions, context-dependent parsing, or non-local meaning.

## Constraints

- Initial development host and target: native Windows x86-64.
- Bootstrap implementation language: C, per project ADR-001.
- Third-party compiler libraries are not part of the compiler core.
- Build trees, caches, and generated artifacts must reside on the E: workspace rather than consume the critically constrained C: drive.
- Secrets never enter this repository.
- Public repository status is explicitly authorized by Marcel; publication does not waive future release, security, or acceptance gates for executable artifacts.

## Decision and delivery ownership

- Main Designer: project architecture and language-design decisions.
- Planner: normative language specification, milestones, acceptance criteria, and implementation-ready work packages.
- Coordinator: board routing, readiness, dependencies, and completion gates.
- Researcher: source-qualified evidence.
- Specialists: implementation and primary verification.
- Reviewer: independent assurance.
- Historian: project record freshness and supersession.
- Marcel: project-purpose changes, public-release gates, material risk, and other constitutional approvals.
