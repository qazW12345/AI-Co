# bootstrap/src/types — WP-M0-11a Type identity and type tables

Owned area: `bootstrap/src/types/type_identity.*`, `bootstrap/src/types/type_tables.*`
and `bootstrap/build/types_a.txt` (work-package manifest WP-M0-11a). Consumes the
resolved name tables (WP-M0-10) and the parsed AST (WP-M0-09) and implements
type representation, structural type identity (spec §7.3), the primitive and
composite type tables (spec §7.1–7.2), and the completeness rules (spec §7.6)
with `AIC-T0302`/`AIC-T0303`.

Explicitly out of scope: layout computation (WP-M0-11b), implicit conversions
and common type (WP-M0-11c), explicit cast/wrap matrix and operator typing
(WP-M0-11d). This package does not modify the spec, ADRs, or other owned
areas.

## Scope

- `type_identity.h/.c` — the `Type` descriptor model (primitive / array /
  slice / pointer / named struct / named enum) and the structural identity
  function `type_identical` (spec §7.3): same primitive; same named
  struct/enum declaration (same `NameSymbol`, which the name package
  guarantees is unique per declaration within a build); composites with
  identical element type and identical extent (`T[N]` vs `T[N]`, `T[]` vs
  `T[]`, `T*` vs `T*`). There are no anonymous struct/enum types and no type
  aliases in the minimal language, so no extra identity machinery is needed.
- `type_tables.h/.c` — the normative catalogs:
  - the thirteen primitives of §7.1 with the spec's recorded
    size/alignment facts (`types_prim_info`, `types_prim_by_name`);
  - the five composite forms of §7.2 (`types_composite_info`);
  - the completeness pass `types_check_completeness` (spec §7.6).

## API

`bootstrap/src/types/type_tables.h`:

- `types_prim_info(kind)` / `types_prim_by_name(name)` / `types_prim_count()`
  — §7.1 table rows (name, integer/signed/pointer-sized flags, width bits,
  spec-recorded size/alignment).
- `types_composite_info(kind)` / `types_composite_count()` — §7.2 table rows
  (name, form, recorded layout note).
- `types_check_completeness(result, &records, &record_count)` — runs the
  §7.6 completeness pass over the resolved build. Returns
  `TYPE_CHECK_OK` / `TYPE_CHECK_DIAG_ERROR` with `*out_records` set (and
  records when non-empty), or `TYPE_CHECK_OOM` with nothing owned. Records
  are sorted with the contract §9 comparator and carry phase `type`,
  severity `error`, recovery `authoritative`.
- `types_records_free(records, count)` — ownership for the returned records.

`bootstrap/src/types/type_identity.h`:

- `type_prim_new` / `type_array_new` / `type_slice_new` / `type_ptr_new` /
  `type_struct_new` / `type_enum_new` — constructors. Composite constructors
  take ownership of their element `Type` (transfer); the `NameSymbol` for
  struct/enum is borrowed.
- `type_free` — recursive release (never frees borrowed symbols).
- `type_identical(a, b)` — structural identity per §7.3.
- `type_describe(t)` — deterministic rendering for messages
  (`"i32"`, `"u8[4]"`, `"Node*"`, `"Node"`).
- `type_kind_text(kind)` — kind label.

## Design decisions

1. **Named identity is declaration identity, not name identity.** The type
   package never compares struct/enum types by spelling; it stores the
   `NameSymbol` from name resolution and compares pointers (spec §7.3: same
   declaration). Same-FQN-same-declaration is a name-package guarantee
   (WP-M0-10 decision 2), so pointer equality is exact. A unit test builds
   `Pair { a: Point; b: Point; }` and asserts both field type references
   resolve to the same symbol and therefore the same type.
2. **The §7.1 size/alignment columns are recorded as table data.** The
   primitive table transcribes the accepted spec facts (e.g. `str` 16/8,
   `isize` 8/8). This is data, not layout computation: this package never
   derives a composite size or offset, and `sizeof`/`alignof` are not
   provided here (WP-M0-11b owns layout; §12/§10.5 consumers use it).
3. **Completeness processes struct declarations in deterministic order.**
   Modules iterate in `NameResult` order (entry first, then imports
   depth-first); within a module, top-level declarations in source order. A
   struct becomes complete when its own declaration has been processed
   (spec §7.6: "incomplete until its closing brace"). A field whose type
   reaches a *different* struct that has not yet been processed is
   `AIC-T0302` (forming a field of an incomplete struct type); pointers
   stop the walk (`S*` is permitted). Direct by-value self-recursion is
   detected as infinite size.
4. **Corpus-pinned selection between T0302 and T0303.** The accepted
   negative corpus
   (`tests/negative/cases/derived-type-incomplete-struct-use`,
   `derived-type-struct-recursion`) pins: a self-recursive struct that is
   *also used as a value* anywhere in the build (variable/constant
   declaration, parameter, return type, struct literal, array/slice of the
   struct at a value position) is reported `AIC-T0302` ("use of incomplete
   struct type 'S' as a value"); a self-recursive struct that is never used
   as a value is reported `AIC-T0303` ("struct 'S' has infinite size due to
   recursive by-value field 'f'"). Both use the struct's declaration-name
   span as primary span, exactly as the fixtures pin. The same convention
   (declaration-name span) is applied to the unpinned forward-reference
   case.
5. **One record per broken struct.** A symbol that already received a
   completeness record is not reported again for the same root cause
   (contract §13: a code at most once per root cause). In particular, a
   struct that is both self-recursive and forward-referenced reports once.
6. **Records are emitted through the WP-M0-06 diag package** with the
   registry codes `AIC-T0302`/`AIC-T0303` (already allocated in the
   contract registry and in `diag_codes.c`; this package does not modify
   the registry) and are sorted with the contract §9 comparator before
   return (determinism, spec §14.2).

## Verification

- `types_test.c` (14 test functions, ~90 checks) covers: primitive and
  composite table contents vs §7.1–7.2; primitive/composite/named identity
  incl. same-declaration-through-name-resolution; `type_describe`; the
  §7.6 completeness rules (pointer recursion OK, by-value recursion
  `AIC-T0303`, incomplete-use `AIC-T0302` incl. value uses through arrays);
  determinism (byte-identical sorted JSONL streams); and re-execution of
  the two type-owned negative-corpus anchors with exact spans.
- Build and run per the header of `types_test.c` with both accepted host
  compilers (MSVC and LLVM Clang).

## Residual risk

- Mutual by-value recursion (`struct A { b: B; } struct B { a: A; }`) is
  rejected as `AIC-T0302` (B is incomplete when A's field is formed); it is
  not additionally classified as `AIC-T0303` (infinite size) because the
  completeness pass detects direct self-recursion only, matching the
  corpus-pinned cases. If the Planner intends transitive value-recursion to
  be classified `AIC-T0303`, this is a small extension of the field walk
  and returns to the Planner via the Coordinator.
- The forward-reference span choice (referenced struct's declaration name)
  is unpinned by the corpus; it follows the corpus convention for the
  pinned cases.
