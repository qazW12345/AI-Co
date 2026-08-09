# AI-Co Stage-0 Bootstrap Milestone Plan

> **TRIAL — Constitution §8 temporary exception (2026-08-08).** This attribution header is a trial addition per [TRIAL-APPROVAL-authority-attribution-control-2026-08-08.md](../../../governance/runtime/TRIAL-APPROVAL-authority-attribution-control-2026-08-08.md) and proposal v0.3.1 (Controls 1–5). Expires 30 days from trial start or 3 ADRs + 5 reports, whichever comes first. Rollback by the Main Designer through the authorizing approval path.

- **Recorded by:** Planner
- **Decision authority (if applicable):** Main Designer (accepted architecture: ADR-001, ADR-002, ADR-004); Marcel, Human Sponsor (ADR-004 resolutions; Stage-0 direction and working philosophy, 2026-08-08)
- **Human approver (if a gate applies):** Not applicable — this report is a planning handoff; it relies on already-recorded approvals and introduces no new gate.

**Status:** Accepted
**Owner:** Planner
**Date:** 2026-08-09
**Scope:** project AI-Co
**Supersedes:** none (first implementation-planning artifact for AI-Co)
**Accepted:** 2026-08-10 — Main Designer (decision owner for plan acceptance; decision in session 2026-08-10, routing task t_5bcee55d). Scope: M0 milestone plan only; M1/M2 remain as recorded; the M1 refinement checkpoint is unchanged. No new architecture decisions; no new Human Sponsor claims.

## 1. Requested outcome and governing decisions

Transform the accepted AI-Co specification set into an implementation-ready milestone plan and work-package manifest for the Coordinator, focused on the first serious proof per the project charter: a conservative C17 Stage-0 compiler that can compile the AI-Co compiler, after which the AI-Co compiler recompiles itself under the strict deterministic bootstrap contract (Stage 1/Stage 2 byte identity, manifest byte-identity gates, diagnostic conformance).

Governing foundation (all accepted, all on `origin/main`):

| Source | Role in this plan |
|---|---|
| `PROJECT_CHARTER.md` v0.1.0 (Accepted) | Purpose, initial outcome, initial scope, non-goals, constraints |
| `spec/AI-CO-LANGUAGE-SPECIFICATION.md` v0.1.1 (Accepted) | Normative language, pipeline §14, runtime §15, bootstrap contract §16, exclusions §17, examples §18, traceability §19 |
| `spec/DIAGNOSTIC-CONTRACT.md` v0.1.1 (Accepted) | JSONL schema, code registry, deterministic ordering, trap contract |
| `spec/OPEN-QUESTIONS.md` v0.1.1 (Accepted) | Monitored resolutions; no implementation-blocking open questions |
| `docs/adr/ADR-001` (Accepted) | Stage-0 in C17; explicit pipeline stages; deterministic COFF; diagnostic JSONL; bootstrap equivalence; build/dependency policy; initial target Windows x86-64 + SSE2; external linker time-bounded |
| `docs/adr/ADR-002` (Accepted) | Minimal core semantics: canonical declarations, fixed-width types, no implicit narrowing, checked arithmetic, braces-only control flow, no preprocessor, explicit modules |
| `docs/adr/ADR-003` (Superseded by ADR-004) | Historical evidence only; no governing weight |
| `docs/adr/ADR-004` (Accepted, Human Sponsor) | Temporal baseline (allocator registry, 0xDD poisoning, deterministic reuse), wrapping baseline (`wrap<T>` is a conversion only; no same-width wrapping arithmetic), Windows 10 22H2 x64 pinned bootstrap baseline |
| `research/ENVIRONMENT_BASELINE_2026-08-08.md` | Host evidence: i7-4770 Haswell, Win10 22H2 build 19045.6466, MSVC cl 19.50.35717 on PATH, LLVM/Clang 22.1.8 off PATH, `link` collision hazard, E: workspace 726 GB free, C: critically constrained |
| `LICENSE` ("NO LICENSE GRANTED") | Private repo; no public release; no executable publication gate here |

Marcel's working philosophy (hard constraints from task t_dd420fb9, 2026-08-08): take it slow; no two specialists on the same code area (strict serial dispatch); no pressure; consistency over speed in every trade-off.

## 2. Milestone interpretation (M0 / M1 / M2)

ADR-001 fixes the bootstrap line (Stage 0 C17 → Stage 1 AI-Co → Stage 2 identical source) and the external-linker time bound. The accepted specification names two milestones in §16.5: **M1 — first self-hosting proof** and **M2 — self-sufficient development baseline**. The task's "M0/M1 structure per ADR-001 as you interpret it" maps as follows (documented interpretation; the spec's §16.5 naming is authoritative):

- **M0 — Stage-0 bootstrap compiler and verification infrastructure.** Construction of the conservative C17 compiler implementing the accepted minimal language, plus the conformance / negative-diagnostic / executable-smoke suites, verification harness, deterministic build conventions, and the project-owned runtime. Exit: Stage 0 passes the full suite set, produces deterministic COFF + build manifests, builds under both accepted host compilers, and is ready to compile the AI-Co compiler source.
- **M1 — first self-hosting proof (spec §16.5).** Stage 0 compiles the AI-Co compiler source → Stage 1; Stage 1 recompiles the same source → Stage 2; primary artifacts (COFF objects + build manifests, identical relative output paths) byte-identical with no normalization; PE byte-identity under accepted deterministic linker modes; full suite pass on both outputs; recorded comparison evidence per spec §16.6.
- **M2 — self-sufficient development baseline (spec §16.5).** Project-owned AI-Co linker used for ordinary builds; external linkers demoted to oracle/comparison. **Out of scope for this manifest**; recorded here only as the boundary that ends the external-linker exception at M1.

M1 and M2 acceptance criteria are already normative in spec §16.5/§16.6; this plan does not restate them as new decisions.

## 3. M0 exit gate (what "Stage-0 milestone done" means)

M0 is complete when all M0 work packages (WP-M0-01..20, see manifest) are verified and:

1. Stage 0 (built with MSVC **and** with LLVM Clang, per spec §16.4 host-compiler independence) accepts the valid corpus, rejects the negative corpus with the required stable codes/spans in deterministic order, and passes the executable smoke suite;
2. two identical builds of the same input set produce byte-identical COFF objects and build manifests (spec §14.2, §16.2; no normalization);
3. the build manifest fields satisfy spec §14.4 (no self-hash; stage-invariant version/identity field; repository-relative paths; identical relative output paths for comparison builds);
4. the project-owned runtime passes its allocator/trap/I/O conformance checks per spec §15 and ADR-004 (deterministic reuse rule, 0xDD poisoning, duplicate/invalid-release traps, zero-size rule, resource-exhaustion as explicit `null`);
5. the verification harness and corpora are committed and reproducible without network access after the host toolchain is present (ADR-001 build/dependency policy);
6. integration evidence is recorded under `docs/verification/` and Reviewer-verified; Main Designer accepts the M0 milestone gate.

## 4. Repository layout (proposed, under the AI-Co repo)

```
projects/AI-Co/
  bootstrap/                 # all bootstrap-line source (committed)
    build/                   # build entry points, toolchain init, per-area build fragments
    src/
      diag/                  # diagnostic record model, JSONL emitter, span model, ordering
      load/                  # source loader, UTF-8 validation
      lex/                   # deterministic lexer
      ast/                   # typed syntax representation (AST) — owned with parser package
      parse/                 # single-meaning parser + deterministic recovery
      name/                  # name binding, scopes, visibility, modules, imports
      types/                 # type identity, layout, conversions, operator typing
      const/                 # constant-expression evaluator
      sema/                  # semantic validation: declarations, statements, control flow, reachability
      ir/                    # canonical target-neutral IR
      backend/               # deterministic x86-64 code generation
      coff/                  # deterministic COFF emission
      driver/                # pipeline driver, build manifest, external link integration, main()
    runtime/
      rt_mem/                # allocator (ADR-004 temporal baseline)
      rt_io/                 # file I/O
      rt_proc/               # process args, exit
      rt_trap/               # trap reporting
    stage0/                  # M0 build outputs — GITIGNORED (existing .gitignore rule)
    stage1/                  # M1 Stage-1 compiler outputs — GITIGNORED
    stage2/                  # M1 Stage-2 compiler outputs — GITIGNORED
  tests/
    conformance/             # valid programs + expected observable behavior
    negative/                # invalid programs + expected diagnostic codes/spans/order
    smoke/                   # representative executable + trap programs
    harness/                 # Python verification harness (uses `python`, not `python3`)
    artifacts/               # harness temp/evidence output — GITIGNORED
  docs/
    contracts/               # IR contract (Planner-owned implementation contract area)
    planning/                # this plan, work-package manifest, planning handoff
    verification/            # M0/M1 integration evidence
  spec/  docs/adr/  research/  # existing, read-only for this milestone
```

Build outputs live only under `bootstrap/stage0|1|2/` and `tests/artifacts/` (both already covered by `.gitignore` or added in WP-M0-01). All build trees, caches, and temporary artifacts stay on the E: workspace per charter/ADR-001; nothing may be written to the critically constrained C: drive.

## 5. Build conventions (established in WP-M0-01, binding on later packages)

- One deterministic build entry point per host compiler: `bootstrap/build/` scripts that initialize the exact environment — `vcvarsall.bat x64` (Build Tools 2026) for MSVC, or explicit full paths to LLVM 22.1.8 binaries (off PATH per baseline) for Clang.
- **Never invoke bare `link` from Git Bash** (coreutils `link.exe` shadows MSVC `link.exe` per baseline §5.11). Linker invocations use explicit paths or an initialized developer environment.
- The top-level build entry point aggregates **per-area build fragments** (e.g., `bootstrap/build/*.txt` or equivalent), so each source area's file list is owned by the package that creates that area and later packages never edit another package's build fragment.
- Deterministic flags: reproducible/debug-info paths repository-relative or trimmed, `/Brepro` (or documented equivalent) for the PE link step, no timestamps in artifacts. Exact flag sets are recorded in the build manifest per spec §14.4/§16.3.
- No network access required to build after the accepted host toolchain is present (ADR-001 §build policy). No third-party compiler libraries, parser generators, or package managers.
- Host toolchain versions and identities are recorded in comparison evidence (spec §16.3), never in the build manifest (spec §14.4).

## 6. Verification harness design (WP-M0-05)

- Language: Python 3.11.15 (verified present as `python`; do **not** use `python3`, which resolves to a broken Store alias per baseline).
- Responsibilities:
  - run the conformance corpus: compile + execute, compare stdout bytes and exit code against expected records;
  - run the negative corpus: compile, assert emitted JSONL diagnostic set (codes, primary/secondary spans, ordering per DIAGNOSTIC-CONTRACT §9) against expected records;
  - run the smoke corpus: deterministic execution of representative and trap programs (trap exit code 70, trap record on stderr);
  - determinism runner: build the same input set twice, byte-compare COFF objects + manifests;
  - identity runner (M1): compare Stage 1 vs Stage 2 primary artifacts and linked PEs byte-for-byte, and record the comparison input evidence per spec §16.3;
  - manifest validator: check spec §14.4 fields (no self-hash, stage-invariant version, relative paths, sorted options).
- Harness itself must be deterministic (no unordered iteration affecting output; repository-relative paths; no host identity embedded in reports).
- Harness fixtures allow unit-testing the harness with fake compiler output before Stage 0 exists (WP-M0-05 is verified independently of the compiler).

## 7. Serial sequencing and file-ownership discipline

- **Strictly serial.** One Specialist at a time, one code area at a time (Marcel's constraint). No parallel specialist lanes within M0. The Coordinator dispatches packages in the manifest order; each package's card carries its dependency edges.
- **File ownership.** Each work package owns a disjoint repository area (see manifest §2 matrix). Package N+1 must not modify files owned by package N until N is verified (accepted + review passed). Reading a previously owned area (e.g., AST definitions consumed by downstream stages) is permitted; **modifying** it is not.
- **Downstream gap rule.** If a downstream package discovers it needs to extend an earlier package's owned artifact (e.g., AST node, diag field, IR op), it does **not** edit opportunistically; it records the gap and returns it to the Planner via the Coordinator for a re-planning/scope-amendment decision. This preserves both the serial rule and the "no silent architecture" discipline.
- **Corpus-before-compiler.** The conformance/negative/smoke corpora (WP-M0-02..04) are authored from the specification **before** the compiler stages are implemented, so the specification and suite (not the C17 implementation) define AI-Co behavior (ADR-001 risk control: "Stage-0 semantics become accidental language policy").

## 8. Host-toolchain requirement by package

- **Require the C17 host toolchain (MSVC cl and/or LLVM Clang per baseline):** WP-M0-01 (build entry verification), WP-M0-06..19 (all compiler and runtime implementation), WP-M0-20 (integration runs).
- **Pure spec/verification scaffolding (no C17 toolchain):** WP-M0-02, WP-M0-03, WP-M0-04 (corpus authoring from spec), WP-M0-05 (Python harness; verified with fixtures).
- M1 packages require Stage 0 (and therefore the C17 host toolchain) plus the accepted external linker in deterministic mode (spec §16.3).

## 9. Confidence, assumptions, risks

**Confidence:** Requirements confidence High (spec v0.1.1 Accepted, all review gates closed, no open questions). Architecture confidence High (ADR-001/002/004 Accepted; pipeline stages, runtime contract, and bootstrap contract normative). The only delegated design item is the IR instruction set (spec §14.1(6)), handled by WP-M0-16 with a Main Designer review gate.

**Assumptions (recorded; owners where applicable):**
1. Host toolchain remains as recorded in `ENVIRONMENT_BASELINE_2026-08-08.md` (MSVC on PATH via vcvarsall x64; LLVM off PATH, full paths required). Owner: Coordinator if environment changes.
2. The C17 runtime serves both Stage 0 and the M1 Stage 1/2 toolchain; the runtime language is not required to be AI-Co for M1 (spec requires the project-owned **linker** in AI-Co only at M2). Assumption to be revisited at M1 planning. Owner: Planner at M1 refresh.
3. Conformance/negative/smoke corpora are authored from spec §18/§19 and the diagnostic contract; they are the normative oracle, not implementation-derived.
4. `bootstrap/stage0|1|2/` and `tests/artifacts/` remain gitignored build output; source never lives under those names.
5. No same-width wrapping arithmetic and no function pointers are needed for the C17 compiler itself (spec §11.3, §17.3); if the compiler demonstrates a concrete need, the preserved triggers route to the Main Designer (OQ-002, §17.3).
6. Private repo: no publication, no executable release gate in M0.

**Material risks and controls:**

| Risk | Control |
|---|---|
| Stage-0 semantics become accidental language policy | Corpus authored from spec before implementation (WP-M0-02..04); negative suite asserts stable codes/spans |
| Determinism leaks (timestamps, absolute paths, unordered iteration) | Zero/canonical metadata rules in every emitter package; determinism runner in harness; byte-identity in integration gate |
| `link` PATH collision silently corrupts builds | Explicit linker invocation convention (WP-M0-01) enforced in every package |
| C: drive pressure | All outputs on E: under gitignored dirs; verified in WP-M0-01 acceptance |
| IR design becomes architecture-by-specialist | WP-M0-16 contract draft reviewed by Main Designer before implementation proceeds |
| Backend effort delays language validation | Backend is a bounded package after semantic validation; optimization deferred to M2+ per ADR-001 |
| Corpus too large for one junior package | Three separate corpus packages (conformance/negative/smoke), each bite-sized |
| Scope creep into M2 (project-owned linker) | Explicit exclusion in manifest; M2 only as boundary note |

**Escalation:** spec/contract ambiguity → Planner via Coordinator; architecture/feature gap → Main Designer; evidence gap → Researcher; environment/toolchain failure → Coordinator; verification failure or review finding → block and route per OM §11/§13; no guessing through missing facts.

## 10. Review and acceptance plan

- Every package: self-review (OM §11 stage 1) plus the review class stated in the manifest (mostly Reviewer independent review; WP-M0-16 additionally Main Designer architecture review of the IR contract; WP-M0-20 additionally Main Designer acceptance of the M0 gate).
- M0 milestone acceptance: Reviewer verification of `docs/verification/` evidence; Main Designer accepts the M0 gate. No human gate applies at M0 (no release, no public deployment; LICENSE unchanged).
- Authority attribution per the live trial (Controls 2/4) applies to all reports carrying decisions/gates in this milestone.

## 11. Coordinator handoff

- The work-package manifest (`AI-CO-STAGE0-WORK-PACKAGE-MANIFEST-2026-08-09.md`) contains every field the Coordinator needs to create structurally correct cards: objective, scope/exclusions, dependencies, inputs, artifacts/area, capability, acceptance criteria, verification/review class, risks, escalation, workspace, delivery destination.
- Dispatch **serially** in manifest order; do not start WP-M0-N+1 until WP-M0-N is verified (dependency edges are explicit on each card).
- Assign each package to its stated profile (`senior_specialist` / `junior_specialist`); do not parallelize same-milestone packages.
- M1 cards are included at milestone level with a Planner refinement checkpoint after M0 verification.
