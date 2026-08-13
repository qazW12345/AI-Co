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

### S2a — Feature-scoped differential micro-programs (per-feature correctness)

- **Targets:** G2, G3, G4 (with the refinements below).
- **Idea:** a suite of **minimal programs, each exercising exactly one AI-Co
  feature** (or a small deliberately-composed batch), each with a reference
  translation and expected observable behavior. A failure of a feature micro-
  program **localizes the bug by construction**: the bug must be in the code
  path for that feature, and — given the IR boundary — in the backend rules
  that translate that feature's IR node kind. This makes the "find the bug"
  step nearly automatic; the debugging loop becomes
  *run → fail → inspect that program's IR → inspect emitted bytes → fix one rule*.
  This is the concrete implementation shape that merges S1 (external reference)
  with S2 (per-feature stress) into a per-feature correctness check.
- **Refinement R1 — dual-reference discipline.** The "reference compiler is
  always correct" assumption must not be taken literally. For features that map
  cleanly to C semantics, compare against **agreement between two independent
  compilers** (e.g., Clang + MSVC, or Clang + GCC) on the equivalent C program;
  if the two references disagree on a micro-program, treat it as a reference
  problem, not an AI-Co problem. For AI-Co-specific features with **no C
  equivalent** (checked-arithmetic traps with AIC-R codes, the cast matrix,
  defined wrapping), the reference is a **hand-computed oracle** derived from
  the spec, not a compiler.
- **Refinement R2 — boundary values mandatory.** Each micro-program must test
  **edge values**, not typical values: 0, INT32_MIN/MAX, -1, 64-bit boundaries,
  overflow-into-trap cases, negative immediates, max loop iterations. A wrong
  codegen path (e.g., a 32-bit truncating add or a signedness slip) often
  produces the *correct* answer for typical inputs; boundary values are what
  turn "wrong but observably correct" into "wrong and caught."
- **Composition subset.** Single-feature tests miss feature-**interaction** bugs
  (each feature works alone; two interact wrongly). Include a modest set of
  composition programs deliberately combining 2–3 features (e.g., struct
  returns inside loops, slice indexing + checked arithmetic). These localize to
  the *intersection* of the involved code paths — still a small search space.
- **Implementation sketch:** `tests/features/` with per-feature subdirectories
  (`arith-wrap/`, `cast-matrix/`, `control-flow/`, `struct-return/`, `trap/`,
  ...), each containing `input.ai` + reference C (where applicable) + expected
  behavior + boundary-value notes; a runner that compiles with the AI-Co
  compiler and the reference toolchain(s) and diffs observable behavior
  (stdout / exit code / trap code), plus hand-oracle checks for AI-Co-specific
  features.
- **Why valuable:** highest value-per-effort of the candidate strategies; every
  failure is small and localizable; it works at the M0 boundary with no external
  oracle infrastructure beyond an available reference toolchain; and it breaks
  the spec-echo loop per feature.
- **Status note (2026-08-12):** recorded as an *approach we may utilize*;
  adoption is deferred to the M0-20 review point per Main Designer direction
  ("we can decide later, when we come to that point"). No work package exists
  yet; if adopted it becomes a work package via the normal Planner/design
  process.

### S2a-IR — Pre-codegen stopgap: feature-scoped micro-programs at the IR boundary (early adoption, 2026-08-13)

- **Status:** ADOPTED as an early, bounded verification stopgap by Main Designer decision
  2026-08-13, per Marcel's direction ("if you believe doing S2a style checks now ... will pay
  off, please delegate doing them"). This is NOT the full post-M0 S2a corpus; it is a first
  tranche targeted at the IR builder (WP-M0-16) before codegen (WP-M0-17) consumes the IR.
- **Rationale:** the 16c1c review cycle (compound-assignment destination-location ordering,
  alias-through-global) proved the wrong-but-consistent (G2) risk is real in builder lowering.
  Review samples; differential checks exhaust. The cheapest intervention point is while the IR
  builder is still the only consumer of the IR — before 17 bakes a wrong lowering into machine
  code and the bootstrap chain. At WP-M0-20 the same bug would be a cascade.
- **Trigger:** WP-M0-16c2 (t_07aacd82) done — the earliest moment the IR surface is final
  (end-to-end `ir_builder_build` exists only after 16c1d; 16c2 adds span/cause fields, so
  expected dumps authored earlier would churn) and the shared tree is quiescent (W2a).
- **Scope (IR-boundary, not behavioral):** minimal AI-Co programs, one feature each, focused
  on the trickiest lowerings: compound assignment (destination-location order,
  alias-through-global), repetition form `[e; N]` (IRC-N1 evaluate-exactly-once), cast/wrap
  matrix, pointer arithmetic / value categories, struct/array literal lowering, trap
  obligations. Oracle = hand-computed expected IR per IR-CONTRACT-2026-08-12 +
  `ir_core_verify` (AIC-I0501) + `ir_dump` round-trip byte-identity (16b2 machinery).
  Behavioral dual-reference (R1) remains post-M0, when executables exist.
- **Placement:** structural gate between WP-M0-16 and WP-M0-17 (parent of 17a1), and a parent
  of the 16-section push wave so origin receives validated code.
- **Delegation:** Main Designer decision recorded here; Planner sizes the work package and
  creates the specialist implementation card; normal review/gate flow applies.


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

0. **Pre-codegen stopgap (adopted 2026-08-13, see §S2a-IR):** at WP-M0-16c2 completion, run
   the S2a-IR IR-boundary micro-program tranche on the finished IR builder — before codegen
   (WP-M0-17) consumes the IR. This inserts the first true semantic verification at the
   cheapest possible point (builder is still the only consumer) and gates WP-M0-17.
1. **At M0 completion:** run the corpus against the real compiler (`--compiler`)
   — this is the first true behavior gate; fix whatever it surfaces.
2. **First post-M0 package:** S2a (feature-scoped differential micro-programs)
   — highest value-per-effort, no external oracle infrastructure beyond an
   available reference toolchain; every failure is small and localizable. Plus
   S5 (spec-echo audit). S2 (torture) may be folded into S2a's boundary-value
   discipline rather than run as a separate corpus.
3. **Second:** S1 (differential vs reference) — S2a's dual-reference discipline
   generalizes into the full differential corpus; medium effort.
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
