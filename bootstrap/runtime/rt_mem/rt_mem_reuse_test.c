/* bootstrap/runtime/rt_mem/rt_mem_reuse_test.c
 *
 * WP-M0-14b1 reuse policy and poisoning tests: exact-fit reuse with
 * 0xDD overwrite before reuse and reverse-order-of-release within a
 * size class per spec sec. 15.1 / ADR-004; no split, no coalesce.
 *
 * The reuse policy is opt-in: the 14a1 core exposes two hooks
 * (rt_mem_reg_set_release_hook / rt_mem_reg_set_reuse_lookup) and this
 * package registers its implementation through rt_mem_reuse_init().
 * All tests run through the public API (rt_mem_alloc_bytes /
 * rt_mem_dealloc_bytes) because the observable contract of spec sec.
 * 15.1 is the source-visible surface; the registry introspection
 * (rt_mem_reg_*) is used only to assert the FREE/LIVE bookkeeping.
 *
 * The allocator is a singleton (registry + controlled region + reuse
 * stack). Tests run in one process and each test starts from a pristine
 * state via the local pristine_reuse() helper:
 *   rt_mem_reg_reset()    -- core-owned test reset (empties registry,
 *                            frees the region, clears the hooks);
 *   rt_mem_reuse_reset()  -- this package's test reset (clears the
 *                            reuse stack);
 *   rt_mem_reuse_init()   -- (re-)registers the 14b1 hooks.
 *
 * Registry-slot recycling note (14a1 core, unchanged): a fresh
 * allocation of a different size recycles the first non-LIVE registry
 * slot, overwriting a FREE entry whose block may still be tracked on
 * this package's reuse stack. The core verifies every lookup result
 * defensively (rt_mem_alloc.h: an unusable result falls through to a
 * fresh block), so such an entry is simply never reused - the outcome
 * stays deterministic and no address outside the controlled region is
 * ever returned. test_slot_recycle_fallback locks this in.
 *
 * "Never a trap": any exhaustion or double/invalid-release path that
 * trapped would exit the process nonzero (trap exit code 70) or abort;
 * the harness therefore proves the requirement by the test process
 * exiting 0 with all checks passing. The AIC-R0812/AIC-R0813 trap
 * wiring itself is owned by WP-M0-14b2 and is out of scope here.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-mem-b1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_mem/rt_mem_reuse_test.c \
 *     bootstrap/runtime/rt_mem/rt_mem_reuse.c \
 *     bootstrap/runtime/rt_mem/rt_mem_api.c \
 *     bootstrap/runtime/rt_mem/rt_mem_alloc.c
 *   ./bootstrap/stage0/msvc-rt-mem-b1/rt_mem_reuse_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-mem-b1)
 */
#include "rt_mem_api.h"

#include "rt_mem_alloc.h"
#include "rt_mem_reuse.h"

#include <stdio.h>
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

#define MIB ((size_t)1024 * 1024)

/* Poison byte of ADR-004 / spec sec. 15.1. */
#define POISON_BYTE ((unsigned char)0xDD)

/* True when every byte in [p, p+n) equals `value`. */
static int block_is_value(const unsigned char *p, size_t n, unsigned char value)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != value) {
            return 0;
        }
    }
    return 1;
}

/* Pristine state for one test: empty core registry/region, cleared
 * reuse stack, hooks registered. */
static void pristine_reuse(void)
{
    rt_mem_reg_reset();
    rt_mem_reuse_reset();
    rt_mem_reuse_init();
}

/* ---------------------------------------------------------------------------
 * test_no_reuse_before_register
 * ---------------------------------------------------------------------------
 * Without the 14b1 hooks registered, the allocator neither poisons nor
 * reuses: a release is just a registry FREE transition and the next
 * same-size request comes from the bump. This guards the integration
 * contract (reuse is opt-in through rt_mem_reuse_init) and the 14a
 * baseline remains intact when the hooks are absent.
 */
static void test_no_reuse_before_register(void)
{
    unsigned char *a, *b;

    rt_mem_reg_reset(); /* pristine core; hooks are NOT registered here */

    a = rt_mem_alloc_bytes(8);
    CHECK(a != NULL);
    if (a == NULL) {
        return;
    }
    rt_mem_dealloc_bytes(a);

    /* No release hook: the freed bytes keep their pre-release content
     * (fresh pages are zero), so a stale read does not see 0xDD. */
    CHECK(a[0] == 0x00);
    CHECK(rt_mem_reg_free_count() == 1);

    /* No reuse lookup: the next 8-byte request is a fresh block. */
    b = rt_mem_alloc_bytes(8);
    CHECK(b != NULL);
    CHECK(b != a);
    if (b == NULL) {
        return;
    }
    CHECK(block_is_value(b, 8, 0x00));

    rt_mem_dealloc_bytes(b);
}

/* ---------------------------------------------------------------------------
 * test_poison_on_release
 * ---------------------------------------------------------------------------
 * dealloc_bytes overwrites the FULL allocation with 0xDD before the
 * block becomes eligible for deterministic reuse (ADR-004 / spec sec.
 * 15.1). The registry marks the entry FREE; the poisoned bytes are
 * observable to a stale read. Sized loop includes odd and page-spanning
 * sizes to prove the overwrite covers exactly `size` bytes. All blocks
 * are allocated before any is released so the registry counts track
 * releases exactly (a fresh allocation would recycle the FREE slot).
 */
static void test_poison_on_release(void)
{
    static const size_t sizes[] = {
        1, 7, 8, 15, 16, 64, 127, 4095, 4096, 4096 + 3
    };
    enum { N = sizeof(sizes) / sizeof(sizes[0]) };
    unsigned char *p[N];
    size_t i;
    size_t live0, free0;

    pristine_reuse();
    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    for (i = 0; i < N; i++) {
        p[i] = rt_mem_alloc_bytes(sizes[i]);
        CHECK(p[i] != NULL);
        if (p[i] != NULL) {
            rt_mem_fill(p[i], 0x5A, sizes[i]); /* dirty every byte */
        }
    }

    for (i = 0; i < N; i++) {
        if (p[i] == NULL) {
            continue;
        }
        rt_mem_dealloc_bytes(p[i]);
        /* Full allocation overwritten with 0xDD before reuse. */
        CHECK(block_is_value(p[i], sizes[i], POISON_BYTE));
    }
    CHECK(rt_mem_reg_live_count() == live0);
    CHECK(rt_mem_reg_free_count() == free0 + N);

    /* A released block that is never reused stays poisoned and FREE. */
    CHECK(block_is_value(p[0], sizes[0], POISON_BYTE));
}

/* ---------------------------------------------------------------------------
 * test_reuse_exact_fit
 * ---------------------------------------------------------------------------
 * Reuse is exact-fit: a freed block of size S satisfies only a request
 * of size S. The allocator never splits a larger freed block for a
 * smaller request and never coalesces adjacent freed blocks (spec sec.
 * 15.1). A reused block is re-zeroed before it is returned. The reuse
 * checks run before the no-split/no-coalesce fresh allocations so the
 * registry slots of the freed blocks are still trackable (see the
 * registry-slot recycling note at the top).
 */
static void test_reuse_exact_fit(void)
{
    unsigned char *a, *b, *c;
    size_t live0, free0;

    pristine_reuse();

    a = rt_mem_alloc_bytes(8);
    b = rt_mem_alloc_bytes(16);
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        return;
    }
    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    rt_mem_dealloc_bytes(a);
    rt_mem_dealloc_bytes(b);
    CHECK(rt_mem_reg_live_count() == live0 - 2);
    CHECK(rt_mem_reg_free_count() == free0 + 2);

    /* An 8-byte request reuses the freed 8-byte block, not the 16. */
    c = rt_mem_alloc_bytes(8);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(c == a);
        CHECK(block_is_value(c, 8, 0x00)); /* re-zeroed before return */
        CHECK(rt_mem_reg_live_count() == live0 - 1);
        CHECK(rt_mem_reg_free_count() == free0 + 1);
    }

    /* A 16-byte request reuses the freed 16-byte block. */
    c = rt_mem_alloc_bytes(16);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(c == b);
        CHECK(block_is_value(c, 16, 0x00));
        CHECK(rt_mem_reg_live_count() == live0);
        CHECK(rt_mem_reg_free_count() == free0);
    }

    /* No split: with the 8-byte block free again, a 7-byte request
     * must not carve into it; it is served from the bump. */
    rt_mem_dealloc_bytes(a);
    c = rt_mem_alloc_bytes(7);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(c != a && c != b);
        CHECK(block_is_value(c, 7, 0x00));
        rt_mem_dealloc_bytes(c);
    }

    /* No coalesce: with the freed 8-byte and 16-byte blocks adjacent,
     * a 24-byte request must not be served by their combined range
     * (base == a's address); it is served from the bump. */
    rt_mem_dealloc_bytes(b);
    c = rt_mem_alloc_bytes(24);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(c != a && c != b);
        CHECK(block_is_value(c, 24, 0x00));
        rt_mem_dealloc_bytes(c);
    }
}

/* ---------------------------------------------------------------------------
 * test_slot_recycle_fallback
 * ---------------------------------------------------------------------------
 * A fresh allocation of a different size recycles the first non-LIVE
 * registry slot (14a1 core behavior), overwriting a FREE entry whose
 * block may still be tracked on the reuse stack. The core then refuses
 * to activate that address (defensive contract, rt_mem_alloc.h) and
 * the request falls through to a fresh block. The outcome stays
 * deterministic, never returns an address outside the controlled
 * region, and never traps. This test locks in that defensive path.
 */
static void test_slot_recycle_fallback(void)
{
    unsigned char *a, *b, *c;
    unsigned char *d;

    pristine_reuse();

    a = rt_mem_alloc_bytes(8);
    b = rt_mem_alloc_bytes(16);
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        return;
    }

    rt_mem_dealloc_bytes(a);          /* a is FREE and tracked */

    /* A fresh 16-byte request cannot reuse the freed 8-byte block
     * (exact-fit) and recycles a's registry slot for the new block. */
    c = rt_mem_alloc_bytes(16);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(c != a && c != b);
        rt_mem_dealloc_bytes(b);

        /* a's address is no longer a FREE registry entry; a same-size
         * request falls through to a fresh block instead of reusing
         * it (deterministic, never a trap). */
        d = rt_mem_alloc_bytes(8);
        CHECK(d != NULL);
        CHECK(d != a);
        CHECK(block_is_value(d, 8, 0x00));
        rt_mem_dealloc_bytes(d);
        rt_mem_dealloc_bytes(c);
    }
}

/* ---------------------------------------------------------------------------
 * test_reverse_order_of_release
 * ---------------------------------------------------------------------------
 * Among free blocks of the same size, the block most recently released
 * is reused first (reverse order of release within a size class, spec
 * sec. 15.1). Releases of other sizes do not affect the order.
 */
static void test_reverse_order_of_release(void)
{
    unsigned char *a, *b, *c;
    unsigned char *x, *y;

    /* Release order A, B, C -> most recent is C, then B, then A. */
    pristine_reuse();
    a = rt_mem_alloc_bytes(8);
    b = rt_mem_alloc_bytes(8);
    c = rt_mem_alloc_bytes(8);
    CHECK(a != NULL && b != NULL && c != NULL);
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }
    rt_mem_dealloc_bytes(a);
    rt_mem_dealloc_bytes(b);
    rt_mem_dealloc_bytes(c);

    x = rt_mem_alloc_bytes(8);
    CHECK(x == c);
    y = rt_mem_alloc_bytes(8);
    CHECK(y == b);
    x = rt_mem_alloc_bytes(8);
    CHECK(x == a);

    /* Release order C, A (with an unrelated live 16-byte block in
     * between) -> most recent 8-byte release is A, then C. */
    pristine_reuse();
    a = rt_mem_alloc_bytes(8);
    b = rt_mem_alloc_bytes(16);
    c = rt_mem_alloc_bytes(8);
    CHECK(a != NULL && b != NULL && c != NULL);
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }
    rt_mem_dealloc_bytes(c);
    rt_mem_dealloc_bytes(a);

    x = rt_mem_alloc_bytes(8);
    CHECK(x == a);
    y = rt_mem_alloc_bytes(8);
    CHECK(y == c);

    /* Interleaved sizes: a 16-byte release after the 8-byte releases
     * must not overtake the 8-byte class order. */
    pristine_reuse();
    a = rt_mem_alloc_bytes(8);
    b = rt_mem_alloc_bytes(8);
    c = rt_mem_alloc_bytes(16);
    CHECK(a != NULL && b != NULL && c != NULL);
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }
    rt_mem_dealloc_bytes(b); /* most recent 8-byte release */
    rt_mem_dealloc_bytes(c); /* released later, different size */

    x = rt_mem_alloc_bytes(8);
    CHECK(x == b);
    y = rt_mem_alloc_bytes(16);
    CHECK(y == c);
}

/* ---------------------------------------------------------------------------
 * test_reuse_under_exhaustion
 * ---------------------------------------------------------------------------
 * When the controlled region is full, releasing blocks makes them
 * reusable and a same-size request succeeds through exact-fit reuse
 * (a fresh block would fail). Requests with no exact-fit free block
 * still return NULL (never a trap). This proves reuse is the recycling
 * path for the declared resource bound.
 */
static void test_reuse_under_exhaustion(void)
{
    enum { N = RT_MEM_REGION_SIZE / MIB }; /* 64 */
    unsigned char *blocks[N];
    unsigned char *p;
    size_t i;

    pristine_reuse();

    for (i = 0; i < N; i++) {
        blocks[i] = rt_mem_alloc_bytes(MIB);
        CHECK(blocks[i] != NULL);
        if (blocks[i] == NULL) {
            break;
        }
        CHECK(block_is_value(blocks[i], MIB, 0x00));
    }
    CHECK(rt_mem_reg_live_count() == N);

    /* Region is full; no exact-fit free block for 1 byte exists, so a
     * 1-byte request fails (null, never a trap). */
    CHECK(rt_mem_alloc_bytes(1) == NULL);

    for (i = 0; i < N; i++) {
        rt_mem_dealloc_bytes(blocks[i]);
    }
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == N);

    /* Same-size request succeeds through reuse; reverse order of
     * release gives the last released block. Re-zeroed before return. */
    p = rt_mem_alloc_bytes(MIB);
    CHECK(p != NULL);
    if (p != NULL) {
        CHECK(p == blocks[N - 1]);
        CHECK(block_is_value(p, MIB, 0x00));
        rt_mem_dealloc_bytes(p);
    }

    /* Still exact-fit: a 1-byte request finds no 1-byte free block and
     * the region is still full -> null, never a trap. */
    CHECK(rt_mem_alloc_bytes(1) == NULL);
}

/* ---------------------------------------------------------------------------
 * test_determinism_with_reuse
 * ---------------------------------------------------------------------------
 * Identical allocation/release sequences, replayed from a pristine
 * state, yield identical addresses (observable contract, spec sec.
 * 15.1). Offsets inside the controlled region are the deterministic
 * contract; the region base itself is an OS-provided environmental
 * input (spec sec. 15.6). Reuse must not introduce any dependence on
 * host-allocator behavior, timing, environment values, or build
 * options.
 */
static void run_reuse_sequence(uintptr_t *offsets, size_t *out_count)
{
    unsigned char *a, *b, *c, *d, *e;
    size_t n = 0;

    a = rt_mem_alloc_bytes(8);
    b = rt_mem_alloc_bytes(16);
    c = rt_mem_alloc_bytes(8);
    CHECK(a != NULL && b != NULL && c != NULL);

    rt_mem_fill(a, 0x11, 8);
    rt_mem_fill(b, 0x22, 16);

    rt_mem_dealloc_bytes(a);
    rt_mem_dealloc_bytes(c);      /* free 8-byte blocks: a then c */
    d = rt_mem_alloc_bytes(8);    /* reuse: most recent 8-byte = c */
    CHECK(d == c);
    CHECK(block_is_value(d, 8, 0x00));

    rt_mem_dealloc_bytes(b);
    e = rt_mem_alloc_bytes(16);   /* reuse: b */
    CHECK(e == b);
    CHECK(block_is_value(e, 16, 0x00));

    rt_mem_dealloc_bytes(d);
    rt_mem_dealloc_bytes(e);

    if (rt_mem_reg_region_base() != NULL) {
        uintptr_t base = (uintptr_t)rt_mem_reg_region_base();
        offsets[n++] = (uintptr_t)a - base;
        offsets[n++] = (uintptr_t)b - base;
        offsets[n++] = (uintptr_t)c - base;
        offsets[n++] = (uintptr_t)d - base;
        offsets[n++] = (uintptr_t)e - base;
    }
    *out_count = n;
}

static void test_determinism_with_reuse(void)
{
    uintptr_t off1[16], off2[16];
    size_t n1 = 0, n2 = 0, i;

    pristine_reuse();
    run_reuse_sequence(off1, &n1);

    pristine_reuse();
    run_reuse_sequence(off2, &n2);

    CHECK(n1 == n2);
    CHECK(n1 == 5);
    for (i = 0; i < n1 && i < n2; i++) {
        CHECK(off1[i] == off2[i]);
    }
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    test_no_reuse_before_register();
    fprintf(stderr, "after test_no_reuse_before_register\n");
    test_poison_on_release();
    fprintf(stderr, "after test_poison_on_release\n");
    test_reuse_exact_fit();
    fprintf(stderr, "after test_reuse_exact_fit\n");
    test_slot_recycle_fallback();
    fprintf(stderr, "after test_slot_recycle_fallback\n");
    test_reverse_order_of_release();
    fprintf(stderr, "after test_reverse_order_of_release\n");
    test_reuse_under_exhaustion();
    fprintf(stderr, "after test_reuse_under_exhaustion\n");
    test_determinism_with_reuse();
    fprintf(stderr, "after test_determinism_with_reuse\n");

    if (g_failures) {
        fprintf(stderr, "rt_mem_reuse_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_mem_reuse_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
