# AI-Co bootstrap

This directory holds the bootstrap line of the AI-Co project: the conservative
C17 Stage-0 compiler that will compile the AI-Co compiler, after which the
AI-Co compiler recompiles itself under the strict deterministic bootstrap
contract (Stage 1 / Stage 2 byte identity). See ADR-001 and the Stage-0
milestone plan for the architecture and the normative contract.

## Layout

| Path | Purpose |
|---|---|
| `build/` | Build entry points, toolchain initializers, per-area build fragments, and the build conventions (`build/CONVENTIONS.md`). |
| `src/` | Stage-0 compiler sources, one directory per pipeline area (`diag`, `load`, `lex`, `ast`, `parse`, `name`, `types`, `const`, `sema`, `ir`, `backend`, `coff`, `driver`). Added by later work packages. |
| `runtime/` | Project-owned runtime (`rt_mem`, `rt_io`, `rt_proc`, `rt_trap`). Added by later work packages. |
| `stage0/` | M0 build outputs. **Gitignored** — never commit files under this directory. |
| `stage1/`, `stage2/` | M1 Stage-1/Stage-2 compiler outputs. **Gitignored.** |

## Quick start (entry-point verification)

The build entry points compile a trivial C17 program through each accepted
host compiler and put the executable under `bootstrap/stage0/` on the E:
workspace. From the repository root, in Git Bash:

```bash
# MSVC host compiler (cl 19.50.35728 via vcvarsall.bat x64)
./bootstrap/build/build-stage0-msvc.cmd bootstrap/build/verify-hello.c
./bootstrap/stage0/msvc/verify-hello.exe
# expected: hello from AI-Co stage0

# LLVM Clang host compiler (clang-cl 22.1.8 by full path, off PATH)
./bootstrap/build/build-stage0-clang.cmd bootstrap/build/verify-hello.c
./bootstrap/stage0/clang/verify-hello.exe
# expected: hello from AI-Co stage0
```

Repeat builds produce byte-identical executables (deterministic flags,
`/Brepro` PE timestamp). To keep a second build in a separate output
directory, set `STAGE0_OUT_DIR` (e.g. `STAGE0_OUT_DIR='bootstrap\stage0\msvc-b'
./bootstrap/build/build-stage0-msvc.cmd bootstrap/build/verify-hello.c`); the
default output directories are `bootstrap/stage0/msvc` and
`bootstrap/stage0/clang`. The same commands work from `cmd`/PowerShell.

Build outputs never leave the E: workspace; nothing is written to the
critically constrained C: drive.

## Conventions

- Deterministic build conventions: `build/CONVENTIONS.md` (binding on every
  later work package). Read it before building or authoring a build fragment.
- Per-area source file lists are owned by their area packages as
  `build/<area>.txt` fragments; the top-level entry points aggregate them.
- Never invoke bare `link` from Git Bash (coreutils `link.exe` collision); the
  linker is reached inside an initialized developer environment or by explicit
  full path.

## Further reading

- Milestone plan: `docs/planning/AI-CO-STAGE0-MILESTONE-PLAN-2026-08-09.md` (§4 layout, §5 build conventions).
- Work-package manifest: `docs/planning/AI-CO-STAGE0-WORK-PACKAGE-MANIFEST-2026-08-09.md` (§2 ownership matrix).
- Architecture: `docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md`, `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md`.
- Environment evidence: `research/ENVIRONMENT_BASELINE_2026-08-08.md`.
