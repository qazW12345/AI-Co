"""Executable smoke suite runner.

Deterministic execution of representative and trap programs.
Trap programs exit with code 70 and emit a trap record on stderr.
"""

from __future__ import annotations

import json
import os
from typing import Any

from .adapter import CompilerAdapter
from .case_schema import Case, RunExpected, load_case, load_manifest
from .report import CaseResult, SuiteResult


def _check_stderr_contains(
    stderr: bytes, expected_substrings: list[str]
) -> tuple[bool, str]:
    """Check that stderr contains all expected substrings.

    Returns (passed, reason_if_failed).
    """
    stderr_text = stderr.decode("utf-8", errors="replace")
    for substr in expected_substrings:
        if substr not in stderr_text:
            return False, (
                f"stderr does not contain expected substring '{substr}'"
            )
    return True, ""


def run_smoke(
    corpus_dir: str,
    adapter: CompilerAdapter,
    artifacts_dir: str,
    *,
    repo_root: str = "",
) -> SuiteResult:
    """Run all active smoke cases and return a suite result.

    Args:
        corpus_dir: path to tests/smoke/
        adapter: compiler adapter instance
        artifacts_dir: path for temporary compiler output
        repo_root: repository root for relative path display
    """
    manifest_path = os.path.join(corpus_dir, "manifest.json")
    manifest = load_manifest(manifest_path)
    cases_dir = os.path.join(corpus_dir, "cases")

    results: list[CaseResult] = []
    total = 0
    passed = 0
    failed = 0
    deferred = 0

    for case_name in sorted(manifest["cases"]):
        case_dir = os.path.join(cases_dir, case_name)
        total += 1

        try:
            case = load_case(case_dir)
        except Exception as e:
            results.append(CaseResult(
                case=case_name,
                corpus="smoke",
                passed=False,
                reason=f"schema error: {e}",
            ))
            failed += 1
            continue

        if case.deferred:
            results.append(CaseResult(
                case=case_name,
                corpus="smoke",
                passed=True,
                deferred=True,
                reason=f"deferred: {case.meta.deferral_reason}",
            ))
            deferred += 1
            continue

        assert isinstance(case.expected, RunExpected)

        # Compile and run
        output_dir = os.path.join(artifacts_dir, "smoke", case_name)
        try:
            compiler_result = adapter.compile_and_run(case_dir, output_dir)
        except Exception as e:
            results.append(CaseResult(
                case=case_name,
                corpus="smoke",
                passed=False,
                reason=f"compiler error: {e}",
            ))
            failed += 1
            continue

        # Check exit code
        if compiler_result.exit_code != case.expected.exit_code:
            results.append(CaseResult(
                case=case_name,
                corpus="smoke",
                passed=False,
                reason=(
                    f"exit code mismatch: expected {case.expected.exit_code}, "
                    f"got {compiler_result.exit_code}"
                ),
                details={
                    "expected_exit_code": case.expected.exit_code,
                    "actual_exit_code": compiler_result.exit_code,
                },
            ))
            failed += 1
            continue

        # Check trap flag consistency
        if case.expected.trap and compiler_result.exit_code != 70:
            results.append(CaseResult(
                case=case_name,
                corpus="smoke",
                passed=False,
                reason=(
                    f"expected trap (exit code 70), got exit code {compiler_result.exit_code}"
                ),
            ))
            failed += 1
            continue

        # Check stdout (byte-exact)
        expected_stdout = case.expected.stdout.encode("utf-8")
        if compiler_result.stdout != expected_stdout:
            results.append(CaseResult(
                case=case_name,
                corpus="smoke",
                passed=False,
                reason="stdout mismatch",
                details={
                    "expected_stdout_hex": expected_stdout.hex(),
                    "actual_stdout_hex": compiler_result.stdout.hex(),
                },
            ))
            failed += 1
            continue

        # Check stderr_contains substrings
        if case.expected.stderr_contains:
            ok, reason = _check_stderr_contains(
                compiler_result.stderr, case.expected.stderr_contains
            )
            if not ok:
                results.append(CaseResult(
                    case=case_name,
                    corpus="smoke",
                    passed=False,
                    reason=reason,
                    details={
                        "expected_substrings": case.expected.stderr_contains,
                        "actual_stderr_hex": compiler_result.stderr.hex(),
                    },
                ))
                failed += 1
                continue

        # All checks passed
        results.append(CaseResult(
            case=case_name,
            corpus="smoke",
            passed=True,
        ))
        passed += 1

    return SuiteResult(
        corpus="smoke",
        total=total,
        passed=passed,
        failed=failed,
        deferred=deferred,
        cases=results,
    )
