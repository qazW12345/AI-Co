/* bootstrap/src/backend/isel_x64_test.c
 *
 * WP-M0-17a2 x86-64+SSE2 instruction coverage and backend constraint
 * unit tests + assembly dump coverage tests.
 *
 * Proves, against the closed coverage registry of isel_x64.c and real
 * isel_core selections on hand-constructed IR builds:
 *   1. registry closed set - the coverage table is closed, has no
 *      duplicate mnemonics, and its covered entries (BASE/SSE2/PSEUDO)
 *      contain no AVX2/host-specific instruction (acceptance criterion
 *      1: "generated instruction set uses only x86-64 + SSE2; no
 *      AVX2/host-specific instructions required");
 *   2. opcode coverage - every IselOpcode of the 17a1 closed set maps to
 *      a registered, within-baseline entry (no instruction-selection
 *      gaps at the coverage layer);
 *   3. baseline queries - x64_check_mnemonic classifies x86-64, SSE2,
 *      pseudo, AVX2/higher, host-specific, and unknown mnemonics;
 *   4. constraint records - x64_constraint_record emits a valid
 *      AIC-B0601 record (phase "backend", severity "error", recovery
 *      "authoritative", derived span) for out-of-baseline instructions
 *      (acceptance criterion 2);
 *   5. clean verification - x64_verify_constraints over real selections
 *      (arithmetic + control flow, and the composite pseudo-op set)
 *      returns ISEL_X64_OK with no records;
 *   6. violation verification - an uncovered opcode in a selection
 *      produces AIC-B0601 records carrying the originating IR node's
 *      span as the derived span, deterministically ordered (contract
 *      sec. 9 comparator);
 *   7. assembly dump coverage - every instruction mnemonic rendered by
 *      isel_asm_dump resolves to a registered entry within the baseline
 *      (x86-64 + SSE2 + pseudo), and no AVX2/host-specific token
 *      appears in the dump.
 *
 * The IR graphs are built directly with the ir_core constructors (same
 * pattern as isel_core_test.c / ir_core_test.c) and are NOT passed
 * through ir_core_verify: isel_select consumes verified IR per contract
 * sec. 1.3, but verification is the IR package's concern.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17a2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/isel_x64_test.c \
 *     bootstrap/src/backend/isel_x64.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-17a2/isel_x64_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17a2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */
#include "isel_x64.h"

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
 * Shared construction helpers (mirror isel_core_test.c)
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

/* ---------------------------------------------------------------------------
 * Build fixtures
 * ------------------------------------------------------------------------- */

/* A function `f(i32 x, i32 y) -> i32` exercising the x86-64 baseline
 * instruction families: mov/lea/movzx/movsx, add/sub/imul/idiv/neg,
 * and/or/xor/not, shl/shr/sar, cmp/test/setcc, jcc/jmp/labels, ret. */
static IrBuild *make_arith_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body, *ret;
    IrNode *x_ref, *y_ref, *z_ref;
    int64_t x_idx, y_idx, z_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "arith", "arith.ai");
    fn = mk_fn(b, "arith.ai", "f", ir_type_i32(b));
    body = mk_block(b, "arith.ai", 6);
    x_idx = add_param_slot(b, fn, "x", ir_type_i32(b), 3);
    y_idx = add_param_slot(b, fn, "y", ir_type_i32(b), 3);
    z_idx = add_local_slot(fn, "z", ir_type_i32(b), 4);
    CHECK(x_idx == 0);
    CHECK(y_idx == 1);
    CHECK(z_idx == 2);
    x_ref = mk_local(b, "arith.ai", ir_type_i32(b), x_idx);
    y_ref = mk_local(b, "arith.ai", ir_type_i32(b), y_idx);

    /* var z: i32 = x + y; */
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "arith.ai", 4);
        decl->u.local_decl.slot_index = z_idx;
        decl->u.local_decl.init = mk_binary(b, "arith.ai", IR_ADD,
                                            x_ref, y_ref, ir_type_i32(b));
        ir_block_add_stmt(b, body, decl);
    }
    z_ref = mk_local(b, "arith.ai", ir_type_i32(b), z_idx);
    /* z = z * 2;  (imul) */
    {
        IrNode *mul = mk_binary(b, "arith.ai", IR_MUL, z_ref,
                                mk_int(b, "arith.ai", ir_type_i32(b), 2),
                                ir_type_i32(b));
        IrNode *st = mk_store(b, "arith.ai",
                              mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
                              mul, ir_type_i32(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, body, es);
    }
    /* z = z / y;  (idiv, trap AIC-R0803) */
    {
        IrNode *dv = mk_binary(b, "arith.ai", IR_DIV, z_ref, y_ref,
                               ir_type_i32(b));
        dv->trap_code = "AIC-R0803";
        IrNode *st = mk_store(b, "arith.ai",
                              mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
                              dv, ir_type_i32(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, body, es);
    }
    /* z = z & 1 | 8 ^ 4;  (and/or/xor) */
    {
        IrNode *and_ = mk_binary(b, "arith.ai", IR_BAND, z_ref,
                                 mk_int(b, "arith.ai", ir_type_i32(b), 1),
                                 ir_type_i32(b));
        IrNode *or_ = mk_binary(b, "arith.ai", IR_BOR, and_,
                                mk_int(b, "arith.ai", ir_type_i32(b), 8),
                                ir_type_i32(b));
        IrNode *xor_ = mk_binary(b, "arith.ai", IR_BXOR, or_,
                                 mk_int(b, "arith.ai", ir_type_i32(b), 4),
                                 ir_type_i32(b));
        IrNode *st = mk_store(b, "arith.ai",
                              mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
                              xor_, ir_type_i32(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es->u.expr_stmt.expr = st;
        ir_block_add_stmt(b, body, es);
    }
    /* z = z << 1; z = z >> 2; z = z - 3; z = ~z;  (shl/shr/sub/not) */
    {
        IrNode *shl = mk_binary(b, "arith.ai", IR_SHL, z_ref,
                                mk_int(b, "arith.ai", ir_type_i32(b), 1),
                                ir_type_i32(b));
        shl->trap_code = "AIC-R0804";
        IrNode *es1 = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es1->u.expr_stmt.expr = mk_store(
            b, "arith.ai", mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
            shl, ir_type_i32(b));
        ir_block_add_stmt(b, body, es1);
        IrNode *shr = mk_binary(b, "arith.ai", IR_SHR, z_ref,
                                mk_int(b, "arith.ai", ir_type_i32(b), 2),
                                ir_type_i32(b));
        shr->trap_code = "AIC-R0804";
        IrNode *es2 = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es2->u.expr_stmt.expr = mk_store(
            b, "arith.ai", mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
            shr, ir_type_i32(b));
        ir_block_add_stmt(b, body, es2);
        IrNode *sub = mk_binary(b, "arith.ai", IR_SUB, z_ref,
                                mk_int(b, "arith.ai", ir_type_i32(b), 3),
                                ir_type_i32(b));
        IrNode *es3 = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es3->u.expr_stmt.expr = mk_store(
            b, "arith.ai", mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
            sub, ir_type_i32(b));
        ir_block_add_stmt(b, body, es3);
        IrNode *bn = mk(b, IR_BNOT, "arith.ai", 5);
        bn->type = ir_type_i32(b);
        bn->u.unary.operand = z_ref;
        IrNode *es4 = mk(b, IR_EXPR_STMT, "arith.ai", 5);
        es4->u.expr_stmt.expr = mk_store(
            b, "arith.ai", mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
            bn, ir_type_i32(b));
        ir_block_add_stmt(b, body, es4);
    }
    /* if (x < y) { z = x; } else { z = y; }  (cmp/jl/test/jcc/jmp) */
    {
        IrNode *cond = mk_binary(b, "arith.ai", IR_LT, x_ref, y_ref,
                                 ir_type_i32(b));
        IrNode *then_blk = mk_block(b, "arith.ai", 6);
        IrNode *else_blk = mk_block(b, "arith.ai", 6);
        IrNode *if_stmt = mk(b, IR_IF, "arith.ai", 6);
        IrNode *es;
        es = mk(b, IR_EXPR_STMT, "arith.ai", 6);
        es->u.expr_stmt.expr = mk_store(
            b, "arith.ai", mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
            x_ref, ir_type_i32(b));
        ir_block_add_stmt(b, then_blk, es);
        es = mk(b, IR_EXPR_STMT, "arith.ai", 6);
        es->u.expr_stmt.expr = mk_store(
            b, "arith.ai", mk_local(b, "arith.ai", ir_type_i32(b), z_idx),
            y_ref, ir_type_i32(b));
        ir_block_add_stmt(b, else_blk, es);
        if_stmt->u.if_stmt.cond = cond;
        if_stmt->u.if_stmt.then_block = then_blk;
        if_stmt->u.if_stmt.else_block = else_blk;
        ir_block_add_stmt(b, body, if_stmt);
    }
    /* return z; */
    ret = mk_return(b, "arith.ai",
                    mk_local(b, "arith.ai", ir_type_i32(b), z_idx));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    return b;
}

/* A function `g()` exercising the composite pseudo-ops (rep movsb /
 * rep stosb / sliceeq / slice / strcmp / utf8 / ptrdiff / trap) plus a
 * call and a widening cast (movzx/movsx). */
static IrBuild *make_pseudo_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *fcallee, *body;
    int64_t s_addr, s_sl, s_str, s_u8sl;
    IrNode *addr_ref, *sl_ref, *str_ref, *u8sl_ref;
    IrNode *ret;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "pseudo", "pseudo.ai");
    /* callee for ISEL_CALL */
    fcallee = mk_fn(b, "pseudo.ai", "h", ir_type_i32(b));
    fcallee->u.function.body = mk_block(b, "pseudo.ai", 2);
    fcallee->u.function.body->u.block.nstmts = 0;
    ir_module_add_decl(b, mod, fcallee);
    fn = mk_fn(b, "pseudo.ai", "g", ir_type_i32(b));
    body = mk_block(b, "pseudo.ai", 4);
    s_addr = add_local_slot(fn, "arr", ir_type_ptr(b, ir_type_i32(b)), 4);
    s_sl = add_local_slot(fn, "sl", ir_type_slice(b, ir_type_i32(b)), 5);
    s_str = add_local_slot(fn, "s", ir_type_str(b), 6);
    s_u8sl = add_local_slot(fn, "bytes", ir_type_slice(b, ir_type_u8(b)), 7);
    CHECK(s_addr == 0);
    CHECK(s_sl == 1);
    CHECK(s_str == 2);
    CHECK(s_u8sl == 3);
    addr_ref = mk_local(b, "pseudo.ai", ir_type_ptr(b, ir_type_i32(b)),
                        s_addr);
    sl_ref = mk_local(b, "pseudo.ai", ir_type_slice(b, ir_type_i32(b)),
                      s_sl);
    str_ref = mk_local(b, "pseudo.ai", ir_type_str(b), s_str);
    u8sl_ref = mk_local(b, "pseudo.ai", ir_type_slice(b, ir_type_u8(b)),
                        s_u8sl);

    /* var sl: i32[] = *(arr);  -> composite decl init: rep movsb */
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "pseudo.ai", 5);
        decl->u.local_decl.slot_index = s_sl;
        decl->u.local_decl.init = addr_ref;
        ir_block_add_stmt(b, body, decl);
    }
    /* IR_ZERO(arr): rep stosb [rN], $8 */
    {
        IrNode *z = mk(b, IR_ZERO, "pseudo.ai", 5);
        z->u.unary.operand = addr_ref;
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = z;
        ir_block_add_stmt(b, body, es);
    }
    /* sliceeq: sl == sl -> sliceeq */
    {
        IrNode *eq = mk(b, IR_SLICE_EQ, "pseudo.ai", 5);
        eq->type = ir_type_bool(b);
        eq->u.binary.left = sl_ref;
        eq->u.binary.right = sl_ref;
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = eq;
        ir_block_add_stmt(b, body, es);
    }
    /* slice: sl[1..] -> slice (pair construction pseudo) */
    {
        IrNode *sl = mk(b, IR_SLICE, "pseudo.ai", 5);
        sl->type = ir_type_slice(b, ir_type_i32(b));
        sl->u.slice.base = sl_ref;
        sl->u.slice.start = mk_int(b, "pseudo.ai", ir_type_usize(b), 1);
        sl->u.slice.end = NULL;
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = sl;
        ir_block_add_stmt(b, body, es);
    }
    /* strcmp: s < s -> strcmp */
    {
        IrNode *lt = mk_binary(b, "pseudo.ai", IR_LT, str_ref, str_ref,
                               ir_type_str(b));
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = lt;
        ir_block_add_stmt(b, body, es);
    }
    /* utf8: u8[] -> str cast -> utf8 (trap AIC-R0806) */
    {
        IrNode *cast = mk(b, IR_CAST, "pseudo.ai", 5);
        cast->type = ir_type_str(b);
        cast->u.cast_wrap.value = u8sl_ref;
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = cast;
        ir_block_add_stmt(b, body, es);
    }
    /* ptrdiff: arr - arr -> ptrdiff (trap AIC-R0810) */
    {
        IrNode *pd = mk_binary(b, "pseudo.ai", IR_PTR_DIFF, addr_ref,
                               addr_ref, ir_type_i64(b));
        pd->trap_code = "AIC-R0810";
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = pd;
        ir_block_add_stmt(b, body, es);
    }
    /* widening cast i32 -> i64 (movzx/movsx) */
    {
        IrNode *cast = mk(b, IR_CAST, "pseudo.ai", 5);
        cast->type = ir_type_i64(b);
        cast->u.cast_wrap.value =
            mk_local(b, "pseudo.ai", ir_type_i32(b), s_addr);
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = cast;
        ir_block_add_stmt(b, body, es);
    }
    /* call: h(1) -> call */
    {
        IrNode *call = mk(b, IR_CALL, "pseudo.ai", 5);
        call->type = ir_type_i32(b);
        call->u.call.callee = fcallee;
        ir_call_add_arg(b, call,
                        mk_int(b, "pseudo.ai", ir_type_i32(b), 1));
        IrNode *es = mk(b, IR_EXPR_STMT, "pseudo.ai", 5);
        es->u.expr_stmt.expr = call;
        ir_block_add_stmt(b, body, es);
    }
    /* trap statement: unconditional trap marker (code AIC-R0815) */
    {
        IrNode *t = mk(b, IR_TRAP, "pseudo.ai", 5);
        t->u.trap.code = "AIC-R0815";
        t->u.trap.has_user_code = false;
        t->u.trap.user_code = 0;
        ir_block_add_stmt(b, body, t);
    }
    ret = mk_return(b, "pseudo.ai",
                    mk_int(b, "pseudo.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    return b;
}

/* ---------------------------------------------------------------------------
 * Dump mnemonic resolution (test-side mirror of the dump's mnemonic
 * rendering: width suffix b/w/l/q is a rendering detail; "rep movsb" /
 * "rep stosb" are two-token pseudo names).
 * ------------------------------------------------------------------------- */

/* Return the registry entry for a dump-rendered instruction token, or
 * NULL when the token does not resolve to a registered mnemonic. The
 * width suffix (b/w/l/q) is stripped when the raw token is unknown. */
static const IselX64InsnInfo *resolve_dump_mnemonic(const char *tok)
{
    const IselX64InsnInfo *info = x64_insn_info(tok);
    size_t n;
    if (info != NULL) {
        return info;
    }
    n = strlen(tok);
    if (n > 1 && (tok[n - 1] == 'b' || tok[n - 1] == 'w' ||
                  tok[n - 1] == 'l' || tok[n - 1] == 'q')) {
        char base[64];
        size_t i;
        if (n - 1 < sizeof(base)) {
            for (i = 0; i < n - 1; i++) {
                base[i] = tok[i];
            }
            base[n - 1] = '\0';
            info = x64_insn_info(base);
            if (info != NULL) {
                return info;
            }
        }
    }
    return NULL;
}

/* Verify every instruction line of a dump resolves to a registered
 * mnemonic within the baseline, and that no out-of-baseline mnemonic
 * token appears anywhere in the dump. Returns the number of instruction
 * lines scanned (also counts CHECKs). */
static size_t check_dump_coverage(const char *dump)
{
    size_t insn_lines = 0;
    const char *p = dump;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        if (line_len >= 2 && p[0] == ' ' && p[1] == ' ') {
            const char *tok = p + 2;
            const char *tok_end = tok;
            char mnem[64];
            size_t tlen;
            const IselX64InsnInfo *info;
            while (*tok_end != '\0' && *tok_end != ' ' && *tok_end != '\t') {
                tok_end++;
            }
            tlen = (size_t)(tok_end - tok);
            if (tlen == 0) {
                CHECK(!"empty mnemonic token in dump");
                insn_lines++;
                if (eol == NULL) {
                    break;
                }
                p = eol + 1;
                continue;
            }
            if (tlen >= sizeof(mnem)) {
                tlen = sizeof(mnem) - 1;
            }
            memcpy(mnem, tok, tlen);
            mnem[tlen] = '\0';
            /* two-token pseudo names: "rep movsb" / "rep stosb" */
            if (strcmp(mnem, "rep") == 0) {
                const char *second = tok_end;
                while (*second == ' ' || *second == '\t') {
                    second++;
                }
                {
                    const char *second_end = second;
                    size_t slen;
                    while (*second_end != '\0' && *second_end != ' ' &&
                           *second_end != '\t') {
                        second_end++;
                    }
                    slen = (size_t)(second_end - second);
                    if (slen > 0 && tlen + 1 + slen < sizeof(mnem)) {
                        mnem[tlen] = ' ';
                        memcpy(mnem + tlen + 1, second, slen);
                        mnem[tlen + 1 + slen] = '\0';
                    }
                }
            }
            info = resolve_dump_mnemonic(mnem);
            CHECK(info != NULL);
            if (info != NULL) {
                CHECK(info->feature == ISEL_X64_BASE ||
                      info->feature == ISEL_X64_SSE2 ||
                      info->feature == ISEL_X64_PSEUDO);
            }
            insn_lines++;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return insn_lines;
}

static void check_no_forbidden_tokens(const char *dump)
{
    static const char *const forbidden[] = {
        "vaddps", "vpaddd", "vmovaps", "vmovdqu", "vpxor",
        "vbroadcastss", "vzeroupper", "vmovdqa32", "vpternlogd",
        "kmovw", "aesenc", "aesenclast", "pclmulqdq", "rdrand",
        "rdseed", "sha1rnds4", "xgetbv"
    };
    size_t i;
    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        CHECK(strstr(dump, forbidden[i]) == NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_registry_closed_set(void)
{
    size_t count = x64_insn_count();
    size_t i, j;
    int n_base = 0, n_sse2 = 0, n_pseudo = 0, n_avx2 = 0, n_host = 0;
    CHECK(count == 132);
    for (i = 0; i < count; i++) {
        const IselX64InsnInfo *info = x64_insn_at(i);
        size_t occ = 0;
        CHECK(info != NULL);
        if (info == NULL) {
            continue;
        }
        CHECK(info->mnemonic != NULL && info->mnemonic[0] != '\0');
        CHECK(info->feature >= ISEL_X64_BASE && info->feature <= ISEL_X64_UNKNOWN);
        switch (info->feature) {
        case ISEL_X64_BASE:   n_base++;   CHECK(info->is_real); break;
        case ISEL_X64_SSE2:   n_sse2++;   CHECK(info->is_real); break;
        case ISEL_X64_PSEUDO: n_pseudo++; CHECK(!info->is_real); break;
        case ISEL_X64_AVX2:   n_avx2++;   CHECK(info->is_real); break;
        case ISEL_X64_HOST:   n_host++;   CHECK(info->is_real); break;
        default:              CHECK(!"bad feature");
        }
        /* no duplicate mnemonics: each mnemonic occurs exactly once */
        for (j = 0; j < count; j++) {
            const IselX64InsnInfo *other = x64_insn_at(j);
            if (other != NULL && other->mnemonic != NULL &&
                info->mnemonic != NULL &&
                strcmp(info->mnemonic, other->mnemonic) == 0) {
                occ++;
            }
        }
        CHECK(occ == 1);
    }
    /* the covered set (BASE/SSE2/PSEUDO) contains no AVX2/host entry;
     * forbidden entries are present so the enforcement path is real */
    CHECK(n_base == 43);
    CHECK(n_sse2 == 62);
    CHECK(n_pseudo == 10);
    CHECK(n_avx2 == 10);
    CHECK(n_host == 7);
    /* representative lookups */
    CHECK(x64_insn_info("mov") != NULL);
    CHECK(x64_insn_info("vaddps") != NULL);
    CHECK(x64_insn_info("not-a-mnemonic") == NULL);
}

static void test_opcode_coverage(void)
{
    IselOpcode op;
    for (op = ISEL_COMMENT; op <= ISEL_TRAP; op = (IselOpcode)(op + 1)) {
        IselX64Feature f = x64_opcode_feature(op);
        const char *text = isel_opcode_text(op);
        CHECK(f != ISEL_X64_UNKNOWN);
        CHECK(x64_opcode_within_baseline(op));
        /* every opcode's dump-rendered base name is registered */
        CHECK(x64_insn_info(text) != NULL);
        /* pseudo classification matches the 17a1 pseudo set */
        if (f == ISEL_X64_PSEUDO) {
            CHECK(x64_opcode_is_pseudo(op));
        } else {
            CHECK(!x64_opcode_is_pseudo(op));
        }
    }
    /* out-of-range opcodes are unknown (defense in depth) */
    CHECK(x64_opcode_feature((IselOpcode)999) == ISEL_X64_UNKNOWN);
    CHECK(!x64_opcode_within_baseline((IselOpcode)999));
}

static void test_baseline_mnemonic_queries(void)
{
    /* x86-64 baseline */
    CHECK(x64_check_mnemonic("mov") == ISEL_X64_BASE);
    CHECK(x64_check_mnemonic("call") == ISEL_X64_BASE);
    CHECK(x64_check_mnemonic("je") == ISEL_X64_BASE);
    CHECK(x64_check_mnemonic("setl") == ISEL_X64_BASE);
    CHECK(x64_mnemonic_within_baseline("mov"));
    CHECK(x64_mnemonic_within_baseline("ret"));
    /* SSE2 (permitted) */
    CHECK(x64_check_mnemonic("movsd") == ISEL_X64_SSE2);
    CHECK(x64_check_mnemonic("pxor") == ISEL_X64_SSE2);
    CHECK(x64_check_mnemonic("cvtsi2sd") == ISEL_X64_SSE2);
    CHECK(x64_mnemonic_within_baseline("addsd"));
    /* pseudo-ops (covered non-machine entries) */
    CHECK(x64_check_mnemonic("rep movsb") == ISEL_X64_PSEUDO);
    CHECK(x64_check_mnemonic("sliceeq") == ISEL_X64_PSEUDO);
    CHECK(x64_mnemonic_within_baseline("trap"));
    /* out of baseline */
    CHECK(x64_check_mnemonic("vaddps") == ISEL_X64_AVX2);
    CHECK(x64_check_mnemonic("vzeroupper") == ISEL_X64_AVX2);
    CHECK(x64_check_mnemonic("vmovdqa32") == ISEL_X64_AVX2);   /* AVX-512 */
    CHECK(!x64_mnemonic_within_baseline("vaddps"));
    CHECK(x64_check_mnemonic("rdrand") == ISEL_X64_HOST);
    CHECK(x64_check_mnemonic("aesenc") == ISEL_X64_HOST);
    CHECK(!x64_mnemonic_within_baseline("rdrand"));
    /* unknown */
    CHECK(x64_check_mnemonic("frobnicate") == ISEL_X64_UNKNOWN);
    CHECK(x64_check_mnemonic(NULL) == ISEL_X64_UNKNOWN);
    CHECK(!x64_mnemonic_within_baseline("frobnicate"));
    /* feature names are stable */
    CHECK(strcmp(x64_feature_text(ISEL_X64_BASE), "x86-64 baseline") == 0);
    CHECK(strcmp(x64_feature_text(ISEL_X64_SSE2), "SSE2") == 0);
    CHECK(strcmp(x64_feature_text(ISEL_X64_AVX2), "AVX2 or higher") == 0);
    CHECK(strcmp(x64_feature_text((IselX64Feature)99), "unknown") == 0);
}

static void test_constraint_record(void)
{
    DiagSpan *span = mk_span("main.ai", 12, 3, 205);
    DiagRecord *rec = x64_constraint_record("vaddps", ISEL_X64_AVX2, span);
    char errbuf[128];
    CHECK(rec != NULL);
    if (rec != NULL) {
        CHECK(strcmp(rec->code, "AIC-B0601") == 0);
        CHECK(strcmp(rec->severity, "error") == 0);
        CHECK(strcmp(rec->phase, "backend") == 0);
        CHECK(strcmp(rec->recovery, "authoritative") == 0);
        CHECK(rec->primary_span != NULL);
        if (rec->primary_span != NULL) {
            CHECK(strcmp(rec->primary_span->file, "main.ai") == 0);
            CHECK(rec->primary_span->start.offset == 205);
        }
        CHECK(strstr(rec->message, "vaddps") != NULL);
        CHECK(strstr(rec->message, "AVX2 or higher") != NULL);
        CHECK(diag_record_validate(rec, errbuf, sizeof(errbuf)));
        diag_record_free(rec);
    }
    /* host-specific variant */
    rec = x64_constraint_record("rdrand", ISEL_X64_HOST, NULL);
    CHECK(rec != NULL);
    if (rec != NULL) {
        CHECK(rec->primary_span == NULL);
        CHECK(strstr(rec->message, "rdrand") != NULL);
        CHECK(strstr(rec->message, "host-specific feature") != NULL);
        CHECK(diag_record_validate(rec, errbuf, sizeof(errbuf)));
        diag_record_free(rec);
    }
    /* unknown mnemonic variant */
    rec = x64_constraint_record("frobnicate", ISEL_X64_UNKNOWN, span);
    CHECK(rec != NULL);
    if (rec != NULL) {
        CHECK(strstr(rec->message, "not in the x86-64+SSE2") != NULL);
        CHECK(diag_record_validate(rec, errbuf, sizeof(errbuf)));
        diag_record_free(rec);
    }
    /* NULL mnemonic is rejected */
    CHECK(x64_constraint_record(NULL, ISEL_X64_AVX2, span) == NULL);
    diag_span_free(span);
}

static void test_verify_clean_selection(void)
{
    IrBuild *b = make_arith_build();
    IselOutput *sel = NULL;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;
    CHECK(b != NULL);
    if (b != NULL) {
        CHECK(isel_select(b, &sel) == ISEL_OK);
        if (sel != NULL) {
            /* the whole generated instruction set is within the
             * x86-64+SSE2 baseline (acceptance criterion 1) */
            CHECK(x64_verify_constraints(sel, b, &recs, &nrecs) ==
                  ISEL_X64_OK);
            CHECK(recs == NULL);
            CHECK(nrecs == 0);
            isel_output_free(sel);
        }
        ir_build_free(b);
    }
    b = make_pseudo_build();
    CHECK(b != NULL);
    if (b != NULL) {
        CHECK(isel_select(b, &sel) == ISEL_OK);
        if (sel != NULL) {
            CHECK(x64_verify_constraints(sel, b, &recs, &nrecs) ==
                  ISEL_X64_OK);
            CHECK(recs == NULL);
            CHECK(nrecs == 0);
            isel_output_free(sel);
        }
        ir_build_free(b);
    }
}

static void test_verify_violation_uncovered(void)
{
    /* Build a tiny IR graph, then hand-construct a selection containing
     * an out-of-range opcode value (no IselOpcode maps to it): the
     * coverage verifier must report AIC-B0601 with the originating IR
     * node's span as the derived span, deterministically ordered. */
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body, *ret;
    IselOutput *sel;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;
    size_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "m", "m.ai");
    fn = mk_fn(b, "m.ai", "f", ir_type_i32(b));
    body = mk_block(b, "m.ai", 4);
    ret = mk_return(b, "m.ai", mk_int(b, "m.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);

    sel = (IselOutput *)calloc(1, sizeof(IselOutput));
    CHECK(sel != NULL);
    if (sel != NULL) {
        sel->count = 2;
        sel->cap = 2;
        sel->insns = (IselInsn *)calloc(2, sizeof(IselInsn));
        CHECK(sel->insns != NULL);
        if (sel->insns != NULL) {
            sel->insns[0].op = (IselOpcode)999;
            sel->insns[0].ir_node_id = mod->id;   /* m.ai:1:1 offset 10 */
            sel->insns[1].op = (IselOpcode)999;
            sel->insns[1].ir_node_id = fn->id;    /* m.ai:3:1 offset 30 */

            CHECK(x64_verify_constraints(sel, b, &recs, &nrecs) ==
                  ISEL_X64_VIOLATION);
            CHECK(recs != NULL);
            CHECK(nrecs == 2);
            if (recs != NULL && nrecs == 2) {
                for (i = 0; i < nrecs; i++) {
                    CHECK(strcmp(recs[i]->code, "AIC-B0601") == 0);
                    CHECK(strcmp(recs[i]->severity, "error") == 0);
                    CHECK(strcmp(recs[i]->phase, "backend") == 0);
                    CHECK(strcmp(recs[i]->recovery, "authoritative") == 0);
                    CHECK(strstr(recs[i]->message, "<opcode 999>") != NULL);
                }
                /* derived spans: record 0 = module span (offset 10),
                 * record 1 = function span (offset 30); contract sec. 9
                 * ordering: same file, then start.offset */
                CHECK(recs[0]->primary_span != NULL);
                CHECK(recs[1]->primary_span != NULL);
                if (recs[0]->primary_span != NULL) {
                    CHECK(recs[0]->primary_span->start.offset == 10);
                }
                if (recs[1]->primary_span != NULL) {
                    CHECK(recs[1]->primary_span->start.offset == 30);
                }
                /* related facts carry opcode and originating node id */
                CHECK(recs[1]->related_count >= 4);
                CHECK(recs[1]->related[2].kind == DIAG_KV_INT &&
                      recs[1]->related[2].i == 999);      /* opcode */
                CHECK(recs[1]->related[3].kind == DIAG_KV_INT &&
                      recs[1]->related[3].i == fn->id);    /* node_id */
            }
            x64_records_free(recs, nrecs);
            recs = NULL;
            nrecs = 0;
        }
        free(sel->insns);
        free(sel);
    }
    ir_build_free(b);
}

static void test_dump_coverage(void)
{
    IrBuild *b = make_arith_build();
    char *dump;
    size_t lines = 0;
    CHECK(b != NULL);
    if (b != NULL) {
        dump = select_dump(b, NULL, NULL);
        CHECK(dump != NULL);
        if (dump != NULL) {
            /* every rendered instruction mnemonic resolves to a
             * registered entry within the baseline */
            lines = check_dump_coverage(dump);
            CHECK(lines > 0);
            /* dump is the deterministic 17a1 rendering */
            CHECK(strstr(dump, "; AI-Co isel_core assembly dump") != NULL);
            /* no AVX2/host-specific token anywhere in the dump */
            check_no_forbidden_tokens(dump);
            free(dump);
        }
        ir_build_free(b);
    }
    b = make_pseudo_build();
    CHECK(b != NULL);
    if (b != NULL) {
        dump = select_dump(b, NULL, NULL);
        CHECK(dump != NULL);
        if (dump != NULL) {
            lines = check_dump_coverage(dump);
            CHECK(lines > 0);
            check_no_forbidden_tokens(dump);
            /* the composite pseudo-op mnemonics are present and resolve
             * as covered pseudo entries */
            CHECK(strstr(dump, "rep movsb") != NULL);
            CHECK(strstr(dump, "rep stosb") != NULL);
            CHECK(strstr(dump, "sliceeq") != NULL);
            CHECK(strstr(dump, "slice") != NULL);
            CHECK(strstr(dump, "strcmp") != NULL);
            CHECK(strstr(dump, "utf8") != NULL);
            CHECK(strstr(dump, "ptrdiff") != NULL);
            CHECK(strstr(dump, "trap") != NULL);
            CHECK(strstr(dump, "call ") != NULL);
            free(dump);
        }
        ir_build_free(b);
    }
}

int main(void)
{
    test_registry_closed_set();
    fprintf(stderr, "after test_registry_closed_set\n");
    test_opcode_coverage();
    fprintf(stderr, "after test_opcode_coverage\n");
    test_baseline_mnemonic_queries();
    fprintf(stderr, "after test_baseline_mnemonic_queries\n");
    test_constraint_record();
    fprintf(stderr, "after test_constraint_record\n");
    test_verify_clean_selection();
    fprintf(stderr, "after test_verify_clean_selection\n");
    test_verify_violation_uncovered();
    fprintf(stderr, "after test_verify_violation_uncovered\n");
    test_dump_coverage();
    fprintf(stderr, "after test_dump_coverage\n");

    if (g_failures) {
        fprintf(stderr, "isel_x64_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("isel_x64_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
