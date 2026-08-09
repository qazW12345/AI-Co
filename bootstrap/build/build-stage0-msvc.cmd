@echo off
setlocal EnableDelayedExpansion
rem bootstrap/build/build-stage0-msvc.cmd
rem ---------------------------------------------------------------------------
rem Top-level Stage-0 build entry point: MSVC host compiler.
rem Owned by WP-M0-01. Binding conventions: bootstrap/build/CONVENTIONS.md.
rem
rem Usage (invoke from Git Bash, cmd, or PowerShell; cwd-independent):
rem   build-stage0-msvc.cmd [source ...]
rem     source   source files relative to the repository root. When any are
rem              given they are compiled instead of the per-area fragment
rem              aggregation. The output executable is named after the first
rem              source (e.g. verify-hello.c -> verify-hello.exe).
rem
rem Output directory: %STAGE0_OUT_DIR% (optional, relative to repository root;
rem default: bootstrap\stage0\msvc).
rem
rem Default source selection: aggregate of the per-area build fragments
rem bootstrap\build\*.txt (see CONVENTIONS.md). With no fragments and no
rem explicit sources the script fails with a clear message.
rem
rem Deterministic flags (also recorded in CONVENTIONS.md):
rem   cl /nologo /std:c17 /O2 /W4 <sources> /Fo:<out>\obj\ /Fe:<out>\<exe>.exe /link /Brepro
rem The /Brepro PE-link flag makes the executable timestamp deterministic
rem (verified byte-identical exes across repeated builds).
rem
rem Linker policy: cl.exe resolves and invokes the MSVC linker (link.exe)
rem internally inside the vcvars-initialized developer environment. This script
rem never invokes bare `link` from Git Bash (coreutils link.exe collision).
rem ---------------------------------------------------------------------------

rem --- repository root (canonical) ------------------------------------------
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
cd /d "%ROOT%"
if errorlevel 1 (
    echo [build-stage0-msvc] ERROR: cannot cd to repository root "%ROOT%". 1>&2
    exit /b 1
)

rem --- toolchain init -------------------------------------------------------
call "%~dp0init-msvc.cmd"
if errorlevel 1 exit /b 1

rem --- arguments ------------------------------------------------------------
set "OUT_DIR=%STAGE0_OUT_DIR%"
if "!OUT_DIR!"=="" set "OUT_DIR=bootstrap\stage0\msvc"

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
    echo [build-stage0-msvc] ERROR: no sources. Pass source files as arguments or add bootstrap\build\*.txt fragments. 1>&2
    exit /b 1
)

rem --- output naming --------------------------------------------------------
for %%f in ("!FIRST_SRC!") do set "EXE_NAME=%%~nf"
if not exist "!OUT_DIR!\obj" mkdir "!OUT_DIR!\obj"

echo [build-stage0-msvc] root: !ROOT!
echo [build-stage0-msvc] out:  !OUT_DIR!\!EXE_NAME!.exe
echo [build-stage0-msvc] compile: cl /nologo /std:c17 /O2 /W4 !SRC_LIST! /Fo:!OUT_DIR!\obj\ /Fe:!OUT_DIR!\!EXE_NAME!.exe /link /Brepro

cl /nologo /std:c17 /O2 /W4 !SRC_LIST! /Fo:!OUT_DIR!\obj\ /Fe:!OUT_DIR!\!EXE_NAME!.exe /link /Brepro
if errorlevel 1 (
    echo [build-stage0-msvc] ERROR: cl failed. 1>&2
    exit /b 1
)
echo [build-stage0-msvc] OK: !OUT_DIR!\!EXE_NAME!.exe
exit /b 0
