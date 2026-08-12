/* bootstrap/runtime/rt_mem/rt_mem_alloc.c
 *
 * AI-Co Stage-0 runtime: deterministic project-owned allocator core
 * (WP-M0-14a1). See rt_mem_alloc.h for the interface, the ownership
 * boundaries, and the integration contract.
 *
 * Implementation summary:
 *   - one controlled virtual-address region (VirtualAlloc MEM_RESERVE,
 *     RT_MEM_REGION_SIZE bytes) reserved on first use; pages committed
 *     incrementally in whole-page units (MEM_COMMIT); freshly committed
 *     pages are zero, which is the zero-initialization mechanism;
 *   - a bump allocator over the region: fresh blocks are carved at the
 *     first RT_MEM_ALIGNMENT-aligned offset at or after the current
 *     bump; offsets are deterministic; the region base is an
 *     OS-provided environmental input (spec sec. 15.6);
 *   - a fixed registry of live allocations (address, size, state);
 *     deterministic slot selection (first non-LIVE slot in index
 *     order); release marks entries FREE and returns a status that the
 *     14b2 trap package turns into AIC-R0812/AIC-R0813;
 *   - reuse and 0xDD poisoning are NOT implemented here (WP-M0-14b1);
 *     the two hooks are the integration points and are NULL by default.
 *
 * Windows API usage (ADR-004 baseline: Windows 10 22H2 x64):
 *   VirtualAlloc (MEM_RESERVE / MEM_COMMIT), GetSystemInfo (page
 *   size). No CRT heap, no host allocator, no file or process API.
 */
#define WIN32_LEAN_AND_MEAN 1
#include "rt_mem_alloc.h"

#include <windows.h>

#include <string.h>

/* Registry entry states. UNUSED = never allocated (addr 0); LIVE =
 * owned by a caller; FREE = released, available as a slot, never
 * reused for its address at 14a1 (reuse is 14b1's exact-fit policy). */
enum {
    RT_MEM_REG_UNUSED = 0,
    RT_MEM_REG_LIVE = 1,
    RT_MEM_REG_FREE = 2
};

typedef struct RtMemRegEntry {
    uintptr_t addr;   /* start address of the block (0 for UNUSED) */
    size_t size;      /* exact allocation size in bytes */
    uint8_t state;    /* RT_MEM_REG_* */
} RtMemRegEntry;

static RtMemRegEntry s_registry[RT_MEM_REGISTRY_CAPACITY];
static size_t s_live_count = 0;
static size_t s_free_count = 0;

static void *s_region_base = NULL;   /* controlled region base; NULL until first use */
static size_t s_region_used = 0;     /* bump offset: start of the next fresh block */
static size_t s_region_committed = 0;/* committed bytes from base (page-aligned) */
static size_t s_page_size = 0;       /* cached GetSystemInfo page size */

static RtMemReuseLookup s_reuse_lookup = NULL;
static RtMemReleaseHook s_release_hook = NULL;

/* Align `v` up to a power-of-two alignment `a`. */
static size_t rt_mem_align_up(size_t v, size_t a)
{
    return (v + (a - 1)) & ~(a - 1);
}

/* Ensure the controlled region is reserved. Returns 1 on success.
 * A reservation failure is resource exhaustion for the caller
 * (rt_mem_core_alloc returns NULL); it is never a trap. */
static int rt_mem_region_ensure(void)
{
    if (s_region_base != NULL) {
        return 1;
    }
    if (s_page_size == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s_page_size = (si.dwPageSize != 0) ? (size_t)si.dwPageSize : (size_t)4096;
    }
    s_region_base = VirtualAlloc(NULL, RT_MEM_REGION_SIZE,
                                 MEM_RESERVE, PAGE_READWRITE);
    return s_region_base != NULL;
}

/* Commit whole pages so that at least `need` bytes (page-aligned,
 * <= RT_MEM_REGION_SIZE) are committed from the region base. Returns 1
 * on success; on failure the caller treats it as exhaustion (NULL). */
static int rt_mem_region_commit(size_t need)
{
    size_t start;
    size_t count;
    void *p;

    if (need <= s_region_committed) {
        return 1;
    }
    start = s_region_committed;
    count = need - start;
    p = VirtualAlloc((char *)s_region_base + start, count,
                     MEM_COMMIT, PAGE_READWRITE);
    if (p == NULL) {
        return 0;
    }
    /* Defensive internal-contract check: a MEM_COMMIT inside a
     * reserved region returns the requested address. A mismatch cannot
     * happen on the pinned baseline; treating it as exhaustion keeps
     * the allocator from ever returning an address outside the region. */
    if (p != (char *)s_region_base + start) {
        return 0;
    }
    s_region_committed = need;
    return 1;
}

/* Mark the FREE registry entry whose address is `addr` as LIVE again
 * (the reuse path). The entry's size must equal `size` (exact-fit
 * contract, spec sec. 15.1). Returns 1 on success, 0 when `addr` is
 * not a FREE entry of the exact size. */
static int rt_mem_registry_activate(void *addr, size_t size)
{
    size_t i;

    for (i = 0; i < RT_MEM_REGISTRY_CAPACITY; i++) {
        if (s_registry[i].state == RT_MEM_REG_FREE &&
            s_registry[i].addr == (uintptr_t)addr) {
            if (s_registry[i].size != size) {
                return 0;
            }
            s_registry[i].state = RT_MEM_REG_LIVE;
            s_free_count--;
            s_live_count++;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Core allocation (spec sec. 15.1)
 * ------------------------------------------------------------------------- */

void *rt_mem_core_alloc(size_t count)
{
    size_t aligned;
    size_t need;
    size_t i;
    size_t slot;
    void *addr;

    /* Zero-size: no allocation, no state change, never a trap. The
     * region is not even reserved by a zero-size request. */
    if (count == 0) {
        return NULL;
    }

    /* A request larger than the whole region can never fit. Reject
     * before touching any state (also bounds the arithmetic below). */
    if (count > RT_MEM_REGION_SIZE) {
        return NULL;
    }

    /* Reuse hook (WP-M0-14b1), NULL by default. If a registered
     * exact-fit lookup yields a usable FREE entry, activate it and
     * return the re-zeroed block. An unusable lookup result is ignored
     * defensively and falls through to a fresh block. */
    if (s_reuse_lookup != NULL) {
        void *candidate = NULL;
        if (s_reuse_lookup(count, &candidate) != 0 &&
            candidate != NULL &&
            rt_mem_registry_activate(candidate, count)) {
            memset(candidate, 0, count);
            return candidate;
        }
    }

    if (!rt_mem_region_ensure()) {
        return NULL;
    }

    /* Fresh block from the controlled region: first aligned offset at
     * or after the bump. The alignment is RT_MEM_ALIGNMENT (>= 16). */
    aligned = rt_mem_align_up(s_region_used, RT_MEM_ALIGNMENT);
    if (aligned + count > RT_MEM_REGION_SIZE) {
        return NULL; /* region exhausted: explicit null, never a trap */
    }

    /* Commit the pages covering [aligned, aligned + count). */
    need = rt_mem_align_up(aligned + count, s_page_size);
    if (!rt_mem_region_commit(need)) {
        return NULL; /* commit failure: exhaustion */
    }

    /* Registry slot: first non-LIVE entry in index order (deterministic
     * and independent of any host behavior). */
    slot = RT_MEM_REGISTRY_CAPACITY;
    for (i = 0; i < RT_MEM_REGISTRY_CAPACITY; i++) {
        if (s_registry[i].state != RT_MEM_REG_LIVE) {
            slot = i;
            break;
        }
    }
    if (slot == RT_MEM_REGISTRY_CAPACITY) {
        return NULL; /* registry exhausted: explicit null, never a trap */
    }

    addr = (char *)s_region_base + aligned;
    if (s_registry[slot].state == RT_MEM_REG_FREE) {
        s_free_count--;
    }
    s_registry[slot].addr = (uintptr_t)addr;
    s_registry[slot].size = count;
    s_registry[slot].state = RT_MEM_REG_LIVE;
    s_live_count++;
    s_region_used = aligned + count;

    return addr;
}

/* ---------------------------------------------------------------------------
 * Core release (spec sec. 15.1; trap wiring owned by 14b2)
 * ------------------------------------------------------------------------- */

int rt_mem_core_release(void *p)
{
    size_t i;

    /* Documented null no-op (spec sec. 15.1: "Passing null is a
     * no-op"). There is nothing to release for a zero-size request. */
    if (p == NULL) {
        return RT_MEM_REL_OK;
    }

    for (i = 0; i < RT_MEM_REGISTRY_CAPACITY; i++) {
        if (s_registry[i].addr == (uintptr_t)p) {
            if (s_registry[i].state == RT_MEM_REG_FREE) {
                return RT_MEM_REL_DOUBLE; /* already released (14b2: AIC-R0812) */
            }
            if (s_registry[i].state == RT_MEM_REG_UNUSED) {
                continue; /* defensive: stale slot; keep scanning */
            }
            s_registry[i].state = RT_MEM_REG_FREE;
            s_live_count--;
            s_free_count++;
            if (s_release_hook != NULL) {
                /* 14b1 poisoning point (ADR-004: 0xDD overwrite before
                 * the block becomes eligible for deterministic reuse). */
                s_release_hook(p, s_registry[i].size);
            }
            return RT_MEM_REL_OK;
        }
    }
    return RT_MEM_REL_INVALID; /* not a live allocation start (14b2: AIC-R0813) */
}

/* ---------------------------------------------------------------------------
 * Hook registration and introspection
 * ------------------------------------------------------------------------- */

void rt_mem_reg_set_reuse_lookup(RtMemReuseLookup lookup)
{
    s_reuse_lookup = lookup;
}

void rt_mem_reg_set_release_hook(RtMemReleaseHook hook)
{
    s_release_hook = hook;
}

size_t rt_mem_reg_capacity(void)
{
    return RT_MEM_REGISTRY_CAPACITY;
}

size_t rt_mem_reg_live_count(void)
{
    return s_live_count;
}

size_t rt_mem_reg_free_count(void)
{
    return s_free_count;
}

size_t rt_mem_reg_used(void)
{
    return s_region_used;
}

void *rt_mem_reg_region_base(void)
{
    return s_region_base;
}

size_t rt_mem_reg_region_size(void)
{
    return RT_MEM_REGION_SIZE;
}

void rt_mem_reg_reset(void)
{
    size_t i;

    if (s_region_base != NULL) {
        VirtualFree(s_region_base, 0, MEM_RELEASE);
    }
    for (i = 0; i < RT_MEM_REGISTRY_CAPACITY; i++) {
        s_registry[i].addr = 0;
        s_registry[i].size = 0;
        s_registry[i].state = RT_MEM_REG_UNUSED;
    }
    s_live_count = 0;
    s_free_count = 0;
    s_region_base = NULL;
    s_region_used = 0;
    s_region_committed = 0;
    s_page_size = 0;
    s_reuse_lookup = NULL;
    s_release_hook = NULL;
}
