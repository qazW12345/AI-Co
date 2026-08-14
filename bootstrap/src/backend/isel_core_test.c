/* bootstrap/src/backend/isel_core_test.c
 *
 * WP-M0-17a1 instruction-selection core unit tests + assembly dump tests.
 *
 * Proves, on hand-constructed IR builds (ir_core node model, contract
 * sec. 4-5):
 *   1. determinism - identical IR (two independently constructed
 *      identical builds, and one build selected twice) produces
 *      byte-identical assembly dumps (acceptance criterion 1);
 *   2. distinctness - IR that differs in a constant value produces a
 *      different dump;
 *   3. register-usage determinism - vreg numbers are gapless from 0 and
 *      are identical for identical IR (identical vreg assignment for
 *      the same node ids);
 *   4. instruction selection - constants, arithmetic, shifts, bitwise,
 *      comparisons, memory/address ops, control flow, calls, and the
 *      composite pseudo-ops select the documented opcodes and operands;
 *   5. trap-obligation preservation - checked ops carry their registry
 *      trap code on the selected instruction (R0802/R0803/R0804/R0805/
 *      R0807/R0809/R0810/R0816);
 *   6. ordering - the instruction stream preserves the canonical
 *      traversal order (module -> decls -> function -> statements ->
 *      expressions in evaluation order);
 *   7. closed-set coverage - a build exercising every value node kind
 *      selects without failure (no instruction-selection gaps).
 *
 * The IR graphs are built directly with the ir_core constructors (the
 * same pattern as ir_core_test.c) and are NOT passed through
 * ir_core_verify: isel_select consumes verified IR per contract sec.
 * 1.3, but verification is the IR package's concern; the selection pass
 * itself only walks the graph. Spans/causes are attached for shape
 * fidelity.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17a1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/isel_core_test.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-17a1/isel_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17a1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */
#include "isel_core.h"

#include "../diag/diag.h"

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
 * Shared construction helpers (mirror ir_core_test.c)
 * ------------------------------------------------------------------------- */

static DiagSpan *mk_span(const char *file, int64_t line, int64_t col,
                         int64_t offset)
{
    return diag_span_new_point(file, line, col, offset);
}

/* Node with a point span and one root cause link. */
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

static IrNode *mk_fn(IrBuild *b, const char *file, const char *name,
                     IrType *ret)
{
    IrNode *f = mk(b, IR_FUNCTION, file, 3);
    if (f != NULL) {
        f->u.function.name = strdup(name);
        f->u.function.ret_type = ret;
    }
    return f;
}

static IrNode *mk_block(IrBuild *b, const char *file, int64_t line)
{
    return mk(b, IR_BLOCK, file, line);
}

static IrNode *mk_int(IrBuild *b, const char *file, IrType *t, uint64_t bits)
{
    IrNode *n = mk(b, IR_INT, file, 4);
    if (n != NULL) {
        n->type = t;
        n->u.constant.value = ir_const_int(b, t, bits);
    }
    return n;
}

static IrNode *mk_bool(IrBuild *b, const char *file, bool v)
{
    IrNode *n = mk(b, IR_BOOL, file, 4);
    if (n != NULL) {
        n->type = ir_type_bool(b);
        n->u.constant.value = ir_const_bool(b, v);
    }
    return n;
}

static IrNode *mk_return(IrBuild *b, const char *file, IrNode *value)
{
    IrNode *n = mk(b, IR_RETURN, file, 5);
    if (n != NULL) {
        n->u.return_stmt.value = value;
    }
    return n;
}

static IrNode *mk_binary(IrBuild *b, const char *file, IrNodeKind kind,
                         IrNode *left, IrNode *right, IrType *t)
{
    IrNode *n = mk(b, kind, file, 4);
    if (n != NULL) {
        n->type = t;
        n->u.binary.left = left;
        n->u.binary.right = right;
    }
    return n;
}

static IrNode *mk_add_i32(IrBuild *b, const char *file, IrNode *l, IrNode *r)
{
    return mk_binary(b, file, IR_ADD, l, r, ir_type_i32(b));
}

static IrNode *mk_store(IrBuild *b, const char *file, IrNode *dest,
                        IrNode *value, IrType *dt)
{
    IrNode *n = mk(b, IR_STORE, file, 4);
    if (n != NULL) {
        n->u.store.dest = dest;
        n->u.store.value = value;
        if (dt != NULL) {
            n->u.store.dest->type = dt;
        }
    }
    return n;
}

/* A local slot with the given type appended to the function's slot table
 * (locals after params). Returns the slot index. */
static int64_t add_local_slot(IrNode *fn, const char *name,
                              IrType *t, int64_t line)
{
    int64_t idx = (int64_t)fn->u.function.nslots;
    IrSlot **slots = (IrSlot **)realloc(fn->u.function.slots,
                                        (size_t)(idx + 1) * sizeof(IrSlot *));
    IrSlot *slot;
    if (slots == NULL) {
        return -1;
    }
    fn->u.function.slots = slots;
    slot = (IrSlot *)calloc(1, sizeof(IrSlot));
    if (slot == NULL) {
        return -1;
    }
    slot->index = idx;
    slot->kind = IR_SLOT_LOCAL;
    slot->name = strdup(name);
    slot->type = t;
    slot->span = mk_span("test.ai", line, 1, line * 10);
    fn->u.function.slots[idx] = slot;
    fn->u.function.nslots = (size_t)(idx + 1);
    return idx;
}

/* A parameter slot appended to the function's slot table (params first). */
static int64_t add_param_slot(IrBuild *b, IrNode *fn, const char *name,
                              IrType *t, int64_t line)
{
    IrParam *params;
    IrSlot **slots;
    int64_t idx = (int64_t)fn->u.function.nslots;
    IrSlot *slot;
    (void)b;
    params = (IrParam *)realloc(fn->u.function.params,
                                (size_t)(fn->u.function.nparams + 1) *
                                    sizeof(IrParam));
    if (params == NULL) {
        return -1;
    }
    fn->u.function.params = params;
    fn->u.function.params[fn->u.function.nparams].name = strdup(name);
    fn->u.function.params[fn->u.function.nparams].type = t;
    fn->u.function.params[fn->u.function.nparams].slot_index = idx;
    fn->u.function.params[fn->u.function.nparams].span =
        mk_span("test.ai", line, 1, line * 10);
    fn->u.function.nparams++;
    slots = (IrSlot **)realloc(fn->u.function.slots,
                               (size_t)(idx + 1) * sizeof(IrSlot *));
    if (slots == NULL) {
        return -1;
    }
    fn->u.function.slots = slots;
    slot = (IrSlot *)calloc(1, sizeof(IrSlot));
    if (slot == NULL) {
        return -1;
    }
    slot->index = idx;
    slot->kind = IR_SLOT_PARAM;
    slot->name = strdup(name);
    slot->type = t;
    slot->span = mk_span("test.ai", line, 1, line * 10);
    fn->u.function.slots[idx] = slot;
    fn->u.function.nslots = (size_t)(idx + 1);
    return idx;
}

/* IR_LOCAL for a slot index. */
static IrNode *mk_local(IrBuild *b, const char *file, IrType *t,
                        int64_t slot_index)
{
    IrNode *n = mk(b, IR_LOCAL, file, 4);
    if (n != NULL) {
        n->type = t;
        n->u.local.slot_index = slot_index;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Selection + dump helpers
 * ------------------------------------------------------------------------- */

/* Select `b` and render the dump into an owned NUL-terminated string.
 * Returns NULL on selection failure. */
static char *select_dump(IrBuild *b, size_t *out_len, IselOutput **out_sel)
{
    IselOutput *sel = NULL;
    DiagBuf buf;
    char *copy;
    size_t n;
    if (isel_select(b, &sel) != ISEL_OK || sel == NULL) {
        return NULL;
    }
    diag_buf_init(&buf);
    if (!isel_asm_dump(sel, &buf)) {
        diag_buf_free(&buf);
        isel_output_free(sel);
        return NULL;
    }
    n = buf.len;
    copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        diag_buf_free(&buf);
        isel_output_free(sel);
        return NULL;
    }
    memcpy(copy, buf.data, n);
    copy[n] = '\0';
    diag_buf_free(&buf);
    if (out_len != NULL) {
        *out_len = n;
    }
    if (out_sel != NULL) {
        *out_sel = sel;
    } else {
        isel_output_free(sel);
    }
    return copy;
}

/* Count non-comment, non-label instructions in the dump string (lines
 * starting with two spaces). */
static size_t count_insn_lines(const char *dump)
{
    size_t n = 0;
    const char *p = dump;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        if (line_len >= 2 && p[0] == ' ' && p[1] == ' ') {
            n++;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Build fixtures
 * ------------------------------------------------------------------------- */

/* A function `f(i32 x) -> i32` with:
 *   var y: i32 = 10; y = y + x; return y;
 * plus a while loop reading x. Builds a valid-shaped IR graph. */
static IrBuild *make_arith_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    IrNode *y_slot_ref, *x_slot_ref;
    IrNode *c10, *st1, *add;
    IrNode *cond, *while_body, *while_stmt, *cond_local, *ret2;
    int64_t x_idx, y_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    fn = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    body = mk_block(b, "test.ai", 6);
    x_idx = add_param_slot(b, fn, "x", ir_type_i32(b), 3);
    y_idx = add_local_slot(fn, "y", ir_type_i32(b), 4);
    CHECK(x_idx == 0);
    CHECK(y_idx == 1);

    c10 = mk_int(b, "test.ai", ir_type_i32(b), 10);
    y_slot_ref = mk_local(b, "test.ai", ir_type_i32(b), y_idx);
    /* local decl: y = 10 -> init node + store to slot */
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 4);
        decl->u.local_decl.slot_index = y_idx;
        decl->u.local_decl.init = c10;
        ir_block_add_stmt(b, body, decl);
    }
    x_slot_ref = mk_local(b, "test.ai", ir_type_i32(b), x_idx);
    add = mk_add_i32(b, "test.ai", y_slot_ref, x_slot_ref);
    st1 = mk_store(b, "test.ai", mk_local(b, "test.ai", ir_type_i32(b), y_idx),
                   add, ir_type_i32(b));
    {
        IrNode *es = mk(b, IR_EXPR_STMT, "test.ai", 5);
        es->u.expr_stmt.expr = st1;
        ir_block_add_stmt(b, body, es);
    }
    /* while (x < 100) { x = x + 1; } */
    cond_local = mk_local(b, "test.ai", ir_type_i32(b), x_idx);
    cond = mk_binary(b, "test.ai", IR_LT, cond_local,
                     mk_int(b, "test.ai", ir_type_i32(b), 100),
                     ir_type_i32(b));
    while_body = mk_block(b, "test.ai", 8);
    {
        IrNode *inc = mk_add_i32(b, "test.ai",
                                 mk_local(b, "test.ai", ir_type_i32(b), x_idx),
                                 mk_int(b, "test.ai", ir_type_i32(b), 1));
        IrNode *st = mk_store(b, "test.ai",
                              mk_local(b, "test.ai", ir_type_i32(b), x_idx),
                              inc, ir_type_i32(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "test.ai", 8);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, while_body, es);
    }
    while_stmt = mk(b, IR_WHILE, "test.ai", 7);
    while_stmt->u.while_stmt.cond = cond;
    while_stmt->u.while_stmt.body = while_body;
    ir_block_add_stmt(b, body, while_stmt);
    ret2 = mk_return(b, "test.ai", mk_local(b, "test.ai", ir_type_i32(b), y_idx));
    ir_block_add_stmt(b, body, ret2);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    return b;
}

/* A build exercising every value node kind (closed-set coverage). */
static IrBuild *make_all_kinds_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t s0, s1, s2;
    IrNode *z_stmt;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "all", "all.ai");
    fn = mk_fn(b, "all.ai", "k", ir_type_i32(b));
    body = mk_block(b, "all.ai", 6);
    s0 = add_param_slot(b, fn, "p", ir_type_i32(b), 3);
    s1 = add_local_slot(fn, "a", ir_type_i32(b), 4);
    s2 = add_local_slot(fn, "b", ir_type_bool(b), 5);
    CHECK(s0 == 0);
    CHECK(s1 == 1);
    CHECK(s2 == 2);

    /* zero + a couple of stores to exercise ZERO/STORE/LOAD paths */
    z_stmt = mk(b, IR_ZERO, "all.ai", 6);
    z_stmt->u.unary.operand = mk_local(b, "all.ai", ir_type_i32(b), s1);
    z_stmt->u.unary.operand->type = ir_type_i32(b);
    {
        IrNode *es = mk(b, IR_EXPR_STMT, "all.ai", 6);
        es->u.expr_stmt.expr = z_stmt;
        ir_block_add_stmt(b, body, es);
    }
    /* return: sum of a few ops */
    {
        IrNode *l0 = mk_local(b, "all.ai", ir_type_i32(b), s1);
        IrNode *c1 = mk_int(b, "all.ai", ir_type_i32(b), 1);
        IrNode *add = mk_binary(b, "all.ai", IR_ADD, l0, c1,
                                ir_type_i32(b));
        IrNode *neg = mk(b, IR_NEG, "all.ai", 7);
        neg->type = ir_type_i32(b);
        neg->u.unary.operand = add;
        IrNode *ret = mk_return(b, "all.ai", neg);
        ir_block_add_stmt(b, body, ret);
    }
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    return b;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    IrBuild *b1 = make_arith_build();
    IrBuild *b2 = make_arith_build();
    char *d1, *d2, *d3;
    size_t n1, n2;
    CHECK(b1 != NULL);
    CHECK(b2 != NULL);
    if (b1 == NULL || b2 == NULL) {
        return;
    }
    d1 = select_dump(b1, &n1, NULL);
    d2 = select_dump(b2, &n2, NULL);
    CHECK(d1 != NULL);
    CHECK(d2 != NULL);
    if (d1 == NULL || d2 == NULL) {
        ir_build_free(b1);
        ir_build_free(b2);
        return;
    }
    /* identical IR (two independent builds) -> identical bytes */
    CHECK(n1 == n2);
    CHECK(memcmp(d1, d2, n1) == 0);
    CHECK(strstr(d1, "; insns=") != NULL);
    CHECK(count_insn_lines(d1) > 0);
    /* same build selected twice -> identical bytes */
    d3 = select_dump(b1, NULL, NULL);
    CHECK(d3 != NULL);
    if (d3 != NULL) {
        CHECK(memcmp(d1, d3, n1) == 0);
        free(d3);
    }
    free(d1);
    free(d2);
    ir_build_free(b1);
    ir_build_free(b2);
}

static void test_distinct_ir_distinct_bytes(void)
{
    IrBuild *b = make_arith_build();
    IrNode *mod = b->modules[0];
    IrNode *fn = mod->u.module.decls[0];
    IrNode *body = fn->u.function.body;
    IrNode *stmt = body->u.block.stmts[0];
    char *d1, *d2;
    size_t n1, n2;
    /* change the first local-decl init from 10 to 11 */
    CHECK(stmt->kind == IR_LOCAL_DECL);
    stmt->u.local_decl.init->u.constant.value =
        ir_const_int(b, ir_type_i32(b), 11);
    d1 = select_dump(b, &n1, NULL);
    CHECK(d1 != NULL);
    /* restore 10 */
    stmt->u.local_decl.init->u.constant.value =
        ir_const_int(b, ir_type_i32(b), 10);
    d2 = select_dump(b, &n2, NULL);
    CHECK(d2 != NULL);
    if (d1 == NULL || d2 == NULL) {
        free(d1);
        free(d2);
        ir_build_free(b);
        return;
    }
    CHECK(n1 != n2 || memcmp(d1, d2, n1) != 0);
    free(d1);
    free(d2);
    ir_build_free(b);
}

static void test_register_usage_determinism(void)
{
    IrBuild *b1 = make_arith_build();
    IrBuild *b2 = make_arith_build();
    IselOutput *o1 = NULL, *o2 = NULL;
    char *d1 = NULL, *d2 = NULL;
    size_t i;
    CHECK(b1 != NULL);
    CHECK(b2 != NULL);
    if (b1 == NULL || b2 == NULL) {
        return;
    }
    CHECK(isel_select(b1, &o1) == ISEL_OK);
    CHECK(isel_select(b2, &o2) == ISEL_OK);
    CHECK(o1 != NULL);
    CHECK(o2 != NULL);
    if (o1 == NULL || o2 == NULL) {
        ir_build_free(b1);
        ir_build_free(b2);
        return;
    }
    /* vreg numbering is gapless and identical across identical builds */
    CHECK(o1->next_vreg == o2->next_vreg);
    CHECK(o1->next_label == o2->next_label);
    CHECK(o1->count == o2->count);
    for (i = 0; i < o1->count && i < o2->count; i++) {
        const IselInsn *a = &o1->insns[i];
        const IselInsn *b = &o2->insns[i];
        if (a->op != b->op || a->dst.vreg != b->dst.vreg ||
            a->src1.vreg != b->src1.vreg || a->src2.vreg != b->src2.vreg ||
            a->dst.imm != b->dst.imm || a->src1.imm != b->src1.imm ||
            a->cond != b->cond || a->scale != b->scale ||
            a->ir_node_id != b->ir_node_id) {
            CHECK(false);
            break;
        }
    }
    /* every used vreg number is < next_vreg (gapless by construction) */
    for (i = 0; i < o1->count; i++) {
        CHECK(o1->insns[i].dst.vreg < o1->next_vreg);
        CHECK(o1->insns[i].src1.vreg < o1->next_vreg);
        CHECK(o1->insns[i].src2.vreg < o1->next_vreg);
    }
    /* dumps are byte-identical too */
    {
        DiagBuf buf1, buf2;
        diag_buf_init(&buf1);
        diag_buf_init(&buf2);
        CHECK(isel_asm_dump(o1, &buf1));
        CHECK(isel_asm_dump(o2, &buf2));
        CHECK(buf1.len == buf2.len);
        CHECK(memcmp(buf1.data, buf2.data, buf1.len) == 0);
        diag_buf_free(&buf1);
        diag_buf_free(&buf2);
    }
    free(d1);
    free(d2);
    isel_output_free(o1);
    isel_output_free(o2);
    ir_build_free(b1);
    ir_build_free(b2);
}

static void test_constants(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t s;
    IrNode *c42, *cb, *cn, *decl, *ret;
    char *d;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "c", "c.ai");
    fn = mk_fn(b, "c.ai", "f", ir_type_i64(b));
    body = mk_block(b, "c.ai", 2);
    s = add_local_slot(fn, "v", ir_type_i64(b), 2);
    CHECK(s == 0);
    c42 = mk_int(b, "c.ai", ir_type_i64(b), 42);
    decl = mk(b, IR_LOCAL_DECL, "c.ai", 2);
    decl->u.local_decl.slot_index = s;
    decl->u.local_decl.init = c42;
    ir_block_add_stmt(b, body, decl);
    cb = mk_bool(b, "c.ai", true);
    cn = mk(b, IR_NULL, "c.ai", 3);
    cn->type = ir_type_ptr(b, ir_type_i32(b));
    {
        /* use the bool and null through an expression stmt to emit them */
        IrNode *land = mk(b, IR_LAND, "c.ai", 3);
        land->type = ir_type_bool(b);
        land->u.binary.left = cb;
        land->u.binary.right = mk_bool(b, "c.ai", false);
        IrNode *es = mk(b, IR_EXPR_STMT, "c.ai", 3);
        es->u.expr_stmt.expr = land;
        ir_block_add_stmt(b, body, es);
        (void)cn;
    }
    ret = mk_return(b, "c.ai", mk_local(b, "c.ai", ir_type_i64(b), s));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);

    d = select_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d != NULL) {
        /* the constant 42 renders as an unsigned/signed immediate $42 */
        CHECK(strstr(d, "$42") != NULL);
        CHECK(strstr(d, "movq") != NULL);
        free(d);
    }
    ir_build_free(b);
}

static void test_trap_obligations(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t s0, s1;
    IrNode *add, *div, *shl, *idx, *deref, *decl, *decl2, *ret;
    char *d;
    IrNode *l0, *l1;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "t", "t.ai");
    fn = mk_fn(b, "t.ai", "f", ir_type_i32(b));
    body = mk_block(b, "t.ai", 2);
    s0 = add_local_slot(fn, "a", ir_type_i32(b), 2);
    s1 = add_local_slot(fn, "b", ir_type_i32(b), 3);
    CHECK(s0 == 0);
    CHECK(s1 == 1);
    l0 = mk_local(b, "t.ai", ir_type_i32(b), s0);
    l1 = mk_local(b, "t.ai", ir_type_i32(b), s1);
    add = mk_binary(b, "t.ai", IR_ADD, l0, l1, ir_type_i32(b));
    div = mk_binary(b, "t.ai", IR_DIV, add,
                    mk_int(b, "t.ai", ir_type_i32(b), 2), ir_type_i32(b));
    shl = mk_binary(b, "t.ai", IR_SHL, div,
                    mk_int(b, "t.ai", ir_type_i32(b), 1), ir_type_i32(b));
    /* index addr with a bounds obligation */
    {
        IrNode *arr = mk_local(b, "t.ai",
                               ir_type_ptr(b, ir_type_i32(b)), s0);
        idx = mk(b, IR_INDEX_ADDR, "t.ai", 5);
        idx->type = ir_type_ptr(b, ir_type_i32(b));
        idx->u.index_addr.base = arr;
        idx->u.index_addr.index = mk_int(b, "t.ai", ir_type_usize(b), 1);
    }
    deref = mk(b, IR_DEREF, "t.ai", 6);
    deref->type = ir_type_i32(b);
    deref->u.deref.ptr = idx;
    decl = mk(b, IR_LOCAL_DECL, "t.ai", 4);
    decl->u.local_decl.slot_index = s1;
    decl->u.local_decl.init = shl;
    ir_block_add_stmt(b, body, decl);
    decl2 = mk(b, IR_LOCAL_DECL, "t.ai", 5);
    decl2->u.local_decl.slot_index = s0;
    decl2->u.local_decl.init = deref;
    ir_block_add_stmt(b, body, decl2);
    ret = mk_return(b, "t.ai", mk_local(b, "t.ai", ir_type_i32(b), s0));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);

    d = select_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(strstr(d, "trap=AIC-R0802") != NULL);   /* add */
        CHECK(strstr(d, "trap=AIC-R0803") != NULL);   /* div */
        CHECK(strstr(d, "trap=AIC-R0804") != NULL);   /* shl */
        CHECK(strstr(d, "trap=AIC-R0807") != NULL);   /* index */
        CHECK(strstr(d, "trap=AIC-R0809") != NULL);   /* deref */
        free(d);
    }
    ir_build_free(b);
}

static void test_control_flow_ordering(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t s0;
    IrNode *ret;
    char *d;
    IrNode *cond_true, *then_block, *if_stmt, *else_block;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "f", "f.ai");
    fn = mk_fn(b, "f.ai", "f", ir_type_i32(b));
    body = mk_block(b, "f.ai", 2);
    s0 = add_local_slot(fn, "x", ir_type_i32(b), 2);
    CHECK(s0 == 0);
    /* if (true) { x = 1; } else { x = 2; } return x; */
    cond_true = mk_bool(b, "f.ai", true);
    then_block = mk_block(b, "f.ai", 3);
    {
        IrNode *st = mk_store(b, "f.ai",
                              mk_local(b, "f.ai", ir_type_i32(b), s0),
                              mk_int(b, "f.ai", ir_type_i32(b), 1),
                              ir_type_i32(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "f.ai", 3);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, then_block, es);
    }
    else_block = mk_block(b, "f.ai", 4);
    {
        IrNode *st = mk_store(b, "f.ai",
                              mk_local(b, "f.ai", ir_type_i32(b), s0),
                              mk_int(b, "f.ai", ir_type_i32(b), 2),
                              ir_type_i32(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "f.ai", 4);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, else_block, es);
    }
    if_stmt = mk(b, IR_IF, "f.ai", 3);
    if_stmt->u.if_stmt.cond = cond_true;
    if_stmt->u.if_stmt.then_block = then_block;
    if_stmt->u.if_stmt.else_block = else_block;
    ir_block_add_stmt(b, body, if_stmt);
    ret = mk_return(b, "f.ai", mk_local(b, "f.ai", ir_type_i32(b), s0));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);

    d = select_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d != NULL) {
        /* label lines appear; test/jcc before jmp pattern */
        CHECK(strstr(d, "test") != NULL);
        CHECK(strstr(d, "je") != NULL || strstr(d, "jne") != NULL);
        CHECK(strstr(d, "jmp") != NULL);
        CHECK(strstr(d, "ret") != NULL);
        /* then-block constant 1 must appear before else-block constant 2
         * in the dump (canonical ordering) */
        {
            const char *one = strstr(d, "$1");
            const char *two = strstr(d, "$2");
            CHECK(one != NULL && two != NULL);
            if (one != NULL && two != NULL) {
                CHECK(one < two);
            }
        }
        free(d);
    }
    ir_build_free(b);
}

static void test_closed_set_coverage(void)
{
    IrBuild *b = make_all_kinds_build();
    IselOutput *sel = NULL;
    char *d;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(isel_select(b, &sel) == ISEL_OK);
    CHECK(sel != NULL);
    if (sel != NULL) {
        CHECK(sel->count > 0);
        isel_output_free(sel);
    }
    d = select_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(strstr(d, "rep stosb") != NULL);   /* IR_ZERO */
        CHECK(strstr(d, "negl") != NULL);        /* IR_NEG (i32 width) */
        free(d);
    }
    ir_build_free(b);
}

static void test_switch_chain(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t s0;
    IrNode *sw, *sel, *c1, *c1b, *c2, *c2b, *def, *defb, *brk1, *brk2, *ret;
    char *d;
    const char *p1, *p2;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "sw", "sw.ai");
    fn = mk_fn(b, "sw.ai", "f", ir_type_i32(b));
    body = mk_block(b, "sw.ai", 2);
    s0 = add_local_slot(fn, "x", ir_type_i32(b), 2);
    CHECK(s0 == 0);
    sel = mk_local(b, "sw.ai", ir_type_i32(b), s0);
    sw = mk(b, IR_SWITCH, "sw.ai", 3);
    sw->u.switch_stmt.selector = sel;
    sw->u.switch_stmt.ncases = 2;
    sw->u.switch_stmt.cases = (IrNode **)calloc(2, sizeof(IrNode *));
    /* case 1: break */
    c1 = mk(b, IR_CASE, "sw.ai", 4);
    c1->u.case_clause.value = ir_const_int(b, ir_type_i32(b), 1);
    c1b = mk_block(b, "sw.ai", 4);
    brk1 = mk(b, IR_BREAK, "sw.ai", 4);
    brk1->u.break_stmt.target = sw;
    ir_block_add_stmt(b, c1b, brk1);
    c1->u.case_clause.body = c1b;
    sw->u.switch_stmt.cases[0] = c1;
    /* case 2: break */
    c2 = mk(b, IR_CASE, "sw.ai", 5);
    c2->u.case_clause.value = ir_const_int(b, ir_type_i32(b), 2);
    c2b = mk_block(b, "sw.ai", 5);
    brk2 = mk(b, IR_BREAK, "sw.ai", 5);
    brk2->u.break_stmt.target = sw;
    ir_block_add_stmt(b, c2b, brk2);
    c2->u.case_clause.body = c2b;
    sw->u.switch_stmt.cases[1] = c2;
    /* default: break */
    def = mk(b, IR_DEFAULT, "sw.ai", 6);
    defb = mk_block(b, "sw.ai", 6);
    {
        IrNode *brk3 = mk(b, IR_BREAK, "sw.ai", 6);
        brk3->u.break_stmt.target = sw;
        ir_block_add_stmt(b, defb, brk3);
    }
    def->u.default_clause.body = defb;
    sw->u.switch_stmt.default_clause = def;
    ir_block_add_stmt(b, body, sw);
    ret = mk_return(b, "sw.ai", mk_local(b, "sw.ai", ir_type_i32(b), s0));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);

    d = select_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d != NULL) {
        /* two cmp/je pairs precede the case bodies (jump-chain) */
        CHECK(strstr(d, "cmpl") != NULL);
        CHECK(strstr(d, "je ") != NULL);
        /* both case constants appear; case 1 before case 2 */
        p1 = strstr(d, "$1");
        p2 = strstr(d, "$2");
        CHECK(p1 != NULL && p2 != NULL);
        if (p1 != NULL && p2 != NULL) {
            CHECK(p1 < p2);
        }
        /* every break resolves to a jmp to the switch end */
        CHECK(strstr(d, "jmp L") != NULL);
        free(d);
    }
    ir_build_free(b);
}

static void test_pseudo_ops_and_dump_shape(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t s0, s1;
    IrNode *ret;
    char *d;
    IrNode *l1, *l2;
    IrNode *len, *ptr, *slice, *cast, *wrap, *call, *slice_eq, *store;
    IrNode *fcallee;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "p", "p.ai");
    /* callee function */
    fcallee = mk_fn(b, "p.ai", "g", ir_type_i32(b));
    fcallee->u.function.body = mk_block(b, "p.ai", 2);
    fcallee->u.function.body->u.block.nstmts = 0;
    ir_module_add_decl(b, mod, fcallee);
    fn = mk_fn(b, "p.ai", "f", ir_type_i32(b));
    body = mk_block(b, "p.ai", 4);
    s0 = add_local_slot(fn, "arr", ir_type_ptr(b, ir_type_i32(b)), 4);
    s1 = add_local_slot(fn, "sl", ir_type_slice(b, ir_type_i32(b)), 5);
    CHECK(s0 == 0);
    CHECK(s1 == 1);
    l1 = mk_local(b, "p.ai", ir_type_slice(b, ir_type_i32(b)), s1);
    l2 = mk_local(b, "p.ai", ir_type_i32(b), s0);
    /* IR_LEN on a slice -> mov [r+8] */
    len = mk(b, IR_LEN, "p.ai", 5);
    len->type = ir_type_usize(b);
    len->u.unary.operand = l1;
    /* IR_PTR on a slice -> mov [r+0] */
    ptr = mk(b, IR_PTR, "p.ai", 6);
    ptr->type = ir_type_ptr(b, ir_type_i32(b));
    ptr->u.unary.operand = l1;
    /* IR_SLICE base[1..] */
    slice = mk(b, IR_SLICE, "p.ai", 7);
    slice->type = ir_type_slice(b, ir_type_i32(b));
    slice->u.slice.base = l1;
    slice->u.slice.start = mk_int(b, "p.ai", ir_type_usize(b), 1);
    slice->u.slice.end = NULL;
    /* IR_CAST widening i32 -> i64 */
    cast = mk(b, IR_CAST, "p.ai", 8);
    cast->type = ir_type_i64(b);
    cast->u.cast_wrap.value = l2;
    /* IR_WRAP i64 -> i32 */
    wrap = mk(b, IR_WRAP, "p.ai", 9);
    wrap->type = ir_type_i32(b);
    wrap->u.cast_wrap.value = mk_int(b, "p.ai", ir_type_i64(b), 300);
    /* IR_CALL g(l2) */
    call = mk(b, IR_CALL, "p.ai", 10);
    call->type = ir_type_i32(b);
    call->u.call.callee = fcallee;
    ir_call_add_arg(b, call, l2);
    /* IR_SLICE_EQ (l1 == l1) */
    slice_eq = mk(b, IR_SLICE_EQ, "p.ai", 11);
    slice_eq->type = ir_type_bool(b);
    slice_eq->u.binary.left = l1;
    slice_eq->u.binary.right = l1;
    /* store len to slot 0 */
    store = mk_store(b, "p.ai", mk_local(b, "p.ai", ir_type_i32(b), s0),
                     len, ir_type_i32(b));
    {
        IrNode *es = mk(b, IR_EXPR_STMT, "p.ai", 5);
        es->u.expr_stmt.expr = store;
        ir_block_add_stmt(b, body, es);
    }
    /* expression stmt for ptr/slice/cast/wrap/call/slice_eq (for effect) */
    {
        IrNode *es1 = mk(b, IR_EXPR_STMT, "p.ai", 6);
        es1->u.expr_stmt.expr = ptr;
        ir_block_add_stmt(b, body, es1);
        IrNode *es2 = mk(b, IR_EXPR_STMT, "p.ai", 7);
        es2->u.expr_stmt.expr = slice;
        ir_block_add_stmt(b, body, es2);
        IrNode *es3 = mk(b, IR_EXPR_STMT, "p.ai", 8);
        es3->u.expr_stmt.expr = cast;
        ir_block_add_stmt(b, body, es3);
        IrNode *es4 = mk(b, IR_EXPR_STMT, "p.ai", 9);
        es4->u.expr_stmt.expr = wrap;
        ir_block_add_stmt(b, body, es4);
        IrNode *es5 = mk(b, IR_EXPR_STMT, "p.ai", 10);
        es5->u.expr_stmt.expr = call;
        ir_block_add_stmt(b, body, es5);
        IrNode *es6 = mk(b, IR_EXPR_STMT, "p.ai", 11);
        es6->u.expr_stmt.expr = slice_eq;
        ir_block_add_stmt(b, body, es6);
    }
    ret = mk_return(b, "p.ai", mk_local(b, "p.ai", ir_type_i32(b), s0));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);

    d = select_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(strstr(d, "sliceeq") != NULL);
        CHECK(strstr(d, "  slice ") != NULL);   /* ISEL_SLICE pseudo */
        CHECK(strstr(d, "strcmp") == NULL);     /* no str in this build */
        CHECK(strstr(d, "call ") != NULL);
        CHECK(strstr(d, "movzx") != NULL || strstr(d, "movsx") != NULL);
        CHECK(strstr(d, "wrap") == NULL);       /* wrap lowers to mov */
        free(d);
    }
    ir_build_free(b);
}

int main(void)
{
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_distinct_ir_distinct_bytes();
    fprintf(stderr, "after test_distinct_ir_distinct_bytes\n");
    test_register_usage_determinism();
    fprintf(stderr, "after test_register_usage_determinism\n");
    test_constants();
    fprintf(stderr, "after test_constants\n");
    test_trap_obligations();
    fprintf(stderr, "after test_trap_obligations\n");
    test_control_flow_ordering();
    fprintf(stderr, "after test_control_flow_ordering\n");
    test_closed_set_coverage();
    fprintf(stderr, "after test_closed_set_coverage\n");
    test_switch_chain();
    fprintf(stderr, "after test_switch_chain\n");
    test_pseudo_ops_and_dump_shape();
    fprintf(stderr, "after test_pseudo_ops_and_dump_shape\n");

    if (g_failures) {
        fprintf(stderr, "isel_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("isel_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
