#!/usr/bin/env bash
# Probe 05 - Toolchain/runtime scan from Git Bash (read-only)
# Sneedworks Researcher - AI-Co environment baseline, 2026-08-08
echo "=== PROBE 05: Toolchain/runtime scan (Git Bash) ==="
echo "Run at: $(date -Iseconds)"

check() {
  local name="$1"; shift
  local cmd
  cmd=$(command -v "$name" 2>/dev/null)
  if [ -n "$cmd" ]; then
    local ver
    ver=$("$@" 2>/dev/null | head -n 1 | tr -d '\r')
    echo "FOUND  : $name -> $cmd"
    if [ -n "$ver" ]; then echo "         version: $ver"; fi
  else
    echo "absent : $name"
  fi
}

echo ""
echo "--- C/C++ compilers, assemblers, linkers ---"
for n in gcc g++ clang clang++ clang-cl cl tcc zig cc; do check "$n" "$n" --version; done
for n in ld lld ld.lld lld-link link lib ar ranlib as nasm yasm windres objdump nm strip dumpbin ml ml64; do check "$n" "$n" --version; done

echo ""
echo "--- LLVM tooling ---"
for n in llvm-config llc opt llvm-as llvm-dis llvm-nm llvm-objdump llvm-size llvm-ar llvm-ranlib llvm-strip llvm-link clang-tidy clang-format; do check "$n" "$n" --version; done

echo ""
echo "--- Language runtimes / package tools ---"
for n in python python3 pip pip3 uv rustc cargo go dotnet java javac node npm npx yarn pnpm bun deno ruby perl php swift lua luajit R Rscript; do check "$n" "$n" --version; done

echo ""
echo "--- Build systems / utilities ---"
for n in make cmake ninja meson autoconf automake libtool pkg-config bison flex m4 gdb lldb gcov valgrind strace; do check "$n" "$n" --version; done

echo ""
echo "--- Version control / helpers ---"
for n in git gh svn hg; do check "$n" "$n" --version; done

echo ""
echo "--- WSL / terminal helpers ---"
for n in wsl wslconfig tmux screen zsh fish; do check "$n" "$n" --version; done

echo ""
echo "--- MSYS2 environment check ---"
for d in /c/msys64 /c/ProgramData/chocolatey /c/tools /c/Program\ Files/LLVM /c/Program\ Files/Microsoft\ Visual\ Studio; do
  if [ -e "$d" ]; then echo "dir-exists: $d"; else echo "no-dir   : $d"; fi
done

echo ""
echo "--- PATH summary ---"
echo "PATH entries: $(echo "$PATH" | tr ':' '\n' | wc -l)"
