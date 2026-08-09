"""Conformance suite runner.

Compiles valid AI-Co programs, executes them, and compares stdout bytes
and exit code against expected records (§1a conformance schema).
"""

from __future__ import annotations

import json
import os
from typing import Any

from .adapter import CompilerAdapter
from .case_schema import Case, RunExpected, load_case, load_manifest
from .report import CaseResult, SuiteResult


def run_conformance(
    corpus_dir: str,
    adapter: CompilerAdapter,
    artifacts_dir: str,
    *,
    repo_root: str = "",
) -> SuiteResult:
    """Run all active conformance cases and return a suite result.

    Args:
        corpus_dir: path to tests/conformance/
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
                corpus="conformance",
                passed=False,
                reason=f"schema error: {e}",
            ))
            failed += 1
            continue

        if case.deferred:
            results.append(CaseResult(
                case=case_name,
                corpus="conformance",
                passed=True,
                deferred=True,
                reason=f"deferred: {case.meta.deferral_reason}",
            ))
            deferred += 1
            continue

        assert isinstance(case.expected, RunExpected)

        # Compile and run
        output_dir = os.path.join(artifacts_dir, "conformance", case_name)
        try:
            result = adapter.compile_and_run(case_dir, output_dir)
        except Exception as e:
            results.append(CaseResult(
                case=case_name,
                corpus="conformance",
                passed=False,
                reason=f"compiler error: {e}",
            ))
            failed += 1
            continue

        # Compare exit code
        if result.exit_code != case.expected.exit_code:
            results.append(CaseResult(
                case=case_name,
                corpus="conformance",
                passed=False,
                reason=(
                    f"exit code mismatch: expected {case.expected.exit_code}, "
                    f"got {result.exit_code}"
                ),
                details={
                    "expected_exit_code": case.expected.exit_code,
                    "actual_exit_code": result.exit_code,
                },
            ))
            failed += 1
            continue

        # Compare stdout (byte-exact)
        expected_stdout = case.expected.stdout.encode("utf-8")
        if result.stdout != expected_stdout:
            results.append(CaseResult(
                case=case_name,
                corpus="conformance",
                passed=False,
                reason="stdout mismatch",
                details={
                    "expected_stdout_hex": expected_stdout.hex(),
                    "actual_stdout_hex": result.stdout.hex(),
                    "expected_stdout_len": len(expected_stdout),
                    "actual_stdout_len": len(result.stdout),
                },
            ))
            failed += 1
            continue

        # All checks passed
        results.append(CaseResult(
            case=case_name,
            corpus="conformance",
            passed=True,
        ))
        passed += 1

    return SuiteResult(
        corpus="conformance",
        total=total,
        passed=passed,
        failed=failed,
        deferred=deferred,
        cases=results,
    )
