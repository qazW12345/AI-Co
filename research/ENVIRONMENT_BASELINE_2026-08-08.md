# AI-Co Current-Machine Environment Baseline

**Status:** Complete (Researcher self-review passed 2026-08-08; independent assurance optional per task)
**Owner:** Researcher (Sneedworks role)
**Decision owner:** Main Designer
**Intended use:** Evidence for AI-Co language architecture and bootstrap-language decisions.
**Evidence date:** 2026-08-08 (probes run 15:01–15:08 local, UTC+02:00)
**Project:** AI-Co (workspace `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`)
**Kanban task:** t_e198100d
**Governing sources:** `governance/CONSTITUTION.md`; `governance/OPERATIONS_MANUAL.md`; `governance/profiles/RESEARCHER_PROFILE.md`; `governance/HERMES_DEPLOYMENT_GUIDE.md`; task record t_e198100d (authored by the Main Designer and Coordinated to the Researcher). The originating Designer conversation text was not retrievable from this profile's session store; the task body is treated as the governing instruction, consistent with Researcher Profile §Inputs.

---

## 1. Question

What is the current state of this Windows PC's hardware and software environment, in enough source-qualified detail to inform (a) the AI-Co language architecture and (b) which language/toolchain is the most evidenced choice for bootstrapping a compiler on this machine?

## 2. Scope and exclusions

**In scope:** Windows edition/version/build/architecture; CPU model, topology, and trustworthy compiler-target indicators (ISA features); RAM; GPU and available GPU tooling; fixed-drive capacity/free space/filesystems (no serials); native virtualization and WSL state; shells; Git and GitHub CLI authentication status (identity/token detail suppressed); installed compilers, assemblers, linkers, SDKs, build tools, LLVM tooling; relevant language runtimes and package tools; PATH/tool-name hazards; local constraints affecting compiler bootstrap and self-hosting; evidence-based bootstrap-language shortlist; minimal non-destructive compile/link smoke test.

**Excluded (not performed):** system changes, installs, repository creation, architecture decisions, specification drafting, source implementation, credential inspection or disclosure, environment-variable values, account identity, serial numbers, network identity, browser/personal data, unnecessary PII. A credential-bearing file present in the workspace root was deliberately not read and is not reported.

## 3. Method and stopping condition

- All probes were read-only or used safely temporary artifacts outside production source, executed under the `researcher` profile.
- Reproducible probe scripts are retained beside this report in `research/` (`PROBE_01_OS_CPU_RAM.ps1`, `PROBE_02_GPU.ps1`, `PROBE_03_STORAGE.ps1`, `PROBE_04_VIRTUALIZATION.ps1`, `PROBE_05_TOOLCHAIN.sh`). Command families and exact observations are recorded in Section 12.
- Smoke-test sources and binaries were created under `%TEMP%\sneedworks_smoke\` and removed afterward (Section 8; cleanup verified — see Limitations for the one residual detail).
- Stopping condition: all in-scope areas probed with direct evidence; smoke tests executed; report written and verified; no secrets present; further checks would have required elevation, installs, system changes, or out-of-scope actions (disclosed in Sections 9–10).

## 4. Executive summary

| Area | Key result |
|---|---|
| OS | Windows 10 Pro, 22H2, build 10.0.19045.6466, 64-bit; installed 2024-02-15; last boot 2026-07-25. Past Microsoft end-of-support as of evidence date.[1] |
| CPU | Intel Core i7-4770 @ 3.40 GHz, 4 cores / 8 threads; firmware virtualization enabled; Haswell-class ISA (AVX2, FMA, BMI1/2, AES, SSE4.2; no AVX-512) verified by CPUID probe. |
| RAM | 16 GiB DDR3-1600 (2 × 8 GiB); ~1.6 GiB free at probe time (point-in-time). |
| GPU | NVIDIA GeForce GTX 1060 6 GB (compute capability 6.1); driver 580.88; CUDA Toolkit 12.6.68; Nsight Compute 2024.3.1 / Nsight Systems 2024.4.2; Vulkan tools; no cuDNN found in CUDA include dir. |
| Storage | C: 237.8 GB NTFS — **3.2 GB free (1.4%)**; D: 476.9 GB NTFS — 22.3 GB free (4.7%); E: 1397.3 GB NTFS — 726.4 GB free (52%); F: 100 MB System Reserved. |
| Virtualization | No hypervisor running (HypervisorPresent=False); Hyper-V and Virtual Machine Platform features Disabled; WSL optional feature Enabled; WSL2 cannot start (VMP disabled); no WSL distros installed; QEMU 11.0.50 present. |
| Shells | Git Bash (MSYS2/MINGW64, bash 5.2.26), PowerShell 5.1.19041.6456, cmd (Windows 10.0.19045.6466). |
| Auth | GitHub CLI: not logged in to any host. Git: credential manager configured; no user identity configured at system or global level (no values disclosed). |
| Compilers | MSVC cl 19.50.35717 (VS Build Tools 2026, on PATH); MSVC 14.44.35207 also present in VS Community 2022 (off PATH); LLVM/Clang 22.1.8 installed (off PATH); ml64 (MASM) on PATH; no GCC/MinGW/MSYS2. |
| Build systems | No CMake/make/ninja/autotools on PATH; CMake + ninja bundled inside VS Build Tools 2026 (off PATH); MSBuild present in both VS instances. |
| Runtimes | Python 3.11.15 (Hermes venv, on PATH as `python`), pip 26.1.2, uv 0.12.2; Node.js v22.23.0 (Hermes bundle, on PATH) + v18.20.2 (Program Files); .NET runtimes 6.0.36/7.0.20/8.0.8 (**no SDK**); Java 21 JRE only (no javac); no Rust, Go, Zig, TCC. |
| Smoke test | MSVC cl, clang-cl, clang driver + LLD, and ml64 assembler+link all compiled/linked/ran successfully (x64). |

## 5. Findings by area

### 5.1 Operating system

| Observation | Value | Source |
|---|---|---|
| Caption | Microsoft Windows 10 Pro | PROBE_01 (Win32_OperatingSystem) |
| Version / build | 10.0.19045 (22H2), UBR 6466 | PROBE_01 (registry `HKLM\...\Windows NT\CurrentVersion`, `cmd ver`) |
| Architecture | 64-bit (OSArchitecture=64-bit; CPUID target x86_64) | PROBE_01; PROBE_05 |
| ProductType / InstallationType | 1 (Work Station), Client | PROBE_01 |
| Install date | 2024-02-15 | PROBE_01 |
| Last boot | 2026-07-25 08:23 local | PROBE_01 |
| Support status | Windows 10 Home/Pro reached end of support on October 14, 2025; this machine is past that date as of the evidence date.[1] | Microsoft Learn lifecycle page (fetched 2026-08-08) |

Interpretation: 22H2 is the final Windows 10 feature release; the machine receives no further security servicing as of the evidence date.[1]

### 5.2 CPU and compiler-target indicators

| Observation | Value | Source |
|---|---|---|
| Model | Intel(R) Core(TM) i7-4770 CPU @ 3.40 GHz | PROBE_01 (Win32_Processor) |
| Topology | 1 socket, 4 cores, 8 logical processors; L2 1024 KB, L3 8192 KB | PROBE_01 |
| Clock | 3401 MHz (current and max reported) | PROBE_01 |
| Virtualization | VirtualizationFirmwareEnabled=True; SecondLevelAddressTranslation=True; VMMonitorModeExtensions=True | PROBE_01, PROBE_04 |
| ISA (CPUID, compiled+run) | sse2/sse3/ssse3/sse4.1/sse4.2=1, popcnt=1, aes=1, avx=1, fma=1, bmi1=1, bmi2=1, avx2=1, avx512f=0 | cpufeatures probe (Section 8, test 5) |

Interpretation: Haswell-generation (2013) desktop part. Trustworthy compiler-target floor is SSE2; the machine supports AVX2/FMA/BMI2 and 64-bit only. AVX-512 must not be assumed. `Win32_Processor.Architecture=9` corresponds to x64 per Microsoft's documented enum (cross-checked against OSArchitecture=64-bit and the x86_64-pc-windows-msvc clang target).

### 5.3 RAM

| Observation | Value | Source |
|---|---|---|
| Total physical | 17,104,297,984 bytes (~16 GiB) | PROBE_01 (Win32_ComputerSystem) |
| Modules | 2 × 8 GiB DDR3, 1600 MT/s configured, manufacturer code "1315" (JEDEC code; commonly decoded as Samsung — **inference, unverified**) | PROBE_01 (Win32_PhysicalMemory) |
| Free at probe | 1,643,240 KB (~1.57 GiB) — point-in-time | PROBE_01 |
| Virtual memory | ~47.2 GiB total, ~18.4 GiB free at probe (pagefile in use) | PROBE_01 |

### 5.4 GPU and GPU tooling

| Observation | Value | Source |
|---|---|---|
| GPU | NVIDIA GeForce GTX 1060 6 GB, 1920×1080@60 | PROBE_02 (Win32_VideoController), nvidia-smi |
| VRAM | 6144 MiB total, 2522 MiB free at probe; compute capability 6.1 | nvidia-smi |
| Driver | 580.88 (2025-07-27 driver date observed) | nvidia-smi; PROBE_02; registry |
| CUDA toolkit | 12.6.68 (nvcc, cuobjdump); CUDA VS integration installed | PROBE_02; nvcc --version; registry |
| Nsight | Nsight Compute 2024.3.1, Nsight Systems 2024.4.2, Nsight VS Edition 2024.3.0 | PROBE_02; registry |
| Vulkan | `vulkaninfo.exe` in System32 (Vulkan runtime present) | PROBE_02 |
| Absent | `clinfo`, `dxc`, glslangValidator, glslc, `nsys` on PATH; cuDNN not found under CUDA v12.6 include | PROBE_02 |

Note: `Win32_AdapterRAM` reported ~4 GB (known 32-bit cap); `nvidia-smi` (6144 MiB) is treated as authoritative for VRAM. Contradiction documented in Section 11.

### 5.5 Storage (no serials)

| Drive | Size | Free | FS | Disk model (observed) | Source |
|---|---:|---:|---|---|---|
| C: | 237.8 GB | 3.2 GB (1.4%) | NTFS | SSDPR-CX400-256-G2 (256 GB SSD) | PROBE_03 |
| D: | 476.9 GB | 22.3 GB (4.7%) | NTFS | SSDPR-CX400-512-G2 (512 GB SSD) | PROBE_03 |
| E: | 1397.3 GB | 726.4 GB (52%) | NTFS | SAMSUNG HD154UI (1.5 TB HDD) | PROBE_03 |
| F: | ~100 MB System Reserved | — | NTFS | on C: disk | PROBE_03 |

Partition layout (Win32_DiskPartition, GPT): Disk 0 (256 GB SSD) holds the System partition (boot), the C: partition, a ~100 MB System partition, and a ~0.5 GB unknown (likely recovery) partition; Disk 1 (512 GB SSD) is D:; Disk 2 (1.5 TB HDD) is E:. Interface type reported "IDE" for all three — interpreted as SATA drives reporting via WMI in IDE-compatible mode (low-risk inference; not verified).

**Constraint:** C: is critically low on free space (1.4%). This is material for any build/install planning.

### 5.6 Virtualization and WSL

| Observation | Value | Source |
|---|---|---|
| Hypervisor running | No (HypervisorPresent=False) | PROBE_04 |
| CPU virtualization | Enabled in firmware (VT-x, SLAT, VM Monitor Mode) | PROBE_04 |
| Windows features | Hyper-V family: Disabled; VirtualMachinePlatform: Disabled; Containers: Disabled; Microsoft-Windows-Subsystem-Linux: **Enabled** | PROBE_04 (Win32_OptionalFeature) |
| WSL | `wsl.exe`/`wslconfig.exe` present; WSL version 2.7.11.0; Default Version 2; **no distributions installed**; WSL2 reports it cannot start ("virtualisation is not enabled") | PROBE_04, PROBE_05, `wsl --status` |
| Emulation | QEMU 11.0.50 (`qemu-system-x86_64.exe` at C:\Program Files\qemu) | PROBE_04, registry |
| Absent | Docker, Podman, VirtualBox, VMware tools | PROBE_04 |

Contradiction note: firmware VT-x is enabled, yet WSL2 says virtualization is "not enabled". Resolution: the Windows **Virtual Machine Platform optional feature is Disabled**, so the WSL2 hypervisor cannot start; this is a Windows feature state, not a CPU capability failure (see Section 11).

### 5.7 Shells

| Shell | Version/identity | Source |
|---|---|---|
| Git Bash (MSYS2/MINGW64) | bash 5.2.26(1) x86_64-pc-msys; MSYS kernel 3.4.10 (MINGW64_NT-10.0-19045) | PROBE_05; session env |
| PowerShell | 5.1.19041.6456 | direct probe |
| cmd | Microsoft Windows [Version 10.0.19045.6466] | direct probe |
| WSL shell | `wsl` present (2.7.11.0); no distro to run | PROBE_05 |

### 5.8 Git and GitHub CLI authentication (sanitized)

| Observation | Value | Source |
|---|---|---|
| GitHub CLI | **Not logged in to any GitHub host** | `gh auth status` (identity-bearing output redacted; only the "not logged in" state is reported) |
| Git credential helper | `manager` (Git Credential Manager) configured at system level | `git config --get credential.helper` |
| Git global config | Only `windows.appendatomically`, `gui.recentrepo` keys; **no user identity configured** at system or global level (absence reported, no values disclosed) | `git config --global --list --name-only` |
| Git system config keys | credential.helper, LFS clean/smudge/required filters, http.sslbackend, core.autocrlf, core.fscache, core.symlinks, pull.rebase, init.defaultbranch (names only) | `git config --system --list --name-only` |
| Git version | 2.46.0.windows.1 (mingw64) | PROBE_05 |
| GitHub CLI version | 2.97.0 (2026-07-31) | PROBE_05 |

No tokens, identities, or credential values were read, stored, or reported. The credential-bearing file in the workspace root was not read.

### 5.9 Compilers, assemblers, linkers, SDKs, build tools

| Tool | Version | On PATH | Evidence |
|---|---|---|---|
| MSVC `cl` | 19.50.35717 (VS Build Tools 2026, toolset 14.50.35717; Hostx64/x64) | Yes | PROBE_05, `where cl`, vswhere, smoke test 1 |
| MSVC `link` (PE linker) | 14.50.35728 | Yes (shadowed — see 5.11) | PROBE_05, `where link` |
| MSVC `lib` | 14.50.35728 | Yes | PROBE_05 |
| MSVC `dumpbin` | 14.50.35728 | Yes | PROBE_05 |
| MSVC `ml64` (MASM x64) | 14.50.35717 | Yes | PROBE_05, smoke test 4 |
| MSVC `ml` (MASM x86) | absent from PATH | No | PROBE_05 |
| MSVC 14.44.35207 | VS Community 2022 (off PATH) | No | `ls` MSVC toolsets |
| LLVM/Clang | 22.1.8 (clang, clang-cl, clang++, clangd, flang, lld-link, ld.lld, llvm-ar/lib/objdump/rc/ml64/strip, etc.; 90 bin entries) | **No** | PROBE_05, `ls` + clang --version |
| LLD | 22.1.8 | No (with LLVM) | clang driver smoke test 3 |
| GCC / MinGW / MSYS2 gcc | absent | — | PROBE_05 |
| TCC, Zig | absent | — | PROBE_05 |
| CMake | bundled inside VS Build Tools 2026 (`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) | **No** | `ls`, `where cmake` (empty) |
| Ninja | bundled inside VS Build Tools 2026 (`...\CMake\Ninja\ninja.exe`) | **No** | `ls` |
| MSBuild | VS Build Tools 2026 and VS Community 2022 | No (invoked by full path) | `ls` |
| Windows SDK | 10.0.26100.0 (Include/Lib); installer display version "10.1.26100.7705" (see 5.10/11) | — | `ls` Windows Kits, registry |
| vcvarsall.bat | present in both VS instances | — | `ls` |
| VS Installer | Visual Studio Build Tools 2026 18.4.2; Visual Studio Community 2022 17.14.29 | — | vswhere, registry |

### 5.10 Language runtimes and package tools

| Tool | Version | Notes | Evidence |
|---|---|---|---|
| Python | 3.11.15 (`python` → Hermes venv) | pip 26.1.2, uv 0.12.2 | PROBE_05 |
| `python3` | WindowsApps Store alias (stub) | No version output; do not rely on it | PROBE_05, `where python3` |
| Node.js | v22.23.0 (Hermes bundle, on PATH); v18.20.2 (Program Files, registered) | Two installs; PATH resolves to v22.23.0 | PROBE_05, `where node`, registry |
| npm/npx | 10.9.8 | Hermes bundle | PROBE_05 |
| .NET | Runtimes 6.0.36 / 7.0.20 / 8.0.8; **no SDK installed** (`dotnet --list-sdks` empty) | C# compilation unavailable without an SDK install | PROBE_05, `dotnet --list-sdks` |
| Java | OpenJDK 21.0.2 JRE only (Eclipse Temurin; registry: Eclipse Temurin JRE 21.0.2+13) | `javac` absent | PROBE_05, registry |
| Ruby, PHP, Swift, Lua, R, Go, Rust | absent | — | PROBE_05 |
| Perl | present (MSYS) | — | PROBE_05 |

### 5.11 PATH and tool-name hazards

Findings (resolution evidence from `where.exe` and `command -v`, PATH values themselves not disclosed per exclusions):

1. **`link` collision (material):** in the resolved PATH, `C:\Program Files\Git\usr\bin\link.exe` (GNU coreutils hard-link utility) precedes the MSVC `link.exe`. A bare `link` invocation in Git Bash therefore runs coreutils `link`, not the PE linker. Build scripts must use full paths or an initialized developer environment.
2. **`python3` vs `python`:** `python` → Hermes venv 3.11.15; `python3` → WindowsApps Store alias stub (no interpreter version). Scripts must use `python` (or `uv`).
3. **Clang/LLVM off PATH:** LLVM 22.1.8 is fully installed but absent from PATH; use full paths or add to PATH only as an explicit system change.
4. **CMake/ninja off PATH:** bundled inside VS Build Tools 2026 only.
5. **Node dual install:** Hermes-bundled v22.23.0 shadows Program Files v18.20.2 in PATH resolution; version-sensitive tooling must account for this.
6. **MSVC needs a developer environment:** `INCLUDE`/`LIB`/`LIBPATH`/`VSINSTALLDIR` are not set in the session environment (only PATH, TEMP/TMP, USERPROFILE, HOMEDRIVE/HOMEPATH observed). All smoke tests were run with `vcvarsall.bat x64` initialization and succeeded; without it, bare `cl`/`clang-cl` invocation cannot locate headers/libs (inference consistent with MSVC behavior; failure without vcvars was not separately tested).
7. **`ml` (x86 MASM) absent** from PATH; `ml64` present.
8. **No POSIX build tools on PATH** (make, cmake, autoconf, automake, libtool, pkg-config, bison, flex, m4 all absent), so classic `./configure && make` flows are not available without installs.

## 6. Local constraints affecting compiler bootstrap and self-hosting

1. **Disk space on C: is the dominant constraint** (3.2 GB free, 1.4%). Compiler builds, tool installs, caches, and temp directories must target E: (726 GB free) or D:. Any plan that assumes multi-GB installs or build trees on C: will fail.
2. **Toolchain reachability is fragmented:** compilers exist (MSVC on PATH; LLVM off PATH; CMake/ninja bundled off PATH) but no single developer environment is wired. Bootstrap tooling should standardize on `vcvarsall.bat x64` + explicit LLVM/CMake paths, or a Coordinator-approved PATH change.
3. **No GCC/MinGW/MSYS2 toolchain:** POSIX-style native builds (make/autotools, `-fuse-ld=bfd` GNU flows) are unavailable. The Git Bash/MSYS environment supplies coreutils, perl, strace (cygwin) but no compiler.
4. **`link` collision** can silently break any script that invokes `link` bare.
5. **WSL2 is non-functional** (Virtual Machine Platform disabled, no distro); Linux-side self-hosting/testing would require enabling VMP + installing a distro (system changes, out of scope here) or using QEMU (present).
6. **CPU capability floor:** x86-64 with AVX2/FMA/BMI2; no AVX-512. Code generation must default to ≤ AVX2; 4C/8T Haswell limits parallel build capacity.
7. **RAM pressure:** ~1.6 GiB free at probe time; 16 GiB total. Parallel builds of large projects will contend with the running workload.
8. **Windows 10 is past end of support** as of the evidence date,[1] so no further OS security patches; relevant to long-lived toolchain and CI decisions, not to local functionality observed.
9. **GPU compute:** CUDA 12.6 + GTX 1060 (sm_61) is usable for GPU experiments; cuDNN is absent; driver 580.88 is recent.
10. **No .NET SDK, no javac, no Rust/Go/Zig/TCC:** any bootstrap choice among those languages requires an install first (out of scope; a decision for the Main Designer/Coordinator).

## 7. Bootstrap-language shortlist (evidence-based; decision left to Main Designer)

Candidates ranked by direct machine evidence (verified by smoke tests where marked):

1. **C (primary candidate)** — matches AI-Co's stated C-based intent (ADR-003 context). Verified working on this machine: MSVC `cl` 19.50.35717 and LLVM Clang 22.1.8 (clang-cl and clang driver + LLD) both compile/link/run x64 PE executables against Windows SDK 10.0.26100. Both support C17/C23-class features. Two independent verified compilers reduce single-toolchain risk.
2. **C++ (alternative)** — same two verified toolchains; viable if the implementation wants RAII/templates; adds language complexity not evidenced as needed.
3. **x64 assembly (support role)** — `ml64` 14.50.35717 verified (assemble + link + run); LLVM's `llvm-ml64` also present off PATH. Suitable for low-level runtime/startup code, not the compiler core.
4. **Scripting/tooling languages (support role, not compiler core)** — Python 3.11.15 (with pip/uv) and Node 22.23.0 are present and suitable for build glue, test harnesses, and code generation scripts; performance rules them out for the compiler core.
5. **Not viable without installs:** Rust, Go, Zig, TCC, GCC/MinGW, .NET/C# (no SDK), Java (JRE only, no javac).

The shortlist is evidence about what this machine can build today; it is not an architecture or language decision.

## 8. Smoke test results (temporary artifacts, cleaned)

Setup: sources under `%TEMP%\sneedworks_smoke\`; each test initialized the MSVC x64 developer environment via `vcvarsall.bat x64` (Build Tools 2026); outputs and sources removed afterward.

| # | Test | Command family | Result |
|---|---|---|---|
| 1 | MSVC C compile+link+run | `cl /nologo /W4 hello.c /Fe:hello_msvc.exe` → run | PASS — printed "smoke-ok: Microsoft C/C++ compiler"; exit 0 |
| 2 | clang-cl C compile+link+run | `clang-cl /nologo /W4 hello.c /Fe:hello_clang.exe` → run | PASS — printed "smoke-ok: clang 22.1.8 (clang-cl mode)"; exit 0 |
| 3 | clang driver + LLD | `clang -target x86_64-pc-windows-msvc -fuse-ld=lld -O1 hello.c -o hello_clangd.exe` → run | PASS — printed clang 22.1.8; exit 0 (one benign warning: `-W4` is clang-driver-unknown; ignored) |
| 4 | MASM64 assemble + C link | `ml64 /c add.asm` + `cl main_asm.c add.obj /Fe:hello_masm.exe` → run | PASS — printed "asm-result: 42"; exit 0 |
| 5 | CPUID ISA probe | `clang-cl cpufeatures.c` (uses `<intrin.h>` `__cpuid`) → run | PASS — AVX2=1, FMA=1, BMI1/2=1, AES=1, AVX-512F=0, etc. |
| 5a | CPUID probe attempt via `__builtin_cpu_supports` | clang-cl default link | **FAILED** — `lld-link: error: undefined symbol: __cpu_model`. Finding: clang on Windows does not auto-link compiler-rt builtins; `clang_rt.builtins-x86_64.lib` is shipped under `LLVM\lib\clang\22\lib\windows\`, so the failure is resolvable by explicitly linking builtins (not re-tested; documented as an unverified fix). The `<intrin.h>` CPUID probe succeeded, so the ISA evidence stands. |

## 9. Negative findings and unavailable checks

- No GCC/MinGW/MSYS2, no Rust/Go/Zig/TCC, no .NET SDK, no `javac`, no cuDNN (in CUDA include dir), no WSL distro, no Docker/Podman/VirtualBox/VMware.
- Not checked (would need elevation, install, or system change): exact Hyper-V hypervisor runtime state via elevated DISM; firmware-level settings beyond WMI; store-alias behavior of `python3` (deliberately not executed to avoid Store side effects); Microsoft Store app inventory; licensing/activation state (out of scope).
- The originating Designer conversation text was not retrievable; the task body was used (disclosed above).
- A CPUID probe first attempt failed (Section 8, 5a) and was reworked with a genuinely different method (`<intrin.h>`), per retry policy.

## 10. Limitations, assumptions, confidence

- **Point-in-time:** free RAM, free disk, GPU memory, process load, and boot time are snapshots of 2026-08-08; they will drift.
- **PATH dependence:** software absent from PATH may exist elsewhere (confirmed for LLVM, CMake, ninja); conversely PATH presence does not imply a working developer environment (MSVC requires vcvars).
- **Non-elevated inspection:** some Windows feature states were read via CIM (Win32_OptionalFeature), which is authoritative for InstallState but does not reflect hypervisor runtime internals.
- **Inferences (marked):** RAM manufacturer code "1315" → Samsung (unverified); "IDE" interface type → SATA/IDE-compatible reporting (low risk); MSI MS-7816 board identity beyond the reported model (unverified); `Architecture=9` → x64 (cross-checked).
- **Confidence:** High for OS/CPU/RAM/GPU/storage/toolchain presence and versions (direct WMI/registry/CLI observation); High for ISA (CPUID executed); High for smoke tests (executed with exit codes); Medium for "compiler bootstrap works end-to-end on this machine" beyond the tested hello-world/asm scope; Medium for WSL interpretation (feature state is direct, runtime behavior inferred); Low for anything requiring elevation (not performed).
- **Cleanup disclosure:** all smoke-test files and the temporary directory were removed and verified; one removal attempt initially failed due to a process holding the directory as its working directory, and succeeded on retry after the shell moved out of it. The web-evidence temp file (`win10_lifecycle.*`) under `%TEMP%` is evidence material; it contains no secrets and may be deleted.

## 11. Contradictions and resolutions

| Contradiction | Resolution |
|---|---|
| `Win32_AdapterRAM` ≈ 4 GB vs `nvidia-smi` 6144 MiB | Known 32-bit cap in WMI field; nvidia-smi authoritative (6 GB). |
| WSL says "virtualisation is not enabled" while firmware VT-x is enabled | Windows **Virtual Machine Platform** feature is Disabled; CPU virtualization is available but the WSL2 hypervisor cannot start. |
| Windows SDK installer displays "10.1.26100.7705"; Windows Kits dir is `10.0.26100.0` | Installer display/update version vs on-disk toolset version; on-disk SDK is 10.0.26100.0. |
| `where link` resolves Git's coreutils `link.exe` before MSVC `link.exe` | PATH ordering hazard; MSVC linker is reachable by full path or via vcvars/dev prompt. |
| Node v22.23.0 on PATH vs registered Node 18.20.2 | Two installs; PATH wins with v22.23.0 (Hermes bundle). |
| `clang` installed (22.1.8) but `command -v clang` empty | LLVM not on PATH. |
| `python3` "found" but prints no version | WindowsApps Store alias stub, not a real interpreter. |

## 12. Command provenance (reproducibility)

All commands executed 2026-08-08 from the `researcher` profile on this machine; identities/tokens/serials never captured. Probe scripts retained in `research/` beside this report.

| # | Command family (exact commands in the retained scripts) | Purpose | Section |
|---|---|---|---|
| P1 | `powershell -File PROBE_01_OS_CPU_RAM.ps1` (Win32_OperatingSystem, registry NT CurrentVersion, Win32_Processor, Win32_PhysicalMemory, Win32_ComputerSystem) | OS/CPU/RAM | 5.1–5.3 |
| P2 | `powershell -File PROBE_02_GPU.ps1` (Win32_VideoController; Get-Command for GPU tools) | GPU | 5.4 |
| P3 | `powershell -File PROBE_03_STORAGE.ps1` (Win32_LogicalDisk/Volume/DiskDrive/DiskPartition) | Storage | 5.5 |
| P4 | `powershell -File PROBE_04_VIRTUALIZATION.ps1` (Win32_ComputerSystem, Win32_OptionalFeature, Get-Command) | Virtualization | 5.6 |
| P5 | `bash PROBE_05_TOOLCHAIN.sh` (`command -v` + `--version` for 60+ tools) | Toolchain/runtimes/shells | 5.7, 5.9, 5.10 |
| P6 | `cmd ver`, `where.exe <tool>` for cl/link/lib/dumpbin/ml64/python/python3/node/git/dotnet/clang/cmake/make; `type -a` equivalents | PATH hazards | 5.11 |
| P7 | `vswhere -all -products '*' -format json`; `ls` of MSVC toolsets, Windows Kits, VS CMake/Ninja dirs; MSBuild/vcvarsall presence | SDKs/build tools | 5.9 |
| P8 | `dotnet --list-sdks`, `dotnet --list-runtimes` | .NET | 5.10 |
| P9 | `nvidia-smi --query-gpu=name,memory.total,memory.free,driver_version,compute_cap`; `nvcc --version` | GPU detail | 5.4 |
| P10 | `gh auth status` (output redacted to boolean state); `git config --get credential.helper`; `git config --global/--system --list --name-only` | Auth (sanitized) | 5.8 |
| P11 | `wsl --status`, `wsl --list --verbose`; `qemu-system-x86_64 --version` | WSL/emulation | 5.6 |
| P12 | Registry uninstall scan (HKLM/HKCU Uninstall keys), dev-tool keyword filter, `DisplayName`/`DisplayVersion` only | Installed versions | 5.9–5.10 |
| P13 | Smoke tests (Section 8): `vcvarsall.bat x64` + `cl`, `clang-cl`, `clang -fuse-ld=lld`, `ml64`/`cl`, CPUID program | Verification | 8 |
| P14 | `curl` fetch of Microsoft Windows 10 lifecycle page; plain-text extraction; verbatim quote attached to ledger | External claim | 5.1 |

## 13. Implications for the Main Designer (evidence, not decisions)

- The most directly evidenced bootstrap path on this machine is **C compiled with MSVC and/or LLVM Clang 22.1.8** against the installed Windows SDK; both were smoke-verified end-to-end.
- **C: drive space (3.2 GB free) is the binding physical constraint**; any architecture/plan that stores build outputs, toolchains, or caches on C: should be reconsidered, and a destination policy (E: or D:) decided.
- Toolchain wiring (LLVM off PATH; CMake/ninja bundled off PATH; MSVC requires vcvars; `link` collision) needs an explicit operational decision before build automation can be reliable.
- If Linux cross-testing or self-hosting is required, WSL2 is currently non-functional and would need enabling (system change) or QEMU usage (present).
- Windows 10 being past end-of-support[1] may affect target-platform and security-posture decisions for the project.
- GPU (CUDA 12.6, sm_61) is available for any accelerator experiments; cuDNN absent.
- No decision is made here; the Researcher's role ends at evidence and routing.

## 14. Recommended further inquiry

- (Main Designer) Decide bootstrap language and toolchain wiring, including PATH/dev-environment policy and build-artifact storage location.
- (Coordinator, if approved) A follow-up task could validate clang + explicit `clang_rt.builtins` linking (Section 8, test 5a) and a CMake+ninja out-of-tree build against the bundled VS toolchain.
- (Researcher, on request) A disk-space audit of C: to identify reclaimable space if the Designer wants it (read-only).
- (Reviewer, optional) Independent assurance of this baseline if the architecture consequence warrants it.

## Sources

[1] https://learn.microsoft.com/en-us/lifecycle/products/windows-10-home-and-pro — Windows 10 Home and Pro lifecycle - Microsoft Learn
    > "Windows 10 will reach end of support on October 14, 2025."
