/* bootstrap/src/sema/expr_ops_test.c
 *
 * WP-M0-13b2 operator semantics tests: the checked-arithmetic
 * compile-time decision model (spec sec. 11.3), the comparison
 * semantics model (sec. 11.4), and the operator-site failure-record
 * emission at the const-context sites owned by WP-M0-13b1 (array type
 * extents sec. 12.1, switch case labels sec. 13.2, local const
 * declaration initializers sec. 8.1 - AIC-E0405..E0411).
 *
 * The decision/comparison tests are pure model tests (no pipeline):
 * they exercise expr_op_binary_decision / expr_op_neg_decision /
 * expr_op_shift_value / expr_cmp_kind_of / expr_cmp_ints /
 * expr_cmp_bytes / expr_cmp_slice_equal / expr_cmp_addr directly with
 * hand-built EvalInt values and Type descriptors. The operator-site
 * tests run the full pipeline through layout/convert/optype/const_eval
 * (12a) / rec_fail_emit (12b2) / expr_core (13b1) / expr_ops (13b2) so
 * the boundary tests can prove 13b2 emits only its own sites' records
 * and closes the 13b1 routing gap without duplicating 12b2's records.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-sema-b2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/expr_ops_test.c \
 *     bootstrap/src/sema/expr_ops.c \
 *     bootstrap/src/sema/expr_core.c \
 *     bootstrap/src/const/eval_fail_rec.c \
 *     bootstrap/src/const/eval_fail_arith.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-sema-b2/expr_ops_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-sema-b2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "expr_ops.h"
#include "expr_core.h"

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
 * EvalInt helpers (the WP-M0-12a conventions: big marks values in
 * [2^63, 2^64-1] stored two's complement)
 * ------------------------------------------------------------------------- */

static EvalInt ei(int64_t v)
{
    EvalInt r;
    r.v = v;
    r.big = false;
    return r;
}

static EvalInt eiu(uint64_t v)
{
    EvalInt r;
    r.v = (int64_t)v;
    r.big = v >= ((uint64_t)1 << 63);
    return r;
}

static void check_int_eq(EvalInt got, int64_t want_v, bool want_big,
                         const char *what)
{
    CHECK(got.v == want_v);
    CHECK(got.big == want_big);
    if (got.v != want_v || got.big != want_big) {
        fprintf(stderr, "  %s: got v=%lld big=%d\n", what,
                (long long)got.v, (int)got.big);
    }
}

/* ---------------------------------------------------------------------------
 * 1. Checked-arithmetic decisions: add / sub (spec sec. 11.3)
 * ------------------------------------------------------------------------- */

static void test_decision_add_sub(void)
{
    /* signed add: overflow at the range edges */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(127), ei(1), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(-128), ei(-1), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(100), ei(28), 8, true)
          == EXPR_OP_OVERFLOW);   /* 128 > 127 */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(100), ei(27), 8, true)
          == EXPR_OP_OK);         /* 127 fits */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(-128), ei(127), 8, true)
          == EXPR_OP_OK);
    /* signed add at 64-bit width */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(INT64_MAX), ei(1), 64, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(INT64_MIN), ei(-1), 64, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(INT64_MAX), ei(0), 64, true)
          == EXPR_OP_OK);
    /* unsigned add: wrap-around is rejected (no silent wrap) */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(255), ei(1), 8, false)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(200), ei(56), 8, false)
          == EXPR_OP_OVERFLOW);   /* 256 > 255 */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(200), ei(55), 8, false)
          == EXPR_OP_OK);         /* 255 fits */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, eiu(UINT64_MAX), ei(1), 64,
                                  false) == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, eiu(UINT64_MAX), ei(0), 64,
                                  false) == EXPR_OP_OK);
    /* signed sub */
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(-128), ei(1), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(127), ei(-1), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(-128), ei(-1), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(0), ei(127), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(INT64_MIN), ei(1), 64, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(INT64_MAX), ei(-1), 64, true)
          == EXPR_OP_OVERFLOW);
    /* unsigned sub: result would wrap */
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(0), ei(1), 8, false)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(5), ei(5), 8, false)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(5), ei(3), 8, false)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_SUB, ei(0), ei(1), 64, false)
          == EXPR_OP_OVERFLOW);
    /* decision -> code mapping */
    CHECK(expr_op_decision_code(EXPR_OP_OK) == NULL);
    CHECK(expr_op_decision_code(EXPR_OP_OVERFLOW) &&
          strcmp(expr_op_decision_code(EXPR_OP_OVERFLOW), "AIC-E0405") == 0);
    CHECK(expr_op_decision_code(EXPR_OP_DIV_ZERO) &&
          strcmp(expr_op_decision_code(EXPR_OP_DIV_ZERO), "AIC-E0406") == 0);
    CHECK(expr_op_decision_code(EXPR_OP_SHIFT_RANGE) &&
          strcmp(expr_op_decision_code(EXPR_OP_SHIFT_RANGE), "AIC-E0407") == 0);
    /* defensive width */
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(1), ei(1), 0, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_ADD, ei(1), ei(1), 65, true)
          == EXPR_OP_OK);
}

/* ---------------------------------------------------------------------------
 * 2. Checked-arithmetic decisions: mul (spec sec. 11.3)
 * ------------------------------------------------------------------------- */

static void test_decision_mul(void)
{
    /* signed mul, i8 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(100), ei(2), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(-128), ei(-1), 8, true)
          == EXPR_OP_OVERFLOW);    /* 128 > i8 max */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(-128), ei(1), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(11), ei(11), 8, true)
          == EXPR_OP_OK);          /* 121 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(12), ei(11), 8, true)
          == EXPR_OP_OVERFLOW);    /* 132 */
    /* signed mul, i16 exact boundary */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(181), ei(181), 16, true)
          == EXPR_OP_OK);          /* 32761 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(182), ei(182), 16, true)
          == EXPR_OP_OVERFLOW);    /* 33124 */
    /* signed mul, i64 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(INT64_MIN), ei(-1), 64, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(INT64_MIN), ei(1), 64, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(INT64_MAX), ei(2), 64, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(1000000000), ei(1000000000),
                                  64, true) == EXPR_OP_OK);      /* 1e18 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(4000000000), ei(4000000000),
                                  64, true) == EXPR_OP_OVERFLOW); /* 1.6e19 */
    /* unsigned mul */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(16), ei(16), 8, false)
          == EXPR_OP_OVERFLOW);    /* 256 > 255 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(15), ei(17), 8, false)
          == EXPR_OP_OK);          /* 255 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(16), ei(15), 8, false)
          == EXPR_OP_OK);          /* 240 */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, eiu(UINT64_MAX), ei(2), 64,
                                  false) == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_MUL, eiu(UINT64_MAX), ei(0), 64,
                                  false) == EXPR_OP_OK);
    /* zero never overflows */
    CHECK(expr_op_binary_decision(AST_BIN_MUL, ei(0), ei(INT64_MIN), 64, true)
          == EXPR_OP_OK);
}

/* ---------------------------------------------------------------------------
 * 3. Checked-arithmetic decisions: div / mod (spec sec. 11.3)
 * ------------------------------------------------------------------------- */

static void test_decision_div_mod(void)
{
    /* division by zero */
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(5), ei(0), 8, true)
          == EXPR_OP_DIV_ZERO);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(5), ei(0), 8, false)
          == EXPR_OP_DIV_ZERO);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, eiu(UINT64_MAX), ei(0), 64,
                                  false) == EXPR_OP_DIV_ZERO);
    CHECK(expr_op_binary_decision(AST_BIN_MOD, ei(5), ei(0), 8, true)
          == EXPR_OP_DIV_ZERO);
    CHECK(expr_op_binary_decision(AST_BIN_MOD, ei(5), ei(0), 8, false)
          == EXPR_OP_DIV_ZERO);
    /* signed min / -1 and min % -1 are checked overflow (sec. 11.3) */
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(-128), ei(-1), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_MOD, ei(-128), ei(-1), 8, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(INT64_MIN), ei(-1), 64, true)
          == EXPR_OP_OVERFLOW);
    CHECK(expr_op_binary_decision(AST_BIN_MOD, ei(INT64_MIN), ei(-1), 64, true)
          == EXPR_OP_OVERFLOW);
    /* valid divisions */
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(-128), ei(1), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(127), ei(-1), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(-7), ei(2), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_MOD, ei(5), ei(2), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, ei(255), ei(5), 8, false)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_DIV, eiu(UINT64_MAX), ei(1), 64,
                                  false) == EXPR_OP_OK);
}

/* ---------------------------------------------------------------------------
 * 4. Checked-arithmetic decisions: unary negation and shifts (sec. 11.3)
 * ------------------------------------------------------------------------- */

static void test_decision_neg_shift(void)
{
    /* unary negation of the signed minimum is checked overflow */
    CHECK(expr_op_neg_decision(ei(-128), 8, true) == EXPR_OP_OVERFLOW);
    CHECK(expr_op_neg_decision(ei(INT64_MIN), 64, true) == EXPR_OP_OVERFLOW);
    CHECK(expr_op_neg_decision(ei(-127), 8, true) == EXPR_OP_OK);
    CHECK(expr_op_neg_decision(ei(0), 8, true) == EXPR_OP_OK);
    CHECK(expr_op_neg_decision(ei(127), 8, true) == EXPR_OP_OK);
    CHECK(expr_op_neg_decision(ei(5), 64, true) == EXPR_OP_OK);
    /* unsigned negation is 11d's rejection; the decision API is
     * defensively OK (never reached on a valid build) */
    CHECK(expr_op_neg_decision(ei(5), 8, false) == EXPR_OP_OK);
    /* shift count must be in 0..width-1 */
    CHECK(expr_op_binary_decision(AST_BIN_SHL, ei(1), ei(8), 8, true)
          == EXPR_OP_SHIFT_RANGE);
    CHECK(expr_op_binary_decision(AST_BIN_SHL, ei(1), ei(-1), 8, true)
          == EXPR_OP_SHIFT_RANGE);
    CHECK(expr_op_binary_decision(AST_BIN_SHR, ei(1), ei(32), 32, true)
          == EXPR_OP_SHIFT_RANGE);
    CHECK(expr_op_binary_decision(AST_BIN_SHR, ei(1), eiu((uint64_t)1 << 63),
                                  64, true) == EXPR_OP_SHIFT_RANGE);
    CHECK(expr_op_binary_decision(AST_BIN_SHL, ei(1), ei(7), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_SHL, ei(1), ei(0), 8, true)
          == EXPR_OP_OK);
    /* bitwise operators never fail per sec. 11.3 */
    CHECK(expr_op_binary_decision(AST_BIN_BAND, ei(1), ei(2), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_BXOR, ei(1), ei(2), 8, true)
          == EXPR_OP_OK);
    CHECK(expr_op_binary_decision(AST_BIN_BOR, ei(1), ei(2), 8, true)
          == EXPR_OP_OK);

    /* defined shift result semantics: left shift of signed values is
     * defined on the two's-complement bit pattern */
    check_int_eq(expr_op_shift_value(AST_BIN_SHL, ei(-1), ei(1), 8, true),
                 -2, false, "i8 -1 << 1");
    check_int_eq(expr_op_shift_value(AST_BIN_SHL, ei(1), ei(7), 8, true),
                 -128, false, "i8 1 << 7");
    check_int_eq(expr_op_shift_value(AST_BIN_SHL, eiu(0xFF), ei(1), 8, false),
                 254, false, "u8 255 << 1");
    /* right shift of signed values is arithmetic (sign-extending) */
    check_int_eq(expr_op_shift_value(AST_BIN_SHR, ei(-128), ei(1), 8, true),
                 -64, false, "i8 -128 >> 1");
    check_int_eq(expr_op_shift_value(AST_BIN_SHR, ei(-1), ei(3), 8, true),
                 -1, false, "i8 -1 >> 3");
    check_int_eq(expr_op_shift_value(AST_BIN_SHR, ei(-2), ei(1), 64, true),
                 -1, false, "i64 -2 >> 1");
    /* right shift of unsigned is logical */
    check_int_eq(expr_op_shift_value(AST_BIN_SHR, eiu(0xFF), ei(1), 8, false),
                 127, false, "u8 255 >> 1");
    check_int_eq(expr_op_shift_value(AST_BIN_SHR, eiu(0x8000000000000000ULL),
                                     ei(63), 64, false),
                 1, false, "u64 2^63 >> 63");
    /* defensive out-of-range count returns the operand unchanged */
    check_int_eq(expr_op_shift_value(AST_BIN_SHL, ei(5), ei(9), 8, true),
                 5, false, "defensive shift");
}

/* ---------------------------------------------------------------------------
 * 5. Comparison mechanism per operand type (spec sec. 11.4)
 * ------------------------------------------------------------------------- */

static void test_cmp_kind_model(void)
{
    Type *t;
    CHECK(expr_cmp_kind_of(NULL) == EXPR_CMP_INT_MATH);

    t = type_prim_new(AST_PRIM_I32);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_INT_MATH);
    type_free(t);
    t = type_prim_new(AST_PRIM_U8);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_INT_MATH);
    type_free(t);
    t = type_prim_new(AST_PRIM_BOOL);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_BOOL_VALUE);
    type_free(t);
    t = type_prim_new(AST_PRIM_STR);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_STR_BYTES);
    type_free(t);
    /* void is never compared on a valid build (11d rejects); the
     * mechanism is defensively INT_MATH */
    t = type_prim_new(AST_PRIM_VOID);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_INT_MATH);
    type_free(t);

    /* enum: by underlying integer value (sym is borrowed and unused by
     * the mechanism query; NULL is defensive) */
    t = type_enum_new(NULL);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_ENUM_UNDERLYING);
    type_free(t);

    t = type_ptr_new(type_prim_new(AST_PRIM_I32));
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_PTR_ADDR);
    type_free(t);

    t = type_slice_new(type_prim_new(AST_PRIM_U8));
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_SLICE_ELEMS);
    type_free(t);

    /* array/struct comparisons are rejected by 11d (AIC-T0304); the
     * mechanism is defensively INT_MATH */
    t = type_array_new(type_prim_new(AST_PRIM_I32), 4);
    CHECK(expr_cmp_kind_of(t) == EXPR_CMP_INT_MATH);
    type_free(t);
}

/* ---------------------------------------------------------------------------
 * 6. Comparison values (spec sec. 11.4)
 * ------------------------------------------------------------------------- */

static bool int_elem_eq(const void *x, const void *y, void *ud)
{
    int32_t a, b;
    (void)ud;
    memcpy(&a, x, sizeof(a));
    memcpy(&b, y, sizeof(b));
    return a == b;
}

static void test_cmp_values(void)
{
    int32_t a3[3] = { 1, 2, 3 };
    int32_t b3[3] = { 1, 2, 3 };
    int32_t c3[3] = { 1, 2, 4 };
    int32_t a2[2] = { 1, 2 };
    static int dummy_a, dummy_b;
    int out;

    /* integer comparisons: mathematical value on the actual type's
     * range (i8: -128 < -1 < 0 < 127) */
    CHECK(expr_cmp_ints(ei(-1), ei(1), 8, true) < 0);
    CHECK(expr_cmp_ints(ei(-128), ei(127), 8, true) < 0);
    CHECK(expr_cmp_ints(ei(127), ei(127), 8, true) == 0);
    CHECK(expr_cmp_ints(ei(0), ei(-1), 8, true) > 0);
    CHECK(expr_cmp_ints(ei(INT64_MIN), ei(INT64_MAX), 64, true) < 0);
    /* unsigned: big values (>= 2^63) compare above every non-big value */
    CHECK(expr_cmp_ints(eiu(UINT64_MAX), ei(0), 64, false) > 0);
    CHECK(expr_cmp_ints(eiu(UINT64_MAX), eiu(UINT64_MAX), 64, false) == 0);
    CHECK(expr_cmp_ints(eiu((uint64_t)1 << 63), ei(INT64_MAX), 64, false) > 0);
    CHECK(expr_cmp_ints(eiu(5), eiu(255), 8, false) < 0);
    /* bool ==/!= by value (the underlying 0/1 ordering) */
    CHECK(expr_cmp_ints(ei(0), ei(1), 8, true) < 0);
    CHECK(expr_cmp_ints(ei(1), ei(1), 8, true) == 0);
    /* enum by underlying integer value: the mechanism delegates to the
     * underlying type's mathematical comparison (tested above) */

    /* str: lexicographic byte-by-byte comparison */
    CHECK(expr_cmp_bytes("abc", 3, "abd", 3) < 0);
    CHECK(expr_cmp_bytes("abc", 3, "abc", 3) == 0);
    CHECK(expr_cmp_bytes("ab", 2, "abc", 3) < 0);
    CHECK(expr_cmp_bytes("abc", 3, "ab", 2) > 0);
    CHECK(expr_cmp_bytes("", 0, "a", 1) < 0);
    CHECK(expr_cmp_bytes(NULL, 0, NULL, 0) == 0);
    CHECK(expr_cmp_bytes("\xC3\xA9", 2, "\xC3\xA8", 2) > 0); /* U+00E9 > U+00E8
                                                              * bytewise */

    /* slices: element-wise equality, length mismatch -> not equal */
    CHECK(expr_cmp_slice_equal(3, a3, sizeof(int32_t), 3, b3,
                               int_elem_eq, NULL));
    CHECK(!expr_cmp_slice_equal(3, a3, sizeof(int32_t), 3, c3,
                                int_elem_eq, NULL));
    CHECK(!expr_cmp_slice_equal(3, a3, sizeof(int32_t), 2, a2,
                                int_elem_eq, NULL));
    CHECK(expr_cmp_slice_equal(0, NULL, sizeof(int32_t), 0, NULL,
                               int_elem_eq, NULL));

    /* pointers: byte addresses as unsigned; same-object ordering by
     * byte offset (the address ordering within the object) */
    out = 99;
    CHECK(expr_cmp_addr((const NameSymbol *)&dummy_a, 5,
                        (const NameSymbol *)&dummy_a, 10, &out));
    CHECK(out < 0);
    CHECK(expr_cmp_addr((const NameSymbol *)&dummy_a, 10,
                        (const NameSymbol *)&dummy_a, 10, &out));
    CHECK(out == 0);
    CHECK(expr_cmp_addr((const NameSymbol *)&dummy_a, 10,
                        (const NameSymbol *)&dummy_a, 5, &out));
    CHECK(out > 0);
    /* distinct objects: defined as unsigned byte-address comparison at
     * link/runtime time; not decided at compile time */
    CHECK(!expr_cmp_addr((const NameSymbol *)&dummy_a, 0,
                         (const NameSymbol *)&dummy_b, 0, &out));
}

/* ---------------------------------------------------------------------------
 * Full pipeline: load -> lex -> parse -> name_resolve -> completeness ->
 * layout -> convert -> optype -> const_eval_check (12a) ->
 * rec_fail_emit (12b2) -> expr_core_check (13b1) -> expr_ops_check (13b2).
 * The 12b2 and 13b1 stages run so the boundary tests can prove 13b2
 * does not duplicate their records.
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
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
    DiagRecord **crecs;     /* convert records (AIC-T0307) */
    size_t crn;
    ConvertStatus cst;
    DiagRecord **orecs;     /* optype records (AIC-T03xx) */
    size_t orn;
    OptypeStatus ost;
    DiagRecord **erecs;     /* 12a const records (AIC-E0401) */
    size_t ern;
    EvalFailureSite *efails;
    size_t efailn;
    ConstEvalStatus esc;
    DiagRecord **rrecs;     /* 12b2 records (AIC-E0405..E0411 at 12a sites) */
    size_t rrn;
    RecFailStatus rsc;
    DiagRecord **xrecs;     /* 13b1 records (AIC-E0401 at 13b1 sites) */
    size_t xrn;
    EvalFailureSite *xfails;
    size_t xfailn;
    ExprCoreStatus xsc;
    DiagRecord **opsrecs;   /* 13b2 records (AIC-E0405..E0411 at 13b1 sites) */
    size_t opsrn;
    ExprOpsStatus osc;
} Pipeline;

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
    /* LAYOUT_UNEVALUABLE is a routing signal, not a rejection: the
     * const stages own the extent evaluation (layout.h boundary note). */
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR &&
        p->lst != LAYOUT_UNEVALUABLE) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    if (p->ost == OPTYPE_DIAG_ERROR) return;
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
    p->rsc = rec_fail_emit(p->result, p->build, &p->rrecs, &p->rrn);
    p->xsc = expr_core_check(p->result, p->build, &p->xrecs, &p->xrn,
                             &p->xfails, &p->xfailn);
    p->osc = expr_ops_check(p->result, p->build, &p->opsrecs, &p->opsrn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    layout_build_free(p->build);
    types_records_free(p->lrecs, p->lrn);
    types_records_free(p->crecs, p->crn);
    types_records_free(p->orecs, p->orn);
    types_records_free(p->erecs, p->ern);
    free(p->efails);
    types_records_free(p->rrecs, p->rrn);
    types_records_free(p->xrecs, p->xrn);
    free(p->xfails);
    types_records_free(p->opsrecs, p->opsrn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* Check a failure record's code, phase/severity/recovery, and that its
 * primary span starts exactly at `marker` (and, when len > 0, spans
 * exactly `len` bytes). */
static void check_fail_span(const DiagRecord *r, const char *src,
                            const char *code, const char *marker,
                            int64_t len)
{
    const char *hit;
    int64_t off;
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->code && strcmp(r->code, code) == 0);
    CHECK(r->severity && strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(r->phase && strcmp(r->phase, DIAG_PHASE_SEMANTIC) == 0);
    CHECK(r->recovery &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    CHECK(r->message != NULL && r->message[0] != '\0');
    CHECK(r->primary_span != NULL);
    if (!r->primary_span) return;
    hit = strstr(src, marker);
    CHECK(hit != NULL);
    if (!hit) return;
    off = (int64_t)(hit - src);
    CHECK(r->primary_span->start.offset == off);
    if (len > 0) CHECK(r->primary_span->end.offset == off + len);
}

/* ---------------------------------------------------------------------------
 * 7. Operator-site records at the 13b1-owned const-context sites
 *    (spec sec. 11.3-11.6 / sec. 18.4; the 13b1 routing gap)
 * ------------------------------------------------------------------------- */

static void test_ops_check_sites(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  const A: i32 = 1 / 0;\n"                  /* E0406, site span */
        "  const B: i8 = cast<i8>(127) + cast<i8>(1);\n" /* E0405, site span */
        "  const C: i32 = 1 << 40;\n"                /* E0407, shift span */
        "  const D: i8 = cast<i8>(200);\n"           /* E0408, cast span */
        "  return A;\n"
        "}\n"
        "fn g(x: i32) -> i32 {\n"
        "  var a: i32[5 / 0];\n"                     /* E0406, extent span */
        "  switch (x) {\n"
        "    case 7 / 0: { return 0; }\n"            /* E0406, label span */
        "    default: { return 1; }\n"
        "  }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.osc == EXPR_OPS_DIAG_ERROR);
    CHECK(p.opsrn == 6);
    if (p.opsrn != 6) { pipeline_free(&p); return; }
    /* records sorted by the contract sec. 9 comparator (offset order) */
    check_fail_span(p.opsrecs[0], src, "AIC-E0406", "1 / 0", 5);
    CHECK(p.opsrecs[0]->message &&
          strcmp(p.opsrecs[0]->message, "constant division by zero") == 0);
    check_fail_span(p.opsrecs[1], src, "AIC-E0405",
                    "cast<i8>(127) + cast<i8>(1)", 27);
    CHECK(p.opsrecs[1]->message &&
          strcmp(p.opsrecs[1]->message, "constant expression overflow") == 0);
    check_fail_span(p.opsrecs[2], src, "AIC-E0407", "1 << 40", 7);
    CHECK(p.opsrecs[2]->message &&
          strcmp(p.opsrecs[2]->message,
                 "constant shift count out of range: 40 exceeds i32 bit "
                 "width") == 0);
    check_fail_span(p.opsrecs[3], src, "AIC-E0408", "cast<i8>(200)", 13);
    CHECK(p.opsrecs[3]->message &&
          strcmp(p.opsrecs[3]->message,
                 "constant cast out of range: 200 does not fit in i8") == 0);
    check_fail_span(p.opsrecs[4], src, "AIC-E0406", "5 / 0", 5);
    check_fail_span(p.opsrecs[5], src, "AIC-E0406", "7 / 0", 5);
    /* 13b1 produces no E0401 at these sites (the compositions hold) */
    CHECK(p.xsc == EXPR_CORE_FAILURE);
    CHECK(p.xrn == 0);
    CHECK(p.xfailn == 6);
    /* 12b2 produces nothing for these (they are not 12a's sites) */
    CHECK(p.rsc == REC_FAIL_OK);
    CHECK(p.rrn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 8. Boundaries: 13b2 emits only its own sites' records
 * ------------------------------------------------------------------------- */

/* 12a-owned sites (global const/var initializers, enum member values)
 * are 12b2's records; 13b2 must produce none. */
static void test_ops_check_boundary_12a(void)
{
    static const char src[] =
        "module main;\n"
        "const G: i32 = 1 / 0;\n"          /* 12a site -> 12b2 E0406 */
        "var v: i32 = 1 << 40;\n"          /* 12a site -> 12b2 E0407 */
        "enum E: i8 { A = cast<i8>(200) }\n" /* 12a site -> 12b2 E0408 */
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc == REC_FAIL_FAILURE);
    CHECK(p.rrn == 3);
    CHECK(p.osc == EXPR_OPS_OK);
    CHECK(p.opsrn == 0);            /* no 13b1-owned site fails */
    CHECK(p.xsc == EXPR_CORE_OK);
    CHECK(p.xrn == 0);
    pipeline_free(&p);
}

/* Non-const sites at the 13b1-owned const-context positions are 13b1's
 * AIC-E0401; 13b2 must produce no record for them. */
static void test_ops_check_boundary_not_const(void)
{
    static const char src[] =
        "module main;\n"
        "var n: i32 = 5;\n"
        "fn f(x: i32) -> i32 {\n"
        "  const C: i32 = n;\n"          /* 13b1 E0401 (not const) */
        "  switch (x) {\n"
        "    case n: { return 0; }\n"    /* 13b1 E0401 */
        "    default: { return 1; }\n"
        "  }\n"
        "  var a: i32[n];\n"             /* 13b1 E0401 (extent) */
        "  return C;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_DIAG_ERROR);
    CHECK(p.xrn == 3);
    CHECK(p.osc == EXPR_OPS_OK);
    CHECK(p.opsrn == 0);            /* composition violation, not failure */
    pipeline_free(&p);
}

/* The 13b1 routing gap (acceptance criterion: no expression semantic
 * rule silently passes an invalid program): a failing local const
 * initializer yields no 13b1 record but MUST yield a 13b2 record. */
static void test_ops_check_routing_gap(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  const C: i32 = 1 / 0;\n"
        "  return C;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.xsc == EXPR_CORE_FAILURE);
    CHECK(p.xrn == 0);                 /* 13b1 routes, never emits */
    CHECK(p.xfailn == 1);
    CHECK(p.xfails[0].kind == EVAL_FAIL_DIV_ZERO);
    CHECK(p.osc == EXPR_OPS_DIAG_ERROR);
    CHECK(p.opsrn == 1);               /* 13b2 emits the record */
    if (p.opsrn == 1) {
        check_fail_span(p.opsrecs[0], src, "AIC-E0406", "1 / 0", 5);
    }
    pipeline_free(&p);
}

/* Determinism: two runs produce byte-identical records. */
static void test_ops_check_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "fn f() -> i32 {\n"
        "  const A: i32 = 1 / 0;\n"
        "  const B: i8 = cast<i8>(127) + cast<i8>(1);\n"
        "  const C: i32 = 1 << 40;\n"
        "  return A;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    CHECK(p1.osc == p2.osc);
    CHECK(p1.opsrn == p2.opsrn);
    CHECK(p1.opsrn == 3);
    if (p1.opsrn != p2.opsrn || p1.opsrn != 3) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.opsrn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.opsrecs[i]));
        CHECK(diag_emit_record(&b2, p2.opsrecs[i]));
        CHECK(b1.len == b2.len);
        CHECK(b1.len == 0 || memcmp(b1.data, b2.data, b1.len) == 0);
        diag_buf_free(&b1);
        diag_buf_free(&b2);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_decision_add_sub();
    fprintf(stderr, "after test_decision_add_sub\n");
    test_decision_mul();
    fprintf(stderr, "after test_decision_mul\n");
    test_decision_div_mod();
    fprintf(stderr, "after test_decision_div_mod\n");
    test_decision_neg_shift();
    fprintf(stderr, "after test_decision_neg_shift\n");
    test_cmp_kind_model();
    fprintf(stderr, "after test_cmp_kind_model\n");
    test_cmp_values();
    fprintf(stderr, "after test_cmp_values\n");
    test_ops_check_sites();
    fprintf(stderr, "after test_ops_check_sites\n");
    test_ops_check_boundary_12a();
    fprintf(stderr, "after test_ops_check_boundary_12a\n");
    test_ops_check_boundary_not_const();
    fprintf(stderr, "after test_ops_check_boundary_not_const\n");
    test_ops_check_routing_gap();
    fprintf(stderr, "after test_ops_check_routing_gap\n");
    test_ops_check_determinism();
    fprintf(stderr, "after test_ops_check_determinism\n");

    if (g_failures) {
        fprintf(stderr, "expr_ops_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("expr_ops_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
