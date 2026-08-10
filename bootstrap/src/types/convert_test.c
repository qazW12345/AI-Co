/* bootstrap/src/types/convert_test.c
 *
 * WP-M0-11c unit and integration tests: the implicit-conversion whitelist
 * (spec sec. 11.1), common-type promotion (sec. 11.1 bullet / sec. 11.4),
 * the build-level AIC-T0307 rejection with correct spans at every
 * conversion site (initializers, assignments, compound assignments, call
 * arguments, return values, binary integer operators, ternary, array
 * literal elements, struct-literal field values), determinism, and
 * re-execution of the three conversion-owned negative-corpus anchors
 * against the real fixture files under tests/negative/cases/ (read-only;
 * owned by WP-M0-03).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-convert' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/types/convert_test.c \
 *     bootstrap/src/types/convert.c bootstrap/src/types/layout.c \
 *     bootstrap/src/types/type_identity.c bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-convert/convert_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-convert)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "convert.h"

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
 * Type helpers
 * ------------------------------------------------------------------------- */

static Type *prim(AstPrimKind k)
{
    return type_prim_new(k);
}

/* ---------------------------------------------------------------------------
 * Expected whitelist: the spec sec. 11.1 table transcribed as test data
 * ------------------------------------------------------------------------- */

/* The exact rows of Table 11.1 for the ten integer types (identity is
 * handled separately). This is the test's normative transcription; the
 * production formula must agree with it on all 100 pairs. */
static bool spec_table_allows(AstPrimKind from, AstPrimKind to)
{
    switch (from) {
    case AST_PRIM_I8:
        return to == AST_PRIM_I16 || to == AST_PRIM_I32 ||
               to == AST_PRIM_I64 || to == AST_PRIM_ISIZE;
    case AST_PRIM_I16:
        return to == AST_PRIM_I32 || to == AST_PRIM_I64 ||
               to == AST_PRIM_ISIZE;
    case AST_PRIM_I32:
        return to == AST_PRIM_I64 || to == AST_PRIM_ISIZE;
    case AST_PRIM_I64:
        return to == AST_PRIM_ISIZE;
    case AST_PRIM_U8:
        return to == AST_PRIM_U16 || to == AST_PRIM_U32 ||
               to == AST_PRIM_U64 || to == AST_PRIM_USIZE ||
               to == AST_PRIM_I16 || to == AST_PRIM_I32 ||
               to == AST_PRIM_I64 || to == AST_PRIM_ISIZE;
    case AST_PRIM_U16:
        return to == AST_PRIM_U32 || to == AST_PRIM_U64 ||
               to == AST_PRIM_USIZE || to == AST_PRIM_I32 ||
               to == AST_PRIM_I64 || to == AST_PRIM_ISIZE;
    case AST_PRIM_U32:
        return to == AST_PRIM_U64 || to == AST_PRIM_USIZE ||
               to == AST_PRIM_I64 || to == AST_PRIM_ISIZE;
    case AST_PRIM_U64:
        return to == AST_PRIM_USIZE;
    case AST_PRIM_ISIZE:
        return to == AST_PRIM_I64;
    case AST_PRIM_USIZE:
        return to == AST_PRIM_U64;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Whitelist (spec sec. 11.1)
 * ------------------------------------------------------------------------- */

static void test_whitelist_integer_matrix(void)
{
    const AstPrimKind ints[] = {
        AST_PRIM_I8, AST_PRIM_I16, AST_PRIM_I32, AST_PRIM_I64,
        AST_PRIM_U8, AST_PRIM_U16, AST_PRIM_U32, AST_PRIM_U64,
        AST_PRIM_ISIZE, AST_PRIM_USIZE
    };
    size_t i, j;
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            AstPrimKind a = ints[i], b = ints[j];
            bool expected = (a == b) || spec_table_allows(a, b);
            Type *ta = prim(a);
            Type *tb = prim(b);
            CHECK(ta != NULL && tb != NULL);
            if (ta && tb) {
                if (convert_implicit_allowed(ta, tb) != expected) {
                    g_failures++;
                    fprintf(stderr,
                            "FAIL whitelist: %s -> %s expected %s\n",
                            types_prim_info(a)->name, types_prim_info(b)->name,
                            expected ? "allowed" : "rejected");
                }
                g_checks++;
            }
            type_free(ta);
            type_free(tb);
        }
    }
}

static void test_whitelist_non_integer(void)
{
    /* bool, str, void: identity only; no cross conversion with integers
     * or with each other. */
    const AstPrimKind others[] = {
        AST_PRIM_BOOL, AST_PRIM_STR, AST_PRIM_VOID
    };
    size_t i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            Type *a = prim(others[i]);
            Type *b = prim(others[j]);
            CHECK(a != NULL && b != NULL);
            if (a && b) {
                CHECK(convert_implicit_allowed(a, b) == (i == j));
            }
            type_free(a);
            type_free(b);
        }
    }
    /* bool <-> integer: never implicit (sec. 11.1 last bullet). */
    {
        Type *b = prim(AST_PRIM_BOOL);
        Type *i32 = prim(AST_PRIM_I32);
        CHECK(b && i32);
        if (b && i32) {
            CHECK(!convert_implicit_allowed(b, i32));
            CHECK(!convert_implicit_allowed(i32, b));
        }
        type_free(b);
        type_free(i32);
    }
}

static void test_whitelist_pointer_and_null(void)
{
    Type *pi32a = type_ptr_new(prim(AST_PRIM_I32));
    Type *pi32b = type_ptr_new(prim(AST_PRIM_I32));
    Type *pi64 = type_ptr_new(prim(AST_PRIM_I64));
    Type *pu8 = type_ptr_new(prim(AST_PRIM_U8));
    Type *ui32 = prim(AST_PRIM_U32);

    CHECK(pi32a && pi32b && pi64 && pu8 && ui32);
    if (pi32a && pi32b && pi64 && pu8 && ui32) {
        /* any T* -> T* (same type) */
        CHECK(convert_implicit_allowed(pi32a, pi32b));
        CHECK(convert_implicit_allowed(pi32b, pi32a));
        /* different pointee: rejected */
        CHECK(!convert_implicit_allowed(pi32a, pi64));
        CHECK(!convert_implicit_allowed(pi32a, pu8));
        /* pointer <-> integer: never implicit */
        CHECK(!convert_implicit_allowed(pi32a, ui32));
        CHECK(!convert_implicit_allowed(ui32, pi32a));
        /* null -> any T* permitted at the expression level */
        CHECK(convert_implicit_allowed_ex(NULL, true, pi32a));
        CHECK(convert_implicit_allowed_ex(NULL, true, pu8));
        /* null -> non-pointer rejected */
        CHECK(!convert_implicit_allowed_ex(NULL, true, ui32));
        /* null source with a non-null from is a caller contract error:
         * when from_is_null is true, from must be NULL. */
        CHECK(!convert_implicit_allowed_ex(ui32, true, pi32a));
        /* unknown (non-null, non-nullable from) with from_is_null false */
        CHECK(!convert_implicit_allowed_ex(NULL, false, pi32a));
    }
    type_free(pi32a);
    type_free(pi32b);
    type_free(pi64);
    type_free(pu8);
    type_free(ui32);
}

static void test_whitelist_composite_identity(void)
{
    Type *a4a = type_array_new(prim(AST_PRIM_I32), 4);
    Type *a4b = type_array_new(prim(AST_PRIM_I32), 4);
    Type *a8 = type_array_new(prim(AST_PRIM_I32), 8);
    Type *sa = type_slice_new(prim(AST_PRIM_U8));
    Type *sb = type_slice_new(prim(AST_PRIM_U8));
    Type *su = type_slice_new(prim(AST_PRIM_U32));
    Type *pi32 = type_ptr_new(prim(AST_PRIM_I32));

    CHECK(a4a && a4b && a8 && sa && sb && su && pi32);
    if (a4a && a4b && a8 && sa && sb && su && pi32) {
        /* identical composites: allowed (identity, sec. 11.6) */
        CHECK(convert_implicit_allowed(a4a, a4b));
        CHECK(convert_implicit_allowed(sa, sb));
        /* different extent / different element: rejected */
        CHECK(!convert_implicit_allowed(a4a, a8));
        CHECK(!convert_implicit_allowed(sa, su));
        /* array -> slice is never implicit (sec. 11.2 last bullet) */
        CHECK(!convert_implicit_allowed(a4a, sa));
        CHECK(!convert_implicit_allowed(a4a, su));
        /* slice -> pointer, array -> pointer: rejected */
        CHECK(!convert_implicit_allowed(sa, pi32));
        CHECK(!convert_implicit_allowed(a4a, pi32));
    }
    type_free(a4a);
    type_free(a4b);
    type_free(a8);
    type_free(sa);
    type_free(sb);
    type_free(su);
    type_free(pi32);
}

/* ---------------------------------------------------------------------------
 * Common type (spec sec. 11.1 bullet / sec. 11.4)
 * ------------------------------------------------------------------------- */

static void check_common(AstPrimKind a, AstPrimKind b, AstPrimKind expected)
{
    Type *ta = prim(a);
    Type *tb = prim(b);
    Type *ct = convert_common_type(ta, tb);
    CHECK(ta != NULL && tb != NULL);
    if (ta && tb && ct) {
        CHECK(ct->kind == TYPE_PRIM && ct->u.prim == expected);
        if (ct->kind != TYPE_PRIM || ct->u.prim != expected) {
            g_failures++;
            fprintf(stderr, "FAIL common: %s with %s -> %s (want %s)\n",
                    types_prim_info(a)->name, types_prim_info(b)->name,
                    ct->kind == TYPE_PRIM
                        ? types_prim_info(ct->u.prim)->name
                        : "non-prim",
                    types_prim_info(expected)->name);
        }
        g_checks++;
    } else {
        g_failures++;
        fprintf(stderr, "FAIL common: %s with %s -> NULL (want %s)\n",
                types_prim_info(a)->name, types_prim_info(b)->name,
                types_prim_info(expected)->name);
        g_checks++;
    }
    type_free(ct);
    type_free(ta);
    type_free(tb);
}

static void check_no_common(AstPrimKind a, AstPrimKind b)
{
    Type *ta = prim(a);
    Type *tb = prim(b);
    Type *ct = convert_common_type(ta, tb);
    CHECK(ta != NULL && tb != NULL);
    CHECK(ct == NULL);
    if (ct != NULL) {
        g_failures++;
        fprintf(stderr, "FAIL no-common: %s with %s -> common exists (want NULL)\n",
                types_prim_info(a)->name, types_prim_info(b)->name);
        g_checks++;
    }
    type_free(ct);
    type_free(ta);
    type_free(tb);
}

static void test_common_type_cases(void)
{
    /* Wider of the two, both conversions in Table 11.1. */
    check_common(AST_PRIM_I32, AST_PRIM_I16, AST_PRIM_I32);
    check_common(AST_PRIM_I16, AST_PRIM_I32, AST_PRIM_I32);   /* commutative */
    check_common(AST_PRIM_U8, AST_PRIM_I16, AST_PRIM_I16);
    check_common(AST_PRIM_U32, AST_PRIM_I64, AST_PRIM_I64);
    check_common(AST_PRIM_U16, AST_PRIM_I32, AST_PRIM_I32);
    check_common(AST_PRIM_I8, AST_PRIM_I16, AST_PRIM_I16);
    check_common(AST_PRIM_U8, AST_PRIM_U16, AST_PRIM_U16);
    check_common(AST_PRIM_I32, AST_PRIM_ISIZE, AST_PRIM_ISIZE);
    check_common(AST_PRIM_U8, AST_PRIM_ISIZE, AST_PRIM_ISIZE);
    check_common(AST_PRIM_U16, AST_PRIM_I64, AST_PRIM_I64);
    /* Identical types: the common type is that type. */
    check_common(AST_PRIM_I32, AST_PRIM_I32, AST_PRIM_I32);
    check_common(AST_PRIM_U8, AST_PRIM_U8, AST_PRIM_U8);
    check_common(AST_PRIM_ISIZE, AST_PRIM_ISIZE, AST_PRIM_ISIZE);
    /* Equal-width identical-sign pairs (i64/isize, u64/usize): both
     * conversions in the table; deterministic tie-break prefers the
     * fixed-width type, commutative. */
    check_common(AST_PRIM_I64, AST_PRIM_ISIZE, AST_PRIM_I64);
    check_common(AST_PRIM_ISIZE, AST_PRIM_I64, AST_PRIM_I64);
    check_common(AST_PRIM_U64, AST_PRIM_USIZE, AST_PRIM_U64);
    check_common(AST_PRIM_USIZE, AST_PRIM_U64, AST_PRIM_U64);
    /* No common type: same width different sign; narrower cannot hold the
     * wider's full range; sign-changing without widening. */
    check_no_common(AST_PRIM_I32, AST_PRIM_U32);
    check_no_common(AST_PRIM_U32, AST_PRIM_I32);
    check_no_common(AST_PRIM_I64, AST_PRIM_U64);
    check_no_common(AST_PRIM_U64, AST_PRIM_I64);
    check_no_common(AST_PRIM_ISIZE, AST_PRIM_USIZE);
    check_no_common(AST_PRIM_I32, AST_PRIM_U64);
    check_no_common(AST_PRIM_I16, AST_PRIM_U16);
    check_no_common(AST_PRIM_I8, AST_PRIM_U8);
    check_no_common(AST_PRIM_U8, AST_PRIM_I8);
    check_no_common(AST_PRIM_ISIZE, AST_PRIM_U64);
    check_no_common(AST_PRIM_I64, AST_PRIM_USIZE);
    check_no_common(AST_PRIM_U64, AST_PRIM_ISIZE);
    /* Non-integer operands: no common type (common-type promotion is
     * defined for integer binary operators only). */
    {
        Type *b1 = prim(AST_PRIM_BOOL);
        Type *b2 = prim(AST_PRIM_BOOL);
        Type *s1 = type_slice_new(prim(AST_PRIM_U8));
        Type *s2 = type_slice_new(prim(AST_PRIM_U8));
        CHECK(b1 && b2 && s1 && s2);
        CHECK(convert_common_type(b1, b2) == NULL);
        CHECK(convert_common_type(s1, s2) == NULL);
        CHECK(convert_common_type(b1, s1) == NULL);
        type_free(b1);
        type_free(b2);
        type_free(s1);
        type_free(s2);
    }
}

/* ---------------------------------------------------------------------------
 * Shared pipeline: read bytes -> load -> lex -> parse -> name_resolve ->
 * types_check_completeness -> types_layout_build -> types_convert_check.
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
    LayoutBuild *build;
    DiagRecord **lrecs;     /* layout records (AIC-T0301) */
    size_t lrn;
    LayoutStatus lst;
    DiagRecord **crecs;     /* convert records (AIC-T0307) */
    size_t crn;
    ConvertStatus cst;
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
    if (p->st != NAME_OK) return;
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
    if (p->tst != TYPE_CHECK_OK) return;
    p->lst = types_layout_build(p->result, &p->build, &p->lrecs, &p->lrn);
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
}

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
    if (p->st != NAME_OK) return;
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
    if (p->tst != TYPE_CHECK_OK) return;
    p->lst = types_layout_build(p->result, &p->build, &p->lrecs, &p->lrn);
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    layout_build_free(p->build);
    types_records_free(p->lrecs, p->lrn);
    types_records_free(p->crecs, p->crn);
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

/* Compute the line/col/offset span of the first occurrence of `needle` in
 * `src` (LF line endings; 1-based lines/cols, 0-based byte offsets - the
 * loader's normalized coordinates). The needle must be unique. */
static void span_of(const char *src, const char *needle,
                    int64_t *sl, int64_t *sc, int64_t *so,
                    int64_t *el, int64_t *ec, int64_t *eo)
{
    const char *p = strstr(src, needle);
    int64_t off, end, line, col, i;
    CHECK(p != NULL);
    if (!p) {
        *sl = *sc = *so = *el = *ec = *eo = -1;
        return;
    }
    off = (int64_t)(p - src);
    line = 1;
    col = 1;
    for (i = 0; i < off; i++) {
        if (src[i] == '\n') { line++; col = 1; }
        else col++;
    }
    *sl = line;
    *sc = col;
    *so = off;
    end = off + (int64_t)strlen(needle);
    line = 1;
    col = 1;
    for (i = 0; i < end; i++) {
        if (src[i] == '\n') { line++; col = 1; }
        else col++;
    }
    *el = line;
    *ec = col;
    *eo = end;
}

static void check_record_at(Pipeline *p, size_t idx, const char *code,
                            const char *message, const char *src,
                            const char *needle)
{
    int64_t sl, sc, so, el, ec, eo;
    span_of(src, needle, &sl, &sc, &so, &el, &ec, &eo);
    if (!rec_matches(p->crecs[idx], code, message, sl, sc, so, el, ec, eo,
                     0)) {
        g_failures++;
        fprintf(stderr, "FAIL record %zu: code=%s msg=%s span=(%lld,%lld,%lld)"
                        "-(%lld,%lld,%lld) sec=%zu (want %s at '%s')\n",
                idx, p->crecs[idx]->code, p->crecs[idx]->message,
                (long long)p->crecs[idx]->primary_span->start.line,
                (long long)p->crecs[idx]->primary_span->start.col,
                (long long)p->crecs[idx]->primary_span->start.offset,
                (long long)p->crecs[idx]->primary_span->end.line,
                (long long)p->crecs[idx]->primary_span->end.col,
                (long long)p->crecs[idx]->primary_span->end.offset,
                p->crecs[idx]->secondary_count,
                message ? message : "(any)", needle);
        g_checks++;
    }
}

/* ---------------------------------------------------------------------------
 * Conversion sites (spec sec. 11.1 bullet)
 * ------------------------------------------------------------------------- */

static void test_site_initializer_narrowing(void)
{
    const char *src =
        "module main;\n"
        "var a: i8 = cast<i8>(100);\n"
        "var c: i8 = 200;\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        check_record_at(&p, 0, "AIC-T0307",
                        "no common type: i32 literal 200 cannot be "
                        "implicitly narrowed to i8",
                        src, "200");
    }
    pipeline_free(&p);
}

static void test_site_assign_and_compound(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i8 = 0i8;\n"
        "  x = 300;\n"
        "  x = cast<i8>(300);\n"
        "  x += 301;\n"
        "  x += cast<i8>(1);\n"
        "  var u64v: u64 = 0u64;\n"
        "  x += u64v;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 3);
    if (p.crn >= 3) {
        check_record_at(&p, 0, "AIC-T0307",
                        "no common type: i32 literal 300 cannot be "
                        "implicitly narrowed to i8",
                        src, "300");
        check_record_at(&p, 1, "AIC-T0307",
                        "no common type: i32 literal 301 cannot be "
                        "implicitly narrowed to i8",
                        src, "301");
        /* compound with an identifier source: the binary no-common
         * record's span is the value expression (the u64v use at
         * line 9), not the declaration (the identifier-to-declaration
         * mapping applies to check_conversion sites, not to the
         * binary no-common shape; documented in convert.h). */
        CHECK(rec_matches(p.crecs[2], "AIC-T0307",
                          "no common type: i8 and u64",
                          9, 8, 146, 9, 12, 150, 0));
    }
    pipeline_free(&p);
}

static void test_site_binary_common(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var a: i32 = 0;\n"
        "  var b64: u64 = 0u64;\n"
        "  var c16: u16 = 0u16;\n"
        "  var r: i32 = a + c16;\n"
        "  var q: i64 = a + b64;\n"
        "  if (a < b64) { }\n"
        "  var cmp: bool = a == b64;\n"
        "  var s: i32 = a << b64;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 4);
    if (p.crn >= 4) {
        /* the shift rhs source is the identifier b64: declaration span
         * (corpus convention), which sorts before the binary records */
        check_record_at(&p, 0, "AIC-T0307",
                        "no common type: u64 value 'b64' cannot be "
                        "implicitly narrowed to i32",
                        src, "b64");
        check_record_at(&p, 1, "AIC-T0307", "no common type: i32 and u64",
                        src, "a + b64");
        check_record_at(&p, 2, "AIC-T0307", "no common type: i32 and u64",
                        src, "a < b64");
        check_record_at(&p, 3, "AIC-T0307", "no common type: i32 and u64",
                        src, "a == b64");
    }
    pipeline_free(&p);
}

static void test_site_call_and_return(void)
{
    const char *src =
        "module main;\n"
        "fn f(x: u8, y: i32) -> u8 { return 300; }\n"
        "fn main() -> i32 {\n"
        "  f(400, 0);\n"
        "  f(cast<u8>(1), 2);\n"
        "  var r: i32 = f(500, 2);\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 3);
    if (p.crn >= 3) {
        check_record_at(&p, 0, "AIC-T0307",
                        "no common type: i32 literal 300 cannot be "
                        "implicitly narrowed to u8",
                        src, "300");
        check_record_at(&p, 1, "AIC-T0307",
                        "no common type: i32 literal 400 cannot be "
                        "implicitly narrowed to u8",
                        src, "400");
        check_record_at(&p, 2, "AIC-T0307",
                        "no common type: i32 literal 500 cannot be "
                        "implicitly narrowed to u8",
                        src, "500");
    }
    pipeline_free(&p);
}

static void test_site_array_literal(void)
{
    const char *src =
        "module main;\n"
        "import rt.mem;\n"
        "import rt.io;\n"
        "pub fn main() -> i32 {\n"
        "  var goodbuf: u8[16] = [0u8; 16];\n"
        "  var badbuf: u8[16] = [0; 16];\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        check_record_at(&p, 0, "AIC-T0307",
                        "no common type: i32 literal 0 cannot be "
                        "implicitly narrowed to u8 in array literal",
                        src, "[0; 16]");
    }
    pipeline_free(&p);
}

static void test_site_array_to_slice(void)
{
    const char *src =
        "module main;\n"
        "import rt.mem;\n"
        "import rt.io;\n"
        "pub fn main() -> i32 {\n"
        "  var buf: u8[16] = [0u8; 16];\n"
        "  var s: u8[] = buf;\n"
        "  var t: u8[] = s;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        check_record_at(&p, 0, "AIC-T0307",
                        "implicit array-to-slice conversion is absent",
                        src, "buf");
    }
    pipeline_free(&p);
}

static void test_site_ternary(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var c: bool = true;\n"
        "  var a: i32 = 0;\n"
        "  var b64: u64 = 0u64;\n"
        "  var r: i32 = c ? a : b64;\n"
        "  var q: i32 = c ? a : 5;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        check_record_at(&p, 0, "AIC-T0307", "no common type: i32 and u64",
                        src, "c ? a : b64");
    }
    pipeline_free(&p);
}

static void test_site_struct_init(void)
{
    const char *src =
        "module main;\n"
        "struct P { x: i8; y: i64; }\n"
        "fn main() -> i32 {\n"
        "  var p: P = P { x: 300, y: 1 };\n"
        "  var q: P = P { x: cast<i8>(300), y: 1 };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        check_record_at(&p, 0, "AIC-T0307",
                        "no common type: i32 literal 300 cannot be "
                        "implicitly narrowed to i8",
                        src, "300");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Positive program: every whitelist row and common type, no records
 * ------------------------------------------------------------------------- */

static void test_positive_program(void)
{
    const char *src =
        "module main;\n"
        "import rt.mem;\n"
        "import rt.io;\n"
        "struct P { x: i32; }\n"
        "enum C: u8 { Red }\n"
        "fn id(p: P) -> P { return p; }\n"
        "fn pick(c: bool, a: i32, b: i16) -> i32 {\n"
        "  return c ? a : b;\n"
        "}\n"
        "fn main() -> i32 {\n"
        "  var a: i8 = 1i8;\n"
        "  var b: i16 = a;\n"
        "  var c: i32 = b;\n"
        "  var d: i64 = c;\n"
        "  var e: isize = d;\n"
        "  var f: i64 = e;\n"
        "  var g: u8 = 1u8;\n"
        "  var h: i32 = g;\n"
        "  var i: u64 = 1u64;\n"
        "  var j: usize = i;\n"
        "  var k: u64 = j;\n"
        "  var p: i32* = null;\n"
        "  var q: i32* = p;\n"
        "  var r: i32 = a + c;\n"
        "  var s: i32 = r + b;\n"
        "  var t: i64 = d + g;\n"
        "  var sh: i32 = c << a;\n"
        "  var cmp: bool = a < c;\n"
        "  var eq: bool = p == q;\n"
        "  var bcmp: bool = true == (a < c);\n"
        "  var col: C = C.Red;\n"
        "  var u: P = P { x: c };\n"
        "  var v: P = id(u);\n"
        "  var w: i32 = pick(true, 1, cast<i16>(2));\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.crn == 0);
    if (p.crn > 0) {
        size_t i;
        for (i = 0; i < p.crn; i++) {
            fprintf(stderr, "  [unexpected] %s: %s\n",
                    p.crecs[i]->code, p.crecs[i]->message);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Unknown sites: expressions the 11c subset does not type (no record)
 * ------------------------------------------------------------------------- */

static void test_unknown_sites(void)
{
    /* A standalone array literal has no type (no inference); the
     * conversion site is outside the 11c subset and yields CONVERT_UNKNOWN
     * with no record (the owning package reports it). */
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = [1, 2];\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.crn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Determinism
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    const char *src =
        "module main;\n"
        "var c: i8 = 200;\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline a, b;
    pipeline_run_mem(&a, src);
    pipeline_run_mem(&b, src);
    CHECK(a.cst == CONVERT_DIAG_ERROR && b.cst == CONVERT_DIAG_ERROR);
    CHECK(a.crn == b.crn);
    DiagBuf ea, eb;
    diag_buf_init(&ea);
    diag_buf_init(&eb);
    CHECK(diag_emit_records_sorted(&ea, a.crecs, a.crn));
    CHECK(diag_emit_records_sorted(&eb, b.crecs, b.crn));
    CHECK(diag_buf_ok(&ea) && diag_buf_ok(&eb));
    CHECK(ea.len == eb.len &&
          (ea.len == 0 || memcmp(ea.data, eb.data, ea.len) == 0));
    diag_buf_free(&ea);
    diag_buf_free(&eb);
    pipeline_free(&a);
    pipeline_free(&b);
}

/* ---------------------------------------------------------------------------
 * Corpus anchors: re-execute the three conversion-owned negative fixtures
 * ------------------------------------------------------------------------- */

static void test_corpus_anchor_narrowing(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/18-4-type-implicit-narrowing");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/18-4-type-implicit-narrowing/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        if (!rec_matches(p.crecs[0], "AIC-T0307",
                         "no common type: i32 literal 200 cannot be "
                         "implicitly narrowed to i8",
                         3, 13, 52, 3, 16, 55, 0)) {
            fprintf(stderr, "  [18-4] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld) sec=%zu\n",
                    p.crecs[0]->code, p.crecs[0]->message,
                    (long long)p.crecs[0]->primary_span->start.line,
                    (long long)p.crecs[0]->primary_span->start.col,
                    (long long)p.crecs[0]->primary_span->start.offset,
                    (long long)p.crecs[0]->primary_span->end.line,
                    (long long)p.crecs[0]->primary_span->end.col,
                    (long long)p.crecs[0]->primary_span->end.offset,
                    p.crecs[0]->secondary_count);
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_array_literal(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/18-6-type-array-literal-narrowing");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/18-6-type-array-literal-narrowing/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        if (!rec_matches(p.crecs[0], "AIC-T0307",
                         "no common type: i32 literal 0 cannot be "
                         "implicitly narrowed to u8 in array literal",
                         5, 24, 88, 5, 31, 95, 0)) {
            fprintf(stderr, "  [18-6 array-literal] record mismatch: "
                            "code=%s msg=%s span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.crecs[0]->code, p.crecs[0]->message,
                    (long long)p.crecs[0]->primary_span->start.line,
                    (long long)p.crecs[0]->primary_span->start.col,
                    (long long)p.crecs[0]->primary_span->start.offset,
                    (long long)p.crecs[0]->primary_span->end.line,
                    (long long)p.crecs[0]->primary_span->end.col,
                    (long long)p.crecs[0]->primary_span->end.offset);
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_array_slice(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/18-6-type-array-slice-implicit");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/18-6-type-array-slice-implicit/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_DIAG_ERROR);
    CHECK(p.crn == 1);
    if (p.crn >= 1) {
        if (!rec_matches(p.crecs[0], "AIC-T0307",
                         "implicit array-to-slice conversion is absent",
                         5, 7, 71, 5, 10, 74, 0)) {
            fprintf(stderr, "  [18-6 array-slice] record mismatch: "
                            "code=%s msg=%s span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.crecs[0]->code, p.crecs[0]->message,
                    (long long)p.crecs[0]->primary_span->start.line,
                    (long long)p.crecs[0]->primary_span->start.col,
                    (long long)p.crecs[0]->primary_span->start.offset,
                    (long long)p.crecs[0]->primary_span->end.line,
                    (long long)p.crecs[0]->primary_span->end.col,
                    (long long)p.crecs[0]->primary_span->end.offset);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * main (slices land here progressively; see TDD plan in the header)
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_whitelist_integer_matrix();
    test_whitelist_non_integer();
    test_whitelist_pointer_and_null();
    test_whitelist_composite_identity();
    test_common_type_cases();
    test_site_initializer_narrowing();
    test_site_assign_and_compound();
    test_site_binary_common();
    test_site_call_and_return();
    test_site_array_literal();
    test_site_array_to_slice();
    test_site_ternary();
    test_site_struct_init();
    test_positive_program();
    test_unknown_sites();
    test_determinism();
    test_corpus_anchor_narrowing();
    test_corpus_anchor_array_literal();
    test_corpus_anchor_array_slice();

    if (g_failures == 0) {
        printf("convert_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "convert_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
