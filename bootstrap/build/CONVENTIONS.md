# AI-Co Stage-0 Build Conventions

**Status:** Accepted baseline (WP-M0-01, 2026-08-10)
**Owner:** WP-M0-01 (`bootstrap/build/` area; conventions binding on all later packages)
**Governing sources:** milestone plan `docs/planning/AI-CO-STAGE0-MILESTONE-PLAN-2026-08-09.md` §5; work-package manifest §2/§3 WP-M0-01; `research/ENVIRONMENT_BASELINE_2026-08-08.md`; ADR-001 (build and dependency policy); ADR-004 (Windows 10 22H2 x64 pinned bootstrap baseline).

This file establishes the deterministic build conventions every later Stage-0 work package builds on. Later packages own only their per-area build fragments (`bootstrap/build/<area>.txt`); they must not edit the entry points, initializers, this file, or another package's fragment. A needed change to this area is a downstream gap and returns to the Planner via the Coordinator (milestone plan §7).

## 1. Repository layout for build output

| Path | Purpose |
|---|---|
| `bootstrap/build/` | Build entry points, toolchain initializers, per-area fragments, this file. Committed. |
| `bootstrap/stage0/` | M0 build outputs (exes, objects, intermediates). **Gitignored.** |
| `bootstrap/stage1/`, `bootstrap/stage2/` | M1 Stage-1/Stage-2 compiler outputs. **Gitignored.** |
| `tests/artifacts/` | Harness temp/evidence output (WP-M0-05). **Gitignored.** |

All build outputs, caches, and temporary compiler artifacts live under the E: project workspace (ADR-001 build policy; baseline §6.1). Nothing may be written to the critically constrained C: drive. The gitignore rules are verified by acceptance criterion 5 of WP-M0-01.

> **Gitignore exception (WP-M0-01 deviation, disclosed):** the repository's pre-existing `build/` ignore rule matches any `build/` directory, including `bootstrap/build/` where the committed build entry points live. `.gitignore` therefore carries the scoped negation `!bootstrap/build/` (after the `build/` rule) so the accepted milestone layout can be committed. The manifest's ".gitignore (tests/artifacts only)" wording did not anticipate this collision; this is the minimal deviation required to land WP-M0-01's own expected artifacts. All other `build/` directories remain ignored. Routed for Reviewer confirmation; if a different convention is preferred (e.g. a top-level-only rule), the change returns to the Planner via the Coordinator.

## 2. Build entry points

One deterministic build entry point per accepted host compiler, both in `bootstrap/build/`:

| Entry point | Host compiler | Toolchain init |
|---|---|---|
| `build-stage0-msvc.cmd` | MSVC `cl` 19.50.35717 (VS Build Tools 2026) | `vcvarsall.bat x64` via `init-msvc.cmd` |
| `build-stage0-clang.cmd` | LLVM Clang 22.1.8 `clang-cl` (off PATH, full path) | `vcvarsall.bat x64` (SDK headers/libs) + `init-clang.cmd` (LLVM full paths) |

### 2.1 Usage

```
build-stage0-msvc.cmd  [source ...]
build-stage0-clang.cmd [source ...]
```

- `source` (optional): source files relative to the repository root. When any are given, they are compiled instead of the fragment aggregation. The output executable is named after the first source (`verify-hello.c` → `verify-hello.exe`).
- Output directory (optional): environment variable `STAGE0_OUT_DIR`, relative to the repository root. Defaults: `bootstrap\stage0\msvc` (MSVC) and `bootstrap\stage0\clang` (Clang). From Git Bash: `STAGE0_OUT_DIR='bootstrap\stage0\msvc-run2' ./bootstrap/build/build-stage0-msvc.cmd bootstrap/build/verify-hello.c`.
- With no arguments, the entry point aggregates the per-area fragments (§3). With no fragments and no explicit sources it fails with a clear message.
- Determinism check: build the same source twice (same flags; `STAGE0_OUT_DIR` may vary) and byte-compare the executables.

Entry points are cwd-independent: they `cd` to the canonical repository root first. They may be invoked from Git Bash, `cmd`, or PowerShell (from Git Bash, `./bootstrap/build/build-stage0-msvc.cmd` executes the batch through `cmd`).

### 2.2 Deterministic flag sets

Recorded here exactly; the build manifest records them per spec §14.4/§16.3 once the driver exists (WP-M0-19).

MSVC (`cl`, inside vcvars x64):

```
cl /nologo /std:c17 /O2 /W4 <sources> /Fo:<out>\obj\ /Fe:<out>\<exe>.exe /link /Brepro
```

Clang (`clang-cl` by full path, inside vcvars x64 for INCLUDE/LIB):

```
clang-cl /nologo /std:c17 /O2 /W4 /Brepro <sources> /Fo:<out>\obj\ /Fe:<out>\<exe>.exe
```

Rationale and reproducibility properties (verified 2026-08-10, WP-M0-01):

- `/std:c17` — conservative C17 per ADR-001.
- `/O2` — fixed optimization level. Determinism requires the exact same flags; no flag may vary between builds.
- `/W4` — baseline warning level (matches environment-baseline smoke tests).
- `/Brepro` — deterministic PE timestamp. Two consecutive builds of `verify-hello.c` produced **byte-identical exes** with both toolchains (`fc /b` / `cmp`, no normalization).
- No debug info is emitted (`/Zi`/`/Z7` absent), so no PDBs and no source-path embedding. If debug info is added later it must use repository-relative or trimmed paths (milestone plan §5), not absolute host paths.
- MSVC host note: `cl` embeds a per-invocation randomized identifier in each `.obj` (`.drectve` section); MSVC object files are therefore **not** byte-identical across builds. Clang-cl object files are byte-identical. Neither matters for this package (the verified artifact is the linked PE, which is byte-identical); object-level byte determinism for the Stage-0 compiler's own outputs is a project-owned contract (WP-M0-18 COFF emission, ADR-001), not a host-toolchain property.

### 2.3 Linker policy — never bare `link` from Git Bash

Per baseline §5.11 (re-verified 2026-08-10), `C:\Program Files\Git\usr\bin\link.exe` (GNU coreutils hard-link utility) precedes MSVC `link.exe` in the Git Bash PATH. A bare `link` from Git Bash runs coreutils `link`, not the PE linker.

Rules binding on every later package:

1. Never invoke bare `link` from Git Bash.
2. The linker is called only (a) inside an initialized developer environment — the entry points do this: `cl`/`clang-cl` resolve and invoke the linker (MSVC `link.exe` / `lld-link.exe`) internally within the vcvars-initialized environment, or `clang-cl` uses `lld-link` from its own LLVM bin directory — or (b) by explicit full path.
3. When a build script must call a linker directly, it either initializes the developer environment first (see `init-msvc.cmd`) or uses the full path to the linker (`%LLD_LINK%` from `init-clang.cmd`, or the MSVC `link.exe` full path from `where link` inside an initialized environment).
4. Linker identity/version is recorded only in comparison evidence (spec §16.3), never in the build manifest (spec §14.4).

## 3. Per-area build fragment convention

The top-level entry point builds the Stage-0 compiler by aggregating **per-area build fragments**, one per source area. This keeps each source area's file list owned by the package that creates that area.

Format of `bootstrap/build/<area>.txt`:

- File name: `<area>` matches the owning package's area name (manifest §2 matrix), e.g. `diag.txt` for `bootstrap/src/diag/**`.
- Content: one repository-root-relative source path per line (forward or back slashes both accepted; back slashes conventional), e.g. `bootstrap/src/diag/diag.c`.
- `#` at line start is a comment; blank lines are ignored.
- Lines must be sorted lexicographically (stable diffs).
- No duplicate paths across all fragments (areas are disjoint by the ownership matrix; a duplicate is a defect).
- Paths must not contain spaces or `!` (repository root is space-free on this baseline; fragment lines are consumed by the batch aggregation).
- Each area package authors **only** its own `<area>.txt` and never edits another package's fragment.

The entry point reads the fragments in sorted filename order (`dir /b /on`) and concatenates the lines. Source arguments given on the command line replace the aggregation.

## 4. Toolchain initializers

- `init-msvc.cmd` — locates and calls `vcvarsall.bat x64`. Pinned path: VS Build Tools 2026 `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat` (cl 19.50.35717). Fallback: `vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`. Fails loudly if no `vcvarsall.bat` is found.
- `init-clang.cmd` — pins LLVM 22.1.8 at `C:\Program Files\LLVM` (off PATH per baseline) and exports full paths: `CLANG_DIR`, `CLANG_CL`, `CLANG_DRIVER`, `LLD_LINK`, `LLVM_AR`. Fails loudly if the pinned directory is missing.

Environment deviation (toolchain moved, version changed, new host) → escalate to the Coordinator per the manifest; do not silently repin.

## 5. Environment hazards (binding)

- **`python` vs `python3`:** `python` is Python 3.11.15 (Hermes venv); `python3` is a broken WindowsApps Store alias. Build/harness scripts must use `python`.
- **C: drive pressure:** baseline reports C: at 3.2 GB free. No project build output, cache, or temporary artifact may be written to C:. All output goes to `bootstrap/stage0|1|2/` and `tests/artifacts/` (gitignored) on E:.
- **LLVM off PATH:** clang binaries are reached by the full paths pinned in `init-clang.cmd`, never by bare name.
- **No network:** the build must be reproducible without network access after the accepted host toolchain is present (ADR-001). No third-party compiler libraries, parser generators, or package managers.
- **No secrets:** secrets never enter the repository or build output.

## 6. Ownership and change discipline

- `bootstrap/build/` entry points, initializers, and this file: owned by WP-M0-01. Later packages do not edit them; a required change is a downstream gap → Planner via Coordinator.
- `bootstrap/build/<area>.txt`: owned by the area's package (manifest §2). A package adds its fragment in its own package; it does not touch other fragments.
- `verify-hello.c` in `bootstrap/build/`: WP-M0-01 verification program; kept for re-running entry-point verification. Not part of any fragment.
