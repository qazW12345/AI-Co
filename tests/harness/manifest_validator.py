"""Build manifest validator.

Checks spec §14.4 fields:
- no self-hash (FIND-G2-02)
- stage-invariant version/identity field (FIND-G2-03)
- relative paths (no absolute host paths)
- sorted options
- SHA-256 hashes present for each artifact
- schema_version present
- required fields present
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from typing import Any

from .report import ManifestValidationResult


# Required fields per spec §14.4
REQUIRED_MANIFEST_FIELDS = [
    "schema_version",
    "project_root",
    "entry_module",
    "module_list",
    "language_version",
    "build_options",
    "output_artifacts",
    "diagnostic_summary",
    "exit_status",
]


def _is_relative_path(path: str) -> bool:
    """Check if a path is relative (no drive letter, no leading slash)."""
    # Reject Windows absolute paths (C:\, D:\, etc.)
    if re.match(r"^[A-Za-z]:[/\\]", path):
        return False
    # Reject Unix absolute paths
    if path.startswith("/"):
        return False
    # Reject UNC paths
    if path.startswith("\\\\"):
        return False
    return True


def _is_canonical_path(path: str) -> bool:
    """Check if a path uses canonical separators (/ not \\)."""
    return "\\\\" not in path


def validate_manifest(manifest_path: str) -> ManifestValidationResult:
    """Validate a build manifest against spec §14.4.

    Args:
        manifest_path: path to the build manifest JSON file

    Returns:
        ManifestValidationResult with errors and warnings.
    """
    errors = []
    warnings = []

    # Load manifest
    if not os.path.isfile(manifest_path):
        return ManifestValidationResult(
            valid=False,
            errors=[f"manifest file not found: {manifest_path}"],
        )

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except json.JSONDecodeError as e:
        return ManifestValidationResult(
            valid=False,
            errors=[f"invalid JSON: {e}"],
        )

    if not isinstance(manifest, dict):
        return ManifestValidationResult(
            valid=False,
            errors=["manifest must be a JSON object"],
        )

    # Check required fields
    for field in REQUIRED_MANIFEST_FIELDS:
        if field not in manifest:
            errors.append(f"missing required field '{field}'")

    if errors:
        return ManifestValidationResult(valid=False, errors=errors)

    # Check schema_version
    sv = manifest.get("schema_version")
    if not isinstance(sv, (str, int)):
        errors.append(f"schema_version must be string or int, got {type(sv).__name__}")

    # Check language_version is stage-invariant (string, non-empty)
    lv = manifest.get("language_version", "")
    if not isinstance(lv, str) or not lv:
        errors.append("language_version must be a non-empty string")

    # Check project_root is a relative path
    project_root = manifest.get("project_root", "")
    if project_root and not _is_relative_path(project_root):
        errors.append(
            f"project_root must be relative, got '{project_root}'"
        )

    # Check build_options is sorted
    build_options = manifest.get("build_options", [])
    if isinstance(build_options, list):
        if build_options != sorted(build_options):
            errors.append("build_options must be in sorted order")

    # Check output_artifacts structure and hashes
    output_artifacts = manifest.get("output_artifacts", {})
    if isinstance(output_artifacts, dict):
        for artifact_path, hash_value in output_artifacts.items():
            # Check artifact path is relative
            if not _is_relative_path(artifact_path):
                errors.append(
                    f"output_artifact path must be relative: '{artifact_path}'"
                )
            # Check path uses canonical separators
            if not _is_canonical_path(artifact_path):
                warnings.append(
                    f"output_artifact path uses non-canonical separators: "
                    f"'{artifact_path}'"
                )
            # Check hash is a valid hex string (SHA-256 = 64 hex chars)
            if isinstance(hash_value, str):
                if len(hash_value) != 64:
                    errors.append(
                        f"output_artifact hash must be 64-char hex "
                        f"(SHA-256), got {len(hash_value)} chars "
                        f"for '{artifact_path}'"
                    )
                elif not all(c in "0123456789abcdef" for c in hash_value):
                    errors.append(
                        f"output_artifact hash must be lowercase hex "
                        f"for '{artifact_path}'"
                    )
            else:
                errors.append(
                    f"output_artifact hash must be string for '{artifact_path}'"
                )

    # Check module_list entries have relative paths
    module_list = manifest.get("module_list", [])
    if isinstance(module_list, list):
        for module in module_list:
            if isinstance(module, dict):
                source_path = module.get("source_path", "")
                if source_path and not _is_relative_path(source_path):
                    errors.append(
                        f"module source_path must be relative: "
                        f"'{source_path}'"
                    )
            elif isinstance(module, str):
                # Module name only, no path to check
                pass

    # Check diagnostic_summary structure
    diag_summary = manifest.get("diagnostic_summary", {})
    if isinstance(diag_summary, dict):
        for severity, count in diag_summary.items():
            if not isinstance(count, int):
                errors.append(
                    f"diagnostic_summary.{severity} must be int, "
                    f"got {type(count).__name__}"
                )

    # Check exit_status is int
    exit_status = manifest.get("exit_status")
    if not isinstance(exit_status, int):
        errors.append(f"exit_status must be int, got {type(exit_status).__name__}")

    # SELF-HASH EXCLUSION (FIND-G2-02): manifest must NOT contain a hash of itself
    for key in ("self_hash", "manifest_hash", "hash", "content_hash"):
        if key in manifest:
            errors.append(
                f"manifest must not contain self-referential hash field '{key}' "
                f"(FIND-G2-02: self-hash exclusion)"
            )

    # Check entry_module
    entry_module = manifest.get("entry_module", "")
    if isinstance(entry_module, str) and entry_module:
        # Entry module should not contain path separators (it's a module name)
        if "/" in entry_module or "\\" in entry_module:
            warnings.append(
                f"entry_module contains path separators: '{entry_module}'"
            )

    return ManifestValidationResult(
        valid=len(errors) == 0,
        errors=errors,
        warnings=warnings,
    )
