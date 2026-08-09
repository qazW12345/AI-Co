@echo off
rem bootstrap/build/init-msvc.cmd
rem ---------------------------------------------------------------------------
rem MSVC x64 developer-environment initializer for the AI-Co build entry points.
rem Owned by WP-M0-01. Consume with `call "%~dp0init-msvc.cmd"` from a batch
rem file; there is intentionally NO setlocal here so the initialized
rem environment (PATH, INCLUDE, LIB, LIBPATH, ...) persists into the caller.
rem
rem Deterministic policy (ENVIRONMENT_BASELINE_2026-08-08.md):
rem   - pinned toolset: Visual Studio Build Tools 2026, cl 19.50.35717, via
rem     vcvarsall.bat x64;
rem   - fallback: vswhere -latest instance with C++ tools;
rem   - fail loudly (exit /b 1) when no vcvarsall.bat is found.
rem
rem Never invoke bare `link` from Git Bash after this initializer; the MSVC
rem linker is only reachable inside this initialized developer environment or
rem by explicit full path (see CONVENTIONS.md).
rem ---------------------------------------------------------------------------

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSBT2026=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

rem 1) Pinned path: VS Build Tools 2026 (baseline cl 19.50.35717).
if exist "%VSBT2026%" set "VCVARS=%VSBT2026%"

rem 2) Fallback: newest VS instance with the x64 C++ tools workload, via vswhere.
if not defined VCVARS if exist "%VSWHERE%" (
    for /f "usebackq tokens=* delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

if not defined VCVARS (
    echo [init-msvc] ERROR: vcvarsall.bat not found. Install Visual Studio Build Tools with the C++ workload. 1>&2
    exit /b 1
)

call "%VCVARS%" x64 >nul
if errorlevel 1 (
    echo [init-msvc] ERROR: vcvarsall.bat x64 failed for "%VCVARS%". 1>&2
    exit /b 1
)
echo [init-msvc] MSVC x64 developer environment initialized: "%VCVARS%"
exit /b 0
