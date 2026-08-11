/* bootstrap/src/types/optype_test.c
 *
 * WP-M0-11d unit and integration tests: the explicit cast/wrap pair matrix
 * (spec sec. 11.2 / sec. 11.5), the build-level operator/typing checks
 * (sec. 10.2, sec. 11.2, sec. 11.4, sec. 12.7, sec. 13.1, sec. 13.2,
 * sec. 7.3) with the AIC-T0304/05/06/08/09/10/11/12/13 rejections at their
 * corpus-pinned spans, determinism, and re-execution of the nine
 * optype-owned negative-corpus anchors against the real fixture files under
 * tests/negative/cases/ (read-only; owned by WP-M0-03).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-optype' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/types/optype_test.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-optype/optype_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-optype)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "optype.h"

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
 * Cast pair matrix (spec sec. 11.2)
 * ------------------------------------------------------------------------- */

static void test_cast_pair_integer_matrix(void)
{
    const AstPrimKind ints[] = {
        AST_PRIM_I8, AST_PRIM_I16, AST_PRIM_I32, AST_PRIM_I64,
        AST_PRIM_U8, AST_PRIM_U16, AST_PRIM_U32, AST_PRIM_U64,
        AST_PRIM_ISIZE, AST_PRIM_USIZE
    };
    size_t i, j;
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            Type *ta = prim(ints[i]);
            Type *tb = prim(ints[j]);
            CHECK(ta != NULL && tb != NULL);
            if (ta && tb) {
                /* integer -> integer is valid for cast and wrap both */
                CHECK(optype_cast_pair_valid(ta, false, tb));
                CHECK(optype_wrap_pair_valid(ta, false, tb));
            }
            type_free(ta);
            type_free(tb);
        }
    }
}

static void test_cast_pair_bool(void)
{
    Type *b = prim(AST_PRIM_BOOL);
    Type *i32 = prim(AST_PRIM_I32);
    Type *str = prim(AST_PRIM_STR);
    CHECK(b && i32 && str);
    if (b && i32 && str) {
        /* bool <-> integer: cast only, never wrap */
        CHECK(optype_cast_pair_valid(b, false, i32));
        CHECK(optype_cast_pair_valid(i32, false, b));
        CHECK(!optype_wrap_pair_valid(b, false, i32));
        CHECK(!optype_wrap_pair_valid(i32, false, b));
        /* bool -> str: invalid both */
        CHECK(!optype_cast_pair_valid(b, false, str));
        CHECK(!optype_wrap_pair_valid(b, false, str));
        /* identity */
        CHECK(optype_cast_pair_valid(b, false, b));
    }
    type_free(b);
    type_free(i32);
    type_free(str);
}

static void test_cast_pair_enum(void)
{
    /* Enum types need real symbols; construct via a minimal parsed enum in
     * the integration tests below. Here we only exercise the non-enum
     * rejection path with NULL (defensive) and identity. */
    Type *i32 = prim(AST_PRIM_I32);
    Type *ptr = type_ptr_new(prim(AST_PRIM_I32));
    CHECK(i32 && ptr);
    if (i32 && ptr) {
        CHECK(!optype_cast_pair_valid(NULL, false, i32));
        CHECK(!optype_wrap_pair_valid(NULL, false, i32));
        /* null -> any T* is a valid cast pair */
        CHECK(optype_cast_pair_valid(NULL, true, ptr));
        CHECK(!optype_wrap_pair_valid(NULL, true, ptr));
        CHECK(!optype_cast_pair_valid(NULL, true, i32));
    }
    type_free(i32);
    type_free(ptr);
}

static void test_cast_pair_pointer_and_str(void)
{
    Type *pi32 = type_ptr_new(prim(AST_PRIM_I32));
    Type *pi64 = type_ptr_new(prim(AST_PRIM_I64));
    Type *pu8 = type_ptr_new(prim(AST_PRIM_U8));
    Type *u64 = prim(AST_PRIM_U64);
    Type *i64 = prim(AST_PRIM_I64);
    Type *usize = prim(AST_PRIM_USIZE);
    Type *u32 = prim(AST_PRIM_U32);
    Type *str = prim(AST_PRIM_STR);
    Type *u8s = type_slice_new(prim(AST_PRIM_U8));
    Type *u32s = type_slice_new(prim(AST_PRIM_U32));
    CHECK(pi32 && pi64 && pu8 && u64 && i64 && usize && u32 && str && u8s && u32s);
    if (pi32 && pi64 && pu8 && u64 && i64 && usize && u32 && str && u8s && u32s) {
        /* pointer <-> usize/u64/isize/i64: cast only */
        CHECK(optype_cast_pair_valid(pi32, false, usize));
        CHECK(optype_cast_pair_valid(pi32, false, u64));
        CHECK(optype_cast_pair_valid(pi32, false, i64));
        CHECK(!optype_cast_pair_valid(pi32, false, u32));
        CHECK(!optype_wrap_pair_valid(pi32, false, usize));
        /* integer -> pointer */
        CHECK(optype_cast_pair_valid(u64, false, pi32));
        CHECK(optype_cast_pair_valid(u32, false, pi32));
        CHECK(!optype_wrap_pair_valid(u32, false, pi32));
        /* T* -> U* (bit-preserving) */
        CHECK(optype_cast_pair_valid(pi32, false, pi64));
        CHECK(optype_cast_pair_valid(pi32, false, pu8));
        CHECK(!optype_wrap_pair_valid(pi32, false, pi64));
        /* str <-> u8[] */
        CHECK(optype_cast_pair_valid(str, false, u8s));
        CHECK(optype_cast_pair_valid(u8s, false, str));
        CHECK(!optype_cast_pair_valid(str, false, u32s));
        CHECK(!optype_cast_pair_valid(u32s, false, str));
        CHECK(!optype_wrap_pair_valid(str, false, u8s));
        /* slice identity */
        CHECK(optype_cast_pair_valid(u8s, false, u8s));
        /* non-convertible pairs */
        CHECK(!optype_cast_pair_valid(str, false, u32s));
    }
    type_free(pi32);
    type_free(pi64);
    type_free(pu8);
    type_free(u64);
    type_free(i64);
    type_free(usize);
    type_free(u32);
    type_free(str);
    type_free(u8s);
    type_free(u32s);
}

static void test_wrap_target_rule(void)
{
    /* wrap requires an integer target and integer/enum source; identity
     * included. str/bool/pointer targets are never valid. */
    Type *i32 = prim(AST_PRIM_I32);
    Type *b = prim(AST_PRIM_BOOL);
    Type *str = prim(AST_PRIM_STR);
    Type *ptr = type_ptr_new(prim(AST_PRIM_I32));
    CHECK(i32 && b && str && ptr);
    if (i32 && b && str && ptr) {
        CHECK(optype_wrap_pair_valid(i32, false, i32));
        CHECK(!optype_wrap_pair_valid(b, false, i32));
        CHECK(!optype_wrap_pair_valid(str, false, i32));
        CHECK(!optype_wrap_pair_valid(ptr, false, i32));
        CHECK(!optype_wrap_pair_valid(i32, false, b));
        CHECK(!optype_wrap_pair_valid(i32, false, str));
        CHECK(!optype_wrap_pair_valid(i32, false, ptr));
        CHECK(!optype_wrap_pair_valid(NULL, false, i32));
    }
    type_free(i32);
    type_free(b);
    type_free(str);
    type_free(ptr);
}

/* ---------------------------------------------------------------------------
 * Shared pipeline: read bytes -> load -> lex -> parse -> name_resolve ->
 * types_check_completeness -> types_layout_build -> types_convert_check ->
 * types_optype_check.
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
    DiagRecord **orecs;     /* optype records (AIC-T03xx) */
    size_t orn;
    OptypeStatus ost;
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
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
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
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    layout_build_free(p->build);
    types_records_free(p->lrecs, p->lrn);
    types_records_free(p->crecs, p->crn);
    types_records_free(p->orecs, p->orn);
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
    if (!rec_matches(p->orecs[idx], code, message, sl, sc, so, el, ec, eo,
                     0)) {
        g_failures++;
        fprintf(stderr, "FAIL record %zu: code=%s msg=%s span=(%lld,%lld,%lld)"
                        "-(%lld,%lld,%lld) sec=%zu (want %s at '%s')\n",
                idx, p->orecs[idx]->code, p->orecs[idx]->message,
                (long long)p->orecs[idx]->primary_span->start.line,
                (long long)p->orecs[idx]->primary_span->start.col,
                (long long)p->orecs[idx]->primary_span->start.offset,
                (long long)p->orecs[idx]->primary_span->end.line,
                (long long)p->orecs[idx]->primary_span->end.col,
                (long long)p->orecs[idx]->primary_span->end.offset,
                p->orecs[idx]->secondary_count,
                message ? message : "(any)", needle);
        g_checks++;
    }
}

/* ---------------------------------------------------------------------------
 * Equality on array/struct (AIC-T0304)
 * ------------------------------------------------------------------------- */

static void test_site_struct_equality(void)
{
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "fn main() -> i32 {\n"
        "  var a: Point = Point { x: 1, y: 2 };\n"
        "  var b: Point = Point { x: 1, y: 2 };\n"
        "  var eq: bool = a == b;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0304",
                        "'==' operator not applicable to struct type 'Point'",
                        src, "==");
    }
    pipeline_free(&p);
}

static void test_site_array_equality(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var a: i32[2] = [1, 2];\n"
        "  var b: i32[2] = [1, 2];\n"
        "  var eq: bool = a == b;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0304",
                        "'==' operator not applicable to array type 'i32[2]'",
                        src, "==");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Chained comparison (AIC-T0305)
 * ------------------------------------------------------------------------- */

static void test_site_chained_comparison(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = 5;\n"
        "  var r: bool = 1 < x < 10;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0305",
                        "chained comparison is not allowed",
                        src, "<");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Operator not applicable (AIC-T0306)
 * ------------------------------------------------------------------------- */

static void test_site_binary_bool_plus(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var a: bool = true;\n"
        "  var b: bool = false;\n"
        "  var r: bool = a + b;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'+' operator not applicable to operand type 'bool'",
                        src, "+");
    }
    pipeline_free(&p);
}

static void test_site_binary_mismatch(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var a: i32 = 1;\n"
        "  var s: str = \"x\";\n"
        "  var r: i32 = a * s;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'*' operator not applicable to operand type 'i32'",
                        src, "*");
    }
    pipeline_free(&p);
}

static void test_site_binary_shift_bool(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var b: bool = true;\n"
        "  var r: bool = b << 1;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'<<' operator not applicable to operand type 'bool'",
                        src, "<<");
    }
    pipeline_free(&p);
}

static void test_site_unary_unsigned_neg(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var u: u32 = 1u32;\n"
        "  var r: u32 = -u;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        /* needle "-" would match the "-" of "->" in the fn signature first,
         * so assert the pinned operator-token span directly (unary_op_span:
         * non-ws run between the node start and the operand start) */
        if (!rec_matches(p.orecs[0], "AIC-T0306",
                         "'-' operator not applicable to operand type 'u32'",
                         4, 16, 68, 4, 17, 69, 0)) {
            fprintf(stderr, "  [T0306] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_site_unary_not_int(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var i: i32 = 1;\n"
        "  var r: bool = !i;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'!' operator not applicable to operand type 'i32'",
                        src, "!");
    }
    pipeline_free(&p);
}

static void test_site_deref_non_pointer(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var i: i32 = 1;\n"
        "  var r: i32 = *i;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'*' operator not applicable to operand type 'i32'",
                        src, "*");
    }
    pipeline_free(&p);
}

static void test_site_len_non_array(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var i: i32 = 1;\n"
        "  var r: usize = len(i);\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'len' operator not applicable to operand type 'i32'",
                        src, "len");
    }
    pipeline_free(&p);
}

static void test_site_member_non_struct(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var i: i32 = 1;\n"
        "  var r: i32 = i.field;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'.' operator not applicable to operand type 'i32'",
                        src, ".");
    }
    pipeline_free(&p);
}

static void test_site_struct_literal_non_struct(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var i: i32 = 5;\n"
        "  var r: i32 = i { x: 1 };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_UNKNOWN);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'{}' operator not applicable to operand type 'i32'",
                        src, "i { x: 1 }");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Cast/wrap pair rejections (AIC-T0308) and void misuse (AIC-T0306)
 * ------------------------------------------------------------------------- */

static void test_site_invalid_cast(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var s: str = \"hello\";\n"
        "  var x: i32 = cast<i32>(s);\n"
        "  return x;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0308",
                        "invalid explicit cast pair: str to i32",
                        src, "cast<i32>(s)");
    }
    pipeline_free(&p);
}

static void test_site_invalid_wrap(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var s: str = \"hello\";\n"
        "  var x: i32 = wrap<i32>(s);\n"
        "  return x;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0308",
                        "invalid explicit wrap pair: str to i32",
                        src, "wrap<i32>(s)");
    }
    pipeline_free(&p);
}

static void test_site_cast_void_misuse(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = 1;\n"
        "  var v: void = cast<void>(x);\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0306",
                        "'cast' operator not applicable to operand type 'void'",
                        src, "cast<void>(x)");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Array literal element count (AIC-T0309)
 * ------------------------------------------------------------------------- */

static void test_site_array_count_mismatch(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var arr: i32[3] = [1, 2];\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0309",
                        "array literal element count mismatch: expected 3, "
                        "found 2",
                        src, "[1, 2]");
    }
    pipeline_free(&p);
}

static void test_site_array_count_repeat(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var arr: i32[4] = [0; 3];\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0309",
                        "array literal element count mismatch: expected 4, "
                        "found 3",
                        src, "[0; 3]");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Condition must be bool (AIC-T0310)
 * ------------------------------------------------------------------------- */

static void test_site_condition_not_bool(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = 5;\n"
        "  if (x) { return 1; }\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0310",
                        "condition must be bool, found i32", src, "x");
    }
    pipeline_free(&p);
}

static void test_site_while_condition(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var s: str = \"x\";\n"
        "  while (s) { break; }\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0310",
                        "condition must be bool, found str", src, "s");
    }
    pipeline_free(&p);
}

static void test_site_ternary_condition(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var x: i32 = 5;\n"
        "  var r: i32 = x ? 1 : 2;\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0310",
                        "condition must be bool, found i32", src, "x");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Switch selector (AIC-T0311)
 * ------------------------------------------------------------------------- */

static void test_site_switch_selector(void)
{
    const char *src =
        "module main;\n"
        "fn main() -> i32 {\n"
        "  var s: str = \"hello\";\n"
        "  switch (s) {\n"
        "    case \"hello\": { return 1; }\n"
        "    default: { return 0; }\n"
        "  }\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0311",
                        "switch selector must be integer or enum type, "
                        "found str", src, "s");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Call argument count (AIC-T0312)
 * ------------------------------------------------------------------------- */

static void test_site_arg_count_mismatch(void)
{
    const char *src =
        "module main;\n"
        "fn add(a: i32, b: i32) -> i32 { return a + b; }\n"
        "fn main() -> i32 {\n"
        "  var r: i32 = add(1);\n"
        "  return r;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0312",
                        "call argument count mismatch: expected 2, found 1",
                        src, "add(1)");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Struct literal fields (AIC-T0313)
 * ------------------------------------------------------------------------- */

static void test_site_struct_field_unknown(void)
{
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "fn main() -> i32 {\n"
        "  var p: Point = Point { x: 1, z: 2 };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0313",
                        "unknown field 'z' in struct literal of type 'Point'",
                        src, "z");
    }
    pipeline_free(&p);
}

static void test_site_struct_field_duplicate(void)
{
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "fn main() -> i32 {\n"
        "  var p: Point = Point { x: 1, y: 2, y: 3 };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        /* the record span is the repeated field-name token only */
        int64_t sl, sc, so, el, ec, eo;
        span_of(src, "y: 3", &sl, &sc, &so, &el, &ec, &eo);
        if (!rec_matches(p.orecs[0], "AIC-T0313",
                         "duplicate field 'y' in struct literal of type "
                         "'Point'",
                         sl, sc, so, sl, sc + 1, so + 1, 0)) {
            g_failures++;
            fprintf(stderr, "FAIL duplicate field record: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_checks++;
        }
    }
    pipeline_free(&p);
}

static void test_site_struct_field_missing(void)
{
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "fn main() -> i32 {\n"
        "  var p: Point = Point { x: 1 };\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        check_record_at(&p, 0, "AIC-T0313",
                        "missing field 'y' in struct literal of type 'Point'",
                        src, "Point { x: 1 }");
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Positive program: every applicable operator and valid cast/wrap, no
 * records
 * ------------------------------------------------------------------------- */

static void test_positive_program(void)
{
    const char *src =
        "module main;\n"
        "import rt.mem;\n"
        "import rt.io;\n"
        "struct P { x: i32; }\n"
        "enum C: u8 { Red, Green }\n"
        "fn id(p: P) -> P { return p; }\n"
        "fn pick(c: bool, a: i32, b: i16) -> i32 {\n"
        "  return c ? a : b;\n"
        "}\n"
        "fn main() -> i32 {\n"
        "  var a: i8 = 1i8;\n"
        "  var b: i32 = a;\n"
        "  var c: i64 = b;\n"
        "  var d: u32 = 1u32;\n"
        "  var e: i64 = d;\n"
        "  var p: i32* = null;\n"
        "  var q: i32* = p;\n"
        "  var eq: bool = p == q;\n"
        "  var lt: bool = b < c;\n"
        "  var le: bool = b <= 4;\n"
        "  var g: bool = b > 1;\n"
        "  var ge: bool = b >= 1;\n"
        "  var ne: bool = b != c;\n"
        "  var land: bool = true && false;\n"
        "  var lor: bool = true || false;\n"
        "  var neg: i32 = -b;\n"
        "  var plus: i32 = +b;\n"
        "  var bnot: i32 = ~b;\n"
        "  var lnot: bool = !true;\n"
        "  var shl: i32 = b << 1;\n"
        "  var shr: i32 = b >> 1;\n"
        "  var add: i64 = b + c;\n"
        "  var sub: i32 = b - 1;\n"
        "  var mul: i32 = b * 2;\n"
        "  var div: i32 = b / 2;\n"
        "  var mod: i32 = b % 2;\n"
        "  var band: i32 = b & 1;\n"
        "  var bor: i32 = b | 1;\n"
        "  var bxor: i32 = b ^ 1;\n"
        "  var arr: i32[3] = [1, 2, 3];\n"
        "  var rep: u8[4] = [0u8; 4];\n"
        "  var idx: i32 = arr[1];\n"
        "  var sl: i32[] = arr[..];\n"
        "  var sl2: i32[] = sl;\n"
        "  var lens: usize = len(arr);\n"
        "  var ptrs: i32* = ptr(arr);\n"
        "  var s: str = \"hi\";\n"
        "  var lens2: usize = len(s);\n"
        "  var sc: str = cast<str>(s);\n"
        "  var ci: i64 = cast<i64>(b);\n"
        "  var cu: u32 = wrap<u32>(c);\n"
        "  var cb: bool = cast<bool>(b);\n"
        "  var bi: i32 = cast<i32>(cb);\n"
        "  var col: C = C.Red;\n"
        "  var col2: C = cast<C>(1);\n"
        "  var underlying: u8 = cast<u8>(col);\n"
        "  var u: P = P { x: b };\n"
        "  var v: P = id(u);\n"
        "  var w: i32 = pick(true, 1, cast<i16>(2));\n"
        "  var sz: usize = sizeof(i32);\n"
        "  var al: usize = alignof(i32);\n"
        "  return 0;\n"
        "}\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.cst == CONVERT_OK);
    CHECK(p.ost == OPTYPE_OK);
    CHECK(p.orn == 0);
    if (p.orn > 0) {
        size_t i;
        for (i = 0; i < p.orn; i++) {
            fprintf(stderr, "  [unexpected] %s: %s\n",
                    p.orecs[i]->code, p.orecs[i]->message);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Unknown sites: expressions the 11d subset does not type (no record)
 * ------------------------------------------------------------------------- */

static void test_unknown_sites(void)
{
    /* A standalone array literal has no type (no inference); the count
     * check requires a known array destination and yields UNKNOWN with no
     * record here. */
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
    CHECK(p.ost == OPTYPE_UNKNOWN);
    CHECK(p.orn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Determinism
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    const char *src =
        "module main;\n"
        "struct Point { x: i32; y: i32; }\n"
        "fn main() -> i32 {\n"
        "  var a: Point = Point { x: 1, y: 2 };\n"
        "  var b: Point = Point { x: 1, y: 2 };\n"
        "  var eq: bool = a == b;\n"
        "  var r: bool = a + b;\n"
        "  return 0;\n"
        "}\n";
    Pipeline a, b;
    pipeline_run_mem(&a, src);
    pipeline_run_mem(&b, src);
    CHECK(a.ost == OPTYPE_DIAG_ERROR && b.ost == OPTYPE_DIAG_ERROR);
    CHECK(a.orn == b.orn);
    DiagBuf ea, eb;
    diag_buf_init(&ea);
    diag_buf_init(&eb);
    CHECK(diag_emit_records_sorted(&ea, a.orecs, a.orn));
    CHECK(diag_emit_records_sorted(&eb, b.orecs, b.orn));
    CHECK(diag_buf_ok(&ea) && diag_buf_ok(&eb));
    CHECK(ea.len == eb.len &&
          (ea.len == 0 || memcmp(ea.data, eb.data, ea.len) == 0));
    diag_buf_free(&ea);
    diag_buf_free(&eb);
    pipeline_free(&a);
    pipeline_free(&b);
}

/* ---------------------------------------------------------------------------
 * Corpus anchors: re-execute the nine optype-owned negative fixtures
 * ------------------------------------------------------------------------- */

static void test_corpus_anchor_operator_not_applicable(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-operator-not-applicable");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-operator-not-applicable/"
             "input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0306",
                         "'+' operator not applicable to operand type 'bool'",
                         5, 19, 95, 5, 20, 96, 0)) {
            fprintf(stderr, "  [T0306] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_struct_equality(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-struct-equality");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-struct-equality/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0304",
                         "'==' operator not applicable to struct type 'Point'",
                         6, 20, 162, 6, 22, 164, 0)) {
            fprintf(stderr, "  [T0304] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_chained_comparison(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-chained-comparison");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-chained-comparison/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0305",
                         "chained comparison is not allowed",
                         4, 19, 68, 4, 20, 69, 0)) {
            fprintf(stderr, "  [T0305] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_condition_not_bool(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-condition-not-bool");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-condition-not-bool/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0310",
                         "condition must be bool, found i32",
                         3, 7, 38, 3, 8, 39, 0)) {
            fprintf(stderr, "  [T0310] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_invalid_cast(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-invalid-cast");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-invalid-cast/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0308",
                         "invalid explicit cast pair: str to i32",
                         4, 16, 71, 4, 28, 83, 0)) {
            fprintf(stderr, "  [T0308] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_array_count_mismatch(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-array-count-mismatch");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-array-count-mismatch/"
             "input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0309",
                         "array literal element count mismatch: expected 3, "
                         "found 2",
                         3, 21, 52, 3, 27, 58, 0)) {
            fprintf(stderr, "  [T0309] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_switch_selector(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-switch-selector-type");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-switch-selector-type/"
             "input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0311",
                         "switch selector must be integer or enum type, "
                         "found str",
                         3, 7, 38, 3, 8, 39, 0)) {
            fprintf(stderr, "  [T0311] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_arg_count_mismatch(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-arg-count-mismatch");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-arg-count-mismatch/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0312",
                         "call argument count mismatch: expected 2, found 1",
                         4, 16, 95, 4, 22, 101, 0)) {
            fprintf(stderr, "  [T0312] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

static void test_corpus_anchor_struct_field_error(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-struct-field-error");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-struct-field-error/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.ost == OPTYPE_DIAG_ERROR);
    CHECK(p.orn == 1);
    if (p.orn >= 1) {
        if (!rec_matches(p.orecs[0], "AIC-T0313",
                         "unknown field 'z' in struct literal of type 'Point'",
                         4, 32, 96, 4, 33, 97, 0)) {
            fprintf(stderr, "  [T0313] record mismatch: code=%s msg=%s "
                            "span=(%lld,%lld,%lld)-(%lld,%lld,%lld)\n",
                    p.orecs[0]->code, p.orecs[0]->message,
                    (long long)p.orecs[0]->primary_span->start.line,
                    (long long)p.orecs[0]->primary_span->start.col,
                    (long long)p.orecs[0]->primary_span->start.offset,
                    (long long)p.orecs[0]->primary_span->end.line,
                    (long long)p.orecs[0]->primary_span->end.col,
                    (long long)p.orecs[0]->primary_span->end.offset);
            g_failures++;
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_cast_pair_integer_matrix();
    test_cast_pair_bool();
    test_cast_pair_enum();
    test_cast_pair_pointer_and_str();
    test_wrap_target_rule();
    test_site_struct_equality();
    test_site_array_equality();
    test_site_chained_comparison();
    test_site_binary_bool_plus();
    test_site_binary_mismatch();
    test_site_binary_shift_bool();
    test_site_unary_unsigned_neg();
    test_site_unary_not_int();
    test_site_deref_non_pointer();
    test_site_len_non_array();
    test_site_member_non_struct();
    test_site_struct_literal_non_struct();
    test_site_invalid_cast();
    test_site_invalid_wrap();
    test_site_cast_void_misuse();
    test_site_array_count_mismatch();
    test_site_array_count_repeat();
    test_site_condition_not_bool();
    test_site_while_condition();
    test_site_ternary_condition();
    test_site_switch_selector();
    test_site_arg_count_mismatch();
    test_site_struct_field_unknown();
    test_site_struct_field_duplicate();
    test_site_struct_field_missing();
    test_positive_program();
    test_unknown_sites();
    test_determinism();
    test_corpus_anchor_operator_not_applicable();
    test_corpus_anchor_struct_equality();
    test_corpus_anchor_chained_comparison();
    test_corpus_anchor_condition_not_bool();
    test_corpus_anchor_invalid_cast();
    test_corpus_anchor_array_count_mismatch();
    test_corpus_anchor_switch_selector();
    test_corpus_anchor_arg_count_mismatch();
    test_corpus_anchor_struct_field_error();

    if (g_failures == 0) {
        printf("optype_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "optype_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}




