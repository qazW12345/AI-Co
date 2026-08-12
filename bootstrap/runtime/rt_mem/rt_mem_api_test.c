/* bootstrap/runtime/rt_mem/rt_mem_api_test.c
 *
 * WP-M0-14a2 public allocator API tests: alloc_bytes/dealloc_bytes/
 * copy/fill behave per spec sec. 15.1 with deterministic observable
 * behavior, the public API integrates with the allocation registry
 * (no host allocator is ever used per allocation), and identical
 * sequences replay to identical addresses and bytes.
 *
 * The allocator core is a singleton (registry + controlled region).
 * Tests run in one process and are ordered in main() so that state
 * expectations hold; tests that need a pristine state call
 * rt_mem_reg_reset() first (test-only helper, see rt_mem_alloc.h).
 *
 * "Never a trap": if any exhaustion path or release-status path
 * trapped, the process would exit nonzero (trap exit code 70) or
 * abort; the harness therefore proves the requirement by the test
 * process exiting 0 with all checks passing. The AIC-R0812/AIC-R0813
 * trap wiring itself is owned by WP-M0-14b2; at 14a2 the public
 * dealloc_bytes reports the release status to a registered handler
 * (NULL by default, so at 14a2 a double/invalid release is a
 * deterministic no-op that leaves registry state unchanged).
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-mem-a2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_mem/rt_mem_api_test.c \
 *     bootstrap/runtime/rt_mem/rt_mem_api.c \
 *     bootstrap/runtime/rt_mem/rt_mem_alloc.c
 *   ./bootstrap/stage0/msvc-rt-mem-a2/rt_mem_api_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-mem-a2)
 */
#include "rt_mem_api.h"

#include "rt_mem_alloc.h"

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

/* Release-status handler record (14b2 contract probe). */
static int g_hook_calls = 0;
static int g_hook_status[8];
static void *g_hook_addr[8];

static void test_release_status_handler(int status, void *addr)
{
    if (g_hook_calls < 8) {
        g_hook_status[g_hook_calls] = status;
        g_hook_addr[g_hook_calls] = addr;
    }
    g_hook_calls++;
}

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
 * dealloc_bytes(null) is the documented no-op. Must run first: the
 * process starts pristine, so this also proves a zero-size request does
 * not even reserve the controlled region.
 */
static void test_zero_size_no_state(void)
{
    unsigned char *p;
    size_t used0, live0, free0;

    CHECK(rt_mem_reg_region_base() == NULL); /* pristine: no reservation yet */
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == 0);
    CHECK(rt_mem_reg_used() == 0);

    p = rt_mem_alloc_bytes(0);
    CHECK(p == NULL);

    /* No state change at all. */
    CHECK(rt_mem_reg_region_base() == NULL);
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == 0);
    CHECK(rt_mem_reg_used() == 0);

    /* dealloc_bytes(null) is a no-op (spec sec. 15.1). */
    rt_mem_dealloc_bytes(NULL);
    CHECK(rt_mem_reg_region_base() == NULL);
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == 0);

    /* Zero-size requests between real allocations must not move the
     * bump or the registry either. */
    p = rt_mem_alloc_bytes(1);
    CHECK(p != NULL);
    if (p == NULL) {
        return;
    }
    used0 = rt_mem_reg_used();
    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    CHECK(rt_mem_alloc_bytes(0) == NULL);
    CHECK(rt_mem_alloc_bytes(0) == NULL);
    CHECK(rt_mem_reg_used() == used0);
    CHECK(rt_mem_reg_live_count() == live0);
    CHECK(rt_mem_reg_free_count() == free0);

    rt_mem_dealloc_bytes(p);
    CHECK(rt_mem_reg_live_count() == live0 - 1);
}

/* ---------------------------------------------------------------------------
 * test_alloc_bytes_public
 * ---------------------------------------------------------------------------
 * alloc_bytes returns zero-initialized, >= 16-byte aligned blocks inside
 * the controlled region and integrates with the registry (live count
 * follows). Oversized requests and exhaustion return null (never a
 * trap) exactly like the core.
 */
static void test_alloc_bytes_public(void)
{
    static const size_t sizes[] = {
        1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 255, 256, 1024, 4095, 4096, 4097, 64 * 1024 + 1
    };
    size_t i;
    size_t live0;

    CHECK(rt_mem_reg_region_base() != NULL); /* reserved by earlier tests */
    live0 = rt_mem_reg_live_count();

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned char *p = rt_mem_alloc_bytes(sizes[i]);
        CHECK(p != NULL);
        if (p == NULL) {
            continue;
        }
        CHECK(((uintptr_t)p % RT_MEM_ALIGNMENT) == 0);
        CHECK(in_controlled_region(p, sizes[i]));
        CHECK(block_is_value(p, sizes[i], 0x00));
    }
    CHECK(rt_mem_reg_live_count() == live0 + (sizeof(sizes) / sizeof(sizes[0])));

    /* Oversized request: rejected up front, no state change. */
    CHECK(rt_mem_alloc_bytes(RT_MEM_REGION_SIZE + 1) == NULL);
    CHECK(rt_mem_alloc_bytes((size_t)-1) == NULL);
    CHECK(rt_mem_reg_live_count() == live0 + (sizeof(sizes) / sizeof(sizes[0])));

    /* Blocks stay live for the remainder of the process; later tests
     * snapshot the registry counts before their own allocations. */
}

/* ---------------------------------------------------------------------------
 * test_dealloc_bytes_public
 * ---------------------------------------------------------------------------
 * dealloc_bytes releases a live allocation (registry LIVE -> FREE, live
 * count -1, free count +1) and null is a no-op. The release-status
 * handler contract for WP-M0-14b2 is probed: with a handler registered,
 * DOUBLE and INVALID releases call it with the exact status; OK releases
 * never call it. With NULL (14a2 default), double/invalid releases are
 * deterministic no-ops that leave registry state unchanged and never
 * trap.
 */
static void test_dealloc_bytes_public(void)
{
    unsigned char *a, *b;
    size_t live0, free0;

    a = rt_mem_alloc_bytes(10);
    b = rt_mem_alloc_bytes(20);
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        return;
    }
    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    /* Live release: registry transitions LIVE -> FREE. */
    rt_mem_dealloc_bytes(a);
    CHECK(rt_mem_reg_live_count() == live0 - 1);
    CHECK(rt_mem_reg_free_count() == free0 + 1);

    /* Without a handler (14a2 default): double/invalid release is a
     * deterministic no-op, no trap (process continues; exit 0 below). */
    rt_mem_dealloc_bytes(a);                 /* double release: no-op at 14a2 */
    CHECK(rt_mem_reg_live_count() == live0 - 1);
    CHECK(rt_mem_reg_free_count() == free0 + 1);
    {
        unsigned char stack_var = 0;
        rt_mem_dealloc_bytes(&stack_var);    /* invalid release: no-op at 14a2 */
    }
    CHECK(rt_mem_reg_live_count() == live0 - 1);
    CHECK(rt_mem_reg_free_count() == free0 + 1);

    /* With a handler registered: DOUBLE/INVALID report the exact status
     * and address (this is the 14b2 trap-wiring contract); OK never
     * calls the handler. */
    g_hook_calls = 0;
    rt_mem_api_set_release_status_handler(test_release_status_handler);

    rt_mem_dealloc_bytes(a);                 /* double release -> DOUBLE */
    CHECK(g_hook_calls == 1);
    CHECK(g_hook_status[0] == RT_MEM_REL_DOUBLE);
    CHECK(g_hook_addr[0] == (void *)a);

    {
        unsigned char stack_var = 0;
        rt_mem_dealloc_bytes(&stack_var);    /* invalid release -> INVALID */
    }
    CHECK(g_hook_calls == 2);
    CHECK(g_hook_status[1] == RT_MEM_REL_INVALID);

    rt_mem_dealloc_bytes(b);                 /* live release -> OK, no call */
    CHECK(g_hook_calls == 2);
    CHECK(rt_mem_reg_live_count() == live0 - 2);
    CHECK(rt_mem_reg_free_count() == free0 + 2);

    /* Restore the 14a2 default (NULL handler). */
    rt_mem_api_set_release_status_handler(NULL);
    g_hook_calls = 0;
    rt_mem_dealloc_bytes(a);                 /* double release: no-op again */
    CHECK(g_hook_calls == 0);
}

/* ---------------------------------------------------------------------------
 * test_copy_disjoint
 * ---------------------------------------------------------------------------
 * copy copies count bytes byte-exactly between disjoint regions (both
 * inside allocated blocks and inside one block), handles count == 0 as
 * a no-op, and never touches the registry.
 */
static void test_copy_disjoint(void)
{
    unsigned char *src, *dst, *one;
    size_t i;
    size_t live0, free0;

    src = rt_mem_alloc_bytes(256);
    dst = rt_mem_alloc_bytes(256);
    one = rt_mem_alloc_bytes(64);
    CHECK(src != NULL && dst != NULL && one != NULL);
    if (src == NULL || dst == NULL || one == NULL) {
        return;
    }

    for (i = 0; i < 256; i++) {
        src[i] = (unsigned char)(i * 3u + 1u);
        dst[i] = 0xAA;
    }

    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    rt_mem_copy(dst, src, 256);
    CHECK(memcmp(dst, src, 256) == 0);

    /* count == 0: no-op even with valid pointers. */
    rt_mem_copy(dst, src, 0);
    CHECK(memcmp(dst, src, 256) == 0);

    /* Partial copy within one block. */
    rt_mem_fill(one, 0x00, 64);
    rt_mem_copy(one + 4, src, 32);
    CHECK(memcmp(one + 4, src, 32) == 0);
    CHECK(block_is_value(one, 4, 0x00));
    CHECK(block_is_value(one + 36, 28, 0x00));

    /* Copy does not disturb the registry. */
    CHECK(rt_mem_reg_live_count() == live0);
    CHECK(rt_mem_reg_free_count() == free0);
}

/* ---------------------------------------------------------------------------
 * test_copy_overlap
 * ---------------------------------------------------------------------------
 * copy is overlap-safe "as if a temporary buffer were used" (spec sec.
 * 15.1): forward overlap (dst < src) and backward overlap (dst > src)
 * both produce the memmove result.
 */
static void test_copy_overlap(void)
{
    unsigned char b[64];
    size_t i;

    for (i = 0; i < 64; i++) {
        b[i] = (unsigned char)i;
    }
    /* Forward overlap: dst == b+1, src == b, count 63. As-if-temporary:
     * b[1..63] = original b[0..62] = 0..62. */
    rt_mem_copy(b + 1, b, 63);
    CHECK(b[0] == 0);
    for (i = 1; i < 64; i++) {
        CHECK(b[i] == (unsigned char)(i - 1));
    }

    for (i = 0; i < 64; i++) {
        b[i] = (unsigned char)i;
    }
    /* Backward overlap: dst == b, src == b+1, count 63. As-if-temporary:
     * b[0..62] = original b[1..63] = 1..63. */
    rt_mem_copy(b, b + 1, 63);
    CHECK(b[63] == 63);
    for (i = 0; i < 63; i++) {
        CHECK(b[i] == (unsigned char)(i + 1));
    }

    /* Exact same pointer: no-op copy. */
    for (i = 0; i < 16; i++) {
        b[i] = (unsigned char)(i + 0x40);
    }
    rt_mem_copy(b, b, 16);
    for (i = 0; i < 16; i++) {
        CHECK(b[i] == (unsigned char)(i + 0x40));
    }
}

/* ---------------------------------------------------------------------------
 * test_fill
 * ---------------------------------------------------------------------------
 * fill sets count bytes to value (including 0 and 0xFF), handles count
 * == 0 as a no-op, and never touches the registry.
 */
static void test_fill(void)
{
    unsigned char *p, *q;
    size_t i;
    size_t live0, free0;

    p = rt_mem_alloc_bytes(128);
    q = rt_mem_alloc_bytes(8);
    CHECK(p != NULL && q != NULL);
    if (p == NULL || q == NULL) {
        return;
    }

    live0 = rt_mem_reg_live_count();
    free0 = rt_mem_reg_free_count();

    rt_mem_fill(p, 0xFF, 128);
    CHECK(block_is_value(p, 128, 0xFF));

    rt_mem_fill(p, 0x00, 128);
    CHECK(block_is_value(p, 128, 0x00));

    rt_mem_fill(p, 0x55, 64);
    CHECK(block_is_value(p, 64, 0x55));
    CHECK(block_is_value(p + 64, 64, 0x00));

    /* count == 0: no-op. */
    rt_mem_fill(p, 0x33, 0);
    CHECK(block_is_value(p, 64, 0x55));

    /* Fill a single byte. */
    for (i = 0; i < 8; i++) {
        q[i] = 0x11;
    }
    rt_mem_fill(q + 3, 0x77, 1);
    CHECK(q[0] == 0x11 && q[1] == 0x11 && q[2] == 0x11 && q[3] == 0x77);
    CHECK(q[4] == 0x11 && q[5] == 0x11 && q[6] == 0x11 && q[7] == 0x11);

    /* Fill does not disturb the registry. */
    CHECK(rt_mem_reg_live_count() == live0);
    CHECK(rt_mem_reg_free_count() == free0);
}

/* ---------------------------------------------------------------------------
 * test_exhaustion_public
 * ---------------------------------------------------------------------------
 * Exhaustion through the public API returns null and never traps: the
 * region fills, further alloc_bytes returns null, and release makes
 * registry slots recyclable (region remains full at 14a2: no reuse
 * yet, so a fresh block after full region still fails).
 */
static void test_exhaustion_public(void)
{
    void *blocks[128];
    size_t i;
    size_t got = 0;

    rt_mem_reg_reset();

    for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        blocks[i] = rt_mem_alloc_bytes(MIB);
        if (blocks[i] == NULL) {
            break;
        }
        CHECK(((uintptr_t)blocks[i] % RT_MEM_ALIGNMENT) == 0);
        CHECK(block_is_value((const unsigned char *)blocks[i], MIB, 0x00));
        got++;
    }

    CHECK(got == RT_MEM_REGION_SIZE / MIB);       /* 64 */
    CHECK(rt_mem_reg_live_count() == got);

    /* Still exhausted; zero-size requests remain null no-ops. */
    CHECK(rt_mem_alloc_bytes(1) == NULL);
    CHECK(rt_mem_alloc_bytes(MIB) == NULL);
    CHECK(rt_mem_alloc_bytes(0) == NULL);
    CHECK(rt_mem_reg_live_count() == got);

    /* Release everything; the addresses stay under allocator control
     * (freed blocks remain in the controlled region; reuse is 14b1). */
    for (i = 0; i < got; i++) {
        rt_mem_dealloc_bytes(blocks[i]);
    }
    CHECK(rt_mem_reg_live_count() == 0);
    CHECK(rt_mem_reg_free_count() == got);

    /* Region is full and there is no reuse at 14a2, so a fresh
     * allocation must still return null. */
    CHECK(rt_mem_alloc_bytes(1) == NULL);
    CHECK(rt_mem_reg_live_count() == 0);
}

/* ---------------------------------------------------------------------------
 * test_determinism
 * ---------------------------------------------------------------------------
 * Identical allocation/release/copy/fill sequences, replayed from a
 * pristine state, yield identical addresses and bytes within the
 * process. Offsets inside the controlled region are the deterministic
 * contract; the region base itself is an OS-provided environmental
 * input (spec sec. 15.6). copy/fill are pure byte operations whose
 * observable output is fully specified, so they contribute no
 * nondeterminism. This directly guards the package risk "host-allocator
 * nondeterminism leaking": the only Windows calls are one reservation
 * and fixed-range commits, so the sequence must replay byte-for-byte.
 */
static void run_determinism_sequence(uintptr_t *offsets, size_t *out_count)
{
    unsigned char *a, *c, *e, *f, *tmp;
    void *base = rt_mem_reg_region_base();
    size_t n = 0;

    a = rt_mem_alloc_bytes(1);
    c = rt_mem_alloc_bytes(4096 + 7);
    CHECK(rt_mem_alloc_bytes(0) == NULL);          /* no-op, no offset */
    rt_mem_fill(a, 0xAB, 1);
    e = rt_mem_alloc_bytes(16);
    tmp = rt_mem_alloc_bytes(64);
    CHECK(tmp != NULL);
    if (tmp != NULL) {
        rt_mem_fill(tmp, 0xCD, 64);
        rt_mem_copy(tmp + 8, a, 1);                /* deterministic byte op */
    }
    rt_mem_dealloc_bytes(a);
    rt_mem_dealloc_bytes(a);                       /* double release: 14a2 no-op */
    rt_mem_dealloc_bytes(c);
    f = rt_mem_alloc_bytes(100);                   /* fresh bump block, no reuse */
    CHECK(f != NULL);
    rt_mem_dealloc_bytes(e);
    if (tmp != NULL) {
        rt_mem_dealloc_bytes(tmp);
    }

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
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_zero_size_no_state();
    fprintf(stderr, "after test_zero_size_no_state\n");
    test_alloc_bytes_public();
    fprintf(stderr, "after test_alloc_bytes_public\n");
    test_dealloc_bytes_public();
    fprintf(stderr, "after test_dealloc_bytes_public\n");
    test_copy_disjoint();
    fprintf(stderr, "after test_copy_disjoint\n");
    test_copy_overlap();
    fprintf(stderr, "after test_copy_overlap\n");
    test_fill();
    fprintf(stderr, "after test_fill\n");
    test_exhaustion_public();
    fprintf(stderr, "after test_exhaustion_public\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");

    if (g_failures) {
        fprintf(stderr, "rt_mem_api_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_mem_api_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
