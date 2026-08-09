# AI-Co Tests

Verification infrastructure for the AI-Co Stage-0 bootstrap compiler.

## Directory layout

```
tests/
  conformance/    Valid AI-Co programs with expected observable behavior (WP-M0-02)
  negative/       Invalid programs with expected diagnostic records (WP-M0-03)
  smoke/          Representative executable and trap programs (WP-M0-04)
  harness/        Python verification harness (WP-M0-05)
  artifacts/      Harness output — gitignored
```

## Corpus case schema (§1a)

Each corpus case is a directory under `tests/<corpus>/cases/<case-name>/` containing:

- `input.ai` — the AI-Co source program
- `expected.json` — expected result record:
  - **conformance**: `{"kind": "run", "stdout": "<bytes>", "exit_code": N}`
  - **negative**: `{"kind": "diagnostics", "records": [<expected JSONL records>]}`
  - **smoke**: `{"kind": "run", "stdout": "...", "exit_code": N, "stderr_contains": [...], "trap": bool}`
- `meta.json` (optional): `{"spec_ref": "...", "codes": [...], "deferral_reason": "..."}`

A corpus-level `manifest.json` lists all case names with `{"schema_version": 1}`.

## Running the harness

**Language**: Python 3.11.15 (`python`, NOT `python3`).

### Run all suites

```bash
python -m tests.harness all
```

### Run individual suites

```bash
python -m tests.harness conformance
python -m tests.harness negative
python -m tests.harness smoke
```

### Determinism check

```bash
python -m tests.harness determinism
```

### Identity check (M1-ready)

```bash
python -m tests.harness identity --stage1 <dir> --stage2 <dir> [--pe1 <pe>] [--pe2 <pe>]
```

### Validate build manifest (spec §14.4)

```bash
python -m tests.harness manifest <path-to-build-manifest.json>
```

## Running unit tests

```bash
python -m pytest tests/harness/test_harness.py -v
```

## Output

Harness output goes to `tests/artifacts/` (gitignored). Reports use
repository-relative paths, no timestamps, no absolute host paths.
Output format: JSONL (one JSON object per line).

## Independence

The harness verifies independently of the compiler using fixture adapter
output (StubCompilerAdapter). All 148 corpus cases (55 conformance,
68 negative, 25 smoke) pass against the stub adapter, confirming the
harness comparison logic is correct.
