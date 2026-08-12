/* bootstrap/runtime/rt_mem/rt_mem_api.h
 *
 * AI-Co Stage-0 runtime: public allocator API (WP-M0-14a2). This is
 * the source-visible rt.mem surface of spec sec. 15.1 / 15.8 and the
 * temporal baseline of ADR-004: alloc_bytes, dealloc_bytes, copy,
 * fill.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-14a2 "Public allocator API and
 * integration")
 * ---------------------------------------------------------------------------
 *   - the public allocator API: alloc_bytes/dealloc_bytes/copy/fill,
 *     implemented as thin deterministic wrappers over the 14a1 core
 *     (rt_mem_alloc.*) and standard byte operations;
 *   - registry integration: every allocation goes through the 14a1
 *     allocation registry and controlled region; no host allocator is
 *     ever used per allocation (the only Windows calls are the single
 *     MEM_RESERVE and fixed-range MEM_COMMIT calls made by the core);
 *   - the integration contract for the 14b packages (see "Integration
 *     contract" below): reuse/0xDD (14b1) and the duplicate/invalid
 *     release traps AIC-R0812/AIC-R0813 (14b2).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - allocation registry and semantics -> WP-M0-14a1
 *     (rt_mem_alloc.*); this package only calls into it;
 *   - exact-fit reuse and 0xDD poisoning -> WP-M0-14b1
 *     (rt_mem_reuse.*); the core hooks (rt_mem_reg_set_reuse_lookup /
 *     rt_mem_reg_set_release_hook) are the integration points;
 *   - duplicate/invalid-release traps AIC-R0812/AIC-R0813 -> WP-M0-14b2
 *     (rt_mem_trap.*), wired through the release-status handler below;
 *   - rt.trap implementation and file/process I/O -> WP-M0-15.
 *
 * Rule 4 of the manifest sec. 1 binds all packages: a later package
 * must not modify a previously owned artifact. The 14b packages
 * therefore integrate through the hooks owned here and in rt_mem_alloc.h;
 * they never edit this file.
 *
 * ---------------------------------------------------------------------------
 * Symbol mapping to the AI-Co runtime surface
 * ---------------------------------------------------------------------------
 * The AI-Co source-visible functions (spec sec. 15.1, compiler-emitted
 * call table sec. 15.8) map to these C symbols:
 *
 *   rt.mem.alloc_bytes(count: usize) -> u8*  ==  rt_mem_alloc_bytes
 *   rt.mem.dealloc_bytes(p: u8*) -> void     ==  rt_mem_dealloc_bytes
 *   rt.mem.copy(dst: u8*, src: u8*, count)   ==  rt_mem_copy
 *   rt.mem.fill(dst: u8*, value: u8, count)  ==  rt_mem_fill
 *
 * The C types are the natural ABI mapping: u8* is an unsigned byte
 * pointer (void* in C, identical representation), usize is size_t, and
 * the internal calling convention is the Microsoft x64 convention of
 * spec sec. 15.7 (default C ABI on the pinned toolchains).
 *
 * Determinism: the observable behavior of every function below is fully
 * specified by spec sec. 15.1 and the C17 standard library contracts
 * used (memmove/memset have fully-specified observable output), so the
 * functions contribute no nondeterminism and no host-allocator identity.
 *
 * Record conventions: none. This package emits no diagnostic records;
 * allocation failures are explicit null return values per spec sec.
 * 15.1, and release status is delivered to the 14b2 handler below.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_API_H
#define AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_API_H

#include <stddef.h>

#include "rt_mem_alloc.h"

/* ---------------------------------------------------------------------------
 * Public allocator API (spec sec. 15.1)
 * ------------------------------------------------------------------------- */

/* Allocate `count` zero-initialized bytes (rt.mem.alloc_bytes).
 *
 * Returns NULL when count == 0 (no allocation, no state change, never a
 * trap); NULL when count > RT_MEM_REGION_SIZE (cannot fit); NULL on
 * exhaustion (region full, registry full, or commit failure) - never a
 * trap; otherwise a pointer aligned to at least RT_MEM_ALIGNMENT inside
 * the controlled region, pointing to `count` zero bytes.
 *
 * The block belongs to the caller (state LIVE) until
 * rt_mem_dealloc_bytes or process exit. */
void *rt_mem_alloc_bytes(size_t count);

/* Release an allocation previously returned by rt_mem_alloc_bytes
 * (rt.mem.dealloc_bytes). Passing NULL is a documented no-op (spec
 * sec. 15.1).
 *
 * The registry marks the block FREE and (when 14b1 registers its
 * release hook) the 0xDD poisoning runs in the core. A double release
 * or a release of a pointer that is not a live allocation start is
 * detected by the core (RT_MEM_REL_DOUBLE / RT_MEM_REL_INVALID) and
 * delivered to the registered release-status handler. With no handler
 * registered (the 14a2 default) the release is a deterministic no-op:
 * no state change, never a trap. The AIC-R0812/AIC-R0813 trap wiring is
 * owned by WP-M0-14b2 via rt_mem_api_set_release_status_handler. */
void rt_mem_dealloc_bytes(void *p);

/* Copy `count` bytes from src to dst (rt.mem.copy). Overlapping regions
 * are handled as if a temporary buffer were used (memmove semantics,
 * spec sec. 15.1: deterministic). count == 0 is a no-op. Callers must
 * ensure dst/src point to at least `count` accessible bytes; an
 * inaccessible address faults (AIC-R0811 is a hardware-fault-derived
 * trap, spec sec. 12.8, whose record wiring is owned by later packages). */
void rt_mem_copy(void *dst, const void *src, size_t count);

/* Fill `count` bytes at dst with `value` (rt.mem.fill). count == 0 is a
 * no-op. Callers must ensure dst points to at least `count` accessible
 * bytes (AIC-R0811 as for rt_mem_copy). */
void rt_mem_fill(void *dst, unsigned char value, size_t count);

/* ---------------------------------------------------------------------------
 * Integration contract (for WP-M0-14b2)
 * ---------------------------------------------------------------------------
 * Release-status handler: called by rt_mem_dealloc_bytes exactly when
 * the core reports a status other than RT_MEM_REL_OK (RT_MEM_REL_DOUBLE
 * or RT_MEM_REL_INVALID), with the status and the offending pointer.
 * This is the deterministic delivery point for the 14b2 trap wiring
 * (AIC-R0812 double release, AIC-R0813 invalid release; spec sec. 15.5
 * / DIAGNOSTIC-CONTRACT sec. 11.8). NULL (the default) disables
 * reporting: double/invalid releases become deterministic no-ops. The
 * handler must be registered before the first dealloc_bytes that should
 * report; 14b2 owns the registration and never edits this file. */
typedef void (*RtMemReleaseStatusHandler)(int status, void *addr);

void rt_mem_api_set_release_status_handler(RtMemReleaseStatusHandler handler);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_API_H */
