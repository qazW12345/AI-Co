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
 * NOTE (17c1 dependency defect): the committed 17c1 trap_branch.c
 * append_site grows the per-function site array only when nsites % 8 == 0
 * (initial malloc is a single TrapSite), so a function with 2-8 trap
 * sites writes past the allocation (heap-buffer-overflow, ASan-verified
 * during 17c2 verification). 17c1's own tests never caught it because
 * every 17c1 test function has exactly one trap site (the 17c1
 * determinism test even asserts nsites == 1). 17c2 therefore builds each
 * complex checked-op test function with exactly ONE obligation (the same
 * pattern 17c1 used) so the pipeline is valid under the committed 17c1
 * code; the multi-site scenario is escalated to the Planner as a backend
 * constraint (17c1 trap_branch.c append_site growth defect) so WP-M0-18/19
 * can depend on a fixed site plan.
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
 * Builds: one checked-operation obligation per function (see file header
 * for why: the committed 17c1 site plan grows only at nsites % 8 == 0, so
 * multi-site functions corrupt the heap; 17c2 tests mirror 17c1's
 * single-obligation-per-function pattern).
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
    test_site_plan_and_determinism();
    fprintf(stderr, "after test_site_plan_and_determinism\n");
    fprintf(stderr, "trap_checked_test: %d checks, %d failures\n",
            g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
