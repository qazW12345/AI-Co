"""Identity runner (M1-ready).

Compares Stage 1 vs Stage 2 primary artifacts (COFF objects + build manifests)
and two PE files byte-for-byte.  Records comparison input evidence per spec §16.3.
"""

from __future__ import annotations

import hashlib
import os
from typing import Any

from .report import IdentityResult


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


def _list_artifacts(output_dir: str) -> list[str]:
    """List all files in output_dir as relative paths, sorted."""
    artifacts = []
    for root, dirs, files in os.walk(output_dir):
        dirs.sort()
        for fname in sorted(files):
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, output_dir)
            rel = rel.replace("\\", "/")
            artifacts.append(rel)
    return artifacts


def check_identity(
    stage1_dir: str,
    stage2_dir: str,
    pe1_path: str | None = None,
    pe2_path: str | None = None,
) -> IdentityResult:
    """Compare Stage 1 vs Stage 2 primary artifacts byte-for-byte.

    Per spec §16.3/§16.5:
    - Primary identity: COFF objects + build manifests must be byte-identical
    - Secondary identity: linked PE executables must be byte-identical

    Args:
        stage1_dir: directory containing Stage 1 COFF objects + manifests
        stage2_dir: directory containing Stage 2 COFF objects + manifests
        pe1_path: optional path to Stage 1 PE executable
        pe2_path: optional path to Stage 2 PE executable

    Returns:
        IdentityResult indicating whether all artifacts are byte-identical.
    """
    differences = []

    # Compare COFF objects + manifests (primary identity)
    artifacts1 = _list_artifacts(stage1_dir) if os.path.isdir(stage1_dir) else []
    artifacts2 = _list_artifacts(stage2_dir) if os.path.isdir(stage2_dir) else []

    if artifacts1 != artifacts2:
        missing_in_2 = set(artifacts1) - set(artifacts2)
        extra_in_2 = set(artifacts2) - set(artifacts1)
        if missing_in_2:
            differences.append(
                f"artifacts missing in Stage 2: {sorted(missing_in_2)}"
            )
        if extra_in_2:
            differences.append(
                f"extra artifacts in Stage 2: {sorted(extra_in_2)}"
            )
    else:
        for artifact in artifacts1:
            hash1 = _hash_file(os.path.join(stage1_dir, artifact))
            hash2 = _hash_file(os.path.join(stage2_dir, artifact))
            if hash1 != hash2:
                differences.append(
                    f"artifact '{artifact}' differs: "
                    f"stage1={hash1}, stage2={hash2}"
                )

    # Compare PE executables (secondary identity)
    if pe1_path and pe2_path:
        if not os.path.isfile(pe1_path):
            differences.append(f"Stage 1 PE not found: {pe1_path}")
        elif not os.path.isfile(pe2_path):
            differences.append(f"Stage 2 PE not found: {pe2_path}")
        else:
            hash_pe1 = _hash_file(pe1_path)
            hash_pe2 = _hash_file(pe2_path)
            if hash_pe1 != hash_pe2:
                differences.append(
                    f"PE executable differs: stage1={hash_pe1}, stage2={hash_pe2}"
                )
    elif pe1_path != pe2_path:
        # One provided but not the other
        if pe1_path and not pe2_path:
            differences.append("Stage 2 PE path not provided")
        elif pe2_path and not pe1_path:
            differences.append("Stage 1 PE path not provided")

    return IdentityResult(
        identical=len(differences) == 0,
        artifact_count=len(artifacts1),
        differences=differences,
    )
