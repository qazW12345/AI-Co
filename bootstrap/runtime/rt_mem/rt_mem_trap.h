/* bootstrap/runtime/rt_mem/rt_mem_trap.h
 *
 * AI-Co Stage-0 runtime: release traps (WP-M0-14b2). Duplicate release
 * and invalid release trap integration per spec sec. 15.5 / 15.8 and
 * DIAGNOSTIC-CONTRACT sec. 10 / 11.8: a double release is the trap
 * AIC-R0812, an invalid release (pointer not from the allocator) is the
 * trap AIC-R0813, and every release trap reports a JSONL trap record on
 * stderr and terminates the process with exit code 70.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-14b2 "Release traps")
 * ---------------------------------------------------------------------------
 *   - the release-status handler registered through the 14a2 hook
 *     rt_mem_api_set_release_status_handler (the mandatory integration
 *     point; this package never edits rt_mem_api.c - manifest rule 4);
 *   - AIC-R0812/AIC-R0813 trap-record emission using the WP-M0-06 diag
 *     record shape (bootstrap/src/diag/diag.h, the trap-record shape
 *     owner named by rt_mem_alloc.h), written to stderr as one JSONL
 *     record (DIAGNOSTIC-CONTRACT sec. 10);
 *   - deterministic process termination with the trap exit code 70
 *     (DIAG_TRAP_EXIT_CODE) on every release trap.
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - allocation registry and semantics -> WP-M0-14a1 (rt_mem_alloc.*);
 *   - public alloc/dealloc API -> WP-M0-14a2 (rt_mem_api.*); the
 *     release-status handler hook is owned there;
 *   - exact-fit reuse and 0xDD poisoning -> WP-M0-14b1 (rt_mem_reuse.*);
 *   - the rt.trap module (rt.trap.report, user trap AIC-U0000) and
 *     file/process I/O -> WP-M0-15 (rt_trap.*).
 *
 * Activation: the traps are opt-in. Call rt_mem_trap_init() to register
 * the release-status handler before the first dealloc_bytes that should
 * report. Without registration the allocator keeps the 14a default:
 * a double/invalid release is a deterministic no-op (rt_mem_api.h),
 * never a trap. The registration is idempotent.
 *
 * Trap-record shape: phase = "trap", severity = "error", recovery =
 * "authoritative", exit_code = 70 (DIAGNOSTIC-CONTRACT sec. 10). The
 * primary span is null: a release trap raised by the C runtime carries
 * no compiler-emitted source mapping at this layer (the runtime has no
 * source location for the failing dealloc_bytes call); the compiler
 * attaches spans when it emits the call site. This matches the
 * user-trap null-span record convention (bootstrap/src/diag/golden/).
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_TRAP_H
#define AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_TRAP_H

#include "../../src/diag/diag.h"

/* Register the 14b2 release-status handler with the 14a2 public API:
 * rt_mem_api_set_release_status_handler(rt_mem_trap_release_status).
 * From then on, a double release (RT_MEM_REL_DOUBLE) emits the AIC-R0812
 * trap record and terminates with exit 70; an invalid release
 * (RT_MEM_REL_INVALID) emits AIC-R0813 and terminates with exit 70.
 * Idempotent: calling it again re-registers the same handler. Must be
 * called before the first dealloc_bytes that should report. */
void rt_mem_trap_init(void);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Formats the release-trap record for `status` into `out` as exactly the
 * JSONL line the trap path would write to stderr (no exit, no stderr
 * write). Returns true and appends the record for a release-trap status
 * (RT_MEM_REL_DOUBLE -> AIC-R0812, RT_MEM_REL_INVALID -> AIC-R0813);
 * returns false and appends nothing for any other status (e.g.
 * RT_MEM_REL_OK) or on record/emission failure. `addr` is the offending
 * pointer reported by the core; it is carried in the record's message
 * and related facts. Used by the release-trap tests to assert the exact
 * record bytes without terminating the test process. */
bool rt_mem_trap_format(int status, void *addr, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_MEM_RT_MEM_TRAP_H */
