"""AI-Co verification harness — main entry point and CLI.

Usage:
    python -m tests.harness [OPTIONS] [COMMAND]

Commands:
    conformance   Run conformance suite
    negative      Run negative-diagnostic suite
    smoke         Run executable smoke suite
    all           Run all three suites
    determinism   Run determinism check
    identity      Run identity check (M1-ready)
    manifest      Validate build manifest

Options:
    --compiler PATH    Path to compiler executable (default: stub for fixtures)
    --repo-root PATH   Repository root for relative paths
    --artifacts DIR    Output directory for reports (default: tests/artifacts)
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any

from .adapter import StubCompilerAdapter, ScriptCompilerAdapter
from .case_schema import load_manifest
from .conformance_runner import run_conformance
from .determinism_runner import run_determinism_check
from .identity_runner import check_identity
from .manifest_validator import validate_manifest
from .negative_runner import run_negative
from .report import Report, SuiteResult
from .smoke_runner import run_smoke


def _find_repo_root(start: str) -> str:
    """Walk up from start to find the repository root (has .git or PROJECT_CHARTER.md)."""
    current = os.path.abspath(start)
    for _ in range(10):  # safety limit
        if os.path.isdir(os.path.join(current, ".git")) or os.path.isfile(
            os.path.join(current, "PROJECT_CHARTER.md")
        ):
            return current
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    return os.path.abspath(start)


def run_all_suites(
    tests_dir: str,
    adapter: Any,
    artifacts_dir: str,
    *,
    repo_root: str = "",
) -> list[SuiteResult]:
    """Run conformance, negative, and smoke suites.

    Args:
        tests_dir: path to tests/ directory
        adapter: compiler adapter instance
        artifacts_dir: output directory for reports
        repo_root: repository root for relative paths

    Returns:
        List of SuiteResult for each corpus.
    """
    if not repo_root:
        repo_root = _find_repo_root(tests_dir)

    results = []

    # Conformance
    conf_dir = os.path.join(tests_dir, "conformance")
    if os.path.isdir(conf_dir):
        suite = run_conformance(
            conf_dir, adapter, artifacts_dir, repo_root=repo_root
        )
        results.append(suite)

    # Negative
    neg_dir = os.path.join(tests_dir, "negative")
    if os.path.isdir(neg_dir):
        suite = run_negative(
            neg_dir, adapter, artifacts_dir, repo_root=repo_root
        )
        results.append(suite)

    # Smoke
    smoke_dir = os.path.join(tests_dir, "smoke")
    if os.path.isdir(smoke_dir):
        suite = run_smoke(
            smoke_dir, adapter, artifacts_dir, repo_root=repo_root
        )
        results.append(suite)

    return results


def run_determinism(
    tests_dir: str,
    adapter: Any,
    artifacts_dir: str,
    *,
    corpus: str = "",
) -> list[Any]:
    """Run determinism check on cases from one or all corpora."""
    corpora = [corpus] if corpus else ["conformance", "negative", "smoke"]
    all_results = []

    for corp in corpora:
        manifest_path = os.path.join(tests_dir, corp, "manifest.json")
        if not os.path.isfile(manifest_path):
            continue
        manifest = load_manifest(manifest_path)
        cases_dir = os.path.join(tests_dir, corp, "cases")

        case_dirs = []
        for case_name in manifest["cases"]:
            case_dir = os.path.join(cases_dir, case_name)
            if os.path.isdir(case_dir):
                case_dirs.append(case_dir)

        results = run_determinism_check(case_dirs, adapter, artifacts_dir)
        all_results.extend(results)

    return all_results


def cmd_all(args: argparse.Namespace) -> int:
    """Run all suites and report results."""
    repo_root = args.repo_root or _find_repo_root(args.tests_dir)
    artifacts_dir = args.artifacts or os.path.join(args.tests_dir, "artifacts")

    adapter = StubCompilerAdapter()

    suites = run_all_suites(
        args.tests_dir, adapter, artifacts_dir, repo_root=repo_root
    )

    # Write report
    report = Report(artifacts_dir)
    report.add_suite_result(suites[0] if suites else SuiteResult(
        corpus="none", total=0, passed=0, failed=0, deferred=0
    ))
    for suite in suites[1:]:
        report.add_suite_result(suite)

    report_path = report.write("harness-results.jsonl")
    summary_path = report.write_suite_summary(suites)

    # Print summary
    total_all = sum(s.total for s in suites)
    passed_all = sum(s.passed for s in suites)
    failed_all = sum(s.failed for s in suites)
    deferred_all = sum(s.deferred for s in suites)

    print(f"Harness results: {passed_all}/{total_all} passed, "
          f"{failed_all} failed, {deferred_all} deferred")
    for suite in suites:
        print(f"  {suite.corpus}: {suite.passed}/{suite.total} passed, "
              f"{suite.failed} failed, {suite.deferred} deferred")
    print(f"Report: {report_path}")
    print(f"Summary: {summary_path}")

    return 1 if failed_all > 0 else 0


def cmd_conformance(args: argparse.Namespace) -> int:
    """Run conformance suite only."""
    repo_root = args.repo_root or _find_repo_root(args.tests_dir)
    artifacts_dir = args.artifacts or os.path.join(args.tests_dir, "artifacts")
    adapter = StubCompilerAdapter()

    suite = run_conformance(
        os.path.join(args.tests_dir, "conformance"),
        adapter,
        artifacts_dir,
        repo_root=repo_root,
    )

    print(f"Conformance: {suite.passed}/{suite.total} passed, "
          f"{suite.failed} failed, {suite.deferred} deferred")
    for case in suite.cases:
        if not case.passed:
            print(f"  FAIL {case.case}: {case.reason}")
    return 1 if suite.failed > 0 else 0


def cmd_negative(args: argparse.Namespace) -> int:
    """Run negative suite only."""
    repo_root = args.repo_root or _find_repo_root(args.tests_dir)
    artifacts_dir = args.artifacts or os.path.join(args.tests_dir, "artifacts")
    adapter = StubCompilerAdapter()

    suite = run_negative(
        os.path.join(args.tests_dir, "negative"),
        adapter,
        artifacts_dir,
        repo_root=repo_root,
    )

    print(f"Negative: {suite.passed}/{suite.total} passed, "
          f"{suite.failed} failed, {suite.deferred} deferred")
    for case in suite.cases:
        if not case.passed:
            print(f"  FAIL {case.case}: {case.reason}")
    return 1 if suite.failed > 0 else 0


def cmd_smoke(args: argparse.Namespace) -> int:
    """Run smoke suite only."""
    repo_root = args.repo_root or _find_repo_root(args.tests_dir)
    artifacts_dir = args.artifacts or os.path.join(args.tests_dir, "artifacts")
    adapter = StubCompilerAdapter()

    suite = run_smoke(
        os.path.join(args.tests_dir, "smoke"),
        adapter,
        artifacts_dir,
        repo_root=repo_root,
    )

    print(f"Smoke: {suite.passed}/{suite.total} passed, "
          f"{suite.failed} failed, {suite.deferred} deferred")
    for case in suite.cases:
        if not case.passed:
            print(f"  FAIL {case.case}: {case.reason}")
    return 1 if suite.failed > 0 else 0


def cmd_determinism(args: argparse.Namespace) -> int:
    """Run determinism check."""
    artifacts_dir = args.artifacts or os.path.join(args.tests_dir, "artifacts")
    adapter = StubCompilerAdapter()

    results = run_determinism(
        args.tests_dir, adapter, artifacts_dir, corpus=args.corpus or ""
    )

    identical = sum(1 for r in results if r.identical)
    different = sum(1 for r in results if not r.identical)

    print(f"Determinism: {identical}/{len(results)} identical, "
          f"{different} different")
    for r in results:
        if not r.identical:
            print(f"  DIFF {r.case}:")
            for d in r.differences:
                print(f"    {d}")
    return 1 if different > 0 else 0


def cmd_identity(args: argparse.Namespace) -> int:
    """Run identity check (M1-ready)."""
    if not args.stage1 or not args.stage2:
        print("error: --stage1 and --stage2 are required for identity check")
        return 1

    result = check_identity(
        args.stage1,
        args.stage2,
        pe1_path=args.pe1,
        pe2_path=args.pe2,
    )

    if result.identical:
        print(f"Identity: PASS ({result.artifact_count} artifacts compared)")
    else:
        print(f"Identity: FAIL ({len(result.differences)} differences)")
        for d in result.differences:
            print(f"  {d}")
    return 0 if result.identical else 1


def cmd_manifest(args: argparse.Namespace) -> int:
    """Validate build manifest."""
    result = validate_manifest(args.manifest)

    if result.valid:
        print("Manifest: PASS")
    else:
        print("Manifest: FAIL")
        for e in result.errors:
            print(f"  ERROR: {e}")
    for w in result.warnings:
        print(f"  WARN: {w}")
    return 0 if result.valid else 1


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        prog="tests.harness",
        description="AI-Co verification harness (WP-M0-05)",
    )
    parser.add_argument(
        "--tests-dir",
        default=os.path.join(os.path.dirname(__file__), ".."),
        help="path to tests/ directory",
    )
    parser.add_argument(
        "--repo-root",
        default="",
        help="repository root for relative paths",
    )
    parser.add_argument(
        "--artifacts",
        default="",
        help="output directory for reports",
    )

    subparsers = parser.add_subparsers(dest="command", help="command to run")

    # all
    sub_all = subparsers.add_parser("all", help="run all suites")
    sub_all.set_defaults(func=cmd_all)

    # conformance
    sub_conf = subparsers.add_parser("conformance", help="run conformance suite")
    sub_conf.set_defaults(func=cmd_conformance)

    # negative
    sub_neg = subparsers.add_parser("negative", help="run negative suite")
    sub_neg.set_defaults(func=cmd_negative)

    # smoke
    sub_smoke = subparsers.add_parser("smoke", help="run smoke suite")
    sub_smoke.set_defaults(func=cmd_smoke)

    # determinism
    sub_det = subparsers.add_parser("determinism", help="run determinism check")
    sub_det.add_argument("--corpus", default="", help="corpus to check")
    sub_det.set_defaults(func=cmd_determinism)

    # identity
    sub_id = subparsers.add_parser("identity", help="run identity check (M1)")
    sub_id.add_argument("--stage1", help="Stage 1 artifacts directory")
    sub_id.add_argument("--stage2", help="Stage 2 artifacts directory")
    sub_id.add_argument("--pe1", help="Stage 1 PE executable path")
    sub_id.add_argument("--pe2", help="Stage 2 PE executable path")
    sub_id.set_defaults(func=cmd_identity)

    # manifest
    sub_mf = subparsers.add_parser("manifest", help="validate build manifest")
    sub_mf.add_argument("manifest", help="path to build manifest JSON")
    sub_mf.set_defaults(func=cmd_manifest)

    args = parser.parse_args(argv)
    if not hasattr(args, "func"):
        parser.print_help()
        return 1

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
