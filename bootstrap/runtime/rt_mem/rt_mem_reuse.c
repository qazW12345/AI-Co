/* bootstrap/runtime/rt_mem/rt_mem_reuse.c
 *
 * AI-Co Stage-0 runtime: reuse policy and poisoning (WP-M0-14b1).
 * See rt_mem_reuse.h for the interface, the ownership boundaries, and
 * the integration contract.
 *
 * Implementation summary:
 *   - the package integrates with the 14a1 core through the two hooks
 *     owned by the core (rt_mem_reg_set_release_hook /
 *     rt_mem_reg_set_reuse_lookup); it never edits the core;
 *   - release hook: overwrites the full freed allocation with byte
 *     0xDD (ADR-004: before the block becomes eligible for
 *     deterministic reuse) and records (addr, size) on a LIFO stack;
 *   - reuse lookup: scans the stack from the top (most recently
 *     released) for the first entry whose size matches the request
 *     exactly and removes it. This is exact-fit reuse in reverse order
 *     of release within a size class (spec sec. 15.1): no split, no
 *     coalesce. The core verifies the returned address defensively and
 *     re-zeroes the block before returning it;
 *   - bookkeeping capacity: the stack holds at most
 *     RT_MEM_REGISTRY_CAPACITY entries; each release pushes at most one
 *     entry and each reuse lookup pops at most one. In a pathological
 *     long-lived release/reuse cycle the stack can reach the declared
 *     capacity (a fresh allocation can recycle a FREE registry slot and
 *     orphan a tracked entry, which is then removed only when a
 *     same-size lookup pops it and the core refuses to activate it);
 *     the guard then leaves the block FREE and poisoned but untracked -
 *     correct, simply not reused. No host-allocator or OS behavior can
 *     make the bookkeeping leak into the observable contract.
 *
 * Windows API usage: none in this file. All Windows calls belong to
 * the 14a1 core (one MEM_RESERVE, fixed-range MEM_COMMIT, GetSystemInfo).
 * The only library function used is memset for poisoning (fully
 * specified observable output in C17; deterministic).
 */
#include "rt_mem_reuse.h"

#include "rt_mem_alloc.h"

#include <string.h>

/* Poison byte of ADR-004 / spec sec. 15.1. */
#define RT_MEM_POISON_BYTE ((unsigned char)0xDD)

/* One freed-block record: start address and exact size. */
typedef struct RtMemReuseEntry {
    uintptr_t addr;
    size_t size;
} RtMemReuseEntry;

/* LIFO stack of freed blocks in release order: index 0 is the oldest
 * release, s_stack_count - 1 the most recent. */
static RtMemReuseEntry s_stack[RT_MEM_REGISTRY_CAPACITY];
static size_t s_stack_count = 0;

/* ---------------------------------------------------------------------------
 * Release hook (WP-M0-14b1, registered via rt_mem_reuse_init)
 * -------------------------------------------------------------------------
 * Called by the core AFTER the LIVE entry has been marked FREE, with
 * the block address and its full size. Overwrites the full allocation
 * with 0xDD (the poisoning point of ADR-004) and records the block for
 * exact-fit reuse. The block is never observed by a subsequent
 * allocation before this hook completes: the runtime is single-threaded
 * and the hook runs inline inside rt_mem_core_release.
 */
static void rt_mem_reuse_on_release(void *addr, size_t size)
{
    memset(addr, (int)RT_MEM_POISON_BYTE, size);

    if (s_stack_count < RT_MEM_REGISTRY_CAPACITY) {
        s_stack[s_stack_count].addr = (uintptr_t)addr;
        s_stack[s_stack_count].size = size;
        s_stack_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Reuse lookup (WP-M0-14b1, registered via rt_mem_reuse_init)
 * -------------------------------------------------------------------------
 * Exact-fit lookup: returns 1 and sets *out_addr to the most recently
 * released free block of EXACTLY `size`, or 0 when none exists. The
 * search starts at the top of the stack (most recent release) and
 * takes the first entry with a matching size, so within a size class
 * the block most recently released is reused first (reverse order of
 * release). The matching entry is removed; relative order of the
 * remaining entries is preserved. The core verifies that the returned
 * address is a FREE registry entry of the exact size and falls through
 * to a fresh block otherwise (defensive contract, rt_mem_alloc.h).
 */
static int rt_mem_reuse_lookup(size_t size, void **out_addr)
{
    size_t i;

    for (i = s_stack_count; i > 0; i--) {
        if (s_stack[i - 1].size == size) {
            size_t j;
            void *addr = (void *)s_stack[i - 1].addr;
            for (j = i - 1; j + 1 < s_stack_count; j++) {
                s_stack[j] = s_stack[j + 1];
            }
            s_stack_count--;
            *out_addr = addr;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Registration and test reset
 * ------------------------------------------------------------------------- */

void rt_mem_reuse_init(void)
{
    rt_mem_reg_set_release_hook(rt_mem_reuse_on_release);
    rt_mem_reg_set_reuse_lookup(rt_mem_reuse_lookup);
}

void rt_mem_reuse_reset(void)
{
    s_stack_count = 0;
}
