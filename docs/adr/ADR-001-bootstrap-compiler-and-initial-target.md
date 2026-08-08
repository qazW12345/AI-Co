# ADR-001: Bootstrap Compiler and Initial Target Architecture

## Status

Accepted

## Supersedes

None

## Context

Decision owner: Main Designer. Marcel has approved AI-Co as an independent C-based language optimized primarily for AI agents and has delegated technical architecture within the stated goals.

The required first outcome is a minimal language that can host its own compiler and supporting utilities. The bootstrap must begin in an existing language, then demonstrate that the AI-Co compiler produced by the bootstrap compiler can compile itself into equivalent working machine code.

The current-machine evidence is recorded in `research/ENVIRONMENT_BASELINE_2026-08-08.md`. It verifies native Windows x86-64 compilation and execution through both MSVC C and LLVM Clang/LLD, as well as MASM assembly. Rust, Zig, Go, the .NET SDK, a Java compiler, GCC, and MinGW are not currently available. The E: workspace has ample capacity; the C: drive is critically constrained.

The project also values self-sufficiency and low dependency count more than compatibility with external ecosystems.

Alternatives considered:

1. **Bootstrap in C.** Closest to AI-Co's conceptual base, supported by two verified local compilers, low runtime complexity, and straightforward translation into the self-hosted language. It requires disciplined manual resource management.
2. **Bootstrap in C++.** Provides stronger implementation abstractions and resource-management facilities, but introduces a larger source-language surface and a greater semantic gap from the minimal self-hosting language.
3. **Prototype in Python or TypeScript, then rewrite in C.** Enables rapid grammar experimentation but creates an additional throwaway implementation and a risk that the prototype becomes an accidental semantic authority.
4. **Install and bootstrap in Rust, Zig, or Go.** These languages could offer implementation advantages, but they are absent from the present environment and add toolchain/bootstrap dependencies not justified by project goals.
5. **Use LLVM as the compiler backend.** This provides mature code generation and diagnostics infrastructure but creates a large external dependency and makes eventual self-sufficiency substantially harder.
6. **Emit C as the permanent backend.** This minimizes initial backend work but delegates AI-Co semantics and diagnostics to host C behavior and prevents the compiler from owning machine-code determinism.
7. **Emit native Windows objects through a small project-owned backend.** This requires more project work but preserves semantic control, deterministic output, and a credible path to self-sufficiency.

## Decision

### Bootstrap implementation

The stage-0 AI-Co compiler will be implemented in portable, conservative C17. It will use the C standard library and narrowly isolated host adapters only where required for files, memory, and process behavior. The compiler core will not depend on C++, LLVM libraries, parser generators, package managers, network services, or third-party runtime libraries.

MSVC and LLVM Clang are both accepted host compilers for independent bootstrap verification. Neither compiler defines AI-Co semantics.

### Compiler architecture

The compiler will use explicit, inspectable stages:

1. byte-oriented source loading and UTF-8 validation;
2. deterministic lexer;
3. single-meaning parser producing a typed syntax representation;
4. name and type resolution;
5. semantic validation;
6. canonical, target-neutral intermediate representation;
7. deterministic x86-64 backend;
8. COFF object emission;
9. external link step during the initial bootstrap line;
10. structured diagnostic rendering and build manifest emission.

Each stage must have a bounded input/output contract. Diagnostics retain causal and source-location information as they cross stages rather than being reconstructed from plain strings.

### Initial platform target

The first target is little-endian Windows x86-64 using PE/COFF conventions. The default generated instruction baseline must not exceed x86-64 plus SSE2. AVX2 and other host-specific features may not be required by the minimal language or bootstrap compiler.

The project-owned backend will emit deterministic COFF object files. Microsoft `link.exe` or LLVM `lld-link` may perform final linking through the first Stage 0/1/2 self-hosting proof. This exception ends at that milestone: the next milestone, **self-sufficient development baseline**, requires a project-owned linker implemented in AI-Co and used for normal compiler and utility builds. External linkers may remain independent comparison/oracle tools after that point, but AI-Co may not claim a self-sufficient toolchain while they remain required for ordinary builds.

LLVM may be used as an independent comparison tool or later optional backend, but it is not a dependency of the normative compiler pipeline.

### Foundational language-semantics direction

The Planner-owned specification must preserve C-like surface form while making these properties normative:

- every accepted program has one grammatical interpretation;
- source evaluation order is defined, initially left-to-right where subexpressions have an order;
- primitive integer widths, signedness, representation, alignment, and conversion effects are explicit and target-independent within the initial target contract;
- implicit narrowing, sign-changing conversion, pointer/integer conversion, and truthiness conversion are prohibited;
- signed overflow, invalid shifts, division errors, null dereference, and other C undefined behaviors receive explicit compile-time rejection or deterministic runtime behavior;
- declarations and callable signatures must be explicit before use;
- control-flow bodies use braces and hidden control flow is excluded;
- the textual C preprocessor, token macros, conditional compilation, and header inclusion are absent from the minimal language;
- modules/imports are semantic constructs with deterministic resolution;
- the implementation must not add implementation-defined behavior merely because C permits it.

The exact rules and syntax remain specification work; this ADR fixes the architecture constraints they must satisfy.

### Diagnostic architecture

Structured diagnostics are normative compiler output. The canonical representation will be a versioned JSON Lines stream suitable for machine consumption. A human-readable renderer is secondary and derived from the same diagnostic records.

Every diagnostic record must be capable of carrying:

- stable diagnostic code and schema version;
- severity and compilation phase;
- primary source span and relevant secondary spans;
- observed construct/value/type and expected alternatives;
- causal chain rather than only the final symptom;
- deterministic recovery or continuation status;
- candidate corrections when they can be generated without guessing intent;
- related declaration, scope, module, and type information;
- whether the diagnostic is authoritative, cascading, or recovery-derived.

Diagnostic wording may improve without changing the stable code and structured meaning. Parser recovery must be deterministic, and cascading diagnostics must be marked so agents can prioritize root causes.

### Bootstrap equivalence contract

The bootstrap line consists of:

- **Stage 0:** C implementation compiled by an accepted host C compiler.
- **Stage 1:** Stage 0 compiles the AI-Co implementation of the compiler.
- **Stage 2:** Stage 1 compiles the same AI-Co compiler source again.

The primary identity artifacts are the Stage 1 and Stage 2 compiler-produced COFF object files and build manifests. They must be byte-identical with **no normalization step**. Compiler-controlled timestamps are zero, record and section order is canonical, paths are repository-relative and canonically separated, iteration order is deterministic, and random or host-specific identifiers are prohibited.

The same accepted external linker and exact deterministic/reproducible flags are then applied independently to each stage's identical object set. The resulting PE executables must be byte-identical as a secondary identity gate. If an accepted linker cannot satisfy this requirement, that linker is unsuitable for the bootstrap proof; nondeterministic PE fields may not be excused through an unspecified normalization rule. The Planner's bootstrap contract must list the accepted linker modes and every compiler/linker input included in the comparison evidence.

Both outputs must also pass the same conformance, negative-diagnostic, and executable smoke suites. Byte identity without behavioral verification is insufficient; behavioral success without the deterministic comparison is also insufficient for the self-hosting milestone.

### Build and dependency policy

All build outputs, caches, temporary compiler artifacts, and bootstrap stages reside under the E: project workspace. Build commands use explicit tool paths or a verified Visual Studio developer environment and must never invoke ambiguous bare `link` from Git Bash.

The minimal build must be reproducible without network access after the accepted host toolchain is present. Dependencies used only for research or optional testing may not become hidden requirements of the compiler core.

## Consequences

### Positive

- C minimizes the conceptual gap between stage 0 and the self-hosted compiler.
- Two verified host compilers provide independent evidence without adding a project runtime dependency.
- Direct ownership of IR, diagnostics, and object generation supports deterministic behavior and long-term self-sufficiency.
- A conservative x86-64 baseline avoids coupling the language to the development CPU's AVX2 capability.
- Structured diagnostics become a stable agent interface rather than presentation text.
- Explicit bootstrap stages create a falsifiable self-hosting milestone.

### Negative

- A project-owned x86-64/COFF backend requires more early work than emitting C or embedding LLVM.
- C17 implementation increases manual memory-management and data-structure burden.
- Initial output is Windows x86-64-specific.
- External linking remains a temporary bootstrap dependency.
- Strict semantics will intentionally reject some familiar C idioms and may require more explicit source.
- Byte-identical bootstrap requirements constrain compiler data structures, metadata, and build behavior from the beginning.

### Risks and controls

- **Stage-0 semantics become accidental language policy:** the normative specification and conformance suite, not C implementation behavior, define AI-Co.
- **Backend effort delays language validation:** Planner milestones must separate a minimal executable backend from optimizations and a self-hosted linker.
- **Diagnostics drift between stages:** stable diagnostic codes/schema and golden negative tests are required.
- **Unspecified behavior leaks back in:** every semantic rule must state compile-time rejection or deterministic runtime behavior; absence of a rule blocks implementation readiness.
- **Host-tool PATH hazards:** build entry points initialize the required developer environment and use explicit linker/tool paths.
- **Resource pressure:** parallelism and temporary output are bounded; E: is the only default build-artifact location.

### Approval and follow-up

This architecture is Accepted under Marcel's delegated technical direction. The independent review in `docs/reviews/INITIAL-ARCHITECTURE-REVIEW-2026-08-08.md` approved it with Minor findings. The clarification above closes the architecture-owned portion of FIND-001 and resolves FIND-002 by time-bounding external linking; the Planner must encode the exact bootstrap comparison inputs and accepted deterministic linker modes in the normative specification.

FIND-003 through FIND-005 remain Planner specification gates: explicit function-pointer necessity evaluation, a complete runtime/platform contract, and diagnostic schema v1 with a stable code-allocation policy. Implementation remains blocked until the specification has sufficient requirements and architecture confidence and passes the required review gates.
