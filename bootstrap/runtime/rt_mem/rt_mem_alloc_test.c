/* bootstrap/runtime/rt_mem/rt_mem_alloc_test.c
 *
 * WP-M0-14a1 allocator core unit tests: zero-initialized allocation,
 * zero-size -> null no-op without state change, exhaustion -> null
 * (never a trap), alignment >= 16, addresses within the controlled
 * region, registry release status, and deterministic address sequences.
 *
 * The allocator core is a singleton (registry + controlled region).
 * Tests run in one process and are ordered in main() so that state
 * expectations hold; tests that need a pristine state call
 * rt_mem_reg_reset() first (test-only helper, see rt_mem_alloc.h).
 *
 * "Never a trap": if any exhaustion path trapped, the process would
 * exit nonzero (trap exit code 70) or abort; the harness therefore
 * proves the requirement by the test process exiting 0 with all checks
 * passing.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-mem-a1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_mem/rt_mem_alloc_test.c \
 *     bootstrap/runtime/rt_mem/rt_mem_alloc.c
 *   ./bootstrap/stage0/msvc-rt-mem-a1/rt_mem_alloc_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-mem-a1)
 */
#include "rt_mem_alloc.h"

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

#define MIB ((size_t)1024 * 1024)

/* True when every byte in [p, p+n) is zero. */
static int block_is_zero(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* True when p is a live allocation start inside the controlled region
 * and p+count is still inside it (spec sec. 15.1 acceptance). */
static int in_controlled_region(const void *p, size_t count)
{
    uintptr_t a = (uintptr_t)p;
    uintptr_t base = (uintptr_t)rt_mem_reg_region_base();
    size_t region_size = rt_mem_reg_region_size();
    return a >= base && a < base + region_size &&
           a + count <= base + region_size;
}

/* ---------------------------------------------------------------------------
 * test_zero_size_no_state
 * ---------------------------------------------------------------------------
 * alloc_bytes(0) -> null; no allocation, no state change, never a trap.
 * Must run first: the process starts pristine, so this also proves a
 * zero-size request does not even reserve the controlled region.
 */
static void test_zero_size_no_state(void)
{
    void *p;
    size_t used0, live0, free0;

    CHECK(rt_mem_reg_region_base() == NULL); /* pristine: no reservation yet */
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == 0);
    CHECK(rt_mem_reg_used() == 0);

    p = rt_mem_core_alloc(0);
    CHECK(p == NULL);

    /* No state change at all. */
    CHECK(rt_mem_reg_region_base() == NULL);
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == 0);
    CHECK(rt_mem_reg_used() == 0);

    /* Zero-size requests between real allocations must not move the
     * bump or the registry either. */
    p = rt_mem_core_alloc(1);
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    used0 = rt_mem_reg_used();
    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    CHECK(rt_mem_core_alloc(0) == NULL);
    CHECK(rt_mem_core_alloc(0) == NULL);
    CHECK(rt_mem_reg_used() == used0);
    CHECK(rt_mem_reg_live_count() == live0);
    CHECK(rt_mem_reg_free_count() == free0);

    CHECK(rt_mem_core_release(p) == RT_MEM_REL_OK);
}

/* ---------------------------------------------------------------------------
 * test_zero_init
 * ---------------------------------------------------------------------------
 * Every allocation returns count zero bytes; later allocations never
 * disturb earlier blocks (fresh bump blocks are disjoint).
 */
static void test_zero_init(void)
{
    unsigned char *a, *b, *c, *big;
    size_t i;

    a = (unsigned char *)rt_mem_core_alloc(1);
    CHECK(a != NULL);
    if (a == NULL) {
        return;
    }
    CHECK(block_is_zero(a, 1));

    b = (unsigned char *)rt_mem_core_alloc(64);
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(block_is_zero(b, 64));

    /* Odd sizes, including page-crossing and large blocks. */
    c = (unsigned char *)rt_mem_core_alloc(4096 + 3);
    CHECK(c != NULL);
    if (c == NULL) {
        return;
    }
    CHECK(block_is_zero(c, 4096 + 3));

    big = (unsigned char *)rt_mem_core_alloc(3 * MIB + 7);
    CHECK(big != NULL);
    if (big == NULL) {
        return;
    }
    CHECK(block_is_zero(big, 3 * MIB + 7));

    /* Fill earlier blocks with a pattern; new blocks must still come
     * back zero and the patterns must remain intact. */
    for (i = 0; i < 64; i++) {
        b[i] = (unsigned char)(i * 3u + 1u);
    }
    big[0] = 0xAA;
    big[3 * MIB + 6] = 0xBB;

    {
        unsigned char *d = (unsigned char *)rt_mem_core_alloc(128);
        CHECK(d != NULL);
        if (d != NULL) {
            CHECK(block_is_zero(d, 128));
        }
    }

    CHECK(b[0] == 1u && b[63] == (unsigned char)(63u * 3u + 1u));
    CHECK(big[0] == 0xAA && big[3 * MIB + 6] == 0xBB);
}

/* ---------------------------------------------------------------------------
 * test_alignment_region
 * ---------------------------------------------------------------------------
 * Alignment >= 16 and every returned address lies inside the
 * controlled region (spec sec. 15.1 acceptance criterion 2).
 */
static void test_alignment_region(void)
{
    static const size_t sizes[] = {
        1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 255, 256, 1024, 4095, 4096, 4097,
        64 * 1024 + 1, MIB + 1, 3 * MIB + 7
    };
    size_t i;

    CHECK(rt_mem_reg_region_base() != NULL); /* reserved by earlier tests */
    CHECK(rt_mem_reg_region_size() == RT_MEM_REGION_SIZE);

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = rt_mem_core_alloc(sizes[i]);
        CHECK(p != NULL);
        if (p == NULL) {
            continue;
        }
        CHECK(((uintptr_t)p % RT_MEM_ALIGNMENT) == 0);
        CHECK(in_controlled_region(p, sizes[i]));
        CHECK(block_is_zero((const unsigned char *)p, sizes[i]));
    }
}

/* ---------------------------------------------------------------------------
 * test_release_semantics
 * ---------------------------------------------------------------------------
 * Registry release status: null no-op, OK on live release, DOUBLE on a
 * second release of the same pointer, INVALID for a pointer that is not
 * a live allocation start. (Trap wiring for AIC-R0812/R0813 is owned by
 * WP-M0-14b2 and is not exercised here.)
 */
static void test_release_semantics(void)
{
    void *a, *b, *c;
    size_t live0, free0;
    char stack_var = 0;

    CHECK(rt_mem_core_release(NULL) == RT_MEM_REL_OK); /* null no-op */

    a = rt_mem_core_alloc(10);
    b = rt_mem_core_alloc(20);
    c = rt_mem_core_alloc(30);
    CHECK(a != NULL && b != NULL && c != NULL);
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }

    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    /* Invalid release: pointer never returned by the allocator. */
    CHECK(rt_mem_core_release(&stack_var) == RT_MEM_REL_INVALID);
    CHECK(rt_mem_core_release((void *)((uintptr_t)a + 1)) == RT_MEM_REL_INVALID);

    /* Live release -> OK, registry transitions LIVE -> FREE. */
    CHECK(rt_mem_core_release(b) == RT_MEM_REL_OK);
    CHECK(rt_mem_reg_live_count() == live0 - 1);
    CHECK(rt_mem_reg_free_count() == free0 + 1);

    /* Double release of the same pointer -> DOUBLE (14b2: AIC-R0812). */
    CHECK(rt_mem_core_release(b) == RT_MEM_REL_DOUBLE);
    CHECK(rt_mem_reg_live_count() == live0 - 1); /* state unchanged by DOUBLE */
    CHECK(rt_mem_reg_free_count() == free0 + 1);

    /* Releasing a still-live pointer twice in a row. */
    CHECK(rt_mem_core_release(a) == RT_MEM_REL_OK);
    CHECK(rt_mem_core_release(a) == RT_MEM_REL_DOUBLE);

    CHECK(rt_mem_core_release(c) == RT_MEM_REL_OK);
    CHECK(rt_mem_reg_live_count() == live0 - 3);
}

/* ---------------------------------------------------------------------------
 * test_determinism
 * ---------------------------------------------------------------------------
 * Identical allocation/release sequences, replayed from a pristine
 * state, yield identical addresses within the process. Offsets inside
 * the controlled region are the deterministic contract; the region base
 * itself is an OS-provided environmental input (spec sec. 15.6). This
 * directly guards the package risk "host-allocator nondeterminism
 * leaking": the only Windows calls are one reservation and fixed-range
 * commits, so the sequence must replay byte-for-byte.
 */
static void run_determinism_sequence(uintptr_t *offsets, size_t *out_count)
{
    void *a, *c, *e, *f;
    void *base = rt_mem_reg_region_base();
    size_t n = 0;

    a = rt_mem_core_alloc(1);
    c = rt_mem_core_alloc(4096 + 7);
    CHECK(rt_mem_core_alloc(0) == NULL);          /* no-op, no offset */
    e = rt_mem_core_alloc(16);
    CHECK(rt_mem_core_release(a) == RT_MEM_REL_OK);
    CHECK(rt_mem_core_release(a) == RT_MEM_REL_DOUBLE); /* double release */
    CHECK(rt_mem_core_release(c) == RT_MEM_REL_OK);
    f = rt_mem_core_alloc(100);                   /* fresh bump block, no reuse */
    CHECK(f != NULL);
    CHECK(rt_mem_core_release(e) == RT_MEM_REL_OK);

    /* Note: at 14a1 there is no reuse (hook is NULL); every block is
     * fresh from the bump. The sequence below records only live blocks
     * and is what must replay identically. */
    offsets[n++] = (uintptr_t)a - (uintptr_t)base;
    offsets[n++] = (uintptr_t)c - (uintptr_t)base;
    offsets[n++] = (uintptr_t)e - (uintptr_t)base;
    offsets[n++] = (uintptr_t)f - (uintptr_t)base;
    *out_count = n;
}

static void test_determinism(void)
{
    uintptr_t off1[8], off2[8];
    size_t n1 = 0, n2 = 0, i;
    size_t live1, live2;

    rt_mem_reg_reset();
    CHECK(rt_mem_reg_region_base() == NULL); /* reset restores pristine state */
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_used() == 0);

    run_determinism_sequence(off1, &n1);
    live1 = rt_mem_reg_live_count();

    rt_mem_reg_reset();
    run_determinism_sequence(off2, &n2);
    live2 = rt_mem_reg_live_count();

    CHECK(n1 == n2);
    CHECK(live1 == live2);
    for (i = 0; i < n1 && i < n2; i++) {
        CHECK(off1[i] == off2[i]);
    }
}

/* ---------------------------------------------------------------------------
 * test_exhaustion_region
 * ---------------------------------------------------------------------------
 * Region exhaustion -> null, never a trap. Requests larger than the
 * controlled region are rejected up front without state change.
 */
static void test_exhaustion_region(void)
{
    void *blocks[128];
    size_t i;
    size_t got = 0;
    size_t used0;

    rt_mem_reg_reset();

    /* Oversized requests: cannot fit, rejected before any state. */
    CHECK(rt_mem_core_alloc(RT_MEM_REGION_SIZE + 1) == NULL);
    CHECK(rt_mem_core_alloc((size_t)-1) == NULL); /* SIZE_MAX */
    CHECK(rt_mem_reg_region_base() == NULL);      /* no reservation happened */
    CHECK(rt_mem_reg_used() == 0);
    CHECK(rt_mem_reg_live_count() == 0);

    /* Fill the 64 MiB region with 1 MiB blocks. 64 succeed; the 65th
     * must return null (region exhausted), never trap. */
    for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        blocks[i] = rt_mem_core_alloc(MIB);
        if (blocks[i] == NULL) {
            break;
        }
        CHECK(((uintptr_t)blocks[i] % RT_MEM_ALIGNMENT) == 0);
        CHECK(block_is_zero((const unsigned char *)blocks[i], MIB));
        got++;
    }

    CHECK(got == RT_MEM_REGION_SIZE / MIB);       /* 64 */
    CHECK(rt_mem_reg_live_count() == got);
    CHECK(rt_mem_reg_used() == got * MIB);

    /* Still exhausted; zero-size requests remain null no-ops. */
    CHECK(rt_mem_core_alloc(1) == NULL);
    CHECK(rt_mem_core_alloc(MIB) == NULL);
    CHECK(rt_mem_core_alloc(0) == NULL);
    CHECK(rt_mem_reg_live_count() == got);

    /* Release everything; the addresses stay under allocator control
     * (freed blocks remain in the controlled region; reuse is 14b1). */
    for (i = 0; i < got; i++) {
        CHECK(rt_mem_core_release(blocks[i]) == RT_MEM_REL_OK);
    }
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == got);

    /* Registry slots are recyclable even before reuse exists: a fresh
     * allocation after the region is exhausted succeeds again only if
     * a slot was freed AND region space remains; here the region is
     * full, so allocation must still return null. */
    CHECK(rt_mem_core_alloc(1) == NULL);
    used0 = rt_mem_reg_used();
    CHECK(rt_mem_reg_used() == used0);
}

/* ---------------------------------------------------------------------------
 * test_exhaustion_registry
 * ---------------------------------------------------------------------------
 * Registry exhaustion -> null, never a trap; release frees slots so
 * allocation can resume; DOUBLE/INVALID release detection still works
 * after slot recycling.
 */
static void test_exhaustion_registry(void)
{
    static void *ptrs[RT_MEM_REGISTRY_CAPACITY];
    size_t i;
    size_t cap = rt_mem_reg_capacity();
    size_t got = 0;

    rt_mem_reg_reset();

    /* Fill the registry with 1-byte allocations (region usage is tiny:
     * 16384 * 16 bytes). The (cap+1)-th allocation must return null. */
    for (i = 0; i < cap + 1; i++) {
        void *p = rt_mem_core_alloc(1);
        if (p == NULL) {
            break;
        }
        CHECK(block_is_zero((const unsigned char *)p, 1));
        ptrs[got++] = p;
    }

    CHECK(got == cap);                      /* registry full */
    CHECK(rt_mem_reg_live_count() == cap);
    CHECK(rt_mem_reg_free_count() == 0);

    /* Still full: allocation returns null, state unchanged. */
    CHECK(rt_mem_core_alloc(1) == NULL);
    CHECK(rt_mem_reg_live_count() == cap);

    /* Release four entries; slots become recyclable. */
    for (i = 0; i < 4; i++) {
        CHECK(rt_mem_core_release(ptrs[cap - 1 - i]) == RT_MEM_REL_OK);
    }
    CHECK(rt_mem_reg_live_count() == cap - 4);
    CHECK(rt_mem_reg_free_count() == 4);

    /* Double release of a released entry is still detected. */
    CHECK(rt_mem_core_release(ptrs[cap - 1]) == RT_MEM_REL_DOUBLE);

    /* Allocation resumes once slots are free. */
    CHECK(rt_mem_core_alloc(1) != NULL);
    CHECK(rt_mem_core_alloc(1) != NULL);
    CHECK(rt_mem_core_alloc(1) != NULL);
    CHECK(rt_mem_core_alloc(1) != NULL);
    CHECK(rt_mem_reg_live_count() == cap);

    /* Invalid release detection is unaffected by recycling. */
    CHECK(rt_mem_core_release((void *)(uintptr_t)1) == RT_MEM_REL_INVALID);
    CHECK(rt_mem_core_release(&ptrs[0]) == RT_MEM_REL_INVALID);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_zero_size_no_state();
    fprintf(stderr, "after test_zero_size_no_state\n");
    test_zero_init();
    fprintf(stderr, "after test_zero_init\n");
    test_alignment_region();
    fprintf(stderr, "after test_alignment_region\n");
    test_release_semantics();
    fprintf(stderr, "after test_release_semantics\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_exhaustion_region();
    fprintf(stderr, "after test_exhaustion_region\n");
    test_exhaustion_registry();
    fprintf(stderr, "after test_exhaustion_registry\n");

    if (g_failures) {
        fprintf(stderr, "rt_mem_alloc_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_mem_alloc_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
