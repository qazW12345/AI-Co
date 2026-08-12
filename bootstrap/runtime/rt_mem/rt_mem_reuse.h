/* bootstrap/runtime/rt_mem/rt_mem_reuse.h
 *
 * AI-Co Stage-0 runtime: reuse policy and poisoning (WP-M0-14b1).
 * Implements the deterministic reuse rule of spec sec. 15.1 and the
 * temporal baseline of ADR-004: exact-fit reuse with 0xDD overwrite
 * before reuse, in reverse order of release within a size class; no
 * split, no coalesce.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-14b1 "Reuse policy and poisoning")
 * ---------------------------------------------------------------------------
 *   - 0xDD poisoning: on release, the full allocation is overwritten
 *     with byte 0xDD before the block becomes eligible for reuse;
 *   - exact-fit reuse: a freed block of size S satisfies only a
 *     request of size S (never split, never coalesced);
 *   - reverse order of release within a size class: among free blocks
 *     of the same size, the most recently released is reused first;
 *   - the observable contract: identical allocation/release sequences
 *     yield identical addresses (offsets inside the controlled region;
 *     the region base is an OS-provided environmental input).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - allocation registry and semantics -> WP-M0-14a1
 *     (rt_mem_alloc.*); this package integrates through the two hooks
 *     rt_mem_reg_set_reuse_lookup / rt_mem_reg_set_release_hook and
 *     never edits the core;
 *   - public alloc/dealloc API -> WP-M0-14a2 (rt_mem_api.*);
 *   - duplicate/invalid-release traps AIC-R0812/AIC-R0813 -> WP-M0-14b2
 *     (rt_mem_trap.*).
 *
 * Activation: the policy is opt-in. Call rt_mem_reuse_init() to
 * register the hooks before the first allocation that should consider
 * reuse. Without registration the allocator behaves exactly like the
 * 14a baseline (no poisoning, no reuse).
 *
 * The reuse bookkeeping lives in this package (a LIFO stack of freed
 * block addresses keyed by exact size). Capacity is
 * RT_MEM_REGISTRY_CAPACITY entries; each release pushes at most one
 * entry and each reuse lookup pops at most one. Because a fresh
 * allocation can recycle a FREE registry slot (orphaning a tracked
 * entry, see below), occupancy can in a pathological long-lived
 * release/reuse cycle reach the declared capacity; the release hook
 * then simply does not track the block (it stays FREE and poisoned but
 * is never reused). The core verifies every lookup result defensively
 * (rt_mem_core_alloc falls through to a fresh block if the returned
 * address is not a FREE entry of the exact size), so the allocator
 * stays correct in every case.
 *
 * Registry-slot recycling (14a1 core, unchanged): a fresh allocation
 * of a different size recycles the first non-LIVE registry slot,
 * overwriting a FREE entry whose block may still be tracked here.
 * Such an entry is never reused (the core refuses to activate its
 * address); the outcome stays deterministic and the freed memory
 * remains inside the controlled region until process exit.
 *
 * Record conventions: none. This package emits no diagnostic records;
 * reuse failure is not an error (the allocator falls through to a
 * fresh block) and exhaustion remains an explicit null return per spec
 * sec. 15.1.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_REUSE_H
#define AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_REUSE_H

/* Register the 14b1 hooks with the 14a1 core:
 *   - release hook: 0xDD poisoning + record for reuse;
 *   - reuse lookup: exact-fit, reverse order of release.
 * Idempotent: calling it again re-registers the same callbacks. Must
 * be called before the first allocation that should consider reuse. */
void rt_mem_reuse_init(void);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Clears this package's reuse bookkeeping (the LIFO stack) without
 * touching the core. The core's hooks are cleared by rt_mem_reg_reset
 * (rt_mem_alloc.h); a test that wants a pristine registered state
 * calls rt_mem_reg_reset() + rt_mem_reuse_reset() + rt_mem_reuse_init()
 * so an identical sequence can be replayed from scratch. */
void rt_mem_reuse_reset(void);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_REUSE_H */
