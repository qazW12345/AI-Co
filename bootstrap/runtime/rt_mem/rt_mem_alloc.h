/* bootstrap/runtime/rt_mem/rt_mem_alloc.h
 *
 * AI-Co Stage-0 runtime: deterministic project-owned allocator core
 * (WP-M0-14a1). Internal core interface for the `rt.mem` module of
 * spec sec. 15.1 and the temporal baseline of ADR-004.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-14a1 "Allocation registry and semantics")
 * ---------------------------------------------------------------------------
 *   - allocation registry: every live allocation is tracked in a fixed
 *     registry (address + size + state), with deterministic slot
 *     selection and deterministic release status;
 *   - allocation semantics: zero-initialized blocks, alignment >= 16
 *     (RT_MEM_ALIGNMENT), zero-size -> null no-op with no state change,
 *     null on exhaustion (never a trap), and every returned address
 *     inside one controlled address region;
 *   - integration contracts for the later 14a2/14b1/14b2 packages
 *     (see "Integration contract" below).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - public alloc/dealloc/copy/fill API and integration (the
 *     source-visible rt.mem functions of spec sec. 15.1) ->
 *     WP-M0-14a2 (rt_mem_api.*);
 *   - exact-fit reuse, 0xDD poisoning, reverse-order-of-release ->
 *     WP-M0-14b1 (rt_mem_reuse.*);
 *   - duplicate/invalid-release traps AIC-R0812/AIC-R0813 (spec sec.
 *     15.5, DIAGNOSTIC-CONTRACT sec. 11.x; trap-record shape owned by
 *     WP-M0-06 via bootstrap/src/diag/diag.h) -> WP-M0-14b2
 *     (rt_mem_trap.*);
 *   - rt.trap implementation and file/process I/O -> WP-M0-15.
 *
 * Rule 4 of the manifest sec. 1 binds all packages: a later package
 * must not modify a previously owned artifact. The reuse policy (14b1)
 * and the release traps (14b2) therefore plug in through the two hooks
 * and the release status codes below; they never edit this file.
 *
 * ---------------------------------------------------------------------------
 * Controlled region and determinism
 * ---------------------------------------------------------------------------
 * The allocator reserves one virtual-address region of
 * RT_MEM_REGION_SIZE bytes (VirtualAlloc, MEM_RESERVE) on first use and
 * commits pages from it incrementally (MEM_COMMIT). Every returned
 * address satisfies  base <= p  and  p + count <= base + RT_MEM_REGION_SIZE.
 * Freshly committed pages are guaranteed zero by Windows, which is how
 * zero-initialization is provided at 14a1 (no reuse exists yet; reuse
 * also re-zeroes the block in rt_mem_core_alloc before returning it).
 *
 * The region base address is obtained once from the operating system
 * and is an environmental input (spec sec. 15.6): it may vary between
 * processes (ASLR). What the allocator guarantees is that all offsets
 * inside the region are deterministic: an identical allocation/release
 * sequence yields an identical sequence of offsets, and therefore
 * identical absolute addresses within a given process. No host
 * allocator is ever used per allocation; the only Windows calls are the
 * single MEM_RESERVE and the fixed-range MEM_COMMIT calls.
 *
 * The runtime is single-threaded (the minimal language has no threads
 * and no thread API); the allocator uses no locks.
 *
 * ---------------------------------------------------------------------------
 * Integration contract (for WP-M0-14a2 / 14b1 / 14b2)
 * ---------------------------------------------------------------------------
 *   - rt_mem_core_alloc(size_t)  -> block or NULL. NULL means: zero
 *     size (no-op), region exhausted, registry exhausted, or commit
 *     failure. Never traps.
 *   - rt_mem_core_release(void*) -> RT_MEM_REL_OK / RT_MEM_REL_DOUBLE /
 *     RT_MEM_REL_INVALID. The status is the contract for 14b2's trap
 *     wiring (AIC-R0812 double release, AIC-R0813 invalid release); the
 *     trap itself is NOT raised here.
 *   - Reuse hook: 14b1 registers an exact-fit reuse lookup via
 *     rt_mem_reg_set_reuse_lookup. When registered, rt_mem_core_alloc
 *     asks it for a freed block of exactly `size`; if it returns a
 *     usable FREE registry entry, the block is marked LIVE and
 *     re-zeroed. The lookup must only return addresses of blocks whose
 *     registry entry is FREE with the exact requested size; any other
 *     result is treated defensively as no reuse and falls through to a
 *     fresh block. At 14a1 no hook is registered and no reuse occurs.
 *   - Release hook: 14b1 registers rt_mem_reg_set_release_hook; it is
 *     invoked AFTER a LIVE entry has been marked FREE, with the block
 *     address and its full size. This is the 0xDD poisoning point
 *     (ADR-004: overwrite before the block becomes eligible for
 *     deterministic reuse). NULL by default.
 *   - Registry introspection (rt_mem_reg_*) is provided for tests and
 *     for the 14a2/14b integration; rt_mem_reg_reset is test-only.
 *
 * Record conventions: none. This package emits no diagnostic records;
 * all failures are explicit return values per spec sec. 15.1.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_ALLOC_H
#define AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_ALLOC_H

#include <stddef.h>
#include <stdint.h>

/* Alignment guarantee for every allocation (spec sec. 15.1: at least
 * alignof(max_align) for the target = 16 bytes on x64). */
#define RT_MEM_ALIGNMENT ((size_t)16)

/* Controlled region size in bytes: the allocator reserves this much
 * virtual address space once and never leaves it. Every returned
 * address lies inside [base, base + RT_MEM_REGION_SIZE). */
#define RT_MEM_REGION_SIZE ((size_t)64 * 1024 * 1024)

/* Registry capacity: maximum number of simultaneously LIVE allocations
 * the allocator can track. A request that cannot find a free slot is
 * resource exhaustion (returns null, never a trap). This is a declared
 * resource bound of the runtime (like the stack limit of spec sec.
 * 15.5) and is part of the observable environmental input model of
 * spec sec. 15.6. */
#define RT_MEM_REGISTRY_CAPACITY ((size_t)16384)

/* Release status codes returned by rt_mem_core_release. The trap
 * wiring for AIC-R0812 (double release) / AIC-R0813 (invalid release)
 * is owned by WP-M0-14b2; the status is the integration contract. */
enum {
    RT_MEM_REL_OK = 0,      /* released a live allocation (or null no-op) */
    RT_MEM_REL_DOUBLE = 1,  /* pointer is a registry entry already FREE */
    RT_MEM_REL_INVALID = 2  /* pointer is not a live allocation start */
};

/* Exact-fit reuse lookup hook (WP-M0-14b1). Given a requested size,
 * returns 1 and sets *out_addr to a freed block of EXACTLY that size
 * (most recently released within the size class), or returns 0 when no
 * such free block exists. The returned address must be the start of a
 * FREE registry entry with the exact requested size; rt_mem_core_alloc
 * verifies this defensively and falls through to a fresh block
 * otherwise. NULL (the default) means no reuse. */
typedef int (*RtMemReuseLookup)(size_t size, void **out_addr);

/* Release hook (WP-M0-14b1). Called after a LIVE entry has been marked
 * FREE, with the freed block address and its full size. This is the
 * deterministic 0xDD poisoning point of ADR-004. NULL (the default)
 * means no poisoning. */
typedef void (*RtMemReleaseHook)(void *addr, size_t size);

/* Allocate `count` zero-initialized bytes from the controlled region.
 *
 * Returns:
 *   NULL when count == 0 (no allocation, no state change, never a trap);
 *   NULL when count > RT_MEM_REGION_SIZE (cannot fit; no state change);
 *   NULL on exhaustion (region full, registry full, or commit failure) -
 *     never a trap, per spec sec. 15.1;
 *   otherwise a pointer aligned to at least RT_MEM_ALIGNMENT, inside
 *   the controlled region, pointing to `count` zero bytes.
 *
 * The returned block belongs to the caller (state LIVE) until
 * rt_mem_core_release or process exit. */
void *rt_mem_core_alloc(size_t count);

/* Release an allocation previously returned by rt_mem_core_alloc.
 *
 * Returns:
 *   RT_MEM_REL_OK      when `p` is NULL (documented null no-op, spec
 *                      sec. 15.1) or when `p` is the start of a LIVE
 *                      allocation that is now marked FREE;
 *   RT_MEM_REL_DOUBLE  when `p` is a registry entry already FREE
 *                      (double release; trap wiring is 14b2's);
 *   RT_MEM_REL_INVALID when `p` is not the start of a tracked
 *                      allocation (invalid release; 14b2's).
 *
 * The 0xDD poisoning of freed blocks is deferred to the release hook
 * (14b1) and is not performed at 14a1. */
int rt_mem_core_release(void *p);

/* Register the exact-fit reuse lookup hook (WP-M0-14b1). Passing NULL
 * disables reuse. The hook must be registered before the first
 * allocation that should consider reuse; the registry never calls it
 * when NULL. */
void rt_mem_reg_set_reuse_lookup(RtMemReuseLookup lookup);

/* Register the release hook (WP-M0-14b1). Passing NULL disables it. */
void rt_mem_reg_set_release_hook(RtMemReleaseHook hook);

/* Registry/region introspection (tests, diagnostics, integration). */

/* Capacity of the registry (== RT_MEM_REGISTRY_CAPACITY). */
size_t rt_mem_reg_capacity(void);

/* Number of LIVE allocations currently tracked. */
size_t rt_mem_reg_live_count(void);

/* Number of FREE registry entries (released, not yet reused). */
size_t rt_mem_reg_free_count(void);

/* Bump offset in bytes: the start offset of the next fresh block that
 * will be carved from the controlled region. Consumed state grows
 * monotonically; zero-size requests never change it. */
size_t rt_mem_reg_used(void);

/* Base address of the controlled region (NULL until first use). */
void *rt_mem_reg_region_base(void);

/* Size of the controlled region (== RT_MEM_REGION_SIZE). */
size_t rt_mem_reg_region_size(void);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Resets the allocator to its initial state: releases the controlled
 * region reservation (if any), empties the registry, clears the hooks.
 * Provided so the determinism tests can replay identical allocation
 * sequences from a pristine state. */
void rt_mem_reg_reset(void);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_ALLOC_H */
