/* bootstrap/src/backend/trap_checked_test.c
 *
 * AI-Co Stage-0 checked-op emission tests (WP-M0-17c2).
 *
 * Exercises trap_checked_build / checked_asm_dump over the
 * 17a1->17b2->17c1 pipeline: every complex checked-operation obligation
 * (index bounds, null deref, cast range, slice bounds, UTF-8 validation,
 * pointer difference) must receive its deterministic multi-instruction
 * check sequence at the obligation body, branching to the site's trap
 * path, and every trap record must preserve the failing operation's
 * source span AND its full causal chain (root cause first, IR contract
 * sec. 8.4). Also verifies determinism (byte-identical dumps for
 * identical inputs) and the per-function site plan with span + causes.
 *
 * NOTE (17c1 dependency defect, RESOLVED at 39fb443): the committed 17c1
 * trap_branch.c append_site originally grew the per-function site array
 * only when nsites % 8 == 0 (initial malloc is a single TrapSite), so a
 * function with 2-8 trap sites wrote past the allocation
 * (heap-buffer-overflow, ASan-verified during 17c2 verification); 17c1's
 * own tests never caught it because every 17c1 test function had exactly
 * one trap site. The 17c1 remediation (Planner t_7197bb24, commit 39fb443)
 * fixed the growth pattern (grow at nsites % 8 == 0 from the start) and
 * added 2/8/9-site regressions in trap_branch_test.c. This file therefore
 * builds each complex checked-op test function with exactly ONE obligation
 * for the dedicated per-sequence tests (mirroring 17c1's pattern), AND
 * adds a multi-site-in-one-function test (test_multi_site_checked:
 * index bounds + null deref + ptr-arith overflow) that exercises the
 * corrected site-plan growth end-to-end (Planner RULING 3 / gate
 * disposition on this card).
 */
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (test only) */

#include "trap_checked.h"

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
 * Shared construction helpers (mirror trap_branch_test.c)
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

static IrNode *mk_return(IrBuild *b, const char *file, IrNode *value)
{
    IrNode *n = mk(b, IR_RETURN, file, 5);
    if (n != NULL) {
        n->u.return_stmt.value = value;
    }
    return n;
}

static IrNode *mk_int_const(IrBuild *b, const char *file, IrType *t,
                            int64_t v)
{
    IrNode *n = mk(b, IR_INT, file, 5);
    if (n != NULL) {
        n->type = t;
        n->u.constant.value = ir_const_int(b, t, v);
    }
    return n;
}

static IrNode *mk_expr_stmt(IrBuild *b, const char *file, IrNode *expr)
{
    IrNode *n = mk(b, IR_EXPR_STMT, file, 5);
    if (n != NULL) {
        n->u.expr_stmt.expr = expr;
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
 * Pipeline helper: select + frame + call + trap_branch + trap_checked,
 * render the checked dump into an owned NUL-terminated string. When
 * out_co is non-NULL the CheckedOutput is returned (caller frees);
 * otherwise it is freed here. Mirrors trap_branch_test.c trap_dump.
 * ------------------------------------------------------------------------- */

static char *checked_dump(IrBuild *b, size_t *out_len, CheckedOutput **out_co)
{
    IselOutput *sel = NULL;
    FrameOutput *fr = NULL;
    CallOutput *co = NULL;
    TrapOutput *to = NULL;
    CheckedOutput *cho = NULL;
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
    if (trap_checked_build(b, fr, co, to, &cho) != CHK_OK || cho == NULL) {
        trap_output_free(to);
        call_output_free(co);
        frame_output_free(fr);
        isel_output_free(sel);
        return NULL;
    }
    diag_buf_init(&buf);
    if (!checked_asm_dump(cho, &buf)) {
        diag_buf_free(&buf);
        checked_output_free(cho);
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
        checked_output_free(cho);
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
    if (out_co != NULL) {
        *out_co = cho;
    } else {
        checked_output_free(cho);
    }
    trap_output_free(to);
    call_output_free(co);
    frame_output_free(fr);
    isel_output_free(sel);
    return copy;
}

/* ---------------------------------------------------------------------------
 * Builds: dedicated per-sequence tests use one checked-operation obligation
 * per function (mirroring 17c1's pattern; see the file header note on the
 * resolved 17c1 append_site defect). test_multi_site_checked uses one
 * function with THREE obligations (index bounds + null deref + ptr-arith
 * overflow) to exercise the corrected site-plan growth end-to-end
 * (Planner RULING 3 / gate disposition on this card).
 * ------------------------------------------------------------------------- */

/* f_index(s: slice<i32>, i: usize) -> i32 { s[i]; return 0; }
 * -- index bounds (AIC-R0807): cmp <index>, [<base>+8]; jae .Ltrap0 */
static IrBuild *make_index_bounds_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_sl, p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_index", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_sl = add_param_slot(b, f, "s", ir_type_slice(b, ir_type_i32(b)), 4);
    p_i = add_param_slot(b, f, "i", ir_type_usize(b), 5);
    {
        IrNode *idx = mk(b, IR_INDEX_ADDR, "test.ai", 5);
        idx->type = ir_type_ptr(b, ir_type_i32(b));
        idx->u.index_addr.base = mk_local(b, "test.ai",
            ir_type_slice(b, ir_type_i32(b)), p_sl);
        idx->u.index_addr.index = mk_local(b, "test.ai", ir_type_usize(b),
                                           p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", idx));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_deref(p: *i32) -> i32 { *p; return 0; }
 * -- null deref (AIC-R0809): test <ptr>, <ptr>; jz .Ltrap0 BEFORE the
 *    deref pointer copy */
static IrBuild *make_null_deref_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_ptr;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_deref", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_ptr = add_param_slot(b, f, "p", ir_type_ptr(b, ir_type_i32(b)), 4);
    {
        IrNode *d = mk(b, IR_DEREF, "test.ai", 5);
        d->type = ir_type_i32(b);
        d->u.deref.ptr = mk_local(b, "test.ai",
                                  ir_type_ptr(b, ir_type_i32(b)), p_ptr);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", d));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_cast(i: i64) -> i32 { cast<i8>(i); return 0; }
 * -- cast range (AIC-R0801, discovered from the IR_CAST node's trap_code):
 *    extend by source signedness, cmp against the i8 range bounds; jl/jg
 *    .Ltrap0 BEFORE the narrowing MOV */
static IrBuild *make_cast_range_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_cast", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_i = add_param_slot(b, f, "i", ir_type_i64(b), 4);
    {
        IrNode *c = mk(b, IR_CAST, "test.ai", 5);
        c->type = ir_type_i8(b);
        c->trap_code = "AIC-R0801";
        c->u.cast_wrap.value = mk_local(b, "test.ai", ir_type_i64(b), p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", c));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_slice(s: slice<i32>) -> i32 { s[1..2]; return 0; }
 * -- slice bounds (AIC-R0807): end <= base_len and start <= end BEFORE
 *    the pair construction */
static IrBuild *make_slice_bounds_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_sl;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_slice", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_sl = add_param_slot(b, f, "s", ir_type_slice(b, ir_type_i32(b)), 4);
    {
        IrNode *sl = mk(b, IR_SLICE, "test.ai", 5);
        sl->type = ir_type_slice(b, ir_type_i32(b));
        sl->u.slice.base = mk_local(b, "test.ai",
            ir_type_slice(b, ir_type_i32(b)), p_sl);
        sl->u.slice.start = mk_int_const(b, "test.ai", ir_type_usize(b), 1);
        sl->u.slice.end = mk_int_const(b, "test.ai", ir_type_usize(b), 2);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", sl));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_utf8(u: u8[]) -> i32 { cast<str>(u); return 0; }
 * -- UTF-8 validation (AIC-R0806): byte validation loop AFTER the
 *    ISEL_UTF8 marker */
static IrBuild *make_utf8_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_u8sl;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_utf8", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_u8sl = add_param_slot(b, f, "u", ir_type_slice(b, ir_type_u8(b)), 4);
    {
        IrNode *c = mk(b, IR_CAST, "test.ai", 5);
        c->type = ir_type_str(b);
        c->u.cast_wrap.value = mk_local(b, "test.ai",
            ir_type_slice(b, ir_type_u8(b)), p_u8sl);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", c));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_ptrdiff(p: *i32) -> i32 { p - p; return 0; }
 * -- pointer difference (AIC-R0810): byte diff % 4 == 0 via test-and-mask
 *    (power-of-two element size) AFTER the ISEL_PTRDIFF marker */
static IrBuild *make_ptrdiff_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_ptr;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_ptrdiff", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_ptr = add_param_slot(b, f, "p", ir_type_ptr(b, ir_type_i32(b)), 4);
    {
        IrNode *pd = mk(b, IR_PTR_DIFF, "test.ai", 5);
        pd->type = ir_type_isize(b);
        pd->u.binary.left = mk_local(b, "test.ai",
            ir_type_ptr(b, ir_type_i32(b)), p_ptr);
        pd->u.binary.right = mk_local(b, "test.ai",
            ir_type_ptr(b, ir_type_i32(b)), p_ptr);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", pd));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_pa(p: *i32, i: usize) -> i32 { p + i; return 0; }
 * -- pointer-arithmetic overflow (AIC-R0816): the obligation body is the
 *    ISEL_ADD from select_ptr_arith (the IMUL scale carries no trap);
 *    the check runs AFTER the body: jc .Ltrap0 */
static IrBuild *make_ptr_arith_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_p, p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_pa", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_p = add_param_slot(b, f, "p", ir_type_ptr(b, ir_type_i32(b)), 4);
    p_i = add_param_slot(b, f, "i", ir_type_usize(b), 5);
    {
        IrNode *pa = mk(b, IR_PTR_ADD, "test.ai", 5);
        pa->type = ir_type_ptr(b, ir_type_i32(b));
        pa->u.ptr_arith.ptr = mk_local(b, "test.ai",
            ir_type_ptr(b, ir_type_i32(b)), p_p);
        pa->u.ptr_arith.offset = mk_local(b, "test.ai", ir_type_usize(b),
                                           p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", pa));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_ss(s: str) -> i32 { s[1..2]; return 0; }
 * -- str slice (AIC-R0808): bounds + code-point boundary check. The base
 *    is a str pair image; the boundary check reads the byte at
 *    data+start and traps if it is a UTF-8 continuation byte
 *    (0x80..0xBF). */
static IrBuild *make_str_slice_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_s;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_ss", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_s = add_param_slot(b, f, "s", ir_type_str(b), 4);
    {
        IrNode *sl = mk(b, IR_SLICE, "test.ai", 5);
        sl->type = ir_type_str(b);
        sl->u.slice.base = mk_local(b, "test.ai", ir_type_str(b), p_s);
        sl->u.slice.start = mk_int_const(b, "test.ai", ir_type_usize(b), 1);
        sl->u.slice.end = mk_int_const(b, "test.ai", ir_type_usize(b), 2);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", sl));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* S3: struct of three u8 fields (size 3, not a power of two). */
static IrNode *mk_struct_s3(IrBuild *b, IrNode *mod)
{
    IrNode *decl = mk(b, IR_STRUCT_DECL, "test.ai", 3);
    IrField *fields;
    int64_t i;
    CHECK(decl != NULL);
    if (decl == NULL) {
        return NULL;
    }
    decl->u.struct_decl.name = strdup("S3");
    decl->u.struct_decl.nfields = 3;
    fields = (IrField *)calloc(3, sizeof(IrField));
    CHECK(fields != NULL);
    if (fields == NULL) {
        return decl;
    }
    decl->u.struct_decl.fields = fields;
    for (i = 0; i < 3; i++) {
        fields[i].name = strdup("x");
        fields[i].type = ir_type_u8(b);
        fields[i].span = mk_span("test.ai", 3, 8, 30 + i);
        fields[i].byte_offset = i;
    }
    decl->u.struct_decl.size = 3;
    decl->u.struct_decl.align = 1;
    ir_module_add_decl(b, mod, decl);
    return decl;
}

/* f_pd(p, q: *S3) -> i32 { p - q; return 0; }
 * -- pointer difference (AIC-R0810) with a non-power-of-two element size
 *    (S3 is 3 bytes): the byte diff uses the idiv-remainder path
 *    (sar rdx, $63; mov r11, $3; idiv r11; test rdx, rdx; jnz .Ltrap0)
 *    instead of the power-of-two test-and-mask. */
static IrBuild *make_ptrdiff_idiv_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret, *s3decl;
    IrType *s3, *ps3;
    int64_t p_p, p_q;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    s3decl = mk_struct_s3(b, mod);
    CHECK(s3decl != NULL);
    if (s3decl == NULL) {
        ir_build_free(b);
        return NULL;
    }
    s3 = ir_type_struct(b, s3decl);
    CHECK(s3 != NULL);
    if (s3 == NULL) {
        ir_build_free(b);
        return NULL;
    }
    ps3 = ir_type_ptr(b, s3);
    CHECK(ps3 != NULL);
    if (ps3 == NULL) {
        ir_build_free(b);
        return NULL;
    }
    f = mk_fn(b, "test.ai", "f_pd", ir_type_i32(b));
    body = mk_block(b, "test.ai", 4);
    p_p = add_param_slot(b, f, "p", ps3, 4);
    p_q = add_param_slot(b, f, "q", ps3, 5);
    {
        IrNode *pd = mk(b, IR_PTR_DIFF, "test.ai", 5);
        pd->type = ir_type_isize(b);
        pd->u.binary.left = mk_local(b, "test.ai", ps3, p_p);
        pd->u.binary.right = mk_local(b, "test.ai", ps3, p_q);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", pd));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_ae(a: i32[8], i: usize) -> i32 { a[i]; return 0; }
 * -- index bounds against a compile-time array extent (AIC-R0807):
 *    cmp <index>, $8; jae .Ltrap0 (the extent is a constant, not a
 *    runtime [base+8] field) */
static IrBuild *make_array_extent_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    IrType *arr;
    int64_t p_a, p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    arr = ir_type_array(b, ir_type_i32(b), 8);
    CHECK(arr != NULL);
    if (arr == NULL) {
        ir_build_free(b);
        return NULL;
    }
    f = mk_fn(b, "test.ai", "f_ae", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_a = add_param_slot(b, f, "a", arr, 4);
    p_i = add_param_slot(b, f, "i", ir_type_usize(b), 5);
    {
        IrNode *idx = mk(b, IR_INDEX_ADDR, "test.ai", 5);
        idx->type = ir_type_ptr(b, ir_type_i32(b));
        idx->u.index_addr.base = mk_local(b, "test.ai", arr, p_a);
        idx->u.index_addr.index = mk_local(b, "test.ai", ir_type_usize(b),
                                           p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", idx));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_cu(i: i64) -> i32 { cast<u8>(i); return 0; }
 * -- unsigned cast range (AIC-R0801): a single unsigned upper bound
 *    (mov r11, $255; cmp r10, r11; ja .Ltrap0) with no signed lower
 *    bound */
static IrBuild *make_cast_unsigned_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_cu", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_i = add_param_slot(b, f, "i", ir_type_i64(b), 4);
    {
        IrNode *c = mk(b, IR_CAST, "test.ai", 5);
        c->type = ir_type_u8(b);
        c->trap_code = "AIC-R0801";
        c->u.cast_wrap.value = mk_local(b, "test.ai", ir_type_i64(b), p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", c));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_cs(i: i32) -> i32 { cast<u32>(i); return 0; }
 * -- same-width signedness-change cast range (AIC-R0801): i32 -> u32
 *    (type_width equal, signedness differs): source sign-extended
 *    (movsx r10, r10d), then single unsigned upper bound
 *    (mov r11, $4294967295; cmp r10, r11; ja .Ltrap0) */
static IrBuild *make_cast_same_width_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_cs", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_i = add_param_slot(b, f, "i", ir_type_i32(b), 4);
    {
        IrNode *c = mk(b, IR_CAST, "test.ai", 5);
        c->type = ir_type_u32(b);
        c->trap_code = "AIC-R0801";
        c->u.cast_wrap.value = mk_local(b, "test.ai", ir_type_i32(b), p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", c));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* f_multi(s: slice<i32>, p: *i32, i: usize) -> i32 {
 *     s[i];    // index bounds (AIC-R0807) at line 5
 *     *p;      // null deref (AIC-R0809) at line 6
 *     p + i;   // ptr-arith overflow (AIC-R0816) at line 7
 *     return 0;
 * }
 * -- three checked-op obligations in ONE function (the multi-site
 *    scenario: site-plan growth + per-site span/cause preservation across
 *    mixed complex codes). Post-remediation regression (Planner RULING 3). */
static IrBuild *make_multi_site_build(void)
{
    IrBuild *b = ir_build_new();
    IrNode *mod, *f, *body, *ret;
    int64_t p_s, p_p, p_i;
    CHECK(b != NULL);
    if (b == NULL) {
        return NULL;
    }
    mod = mk_module(b, "test", "test.ai");
    f = mk_fn(b, "test.ai", "f_multi", ir_type_i32(b));
    body = mk_block(b, "test.ai", 3);
    p_s = add_param_slot(b, f, "s", ir_type_slice(b, ir_type_i32(b)), 4);
    p_p = add_param_slot(b, f, "p", ir_type_ptr(b, ir_type_i32(b)), 4);
    p_i = add_param_slot(b, f, "i", ir_type_usize(b), 4);
    {
        IrNode *idx = mk(b, IR_INDEX_ADDR, "test.ai", 5);
        idx->type = ir_type_ptr(b, ir_type_i32(b));
        idx->u.index_addr.base = mk_local(b, "test.ai",
            ir_type_slice(b, ir_type_i32(b)), p_s);
        idx->u.index_addr.index = mk_local(b, "test.ai", ir_type_usize(b),
                                           p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", idx));
    }
    {
        IrNode *d = mk(b, IR_DEREF, "test.ai", 6);
        d->type = ir_type_i32(b);
        d->u.deref.ptr = mk_local(b, "test.ai",
            ir_type_ptr(b, ir_type_i32(b)), p_p);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", d));
    }
    {
        IrNode *pa = mk(b, IR_PTR_ADD, "test.ai", 7);
        pa->type = ir_type_ptr(b, ir_type_i32(b));
        pa->u.ptr_arith.ptr = mk_local(b, "test.ai",
            ir_type_ptr(b, ir_type_i32(b)), p_p);
        pa->u.ptr_arith.offset = mk_local(b, "test.ai", ir_type_usize(b),
                                          p_i);
        ir_block_add_stmt(b, body, mk_expr_stmt(b, "test.ai", pa));
    }
    ret = mk_return(b, "test.ai",
                    mk_int_const(b, "test.ai", ir_type_i32(b), 0));
    ir_block_add_stmt(b, body, ret);
    f->u.function.body = body;
    ir_module_add_decl(b, mod, f);
    declare_trap_report(b, mod);
    ir_build_add_module(b, mod);
    return b;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_index_bounds_check(void)
{
    IrBuild *b = make_index_bounds_build();
    char *d = NULL;
    CheckedOutput *co = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, &co);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* the index-bounds check runs BEFORE the address computation:
     * cmp <index>, [<base>+8]; jae .Ltrap0 */
    CHECK(strstr(d, "cmp r10, [r11+8]") != NULL);
    CHECK(strstr(d, "jae .Ltrap0") != NULL);
    /* the address LEA still follows */
    CHECK(strstr(d, "lea (scaled)") != NULL);
    /* site plan: stable code + numeric code + span */
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0807 code=2055 span=test.ai:5:1")
          != NULL);
    /* span/cause preservation: the plan lists the cause chain (root
     * cause first) and the message text carries code + span + cause */
    CHECK(strstr(d, ";   cause 0: AST_MODULE_DECL at test.ai:1:1") != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0807 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    /* trap path: shadow + code + message (R8 = strlen of the message text
     * with causes = 56 bytes) + report call */
    CHECK(strstr(d, ".Ltrap0:") != NULL);
    CHECK(strstr(d, "sub rsp, $32") != NULL);
    CHECK(strstr(d, "mov rcx, $2055") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    CHECK(strstr(d, "call fn") != NULL);
    /* accessors */
    CHECK(checked_output_count(co) > 0);
    CHECK(checked_function_count(co) >= 2);   /* f_index + rt.trap.report */
    if (checked_function_count(co) >= 1) {
        const CheckedFunction *cf = checked_function_at(co, 0);
        CHECK(cf != NULL);
        if (cf != NULL) {
            CHECK(cf->nsites == 1);
            const CheckedSite *s = checked_function_site(cf, 0);
            CHECK(s != NULL);
            if (s != NULL) {
                CHECK(s->code != NULL);
                CHECK(strcmp(s->code, "AIC-R0807") == 0);
                CHECK(s->numeric_code == 2055);
                CHECK(s->span != NULL);
                CHECK(s->span->file != NULL);
                CHECK(strcmp(s->span->file, "test.ai") == 0);
                CHECK(s->span->start.line == 5);
                CHECK(s->causes != NULL);
                CHECK(s->cause_count == 1);
                if (s->cause_count >= 1) {
                    CHECK(s->causes[0].construct_kind != NULL);
                    CHECK(strcmp(s->causes[0].construct_kind,
                                 "AST_MODULE_DECL") == 0);
                }
            }
        }
    }
    free(d);
    checked_output_free(co);
    ir_build_free(b);
}

static void test_null_deref_check(void)
{
    IrBuild *b = make_null_deref_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* the pointer is tested before the deref copy: test <ptr>, <ptr>;
     * jz .Ltrap0 */
    CHECK(strstr(d, "test r10, r10") != NULL);
    CHECK(strstr(d, "jz .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0809 code=2057 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0809 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2057") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_cast_range_check(void)
{
    IrBuild *b = make_cast_range_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* cast i64 -> i8: range check against [-128, 127] BEFORE the
     * truncating MOV */
    CHECK(strstr(d, "mov r11, $-128") != NULL);
    CHECK(strstr(d, "cmp r10, r11") != NULL);
    CHECK(strstr(d, "jl .Ltrap0") != NULL);
    CHECK(strstr(d, "mov r11, $127") != NULL);
    CHECK(strstr(d, "jg .Ltrap0") != NULL);
    /* the narrowing MOV still follows */
    CHECK(strstr(d, "movb r10b, r11") != NULL);
    /* the cast-range site is discovered from the IR (17c1 cannot see it);
     * code + numeric + span + cause are preserved */
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0801 code=2049 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, ";   cause 0: AST_MODULE_DECL at test.ai:1:1") != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0801 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2049") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_slice_bounds_check(void)
{
    IrBuild *b = make_slice_bounds_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* slice bounds run BEFORE the pair construction: end <= base_len
     * (cmp r10, r11; ja) and start <= end */
    CHECK(strstr(d, "jae .Ltrap0") != NULL || strstr(d, "ja .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0807 code=2055 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0807 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_utf8_check(void)
{
    IrBuild *b = make_utf8_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* the validation loop runs after the ISEL_UTF8 marker: loop labels,
     * byte class compares, continuation-byte checks, overlong/surrogate
     * rejections */
    CHECK(strstr(d, ".Lchk0:") != NULL);
    CHECK(strstr(d, "cmp r8b, $128") != NULL);
    CHECK(strstr(d, "cmp r8b, $194") != NULL);
    CHECK(strstr(d, "cmp r8b, $224") != NULL);
    CHECK(strstr(d, "cmp r8b, $240") != NULL);
    CHECK(strstr(d, "cmp r10b, $128") != NULL);
    CHECK(strstr(d, "cmp r10b, $191") != NULL);
    CHECK(strstr(d, "cmp r8b, $224\n  jnz .Lchk5") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0806 code=2054 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0806 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2054") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    free(d);
    ir_build_free(b);
}

static void test_ptrdiff_check(void)
{
    IrBuild *b = make_ptrdiff_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* element size 4 (i32) is a power of two: the byte difference is
     * tested with a mask (and $3; jnz .Ltrap0) after the ISEL_PTRDIFF
     * marker */
    CHECK(strstr(d, "and r10, $3") != NULL);
    CHECK(strstr(d, "jnz .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0810 code=2064 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0810 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2064") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    free(d);
    ir_build_free(b);
}

/* R0816 pointer-arithmetic overflow: jc AFTER the obligation ADD. */
static void test_ptr_arith_overflow_check(void)
{
    IrBuild *b = make_ptr_arith_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* the overflow check is a single post-body branch: jc .Ltrap0 */
    CHECK(strstr(d, "jc .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0816 code=2070 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0816 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2070") != NULL);
    CHECK(strstr(d, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    free(d);
    ir_build_free(b);
}

/* R0808 str-slice code-point boundary: bounds + the byte at data+start
 * must not be a UTF-8 continuation byte (0x80..0xBF). */
static void test_str_slice_codepoint_check(void)
{
    IrBuild *b = make_str_slice_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* base length from the pair image [base+8] */
    CHECK(strstr(d, "mov r11, [r10+8]") != NULL);
    /* code-point boundary: byte at data+start tested against
     * 0x80 (jb skip) and 0xBF (ja skip); in-range means continuation
     * byte -> jmp .Ltrap0; skip label .Lchk0: */
    CHECK(strstr(d, "cmp r11b, $128") != NULL);
    CHECK(strstr(d, "jb .Lchk0") != NULL);
    CHECK(strstr(d, "cmp r11b, $191") != NULL);
    CHECK(strstr(d, "ja .Lchk0") != NULL);
    CHECK(strstr(d, "jmp .Ltrap0") != NULL);
    CHECK(strstr(d, ".Lchk0:") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0808 code=2056 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0808 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2056") != NULL);
    free(d);
    ir_build_free(b);
}

/* R0810 ptrdiff with a non-power-of-two element size (S3 = 3 bytes): the
 * idiv-remainder path instead of the test-and-mask. */
static void test_ptrdiff_idiv_check(void)
{
    IrBuild *b = make_ptrdiff_idiv_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* signed remainder: sar rdx, $63; mov r11, $3; idiv r11; test rdx;
     * jnz .Ltrap0 */
    CHECK(strstr(d, "sar rdx, $63") != NULL);
    CHECK(strstr(d, "mov r11, $3") != NULL);
    CHECK(strstr(d, "idiv r11") != NULL);
    CHECK(strstr(d, "test rdx, rdx") != NULL);
    CHECK(strstr(d, "jnz .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0810 code=2064 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0810 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2064") != NULL);
    free(d);
    ir_build_free(b);
}

/* R0807 index bounds against a compile-time array extent (cmp r10, $8). */
static void test_array_extent_index_check(void)
{
    IrBuild *b = make_array_extent_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* the extent is a constant: cmp <index>, $8; jae .Ltrap0 (no
     * [base+8] memory operand) */
    CHECK(strstr(d, "cmp r10, $8") != NULL);
    CHECK(strstr(d, "jae .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0807 code=2055 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0807 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2055") != NULL);
    free(d);
    ir_build_free(b);
}

/* R0801 cast range with an unsigned target (u8): single upper bound. */
static void test_cast_unsigned_check(void)
{
    IrBuild *b = make_cast_unsigned_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* unsigned target: mov r11, $255; cmp r10, r11; ja .Ltrap0 (no
     * signed lower bound) */
    CHECK(strstr(d, "mov r11, $255") != NULL);
    CHECK(strstr(d, "cmp r10, r11") != NULL);
    CHECK(strstr(d, "ja .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0801 code=2049 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0801 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2049") != NULL);
    free(d);
    ir_build_free(b);
}

/* R0801 cast range with a same-width signedness change (i32 -> u32):
 * source sign-extended, single unsigned upper bound. */
static void test_cast_same_width_check(void)
{
    IrBuild *b = make_cast_same_width_build();
    char *d = NULL;
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    d = checked_dump(b, NULL, NULL);
    CHECK(d != NULL);
    if (d == NULL) {
        ir_build_free(b);
        return;
    }
    /* same-width signedness change: movsx r10, r10d; mov r11,
     * $4294967295; cmp r10, r11; ja .Ltrap0 */
    CHECK(strstr(d, "movsx r10, r10d") != NULL);
    CHECK(strstr(d, "mov r11, $4294967295") != NULL);
    CHECK(strstr(d, "cmp r10, r11") != NULL);
    CHECK(strstr(d, "ja .Ltrap0") != NULL);
    CHECK(strstr(d, ";   .Ltrap0 AIC-R0801 code=2049 span=test.ai:5:1")
          != NULL);
    CHECK(strstr(d, "; .Lmsg0 = \"AIC-R0801 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
    CHECK(strstr(d, "mov rcx, $2049") != NULL);
    free(d);
    ir_build_free(b);
}

/* Multi-site regression (Planner RULING 3): three checked-op obligations
 * (index bounds + null deref + ptr-arith overflow) in ONE function. Each
 * site keeps its own span + cause chain; the dump shows all three check
 * sequences, all three trap paths, and a sites=3 plan. */
static void test_multi_site_checked(void)
{
    IrBuild *b1 = make_multi_site_build();
    IrBuild *b2 = make_multi_site_build();
    CheckedOutput *co = NULL;
    char *d1 = NULL, *d2 = NULL;
    size_t n1 = 0, n2 = 0;
    CHECK(b1 != NULL && b2 != NULL);
    if (b1 == NULL || b2 == NULL) {
        free(d1);
        free(d2);
        checked_output_free(co);
        ir_build_free(b1);
        ir_build_free(b2);
        return;
    }
    d1 = checked_dump(b1, &n1, &co);
    d2 = checked_dump(b2, &n2, NULL);
    CHECK(d1 != NULL && d2 != NULL);
    if (d1 != NULL && d2 != NULL) {
        CHECK(n1 == n2);
        CHECK(memcmp(d1, d2, n1) == 0);
    }
    if (d1 != NULL) {
        /* three check sequences in stream order */
        CHECK(strstr(d1, "cmp r10, [r11+8]") != NULL);   /* index */
        CHECK(strstr(d1, "jae .Ltrap0") != NULL);
        CHECK(strstr(d1, "test r10, r10") != NULL);      /* null deref */
        CHECK(strstr(d1, "jz .Ltrap1") != NULL);
        CHECK(strstr(d1, "jc .Ltrap2") != NULL);         /* ptr-arith */
        /* three site-plan lines with span + causes */
        CHECK(strstr(d1, ";   .Ltrap0 AIC-R0807 code=2055 span=test.ai:5:1")
              != NULL);
        CHECK(strstr(d1, ";   .Ltrap1 AIC-R0809 code=2057 span=test.ai:6:1")
              != NULL);
        CHECK(strstr(d1, ";   .Ltrap2 AIC-R0816 code=2070 span=test.ai:7:1")
              != NULL);
        CHECK(strstr(d1, ";   cause 0: AST_MODULE_DECL at test.ai:1:1")
              != NULL);
        /* three messages (code + span + cause) */
        CHECK(strstr(d1, "; .Lmsg0 = \"AIC-R0807 at test.ai:5:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
        CHECK(strstr(d1, "; .Lmsg1 = \"AIC-R0809 at test.ai:6:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
        CHECK(strstr(d1, "; .Lmsg2 = \"AIC-R0816 at test.ai:7:1; "
                     "AST_MODULE_DECL at test.ai:1:1\"") != NULL);
        /* three trap paths */
        CHECK(strstr(d1, ".Ltrap0:") != NULL);
        CHECK(strstr(d1, ".Ltrap1:") != NULL);
        CHECK(strstr(d1, ".Ltrap2:") != NULL);
        CHECK(strstr(d1, "lea rdx, [.Lmsg0]\n  mov r8, $56") != NULL);
    }
    if (co != NULL) {
        CHECK(checked_function_count(co) >= 2);
        if (checked_function_count(co) >= 1) {
            const CheckedFunction *cf = checked_function_at(co, 0);
            CHECK(cf != NULL);
            if (cf != NULL) {
                /* f_multi carries exactly the three obligations */
                CHECK(cf->nsites == 3);
                if (cf->nsites >= 3) {
                    const CheckedSite *s0 = checked_function_site(cf, 0);
                    const CheckedSite *s1 = checked_function_site(cf, 1);
                    const CheckedSite *s2 = checked_function_site(cf, 2);
                    CHECK(s0 != NULL && s1 != NULL && s2 != NULL);
                    if (s0 != NULL && s1 != NULL && s2 != NULL) {
                        CHECK(s0->site_index == 0);
                        CHECK(s0->code != NULL);
                        CHECK(strcmp(s0->code, "AIC-R0807") == 0);
                        CHECK(s0->numeric_code == 2055);
                        CHECK(s0->span != NULL);
                        CHECK(s0->span->start.line == 5);
                        CHECK(s0->cause_count == 1);
                        CHECK(s1->site_index == 1);
                        CHECK(strcmp(s1->code, "AIC-R0809") == 0);
                        CHECK(s1->numeric_code == 2057);
                        CHECK(s1->span->start.line == 6);
                        CHECK(s1->cause_count == 1);
                        CHECK(s2->site_index == 2);
                        CHECK(strcmp(s2->code, "AIC-R0816") == 0);
                        CHECK(s2->numeric_code == 2070);
                        CHECK(s2->span->start.line == 7);
                        CHECK(s2->cause_count == 1);
                    }
                }
            }
        }
    }
    free(d1);
    free(d2);
    checked_output_free(co);
    ir_build_free(b1);
    ir_build_free(b2);
}

static void test_site_plan_and_determinism(void)
{
    IrBuild *b1 = make_index_bounds_build();
    IrBuild *b2 = make_index_bounds_build();
    IrBuild *bd = make_null_deref_build();
    CheckedOutput *co1 = NULL;
    char *d1 = NULL, *d2 = NULL, *dd = NULL;
    size_t n1 = 0, n2 = 0, nd = 0;
    CHECK(b1 != NULL && b2 != NULL && bd != NULL);
    if (b1 == NULL || b2 == NULL || bd == NULL) {
        free(d1);
        free(d2);
        free(dd);
        checked_output_free(co1);
        ir_build_free(b1);
        ir_build_free(b2);
        ir_build_free(bd);
        return;
    }
    d1 = checked_dump(b1, &n1, &co1);
    d2 = checked_dump(b2, &n2, NULL);
    dd = checked_dump(bd, &nd, NULL);
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
        CHECK(strstr(d1, "; AI-Co checked-op emission dump "
                         "(WP-M0-17c2, deterministic)") != NULL);
        CHECK(strstr(d1, "; function ") != NULL);
    }
    if (co1 != NULL) {
        CHECK(checked_function_count(co1) >= 2);
        CHECK(checked_output_count(co1) > 0);
        CHECK(checked_message(co1, 0) != NULL);
    }
    free(d1);
    free(d2);
    free(dd);
    checked_output_free(co1);
    ir_build_free(b1);
    ir_build_free(b2);
    ir_build_free(bd);
}

int main(void)
{
    test_index_bounds_check();
    fprintf(stderr, "after test_index_bounds_check\n");
    test_null_deref_check();
    fprintf(stderr, "after test_null_deref_check\n");
    test_cast_range_check();
    fprintf(stderr, "after test_cast_range_check\n");
    test_slice_bounds_check();
    fprintf(stderr, "after test_slice_bounds_check\n");
    test_utf8_check();
    fprintf(stderr, "after test_utf8_check\n");
    test_ptrdiff_check();
    fprintf(stderr, "after test_ptrdiff_check\n");
    test_ptr_arith_overflow_check();
    fprintf(stderr, "after test_ptr_arith_overflow_check\n");
    test_str_slice_codepoint_check();
    fprintf(stderr, "after test_str_slice_codepoint_check\n");
    test_ptrdiff_idiv_check();
    fprintf(stderr, "after test_ptrdiff_idiv_check\n");
    test_array_extent_index_check();
    fprintf(stderr, "after test_array_extent_index_check\n");
    test_cast_unsigned_check();
    fprintf(stderr, "after test_cast_unsigned_check\n");
    test_cast_same_width_check();
    fprintf(stderr, "after test_cast_same_width_check\n");
    test_multi_site_checked();
    fprintf(stderr, "after test_multi_site_checked\n");
    test_site_plan_and_determinism();
    fprintf(stderr, "after test_site_plan_and_determinism\n");
    fprintf(stderr, "trap_checked_test: %d checks, %d failures\n",
            g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
