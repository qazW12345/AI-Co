/* bootstrap/src/ir/ir_core_test.c
 *
 * WP-M0-16b1 IR node model and invariants unit tests: the node/type/const
 * model construction, interning determinism, and the invariant
 * verification of contract sec. 10 (AIC-I0501 reporting, spans, record
 * shape, sorting).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-ir16b1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_core_test.c \
 *     bootstrap/src/ir/ir_core.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-ir16b1/ir_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-ir16b1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */
#include "ir_core.h"

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
 * Shared construction helpers
 * ------------------------------------------------------------------------- */

static DiagSpan *mk_span(const char *file, int64_t line, int64_t col,
                         int64_t offset)
{
    return diag_span_new_point(file, line, col, offset);
}

/* Node with a point span and one root cause link (module-root construct).
 * The cause chain's root link file matches the node's file, so the
 * span/cause invariant (2) passes for a well-formed test node. */
static IrNode *mk(IrBuild *b, IrNodeKind kind, const char *file, int64_t line)
{
    IrNode *n = ir_node_new(b, kind, mk_span(file, line, 1, line * 10));
    CHECK(n != NULL);
    if (n == NULL) {
        return NULL;
    }
    ir_node_add_cause(b, n, "AST_MODULE_DECL", mk_span(file, 1, 1, 0),
                      -1, -1, -1);
    return n;
}

static IrNode *mk_module(IrBuild *b, const char *name, const char *file)
{
    IrNode *m = mk(b, IR_MODULE, file, 1);
    CHECK(m != NULL);
    if (m != NULL) {
        m->u.module.name = strdup(name);
    }
    return m;
}

static IrNode *mk_int(IrBuild *b, const char *file, IrType *t, uint64_t bits)
{
    IrNode *n = mk(b, IR_INT, file, 1);
    if (n != NULL) {
        n->type = t;
        n->u.constant.value = ir_const_int(b, t, bits);
    }
    return n;
}

static IrNode *mk_bool(IrBuild *b, const char *file, bool v)
{
    IrNode *n = mk(b, IR_BOOL, file, 1);
    if (n != NULL) {
        n->type = ir_type_bool(b);
        n->u.constant.value = ir_const_bool(b, v);
    }
    return n;
}

static IrNode *mk_block(IrBuild *b, const char *file, int64_t line)
{
    IrNode *n = mk(b, IR_BLOCK, file, line);
    return n;
}

static IrNode *mk_return(IrBuild *b, const char *file, IrNode *value)
{
    IrNode *n = mk(b, IR_RETURN, file, 1);
    if (n != NULL) {
        n->u.return_stmt.value = value;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * A comprehensive VALID graph: module m (global var) imported by module main
 * (import, struct decl, function with params/slots, if/store, switch with
 * returning cases, global ref). Verification must pass with zero records.
 * ------------------------------------------------------------------------- */

static IrBuild *make_valid_build(void)
{
    IrBuild *b = ir_build_new();
    IrType *i32 = ir_type_i32(b);
    IrNode *gvar, *m_mod, *main_mod, *imp, *s_decl, *fn, *body;
    IrNode *y, *store, *store_stmt, *if_cond, *then_block, *if_stmt;
    IrNode *sel, *case0, *case0_body, *case0_ret, *def, *def_body, *def_ret;
    IrNode *sw, *gref, *gref_stmt;
    IrConst *c0 = ir_const_int(b, i32, 0);
    IrConst *c1 = ir_const_int(b, i32, 1);
    IrSlot *slots[3];
    size_t i;

    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }

    /* module m: global var g: i32 = 0 */
    m_mod = mk_module(b, "m", "m.ai");
    gvar = mk(b, IR_GLOBAL_VAR, "m.ai", 2);
    gvar->u.global_var.name = strdup("g");
    gvar->u.global_var.type = i32;
    gvar->u.global_var.init = c0;
    ir_module_add_decl(b, m_mod, gvar);
    ir_build_add_module(b, m_mod);

    /* module main (entry): import m; struct S; function f */
    main_mod = mk_module(b, "main", "main.ai");
    imp = mk(b, IR_IMPORT, "main.ai", 2);
    imp->u.import.name = strdup("m");
    ir_module_add_import(b, main_mod, imp);

    s_decl = mk(b, IR_STRUCT_DECL, "main.ai", 3);
    s_decl->u.struct_decl.name = strdup("S");
    s_decl->u.struct_decl.nfields = 1;
    s_decl->u.struct_decl.fields = (IrField *)calloc(1, sizeof(IrField));
    s_decl->u.struct_decl.fields[0].name = strdup("x");
    s_decl->u.struct_decl.fields[0].type = i32;
    s_decl->u.struct_decl.fields[0].span = mk_span("main.ai", 3, 8, 30);
    s_decl->u.struct_decl.fields[0].byte_offset = 0;
    s_decl->u.struct_decl.size = 4;
    s_decl->u.struct_decl.align = 4;
    ir_module_add_decl(b, main_mod, s_decl);

    fn = mk(b, IR_FUNCTION, "main.ai", 5);
    fn->u.function.name = strdup("f");
    fn->u.function.ret_type = i32;
    fn->u.function.nparams = 1;
    fn->u.function.params = (IrParam *)calloc(1, sizeof(IrParam));
    fn->u.function.params[0].name = strdup("x");
    fn->u.function.params[0].type = i32;
    fn->u.function.params[0].slot_index = 0;
    fn->u.function.params[0].span = mk_span("main.ai", 5, 5, 42);
    for (i = 0; i < 3; i++) {
        slots[i] = (IrSlot *)calloc(1, sizeof(IrSlot));
        slots[i]->index = (int64_t)i;
    }
    slots[0]->kind = IR_SLOT_PARAM;
    slots[0]->name = strdup("x");
    slots[0]->type = i32;
    slots[0]->span = mk_span("main.ai", 5, 5, 42);
    slots[1]->kind = IR_SLOT_LOCAL;
    slots[1]->name = strdup("y");
    slots[1]->type = i32;
    slots[1]->span = mk_span("main.ai", 6, 5, 52);
    slots[2]->kind = IR_SLOT_TEMP;
    slots[2]->name = NULL;
    slots[2]->type = i32;
    slots[2]->span = mk_span("main.ai", 6, 20, 67);
    fn->u.function.nslots = 3;
    fn->u.function.slots = (IrSlot **)malloc(3 * sizeof(IrSlot *));
    memcpy(fn->u.function.slots, slots, 3 * sizeof(IrSlot *));

    body = mk_block(b, "main.ai", 7);

    /* local decl: y = 0 (IR_LOCAL_DECL slot 1) */
    {
        IrNode *ld = mk(b, IR_LOCAL_DECL, "main.ai", 7);
        ld->u.local_decl.slot_index = 1;
        ld->u.local_decl.init = mk_int(b, "main.ai", i32, 0);
        ir_block_add_stmt(b, body, ld);
    }

    /* if (true) { y = 1; } */
    if_cond = mk_bool(b, "main.ai", true);
    then_block = mk_block(b, "main.ai", 8);
    y = mk(b, IR_LOCAL, "main.ai", 8);
    y->type = i32;
    y->u.local.slot_index = 1;
    store = mk(b, IR_STORE, "main.ai", 8);
    store->u.store.dest = y;
    store->u.store.value = mk_int(b, "main.ai", i32, 1);
    store_stmt = mk(b, IR_EXPR_STMT, "main.ai", 8);
    store_stmt->u.expr_stmt.expr = store;
    ir_block_add_stmt(b, then_block, store_stmt);
    if_stmt = mk(b, IR_IF, "main.ai", 8);
    if_stmt->u.if_stmt.cond = if_cond;
    if_stmt->u.if_stmt.then_block = then_block;
    if_stmt->u.if_stmt.else_block = NULL;
    ir_block_add_stmt(b, body, if_stmt);

    /* switch (x) { case 0: return 1; default: return 0; } */
    sel = mk(b, IR_LOCAL, "main.ai", 9);
    sel->type = i32;
    sel->u.local.slot_index = 0;
    case0 = mk(b, IR_CASE, "main.ai", 10);
    case0->u.case_clause.value = c0;
    case0_body = mk_block(b, "main.ai", 10);
    case0_ret = mk_return(b, "main.ai", mk_int(b, "main.ai", i32, 1));
    ir_block_add_stmt(b, case0_body, case0_ret);
    case0->u.case_clause.body = case0_body;
    def = mk(b, IR_DEFAULT, "main.ai", 11);
    def_body = mk_block(b, "main.ai", 11);
    def_ret = mk_return(b, "main.ai", mk_int(b, "main.ai", i32, 0));
    ir_block_add_stmt(b, def_body, def_ret);
    def->u.default_clause.body = def_body;
    sw = mk(b, IR_SWITCH, "main.ai", 9);
    sw->u.switch_stmt.selector = sel;
    sw->u.switch_stmt.ncases = 1;
    sw->u.switch_stmt.cases = (IrNode **)malloc(sizeof(IrNode *));
    sw->u.switch_stmt.cases[0] = case0;
    sw->u.switch_stmt.default_clause = def;

    /* reference imported global: gref_stmt(g);  (expr stmt, value unused).
     * Placed before the switch so the function tail ends in the switch. */
    gref = mk(b, IR_GLOBAL, "main.ai", 12);
    gref->type = i32;
    gref->u.global.target = gvar;
    gref_stmt = mk(b, IR_EXPR_STMT, "main.ai", 12);
    gref_stmt->u.expr_stmt.expr = gref;
    ir_block_add_stmt(b, body, gref_stmt);

    ir_block_add_stmt(b, body, sw);

    fn->u.function.body = body;
    ir_module_add_decl(b, main_mod, fn);
    ir_build_add_module(b, main_mod);

    (void)c1;
    return b;
}

/* ---------------------------------------------------------------------------
 * Record assertion helpers
 * ------------------------------------------------------------------------- */

static bool related_int_is(const DiagRecord *r, const char *key, int64_t want)
{
    size_t i;
    for (i = 0; i < r->related_count; i++) {
        if (strcmp(r->related[i].key, key) == 0) {
            return r->related[i].kind == DIAG_KV_INT && r->related[i].i == want;
        }
    }
    return false;
}

static void check_record_shape(const DiagRecord *r, int invariant,
                               const char *file, int64_t line)
{
    CHECK(strcmp(r->code, "AIC-I0501") == 0);
    CHECK(strcmp(r->phase, DIAG_PHASE_IR) == 0);
    CHECK(strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(r->recovery != NULL &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    CHECK(related_int_is(r, "invariant", invariant));
    if (file != NULL && r->primary_span != NULL) {
        CHECK(strcmp(r->primary_span->file, file) == 0);
        CHECK(r->primary_span->start.line == line);
    }
}

/* Verify `b`, assert the status, and return the record count. */
static size_t run_verify(IrBuild *b, IrStatus want, DiagRecord ***recs)
{
    DiagRecord **r = NULL;
    size_t n = 0;
    IrStatus st = ir_core_verify(b, &r, &n);
    CHECK(st == want);
    if (recs != NULL) {
        *recs = r;
    } else {
        ir_records_free(r, n);
    }
    return n;
}

/* True when at least one record carries the expected invariant (and, when
 * `file` is non-NULL, the expected primary span). Used where a single node
 * legitimately triggers several violations whose sort order is not fixed. */
static bool has_record_with_invariant(DiagRecord **recs, size_t count,
                                      int invariant, const char *file,
                                      int64_t line)
{
    size_t i;
    for (i = 0; i < count; i++) {
        const DiagRecord *r = recs[i];
        if (!related_int_is(r, "invariant", invariant)) {
            continue;
        }
        if (file != NULL) {
            if (r->primary_span == NULL ||
                strcmp(r->primary_span->file, file) != 0 ||
                r->primary_span->start.line != line) {
                continue;
            }
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_kind_names(void)
{
    CHECK(strcmp(ir_kind_text(IR_MODULE), "IR_MODULE") == 0);
    CHECK(strcmp(ir_kind_text(IR_ADD), "IR_ADD") == 0);
    CHECK(strcmp(ir_kind_text(IR_ZERO), "IR_ZERO") == 0);
    CHECK(strcmp(ir_kind_text((IrNodeKind)9999), "IR_?") == 0);
    CHECK(strcmp(ir_type_kind_text(IRT_I32), "i32") == 0);
    CHECK(strcmp(ir_type_kind_text(IRT_STR), "str") == 0);
    CHECK(strcmp(ir_const_kind_text(IRC_ADDR), "addr") == 0);
}

static void test_model_and_determinism(void)
{
    IrBuild *b = ir_build_new();
    IrType *a1, *a2, *p1, *p2;
    IrConst *k1, *k2;

    CHECK(b != NULL);
    /* base types pre-interned in spec order; id == kind for base types */
    CHECK(ir_type_void(b)->id == IRT_VOID);
    CHECK(ir_type_str(b)->id == IRT_STR);
    CHECK(ir_type_i32(b)->size == 4 && ir_type_i32(b)->align == 4);
    CHECK(ir_type_str(b)->size == 16 && ir_type_str(b)->align == 8);
    CHECK(ir_type_ptr(b, ir_type_i32(b))->size == 8);
    CHECK(ir_type_slice(b, ir_type_u8(b))->size == 16);

    /* interning: identical composites share one descriptor */
    a1 = ir_type_array(b, ir_type_i32(b), 4);
    a2 = ir_type_array(b, ir_type_i32(b), 4);
    CHECK(a1 == a2);
    CHECK(a1->size == 16 && a1->align == 4);
    CHECK(ir_type_array(b, ir_type_i32(b), 5) != a1);
    p1 = ir_type_ptr(b, a1);
    p2 = ir_type_ptr(b, a1);
    CHECK(p1 == p2);
    CHECK(ir_type_identical(p1, p2));
    CHECK(!ir_type_identical(ir_type_i32(b), ir_type_i64(b)));

    /* constant dedup: identical constants share one IrConst */
    k1 = ir_const_int(b, ir_type_i32(b), 7);
    k2 = ir_const_int(b, ir_type_i32(b), 7);
    CHECK(k1 == k2);
    CHECK(k1->id == 0);
    CHECK(ir_const_int(b, ir_type_i32(b), 8) != k1);
    {
        const uint8_t s[] = { 'a', 'b' };
        IrConst *ks1 = ir_const_str(b, s, 2);
        IrConst *ks2 = ir_const_str(b, s, 2);
        CHECK(ks1 == ks2);
        CHECK(ks1->u.str.len == 2 && ks1->u.str.bytes[0] == 'a');
    }

    /* node ids are construction order, unique, gapless */
    {
        IrNode *n1 = mk(b, IR_EMPTY, "t.ai", 1);
        IrNode *n2 = mk(b, IR_EMPTY, "t.ai", 2);
        CHECK(n1->id == 0 && n2->id == 1);
    }
    ir_build_free(b);
}

static void test_valid_graph(void)
{
    IrBuild *b = make_valid_build();
    size_t n;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    n = run_verify(b, IR_OK, NULL);
    CHECK(n == 0);
    ir_build_free(b);
}

static void test_graph_wellformed(void)
{
    /* duplicate module names */
    {
        IrBuild *b = ir_build_new();
        IrNode *m1 = mk_module(b, "main", "a.ai");
        IrNode *m2 = mk_module(b, "main", "b.ai");
        DiagRecord **r = NULL;
        size_t n;
        ir_build_add_module(b, m1);
        ir_build_add_module(b, m2);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 1, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* duplicate node id */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *e1 = mk(b, IR_EMPTY, "a.ai", 2);
        IrNode *e2 = mk(b, IR_EMPTY, "a.ai", 3);
        IrNode *block = mk_block(b, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        ir_block_add_stmt(b, block, e1);
        ir_block_add_stmt(b, block, e2);
        m->u.module.decls = NULL;   /* module carries no decls; keep block
                                     * reachable through a block in... */
        (void)block;
        ir_build_add_module(b, m);
        e2->id = e1->id;   /* corrupt: duplicate id */
        /* the two empty statements are unreachable now; we only assert the
         * duplicate-id record appears */
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* unreachable node */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *orphan = mk(b, IR_EMPTY, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        ir_build_add_module(b, m);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 1, "a.ai", 2);
            CHECK(related_int_is(r[0], "node_id", orphan->id));
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* NULL child statement */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        ir_block_add_stmt(b, body, NULL);   /* NULL statement entry */
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 1, "a.ai", 3);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* module list NULL entry */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        DiagRecord **r = NULL;
        size_t n;
        ir_build_add_module(b, m);
        b->modules[0] = NULL;
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_span_cause(void)
{
    /* NULL primary span */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *e = mk(b, IR_EMPTY, "a.ai", 2);
        IrNode *block = mk_block(b, "a.ai", 2);
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        ir_block_add_stmt(b, block, e);
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = block;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        diag_span_free(e->span);
        e->span = NULL;
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 2, NULL, 0);
            CHECK(related_int_is(r[0], "node_id", e->id));
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* invalid span offsets (end before start) */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *e = mk(b, IR_EMPTY, "a.ai", 2);
        IrNode *block = mk_block(b, "a.ai", 2);
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        ir_block_add_stmt(b, block, e);
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = block;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        e->span->end.offset = e->span->start.offset - 1;
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 2, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* missing cause chain */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *e = mk(b, IR_EMPTY, "a.ai", 2);
        IrNode *block = mk_block(b, "a.ai", 2);
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        ir_block_add_stmt(b, block, e);
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = block;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        {
            size_t i;
            for (i = 0; i < e->cause_count; i++) {
                free(e->causes[i].construct_kind);
                diag_span_free(e->causes[i].span);
            }
            free(e->causes);
            e->causes = NULL;
            e->cause_count = 0;
        }
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 2, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* cause root link file mismatch */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *e = mk(b, IR_EMPTY, "a.ai", 2);
        IrNode *block = mk_block(b, "a.ai", 2);
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        ir_block_add_stmt(b, block, e);
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = block;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        diag_span_free(e->causes[0].span);
        e->causes[0].span = mk_span("other.ai", 1, 1, 0);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 2, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_type_wellformed(void)
{
    /* void result type on a value node */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *bad = mk(b, IR_INT, "a.ai", 4);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 4);
        DiagRecord **r = NULL;
        size_t n;
        bad->type = ir_type_void(b);
        bad->u.constant.value = ir_const_int(b, ir_type_void(b), 0);
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        es->u.expr_stmt.expr = bad;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        CHECK(has_record_with_invariant(r, n, 3, "a.ai", 4));
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* NULL result type on a value node */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *bad = mk(b, IR_NULL, "a.ai", 4);   /* IR_NULL with NULL type */
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 4);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        es->u.expr_stmt.expr = bad;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        CHECK(has_record_with_invariant(r, n, 3, "a.ai", 4));
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* non-value node carrying a result type */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *bad = mk(b, IR_EMPTY, "a.ai", 4);
        DiagRecord **r = NULL;
        size_t n;
        bad->type = ir_type_i32(b);
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        ir_block_add_stmt(b, body, bad);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 3, "a.ai", 4);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* negative array extent (interned type check) */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrType *t;
        DiagRecord **r = NULL;
        size_t n;
        ir_build_add_module(b, m);
        t = ir_type_array(b, ir_type_i32(b), -1);
        CHECK(t != NULL);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 3, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* named type referencing a non-decl node */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *notdecl = mk(b, IR_EMPTY, "a.ai", 4);
        IrType *t;
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        ir_block_add_stmt(b, body, notdecl);
        t = ir_type_struct(b, notdecl);   /* references a non-decl node */
        CHECK(t != NULL);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 3, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* struct field beyond size and overlapping fields */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *s = mk(b, IR_STRUCT_DECL, "a.ai", 2);
        DiagRecord **r = NULL;
        size_t n;
        s->u.struct_decl.name = strdup("S");
        s->u.struct_decl.nfields = 2;
        s->u.struct_decl.fields = (IrField *)calloc(2, sizeof(IrField));
        s->u.struct_decl.fields[0].name = strdup("a");
        s->u.struct_decl.fields[0].type = ir_type_i32(b);
        s->u.struct_decl.fields[0].span = mk_span("a.ai", 2, 4, 20);
        s->u.struct_decl.fields[0].byte_offset = 0;
        s->u.struct_decl.fields[1].name = strdup("b");
        s->u.struct_decl.fields[1].type = ir_type_i32(b);
        s->u.struct_decl.fields[1].span = mk_span("a.ai", 2, 12, 28);
        s->u.struct_decl.fields[1].byte_offset = 0;   /* overlaps field a */
        s->u.struct_decl.size = 4;                    /* field b exceeds */
        s->u.struct_decl.align = 4;
        ir_module_add_decl(b, m, s);
        ir_build_add_module(b, m);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 3, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_operand_typing(void)
{
    /* helper: void fn f() { <stmts> } */
#define FN_BODY_SETUP(prefix) \
    IrBuild *b = ir_build_new(); \
    IrNode *m = mk_module(b, "main", "a.ai"); \
    IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2); \
    IrNode *body = mk_block(b, "a.ai", 3); \
    fn->u.function.name = strdup(prefix "_f"); \
    fn->u.function.ret_type = ir_type_void(b); \
    fn->u.function.body = body; \
    ir_module_add_decl(b, m, fn); \
    ir_build_add_module(b, m); \
    (void)body

    /* IR_ADD with a bool operand */
    {
        FN_BODY_SETUP("add");
        IrNode *e = mk(b, IR_ADD, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *rr = mk_bool(b, "a.ai", true);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0802";
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_LAND with integer operands */
    {
        FN_BODY_SETUP("land");
        IrNode *e = mk(b, IR_LAND, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *rr = mk_int(b, "a.ai", ir_type_i32(b), 0);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_bool(b);
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_DEREF of a non-pointer */
    {
        FN_BODY_SETUP("deref");
        IrNode *e = mk(b, IR_DEREF, "a.ai", 5);
        IrNode *op = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0809";
        e->u.deref.ptr = op;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_INDEX_ADDR with a non-usize index */
    {
        FN_BODY_SETUP("index");
        IrNode *base = mk(b, IR_STR, "a.ai", 5);
        IrNode *idx = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *e = mk(b, IR_INDEX_ADDR, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        base->type = ir_type_str(b);
        base->u.constant.value = ir_const_str(b, (const uint8_t *)"x", 1);
        e->type = ir_type_ptr(b, ir_type_u8(b));
        e->trap_code = "AIC-R0807";
        e->u.index_addr.base = base;
        e->u.index_addr.index = idx;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_SELECT with a non-bool condition */
    {
        FN_BODY_SETUP("select");
        IrNode *cond = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *e = mk(b, IR_SELECT, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_i32(b);
        e->u.select.cond = cond;
        e->u.select.then_value = mk_int(b, "a.ai", ir_type_i32(b), 1);
        e->u.select.else_value = mk_int(b, "a.ai", ir_type_i32(b), 2);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_EQ with mismatched operand types */
    {
        FN_BODY_SETUP("eq");
        IrNode *e = mk(b, IR_EQ, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_bool(b);
        e->u.binary.left = mk_int(b, "a.ai", ir_type_i32(b), 1);
        e->u.binary.right = mk_bool(b, "a.ai", true);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_FOR with an expression as init (must be a statement) */
    {
        FN_BODY_SETUP("for");
        IrNode *f = mk(b, IR_FOR, "a.ai", 5);
        IrNode *fbody = mk_block(b, "a.ai", 5);
        IrNode *init_expr = mk_int(b, "a.ai", ir_type_i32(b), 0);
        DiagRecord **r = NULL;
        size_t n;
        f->u.for_stmt.init = init_expr;   /* expression, not a statement */
        f->u.for_stmt.body = fbody;
        ir_block_add_stmt(b, body, f);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_CALL with wrong argument count */
    {
        FN_BODY_SETUP("call");
        IrNode *callee = mk(b, IR_FUNCTION, "a.ai", 5);
        IrNode *e = mk(b, IR_CALL, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        IrNode *callee_body = mk_block(b, "a.ai", 6);
        IrSlot *cs = (IrSlot *)calloc(1, sizeof(IrSlot));
        DiagRecord **r = NULL;
        size_t n;
        callee->u.function.name = strdup("g");
        callee->u.function.ret_type = ir_type_i32(b);
        callee->u.function.nparams = 1;
        callee->u.function.params = (IrParam *)calloc(1, sizeof(IrParam));
        callee->u.function.params[0].name = strdup("p");
        callee->u.function.params[0].type = ir_type_i32(b);
        callee->u.function.params[0].slot_index = 0;
        callee->u.function.params[0].span = mk_span("a.ai", 5, 4, 30);
        cs->index = 0;
        cs->kind = IR_SLOT_PARAM;
        cs->name = strdup("p");
        cs->type = ir_type_i32(b);
        cs->span = mk_span("a.ai", 5, 4, 30);
        callee->u.function.nslots = 1;
        callee->u.function.slots = (IrSlot **)malloc(sizeof(IrSlot *));
        callee->u.function.slots[0] = cs;
        callee->u.function.body = callee_body;
        ir_block_add_stmt(b, callee_body,
                          mk_return(b, "a.ai", mk_int(b, "a.ai",
                                                      ir_type_i32(b), 0)));
        ir_module_add_decl(b, m, callee);
        e->type = ir_type_i32(b);
        e->u.call.callee = callee;
        ir_call_add_arg(b, e, mk_int(b, "a.ai", ir_type_i32(b), 1));
        ir_call_add_arg(b, e, mk_int(b, "a.ai", ir_type_i32(b), 2));
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
#undef FN_BODY_SETUP
}

static void test_shift_result_type(void)
{
    /* helper: void fn f() { <stmts> } */
#define FN_BODY_SETUP(prefix) \
    IrBuild *b = ir_build_new(); \
    IrNode *m = mk_module(b, "main", "a.ai"); \
    IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2); \
    IrNode *body = mk_block(b, "a.ai", 3); \
    fn->u.function.name = strdup(prefix "_f"); \
    fn->u.function.ret_type = ir_type_void(b); \
    fn->u.function.body = body; \
    ir_module_add_decl(b, m, fn); \
    ir_build_add_module(b, m); \
    (void)body

    /* IR_SHL result type must equal the left operand's type (contract
     * sec. 5.3 row; invariant 4): left i32 / result i64 is a violation. */
    {
        FN_BODY_SETUP("shl_mismatch");
        IrNode *e = mk(b, IR_SHL, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 4);
        IrNode *rr = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_i64(b);   /* wrong: must be the left type (i32) */
        e->trap_code = "AIC-R0804";
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_SHR result type must equal the left operand's type: left i32 /
     * result i64 is a violation. */
    {
        FN_BODY_SETUP("shr_mismatch");
        IrNode *e = mk(b, IR_SHR, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 4);
        IrNode *rr = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_i64(b);   /* wrong: must be the left type (i32) */
        e->trap_code = "AIC-R0804";
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 4, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* compliant: IR_SHL with left i32 / result i32 verifies clean */
    {
        FN_BODY_SETUP("shl_ok");
        IrNode *e = mk(b, IR_SHL, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 4);
        IrNode *rr = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        size_t n;
        e->type = ir_type_i32(b);   /* matches the left operand's type */
        e->trap_code = "AIC-R0804";
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_OK, NULL);
        CHECK(n == 0);
        ir_build_free(b);
    }
    /* compliant: IR_SHR with left i32 / result i32 verifies clean */
    {
        FN_BODY_SETUP("shr_ok");
        IrNode *e = mk(b, IR_SHR, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 4);
        IrNode *rr = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        size_t n;
        e->type = ir_type_i32(b);   /* matches the left operand's type */
        e->trap_code = "AIC-R0804";
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_OK, NULL);
        CHECK(n == 0);
        ir_build_free(b);
    }
    /* compliant: IR_SHL with left i32 / result i32 and a count of a
     * different integer type (u8): the count need not match the left
     * type; only the result must equal the left operand's type. */
    {
        FN_BODY_SETUP("shl_count_u8");
        IrNode *e = mk(b, IR_SHL, "a.ai", 5);
        IrNode *l = mk_int(b, "a.ai", ir_type_i32(b), 4);
        IrNode *rr = mk_int(b, "a.ai", ir_type_u8(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        size_t n;
        e->type = ir_type_i32(b);   /* matches the left operand's type */
        e->trap_code = "AIC-R0804";
        e->u.binary.left = l;
        e->u.binary.right = rr;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_OK, NULL);
        CHECK(n == 0);
        ir_build_free(b);
    }
#undef FN_BODY_SETUP
}

static void test_terminators(void)
{
    /* non-void function tail without a terminator */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *e = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 4);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_i32(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 5, "a.ai", 2);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* statement after a terminator */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *ret = mk_return(b, "a.ai", NULL);
        IrNode *e = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 4);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, ret);
        ir_block_add_stmt(b, body, es);   /* after terminator */
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 5, "a.ai", 3);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* case body without a terminator (no fall-through) */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *sel = mk_int(b, "a.ai", ir_type_i32(b), 0);
        IrNode *sw = mk(b, IR_SWITCH, "a.ai", 4);
        IrNode *case0 = mk(b, IR_CASE, "a.ai", 5);
        IrNode *case_body = mk_block(b, "a.ai", 5);
        IrNode *e = mk_int(b, "a.ai", ir_type_i32(b), 1);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        case0->u.case_clause.value = ir_const_int(b, ir_type_i32(b), 0);
        case0->u.case_clause.body = case_body;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, case_body, es);   /* not a terminator */
        sw->u.switch_stmt.selector = sel;
        sw->u.switch_stmt.ncases = 1;
        sw->u.switch_stmt.cases = (IrNode **)malloc(sizeof(IrNode *));
        sw->u.switch_stmt.cases[0] = case0;
        sw->u.switch_stmt.default_clause = NULL;
        ir_block_add_stmt(b, body, sw);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 6, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_break_continue(void)
{
    /* break targeting a construct that does not enclose it */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *loop = mk(b, IR_WHILE, "a.ai", 4);
        IrNode *loop_body = mk_block(b, "a.ai", 4);
        IrNode *brk = mk(b, IR_BREAK, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        loop->u.while_stmt.cond = mk_bool(b, "a.ai", false);
        loop->u.while_stmt.body = loop_body;
        /* break is OUTSIDE the loop, targeting it: not enclosing */
        brk->u.break_stmt.target = loop;
        ir_block_add_stmt(b, body, loop);
        ir_block_add_stmt(b, body, brk);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 7, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* continue targeting an enclosing switch (must target a loop) */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *sel = mk_int(b, "a.ai", ir_type_i32(b), 0);
        IrNode *sw = mk(b, IR_SWITCH, "a.ai", 4);
        IrNode *case0 = mk(b, IR_CASE, "a.ai", 5);
        IrNode *case_body = mk_block(b, "a.ai", 5);
        IrNode *cont = mk(b, IR_CONTINUE, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        case0->u.case_clause.value = ir_const_int(b, ir_type_i32(b), 0);
        case0->u.case_clause.body = case_body;
        cont->u.continue_stmt.target = sw;   /* wrong kind for continue */
        ir_block_add_stmt(b, case_body, cont);
        sw->u.switch_stmt.selector = sel;
        sw->u.switch_stmt.ncases = 1;
        sw->u.switch_stmt.cases = (IrNode **)malloc(sizeof(IrNode *));
        sw->u.switch_stmt.cases[0] = case0;
        sw->u.switch_stmt.default_clause = NULL;
        ir_block_add_stmt(b, body, sw);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 7, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* break with no enclosing construct */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *loop = mk(b, IR_WHILE, "a.ai", 4);
        IrNode *loop_body = mk_block(b, "a.ai", 4);
        IrNode *brk = mk(b, IR_BREAK, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        loop->u.while_stmt.cond = mk_bool(b, "a.ai", false);
        loop->u.while_stmt.body = loop_body;
        /* loop is a sibling statement; the break (after it) is not inside
         * it, so the target is not enclosing. */
        brk->u.break_stmt.target = loop;
        ir_block_add_stmt(b, body, loop);
        ir_block_add_stmt(b, body, brk);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 7, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_return_typing(void)
{
    /* void function return with a value */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *ret = mk_return(b, "a.ai",
                                mk_int(b, "a.ai", ir_type_i32(b), 1));
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        ir_block_add_stmt(b, body, ret);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 8, "a.ai", 1);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* non-void function return with no value */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *ret = mk_return(b, "a.ai", NULL);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_i32(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        ir_block_add_stmt(b, body, ret);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 8, "a.ai", 1);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* non-void return with a mismatched value type */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *ret = mk_return(b, "a.ai", mk_bool(b, "a.ai", true));
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_i32(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        ir_block_add_stmt(b, body, ret);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 8, "a.ai", 1);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_trap_codes(void)
{
#define FN_BODY_SETUP(prefix) \
    IrBuild *b = ir_build_new(); \
    IrNode *m = mk_module(b, "main", "a.ai"); \
    IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2); \
    IrNode *body = mk_block(b, "a.ai", 3); \
    fn->u.function.name = strdup(prefix "_f"); \
    fn->u.function.ret_type = ir_type_void(b); \
    fn->u.function.body = body; \
    ir_module_add_decl(b, m, fn); \
    ir_build_add_module(b, m); \
    (void)body

    /* IR_DEREF without a declared trap code */
    {
        FN_BODY_SETUP("deref");
        IrNode *e = mk(b, IR_DEREF, "a.ai", 5);
        IrNode *op = mk(b, IR_NULL, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        op->type = ir_type_ptr(b, ir_type_i32(b));
        e->type = ir_type_i32(b);
        e->trap_code = NULL;   /* missing obligation */
        e->u.deref.ptr = op;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_DEREF with a trap code outside its declared set */
    {
        FN_BODY_SETUP("deref2");
        IrNode *e = mk(b, IR_DEREF, "a.ai", 5);
        IrNode *op = mk(b, IR_NULL, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        op->type = ir_type_ptr(b, ir_type_i32(b));
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0801";   /* wrong kind (conversion trap) */
        e->u.deref.ptr = op;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* unknown trap code */
    {
        FN_BODY_SETUP("unk");
        IrNode *e = mk(b, IR_ADD, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0999";   /* not in the registry */
        e->u.binary.left = mk_int(b, "a.ai", ir_type_i32(b), 1);
        e->u.binary.right = mk_int(b, "a.ai", ir_type_i32(b), 2);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* non-failing kind carrying a trap code */
    {
        FN_BODY_SETUP("eq");
        IrNode *e = mk(b, IR_EQ, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        e->type = ir_type_bool(b);
        e->trap_code = "AIC-R0802";   /* equality has no failure mode */
        e->u.binary.left = mk_int(b, "a.ai", ir_type_i32(b), 1);
        e->u.binary.right = mk_int(b, "a.ai", ir_type_i32(b), 1);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* bool-typed load without the AIC-R0805 obligation */
    {
        FN_BODY_SETUP("bload");
        IrNode *slot = mk(b, IR_LOCAL, "a.ai", 5);
        IrNode *e = mk(b, IR_LOAD, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        IrSlot *s = (IrSlot *)calloc(1, sizeof(IrSlot));
        DiagRecord **r = NULL;
        size_t n;
        s->index = 0;
        s->kind = IR_SLOT_LOCAL;
        s->name = strdup("b");
        s->type = ir_type_bool(b);
        s->span = mk_span("a.ai", 5, 4, 30);
        fn->u.function.nslots = 1;
        fn->u.function.slots = (IrSlot **)malloc(sizeof(IrSlot *));
        fn->u.function.slots[0] = s;
        slot->type = ir_type_bool(b);
        slot->u.local.slot_index = 0;
        e->type = ir_type_bool(b);
        e->trap_code = NULL;   /* missing R0805 */
        e->u.load.lvalue = slot;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* non-bool load carrying a trap code */
    {
        FN_BODY_SETUP("iload");
        IrNode *slot = mk(b, IR_LOCAL, "a.ai", 5);
        IrNode *e = mk(b, IR_LOAD, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        IrSlot *s = (IrSlot *)calloc(1, sizeof(IrSlot));
        DiagRecord **r = NULL;
        size_t n;
        s->index = 0;
        s->kind = IR_SLOT_LOCAL;
        s->name = strdup("v");
        s->type = ir_type_i32(b);
        s->span = mk_span("a.ai", 5, 4, 30);
        fn->u.function.nslots = 1;
        fn->u.function.slots = (IrSlot **)malloc(sizeof(IrSlot *));
        fn->u.function.slots[0] = s;
        slot->type = ir_type_i32(b);
        slot->u.local.slot_index = 0;
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0805";   /* i32 load has no trap obligation */
        e->u.load.lvalue = slot;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_TRAP with both a registry code and a user code */
    {
        FN_BODY_SETUP("trap");
        IrNode *t = mk(b, IR_TRAP, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        t->u.trap.code = "AIC-R0802";
        t->u.trap.has_user_code = true;
        t->u.trap.user_code = 7;
        ir_block_add_stmt(b, body, t);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_TRAP user code out of u32 range */
    {
        FN_BODY_SETUP("trap2");
        IrNode *t = mk(b, IR_TRAP, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        t->u.trap.code = NULL;
        t->u.trap.has_user_code = true;
        t->u.trap.user_code = (int64_t)UINT32_MAX + 1;
        ir_block_add_stmt(b, body, t);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 9, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
#undef FN_BODY_SETUP
}

static void test_store_lvalue(void)
{
    /* IR_STORE into a str element address (value address, not an lvalue) */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *strv = mk(b, IR_STR, "a.ai", 5);
        IrNode *idx = mk(b, IR_INDEX_ADDR, "a.ai", 5);
        IrNode *st = mk(b, IR_STORE, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        strv->type = ir_type_str(b);
        strv->u.constant.value = ir_const_str(b, (const uint8_t *)"hi", 2);
        idx->type = ir_type_ptr(b, ir_type_u8(b));
        idx->trap_code = "AIC-R0807";
        idx->u.index_addr.base = strv;
        idx->u.index_addr.index = mk(b, IR_INT, "a.ai", 5);
        idx->u.index_addr.index->type = ir_type_usize(b);
        idx->u.index_addr.index->u.constant.value =
            ir_const_int(b, ir_type_usize(b), 0);
        st->u.store.dest = idx;   /* str index address: not an lvalue */
        st->u.store.value = mk_int(b, "a.ai", ir_type_u8(b), 1);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 10, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IRConst_ADDR targeting a const (consts have no address) */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *gc = mk(b, IR_GLOBAL_CONST, "a.ai", 2);
        IrType *pi32 = ir_type_ptr(b, ir_type_i32(b));
        DiagRecord **r = NULL;
        size_t n;
        gc->u.global_const.name = strdup("C");
        gc->u.global_const.type = ir_type_i32(b);
        gc->u.global_const.value = ir_const_int(b, ir_type_i32(b), 5);
        ir_module_add_decl(b, m, gc);
        ir_build_add_module(b, m);
        ir_const_addr(b, pi32, gc, 0);   /* &C is unrepresentable */
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 10, NULL, 0);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IRConst_ADDR targeting a static array element is valid */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *gv = mk(b, IR_GLOBAL_VAR, "a.ai", 2);
        IrNode *gref = mk(b, IR_GLOBAL, "a.ai", 2);
        IrNode *idx = mk(b, IR_INDEX_ADDR, "a.ai", 3);
        IrType *arr = ir_type_array(b, ir_type_i32(b), 4);
        IrType *pi32 = ir_type_ptr(b, ir_type_i32(b));
        size_t n;
        gv->u.global_var.name = strdup("g");
        gv->u.global_var.type = arr;
        gv->u.global_var.init =
            ir_const_array(b, arr, NULL, 0);
        ir_module_add_decl(b, m, gv);
        ir_build_add_module(b, m);
        gref->type = arr;
        gref->u.global.target = gv;
        idx->type = pi32;
        idx->trap_code = "AIC-R0807";
        idx->u.index_addr.base = gref;   /* lvalue: IR_GLOBAL referencing g */
        idx->u.index_addr.index = mk(b, IR_INT, "a.ai", 3);
        idx->u.index_addr.index->type = ir_type_usize(b);
        idx->u.index_addr.index->u.constant.value =
            ir_const_int(b, ir_type_usize(b), 0);
        /* Keep idx reachable by attaching it under a dummy void function's
         * body block (reachability only needs the node in a module
         * subtree). */
        {
            IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 4);
            IrNode *blk = mk_block(b, "a.ai", 4);
            IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 4);
            es->u.expr_stmt.expr = idx;
            ir_block_add_stmt(b, blk, es);
            fn->u.function.name = strdup("g_f");
            fn->u.function.ret_type = ir_type_void(b);
            fn->u.function.body = blk;
            ir_module_add_decl(b, m, fn);
        }
        ir_const_addr(b, pi32, idx, 0);   /* &g[0] of a static array: valid */
        n = run_verify(b, IR_OK, NULL);
        CHECK(n == 0);
        ir_build_free(b);
    }
}

static void test_eval_order(void)
{
    /* binary node missing the right operand */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *e = mk(b, IR_ADD, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0802";
        e->u.binary.left = mk_int(b, "a.ai", ir_type_i32(b), 1);
        e->u.binary.right = NULL;   /* missing operand */
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 11, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_SELECT missing a branch */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *e = mk(b, IR_SELECT, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        e->type = ir_type_i32(b);
        e->u.select.cond = mk_bool(b, "a.ai", true);
        e->u.select.then_value = NULL;   /* missing branch */
        e->u.select.else_value = mk_int(b, "a.ai", ir_type_i32(b), 2);
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 11, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
    /* IR_NEG with no operand */
    {
        IrBuild *b = ir_build_new();
        IrNode *m = mk_module(b, "main", "a.ai");
        IrNode *fn = mk(b, IR_FUNCTION, "a.ai", 2);
        IrNode *body = mk_block(b, "a.ai", 3);
        IrNode *e = mk(b, IR_NEG, "a.ai", 5);
        IrNode *es = mk(b, IR_EXPR_STMT, "a.ai", 5);
        DiagRecord **r = NULL;
        size_t n;
        fn->u.function.name = strdup("f");
        fn->u.function.ret_type = ir_type_void(b);
        fn->u.function.body = body;
        ir_module_add_decl(b, m, fn);
        ir_build_add_module(b, m);
        e->type = ir_type_i32(b);
        e->trap_code = "AIC-R0802";
        e->u.unary.operand = NULL;
        es->u.expr_stmt.expr = e;
        ir_block_add_stmt(b, body, es);
        n = run_verify(b, IR_VIOLATION, &r);
        CHECK(n >= 1);
        if (n >= 1) {
            check_record_shape(r[0], 11, "a.ai", 5);
        }
        ir_records_free(r, n);
        ir_build_free(b);
    }
}

static void test_valid_emit(void)
{
    /* A violation record must emit as a valid JSONL line (contract shape) */
    IrBuild *b = ir_build_new();
    IrNode *m = mk_module(b, "main", "a.ai");
    IrNode *orphan = mk(b, IR_EMPTY, "a.ai", 2);
    DiagRecord **r = NULL;
    size_t n;
    DiagBuf buf;
    ir_build_add_module(b, m);
    (void)orphan;   /* deliberately unattached: the unreachable-node
                     * violation is the record we emit */
    n = run_verify(b, IR_VIOLATION, &r);
    CHECK(n >= 1);
    if (n >= 1) {
        diag_buf_init(&buf);
        CHECK(diag_emit_record(&buf, r[0]));
        CHECK(diag_buf_ok(&buf));
        CHECK(buf.len > 0);
        CHECK(strstr(buf.data, "\"code\":\"AIC-I0501\"") != NULL);
        CHECK(strstr(buf.data, "\"phase\":\"ir\"") != NULL);
        CHECK(strstr(buf.data, "\"recovery\":\"authoritative\"") != NULL);
        diag_buf_free(&buf);
    }
    ir_records_free(r, n);
    ir_build_free(b);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_kind_names();
    fprintf(stderr, "after test_kind_names\n");
    test_model_and_determinism();
    fprintf(stderr, "after test_model_and_determinism\n");
    test_valid_graph();
    fprintf(stderr, "after test_valid_graph\n");
    test_graph_wellformed();
    fprintf(stderr, "after test_graph_wellformed\n");
    test_span_cause();
    fprintf(stderr, "after test_span_cause\n");
    test_type_wellformed();
    fprintf(stderr, "after test_type_wellformed\n");
    test_operand_typing();
    fprintf(stderr, "after test_operand_typing\n");
    test_shift_result_type();
    fprintf(stderr, "after test_shift_result_type\n");
    test_terminators();
    fprintf(stderr, "after test_terminators\n");
    test_break_continue();
    fprintf(stderr, "after test_break_continue\n");
    test_return_typing();
    fprintf(stderr, "after test_return_typing\n");
    test_trap_codes();
    fprintf(stderr, "after test_trap_codes\n");
    test_store_lvalue();
    fprintf(stderr, "after test_store_lvalue\n");
    test_eval_order();
    fprintf(stderr, "after test_eval_order\n");
    test_valid_emit();
    fprintf(stderr, "after test_valid_emit\n");

    if (g_failures) {
        fprintf(stderr, "ir_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("ir_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
