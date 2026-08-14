/* bootstrap/src/backend/trap_branch_test.c
 *
 * AI-Co Stage-0 trap branch emission tests (WP-M0-17c1).
 *
 * Exercises trap_branch_build / trap_asm_dump over the 17a1->17b2
 * pipeline: every runtime-failable checked operation in a test build
 * (overflowing add, divisor-zero idiv, out-of-range shift count, invalid
 * bool byte load, unconditional ISEL_TRAP) must produce a deterministic
 * trap branch to a trap site with the correct stable code and source
 * span, and each site must have its trap path (rt.trap.report call) at
 * the end of the function. Also verifies determinism (byte-identical
 * dumps for identical inputs, distinct dumps for distinct inputs) and
 * the per-function site plan.
 */
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */

#include "trap_branch.h"

#include "../ir/ir_core.h"
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
 * Shared construction helpers (mirror call_test.c / isel_core_test.c)
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

static IrNode *mk_return(IrBuild *b, const char *file, IrNode *value)
{
    IrNode *n = mk(b, IR_RETURN, file, 5);
    if (n != NULL) {
        n->u.return_stmt.value = value;
    }
    return n;
}

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

/* Declare the runtime trap-report function (bodyless, noreturn) so the
 * trap paths resolve a real call target. */
static IrNode *declare_trap_report(IrBuild *b, IrNode *mod)
{
    IrNode *report = mk_fn(b, "test.ai", "rt.trap.report", ir_type_void(b));
    if (report != NULL) {
        report->u.function.noreturn = true;
        report->u.function.body = NULL;
    }
    ir_module_add_decl(b, mod, report);
    return report;
}

/* ---------------------------------------------------------------------------
 * Pipeline helpers
 * ------------------------------------------------------------------------- */

/* Select + frame + call + trap_branch; render the trap dump into an owned
 * NUL-terminated string. When out_to is non-NULL the TrapOutput is
 * returned (caller frees); otherwise it is freed here. */
static char *trap_dump(IrBuild *b, size_t *out_len, TrapOutput **out_to)
{
    IselOutput *sel = NULL;
    FrameOutput *fr = NULL;
    CallOutput *co = NULL;
    TrapOutput *to = NULL;
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
    if (trap_branch_build(b, fr, co, &to) != TRAP_OK || to == NULL) {
        call_output_free(co);
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    diag_buf_init(&buf);
    if (!trap_asm_dump(to, &buf)) {
        diag_buf_free(&buf);
        trap_output_free(to);
        call_output_free(co);
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    n = buf.len;
    copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        diag_buf_free(&buf);
        trap_output_free(to);
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
    if (out_to != NULL) {
        *out_to = to;
    } else {
        trap_output_free(to);
    }
    call_output_free(co);
    frame_output_free(fr);
    isel_output_free(sel);
    return copy;
}

/* ---------------------------------------------------------------------------
 * Builds
 * ------------------------------------------------------------------------- */

/* f(a, b: i32) -> i32 { return a + b; } -- signed overflow (jo) */
static IrBuild *make_overflow_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *la, *lb, *load_a, *load_b, *add, *ret;
    int64_t p0, p1;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p0 = add_param_slot(b, f, "a", ir_type_i32(b), 4);
    p1 = add_param_slot(b, f, "b", ir_type_i32(b), 4);
    CHECK(p0 == 0 && p1 == 1);
    la = mk_local(b, "test.ai", ir_type_i32(b), 0);
    lb = mk_local(b, "test.ai", ir_type_i32(b), 1);
    load_a = mk_load(b, "test.ai", ir_type_i32(b), la);
    load_b = mk_load(b, "test.ai", ir_type_i32(b), lb);
    add = mk_binary(b, "test.ai", IR_ADD, ir_type_i32(b), load_a, load_b);
    ret = mk_return(b, "test.ai", add);
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f(a, b: u32) -> u32 { return a + b; } -- unsigned overflow (jc) */
static IrBuild *make_overflow_unsigned_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *la, *lb, *load_a, *load_b, *add, *ret;
    int64_t p0, p1;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_u32(b));
    body = mk_block(b, "test.ai", 3);
    p0 = add_param_slot(b, f, "a", ir_type_u32(b), 4);
    p1 = add_param_slot(b, f, "b", ir_type_u32(b), 4);
    CHECK(p0 == 0 && p1 == 1);
    la = mk_local(b, "test.ai", ir_type_u32(b), 0);
    lb = mk_local(b, "test.ai", ir_type_u32(b), 1);
    load_a = mk_load(b, "test.ai", ir_type_u32(b), la);
    load_b = mk_load(b, "test.ai", ir_type_u32(b), lb);
    add = mk_binary(b, "test.ai", IR_ADD, ir_type_u32(b), load_a, load_b);
    ret = mk_return(b, "test.ai", add);
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f(a, b: i32) -> i32 { return a / b; } -- divisor zero (test+jz) */
static IrBuild *make_div_zero_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *la, *lb, *load_a, *load_b, *div, *ret;
    int64_t p0, p1;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p0 = add_param_slot(b, f, "a", ir_type_i32(b), 4);
    p1 = add_param_slot(b, f, "b", ir_type_i32(b), 4);
    CHECK(p0 == 0 && p1 == 1);
    la = mk_local(b, "test.ai", ir_type_i32(b), 0);
    lb = mk_local(b, "test.ai", ir_type_i32(b), 1);
    load_a = mk_load(b, "test.ai", ir_type_i32(b), la);
    load_b = mk_load(b, "test.ai", ir_type_i32(b), lb);
    div = mk_binary(b, "test.ai", IR_DIV, ir_type_i32(b), load_a, load_b);
    ret = mk_return(b, "test.ai", div);
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f(a, b: i32) -> i32 { return a << b; } -- shift count (cmp+jae) */
static IrBuild *make_shift_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *la, *lb, *load_a, *load_b, *shl, *ret;
    int64_t p0, p1;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p0 = add_param_slot(b, f, "a", ir_type_i32(b), 4);
    p1 = add_param_slot(b, f, "b", ir_type_i32(b), 4);
    CHECK(p0 == 0 && p1 == 1);
    la = mk_local(b, "test.ai", ir_type_i32(b), 0);
    lb = mk_local(b, "test.ai", ir_type_i32(b), 1);
    load_a = mk_load(b, "test.ai", ir_type_i32(b), la);
    load_b = mk_load(b, "test.ai", ir_type_i32(b), lb);
    shl = mk_binary(b, "test.ai", IR_SHL, ir_type_i32(b), load_a, load_b);
    ret = mk_return(b, "test.ai", shl);
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f() -> bool { var b: bool; return b; } -- invalid bool byte (cmp+ja) */
static IrBuild *make_bool_load_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *lb, *load_b, *ret;
    int64_t b_slot;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_bool(b));
    body = mk_block(b, "test.ai", 3);
    b_slot = add_local_slot(f, "b", ir_type_bool(b), 4);
    CHECK(b_slot == 0);
    lb = mk_local(b, "test.ai", ir_type_bool(b), b_slot);
    load_b = mk_load(b, "test.ai", ir_type_bool(b), lb);
    ret = mk_return(b, "test.ai", load_b);
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* boom() -> void { trap; } (language trap AIC-R0801) + usr() -> void
 * { trap 7; } (user trap) */
static IrBuild *make_trap_marker_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *boom, *boom_body, *trap, *usr, *usr_body, *utrap;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    boom = mk_fn(b, "test.ai", "boom", ir_type_void(b));
    boom_body = mk_block(b, "test.ai", 5);
    trap = mk(b, IR_TRAP, "test.ai", 5);
    trap->u.trap.code = "AIC-R0801";
    trap->u.trap.has_user_code = false;
    ir_block_add_stmt(b, boom_body, trap);
    boom->u.function.body = boom_body;
    ir_module_add_decl(b, mod, boom);

    usr = mk_fn(b, "test.ai", "usr", ir_type_void(b));
    usr_body = mk_block(b, "test.ai", 7);
    utrap = mk(b, IR_TRAP, "test.ai", 7);
    utrap->u.trap.code = NULL;
    utrap->u.trap.has_user_code = true;
    utrap->u.trap.user_code = 7;
    ir_block_add_stmt(b, usr_body, utrap);
    usr->u.function.body = usr_body;
    ir_module_add_decl(b, mod, usr);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f() -> void { trap; trap; ... } with `n` IR_TRAP statements, each at a
 * distinct source line (5..5+n-1). Every statement becomes an ISEL_TRAP
 * marker and therefore one unconditional trap site; the function exercises
 * the site-plan growth path with n obligations (WP-M0-17c1 regression:
 * append_site heap-buffer-overflow for functions with 2-8 trap sites). */
static IrBuild *make_trap_count_build(int64_t n)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body;
    int64_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f", ir_type_void(b));
    body = mk_block(b, "test.ai", 3);
    for (i = 0; i < n; i++) {
        IrNode *trap = mk(b, IR_TRAP, "test.ai", 5 + i);
        trap->u.trap.code = "AIC-R0801";
        trap->u.trap.has_user_code = false;
        ir_block_add_stmt(b, body, trap);
    }
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_overflow_branch(void)
{
    IrBuild *b = make_overflow_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* the checked add is followed by the signed-overflow branch */
    CHECK(strstr(d, "add r10d, r11d") != NULL);
    CHECK(strstr(d, "jo .Ltrap0") != NULL);
    /* site plan: stable code + numeric code + span */
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0802 code=2050 span=test.ai:4:1")
          != NULL);
    /* trap path: shadow + code + message + report call */
    CHECK(strstr(d, ".Ltrap0:") != NULL);
    CHECK(strstr(d, "sub rsp, $32") != NULL);
    CHECK(strstr(d, "mov rcx, $2050") != NULL);
    /* rt_trap.h ABI: RDX = message_data (the .Lmsg0 message TEXT, not a
     * str pair image) and R8 = message_len (statically known byte length
     * of the message text "AIC-R0802 at test.ai:4:1" = 24 bytes) */
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $24") != NULL);
    CHECK(strstr(d, "call fn") != NULL);
    /* deterministic message text carries code + span */
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0802 at test.ai:4:1\"") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_unsigned_overflow_uses_carry(void)
{
    IrBuild *b = make_overflow_unsigned_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* unsigned add overflow traps on carry (jc), not signed overflow */
    CHECK(strstr(d, "add r10d, r11d") != NULL);
    CHECK(strstr(d, "jc .Ltrap0") != NULL);
    CHECK(strstr(d, "jo .Ltrap0") == NULL);
    free(d);
    ir_build_free(b);
}

static void test_div_zero_branch(void)
{
    IrBuild *b = make_div_zero_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* divisor zero is tested before the idiv */
    CHECK(strstr(d, "test r10d, r10d") != NULL);
    CHECK(strstr(d, "jz .Ltrap0") != NULL);
    CHECK(strstr(d, "idiv r10d") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0803 code=2051 span=test.ai:4:1")
          != NULL);
    CHECK(strstr(d, "mov rcx, $2051") != NULL);
    /* rt_trap.h ABI: R8 carries the message byte length (24 = strlen of
     * "AIC-R0803 at test.ai:4:1") */
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $24") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_shift_count_branch(void)
{
    IrBuild *b = make_shift_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* variable shift count is checked against the operand width (32 bits
     * for i32) before the shift */
    CHECK(strstr(d, "cmp cl, $32") != NULL);
    CHECK(strstr(d, "jae .Ltrap0") != NULL);
    CHECK(strstr(d, "shl r10d, cl") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0804 code=2052 span=test.ai:4:1")
          != NULL);
    CHECK(strstr(d, "mov rcx, $2052") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $24") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_bool_byte_branch(void)
{
    IrBuild *b = make_bool_load_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* a loaded bool byte above 1 traps after the load */
    CHECK(strstr(d, "cmp r10b, $1") != NULL);
    CHECK(strstr(d, "ja .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0805 code=2053 span=test.ai:4:1")
          != NULL);
    CHECK(strstr(d, "mov rcx, $2053") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $24") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_trap_marker_sites(void)
{
    IrBuild *b = make_trap_marker_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* boom: language trap AIC-R0801 -> unconditional jmp + path */
    CHECK(strstr(d, "jmp .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0801 code=2049 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "mov rcx, $2049") != NULL);
    /* rt_trap.h ABI: R8 = message_len (24 = strlen of
     * "AIC-R0801 at test.ai:5:1") */
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $24") != NULL);
    /* usr: user trap -> the caller's u32 code is passed (sites restart
     * per function, so this function's site is .Ltrap0) */
    CHECK(strstr(d, ";   .Ltrap0 user code=7 span=test.ai:7:1") != NULL);
    CHECK(strstr(d, "mov rcx, $7") != NULL);
    /* R8 = message_len (26 = strlen of "user trap 7 at test.ai:7:1") */
    CHECK(strstr(d, "lea rdx, [.Lmsg1]\n  mov r8, $26") != NULL);
    CHECK(strstr(d, "; .Lmsg1 = \"user trap 7 at test.ai:7:1\"") != NULL);
    free(d);
    ir_build_free(b);
}

/* Shared multi-site plan verification: `n` unconditional trap sites in one
 * function must all be recorded (code/numeric_code/span per site), the dump
 * must show sites=n and one site-plan line per site, and every trap path
 * must exist. Exercises the append_site growth boundaries (2 = past the
 * initial-1 write, 8 = last slot of the initial 8-element block, 9 =
 * realloc boundary). */
static void check_site_count(int64_t n)
{
    IrBuild *b = make_trap_count_build(n);
    TrapOutput *to = NULL;
    char *d = NULL;
    size_t dn = 0;
    int64_t i;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = trap_dump(b, &dn, &to);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* dump: per-function site plan with sites=n and one .Ltrap<k> line per
     * site (code + numeric code + span; unconditional traps are AIC-R0801
     * with spans at lines 5..5+n-1) */
    {
        char want[96];
        snprintf(want, sizeof(want), "sites=%lld", (long long)n);
        CHECK(strstr(d, want) != NULL);
    }
    for (i = 0; i < n; i++) {
        char want[128];
        snprintf(want, sizeof(want),
                 ";   .Ltrap%lld AIC-R0801 code=2049 span=test.ai:%lld:1",
                 (long long)i, (long long)(5 + i));
        CHECK(strstr(d, want) != NULL);
        /* trap path label */
        snprintf(want, sizeof(want), ".Ltrap%lld:", (long long)i);
        CHECK(strstr(d, want) != NULL);
        /* report-call code argument */
        snprintf(want, sizeof(want), "mov rcx, $2049");
        CHECK(strstr(d, want) != NULL);
    }
    /* accessor plan: the first function (f) carries exactly n sites, each
     * with the correct code, numeric code, and span */
    if (to != NULL) {
        CHECK(trap_function_count(to) >= 2);   /* f + rt.trap.report */
        if (trap_function_count(to) >= 1) {
            const TrapFunction *tf = trap_function_at(to, 0);
            if (tf != NULL) {
                CHECK(tf->nsites == (size_t)n);
                CHECK(tf->sites != NULL);
                for (i = 0; i < n; i++) {
                    const TrapSite *s = trap_function_site(tf, (size_t)i);
                    CHECK(s != NULL);
                    if (s != NULL) {
                        CHECK(s->site_index == i);
                        CHECK(s->code != NULL);
                        CHECK(strcmp(s->code, "AIC-R0801") == 0);
                        CHECK(s->numeric_code == 2049);
                        CHECK(s->span != NULL);
                        CHECK(s->span->file != NULL);
                        CHECK(strcmp(s->span->file, "test.ai") == 0);
                        CHECK(s->span->start.line == 5 + i);
                        CHECK(s->span->start.col == 1);
                        CHECK(s->unconditional);
                    }
                }
            }
        }
    }
    free(d);
    trap_output_free(to);
    ir_build_free(b);
}

static void test_site_plan_2_sites(void)
{
    check_site_count(2);
}

static void test_site_plan_8_sites(void)
{
    check_site_count(8);
}

static void test_site_plan_9_sites(void)
{
    check_site_count(9);
}

static void test_site_plan_and_determinism(void)
{
    IrBuild *b1 = make_overflow_build();
    IrBuild *b2 = make_overflow_build();
    IrBuild *bd = make_div_zero_build();
    TrapOutput *to1 = NULL;
    char *d1 = NULL, *d2 = NULL, *dd = NULL;
    size_t n1 = 0, n2 = 0, nd = 0;
    CHECK(b1 != NULL && b2 != NULL && bd != NULL);
    if (b1 == NULL || b2 == NULL || bd == NULL) {
        free(d1);
        free(d2);
        free(dd);
        trap_output_free(to1);
        ir_build_free(b1);
        ir_build_free(b2);
        ir_build_free(bd);
        return;
    }
    d1 = trap_dump(b1, &n1, &to1);
    d2 = trap_dump(b2, &n2, NULL);
    dd = trap_dump(bd, &nd, NULL);
    CHECK(d1 != NULL && d2 != NULL && dd != NULL);
    if (d1 != NULL && d2 != NULL) {
        CHECK(n1 == n2);
        CHECK(memcmp(d1, d2, n1) == 0);
    }
    if (d1 != NULL && dd != NULL) {
        CHECK(n1 != nd);
        CHECK(memcmp(d1, dd, n1 < nd ? n1 : nd) != 0);
    }
    if (d1 != NULL) {
        CHECK(strstr(d1, "; AI-Co trap branch dump "
                         "(WP-M0-17c1, deterministic)") != NULL);
        CHECK(strstr(d1, "; function ") != NULL);
        CHECK(strstr(d1, "sites=1") != NULL);
    }
    if (to1 != NULL) {
        CHECK(trap_function_count(to1) >= 2);   /* f + rt.trap.report */
        CHECK(trap_output_count(to1) > 0);
        CHECK(trap_message(to1, 0) != NULL);
        if (trap_function_count(to1) >= 1) {
            const TrapFunction *tf = trap_function_at(to1, 0);
            if (tf != NULL) {
                CHECK(tf->nsites == 1);
                CHECK(tf->sites != NULL);
                if (tf->nsites == 1) {
                    const TrapSite *s = trap_function_site(tf, 0);
                    CHECK(s != NULL);
                    if (s != NULL) {
                        CHECK(s->code != NULL);
                        CHECK(strcmp(s->code, "AIC-R0802") == 0);
                        CHECK(s->numeric_code == 2050);
                        CHECK(s->span != NULL);
                        CHECK(s->span->file != NULL);
                        CHECK(strcmp(s->span->file, "test.ai") == 0);
                        CHECK(s->span->start.line == 4);
                    }
                }
            }
        }
    }
    free(d1);
    free(d2);
    free(dd);
    trap_output_free(to1);
    ir_build_free(b1);
    ir_build_free(b2);
    ir_build_free(bd);
}

int main(void)
{
    test_overflow_branch();
    fprintf(stderr, "after test_overflow_branch\n");
    test_unsigned_overflow_uses_carry();
    fprintf(stderr, "after test_unsigned_overflow_uses_carry\n");
    test_div_zero_branch();
    fprintf(stderr, "after test_div_zero_branch\n");
    test_shift_count_branch();
    fprintf(stderr, "after test_shift_count_branch\n");
    test_bool_byte_branch();
    fprintf(stderr, "after test_bool_byte_branch\n");
    test_trap_marker_sites();
    fprintf(stderr, "after test_trap_marker_sites\n");
    test_site_plan_2_sites();
    fprintf(stderr, "after test_site_plan_2_sites\n");
    test_site_plan_8_sites();
    fprintf(stderr, "after test_site_plan_8_sites\n");
    test_site_plan_9_sites();
    fprintf(stderr, "after test_site_plan_9_sites\n");
    test_site_plan_and_determinism();
    fprintf(stderr, "after test_site_plan_and_determinism\n");
    fprintf(stderr, "trap_branch_test: %d checks, %d failures\n",
            g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
