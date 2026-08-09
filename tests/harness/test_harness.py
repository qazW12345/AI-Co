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


class TestSchemaValidation(unittest.TestCase):
    """Test case schema validation per §1a."""

    def test_validate_run_expected(self):
        """Valid run expected record."""
        data = {"kind": "run", "stdout": "hello", "exit_code": 0}
        result = validate_expected(data, "conformance")
        self.assertIsInstance(result, RunExpected)
        self.assertEqual(result.stdout, "hello")
        self.assertEqual(result.exit_code, 0)

    def test_validate_run_expected_with_trap(self):
        """Run expected with trap fields."""
        data = {
            "kind": "run",
            "stdout": "",
            "exit_code": 70,
            "trap": True,
            "stderr_contains": ["AIC-R0802"],
        }
        result = validate_expected(data, "smoke")
        self.assertIsInstance(result, RunExpected)
        self.assertTrue(result.trap)
        self.assertEqual(result.stderr_contains, ["AIC-R0802"])

    def test_validate_diagnostics_expected(self):
        """Valid diagnostics expected record."""
        data = {
            "kind": "diagnostics",
            "records": [
                {
                    "schema_version": "1",
                    "code": "AIC-L0006",
                    "severity": "error",
                    "phase": "lex",
                    "message": "test message",
                    "primary_span": {
                        "file": "input.ai",
                        "start": {"line": 1, "col": 1, "offset": 0},
                        "end": {"line": 1, "col": 5, "offset": 4},
                    },
                    "recovery": "authoritative",
                }
            ],
        }
        result = validate_expected(data, "negative")
        self.assertIsInstance(result, DiagnosticsExpected)
        self.assertEqual(len(result.records), 1)

    def test_validate_diagnostics_empty_records(self):
        """Diagnostics with empty records (deferred cases)."""
        data = {"kind": "diagnostics", "records": []}
        result = validate_expected(data, "negative")
        self.assertIsInstance(result, DiagnosticsExpected)
        self.assertEqual(len(result.records), 0)

    def test_validate_expected_missing_kind(self):
        """Missing kind field raises error."""
        with self.assertRaises(SchemaError):
            validate_expected({}, "conformance")

    def test_validate_expected_unknown_kind(self):
        """Unknown kind raises error."""
        with self.assertRaises(SchemaError):
            validate_expected({"kind": "unknown"}, "conformance")

    def test_validate_expected_missing_exit_code(self):
        """Run expected missing exit_code raises error."""
        with self.assertRaises(SchemaError):
            validate_expected({"kind": "run", "stdout": ""}, "conformance")

    def test_validate_diag_record_missing_field(self):
        """Diagnostic record missing required field."""
        with self.assertRaises(SchemaError):
            validate_diag_record({"schema_version": "1"})  # missing code

    def test_validate_diag_record_invalid_severity(self):
        """Diagnostic record with invalid severity."""
        record = {
            "schema_version": "1",
            "code": "AIC-L0001",
            "severity": "fatal",  # invalid
            "phase": "lex",
            "message": "test",
            "primary_span": None,
            "recovery": "authoritative",
        }
        with self.assertRaises(SchemaError):
            validate_diag_record(record)

    def test_validate_diag_record_invalid_phase(self):
        """Diagnostic record with invalid phase."""
        record = {
            "schema_version": "1",
            "code": "AIC-L0001",
            "severity": "error",
            "phase": "invalid_phase",
            "message": "test",
            "primary_span": None,
            "recovery": "authoritative",
        }
        with self.assertRaises(SchemaError):
            validate_diag_record(record)

    def test_validate_diag_record_span_structure(self):
        """Diagnostic record with well-formed span."""
        record = {
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
        }
        validate_diag_record(record)  # should not raise

    def test_validate_diag_record_span_missing_coord(self):
        """Diagnostic record with span missing a coordinate."""
        record = {
            "schema_version": "1",
            "code": "AIC-L0001",
            "severity": "error",
            "phase": "lex",
            "message": "test",
            "primary_span": {
                "file": "input.ai",
                "start": {"line": 1, "col": 1},  # missing offset
                "end": {"line": 1, "col": 2, "offset": 1},
            },
            "recovery": "authoritative",
        }
        with self.assertRaises(SchemaError):
            validate_diag_record(record)


class TestManifestLoading(unittest.TestCase):
    """Test corpus manifest loading."""

    def setUp(self):
        self.corpus_dir = os.path.join(_TESTS_DIR, "conformance")

    def test_load_conformance_manifest(self):
        """Load the conformance manifest successfully."""
        manifest = load_manifest(os.path.join(self.corpus_dir, "manifest.json"))
        self.assertEqual(manifest["schema_version"], 1)
        self.assertIn("cases", manifest)
        self.assertIsInstance(manifest["cases"], list)
        self.assertGreater(len(manifest["cases"]), 0)

    def test_load_negative_manifest(self):
        """Load the negative manifest successfully."""
        manifest = load_manifest(
            os.path.join(_TESTS_DIR, "negative", "manifest.json")
        )
        self.assertEqual(manifest["schema_version"], 1)
        self.assertGreater(len(manifest["cases"]), 0)

    def test_load_smoke_manifest(self):
        """Load the smoke manifest successfully."""
        manifest = load_manifest(
            os.path.join(_TESTS_DIR, "smoke", "manifest.json")
        )
        self.assertEqual(manifest["schema_version"], 1)
        self.assertGreater(len(manifest["cases"]), 0)


class TestCaseLoading(unittest.TestCase):
    """Test loading individual cases from the corpus."""

    def test_load_conformance_case(self):
        """Load a conformance case."""
        case = load_case(
            os.path.join(_TESTS_DIR, "conformance", "cases", "18-1-var-i32-literal")
        )
        self.assertEqual(case.name, "18-1-var-i32-literal")
        self.assertIsInstance(case.expected, RunExpected)
        self.assertFalse(case.deferred)

    def test_load_negative_case(self):
        """Load a negative case."""
        case = load_case(
            os.path.join(_TESTS_DIR, "negative", "cases", "18-1-lex-int-overrun")
        )
        self.assertEqual(case.name, "18-1-lex-int-overrun")
        self.assertIsInstance(case.expected, DiagnosticsExpected)
        self.assertFalse(case.deferred)

    def test_load_deferred_case(self):
        """Load a deferred negative case."""
        case = load_case(
            os.path.join(
                _TESTS_DIR, "negative", "cases", "deferred-ir-invariant-violation"
            )
        )
        self.assertTrue(case.deferred)
        self.assertTrue(case.meta.deferral_reason.startswith("Compiler internal"))

    def test_load_smoke_trap_case(self):
        """Load a smoke trap case."""
        case = load_case(
            os.path.join(_TESTS_DIR, "smoke", "cases", "smoke-trap-overflow")
        )
        self.assertIsInstance(case.expected, RunExpected)
        self.assertTrue(case.expected.trap)
        self.assertEqual(case.expected.exit_code, 70)

    def test_load_smoke_deferred_case(self):
        """Load a deferred smoke case."""
        case = load_case(
            os.path.join(
                _TESTS_DIR, "smoke", "cases", "deferred-trap-bool-byte"
            )
        )
        self.assertTrue(case.deferred)


class TestStubCompilerAdapter(unittest.TestCase):
    """Test the stub compiler adapter."""

    def setUp(self):
        self.adapter = StubCompilerAdapter()
        self.tmpdir = tempfile.mkdtemp()

    def test_conformance_conformance_output(self):
        """Stub produces correct output for conformance cases."""
        case_dir = os.path.join(
            _TESTS_DIR, "conformance", "cases", "18-1-var-i32-literal"
        )
        output_dir = os.path.join(self.tmpdir, "conf-output")
        result = self.adapter.compile_and_run(case_dir, output_dir)
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.stdout, b"")

    def test_negative_diagnostics_output(self):
        """Stub produces diagnostic records on stderr for negative cases."""
        case_dir = os.path.join(
            _TESTS_DIR, "negative", "cases", "18-1-lex-int-overrun"
        )
        output_dir = os.path.join(self.tmpdir, "neg-output")
        result = self.adapter.compile_only(case_dir, output_dir)
        self.assertNotEqual(result.exit_code, 0)
        # stderr should contain JSONL diagnostic records
        lines = [l for l in result.stderr.decode("utf-8").splitlines() if l.strip()]
        self.assertGreater(len(lines), 0)

    def test_smoke_trap_output(self):
        """Stub produces trap output for smoke trap cases."""
        case_dir = os.path.join(
            _TESTS_DIR, "smoke", "cases", "smoke-trap-overflow"
        )
        output_dir = os.path.join(self.tmpdir, "smoke-output")
        result = self.adapter.compile_and_run(case_dir, output_dir)
        self.assertEqual(result.exit_code, 70)
        self.assertIn(b"AIC-R0802", result.stderr)

    def test_deterministic_output(self):
        """Stub produces deterministic output across multiple runs."""
        case_dir = os.path.join(
            _TESTS_DIR, "conformance", "cases", "18-1-var-i32-literal"
        )
        output_a = os.path.join(self.tmpdir, "det-a")
        output_b = os.path.join(self.tmpdir, "det-b")
        result_a = self.adapter.compile_for_determinism(case_dir, output_a)
        result_b = self.adapter.compile_for_determinism(case_dir, output_b)
        self.assertEqual(result_a.stdout, result_b.stdout)
        self.assertEqual(result_a.stderr, result_b.stderr)
        self.assertEqual(result_a.exit_code, result_b.exit_code)


class TestRunnerIntegration(unittest.TestCase):
    """Integration tests running the full runners against the corpus."""

    def setUp(self):
        self.adapter = StubCompilerAdapter()
        self.tmpdir = tempfile.mkdtemp()

    def test_conformance_runner(self):
        """Run conformance suite against existing corpus."""
        suite = run_conformance(
            os.path.join(_TESTS_DIR, "conformance"),
            self.adapter,
            self.tmpdir,
            repo_root=_REPO_ROOT,
        )
        self.assertGreater(suite.total, 0)
        self.assertEqual(suite.failed, 0, f"Failures: {[c.case + ': ' + c.reason for c in suite.cases if not c.passed]}")

    def test_negative_runner(self):
        """Run negative suite against existing corpus."""
        suite = run_negative(
            os.path.join(_TESTS_DIR, "negative"),
            self.adapter,
            self.tmpdir,
            repo_root=_REPO_ROOT,
        )
        self.assertGreater(suite.total, 0)
        self.assertEqual(suite.failed, 0, f"Failures: {[c.case + ': ' + c.reason for c in suite.cases if not c.passed]}")

    def test_smoke_runner(self):
        """Run smoke suite against existing corpus."""
        suite = run_smoke(
            os.path.join(_TESTS_DIR, "smoke"),
            self.adapter,
            self.tmpdir,
            repo_root=_REPO_ROOT,
        )
        self.assertGreater(suite.total, 0)
        self.assertEqual(suite.failed, 0, f"Failures: {[c.case + ': ' + c.reason for c in suite.cases if not c.passed]}")


class TestDeterminismRunner(unittest.TestCase):
    """Test determinism checking."""

    def setUp(self):
        self.adapter = StubCompilerAdapter()
        self.tmpdir = tempfile.mkdtemp()

    def test_determinism_check(self):
        """Verify the stub adapter produces deterministic output."""
        case_dir = os.path.join(
            _TESTS_DIR, "conformance", "cases", "18-1-var-i32-literal"
        )
        result = check_determinism(
            case_dir, self.adapter, self.tmpdir, case_name="test-det"
        )
        self.assertTrue(result.identical, f"Differences: {result.differences}")


class TestIdentityRunner(unittest.TestCase):
    """Test identity checking (M1-ready)."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def test_identity_check_identical(self):
        """Verify identical directories pass identity check."""
        # Create two identical directories
        stage1 = os.path.join(self.tmpdir, "stage1")
        stage2 = os.path.join(self.tmpdir, "stage2")
        os.makedirs(stage1)
        os.makedirs(stage2)

        # Write identical files
        for dirname in (stage1, stage2):
            with open(os.path.join(dirname, "test.obj"), "wb") as f:
                f.write(b"\x00" * 100)
            with open(os.path.join(dirname, "manifest.json"), "w") as f:
                json.dump({"schema_version": 1}, f)

        result = check_identity(stage1, stage2)
        self.assertTrue(result.identical)

    def test_identity_check_different(self):
        """Verify different directories fail identity check."""
        stage1 = os.path.join(self.tmpdir, "stage1")
        stage2 = os.path.join(self.tmpdir, "stage2")
        os.makedirs(stage1)
        os.makedirs(stage2)

        with open(os.path.join(stage1, "test.obj"), "wb") as f:
            f.write(b"\x00" * 100)
        with open(os.path.join(stage2, "test.obj"), "wb") as f:
            f.write(b"\x01" * 100)

        result = check_identity(stage1, stage2)
        self.assertFalse(result.identical)
        self.assertGreater(len(result.differences), 0)


class TestManifestValidator(unittest.TestCase):
    """Test build manifest validation against spec §14.4."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _write_manifest(self, data: dict) -> str:
        path = os.path.join(self.tmpdir, "build-manifest.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        return path

    def test_valid_manifest(self):
        """A well-formed manifest passes validation."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "src",
            "entry_module": "main",
            "module_list": [
                {"name": "main", "source_path": "src/main.ai"},
            ],
            "language_version": "0.1.1",
            "build_options": ["--release", "--target-x86_64"],
            "output_artifacts": {
                "build/main.obj": "a" * 64,
                "build/main.exe": "b" * 64,
            },
            "diagnostic_summary": {"error": 0, "warning": 0},
            "exit_status": 0,
        })
        result = validate_manifest(path)
        self.assertTrue(result.valid, f"Errors: {result.errors}")

    def test_manifest_with_self_hash(self):
        """Manifest with self-referential hash field is rejected."""
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
            "self_hash": "abc123",  # FORBIDDEN
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)
        self.assertTrue(any("self_hash" in e for e in result.errors))

    def test_manifest_with_absolute_path(self):
        """Manifest with absolute path in project_root is rejected."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "C:/Users/test/src",
            "entry_module": "main",
            "module_list": [],
            "language_version": "0.1.1",
            "build_options": [],
            "output_artifacts": {},
            "diagnostic_summary": {},
            "exit_status": 0,
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)
        self.assertTrue(any("project_root" in e for e in result.errors))

    def test_manifest_with_unsorted_options(self):
        """Manifest with unsorted build_options is rejected."""
        path = self._write_manifest({
            "schema_version": "1",
            "project_root": "src",
            "entry_module": "main",
            "module_list": [],
            "language_version": "0.1.1",
            "build_options": ["--target-x86_64", "--release"],  # not sorted
            "output_artifacts": {},
            "diagnostic_summary": {},
            "exit_status": 0,
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)
        self.assertTrue(any("sorted" in e for e in result.errors))

    def test_manifest_missing_required_field(self):
        """Manifest missing required field is rejected."""
        path = self._write_manifest({
            "schema_version": "1",
            # missing project_root, entry_module, etc.
        })
        result = validate_manifest(path)
        self.assertFalse(result.valid)

    def test_manifest_nonexistent_file(self):
        """Non-existent manifest file returns error."""
        result = validate_manifest("/nonexistent/path.json")
        self.assertFalse(result.valid)


class TestReport(unittest.TestCase):
    """Test deterministic report generation."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def test_report_writes_jsonl(self):
        """Report writes sorted JSONL output."""
        report = Report(self.tmpdir)
        report.add_case_result(CaseResult(
            case="z-case", corpus="conformance", passed=True
        ))
        report.add_case_result(CaseResult(
            case="a-case", corpus="conformance", passed=False, reason="fail"
        ))
        path = report.write("results.jsonl")
        self.assertTrue(os.path.isfile(path))

        with open(path, "r", encoding="utf-8") as f:
            lines = [l.strip() for l in f if l.strip()]
        self.assertEqual(len(lines), 2)

        # Verify sorted by case name
        first = json.loads(lines[0])
        second = json.loads(lines[1])
        self.assertEqual(first["case"], "a-case")
        self.assertEqual(second["case"], "z-case")

    def test_report_no_timestamps_or_absolute_paths(self):
        """Report contains no timestamps or absolute host paths."""
        report = Report(self.tmpdir)
        report.add_case_result(CaseResult(
            case="test", corpus="conformance", passed=True
        ))
        path = report.write("results.jsonl")

        with open(path, "r", encoding="utf-8") as f:
            content = f.read()

        # No timestamp patterns (ISO 8601 or epoch)
        self.assertNotIn("2026-", content)
        self.assertNotIn("T00:", content)

        # No Windows absolute paths
        self.assertNotIn("C:\\", content)
        self.assertNotIn("D:\\", content)
        self.assertNotIn("E:\\", content)


class TestJSONLParsing(unittest.TestCase):
    """Test JSONL stderr parsing."""

    def test_parse_empty_stderr(self):
        """Empty stderr produces empty list."""
        records = _parse_jsonl_stderr(b"")
        self.assertEqual(records, [])

    def test_parse_single_record(self):
        """Single JSONL line produces one record."""
        line = json.dumps({"code": "AIC-L0001", "severity": "error"})
        records = _parse_jsonl_stderr(line.encode("utf-8"))
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["code"], "AIC-L0001")

    def test_parse_multiple_records(self):
        """Multiple JSONL lines produce multiple records."""
        lines = [
            json.dumps({"code": "AIC-L0001"}),
            json.dumps({"code": "AIC-L0002"}),
        ]
        records = _parse_jsonl_stderr(("\n".join(lines) + "\n").encode("utf-8"))
        self.assertEqual(len(records), 2)

    def test_parse_skips_non_json(self):
        """Non-JSON lines are skipped."""
        text = "some output\n" + json.dumps({"code": "AIC-L0001"}) + "\n"
        records = _parse_jsonl_stderr(text.encode("utf-8"))
        self.assertEqual(len(records), 1)


if __name__ == "__main__":
    unittest.main()
