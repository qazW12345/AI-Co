# bootstrap/src/name — WP-M0-10 Name resolution, modules, and imports

Owned area: `bootstrap/src/name/**` and `bootstrap/build/name.txt`
(work-package manifest WP-M0-10). Consumes the parsed AST (WP-M0-09) of the
entry module plus the project root and entry module name supplied by the
driver (WP-M0-19 reads them from the build manifest per spec §14.4) and
produces the module graph (entry + imports with canonical module-to-file
mapping), per-module name tables (module/function/block scopes with
shadowing, the single-name-space rule, struct field / enum member
namespaces), and the name-phase diagnostics `AIC-N0201..N0209` per spec §6
and the diagnostic contract §11.3 / §7.

## Scope

- Scope stack with module scope (order-independent, entire module
  visible per spec §6.1), function parameter scope, and block scopes;
  shadowing of outer declarations by inner ones is permitted.
- Single name space per scope (spec §6.2): struct/enum names share the
  scope with values; same-scope duplicates are `AIC-N0201`.
- Visibility (spec §6.3): top-level declarations are private by default;
  `pub` makes them public; cross-module access to a private item is
  `AIC-N0203`. Struct fields and enum members follow the type's
  visibility (no separate field visibility in the minimal language).
- Module declarations (spec §6.4): the entry module name from the build
  manifest must match the file's `module` declaration (`AIC-N0205`);
  a `module` name with the reserved `rt` prefix is `AIC-N0207`.
- Import resolution (spec §6.5): canonical, cwd-independent mapping
  `a.b.c` -> `<project_root>/a/b/c.ai`; `AIC-N0204` when the module is
  not found at its canonical path; `AIC-N0205` when an imported file's
  `module` declaration does not match its canonical path; import cycles
  are `AIC-N0206` (primary span = the import that closes the cycle, the
  diagnostic names the cycle); importing the same module twice (directly
  or transitively) is not an error and reuses the same module object.
- Reserved runtime modules (spec §6.5): `rt.mem`, `rt.io`, `rt.proc`,
  `rt.trap` bind to compiler-provided modules (no user file is
  consulted); `AIC-N0207` for a user `module` with the `rt` prefix,
  `AIC-N0208` for an import of a reserved `rt` submodule outside the
  runtime surface, `AIC-N0209` for bare `import rt;`. A valid rt
  submodule import registers its Section 15 surface members as
  compiler-provided `NAME_SYM_FN` symbols (`decl == NULL`, public) so
  with-import member references resolve through the name phase (spec
  §15.8: the runtime API is source-visible). Runtime members are not
  auto-available: without the matching import a reference resolves as an
  ordinary undeclared name (`AIC-N0202`).
- Reference map: every resolved identifier/named-type/member-chain node
  maps to its symbol (`NameModule::refs`), so the same fully qualified
  name always resolves to the same declaration within a build (spec
  §6.6, acceptance criterion 4).
- Determinism obligations (spec §14.2): resolution depends only on the
  project root and the entry file; module iteration order is
  deterministic (entry first, then imports in source order, depth-first);
  records are returned sorted with the contract §9 comparator and carry
  phase `name`, severity `error`, recovery `authoritative`; span file
  names are repository-relative canonical paths (never absolute host
  paths).

Explicitly out of scope: type resolution/checking (WP-M0-11), semantic
validation (WP-M0-13), build-manifest/project-root parsing (WP-M0-19
provides the root; this package defines the resolution API contract it
consumes). This package does not modify the spec, ADRs, or other owned
areas.

## API

`bootstrap/src/name/name.h`:

- `name_resolve(project_root, entry_module_name, entry_file, entry_src,
  entry_program, &result, &records, &record_count)` — resolve the entry
  program (a clean `PARSE_OK` tree, `LOAD_OK` source) plus its imports.
  Returns `NAME_OK` / `NAME_DIAG_ERROR` with `*out_result` set (and
  records when non-empty), or `NAME_OOM` with nothing owned.
- `name_result_free` / `name_records_free` — ownership. The result owns
  the imported modules' sources and ASTs; the entry module's source/AST
  are borrowed from the caller.
- Lookup helpers for later packages (e.g. WP-M0-11 types):
  `name_module_by_fqn`, `name_module_lookup`, `name_symbol_for_node`,
  `name_symbol_kind_text`.

## Design decisions

1. **Module-to-file mapping is canonical and cwd-independent.**
   `import a.b.c;` resolves to `<project_root>/a/b/c.ai`; spans carry the
   canonical repository-relative name (`a/b/c.ai`), never an absolute
   path. The project root is used only to build the file path for
   loading; it never appears in any span or record.
2. **Module graph resolution is a depth-first walk in import source
   order.** The entry module is resolved first; each imported module is
   loaded, lexed, and parsed once and reused by FQN (spec §6.5: same
   fully qualified name always denotes the same declaration). Repeated
   imports (diamond) add no duplicate work and no duplicate modules.
3. **Cycle detection names the closing import.** While resolving an
   import, if the target FQN is already on the current DFS path, the
   import that closes the cycle is rejected with `AIC-N0206`. Primary
   span: the import statement in the entry-most cycle member that leads
   into the cycle (corpus-pinned: `import a;` in the entry for
   `main -> a -> main`); secondary spans: the module declarations of the
   remaining cycle members.
4. **Module scope is the entire module.** All top-level declarations are
   registered before any body/type is resolved, so mutual recursion
   works without forward declarations (spec §6.1). Locals must be
   declared before use (point-of-declaration visibility).
5. **Single name space per scope.** Struct/enum names share the scope
   with values. Duplicate declarations in the same scope (module,
   function-parameter, block, struct-field namespace, enum member
   namespace) are `AIC-N0201`. Struct fields and enum members live in the
   type's own namespace and do not leak into enclosing scopes; they are
   accessed only through `.`/`->` (fields) or `EnumType.Member`
   (enum members), and bare use of an enum member name is an undeclared
   name (`AIC-N0202`).
6. **Span choices follow the accepted negative corpus exactly.** N0201
   primary = the later declaration's identifier, secondary = the earlier
   declaration's identifier; N0203 primary = the reference (member
   chain), secondary = the private declaration's head (keyword through
   identifier); N0204/N0208/N0209 primary = the import's qualified
   name; N0205/N0207 primary = the whole module declaration; N0206
   primary = the closing import statement, secondary = module
   declarations of the remaining cycle members. (Verified against the
   committed `expected.json` fixtures.)
7. **rt.\* rules match spec §6.5 exactly.** A user `module` declaration
   with the reserved `rt` prefix is `AIC-N0207`. An import of a reserved
   `rt` submodule outside the runtime surface (the four Section 15
   submodules `rt.mem`, `rt.io`, `rt.proc`, `rt.trap`) is `AIC-N0208`.
   Bare `import rt;` is `AIC-N0209`. A valid rt submodule import binds a
   compiler-provided module and registers its Section 15 surface members
   as compiler-provided `NAME_SYM_FN` symbols (`decl == NULL`, public):
   with the matching import, `rt.mem.alloc_bytes` etc. resolve through
   the name phase (spec §15.8: the runtime API is source-visible, and the
   WP-M0-11 types contract treats a function symbol with no decl as a
   runtime built-in whose signature is not in the build). Runtime members
   are not auto-available: a reference to a reserved runtime name without
   the matching import resolves as an ordinary undeclared name
   (`AIC-N0202`, primary span = the reference). No file under
   `<project_root>/rt/` is ever consulted for a reserved import; a valid
   runtime submodule import binds to a compiler-provided module with no
   user source.
8. **Records are collected and sorted before return.** Every name record
   is phase `name`, severity `error`, recovery `authoritative`; records
   are sorted with the contract §9 comparator (`diag_sort_records`).
   Load/lex/parse diagnostics from imported files are propagated into
   the returned record stream (they already carry their own phase and
   recovery marks; the contract comparator orders them deterministically
   with the name records).
9. **Name phase is a checker, not a transformer.** Resolution does not
   mutate the AST; it records `refs` (node -> symbol) per module. On
   `NAME_DIAG_ERROR` the result and records are still produced so the
   driver can report all findings; a caller that needs a clean tree must
   check `NAME_OK` first (mirrors the parser's authoritative/derived
   recovery contract).
10. **OOM is terminal and clean.** On allocation failure the resolver
    returns `NAME_OOM` with nothing owned (partial records and symbols
    are freed). No host path, environment, registry, or network access
    is ever performed.

## Diagnostics emitted (contract §11.3; all phase `name`, severity
`error`, recovery `authoritative`)

| Code | Condition | Primary span |
|---|---|---|
| AIC-N0201 | duplicate declaration of the same name in the same scope | the later declaration's identifier (secondary: the earlier identifier) |
| AIC-N0202 | use of an undeclared name (incl. reserved runtime names without the matching import, and bare enum members) | the reference identifier |
| AIC-N0203 | access to a private item from another module | the reference (member chain) |
| AIC-N0204 | imported module not found at its canonical path | the import's qualified name |
| AIC-N0205 | module declaration does not match its canonical path name (entry or import) | the module declaration |
| AIC-N0206 | import cycle | the import that closes the cycle |
| AIC-N0207 | module declaration uses the reserved `rt` prefix | the module declaration |
| AIC-N0208 | import of a reserved rt submodule outside the runtime surface | the import's qualified name |
| AIC-N0209 | bare `import rt;` | the import's qualified name |

## Build and test

The area source list is aggregated by the stage-0 entry points via
`bootstrap/build/name.txt`. `name_test.c` is intentionally not in the
fragment (it has its own `main()`); unit tests build and run from the
repository root:

```sh
STAGE0_OUT_DIR='bootstrap\stage0\msvc-name' ./bootstrap/build/build-stage0-msvc.cmd \
    bootstrap/src/name/name_test.c bootstrap/src/name/name.c \
    bootstrap/src/ast/ast.c bootstrap/src/parse/parse.c \
    bootstrap/src/lex/lex.c bootstrap/src/load/load.c \
    bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
    bootstrap/src/diag/diag_emit.c
./bootstrap/stage0/msvc-name/name_test.exe
```

Repeat with `build-stage0-clang.cmd` / `bootstrap\stage0\clang-name` for the
Clang build. Expected: `name_test: N checks, 0 failures`, exit 0.

`name_test.c` covers:

- Scopes and shadowing: inner declarations may shadow outer ones;
  module scope is order-independent (mutual recursion); block/param
  shadowing is permitted; same-scope duplicates are `AIC-N0201` with the
  later declaration's identifier as primary span and the earlier
  declaration's identifier as secondary span.
- Single name space: a struct and a fn of the same name in the same
  scope are a duplicate; duplicate locals in the same block are
  `AIC-N0201`.
- Undeclared names: `AIC-N0202` with exact spans; bare enum members are
  not injected into the enclosing scope; enum member duplicates are
  `AIC-N0201`.
- Module declaration rules: `AIC-N0207` for `module rt.foo;`,
  `AIC-N0205` when the entry module name does not match the manifest.
- rt.\* rules: `AIC-N0209` for bare `import rt;`, `AIC-N0208` for
  `import rt.internal;`, successful binding of `import rt.mem;` (module
  registered with `is_runtime`, `path == NULL`), `AIC-N0202` when a
  runtime name is referenced without the matching import, and positive
  with-import member resolution: `import rt.mem; rt.mem.alloc_bytes(16)`
  resolves to a compiler-provided `NAME_SYM_FN` symbol (`decl == NULL`,
  public) with no record emitted; the full Section 15 member lists
  (`rt.mem` 4, `rt.io` 7, `rt.proc` 2, `rt.trap` 1) are registered and
  references to `rt.io.stdout`, `rt.proc.args`, `rt.trap.report` resolve.
- Integration (multi-module fixtures): private-access `AIC-N0203` with
  cross-module secondary span, `AIC-N0204` not-found, `AIC-N0206` cycle
  (naming the cycle via secondary spans), diamond import reuse
  (same module object, no duplicate), module-qualified type and value
  references through an imported module (`a.b.Point`, `a.b.f()`).
- Same-FQN identity (criterion 4): two references to the same fully
  qualified name (unqualified and module-qualified) resolve to the same
  symbol.
- Determinism (spec §14.2): resolving the same program twice yields
  byte-identical sorted JSONL record streams.
- Re-execution of the name-owned negative-corpus anchors against the
  real fixture files under `tests/negative/cases/` (read-only; owned by
  WP-M0-03): `18-3-name-private-access`,
  `derived-name-undeclared`, `derived-name-duplicate-decl`,
  `derived-name-import-not-found`, `derived-name-module-mismatch`,
  `derived-name-import-cycle`, `derived-name-module-rt-prefix`,
  `derived-name-import-reserved-rt-submodule`,
  `derived-name-bare-import-rt` — each asserts the exact code, message,
  span, and secondary count from the fixture `expected.json` files.

## Corpus notes

The name-owned negative-corpus anchors are the nine fixtures listed
above. Each expects exactly one record; `name_test.c` asserts the exact
codes, messages, phases (`name`), recovery marks (`authoritative`), and
spans from the fixture `expected.json` files. The `18-3-name-private-access`
fixture's `a/b.ai` is at the canonical path for `import a.b;`, exercising
the canonical module-to-file mapping with a multi-segment module.

## Ownership

Owned by WP-M0-10: `bootstrap/src/name/**`, `bootstrap/build/name.txt`.
Consumers include `name.h` but do not modify this area. The resolution
API contract (this header) is consumed by the WP-M0-19 build driver,
which supplies `project_root` and `entry_module_name` from the build
manifest. WP-M0-11 (types) is the next consumer. A needed change to the
name package's public contract is a downstream gap → Planner via the
Coordinator per milestone plan §7.
