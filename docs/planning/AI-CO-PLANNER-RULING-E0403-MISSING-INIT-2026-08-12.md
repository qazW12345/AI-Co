# Planner Ruling — AIC-E0403 missing-initializer reachability (WP-M0-13a2 / WP-M0-09)

**Status:** Planner decision record + parser-fix work-package manifest (implementation-ready)
**Owner:** Planner
**Date:** 2026-08-12
**Decision card:** t_dcb5540e (auto-routed escalation, OM §6.1 watchdog)
**Blocked task:** t_b174081d — [AI-Co][WP-M0-13a2] Initializers (specialist evidence: comment 744)
**Decision authority:** milestone plan §9 escalation "spec/contract ambiguity → Planner via Coordinator"; downstream-gap rule milestone plan §7; OM §6.1. This is an ambiguity ruling within accepted direction — no new architecture decision, no ADR/contract change, no Human Sponsor gate applies.
**Governing sources:** spec §5.2 (grammar), §8.2 (AIC-E0403 hard requirement); DIAGNOSTIC-CONTRACT §11.5 (E0403 row); corpus anchor `tests/negative/cases/derived-semantic-missing-init/` (`expected.json`, `meta.json`); parse README design decision 2; AST contract (`bootstrap/src/ast/ast.h`), `bootstrap/src/name/name.c` NULL-init guards; Stage-0 manifest §2/§3 (WP-M0-09 ownership, WP-M0-13a2).

## 1. The conflict (independently verified against the committed tree)

1. **Spec §8.2:** "Every variable declaration must have an initializer. There is no uninitialized-variable state in the language; this is a hard requirement (primary span: the declaration) with `AIC-E0403`." — a *semantic* rule whose primary span is the whole declaration.
2. **DIAGNOSTIC-CONTRACT §11.5 row:** `AIC-E0403 | missing initializer on variable declaration | error | the declaration` — phase `semantic`.
3. **Corpus anchor** `derived-semantic-missing-init` (WP-M0-03 owned, `spec_ref §8`): `input.ai` contains `var x: i32;` and expects **exactly one** record — AIC-E0403, phase `semantic`, recovery `authoritative`, message "missing initializer on variable declaration", primary span = the whole declaration `var x: i32;` (line 3 col 3 offset 34 → col 14 offset 45).
4. **Spec §5.2 grammar contradicts the above:** `var_decl = "var" IDENT ":" type "=" expr ";"` and `global_var_decl = "var" IDENT ":" type "=" const_expr ";"` make the initializer *syntactically required*.
5. **Accepted parser (WP-M0-09, commit 70efe05) enforces the grammar strictly:** `parse_local_var_decl` (parse.c:1754) and `parse_global_var` (parse.c:2396) call `expect_punct(PUNCT_ASSIGN)`; on failure they report `AIC-S0101 "expected '='"` and return NULL; `recover_statement` DROPS the declaration from the AST (parse README design decision 2: failed constructs are dropped; "a parse with diagnostics must not be processed downstream").
6. **Consequence (probe-verified by the specialist, rebuilt from HEAD):** no `var`/`global`/`const` declaration with `init == NULL` can ever reach the semantic stage through the accepted pipeline. `AIC-E0403` is dead code, and WP-M0-13a2 AC2 + the corpus anchor are unsatisfiable without a ruling.

The AST model and name layer already anticipate a declaration with `init == NULL` (`name.c:1226` guards `stmt->u.local_decl.init && ...`; `name.c:1373` guards `node->u.global_decl.init && ...`), confirming the design intent: a declaration with no initializer is a *valid parse* whose rejection belongs to the semantic stage.

## 2. Decision

**Adopt Option A — the initializer of a `var` declaration is syntactically optional; a missing initializer is a semantic error `AIC-E0403` emitted by the semantic stage with the declaration span.**

The parser's strictness is the deviation; the §5.2 grammar text is amended to match the accepted semantic rule, the diagnostic contract, the AST model, and the normative corpus anchor.

### 2.1 Grammar amendment (spec §5.2, applied by this ruling)

- `var_decl       = "var" IDENT ":" type [ "=" expr ] ";" ;`   (was `"=" expr` required)
- `global_var_decl = "var" IDENT ":" type [ "=" const_expr ] ";" ;`   (was `"=" const_expr` required)
- `const_decl` and `global_const_decl` are **unchanged**: the initializer remains syntactically required for `const` forms.

The `[ ... ]` optional-element notation is already the spec's established grammar convention (§5.2 uses it for `[ "pub" ]`, `[ "," ]`, `[ "else" ...]`, `[ expr ]` in `for_stmt`), so no new notation is introduced. `for_stmt` reuses `var_decl`/`const_decl` (spec §5.2 line 234), so the amended production applies uniformly there: a missing initializer in a for-init `var` declaration is likewise `AIC-E0403` at the semantic stage.

### 2.2 Parser change (WP-M0-09 owned area — follow-up card, §4/§5)

- `parse_local_var_decl` (parse.c:1728) and `parse_global_var` (parse.c:2377): for the **`var`** form, accept an optional `= expr`; when absent, set `init = NULL` and emit **no** syntax record (the declaration must NOT be dropped by recovery).
- For the **`const`** form (both scopes), keep the strict `expect_punct(PUNCT_ASSIGN)` — a missing `=` remains `AIC-S0101`.

### 2.3 Semantic rule (WP-M0-13a2 scope — unchanged by this ruling, now reachable)

- `decl_init` must reject every `var` declaration (local and module scope) whose `init == NULL` with `AIC-E0403`: phase `semantic`, recovery `authoritative`, message "missing initializer on variable declaration", primary span = the whole declaration (spec §8.2, DIAGNOSTIC-CONTRACT §11.5, corpus anchor).

### 2.4 Scope boundary: `const` missing-initializer

- No E-code names a missing const initializer: `AIC-E0403`'s message is "missing initializer on variable declaration" (spec §8.2 heading "Variable declarations"; §8.1 is "Constant declarations"), and no corpus anchor exists for a const-missing-init case. `const` therefore keeps the strict grammar (missing `=` → `AIC-S0101`).
- If a const-missing-init anchor is authored later, route to the Planner (spec/contract ambiguity) before any parser leniency for `const`.

## 3. Reasoning

1. **Corpus-before-compiler doctrine.** The negative suite is authored from the spec as the normative oracle (manifest §1 rule 5; milestone plan §9 risk control). The anchor pins E0403 as `semantic`/`authoritative` with the declaration span — that IS the accepted behavior. Option B would reverse the oracle.
2. **Accepted normative text already agrees.** Spec §8.2 and DIAGNOSTIC-CONTRACT §11.5 both make missing-initializer a *semantic* error with the declaration span. The grammar production in §5.2 is the single contradictory text.
3. **The AST/name layer was designed for NULL init.** `name.c` guards `init` for both local and global declarations; the semantic rule is anticipatable dead code only because the parser currently blocks it.
4. **Minimal ripple.** Option A changes two grammar productions, one parser function pair (a `var`-only conditional), and unblocks the already-specified E0403 rule in WP-M0-13a2. Option B would require rewriting spec §8.2, DIAGNOSTIC-CONTRACT §11.5, the corpus anchor, and the AST NULL-init design — against the corpus, for no compensating benefit.
5. **This is an ambiguity ruling, not a new architecture decision.** No ADR, charter, or governance change; no new language feature; no Human Sponsor gate. The parser change is a re-planning/scope-amendment per the milestone plan §7 downstream-gap rule (reopening the accepted WP-M0-09 area through the Planner, then the Coordinator).

## 4. Follow-up owners

| Follow-up | Owner | Routing |
|---|---|---|
| Parser leniency fix (WP-M0-09 area) | senior_specialist | new card §5, serial before WP-M0-13a2 resumption |
| E0403 enforcement in `decl_init` + corpus-anchor re-execution | senior_specialist | t_b174081d resumes after the parser-fix review PASS |
| Spec revision v0.1.3 independent verification | Reviewer (reviewer2) | spec §21 revision pattern (precedent: t_9410ebc6 for v0.1.2) |
| Card creation/dispatch; 13a2 resumption gate | Coordinator | creates the §5 card from this manifest; unblocks t_b174081d after review PASS |

## 5. Parser-fix card (implementation-ready work-package manifest)

### Card P — [AI-Co][WP-M0-09-fix] Missing-initializer leniency for `var` declarations (Planner ruling t_dcb5540e)

- **Objective:** amend the accepted parser per ruling §2.2 — `var` declarations (local and module scope) with no initializer parse successfully with `init == NULL` and no syntax record; `const` declarations keep the strict required-initializer grammar.
- **Scope / owned files (ONLY):**
  - `bootstrap/src/parse/parse.c` (`parse_local_var_decl`, `parse_global_var`; recovery path if the lenient form must not be dropped)
  - `bootstrap/src/parse/parse_test.c` (+ `golden_cases.h` if a golden dump must be added) — new unit tests for the lenient form and const strictness
  - `bootstrap/src/parse/README.md` only if a design-decision note is needed (optional)
  - `bootstrap/build/parse.txt` fragment if the area convention updates it on test-count changes
- **Exclusions:** AST node shapes (no change needed — `init` is already a nullable field); `name.c`; spec/DIAGNOSTIC-CONTRACT/corpus/manifest (the grammar text is already amended by the Planner ruling; do NOT edit those files); WP-M0-13a2's `decl_init` (separate card).
- **Dependencies / inputs:** spec §5.2 (as amended 2026-08-12 by ruling t_dcb5540e), §8.2; DIAGNOSTIC-CONTRACT §7/§11.5; corpus anchor `tests/negative/cases/derived-semantic-missing-init/` (read-only reference).
- **Expected artifacts:** modified `bootstrap/src/parse/parse.c`, `bootstrap/src/parse/parse_test.c`; updated `bootstrap/build/parse.txt`; committed on local main, not pushed.
- **Sizing estimate:** small — 60–150 LOC incl. tests, ~2 test functions, 1 normative change; well under threshold.
- **Capability / authority:** senior_specialist. Host toolchain required: yes (build + run parser tests both toolchains where feasible).
- **Acceptance criteria:**
  1. Local `var x: i32;` parses with **no** syntax record; AST contains the `AST_VAR_DECL` with `name = "x"`, `init == NULL`, span = the whole declaration.
  2. Module-scope `var g: i32;` parses with no syntax record; AST contains `AST_GLOBAL_VAR_DECL` with `init == NULL`, full declaration span.
  3. `var` in `for (var x: i32; ...)` with missing init parses with `init == NULL` (production reuse).
  4. Local and global `const` with missing `=` still produce `AIC-S0101 "expected '='"` and are dropped by recovery (unchanged behavior).
  5. All existing parser tests + golden dumps pass unchanged (no behavior change for valid programs or any other malformed input).
  6. No writes outside the owned file list; no spec/DIAGNOSTIC-CONTRACT/corpus/manifest modification; no C: drive writes; no secrets.
  7. Commit locally on main with a clear message naming the ruling (e.g. "WP-M0-09-fix: missing-initializer leniency for var declarations per Planner ruling t_dcb5540e"); do NOT push.
- **Verification / review:** self-review + Reviewer independent review (class: Reviewer). Completion protocol per manifest §1 rule 8 (block `review-required:` after commit; do NOT complete your own card).
- **Risks / escalation:** recovery interplay (lenient form must not be dropped and must not disturb recovery state for later malformed statements) → Planner if ambiguity remains; architecture question → Main Designer.
- **Confidence:** requirements High (ruling + corpus pin); architecture High (small, contained change in the accepted parser).
- **Serial note:** this card is the serial prerequisite for WP-M0-13a2 (t_b174081d) resumption; after this card's review PASS, t_b174081d resumes to implement `decl_init` E0403 and re-execute the corpus anchor.
