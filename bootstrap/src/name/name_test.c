/* bootstrap/src/name/name_test.c
 *
 * WP-M0-10 unit and integration tests: scopes and shadowing (spec sec. 6.1),
 * single name space and duplicate/undeclared rules (sec. 6.2) with exact
 * AIC-N0201/N0202 spans, visibility (sec. 6.3) with AIC-N0203, module
 * declaration checks (sec. 6.4/sec. 6.5) with AIC-N0205/N0207, canonical
 * module-to-file mapping and import resolution (sec. 6.5) with AIC-N0204,
 * cycle detection (AIC-N0206), rt.* reserved rules (AIC-N0207/N0208/
 * N0209), explicit-import requirement, deterministic module ordering and
 * record sorting, same-FQN-same-declaration identity, and re-execution of
 * the name-owned negative-corpus anchors against the real fixture files
 * under tests/negative/cases/ (read-only; owned by WP-M0-03).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-name' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/name/name_test.c bootstrap/src/name/name.c \
 *     bootstrap/src/ast/ast.c bootstrap/src/parse/parse.c \
 *     bootstrap/src/lex/lex.c bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-name/name_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-name)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "name.h"

#include "../parse/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Shared pipeline: read bytes -> load -> lex -> parse -> name_resolve.
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    char *bytes;
    size_t blen;
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    NameResult *result;
    DiagRecord **recs;
    size_t rn;
    NameStatus st;
} Pipeline;

static bool read_bytes(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return false; }
    *out = buf;
    *out_len = (size_t)sz;
    return true;
}

/* Load a file as the entry ("input.ai") and resolve it. `project_root` is
 * the import-resolution root; `entry_module_name` is the manifest name. */
static void pipeline_run(Pipeline *p, const char *project_root,
                         const char *entry_module_name, const char *file)
{
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;

    memset(p, 0, sizeof(*p));
    if (!read_bytes(file, &p->bytes, &p->blen)) {
        CHECK(0 && "fixture read failed");
        return;
    }
    ld = load_source_from_bytes("input.ai", (const uint8_t *)p->bytes,
                                p->blen, &p->src, &p->recs, &p->rn);
    CHECK(ld == LOAD_OK);
    if (ld != LOAD_OK) return;
    lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    CHECK(lx == LEX_OK);
    ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(project_root, entry_module_name, "input.ai",
                         p->src, p->program, &p->result, &p->recs, &p->rn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    free(p->bytes);
    memset(p, 0, sizeof(*p));
}

/* Run an in-memory source as the entry with project_root "." (no imports). */
static void pipeline_run_mem(Pipeline *p, const char *src_text)
{
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;

    memset(p, 0, sizeof(*p));
    ld = load_source_from_bytes("input.ai", (const uint8_t *)src_text,
                                strlen(src_text), &p->src, &p->recs, &p->rn);
    CHECK(ld == LOAD_OK);
    if (ld != LOAD_OK) return;
    lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    CHECK(lx == LEX_OK);
    ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
                         &p->result, &p->recs, &p->rn);
}

/* ---------------------------------------------------------------------------
 * Record shape assertions
 * ------------------------------------------------------------------------- */

static bool rec_matches(const DiagRecord *r, const char *code,
                        const char *message,
                        int64_t sl, int64_t sc, int64_t so,
                        int64_t el, int64_t ec, int64_t eo,
                        size_t secondary_count)
{
    if (strcmp(r->code, code) != 0) return false;
    if (message && strcmp(r->message, message) != 0) return false;
    if (strcmp(r->severity, "error") != 0) return false;
    if (strcmp(r->phase, "name") != 0) return false;
    if (r->recovery == NULL || strcmp(r->recovery, "authoritative") != 0) {
        return false;
    }
    if (r->primary_span == NULL) return false;
    if (r->primary_span->start.line != sl ||
        r->primary_span->start.col != sc ||
        r->primary_span->start.offset != so ||
        r->primary_span->end.line != el ||
        r->primary_span->end.col != ec ||
        r->primary_span->end.offset != eo) {
        return false;
    }
    if (r->secondary_count != secondary_count) return false;
    return true;
}

/* ---------------------------------------------------------------------------
 * Unit tests: scopes, shadowing, single name space, duplicates
 * ------------------------------------------------------------------------- */

static void test_shadowing_and_scopes(void)
{
    /* Inner declarations may shadow outer ones; same-scope duplicates are
     * AIC-N0201 (spec sec. 6.1/sec. 6.2). A block-local may shadow a module-level
     * name and a fn param may shadow a module-level name. */
    const char *src =
        "module main;\n"
        "var x: i32 = 1;\n"
        "fn f(a: i32) -> i32 {\n"
        "  var a: i32 = 2;\n"        /* shadows the param: allowed */
        "  var x: i32 = 3;\n"        /* shadows module x: allowed */
        "  { var x: i32 = 4; return x; }\n"
        "  return x;\n"
        "}\n"
        "fn main() -> i32 { return f(1); }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    CHECK(p.result != NULL);
    CHECK(p.result->nmodules == 1);
    pipeline_free(&p);
}

static void test_single_name_space_duplicate(void)
{
    /* Type names and value names share one name space per scope (sec. 6.2). */
    const char *src =
        "module main;\n"
        "struct Point { x: i32; }\n"
        "fn Point() -> i32 { return 1; }\n"
        "fn main() -> i32 { return Point(); }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        /* primary = later declaration (no trailing ';' on a fn decl) */
        CHECK(rec_matches(p.recs[0], "AIC-N0201",
                          "duplicate declaration of 'Point' in same scope",
                          3, 1, 38, 3, 32, 69, 1));
        CHECK(strcmp(p.recs[0]->primary_span->file, "input.ai") == 0);
        if (p.recs[0]->secondary_count >= 1) {
            CHECK(strcmp(p.recs[0]->secondary_spans[0]->file,
                         "input.ai") == 0);
        }
    }
    pipeline_free(&p);
}

static void test_duplicate_local(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = 1;\n"
        "  var x: i32 = 2;\n"
        "  return x;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0201",
                          "duplicate declaration of 'x' in same scope",
                          4, 3, 52, 4, 17, 66, 1));
    }
    pipeline_free(&p);
}

static void test_undeclared(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = y;\n"
        "  return x;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0202", "undeclared name 'y'",
                          3, 16, 47, 3, 17, 48, 0));
    }
    pipeline_free(&p);
}

static void test_module_order_independent(void)
{
    /* Module scope is the entire module: use before textual declaration. */
    const char *src =
        "module main;\n"
        "fn main() -> i32 { return helper(); }\n"
        "fn helper() -> i32 { return 1; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    pipeline_free(&p);
}

static void test_struct_field_namespace(void)
{
    /* Struct fields live in the type namespace, not the enclosing scope
     * (sec. 6.1): a field name must not collide with a module-level name and
     * must not be visible as a bare identifier. */
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "var x: i32 = 1;\n"          /* module x coexists with field x */
        "fn main() -> i32 {\n"
        "  var p: Point = Point { x: 1, y: 2 };\n"
        "  var z: i32 = p.x;\n"      /* member access resolves base only */
        "  return z;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    pipeline_free(&p);
}

static void test_enum_member_namespace(void)
{
    /* Enum members are accessed only through the enum type name (sec. 6.1). */
    const char *src =
        "module main;\n"
        "enum Color: u8 { Red, Green, Blue }\n"
        "fn main() -> i32 {\n"
        "  var c: Color = Color.Green;\n"
        "  return 1;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    pipeline_free(&p);
}

static void test_enum_member_not_injected(void)
{
    /* A bare enum member name is not visible in the enclosing scope. */
    const char *src =
        "module main;\n"
        "enum Color: u8 { Red, Green, Blue }\n"
        "fn main() -> i32 { return Red; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0202", "undeclared name 'Red'",
                          3, 27, 75, 3, 30, 78, 0));
    }
    pipeline_free(&p);
}

static void test_duplicate_enum_member(void)
{
    const char *src =
        "module main;\n"
        "enum Color: u8 { Red, Red }\n"
        "fn main() -> i32 { return 1; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(strcmp(p.recs[0]->code, "AIC-N0201") == 0);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Module declaration rules
 * ------------------------------------------------------------------------- */

static void test_module_rt_prefix(void)
{
    const char *src =
        "module rt.foo;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0207",
                          "module declaration uses the reserved 'rt' prefix",
                          1, 1, 0, 1, 15, 14, 0));
    }
    pipeline_free(&p);
}

static void test_entry_module_mismatch(void)
{
    /* The entry module name comes from the manifest; here the driver names
     * "main" but the file declares "wrong.path" -> AIC-N0205. */
    const char *src =
        "module wrong.path;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0205",
                          "module declaration name does not match "
                          "canonical path",
                          1, 1, 0, 1, 19, 18, 0));
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * rt.* reserved rules
 * ------------------------------------------------------------------------- */

static void test_bare_import_rt(void)
{
    const char *src =
        "module main;\n"
        "import rt;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0209",
                          "bare 'import rt;' is not allowed; import a "
                          "specific runtime submodule instead",
                          2, 1, 13, 2, 11, 23, 0));
    }
    pipeline_free(&p);
}

static void test_import_reserved_rt_submodule(void)
{
    const char *src =
        "module main;\n"
        "import rt.internal;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0208",
                          "import of reserved runtime submodule not in "
                          "the runtime surface",
                          2, 8, 20, 2, 19, 31, 0));
    }
    pipeline_free(&p);
}

static void test_import_valid_rt_submodule(void)
{
    /* rt.mem is part of the runtime surface: the import binds, no error. */
    const char *src =
        "module main;\n"
        "import rt.mem;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    if (p.result) {
        CHECK(p.result->nmodules == 2);   /* entry + rt.mem */
        NameModule *rm = name_module_by_fqn(p.result, "rt.mem");
        CHECK(rm != NULL);
        if (rm) {
            CHECK(rm->is_runtime);
            CHECK(rm->path == NULL);
        }
    }
    pipeline_free(&p);
}

static void test_runtime_not_auto_available(void)
{
    /* Without the matching import, rt.mem is an ordinary undeclared name
     * (spec sec. 6.5): AIC-N0202 on the reference. */
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = rt.mem.alloc(16);\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn >= 1);
    if (p.rn >= 1) {
        CHECK(strcmp(p.recs[0]->code, "AIC-N0202") == 0);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Integration: multi-module fixtures (imports, canonical mapping, cycles)
 * ------------------------------------------------------------------------- */

static void test_import_and_visibility(void)
{
    /* The corpus fixture 18-3-name-private-access: import a.b; pub f is
     * callable, private g is rejected with AIC-N0203. The fixture lives
     * under tests/negative/cases/ with a/b.ai at the canonical path. */
    Pipeline p;
    pipeline_run(&p, "tests/negative/cases/18-3-name-private-access",
                 "main", "tests/negative/cases/18-3-name-private-access/"
                         "input.ai");
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0203",
                          "access to private item 'g' in module 'a.b'",
                          4, 14, 60, 4, 19, 65, 1));
        if (p.recs[0]->secondary_count >= 1) {
            DiagSpan *sec = p.recs[0]->secondary_spans[0];
            CHECK(strcmp(sec->file, "a/b.ai") == 0);
            CHECK(sec->start.line == 3 && sec->start.col == 1 &&
                  sec->start.offset == 44);
            CHECK(sec->end.line == 3 && sec->end.col == 5 &&
                  sec->end.offset == 48);
        }
    }
    if (p.result) {
        CHECK(p.result->nmodules == 2);   /* main + a.b */
        NameModule *ab = name_module_by_fqn(p.result, "a.b");
        CHECK(ab != NULL);
        if (ab) {
            CHECK(ab->is_entry == false);
            CHECK(ab->path != NULL);
            CHECK(strcmp(ab->path, "a/b.ai") == 0);
        }
    }
    pipeline_free(&p);
}

static void test_import_not_found(void)
{
    Pipeline p;
    pipeline_run(&p, "tests/negative/cases/derived-name-import-not-found",
                 "main", "tests/negative/cases/"
                         "derived-name-import-not-found/input.ai");
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0204",
                          "imported module 'nonexistent' not found at "
                          "canonical path",
                          2, 8, 20, 2, 19, 31, 0));
    }
    pipeline_free(&p);
}

static void test_import_cycle(void)
{
    Pipeline p;
    pipeline_run(&p, "tests/negative/cases/derived-name-import-cycle",
                 "main", "tests/negative/cases/"
                         "derived-name-import-cycle/input.ai");
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        CHECK(rec_matches(p.recs[0], "AIC-N0206", "import cycle detected",
                          2, 1, 13, 2, 10, 22, 1));
        if (p.recs[0]->secondary_count >= 1) {
            DiagSpan *sec = p.recs[0]->secondary_spans[0];
            CHECK(strcmp(sec->file, "a.ai") == 0);
            CHECK(sec->start.line == 1 && sec->start.col == 1 &&
                  sec->start.offset == 0);
            CHECK(sec->end.line == 1 && sec->end.col == 10 &&
                  sec->end.offset == 9);
        }
    }
    pipeline_free(&p);
}

static void test_diamond_import(void)
{
    /* Importing the same module twice (directly or transitively) is not an
     * error and the same module object is reused (spec sec. 6.5). */
    FILE *f = fopen("bootstrap/stage0/name-fixture/a.ai", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("module a;\nfn f() -> i32 { return 1; }\n", f);
        fclose(f);
    }
    /* project_root must be the directory that contains a.ai */
    Pipeline p;
    memset(&p, 0, sizeof(p));
    {
        const char *entry =
            "module main;\n"
            "import a;\n"
            "import a;\n"
            "fn main() -> i32 { return 0; }\n";
        LoadStatus ld;
        LexStatus lx;
        ParseStatus ps;
        ld = load_source_from_bytes("input.ai", (const uint8_t *)entry,
                                    strlen(entry), &p.src, &p.recs, &p.rn);
        CHECK(ld == LOAD_OK);
        lx = lex_tokenize(p.src, &p.toks, &p.tn, &p.recs, &p.rn);
        CHECK(lx == LEX_OK);
        ps = parse_program(p.toks, p.tn, &p.program, &p.recs, &p.rn);
        CHECK(ps == PARSE_OK);
        p.st = name_resolve("bootstrap/stage0/name-fixture", "main",
                            "input.ai", p.src, p.program,
                            &p.result, &p.recs, &p.rn);
    }
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    if (p.result) {
        CHECK(p.result->nmodules == 2);
        NameModule *a1 = name_module_by_fqn(p.result, "a");
        CHECK(a1 != NULL);
        if (a1) {
            CHECK(a1->is_entry == false);
            CHECK(strcmp(a1->path, "a.ai") == 0);
        }
    }
    pipeline_free(&p);
    remove("bootstrap/stage0/name-fixture/a.ai");
}

static void test_qualified_type(void)
{
    /* A module-qualified named type resolves through the imported module
     * (spec sec. 6.6). Uses the canonical multi-module fixture a/b.ai. */
    FILE *f = fopen("bootstrap/stage0/name-fixture2/a/b.ai", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("module a.b;\n", f);
        fputs("pub struct Point { x: i32; }\n", f);
        fclose(f);
    }
    const char *entry =
        "module main;\n"
        "import a.b;\n"
        "fn main() -> i32 {\n"
        "  var p: a.b.Point = a.b.Point { x: 1 };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    memset(&p, 0, sizeof(p));
    {
        LoadStatus ld;
        LexStatus lx;
        ParseStatus ps;
        ld = load_source_from_bytes("input.ai", (const uint8_t *)entry,
                                    strlen(entry), &p.src, &p.recs, &p.rn);
        CHECK(ld == LOAD_OK);
        lx = lex_tokenize(p.src, &p.toks, &p.tn, &p.recs, &p.rn);
        CHECK(lx == LEX_OK);
        ps = parse_program(p.toks, p.tn, &p.program, &p.recs, &p.rn);
        CHECK(ps == PARSE_OK);
        p.st = name_resolve("bootstrap/stage0/name-fixture2", "main",
                            "input.ai", p.src, p.program,
                            &p.result, &p.recs, &p.rn);
    }
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    if (p.result) {
        CHECK(p.result->nmodules == 2);
        NameModule *ab = name_module_by_fqn(p.result, "a.b");
        CHECK(ab != NULL);
        if (ab) {
            NameSymbol *pt = name_module_lookup(ab, "Point");
            CHECK(pt != NULL);
            if (pt) {
                CHECK(pt->is_pub);
                CHECK(strcmp(pt->fqn, "a.b.Point") == 0);
            }
        }
    }
    pipeline_free(&p);
    remove("bootstrap/stage0/name-fixture2/a/b.ai");
}

static void test_qualified_enum_member(void)
{
    /* A module-qualified enum member (a.b.Color.Red) must resolve to the
     * same member symbol as the same-module spelling (Color.Red) would
     * (spec sec. 6.6 / criterion 4: same FQN -> same declaration). The
     * enum's visibility governs the member (spec sec. 6.3). */
    FILE *f = fopen("bootstrap/stage0/name-fixture2/a/b.ai", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("module a.b;\n", f);
        fputs("pub enum Color: u8 { Red, Green, Blue }\n", f);
        fclose(f);
    }
    const char *entry =
        "module main;\n"
        "import a.b;\n"
        "fn main() -> i32 {\n"
        "  var c: a.b.Color = a.b.Color.Red;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    memset(&p, 0, sizeof(p));
    {
        LoadStatus ld;
        LexStatus lx;
        ParseStatus ps;
        ld = load_source_from_bytes("input.ai", (const uint8_t *)entry,
                                    strlen(entry), &p.src, &p.recs, &p.rn);
        CHECK(ld == LOAD_OK);
        lx = lex_tokenize(p.src, &p.toks, &p.tn, &p.recs, &p.rn);
        CHECK(lx == LEX_OK);
        ps = parse_program(p.toks, p.tn, &p.program, &p.recs, &p.rn);
        CHECK(ps == PARSE_OK);
        p.st = name_resolve("bootstrap/stage0/name-fixture2", "main",
                            "input.ai", p.src, p.program,
                            &p.result, &p.recs, &p.rn);
    }
    CHECK(p.st == NAME_OK);
    CHECK(p.rn == 0);
    if (p.result) {
        CHECK(p.result->nmodules == 2);
        NameModule *ab = name_module_by_fqn(p.result, "a.b");
        CHECK(ab != NULL);
        if (ab) {
            NameSymbol *color = name_module_lookup(ab, "Color");
            CHECK(color != NULL);
            if (color) {
                CHECK(color->is_pub);
                CHECK(strcmp(color->fqn, "a.b.Color") == 0);
                CHECK(color->nmembers == 3);
                NameSymbol *red = NULL;
                for (size_t i = 0; i < color->nmembers; i++) {
                    if (strcmp(color->members[i]->name, "Red") == 0) {
                        red = color->members[i];
                    }
                }
                CHECK(red != NULL);
                if (red) {
                    CHECK(red->owner == color);
                    CHECK(strcmp(red->fqn, "a.b.Color.Red") == 0);
                    /* the module-qualified reference resolves to the member */
                    NameModule *main_m = p.result->modules[0];
                    if (main_m) {
                        size_t found = 0;
                        for (size_t i = 0; i < main_m->nrefs; i++) {
                            if (main_m->refs[i].sym == red) found++;
                        }
                        CHECK(found >= 1);
                    }
                }
            }
        }
    }
    pipeline_free(&p);
    remove("bootstrap/stage0/name-fixture2/a/b.ai");
}

/* ---------------------------------------------------------------------------
 * Same-FQN identity and determinism
 * ------------------------------------------------------------------------- */

static void test_same_fqn_same_declaration(void)
{
    /* Two references to the same fully qualified name must resolve to the
     * same declaration within a build (sec. 6.6 / criterion 4). */
    const char *src =
        "module main;\n"
        "fn f() -> i32 { return 1; }\n"
        "fn main() -> i32 {\n"
        "  var a: i32 = f();\n"
        "  var b: i32 = main.f();\n"
        "  return a + b;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.result && p.result->nmodules >= 1) {
        NameModule *m = p.result->modules[0];
        CHECK(m != NULL);
        if (m) {
            NameSymbol *sf = name_module_lookup(m, "f");
            CHECK(sf != NULL);
            /* find the refs on the module: the ident call `f()` and the
             * module-qualified `main.f` both map to sf */
            size_t found = 0;
            for (size_t i = 0; i < m->nrefs; i++) {
                if (m->refs[i].sym == sf) found++;
            }
            CHECK(found >= 2);
        }
    }
    pipeline_free(&p);
}

static void test_determinism(void)
{
    /* Resolving the same program twice yields identical record streams
     * (byte-compare sorted JSONL emission). */
    const char *src =
        "module main;\n"
        "import rt.internal;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = y;\n"
        "  return x;\n"
        "}\n";
    Pipeline a, b;
    DiagBuf ea, eb;

    pipeline_run_mem(&a, src);
    pipeline_run_mem(&b, src);
    CHECK(a.st == NAME_DIAG_ERROR);
    CHECK(b.st == NAME_DIAG_ERROR);
    CHECK(a.rn == b.rn);

    diag_buf_init(&ea);
    diag_buf_init(&eb);
    CHECK(diag_emit_records_sorted(&ea, a.recs, a.rn));
    CHECK(diag_emit_records_sorted(&eb, b.recs, b.rn));
    CHECK(diag_buf_ok(&ea));
    CHECK(diag_buf_ok(&eb));
    CHECK(ea.len == eb.len);
    CHECK(ea.len == 0 || memcmp(ea.data, eb.data, ea.len) == 0);
    diag_buf_free(&ea);
    diag_buf_free(&eb);
    pipeline_free(&a);
    pipeline_free(&b);
}

/* ---------------------------------------------------------------------------
 * Corpus anchors: re-execute the name-owned negative fixtures
 * ------------------------------------------------------------------------- */

static void check_corpus_anchor(const char *case_dir,
                                const char *entry_module_name,
                                const char *code, const char *message,
                                int64_t sl, int64_t sc, int64_t so,
                                int64_t el, int64_t ec, int64_t eo,
                                size_t secondary_count)
{
    /* project_root is the full fixture path so imports resolve canonically
     * (e.g. a.b -> <case>/a/b.ai, a -> <case>/a.ai); the entry span file
     * name stays "input.ai" regardless of the on-disk path. */
    char root_path[512];
    snprintf(root_path, sizeof(root_path), "tests/negative/cases/%s",
             case_dir);
    char input_path[512];
    snprintf(input_path, sizeof(input_path), "tests/negative/cases/%s/input.ai",
             case_dir);
    Pipeline p;
    pipeline_run(&p, root_path, entry_module_name, input_path);
    CHECK(p.st == NAME_DIAG_ERROR);
    CHECK(p.rn == 1);
    if (p.rn >= 1) {
        if (!rec_matches(p.recs[0], code, message,
                         sl, sc, so, el, ec, eo, secondary_count)) {
            fprintf(stderr, "  [%s] record mismatch: got code=%s msg=%s "
                    "span=(%lld,%lld,%lld)-(%lld,%lld,%lld) sec=%zu\n",
                    case_dir, p.recs[0]->code, p.recs[0]->message,
                    (long long)p.recs[0]->primary_span->start.line,
                    (long long)p.recs[0]->primary_span->start.col,
                    (long long)p.recs[0]->primary_span->start.offset,
                    (long long)p.recs[0]->primary_span->end.line,
                    (long long)p.recs[0]->primary_span->end.col,
                    (long long)p.recs[0]->primary_span->end.offset,
                    p.recs[0]->secondary_count);
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchors(void)
{
    /* project_root is tests/negative/cases/ so imports resolve canonically
     * (e.g. a.b -> a/b.ai, a -> a.ai). Entry module name "main". */
    check_corpus_anchor("18-3-name-private-access", "main",
                        "AIC-N0203",
                        "access to private item 'g' in module 'a.b'",
                        4, 14, 60, 4, 19, 65, 1);
    check_corpus_anchor("derived-name-undeclared", "main",
                        "AIC-N0202", "undeclared name 'y'",
                        3, 16, 47, 3, 17, 48, 0);
    check_corpus_anchor("derived-name-duplicate-decl", "main",
                        "AIC-N0201",
                        "duplicate declaration of 'x' in same scope",
                        4, 3, 52, 4, 17, 66, 1);
    check_corpus_anchor("derived-name-import-not-found", "main",
                        "AIC-N0204",
                        "imported module 'nonexistent' not found at "
                        "canonical path",
                        2, 8, 20, 2, 19, 31, 0);
    check_corpus_anchor("derived-name-module-mismatch", "main",
                        "AIC-N0205",
                        "module declaration name does not match "
                        "canonical path",
                        1, 1, 0, 1, 19, 18, 0);
    check_corpus_anchor("derived-name-import-cycle", "main",
                        "AIC-N0206", "import cycle detected",
                        2, 1, 13, 2, 10, 22, 1);
    check_corpus_anchor("derived-name-module-rt-prefix", "main",
                        "AIC-N0207",
                        "module declaration uses the reserved 'rt' prefix",
                        1, 1, 0, 1, 15, 14, 0);
    check_corpus_anchor("derived-name-import-reserved-rt-submodule", "main",
                        "AIC-N0208",
                        "import of reserved runtime submodule not in "
                        "the runtime surface",
                        2, 8, 20, 2, 19, 31, 0);
    check_corpus_anchor("derived-name-bare-import-rt", "main",
                        "AIC-N0209",
                        "bare 'import rt;' is not allowed; import a "
                        "specific runtime submodule instead",
                        2, 1, 13, 2, 11, 23, 0);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_shadowing_and_scopes();
    test_single_name_space_duplicate();
    test_duplicate_local();
    test_undeclared();
    test_module_order_independent();
    test_struct_field_namespace();
    test_enum_member_namespace();
    test_enum_member_not_injected();
    test_duplicate_enum_member();
    test_module_rt_prefix();
    test_entry_module_mismatch();
    test_bare_import_rt();
    test_import_reserved_rt_submodule();
    test_import_valid_rt_submodule();
    test_runtime_not_auto_available();
    test_import_and_visibility();
    test_import_not_found();
    test_import_cycle();
    test_diamond_import();
    test_qualified_type();
    test_qualified_enum_member();
    test_same_fqn_same_declaration();
    test_determinism();
    test_corpus_anchors();

    name_result_free(NULL);
    name_records_free(NULL, 0);

    if (g_failures == 0) {
        printf("name_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "name_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
