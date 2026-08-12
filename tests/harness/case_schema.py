"""Case schema definitions and validation per manifest §1a (normative).

Each corpus case is a directory containing:
  - input.ai     — the AI-Co source program
  - expected.json — expected result record
  - meta.json    — (optional) spec_ref, codes, deferral_reason

Corpus-level manifest.json: {"schema_version": 1, "cases": [...]}.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Any

SCHEMA_VERSION = 1

# --- Error / diagnostic record shape (per DIAGNOSTIC-CONTRACT §2-§3) ---

REQUIRED_DIAG_FIELDS = {
    "schema_version": str,
    "code": str,
    "severity": str,
    "phase": str,
    "message": str,
    "primary_span": (dict, type(None)),
}

# All fields a diagnostic record may contain per DIAGNOSTIC-CONTRACT §3-§4
# (required + optional).  Unknown fields in expected records are format
# errors per manifest §1a ("missing/extra fields are format errors").
KNOWN_DIAG_RECORD_FIELDS = {
    "schema_version", "code", "severity", "phase", "message",
    "primary_span", "recovery", "secondary_spans",
    "expected", "actual", "causes", "corrections", "related",
    "trap_code", "exit_code",
}

VALID_SEVERITIES = {"error", "warning", "note"}
VALID_PHASES = {
    "lex", "syntax", "name", "type", "semantic",
    "ir", "backend", "object", "link", "build", "trap",
}
VALID_RECOVERIES = {"authoritative", "cascading", "recovery_derived"}


class SchemaError(Exception):
    """Raised when a case or manifest violates the §1a schema."""


# --- Expected record shapes ---


@dataclass
class RunExpected:
    """Expected output for a successful execution (conformance / smoke)."""

    kind: str  # "run"
    stdout: str  # expected stdout bytes (may be empty string)
    exit_code: int
    stderr_contains: list[str] = field(default_factory=list)
    trap: bool = False


@dataclass
class DiagnosticsExpected:
    """Expected output for a compilation that emits diagnostics (negative)."""

    kind: str  # "diagnostics"
    records: list[dict[str, Any]]  # ordered list of expected records


@dataclass
class Meta:
    """Optional case metadata."""

    spec_ref: str = ""
    codes: list[str] = field(default_factory=list)
    deferral_reason: str = ""


@dataclass
class Case:
    """A single corpus case with all associated files."""

    name: str
    case_dir: str  # absolute path to case directory
    expected: RunExpected | DiagnosticsExpected
    meta: Meta
    deferred: bool = False


# --- Validation helpers ---


def _validate_span(span: Any, *, strict: bool = True) -> None:
    """Validate a span object or null."""
    if span is None:
        return
    if not isinstance(span, dict):
        raise SchemaError(f"span must be object or null, got {type(span).__name__}")
    for key in ("file", "start", "end"):
        if key not in span:
            raise SchemaError(f"span missing required field '{key}'")
    if not isinstance(span["file"], str):
        raise SchemaError(f"span.file must be string, got {type(span['file']).__name__}")
    for point_name in ("start", "end"):
        point = span[point_name]
        if not isinstance(point, dict):
            raise SchemaError(f"span.{point_name} must be object")
        for coord in ("line", "col", "offset"):
            if coord not in point:
                raise SchemaError(f"span.{point_name} missing '{coord}'")
            if not isinstance(point[coord], int):
                raise SchemaError(
                    f"span.{point_name}.{coord} must be int, "
                    f"got {type(point[coord]).__name__}"
                )


def validate_diag_record(record: dict[str, Any], *, strict: bool = True) -> None:
    """Validate a single diagnostic record against the contract shape."""
    if not isinstance(record, dict):
        raise SchemaError(f"diagnostic record must be object, got {type(record).__name__}")

    # Manifest §1a: extra fields in expected records are format errors
    for key in record:
        if key not in KNOWN_DIAG_RECORD_FIELDS:
            raise SchemaError(
                f"diagnostic record contains unknown field '{key}'"
            )

    # Check required fields exist with correct types
    for field_name, expected_type in REQUIRED_DIAG_FIELDS.items():
        if field_name not in record:
            raise SchemaError(f"diagnostic record missing required field '{field_name}'")
        value = record[field_name]
        if isinstance(expected_type, tuple):
            if not isinstance(value, expected_type):
                raise SchemaError(
                    f"record.{field_name} expected {expected_type}, "
                    f"got {type(value).__name__}"
                )
        else:
            if not isinstance(value, expected_type):
                raise SchemaError(
                    f"record.{field_name} expected {expected_type.__name__}, "
                    f"got {type(value).__name__}"
                )

    # Validate enum values
    if record["severity"] not in VALID_SEVERITIES:
        raise SchemaError(
            f"record.severity must be one of {VALID_SEVERITIES}, "
            f"got '{record['severity']}'"
        )
    if record["phase"] not in VALID_PHASES:
        raise SchemaError(
            f"record.phase must be one of {VALID_PHASES}, "
            f"got '{record['phase']}'"
        )
    if record["schema_version"] != "1":
        raise SchemaError(
            f"record.schema_version must be '1', got '{record['schema_version']}'"
        )

    # recovery is required on error and trap records
    if record["severity"] == "error" or record["phase"] == "trap":
        if record.get("recovery") not in VALID_RECOVERIES:
            raise SchemaError(
                f"record.recovery must be one of {VALID_RECOVERIES} "
                f"(required for error/trap), got '{record.get('recovery')}'"
            )

    # Validate primary span
    _validate_span(record["primary_span"], strict=strict)

    # Validate optional secondary_spans
    if "secondary_spans" in record and record["secondary_spans"] is not None:
        if not isinstance(record["secondary_spans"], list):
            raise SchemaError("record.secondary_spans must be array or null")
        for i, span in enumerate(record["secondary_spans"]):
            try:
                _validate_span(span, strict=strict)
            except SchemaError as e:
                raise SchemaError(f"record.secondary_spans[{i}]: {e}")


def validate_expected(expected: dict[str, Any], corpus: str) -> RunExpected | DiagnosticsExpected:
    """Validate and parse expected.json content."""
    if not isinstance(expected, dict):
        raise SchemaError("expected.json must be a JSON object")

    kind = expected.get("kind")
    if kind is None:
        raise SchemaError("expected.json missing 'kind' field")

    if kind == "run":
        # Validate allowed fields for run expected
        allowed_keys = {"kind", "stdout", "exit_code", "stderr_contains", "trap"}
        for key in expected:
            if key not in allowed_keys:
                raise SchemaError(f"expected.json contains unknown field '{key}' for run record")
        if "exit_code" not in expected:
            raise SchemaError("run expected record missing 'exit_code'")
        if not isinstance(expected["exit_code"], int):
            raise SchemaError(f"exit_code must be int, got {type(expected['exit_code']).__name__}")
        if "stdout" not in expected:
            raise SchemaError("run expected record missing 'stdout'")
        stdout = expected["stdout"]
        if not isinstance(stdout, str):
            raise SchemaError(f"stdout must be string, got {type(stdout).__name__}")
        stderr_contains = expected.get("stderr_contains", [])
        if not isinstance(stderr_contains, list):
            raise SchemaError("stderr_contains must be array")
        trap = expected.get("trap", False)
        if not isinstance(trap, bool):
            raise SchemaError(f"trap must be bool, got {type(trap).__name__}")
        return RunExpected(
            kind="run",
            stdout=stdout,
            exit_code=expected["exit_code"],
            stderr_contains=stderr_contains,
            trap=trap,
        )

    elif kind == "diagnostics":
        # Validate allowed fields for diagnostics expected
        allowed_keys = {"kind", "records"}
        for key in expected:
            if key not in allowed_keys:
                raise SchemaError(f"expected.json contains unknown field '{key}' for diagnostics record")
        records = expected.get("records", [])
        if not isinstance(records, list):
            raise SchemaError("diagnostics records must be array")
        for i, rec in enumerate(records):
            try:
                validate_diag_record(rec)
            except SchemaError as e:
                raise SchemaError(f"expected record[{i}]: {e}")
        return DiagnosticsExpected(kind="diagnostics", records=records)

    else:
        raise SchemaError(f"unknown expected kind '{kind}'")


def validate_meta(meta: dict[str, Any]) -> Meta:
    """Validate and parse meta.json content."""
    if not isinstance(meta, dict):
        raise SchemaError("meta.json must be a JSON object")
    # Manifest §1a: extra fields in meta.json are format errors
    known_meta_fields = {"spec_ref", "codes", "deferral_reason"}
    for key in meta:
        if key not in known_meta_fields:
            raise SchemaError(f"meta.json contains unknown field '{key}'")
    return Meta(
        spec_ref=meta.get("spec_ref", ""),
        codes=meta.get("codes", []),
        deferral_reason=meta.get("deferral_reason", ""),
    )


def load_case(case_dir: str) -> Case:
    """Load and validate a single corpus case from its directory."""
    case_name = os.path.basename(case_dir)

    # Read meta.json first (may indicate deferral without expected.json)
    meta = Meta()
    meta_path = os.path.join(case_dir, "meta.json")
    if os.path.isfile(meta_path):
        with open(meta_path, "r", encoding="utf-8") as f:
            meta_data = json.load(f)
        meta = validate_meta(meta_data)

    # Determine deferral
    deferred = bool(meta.deferral_reason)

    # Read expected.json
    expected_path = os.path.join(case_dir, "expected.json")
    if not os.path.isfile(expected_path):
        if deferred:
            # Deferred cases may lack expected.json; provide empty defaults
            expected: RunExpected | DiagnosticsExpected = DiagnosticsExpected(
                kind="diagnostics", records=[]
            )
            return Case(
                name=case_name,
                case_dir=case_dir,
                expected=expected,
                meta=meta,
                deferred=True,
            )
        raise SchemaError(f"case '{case_name}': missing expected.json")

    with open(expected_path, "r", encoding="utf-8") as f:
        expected_data = json.load(f)
    expected = validate_expected(expected_data, corpus="unknown")

    return Case(
        name=case_name,
        case_dir=case_dir,
        expected=expected,
        meta=meta,
        deferred=deferred,
    )


def load_manifest(manifest_path: str) -> dict[str, Any]:
    """Load and validate a corpus-level manifest.json."""
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    if not isinstance(manifest, dict):
        raise SchemaError("manifest.json must be a JSON object")
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise SchemaError(
            f"manifest schema_version must be {SCHEMA_VERSION}, "
            f"got {manifest.get('schema_version')}"
        )
    if "cases" not in manifest:
        raise SchemaError("manifest.json missing 'cases' array")
    if not isinstance(manifest["cases"], list):
        raise SchemaError("manifest.cases must be array")

    # Verify all case names are strings and unique
    seen: set[str] = set()
    for name in manifest["cases"]:
        if not isinstance(name, str):
            raise SchemaError(f"case name must be string, got {type(name).__name__}")
        if name in seen:
            raise SchemaError(f"duplicate case name '{name}'")
        seen.add(name)

    # Manifest §1a: reject unknown top-level fields
    known_keys = {"schema_version", "corpus", "description", "cases"}
    for key in manifest:
        if key not in known_keys:
            raise SchemaError(f"manifest.json contains unknown field '{key}'")

    return manifest
