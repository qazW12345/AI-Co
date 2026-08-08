# AI-Co Language Specification v0.1.1 (Proposed)

**Status:** Proposed
**Owner:** Planner
**Decision owner:** Main Designer (architecture); decisions recorded in ADR-004 are Human Sponsor approvals and are applied here as governing direction.
**Approver:** Main Designer (architectural acceptance); Reviewer (independent conformance review); Marcel (Human Sponsor) for purpose-affecting questions.
**Version:** 0.1.1
**Date:** 2026-08-08
**Scope:** project AI-Co

## Authority

This specification is derived from, and must not contradict:

1. `PROJECT_CHARTER.md` (Accepted, v0.1.0)
2. `docs/adr/ADR-001-bootstrap-compiler-and-initial-target.md` (Accepted)
3. `docs/adr/ADR-002-minimal-core-language-semantics.md` (Accepted)
4. `docs/adr/ADR-003-safety-wrapping-and-host-boundaries.md` (Superseded by ADR-004; retained as historical evidence of the Main Designer's interim containment response)
5. `docs/adr/ADR-004-human-sponsor-bootstrap-resolutions.md` (Accepted; Human Sponsor resolutions on the escalated open questions; supersedes the targeted parts of ADR-003 and ADR-002 identified therein)
6. `research/ENVIRONMENT_BASELINE_2026-08-08.md` (evidence)
7. `../../governance/CONSTITUTION.md` and `../../governance/OPERATIONS_MANUAL.md`
8. Marcel's accepted base direction as durably captured in the charter.
9. `docs/reviews/INITIAL-ARCHITECTURE-REVIEW-2026-08-08.md` (Approved with Minor findings; FIND-001/FIND-002 architecture-resolved, FIND-003/FIND-004/FIND-005 Planner gates closed in this specification).

This document is a *Proposed* normative draft. It becomes normative only after the required review gates pass: Planner self-review, independent Reviewer conformance review, and Main Designer architectural acceptance. Nothing in this document authorizes implementation before those gates pass. Implementation remains blocked until the review gates in Section 21 are met; this Proposed draft is not an Accepted specification and must not be relabeled as one.

Related documents:

- `spec/DIAGNOSTIC-CONTRACT.md` — versioned JSON Lines diagnostic contract and proposed diagnostic-code/trap-code table (normative companion).
- `spec/OPEN-QUESTIONS.md` — monitored resolution record for previously escalated architecture/requirements gaps; no implementation-blocking open questions remain.

---

## 1. Purpose and intended outcome

The AI-Co minimal language must be sufficient to:

1. implement an AI-Co compiler in AI-Co;
2. compile that compiler through a Stage-0 bootstrap compiler written in C17;
3. use the resulting AI-Co compiler to compile itself again (Stage 1, Stage 2);
4. demonstrate deterministic equivalence between the self-hosting stages under the accepted bootstrap contract (Section 16);
5. develop subsequent AI-Co features and supporting utilities primarily in AI-Co.

This specification defines, normatively and testably, what a conforming AI-Co compiler must **accept**, **reject**, **produce**, and **diagnose** for the minimal language. It deliberately does **not** specify implementation techniques, internal data structures, optimization policy, or instruction selection.

---

## 2. Normative vocabulary and conformance terminology

This section defines the meaning of terms used throughout this specification. Definitions are normative.

### 2.1 Normative key words

- **must / must not / shall / shall not**: an absolute requirement or prohibition on a conforming implementation or program.
- **required**: a condition that must hold.
- **may**: an optional condition; a conforming implementation or program is free to choose either side without affecting conformance.
- **recommended / should**: a condition that is preferred but not required; deviation must be documented.

### 2.2 Core terms

- **Program**: a set of source files, one per module, plus a build manifest that selects one entry module and states build options.
- **Valid program**: a program that satisfies every requirement in this specification and the diagnostic contract. A conforming compiler must accept it and produce the required artifacts.
- **Invalid program**: a program that violates one or more normative requirements. A conforming compiler must reject it and emit a structured diagnostic with the required stable code and source span.
- **Reject**: the compiler's deterministic refusal to accept a program or expression; rejection always produces a diagnostic.
- **Trap**: a deterministic runtime failure with a stable trap code, per Section 15 and the diagnostic contract. A trap terminates the process with a defined exit code after reporting a trap record.
- **Diagnostic**: a structured, versioned, machine-readable record describing a compile-time problem (rejection) or runtime trap. See `DIAGNOSTIC-CONTRACT.md`.
- **Compile time**: the phase in which source is transformed into the specified artifacts. Constant expressions are evaluated at compile time; violations in constant contexts are compile-time rejections.
- **Runtime**: the phase in which a compiled program executes. Runtime failures are traps, never undefined behavior.
- **Undefined behavior**: forbidden in this specification. Every operation either has fully defined observable behavior, is rejected at compile time, or traps deterministically at runtime.
- **Unspecified behavior / implementation-defined behavior**: forbidden in this specification for the minimal feature set. Where a rule has more than one acceptable outcome, this specification states the exact outcome or names the deterministic rule that selects it. Genuine gaps are escalated in `OPEN-QUESTIONS.md`, never papered over.
- **Observable behavior**: the deterministic sequence of process exit code, standard output bytes, standard error bytes (including trap records and diagnostics), file contents written through `rt.io`, and resource exhaustion outcomes of a program run, given identical inputs and resources.
- **Conforming compiler**: a compiler that accepts exactly the valid programs, rejects every invalid program with the required diagnostics, and produces deterministic outputs per Section 14 and Section 16.
- **Conformance suite**: a test corpus of valid programs (with expected behavior) and invalid programs (with expected diagnostic codes and spans).
- **Self-hosting line**: the Stage 0 / Stage 1 / Stage 2 bootstrap sequence defined in Section 16.
- **Object representation**: the bytes stored in memory for a value, including padding, per Section 9.
- **Lvalue**: an expression that denotes a storage location (variable, field, element, or dereferenced pointer) that can be read, and, if mutable, written.
- **Constant expression**: an expression whose value is computable at compile time, per Section 10.5.

### 2.3 Conformance statements

A conforming implementation must:

1. accept every valid program;
2. reject every invalid program and emit at least one diagnostic for each root cause, with the stable code and primary span required by the diagnostic contract;
3. produce deterministic output: identical inputs and options yield byte-identical artifacts (Section 14);
4. never exhibit undefined, unspecified, or implementation-defined behavior in the minimal feature set; every dynamic failure is a trap with a stable code.

A program is *portable within the initial target contract*: its observable behavior is independent of the host compiler (MSVC or LLVM Clang) used to build the Stage-0 compiler, and independent of build-time environment values, except where this specification names environmental inputs (Section 15.7).

---

## 3. Source model

### 3.1 Source files

- A source file is a sequence of bytes that must be **valid UTF-8** (no overlong encodings, no surrogate code points, no out-of-range code points).
- A UTF-8 Byte Order Mark (BOM) at the start of a file is **rejected** with diagnostic `AIC-L0002` (primary span: the BOM bytes). UTF-8 without BOM is required.
- A NUL byte (U+0000) anywhere in source outside a string literal or comment is **rejected** with `AIC-L0003`.
- Line terminators are LF (U+000A) and CRLF (U+000D U+000A). A lone CR is treated as whitespace (see Section 4.1).
- Each module is exactly one source file. The mapping from module name to file is defined in Section 8.4.

### 3.2 Unicode policy

- Identifiers and keywords are restricted to ASCII (Section 4.2).
- UTF-8 may appear in comments and string literals, and string literals carry UTF-8 semantics (Section 4.5).
- There is no Unicode normalization of any kind. Two identifiers are equal iff their ASCII bytes are equal. String literals compare byte-for-byte.

---

## 4. Lexical structure

### 4.1 Whitespace and comments

- Whitespace characters: space (U+0020), horizontal tab (U+0009), LF (U+000A), CR (U+000D), CRLF (U+000D U+000A). Whitespace separates tokens but is otherwise insignificant.
- Line comments: `//` through the end of the line (not including the line terminator).
- Block comments: `/* ... */`. Block comments do **not** nest. An unterminated block comment is **rejected** with `AIC-L0004` (primary span: from the opening `/*` to end of file).
- Comments are replaced by whitespace for tokenization purposes; they never concatenate tokens. Example: `a/**/b` is two tokens `a` and `b`, never `ab`.

### 4.2 Identifiers and reserved words

- Identifier: ASCII only — `[A-Za-z_][A-Za-z0-9_]*`.
- Identifiers are case-sensitive.
- Reserved words may not be used as identifiers:

```
module import pub
var const fn struct enum
if else while for break continue return switch case default
true false null
void bool i8 i16 i32 i64 u8 u16 u32 u64 isize usize str
sizeof alignof cast wrap len ptr
```

The type names `void bool i8 i16 i32 i64 u8 u16 u32 u64 isize usize str` are reserved words, not ordinary identifiers.

### 4.3 Integer literals

- Decimal: nonzero digit followed by decimal digits; `0` alone is the decimal zero.
- Hexadecimal: `0x` / `0X` followed by one or more hex digits.
- Binary: `0b` / `0B` followed by one or more binary digits.
- Octal: `0o` / `0O` followed by one or more octal digits.
- Digits may be separated by `_` for readability; `_` must appear between two digits. Leading, trailing, doubled, or misplaced `_` is **rejected** with `AIC-L0005`.
- Optional suffix selects the literal's type: `i8 i16 i32 i64 u8 u16 u32 u64 isize usize`.
- Unsuffixed integer literals have type:
  - the value fits in `i32` → `i32`;
  - else fits in `i64` → `i64`;
  - else fits in `u64` → `u64`;
  - else the literal is **rejected** with `AIC-L0006` (primary span: the literal).
- A suffixed literal must have its value representable in the suffix type. Exception: a signed-suffixed integer literal that is the direct operand of unary minus (grammar production `"-" unary_expr`) may have magnitude `max_abs(type)` (that is, `2^(width-1)`), and the unary-minus expression then denotes the minimum value of that signed type. This rule depends only on the grammar relation between the `-` token and the literal token, never on the absence or presence of whitespace or comments between them (Section 4.1); `-128i8`, `- 128i8`, and `-/*c*/128i8` are all the `i8` minimum, while `128i8` alone, `x - 128i8`, or `-(128i8)` are out of range because the literal is not the direct operand of unary minus. All other out-of-range literals are **rejected** with `AIC-L0006`.
- Examples: `0`, `42`, `-128i8` (valid; denotes the `i8` minimum), `255u8`, `0xFF`, `0b1010`, `0o17`, `1_000_000`, `0xFFFF_FFFF_FFFF_FFFFu64`.

### 4.4 String literals

- String literal: `"` followed by zero or more characters or escapes, followed by `"`.
- Characters inside a string literal may be any UTF-8 code point except `"` (U+0022), `\` (U+005C), and the line terminators LF/CRLF. A raw LF or CRLF inside a string literal is **rejected** with `AIC-L0007`.
- Escape sequences:

| Escape | Meaning |
|---|---|
| `\0` | NUL (U+0000) |
| `\n` | line feed (U+000A) |
| `\r` | carriage return (U+000D) |
| `\t` | horizontal tab (U+0009) |
| `\\` | backslash (U+005C) |
| `\"` | quotation mark (U+0022) |
| `\xHH` | byte with hexadecimal value HH (two hex digits exactly) |

- Any other escape sequence is **rejected** with `AIC-L0008` (primary span: the escape sequence).
- A `\xHH` escape contributes a byte to the literal's byte sequence. The complete decoded byte sequence of a string literal must be valid UTF-8; otherwise **rejected** with `AIC-L0009`.
- Adjacent string literals separated only by whitespace/comments are concatenated at compile time: `"ab" "cd"` is the string `"abcd"`. Concatenation preserves the single-literal validity rules.
- The value of a string literal has type `str` and is immutable.
- Example: `"hello\n"`, `"caf\xC3\xA9"` (the two bytes C3 A9 form U+00E9), `"tab\there"`.

### 4.5 Reserved built-in names

The following are reserved for the runtime and language and may not be declared by user programs: the conversion operators `cast` and `wrap`; the built-in functions `len` and `ptr`; the module name `rt` and its submodules; and the trap-reporting function `rt.trap.report` (Section 15.4). All names beginning with the reserved module prefix `rt.` are reserved. Reserved `rt` module names resolve to the compiler-provided runtime and never to user files; see Section 6.5.

### 4.6 Token types and lexical failures

Tokens: identifiers, keywords, integer literals, string literals, and punctuation `( ) { } [ ] ; , : . -> * & + - ~ ! / % < <= > >= == != && || = += -= *= /= %= <<= >>= &= |= ^= << >> ?` and `..` (used in slice expressions, Section 12.4).

Any other character or malformed token is **rejected** with `AIC-L0001` (primary span: the offending character or maximal malformed run). Lexical analysis never recovers silently: every lexical failure is a diagnostic.

---

## 5. Syntax: grammar

### 5.1 Grammar dialect

This specification uses ISO/IEC 14977-style EBNF:

- `A = B;` defines nonterminal A.
- `"x"` or `'x'` is a terminal.
- `A | B` alternation.
- `[ A ]` optional (zero or one).
- `{ A }` repetition (zero or more).
- `( A )` grouping.
- Nonterminals are written in lowercase; token families are named in UPPERCASE (e.g., `IDENT`, `INT_LITERAL`, `STR_LITERAL`).
- Implicit token separation: whitespace and comments may appear between any two tokens. Where this section writes a literal terminal (e.g., `;`), it denotes a token, and adjacency to an identifier means token separation is still governed by tokenization (an identifier and a keyword never merge).
- **Uniqueness requirement**: every valid program has exactly one parse tree. Where a production is written with alternatives, the alternatives are disjoint on their first tokens, are resolved by the stated longest-match lexical rule, or are resolved by a normative resolution rule stated with the production (as for `sizeof`'s type/expression operand, resolved by the single-name-space rule of Section 6.2). This requirement is normative; a conforming parser must accept exactly the language defined here and must not accept ambiguous sequences.

### 5.2 Full grammar

```
program        = module_decl { import_decl } { top_level_decl } EOF ;
module_decl    = "module" qualified_name ";" ;
import_decl    = "import" qualified_name ";" ;
qualified_name = IDENT { "." IDENT } ;
top_level_decl = [ "pub" ] ( struct_decl | enum_decl | fn_decl
               | global_var_decl | global_const_decl ) ;

struct_decl    = "struct" IDENT "{" { field_decl } "}" ;
field_decl     = IDENT ":" type ";" ;
enum_decl      = "enum" IDENT ":" int_type "{"
                 enum_member { "," enum_member } [ "," ] "}" ;
enum_member    = IDENT [ "=" const_expr ] ;
fn_decl        = "fn" IDENT "(" [ param_list ] ")" "->" type block ;
param_list     = param { "," param } ;
param          = IDENT ":" type ;
block          = "{" { statement } "}" ;

global_var_decl   = "var" IDENT ":" type "=" const_expr ";" ;
global_const_decl = "const" IDENT ":" type "=" const_expr ";" ;

statement      = block
               | var_decl | const_decl
               | if_stmt | while_stmt | for_stmt | switch_stmt
               | break_stmt | continue_stmt | return_stmt
               | expr_stmt | empty_stmt ;
var_decl       = "var" IDENT ":" type "=" expr ";" ;
const_decl     = "const" IDENT ":" type "=" const_expr ";" ;
if_stmt        = "if" "(" expr ")" block [ "else" ( block | if_stmt ) ] ;
while_stmt     = "while" "(" expr ")" block ;
for_stmt       = "for" "(" ( var_decl | const_decl | [ expr ] ";" )
                 [ expr ] ";" [ expr ] ")" block ;
switch_stmt    = "switch" "(" expr ")" "{"
                 { ( case_clause | default_clause ) } "}" ;
case_clause    = "case" const_expr ":" block ;
default_clause = "default" ":" block ;
break_stmt     = "break" ";" ;
continue_stmt  = "continue" ";" ;
return_stmt    = "return" [ expr ] ";" ;
expr_stmt      = expr ";" ;
empty_stmt     = ";" ;

type           = base_type { type_postfix } ;
base_type      = void_type | bool_type | str_type | int_type
               | named_type ;
void_type      = "void" ;
bool_type      = "bool" ;
str_type       = "str" ;
int_type       = "i8" | "i16" | "i32" | "i64"
               | "u8" | "u16" | "u32" | "u64" | "isize" | "usize" ;
named_type     = qualified_name ;
type_postfix   = "*" | "[" const_expr "]" | "[]" ;

expr           = assignment_expr ;
assignment_expr = conditional_expr [ assign_op assignment_expr ] ;
assign_op      = "=" | "+=" | "-=" | "*=" | "/=" | "%="
               | "<<=" | ">>=" | "&=" | "|=" | "^=" ;
conditional_expr = logical_or_expr [ "?" expr ":" conditional_expr ] ;
logical_or_expr = logical_and_expr { "||" logical_and_expr } ;
logical_and_expr = bit_or_expr { "&&" bit_or_expr } ;
bit_or_expr    = bit_xor_expr { "|" bit_xor_expr } ;
bit_xor_expr   = bit_and_expr { "^" bit_and_expr } ;
bit_and_expr   = equality_expr { "&" equality_expr } ;
equality_expr  = relational_expr { ( "==" | "!=" ) relational_expr } ;
relational_expr = shift_expr { ( "<" | "<=" | ">" | ">=" ) shift_expr } ;
shift_expr     = additive_expr { ( "<<" | ">>" ) additive_expr } ;
additive_expr  = multiplicative_expr { ( "+" | "-" ) multiplicative_expr } ;
multiplicative_expr = unary_expr { ( "*" | "/" | "%" ) unary_expr } ;
unary_expr     = postfix_expr
               | "-" unary_expr | "+" unary_expr | "!" unary_expr
               | "~" unary_expr | "*" unary_expr | "&" unary_expr
               | sizeof_expr | alignof_expr | cast_expr | wrap_expr
               | len_expr | ptr_expr ;
sizeof_expr    = "sizeof" "(" ( type | expr ) ")" ;
alignof_expr   = "alignof" "(" type ")" ;
cast_expr      = "cast" "<" type ">" "(" expr ")" ;
wrap_expr      = "wrap" "<" type ">" "(" expr ")" ;
len_expr       = "len" "(" expr ")" ;
ptr_expr       = "ptr" "(" expr ")" ;
postfix_expr   = primary_expr { postfix_op } ;
postfix_op     = "[" expr "]"
               | "[" [ expr ] ".." [ expr ] "]"
               | "(" [ argument_list ] ")"
               | "." IDENT
               | "->" IDENT
               | struct_init ;
primary_expr   = INT_LITERAL | STR_LITERAL
               | "true" | "false" | "null"
               | IDENT
               | array_literal
               | "(" expr ")" ;
array_literal  = "[" [ expr { "," expr } [ "," ] ] "]"
               | "[" expr ";" const_expr "]" ;
struct_init    = "{" [ field_init { "," field_init } [ "," ] ] "}" ;
field_init     = IDENT ":" expr ;
argument_list  = expr { "," expr } ;
const_expr     = assignment_expr ;  (* constrained by Section 10.5 *)
```

Notes:

- `[ expr ]` in `postfix_op` denotes an optional sub-expression between `[` and `..` / between `..` and `]`. The forms `a[..]`, `a[x..]`, `a[..y]`, `a[x..y]` are slice expressions (Section 12.4).
- A `{` that follows an expression in expression position is a struct-literal initializer (`struct_init`); a `{` at statement position is a block. No expression begins with `{` and no statement begins with anything other than a statement form, so every token sequence has exactly one parse (Section 12.7).
- Ternary `?:` is right-associative; all other binary operators are left-associative; assignment is right-associative.
- The type grammar is a base type followed by postfixes applied in the order written (left to right): `u8*[4]` is an array of 4 pointers to `u8`; `u8[4]*` is a pointer to an array of 4 `u8`; `u8*[]` is a slice of pointers to `u8`; `u8[]*` is a pointer to a slice of `u8`. Every valid type has exactly one parse: the base type is fixed first, then each postfix attaches to the type built so far.
- `sizeof`'s operand may be a type or an expression; those alternatives are not disjoint on their first tokens (both may begin with an identifier). The parse is resolved deterministically by the single-name-space rule of Section 6.2: if the operand's leading identifier or keyword denotes a type name (a primitive type keyword or a declared struct/enum type), the operand is parsed as a type; otherwise it is parsed as an expression. Because a name cannot denote both a type and a value in one scope, exactly one interpretation applies to every valid program.
- `void` is not a value type: it may appear only as a function return type. `void` in any other type position (variable, parameter, field, pointer base `void*`, array/slice element, or `cast`/`wrap` target) and `sizeof`/`alignof` on `void` are rejected (`AIC-T0306`).

### 5.3 Grammar-level rejections

Malformed constructs (unbalanced delimiters, missing `;`, missing `)`, unknown token, etc.) are **rejected** with syntax diagnostics `AIC-S0101` (expected-token) or `AIC-S0102` (unexpected-token), primary span at the offending token. A conforming parser must recover deterministically and mark recovery-derived diagnostics per the diagnostic contract.

---

## 6. Name binding, scopes, visibility, modules, and imports

### 6.1 Scopes

- **Module scope**: top-level declarations. All module-level declarations are visible throughout their own module **regardless of textual order** (module scope is the entire module). This is required to support mutually recursive functions in the self-hosting compiler without forward declarations.
- **Function body scope**: parameters and block-local declarations.
- **Block scope**: each `{ ... }` introduces a scope. A local declaration is visible from its point of declaration to the end of the enclosing block.
- **Struct field namespace**: struct fields are members of the struct type and are accessed only through `.` or `->`; they do not leak into enclosing scopes.
- **Enum member namespace**: enum members are accessed only through the enum type name (e.g., `Color.Red`); they are not injected into enclosing scopes.
- Shadowing: an inner declaration may shadow an outer declaration of the same name; this is permitted and is not an error. A name must be declared before use in its scope (except module-level declarations, per above).

### 6.2 Declaration rules

- A declaration introduces exactly one name.
- Duplicate declaration of the same name in the same scope is **rejected** with `AIC-N0201` (primary span: the later declaration's identifier).
- Use of an undeclared name is **rejected** with `AIC-N0202` (primary span: the identifier).
- Type names and value names share a single name space per scope: a struct/enum name and a variable/function/const of the same name in the same scope are a duplicate declaration.

### 6.3 Visibility

- Top-level declarations are private by default.
- The `pub` keyword makes a top-level declaration public: `pub fn`, `pub struct`, `pub enum`, `pub const`, `pub var`.
- A private top-level declaration may be used anywhere in its own module but never by other modules.
- Accessing a private item from another module is **rejected** with `AIC-N0203` (primary span: the reference).
- Struct fields have no separate visibility in the minimal language: a field is accessible wherever the struct type is accessible.
- Enum members follow the enum type's visibility.

### 6.4 Modules

- Each source file begins with exactly one `module` declaration naming its module: `module a.b.c;`. The `module` declaration must be the first non-whitespace/non-comment element; otherwise **rejected** with `AIC-S0103`.
- The module's fully qualified name is the qualified name in the `module` declaration.
- Within its own module, a program may refer to its own declarations by unqualified name or by the module-qualified name.

### 6.5 Import resolution

- `import a.b.c;` names a module, not a file.
- Module-to-file mapping (canonical, deterministic): module `a.b.c` maps to file `<project_root>/a/b/c.ai`. The project root is the single root recorded in the build manifest (Section 14.4). Example: module `lexer.tokens` → `<root>/lexer/tokens.ai`.
- **Reserved runtime modules:** module names beginning with the reserved prefix `rt` (that is, `rt` or `rt.<submodule>`) are compiler-provided and never resolve to user files. `import rt.mem;`, `import rt.io;`, `import rt.proc;`, and `import rt.trap;` bind to the project-owned runtime modules defined in Section 15; the canonical module-to-file mapping above applies only to non-reserved module names. A user module therefore cannot collide with or shadow a reserved runtime module, and no file under `<project_root>/rt/` is ever consulted for a reserved import.
- A `module` declaration whose name begins with the reserved prefix `rt` is **rejected** with `AIC-N0207` (primary span: the `module` declaration).
- An import of a reserved `rt` submodule that is not part of the runtime surface in Section 15 is **rejected** with `AIC-N0208` (primary span: the import's qualified name).
- A bare `import rt;` — importing the runtime module itself rather than one of its submodules — is **rejected** with `AIC-N0209` (primary span: the import's qualified name). The runtime surface is the four submodules of Section 15 (`rt.mem`, `rt.io`, `rt.proc`, `rt.trap`); bare `rt` binds nothing.
- Runtime members are **not auto-available**: a program must explicitly import each runtime submodule it uses (`import rt.mem;`, etc.) before referencing its members. A reference to a reserved runtime name without the matching import is resolved as an ordinary undeclared name and **rejected** with `AIC-N0202` (primary span: the reference). There is no implicit runtime import.
- Resolution of non-reserved modules depends **only** on the project root from the build manifest. It must not depend on the current working directory, environment-variable search paths, registry state, network access, or first-match behavior across multiple roots. Violation of this rule is an implementation defect, not a language behavior.
- A module that is not found at its canonical path is **rejected** with `AIC-N0204` (primary span: the import's qualified name).
- A module whose `module` declaration does not match its canonical path name is **rejected** with `AIC-N0205` (primary span: the `module` declaration).
- Importing the same module twice (directly or transitively) is not an error; the same module is compiled once and the same fully qualified name always denotes the same declaration within a build (Section 8.3).
- **Import cycles are rejected** with `AIC-N0206` (primary span: the import that closes the cycle). The diagnostic must name the cycle.
- Re-export and wildcard import are absent in the minimal language. An import binds exactly the module name written; members are referenced as `a.b.c.Name`.

### 6.6 Fully qualified names

- A fully qualified name `m.x.y` denotes: if `m` is an imported module or the current module, then the declaration `x.y` reachable within it; otherwise the field/member path is resolved per Section 6.1.
- The same fully qualified name always denotes the same declaration within one build.

---

## 7. Types

### 7.1 Primitive types

| Type | Meaning | Size (bytes) | Alignment | Notes |
|---|---|---|---|---|
| `void` | no value | — | — | function return type only |
| `bool` | boolean | 1 | 1 | values `true`/`false` only |
| `i8` | signed integer | 1 | 1 | two's complement |
| `i16` | signed integer | 2 | 2 | two's complement |
| `i32` | signed integer | 4 | 4 | two's complement |
| `i64` | signed integer | 8 | 8 | two's complement |
| `u8` | unsigned integer, the byte type | 1 | 1 | |
| `u16` | unsigned integer | 2 | 2 | |
| `u32` | unsigned integer | 4 | 4 | |
| `u64` | unsigned integer | 8 | 8 | |
| `isize` | signed pointer-sized integer | 8 | 8 | fixed to 64 bits for the initial target |
| `usize` | unsigned pointer-sized integer | 8 | 8 | fixed to 64 bits for the initial target |
| `str` | immutable UTF-8 byte sequence (data + length) | 16 | 8 | Section 12.2 |

- All integer types are exactly the stated width, two's complement, with no padding bits, on the initial target.
- `bool` is **not** an integer type and has no implicit numeric conversion.
- The initial target is little-endian Windows x86-64; endianness and integer representation are fixed by this table and are not implementation-defined.

### 7.2 Composite types

| Type | Form | Meaning | Layout (initial target) |
|---|---|---|---|
| Array | `T[N]` | fixed-size array of `N` elements of `T` | `N * sizeof(T)` bytes, alignment `alignof(T)` |
| Slice | `T[]` | bounded view: data pointer + element count | 16 bytes: pointer (offset 0), `usize` length (offset 8); alignment 8 |
| Pointer | `T*` | nullable raw pointer to `T` | 8 bytes; alignment 8 |
| Struct | `struct S { ... }` | named aggregate of fields | Section 7.4 |
| Enum | `enum E: T { ... }` | named set of constants over integer type `T` | same size/alignment as `T` |

- Arrays are values with fixed size. There is no implicit array-to-pointer decay.
- Slices are explicit views; they do not own storage.
- Raw pointers never carry an implicit length.
- Conversions among arrays, slices, pointers, and integers are never implicit (Section 11).

### 7.3 Type identity

- Two types are identical iff:
  - they are the same primitive type; or
  - they are the same named struct or enum type (same declaration); or
  - they are composite types with identical element type and identical extent (`T[N]` vs `T[N]` for the same constant `N`; `T[]` vs `T[]`; `T*` vs `T*`).
- There are no anonymous struct/enum types and no type aliases in the minimal language.

### 7.4 Struct layout

- Fields are laid out in declaration order.
- Each field is placed at the first offset (relative to struct start) that is a multiple of `alignof(field)`.
- The struct's alignment is the maximum alignment of its fields; the struct's size is rounded up to a multiple of that alignment.
- No field reordering, no bit-fields, no packing controls, no unions.
- Padding bytes: on initialization, all padding bytes of a struct value are set to zero; struct assignment copies the complete object representation (fields and padding), so padding is never indeterminate (Section 9.4).

### 7.5 Enum layout

- `enum E: T { A, B = expr, ... }` declares an enum with underlying integer type `T` (must be one of the ten integer types; `bool` is not allowed).
- Members without an explicit initializer continue the sequence: if the previous member had value `v`, the next has value `v + 1`; the first member defaults to `0`.
- The value of every member must be representable in `T`; otherwise **rejected** with `AIC-T0301`.
- Two members with the same value are permitted (aliasing constants) — this is not an error.
- An enum value's object representation is exactly the underlying integer's representation.

### 7.6 Completeness

- A struct type is incomplete until its closing brace. A pointer to an incomplete struct may be formed (`S*`), but forming a value, array, slice, field, or dereference of an incomplete struct type is **rejected** with `AIC-T0302`.
- Struct recursion through pointers is permitted; struct recursion by value is rejected (infinite size) with `AIC-T0303`.
- Enums are complete immediately after their declaration.

---

## 8. Constants, variables, and storage

### 8.1 Constant declarations

- `const name: T = const_expr;` declares an immutable compile-time constant.
- The initializer must be a constant expression (Section 10.5); otherwise **rejected** with `AIC-E0401`.
- `const` may appear at module scope and inside functions/blocks. Its scope follows Section 6.1.
- A `const` has no storage location: it is a compile-time value. Taking its address is **rejected** with `AIC-E0402`.

### 8.2 Variable declarations

- `var name: T = expr;` declares a mutable variable with an initializer.
- Every variable declaration must have an initializer. There is no uninitialized-variable state in the language; this is a hard requirement (primary span: the declaration) with `AIC-E0403`.
- Initializer types: the initializer must be assignable to `T` per Section 11.1.

### 8.3 Storage duration

- **Static storage** (module-scope `var`): storage exists for the program's lifetime; initialized before program entry by the constant initializer. There is no dynamic initialization of globals.
- **No storage for `const`**: a `const` at any scope is a compile-time value with no storage location (Section 8.1), so it has neither static nor automatic storage. Its address may not be taken (`AIC-E0402`).
- **Automatic storage** (function-local `var`): storage exists from entry into the block to exit from the block; initialized each time the declaration is executed.
- **Parameters**: automatic storage, initialized from the argument at call entry.
- **Return values**: the function's result; per Section 13.4.
- **Allocated storage** (via `rt.mem.alloc_bytes`): storage exists until released by `rt.mem.dealloc_bytes` or program exit; zero-initialized at allocation (Section 15.3).

### 8.4 Mutability and assignability

- `var` names denote mutable lvalues.
- `const` names denote immutable values; assignment to a `const` is **rejected** with `AIC-E0404`.
- Parameters are mutable locals in the minimal language (a parameter may be assigned within the function).
- Assignment to a non-lvalue is **rejected** with `AIC-E0419`.

### 8.5 Equality rules (overview)

- Integer, bool, enum, pointer, and `str` values support `==`/`!=` as defined in Section 11.4.
- Arrays and structs do **not** support `==`/`!=` in the minimal language; such a use is **rejected** with `AIC-T0304` (use loops or `rt.mem` byte comparison instead).
- Slices support element-wise `==`/`!=` (Section 12.3).

---

## 9. Values, representation, and initialization

### 9.1 Object representation

- The object representation of a value is its stored bytes, including padding.
- Integer and enum object representations are exactly the value in two's complement / the underlying integer's representation (little-endian on the initial target).
- `bool` object representation is exactly one byte: `0` for `false`, `1` for `true`. Any other byte value observed as a `bool` (e.g., through byte reinterpretation) is **rejected at compile time where statically known**, and otherwise reading a non-`0`/non-`1` byte as a `bool` is a deterministic trap `AIC-R0805` (Section 15.2). Programs cannot construct such a `bool` value through well-typed operations.
- `str` and slice object representations are the (data, length) pair as defined in Section 7.2.
- Arrays are laid out contiguously with no padding between elements (element alignment may imply padding; array size is `N * sizeof(T)` only if `sizeof(T)` is a multiple of `alignof(T)`, which holds for all initial-target types; the general formula is the sum of element sizes with per-element alignment, which this target satisfies).

### 9.2 Initialization

- Every variable is initialized by its declaration initializer (Section 8.2).
- Allocation zero-initializes the block (Section 15.3).
- Global storage is initialized from the constant initializer before entry.
- Local variables inside a loop body are re-initialized on each execution of the declaration.

### 9.3 Assignment semantics

- Assignment copies the source value into the destination storage per the object representation (Section 9.1). For arrays and structs, assignment copies the complete object representation, including padding.
- Slice assignment copies only the (data, length) pair; it never copies pointed-to elements.
- Assignment evaluation order is defined in Section 11.6.

### 9.4 Deterministic padding

- Struct padding bytes are zero on initialization and preserved on assignment. They are therefore deterministic and observable via byte-level inspection (`rt.mem.copy`/`&`-based byte access is possible through `u8*` reinterpretation, Section 11.5).

---

## 10. Expressions and operators

### 10.1 Precedence and associativity

From the grammar (Section 5.2), highest to lowest precedence:

1. postfix: `[]`, slice `[..]`, call `()`, `.`, `->`, struct literal `{...}`
2. unary: `- + ! ~ * & sizeof alignof cast wrap len ptr`
3. multiplicative: `* / %`
4. additive: `+ -`
5. shift: `<< >>`
6. relational: `< <= > >=`
7. equality: `== !=`
8. bitwise AND: `&`
9. bitwise XOR: `^`
10. bitwise OR: `|`
11. logical AND: `&&`
12. logical OR: `||`
13. conditional: `?:` (right)
14. assignment: `= += -= *= /= %= <<= >>= &= |= ^=` (right)

Chained comparisons (`a < b < c`) are **rejected** with `AIC-T0305` rather than assigned a C-like meaning; use explicit conjunction.

### 10.2 Operator semantics (static typing)

Every operator has a defined operand type rule; violations are **rejected** with `AIC-T0306` (operator not applicable to operand type), primary span at the operator.

| Operator | Operand rule | Result type | Semantics |
|---|---|---|---|
| `+ - *` | integer, integer | integer (promoted common type, §11.1) | checked arithmetic (§11.3) |
| `/ %` | integer, integer (non-zero) | integer | checked division (§11.3) |
| `-` (unary) | signed integer | same | checked negation; unsigned operand rejected (`AIC-T0306`) |
| `+` (unary) | integer | same | identity |
| `~` (unary) | integer | same | bitwise complement |
| `!` (unary) | bool | bool | logical negation |
| `&` (unary) | lvalue | `T*` | address of storage (§12.5) |
| `*` (unary) | `T*` | `T` lvalue | dereference (§12.5) |
| `<< >>` | integer, integer | integer (left operand type) | checked shift (§11.3) |
| `< <= > >=` | integer/int; enum/enum; `str`/`str`; `T*`/`T*` | bool | total ordering (§11.4) |
| `== !=` | same-type pair as above, bool/bool, enum/enum, `T*`/`T*`, `str`/`str`, slice/slice | bool | equality (§11.4) |
| `& \| ^` | integer, integer | integer | bitwise |
| `&& \|\|` | bool, bool | bool | short-circuit (§11.6) |
| `?:` | bool condition, then/else same assignable result type | common type | only chosen branch evaluated (§11.6) |
| `[]` | indexing | element type | §12.3 |
| `[a..b]` | slice expr | slice type | §12.4 |
| `()` | function call | return type | §13.4 |
| `.` | struct value / enum type | field / member | §12.6 |
| `->` | `T*` struct pointer | field | §12.6 |
| `=` and compound | lvalue = assignable value | value of lvalue type | §11.6 |
| `sizeof` | type or expr | `usize` | constant §10.5 |
| `alignof` | type | `usize` | constant §10.5 |
| `cast<T>(e)` | per conversion matrix §11.2 | `T` | checked conversion §11.5 |
| `wrap<T>(e)` | integer/enum source | `T` | truncating/modulo conversion §11.5 |
| `len(e)` | array, slice, or `str` | `usize` | element/byte count §12.2–12.3 |
| `ptr(e)` | array, slice, or `str` | element pointer / `u8*` | first-element address §12.2–12.3 |

### 10.3 Excluded expression constructs

The following C constructs are **absent** and their use is a syntax rejection (`AIC-S0102`): `++`, `--`, comma operator, `goto` and labels, assignment expressions as lvalues of comma chains, `?:` with non-bool condition, and any implicit arithmetic on pointers beyond Section 12.5.

### 10.4 Evaluation order

- **Binary operators** evaluate operands left-to-right: the left operand fully evaluates before the right operand.
- **Function call**: the callee expression (a name), then arguments left-to-right.
- **Assignment**: the destination location is evaluated before the source value (per ADR-002). For compound assignment `a op= b`: evaluate destination location, then `b`, then read the destination, apply `op`, then store.
- **Indexing `a[i]`**: evaluate `a`, then `i`.
- **Slice `a[x..y]`**: evaluate `a`, then `x`, then `y`.
- **`&&` and `||`**: evaluate left operand; if the result determines the outcome, the right operand is not evaluated (short-circuit), otherwise it is evaluated; evaluation is otherwise left-to-right.
- **`?:`**: evaluate condition; evaluate exactly one of the then/else branches.
- **Member access `.`/`->`**: evaluate the operand (for `->`, evaluate the pointer operand) to obtain the location, then access the member; no member expression is evaluated.
- **`sizeof`/`alignof`**: operand is not evaluated (only its type is considered).
- There is no unspecified evaluation order anywhere in the language.

### 10.5 Constant expressions

A constant expression is an expression composed only of:

- integer literals and string literals;
- `true`, `false`, `null`;
- `const` names declared at module scope or enclosing scopes;
- enum members (`Color.Red`);
- `sizeof`/`alignof`;
- `&` of a static-storage lvalue (a global `var`), `&arr[0]` of a static array, and slice expressions of static arrays with constant bounds;
- unary and binary operators on constant expressions, provided every operation is valid and checked at compile time (Section 11.3);
- `cast`/`wrap` of constant expressions (Section 11.5).

A violation of const-ness is **rejected** with `AIC-E0401`. Constant expressions are evaluated exactly as their runtime semantics define, but at compile time; overflow, division by zero, invalid shift, and out-of-range casts in constant expressions are compile-time rejections (not traps): `AIC-E0405` (constant overflow), `AIC-E0406` (constant division by zero), `AIC-E0407` (constant shift out of range), `AIC-E0408` (constant cast out of range).

---

## 11. Conversions and operator typing

### 11.1 Implicit conversions (only these)

Implicit conversions are permitted only where the source type's full value range is representable in the destination type (value-preserving widening):

| From | To |
|---|---|
| `i8` | `i16 i32 i64 isize` |
| `i16` | `i32 i64 isize` |
| `i32` | `i64 isize` |
| `i64` | `isize` (identical width/sign) |
| `u8` | `u16 u32 u64 usize i16 i32 i64 isize` |
| `u16` | `u32 u64 usize i32 i64 isize` |
| `u32` | `u64 usize i64 isize` |
| `u64` | `usize` (identical width/sign) |
| `isize` | `i64` (identical width/sign) |
| `usize` | `u64` (identical width/sign) |
| any `T*` | `T*` (same type) |
| `null` | any `T*` |

- Implicit conversions apply in: initializers, assignments, argument passing, return values, and binary operator common-type promotion.
- Binary operator common-type promotion: when two operands of an integer binary operator have different types, both are implicitly converted to the common type if and only if both conversions appear in the table above (i.e., the common type is the wider of the two, and both values fit). If no such common type exists, the expression is **rejected** with `AIC-T0307` (no common type; use an explicit cast).
- No narrowing, sign-changing, integer/bool, pointer/integer, array/pointer, or slice/pointer conversion is ever implicit.

### 11.2 Explicit conversions

All other conversions are explicit and named. The minimal language provides two conversion families:

- `cast<T>(expr)`: checked conversion; the value must be representable in `T`, otherwise a compile-time rejection in constant contexts (`AIC-E0408`) or a runtime trap `AIC-R0801` (Section 15.2).
- `wrap<T>(expr)`: wrapping/truncating conversion; defined modulo arithmetic (Section 11.5).

The conversion matrix:

| From | To | Mechanism |
|---|---|---|
| integer | integer (same sign, widening) | implicit (Table 11.1) or `cast` |
| integer | integer (narrowing, any sign change) | `cast` (checked) or `wrap` (truncating) |
| `bool` | `bool` | identity |
| `bool` | integer | `cast` (checked; `false`→0, `true`→1) |
| integer | `bool` | `cast` (checked; 0→`false`, 1→`true`, other → out-of-range) |
| enum | underlying integer | `cast` (always representable; checked identity) |
| integer | enum | `cast` (checked: value must equal a declared member value) |
| enum | enum (different) | `cast` (via integer semantics, checked) |
| `T*` | `usize` / `u64` / `isize` / `i64` | `cast` (address value; checked for signed representability) |
| integer | `T*` | `cast` (checked for alignment on dereference; value preserved) |
| `T*` | `U*` | `cast` (bit-preserving; alignment obligation on dereference §12.5) |
| array | slice | explicit slice expression only (Section 12.4) |
| slice | slice of same element type | identity (no conversion needed) |
| `str` | `u8[]` | `cast` (reinterpret same data/len) |
| `u8[]` | `str` | `cast` (checked: bytes must be valid UTF-8, else trap `AIC-R0806`) |
| `null` | any `T*` | implicit (Table 11.1) |

No other conversion pair exists; any other cast is **rejected** with `AIC-T0308`.

### 11.3 Checked arithmetic and failure semantics

- Default signed and unsigned integer arithmetic is **checked**:
  - signed addition, subtraction, multiplication: overflow → compile-time rejection for constants (`AIC-E0405`) or runtime trap `AIC-R0802`;
  - unsigned addition, subtraction, multiplication: wrap-around → same handling (unsigned arithmetic does not silently wrap);
  - division/remainder: divisor zero → `AIC-E0406` (constant) or trap `AIC-R0803`; signed `min / -1` and `min % -1` → overflow rejection/trap (`AIC-E0405` / `AIC-R0802`);
  - unary negation on signed minimum → same overflow handling;
  - shifts: shift count must be in `0 .. width-1`; otherwise `AIC-E0407` (constant) or trap `AIC-R0804`. Left shift of signed values is defined on the two's-complement bit pattern (no overflow trap for shifts themselves; the resulting bits are the mathematical value of `(unsigned)x << n` re-read as the signed type). Right shift of signed values is arithmetic (sign-extending); right shift of unsigned is logical.
- `wrap<T>(expr)` is an explicit modulo/truncating **conversion** only (ADR-004). Its operand is evaluated under the ordinary checked-arithmetic rules above; `wrap<T>` does not establish a wrapping evaluation context. The expression `wrap<T>(a op b)` must not be described as supplying same-width wrapping arithmetic: `a op b` is a checked operation, so overflow, division by zero, or an invalid shift inside the operand rejects (constants) or traps before the conversion applies.
- The minimal language provides **no same-width wrapping addition, subtraction, multiplication, or negation operation** (ADR-004, which supersedes ADR-002's requirement that same-width explicit wrapping arithmetic exist). Dedicated wrapping arithmetic built-ins are deferred: the decision must be reopened if bootstrap or self-hosting work demonstrates a concrete need, and it may be reconsidered later independently. Adding such operations is a public language-surface change requiring an accepted ADR and conformance coverage.

### 11.4 Comparison semantics

- Integer comparisons: mathematical value comparison on the actual type's range.
- Enum comparisons: by underlying integer value; mixed enum types require an explicit cast.
- Pointer comparisons (`== != < <= > >=`): compare byte addresses as unsigned integers. Relational pointer comparison between pointers that do not point into the same object is **defined** (address ordering) — it is not undefined behavior in this language, but only `==`/`!=` between pointers into the same object is *meaningful*; the language does not diagnose meaningless orderings (raw-pointer caveat, Section 12.8).
- `str` comparisons: lexicographic byte-by-byte comparison of the UTF-8 byte sequences (byte ordering, not code point ordering; valid UTF-8 byte order matches code point order for BMP and above within a code point's own bytes, but the language defines byte comparison explicitly).
- `bool` `==`/`!=`: by value.
- Slices `==`/`!=`: element-wise, using the element type's equality; length mismatch → not equal.

### 11.5 `cast` and `wrap` details

- `cast<T>(expr)` where T is a scalar/pointer/str-compatible type per the matrix in 11.2. Checked conversions verify representability at compile time (constants) or runtime (trap `AIC-R0801` / `AIC-R0806`).
- `wrap<T>(expr)` is defined only for integer targets and integer or enum sources: the mathematical value is reduced modulo `2^width` and re-read as the target type (two's complement). `wrap` is never checked and never traps.
- `cast<u8[]>(s)` reinterprets the `str`'s (data, length) as a `u8[]`; the length is in bytes.
- `cast<str>(b)` checks that the byte slice is valid UTF-8; failure is a trap `AIC-R0806` (runtime) or rejection (constant, `AIC-E0408`).

### 11.6 Assignment and compound-assignment typing

- `a = v`: `a` must be a mutable lvalue; `v` must be assignable to `typeof(a)` (identical type or per Table 11.1; or `null` to `T*`). Otherwise **rejected** with `AIC-T0307`.
- `a op= v`: equivalent to `a = a op v` with destination-location evaluation order (Section 10.4). The compound operator must be valid for the lvalue type; pointer `+=`/`-=` are pointer arithmetic (Section 12.5).

---

## 12. Arrays, slices, str, pointers, indexing, and memory

### 12.1 Arrays

- `var a: T[N] = initializer;` where `N` is a constant expression of type `usize`-compatible integer ≥ 0. `N == 0` is permitted (empty array).
- Array initializer: array literals use square brackets — `[e0, e1, ..., eN-1]` (element list; the count must equal `N`) or `[e; N]` (repetition form). A literal whose count does not equal the declared size is **rejected** with `AIC-T0309`.
- Arrays are values: they may be assigned (full copy), passed by value to functions (full copy), and returned by value.
- Array indexing `a[i]` is bounds-checked: `0 <= i < N`; out-of-range constant index → `AIC-E0409`; runtime → trap `AIC-R0807`.
- `sizeof(a)` = `N * sizeof(T)`; `alignof(a)` = `alignof(T)`.

### 12.2 str

- `str` is an immutable UTF-8 byte sequence represented semantically as (data pointer, byte length). It is a value type with the layout in Section 7.2.
- String literals have type `str`.
- `str` indexing `s[i]` yields `u8` (byte value) with bounds checking (same rules as arrays; trap `AIC-R0807`). Indexing is by byte offset, not code point.
- `s.len` is not a member access in the minimal language (the fields of `str` are not exposed). Length and data are obtained only through the built-in functions:
  - `len(s) -> usize` for `str`, `len(a) -> usize` for arrays, and `len(sl) -> usize` for slices: the byte count (for `str`) or element count (for arrays/slices);
  - `ptr(s) -> u8*`, `ptr(a) -> T*`, and `ptr(sl) -> T*`: the address of the first element. `ptr` of an empty array, an empty slice, or a zero-length `str` returns `null` (documented; there is no separate non-null sentinel).
  - These built-ins are reserved names (Section 4.5).
- `str` slicing (Section 12.4) must produce byte offsets that fall on UTF-8 code point boundaries; otherwise trap `AIC-R0808` (runtime) / rejection (constant `AIC-E0410`).

### 12.3 Slices

- `T[]` is a bounded view: data pointer + element count. It does not own storage.
- Creating a slice:
  - slice expression on an array: `a[..]` (whole array), `a[x..]`, `a[..y]`, `a[x..y]` (bounds checked against `N`);
  - slice expression on a slice: `s[..]`, `s[x..]`, `s[..y]`, `s[x..y]` (bounds checked against `len(s)`);
  - slice expression on `str`: yields `str` (code point boundary rule, Section 12.2);
  - a slice variable initialized from another slice (copies the pair);
  - no conversion from array to slice is implicit (Section 11.2).
- Slice bounds: `0 <= x <= y <= extent`; otherwise constant → `AIC-E0409`, runtime → trap `AIC-R0807`.
- Slice indexing `s[i]` is bounds-checked against `len(s)`; trap `AIC-R0807`.
- Slice equality (Section 11.4) is element-wise.
- `len(s)` returns the element count; `ptr(s)` returns the data pointer.

### 12.4 Slice expression syntax

- `a[..]`, `a[x..]`, `a[..y]`, `a[x..y]` per grammar `postfix_op`. `x` and `y` must be integers (implicit conversion to `usize` permitted per Table 11.1). Negative bounds are rejected statically when constant (`AIC-E0409`) and trap otherwise.

### 12.5 Pointers

- `T*` is a nullable raw pointer. `null` is the null pointer value; dereferencing `null` is a trap `AIC-R0809`.
- `&expr` (address-of) requires an lvalue operand and yields `T*`. Address of a `const` or of a non-lvalue is **rejected** (`AIC-E0402`).
- `*p` dereferences; the result is a mutable lvalue of type `T`.
- Pointer arithmetic:
  - `p + i`, `p - i` (i integer, any integer type; scaled by `sizeof(T)`); `p += i`, `p -= i`. Result type `T*`. The address computation is **checked**: the scaling product `i * sizeof(T)` and the resulting byte address `byte_address(p) ± (i * sizeof(T))` must both be representable in the address arithmetic without overflow. Overflow is **rejected at compile time when the expression is constant** (`AIC-E0405`) and traps at runtime (`AIC-R0816`, pointer arithmetic overflow).
  - `p - q` (both `T*`): result `isize`, defined as the exact quotient `(byte_address(p) - byte_address(q)) / sizeof(T)`. The byte difference is computed as a signed `isize` value; if it is not representable (byte-difference overflow), the expression is **rejected at compile time when both operands are constant** (`AIC-E0405`) and traps at runtime (`AIC-R0816`). If the byte difference is not a multiple of `sizeof(T)`, the expression is **rejected at compile time when both operands are constant** (`AIC-E0411`) and traps at runtime (`AIC-R0810`). This keeps the operation total and deterministic.
  - Pointer relational comparison per Section 11.4 (address ordering).
- Dereference obligations (raw pointer caveat, Section 12.8): the pointed-to address must be aligned to `alignof(T)` and must be accessible; violating alignment or access causes a hardware-fault-derived deterministic trap `AIC-R0811` (invalid address/alignment) where detectable, and where not detectable the program has violated the raw-pointer contract documented in Section 12.8. The language does **not** claim complete memory safety; it requires deterministic rules and traps for every operation the runtime can observe, and explicitly documents the residual raw-pointer contract.
- Conversions: `cast<T*>(usize_value)` preserves the value; alignment is checked when the pointer is dereferenced. `cast<usize>(p)` yields the address.

### 12.6 Member access

- `s.field` for struct values; `p->field` ≡ `(*p).field` for `T*` to a struct (only when `T` is a struct type; otherwise rejected `AIC-T0306`).
- `E.Member` for enum types.
- Struct field access requires a complete struct type.

### 12.7 Struct literals

- A struct literal names its type explicitly and is written `TypeName { f1: e1, f2: e2 }` (grammar `struct_init` applied as a postfix to the type name, Section 5.2). The type name must denote a complete struct type. There is no anonymous struct literal and no type inference: a brace-initializer without a preceding type name in expression position is a syntax error (`AIC-S0101`), and a `struct_init` attached to an expression that does not denote a struct type is **rejected** with `AIC-T0306`.
- In statement position a `{` is always a block (Section 5.2), so a struct literal can never be confused with a block.
- Every field of the struct must appear exactly once, in any order; the value is laid out in declaration order.
- A missing field, a duplicate field, or an unknown field name is **rejected** with `AIC-T0313` (primary span: the offending literal).
- Field initializers are expressions evaluated left-to-right in literal order (Section 10.4); the resulting struct value's padding is zeroed (Section 9.4).

### 12.8 Raw-pointer contract (normative caveat)

The minimal language provides explicit raw pointers without a complete spatial or temporal safety model. The normative contract is:

1. Every operation on a pointer (dereference, arithmetic, comparison, conversion) has defined semantics in this specification or a deterministic trap.
2. The runtime guarantees traps for: null dereference (`AIC-R0809`), bounds violations on arrays/slices/str (`AIC-R0807`), pointer arithmetic overflow (`AIC-R0816`), double release (`AIC-R0812`), invalid release (`AIC-R0813`), and byte-misalignment/access violations it can observe (`AIC-R0811`).
3. **Accepted temporal baseline (ADR-004, Human Sponsor approval):** the project-owned allocator tracks every allocation and deterministically traps duplicate release and release of a pointer that is not the start of a live allocation. Deallocation overwrites the full allocation with byte `0xDD` before the block becomes eligible for deterministic reuse. Freed blocks remain under the project allocator's controlled address space until deterministic reuse or process exit; the allocator must not expose host-allocator or OS-dependent stale-access semantics as a language rule (Section 15.1). Within this model:
   - before reuse, a stale access to still-accessible freed storage observes or modifies the poisoned (`0xDD`) bytes at that address;
   - after deterministic reuse, the address denotes the new allocation and a stale pointer accesses that current allocation according to ordinary raw-pointer rules;
   - an address that is not accessible according to the runtime's controlled region model traps as invalid address/access (`AIC-R0811`);
   - the minimal runtime does **not** guarantee that every stale access through an invalidated pointer traps; stronger temporal checking is a future safety feature requiring an accepted ADR.
4. **Compiler obligations:** the compiler may not assume that a stale pointer access is unreachable, may not optimize on a C-style lifetime undefined-behavior assumption, and may not produce arbitrary behavior from such an access. A stale access is a defined raw-pointer operation with the deterministic outcomes above.
5. **No temporal-safety claim:** the language explicitly does not claim complete temporal memory safety. Documentation and diagnostics must state this limit prominently; no memory-safety claim is permitted.

---

## 13. Statements and control flow

### 13.1 Blocks

- A block is a sequence of statements. Blocks introduce scope (Section 6.1).
- All controlled statement bodies use braces: `if`, `while`, `for`, and each `switch` case body is a brace-delimited block (grammar `case_clause` / `default_clause`). A single statement without braces is **rejected** with `AIC-S0104` (expected `{`).

### 13.2 Selection

- `if (cond) block [else block|if]`: `cond` must be `bool` (`AIC-T0310` if not). The `else` binds to the nearest unmatched `if`.
- `switch (expr) { case ... }`: the selector must be an integer or enum type (`AIC-T0311`). Each `case` label is a constant expression of the selector's type (after promotion; enum selector requires enum constant members). The selector is evaluated once, left-to-right with no other operand evaluation.
- **No fall-through**: each case body is a brace-delimited block, and the block's final statement must be a terminator — `break`, `return`, `continue` (if inside a loop), or a call to a noreturn function (`rt.proc.exit`, `rt.trap.report`) — otherwise **rejected** with `AIC-E0412`. A case block with no terminating statement is rejected, including an empty block `{}`; `case x: { break; }` is valid because `break;` is a statement.
- `default` is optional; if present it may appear once, in any position among cases (position is semantically irrelevant; cases are matched in source order, then default).
- Duplicate case values are **rejected** with `AIC-E0413`.
- `break` inside a switch exits the switch. `break`/`continue` are otherwise valid only inside a loop (`AIC-E0414`).
- `continue` inside a switch that is itself inside a loop continues the loop (the switch's selector/cases are not re-entered).

### 13.3 Iteration

- `while (cond) block`: `cond` must be `bool`. Evaluate cond; if `true`, execute block, repeat.
- `for (init; cond; step) block`: `init` may be a declaration (`var`/`const`, scoped to the for statement), an expression (evaluated once), or absent; `cond` must be `bool` or absent (absent = `true`); `step` is an expression evaluated after each iteration. Semantics: evaluate `init`; then repeatedly evaluate `cond` (if present), and if `true` execute the block, then evaluate `step`; until `cond` is `false` (or absent, forever unless `break`/`return`).
- `break` exits the innermost loop; `continue` jumps to the loop's step (for) or condition (while), skipping the rest of the body.

### 13.4 Functions and calls

- `fn name(params) -> T { body }`: exactly one return type, which may be `void`.
- Parameters: `name: Type`, comma-separated; no default arguments, no variadics, no overloading.
- Calls: `name(args)` — the callee must be a function name (no function pointers in the minimal language; see the function-pointer evaluation in Section 17.3 and the resolution record in `OPEN-QUESTIONS.md`). Arguments are evaluated left-to-right and assigned to parameters (assignability per Table 11.1). The number of arguments must match the parameter count (`AIC-T0312`).
- Return: `return expr;` in non-`void` functions (expression required, `AIC-E0415`); `return;` or bare block-end in `void` functions.
- **Reachability**: every reachable path in a non-`void` function must end in `return`; otherwise **rejected** with `AIC-E0416`. A `void` function may fall off the end.
- Recursion is permitted.
- Functions may be mutually recursive at module scope (Section 6.1).
- Nested function definitions are **absent** (`AIC-S0102`).

### 13.5 Reachability rules

- Statement after `return`, `break`, `continue`, or a noreturn call in the same block is unreachable and **rejected** with `AIC-E0417`.
- A non-`void` function's body must satisfy: every block in the function that is reachable and not the function tail must end in a terminator, and the tail must be `return`; `if`/`else` merge paths conservatively; loops are treated as potentially non-terminating (a function whose only return is inside a loop must still have a terminating path; `while (true)`/`for (;;)` with no `break` is considered non-terminating and satisfies the tail requirement if it cannot reach the tail).

---

## 14. Compiler pipeline and observable contracts

This section states what a conforming compiler must produce and how it must behave. It does not prescribe implementation techniques.

### 14.1 Pipeline stages (observable boundaries)

The compiler implements the accepted stage architecture (ADR-001) with these observable contracts:

1. **Source loading and UTF-8 validation** — rejects invalid UTF-8, BOM, NUL (Section 3.1; codes `AIC-L0002`, `AIC-L0003`, and `AIC-L0001` for byte-sequence errors).
2. **Lexer** — produces the token stream per Section 4; every lexical failure is a diagnostic.
3. **Parser** — produces the single parse tree per Section 5; rejects ambiguous or malformed input (`AIC-S01xx`).
4. **Name and type resolution** — per Sections 6, 7; rejects name/visibility/module errors (`AIC-N02xx`).
5. **Semantic validation** — per Sections 8–13; rejects type, const, reachability, and control-flow errors (`AIC-T03xx`, `AIC-E04xx`).
6. **Canonical IR** — a target-neutral intermediate representation. Specification obligations at the boundary:
   - the IR must preserve source spans and the causal chain of each construct;
   - the IR must be deterministic (identical source → identical IR);
   - the IR must be target-neutral (no x86-64 specifics);
   - the IR is **not** specified in this document; its instruction set is an implementation/architecture matter for the implementation planning phase. The spec boundary is: every semantic rule in this document must be representable and enforceable in the IR.
7. **x86-64 backend** — deterministic code generation; instruction baseline no greater than x86-64 + SSE2 (no AVX2 or higher required) per ADR-001.
8. **COFF object emission** — deterministic COFF objects per Section 14.2.
9. **Link step** — during the initial bootstrap line, an external linker (`link.exe` or `lld-link`) performs final linking; the build entry point must invoke it by explicit path or an initialized developer environment, never a bare ambiguous `link` from Git Bash (ADR-001).
10. **Diagnostic rendering and build manifest emission** — per the diagnostic contract and Section 14.4.

### 14.2 Determinism obligations

- Identical inputs (source files, build manifest, options) must yield byte-identical artifacts: COFF objects and, after link, the executable. There is **no permitted normalization** of any compiler-produced artifact (Section 16.2; ADR-001).
- The compiler must not embed: timestamps, random identifiers, absolute host paths (paths relative to the project root are permitted), environment values, iteration order of unordered containers that affects output, or build-machine identity.
- Diagnostic output ordering is deterministic per the diagnostic contract (primary span order, then code).
- The compiler itself must be deterministic under repetition: running the same build twice on the same inputs produces the same outputs and the same diagnostics.

### 14.3 COFF/PE target obligations (initial target)

- Target: little-endian Windows x86-64 using PE/COFF conventions.
- Generated instruction baseline: x86-64 plus SSE2; AVX2 and other host-specific features must not be required.
- COFF objects must be deterministic (Section 14.2).
- Executables must link with at least one accepted external linker (`link.exe` or `lld-link`) and run on the environment baseline machine (Intel Haswell, Windows 10 22H2 x64; see `ENVIRONMENT_BASELINE_2026-08-08.md`). Windows 10 22H2 is the pinned development-host and execution baseline for the bootstrap line despite the absence of further OS updates. Compatibility with newer Windows versions is desirable but is not a v0.1.0 conformance guarantee.
- Runtime-facing Windows calls used by the project-owned runtime must be minimized, enumerated, and documented against this pinned baseline (ADR-004). The runtime's Windows API dependency surface is part of the implementation contract but is bounded and documented so later supported-version testing is possible; host- or OS-dependent behavior must not be exposed as a language rule.
- The exact COFF/PE encoding is delegated to later implementation contracts; this section fixes the observable obligations only.

### 14.4 Build manifest

A build produces a build manifest recording:

- schema version of the manifest;
- project root (the single import-resolution root);
- entry module name and its source path;
- complete module list compiled (with source file paths relative to project root);
- the AI-Co language/specification version string (identical across all bootstrap stages; this is the manifest's only version/identity field);
- build options that affect output (in normalized, sorted form);
- the external linker flag set used, when linking (Section 16.3; identical across the compared stage builds);
- output artifact paths (relative to project root) and SHA-256 hashes of each artifact;
- diagnostic summary (counts by severity, stable codes emitted);
- exit status.

The manifest must itself be deterministic and must not contain absolute host paths.

**Hashed artifact set and self-hash exclusion (FIND-G2-02):** the SHA-256 hashes recorded in the manifest cover every compiler-produced artifact named in "output artifact paths" **except the manifest itself**. The manifest does not hash itself and contains no self-referential hash field; a manifest's own identity is established by byte comparison of the manifest file (Section 16.2), never by a hash inside it. The exact hashed artifact set is therefore: every COFF object file produced by the build and, after the link step, the linked executable. No fixed-point or exclusion-by-zeroing rule is needed because the manifest is excluded from its own hash list.

**Stage invariance (FIND-G2-03):** the manifest fields are defined so that the Stage 1 and Stage 2 manifests for the same AI-Co compiler source, inputs, and options are byte-identical (Section 16.2). The only version/identity field is the AI-Co language/specification version string, which is identical across stages. The identity of the producing executable (e.g., the Stage 0 host C compiler versus the Stage 1 AI-Co compiler) is **not** recorded in the manifest; host compiler identity/version and linker identity/version are recorded only in the external comparison evidence (Section 16.3), never in the manifest. Output artifact paths are recorded relative to the project root, and the byte-identity comparison is defined over builds that write to **identical output artifact paths**; the M1 identity comparison therefore invokes both stage builds with the same relative output paths (Section 16.5). No normalization of manifest fields is permitted (Section 16.2).

---

## 15. Minimal runtime and system contract

The runtime is project-owned and small (ADR-002). It is exposed as the reserved module `rt` with submodules `rt.mem`, `rt.io`, `rt.proc`, and `rt.trap`. All functions are deterministic; all failures are either defined return values or named traps.

### 15.1 Module `rt.mem` (allocation and bytes)

- `rt.mem.alloc_bytes(count: usize) -> u8*` — allocates `count` zero-initialized bytes; returns `null` if allocation fails (explicit result value, per ADR-002's allowance). The returned pointer is aligned to at least `alignof(max_align)` for the target (16 bytes); the exact allocation alignment is documented in the implementation contract. Allocates storage with static-like lifetime until release.
  - **Zero-size allocation:** `alloc_bytes(0)` performs no allocation, consumes no allocator state, and returns `null`. It is not a failure and never traps; deallocating the returned value is the documented `null` no-op. There is nothing to release for a zero-size request.
- `rt.mem.dealloc_bytes(p: u8*) -> void` — deallocates an allocation previously returned by `alloc_bytes`. Passing `null` is a no-op. Releasing a pointer not returned by `alloc_bytes`, or releasing the same allocation twice, is a trap `AIC-R0813` (invalid release) / `AIC-R0812` (double release).
  - **Deterministic reuse rule (ADR-004):** before a freed block becomes eligible for reuse, the allocator overwrites the full allocation with byte pattern `0xDD`. Freed blocks remain under the allocator's control until deterministic reuse or process exit; the allocator never returns them to a host allocator in a way that exposes host-allocator or OS-dependent stale-access semantics as a language rule. Reuse is **exact-fit**: a freed block of size `S` may satisfy only a request of size `S`; the allocator never splits a larger freed block to satisfy a smaller request and never coalesces adjacent freed blocks. Among free blocks of the same size, the block most recently released is reused first (reverse order of release within a size class). If no free block of the exact requested size exists, `alloc_bytes` obtains a fresh block from the allocator's controlled region. The reuse outcome — which address (if any) is returned for a given allocation/release sequence — is part of the observable contract: it must not depend on host-allocator behavior, timing, environment values, or build options.
  - **Resource exhaustion:** if `alloc_bytes` cannot satisfy a request, it returns `null` (an explicit result value, per ADR-002). It never traps for exhaustion, never returns an address outside the allocator's controlled region, and never violates the deterministic reuse rule. Exhaustion is an environmental input (Section 15.6), not language ambiguity.
- `rt.mem.copy(dst: u8*, src: u8*, count: usize) -> void` — copies `count` bytes; behavior is defined for overlapping regions as if a temporary buffer were used (deterministic; callers must ensure dst/src point to at least `count` accessible bytes, else trap `AIC-R0811`).
- `rt.mem.fill(dst: u8*, value: u8, count: usize) -> void` — fills `count` bytes with `value`.
- Byte operations on `str`/slices use these primitives; there is no separate string library in the minimal runtime.

### 15.2 Module `rt.io` (file I/O)

- File handles are `usize` values (runtime-managed indices). `0` is invalid and never returned by a successful operation.
- `rt.io.open(path: str, mode: u32) -> usize` — opens a file. `mode`: `0` = read, `1` = write (truncate), `2` = write (append), `3` = read/write (create/truncate). Returns a handle, or `0` on failure (explicit result value).
- `rt.io.read(handle: usize, buf: u8[], count: usize) -> usize` — reads up to `count` bytes into `buf` (must have `len(buf) >= count`, else trap `AIC-R0807`); returns the number of bytes read (0 at EOF). Invalid handle → trap `AIC-R0814`.
- `rt.io.write(handle: usize, buf: u8[], count: usize) -> usize` — writes up to `count` bytes from `buf`; returns bytes written. Invalid handle → trap `AIC-R0814`.
- `rt.io.close(handle: usize) -> void` — closes; closing an invalid or already-closed handle is a trap `AIC-R0814`.
- `rt.io.stdin() -> usize`, `rt.io.stdout() -> usize`, `rt.io.stderr() -> usize` — returns the standard stream handles (never `0`).
- I/O operates on bytes; no text translation, no buffering semantics beyond what the OS provides (deterministic per run).

### 15.3 Module `rt.proc` (process)

- `rt.proc.args() -> u8[][]` — process arguments as UTF-8 byte slices. `args()[0]` is the program path (Windows: the executable path as provided); the remainder are command-line arguments. Argument bytes are converted from the platform encoding (Windows UTF-16 command line) to UTF-8; conversion is deterministic and lossless for valid Unicode (invalid surrogate handling is a documented implementation contract; the bytes produced must still be valid UTF-8 — invalid surrogates are replaced deterministically with U+FFFD).
- `rt.proc.exit(code: i32) -> void` — terminates the process with the given exit code. This function never returns; it is a noreturn function for reachability purposes (Section 13.4).
- Normal return from `main` produces exit code `0` if `main` returns `0` (or is `void`), else the returned `i32`. The entry-point contract: the entry module must declare `fn main() -> i32` (or `fn main() -> void`, which is equivalent to returning 0). Missing or mis-typed `main` in the entry module is **rejected** with `AIC-E0418` at link/entry validation.

### 15.4 Module `rt.trap` (trap reporting)

- `rt.trap.report(code: u32, message: str) -> void` — raises a named user trap with the given code and message; never returns. Trap records are reported per the diagnostic contract; the process exits with code `70` (trap exit code).

### 15.5 Traps (summary)

| Trap | Code | Exit code |
|---|---|---|
| checked conversion out of range | `AIC-R0801` | 70 |
| arithmetic overflow | `AIC-R0802` | 70 |
| division by zero | `AIC-R0803` | 70 |
| shift count out of range | `AIC-R0804` | 70 |
| invalid `bool` byte | `AIC-R0805` | 70 |
| invalid UTF-8 → `str` | `AIC-R0806` | 70 |
| index/span out of bounds | `AIC-R0807` | 70 |
| str slice not on code point boundary | `AIC-R0808` | 70 |
| null dereference | `AIC-R0809` | 70 |
| pointer difference not divisible | `AIC-R0810` | 70 |
| invalid address/alignment (memory access) | `AIC-R0811` | 70 |
| double release | `AIC-R0812` | 70 |
| invalid release | `AIC-R0813` | 70 |
| invalid file handle / closed handle | `AIC-R0814` | 70 |
| stack exhaustion (unbounded recursion) | `AIC-R0815` | 70 |
| pointer arithmetic overflow (checked scaling / byte-difference overflow) | `AIC-R0816` | 70 |
| user trap via `rt.trap.report` | caller-supplied code | 70 |

Stack exhaustion: recursion that exhausts the available stack is a deterministic trap `AIC-R0815`. The stack limit is a resource bound declared in the build manifest (default: 8 MiB); exceeding it is a trap, not undefined behavior.

### 15.6 Environmental inputs

File contents, available memory, and system-provided arguments are environmental inputs: they influence observable behavior deterministically but are not language ambiguity (ADR-002). Given identical inputs and identical environmental inputs, behavior is identical.

### 15.7 Calling convention and ABI obligations (internal contract)

- The compiler and the project-owned runtime use the Microsoft x64 calling convention **internally**: integer and pointer arguments in RCX, RDX, R8, R9 (left to right); additional arguments on the stack, right to left; 32-byte shadow space reserved by the caller; stack 16-byte aligned at call sites; return value in RAX; callee-saved registers RBX, RBP, RDI, RSI, R12–R15; no floating-point arguments exist in the minimal language.
- This convention is an **internal implementation contract**. It is not a source-language guarantee, does not constitute a foreign-function interface, and does not promise Windows ABI compatibility (ADR-002). The compiler is free to use any internal calling convention for AI-Co functions; the convention above binds only the compiler-to-runtime call boundary and the runtime's exposed functions.
- ABI/layout obligations: runtime-visible types follow the layout rules of Section 7 (`File` handle is `usize`; byte slices and `str` follow Section 7.2 layouts); the runtime must honor the alignment obligations of Section 7 for all memory it returns to programs.
- Noreturn functions: `rt.proc.exit` and `rt.trap.report` never return; the compiler must treat calls to them as terminators for reachability (Section 13.5) and must not emit code after them.

### 15.8 Compiler-emitted runtime calls

The runtime API of Sections 15.1–15.4 is **source-visible**: programs may call any listed function directly. The table below is the complete normative list of runtime calls a conforming compiler may emit **implicitly** for language-defined behavior (checked operations, traps, allocation, and I/O). A call outside this table emitted implicitly is a specification defect unless added by a later accepted contract.

Functions listed in Sections 15.1–15.4 that do not appear in the table below — specifically `rt.io.stdin`, `rt.io.stdout`, and `rt.io.stderr` — are source-visible only: programs may call them, but the compiler never emits them implicitly. Every table entry is a source-visible runtime function; the table is the complete subset that the compiler may emit implicitly.

| Purpose | Emitted runtime call | Notes |
|---|---|---|
| heap allocation | `rt.mem.alloc_bytes(count: usize) -> u8*` | zero-initialized; `null` on failure |
| heap release | `rt.mem.dealloc_bytes(p: u8*)` | no-op on `null`; traps on double/invalid release |
| byte copy | `rt.mem.copy(dst: u8*, src: u8*, count: usize)` | overlap-safe |
| byte fill | `rt.mem.fill(dst: u8*, value: u8, count: usize)` | |
| file open | `rt.io.open(path: str, mode: u32) -> usize` | `0` on failure |
| file read | `rt.io.read(handle: usize, buf: u8[], count: usize) -> usize` | EOF → 0 |
| file write | `rt.io.write(handle: usize, buf: u8[], count: usize) -> usize` | bytes written |
| file close | `rt.io.close(handle: usize)` | |
| process args | `rt.proc.args() -> u8[][]` | |
| process exit | `rt.proc.exit(code: i32)` | noreturn |
| trap report | `rt.trap.report(code: u32, message: str)` | noreturn; used for every language-defined runtime trap (checked arithmetic, bounds, pointer arithmetic overflow, null dereference, conversion failures, invalid UTF-8, stack exhaustion, etc.) |

Checked operations that can fail at runtime (Section 11.3, Section 12, Section 15.5) must compile to a branch to a trap-report call with the stable trap code, the source span of the failing operation, and the relevant type/value facts, per the diagnostic contract. The stack-exhaustion trap (`AIC-R0815`) may be implemented by a guard mechanism of the implementation's choice (guard page or prologue check); the observable contract is the trap record and exit code.

---

## 16. Bootstrap contract

### 16.1 Stage definitions

- **Stage 0**: the bootstrap compiler implemented in conservative C17, compiled by an accepted host C compiler (MSVC `cl` or LLVM Clang; both are accepted for independent verification, ADR-001). Stage 0 defines no AI-Co semantics beyond what it implements; the normative specification and conformance suite define AI-Co.
- **Stage 1**: Stage 0 compiles the AI-Co source of the compiler, producing a Stage 1 compiler executable.
- **Stage 2**: Stage 1 compiles the *same* AI-Co compiler source again, producing a Stage 2 compiler executable.

### 16.2 Deterministic equivalence (no normalization)

- The **primary identity artifacts** are the Stage 1 and Stage 2 compiler-produced **COFF object files and build manifests** for the same AI-Co compiler source, inputs, and options.
- These primary artifacts must be **byte-identical with no normalization step** (per ADR-001 §99–109). There is no permitted normalization for compiler-produced artifacts.
- Zero/canonical metadata rules that make this achievable and that the compiler must obey:
  - compiler-controlled timestamps are zero (ADR-001 §107);
  - record and section order in COFF output is canonical (the spec fixes a deterministic order; see Section 14.2);
  - paths embedded in objects/manifests are repository-relative and canonically separated (`/`);
  - build manifests are emitted per Section 14.4: no self-hash, no stage-dependent version/identity fields, and no absolute host paths;
  - iteration over any collection that affects output order is deterministic (sorted by stable keys);
  - random identifiers, build-machine identity, environment values, and host-specific strings are prohibited in artifacts.
- Byte identity without behavioral verification is insufficient; behavioral success without the deterministic comparison is also insufficient for the self-hosting milestone.

### 16.3 PE identity gate and accepted linker modes

- The same accepted external linker, invoked with the exact same deterministic/reproducible flags, is applied independently to each stage's identical object set. The resulting PE executables must be **byte-identical** as a **secondary identity gate**.
- Accepted linker modes for the bootstrap proof:
  - Microsoft `link.exe` invoked with its documented reproducible-build mode (`/Brepro` or equivalent that zeroes/derives the PE timestamp and other nondeterministic fields);
  - LLVM `lld-link` invoked with its documented reproducible-build mode (`/Brepro` or equivalent).
  - The exact flag set used must be recorded in the build manifest.
- If an accepted linker cannot satisfy byte identity in its reproducible mode, that linker is unsuitable for the bootstrap proof; nondeterministic PE fields may not be excused through any normalization rule.
- The comparison evidence must record **every compiler and linker input**: source file list with content hashes, build manifest, compiler options, host compiler identity/version, linker identity/version, linker flags, and any libraries or objects passed to the linker. Host compiler identity/version and linker identity/version are recorded **only** in this comparison evidence (and in build logs), never in the build manifest (Section 14.4); the manifest's only version/identity field is the stage-invariant AI-Co language/specification version string.

### 16.4 Host-compiler independence

- The equivalence and suite results must hold when Stage 0 is built with MSVC and when built with LLVM Clang. Host compiler identity must not affect AI-Co semantics or deterministic outputs; any divergence is a defect in the Stage 0 compiler, not a language property.

### 16.5 Milestone boundaries (external-linker time bound)

- **Milestone M1 — first self-hosting proof:** the Stage 0/1/2 line above, with external linking permitted through this milestone only. M1 acceptance criteria:
  - raw byte identity of Stage 1 and Stage 2 primary artifacts (COFF objects and build manifests produced with identical relative output paths, per Section 14.4), with the zero/canonical metadata rules of Section 16.2;
  - PE byte identity under the accepted deterministic linker modes (Section 16.3);
  - full pass of conformance, negative-diagnostic, and executable smoke suites on both outputs;
  - recorded comparison evidence per Section 16.3.
- **Milestone M2 — self-sufficient development baseline:** the next milestone after M1. Acceptance criteria:
  - a **project-owned AI-Co linker** is implemented in AI-Co and is used for normal compiler and utility builds;
  - external linkers may remain independent comparison/oracle tools, but AI-Co does not claim a self-sufficient toolchain while an external linker remains required for ordinary builds;
  - the self-hosted linker itself passes the same deterministic identity and behavioral obligations, adapted to its own artifact set.
- The external-linker exception ends at M1; M2 is required before the toolchain may be described as self-sufficient.

### 16.6 Acceptance evidence

The self-hosting milestone is accepted only with:

1. recorded Stage 0/1/2 build logs and tool versions;
2. byte-identity comparison result for Stage 1 vs Stage 2 primary artifacts (COFF objects and build manifests, produced with identical relative output paths per Section 14.4), with the zero-metadata evidence (Section 16.2);
3. PE byte-identity evidence under the accepted deterministic linker modes, with the full comparison input record (Section 16.3);
4. full pass of the conformance suite on both Stage 1 and Stage 2 outputs;
5. full pass of the negative-diagnostic suite (expected stable codes and spans) on both outputs;
6. full pass of the executable smoke suite (deterministic runs of representative programs) on both outputs;
7. a build manifest for each stage containing the required fields (Section 14.4).

---

## 17. Exclusions and feature-deferral rule

### 17.1 Excluded features (minimal language)

The following are **excluded** from the minimal language (ADR-002; absent until separately justified and designed): floating point; unions; bit-fields; function pointers; generics; type inference; methods; operator overloading; closures; exceptions; garbage collection; reflection; compile-time execution (beyond constant expressions); macros; textual preprocessor and header inclusion; concurrency and atomics; inline assembly; dynamic linking contracts; package management; network dependency resolution; object orientation and inheritance; variadic functions; default arguments; `goto` and labels; `++`/`--`; comma operator; anonymous structs; typedef/aliases; C-style casts (replaced by `cast`/`wrap`); implicit anything beyond Section 11.1.

### 17.2 Feature-deferral rule

- A feature excluded here may be added only through an accepted feature ADR that states: why the feature is needed, how it preserves deterministic semantics and diagnostics, its effect on self-hosting, and its conformance-test implications.
- If implementation planning demonstrates that the minimal feature set cannot express the self-hosting compiler, the Planner must return the exact capability gap to the Main Designer rather than adding a feature silently.

### 17.3 Function-pointer necessity evaluation (FIND-003 closure)

**Decision: function pointers remain excluded from the minimal language; the deferral is retained.**

Evaluation basis (recorded here per review FIND-003 and ADR-002 §71):

- **Static dispatch suffices.** A compiler's dispatch needs (node-kind dispatch, opcode-to-handler tables, visitor-style traversal, callback-style error/emission hooks) are all expressible with the minimal feature set: an explicit discriminant field (enum) plus `switch` on that discriminant. Nothing in the compiler architecture requires an indirect call whose callee is not statically known at the call site.
- **No vtable/interface requirement.** The canonical IR and backend are defined by this specification as closed sets of node/opcode kinds; there is no plugin, extension, or interface-abstraction requirement that would demand method tables.
- **No callback requirement in the runtime contract.** `rt.*` calls are direct; the runtime exposes no callback-taking API (Section 15). Compiler-internal "callbacks" (e.g., error reporter, emission sink) can be modeled as direct calls to a fixed reporter function or as an explicitly threaded parameter/struct, without indirect calls.
- **Known precedent:** small self-hosting compilers without function pointers are a well-established pattern; the cost is verbosity at dispatch sites, which is acceptable for an agent-oriented language whose value proposition includes explicit, unambiguous source.
- **Residual risk and trigger:** if implementation planning (after this specification passes review) demonstrates a genuine self-hosting need for indirect calls that cannot be modeled by static dispatch, the Planner returns that exact capability gap to the Main Designer per ADR-002 §71 and this section. That trigger is not currently met; no architecture question is raised at this time.

---

## 18. Normative examples and counterexamples

Conventions: `// valid` marks conforming examples; `// ERROR AIC-xxxx: span` marks invalid examples with the expected primary span described in prose (the span is the smallest token/expression responsible). Runtime traps are labeled TRAP.

### 18.1 Lexical examples

```
module main;              // valid

var x: i32 = 42;          // valid
var b: bool = true;       // valid
var c: i32 = 0b1010;      // valid
var h: u64 = 0xFF_FF;     // valid
// ERROR AIC-L0006: span "300u8"
var bad: u8 = 300u8;
// ERROR AIC-L0008: span "\q"
var s: str = "bad \q escape";
// ERROR AIC-L0002: span BOM (file starts with EF BB BF)
// ERROR AIC-L0004: unterminated block comment, span "/* ... EOF"
```

### 18.2 Grammar examples

```
struct Point { x: i32; y: i32; }          // valid
pub struct Public { v: i32; }             // valid; pub applies to top-level declarations
enum Color: u8 { Red, Green, Blue }        // valid; Red=0, Green=1, Blue=2
enum Mask: u8 { None = 0, A = 1, B = 2 }   // valid
var q: Point = Point { x: 1, y: 2 };      // valid; struct literal names its type
// ERROR AIC-S0101: span "{" — a brace-initializer without a type name is not an expression
var p: Point = { x: 1, y: 2 };
```

### 18.3 Binding/visibility examples

```
module a.b;
pub fn f() -> i32 { return 1; }
fn g() -> i32 { return 2; }                // private
```
```
module main;
import a.b;
var x: i32 = a.b.f();                      // valid
// ERROR AIC-N0203: span "a.b.g"
var y: i32 = a.b.g();
```

### 18.4 Conversions and arithmetic

```
var a: i8 = cast<i8>(100);                 // valid; explicit narrowing cast
var b: i16 = a;                            // valid (widening)
// ERROR AIC-T0307: span "200" — 200 is i32; implicit narrowing is absent
var c: i8 = 200;
var d: i8 = cast<i8>(200);                 // ERROR AIC-E0408 (constant cast out of range)
var e: u8 = wrap<u8>(300);                 // valid; e == 44
// ERROR AIC-E0405: span "cast<i8>(127) + cast<i8>(1)" — i8 overflow
var f: i8 = cast<i8>(127) + cast<i8>(1);
var g: i32 = 5 / 0;                        // ERROR AIC-E0406 (constant division by zero)
```

### 18.5 Control flow

```
fn classify(n: i32) -> str {
  switch (n) {
    case 0: { return "zero"; }
    case 1: { return "one"; }
    default: { return "other"; }
  }
}
// ERROR AIC-N0202: span "x" (undeclared; x is declared inside case 0's block, out of scope)
// ERROR AIC-E0412: span "case 0" (case body lacks a terminating statement)
// ERROR AIC-E0416: span "fn bad" (non-void, path without return)
fn bad(n: i32) -> i32 {
  switch (n) {
    case 0: { var x: i32 = 1; }
    case 1: { return x; }
  }
}
// ERROR AIC-E0416: span "fn missing" (non-void, path without return)
fn missing(n: i32) -> i32 {
  if (n > 0) { return n; }
}
```

### 18.6 Pointers, slices, str

```
module main;
import rt.mem;
import rt.io;

pub fn main() -> i32 {
  var buf: u8[16] = [0u8; 16];             // valid; 0u8 is a u8 literal
  // ERROR AIC-T0307: span "[0; 16]" — array-literal elements are i32; implicit narrowing to u8 is absent
  var badbuf: u8[16] = [0; 16];
  var sl: u8[] = buf[..];                  // valid explicit slice
  sl[0] = 65u8;                            // valid; buffer element mutated (65u8 is a u8 literal)
  // ERROR AIC-T0307: span "buf" — implicit array→slice conversion is absent
  var s: u8[] = buf;
  var p: u8* = ptr(buf);                   // valid
  var p2: u8* = null;                      // valid
  // ERROR AIC-R0809: TRAP — dereference of null
  var q: u8 = *p2;
  var n: usize = len(sl);                  // valid
  var one: u8 = sl[0];                     // valid
  // ERROR AIC-R0807: TRAP — index out of bounds
  var o: u8 = sl[16];
  return 0;
}
```

---

## 19. Requirements traceability

| Requirement source | Requirement | Spec section | Testable acceptance |
|---|---|---|---|
| Charter purpose | C-based, agent-optimized, deterministic | §1, §2, §5 | conformance suite |
| Charter §Initial outcome 1–5 | self-hosting line, deterministic equivalence | §16 | bootstrap acceptance evidence |
| Charter §Initial scope | unambiguous lexical/grammar, fixed primitives, explicit memory ops, minimal module/import, deterministic eval, small runtime, structured diagnostics, deterministic pipeline | §4–§16 | suites per §16.5 |
| Charter §Constraints | Windows x86-64 target; C17 bootstrap; no third-party compiler libs; E: workspace | §14.3, §16.1, §17 | build manifest, environment baseline |
| ADR-001 bootstrap | stage 0 in C17; explicit pipeline stages; deterministic COFF; diagnostic JSONL; bootstrap equivalence; build/dependency policy | §14, §16 | per-stage manifests |
| ADR-001 target | little-endian Windows x86-64; SSE2 baseline; external linker during bootstrap | §14.2–14.3 | smoke suite on baseline |
| ADR-004 Windows baseline | Windows 10 22H2 x64 pinned as bootstrap development and execution baseline; no ordinary OS updates; newer-Windows compatibility not guaranteed; executable release remains a separate Human Sponsor gate | §14.3 | smoke suite on baseline |
| ADR-001 diagnostics | structured diagnostics carry code/severity/phase/spans/facts/cause/recovery/corrections | `DIAGNOSTIC-CONTRACT.md` | negative-diagnostic suite |
| ADR-002 declarations | canonical `var`/`const`/`fn` forms; no alternative spellings; one name per declaration | §5.2, §8 | negative suite |
| ADR-002 source | valid UTF-8; ASCII identifiers; comments; no preprocessor | §3, §4 | negative suite |
| ADR-002 primitives | fixed-width ints, bool, str; no char/short/int/long/float | §7.1 | conformance suite |
| ADR-002 composites | arrays, slices, raw pointers, structs, enums; no unions/bit-fields/decay | §7, §12 | conformance suite |
| ADR-002 initialization/storage + ADR-004 temporal baseline | no uninitialized state; explicit memory management; bounds checks; allocator registry with deterministic release traps; controlled retention/reuse; accepted stale-access limit | §8, §9, §12, §15 | conformance + negative suite |
| ADR-004 temporal baseline | project-owned allocator registry; deterministic duplicate/invalid-release traps; 0xDD overwrite before reuse; freed blocks in project control until reuse/process exit; stale-access semantics; compiler may not exploit C-style lifetime UB; no complete temporal memory safety | §12.8, §15.1 | conformance + trap suite |
| ADR-002 order/conversions | left-to-right; bool-only conditions; no implicit narrowing; explicit conversions | §10.4, §11 | conformance suite |
| ADR-002 checked arithmetic + ADR-004 wrapping baseline | checked default; `wrap<T>` truncating conversion only; no same-width wrapping arithmetic; defined shifts/division | §11.3 | trap suite |
| ADR-004 wrapping baseline | `wrap<T>` is an explicit modulo/truncating conversion whose operand evaluates under ordinary checked semantics; no same-width wrapping addition/subtraction/multiplication/negation; evidence-triggered revisit; additions require an accepted ADR and conformance coverage | §11.3 | trap suite |
| ADR-002 control flow | braces; no fall-through; no goto/exceptions | §13 | negative suite |
| ADR-002 modules | module decl; imports name modules; one project root; cycle rejection; explicit visibility | §6 | negative suite |
| ADR-002 runtime | small runtime; platform calls behind modules; no FFI promise | §15 | smoke suite |
| ADR-002 failure model | valid program fully specified; invalid rejected; dynamic failure = named trap | §2, §15 | trap suite |
| Review FIND-001 | exact bootstrap comparison artifacts/inputs, zero metadata rules, accepted linker modes, raw byte identity, no normalization | §16.2–16.3 | M1 acceptance evidence |
| Review FIND-002 | external linker time-bounded; milestone boundaries and acceptance criteria | §16.5 | M1/M2 gates |
| Review FIND-003 | explicit function-pointer necessity evaluation | §17.3 | spec review |
| Review FIND-004 | complete runtime/platform API, signatures, error semantics, calling convention, ABI obligations, all compiler-emitted runtime calls | §15, §15.7–15.8 | spec review, smoke suite |
| Review FIND-005 | diagnostic schema version 1, stable code registry, compatibility rules | `DIAGNOSTIC-CONTRACT.md` | negative-diagnostic suite |
| Review FIND-G2-01 | normative examples conform to the implicit-conversion rules; explicit conversions/suffixed literals in examples; negative coverage for rejected spellings | §18.4, §18.6 | negative-diagnostic suite |
| Review FIND-G2-02 | manifest self-hash exclusion and enumerated hashed artifact set | §14.4 | M1 acceptance evidence |
| Review FIND-G2-03 | stage-invariant manifest fields; identical relative output paths for byte identity; host identity in external evidence only | §14.4, §16.2–16.3 | M1 acceptance evidence |
| Review FIND-G2-04 | deterministic ordering for null-span diagnostics | `DIAGNOSTIC-CONTRACT.md` §9 | negative-diagnostic suite |
| Review FIND-G2-05 | allocator zero-size and exact-fit reuse semantics | §15.1 | conformance + trap suite |
| Review FIND-G2-06 | pointer arithmetic overflow is checked with a stable trap | §12.5, §15.5, `DIAGNOSTIC-CONTRACT.md` §11.8 | trap suite |
| Review FIND-G2-07 | bare `import rt;` rejected; explicit runtime submodule imports required | §6.5 | negative suite |
| Review FIND-G2-08 | §18.5 example annotations correct and complete | §18.5 | negative suite |
| Review FIND-G2-09 | schema-versioning draft exemption | `DIAGNOSTIC-CONTRACT.md` §11.10 | spec review |

---

## 20. Acceptance criteria for this document

Self-checklist against the task acceptance criteria:

- [x] Status/owner/version/authority and normative vocabulary explicit — header, §2.
- [x] All accepted ADRs and supersession are accurately represented — header, §19 (ADR-001, ADR-002 accepted; ADR-003 superseded by ADR-004; ADR-004 accepted).
- [x] Every syntax form has exactly one grammar interpretation; `pub`, postfix types, struct literals, runtime imports, case braces, and negative literals are coherent — §4.3, §5.2, §6.5, §12.7, §13.2.
- [x] Every supported operation has static and dynamic semantics or a specified rejection — §10–§13.
- [x] No undefined/unspecified/implementation-defined behavior remains in the stated minimal set; accepted raw-pointer limits and deferred features are explicit — §2.2, §11.3, §12.8, `OPEN-QUESTIONS.md`.
- [x] Temporal/raw-pointer, wrapping, and Windows baseline exactly follow ADR-004 — §11.3, §12.8, §14.3, §15.1.
- [x] No normalization is allowed anywhere — §14.2, §16.2.
- [x] Conversion, operator, evaluation-order, layout, storage, pointer/slice, module, diagnostic, trap, and bootstrap rules testable — §11, §12, §14, §15, §16.
- [x] Examples/counterexamples with expected stable diagnostic codes cover all major rule families — §18.
- [x] Requirements traceability to the accepted ADRs (ADR-001/002/004) and the charter — §19.
- [x] Draft does not make implementation decisions owned by Specialists or architecture decisions owned by the Main Designer — decisions are constrained to elaboration within ADR-001/002/004; no false decision attribution and no nonexistent decision records are referenced.
- [x] Draft is internally cross-referenced, contains no contradictions known to the Planner, and contains no unresolved placeholders or draft-decision markers — §20 review pass and deterministic checks.

## 21. Review state and handoff

- Status: **Proposed** (not Accepted). Implementation remains blocked until the gates below are met.
- Required before implementation may be planned:
  1. Planner self-review (this draft);
  2. independent Reviewer conformance review of this specification and `DIAGNOSTIC-CONTRACT.md`;
  3. Main Designer architectural acceptance;
  4. Marcel informed of any purpose-affecting question (none currently).
- Review-finding closure map:
  - FIND-001 (bootstrap comparison artifacts/inputs, zero metadata, linker modes, raw byte identity): closed in Section 16.2–16.3.
  - FIND-002 (external-linker time bound): closed via milestone boundaries M1/M2 in Section 16.5.
  - FIND-003 (function-pointer evaluation): closed in Section 17.3 (not necessary; static dispatch design basis documented; trigger preserved).
  - FIND-004 (runtime/platform contract): closed in Section 15.7–15.8 and the runtime tables in Section 15.
  - FIND-005 (diagnostic schema v1, code registry, compatibility): closed in `DIAGNOSTIC-CONTRACT.md` (schema version 1; Sections 5, 11.9, and 11.10 of that document).
  - FIND-G2-01..09 (gate-2 conformance review, 2026-08-08): corrected in the correction round below; closure confirmed by the gate-2 re-review round 1 (2026-08-08), with FIND-G2-08's residual RER-G2-01 closed in the entry below.
- Changes from review will be recorded as new versions of this document; supersession is handled per organization governance.
- **v0.1.1 (2026-08-08):** applied the ADR-004 Human Sponsor resolutions (temporal baseline, wrapping baseline, Windows baseline), removed every reference to the deleted, never-committed Main Designer decision-note artifact, and corrected the grammar and contract defects identified by Main Designer inspection. This revision supersedes the v0.1.0 Proposed draft for review purposes.
- **Planner self-review (2026-08-08):** Pass with one correction. The Planner self-review of the ADR-003/ADR-004-aligned v0.1.1 draft confirmed the three decisions (temporal baseline, wrapping baseline, Windows baseline) are requirements-complete, internally coherent, and implementation-ready; it added the ADR-004 §63 obligation that runtime-facing Windows calls be enumerated and documented against the pinned baseline (§14.3), which the v0.1.1 draft had not carried explicitly. No other corrections required; no new architecture decisions made.
- **Gate-2 correction round (2026-08-08):** corrected the six Major findings (FIND-G2-01..06) and three Minor findings (FIND-G2-07..09) of the independent gate-2 conformance review (`docs/reviews/LANGUAGE-SPEC-v0.1.1-REVIEW-2026-08-08.md`, verdict "Changes required") and adopted the three Suggestions (SUG-G2-01, SUG-G2-02, SUG-G2-03). Resolution per finding: FIND-G2-01 — §18.4/§18.6 examples corrected to conform to the §4.3/§11.1 conversion rules (explicit `cast` and suffixed literals), with negative coverage for the rejected spellings; FIND-G2-02 — §14.4 defines the hashed artifact set and explicitly excludes the manifest from its own hash list; FIND-G2-03 — §14.4 defines stage-invariant manifest fields (AI-Co language/spec version string only), carries host compiler and linker identity exclusively in the §16.3 external evidence, and requires identical relative output paths for the byte-identity comparison; FIND-G2-04 — deterministic null-span ordering rule added to `DIAGNOSTIC-CONTRACT.md` §9; FIND-G2-05 — §15.1 defines `alloc_bytes(0)` (returns `null`, no allocation) and an exact-fit size/reuse rule (reverse order of release within a size class; no splitting or coalescing); FIND-G2-06 — §12.5 defines checked pointer-arithmetic overflow with new stable trap `AIC-R0816` (registry and trap table updated); FIND-G2-07 — §6.5 rejects bare `import rt;` with new code `AIC-N0209` and requires explicit runtime submodule imports (no auto-availability); FIND-G2-08 — §18.5 annotations repositioned and completed (missing `AIC-N0202` added); FIND-G2-09 — `DIAGNOSTIC-CONTRACT.md` §11.10 gained a draft-exemption clause. Suggestions adopted: SUG-G2-01 (compiler-controlled timestamps are zero, ADR-001 §107), SUG-G2-02 (`U` class exempt from the phase-group digit mapping), SUG-G2-03 (manifest version/identity field is the AI-Co language/spec version string). No ADR, charter, or governance document was modified; this round is specification precision repair within the accepted architecture. The amended set is subject to Reviewer re-review (gate 2) before Main Designer architectural acceptance (gate 3); push remains held pending a passing re-review verdict.
- **RER-G2-01 correction (2026-08-08):** closed the residual Minor finding of the gate-2 re-review round 1 (`docs/reviews/LANGUAGE-SPEC-v0.1.1-REREVIEW-2026-08-08.md`, verdict "Approved with Minor findings") by completing the §18.5 `fn bad` example's error annotation set: added `// ERROR AIC-E0416: span "fn bad"` for the reachable no-return path (a `switch` with no matching case and no optional `default` reaches the function tail per §13.2/§13.4/§13.5; span is the function name per `DIAGNOSTIC-CONTRACT.md` §11.5). The example's annotation set (AIC-N0202 + AIC-E0412 + AIC-E0416) now matches the actual error set. No registry or contract change was required; no ADR, charter, or governance document was modified.
- Follow-up decision owners: Main Designer (specification acceptance); Coordinator (routing of the Reviewer conformance review and later implementation work packages). No open questions remain; `spec/OPEN-QUESTIONS.md` is the monitored resolution record.
