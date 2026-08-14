/* bootstrap/src/backend/frame_test.c
 *
 * WP-M0-17b1 stack frame layout and prologue/epilogue unit tests.
 *
 * Proves, on hand-constructed IR builds (ir_core node model) selected
 * by isel_core and framed by frame_build:
 *   1. slot layout - deterministic RBP-relative offsets for params/
 *      locals/temps in slot-index order, base-aligned, non-overlapping,
 *      frame_size 16-byte aligned (acceptance criterion 1, layout);
 *   2. prologue - exactly `push rbp; mov rbp, rsp; [sub rsp,
 *      frame_size]` at each function start (SUB present iff
 *      frame_size > 0);
 *   3. epilogue - exactly `mov rsp, rbp; pop rbp` before every RET, and
 *      the RET retained with its return-value vreg;
 *   4. main entry setup - the entry-module function named `main` is
 *      marked is_entry and recorded as FrameOutput.entry_function_id;
 *   5. noreturn handling - a bodyless noreturn declaration (rt.proc.exit)
 *      receives no frame; a function whose body ends in a noreturn call
 *      terminator receives the prologue but no epilogue after the call
 *      (the frame is never corrupted);
 *   6. slot rewriting - ISEL_OP_SLOT operands become ISEL_OP_MEM with
 *      base FRAME_BASE_VREG and the slot's RBP offset as displacement,
 *      and the dump renders them as [rbp-<off>];
 *   7. determinism - identical IR produces byte-identical framed dumps;
 *      distinct IR produces distinct dumps.
 *
 * The IR graphs are built directly with the ir_core constructors (the
 * same pattern as isel_core_test.c) and are NOT passed through
 * ir_core_verify: the backend consumes verified IR per contract
 * sec. 1.3, but the tests build well-shaped graphs and the frame pass
 * only walks the graph and the selection.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-17b1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/backend/frame_test.c \
 *     bootstrap/src/backend/frame.c \
 *     bootstrap/src/backend/isel_core.c \
 *     bootstrap/src/ir/ir_core.c \
 *     bootstrap/src/diag/diag.c bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-17b1/frame_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-17b1)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */
#include "frame.h"

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
 * Select + frame helpers
 * ------------------------------------------------------------------------- */

/* Select `b`, frame it, and render the framed dump into an owned
 * NUL-terminated string. Returns NULL on any failure. */
static char *frame_dump(IrBuild *b, size_t *out_len, FrameOutput **out_fr)
{
    IselOutput *sel = NULL;
    FrameOutput *fr = NULL;
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
    diag_buf_init(&buf);
    if (!frame_asm_dump(fr, &buf)) {
        diag_buf_free(&buf);
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    n = buf.len;
    copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        diag_buf_free(&buf);
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
    if (out_fr != NULL) {
        *out_fr = fr;
    } else {
        frame_output_free(fr);
    }
    isel_output_free(sel);
    return copy;
}

/* ---------------------------------------------------------------------------
 * Build fixtures
 * ------------------------------------------------------------------------- */

/* fn f(i32 x) -> i32 with locals y: i32 and z: u8; body stores y and
 * returns it. Slots: [0]=x i32, [1]=y i32, [2]=z u8. */
static IrBuild *make_layout_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t x_idx, y_idx, z_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    fn = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    body = mk_block(b, "test.ai", 6);
    x_idx = add_param_slot(b, fn, "x", ir_type_i32(b), 3);
    y_idx = add_local_slot(fn, "y", ir_type_i32(b), 4);
    z_idx = add_local_slot(fn, "z", ir_type_u8(b), 5);
    CHECK(x_idx == 0);
    CHECK(y_idx == 1);
    CHECK(z_idx == 2);
    /* local decl y = 10 -> store to slot 1 */
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 4);
        decl->u.local_decl.slot_index = y_idx;
        decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_i32(b), 10);
        ir_block_add_stmt(b, body, decl);
    }
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 5);
        decl->u.local_decl.slot_index = z_idx;
        decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_u8(b), 1);
        ir_block_add_stmt(b, body, decl);
    }
    {
        IrNode *ret = mk_return(b, "test.ai",
                                mk_local(b, "test.ai", ir_type_i32(b), y_idx));
        ir_block_add_stmt(b, body, ret);
    }
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    return b;
}

/* fn f(i64 a) -> void with locals u: i32, v: i32, s: str. Exercises
 * alignment padding: i64 (8) then two i32 (4+4) then str (16, align 8).
 * Slots: [0]=a i64, [1]=u i32, [2]=v i32, [3]=s str. */
static IrBuild *make_alignment_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    int64_t a_idx, u_idx, v_idx, s_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    fn = mk_fn(b, "test.ai", "f", ir_type_void(b));
    body = mk_block(b, "test.ai", 6);
    a_idx = add_param_slot(b, fn, "a", ir_type_i64(b), 3);
    u_idx = add_local_slot(fn, "u", ir_type_i32(b), 4);
    v_idx = add_local_slot(fn, "v", ir_type_i32(b), 5);
    s_idx = add_local_slot(fn, "s", ir_type_str(b), 6);
    CHECK(a_idx == 0);
    CHECK(u_idx == 1);
    CHECK(v_idx == 2);
    CHECK(s_idx == 3);
    /* local decl u = 0 */
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 4);
        decl->u.local_decl.slot_index = u_idx;
        decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_i32(b), 0);
        ir_block_add_stmt(b, body, decl);
    }
    /* return (void) */
    {
        IrNode *ret = mk_return(b, "test.ai", NULL);
        ir_block_add_stmt(b, body, ret);
    }
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    return b;
}

/* Entry build: main() -> i32 with a local, plus a helper h(i32) -> i32.
 * Modules[0] is the entry module and declares main. */
static IrBuild *make_main_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *main_fn, *h_fn, *main_body, *h_body;
    int64_t m_idx, h_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    main_fn = mk_fn(b, "test.ai", "main", ir_type_i32(b));
    main_body = mk_block(b, "test.ai", 4);
    m_idx = add_local_slot(main_fn, "r", ir_type_i32(b), 4);
    CHECK(m_idx == 0);
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 4);
        decl->u.local_decl.slot_index = m_idx;
        decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_i32(b), 7);
        ir_block_add_stmt(b, main_body, decl);
    }
    {
        IrNode *ret = mk_return(b, "test.ai",
                                mk_local(b, "test.ai", ir_type_i32(b), m_idx));
        ir_block_add_stmt(b, main_body, ret);
    }
    main_fn->u.function.body = main_body;
    ir_module_add_decl(b, mod, main_fn);

    h_fn = mk_fn(b, "test.ai", "h", ir_type_i32(b));
    h_body = mk_block(b, "test.ai", 6);
    h_idx = add_param_slot(b, h_fn, "p", ir_type_i32(b), 6);
    CHECK(h_idx == 0);
    {
        IrNode *ret = mk_return(b, "test.ai",
                                mk_local(b, "test.ai", ir_type_i32(b), h_idx));
        ir_block_add_stmt(b, h_body, ret);
    }
    h_fn->u.function.body = h_body;
    ir_module_add_decl(b, mod, h_fn);
    ir_build_add_module(b, mod);
    return b;
}

/* Noreturn build: rt.proc.exit (bodyless, noreturn) + fn g() -> i32
 * whose body is a local decl followed by a call terminator to exit. */
static IrBuild *make_noreturn_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *exit_fn, *g_fn, *g_body;
    int64_t q_idx;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    exit_fn = mk_fn(b, "test.ai", "rt.proc.exit", ir_type_void(b));
    exit_fn->u.function.noreturn = true;
    exit_fn->u.function.body = NULL;
    ir_module_add_decl(b, mod, exit_fn);

    g_fn = mk_fn(b, "test.ai", "g", ir_type_i32(b));
    g_body = mk_block(b, "test.ai", 5);
    q_idx = add_local_slot(g_fn, "q", ir_type_i32(b), 5);
    CHECK(q_idx == 0);
    {
        IrNode *decl = mk(b, IR_LOCAL_DECL, "test.ai", 5);
        decl->u.local_decl.slot_index = q_idx;
        decl->u.local_decl.init = mk_int(b, "test.ai", ir_type_i32(b), 3);
        ir_block_add_stmt(b, g_body, decl);
    }
    {
        IrNode *term = mk(b, IR_CALL_TERM, "test.ai", 6);
        term->u.call_term.callee = exit_fn;
        ir_call_term_add_arg(b, term, mk_int(b, "test.ai", ir_type_i32(b), 0));
        ir_block_add_stmt(b, g_body, term);
    }
    g_fn->u.function.body = g_body;
    ir_module_add_decl(b, mod, g_fn);
    ir_build_add_module(b, mod);
    return b;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_slot_layout_basic(void)
{
    IrBuild *b = make_layout_build();
    FrameOutput *fr = NULL;
    const FrameLayout *layout;
    const FrameSlotLayout *slot;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(frame_dump(b, NULL, &fr) != NULL);
    CHECK(fr != NULL);
    if (fr == NULL) {
        ir_build_free(b);
        return;
    }
    CHECK(frame_layout_count(fr) == 1);
    layout = frame_layout_at(fr, 0);
    CHECK(layout != NULL);
    CHECK(layout->function_id >= 0);
    CHECK(layout->has_body);
    CHECK(!layout->noreturn);
    CHECK(!layout->is_entry);
    CHECK(layout->nslots == 3);
    /* i32 at slot 0: size 4, align 4, offset -4 */
    slot = frame_layout_slot(layout, 0);
    CHECK(slot != NULL && slot->size == 4 && slot->align == 4 &&
          slot->offset == -4);
    /* i32 at slot 1: offset -8 */
    slot = frame_layout_slot(layout, 1);
    CHECK(slot != NULL && slot->size == 4 && slot->align == 4 &&
          slot->offset == -8);
    /* u8 at slot 2: offset -9 */
    slot = frame_layout_slot(layout, 2);
    CHECK(slot != NULL && slot->size == 1 && slot->align == 1 &&
          slot->offset == -9);
    /* total 9 bytes -> frame 16 */
    CHECK(layout->frame_size == 16);
    CHECK(layout->frame_size % 16 == 0);
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_slot_layout_alignment_padding(void)
{
    IrBuild *b = make_alignment_build();
    FrameOutput *fr = NULL;
    const FrameLayout *layout;
    const FrameSlotLayout *slot;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(frame_dump(b, NULL, &fr) != NULL);
    CHECK(fr != NULL);
    if (fr == NULL) {
        ir_build_free(b);
        return;
    }
    layout = frame_layout_at(fr, 0);
    CHECK(layout != NULL);
    CHECK(layout->nslots == 4);
    /* a: i64 -> size 8, align 8, offset -8 */
    slot = frame_layout_slot(layout, 0);
    CHECK(slot != NULL && slot->size == 8 && slot->align == 8 &&
          slot->offset == -8);
    /* u: i32 -> size 4, align 4, offset -12 (packed after i64) */
    slot = frame_layout_slot(layout, 1);
    CHECK(slot != NULL && slot->size == 4 && slot->align == 4 &&
          slot->offset == -12);
    /* v: i32 -> offset -16 */
    slot = frame_layout_slot(layout, 2);
    CHECK(slot != NULL && slot->size == 4 && slot->align == 4 &&
          slot->offset == -16);
    /* s: str (16 bytes, align 8): cursor is 16, 16+16=32 is already
     * 8-aligned -> -32 */
    slot = frame_layout_slot(layout, 3);
    CHECK(slot != NULL && slot->size == 16 && slot->align == 8 &&
          slot->offset == -32);
    /* total 24 bytes -> frame 32 */
    CHECK(layout->frame_size == 32);
    /* every slot base is aligned: (-offset) % align == 0 */
    {
        size_t i;
        for (i = 0; i < layout->nslots; i++) {
            const FrameSlotLayout *s = &layout->slots[i];
            CHECK((-s->offset) % s->align == 0);
        }
    }
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_prologue_epilogue(void)
{
    IrBuild *b = make_layout_build();
    FrameOutput *fr = NULL;
    char *d = NULL;
    size_t i;
    bool saw_push = false, saw_movrbp = false, saw_sub = false;
    bool saw_movrsp = false, saw_pop = false, saw_ret = false;
    bool saw_lea = false, saw_mem = false;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = frame_dump(b, NULL, &fr);
    CHECK(d != NULL);
    CHECK(fr != NULL);
    if (fr == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    /* walk the framed stream */
    for (i = 0; i < frame_output_count(fr); i++) {
        const FrameInsn *fi = frame_output_insn(fr, i);
        CHECK(fi != NULL);
        if (fi == NULL) {
            continue;
        }
        switch (fi->op) {
        case FRAME_OP_PUSH_RBP: saw_push = true; break;
        case FRAME_OP_MOV_RBP_RSP: saw_movrbp = true; break;
        case FRAME_OP_SUB_RSP:
            saw_sub = true;
            CHECK(fi->imm == 16);
            break;
        case FRAME_OP_MOV_RSP_RBP: saw_movrsp = true; break;
        case FRAME_OP_POP_RBP: saw_pop = true; break;
        case FRAME_OP_BODY:
            if (fi->body.op == ISEL_RET) {
                saw_ret = true;
            }
            if (fi->body.op == ISEL_LEA &&
                fi->body.src1.kind == ISEL_OP_MEM &&
                fi->body.src1.vreg == FRAME_BASE_VREG) {
                saw_lea = true;
                CHECK(fi->body.src1.imm == -8);  /* y local address */
            }
            if (fi->body.op == ISEL_MOV &&
                fi->body.dst.kind == ISEL_OP_MEM &&
                fi->body.dst.vreg == FRAME_BASE_VREG) {
                saw_mem = true;  /* store to slot via frame base */
            }
            break;
        default:
            break;
        }
    }
    CHECK(saw_push);
    CHECK(saw_movrbp);
    CHECK(saw_sub);
    CHECK(saw_movrsp);
    CHECK(saw_pop);
    CHECK(saw_ret);
    CHECK(saw_lea);
    CHECK(saw_mem);
    /* ordering in the dump: push rbp before mov rbp rsp before sub */
    {
        const char *p_push = strstr(d, "push rbp");
        const char *p_movrbp = strstr(d, "mov rbp, rsp");
        const char *p_sub = strstr(d, "sub rsp");
        const char *p_movrsp = strstr(d, "mov rsp, rbp");
        const char *p_pop = strstr(d, "pop rbp");
        const char *p_ret = strstr(d, "ret");
        CHECK(p_push != NULL && p_movrbp != NULL && p_sub != NULL &&
              p_movrsp != NULL && p_pop != NULL && p_ret != NULL);
        if (p_push != NULL && p_movrbp != NULL && p_sub != NULL &&
            p_movrsp != NULL && p_pop != NULL && p_ret != NULL) {
            CHECK(p_push < p_movrbp && p_movrbp < p_sub);
            CHECK(p_movrsp < p_pop && p_pop < p_ret);
        }
    }
    CHECK(strstr(d, "[rbp-8]") != NULL);
    CHECK(strstr(d, "[rbp-4]") == NULL);  /* slot 0 is a param, not written */
    free(d);
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_frame_size_zero_no_sub(void)
{
    /* A function with no slots gets frame_size 0 and no SUB step. */
    IrBuild *b = ir_build_new();
    IrNode *mod, *fn, *body;
    FrameOutput *fr = NULL;
    size_t i;
    bool saw_sub = false, saw_push = false;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "test", "test.ai");
    fn = mk_fn(b, "test.ai", "empty", ir_type_void(b));
    body = mk_block(b, "test.ai", 3);
    {
        IrNode *ret = mk_return(b, "test.ai", NULL);
        ir_block_add_stmt(b, body, ret);
    }
    fn->u.function.body = body;
    ir_module_add_decl(b, mod, fn);
    ir_build_add_module(b, mod);
    CHECK(frame_dump(b, NULL, &fr) != NULL);
    CHECK(fr != NULL);
    if (fr == NULL) {
        ir_build_free(b);
        return;
    }
    CHECK(frame_layout_count(fr) == 1);
    CHECK(frame_layout_at(fr, 0)->frame_size == 0);
    CHECK(frame_layout_at(fr, 0)->nslots == 0);
    for (i = 0; i < frame_output_count(fr); i++) {
        const FrameInsn *fi = frame_output_insn(fr, i);
        if (fi->op == FRAME_OP_SUB_RSP) {
            saw_sub = true;
        }
        if (fi->op == FRAME_OP_PUSH_RBP) {
            saw_push = true;
        }
    }
    CHECK(saw_push);
    CHECK(!saw_sub);
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_main_entry(void)
{
    IrBuild *b = make_main_build();
    FrameOutput *fr = NULL;
    const FrameLayout *main_l = NULL, *h_l = NULL;
    size_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(frame_dump(b, NULL, &fr) != NULL);
    CHECK(fr != NULL);
    if (fr == NULL) {
        ir_build_free(b);
        return;
    }
    CHECK(frame_layout_count(fr) == 2);
    CHECK(fr->entry_function_id >= 0);
    for (i = 0; i < frame_layout_count(fr); i++) {
        const FrameLayout *l = frame_layout_at(fr, i);
        if (l->is_entry) {
            main_l = l;
        } else {
            h_l = l;
        }
    }
    CHECK(main_l != NULL);
    CHECK(h_l != NULL);
    if (main_l != NULL) {
        CHECK(main_l->function_id == fr->entry_function_id);
        CHECK(main_l->frame_size % 16 == 0);
    }
    if (h_l != NULL) {
        CHECK(!h_l->is_entry);
        CHECK(h_l->function_id != fr->entry_function_id);
    }
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_noreturn_handling(void)
{
    IrBuild *b = make_noreturn_build();
    FrameOutput *fr = NULL;
    char *d = NULL;
    const FrameLayout *exit_l = NULL, *g_l = NULL;
    size_t i;
    bool g_saw_push = false;
    bool g_saw_call = false;
    bool g_saw_restore = false;
    bool g_saw_ret = false;
    bool in_g = false;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = frame_dump(b, NULL, &fr);
    CHECK(d != NULL);
    CHECK(fr != NULL);
    if (fr == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    CHECK(frame_layout_count(fr) == 2);
    for (i = 0; i < frame_layout_count(fr); i++) {
        const FrameLayout *l = frame_layout_at(fr, i);
        if (l->noreturn) {
            exit_l = l;
        } else {
            g_l = l;
        }
    }
    CHECK(exit_l != NULL);
    CHECK(g_l != NULL);
    if (exit_l != NULL) {
        /* bodyless noreturn declaration: no frame */
        CHECK(!exit_l->has_body);
        CHECK(exit_l->frame_size == 0);
        CHECK(exit_l->nslots == 0);
    }
    if (g_l != NULL) {
        CHECK(g_l->has_body);
        CHECK(g_l->frame_size == 16);  /* one i32 local -> 16 */
    }
    /* walk the stream: g's region has prologue + call, no restore/ret */
    for (i = 0; i < frame_output_count(fr); i++) {
        const FrameInsn *fi = frame_output_insn(fr, i);
        if (fi->op == FRAME_OP_BODY && fi->body.op == ISEL_COMMENT) {
            /* function marker comments: "function <name>" */
            if (fi->body.note != NULL &&
                strncmp(fi->body.note, "function g", 10) == 0) {
                in_g = true;
            } else if (fi->body.note != NULL &&
                       strncmp(fi->body.note, "function ", 9) == 0) {
                in_g = false;
            }
            continue;
        }
        if (!in_g) {
            continue;
        }
        switch (fi->op) {
        case FRAME_OP_PUSH_RBP: g_saw_push = true; break;
        case FRAME_OP_MOV_RSP_RBP:
        case FRAME_OP_POP_RBP: g_saw_restore = true; break;
        case FRAME_OP_BODY:
            if (fi->body.op == ISEL_CALL) {
                g_saw_call = true;
            }
            if (fi->body.op == ISEL_RET) {
                g_saw_ret = true;
            }
            break;
        default:
            break;
        }
    }
    CHECK(g_saw_push);
    CHECK(g_saw_call);
    CHECK(!g_saw_restore);
    CHECK(!g_saw_ret);
    frame_output_free(fr);
    free(d);
    ir_build_free(b);
}

static void test_slot_rewrite_and_dump_shape(void)
{
    IrBuild *b = make_layout_build();
    FrameOutput *fr = NULL;
    char *d = NULL;
    const FrameInsn *fi;
    size_t i;
    bool saw_comment = false, saw_label = false;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = frame_dump(b, NULL, &fr);
    CHECK(d != NULL);
    CHECK(fr != NULL);
    if (fr == NULL || d == NULL) {
        free(d);
        ir_build_free(b);
        return;
    }
    /* every SLOT operand in the framed stream is a frame-base MEM */
    for (i = 0; i < frame_output_count(fr); i++) {
        const IselOperand *ops[3];
        int oi;
        fi = frame_output_insn(fr, i);
        if (fi->op != FRAME_OP_BODY) {
            continue;
        }
        if (fi->body.op == ISEL_COMMENT) {
            saw_comment = true;
        }
        if (fi->body.op == ISEL_LABEL) {
            saw_label = true;
        }
        ops[0] = &fi->body.dst;
        ops[1] = &fi->body.src1;
        ops[2] = &fi->body.src2;
        for (oi = 0; oi < 3; oi++) {
            CHECK(ops[oi]->kind != ISEL_OP_SLOT);
            if (ops[oi]->kind == ISEL_OP_MEM) {
                /* frame-base mem: base must be FRAME_BASE_VREG or a vreg */
                CHECK(ops[oi]->vreg == FRAME_BASE_VREG ||
                      ops[oi]->vreg >= 0);
            }
        }
    }
    /* the dump carries the deterministic header and the function marker */
    CHECK(strstr(d, "WP-M0-17b1") != NULL);
    CHECK(strstr(d, "# function f") != NULL);
    CHECK(saw_comment);
    CHECK(!saw_label);   /* this fixture has no control flow */
    free(d);
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_determinism(void)
{
    IrBuild *b1 = make_layout_build();
    IrBuild *b2 = make_layout_build();
    char *d1 = NULL, *d2 = NULL, *d3 = NULL;
    size_t n1 = 0, n2 = 0;
    CHECK(b1 != NULL && b2 != NULL);
    if (b1 == NULL || b2 == NULL) {
        ir_build_free(b1);
        ir_build_free(b2);
        return;
    }
    d1 = frame_dump(b1, &n1, NULL);
    d2 = frame_dump(b2, &n2, NULL);
    CHECK(d1 != NULL && d2 != NULL);
    if (d1 != NULL && d2 != NULL) {
        CHECK(n1 == n2);
        CHECK(memcmp(d1, d2, n1) == 0);
    }
    /* distinct IR -> distinct dump */
    {
        IrBuild *b3 = make_alignment_build();
        d3 = frame_dump(b3, NULL, NULL);
        CHECK(d3 != NULL);
        if (d3 != NULL && d1 != NULL) {
            CHECK(strcmp(d1, d3) != 0);
        }
        ir_build_free(b3);
    }
    free(d1);
    free(d2);
    free(d3);
    ir_build_free(b1);
    ir_build_free(b2);
}

static void test_noreturn_defensive_body(void)
{
    /* Defensive rule 6: a noreturn function WITH a body (not
     * representable in verified IR, but the frame pass must stay
     * deterministic) receives a prologue but no epilogue. */
    IrBuild *b = ir_build_new();
    IrNode *mod, *nr_fn, *helper, *body;
    IrNode *term;
    FrameOutput *fr = NULL;
    const FrameLayout *layout;
    size_t i;
    bool in_nr = false;
    bool saw_push = false, saw_restore = false, saw_ret = false;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    mod = mk_module(b, "test", "test.ai");
    helper = mk_fn(b, "test.ai", "rt.proc.exit", ir_type_void(b));
    helper->u.function.noreturn = true;
    helper->u.function.body = NULL;
    ir_module_add_decl(b, mod, helper);
    nr_fn = mk_fn(b, "test.ai", "f", ir_type_void(b));
    nr_fn->u.function.noreturn = true;
    body = mk_block(b, "test.ai", 5);
    term = mk(b, IR_CALL_TERM, "test.ai", 6);
    term->u.call_term.callee = helper;
    ir_call_term_add_arg(b, term, mk_int(b, "test.ai", ir_type_i32(b), 1));
    ir_block_add_stmt(b, body, term);
    nr_fn->u.function.body = body;
    ir_module_add_decl(b, mod, nr_fn);
    ir_build_add_module(b, mod);

    CHECK(frame_dump(b, NULL, &fr) != NULL);
    CHECK(fr != NULL);
    if (fr == NULL) {
        ir_build_free(b);
        return;
    }
    layout = frame_layout_for_function(fr, nr_fn->id);
    CHECK(layout != NULL);
    if (layout != NULL) {
        CHECK(layout->noreturn);
        CHECK(layout->has_body);
        CHECK(layout->frame_size == 0);   /* no slots */
    }
    for (i = 0; i < frame_output_count(fr); i++) {
        const FrameInsn *fi = frame_output_insn(fr, i);
        if (fi->op == FRAME_OP_BODY && fi->body.op == ISEL_COMMENT &&
            fi->body.note != NULL &&
            strncmp(fi->body.note, "function f", 10) == 0) {
            in_nr = true;
            continue;
        }
        if (fi->op == FRAME_OP_BODY && fi->body.op == ISEL_COMMENT &&
            fi->body.note != NULL &&
            strncmp(fi->body.note, "function ", 9) == 0) {
            in_nr = false;   /* a later function marker (none here) */
            continue;
        }
        if (!in_nr) {
            continue;
        }
        if (fi->op == FRAME_OP_PUSH_RBP) {
            saw_push = true;
        }
        if (fi->op == FRAME_OP_MOV_RSP_RBP || fi->op == FRAME_OP_POP_RBP) {
            saw_restore = true;
        }
        if (fi->op == FRAME_OP_BODY && fi->body.op == ISEL_RET) {
            saw_ret = true;
        }
    }
    CHECK(saw_push);
    CHECK(!saw_restore);
    CHECK(!saw_ret);
    frame_output_free(fr);
    ir_build_free(b);
}

static void test_multi_function_order_and_helpers(void)
{
    IrBuild *b = make_main_build();
    FrameOutput *fr = NULL;
    const FrameLayout *l0, *l1;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(frame_dump(b, NULL, &fr) != NULL);
    CHECK(fr != NULL);
    if (fr == NULL) {
        ir_build_free(b);
        return;
    }
    /* canonical order: main first, then h (declaration order) */
    l0 = frame_layout_at(fr, 0);
    l1 = frame_layout_at(fr, 1);
    CHECK(l0 != NULL && l1 != NULL);
    CHECK(l0->is_entry);
    CHECK(!l1->is_entry);
    /* frame_layout_for_function resolves by function id */
    CHECK(frame_layout_for_function(fr, l0->function_id) == l0);
    CHECK(frame_layout_for_function(fr, l1->function_id) == l1);
    CHECK(frame_layout_for_function(fr, -999) == NULL);
    frame_output_free(fr);
    ir_build_free(b);
}

int main(void)
{
    test_slot_layout_basic();
    fprintf(stderr, "after test_slot_layout_basic\n");
    test_slot_layout_alignment_padding();
    fprintf(stderr, "after test_slot_layout_alignment_padding\n");
    test_prologue_epilogue();
    fprintf(stderr, "after test_prologue_epilogue\n");
    test_frame_size_zero_no_sub();
    fprintf(stderr, "after test_frame_size_zero_no_sub\n");
    test_main_entry();
    fprintf(stderr, "after test_main_entry\n");
    test_noreturn_handling();
    fprintf(stderr, "after test_noreturn_handling\n");
    test_slot_rewrite_and_dump_shape();
    fprintf(stderr, "after test_slot_rewrite_and_dump_shape\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_noreturn_defensive_body();
    fprintf(stderr, "after test_noreturn_defensive_body\n");
    test_multi_function_order_and_helpers();
    fprintf(stderr, "after test_multi_function_order_and_helpers\n");

    if (g_failures) {
        fprintf(stderr, "frame_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("frame_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
