/* bootstrap/src/ir/ir_dump_test.c
 *
 * WP-M0-16b2 IR deterministic dump and verification unit tests: dump
 * determinism (identical IR -> identical dump bytes), round-trip
 * reconstruction (dump -> parse -> re-dump byte-identical, contract
 * sec. 11.4 / invariant 12), span/cause preservation, all node-kind
 * coverage, malformed-dump rejection, and ir_dump_verify reporting.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\stage0\msvc-ir16b2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/ir/ir_dump_test.c \
 *     bootstrap/src/ir/ir_dump.c bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-ir16b2/ir_dump_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\stage0\clang-ir16b2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */
#include "ir_dump.h"

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
 * Shared construction helpers (mirror ir_core_test.c so both suites stay
 * consistent; ir_core_test.c's helpers are static, hence the copies).
 * ------------------------------------------------------------------------- */

static DiagSpan *mk_span(const char *file, int64_t line, int64_t col,
                         int64_t offset)
{
    return diag_span_new_point(file, line, col, offset);
}

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
    return mk(b, IR_BLOCK, file, line);
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
 * A comprehensive VALID graph (mirrors ir_core_test.c make_valid_build):
 * module m (global var) imported by module main (import, struct decl,
 * function with params/slots, if/store, switch with returning cases,
 * global ref). Verification must pass with zero records, and the graph
 * round-trips byte-identically.
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

    /* reference imported global: gref_stmt(g); */
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
 * An ALL-KINDS graph: every node kind appears at least once (module
 * structure, all statements, all value-producing nodes), plus every type
 * kind, every const kind, composite types, range spans, and multi-link
 * cause chains. Not every payload is invariant-clean by design (some
 * operands are NULL); the round-trip assertion is byte identity.
 * ------------------------------------------------------------------------- */

static IrBuild *make_all_kinds_build(void)
{
    IrBuild *b = ir_build_new();
    IrType *i32 = ir_type_i32(b);
    IrType *i32p = ir_type_ptr(b, i32);
    IrType *arr4 = ir_type_array(b, i32, 4);
    IrType *u8s = ir_type_slice(b, ir_type_u8(b));
    IrNode *m_mod, *main_mod, *imp, *s_decl, *e_decl, *gconst, *gvar;
    IrNode *fn, *f2, *noret, *body, *stmt;
    IrConst *c0 = ir_const_int(b, i32, 0);
    IrConst *c1 = ir_const_int(b, i32, 1);
    IrConst *cnull = ir_const_null(b, i32p);
    IrConst *cstr = ir_const_str(b, (const uint8_t *)"hi", 2);
    IrConst *cempty = ir_const_str(b, (const uint8_t *)"", 0);
    IrConst *carr, *caddr;
    IrConst *items[2];
    IrType *st_type, *en_type;
    size_t i;

    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }

    /* module m: global var */
    m_mod = mk_module(b, "m", "m.ai");
    gvar = mk(b, IR_GLOBAL_VAR, "m.ai", 2);
    gvar->u.global_var.name = strdup("g");
    gvar->u.global_var.type = i32;
    gvar->u.global_var.init = c0;
    ir_module_add_decl(b, m_mod, gvar);
    ir_build_add_module(b, m_mod);

    /* module main: everything else */
    main_mod = mk_module(b, "main", "main.ai");
    imp = mk(b, IR_IMPORT, "main.ai", 2);
    imp->u.import.name = strdup("m");
    ir_module_add_import(b, main_mod, imp);

    /* struct S { x: i32 } */
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
    st_type = ir_type_struct(b, s_decl);
    ir_module_add_decl(b, main_mod, s_decl);

    /* enum E: i32 { A = 0 } */
    e_decl = mk(b, IR_ENUM_DECL, "main.ai", 4);
    e_decl->u.enum_decl.name = strdup("E");
    e_decl->u.enum_decl.underlying = i32;
    e_decl->u.enum_decl.nmembers = 1;
    e_decl->u.enum_decl.members = (IrEnumMember *)calloc(1,
                                                         sizeof(IrEnumMember));
    e_decl->u.enum_decl.members[0].name = strdup("A");
    e_decl->u.enum_decl.members[0].value = 0;
    e_decl->u.enum_decl.members[0].span = mk_span("main.ai", 4, 8, 40);
    en_type = ir_type_enum(b, e_decl);
    ir_module_add_decl(b, main_mod, e_decl);

    /* global const K: i32 = 1; global var G: i32 = 0 */
    gconst = mk(b, IR_GLOBAL_CONST, "main.ai", 5);
    gconst->u.global_const.name = strdup("K");
    gconst->u.global_const.type = i32;
    gconst->u.global_const.value = c1;
    ir_module_add_decl(b, main_mod, gconst);
    gvar = mk(b, IR_GLOBAL_VAR, "main.ai", 6);
    gvar->u.global_var.name = strdup("G");
    gvar->u.global_var.type = i32;
    gvar->u.global_var.init = c0;
    ir_module_add_decl(b, main_mod, gvar);

    /* composite consts: array [0,1]; addr &G */
    items[0] = c0;
    items[1] = c1;
    carr = ir_const_array(b, arr4, items, 2);
    caddr = ir_const_addr(b, i32p, gvar, 0);

    /* rt.proc.exit (noreturn callee for IR_CALL_TERM) */
    noret = mk(b, IR_FUNCTION, "main.ai", 8);
    noret->u.function.name = strdup("rt.proc.exit");
    noret->u.function.ret_type = ir_type_void(b);
    noret->u.function.noreturn = true;
    ir_module_add_decl(b, main_mod, noret);

    /* function f: i32 x -> i32, slots 0..4 */
    fn = mk(b, IR_FUNCTION, "main.ai", 10);
    fn->u.function.name = strdup("f");
    fn->u.function.ret_type = i32;
    fn->u.function.nparams = 1;
    fn->u.function.params = (IrParam *)calloc(1, sizeof(IrParam));
    fn->u.function.params[0].name = strdup("x");
    fn->u.function.params[0].type = i32;
    fn->u.function.params[0].slot_index = 0;
    fn->u.function.params[0].span = mk_span("main.ai", 10, 5, 42);
    {
        IrSlot **slots = (IrSlot **)calloc(5, sizeof(IrSlot *));
        const char *names[5] = { "x", "y", "t1", "t2", "t3" };
        for (i = 0; i < 5; i++) {
            slots[i] = (IrSlot *)calloc(1, sizeof(IrSlot));
            slots[i]->index = (int64_t)i;
            slots[i]->kind = (i == 0) ? IR_SLOT_PARAM
                            : (i == 1) ? IR_SLOT_LOCAL : IR_SLOT_TEMP;
            slots[i]->name = strdup(names[i]);
            slots[i]->type = (i == 1) ? arr4 : i32;
            slots[i]->span = mk_span("main.ai", 10 + (int64_t)i, 5,
                                     50 + (int64_t)i * 7);
        }
        fn->u.function.nslots = 5;
        fn->u.function.slots = slots;
    }
    body = mk_block(b, "main.ai", 12);

    /* IR_EMPTY */
    ir_block_add_stmt(b, body, mk(b, IR_EMPTY, "main.ai", 12));

    /* IR_LOCAL_DECL y = 0 (slot 1) */
    {
        IrNode *ld = mk(b, IR_LOCAL_DECL, "main.ai", 13);
        ld->u.local_decl.slot_index = 1;
        ld->u.local_decl.init = mk_int(b, "main.ai", i32, 0);
        ir_block_add_stmt(b, body, ld);
    }

    /* IR_EXPR_STMT chain covering every value-producing kind. All
     * operands reference existing leaves where convenient; NULL where
     * typing would be contrived (round-trip is the assertion). */
    {
        IrNode *e = NULL;
        IrNode *es;

        /* constants */
        e = mk(b, IR_INT, "main.ai", 14);      e->type = i32;
        e->u.constant.value = c0;              es = mk(b, IR_EXPR_STMT, "main.ai", 14); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_BOOL, "main.ai", 14);     e->type = ir_type_bool(b);
        e->u.constant.value = ir_const_bool(b, true); es = mk(b, IR_EXPR_STMT, "main.ai", 14); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_NULL, "main.ai", 14);     e->type = i32p;
        es = mk(b, IR_EXPR_STMT, "main.ai", 14); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_STR, "main.ai", 14);      e->type = ir_type_str(b);
        e->u.constant.value = cstr;            es = mk(b, IR_EXPR_STMT, "main.ai", 14); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_ENUM_VAL, "main.ai", 14); e->type = en_type;
        e->u.constant.value = ir_const_enum(b, en_type, 0);
        es = mk(b, IR_EXPR_STMT, "main.ai", 14); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);

        /* locals / globals / memory */
        e = mk(b, IR_LOCAL, "main.ai", 15);    e->type = i32;
        e->u.local.slot_index = 0;             es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_GLOBAL, "main.ai", 15);   e->type = i32;
        e->u.global.target = gvar;             es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_FIELD_ADDR, "main.ai", 15); e->type = i32p;
        e->u.field_addr.base = NULL; e->u.field_addr.field_index = 0;
        es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_INDEX_ADDR, "main.ai", 15); e->type = i32p;
        e->u.index_addr.base = NULL; e->u.index_addr.index = mk_int(b, "main.ai", ir_type_usize(b), 0);
        es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_DEREF, "main.ai", 15);    e->type = i32;
        e->u.deref.ptr = NULL; e->trap_code = "AIC-R0809";
        es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_LOAD, "main.ai", 15);     e->type = i32;
        e->u.load.lvalue = NULL;               es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_STORE, "main.ai", 15);
        e->u.store.dest = NULL; e->u.store.value = mk_int(b, "main.ai", i32, 0);
        es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_ZERO, "main.ai", 15);
        e->u.unary.operand = NULL;             es = mk(b, IR_EXPR_STMT, "main.ai", 15); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);

        /* arithmetic / logic / comparison */
        {
            static const IrNodeKind binary[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_SHL, IR_SHR, IR_BAND, IR_BOR, IR_BXOR,
                IR_LAND, IR_LOR, IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE,
                IR_SLICE_EQ, IR_PTR_DIFF
            };
            for (i = 0; i < sizeof(binary) / sizeof(binary[0]); i++) {
                e = mk(b, binary[i], "main.ai", 16);
                e->type = ir_type_bool(b);
                e->trap_code = (binary[i] == IR_ADD || binary[i] == IR_SUB ||
                                binary[i] == IR_MUL || binary[i] == IR_DIV ||
                                binary[i] == IR_MOD) ? "AIC-R0802" : NULL;
                e->u.binary.left = NULL;
                e->u.binary.right = NULL;
                es = mk(b, IR_EXPR_STMT, "main.ai", 16);
                es->u.expr_stmt.expr = e;
                ir_block_add_stmt(b, body, es);
            }
        }
        {
            static const IrNodeKind unary[] = {
                IR_NEG, IR_BNOT, IR_LNOT, IR_LEN, IR_PTR, IR_CAST,
                IR_WRAP
            };
            for (i = 0; i < sizeof(unary) / sizeof(unary[0]); i++) {
                e = mk(b, unary[i], "main.ai", 17);
                e->type = (unary[i] == IR_LEN) ? ir_type_usize(b) : i32;
                e->trap_code = (unary[i] == IR_CAST) ? "AIC-R0801" : NULL;
                e->u.unary.operand = NULL;
                es = mk(b, IR_EXPR_STMT, "main.ai", 17);
                es->u.expr_stmt.expr = e;
                ir_block_add_stmt(b, body, es);
            }
        }
        e = mk(b, IR_SELECT, "main.ai", 18);   e->type = i32;
        e->u.select.cond = mk_bool(b, "main.ai", true);
        e->u.select.then_value = mk_int(b, "main.ai", i32, 1);
        e->u.select.else_value = NULL;
        es = mk(b, IR_EXPR_STMT, "main.ai", 18); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_CALL, "main.ai", 18);     e->type = i32;
        e->u.call.callee = fn;
        ir_call_add_arg(b, e, mk_int(b, "main.ai", i32, 3));
        es = mk(b, IR_EXPR_STMT, "main.ai", 18); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_SLICE, "main.ai", 18);    e->type = u8s;
        e->u.slice.base = NULL; e->u.slice.start = NULL; e->u.slice.end = NULL;
        e->trap_code = "AIC-R0807";
        es = mk(b, IR_EXPR_STMT, "main.ai", 18); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_PTR_ADD, "main.ai", 18);  e->type = i32p;
        e->u.ptr_arith.ptr = NULL; e->u.ptr_arith.offset = mk_int(b, "main.ai", ir_type_usize(b), 2);
        e->trap_code = "AIC-R0816";
        es = mk(b, IR_EXPR_STMT, "main.ai", 18); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
        e = mk(b, IR_PTR_SUB, "main.ai", 18);  e->type = i32p;
        e->u.ptr_arith.ptr = NULL; e->u.ptr_arith.offset = mk_int(b, "main.ai", ir_type_usize(b), 2);
        e->trap_code = "AIC-R0816";
        es = mk(b, IR_EXPR_STMT, "main.ai", 18); es->u.expr_stmt.expr = e; ir_block_add_stmt(b, body, es);
    }

    /* IR_IF (then: return 1; else: return 2) */
    {
        IrNode *cond = mk_bool(b, "main.ai", true);
        IrNode *thenb = mk_block(b, "main.ai", 20);
        ir_block_add_stmt(b, thenb, mk_return(b, "main.ai", mk_int(b, "main.ai", i32, 1)));
        IrNode *elseb = mk_block(b, "main.ai", 21);
        ir_block_add_stmt(b, elseb, mk_return(b, "main.ai", mk_int(b, "main.ai", i32, 2)));
        stmt = mk(b, IR_IF, "main.ai", 20);
        stmt->u.if_stmt.cond = cond;
        stmt->u.if_stmt.then_block = thenb;
        stmt->u.if_stmt.else_block = elseb;
        ir_block_add_stmt(b, body, stmt);
    }

    /* IR_WHILE (body: IR_BREAK) */
    {
        IrNode *cond = mk_bool(b, "main.ai", true);
        IrNode *wbody = mk_block(b, "main.ai", 23);
        IrNode *brk = mk(b, IR_BREAK, "main.ai", 23);
        stmt = mk(b, IR_WHILE, "main.ai", 23);
        brk->u.break_stmt.target = stmt;
        ir_block_add_stmt(b, wbody, brk);
        stmt->u.while_stmt.cond = cond;
        stmt->u.while_stmt.body = wbody;
        ir_block_add_stmt(b, body, stmt);
    }

    /* IR_FOR (init: IR_EMPTY; cond true; step: IR_EXPR_STMT of IR_INT;
     * body: IR_CONTINUE) */
    {
        IrNode *init = mk(b, IR_EMPTY, "main.ai", 24);
        IrNode *cond = mk_bool(b, "main.ai", true);
        IrNode *step_expr = mk_int(b, "main.ai", i32, 1);
        IrNode *step = mk(b, IR_EXPR_STMT, "main.ai", 24);
        step->u.expr_stmt.expr = step_expr;
        IrNode *fbody = mk_block(b, "main.ai", 25);
        IrNode *cont = mk(b, IR_CONTINUE, "main.ai", 25);
        stmt = mk(b, IR_FOR, "main.ai", 24);
        cont->u.continue_stmt.target = stmt;
        ir_block_add_stmt(b, fbody, cont);
        stmt->u.for_stmt.init = init;
        stmt->u.for_stmt.cond = cond;
        stmt->u.for_stmt.step = step;
        stmt->u.for_stmt.body = fbody;
        ir_block_add_stmt(b, body, stmt);
    }

    /* IR_CALL_TERM to rt.proc.exit (terminator in its own block) */
    {
        IrNode *ct = mk(b, IR_CALL_TERM, "main.ai", 26);
        ct->u.call_term.callee = noret;
        ir_call_term_add_arg(b, ct, mk_int(b, "main.ai", i32, 1));
        ir_block_add_stmt(b, body, ct);
    }

    /* IR_SWITCH (x) { case 0: return 1; default: IR_TRAP } */
    {
        IrNode *sel = mk(b, IR_LOCAL, "main.ai", 27);
        sel->type = i32;
        sel->u.local.slot_index = 0;
        IrNode *case0 = mk(b, IR_CASE, "main.ai", 28);
        case0->u.case_clause.value = c0;
        IrNode *caseb = mk_block(b, "main.ai", 28);
        ir_block_add_stmt(b, caseb, mk_return(b, "main.ai", mk_int(b, "main.ai", i32, 1)));
        case0->u.case_clause.body = caseb;
        IrNode *def = mk(b, IR_DEFAULT, "main.ai", 29);
        IrNode *defb = mk_block(b, "main.ai", 29);
        IrNode *trap = mk(b, IR_TRAP, "main.ai", 29);
        trap->u.trap.code = "AIC-R0802";
        trap->u.trap.has_user_code = false;
        trap->u.trap.user_code = 0;
        ir_block_add_stmt(b, defb, trap);
        def->u.default_clause.body = defb;
        stmt = mk(b, IR_SWITCH, "main.ai", 27);
        stmt->u.switch_stmt.selector = sel;
        stmt->u.switch_stmt.ncases = 1;
        stmt->u.switch_stmt.cases = (IrNode **)malloc(sizeof(IrNode *));
        stmt->u.switch_stmt.cases[0] = case0;
        stmt->u.switch_stmt.default_clause = def;
        ir_block_add_stmt(b, body, stmt);
    }

    fn->u.function.body = body;
    ir_module_add_decl(b, main_mod, fn);

    /* function f2 (a separate callee) */
    f2 = mk(b, IR_FUNCTION, "main.ai", 31);
    f2->u.function.name = strdup("f2");
    f2->u.function.ret_type = i32;
    f2->u.function.body = NULL;
    ir_module_add_decl(b, main_mod, f2);

    ir_build_add_module(b, main_mod);

    /* Exercise every const kind that was created above (kept alive). */
    (void)cnull;
    (void)carr;
    (void)caddr;
    (void)cempty;
    (void)st_type;
    (void)en_type;
    (void)u8s;
    (void)arr4;
    return b;
}

/* ---------------------------------------------------------------------------
 * Dump helpers
 * ------------------------------------------------------------------------- */

static void dump_to_buf(IrBuild *b, DiagBuf *buf)
{
    diag_buf_init(buf);
    CHECK(ir_dump_write(b, buf));
    CHECK(diag_buf_ok(buf));
}

static bool buf_equal(const DiagBuf *a, const DiagBuf *b2)
{
    return a->len == b2->len &&
           (a->len == 0 || memcmp(a->data, b2->data, a->len) == 0);
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_dump_determinism(void)
{
    /* identical IR -> identical dump bytes: two separate builds built by
     * the same construction, and two dumps of the same build. */
    IrBuild *b1 = make_valid_build();
    IrBuild *b2 = make_valid_build();
    DiagBuf d1, d2, d3;
    CHECK(b1 != NULL && b2 != NULL);
    if (b1 == NULL || b2 == NULL) {
        return;
    }
    dump_to_buf(b1, &d1);
    dump_to_buf(b1, &d2);       /* same build twice */
    dump_to_buf(b2, &d3);       /* identical build */
    CHECK(buf_equal(&d1, &d2));
    CHECK(buf_equal(&d1, &d3));
    CHECK(d1.len > 0);
    /* canonical markers */
    CHECK(strstr(d1.data, "H 2 ") != NULL);
    CHECK(strstr(d1.data, "T 4 i32") != NULL);
    CHECK(strstr(d1.data, "N 0 IR_MODULE") != NULL);
    CHECK(strstr(d1.data, "K AST_MODULE_DECL") != NULL);
    CHECK(strstr(d1.data, "P main ") != NULL);
    diag_buf_free(&d1);
    diag_buf_free(&d2);
    diag_buf_free(&d3);
    ir_build_free(b1);
    ir_build_free(b2);
}

static void test_dump_distinct(void)
{
    /* distinct IR (one constant differs) -> distinct dump bytes */
    IrBuild *a = ir_build_new();
    IrBuild *b = ir_build_new();
    IrType *i32 = ir_type_i32(a);
    DiagBuf da, db;
    IrNode *ma, *mb;
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        return;
    }
    ma = mk_module(a, "main", "a.ai");
    ir_module_add_decl(a, ma,
                       mk_int(a, "a.ai", i32, 7));
    ir_build_add_module(a, ma);
    mb = mk_module(b, "main", "a.ai");
    ir_module_add_decl(b, mb,
                       mk_int(b, "a.ai", i32, 8));
    ir_build_add_module(b, mb);
    dump_to_buf(a, &da);
    dump_to_buf(b, &db);
    CHECK(da.len == db.len);
    CHECK(!buf_equal(&da, &db));
    diag_buf_free(&da);
    diag_buf_free(&db);
    ir_build_free(a);
    ir_build_free(b);
}

static void test_dump_roundtrip_valid(void)
{
    IrBuild *b = make_valid_build();
    IrBuild *r = NULL;
    DiagBuf d1, d2;
    IrDumpStatus st;
    char errbuf[256];
    DiagRecord **recs = NULL;
    size_t nrecs = 0;
    IrStatus vs;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    dump_to_buf(b, &d1);
    st = ir_dump_parse(d1.data, d1.len, &r, errbuf, sizeof(errbuf));
    CHECK(st == IR_DUMP_OK);
    if (st != IR_DUMP_OK) {
        fprintf(stderr, "  parse error: %s\n", errbuf);
        diag_buf_free(&d1);
        ir_build_free(b);
        return;
    }
    CHECK(r != NULL);
    CHECK(r->nnodes == b->nnodes);
    CHECK(r->ntypes == b->ntypes);
    CHECK(r->nconsts == b->nconsts);
    CHECK(r->nmodules == b->nmodules);
    /* re-dump byte-identical (invariant 12) */
    dump_to_buf(r, &d2);
    CHECK(buf_equal(&d1, &d2));
    /* reconstructed graph verifies clean */
    vs = ir_core_verify(r, &recs, &nrecs);
    CHECK(vs == IR_OK);
    CHECK(nrecs == 0);
    if (recs != NULL) {
        ir_records_free(recs, nrecs);
    }
    /* node payload survives: function body has same stmt count */
    {
        const IrNode *fn0 = NULL;
        size_t i;
        for (i = 0; i < r->nmodules; i++) {
            const IrNode *m = r->modules[i];
            size_t j;
            for (j = 0; j < m->u.module.ndecls; j++) {
                if (m->u.module.decls[j]->kind == IR_FUNCTION) {
                    fn0 = m->u.module.decls[j];
                }
            }
        }
        CHECK(fn0 != NULL);
        if (fn0 != NULL) {
            CHECK(fn0->u.function.body != NULL);
            CHECK(fn0->u.function.body->kind == IR_BLOCK);
            CHECK(strcmp(fn0->u.function.name, "f") == 0);
            CHECK(fn0->u.function.body->u.block.nstmts == 4);
        }
    }
    diag_buf_free(&d1);
    diag_buf_free(&d2);
    ir_build_free(r);
    ir_build_free(b);
}

static void test_dump_all_kinds_roundtrip(void)
{
    IrBuild *b = make_all_kinds_build();
    IrBuild *r = NULL;
    DiagBuf d1, d2;
    IrDumpStatus st;
    char errbuf[256];
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    dump_to_buf(b, &d1);
    st = ir_dump_parse(d1.data, d1.len, &r, errbuf, sizeof(errbuf));
    CHECK(st == IR_DUMP_OK);
    if (st != IR_DUMP_OK) {
        fprintf(stderr, "  parse error: %s\n", errbuf);
        diag_buf_free(&d1);
        ir_build_free(b);
        return;
    }
    CHECK(r != NULL);
    CHECK(r->nnodes == b->nnodes);
    dump_to_buf(r, &d2);
    CHECK(buf_equal(&d1, &d2));
    /* every node kind is represented in the dump text */
    {
        static const char *const kinds[] = {
            "IR_MODULE", "IR_IMPORT", "IR_STRUCT_DECL", "IR_ENUM_DECL",
            "IR_GLOBAL_CONST", "IR_GLOBAL_VAR", "IR_FUNCTION",
            "IR_BLOCK", "IR_LOCAL_DECL", "IR_IF", "IR_WHILE", "IR_FOR",
            "IR_SWITCH", "IR_CASE", "IR_DEFAULT", "IR_BREAK",
            "IR_CONTINUE", "IR_RETURN", "IR_EXPR_STMT", "IR_EMPTY",
            "IR_CALL_TERM", "IR_TRAP",
            "IR_INT", "IR_BOOL", "IR_NULL", "IR_STR", "IR_ENUM_VAL",
            "IR_LOCAL", "IR_GLOBAL", "IR_FIELD_ADDR", "IR_INDEX_ADDR",
            "IR_DEREF", "IR_LOAD", "IR_STORE",
            "IR_ADD", "IR_SUB", "IR_MUL", "IR_DIV", "IR_MOD", "IR_NEG",
            "IR_SHL", "IR_SHR",
            "IR_BAND", "IR_BOR", "IR_BXOR", "IR_BNOT", "IR_LNOT",
            "IR_LAND", "IR_LOR",
            "IR_EQ", "IR_NE", "IR_LT", "IR_LE", "IR_GT", "IR_GE",
            "IR_SLICE_EQ", "IR_SELECT", "IR_CALL", "IR_LEN", "IR_PTR",
            "IR_SLICE", "IR_CAST", "IR_WRAP",
            "IR_PTR_ADD", "IR_PTR_SUB", "IR_PTR_DIFF", "IR_ZERO"
        };
        size_t i;
        for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            char needle[64];
            snprintf(needle, sizeof(needle), " %s ", kinds[i]);
            CHECK(strstr(d1.data, needle) != NULL);
        }
    }
    diag_buf_free(&d1);
    diag_buf_free(&d2);
    ir_build_free(r);
    ir_build_free(b);
}

static void test_dump_span_cause_preservation(void)
{
    /* range spans and multi-link cause chains survive round-trip
     * field-by-field (contract sec. 8, 11.6). */
    IrBuild *b = ir_build_new();
    IrBuild *r = NULL;
    DiagBuf d1, d2;
    IrDumpStatus st;
    char errbuf[256];
    IrNode *m, *fn, *body, *ret, *local;
    IrType *i32 = ir_type_i32(b);
    size_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    m = mk_module(b, "main", "main.ai");
    fn = mk(b, IR_FUNCTION, "main.ai", 5);
    fn->u.function.name = strdup("f");
    fn->u.function.ret_type = i32;
    fn->u.function.nslots = 1;
    fn->u.function.slots = (IrSlot **)calloc(1, sizeof(IrSlot *));
    fn->u.function.slots[0] = (IrSlot *)calloc(1, sizeof(IrSlot));
    fn->u.function.slots[0]->index = 0;
    fn->u.function.slots[0]->kind = IR_SLOT_LOCAL;
    fn->u.function.slots[0]->name = strdup("x");
    fn->u.function.slots[0]->type = i32;
    fn->u.function.slots[0]->span = diag_span_new_range(
        "main.ai", 3, 5, 20, 3, 8, 23);
    /* give the function a range span with a two-link cause chain */
    diag_span_free(fn->span);
    fn->span = diag_span_new_range("main.ai", 5, 1, 40, 9, 1, 80);
    free(fn->causes[0].construct_kind);
    diag_span_free(fn->causes[0].span);
    fn->causes[0].construct_kind = strdup("AST_FUNC_DECL");
    fn->causes[0].span = diag_span_new_range("main.ai", 5, 1, 40, 5, 10, 49);
    ir_node_add_cause(b, fn, "AST_BLOCK", diag_span_new_range(
                          "main.ai", 6, 1, 50, 9, 1, 80), 3, 5, -1);
    body = mk_block(b, "main.ai", 6);
    local = mk(b, IR_LOCAL, "main.ai", 7);
    local->type = i32;
    local->u.local.slot_index = 0;
    ret = mk(b, IR_RETURN, "main.ai", 8);
    ret->u.return_stmt.value = local;
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, m, fn);
    ir_build_add_module(b, m);

    dump_to_buf(b, &d1);
    st = ir_dump_parse(d1.data, d1.len, &r, errbuf, sizeof(errbuf));
    CHECK(st == IR_DUMP_OK);
    if (st != IR_DUMP_OK) {
        fprintf(stderr, "  parse error: %s\n", errbuf);
        diag_buf_free(&d1);
        ir_build_free(b);
        return;
    }
    dump_to_buf(r, &d2);
    CHECK(buf_equal(&d1, &d2));
    /* field-by-field comparison of the reconstructed function node */
    {
        const IrNode *m2 = r->modules[0];
        const IrNode *fn2 = m2->u.module.decls[0];
        CHECK(fn2->kind == IR_FUNCTION);
        CHECK(fn2->span != NULL);
        CHECK(strcmp(fn2->span->file, "main.ai") == 0);
        CHECK(fn2->span->start.line == 5 && fn2->span->start.col == 1);
        CHECK(fn2->span->end.line == 9 && fn2->span->end.offset == 80);
        CHECK(fn2->cause_count == 2);
        CHECK(strcmp(fn2->causes[0].construct_kind, "AST_FUNC_DECL") == 0);
        CHECK(fn2->causes[0].span->start.offset == 40);
        CHECK(fn2->causes[0].span->end.col == 10);
        CHECK(strcmp(fn2->causes[1].construct_kind, "AST_BLOCK") == 0);
        CHECK(fn2->causes[1].ref_decl == 3);
        CHECK(fn2->causes[1].ref_type == 5);
        CHECK(fn2->causes[1].ref_const == -1);
        CHECK(fn2->u.function.body != NULL);
        CHECK(fn2->u.function.slots[0]->span->start.offset == 20);
        CHECK(fn2->u.function.slots[0]->span->end.col == 8);
    }
    (void)i;
    diag_buf_free(&d1);
    diag_buf_free(&d2);
    ir_build_free(r);
    ir_build_free(b);
}

static void test_dump_parse_malformed(void)
{
    /* malformed inputs are rejected with a deterministic message */
    static const char *const bad[] = {
        "",                                    /* empty */
        "H 1 13 0 0\n",                        /* missing records */
        "X 1 2 3\n",                           /* bad tag */
        "H 1 1 0 0\nT 0 i32 4 4\n",            /* truncated (no M/N); also
                                                * ntypes < 13 */
        "H 1 13 0 0\nM 0\nT 99 i32 4 4\n",     /* type after module line */
        "H 1 13 0 0\nM 0\nN 0 IR_BOGUS -1 - a.ai 1 1 0 1 1 0 0\nP\n",
        "H 1 13 0 0\nM 0\nN 0 IR_EMPTY -1 - a.ai 1 1 0 1 1 0 0\nP\nN 1 IR_EMPTY -1 - a.ai 1 1 0 1 1 0 0\nP\n",
        "H 1 13 0 0\nM 7\n",                  /* module id out of range */
        "H 0 12 0 0\nM\n"                     /* ntypes < 13 malformed */
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        IrBuild *r = NULL;
        char errbuf[256];
        IrDumpStatus st = ir_dump_parse(bad[i], strlen(bad[i]), &r,
                                        errbuf, sizeof(errbuf));
        CHECK(st == IR_DUMP_MALFORMED);
        CHECK(errbuf[0] != '\0');
        CHECK(r == NULL);
    }
}

static void test_dump_parse_malformed_str_const(void)
{
    /* The 13 base-type records every dump carries (ids 0..12 in spec
     * sec. 7.1 order), used to reach the const table in hand-written
     * malformed inputs. */
    static const char *const cases[] = {
        "H 1 13 1 0\n"
        "T 0 void 0 1\n"
        "T 1 bool 1 1\n"
        "T 2 i8 1 1\n"
        "T 3 i16 2 2\n"
        "T 4 i32 4 4\n"
        "T 5 i64 8 8\n"
        "T 6 u8 1 1\n"
        "T 7 u16 2 2\n"
        "T 8 u32 4 4\n"
        "T 9 u64 8 8\n"
        "T 10 isize 8 8\n"
        "T 11 usize 8 8\n"
        "T 12 str 16 8\n"
        "C 0 str 12 1 zz\n"          /* non-hex str payload */
        "M 0\n",
        "H 1 13 1 0\n"
        "T 0 void 0 1\n"
        "T 1 bool 1 1\n"
        "T 2 i8 1 1\n"
        "T 3 i16 2 2\n"
        "T 4 i32 4 4\n"
        "T 5 i64 8 8\n"
        "T 6 u8 1 1\n"
        "T 7 u16 2 2\n"
        "T 8 u32 4 4\n"
        "T 9 u64 8 8\n"
        "T 10 isize 8 8\n"
        "T 11 usize 8 8\n"
        "T 12 str 16 8\n"
        "C 0 str 12 0 00\n"          /* blen 0 must carry no hex token */
        "M 0\n"
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        IrBuild *r = NULL;
        char errbuf[256];
        IrDumpStatus st = ir_dump_parse(cases[i], strlen(cases[i]), &r,
                                        errbuf, sizeof(errbuf));
        CHECK(st == IR_DUMP_MALFORMED);
        CHECK(errbuf[0] != '\0');
        CHECK(r == NULL);
    }
}

/* ---------------------------------------------------------------------------
 * MIN-1 remediation (reviewer2 t_47cce3e7): empty text fields in
 * hand-crafted dumps must be rejected by ir_dump_parse, not silently
 * misparsed. Two representations are covered: a zero-width field
 * (consecutive separators, which the tokenizer would otherwise collapse
 * into the adjacent field) and a token that decodes to a zero-length
 * string (the dump-format escape '\x00').
 * ------------------------------------------------------------------------- */

/* Minimal well-formed prefix: header (1 module, the 13 base types,
 * 0 consts, 1 node), the 13 base-type records, and the module order
 * line. Callers append the N/K/P records of the single IR_MODULE
 * node. */
static void dump_minimal_prefix(char *buf, size_t cap)
{
    int n = snprintf(buf, cap,
        "H 1 13 0 1\n"
        "T 0 void 0 1\n"
        "T 1 bool 1 1\n"
        "T 2 i8 1 1\n"
        "T 3 i16 2 2\n"
        "T 4 i32 4 4\n"
        "T 5 i64 8 8\n"
        "T 6 u8 1 1\n"
        "T 7 u16 2 2\n"
        "T 8 u32 4 4\n"
        "T 9 u64 8 8\n"
        "T 10 isize 8 8\n"
        "T 11 usize 8 8\n"
        "T 12 str 16 8\n"
        "M 0\n");
    CHECK(n > 0 && (size_t)n < cap);
}

/* Assert ir_dump_parse rejects `text` with a deterministic reason and
 * no owned build. */
static void expect_malformed_dump(const char *text)
{
    IrBuild *r = NULL;
    char errbuf[256];
    IrDumpStatus st = ir_dump_parse(text, strlen(text), &r, errbuf,
                                    sizeof(errbuf));
    CHECK(st == IR_DUMP_MALFORMED);
    CHECK(errbuf[0] != '\0');
    CHECK(r == NULL);
}

/* Append the single IR_MODULE node records (span, one cause, payload)
 * to a dump that already holds the minimal prefix. */
static void append_module_dump(char *buf, size_t cap, const char *payload)
{
    size_t off = strlen(buf);
    int n = snprintf(buf + off, cap - off,
        "N 0 IR_MODULE -1 - a.ai 1 1 0 1 1 0 1\n"
        "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
        "P %s\n",
        payload);
    CHECK(n > 0 && (size_t)n < cap - off);
}

static void test_dump_parse_empty_field_collapse(void)
{
    /* MIN-1: a zero-width text field (consecutive separators) must be
     * rejected; without the check the tokenizer drops the empty token
     * and the adjacent field is silently misread. Payload layout here:
     * name, nimports, [imports], ndecl, [decls]; the intended dump
     * 'name=<empty> nimp=1 import=0 ndecl=0' is misparsed as
     * 'name="1" nimp=0 ndecl=0' and accepted. */
    char dump[2048];
    dump_minimal_prefix(dump, sizeof(dump));
    append_module_dump(dump, sizeof(dump), "  1 0 0");   /* name empty */
    expect_malformed_dump(dump);

    /* same rule applies to non-payload records: the node header has an
     * empty trap field (double space between the type ref and '-') */
    {
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 0 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "M 0\n"
            "N 0 IR_MODULE -1  - a.ai 1 1 0 1 1 0 1\n"
            "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
    }
    expect_malformed_dump(dump);

    /* cleanup-path guard: a malformed span escape on an N header that
     * declares ncauses=1 must be rejected without touching the cause
     * arrays (they are allocated only after the primary span parse). */
    {
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 0 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "M 0\n"
            "N 0 IR_MODULE -1 - \\q 1 1 0 1 1 0 1\n"
            "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
    }
    expect_malformed_dump(dump);
}

static void test_dump_parse_empty_decoded_string(void)
{
    /* MIN-1: a text field that decodes to a zero-length string (the
     * dump-format escape '\x00') is an empty required field and must
     * be rejected. */
    char dump[2048];
    dump_minimal_prefix(dump, sizeof(dump));
    append_module_dump(dump, sizeof(dump), "\\x00 0 0");  /* name empty */
    expect_malformed_dump(dump);

    /* span file path decodes to empty (node header) */
    {
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 0 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "M 0\n"
            "N 0 IR_MODULE -1 - \\x00 1 1 0 1 1 0 1\n"
            "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
    }
    expect_malformed_dump(dump);

    /* construct kind decodes to empty (cause record) */
    {
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 0 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "M 0\n"
            "N 0 IR_MODULE -1 - a.ai 1 1 0 1 1 0 1\n"
            "K \\x00 a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
    }
    expect_malformed_dump(dump);
}

static void test_dump_parse_nonempty_text_field_ok(void)
{
    /* regression guard: a compliant hand-crafted dump with a non-empty
     * name still parses (the empty-field rejection must not reject
     * valid input). */
    char dump[2048];
    IrBuild *r = NULL;
    char errbuf[256];
    IrDumpStatus st;
    dump_minimal_prefix(dump, sizeof(dump));
    append_module_dump(dump, sizeof(dump), "m 0 0");
    st = ir_dump_parse(dump, strlen(dump), &r, errbuf, sizeof(errbuf));
    CHECK(st == IR_DUMP_OK);
    if (st == IR_DUMP_OK) {
        CHECK(r != NULL);
        CHECK(r->nmodules == 1);
        CHECK(r->modules[0]->kind == IR_MODULE);
        CHECK(strcmp(r->modules[0]->u.module.name, "m") == 0);
        ir_build_free(r);
    } else {
        fprintf(stderr, "  parse error: %s\n", errbuf);
    }
}

/* ---------------------------------------------------------------------------
 * MIN-2 remediation (reviewer2 t_47cce3e7): tok_int/tok_uint must reject
 * numeric tokens that overflow strtoll/strtoull (errno == ERANGE) with
 * a deterministic malformed-dump error instead of silently clamping the
 * value. The canonical writer never emits an out-of-range token (all
 * emitted integers fit int64/uint64), so the rejection only affects
 * hand-crafted hostile dumps; dump determinism is unaffected.
 * ------------------------------------------------------------------------- */

static void test_dump_parse_numeric_overflow(void)
{
    char dump[2048];

    /* signed overflow: the header's ntypes token exceeds INT64_MAX
     * (tok_int through the header parse) */
    expect_malformed_dump("H 1 99999999999999999999999999 0 0\n");

    /* signed overflow: a span line number exceeds INT64_MAX (tok_int
     * through tok_span in the node header) */
    {
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 0 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "M 0\n"
            "N 0 IR_MODULE -1 - a.ai 99999999999999999999999999 1 0 1 1 0 1\n"
            "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
    }
    expect_malformed_dump(dump);

    /* unsigned overflow: a const int bit pattern exceeds UINT64_MAX
     * (tok_uint through the const table) */
    {
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 1 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "C 0 int 4 18446744073709551616\n"
            "M 0\n"
            "N 0 IR_MODULE -1 - a.ai 1 1 0 1 1 0 1\n"
            "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
    }
    expect_malformed_dump(dump);

    /* in-range control: maximal in-range tokens (INT64_MAX in a span
     * line, UINT64_MAX as a const int bit pattern) still parse */
    {
        IrBuild *r = NULL;
        char errbuf[256];
        IrDumpStatus st;
        int n = snprintf(dump, sizeof(dump),
            "H 1 13 1 1\n"
            "T 0 void 0 1\n"
            "T 1 bool 1 1\n"
            "T 2 i8 1 1\n"
            "T 3 i16 2 2\n"
            "T 4 i32 4 4\n"
            "T 5 i64 8 8\n"
            "T 6 u8 1 1\n"
            "T 7 u16 2 2\n"
            "T 8 u32 4 4\n"
            "T 9 u64 8 8\n"
            "T 10 isize 8 8\n"
            "T 11 usize 8 8\n"
            "T 12 str 16 8\n"
            "C 0 int 4 18446744073709551615\n"
            "M 0\n"
            "N 0 IR_MODULE -1 - a.ai 9223372036854775807 1 0 1 1 0 1\n"
            "K AST_MODULE_DECL a.ai 1 1 0 1 1 0 0 0 0\n"
            "P m 0 0\n");
        CHECK(n > 0 && (size_t)n < (int)sizeof(dump));
        (void)n;
        st = ir_dump_parse(dump, strlen(dump), &r, errbuf, sizeof(errbuf));
        CHECK(st == IR_DUMP_OK);
        if (st == IR_DUMP_OK) {
            CHECK(r != NULL);
            CHECK(r->nconsts == 1);
            CHECK(r->consts[0]->u.int_bits == UINT64_MAX);
            CHECK(r->modules[0]->span != NULL);
            if (r->modules[0]->span != NULL) {
                CHECK(r->modules[0]->span->start.line == INT64_MAX);
            }
            ir_build_free(r);
        } else {
            fprintf(stderr, "  parse error: %s\n", errbuf);
        }
    }
}

static void test_dump_verify(void)
{
    /* ir_dump_verify: IR_OK on a valid build; IR_VIOLATION with
     * AIC-I0501 records on a build whose dump round-trips but whose
     * reconstructed graph violates invariants. */
    IrBuild *valid = make_valid_build();
    IrBuild *bad = ir_build_new();
    IrDumpStatus st;
    DiagBuf d1;
    IrBuild *r = NULL;
    char errbuf[256];
    DiagRecord **recs = NULL;
    size_t nrecs = 0;
    IrStatus vs;
    IrNode *m, *fn, *body, *e, *es;
    IrType *i32;
    CHECK(valid != NULL);
    if (valid == NULL) {
        return;
    }
    vs = ir_dump_verify(valid, &recs, &nrecs);
    CHECK(vs == IR_OK);
    CHECK(nrecs == 0);
    if (recs != NULL) {
        ir_records_free(recs, nrecs);
    }
    ir_build_free(valid);

    /* bad build: IR_ADD with a NULL operand (invariant 11 violation) in
     * an otherwise well-formed graph. The dump still round-trips; the
     * reconstructed graph fails ir_core_verify. */
    CHECK(bad != NULL);
    if (bad == NULL) {
        return;
    }
    i32 = ir_type_i32(bad);
    m = mk_module(bad, "main", "a.ai");
    fn = mk(bad, IR_FUNCTION, "a.ai", 2);
    fn->u.function.name = strdup("f");
    fn->u.function.ret_type = ir_type_void(bad);
    body = mk_block(bad, "a.ai", 3);
    e = mk(bad, IR_ADD, "a.ai", 5);
    e->type = i32;
    e->trap_code = "AIC-R0802";
    e->u.binary.left = mk_int(bad, "a.ai", i32, 1);
    e->u.binary.right = NULL;
    es = mk(bad, IR_EXPR_STMT, "a.ai", 5);
    es->u.expr_stmt.expr = e;
    ir_block_add_stmt(bad, body, es);
    fn->u.function.body = body;
    ir_module_add_decl(bad, m, fn);
    ir_build_add_module(bad, m);

    /* confirm the dump itself round-trips byte-identically */
    dump_to_buf(bad, &d1);
    st = ir_dump_parse(d1.data, d1.len, &r, errbuf, sizeof(errbuf));
    CHECK(st == IR_DUMP_OK);
    if (st == IR_DUMP_OK) {
        DiagBuf d2;
        dump_to_buf(r, &d2);
        CHECK(buf_equal(&d1, &d2));
        diag_buf_free(&d2);
    }
    diag_buf_free(&d1);
    if (r != NULL) {
        ir_build_free(r);
    }

    vs = ir_dump_verify(bad, &recs, &nrecs);
    CHECK(vs == IR_VIOLATION);
    CHECK(nrecs >= 1);
    if (nrecs >= 1) {
        CHECK(strcmp(recs[0]->code, "AIC-I0501") == 0);
        CHECK(strcmp(recs[0]->phase, DIAG_PHASE_IR) == 0);
        CHECK(strcmp(recs[0]->severity, DIAG_SEVERITY_ERROR) == 0);
        CHECK(recs[0]->recovery != NULL &&
              strcmp(recs[0]->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    }
    if (recs != NULL) {
        ir_records_free(recs, nrecs);
    }
    ir_build_free(bad);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_dump_determinism();
    fprintf(stderr, "after test_dump_determinism\n");
    test_dump_distinct();
    fprintf(stderr, "after test_dump_distinct\n");
    test_dump_roundtrip_valid();
    fprintf(stderr, "after test_dump_roundtrip_valid\n");
    test_dump_all_kinds_roundtrip();
    fprintf(stderr, "after test_dump_all_kinds_roundtrip\n");
    test_dump_span_cause_preservation();
    fprintf(stderr, "after test_dump_span_cause_preservation\n");
    test_dump_parse_malformed();
    fprintf(stderr, "after test_dump_parse_malformed\n");
    test_dump_parse_malformed_str_const();
    fprintf(stderr, "after test_dump_parse_malformed_str_const\n");
    test_dump_parse_empty_field_collapse();
    fprintf(stderr, "after test_dump_parse_empty_field_collapse\n");
    test_dump_parse_empty_decoded_string();
    fprintf(stderr, "after test_dump_parse_empty_decoded_string\n");
    test_dump_parse_nonempty_text_field_ok();
    fprintf(stderr, "after test_dump_parse_nonempty_text_field_ok\n");
    test_dump_parse_numeric_overflow();
    fprintf(stderr, "after test_dump_parse_numeric_overflow\n");
    test_dump_verify();
    fprintf(stderr, "after test_dump_verify\n");

    if (g_failures) {
        fprintf(stderr, "ir_dump_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("ir_dump_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
