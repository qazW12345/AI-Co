/* bootstrap/src/types/layout_test.c
 *
 * WP-M0-11b unit and integration tests: struct layout (spec sec. 7.4:
 * declaration order, alignment, size rounding), deterministic padding
 * (sec. 9.4: exact padding byte ranges), enum layout (sec. 7.5:
 * continuation, aliasing, size/alignment = underlying, AIC-T0301
 * representability with the corpus-pinned message and span), the bounded
 * constant-integer subset, determinism, and re-execution of the
 * enum-owned negative-corpus anchor against the real fixture file under
 * tests/negative/cases/ (read-only; owned by WP-M0-03).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-layout' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/types/layout_test.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c bootstrap/src/name/name.c \
 *     bootstrap/src/ast/ast.c bootstrap/src/parse/parse.c \
 *     bootstrap/src/lex/lex.c bootstrap/src/load/load.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-layout/layout_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-layout)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "layout.h"

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
 * types_check_completeness -> types_layout_build.
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
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    layout_build_free(p->build);
    types_records_free(p->lrecs, p->lrn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    free(p->bytes);
    memset(p, 0, sizeof(*p));
}

/* ---------------------------------------------------------------------------
 * Decl/symbol lookup helpers
 * ------------------------------------------------------------------------- */

static const AstNode *find_type_decl(const AstNode *program, const char *name,
                                     AstNodeKind kind)
{
    size_t i;
    if (!program) return NULL;
    for (i = 0; i < program->u.program.ndecls; i++) {
        const AstNode *d = program->u.program.decls[i];
        const char *dname = NULL;
        if (d->kind == AST_STRUCT_DECL) dname = d->u.struct_decl.name;
        else if (d->kind == AST_ENUM_DECL) dname = d->u.enum_decl.name;
        if (d->kind == kind && dname && strcmp(dname, name) == 0) return d;
    }
    return NULL;
}

static const NameSymbol *find_sym(const Pipeline *p, const char *name)
{
    if (!p->result || p->result->nmodules == 0) return NULL;
    return name_module_lookup(p->result->modules[0], name);
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
 * Type size/alignment from AST type nodes (spec sec. 7.1/7.2)
 * ------------------------------------------------------------------------- */

static void test_type_info_sizes(void)
{
    const char *src =
        "module main;\n"
        "struct P { x: i32; y: i32; }\n"
        "struct S { s: str; p: i32*; sl: u8[]; a: u8[4]; z: i32[0]; }\n"
        "struct N { m: i32[2][3]; }\n"
        "enum C: u8 { Red }\n"
        "struct U { c: C; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    if (p.program && p.build) {
        const NameModule *mod = p.result->modules[0];
        const AstNode *decl = find_type_decl(p.program, "S", AST_STRUCT_DECL);
        CHECK(decl != NULL);
        if (decl) {
            LayoutSizeAlign info;
            const AstNode *f0 = decl->u.struct_decl.fields[0]; /* str */
            const AstNode *f1 = decl->u.struct_decl.fields[1]; /* i32* */
            const AstNode *f2 = decl->u.struct_decl.fields[2]; /* u8[] */
            const AstNode *f3 = decl->u.struct_decl.fields[3]; /* u8[4] */
            const AstNode *f4 = decl->u.struct_decl.fields[4]; /* i32[0] */
            CHECK(layout_build_type_info(p.build, mod, f0->u.named.type,
                                         &info) == LAYOUT_OK);
            CHECK(info.size == 16 && info.align == 8);
            CHECK(layout_build_type_info(p.build, mod, f1->u.named.type,
                                         &info) == LAYOUT_OK);
            CHECK(info.size == 8 && info.align == 8);
            CHECK(layout_build_type_info(p.build, mod, f2->u.named.type,
                                         &info) == LAYOUT_OK);
            CHECK(info.size == 16 && info.align == 8);
            CHECK(layout_build_type_info(p.build, mod, f3->u.named.type,
                                         &info) == LAYOUT_OK);
            CHECK(info.size == 4 && info.align == 1);
            CHECK(layout_build_type_info(p.build, mod, f4->u.named.type,
                                         &info) == LAYOUT_OK);
            CHECK(info.size == 0 && info.align == 4);
        }
        /* nested i32[2][3]: 2 * 4 = 8 for inner, then 3 * 8 = 24 */
        {
            const AstNode *n = find_type_decl(p.program, "N", AST_STRUCT_DECL);
            CHECK(n != NULL);
            if (n) {
                LayoutSizeAlign info;
                CHECK(layout_build_type_info(
                          p.build, mod,
                          n->u.struct_decl.fields[0]->u.named.type,
                          &info) == LAYOUT_OK);
                CHECK(info.size == 24 && info.align == 4);
            }
        }
        /* enum field: same size/alignment as the underlying type */
        {
            const AstNode *u = find_type_decl(p.program, "U", AST_STRUCT_DECL);
            CHECK(u != NULL);
            if (u) {
                LayoutSizeAlign info;
                CHECK(layout_build_type_info(
                          p.build, mod,
                          u->u.struct_decl.fields[0]->u.named.type,
                          &info) == LAYOUT_OK);
                CHECK(info.size == 1 && info.align == 1);
            }
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Struct layout (spec sec. 7.4)
 * ------------------------------------------------------------------------- */

static const LayoutStruct *layout_for(Pipeline *p, const char *name)
{
    const NameSymbol *sym = find_sym(p, name);
    if (!sym) return NULL;
    return layout_build_struct(p->build, sym);
}

static void test_struct_basic(void)
{
    const char *src =
        "module main;\n"
        "struct P { x: i32; y: i32; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *ls = layout_for(&p, "P");
        CHECK(ls != NULL);
        if (ls) {
            CHECK(ls->size == 8 && ls->align == 4);
            CHECK(ls->nfields == 2);
            CHECK(ls->tail_padding == 0);
            if (ls->nfields == 2) {
                CHECK(strcmp(ls->fields[0].name, "x") == 0);
                CHECK(ls->fields[0].offset == 0);
                CHECK(ls->fields[0].pad_before == 0);
                CHECK(ls->fields[0].type.size == 4 &&
                      ls->fields[0].type.align == 4);
                CHECK(strcmp(ls->fields[1].name, "y") == 0);
                CHECK(ls->fields[1].offset == 4);
                CHECK(ls->fields[1].pad_before == 0);
                CHECK(ls->fields[1].type.size == 4);
            }
        }
    }
    pipeline_free(&p);
}

static void test_struct_alignment_padding(void)
{
    /* a:u8 @0; pad 3; b:i32 @4; c:u8 @8; tail pad 3 -> size 12, align 4 */
    const char *src =
        "module main;\n"
        "struct M { a: u8; b: i32; c: u8; }\n"
        "struct N { a: i32; b: u8; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *m = layout_for(&p, "M");
        CHECK(m != NULL);
        if (m) {
            CHECK(m->size == 12 && m->align == 4);
            CHECK(m->nfields == 3 && m->tail_padding == 3);
            if (m->nfields == 3) {
                CHECK(m->fields[0].offset == 0 && m->fields[0].pad_before == 0);
                CHECK(m->fields[1].offset == 4 && m->fields[1].pad_before == 3);
                CHECK(m->fields[2].offset == 8 && m->fields[2].pad_before == 0);
            }
        }
        const LayoutStruct *n = layout_for(&p, "N");
        CHECK(n != NULL);
        if (n) {
            CHECK(n->size == 8 && n->align == 4 && n->tail_padding == 3);
            if (n->nfields == 2) {
                CHECK(n->fields[0].offset == 0);
                CHECK(n->fields[1].offset == 4 && n->fields[1].pad_before == 0);
            }
        }
    }
    pipeline_free(&p);
}

static void test_struct_str_ptr_slice(void)
{
    /* str @0 (16/8); i32* @16 (8/8); u8[] @24 (16/8) -> size 40, align 8 */
    const char *src =
        "module main;\n"
        "struct S { s: str; p: i32*; sl: u8[]; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *ls = layout_for(&p, "S");
        CHECK(ls != NULL);
        if (ls) {
            CHECK(ls->size == 40 && ls->align == 8 && ls->tail_padding == 0);
            if (ls->nfields == 3) {
                CHECK(ls->fields[0].offset == 0);
                CHECK(ls->fields[1].offset == 16);
                CHECK(ls->fields[2].offset == 24);
            }
        }
    }
    pipeline_free(&p);
}

static void test_struct_embedded(void)
{
    /* Inner {x:i32} = 4/4; Outer: Inner @0 (4/4), b:u8 @4, tail 3 -> 8/4 */
    const char *src =
        "module main;\n"
        "struct Inner { x: i32; }\n"
        "struct Outer { i: Inner; b: u8; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *inner = layout_for(&p, "Inner");
        const LayoutStruct *outer = layout_for(&p, "Outer");
        CHECK(inner != NULL && outer != NULL);
        if (inner) CHECK(inner->size == 4 && inner->align == 4);
        if (outer) {
            CHECK(outer->size == 8 && outer->align == 4);
            if (outer->nfields == 2) {
                CHECK(outer->fields[0].offset == 0);
                CHECK(outer->fields[0].type.size == 4 &&
                      outer->fields[0].type.align == 4);
                CHECK(outer->fields[1].offset == 4 &&
                      outer->fields[1].pad_before == 0);
            }
        }
    }
    pipeline_free(&p);
}

static void test_struct_array_fields(void)
{
    const char *src =
        "module main;\n"
        "struct A { b: u8[4]; }\n"
        "struct B { c: i32[2]; }\n"
        "struct E { z: i32[0]; }\n"
        "struct Empty {}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *a = layout_for(&p, "A");
        const LayoutStruct *b = layout_for(&p, "B");
        const LayoutStruct *e = layout_for(&p, "E");
        const LayoutStruct *empty = layout_for(&p, "Empty");
        CHECK(a && b && e && empty);
        if (a) CHECK(a->size == 4 && a->align == 1 && a->nfields == 1);
        if (b) CHECK(b->size == 8 && b->align == 4);
        if (e) {
            /* empty array contributes no size but its alignment applies */
            CHECK(e->size == 0 && e->align == 4 && e->nfields == 1);
            if (e->nfields == 1) {
                CHECK(e->fields[0].offset == 0 &&
                      e->fields[0].type.size == 0 &&
                      e->fields[0].type.align == 4);
            }
        }
        if (empty) CHECK(empty->size == 0 && empty->align == 1 &&
                         empty->nfields == 0 && empty->tail_padding == 0);
    }
    pipeline_free(&p);
}

static void test_struct_padding_ranges(void)
{
    /* M: gaps {3,3} before b and tail {9,3}; P: none */
    const char *src =
        "module main;\n"
        "struct M { a: u8; b: i32; c: u8; }\n"
        "struct P { x: i32; y: i32; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *m = layout_for(&p, "M");
        const LayoutStruct *pl = layout_for(&p, "P");
        LayoutPadRange ranges[8];
        size_t n = 0;
        CHECK(m && pl);
        if (m) {
            CHECK(layout_struct_padding(m, NULL, 0) == 2);
            n = layout_struct_padding(m, ranges, 8);
            CHECK(n == 2);
            if (n == 2) {
                /* gap between a(u8@0, ends 1) and b(i32@4): {1,3};
                 * tail after c(u8@8, ends 9) to size 12: {9,3} */
                CHECK(ranges[0].offset == 1 && ranges[0].length == 3);
                CHECK(ranges[1].offset == 9 && ranges[1].length == 3);
            }
        }
        if (pl) {
            CHECK(layout_struct_padding(pl, ranges, 8) == 0);
            CHECK(pl->tail_padding == 0);
        }
        /* deterministic: two computations give identical ranges */
        if (m) {
            LayoutPadRange r2[8];
            size_t n2 = layout_struct_padding(m, r2, 8);
            CHECK(n2 == n);
            if (n2 == n) {
                CHECK(memcmp(ranges, r2, n * sizeof(LayoutPadRange)) == 0);
            }
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Enum layout (spec sec. 7.5)
 * ------------------------------------------------------------------------- */

static const LayoutEnum *enum_for(Pipeline *p, const char *name)
{
    const NameSymbol *sym = find_sym(p, name);
    if (!sym) return NULL;
    return layout_build_enum(p->build, sym);
}

static void test_enum_values(void)
{
    const char *src =
        "module main;\n"
        "enum A: i32 { X, Y, Z }\n"
        "enum B: i32 { X = 5, Y, Z = 10, W }\n"
        "enum C: i32 { X = 1, Y = 1, Z }\n"
        "enum D: i32 { X = -5, Y }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutEnum *a = enum_for(&p, "A");
        const LayoutEnum *b = enum_for(&p, "B");
        const LayoutEnum *c = enum_for(&p, "C");
        const LayoutEnum *d = enum_for(&p, "D");
        CHECK(a && b && c && d);
        if (a && a->nmembers == 3) {
            CHECK(a->members[0].value == 0 && !a->members[0].has_explicit);
            CHECK(a->members[1].value == 1);
            CHECK(a->members[2].value == 2);
            CHECK(a->size == 4 && a->align == 4);
        }
        if (b && b->nmembers == 4) {
            CHECK(b->members[0].value == 5 && b->members[0].has_explicit);
            CHECK(b->members[1].value == 6);
            CHECK(b->members[2].value == 10 && b->members[2].has_explicit);
            CHECK(b->members[3].value == 11);
        }
        if (c && c->nmembers == 3) {
            /* aliasing constants are permitted (sec. 7.5) */
            CHECK(c->members[0].value == 1);
            CHECK(c->members[1].value == 1);
            CHECK(c->members[2].value == 2);
        }
        if (d && d->nmembers == 2) {
            CHECK(d->members[0].value == -5);
            CHECK(d->members[1].value == -4);
        }
    }
    pipeline_free(&p);
}

static void test_enum_size_align(void)
{
    const char *src =
        "module main;\n"
        "enum A: u8 { X }\n"
        "enum B: i64 { X }\n"
        "enum C: u64 { X }\n"
        "enum D: isize { X }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutEnum *a = enum_for(&p, "A");
        const LayoutEnum *b = enum_for(&p, "B");
        const LayoutEnum *c = enum_for(&p, "C");
        const LayoutEnum *d = enum_for(&p, "D");
        CHECK(a && b && c && d);
        if (a) CHECK(a->size == 1 && a->align == 1 &&
                     a->underlying.size == 1 && a->underlying.align == 1);
        if (b) CHECK(b->size == 8 && b->align == 8);
        if (c) CHECK(c->size == 8 && c->align == 8);
        if (d) CHECK(d->size == 8 && d->align == 8);
    }
    pipeline_free(&p);
}

static void test_enum_representability_ok(void)
{
    /* Boundary values that DO fit: no records, LAYOUT_OK. */
    const char *src =
        "module main;\n"
        "enum A: i8 { M = -128, P = 127 }\n"
        "enum B: u8 { M = 255 }\n"
        "enum C: u64 { M = 0xFFFFFFFFFFFFFFFFu64 }\n"
        "enum D: i8 { M = -128i8, P = 127i8 }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_OK);
    CHECK(p.lrn == 0);
    {
        const LayoutEnum *c = enum_for(&p, "C");
        if (c && c->nmembers == 1) {
            CHECK(c->members[0].big_unsigned);
            CHECK((uint64_t)c->members[0].value == UINT64_MAX);
        }
    }
    pipeline_free(&p);
}

static void test_enum_t0301(void)
{
    /* Unrepresentable explicit values: AIC-T0301 with the corpus message
     * shape; primary span is the value expression. */
    const char *src =
        "module main;\n"
        "enum E: u8 { A = 256 }\n"
        "enum F: i8 { A = -129 }\n"
        "enum G: u8 { A = -1 }\n"
        "enum H: i64 { A = 0xFFFFFFFFFFFFFFFFu64 }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_DIAG_ERROR);
    CHECK(p.lrn == 4);
    if (p.lrn >= 1) {
        /* source line: "enum E: u8 { A = 256 }" -> literal 256 at
         * (2,18,30)-(2,21,33); the corpus fixture pins the same shape
         * with the longer name "Small" at (2,22,34)-(2,25,37) */
        CHECK(rec_matches(p.lrecs[0], "AIC-T0301",
                          "enum member value 256 is not representable in "
                          "underlying type u8",
                          2, 18, 30, 2, 21, 33, 0));
    }
    if (p.lrn >= 2) {
        CHECK(strcmp(p.lrecs[1]->code, "AIC-T0301") == 0);
        CHECK(strcmp(p.lrecs[1]->message,
                     "enum member value -129 is not representable in "
                     "underlying type i8") == 0);
    }
    if (p.lrn >= 3) {
        CHECK(strcmp(p.lrecs[2]->message,
                     "enum member value -1 is not representable in "
                     "underlying type u8") == 0);
    }
    if (p.lrn >= 4) {
        CHECK(strcmp(p.lrecs[3]->message,
                     "enum member value 18446744073709551615 is not "
                     "representable in underlying type i64") == 0);
    }
    pipeline_free(&p);
}

static void test_enum_continuation_overflow(void)
{
    /* i64 max then implicit continuation: B = 2^63, not representable in
     * i64 (nor in any signed type). The primary span is the member. */
    const char *src =
        "module main;\n"
        "enum E: i64 { A = 9223372036854775807, B }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.lst == LAYOUT_DIAG_ERROR);
    CHECK(p.lrn == 1);
    if (p.lrn >= 1) {
        const AstNode *b = NULL;
        const AstNode *ed = find_type_decl(p.program, "E", AST_ENUM_DECL);
        if (ed && ed->u.enum_decl.nmembers == 2) {
            b = ed->u.enum_decl.members[1];
        }
        CHECK(b != NULL);
        CHECK(strcmp(p.lrecs[0]->code, "AIC-T0301") == 0);
        CHECK(strcmp(p.lrecs[0]->message,
                     "enum member value 9223372036854775808 is not "
                     "representable in underlying type i64") == 0);
        if (b && p.lrecs[0]->primary_span) {
            /* primary span equals the member's own span (implicit) */
            CHECK(p.lrecs[0]->primary_span->start.offset ==
                  b->span->start.offset);
            CHECK(p.lrecs[0]->primary_span->end.offset ==
                  b->span->end.offset);
        }
    }
    pipeline_free(&p);
}

static void test_enum_forward_in_struct(void)
{
    /* An enum declared after the struct that uses it as a field resolves
     * (enums are complete immediately; layout phase 1 handles all
     * enums up front). */
    const char *src =
        "module main;\n"
        "struct S { e: E; }\n"
        "enum E: u8 { A }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.lst == LAYOUT_OK);
    {
        const LayoutStruct *s = layout_for(&p, "S");
        const LayoutEnum *e = enum_for(&p, "E");
        CHECK(s && e);
        if (s) CHECK(s->size == 1 && s->align == 1 && s->nfields == 1);
        if (e) CHECK(e->size == 1 && e->align == 1);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Bounded constant-integer evaluator (documented 11b subset)
 * ------------------------------------------------------------------------- */

static AstNode *lit_node(LexIntType type, uint64_t value, bool is_min)
{
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    n->kind = AST_EXPR_INT_LITERAL;
    n->u.int_literal.type = type;
    n->u.int_literal.value = value;
    n->u.int_literal.is_min = is_min;
    return n;
}

static AstNode *unary_node(AstUnaryOp op, AstNode *operand)
{
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    n->kind = AST_EXPR_UNARY;
    n->u.unary.op = op;
    n->u.unary.operand = operand;
    return n;
}

static AstNode *binary_node(AstBinaryOp op, AstNode *lhs, AstNode *rhs)
{
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    n->kind = AST_EXPR_BINARY;
    n->u.binary.op = op;
    n->u.binary.lhs = lhs;
    n->u.binary.rhs = rhs;
    return n;
}

static void test_eval_subset(void)
{
    LayoutEvalValue v;
    AstNode *n;

    n = lit_node(LEX_INT_I32, 42, false);
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.v == 42 && !v.big);
    free(n);

    /* -128i8 is the i8 minimum */
    n = unary_node(AST_UN_NEG, lit_node(LEX_INT_I8, 128, true));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.v == -128 && !v.big);
    free(n->u.unary.operand);
    free(n);

    /* 255u8 is a plain positive value; 0xFFFFFFFFFFFFFFFFu64 is big */
    n = lit_node(LEX_INT_U8, 255, false);
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.v == 255 && !v.big);
    free(n);
    n = lit_node(LEX_INT_U64, UINT64_MAX, false);
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.big && (uint64_t)v.v == UINT64_MAX);
    free(n);

    /* 1 + 2 * 3 == 7 (left-assoc parse is caller's job; here grouped) */
    n = binary_node(AST_BIN_ADD, lit_node(LEX_INT_I32, 1, false),
                    binary_node(AST_BIN_MUL, lit_node(LEX_INT_I32, 2, false),
                                lit_node(LEX_INT_I32, 3, false)));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.v == 7);
    free(n->u.binary.lhs);
    free(n->u.binary.rhs->u.binary.lhs);
    free(n->u.binary.rhs->u.binary.rhs);
    free(n->u.binary.rhs);
    free(n);

    /* -5 */
    n = unary_node(AST_UN_NEG, lit_node(LEX_INT_I32, 5, false));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.v == -5);
    free(n->u.unary.operand);
    free(n);

    /* 7 >> 1 == 3 (arithmetic shift) */
    n = binary_node(AST_BIN_SHR, lit_node(LEX_INT_I32, 7, false),
                    lit_node(LEX_INT_I32, 1, false));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OK);
    CHECK(v.v == 3);
    free(n->u.binary.lhs);
    free(n->u.binary.rhs);
    free(n);

    /* division by zero */
    n = binary_node(AST_BIN_DIV, lit_node(LEX_INT_I32, 1, false),
                    lit_node(LEX_INT_I32, 0, false));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_DIV_ZERO);
    free(n->u.binary.lhs);
    free(n->u.binary.rhs);
    free(n);

    /* shift count out of range */
    n = binary_node(AST_BIN_SHL, lit_node(LEX_INT_I32, 1, false),
                    lit_node(LEX_INT_I32, 64, false));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_SHIFT_RANGE);
    free(n->u.binary.lhs);
    free(n->u.binary.rhs);
    free(n);

    /* big-unsigned operand in arithmetic is outside the subset */
    n = binary_node(AST_BIN_ADD, lit_node(LEX_INT_U64, UINT64_MAX, false),
                    lit_node(LEX_INT_I32, 1, false));
    CHECK(layout_eval_int_expr(n, &v) == LAYOUT_EVAL_OVERFLOW);
    free(n->u.binary.lhs);
    free(n->u.binary.rhs);
    free(n);

    /* unknown form: identifier expressions are not evaluated */
    {
        AstNode *id = (AstNode *)calloc(1, sizeof(AstNode));
        id->kind = AST_EXPR_IDENT;
        id->u.ident.name = (char *)"X";
        CHECK(layout_eval_int_expr(id, &v) == LAYOUT_EVAL_UNEVALUABLE);
        free(id);
    }
}

/* ---------------------------------------------------------------------------
 * Unevaluable const forms (WP-M0-12 owns full composition)
 * ------------------------------------------------------------------------- */

static void test_unevaluable_member_and_extent(void)
{
    const char *src =
        "module main;\n"
        "const X: i32 = 5;\n"
        "enum E: i32 { A = X }\n"
        "struct S { a: u8[X]; }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.lst == LAYOUT_UNEVALUABLE);
    CHECK(p.lrn == 0);
    if (p.build) {
        /* partial results are not published for the affected decls */
        CHECK(layout_build_enum(p.build, find_sym(&p, "E")) == NULL);
        CHECK(layout_build_struct(p.build, find_sym(&p, "S")) == NULL);
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * Determinism
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    const char *src =
        "module main;\n"
        "enum E: u8 { A = 256 }\n"
        "enum F: i8 { A = -129 }\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline a, b;
    pipeline_run_mem(&a, src);
    pipeline_run_mem(&b, src);
    CHECK(a.lst == LAYOUT_DIAG_ERROR && b.lst == LAYOUT_DIAG_ERROR);
    CHECK(a.lrn == b.lrn);
    DiagBuf ea, eb;
    diag_buf_init(&ea);
    diag_buf_init(&eb);
    CHECK(diag_emit_records_sorted(&ea, a.lrecs, a.lrn));
    CHECK(diag_emit_records_sorted(&eb, b.lrecs, b.lrn));
    CHECK(diag_buf_ok(&ea) && diag_buf_ok(&eb));
    CHECK(ea.len == eb.len &&
          (ea.len == 0 || memcmp(ea.data, eb.data, ea.len) == 0));
    diag_buf_free(&ea);
    diag_buf_free(&eb);
    pipeline_free(&a);
    pipeline_free(&b);
}

/* ---------------------------------------------------------------------------
 * Corpus anchor: re-execute the enum-owned negative fixture
 * ------------------------------------------------------------------------- */

static void test_corpus_anchor(void)
{
    char root_path[512];
    char input_path[512];
    Pipeline p;
    snprintf(root_path, sizeof(root_path),
             "tests/negative/cases/derived-type-enum-value-overflow");
    snprintf(input_path, sizeof(input_path),
             "tests/negative/cases/derived-type-enum-value-overflow/input.ai");
    pipeline_run(&p, root_path, "main", input_path);
    CHECK(p.st == NAME_OK);
    CHECK(p.tst == TYPE_CHECK_OK);
    CHECK(p.lst == LAYOUT_DIAG_ERROR);
    CHECK(p.lrn == 1);
    if (p.lrn >= 1) {
        if (!rec_matches(p.lrecs[0], "AIC-T0301",
                         "enum member value 256 is not representable in "
                         "underlying type u8",
                         2, 22, 34, 2, 25, 37, 0)) {
            fprintf(stderr, "  [enum-value-overflow] record mismatch: "
                    "code=%s msg=%s span=(%lld,%lld,%lld)-(%lld,%lld,%lld) "
                    "sec=%zu\n",
                    p.lrecs[0]->code, p.lrecs[0]->message,
                    (long long)p.lrecs[0]->primary_span->start.line,
                    (long long)p.lrecs[0]->primary_span->start.col,
                    (long long)p.lrecs[0]->primary_span->start.offset,
                    (long long)p.lrecs[0]->primary_span->end.line,
                    (long long)p.lrecs[0]->primary_span->end.col,
                    (long long)p.lrecs[0]->primary_span->end.offset,
                    p.lrecs[0]->secondary_count);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_type_info_sizes();
    test_struct_basic();
    test_struct_alignment_padding();
    test_struct_str_ptr_slice();
    test_struct_embedded();
    test_struct_array_fields();
    test_struct_padding_ranges();
    test_enum_values();
    test_enum_size_align();
    test_enum_representability_ok();
    test_enum_t0301();
    test_enum_continuation_overflow();
    test_enum_forward_in_struct();
    test_eval_subset();
    test_unevaluable_member_and_extent();
    test_determinism();
    test_corpus_anchor();

    layout_build_free(NULL);
    types_records_free(NULL, 0);

    if (g_failures == 0) {
        printf("layout_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    fprintf(stderr, "layout_test: %d checks, %d FAILURES\n",
            g_checks, g_failures);
    return 1;
}
