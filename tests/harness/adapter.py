"""Compiler adapter interface and implementations.

The adapter abstracts compiler invocation so the harness can be tested
with fixture output before Stage 0 exists.  Two implementations:

- StubCompilerAdapter: deterministic stub for fixture tests
- ScriptCompilerAdapter: runs a compiler.py script per case (for integration)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any


@dataclass
class CompilerResult:
    """Result of a single compiler invocation (compile or run)."""

    stdout: bytes
    stderr: bytes
    exit_code: int
    artifacts_dir: str | None = None  # path to produced COFF/manifest files


class CompilerAdapter(ABC):
    """Abstract base for compiler invocation."""

    @abstractmethod
    def compile_only(self, case_dir: str, output_dir: str) -> CompilerResult:
        """Compile input.ai; return diagnostics on stderr, artifacts in output_dir."""

    @abstractmethod
    def compile_and_run(self, case_dir: str, output_dir: str) -> CompilerResult:
        """Compile and execute; return stdout/stderr/exit_code."""

    @abstractmethod
    def compile_for_determinism(
        self, case_dir: str, output_dir: str
    ) -> CompilerResult:
        """Compile for determinism check; identical to compile_only but
        records the invocation for byte comparison."""


class StubCompilerAdapter(CompilerAdapter):
    """Deterministic stub that produces fixture output from compiler_output.json.

    Each case directory may contain a compiler_output.json with:
      - stdout: str (base64-encoded or plain text)
      - stderr: str (base64-encoded or plain text)
      - exit_code: int
      - artifacts: dict[str, str] (filename -> hex-encoded bytes)

    If compiler_output.json is absent, the adapter produces deterministic
    output derived from the case's expected.json (for conformance: expected
    stdout; for negative: expected diagnostic records on stderr).
    """

    def compile_only(self, case_dir: str, output_dir: str) -> CompilerResult:
        return self._invoke(case_dir, output_dir, mode="compile")

    def compile_and_run(self, case_dir: str, output_dir: str) -> CompilerResult:
        return self._invoke(case_dir, output_dir, mode="run")

    def compile_for_determinism(
        self, case_dir: str, output_dir: str
    ) -> CompilerResult:
        return self._invoke(case_dir, output_dir, mode="compile")

    def _invoke(
        self, case_dir: str, output_dir: str, *, mode: str
    ) -> CompilerResult:
        os.makedirs(output_dir, exist_ok=True)

        # Check for explicit compiler_output.json fixture
        fixture_path = os.path.join(case_dir, "compiler_output.json")
        if os.path.isfile(fixture_path):
            return self._load_fixture(fixture_path, output_dir)

        # Synthesize output from expected.json
        expected_path = os.path.join(case_dir, "expected.json")
        if not os.path.isfile(expected_path):
            return CompilerResult(
                stdout=b"", stderr=b"error: no expected.json\n", exit_code=1
            )

        with open(expected_path, "r", encoding="utf-8") as f:
            expected = json.load(f)

        kind = expected.get("kind", "run")

        if kind == "diagnostics":
            # Negative case: emit diagnostic records on stderr
            records = expected.get("records", [])
            stderr_lines = []
            for rec in records:
                stderr_lines.append(json.dumps(rec, separators=(",", ":")))
            stderr = "\n".join(stderr_lines)
            if stderr:
                stderr += "\n"
            return CompilerResult(
                stdout=b"",
                stderr=stderr.encode("utf-8"),
                exit_code=1 if records else 0,
                artifacts_dir=output_dir,
            )

        elif kind == "run":
            if mode == "compile":
                # Compile-only mode: no output, no execution
                return CompilerResult(
                    stdout=b"",
                    stderr=b"",
                    exit_code=0,
                    artifacts_dir=output_dir,
                )
            else:
                # Run mode: produce expected stdout and exit code
                stdout_str = expected.get("stdout", "")
                exit_code = expected.get("exit_code", 0)
                stderr = b""

                # For smoke trap cases, produce expected stderr
                if expected.get("trap"):
                    stderr_contains = expected.get("stderr_contains", [])
                    if stderr_contains:
                        # Emit the first substring as a JSONL line (simulating trap record)
                        stderr = (stderr_contains[0] + "\n").encode("utf-8")

                return CompilerResult(
                    stdout=stdout_str.encode("utf-8"),
                    stderr=stderr,
                    exit_code=exit_code,
                    artifacts_dir=output_dir,
                )

        return CompilerResult(
            stdout=b"", stderr=b"error: unknown expected kind\n", exit_code=1
        )

    def _load_fixture(self, fixture_path: str, output_dir: str) -> CompilerResult:
        """Load an explicit compiler_output.json fixture."""
        with open(fixture_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        stdout_str = data.get("stdout", "")
        stderr_str = data.get("stderr", "")
        exit_code = data.get("exit_code", 0)

        # Write any artifact hex blobs to output_dir
        artifacts = data.get("artifacts", {})
        for filename, hex_bytes in artifacts.items():
            artifact_path = os.path.join(output_dir, filename)
            os.makedirs(os.path.dirname(artifact_path), exist_ok=True)
            with open(artifact_path, "wb") as af:
                af.write(bytes.fromhex(hex_bytes))

        return CompilerResult(
            stdout=stdout_str.encode("utf-8") if isinstance(stdout_str, str) else stdout_str,
            stderr=stderr_str.encode("utf-8") if isinstance(stderr_str, str) else stderr_str,
            exit_code=exit_code,
            artifacts_dir=output_dir,
        )


class ScriptCompilerAdapter(CompilerAdapter):
    """Runs a compiler.py script per case directory.

    The script must accept arguments:
      python compiler.py <case_dir> <output_dir> <mode>

    Where mode is "compile" or "run", and write results to stdout/stderr.
    The script's exit code is the compiler's exit code.

    For fixture testing, scripts are provided per case.
    """

    def compile_only(self, case_dir: str, output_dir: str) -> CompilerResult:
        return self._run_script(case_dir, output_dir, "compile")

    def compile_and_run(self, case_dir: str, output_dir: str) -> CompilerResult:
        return self._run_script(case_dir, output_dir, "run")

    def compile_for_determinism(
        self, case_dir: str, output_dir: str
    ) -> CompilerResult:
        return self._run_script(case_dir, output_dir, "compile")

    def _run_script(
        self, case_dir: str, output_dir: str, mode: str
    ) -> CompilerResult:
        os.makedirs(output_dir, exist_ok=True)

        script_path = os.path.join(case_dir, "compiler.py")
        if not os.path.isfile(script_path):
            return CompilerResult(
                stdout=b"",
                stderr=f"error: no compiler.py in {case_dir}\n".encode("utf-8"),
                exit_code=1,
            )

        try:
            proc = subprocess.run(
                [sys.executable, script_path, case_dir, output_dir, mode],
                capture_output=True,
                timeout=30,
                cwd=case_dir,
            )
            return CompilerResult(
                stdout=proc.stdout,
                stderr=proc.stderr,
                exit_code=proc.returncode,
                artifacts_dir=output_dir,
            )
        except subprocess.TimeoutExpired:
            return CompilerResult(
                stdout=b"",
                stderr=b"error: compiler script timed out\n",
                exit_code=1,
            )
        except Exception as e:
            return CompilerResult(
                stdout=b"",
                stderr=f"error: {e}\n".encode("utf-8"),
                exit_code=1,
            )
