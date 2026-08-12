# AI-Co Canonical IR Contract v0.1.0 (Proposed draft — WP-M0-16a)

**Status:** Proposed (draft v0.1.0); pending Main Designer architecture review (WP-M0-16a gate)
**Owner:** WP-M0-16a (`docs/contracts/` area; manifest §2 file-ownership matrix)
**Decision owner:** Main Designer (architecture acceptance of the IR instruction set; spec §14.1(6) delegates the instruction set to the implementation planning phase, milestone plan §9)
**Version:** 0.1.0 (draft)
**Date:** 2026-08-12
**Scope:** project AI-Co; tenant ai-co
**Companion to:** `spec/AI-CO-LANGUAGE-SPECIFICATION.md` (v0.1.6, Accepted); `spec/DIAGNOSTIC-CONTRACT.md` (v0.1.1, Accepted)

This document defines the canonical, target-neutral intermediate representation (IR) of the AI-Co Stage-0 compiler: its structure, node kinds (instruction set), determinism obligations, span and causal-chain preservation obligations, and the representation-coverage requirement of spec §14.1(6) — that every semantic rule in the accepted language specification be representable and enforceable in the IR.

Governing sources (all Accepted unless noted):

1. `spec/AI-CO-LANGUAGE-SPECIFICATION.md` §14.1(6) (the IR boundary), §14.2 (determinism obligations), §15 (runtime and trap contract), §16.2 (deterministic equivalence / no normalization).
2. `docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md` stage 6 (canonical, target-neutral intermediate representation; bounded input/output contract per stage; diagnostics retain causal and source-location information across stages), stage 7 (deterministic x86-64 backend), stage 8 (COFF emission), and the no-optimization policy (optimizations deferred).
3. `docs/adr/ADR-002-minimal-core-language-semantics.md` (minimal semantic direction the IR must serve).
4. `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (temporal baseline, wrapping baseline, Windows baseline — all reflected in the spec text this contract implements).
5. `spec/DIAGNOSTIC-CONTRACT.md` (phase `"ir"`, code `AIC-I0501`, span object §6, causal-chain field `causes` §4, deterministic ordering §9, trap-record contract §10).
6. Work-package manifest §2/§3 (WP-M0-16a objective, scope, exclusions, acceptance criteria) and milestone plan §9 (the only delegated design item is the IR instruction set, handled by WP-M0-16 with a Main Designer review gate).

This document is a **contract**, not an implementation: WP-M0-16b implements the node model and invariants, WP-M0-16c implements the typed-AST → IR builder, and WP-M0-17 consumes the IR for deterministic x86-64 code generation. Where this draft records an interpretation or a design choice that the accepted sources leave open, the item is explicitly flagged (IRC-Nn) for Main Designer confirmation at the acceptance gate; a flagged item is not a spec change and takes effect only on acceptance.

---

## 1. Purpose and boundary

### 1.1 Purpose

The IR is the single semantic lowering target of the compiler. It sits between semantic validation (pipeline stage 5) and the x86-64 backend (pipeline stage 7) per ADR-001 stage 6. Its purpose is:

1. to **represent** every accepted program after semantic validation in one canonical form, carrying the source spans and causal chains required for diagnostics and trap records;
2. to make the language's **enforcement obligations explicit** (checked operations, bounds checks, null/alignment checks, no-fall-through, terminators) so the backend cannot silently weaken them;
3. to be **deterministic**: identical source files plus identical build options yield byte-identical IR (observable through the deterministic dump, WP-M0-16b2);
4. to be **target-neutral**: no x86-64 register, instruction-selection, calling-convention, or code-layout choice appears in the IR.

### 1.2 Boundary

In scope (this contract must define):

- IR structure: module units, functions, blocks, statements, terminators, expressions, types, constants, storage;
- the closed node-kind set (the instruction set) and each node kind's operands, type, evaluation order, span/cause obligations, and (where applicable) trap code;
- determinism rules for construction, ordering, interning, and dumping;
- target-neutrality rules: what may and may not appear in the IR;
- span and causal-chain preservation rules;
- the representation-coverage matrix (Section 9) mapping every semantic-rule family of the spec to IR representation and enforcement;
- IR well-formedness invariants (violations reported `AIC-I0501`, phase `"ir"`);
- the deterministic-dump and verification obligations of WP-M0-16b2.

Out of scope (owned by adjacent packages or explicitly deferred):

- the IR implementation (WP-M0-16b/c), the backend (WP-M0-17), COFF emission (WP-M0-18), the driver (WP-M0-19);
- optimization passes: explicitly deferred per ADR-001; the IR is a lowering target, not an optimization IR. No pass that transforms the IR may be added without an accepted architecture change;
- any change to the public language contract: the IR is internal (manifest §WP-M0-16a Exclusions);
- the exact textual grammar of the deterministic dump: WP-M0-16b2 implements it; this contract fixes what the dump must make observable (Section 11).

### 1.3 Relation to the pipeline

Inputs to the IR builder (WP-M0-16c): the resolved, semantically validated build produced by WP-M0-09/10/11/12/13 — the parse tree (AST with spans, WP-M0-09), the resolved name tables (WP-M0-10), the resolved type representation and layout facts (WP-M0-11), constant-evaluated values and failure records (WP-M0-12), and the semantic-validation results (WP-M0-13). The driver stops before the IR stage when any diagnostic exists (AST design rule: a tree produced by a parse with diagnostics must not be processed downstream; same rule at every stage boundary). The IR is therefore built **only for accepted (valid) programs**; no IR node represents a rejected construct, and no IR-level recovery exists.

Outputs of the IR: (a) the IR graph consumed by the x86-64 backend (WP-M0-17); (b) the deterministic dump (WP-M0-16b2) used by verification and, later, by M1 comparison evidence (spec §16.2: identical inputs → byte-identical artifacts; the dump is a compiler-produced artifact subject to the same no-normalization rule).

Diagnostics: the IR phase participates in the diagnostic ordering (DIAGNOSTIC-CONTRACT §4 phase values include `"ir"`; §9 phase order places `ir` after `semantic` and before `backend`). `AIC-I0501` is the only IR-class diagnostic; it signals an IR invariant violation (compiler internal), never a language rejection.

---

## 2. Normative vocabulary

Terms used in this document are normative in addition to the spec's §2 terms.

- **IR**: the canonical intermediate representation defined by this contract.
- **IR node**: one element of the IR graph (a declaration, statement, terminator, expression, type descriptor, or constant).
- **Node kind**: the closed set of node kinds (the instruction set), Section 5.5.
- **Node id**: the deterministic, construction-order identifier of a node within the whole build; the only node identity the IR uses.
- **Canonical order**: the deterministic total order in which nodes are constructed, dumped, and iterated (Section 6).
- **Storage location (lvalue)**: an IR node denoting a memory location: a local slot, parameter slot, global slot, field address, element address, or dereference result.
- **Value category**: scalar vs. composite (address-resident) as defined in Section 5.4.
- **Primary span**: the source span (DIAGNOSTIC-CONTRACT §6 object) of the smallest source construct a node derives from.
- **Cause chain**: the ordered, parent-linked chain of IR nodes from a node to its module root, with each link carrying the source construct kind and primary span of the construct it derives from (Section 8).
- **Causal chain** (spec §14.1(6) wording): the source-level causal chain of each construct, preserved by the IR as the cause chain plus resolved-reference facts.
- **Enforcement obligation**: a semantic rule that the IR makes explicit so the backend must implement it (e.g., a checked op's trap code, a bounds check, a terminator requirement).
- **Representable**: a semantic rule is representable if every program that satisfies it (an accepted program exercising that rule) has an IR form.
- **Enforceable**: a semantic rule is enforceable in the IR if the IR structure or node attributes make a violation impossible (structural) or detectable as an invariant violation (`AIC-I0501`), or if the rule's runtime obligations are attached to IR nodes the backend must implement (e.g., a declared trap code).
- **Trap code**: a stable `AIC-Rxxxx` / `AIC-Uxxxx` code from the diagnostic contract that a failing IR operation must raise.
- **IR invariant**: a well-formedness property of the IR graph; a violation is a compiler internal error reported as `AIC-I0501` with a derived span (DIAGNOSTIC-CONTRACT §11.6).

---

## 3. Design principles

The IR design follows these principles; each is derived from an accepted source.

1. **One canonical form per accepted program.** The IR is the single semantic form between validation and codegen (ADR-001 stage 6). There is exactly one IR for each accepted program given identical inputs and options (spec §14.1(6) determinism).
2. **Structured, not graph-optimized.** The minimal language is fully structured (no `goto`, no labels; spec §10.3, §17.1). The IR is a structured tree of blocks, statements, terminators, and expressions with explicit, deterministic evaluation order. Optimizations are deferred (ADR-001), so a CFG's transformation benefits are not needed; a structured IR preserves source structure, spans, and causal chains directly.
3. **Enforcement by construction.** The spec's structural rules (no fall-through, terminator requirements, break/continue placement, lvalue-only stores) are encoded in the IR shape so a violating IR cannot be constructed; remaining well-formedness rules are invariants checked at build time (`AIC-I0501`). Runtime obligations (checked arithmetic, bounds, null, alignment) are attached to the IR nodes that must perform them as declared trap codes, so the backend cannot omit them without violating the contract.
4. **Determinism is observable.** The deterministic dump (WP-M0-16b2) makes the IR byte-observable; two builds of identical inputs must produce byte-identical dumps (spec §14.2, §16.2 no-normalization).
5. **Target-neutrality by exclusion.** The IR carries language facts (types, spec-fixed layout, trap codes, evaluation order, runtime function identities). It never carries implementation facts (registers, stack, calling conventions, instruction selection, code layout, PE/COFF details, symbol mangling).
6. **Diagnostics cross stages structurally.** IR nodes carry structured span + cause + type/value facts; diagnostics and trap records are never reconstructed from plain strings (ADR-001 §54).
7. **No architecture-by-specialist.** Where the accepted sources leave a genuine choice, this draft makes a concrete proposal and flags it (IRC-Nn) for the Main Designer gate rather than silently extending the architecture.

---

## 4. IR structure

### 4.1 Whole-build unit graph

The IR covers one build (spec §2.2 Program = set of source files + build manifest). It is a graph of **module units**:

- `IR_MODULE` — one per source module, in canonical order (entry module first, then imports depth-first in import order; the same order the build-level walkers use, e.g. `expr_core.h`'s documented visit order). Carries the module's fully qualified name, its import list (each an `IR_IMPORT` with span), and its top-level declarations in source order.
- `IR_IMPORT` — an import reference (module name, span, cause). Imports are retained so cross-module reference edges and diagnostic contexts stay source-faithful; resolution itself is complete before the IR (WP-M0-10).
- Cross-module references (calls to imported functions, uses of imported types/consts/globals/enum members) are resolved edges to the target IR node. The same fully qualified name always denotes the same IR node within a build (spec §6.6).

### 4.2 Declarations

Top-level declarations in source order within their module:

- `IR_STRUCT_DECL` — struct declaration: name, fully qualified name, ordered field list (name, type, span), computed size/alignment and per-field byte offsets (from the layout package, WP-M0-11b, which encodes spec §7.4). Field order is declaration order; no reordering is representable.
- `IR_ENUM_DECL` — enum declaration: name, underlying integer type, member list (name, value) in declaration order (spec §7.5; continuation values resolved by the builder).
- `IR_GLOBAL_CONST` — module-scope `const`: name, type, constant value (`IRConst`), span, cause. A const has **no storage location** (spec §8.1/§8.3): the IR has no addressable form for it, which makes `&const` unrepresentable (the rejection `AIC-E0402` is enforced pre-IR and is structurally unreachable in the IR).
- `IR_GLOBAL_VAR` — module-scope `var`: name, type, static storage, constant initializer (`IRConst`; accepted programs always carry one per §8.2/`AIC-E0403`), span, cause.
- `IR_FUNCTION` — function: fully qualified name, parameter list (name, type, slot order), return type (may be `void`), body block, span, cause, and a `noreturn` flag on the two known noreturn runtime functions only (`rt.proc.exit`, `rt.trap.report`; spec §13.5/§15.7). The flag marks calls to those functions as terminators (`IR_CALL_TERM`); a user function whose body ends in a noreturn call is **not** itself marked noreturn — spec §13.5 treats only calls to `rt.proc.exit`/`rt.trap.report` as terminators, and callers of a user function may not assume it never returns. Recursion and mutual recursion are ordinary cyclic reference edges (spec §13.4).

### 4.3 Storage model

The IR models storage explicitly and deterministically:

- **Static storage** (`IR_GLOBAL_VAR`): one slot per global; initialized from its constant initializer before program entry (spec §8.3).
- **Automatic storage**: per-function slot table. Parameter slots first (in parameter order), then local slots assigned by the builder in first-declaration order (source order). Each slot has a type and a span. Local initialization is a `IR_LOCAL_DECL` statement executed at the declaration point (spec §9.2: locals re-initialized on each execution of the declaration).
- **Compiler temporaries**: where lowering requires an anonymous object image (struct/array literal evaluation, aggregate argument passing, `?:` / short-circuit result staging if the backend needs it), the builder allocates a temporary slot. Temporaries are part of the slot table with deterministic ids; they are never visible to source semantics and never alter evaluation order.
- **No storage for consts** (Section 4.2). Allocated storage via `rt.mem` is a runtime call result (`IR_CALL`), not an IR storage kind (spec §8.3).

### 4.4 Type representation

The IR type descriptor mirrors the resolved `Type` model (WP-M0-11a) plus the layout facts (WP-M0-11b):

| IR type | Source (spec) | Facts carried |
|---|---|---|
| `void` | §7.1 | function return type only; no value of this type is representable |
| `bool` | §7.1 | size 1, align 1 |
| `i8 i16 i32 i64 u8 u16 u32 u64 isize usize` | §7.1 | width bits, signedness, size, align (spec table; isize/usize fixed 64-bit for the initial target) |
| `str` | §7.1/§7.2 | size 16, align 8; (data, length) pair |
| `T[N]` | §7.2 | element type, extent `N` (non-negative constant), size `N*sizeof(T)`, align `alignof(T)` |
| `T[]` | §7.2 | element type, size 16, align 8; (data, length) pair |
| `T*` | §7.2 | element type, size 8, align 8; nullable |
| struct `S` | §7.4 | declaration ref, field table with byte offsets, size, align |
| enum `E: T` | §7.5 | declaration ref, underlying integer type |

Type identity is the resolved identity (spec §7.3: structural for composites, same-declaration for named types). The IR represents a type by a canonical descriptor; identical types share one descriptor (interning, Section 6.3). The layout facts are the spec's fixed facts for the initial-target contract — language facts, not x86-64 implementation choices (Section 7).

### 4.5 Constant representation

`IRConst` is the closed set of compile-time constant values the IR may carry (all resolved by the constant evaluator, WP-M0-12, and by semantic validation):

- `IRConst_INT` — integer constant: type + value as the exact bit pattern (two's complement; the type's signedness defines interpretation).
- `IRConst_BOOL` — `true`/`false`.
- `IRConst_NULL` — the null pointer value (spec §11.1).
- `IRConst_STR` — string constant: byte sequence + length (UTF-8, spec §4.4); materialized as read-only data by the backend.
- `IRConst_ENUM` — enum member constant: underlying integer value + enum type ref.
- `IRConst_STRUCT` — struct constant: field values in declaration order; padding bytes are zero (spec §7.4/§9.4).
- `IRConst_ARRAY` — array constant: element values in index order (spec §12.1).
- `IRConst_ADDR` — address constant: reference to a static-storage lvalue (`IR_GLOBAL_VAR`, or `&arr[0]` of a static array) plus a byte offset; used for the constant-expression `&` and slice-of-static forms (spec §10.5). The target must be a static-storage lvalue; consts have no address (spec §8.1) so `IRConst_ADDR` never references them.

`sizeof`/`alignof` results are `IRConst_INT` values of type `usize` computed by the builder from the layout package; the constant node carries the span and cause of the `sizeof`/`alignof` expression (Section 8).

---

## 5. Node kinds (instruction set)

This section defines the closed set of IR node kinds. Every node kind lists its children, its result type (where it is a value-producing node), its evaluation order, its span/cause obligation, and its enforcement obligations (trap codes). "Eval order" is total for every node: the language has no unspecified evaluation order anywhere (spec §10.4).

### 5.1 Declarations and module structure

Covered in Section 4.1–4.2 (`IR_MODULE`, `IR_IMPORT`, `IR_STRUCT_DECL`, `IR_ENUM_DECL`, `IR_GLOBAL_CONST`, `IR_GLOBAL_VAR`, `IR_FUNCTION`).

### 5.2 Statements

| Node | Children | Eval order | Notes / enforcement |
|---|---|---|---|
| `IR_BLOCK` | statements (ordered) | in order | introduces a block scope; locals declared inside are scoped to it (spec §13.1). A block that is not a case body or a function tail may fall through to the following statement. |
| `IR_LOCAL_DECL` | slot ref, initializer value | initializer evaluated at the declaration execution point (spec §9.2) | declares one local slot with a value (no uninitialized state; spec §8.2). For composite initializers the value is address-resident (Section 5.4). |
| `IR_IF` | condition (bool type), then-block, optional else-block | condition, then exactly one of then/else | `IR_IF` mirrors `if`/`else if` chains structurally. |
| `IR_WHILE` | condition (bool), body-block | condition; body repeatedly while true | `continue` targets the condition evaluation; `break` exits (spec §13.3). |
| `IR_FOR` | optional init statement, optional condition (bool), optional step expression, body-block | init once; then condition; body; step; repeat | init declarations are scoped to the for statement (spec §13.3). Absent condition = `true`. `continue` targets the step; absent step, the condition (spec §13.3). |
| `IR_SWITCH` | selector expression (integer or enum), case list, optional default | selector once, left-to-right (spec §13.2) | cases matched in source order, then default (spec §13.2); no fall-through is structurally possible (Section 5.6). |
| `IR_CASE` | case constant value (selector type), body-block | — | case values are resolved constants; duplicate case values are rejected pre-IR (`AIC-E0413`) and are structurally absent. |
| `IR_DEFAULT` | body-block | — | at most one per switch (duplicate `default` rejected pre-IR, `AIC-E0420`). Position is semantically irrelevant (spec §13.2). |
| `IR_BREAK` | target ref | — | terminator (Section 5.6); target is an enclosing `IR_SWITCH` or `IR_WHILE`/`IR_FOR`; placement invariant (Section 10). |
| `IR_CONTINUE` | target ref | — | terminator; target is an enclosing `IR_WHILE`/`IR_FOR` only (spec §13.2: `continue` inside a switch inside a loop continues the loop). |
| `IR_RETURN` | optional value expression | value evaluated at the point of return | terminator; value type must equal the function return type (or absent for `void`); non-`void` tails must end in a terminator (Section 5.6). |
| `IR_EXPR_STMT` | expression | expression evaluated for effect | result unused (spec §5.2 expr_stmt). |
| `IR_EMPTY` | — | — | the empty statement `;` (span preserved). |
| `IR_CALL_TERM` | callee ref, arguments (ordered) | arguments left-to-right | terminator; a call to a noreturn function (`rt.proc.exit`, `rt.trap.report`), which never returns (spec §15.7). |
| `IR_TRAP` | trap code, facts | — | terminator; unconditional deterministic trap raise with the stable trap code, the failing operation's span, and the relevant type/value facts (spec §15.8, DIAGNOSTIC-CONTRACT §10). The backend lowers `IR_TRAP` to the trap-report path (call to `rt.trap.report` with code, span, and facts, or the accepted equivalent that produces the required trap record; exit code 70, spec §15.5). |

### 5.3 Value-producing nodes (instructions)

Scalar results are direct values; composite results are address-resident (Section 5.4). Every value node carries its result type.

| Node | Operands | Result type | Eval order | Enforcement |
|---|---|---|---|---|
| `IR_INT` | constant | integer type | — | integer constant value. |
| `IR_BOOL` | constant | `bool` | — | boolean constant. |
| `IR_NULL` | — | `T*` | — | null pointer constant (spec §11.1). |
| `IR_STR` | string constant | `str` | — | string literal; materialized as read-only data by the backend. |
| `IR_ENUM_VAL` | enum constant | enum type | — | enum member value. |
| `IR_LOCAL` | slot ref | slot's type | — | lvalue: local/parameter slot. |
| `IR_GLOBAL` | global ref | global's type | — | lvalue: static global slot. |
| `IR_FIELD_ADDR` | lvalue base, field | `U*` where field type is `U` | base first | lvalue: address of the field at its declared byte offset (spec §12.6). Result is an address; a scalar read uses `IR_LOAD`. |
| `IR_INDEX_ADDR` | base (array/slice lvalue or `str` value), index (usize) | `T*` (element type `T`) | base, then index (spec §10.4: `a[i]` evaluates `a` then `i`) | bounds check `0 <= i < extent`; failure → trap `AIC-R0807` (constant out-of-range rejected pre-IR `AIC-E0409`). For `str`, element type is `u8` (byte offset, spec §12.2) and the result is a **value address, never an lvalue** — `str` is immutable and indexing it yields a byte value (spec §12.2; store target is unrepresentable). Result is an lvalue only when the base is a mutable array/slice lvalue. |
| `IR_DEREF` | pointer (checked) | `T` | operand first | dereference of `T*`; null → trap `AIC-R0809`; invalid address/alignment (where detectable) → trap `AIC-R0811` (spec §12.5/§12.8). Result is an lvalue of type `T`. |
| `IR_LOAD` | lvalue | scalar type | operand first | loads a scalar value from an lvalue (used where a value, not an address, is required). A `bool`-typed load from memory carries the `AIC-R0805` obligation (a non-0/non-1 byte read as `bool` traps; spec §9.1). |
| `IR_STORE` | destination lvalue, value | `void` (effect) | destination location first, then value (spec §10.4 assignment order) | stores into a mutable lvalue. Scalar: stores the value bytes. Composite: copies the **complete object representation** (fields and padding; spec §9.1/§9.3/§9.4) from the source object image to the destination. Store to a const or non-lvalue is unrepresentable (pre-IR `AIC-E0404`/`AIC-E0419`; the IR has no addressable const). |
| `IR_ADD` `IR_SUB` `IR_MUL` | integer operands | integer (common type) | left, then right | checked arithmetic: overflow → trap `AIC-R0802` (unsigned wrap-around handled the same; spec §11.3). |
| `IR_DIV` `IR_MOD` | integer operands | integer | left, then right | divisor zero → trap `AIC-R0803`; signed `min / -1` and `min % -1` → trap `AIC-R0802` (spec §11.3). |
| `IR_NEG` | signed integer | signed integer | operand | unary minus on signed minimum → trap `AIC-R0802`; unsigned operand is unrepresentable (pre-IR `AIC-T0306`; spec §10.2). |
| `IR_SHL` `IR_SHR` | integer left, integer count | left operand's type | left, then right | count must be in `0 .. width-1`; failure → trap `AIC-R0804`. Right shift of signed is arithmetic (sign-extending); of unsigned is logical; left shift is defined on the two's-complement bit pattern (spec §11.3). |
| `IR_BAND` `IR_BOR` `IR_BXOR` | integer operands | integer | left, then right | bitwise (spec §10.2). |
| `IR_BNOT` | integer | integer | operand | bitwise complement (spec §10.2). |
| `IR_LNOT` | `bool` | `bool` | operand | logical negation. |
| `IR_LAND` | `bool` left, `bool` right | `bool` | left; right only if left is true | short-circuit (spec §10.4/§11.6). |
| `IR_LOR` | `bool` left, `bool` right | `bool` | left; right only if left is false | short-circuit (spec §10.4/§11.6). |
| `IR_EQ` `IR_NE` | same-type pair (per spec §11.4) | `bool` | left, then right | equality per operand type: integers by mathematical value; enums by underlying value; pointers by byte address; `bool` by value; `str` by lexicographic byte sequence; slices via `IR_SLICE_EQ`. |
| `IR_LT` `IR_LE` `IR_GT` `IR_GE` | same-type pair (integer/integer, enum/enum, `str`/`str`, `T*`/`T*`) | `bool` | left, then right | total ordering per spec §11.4 (byte-address ordering for pointers; byte-sequence ordering for `str`). |
| `IR_SLICE_EQ` | two slices, element type | `bool` | left, then right | element-wise equality using the element type's equality; length mismatch → not equal (spec §11.4/§12.3). |
| `IR_SELECT` | condition (`bool`), then-value, else-value | common type | condition; then exactly one of then/else | `?:` (spec §10.4/§11.6); only the chosen branch is evaluated. |
| `IR_CALL` | callee ref, arguments (ordered) | return type | callee name (a resolved function), then arguments left-to-right (spec §10.4/§13.4) | direct call only (no function pointers; spec §17.3). Argument assignability per Table 11.1 (pre-IR). Composite arguments are passed as address-resident values; the callee copies into its parameter slots (aggregate-by-value semantics, spec §12.1). |
| `IR_LEN` | array/slice/str value | `usize` | operand | element count for arrays/slices, byte count for `str` (spec §12.2–12.3). Array extent is constant; slice/str length is the runtime pair. |
| `IR_PTR` | array/slice/str value | element pointer / `u8*` | operand | first-element address; empty array/slice or zero-length `str` → null (spec §12.2–12.3). |
| `IR_SLICE` | base (array/slice lvalue or `str`), optional start (usize), optional end (usize) | slice type (or `str` for a `str` base) | base, then start, then end (spec §10.4) | bounds check `0 <= start <= end <= extent`; failure → trap `AIC-R0807` (constant out-of-range rejected pre-IR `AIC-E0409`). For a `str` base: byte offsets must fall on UTF-8 code point boundaries; failure → trap `AIC-R0808` (constant `AIC-E0410`). |
| `IR_CAST` | value, target type | target type | operand | checked conversion per the §11.2 matrix: representability failure → trap `AIC-R0801` (bool↔int, narrowing, enum, pointer↔int signedness); `u8[]`→`str` invalid UTF-8 → trap `AIC-R0806`; `str`↔`u8[]` is a bit-preserving reinterpret (no failure); `T*`→`U*` is bit-preserving with the alignment obligation carried by the dereference (spec §11.5/§12.8). Constant out-of-range rejected pre-IR (`AIC-E0408`). |
| `IR_WRAP` | value, target type | target type | operand | wrapping/truncating conversion: mathematical value reduced modulo `2^width`, re-read as the target type; never checked, never traps (spec §11.3/§11.5/ADR-004). The operand itself evaluates under ordinary checked semantics (spec §11.3). |
| `IR_PTR_ADD` | pointer, integer offset | `T*` | pointer, then offset | `p + i` scaled by `sizeof(T)`; the scaling product and resulting byte address must be representable; failure → trap `AIC-R0816` (constant rejected pre-IR `AIC-E0405`; spec §12.5). |
| `IR_PTR_SUB` | pointer, integer offset | `T*` | pointer, then offset | `p - i` scaled by `sizeof(T)`; same checked arithmetic as `IR_PTR_ADD` → trap `AIC-R0816`. |
| `IR_PTR_DIFF` | two `T*` | `isize` | left, then right | `p - q`: byte difference computed as signed `isize`; not representable → trap `AIC-R0816` (constant `AIC-E0405`); byte difference not a multiple of `sizeof(T)` → trap `AIC-R0810` (constant `AIC-E0411`) (spec §12.5). |
| `IR_ZERO` | lvalue/object image | `void` (effect) | operand first | zero-fills an object image: struct/array padding zeroing (§7.4/§9.4) and struct/array literal temporary initialization before field/element stores. |

### 5.4 Value categories

- **Scalar values** — `bool`, the ten integer types, enums (by underlying integer), and pointers — are direct values in the IR. All arithmetic, logic, comparison, cast, wrap, pointer, `len`/`ptr`, and load instructions produce or consume scalars.
- **Composite values** — `str`, slices, arrays, and structs — are **address-resident**: an IR value of composite type is an address to an object image of the type's object representation (spec §9.1). Composite reads (field/element access, loads of scalars from composites) go through `IR_FIELD_ADDR`/`IR_INDEX_ADDR`/`IR_DEREF`/`IR_LOAD`; composite writes go through `IR_STORE` (full object representation) or `IR_ZERO` + field/element stores. `str` and slices are 16-byte pairs (data, length) per §7.2; array/struct images are contiguous bytes with zeroed padding (§7.4/§9.4).
- **Lvalues** are storage locations (Section 2): `IR_LOCAL`, `IR_GLOBAL`, `IR_FIELD_ADDR`, `IR_INDEX_ADDR` (on a mutable base), and `IR_DEREF`. Only lvalues may be stored into, passed by address, or address-taken; the IR's store/lvalue rules make non-lvalue stores and const-address unrepresentable (Section 10 invariants 4, 10).

### 5.5 Closed set

The node kinds of Sections 4.1–4.2, 5.2, and 5.3 are the **complete instruction set**. No other node kind may be added by an implementation package; adding one is an architecture change requiring Main Designer acceptance (downstream-gap rule, milestone plan §7). The type descriptor model (Section 4.4) and `IRConst` (Section 4.5) are part of the instruction set in this sense.

### 5.6 Terminator rules

A **terminator** is a statement that ends a block and cannot be followed by a statement: `IR_RETURN`, `IR_BREAK`, `IR_CONTINUE`, `IR_CALL_TERM`, `IR_TRAP`.

- Every **switch case body** and **default body** must end in a terminator — the IR cannot express fall-through (spec §13.2 no-fall-through; violation pre-IR `AIC-E0412` and structurally impossible in the IR).
- Every **non-`void` function tail** (the function body's final block and every reachable path) must end in `IR_RETURN` (with a value) or `IR_CALL_TERM`/`IR_TRAP` (noreturn). The IR builder only receives accepted programs (spec §13.4/§13.5 already reject non-returning paths, `AIC-E0416`); the invariant makes the rule structural in the IR.
- A `void` function may fall off the end (spec §13.4); its tail block ends without a terminator.
- Blocks that are neither case bodies nor function tails fall through implicitly to the following statement (Section 5.2 `IR_BLOCK`).

---

## 6. Determinism obligations

The IR is deterministic: **identical source files and identical build options yield byte-identical IR** (spec §14.1(6), §14.2, §16.2). Determinism is observable through the deterministic dump (Section 11). The obligations:

1. **Canonical construction order.** Node ids are assigned by a single deterministic traversal: module order (entry module first, then imports depth-first in import order — the build walker order), within a module source order, within a function depth-first pre-order. Lowering never reorders: evaluation order in the IR equals the spec's evaluation order (§10.4).
2. **Canonical ordering of unordered collections.** Any collection whose iteration could affect output order is iterated in a defined canonical order: source order for program constructs (imports, declarations, fields, members, params, statements, expression children); sorted-by-stable-key order for unordered sets/tables (e.g., interned type/constant tables, symbol lookup tables). Iteration order must never depend on hash seeds, pointer addresses, allocation addresses, or host environment (spec §16.2: "iteration over any collection that affects output order is deterministic (sorted by stable keys)").
3. **Type interning.** Identical types share one descriptor (Section 4.4). Intern keys are structural (kind + element + extent + declaration ref by canonical name), never pointer addresses; the intern table's order is first-occurrence in canonical construction order.
4. **Constant deduplication.** Identical constants share one `IRConst`; the representative is the first occurrence in canonical order. Integer constants are stored as exact bit patterns with their type (no host-int-width dependence; `int64_t`-suffixed values are normalized to the type's width).
5. **No nondeterministic inputs.** The IR embeds no timestamps, random identifiers, absolute host paths, environment values, or build-machine identity (spec §14.2/§16.2). Paths are repository-relative with canonical `/` separators. The build options that affect IR output are the same normalized option set recorded in the build manifest (spec §14.4); identical options → identical IR.
6. **Deterministic failure behavior.** The IR stage runs only on accepted programs (Section 1.3), so no IR-level recovery or continuation state exists. An invariant violation (`AIC-I0501`) is deterministic for a given IR construction, and the diagnostic ordering rule (DIAGNOSTIC-CONTRACT §9, phase order) makes even internal-failure output deterministic.
7. **Dump determinism.** The deterministic dump is byte-identical for identical IRs and must be reconstructible from the dump (round-trip; Section 11).

---

## 7. Target-neutrality obligations

The IR is target-neutral: **no x86-64 or other target-specific choice appears in the IR** (spec §14.1(6), ADR-001 stage 6). The contract distinguishes language facts from implementation facts:

**The IR carries language facts** (target-independent within the accepted initial-target contract, and identical for any target the spec fixes):

- types and their identity (Section 4.4);
- the spec's fixed size/alignment facts (§7.1–§7.5) and full object-representation semantics (§9.1);
- the complete source-level semantics of every operation, including evaluation order and trap codes;
- runtime function identities (`rt.mem`, `rt.io`, `rt.proc`, `rt.trap` — the runtime is a language-defined surface, spec §15), and the compiler-emitted call permission set (spec §15.8).

**The IR never carries implementation facts** (backend-owned, WP-M0-17):

- register allocation, register names, stack layout, frame shape;
- instruction selection, x86-64 instruction encoding, address modes;
- calling conventions (including the internal Microsoft x64 compiler-to-runtime ABI of spec §15.7), argument passing implementation, shadow space;
- symbol mangling, PE/COFF sections, relocations, link details;
- any host-compiler or build-environment identity (spec §16.4).

Consequences:

1. An IR node's size/alignment/offset facts come from the spec tables, not from a target model. The backend maps IR types to the target's layout; for the initial target the mapping is identity (the spec fixes the layout), but the mapping decision lives in the backend.
2. `isize`/`usize` are the spec's fixed 64-bit types (§7.1); the IR records them as the language types, not as "target word size".
3. A node with a trap obligation declares the trap code; the backend chooses the mechanism (guard, branch, call) that produces the required trap record and exit code (spec §15.8: the stack-exhaustion guard mechanism is of the implementation's choice).
4. The IR must compile to any target that implements the spec's type/layout contract without IR changes; a target that cannot implement a trap obligation is a backend defect, not an IR extension.

---

## 8. Span and causal-chain preservation

The IR preserves source spans and the causal chain of each construct (spec §14.1(6), ADR-001 §54: diagnostics retain causal and source-location information as they cross stages rather than being reconstructed from plain strings).

1. **Every IR node carries a primary span**: the DIAGNOSTIC-CONTRACT §6 span object (file relative to project root; start/end line, 1-based column, 0-based byte offset) of the smallest source construct it derives from. Nodes introduced by lowering (e.g., `IR_ZERO` for padding, `IR_TRAP` for a failing check, an `IR_LOCAL` for a compiler temporary) carry the span of the source construct they serve. Spans are exact source ranges; they are never truncated to strings, never reconstructed from messages, and never synthesized.
2. **Every IR node carries a cause chain**: the ordered, parent-linked chain from the node to its module root. Each link records the source construct kind (the AST node kind, e.g. `AST_EXPR_BINARY`) and that construct's primary span, plus resolved-reference facts (declaration/type/constant ids). The chain is total (a node with no parent except the module root it belongs to).
3. **Lowering preserves causality**: when one source construct produces several IR nodes (e.g., a compound assignment produces the destination evaluation, source evaluation, operation, and store; a checked operation produces the operation and its failure path), every produced node carries the source construct's span in its cause chain, and derived nodes record the parent IR node they came from. The causal order is root cause first (the source construct), matching the diagnostic contract's `causes` convention (DIAGNOSTIC-CONTRACT §4).
4. **Consumption**: the backend emits trap records and any backend-phase diagnostic from the failing IR node's span, cause chain, and type/value facts — never from plain strings. `AIC-I0501` invariant diagnostics carry the derived span of the violating node (DIAGNOSTIC-CONTRACT §11.6).
5. **Determinism of spans**: spans are identical to the AST spans produced by the parser (WP-M0-09); the builder copies them without modification. Two builds produce identical span and cause data.

---

## 9. Representation coverage (every semantic rule representable and enforceable)

This section is the acceptance evidence for spec §14.1(6)'s final clause: every semantic rule in the language specification is representable and enforceable in the IR. For each rule family: the IR representation, and how the IR makes the rule enforceable. Rules enforced entirely before the IR (lexical §3–§4, syntax §5, name §6, type §7.6, semantic §8–§13) are representable in the sense that the IR is built only from accepted programs; where the IR additionally makes a rule structural (impossible to violate) or invariant-checked, that is stated.

### 9.1 Lexical and syntax rules (§3–§5)

Enforced at stages 1–3 (load, lex, parse). No IR node represents a rejected token or parse; the driver stops before IR when diagnostics exist (Section 1.3). The IR preserves the parsed program's structure and spans, so any downstream diagnostic (including `AIC-I0501` and trap records) can cite the original source region. Grammar-level uniqueness (spec §5.1) is satisfied before IR by construction.

### 9.2 Name binding, scopes, visibility, modules, imports (§6)

Enforced at stage 4 (WP-M0-10). The IR records the **outcome**: resolved module units, resolved cross-module edges to declaration IR nodes, fully qualified names (spec §6.6: same name → same node), and per-function slot scoping. Reserved `rt.*` handling is pre-IR; the IR references runtime functions only through the four runtime submodules of spec §15 (imports of `rt.*` are represented as `IR_IMPORT` refs to the runtime surface; the reserved-module rejections `AIC-N0207..N0209` are pre-IR).

### 9.3 Types (§7)

Represented by the type descriptors of Section 4.4 (identity, layout facts, completeness). Enforceable: `void`-typed values are unrepresentable (a value node's result type is never `void`); incomplete struct types are rejected pre-IR (`AIC-T0302`) and no IR type references a struct before its `IR_STRUCT_DECL` completes (invariant 3); recursive-by-value structs are rejected pre-IR (`AIC-T0303`); enum continuation/aliasing values are resolved into `IR_ENUM_DECL` members (spec §7.5).

### 9.4 Constants, variables, storage (§8)

- `const` (§8.1): `IR_GLOBAL_CONST` (module) and constant bindings resolved to `IRConst` values at use sites; no storage, no address (Section 4.2) — `&const` is unrepresentable, making `AIC-E0402` structurally unreachable.
- `var` (§8.2): `IR_LOCAL_DECL` (automatic) / `IR_GLOBAL_VAR` (static) always carry an initializer — the missing-initializer rule (`AIC-E0403`) is enforced pre-IR and is structurally unreachable.
- Storage duration (§8.3): static vs. automatic slots; consts have neither (above).
- Mutability/assignability (§8.4): `IR_STORE` targets only lvalues; consts are not addressable; assignment-to-const and non-lvalue stores are unrepresentable (pre-IR `AIC-E0404`/`AIC-E0419`, structurally unreachable).

### 9.5 Values, representation, initialization, padding (§9)

- Object representation (§9.1): the address-resident composite model (Section 5.4) and full-object `IR_STORE`/`IR_ZERO` semantics carry the exact byte-level representation, including padding.
- Initialization (§9.2): local init at declaration execution (`IR_LOCAL_DECL`), static init from constant initializer (`IR_GLOBAL_VAR`), allocation zero-init via the `rt.mem.alloc_bytes` call contract (§15).
- Assignment (§9.3): composite assignment copies the complete object representation via `IR_STORE`; slice assignment copies only the (data, length) pair (slice type is a 16-byte pair — the composite copy copies exactly the pair).
- Deterministic padding (§9.4): `IR_ZERO` on struct/array images zeroes padding; `IR_STORE` preserves it. The IR therefore makes padding deterministic and observable (spec §9.4).

### 9.6 Expressions and operators (§10)

- Precedence/associativity (§10.1) and excluded constructs (§10.3): resolved in the parse tree; the IR tree mirrors the parsed precedence (no re-association is representable). Chained comparisons (`AIC-T0305`) are rejected pre-IR; no `IR_EQ`/`IR_LT` chain of three operands exists in the IR.
- Operator typing (§10.2): each IR value node's operands have the typed rules of Section 5.3; a type mismatch is an invariant violation (`AIC-I0501`, invariant 4), never silent.
- Evaluation order (§10.4): total per-node child order (Section 5); no unspecified order exists in the IR (invariant 11).
- Constant expressions (§10.5): constant expressions are evaluated pre-IR (WP-M0-12) and folded to `IRConst` values (Section 4.5); constant failures (`AIC-E0405..E0411`) are rejected pre-IR. The IR's constants carry the span/cause of the constant expression (Section 8).

### 9.7 Conversions and operator typing (§11)

- Implicit conversions (§11.1): represented by the typed node model — the consumer node's operand carries the widened type; no implicit conversion instruction exists because none is observable at runtime. The value-preserving-widening restriction is enforced pre-IR (`AIC-T0307` for any other pairing).
- Explicit conversions (§11.2/§11.5): `IR_CAST` (checked) and `IR_WRAP` (modulo) with their matrix pairs; invalid pairs are rejected pre-IR (`AIC-T0308`) and unrepresentable.
- Checked arithmetic (§11.3): `IR_ADD/SUB/MUL/DIV/MOD/NEG/SHL/SHR` carry their trap codes (`AIC-R0802/R0803/R0804`); constant failures rejected pre-IR (`AIC-E0405/E0406/E0407`). No same-width wrapping arithmetic exists in the language (ADR-004); `IR_WRAP` is a conversion only, and its operand is an ordinary checked node (spec §11.3). The IR contains no wrapping arithmetic op.
- Comparison semantics (§11.4): `IR_EQ..IR_GE` per-type semantics; `IR_SLICE_EQ` element-wise; `str` byte-sequence ordering.
- Assignment/compound (§11.6): `IR_STORE`; compound assignment is lowered by the builder into destination-location evaluation, source evaluation, the operation, and the store — in the spec's order (§10.4), preserving the whole expression's span/cause.

### 9.8 Arrays, slices, str, pointers (§12)

- Arrays (§12.1): `T[N]` type with extent; `IR_INDEX_ADDR` bounds-checked (`AIC-R0807`); array literals lowered to `IR_ZERO` + element stores (list form) or `IR_ZERO` + repetition (see IRC-N1); array values are full-copy composites (`IR_STORE`, call/return by value).
- `str` (§12.2): 16-byte pair; `IR_INDEX_ADDR` yields `u8` by byte offset with bounds check; `IR_LEN` (byte count) / `IR_PTR` (`u8*`, null for empty); slicing via `IR_SLICE` with code-point-boundary check (`AIC-R0808`/constant `AIC-E0410`).
- Slices (§12.3): 16-byte pair; `IR_SLICE` bounds-checked (`AIC-R0807`); `IR_SLICE_EQ` element-wise equality.
- Slice syntax (§12.4): `IR_SLICE` start/end operands typed `usize` after permitted implicit conversion (Table 11.1).
- Pointers (§12.5): `IR_DEREF` with null (`AIC-R0809`) and address/alignment (`AIC-R0811`) obligations; `IR_PTR_ADD/SUB` checked scaling (`AIC-R0816`); `IR_PTR_DIFF` with divisibility (`AIC-R0810`) and overflow (`AIC-R0816`) obligations; `IR_CAST` pointer↔int/pointer↔pointer per §11.2 with alignment obligation carried to dereference.
- Member access (§12.6): `IR_FIELD_ADDR`; `p->f` ≡ `(*p).f` lowers to `IR_DEREF` + `IR_FIELD_ADDR`; enum member access resolves to `IR_ENUM_VAL`.
- Struct literals (§12.7): typed `IR_ZERO` + field stores in **literal** order (initializer evaluation left-to-right in literal order, spec §12.7/§10.4), laid out in declaration order; field errors (`AIC-T0313`) rejected pre-IR.
- Raw-pointer contract (§12.8): the IR's pointer ops carry the pointer-op subset of the §12.8(2) trap obligations — null dereference (`AIC-R0809`), pointer-adjacent bounds (`AIC-R0807`), pointer arithmetic overflow (`AIC-R0816`), and byte-misalignment/access violations where observable (`AIC-R0811`); the allocator traps of §12.8(2)/§15.5 (`AIC-R0812`, `AIC-R0813`) are obligations of the `rt.mem` runtime calls, carried by the `IR_CALL` contract, not by pointer-op nodes. The compiler obligations of §12.8(4) (no C-style lifetime UB optimization, defined stale-access outcomes) are backend obligations that the IR enforces by declaring every pointer op's trap code — the backend cannot assume away a stale access without violating the contract. The IR carries no temporal-safety claim (spec §12.8(5)); the trap set is exactly the §12.8/§15.5 set.

### 9.9 Statements and control flow (§13)

- Blocks (§13.1): `IR_BLOCK`; brace-only controlled bodies are structural (`IR_IF`/`IR_WHILE`/`IR_FOR`/`IR_CASE`/`IR_DEFAULT` bodies are blocks; the brace-requirement rejection `AIC-S0104` is pre-IR).
- Selection (§13.2): `IR_IF` (bool condition; `AIC-T0310` pre-IR), `IR_SWITCH` (integer/enum selector; `AIC-T0311` pre-IR); no fall-through structural (Section 5.6); duplicate case (`AIC-E0413`) and duplicate default (`AIC-E0420`) pre-IR.
- Iteration (§13.3): `IR_WHILE`, `IR_FOR` with scoped init and defined `break`/`continue` targets.
- Functions/calls (§13.4): `IR_FUNCTION` + `IR_CALL` (direct only); argument count/assignability pre-IR (`AIC-T0312`); return typing invariant (8).
- Reachability (§13.5): enforced pre-IR (`AIC-E0416/E0417`); the IR's terminator rules (Section 5.6) make the accepted program's control flow structurally explicit so no reachable path can fall off a non-`void` function.

### 9.10 Runtime and traps (§15)

- Runtime module surface (§15.1–15.4): `IR_CALL` to resolved `rt.mem`/`rt.io`/`rt.proc`/`rt.trap` functions with spec signatures; standard-stream accessors (`rt.io.stdin/stdout/stderr`) are source-visible only and are never emitted implicitly (spec §15.8) — the IR builder must not synthesize them.
- Trap contract (§15.5): every failing operation's trap code is attached to the IR node that performs it (Section 5.3); `IR_TRAP`/`IR_CALL_TERM` terminate; exit code 70 and the trap record (code, span, facts) are backend obligations carried by the node's span/cause/facts.
- Compiler-emitted runtime calls (§15.8): any implicit runtime call the IR builder emits must be from the §15.8 table (allocation, release, copy, fill, file I/O, process, trap report); emitting a call outside the table is a specification defect (this contract adopts that rule; a violation is an invariant-level error).
- Environmental inputs (§15.6): runtime behavior dependent on environmental inputs (file contents, memory, args, OS handles, resource availability) is represented as ordinary calls/values; the IR adds no language-level semantics to them (deterministic per identical inputs and environment).
- Calling convention/ABI (§15.7): internal ABI facts are backend-owned (Section 7); the IR carries the function boundary (params/return types) only. Noreturn treatment: calls to `rt.proc.exit`/`rt.trap.report` are `IR_CALL_TERM` (terminators; spec §15.7 noreturn).
- Stack exhaustion (§15.5/§15.8): the observable obligation (trap record `AIC-R0815`, exit 70) is backend-owned via a guard of its choice; the IR records the obligation as a function-level attribute the backend must implement (prologue guard or equivalent).

### 9.11 Pipeline, determinism, artifacts (§14, §16)

- §14.1 stage boundary: the IR contract itself (this document) is the stage-6 observable contract.
- §14.2/§16.2 determinism: Section 6 obligations; the deterministic dump (Section 11) makes IR determinism observable and the no-normalization rule applies to it as a compiler-produced artifact.
- §14.4 build manifest: the manifest records output artifact hashes; if the IR dump is included in build evidence, its path/hash follow the manifest rules (relative paths, no self-hash). The dump is not a required manifest field; whether the M1 evidence includes IR dumps is a WP-M0-20/M1 verification decision, not an IR contract obligation.
- §15.8 compiler-emitted calls, §16.4 host-compiler independence: the IR is produced identically regardless of the Stage-0 host compiler (the IR depends only on accepted inputs, Section 6.5).

### 9.12 Interpretation notes (flagged for the Main Designer gate)

- **IRC-N1 — repetition-form array literal evaluation count (§12.1/§10.5).** The spec defines the constant-expression form `[e; N]` (§10.5) but does not state how many times `e` is evaluated when the repetition form appears in a runtime (non-constant) context. This contract adopts the deterministic reading: **`e` is evaluated exactly once** and its value is stored into each of the `N` elements (via `IR_ZERO` + repeated `IR_STORE` of the single evaluated value). This is the only reading that keeps side effects deterministic and single-occurrence; it does not change any accepted spec text. Confirmed on acceptance, else routed to the Planner (spec §17.2) by the Main Designer.
- **IRC-N2 — composite constant image placement.** `IRConst_STRUCT`/`IRConst_ARRAY`/`IRConst_STR` are described as data the backend materializes in read-only data for global/const contexts; for automatic-storage initialization the builder may instead lower the constant to `IR_ZERO` + stores (Section 5.3). Both forms are permitted by this contract; the choice is a WP-M0-16c/backend implementation decision that must not alter observable behavior or dump determinism. This is an implementation latitude note, not an architecture question.
- **IRC-N3 — no IR-level optimization passes.** This contract defines the IR as a lowering target with no transformation passes (Section 1.2). A future pass (even a semantics-preserving canonicalization) is an architecture change requiring Main Designer acceptance before WP-M0-17 or later packages may add one. This merely restates ADR-001's deferral at IR scope; flagged for explicit confirmation because "optimization deferred" could otherwise be read as per-pass discretion.

---

## 10. IR invariants (WP-M0-16b1; violations → `AIC-I0501`)

The IR core (WP-M0-16b1) enforces the following invariants at construction and verification time. A violation is a compiler internal error reported as `AIC-I0501` with a derived span (DIAGNOSTIC-CONTRACT §11.6, phase `"ir"`). The list is closed for the contract; extending it is an implementation detail of WP-M0-16b1 within this contract.

1. **Graph well-formedness.** Exactly one `IR_MODULE` per source module; node ids unique; parent/child edges consistent; every node reachable from its module root; every node's cause chain terminates at its module root.
2. **Span/cause presence.** Every node carries a non-null primary span and a cause chain (Section 8); spans are valid DIAGNOSTIC-CONTRACT §6 objects with `start.offset <= end.offset`.
3. **Type well-formedness.** Every value node has a well-formed result type; `void` never appears as a value type; array extents are non-negative; named types reference existing `IR_STRUCT_DECL`/`IR_ENUM_DECL` nodes; struct field offsets are within the struct size and non-overlapping.
4. **Operand typing.** Every node's operands satisfy the typing rules of Section 5.3 (e.g., `IR_ADD` operands integer-typed, `IR_LAND`/`IR_LOR`/`IR_SELECT` condition bool-typed, `IR_DEREF` operand pointer-typed, `IR_INDEX_ADDR` index `usize`-typed, `IR_SLICE` bounds `usize`-typed, comparisons same-type pairs).
5. **Terminators.** Case/default bodies and non-`void` function tails end in a terminator (Section 5.6); a terminator is the last statement of its block; no statement follows a terminator.
6. **No fall-through.** Case/default bodies contain exactly their own statements; a case body cannot reach the next case.
7. **break/continue placement.** Every `IR_BREAK` targets an enclosing `IR_SWITCH`/`IR_WHILE`/`IR_FOR`; every `IR_CONTINUE` targets an enclosing `IR_WHILE`/`IR_FOR` (a `continue` in a switch inside a loop targets the loop, spec §13.2); targets are unique and resolve to the correct enclosing construct.
8. **Return typing.** `IR_RETURN` in a non-`void` function carries a value of the function's return type; `IR_RETURN` in a `void` function carries none.
9. **Trap-code presence.** Every node kind with a runtime failure mode (Section 5.3 table) carries its declared trap code; the code is from the accepted trap registry (DIAGNOSTIC-CONTRACT §11.8) or a user trap via `IR_TRAP` with a numeric `trap_code`.
10. **Store/lvalue rules.** `IR_STORE`'s destination is an lvalue; composite stores use the complete-object-representation rule; consts have no addressable node (no store target, no `IRConst_ADDR` target).
11. **Evaluation order.** Each node's child order is total and matches Section 5.3; the dump preserves it (Section 11); no reordering is representable.
12. **Determinism.** The IR graph must be reconstructible byte-identically from its deterministic dump (round-trip; WP-M0-16b2 verification); the dump and graph must agree on node set, order, spans, causes, types, and constants.

---

## 11. Verification and deterministic dump (WP-M0-16b2)

WP-M0-16b2 implements the deterministic dump and verification support. This contract fixes what they must make observable:

1. **Dump content.** The dump renders every node: id, kind, result type, operand ids (in evaluation order), constant values (canonical form), spans (DIAGNOSTIC-CONTRACT §6 shape), cause links, and trap codes. Nothing that affects output order may be omitted; the dump is complete for reconstruction.
2. **Canonical form.** The dump is a deterministic canonical textual form: a fixed node order (the canonical construction order of Section 6.1), stable formatting, repository-relative paths with `/` separators, no timestamps/environment/host identity (Section 6.5).
3. **Byte determinism.** Two builds of identical inputs and options produce byte-identical dumps; the dump is a compiler-produced artifact subject to the spec §16.2 no-normalization rule.
4. **Round-trip verification.** The verification support must parse a dump and reconstruct the IR graph, then re-dump it; the re-dump is byte-identical to the input dump (invariant 12).
5. **Invariant verification.** The verification support runs the Section 10 invariant checks over a reconstructed graph and reports violations as `AIC-I0501`.
6. **Tests.** WP-M0-16b2 owns dump-determinism tests (same source → same dump bytes; distinct sources → distinct dumps where semantics differ) and span/cause preservation tests (spans and cause chains survive lowering; WP-M0-16c2 adds builder-level tests).

---

## 12. Interface to adjacent stages

### 12.1 Inputs (builder, WP-M0-16c)

The builder consumes the resolved, validated build (Section 1.3):

- parse tree with spans (WP-M0-09);
- resolved names/modules/imports (WP-M0-10);
- resolved types and layout facts (WP-M0-11);
- evaluated constants and failure records (WP-M0-12);
- semantic validation outcomes (WP-M0-13) — the driver invokes the IR stage only when the build is clean.

The builder's mapping is 1:1 with the AST for structure (Section 3 principle 2) and lowers per Sections 4–5. Mapping rules that require a choice (e.g., `IR_FOR` retained first-class, compound assignment lowered to a sequence, struct literals lowered to `IR_ZERO`+stores) are fixed by this contract; WP-M0-16c implements them.

### 12.2 Outputs (backend, WP-M0-17; verification, WP-M0-16b2)

- The backend consumes the IR graph and must implement every node's semantics and trap obligations (Sections 5, 9.10); it owns all target-specific decisions (Section 7).
- The dump (Section 11) is available for verification and (if the M1 evidence plan selects it) comparison evidence.

### 12.3 Diagnostics integration

- IR-phase diagnostics: `AIC-I0501` (invariant violation) with derived span; phase `"ir"` in the §9 ordering.
- Trap records: emitted at runtime from IR node span/cause/facts (Section 8.4), phase `"trap"`, exit code 70 (DIAGNOSTIC-CONTRACT §10, spec §15.5).

---

## 13. Review state and acceptance record

**Status:** Proposed (draft v0.1.0), pending Main Designer architecture review (WP-M0-16a gate; class: Reviewer + Main Designer per manifest §WP-M0-16a).

**Acceptance criteria coverage (self-check against the card):**

- [x] **IR determinism stated** — Section 6 (identical source + options → byte-identical IR; observable via deterministic dump; spec §14.1(6)/§14.2/§16.2).
- [x] **Target-neutrality stated** — Section 7 (no x86-64 specifics; language facts vs. implementation facts; spec §14.1(6), ADR-001 stage 6).
- [x] **Span/cause preservation stated** — Section 8 (every node carries primary span + cause chain; lowering preserves causality; diagnostics/traps never reconstructed from strings; spec §14.1(6), ADR-001 §54).
- [x] **Representation coverage for every semantic rule** — Section 9 matrix (§3–§16 rule families: representation + enforcement), with interpretation notes IRC-N1..N3 flagged for the gate.
- [x] **Main Designer acceptance** — pending; recorded in the acceptance entry below when the gate passes.

**Exclusions honored:** no IR implementation (16b/16c), no optimizations (ADR-001), no public language-contract change (IR internal), no spec/ADR/governance modification.

**Review record to be appended after the gate:**

- *[pending]* Main Designer architecture review verdict (task t_24eee034; reviewer/gate cards created by the OM §6.1 watchdog routing).
- *[pending]* Interpretation notes IRC-N1 (repetition-form evaluation count) and IRC-N3 (no IR-level optimization passes) — confirmed or routed to Planner; IRC-N2 is an implementation-latitude note needing no decision.

**Change record:**

- **v0.1.0 (2026-08-12):** initial Proposed draft authored by WP-M0-16a (senior_specialist). No changes from any review yet.
