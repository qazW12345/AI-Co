@echo off
setlocal EnableDelayedExpansion
rem bootstrap/build/build-stage0-clang.cmd
rem ---------------------------------------------------------------------------
rem Top-level Stage-0 build entry point: LLVM Clang host compiler.
rem Owned by WP-M0-01. Binding conventions: bootstrap/build/CONVENTIONS.md.
rem
rem Usage (invoke from Git Bash, cmd, or PowerShell; cwd-independent):
rem   build-stage0-clang.cmd [source ...]
rem     source   source files relative to the repository root. When any are
rem              given they are compiled instead of the per-area fragment
rem              aggregation. The output executable is named after the first
rem              source (e.g. verify-hello.c -> verify-hello.exe).
rem
rem Output directory: %STAGE0_OUT_DIR% (optional, relative to repository root;
rem default: bootstrap\stage0\clang).
rem
rem Default source selection: aggregate of the per-area build fragments
rem bootstrap\build\*.txt (see CONVENTIONS.md). With no fragments and no
rem explicit sources the script fails with a clear message.
rem
rem Deterministic flags (also recorded in CONVENTIONS.md):
rem   clang-cl /nologo /std:c17 /O2 /W4 /Brepro <sources> /Fo:<out>\obj\ /Fe:<out>\<exe>.exe
rem /Brepro makes the PE timestamp deterministic (verified byte-identical exes
rem across repeated builds). clang-cl resolves and invokes lld-link from its
rem own LLVM bin directory (explicit full path, never bare `link`).
rem
rem Environment: LLVM 22.1.8 is OFF PATH per the environment baseline, so
rem clang-cl is called by the full path pinned in init-clang.cmd. The MSVC x64
rem developer environment is initialized first (vcvarsall.bat x64) so clang-cl
rem finds the Windows SDK headers and libraries.
rem ---------------------------------------------------------------------------

rem --- repository root (canonical) ------------------------------------------
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
cd /d "%ROOT%"
if errorlevel 1 (
    echo [build-stage0-clang] ERROR: cannot cd to repository root "%ROOT%". 1>&2
    exit /b 1
)

rem --- toolchain init -------------------------------------------------------
call "%~dp0init-msvc.cmd"
if errorlevel 1 exit /b 1
call "%~dp0init-clang.cmd"
if errorlevel 1 exit /b 1

rem --- arguments ------------------------------------------------------------
set "OUT_DIR=%STAGE0_OUT_DIR%"
if "!OUT_DIR!"=="" set "OUT_DIR=bootstrap\stage0\clang"

set "SRC_LIST="
set "FIRST_SRC="
:src_loop
if "%~1"=="" goto :src_done
set "SRC_LIST=!SRC_LIST! "%~1""
if "!FIRST_SRC!"=="" set "FIRST_SRC=%~1"
shift
goto :src_loop
:src_done

rem --- fragment aggregation when no explicit sources were given --------------
if "!SRC_LIST!"=="" (
    for /f "usebackq delims=" %%f in (`dir /b /on "bootstrap\build\*.txt" 2^>nul`) do (
        for /f "usebackq eol=# tokens=* delims=" %%s in ("bootstrap\build\%%f") do (
            if not "%%s"=="" (
                set "SRC_LIST=!SRC_LIST! "%%s""
                if "!FIRST_SRC!"=="" set "FIRST_SRC=%%s"
            )
        )
    )
)

if "!SRC_LIST!"=="" (
    echo [build-stage0-clang] ERROR: no sources. Pass source files as arguments or add bootstrap\build\*.txt fragments. 1>&2
    exit /b 1
)

rem --- output naming --------------------------------------------------------
for %%f in ("!FIRST_SRC!") do set "EXE_NAME=%%~nf"
if not exist "!OUT_DIR!\obj" mkdir "!OUT_DIR!\obj"

echo [build-stage0-clang] root: !ROOT!
echo [build-stage0-clang] out:  !OUT_DIR!\!EXE_NAME!.exe
echo [build-stage0-clang] compile: "%CLANG_CL%" /nologo /std:c17 /O2 /W4 /Brepro !SRC_LIST! /Fo:!OUT_DIR!\obj\ /Fe:!OUT_DIR!\!EXE_NAME!.exe

"%CLANG_CL%" /nologo /std:c17 /O2 /W4 /Brepro !SRC_LIST! /Fo:!OUT_DIR!\obj\ /Fe:!OUT_DIR!\!EXE_NAME!.exe
if errorlevel 1 (
    echo [build-stage0-clang] ERROR: clang-cl failed. 1>&2
    exit /b 1
)
echo [build-stage0-clang] OK: !OUT_DIR!\!EXE_NAME!.exe
exit /b 0
