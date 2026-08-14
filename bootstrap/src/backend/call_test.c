/* bootstrap/src/backend/call_test.c
 *
 * WP-M0-17b2 register allocation and call emission unit tests.
 *
 * Proves, on hand-constructed IR builds (ir_core node model) selected by
 * isel_core, framed by frame_build, and register-allocated/emitted by
 * call_build:
 *   1. scalar call emission - args 0-3 move (64-bit) into RCX/RDX/R8/R9,
 *      the call targets the callee symbol, and the 32-byte shadow space
 *      is reserved in the prologue (acceptance criterion 1);
 *   2. stack arguments - args 4+ store into [rsp+32], [rsp+40], ... and
 *      the prologue reserves the stack-argument area with the total
 *      16-byte aligned (acceptance criterion 1: 16-byte alignment);
 *   3. return value - the call result is captured from RAX into the
 *      result vreg's slot, and a function's return value is placed in
 *      RAX before the epilogue (acceptance criterion 1: RAX return);
 *   4. void and noreturn calls - a void call captures no result; a
 *      noreturn call terminator (rt.proc.exit) emits the argument setup
 *      and the call with no epilogue/ret after it;
 *   5. composite parameters - a slice argument is passed address-resident
 *      (its address in RCX) and the callee copies it into its parameter
 *      slot by value (REP MOVSB), saving/restoring the callee-saved RDI/
 *      RSI it clobbers (sec. 15.7 callee-saved contract);
 *   6. register allocation - every vreg gets a deterministic 8-byte
 *      spill slot below the frame; machine instructions lower to the
 *      R10/R11 two-scratch form with narrow-result widening; the
 *      special-register obligations are honored (idiv RAX/RDX pair with
 *      sign extension, variable shifts with CL, setcc widened before
 *      the spill store);
 *   7. determinism - identical IR produces byte-identical physical
 *      dumps; distinct IR produces distinct dumps; pseudo-ops owned by
 *      17c (ISEL_TRAP, ISEL_STRCMP) pass through as annotated markers.
 *
 * The IR graphs are built directly with the ir_core constructors (the
 * same pattern as isel_core_test.c / frame_test.c) and are NOT passed
 * through ir_core_verify: the backend consumes verified IR per contract
 * sec. 1.3, but the tests build well-shaped graphs and the call pass
 * only walks the graph, the selection, and the framed stream.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17b2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/call_test.c \
 *     bootstrap/src/backend/call.c \
 *     bootstrap/src/backend/frame.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-17b2/call_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17b2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */
#include "call.h"

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
 * Shared construction helpers (mirror isel_core_test.c / frame_test.c)
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

static IrNode *mk_load(IrBuild *b, const char *file, IrType *t,
                       IrNode *lvalue)
{
    IrNode *n = mk(b, IR_LOAD, file, 4);
    if (n != NULL) {
        n->type = t;
        n->u.load.lvalue = lvalue;
    }
    return n;
}

static IrNode *mk_binary(IrBuild *b, const char *file, IrNodeKind kind,
                         IrType *t, IrNode *left, IrNode *right)
{
    IrNode *n = mk(b, kind, file, 4);
    if (n != NULL) {
        n->type = t;
        n->u.binary.left = left;
        n->u.binary.right = right;
    }
    return n;
}

static IrNode *mk_call(IrBuild *b, const char *file, IrNode *callee,
                       IrNode **args, size_t nargs)
{
    IrNode *n = mk(b, IR_CALL, file, 6);
    size_t i;
    if (n != NULL) {
        n->type = callee != NULL ? callee->u.function.ret_type : NULL;
        n->u.call.callee = callee;
        for (i = 0; i < nargs; i++) {
            ir_call_add_arg(b, n, args[i]);
        }
    }
    return n;
}

static IrNode *mk_expr_stmt(IrBuild *b, const char *file, IrNode *expr)
{
    IrNode *n = mk(b, IR_EXPR_STMT, file, 4);
    if (n != NULL) {
        n->u.expr_stmt.expr = expr;
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

/* A local slot appended to the function's slot table (locals after
 * params). Returns the slot index. */
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

/* ---------------------------------------------------------------------------
 * Select + frame + call helpers
 * ------------------------------------------------------------------------- */

/* Select `b`, frame it, register-allocate it, and render the physical
 * dump into an owned NUL-terminated string. Returns NULL on any
 * failure. When out_co is non-NULL the CallOutput is returned (caller
 * frees); otherwise it is freed here. */
static char *call_dump(IrBuild *b, size_t *out_len, CallOutput **out_co)
{
    IselOutput *sel = NULL;
    FrameOutput *fr = NULL;
    CallOutput *co = NULL;
    DiagBuf buf;
    char *copy;
    size_t n;
    if (isel_select(b, &sel) != ISEL_OK || sel == NULL) {
        return NULL;
    }
    if (frame_build(b, sel, &fr) != FRAME_OK || fr == NULL) {
        isel_output_free(sel);
        return NULL;
    }
    if (call_build(b, fr, &co) != CALL_OK || co == NULL) {
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    diag_buf_init(&buf);
    if (!call_asm_dump(co, &buf)) {
        diag_buf_free(&buf);
        call_output_free(co);
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    n = buf.len;
    copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        diag_buf_free(&buf);
        call_output_free(co);
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    memcpy(copy, buf.data, n);
    copy[n] = '\0';
    diag_buf_free(&buf);
    if (out_len != NULL) {
        *out_len = n;
    }
    if (out_co != NULL) {
        *out_co = co;
    } else {
        call_output_free(co);
    }
    frame_output_free(fr);
    isel_output_free(sel);
    return copy;
}

/* Find a "function <name>" region marker index in the physical stream:
 * the index of the CALL_OP_PSEUDO comment whose note starts with
 * "function <name>", or SIZE_MAX. */
static size_t find_function_region(const CallOutput *co, const char *name)
{
    size_t i;
    char prefix[64];
    size_t plen;
    (void)snprintf(prefix, sizeof(prefix), "function %s", name);
    plen = strlen(prefix);
    for (i = 0; i < call_output_count(co); i++) {
        const CallInsn *ci = call_output_insn(co, i);
        if (ci->op == CALL_OP_PSEUDO && ci->isel == ISEL_COMMENT &&
            ci->pseudo.note != NULL &&
            strncmp(ci->pseudo.note, prefix, plen) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

/* ---------------------------------------------------------------------------
 * Builds
 * ------------------------------------------------------------------------- */

/* g() -> i32 (returns 0) + f(a,b,c,d: i32) -> i32 whose body calls
 * g(a,b,c,d) (expression statement) and returns a. */
static IrBuild *make_call4_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *g, *g_body, *f, *f_body, *call, *args[4];
    IrNode *r0, *ret0, *local_a, *ret_a;
    int64_t p0, p1, p2, p3;
    size_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    g = mk_fn(b, "test.ai", "g", ir_type_i32(b));
    g_body = mk_block(b, "test.ai", 2);
    r0 = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ret0 = mk_return(b, "test.ai", r0);
    ir_block_add_stmt(b, g_body, ret0);
    g->u.function.body = g_body;
    ir_module_add_decl(b, mod, g);

    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    f_body = mk_block(b, "test.ai", 5);
    p0 = add_param_slot(b, f, "a", ir_type_i32(b), 6);
    p1 = add_param_slot(b, f, "b", ir_type_i32(b), 6);
    p2 = add_param_slot(b, f, "c", ir_type_i32(b), 6);
    p3 = add_param_slot(b, f, "d", ir_type_i32(b), 6);
    CHECK(p0 == 0 && p1 == 1 && p2 == 2 && p3 == 3);
    for (i = 0; i < 4; i++) {
        args[i] = mk_local(b, "test.ai", ir_type_i32(b), (int64_t)i);
    }
    call = mk_call(b, "test.ai", g, args, 4);
    ir_block_add_stmt(b, f_body, mk_expr_stmt(b, "test.ai", call));
    local_a = mk_local(b, "test.ai", ir_type_i32(b), 0);
    ret_a = mk_return(b, "test.ai", local_a);
    ir_block_add_stmt(b, f_body, ret_a);
    f->u.function.body = f_body;
    ir_module_add_decl(b, mod, f);
    ir_build_add_module(b, mod);
    return b;
}

/* g() -> i32 (returns 0) + f(a..f: i32) -> i32 whose body calls
 * g(a,b,c,d,e,f) (six scalar args; two go on the stack). */
static IrBuild *make_call6_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *g, *g_body, *f, *f_body, *call, *args[6];
    IrNode *r0, *ret0, *local_a, *ret_a;
    int64_t slots[6];
    size_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    g = mk_fn(b, "test.ai", "g", ir_type_i32(b));
    g_body = mk_block(b, "test.ai", 2);
    r0 = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ret0 = mk_return(b, "test.ai", r0);
    ir_block_add_stmt(b, g_body, ret0);
    g->u.function.body = g_body;
    ir_module_add_decl(b, mod, g);

    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    f_body = mk_block(b, "test.ai", 5);
    for (i = 0; i < 6; i++) {
        slots[i] = add_param_slot(b, f, "p", ir_type_i32(b), 6);
        CHECK(slots[i] == (int64_t)i);
    }
    for (i = 0; i < 6; i++) {
        args[i] = mk_local(b, "test.ai", ir_type_i32(b), (int64_t)i);
    }
    call = mk_call(b, "test.ai", g, args, 6);
    ir_block_add_stmt(b, f_body, mk_expr_stmt(b, "test.ai", call));
    local_a = mk_local(b, "test.ai", ir_type_i32(b), 0);
    ret_a = mk_return(b, "test.ai", local_a);
    ir_block_add_stmt(b, f_body, ret_a);
    f->u.function.body = f_body;
    ir_module_add_decl(b, mod, f);
    ir_build_add_module(b, mod);
    return b;
}

/* g() -> i32 (returns 7) + f() -> i32 whose body returns g(). */
static IrBuild *make_return_call_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *g, *g_body, *f, *f_body, *call, *r7, *ret7, *ret_call;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    g = mk_fn(b, "test.ai", "g", ir_type_i32(b));
    g_body = mk_block(b, "test.ai", 2);
    r7 = mk_int(b, "test.ai", ir_type_i32(b), 7);
    ret7 = mk_return(b, "test.ai", r7);
    ir_block_add_stmt(b, g_body, ret7);
    g->u.function.body = g_body;
    ir_module_add_decl(b, mod, g);

    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    f_body = mk_block(b, "test.ai", 4);
    call = mk_call(b, "test.ai", g, NULL, 0);
    ret_call = mk_return(b, "test.ai", call);
    ir_block_add_stmt(b, f_body, ret_call);
    f->u.function.body = f_body;
    ir_module_add_decl(b, mod, f);
    ir_build_add_module(b, mod);
    return b;
}

/* gv() -> void (empty body) + f() -> i32 calling gv() then returning 0;
 * plus rt.proc.exit (bodyless noreturn) + f2() -> i32 whose body is a
 * local decl followed by a noreturn call terminator to exit(0). */
static IrBuild *make_void_noreturn_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *gv, *gv_body, *f, *f_body, *call, *r0, *ret0;
    IrNode *exit_fn, *f2, *f2_body, *term, *exit_arg;
    int64_t q_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    gv = mk_fn(b, "test.ai", "gv", ir_type_void(b));
    gv_body = mk_block(b, "test.ai", 2);
    ir_block_add_stmt(b, gv_body, mk_return(b, "test.ai", NULL));
    gv->u.function.body = gv_body;
    ir_module_add_decl(b, mod, gv);

    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    f_body = mk_block(b, "test.ai", 4);
    call = mk_call(b, "test.ai", gv, NULL, 0);
    ir_block_add_stmt(b, f_body, mk_expr_stmt(b, "test.ai", call));
    r0 = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ret0 = mk_return(b, "test.ai", r0);
    ir_block_add_stmt(b, f_body, ret0);
    f->u.function.body = f_body;
    ir_module_add_decl(b, mod, f);

    exit_fn = mk_fn(b, "test.ai", "rt.proc.exit", ir_type_void(b));
    exit_fn->u.function.noreturn = true;
    exit_fn->u.function.body = NULL;
    ir_module_add_decl(b, mod, exit_fn);

    f2 = mk_fn(b, "test.ai", "f2", ir_type_i32(b));
    f2_body = mk_block(b, "test.ai", 6);
    q_idx = add_local_slot(f2, "q", ir_type_i32(b), 6);
    CHECK(q_idx == 0);
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 6);
        decl->u.local_decl.slot_index = q_idx;
        decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_i32(b), 3);
        ir_block_add_stmt(b, f2_body, decl);
    }
    term = mk(b, IR_CALL_TERM, "test.ai", 7);
    term->u.call_term.callee = exit_fn;
    exit_arg = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ir_call_term_add_arg(b, term, exit_arg);
    ir_block_add_stmt(b, f2_body, term);
    f2->u.function.body = f2_body;
    ir_module_add_decl(b, mod, f2);
    ir_build_add_module(b, mod);
    return b;
}

/* h(s: Slice) -> i32 (returns 0; composite param copied by value) +
 * f() -> i32 whose body declares a local slice, calls h(&local), and
 * returns 0. */
static IrBuild *make_composite_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *h, *h_body, *f, *f_body, *call, *r0, *ret0;
    IrNode *decl, *local_s, *ret_h0, *rh0;
    int64_t h_slot, s_slot;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    h = mk_fn(b, "test.ai", "h", ir_type_i32(b));
    h_body = mk_block(b, "test.ai", 3);
    h_slot = add_param_slot(b, h, "s", ir_type_slice(b, ir_type_i32(b)), 3);
    CHECK(h_slot == 0);
    rh0 = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ret_h0 = mk_return(b, "test.ai", rh0);
    ir_block_add_stmt(b, h_body, ret_h0);
    h->u.function.body = h_body;
    ir_module_add_decl(b, mod, h);

    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    f_body = mk_block(b, "test.ai", 5);
    s_slot = add_local_slot(f, "s", ir_type_slice(b, ir_type_i32(b)), 5);
    CHECK(s_slot == 0);
    decl = mk(b, IR_LOCAL_DECL, "test.ai", 5);
    decl->u.local_decl.slot_index = s_slot;
    decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ir_block_add_stmt(b, f_body, decl);
    local_s = mk_local(b, "test.ai", ir_type_slice(b, ir_type_i32(b)), s_slot);
    call = mk_call(b, "test.ai", h, &local_s, 1);
    ir_block_add_stmt(b, f_body, mk_expr_stmt(b, "test.ai", call));
    r0 = mk_int(b, "test.ai", ir_type_i32(b), 0);
    ret0 = mk_return(b, "test.ai", r0);
    ir_block_add_stmt(b, f_body, ret0);
    f->u.function.body = f_body;
    ir_module_add_decl(b, mod, f);
    ir_build_add_module(b, mod);
    return b;
}

/* f(a,b: i32) -> i32 whose body computes:
 *   eq = (a == b)            (discarded; cmp/setcc)
 *   return ((a + b) * 2 / 3) << 1
 * exercising add/imul/idiv/shl lowering and the spill machinery; plus
 * g(a: i32) -> i32 whose body stores a constant through the parameter's
 * address and returns the load (store-through-address register path:
 * `mov [r10], r11`). */
static IrBuild *make_regalloc_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body;
    IrNode *la1, *lb1, *load_a1, *load_b1, *eq, *eq_stmt;
    IrNode *la2, *lb2, *load_a2, *load_b2;
    IrNode *add, *c2, *mul, *c3, *div, *c1, *shl, *ret;
    IrNode *g, *g_body, *store, *store_stmt, *g_ret;
    IrNode *g_local, *g_val, *g_load, *g_load_local;
    IrType *i32 = NULL;
    int64_t p0, p1, gp0;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    i32 = ir_type_i32(b);
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", i32);
    body = mk_block(b, "test.ai", 5);
    p0 = add_param_slot(b, f, "a", i32, 6);
    p1 = add_param_slot(b, f, "b", i32, 6);
    CHECK(p0 == 0 && p1 == 1);
    /* eq = (a == b), discarded (cmp + setcc) */
    la1 = mk_local(b, "test.ai", i32, 0);
    lb1 = mk_local(b, "test.ai", i32, 1);
    load_a1 = mk_load(b, "test.ai", i32, la1);
    load_b1 = mk_load(b, "test.ai", i32, lb1);
    eq = mk_binary(b, "test.ai", IR_EQ, ir_type_bool(b), load_a1, load_b1);
    eq_stmt = mk_expr_stmt(b, "test.ai", eq);
    ir_block_add_stmt(b, body, eq_stmt);
    /* return ((a + b) * 2 / 3) << 1 */
    la2 = mk_local(b, "test.ai", i32, 0);
    lb2 = mk_local(b, "test.ai", i32, 1);
    load_a2 = mk_load(b, "test.ai", i32, la2);
    load_b2 = mk_load(b, "test.ai", i32, lb2);
    add = mk_binary(b, "test.ai", IR_ADD, i32, load_a2, load_b2);
    c2 = mk_int(b, "test.ai", i32, 2);
    mul = mk_binary(b, "test.ai", IR_MUL, i32, add, c2);
    c3 = mk_int(b, "test.ai", i32, 3);
    div = mk_binary(b, "test.ai", IR_DIV, i32, mul, c3);
    c1 = mk_int(b, "test.ai", i32, 1);
    shl = mk_binary(b, "test.ai", IR_SHL, i32, div, c1);
    ret = mk_return(b, "test.ai", shl);
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    /* g: *a = 9; return *a; */
    g = mk_fn(b, "test.ai", "g", i32);
    g_body = mk_block(b, "test.ai", 7);
    gp0 = add_param_slot(b, g, "a", i32, 7);
    CHECK(gp0 == 0);
    g_local = mk_local(b, "test.ai", i32, 0);
    g_val = mk_int(b, "test.ai", i32, 9);
    store = mk(b, IR_STORE, "test.ai", 7);
    store->u.store.dest = g_local;
    store->u.store.value = g_val;
    store_stmt = mk_expr_stmt(b, "test.ai", store);
    ir_block_add_stmt(b, g_body, store_stmt);
    g_load_local = mk_local(b, "test.ai", i32, 0);
    g_load = mk_load(b, "test.ai", i32, g_load_local);
    g_ret = mk_return(b, "test.ai", g_load);
    ir_block_add_stmt(b, g_body, g_ret);
    g->u.function.body = g_body;
    ir_module_add_decl(b, mod, g);
    ir_build_add_module(b, mod);
    return b;
}

/* strcmp pass-through build: f() -> bool whose body is
 *   eq = (s1 == s2)   (discarded; ISEL_STRCMP pseudo)
 * a function whose body ends in an unconditional trap, and a void
 * function whose body zero-fills a local array (REP STOSB, one saved
 * callee-saved register -> odd-push alignment adjustment). */
static IrBuild *make_pseudo_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *s1, *s2, *cmp, *stmt, *ret;
    IrNode *trap_fn, *trap_body, *trap;
    IrNode *zf, *zf_body, *arr_local, *zero, *zero_stmt;
    int64_t arr_slot;
    const uint8_t a1[] = { 'a' };
    const uint8_t a2[] = { 'b' };
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_bool(b));
    body = mk_block(b, "test.ai", 4);
    s1 = mk(b, IR_STR, "test.ai", 4);
    s1->type = ir_type_str(b);
    s1->u.constant.value = ir_const_str(b, a1, 1);
    s2 = mk(b, IR_STR, "test.ai", 4);
    s2->type = ir_type_str(b);
    s2->u.constant.value = ir_const_str(b, a2, 1);
    cmp = mk_binary(b, "test.ai", IR_EQ, ir_type_bool(b), s1, s2);
    stmt = mk_expr_stmt(b, "test.ai", cmp);
    ir_block_add_stmt(b, body, stmt);
    ret = mk_return(b, "test.ai", mk_int(b, "test.ai", ir_type_bool(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    trap_fn = mk_fn(b, "test.ai", "boom", ir_type_void(b));
    trap_body = mk_block(b, "test.ai", 5);
    trap = mk(b, IR_TRAP, "test.ai", 5);
    trap->u.trap.code = "AIC-R0801";
    trap->u.trap.has_user_code = false;
    ir_block_add_stmt(b, trap_body, trap);
    trap_fn->u.function.body = trap_body;
    ir_module_add_decl(b, mod, trap_fn);
    zf = mk_fn(b, "test.ai", "zfill", ir_type_void(b));
    zf_body = mk_block(b, "test.ai", 6);
    arr_slot = add_local_slot(zf, "arr", ir_type_array(b, ir_type_i32(b), 4), 6);
    CHECK(arr_slot == 0);
    arr_local = mk_local(b, "test.ai", ir_type_array(b, ir_type_i32(b), 4),
                         arr_slot);
    zero = mk(b, IR_ZERO, "test.ai", 6);
    zero->type = ir_type_array(b, ir_type_i32(b), 4);
    zero->u.unary.operand = arr_local;
    zero_stmt = mk_expr_stmt(b, "test.ai", zero);
    ir_block_add_stmt(b, zf_body, zero_stmt);
    ir_block_add_stmt(b, zf_body, mk_return(b, "test.ai", NULL));
    zf->u.function.body = zf_body;
    ir_module_add_decl(b, mod, zf);
    ir_build_add_module(b, mod);
    return b;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_scalar_call_registers(void)
{
    IrBuild *b = make_call4_build();
    CallOutput *co = NULL;
    char *d = NULL;
    const CallFunction *cf;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = call_dump(b, NULL, &co);
    CHECK(d != NULL);
    CHECK(co != NULL);
    if (co == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    /* f's plan: shadow reserved, no stack args, 16-aligned total */
    CHECK(call_function_count(co) == 2);
    cf = call_function_at(co, 1);   /* f declared after g */
    CHECK(cf != NULL);
    if (cf != NULL) {
        CHECK(cf->has_calls);
        CHECK(cf->shadow_bytes == 32);
        CHECK(cf->max_stack_args == 0);
        CHECK(cf->stackarg_bytes == 0);
        CHECK(cf->spill_bytes == 8 * cf->nvregs);
        CHECK(cf->total % 16 == 0);
        CHECK((cf->total + cf->saved_bytes) % 16 == 0);
        CHECK(cf->total >= cf->frame_size + cf->spill_bytes + 32);
    }
    /* register-argument moves and the call target */
    CHECK(strstr(d, "mov rcx, [rbp-32]") != NULL);
    CHECK(strstr(d, "mov rdx, [rbp-40]") != NULL);
    CHECK(strstr(d, "mov r8, [rbp-48]") != NULL);
    CHECK(strstr(d, "mov r9, [rbp-56]") != NULL);
    CHECK(strstr(d, "call fn1") != NULL);
    /* no stack-argument stores for a 4-arg call */
    CHECK(strstr(d, "[rsp+") == NULL);
    /* result capture from RAX (call result vreg 5 -> [rbp-64]) */
    CHECK(strstr(d, "mov r10, rax") != NULL);
    CHECK(strstr(d, "mov [rbp-64], r10") != NULL);
    /* f's prologue reserves the shadow space (frame 16 + spills 56 +
     * shadow 32 = 104 -> align16 = 112) */
    CHECK(strstr(d, "sub rsp, $112") != NULL);
    free(d);
    call_output_free(co);
    ir_build_free(b);
}

static void test_stack_args_and_shadow(void)
{
    IrBuild *b = make_call6_build();
    CallOutput *co = NULL;
    char *d = NULL;
    const CallFunction *cf;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = call_dump(b, NULL, &co);
    CHECK(d != NULL);
    CHECK(co != NULL);
    if (co == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    CHECK(call_function_count(co) == 2);
    cf = call_function_at(co, 1);   /* f declared after g */
    CHECK(cf != NULL);
    if (cf != NULL) {
        CHECK(cf->has_calls);
        CHECK(cf->max_stack_args == 2);
        CHECK(cf->stackarg_bytes == 16);
        CHECK(cf->shadow_bytes == 32);
        CHECK(cf->total % 16 == 0);
    }
    /* args 0-3 in registers, args 4-5 above the shadow space */
    CHECK(strstr(d, "mov rcx, [rbp-") != NULL);
    CHECK(strstr(d, "mov [rsp+32], r10") != NULL);
    CHECK(strstr(d, "mov [rsp+40], r10") != NULL);
    CHECK(strstr(d, "call fn1") != NULL);
    /* frame 32 + spills (9 vregs = 72) + shadow 32 + stack 16 = 152 ->
     * align16 = 160 */
    CHECK(strstr(d, "sub rsp, $160") != NULL);
    free(d);
    call_output_free(co);
    ir_build_free(b);
}

static void test_return_value_rax(void)
{
    IrBuild *b = make_return_call_build();
    CallOutput *co = NULL;
    char *d = NULL;
    const CallFunction *cf_f, *cf_g;
    size_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = call_dump(b, NULL, &co);
    CHECK(d != NULL);
    CHECK(co != NULL);
    if (co == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    CHECK(call_function_count(co) == 2);
    cf_f = call_function_at(co, 1);   /* f declared after g */
    cf_g = call_function_at(co, 0);
    CHECK(cf_f != NULL && cf_g != NULL);
    /* f: call g() then capture RAX into the result slot, then move the
     * result into RAX for the return */
    CHECK(strstr(d, "call fn1") != NULL);
    CHECK(strstr(d, "mov r10, rax") != NULL);
    CHECK(strstr(d, "mov [rbp-16], r10") != NULL);
    CHECK(strstr(d, "mov rax, [rbp-16]") != NULL);
    /* f's epilogue restores the frame and returns */
    CHECK(strstr(d, "mov rsp, rbp") != NULL);
    CHECK(strstr(d, "pop rbp") != NULL);
    /* g: constant 7 in its own spill slot, returned via RAX */
    if (cf_g != NULL) {
        CHECK(cf_g->nvregs == 1);
        CHECK(cf_g->spill_bytes == 8);
    }
    /* walk: every ISEL_RET is preceded by the return-value move into
     * RAX, which is preceded by the epilogue pop of rbp */
    for (i = 0; i < call_output_count(co); i++) {
        const CallInsn *ci = call_output_insn(co, i);
        if (ci->op == CALL_OP_BODY && ci->isel == ISEL_RET) {
            CHECK(i >= 2);
            if (i >= 2) {
                const CallInsn *prev = call_output_insn(co, i - 1);
                const CallInsn *before = call_output_insn(co, i - 2);
                CHECK(prev->op == CALL_OP_BODY && prev->isel == ISEL_MOV &&
                      prev->dst.kind == CALL_OPR_REG &&
                      prev->dst.id == X64_REG_RAX);
                CHECK(before->op == CALL_OP_POP_RBP);
            }
        }
    }
    (void)cf_f;
    free(d);
    call_output_free(co);
    ir_build_free(b);
}

static void test_void_and_noreturn_call(void)
{
    IrBuild *b = make_void_noreturn_build();
    CallOutput *co = NULL;
    char *d = NULL;
    size_t f_start, f2_start, i;
    bool saw_void_call = false;
    bool saw_after_call_rax = false;
    bool in_f2_first = true;
    bool f2_saw_push = false;
    bool f2_saw_call = false;
    bool f2_saw_restore = false;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = call_dump(b, NULL, &co);
    CHECK(d != NULL);
    CHECK(co != NULL);
    if (co == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    /* f calls void gv(): the ISEL_CALL has no result vreg, so no RAX
     * capture follows it. The physical region between f's call and its
     * ret must not contain "mov r10, rax". */
    f_start = find_function_region(co, "f");
    CHECK(f_start != (size_t)-1);
    if (f_start != (size_t)-1) {
        for (i = f_start; i < call_output_count(co); i++) {
            const CallInsn *ci = call_output_insn(co, i);
            if (ci->op == CALL_OP_BODY && ci->isel == ISEL_CALL) {
                saw_void_call = true;
            }
            if (ci->op == CALL_OP_BODY && ci->isel == ISEL_RET) {
                break;
            }
            if (ci->op == CALL_OP_BODY && ci->isel == ISEL_MOV &&
                ci->dst.kind == CALL_OPR_REG &&
                ci->dst.id == X64_REG_R10 &&
                ci->src1.kind == CALL_OPR_REG &&
                ci->src1.id == X64_REG_RAX) {
                saw_after_call_rax = true;
            }
        }
    }
    CHECK(saw_void_call);
    CHECK(!saw_after_call_rax);
    /* f2 (noreturn terminator): prologue + argument setup + call, no
     * epilogue/ret after the call */
    f2_start = find_function_region(co, "f2");
    CHECK(f2_start != (size_t)-1);
    if (f2_start != (size_t)-1) {
        for (i = f2_start; i < call_output_count(co); i++) {
            const CallInsn *ci = call_output_insn(co, i);
            if (in_f2_first) {
                in_f2_first = false;   /* skip f2's own marker comment */
                continue;
            }
            if (ci->op == CALL_OP_PSEUDO && ci->isel == ISEL_COMMENT &&
                ci->pseudo.note != NULL &&
                strncmp(ci->pseudo.note, "function ", 9) == 0) {
                break;   /* next function region */
            }
            if (ci->op == CALL_OP_PUSH_RBP) {
                f2_saw_push = true;
            }
            if (ci->op == CALL_OP_BODY && ci->isel == ISEL_CALL) {
                f2_saw_call = true;
            }
            if (ci->op == CALL_OP_MOV_RSP_RBP ||
                ci->op == CALL_OP_POP_RBP) {
                f2_saw_restore = true;
            }
        }
    }
    CHECK(f2_saw_push);
    CHECK(f2_saw_call);
    CHECK(!f2_saw_restore);
    CHECK(strstr(d, "call fn10") != NULL);   /* rt.proc.exit arg in rcx */
    free(d);
    call_output_free(co);
    ir_build_free(b);
}

static void test_composite_params_and_callee_saved(void)
{
    IrBuild *b = make_composite_build();
    CallOutput *co = NULL;
    char *d = NULL;
    const CallFunction *cf_h, *cf_f;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = call_dump(b, NULL, &co);
    CHECK(d != NULL);
    CHECK(co != NULL);
    if (co == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    CHECK(call_function_count(co) == 2);
    cf_h = call_function_at(co, 0);   /* h declared first */
    cf_f = call_function_at(co, 1);
    CHECK(cf_h != NULL && cf_f != NULL);
    if (cf_h != NULL) {
        /* h copies its composite param and must save RDI/RSI */
        CHECK(cf_h->saves_rdi);
        CHECK(cf_h->saves_rsi);
        CHECK(cf_h->saved_bytes == 16);
        CHECK(cf_h->total % 16 == 0);
        CHECK((cf_h->total + cf_h->saved_bytes) % 16 == 0);
    }
    if (cf_f != NULL) {
        /* f's composite local-decl copy uses REP MOVSB, so it saves the
         * callee-saved RDI/RSI it clobbers; its call passes the slice by
         * address */
        CHECK(cf_f->saves_rdi);
        CHECK(cf_f->saves_rsi);
        CHECK(cf_f->has_calls);
    }
    /* h's prologue saves, param copy, and epilogue restore */
    CHECK(strstr(d, "push rsi") != NULL);
    CHECK(strstr(d, "push rdi") != NULL);
    CHECK(strstr(d, "lea rdi, [rbp-16]") != NULL);
    CHECK(strstr(d, "mov rsi, rcx") != NULL);
    CHECK(strstr(d, "mov rcx, $16") != NULL);
    CHECK(strstr(d, "rep movsb") != NULL);
    CHECK(strstr(d, "pop rdi") != NULL);
    CHECK(strstr(d, "pop rsi") != NULL);
    /* f's call passes the local slice address in RCX (local_s vreg 2:
     * frame 16 + 8*(2+1) = 40) */
    CHECK(strstr(d, "mov rcx, [rbp-40]") != NULL);
    CHECK(strstr(d, "call fn1") != NULL);
    free(d);
    call_output_free(co);
    ir_build_free(b);
}

static void test_register_allocation_special(void)
{
    IrBuild *b = make_regalloc_build();
    CallOutput *co = NULL;
    char *d = NULL;
    const CallFunction *cf;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = call_dump(b, NULL, &co);
    CHECK(d != NULL);
    CHECK(co != NULL);
    if (co == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    CHECK(call_function_count(co) == 2);
    cf = call_function_at(co, 0);
    CHECK(cf != NULL);
    if (cf != NULL) {
        CHECK(cf->nvregs == 16);
        CHECK(cf->spill_bytes == 128);
        CHECK(!cf->has_calls);
        CHECK(cf->total % 16 == 0);
        CHECK(cf->total >= cf->frame_size + cf->spill_bytes);
    }
    /* g's store-through-address path: destination base in R10, source
     * value in R11 (regression: a shared R10 would clobber the base) */
    CHECK(strstr(d, "mov [r10], r11") != NULL);
    CHECK(strstr(d, "mov [r10], r10") == NULL);
    /* two-address arithmetic lowers to the R10/R11 scratch form */
    CHECK(strstr(d, "add r10d, r11d") != NULL);
    CHECK(strstr(d, "imul r10d, r11d") != NULL);
    /* comparison + setcc with narrow-result widening */
    CHECK(strstr(d, "cmp r10d, r11d") != NULL);
    CHECK(strstr(d, "sete r10b") != NULL);
    CHECK(strstr(d, "movzx r10, r10b") != NULL);
    /* idiv: RAX/RDX pair with sign extension (movsx rdx, eax = cdq) */
    CHECK(strstr(d, "movsx rdx, eax") != NULL);
    CHECK(strstr(d, "idiv r10d") != NULL);
    CHECK(strstr(d, "mov r10d, eax") != NULL);
    /* variable shift: count in CL */
    CHECK(strstr(d, "shl r10d, cl") != NULL);
    /* spill loads/stores are 64-bit against [rbp-...] */
    CHECK(strstr(d, "mov r10, [rbp-") != NULL);
    CHECK(strstr(d, "mov [rbp-128], r10") != NULL);
    CHECK(strstr(d, "sub rsp, $144") != NULL);
    free(d);
    call_output_free(co);
    ir_build_free(b);
}

static void test_determinism_and_dump_shape(void)
{
    IrBuild *b1 = make_call4_build();
    IrBuild *b2 = make_call4_build();
    IrBuild *bp = make_pseudo_build();
    CallOutput *co_p = NULL;
    char *d1 = NULL, *d2 = NULL, *dp = NULL;
    size_t n1 = 0, n2 = 0, np = 0;
    CHECK(b1 != NULL && b2 != NULL && bp != NULL);
    if (b1 == NULL || b2 == NULL || bp == NULL) {
        free(d1);
        free(d2);
        free(dp);
        call_output_free(co_p);
        ir_build_free(b1);
        ir_build_free(b2);
        ir_build_free(bp);
        return;
    }
    d1 = call_dump(b1, &n1, NULL);
    d2 = call_dump(b2, &n2, NULL);
    dp = call_dump(bp, &np, &co_p);
    CHECK(d1 != NULL && d2 != NULL && dp != NULL);
    if (d1 != NULL && d2 != NULL) {
        CHECK(n1 == n2);
        CHECK(memcmp(d1, d2, n1) == 0);
    }
    if (d1 != NULL && dp != NULL) {
        CHECK(n1 != np);
        CHECK(memcmp(d1, dp, n1 < np ? n1 : np) != 0);
    }
    if (d1 != NULL) {
        CHECK(strstr(d1, "; AI-Co call emission dump "
                         "(WP-M0-17b2, deterministic)") != NULL);
        CHECK(strstr(d1, "push rbp") != NULL);
        CHECK(strstr(d1, "mov rbp, rsp") != NULL);
    }
    if (dp != NULL) {
        /* 17c-owned pseudo-ops pass through as annotated markers */
        CHECK(strstr(dp, "strcmp") != NULL);
        CHECK(strstr(dp, "trap") != NULL);
        CHECK(strstr(dp, ".Lstr") != NULL);
        /* REP STOSB zero-fill lowering: RDI/RCX setup with RAX zeroed,
         * saving only the callee-saved RDI it clobbers (odd push count
         * -> the plan's total is adjusted so RSP stays 16-aligned) */
        CHECK(strstr(dp, "rep stosb") != NULL);
        CHECK(strstr(dp, "xor eax, eax") != NULL);
        CHECK(strstr(dp, "push rdi") != NULL);
        CHECK(strstr(dp, "push rsi") == NULL);
        if (call_function_count(co_p) == 3) {
            const CallFunction *zf = call_function_at(co_p, 2);
            CHECK(zf != NULL);
            if (zf != NULL) {
                CHECK(zf->saves_rdi);
                CHECK(!zf->saves_rsi);
                CHECK(zf->saved_bytes == 8);
                CHECK(zf->total % 16 == 8);          /* odd-push bump */
                CHECK((zf->total + zf->saved_bytes) % 16 == 0);
            }
        }
    }
    free(d1);
    free(d2);
    free(dp);
    call_output_free(co_p);
    ir_build_free(b1);
    ir_build_free(b2);
    ir_build_free(bp);
}

int main(void)
{
    test_scalar_call_registers();
    fprintf(stderr, "after test_scalar_call_registers\n");
    test_stack_args_and_shadow();
    fprintf(stderr, "after test_stack_args_and_shadow\n");
    test_return_value_rax();
    fprintf(stderr, "after test_return_value_rax\n");
    test_void_and_noreturn_call();
    fprintf(stderr, "after test_void_and_noreturn_call\n");
    test_composite_params_and_callee_saved();
    fprintf(stderr, "after test_composite_params_and_callee_saved\n");
    test_register_allocation_special();
    fprintf(stderr, "after test_register_allocation_special\n");
    test_determinism_and_dump_shape();
    fprintf(stderr, "after test_determinism_and_dump_shape\n");
    fprintf(stderr, "call_test: %d checks, %d failures\n",
            g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
