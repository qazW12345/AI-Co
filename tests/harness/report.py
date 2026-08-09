"""Deterministic report generation for harness output.

Reports use repository-relative paths, no timestamps, no absolute host paths.
Output format: JSONL (one JSON object per line) to tests/artifacts/.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict
from typing import Any


@dataclass
class CaseResult:
    """Result of running a single case through the harness."""

    case: str  # case name (repository-relative-ish, no absolute path)
    corpus: str  # "conformance", "negative", "smoke"
    passed: bool
    deferred: bool = False
    reason: str = ""  # empty if passed; failure explanation if not
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class SuiteResult:
    """Aggregated result for one corpus suite run."""

    corpus: str
    total: int
    passed: int
    failed: int
    deferred: int
    cases: list[CaseResult] = field(default_factory=list)


@dataclass
class DeterminismResult:
    """Result of a determinism comparison."""

    case: str
    identical: bool
    differences: list[str] = field(default_factory=list)


@dataclass
class IdentityResult:
    """Result of an identity (Stage 1 vs Stage 2) comparison."""

    identical: bool
    artifact_count: int
    differences: list[str] = field(default_factory=list)


@dataclass
class ManifestValidationResult:
    """Result of manifest validation against spec §14.4."""

    valid: bool
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


class Report:
    """Deterministic harness report writer.

    All paths are repository-relative.  No timestamps.  No host identity.
    Output is JSONL sorted by case name for reproducibility.
    """

    def __init__(self, artifacts_dir: str) -> None:
        self.artifacts_dir = artifacts_dir
        self._results: list[dict[str, Any]] = []

    def add_case_result(self, result: CaseResult) -> None:
        self._results.append(asdict(result))

    def add_suite_result(self, suite: SuiteResult) -> None:
        for case_result in suite.cases:
            self.add_case_result(case_result)

    def write(self, filename: str) -> str:
        """Write results to a JSONL file. Returns the file path."""
        os.makedirs(self.artifacts_dir, exist_ok=True)
        filepath = os.path.join(self.artifacts_dir, filename)

        # Sort by corpus then case name for deterministic output
        sorted_results = sorted(
            self._results, key=lambda r: (r.get("corpus", ""), r.get("case", ""))
        )

        with open(filepath, "w", encoding="utf-8") as f:
            for result in sorted_results:
                f.write(json.dumps(result, separators=(",", ":")) + "\n")

        return filepath

    def write_suite_summary(self, suites: list[SuiteResult]) -> str:
        """Write a deterministic summary of all suite runs."""
        os.makedirs(self.artifacts_dir, exist_ok=True)
        filepath = os.path.join(self.artifacts_dir, "harness-summary.jsonl")

        summary_lines = []
        for suite in sorted(suites, key=lambda s: s.corpus):
            line = {
                "corpus": suite.corpus,
                "total": suite.total,
                "passed": suite.passed,
                "failed": suite.failed,
                "deferred": suite.deferred,
            }
            summary_lines.append(json.dumps(line, separators=(",", ":")))

        with open(filepath, "w", encoding="utf-8") as f:
            f.write("\n".join(summary_lines) + "\n")

        return filepath

    @staticmethod
    def to_relative_path(path: str, repo_root: str) -> str:
        """Convert an absolute path to a repository-relative path."""
        try:
            rel = os.path.relpath(path, repo_root)
            # Normalize to forward slashes for consistency
            return rel.replace("\\", "/")
        except ValueError:
            # On Windows, relpath fails if on different drives
            return path
