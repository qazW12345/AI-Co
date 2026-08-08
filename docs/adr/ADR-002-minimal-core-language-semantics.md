# ADR-002: Minimal Core Language Semantics

## Status

Accepted

## Supersedes

None

## Context

Decision owner: Main Designer. The AI-Co charter requires a minimal C-based language optimized for AI coding agents, with lack of ambiguity and detailed diagnostics as defining qualities. The first language line must be expressive enough to implement its own compiler and utilities, but unnecessary features increase implementation cost, specification surface, and opportunities for contradictory agent output.

C provides a valuable training-data prior and familiar expression/control-flow vocabulary, but several C properties conflict directly with AI-Co's goals: context-sensitive declarators, implicit conversions, unspecified evaluation order, implementation-defined type widths, textual preprocessing, uninitialized values, undefined behavior, and diagnostics that are not part of the language contract.

Alternatives considered:

1. **Adopt a strict subset of ISO C without changing syntax or semantics.** Maximizes familiarity but preserves many ambiguity and undefined-behavior problems unless the subset is so narrow that self-hosting becomes impractical.
2. **Design a completely new language.** Allows the cleanest formal model but discards C training-data leverage and increases agent learning cost.
3. **Preserve C-like expressions and control flow while replacing the most ambiguity-prone declarations and semantics.** Retains high-value familiarity while making deliberate agent-oriented departures.
4. **Start with a larger modern language surface.** Tagged unions, generics, inference, methods, closures, and concurrency could improve expressiveness, but would materially delay a precise, self-hosting core.

## Decision

### Surface-language boundary

AI-Co retains C-style operators, expressions, blocks, statement terminators, `if`/`else`, `while`, `for`, `break`, `continue`, `return`, `switch`, functions, structs, enums, arrays, and explicit pointers where their meaning can be made deterministic.

AI-Co does not retain C's general declarator grammar. Declarations use one canonical, locally readable form:

- mutable variable: `var name: Type = expression;`
- immutable binding: `const name: Type = constant_expression;`
- function: `fn name(parameters) -> ReturnType { ... }`
- parameter: `name: Type`
- struct field: `name: Type;`

There are no alternative declaration spellings. A declaration introduces exactly one name. Type inference is absent from the minimal language.

### Source and identifiers

Source files are valid UTF-8. Identifiers and keywords are restricted to ASCII in the minimal language. UTF-8 may appear in comments and string literals. This avoids Unicode normalization, confusable-identifier, and visually equivalent spelling ambiguity while retaining UTF-8 data support.

Whitespace is not syntactically significant except as token separation. `//` line comments and non-nesting `/* ... */` block comments are supported. Comments do not alter tokenization beyond replacing themselves with whitespace.

### Primitive types

The minimal primitive set is:

- `void`;
- `bool` with values `true` and `false` only;
- signed integers `i8`, `i16`, `i32`, `i64`;
- unsigned integers `u8`, `u16`, `u32`, `u64`;
- pointer-sized integers `isize`, `usize`, fixed to 64 bits for the initial target;
- `str`, an immutable UTF-8 byte sequence represented semantically as data plus length.

There are no plain `char`, `short`, `int`, `long`, `float`, `double`, or target-variable integer types in the minimal language. Floating point is deferred until its literal, rounding, exceptional-value, comparison, and reproducibility semantics can be specified completely.

All integer representations are two's complement with stated width. `u8` is the byte type. `bool` is not an integer and has no implicit numeric conversion.

### Composite and reference types

The minimal composite/reference set is:

- fixed arrays `T[N]`;
- bounded slices `T[]`, carrying data and length;
- explicit nullable raw pointers `T*` with the value `null`;
- named `struct` types;
- named `enum` types with an explicit fixed integer representation chosen in the declaration.

General C unions, bit-fields, variadic functions, complex declarators, and implicit array-to-pointer decay are absent. Function-pointer types are deferred unless the Planner demonstrates they are essential for the minimal self-hosting compiler; such a finding returns to the Main Designer as an architecture question.

Arrays are values with fixed size. Slices are explicit views and do not own storage. Raw pointers never carry an implicit length. Conversions among arrays, slices, pointers, and integers are never implicit.

### Initialization, storage, and memory

Every variable must be initialized before it can be observed. The minimal language has no uninitialized-value state available to source programs.

Storage duration, mutability, alignment, and lifetime must be specified for globals, locals, parameters, return values, arrays, slices, and allocated memory. Memory management is explicit; there is no garbage collector. A minimal project-owned runtime provides allocation, deallocation, byte copying, file I/O, and process termination needed by the compiler.

Array and slice indexing is bounds-checked. Null dereference, invalid alignment, use of an invalidated allocation, double release through the normative allocator, and out-of-range indexing produce deterministic runtime traps where they cannot be rejected statically. Raw pointer operations are visually explicit and may not be introduced through ordinary arithmetic conversions.

The minimal language does not claim complete spatial or temporal memory safety. It requires deterministic rules and diagnostics/traps for every supported operation rather than C-style undefined behavior.

### Expressions, order, and conversions

Operands and function arguments evaluate left-to-right. `&&` and `||` short-circuit left-to-right. Assignment evaluates the destination location before the source value. The specification must state the order for every compound expression form.

Only `bool` may control `if`, `while`, and `for` conditions. There is no implicit truthiness.

No narrowing, sign-changing, integer/boolean, pointer/integer, array/pointer, or slice/pointer conversion is implicit. The specification may permit only mathematically value-preserving widening conversions implicitly when the source type's full range is representable in the destination. All other conversions use named explicit operations whose checked or truncating behavior is visible in source.

Default signed and unsigned arithmetic is checked. Overflow, division by zero, invalid division overflow, and invalid shift counts produce compile-time diagnostics for constant expressions or deterministic runtime traps otherwise. Explicit wrapping operations are provided as named built-ins; ordinary operators never silently wrap.

Comparison operators return `bool`. Chained comparison syntax is rejected rather than assigned a surprising C-like interpretation.

### Control flow

All controlled statement bodies use braces, including single statements. Fall-through between `switch` cases is prohibited; each case terminates explicitly through its block structure, `break`, `return`, or another specified terminator. `goto`, labels, computed jumps, exceptions, and hidden cleanup control flow are absent from the minimal language.

Functions declare exactly one return type. Every reachable path in a non-`void` function returns a value. Recursion is permitted. Overloading, default arguments, variadic arguments, methods, closures, and implicit constructors/destructors are absent.

### Modules and compilation units

The textual preprocessor, macro substitution, header inclusion, conditional compilation, and pragma mechanism are absent.

Each source file declares one module through a canonical module declaration. Imports name modules, not files, and are resolved through one explicit project root recorded in the build manifest. Resolution may not depend on current working directory, environment-variable search paths, registry state, network access, or first-match behavior across multiple roots.

Import cycles are rejected in the minimal language. Public/private visibility is explicit. Re-export and wildcard import are absent initially. The same fully qualified name always denotes the same declaration within a build.

### Runtime and system boundary

The minimal language runtime is project-owned and small. It exposes only the capabilities required for compiler self-hosting and basic utilities: deterministic allocation/deallocation, byte and string operations, file open/read/write/close, process arguments, process exit, and trap reporting.

Platform-specific operating-system calls remain behind explicit runtime modules. The initial language does not promise a stable foreign-function interface or Windows ABI compatibility. The compiler and runtime may use accepted Windows conventions internally without making them source-language guarantees.

### Compile-time and runtime failure model

A valid program has fully specified observable behavior within declared resource limits. An invalid program is rejected with structured diagnostics. A dynamically invalid operation follows a specified runtime trap contract containing a stable trap code, source location when available, operation, relevant type/value facts, and causal context.

The specification may define resource exhaustion and external I/O failure as explicit result values or named runtime traps, but may not leave their behavior undefined. Environmental values such as file contents and available memory are inputs, not language ambiguity.

### Minimal-feature exclusions

The following are excluded until separately justified and designed: floating point, unions, bit-fields, function pointers unless self-hosting evidence requires them, generics, type inference, methods, operator overloading, closures, exceptions, garbage collection, reflection, compile-time execution, macros, concurrency, atomics, inline assembly, dynamic linking contracts, package management, and network dependency resolution.

Feature absence is preferred over an underspecified placeholder. A later feature ADR must state why the feature is needed, how it preserves deterministic semantics and diagnostics, and its effect on self-hosting.

### Specification requirement

The Planner must turn these decisions into a normative specification containing lexical grammar, syntactic grammar, static semantics, dynamic semantics, diagnostics, runtime contracts, examples, counterexamples, and acceptance criteria. If a self-hosting requirement cannot be met within this feature set, the Planner must return the exact capability gap to the Main Designer rather than adding a feature silently.

## Consequences

### Positive

- C-like expressions and control flow preserve substantial training-data familiarity.
- Canonical declarations remove the most difficult C declarator ambiguity.
- Fixed-width types and explicit conversions eliminate target-dependent arithmetic interpretation.
- Defined evaluation order, checked arithmetic, initialization, bounds checks, and traps replace major classes of C undefined behavior.
- ASCII identifiers avoid normalization and confusable-name problems while UTF-8 data remains supported.
- The small feature set is plausible for a first compiler and conformance suite.
- Explicit modules replace textual preprocessing and hidden include-path behavior.

### Negative

- AI-Co source is not C source and cannot reuse C headers or libraries directly.
- Canonical declaration syntax is a visible departure from C.
- Checked operations and bounds checks increase implementation complexity and runtime cost.
- Excluding floating point, unions, function pointers, and generics limits early application domains.
- A self-hosted compiler may require more verbose data structures than a language with tagged unions or generics.
- Runtime trap behavior creates additional backend and diagnostic obligations.

### Risks and controls

- **Self-hosting capability gap:** Planner must prove the minimal compiler can be expressed or escalate exact missing facilities.
- **Feature pressure causes silent expansion:** only accepted ADRs may expand the minimal feature set.
- **C familiarity is lost through cumulative changes:** retain C operator, expression, block, and control-flow forms unless a documented agent-oriented conflict exists.
- **Checks become implementation-defined:** every check and trap receives a normative condition, stable code, and test.
- **Pointer model remains unsafe:** pointer operations are explicit, conversions restricted, and dynamic failures specified; stronger safety is deferred rather than claimed.
- **Diagnostic requirements outgrow the core:** diagnostic schema and runtime trap contract are specified before implementation work is made ready.

### Approval and follow-up

This minimal semantic direction is Accepted under Marcel's delegated technical authority. It is sufficient for the Planner to begin a Proposed normative language specification. Implementation remains blocked pending requirements review, architecture conformance review, and implementation-ready acceptance criteria.
