"""Determinism runner.

Builds the same input set twice and byte-compares COFF objects + manifests.
Reports differences deterministically.
"""

from __future__ import annotations

import filecmp
import hashlib
import os
from typing import Any

from .adapter import CompilerAdapter
from .report import DeterminismResult


def _list_artifacts(output_dir: str) -> list[str]:
    """List all files in output_dir as relative paths, sorted."""
    artifacts = []
    for root, dirs, files in os.walk(output_dir):
        dirs.sort()  # deterministic traversal
        for fname in sorted(files):
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, output_dir)
            rel = rel.replace("\\", "/")  # normalize separators
            artifacts.append(rel)
    return artifacts


def _hash_file(path: str) -> str:
    """Compute SHA-256 hash of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def check_determinism(
    case_dir: str,
    adapter: CompilerAdapter,
    artifacts_dir: str,
    *,
    case_name: str = "",
) -> DeterminismResult:
    """Run the compiler twice on the same input and byte-compare all artifacts.

    Args:
        case_dir: path to the case directory containing input.ai
        adapter: compiler adapter instance
        artifacts_dir: base directory for temporary output
        case_name: case name for reporting (defaults to dirname)

    Returns:
        DeterminismResult indicating whether outputs are byte-identical.
    """
    if not case_name:
        case_name = os.path.basename(case_dir)

    dir_a = os.path.join(artifacts_dir, "determinism", case_name, "run_a")
    dir_b = os.path.join(artifacts_dir, "determinism", case_name, "run_b")

    # Run twice
    try:
        result_a = adapter.compile_for_determinism(case_dir, dir_a)
        result_b = adapter.compile_for_determinism(case_dir, dir_b)
    except Exception as e:
        return DeterminismResult(
            case=case_name,
            identical=False,
            differences=[f"compiler error: {e}"],
        )

    # Compare stdout
    differences = []
    if result_a.stdout != result_b.stdout:
        differences.append(
            f"stdout differs: run_a={result_a.stdout.hex()}, "
            f"run_b={result_b.stdout.hex()}"
        )

    # Compare stderr
    if result_a.stderr != result_b.stderr:
        differences.append(
            f"stderr differs: run_a={result_a.stderr.hex()}, "
            f"run_b={result_b.stderr.hex()}"
        )

    # Compare exit code
    if result_a.exit_code != result_b.exit_code:
        differences.append(
            f"exit_code differs: run_a={result_a.exit_code}, "
            f"run_b={result_b.exit_code}"
        )

    # Compare all artifact files byte-for-byte
    artifacts_a = _list_artifacts(dir_a) if os.path.isdir(dir_a) else []
    artifacts_b = _list_artifacts(dir_b) if os.path.isdir(dir_b) else []

    if artifacts_a != artifacts_b:
        missing_in_b = set(artifacts_a) - set(artifacts_b)
        extra_in_b = set(artifacts_b) - set(artifacts_a)
        if missing_in_b:
            differences.append(f"artifacts missing in run_b: {sorted(missing_in_b)}")
        if extra_in_b:
            differences.append(f"extra artifacts in run_b: {sorted(extra_in_b)}")
    else:
        for artifact in artifacts_a:
            hash_a = _hash_file(os.path.join(dir_a, artifact))
            hash_b = _hash_file(os.path.join(dir_b, artifact))
            if hash_a != hash_b:
                differences.append(
                    f"artifact '{artifact}' differs: "
                    f"run_a={hash_a}, run_b={hash_b}"
                )

    return DeterminismResult(
        case=case_name,
        identical=len(differences) == 0,
        differences=differences,
    )


def run_determinism_check(
    case_dirs: list[str],
    adapter: CompilerAdapter,
    artifacts_dir: str,
) -> list[DeterminismResult]:
    """Run determinism check on multiple cases.

    Args:
        case_dirs: list of case directory paths
        adapter: compiler adapter instance
        artifacts_dir: base directory for temporary output

    Returns:
        List of DeterminismResult, one per case.
    """
    results = []
    for case_dir in sorted(case_dirs):
        result = check_determinism(
            case_dir, adapter, artifacts_dir
        )
        results.append(result)
    return results
