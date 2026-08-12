# Post-Milestone Compiler Testing Strategy — AI-Co Stage-0

**Status:** Main Designer design record (proposed strategy; not yet an accepted work package)
**Author:** Main Designer
**Date:** 2026-08-12
**Applies to:** AI-Co Stage-0 (M0) compiler and its successors (Stage 1, Stage 2)
**Review trigger:** review at M0 milestone completion (WP-M0-20) or on first post-milestone compiler change

## 1. Purpose

This record captures the identified gaps in the current test infrastructure and the
concrete testing strategies available AFTER the M0 milestone completes. It is a
durable design record, not an acceptance-criteria change; adopting any of these
strategies into the milestone or into a post-milestone work package requires a
separate decision (Main Designer) and, where it changes acceptance criteria,
Human Sponsor approval.

## 2. Current test architecture (as of 2026-08-12)

- **Corpora (spec-derived):** `tests/conformance/`, `tests/negative/`, `tests/smoke/` —
  case directories with `input.ai`, `expected.json` (stdout / stderr records /
  exit code / artifacts), and `meta.json`.
- **Harness:** `tests/harness/` with `case_schema.py`, `manifest_validator.py`,
  runners (`conformance_runner`, `negative_runner`, `smoke_runner`,
  `determinism_runner`, `identity_runner`), `report.py`.
- **Adapters:** `StubCompilerAdapter` (default) and `ScriptCompilerAdapter`
  (`--compiler PATH`). Today the corpus runners default to the stub, which
  synthesizes output from `expected.json` — i.e., the corpus tests validate the
  **test-spec structure and schema**, not compiler behavior.
- **Determinism:** `determinism_runner` + `identity_runner` (Stage 1 vs Stage 2
  PE byte-identity) are the M0 acceptance machinery, to be run against the real
  compiler at WP-M0-19c2 / WP-M0-20.

**Key fact:** until the real compiler produces executable output (16c→19c2), no
corpus test exercises the compiler. After M0, the corpora will run against the
real compiler via `--compiler`, giving the first true behavior checks
(diagnostics, spans, exit codes, stdout).

## 3. Identified test-coverage gaps (risk classes)

These are the error classes the current design can miss, even after the corpus
runs against the real compiler:

| # | Risk class | Detectable today? | Detectable after corpus-vs-compiler? | Notes |
|---|---|---|---|---|
| G1 | Wrong diagnostics / spans / exit codes | No (stub echoes expected) | **Yes** | Corpus breadth directly covers this |
| G2 | Wrong-but-internally-consistent codegen | No | **Partially** | Corpus compares process behavior (stdout/exit), not machine-code semantics; a deterministic miscompile that yields correct observable behavior on tested programs escapes |
| G3 | Silent accept / reject errors | No | **Yes, partially** | Corpus breadth; spec gaps patched by Planner rulings |
| G4 | Spec-echo / shared-blindspot errors | Yes (but blind) | **Persistent** | `expected.json` was authored from the spec; a spec error or agent misread is encoded identically in the corpus, so the test passes while being wrong. Independent review shares the same spec, so it can share the blind spot |
| G5 | Self-hosting-but-wrong (post-M0) | n/a | **No** | Byte-identity (Stage 0→1→2) proves stability/determinism, not semantic correctness; a stably-wrong self-hosting compiler passes |

## 4. Post-milestone testing strategies (candidate)

Each is described with its purpose, the error class it targets, and an
implementation sketch. None is committed; each becomes a work package via the
normal Planner/design process when adopted.

### S1 — Semantic differential testing against a reference implementation

- **Targets:** G2 (wrong codegen), G3 (silent accept/reject), G5.
- **Idea:** compile a set of nontrivial AI-Co programs, run them, and compare
  observable behavior (stdout, exit code, side effects, trap codes) against a
  reference — either (a) the same programs compiled with MSVC/Clang/GCC from an
  equivalent C source, or (b) hand-computed expected outputs authored
  independently of the spec-derived corpus.
- **Implementation sketch:** new corpus family `tests/differential/` with paired
  `input.ai` + reference-expected outputs; a new runner that invokes the AI-Co
  compiler and the reference toolchain and diffs observable behavior.
- **Why valuable:** this is the industry-standard way to find wrong-codegen bugs
  (used by LLVM/GCC); it breaks the spec-echo loop by introducing an external
  reference point.

### S2 — Torture tests for codegen correctness

- **Targets:** G2.
- **Idea:** a small, curated set of programs designed to stress codegen: edge-case
  arithmetic (signed/unsigned boundaries, wrapping, overflow traps), pointer
  aliasing, deeply nested control flow, cross-module calls, large switch
  statements, struct packing, recursion depth, and 64-bit boundary values — each
  with **independently computed** expected results (not derived from the corpus).
- **Implementation sketch:** `tests/torture/` with hand-verified expected outputs;
  runs under the real compiler post-M0.
- **Why valuable:** catches miscompiles that the spec-derived corpus, written for
  diagnostics coverage, may not exercise.

### S3 — Differential fuzzing (post-M0, higher effort)

- **Targets:** G2, G3, G5.
- **Idea:** generate random-but-valid AI-Co programs; for each, compile with the
  AI-Co compiler and a reference implementation of the same semantics, and
  compare behavior. Also generate near-miss programs (single-token mutations of
  valid programs) to probe silent-accept / over-reject boundaries.
- **Implementation sketch:** a generator + oracle harness; requires the reference
  implementation (S1) to exist first. This is a larger, ongoing effort — the
  classic compiler-fuzzing approach (cf. Csmith, differential testing in LLVM).
- **Why valuable:** catches errors no fixed corpus can anticipate; continuously
  explores the input space.

### S4 — Independent oracle for self-hosting semantics (post-M0)

- **Targets:** G5 (self-hosting-but-wrong).
- **Idea:** beyond byte-identity, verify that the self-hosting Stage-1/Stage-2
  builds actually **behave** correctly: compile a set of programs with the
  stage-built compiler and compare observable behavior against the same programs
  compiled by the bootstrap (Stage-0) compiler. Also run the Stage-built
  compiler on the corpus and require identical behavior to the Stage-0-built
  compiler.
- **Implementation sketch:** extend `identity_runner` semantics — compare
  behavioral results (not just bytes) across stage-built artifacts.
- **Why valuable:** byte-identity proves reproducibility; behavioral comparison
  across stages proves the self-hosting chain preserves semantics.

### S5 — Specification-echo audits

- **Targets:** G4.
- **Idea:** periodically audit a sample of `expected.json` records against the
  spec by a role that did NOT author the corpus (independent reviewer), with
  explicit authority to flag spec ambiguities for Planner rulings. Where a spec
  gap is found, the corpus record and the spec are corrected together, breaking
  the echo.
- **Implementation sketch:** a scheduled audit task (Historian/Reviewer) after
  M0; sample-based, not exhaustive; feeds findings back to Planner.
- **Why valuable:** closes the persistent shared-blindspot risk that survives
  even after corpus-vs-compiler execution.

## 5. Sequencing recommendation (informative)

1. **At M0 completion:** run the corpus against the real compiler (`--compiler`)
   — this is the first true behavior gate; fix whatever it surfaces.
2. **First post-M0 package:** S2 (torture tests) + S5 (spec-echo audit) —
   cheap, high value, no external dependency.
3. **Second:** S1 (differential vs reference) — needs the reference mapping of
   AI-Co semantics to an available toolchain; medium effort.
4. **Third:** S4 (self-hosting behavioral comparison) — natural once the stage
   chain is proven byte-identical.
5. **Ongoing:** S3 (fuzzing) — highest effort, most powerful; start after S1
   oracle exists.

## 6. Decision state

- This record is **informational and proposed** — no acceptance criteria changed,
  no work package created.
- Adoption of any strategy into the M0 milestone or into a post-M0 work package
  requires: Main Designer decision + (if acceptance criteria change) Marcel
  approval + Planner work-package sizing + normal review/gate flow.
- Review trigger: revisit at WP-M0-20 completion or on first post-milestone
  compiler change, whichever comes first.
