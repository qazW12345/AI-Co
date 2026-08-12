"""Unit tests for the AI-Co verification harness.

Tests the schema validation, comparison logic, manifest validator,
and runner integration using the existing corpus data.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# Derive paths from this file's location
_HARNESS_DIR = os.path.dirname(os.path.abspath(__file__))
_TESTS_DIR = os.path.dirname(_HARNESS_DIR)
_REPO_ROOT = os.path.dirname(_TESTS_DIR)

# Use relative imports — the test file lives inside tests/harness/
from .case_schema import (
    Case,
    DiagnosticsExpected,
    Meta,
    RunExpected,
    SchemaError,
    load_case,
    load_manifest,
    validate_diag_record,
    validate_expected,
    validate_meta,
)
from .adapter import StubCompilerAdapter, CompilerResult
from .conformance_runner import run_conformance
from .negative_runner import run_negative, _parse_jsonl_stderr
from .smoke_runner import run_smoke
from .determinism_runner import check_determinism
from .identity_runner import check_identity
from .manifest_validator import validate_manifest
from .report import Report, CaseResult, SuiteResult


class TestManifestValidatorFixes(unittest.TestCase):
    """Test fixes for manifest validator minor findings."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _write_manifest(self, data: dict) -> str:
        path = os.path.join(self.tmpdir, "build-manifest.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        return path

    def test_manifest_rejects_stage_field_m1(self):
        """M1: manifest must reject stage field (FIND-G2-03)."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "src",
            "entry_module": "main",
            "module_list": [],
            "language_version": "0.1.1",
            "build_options": [],
            "output_artifacts": {},
            "diagnostic_summary": {},
            "exit_status": 0,
            "stage": "host",  # FORBIDDEN
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)
        self.assertTrue(any("stage/identity field 'stage'" in e for e in result.errors))

    def test_manifest_rejects_host_compiler_field_m1(self):
        """M1: manifest must reject host_compiler field (FIND-G2-03)."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "src",
            "entry_module": "main",
            "module_list": [],
            "language_version": "0.1.1",
            "build_options": [],
            "output_artifacts": {},
            "diagnostic_summary": {},
            "exit_status": 0,
            "host_compiler": "msvc",  # FORBIDDEN
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)
        self.assertTrue(any("stage/identity field 'host_compiler'" in e for e in result.errors))

    def test_manifest_rejects_non_list_build_options_m4(self):
        """M4: manifest must reject non-list build_options."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "src",
            "entry_module": "main",
            "module_list": [],
            "language_version": "0.1.1",
            "build_options": "--release",  # NOT A LIST
            "output_artifacts": {},
            "diagnostic_summary": {},
            "exit_status": 0,
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)
        self.assertTrue(any("build_options must be a list" in e for e in result.errors))

    def test_manifest_accepts_valid_list_build_options_m4(self):
        """M4: manifest must accept valid list build_options."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "src",
            "entry_module": "main",
            "module_list": [],
            "language_version": "0.1.1",
            "build_options": ["--release", "--target-x86_64"],  # VALID LIST
            "output_artifacts": {},
            "diagnostic_summary": {},
            "exit_status": 0,
        })
        result = validate_manifest(path)
        self.assertTrue(result.valid, f"Errors: {result.errors}")


class TestCaseSchemaFixes(unittest.TestCase):
    """Test fixes for case schema minor findings."""

    def test_diagnostic_record_recovery_optional_for_warning_note_m2(self):
        """M2: recovery optional for warning/note records."""
        # Warning record without recovery should be valid
        warning_record = {
            "schema_version": "1",
            "code": "AIC-W0001",
            "severity": "warning",
            "phase": "lex",
            "message": "test warning",
            "primary_span": {
                "file": "input.ai",
                "start": {"line": 1, "col": 1, "offset": 0},
                "end": {"line": 1, "col": 5, "offset": 4},
            },
            # No recovery field - should be OK for warning
        }
        try:
            validate_diag_record(warning_record)
        except SchemaError:
            self.fail("validate_diag_record() raised SchemaError unexpectedly for warning record without recovery")

        # Note record without recovery should be valid
        note_record = {
            "schema_version": "1",
            "code": "AIC-N0001",
            "severity": "note",
            "phase": "lex",
            "message": "test note",
            "primary_span": {
                "file": "input.ai",
                "start": {"line": 1, "col": 1, "offset": 0},
                "end": {"line": 1, "col": 5, "offset": 4},
            },
            # No recovery field - should be OK for note
        }
        try:
            validate_diag_record(note_record)
        except SchemaError:
            self.fail("validate_diag_record() raised SchemaError unexpectedly for note record without recovery")

        # Error record without recovery should be INVALID
        error_record_no_recovery = {
            "schema_version": "1",
            "code": "AIC-E0001",
            "severity": "error",
            "phase": "lex",
            "message": "test error",
            "primary_span": {
                "file": "input.ai",
                "start": {"line": 1, "col": 1, "offset": 0},
                "end": {"line": 1, "col": 5, "offset": 4},
            },
            # Missing recovery field - should FAIL for error
        }
        with self.assertRaises(SchemaError):
            validate_diag_record(error_record_no_recovery)

        # Trap record without recovery should be INVALID
        trap_record_no_recovery = {
            "schema_version": "1",
            "code": "AIC-R0802",
            "severity": "error",
            "phase": "trap",
            "message": "test trap",
            "primary_span": None,
            # Missing recovery field - should FAIL for trap
        }
        with self.assertRaises(SchemaError):
            validate_diag_record(trap_record_no_recovery)

    def test_manifest_rejects_unknown_fields_m3(self):
        """M3: manifest must reject unknown top-level fields."""
        manifest_data = {
            "schema_version": 1,
            "corpus": "conformance",
            "description": "Test manifest",
            "cases": ["test-case"],
            "unknown_field": "should_be_rejected",  # FORBIDDEN
        }
        with self.assertRaises(SchemaError) as cm:
            load_manifest_from_dict(manifest_data)
        self.assertIn("manifest.json contains unknown field 'unknown_field'", str(cm.exception))

    def test_manifest_rejects_extra_fields_in_run_record_m3(self):
        """M3: run expected record must reject unknown fields."""
        with self.assertRaises(SchemaError) as cm:
            validate_expected({
                "kind": "run",
                "stdout": "hello",
                "exit_code": 0,
                "extra_field": "should_be_rejected",  # FORBIDDEN
            }, "conformance")
        self.assertIn("expected.json contains unknown field 'extra_field'", str(cm.exception))

    def test_manifest_rejects_extra_fields_in_diagnostics_record_m3(self):
        """M3: diagnostics expected record must reject unknown fields."""
        with self.assertRaises(SchemaError) as cm:
            validate_expected({
                "kind": "diagnostics",
                "records": [{
                    "schema_version": "1",
                    "code": "AIC-L0001",
                    "severity": "error",
                    "phase": "lex",
                    "message": "test",
                    "primary_span": {
                        "file": "input.ai",
                        "start": {"line": 1, "col": 1, "offset": 0},
                        "end": {"line": 1, "col": 2, "offset": 1},
                    },
                    "recovery": "authoritative",
                    "extra_field": "should_be_rejected",  # FORBIDDEN
                }]
            }, "negative")
        self.assertIn("unknown field 'extra_field'", str(cm.exception))

    def test_meta_rejects_unknown_fields_m3(self):
        """M3: meta.json must reject unknown fields."""
        with self.assertRaises(SchemaError) as cm:
            validate_meta({
                "spec_ref": "§8.1",
                "unknown_field": "should_be_rejected",  # FORBIDDEN
            })
        self.assertIn("meta.json contains unknown field 'unknown_field'", str(cm.exception))

    def test_manifest_requires_stdout_in_run_record_m3(self):
        """M3: run expected record must have stdout field (required per §1a)."""
        with self.assertRaises(SchemaError) as cm:
            validate_expected({
                "kind": "run",
                # Missing stdout field
                "exit_code": 0,
            }, "conformance")
        self.assertIn("run expected record missing 'stdout'", str(cm.exception))


def load_manifest_from_dict(data: dict) -> dict:
    """Helper to load manifest from dict for testing."""
    import tempfile
    import json
    import os
    from .case_schema import load_manifest, SchemaError
    
    tmpdir = tempfile.mkdtemp()
    path = os.path.join(tmpdir, "manifest.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return load_manifest(path)


if __name__ == "__main__":
    unittest.main()