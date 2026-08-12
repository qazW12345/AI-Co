/* bootstrap/src/diag/diag_codes.c
 *
 * AI-Co Stage-0 diagnostic infrastructure (WP-M0-06): code registry table.
 *
 * The authoritative code registry (DIAGNOSTIC-CONTRACT §11.1-11.9) for
 * schema version 1, transcribed from the Accepted contract. The registry is
 * Planner-owned: any code addition, deprecation, or re-scoping is a
 * contract change and must not be made from this package.
 *
 * Every code in the contract tables appears exactly once. Unknown codes are
 * rejected as defects by diag_record_set_code / diag_record_validate /
 * diag_emit_record.
 */
#include "diag.h"

#include <string.h>

static const DiagCodeInfo kCodes[] = {
    /* --- 11.1 Lexical (AIC-L) --- */
    { "AIC-L0001", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "invalid byte / invalid UTF-8 sequence / malformed character or token" },
    { "AIC-L0002", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "UTF-8 BOM present" },
    { "AIC-L0003", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "NUL byte in source (outside literal/comment)" },
    { "AIC-L0004", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "unterminated block comment" },
    { "AIC-L0005", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "misplaced '_' in integer literal" },
    { "AIC-L0006", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "integer literal value not representable in its type" },
    { "AIC-L0007", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "line terminator inside string literal" },
    { "AIC-L0008", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "invalid escape sequence" },
    { "AIC-L0009", DIAG_PHASE_LEX, DIAG_SEVERITY_ERROR,
      "string literal bytes not valid UTF-8 after escape expansion" },

    /* --- 11.2 Syntax (AIC-S) --- */
    { "AIC-S0101", DIAG_PHASE_SYNTAX, DIAG_SEVERITY_ERROR,
      "expected token (details in message)" },
    { "AIC-S0102", DIAG_PHASE_SYNTAX, DIAG_SEVERITY_ERROR,
      "unexpected token / excluded construct" },
    { "AIC-S0103", DIAG_PHASE_SYNTAX, DIAG_SEVERITY_ERROR,
      "module declaration not first element" },
    { "AIC-S0104", DIAG_PHASE_SYNTAX, DIAG_SEVERITY_ERROR,
      "controlled body without braces" },

    /* --- 11.3 Name binding (AIC-N) --- */
    { "AIC-N0201", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "duplicate declaration in same scope" },
    { "AIC-N0202", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "undeclared name" },
    { "AIC-N0203", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "access to private item from another module" },
    { "AIC-N0204", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "imported module not found at canonical path" },
    { "AIC-N0205", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "module declaration name does not match canonical path" },
    { "AIC-N0206", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "import cycle" },
    { "AIC-N0207", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "module declaration uses the reserved rt prefix" },
    { "AIC-N0208", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "import of reserved runtime submodule not in the runtime surface" },
    { "AIC-N0209", DIAG_PHASE_NAME, DIAG_SEVERITY_ERROR,
      "bare import rt; (import a specific runtime submodule instead)" },

    /* --- 11.4 Type (AIC-T) --- */
    { "AIC-T0301", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "enum member value not representable in underlying type" },
    { "AIC-T0302", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "use of incomplete struct type as a value" },
    { "AIC-T0303", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "struct recursion by value (infinite size)" },
    { "AIC-T0304", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "==/!= on array or struct type" },
    { "AIC-T0305", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "chained comparison" },
    { "AIC-T0306", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "operator not applicable to operand type" },
    { "AIC-T0307", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "no common type / type mismatch (implicit conversion absent)" },
    { "AIC-T0308", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "invalid explicit cast pair" },
    { "AIC-T0309", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "array literal element count mismatch" },
    { "AIC-T0310", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "condition is not bool" },
    { "AIC-T0311", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "switch selector is not integer/enum" },
    { "AIC-T0312", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "call argument count mismatch" },
    { "AIC-T0313", DIAG_PHASE_TYPE, DIAG_SEVERITY_ERROR,
      "struct literal field error (missing, duplicate, or unknown field)" },

    /* --- 11.5 Semantic (AIC-E) --- */
    { "AIC-E0401", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "expression is not a constant expression" },
    { "AIC-E0402", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "address of const / address of non-lvalue" },
    { "AIC-E0403", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "missing initializer on variable declaration" },
    { "AIC-E0404", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "assignment to const" },
    { "AIC-E0405", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant expression overflow (checked arithmetic)" },
    { "AIC-E0406", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant division by zero" },
    { "AIC-E0407", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant shift count out of range" },
    { "AIC-E0408", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant cast out of range" },
    { "AIC-E0409", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant index/slice bound out of range" },
    { "AIC-E0410", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant str slice not on code point boundary" },
    { "AIC-E0411", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "constant pointer difference not divisible by element size" },
    { "AIC-E0412", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "switch case body missing terminating statement (no fall-through)" },
    { "AIC-E0413", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "duplicate switch case value" },
    { "AIC-E0414", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "break/continue outside loop (or break outside switch)" },
    { "AIC-E0415", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "return value mismatch (value in void, or missing value in non-void)" },
    { "AIC-E0416", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "non-void function path without return" },
    { "AIC-E0417", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "unreachable statement" },
    { "AIC-E0418", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "entry main signature invalid / missing" },
    { "AIC-E0419", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "assignment to non-lvalue" },
    { "AIC-E0420", DIAG_PHASE_SEMANTIC, DIAG_SEVERITY_ERROR,
      "duplicate switch default clause" },

    /* --- 11.6 IR / backend / object / link (AIC-I / AIC-B / AIC-O) --- */
    { "AIC-I0501", DIAG_PHASE_IR, DIAG_SEVERITY_ERROR,
      "IR invariant violation (compiler internal)" },
    { "AIC-B0601", DIAG_PHASE_BACKEND, DIAG_SEVERITY_ERROR,
      "backend constraint violation (e.g., unsupported target feature)" },
    { "AIC-O0701", DIAG_PHASE_OBJECT, DIAG_SEVERITY_ERROR,
      "object emission failure / deterministic-artifact failure" },
    { "AIC-O0702", DIAG_PHASE_LINK, DIAG_SEVERITY_ERROR,
      "link failure reported by external linker" },

    /* --- 11.7 Build (AIC-BL) --- */
    { "AIC-BL0801", DIAG_PHASE_BUILD, DIAG_SEVERITY_ERROR,
      "build manifest schema error" },
    { "AIC-BL0802", DIAG_PHASE_BUILD, DIAG_SEVERITY_ERROR,
      "project root missing/invalid" },
    { "AIC-BL0803", DIAG_PHASE_BUILD, DIAG_SEVERITY_ERROR,
      "entry module not found" },

    /* --- 11.8 Runtime traps (AIC-R) and user trap (AIC-U) --- */
    { "AIC-R0801", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "checked conversion out of range" },
    { "AIC-R0802", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "arithmetic overflow (checked)" },
    { "AIC-R0803", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "division by zero" },
    { "AIC-R0804", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "shift count out of range" },
    { "AIC-R0805", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "invalid bool byte value" },
    { "AIC-R0806", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "invalid UTF-8 to str cast" },
    { "AIC-R0807", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "index/slice span out of bounds" },
    { "AIC-R0808", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "str slice not on code point boundary" },
    { "AIC-R0809", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "null pointer dereference" },
    { "AIC-R0810", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "pointer difference not divisible by element size" },
    { "AIC-R0811", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "invalid address/alignment memory access" },
    { "AIC-R0812", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "double release" },
    { "AIC-R0813", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "invalid release (pointer not from allocator)" },
    { "AIC-R0814", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "invalid/closed file handle" },
    { "AIC-R0815", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "stack exhaustion" },
    { "AIC-R0816", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "pointer arithmetic overflow (checked scaling / byte-difference overflow)" },
    { "AIC-U0000", DIAG_PHASE_TRAP, DIAG_SEVERITY_ERROR,
      "user trap via rt.trap.report (numeric code in trap_code)" },
};

const DiagCodeInfo *diag_code_lookup(const char *code)
{
    size_t i;
    if (code == NULL) {
        return NULL;
    }
    for (i = 0; i < diag_code_count(); ++i) {
        if (strcmp(kCodes[i].code, code) == 0) {
            return &kCodes[i];
        }
    }
    return NULL;
}

const DiagCodeInfo *diag_code_at(size_t index)
{
    if (index >= diag_code_count()) {
        return NULL;
    }
    return &kCodes[index];
}

size_t diag_code_count(void)
{
    return sizeof(kCodes) / sizeof(kCodes[0]);
}
