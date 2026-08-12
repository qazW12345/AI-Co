# bootstrap/runtime — Windows API baseline (WP-M0-15c2)

**Status:** Implementation complete, pending review (WP-M0-15c2)
**Owner:** WP-M0-15c2 (manifest §2 file-ownership matrix)
**Governing sources:** ADR-004 (Windows 10 22H2 x64 pinned bootstrap baseline);
spec `AI-CO-LANGUAGE-SPECIFICATION.md` §15 (minimal runtime and system contract);
`spec/DIAGNOSTIC-CONTRACT.md` §10 (trap records, exit code 70);
`bootstrap/build/CONVENTIONS.md` (build conventions);
`research/ENVIRONMENT_BASELINE_2026-08-08.md` (observed host evidence);
work-package manifest §2/§3 (WP-M0-15c2).

This document is the **runtime-facing Windows API baseline enumeration** required
by ADR-004. It lists every Windows call the project-owned runtime makes, the
calling module and call site, the exact arguments and constants used, and the
observable failure behavior, all against the pinned operating-system baseline.

---

## 1. Pinned baseline (ADR-004)

ADR-004 records the Human Sponsor-approved selection:

> Windows 10 22H2 x64 is the pinned development-host and execution baseline for
> the Stage 0/1/2 bootstrap line. The project explicitly records that this
> operating-system baseline no longer receives ordinary OS updates.
> Runtime-facing Windows calls must be enumerated and documented against this
> baseline. Compatibility with Windows 11 or later is desirable but not a
> v0.1.0 conformance guarantee.

Baseline facts applied throughout this document:

- **OS:** Windows 10 Pro, 22H2, build 10.0.19045 (observed host build
  10.0.19045.6466, 64-bit; `research/ENVIRONMENT_BASELINE_2026-08-08.md`).
- **No-OS-updates baseline:** Windows 10 Home/Pro reached end of support on
  2025-10-14 (Microsoft lifecycle); the pinned baseline receives no further
  ordinary OS updates or security servicing. This is a recorded acceptance of
  the baseline's security posture for local, public, non-sensitive hobby
  development; it is not a release-support approval (ADR-004 §Consequences).
- **CPU architecture:** x86-64.
- **ABI:** the runtime uses the Microsoft x64 calling convention internally for
  the compiler-to-runtime boundary (spec §15.7). Calls into the Windows API use
  the same Windows x64 convention, which is the platform's native ABI on the
  baseline.
- **Import surface:** every enumerated call resolves statically through
  `kernel32.dll`. The runtime performs **no dynamic binding**
  (`LoadLibrary`/`GetProcAddress` are never used), no COM, no RPC, no
  console/UI API, and no CRT-heap or host-allocator API. The aggregate build
  links the default system import library (`kernel32.lib`) via the accepted
  toolchains (MSVC `cl` and Clang `clang-cl`); no other import library is
  required by the runtime.

All calls below are documented Win32 APIs present on Windows 10 22H2 and on
every Windows version since their introduction; no call requires a post-22H2
behavior. The runtime therefore exercises a stable, minimal, enumerable subset
of the baseline API.

---

## 2. Enumeration scope and method

### 2.1 What counts as a "runtime-facing Windows call"

A call is enumerated here iff:

1. it is made by the **runtime library sources** under `bootstrap/runtime/`
   (the `.c` files that implement the `rt.mem`, `rt.io`, `rt.proc`, and
   `rt.trap` modules of spec §15), **excluding** unit-test programs
   (`*_test.c`); and
2. it is a call into the **Windows API** — a function declared by `<windows.h>`
   and exported by a Windows system DLL.

C17 standard-library calls (`fwrite`, `fflush`, `exit`, `snprintf`,
`memset`, `memmove`) are **not** Windows API calls; they are the C runtime
surface and are documented separately in §6. The trap-record emitters use
`fwrite` on the CRT `stderr` stream, which the C runtime maps onto the OS
standard-error handle; the runtime itself never calls `WriteFile`/`GetStdHandle`
for trap emission (that is the C runtime's implementation detail, not a
runtime-facing Windows call enumerated here).

Unit-test programs (`rt_mem_*_test.c`, `rt_io_*_test.c`, `rt_proc_test.c`,
`rt_trap_test.c`) are **not runtime-facing**: they are build-time verification
harnesses, not part of the runtime delivered to programs. Their Windows usage is
enumerated separately in §7 so that an API audit of the whole
`bootstrap/runtime/` tree is complete and cannot drift unnoticed.

### 2.2 Verification method

The enumeration was produced by scanning every runtime library source file for
`<windows.h>` includes and Win32 function call sites, then cross-checking each
site's documented semantics against the pinned baseline. §8 records the exact
re-verification commands and expected results so the enumeration can be
regenerated mechanically and drift can be detected.

---

## 3. Master table of runtime-facing Windows calls

11 distinct Windows API functions are called by the runtime library. All are
exported by `kernel32.dll`.

| # | Windows API | Module (package) | Source file | Purpose in the runtime |
|---|-------------|------------------|-------------|------------------------|
| 1 | `GetSystemInfo` | `rt.mem` (WP-M0-14a1) | `bootstrap/runtime/rt_mem/rt_mem_alloc.c` | Query the host page size once; drives whole-page commit granularity |
| 2 | `VirtualAlloc` (MEM_RESERVE) | `rt.mem` (WP-M0-14a1) | `bootstrap/runtime/rt_mem/rt_mem_alloc.c` | Reserve the single controlled 64 MiB region on first use |
| 3 | `VirtualAlloc` (MEM_COMMIT) | `rt.mem` (WP-M0-14a1) | `bootstrap/runtime/rt_mem/rt_mem_alloc.c` | Commit whole pages incrementally inside the reserved region |
| 4 | `VirtualFree` (MEM_RELEASE) | `rt.mem` (WP-M0-14a1) | `bootstrap/runtime/rt_mem/rt_mem_alloc.c` | Release the controlled region (test/diagnostic reset path only) |
| 5 | `MultiByteToWideChar` | `rt.io` (WP-M0-15a1) | `bootstrap/runtime/rt_io/rt_io_core.c` | UTF-8 → UTF-16 conversion of the open path |
| 6 | `CreateFileW` | `rt.io` (WP-M0-15a1) | `bootstrap/runtime/rt_io/rt_io_core.c` | Open/create a file per `rt.io.open` mode |
| 7 | `SetFilePointerEx` | `rt.io` (WP-M0-15a1) | `bootstrap/runtime/rt_io/rt_io_core.c` | Position the file pointer at EOF on append-mode open |
| 8 | `ReadFile` | `rt.io` (WP-M0-15a1) | `bootstrap/runtime/rt_io/rt_io_core.c` | Read bytes from a file handle |
| 9 | `WriteFile` | `rt.io` (WP-M0-15a1) | `bootstrap/runtime/rt_io/rt_io_core.c` | Write bytes to a file handle |
| 10 | `CloseHandle` | `rt.io` (WP-M0-15a1) | `bootstrap/runtime/rt_io/rt_io_core.c` | Close an OS file handle (open failure path, `rt.io.close`, reset) |
| 11 | `GetStdHandle` | `rt.io` (WP-M0-15a2) | `bootstrap/runtime/rt_io/rt_io_stdio.c` | Obtain the process standard stream handles for `rt.io.stdin/stdout/stderr` |
| 12 | `GetCommandLineW` | `rt.proc` (WP-M0-15b) | `bootstrap/runtime/rt_proc/rt_proc.c` | Read the process UTF-16 command line for `rt.proc.args` |

Notes on the table:

- `GetSystemInfo` is called at most once (cached page size).
- `VirtualAlloc` appears in two call sites with two different allocation types
  (rows 2 and 3) and is counted as one API function.
- `rt.trap` (WP-M0-15c1, `rt_trap.c`) makes **no** Windows API calls; it uses
  only the C runtime (`fwrite`, `fflush`, `exit`) through the WP-M0-06 diag
  emitter. `rt_mem_reuse.c` (WP-M0-14b1) and `rt_mem_trap.c` (WP-M0-14b2) also
  make no Windows API calls.
- All `rt.io` and `rt.proc` modules include `<windows.h>` with
  `WIN32_LEAN_AND_MEAN` defined before the include, so the runtime pulls in the
  minimal Win32 header surface.

---

## 4. Per-call detail

### 4.1 `GetSystemInfo` — page size

- **Declaration:** `void GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);`
- **Call site:** `rt_mem_region_ensure()` in
  `bootstrap/runtime/rt_mem/rt_mem_alloc.c`, guarded by the cached page-size
  flag (`s_page_size == 0`), so at most one call per process.
- **Arguments:** `SYSTEM_INFO si` on the stack, discarded after
  `si.dwPageSize` is read.
- **Purpose:** the allocator commits virtual memory in whole pages
  (`rt_mem_region_commit`); the commit unit is `align_up(aligned + count,
  s_page_size)`. `dwPageSize` is the machine's page size (typically 4096 on
  x64); if the reported value is 0 the runtime substitutes 4096 defensively.
- **Failure behavior:** `GetSystemInfo` cannot fail (void). The page size is an
  environmental input (spec §15.6); allocation behavior stays deterministic
  given identical inputs.
- **Baseline notes:** available on all supported Windows versions; no version
  drift on 22H2.

### 4.2 `VirtualAlloc` — reserve the controlled region

- **Declaration:** `LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize,
  DWORD flAllocationType, DWORD flProtect);`
- **Call site:** `rt_mem_region_ensure()` in
  `bootstrap/runtime/rt_mem/rt_mem_alloc.c`, once per process (first
  allocation), when the region base is not yet set.
- **Arguments:** `lpAddress = NULL`, `dwSize = RT_MEM_REGION_SIZE` (64 MiB,
  `bootstrap/runtime/rt_mem/rt_mem_alloc.h`), `flAllocationType =
  MEM_RESERVE`, `flProtect = PAGE_READWRITE`.
- **Purpose:** reserve the allocator's single controlled virtual-address region
  (ADR-004 / spec §15.1). All returned addresses lie inside
  `[base, base + RT_MEM_REGION_SIZE)`.
- **Failure behavior:** a NULL return is treated as **resource exhaustion**:
  `rt_mem_core_alloc` returns NULL, never a trap (spec §15.1).
- **Determinism:** the region base is an OS-provided environmental input (ASLR
  makes it vary between processes; spec §15.6). Offsets within the region are
  deterministic.
- **Baseline notes:** `MEM_RESERVE` + later `MEM_COMMIT` is standard, long-stable
  behavior on 22H2; committed pages are guaranteed zero, which is the
  runtime's zero-initialization mechanism.

### 4.3 `VirtualAlloc` — commit pages inside the region

- **Declaration:** same as §4.2.
- **Call site:** `rt_mem_region_commit()` in
  `bootstrap/runtime/rt_mem/rt_mem_alloc.c`, once per allocation request that
  extends the committed range.
- **Arguments:** `lpAddress = (char *)s_region_base + start` (the next
  uncommitted offset), `dwSize = count` (page-aligned need minus committed
  bytes), `flAllocationType = MEM_COMMIT`, `flProtect = PAGE_READWRITE`.
- **Purpose:** commit whole pages so the requested byte range is accessible and
  zero-initialized.
- **Failure behavior:** a NULL return is resource exhaustion → `alloc_bytes`
  returns NULL, never a trap. A defensive internal-contract check also treats a
  committed address that is not the requested address as exhaustion (cannot
  happen on the pinned baseline).
- **Baseline notes:** committing a range inside a previously reserved region
  with `MEM_COMMIT` is guaranteed by the Win32 memory model on 22H2.

### 4.4 `VirtualFree` — release the controlled region (test/diagnostic reset only)

- **Declaration:** `BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize,
  DWORD dwFreeType);`
- **Call site:** `rt_mem_reg_reset()` in
  `bootstrap/runtime/rt_mem/rt_mem_alloc.c`.
- **Arguments:** `lpAddress = s_region_base`, `dwSize = 0`, `dwFreeType =
  MEM_RELEASE`.
- **Purpose:** release the reserved region so determinism tests can replay
  identical allocation sequences from a pristine state.
- **Scope note:** `rt_mem_reg_reset` is explicitly **TEST/DIAGNOSTIC ONLY** —
  not part of the runtime contract (`rt_mem_alloc.h`). The production allocator
  never releases the region during a process lifetime; freed blocks remain
  under the allocator's control until deterministic reuse or process exit
  (ADR-004).
- **Failure behavior:** ignored (void reset helper); the runtime state is
  reset regardless.
- **Baseline notes:** `MEM_RELEASE` with `dwSize == 0` releases the entire
  reservation; standard on 22H2.

### 4.5 `MultiByteToWideChar` — UTF-8 path → UTF-16

- **Declaration:** `int MultiByteToWideChar(UINT CodePage, DWORD dwFlags,
  LPCCH lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int
  cchWideChar);`
- **Call site:** `rt_io_open()` in `bootstrap/runtime/rt_io/rt_io_core.c`.
- **Arguments:** `CodePage = CP_UTF8`, `dwFlags = MB_ERR_INVALID_CHARS`,
  `cbMultiByte = (int)path_len`, `cchWideChar = (int)(path_len + 1)` (the
  buffer is a fixed stack array `wchar_t wide[RT_IO_PATH_MAX_BYTES + 1]`).
- **Purpose:** convert the UTF-8 `str` path to a UTF-16 path for
  `CreateFileW`. `MB_ERR_INVALID_CHARS` makes the conversion deterministic:
  invalid UTF-8 fails the conversion (returns 0) and `rt_io_open` returns 0.
  An embedded NUL converts to `L'\0'`, and `CreateFileW` truncates the path
  there — deterministic and documented (`rt_io_core.h`).
- **Failure behavior:** return 0 → open failure → `rt_io_open` returns 0
  (never a trap).
- **Baseline notes:** `CP_UTF8` + `MB_ERR_INVALID_CHARS` semantics are stable
  on 22H2.

### 4.6 `CreateFileW` — open/create a file

- **Declaration:** `HANDLE CreateFileW(LPCWSTR lpFileName, DWORD
  dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES
  lpSecurityAttributes, DWORD dwCreationDisposition, DWORD
  dwFlagsAndAttributes, HANDLE hTemplateFile);`
- **Call site:** `rt_io_open()` in `bootstrap/runtime/rt_io/rt_io_core.c`.
- **Arguments (per `rt.io.open` mode):**
  - mode 0 (read): `GENERIC_READ`, `OPEN_EXISTING`;
  - mode 1 (write truncate): `GENERIC_WRITE`, `CREATE_ALWAYS`;
  - mode 2 (write append): `GENERIC_WRITE`, `OPEN_ALWAYS` (then positioned at
    EOF, §4.7);
  - mode 3 (read/write create/truncate): `GENERIC_READ | GENERIC_WRITE`,
    `CREATE_ALWAYS`.
  - `dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`
    for every mode (least restrictive deterministic constant, so re-opening
    the same path with create/truncate while a previous handle is open does
    not fail with a sharing violation);
  - `lpSecurityAttributes = NULL`, `dwFlagsAndAttributes =
    FILE_ATTRIBUTE_NORMAL`, `hTemplateFile = NULL`.
- **Purpose:** the single file-open path of the runtime; any invalid mode
  value (not 0..3) fails before the OS call and returns 0.
- **Failure behavior:** `INVALID_HANDLE_VALUE` → `rt_io_open` returns 0 (never
  a trap; resource exhaustion and I/O errors are explicit result values per
  spec §15.2).
- **Baseline notes:** documented Win32 file API, unchanged on 22H2. No
  `FILE_FLAG_*` extended flags are used; no security attributes; no template.

### 4.7 `SetFilePointerEx` — append positioning

- **Declaration:** `BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER
  liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD
  dwMoveMethod);`
- **Call site:** `rt_io_open()` in `bootstrap/runtime/rt_io/rt_io_core.c`,
  only for mode 2 (append), immediately after `CreateFileW` succeeds.
- **Arguments:** `liDistanceToMove.QuadPart = 0`, `lpNewFilePointer = NULL`,
  `dwMoveMethod = FILE_END`.
- **Purpose:** position the file pointer at the end on open, so an append-mode
  handle writes at EOF without truncating existing content.
- **Failure behavior:** FALSE → the OS handle is closed (`CloseHandle`) and
  `rt_io_open` returns 0; no handle leaks (never a trap).
- **Baseline notes:** `FILE_END` move semantics stable on 22H2.

### 4.8 `ReadFile` — file read

- **Declaration:** `BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD
  nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED
  lpOverlapped);`
- **Call site:** `rt_io_read()` in `bootstrap/runtime/rt_io/rt_io_core.c`.
- **Arguments:** `nNumberOfBytesToRead` is the clamped `count`
  (`min(count, 0xFFFFFFFF)` so a slice longer than 4 GiB produces a short read
  deterministically rather than overflowing the DWORD); `lpOverlapped = NULL`
  (synchronous I/O).
- **Purpose:** read up to `count` bytes into the caller's buffer. `count >`
  `buf_len` traps `AIC-R0807` before any OS call (read buffer too small).
- **Failure behavior:** FALSE → `rt_io_read` returns 0 (I/O failure). A
  successful read returns the byte count actually read (0 at EOF). EOF and
  failure are both explicit `0` results per spec §15.2.
- **Baseline notes:** synchronous `ReadFile` semantics stable on 22H2; no
  overlapped I/O, no file pointers consumed by the runtime beyond the OS's
  per-handle pointer.

### 4.9 `WriteFile` — file write

- **Declaration:** `BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD
  nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED
  lpOverlapped);`
- **Call site:** `rt_io_write()` in `bootstrap/runtime/rt_io/rt_io_core.c`.
- **Arguments:** `nNumberOfBytesToWrite` is the clamped effective byte count
  (`min(count, buf_len)`, then clamped to `0xFFFFFFFF`); `lpOverlapped = NULL`.
- **Purpose:** write up to `count` bytes from the caller's buffer; the
  function never reads past the source slice (`buf_len`).
- **Failure behavior:** FALSE → `rt_io_write` returns 0; a successful write
  returns the bytes actually written.
- **Baseline notes:** synchronous `WriteFile` semantics stable on 22H2.

### 4.10 `CloseHandle` — close an OS handle

- **Declaration:** `BOOL CloseHandle(HANDLE hObject);`
- **Call sites:** `bootstrap/runtime/rt_io/rt_io_core.c`:
  - failure path in `rt_io_open` (append positioning failure, table full);
  - `rt_io_close()` for a valid open handle;
  - `rt_io_core_reset()` (test/diagnostic only) for every open handle.
- **Purpose:** close the OS handle so no handle leaks on failure or on
  `rt.io.close`. The runtime handle table slot is freed regardless (close is
  `void` per spec §15.2; `CloseHandle` failure is not observable to the
  caller).
- **Failure behavior:** the return value is ignored; the runtime's table state
  is deterministic and the index becomes invalid for later operations.
- **Baseline notes:** stable on 22H2.

### 4.11 `GetStdHandle` — standard stream handles

- **Declaration:** `HANDLE GetStdHandle(DWORD nStdHandle);`
- **Call sites:** `rt_io_stdio_register_stream()` in
  `bootstrap/runtime/rt_io/rt_io_stdio.c`, one per stream, on first use
  (lazy, idempotent, cached).
- **Arguments:** `STD_INPUT_HANDLE` (for `rt.io.stdin`), `STD_OUTPUT_HANDLE`
  (for `rt.io.stdout`), `STD_ERROR_HANDLE` (for `rt.io.stderr`).
- **Purpose:** obtain the process's standard stream handles and register them
  into the rt.io handle table (`rt_io_core_register_handle`), so
  `rt.io.stdin/stdout/stderr` return runtime-managed handles (spec §15.2,
  ADR-005).
- **Failure behavior:** a NULL or `INVALID_HANDLE_VALUE` result (e.g. a
  detached/windowed process) yields 0 from the corresponding `rt.io.*` stream
  function — the explicit "0 on failure" value, never a trap (ADR-005). A full
  handle table also yields 0 (resource exhaustion, never a trap).
- **Baseline notes:** the only standard-stream Windows call the runtime makes;
  the trap-record emitter writes via the CRT `stderr` stream instead (see
  §6).

### 4.12 `GetCommandLineW` — process command line

- **Declaration:** `LPWSTR GetCommandLineW(void);`
- **Call site:** `rt_proc_args()` in
  `bootstrap/runtime/rt_proc/rt_proc.c`, once per process (first call;
  result cached in process-lifetime static storage).
- **Purpose:** read the process's UTF-16 command line independently of the C
  runtime's `argv` construction; the runtime then splits it (documented
  Windows parsing rules) and converts each argument deterministically to
  UTF-8 with U+FFFD replacement for invalid surrogates (spec §15.3).
- **Failure behavior:** `NULL` result → the argument array is empty (count 0);
  `rt_proc_args` never fails and never traps. The parser is sized to the
  documented `CreateProcess` command-line limit (32,767 UTF-16 units;
  `RT_PROC_MAX_COMMAND_UNITS`), so `rt_proc_args` never overflows and never
  fails for any command line the OS can provide on the baseline.
- **Baseline notes:** `GetCommandLineW` is the documented way to obtain the
  wide command line on 22H2. `CreateProcess` is mentioned only as the
  documented source of the command-line length limit; the runtime itself
  never calls `CreateProcess`.

---

## 5. Kernel32 dependency statement

- Every enumerated call is exported by **`kernel32.dll`**; no other Windows
  DLL is imported by the runtime library (`rt_mem`, `rt_io`, `rt_proc`,
  `rt_trap`).
- The runtime performs **no** dynamic loading, no `LoadLibrary`/
  `GetProcAddress`, no delayed loading, and no direct `ntdll`/`api-ms-*`
  imports. The import surface is exactly the 11 distinct functions of §3
  across 15 call sites (`CloseHandle` 4, `VirtualAlloc` 2 — MEM_RESERVE and
  MEM_COMMIT — and the remaining 9 functions 1 each), resolved through
  `kernel32.lib` at link time by the accepted toolchains.
- This minimal, static, kernel32-only surface is the boundary that makes
  later supported-version testing bounded (ADR-003 §Decision 3; ADR-004): any
  new Windows call must be added to this document (and, if it changes the
  supported-version posture, to the Main Designer and Human Sponsor gates).
- **No `GetLastError` use:** the runtime never reads error codes; every
  failure is mapped to a documented return value (`0`, `NULL`, or
  `INVALID_HANDLE_VALUE` handled inside the caller). This keeps behavior
  deterministic and free of environment-dependent error strings.

---

## 6. C17 standard-library surface (not Windows API)

The runtime also calls the C17 standard library. These are **not** enumerated
as Windows calls; they are the CRT surface the runtime uses, listed here so the
boundary is explicit:

| Function | Used by | Purpose |
|----------|---------|---------|
| `fwrite` | trap emitters (`rt_io_core.c` AIC-R0807, `rt_io_stdio.c` AIC-R0814, `rt_mem_trap.c` AIC-R0812/R0813, `rt_trap.c` AIC-U0000) via `diag_buf_write_file` (`bootstrap/src/diag/diag_emit.c`) | write the JSONL trap record to `stderr` |
| `fflush` | same trap emitters | flush `stderr` before termination |
| `exit` | same trap emitters and `rt_proc_exit` | terminate the process with the given exit code (70 for traps, the given code for `rt.proc.exit`) |
| `snprintf` | trap record message construction (`rt_io_core.c`, `rt_io_stdio.c`, `rt_mem_trap.c`) | format deterministic message text |
| `memset` | `rt_mem_api.c` (`rt_mem_fill`), `rt_mem_alloc.c` (zeroing reused blocks), `rt_mem_reuse.c` (0xDD poisoning) | deterministic byte fill |
| `memmove` | `rt_mem_api.c` (`rt_mem_copy`, overlap-safe semantics) | deterministic byte copy |

The CRT maps `stderr` to the OS standard-error handle internally; the runtime
never calls `WriteFile`/`GetStdHandle` for trap emission. `exit()` runs CRT
atexit/stdio teardown, which the runtime relies on for flush-before-terminate
semantics; the observable trap contract is the JSONL record on `stderr` and the
process exit code 70 (DIAGNOSTIC-CONTRACT §10, spec §15.5).

---

## 7. Test-only Windows API usage (not runtime-facing)

The unit-test programs under `bootstrap/runtime/` are verification harnesses,
not runtime library code. Their Windows usage is enumerated here so an audit of
the tree is complete:

| Windows API | Used by (`*_test.c`) | Purpose |
|-------------|----------------------|---------|
| `CreatePipe` | `rt_io_core_test.c`, `rt_io_stdio_test.c`, `rt_mem_trap_test.c`, `rt_proc_test.c`, `rt_trap_test.c` | capture child-process stdout/stderr through anonymous pipes |
| `SetHandleInformation` | same test programs | make the correct pipe ends inheritable/non-inheritable for `CreateProcess` |
| `CreateProcessA` / `CreateProcessW` | same test programs (`CreateProcessW` in `rt_proc_test.c` for the wide command-line path) | spawn the test binary as a child process for live trap/exit checks |
| `WaitForSingleObject` | same test programs | wait for the child process to terminate |
| `GetExitCodeProcess` | same test programs | read the child's exit code |
| `GetStdHandle` | `rt_io_core_test.c`, `rt_proc_test.c` | pass the parent's standard handles to the child; emit test output |
| `ReadFile` / `WriteFile` | same test programs | read captured pipe output; write test output |
| `CloseHandle` | same test programs | close pipe/process handles |

These calls are excluded from the runtime-facing surface (§2.1) and are not
part of the runtime contract delivered to programs. They do not affect the
runtime's import surface (§5).

---

## 8. Re-verification procedure (enumeration drift control)

To regenerate the enumeration and detect drift, run from the repository root:

```
# All Windows API call sites in the runtime library (excluding tests).
# The `\s*\(` pattern matches actual call sites, not comment mentions:
grep -rnE "\b(GetSystemInfo|VirtualAlloc|VirtualFree|MultiByteToWideChar|CreateFileW|SetFilePointerEx|ReadFile|WriteFile|CloseHandle|GetStdHandle|GetCommandLineW)\s*\(" \
  bootstrap/runtime/ --include='*.c' | grep -v _test.c

# Any <windows.h> includes in the runtime library:
grep -rln "windows.h" bootstrap/runtime/ --include='*.c' | grep -v _test.c
# Expected: rt_mem/rt_mem_alloc.c, rt_io/rt_io_core.c, rt_io/rt_io_stdio.c,
# rt_proc/rt_proc.c

# Confirm no dynamic loading and no other Windows API family is present in the
# runtime library (call-site pattern only):
grep -rnE "\b(LoadLibrary|GetProcAddress|GetLastError|HeapAlloc|LocalAlloc|GlobalAlloc|CreateProcess|ExitProcess|WriteConsole|SetConsoleMode|RegOpenKey|OpenProcess|CreateThread)\s*\(" \
  bootstrap/runtime/ --include='*.c' | grep -v _test.c
# Expected: no output.
```

Expected result: the first grep's output is exactly the 11 distinct functions
of §3 across 15 call sites (`CloseHandle` 4, `VirtualAlloc` 2, the remaining
9 functions 1 each; comment mentions such as the `LocalAlloc`/`CreateProcess`
prose in `rt_proc.c` do not match the call-site pattern), and the third grep
produces no output. If a new runtime-facing Windows call is introduced, this
document must be updated in the same change (ADR-004 enumeration obligation).

---

## 9. Relationship to the work-package manifest

- Owned area: `bootstrap/runtime/README.md` (this file) and
  `bootstrap/build/rt_trap2.txt` (fragment, WP-M0-15c2 per manifest §2).
- The `rt_trap2.txt` fragment supersedes the first-level `rt_trap.txt` entry
  per manifest §5 (second-level sub-split convention, digit suffix): the
  runtime trap sources remain listed in `bootstrap/build/rt_trap1.txt`
  (WP-M0-15c1); WP-M0-15c2 owns documentation only and contributes no source
  paths to the aggregate build.
- Exclusions honored: `rt_trap` module implementation (WP-M0-15c1), allocator
  internals (WP-M0-14), `rt_io` (WP-M0-15a), `rt_proc` (WP-M0-15b) — this
  document only describes their Windows call surface; it modifies none of
  their artifacts (manifest rule 4: reads permitted, modifications not).

## 10. References

- `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (§Decision —
  Windows 10 22H2 baseline; §Consequences — enumeration obligation).
- `docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md` (superseded;
  historical record of the runtime-minimization direction).
- `spec/AI-CO-LANGUAGE-SPECIFICATION.md` §15 (runtime modules, trap codes,
  environmental inputs, calling convention, compiler-emitted calls).
- `spec/DIAGNOSTIC-CONTRACT.md` §10 (trap records, exit code 70).
- `research/ENVIRONMENT_BASELINE_2026-08-08.md` (observed Windows 10 Pro 22H2
  build 10.0.19045.6466, end-of-support evidence).
- `bootstrap/build/CONVENTIONS.md` (fragment convention, toolchain policy).
- Windows API documentation (Microsoft Learn, Win32 API; functions enumerated
  in §3/§4).
