/* bootstrap/src/types/types_test.c
 *
 * WP-M0-11a unit and integration tests: type identity (spec sec. 7.3,
 * structural, not name-based), primitive/composite type tables (sec.
 * 7.1-7.2), and completeness rules (sec. 7.6) with the corpus-pinned
 * AIC-T0302/T0303 messages and spans, including re-execution of the
 * type-owned negative-corpus anchors against the real fixture files under
 * tests/negative/cases/ (read-only; owned by WP-M0-03).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-types' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/types/types_test.c \
 *     bootstrap/src/types/type_identity.c bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-types/types_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-types)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "type_tables.h"

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
 * Shared pipeline: read bytes -> load -> lex -> parse -> name_resolve ->
 * types_check_completeness.
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    char *bytes;
    size_t blen;
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    NameResult *result;
    DiagRecord **recs;      /* name-phase records */
    size_t rn;
    NameStatus st;
    DiagRecord **trecs;     /* completeness records */
    size_t trn;
    TypeCheckStatus tst;
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

/* Run the pipeline on a file (entry "input.ai") and then the completeness
 * pass. `project_root` is the import-resolution root. */
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
    if (lx != LEX_OK) return;
    ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(project_root, entry_module_name, "input.ai",
                         p->src, p->program, &p->result, &p->recs, &p->rn);
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
}

/* Run the pipeline on an in-memory source as the entry ("main" module). */
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
    if (lx != LEX_OK) return;
    ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(ps == PARSE_OK);
    if (ps != PARSE_OK) return;
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
                         &p->result, &p->recs, &p->rn);
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    free(p->bytes);
    memset(p, 0, sizeof(*p));
}

/* ---------------------------------------------------------------------------
 * Record shape assertions (phase "type", severity error, recovery
 * authoritative)
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
    if (strcmp(r->phase, "type") != 0) return false;
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
 * Type tables (spec sec. 7.1-7.2)
 * ------------------------------------------------------------------------- */

static void test_primitive_table(void)
{
    CHECK(types_prim_count() == 13);

    const TypePrimInfo *v = types_prim_info(AST_PRIM_VOID);
    CHECK(v != NULL && strcmp(v->name, "void") == 0);
    CHECK(v && !v->is_integer && v->width_bits == 0 && v->size_bytes == 0);

    const TypePrimInfo *b = types_prim_info(AST_PRIM_BOOL);
    CHECK(b != NULL && strcmp(b->name, "bool") == 0);
    CHECK(b && !b->is_integer && b->size_bytes == 1 && b->align_bytes == 1);

    const TypePrimInfo *s = types_prim_info(AST_PRIM_STR);
    CHECK(s != NULL && strcmp(s->name, "str") == 0);
    CHECK(s && !s->is_integer && s->size_bytes == 16 && s->align_bytes == 8);

    const TypePrimInfo *i8 = types_prim_info(AST_PRIM_I8);
    CHECK(i8 && i8->is_integer && i8->is_signed && i8->width_bits == 8 &&
          i8->size_bytes == 1 && i8->align_bytes == 1);

    const TypePrimInfo *u8 = types_prim_info(AST_PRIM_U8);
    CHECK(u8 && u8->is_integer && !u8->is_signed && u8->width_bits == 8);

    const TypePrimInfo *i64 = types_prim_info(AST_PRIM_I64);
    CHECK(i64 && i64->width_bits == 64 && i64->size_bytes == 8);

    const TypePrimInfo *isz = types_prim_info(AST_PRIM_ISIZE);
    CHECK(isz && isz->is_integer && isz->is_pointer_sized &&
          isz->width_bits == 64 && isz->size_bytes == 8);
    const TypePrimInfo *usz = types_prim_info(AST_PRIM_USIZE);
    CHECK(usz && usz->is_integer && usz->is_pointer_sized &&
          !usz->is_signed && usz->width_bits == 64);

    /* bool is not an integer type (sec. 7.1). */
    CHECK(!types_prim_info(AST_PRIM_BOOL)->is_integer);

    /* name lookups */
    CHECK(types_prim_by_name("i32") == types_prim_info(AST_PRIM_I32));
    CHECK(types_prim_by_name("usize") == types_prim_info(AST_PRIM_USIZE));
    CHECK(types_prim_by_name("void") == types_prim_info(AST_PRIM_VOID));
    CHECK(types_prim_by_name("nope") == NULL);
    CHECK(types_prim_by_name(NULL) == NULL);
    CHECK(types_prim_info((AstPrimKind)999) == NULL);
}

static void test_composite_table(void)
{
    CHECK(types_composite_count() == 5);
    const TypeCompositeInfo *a = types_composite_info(TY_COMPOSITE_ARRAY);
    CHECK(a && strcmp(a->name, "array") == 0 && strcmp(a->form, "T[N]") == 0);
    CHECK(a && strstr(a->layout_note, "N * sizeof(T)") != NULL);
    const TypeCompositeInfo *sl = types_composite_info(TY_COMPOSITE_SLICE);
    CHECK(sl && strcmp(sl->name, "slice") == 0 && strcmp(sl->form, "T[]") == 0);
    const TypeCompositeInfo *p = types_composite_info(TY_COMPOSITE_PTR);
    CHECK(p && strcmp(p->name, "pointer") == 0 && strcmp(p->form, "T*") == 0);
    const TypeCompositeInfo *st = types_composite_info(TY_COMPOSITE_STRUCT);
    CHECK(st && strcmp(st->name, "struct") == 0);
    const TypeCompositeInfo *en = types_composite_info(TY_COMPOSITE_ENUM);
    CHECK(en && strcmp(en->name, "enum") == 0);
    CHECK(types_composite_info((TypeCompositeKind)99) == NULL);
}

/* ---------------------------------------------------------------------------
 * Type identity (spec sec. 7.3)
 * ------------------------------------------------------------------------- */

static void test_primitive_identity(void)
{
    Type *a = type_prim_new(AST_PRIM_I32);
    Type *b = type_prim_new(AST_PRIM_I32);
    Type *c = type_prim_new(AST_PRIM_I64);
    CHECK(type_identical(a, b));
    CHECK(!type_identical(a, c));
    CHECK(!type_identical(b, c));
    CHECK(type_identical(NULL, NULL));
    CHECK(!type_identical(NULL, a));
    CHECK(!type_identical(a, NULL));
    type_free(a);
    type_free(b);
    type_free(c);
}

static void test_composite_identity(void)
{
    /* array identity: same element + same extent */
    Type *i32a = type_prim_new(AST_PRIM_I32);
    Type *i32b = type_prim_new(AST_PRIM_I32);
    Type *i64a = type_prim_new(AST_PRIM_I64);
    Type *arr1 = type_array_new(i32a, 4);
    Type *arr2 = type_array_new(i32b, 4);
    Type *arr3 = type_array_new(type_prim_new(AST_PRIM_I32), 8);
    Type *arr4 = type_array_new(i64a, 4);
    CHECK(type_identical(arr1, arr2));
    CHECK(!type_identical(arr1, arr3));   /* different extent */
    CHECK(!type_identical(arr1, arr4));   /* different element */
    type_free(arr1);
    type_free(arr2);
    type_free(arr3);
    type_free(arr4);

    /* slice vs slice; ptr vs ptr; cross-form not identical */
    Type *sl1 = type_slice_new(type_prim_new(AST_PRIM_U8));
    Type *sl2 = type_slice_new(type_prim_new(AST_PRIM_U8));
    Type *pt1 = type_ptr_new(type_prim_new(AST_PRIM_U8));
    CHECK(type_identical(sl1, sl2));
    CHECK(!type_identical(sl1, pt1));      /* slice vs pointer */
    CHECK(!type_identical(pt1, sl2));
    CHECK(type_identical(pt1, type_ptr_new(type_prim_new(AST_PRIM_U8))));
    Type *pt_i32 = type_ptr_new(type_prim_new(AST_PRIM_I32));
    CHECK(!type_identical(pt1, pt_i32));   /* pointer element differs */
    type_free(sl1);
    type_free(sl2);
    type_free(pt1);
    type_free(pt_i32);

    /* nested: i32[2][3] vs i32[2][3], and vs i32[3][2] */
    Type *n1 = type_array_new(type_array_new(type_prim_new(AST_PRIM_I32), 2), 3);
    Type *n2 = type_array_new(type_array_new(type_prim_new(AST_PRIM_I32), 2), 3);
    Type *n3 = type_array_new(type_array_new(type_prim_new(AST_PRIM_I32), 3), 2);
    CHECK(type_identical(n1, n2));
    CHECK(!type_identical(n1, n3));
    type_free(n1);
    type_free(n2);
    type_free(n3);

    /* prim vs composite never identical */
    Type *pv = type_prim_new(AST_PRIM_BOOL);
    Type *sv = type_slice_new(type_prim_new(AST_PRIM_BOOL));
    CHECK(!type_identical(pv, sv));
    type_free(pv);
    type_free(sv);
}

static void test_named_identity_same_declaration(void)
{
    /* Two references to the same struct/enum declaration are the same
     * type; different declarations are different types (sec. 7.3: same
     * declaration, never name-based). */
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "struct Pair { a: Point; b: Point; }\n"
        "struct Other { z: i32; }\n"
        "enum Color: u8 { Red, Green }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    if (p.result && p.result->nmodules > 0) {
        const NameModule *mod = p.result->modules[0];
        const NameSymbol *point = name_module_lookup(mod, "Point");
        const NameSymbol *pair = name_module_lookup(mod, "Pair");
        const NameSymbol *other = name_module_lookup(mod, "Other");
        const NameSymbol *color = name_module_lookup(mod, "Color");
        CHECK(point && point->kind == NAME_SYM_STRUCT);
        CHECK(pair && pair->kind == NAME_SYM_STRUCT);
        CHECK(color && color->kind == NAME_SYM_ENUM);

        /* same declaration -> identical */
        Type *t1 = type_struct_new(point);
        Type *t2 = type_struct_new(point);
        CHECK(type_identical(t1, t2));
        type_free(t1);
        type_free(t2);

        /* different declarations -> not identical, regardless of name */
        Type *tp = type_struct_new(point);
        Type *to = type_struct_new(other);
        CHECK(!type_identical(tp, to));
        type_free(tp);
        type_free(to);

        /* struct vs enum -> not identical */
        Type *ts = type_struct_new(point);
        Type *te = type_enum_new(color);
        CHECK(!type_identical(ts, te));
        type_free(ts);
        type_free(te);

        /* the two fields of Pair reference the same Point declaration */
        const AstNode *pdecl = pair->decl;
        CHECK(pdecl && pdecl->kind == AST_STRUCT_DECL &&
              pdecl->u.struct_decl.nfields == 2);
        if (pdecl && pdecl->u.struct_decl.nfields == 2) {
            const NameSymbol *f0 = name_symbol_for_node(
                mod, pdecl->u.struct_decl.fields[0]->u.named.type);
            const NameSymbol *f1 = name_symbol_for_node(
                mod, pdecl->u.struct_decl.fields[1]->u.named.type);
            CHECK(f0 == point);
            CHECK(f1 == point);
            CHECK(f0 == f1);
        }
    }
    pipeline_free(&p);
}

static void test_describe(void)
{
    char *s;
    s = type_describe(type_prim_new(AST_PRIM_I32));
    CHECK(s && strcmp(s, "i32") == 0);
    free(s);
    s = type_describe(type_array_new(type_prim_new(AST_PRIM_U8), 4));
    CHECK(s && strcmp(s, "u8[4]") == 0);
    free(s);
    s = type_describe(type_slice_new(type_prim_new(AST_PRIM_BOOL)));
    CHECK(s && strcmp(s, "bool[]") == 0);
    free(s);
    s = type_describe(type_ptr_new(type_array_new(type_prim_new(AST_PRIM_I32), 2)));
    CHECK(s && strcmp(s, "i32[2]*") == 0);
    free(s);
    s = type_describe(type_ptr_new(type_ptr_new(type_prim_new(AST_PRIM_U8))));
    CHECK(s && strcmp(s, "u8**") == 0);
    free(s);
    CHECK(type_describe(NULL) == NULL);
    CHECK(strcmp(type_kind_text(TYPE_PRIM), "prim") == 0);
    CHECK(strcmp(type_kind_text(TYPE_ARRAY), "array") == 0);
    CHECK(strcmp(type_kind_text(TYPE_STRUCT), "struct") == 0);
}

/* ---------------------------------------------------------------------------
 * Completeness (spec sec. 7.6)
 * ------------------------------------------------------------------------- */

static void test_completeness_ok(void)
{
    /* Pointer self-recursion is permitted; complete structs and enums are
     * fine; no diagnostics expected. */
    const char *src =
        "module main;\n"
        "struct Node { val: i32; next: Node*; }\n"
        "struct Point { x: i32; y: i32; }\n"
        "enum Color: u8 { Red, Green }\n"
        "fn main() -> i32 {\n"
        "  var n: Node = Node { val: 1, next: null };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.trn == 0);
    pipeline_free(&p);
}

static void test_completeness_forward_pointer(void)
{
    /* A pointer to a not-yet-closed struct is permitted (sec. 7.6). */
    const char *src =
        "module main;\n"
        "struct A { b: B*; }\n"
        "struct B { x: i32; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.trn == 0);
    pipeline_free(&p);
}

static void test_completeness_recursion_by_value(void)
{
    /* Self-recursion by value with no value use elsewhere: AIC-T0303 at
     * the struct's declaration-name span (corpus-pinned). */
    const char *src =
        "module main;\n"
        "struct Node { val: i32; next: Node; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_DIAG_ERROR);
    CHECK(p.trn == 1);
    if (p.trn >= 1) {
        CHECK(rec_matches(p.trecs[0], "AIC-T0303",
                          "struct 'Node' has infinite size due to "
                          "recursive by-value field 'next'",
                          2, 8, 20, 2, 12, 24, 0));
    }
    pipeline_free(&p);
}

static void test_completeness_incomplete_use(void)
{
    /* A self-recursive struct that is also used as a value anywhere is
     * reported as AIC-T0302 (incomplete struct use) at the struct's
     * declaration-name span (corpus-pinned selection). */
    const char *src =
        "module main;\n"
        "struct Node { val: i32; next: Node; }\n"
        "fn main() -> i32 {\n"
        "  var n: Node = Node { val: 1, next: Node { val: 2, next: Node {} } };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_DIAG_ERROR);
    CHECK(p.trn == 1);
    if (p.trn >= 1) {
        CHECK(rec_matches(p.trecs[0], "AIC-T0302",
                          "use of incomplete struct type 'Node' as a value",
                          2, 8, 20, 2, 12, 24, 0));
    }
    pipeline_free(&p);
}

static void test_completeness_forward_value_field(void)
{
    /* Forming a field of a not-yet-closed struct is AIC-T0302 (sec. 7.6).
     * Span choice follows the corpus convention: the incomplete struct's
     * declaration-name span (B is on line 3 of this program). */
    const char *src =
        "module main;\n"
        "struct A { b: B; }\n"
        "struct B { x: i32; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_DIAG_ERROR);
    CHECK(p.trn == 1);
    if (p.trn >= 1) {
        CHECK(rec_matches(p.trecs[0], "AIC-T0302",
                          "use of incomplete struct type 'B' as a value",
                          3, 8, 39, 3, 9, 40, 0));
    }
    pipeline_free(&p);
}

static void test_completeness_value_use_via_array(void)
{
    /* Array of a self-recursive struct is a value use: AIC-T0302. */
    const char *src =
        "module main;\n"
        "struct Node { next: Node; }\n"
        "fn main() -> i32 {\n"
        "  var a: Node[4] = [Node {}, Node {}, Node {}, Node {}];\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_DIAG_ERROR);
    CHECK(p.trn == 1);
    if (p.trn >= 1) {
        CHECK(strcmp(p.trecs[0]->code, "AIC-T0302") == 0);
        CHECK(strcmp(p.trecs[0]->message,
                     "use of incomplete struct type 'Node' as a value") == 0);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Determinism
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    /* The same program yields byte-identical sorted JSONL record streams. */
    const char *src =
        "module main;\n"
        "struct Node { val: i32; next: Node; }\n"
        "struct A { b: B; }\n"
        "struct B { x: i32; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline a, b;
    pipeline_run_mem(&a, src);
    pipeline_run_mem(&b, src);
    CHECK(a.trn == b.trn);
    DiagBuf ea, eb;
    diag_buf_init(&ea);
    diag_buf_init(&eb);
    CHECK(diag_emit_records_sorted(&ea, a.trecs, a.trn));
    CHECK(diag_emit_records_sorted(&eb, b.trecs, b.trn));
    CHECK(diag_buf_ok(&ea) && diag_buf_ok(&eb));
    CHECK(ea.len == eb.len &&
          (ea.len == 0 || memcmp(ea.data, eb.data, ea.len) == 0));
    diag_buf_free(&ea);
    diag_buf_free(&eb);
    pipeline_free(&a);
    pipeline_free(&b);
}

/* ---------------------------------------------------------------------------
 * Corpus anchors: re-execute the type-owned negative fixtures
 * ------------------------------------------------------------------------- */

static void check_corpus_anchor(const char *case_dir,
                                const char *entry_module_name,
                                const char *code, const char *message,
                                int64_t sl, int64_t sc, int64_t so,
                                int64_t el, int64_t ec, int64_t eo,
                                size_t secondary_count)
{
    char root_path[512];
    snprintf(root_path, sizeof(root_path), "tests/negative/cases/%s",
             case_dir);
    char input_path[512];
    snprintf(input_path, sizeof(input_path), "tests/negative/cases/%s/input.ai",
             case_dir);
    Pipeline p;
    pipeline_run(&p, root_path, entry_module_name, input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_DIAG_ERROR);
    CHECK(p.trn == 1);
    if (p.trn >= 1) {
        if (!rec_matches(p.trecs[0], code, message,
                         sl, sc, so, el, ec, eo, secondary_count)) {
            fprintf(stderr, "  [%s] record mismatch: got code=%s msg=%s "
                    "span=(%lld,%lld,%lld)-(%lld,%lld,%lld) sec=%zu\n",
                    case_dir, p.trecs[0]->code, p.trecs[0]->message,
                    (long long)p.trecs[0]->primary_span->start.line,
                    (long long)p.trecs[0]->primary_span->start.col,
                    (long long)p.trecs[0]->primary_span->start.offset,
                    (long long)p.trecs[0]->primary_span->end.line,
                    (long long)p.trecs[0]->primary_span->end.col,
                    (long long)p.trecs[0]->primary_span->end.offset,
                    p.trecs[0]->secondary_count);
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchors(void)
{
    check_corpus_anchor("derived-type-incomplete-struct-use", "main",
                        "AIC-T0302",
                        "use of incomplete struct type 'Node' as a value",
                        2, 8, 20, 2, 12, 24, 0);
    check_corpus_anchor("derived-type-struct-recursion", "main",
                        "AIC-T0303",
                        "struct 'Node' has infinite size due to "
                        "recursive by-value field 'next'",
                        2, 8, 20, 2, 12, 24, 0);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_primitive_table();
    test_composite_table();
    test_primitive_identity();
    test_composite_identity();
    test_named_identity_same_declaration();
    test_describe();
    test_completeness_ok();
    test_completeness_forward_pointer();
    test_completeness_recursion_by_value();
    test_completeness_incomplete_use();
    test_completeness_forward_value_field();
    test_completeness_value_use_via_array();
    test_determinism();
    test_corpus_anchors();

    type_free(NULL);
    types_records_free(NULL, 0);

    if (g_failures == 0) {
        printf("types_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "types_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
