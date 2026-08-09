@echo off
rem bootstrap/build/init-clang.cmd
rem ---------------------------------------------------------------------------
rem LLVM/Clang path initializer for the AI-Co build entry points.
rem Owned by WP-M0-01. Consume with `call "%~dp0init-clang.cmd"` from a batch
rem file; there is intentionally NO setlocal here so the pinned variables
rem persist into the caller.
rem
rem Sets (full paths, per ENVIRONMENT_BASELINE_2026-08-08.md LLVM is OFF PATH):
rem   CLANG_DIR    - LLVM installation directory
rem   CLANG_CL     - clang-cl.exe (MSVC-compatible driver used by the Clang entry point)
rem   CLANG_DRIVER - clang.exe (GNU-style driver, available for comparison/oracle use)
rem   LLD_LINK     - lld-link.exe (LLD PE linker)
rem   LLVM_AR      - llvm-ar.exe (archiver)
rem
rem The Clang entry point still initializes the MSVC x64 environment first
rem (vcvarsall.bat x64) so clang-cl finds the Windows SDK headers/libs; only
rem the compiler binaries themselves come from the pinned LLVM install.
rem Fail loudly when the pinned LLVM directory is missing.
rem ---------------------------------------------------------------------------

set "CLANG_DIR=C:\Program Files\LLVM"
if not exist "%CLANG_DIR%\bin\clang-cl.exe" (
    echo [init-clang] ERROR: LLVM not found at "%CLANG_DIR%" ^(baseline LLVM 22.1.8, off PATH^). 1>&2
    exit /b 1
)
set "CLANG_CL=%CLANG_DIR%\bin\clang-cl.exe"
set "CLANG_DRIVER=%CLANG_DIR%\bin\clang.exe"
set "LLD_LINK=%CLANG_DIR%\bin\lld-link.exe"
set "LLVM_AR=%CLANG_DIR%\bin\llvm-ar.exe"
echo [init-clang] LLVM pinned: "%CLANG_DIR%" ^(clang-cl %CLANG_CL%^)
exit /b 0
