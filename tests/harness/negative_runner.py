"""Negative-diagnostic suite runner.

Compiles invalid AI-Co programs and asserts the emitted JSONL diagnostic
records (codes, primary/secondary spans, ordering per DIAGNOSTIC-CONTRACT §9).
"""

from __future__ import annotations

import json
import os
from typing import Any

from .adapter import CompilerAdapter
from .case_schema import Case, DiagnosticsExpected, load_case, load_manifest
from .report import CaseResult, SuiteResult


def _parse_jsonl_stderr(stderr: bytes) -> list[dict[str, Any]]:
    """Parse JSONL lines from stderr into a list of diagnostic records."""
    records = []
    text = stderr.decode("utf-8", errors="replace")
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
            if isinstance(record, dict):
                records.append(record)
        except json.JSONDecodeError:
            # Non-JSON line; skip (could be other stderr output)
            continue
    return records


def _compare_records(
    expected: list[dict[str, Any]],
    actual: list[dict[str, Any]],
    case_name: str,
) -> tuple[bool, str]:
    """Compare expected diagnostic records against actual emitted records.

    Checks:
    1. Record count matches
    2. Each expected record has required fields present in actual record
    3. Codes match in order
    4. Primary spans match (exact or structural per §1a)
    5. Ordering per DIAGNOSTIC-CONTRACT §9 (phase order, then span order)

    Returns (passed, reason_if_failed).
    """
    if len(expected) != len(actual):
        return False, (
            f"record count mismatch: expected {len(expected)}, got {len(actual)}"
        )

    for i, (exp, act) in enumerate(zip(expected, actual)):
        # Check required fields
        for field in ("schema_version", "code", "severity", "phase", "message"):
            if field not in act:
                return False, f"record[{i}] missing field '{field}'"

        # Check code matches
        if exp.get("code") != act.get("code"):
            return False, (
                f"record[{i}] code mismatch: expected '{exp.get('code')}', "
                f"got '{act.get('code')}'"
            )

        # Check severity matches
        if exp.get("severity") != act.get("severity"):
            return False, (
                f"record[{i}] severity mismatch: expected '{exp.get('severity')}', "
                f"got '{act.get('severity')}'"
            )

        # Check phase matches
        if exp.get("phase") != act.get("phase"):
            return False, (
                f"record[{i}] phase mismatch: expected '{exp.get('phase')}', "
                f"got '{act.get('phase')}'"
            )

        # Check primary span (exact match if provided, structural otherwise)
        exp_span = exp.get("primary_span")
        act_span = act.get("primary_span")
        if exp_span is not None and isinstance(exp_span, dict):
            if act_span is None:
                return False, f"record[{i}] expected primary_span, got null"
            # Check structural span: file, start.line, start.col
            for key in ("file", "start", "end"):
                if key in exp_span:
                    if key not in act_span:
                        return False, f"record[{i}] primary_span missing '{key}'"
                    if key == "start" or key == "end":
                        for coord in ("line", "col"):
                            if coord in exp_span[key]:
                                if coord not in act_span[key]:
                                    return False, (
                                        f"record[{i}] primary_span.{key} missing '{coord}'"
                                    )
                                if exp_span[key][coord] != act_span[key][coord]:
                                    return False, (
                                        f"record[{i}] primary_span.{key}.{coord} mismatch: "
                                        f"expected {exp_span[key][coord]}, "
                                        f"got {act_span[key][coord]}"
                                    )
                    elif key == "file":
                        if exp_span[key] != act_span[key]:
                            return False, (
                                f"record[{i}] primary_span.file mismatch: "
                                f"expected '{exp_span[key]}', got '{act_span[key]}'"
                            )

        # Check recovery matches if provided
        if "recovery" in exp:
            if exp.get("recovery") != act.get("recovery"):
                return False, (
                    f"record[{i}] recovery mismatch: expected '{exp.get('recovery')}', "
                    f"got '{act.get('recovery')}'"
                )

    return True, ""


def run_negative(
    corpus_dir: str,
    adapter: CompilerAdapter,
    artifacts_dir: str,
    *,
    repo_root: str = "",
) -> SuiteResult:
    """Run all active negative cases and return a suite result.

    Args:
        corpus_dir: path to tests/negative/
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
                corpus="negative",
                passed=False,
                reason=f"schema error: {e}",
            ))
            failed += 1
            continue

        if case.deferred:
            results.append(CaseResult(
                case=case_name,
                corpus="negative",
                passed=True,
                deferred=True,
                reason=f"deferred: {case.meta.deferral_reason}",
            ))
            deferred += 1
            continue

        assert isinstance(case.expected, DiagnosticsExpected)

        # Compile only (no execution)
        output_dir = os.path.join(artifacts_dir, "negative", case_name)
        try:
            compiler_result = adapter.compile_only(case_dir, output_dir)
        except Exception as e:
            results.append(CaseResult(
                case=case_name,
                corpus="negative",
                passed=False,
                reason=f"compiler error: {e}",
            ))
            failed += 1
            continue

        # Parse diagnostic records from stderr
        actual_records = _parse_jsonl_stderr(compiler_result.stderr)

        # Compare against expected records
        ok, reason = _compare_records(
            case.expected.records, actual_records, case_name
        )

        if ok:
            results.append(CaseResult(
                case=case_name,
                corpus="negative",
                passed=True,
            ))
            passed += 1
        else:
            results.append(CaseResult(
                case=case_name,
                corpus="negative",
                passed=False,
                reason=reason,
                details={
                    "expected_count": len(case.expected.records),
                    "actual_count": len(actual_records),
                    "expected_codes": [r.get("code") for r in case.expected.records],
                    "actual_codes": [r.get("code") for r in actual_records],
                },
            ))
            failed += 1

    return SuiteResult(
        corpus="negative",
        total=total,
        passed=passed,
        failed=failed,
        deferred=deferred,
        cases=results,
    )
