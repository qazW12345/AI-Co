/* bootstrap/runtime/rt_io/rt_io_stdio.h
 *
 * AI-Co Stage-0 runtime: rt.io stdio and failure paths (WP-M0-15a2).
 * This is the source-visible rt.io stdio surface of spec sec. 15.2
 * (rt.io.stdin/stdout/stderr) and the AIC-R0814 invalid-handle trap
 * wiring for read/write/close on invalid or already-closed handles.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-15a2 "rt.io stdio and failure paths")
 * ---------------------------------------------------------------------------
 *   - the standard stream functions rt.io.stdin/stdout/stderr, returning
 *     the runtime-managed handle for the process's standard streams
 *     (spec sec. 15.2: "returns the standard stream handles (never 0)").
 *     Each stream's OS handle (GetStdHandle result) is registered into
 *     the 15a1 handle table on first use (lazy, idempotent) through the
 *     integration point rt_io_core_register_handle; repeated calls
 *     return the cached handle and consume no additional table slots;
 *   - the AIC-R0814 invalid-handle trap wiring (spec sec. 15.2/15.5):
 *     read/write on an invalid handle and close on an invalid or
 *     already-closed handle report a JSONL trap record on stderr
 *     (DIAGNOSTIC-CONTRACT sec. 10) and terminate with exit code 70.
 *     Activation is opt-in via rt_io_stdio_init(), exactly mirroring
 *     the WP-M0-14b2 release traps: without registration the 15a1
 *     default is preserved (invalid handles are deterministic no-ops:
 *     read 0, write 0, close does nothing - never a trap).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - handle model, open/read/write/close, the AIC-R0807 read-buffer
 *     trap, and the integration hooks used below -> WP-M0-15a1
 *     (rt_io_core.*); this package only calls into them;
 *   - process args/exit -> WP-M0-15b (rt_proc.*);
 *   - the rt.trap module (rt.trap.report, user trap AIC-U0000) ->
 *     WP-M0-15c1 (rt_trap.*). Stable runtime traps with fixed codes
 *     (AIC-Rxxxx) are raised by the owning package directly through the
 *     WP-M0-06 diag shape, exactly as 14b2 raised AIC-R0812/R0813.
 *
 * ---------------------------------------------------------------------------
 * Symbol mapping to the AI-Co runtime surface (spec sec. 15.2 / 15.8)
 * ---------------------------------------------------------------------------
 *   rt.io.stdin()  -> usize  ==  size_t rt_io_stdin(void)
 *   rt.io.stdout() -> usize  ==  size_t rt_io_stdout(void)
 *   rt.io.stderr() -> usize  ==  size_t rt_io_stderr(void)
 *
 * The compiler never emits these implicitly (spec sec. 15.8: the
 * compiler-emitted table covers open/read/write/close only); they are
 * source-visible runtime functions a program may call directly.
 *
 * Determinism: the stream functions register each standard stream
 * exactly once (first use); on an empty table the handles are the
 * deterministic first-free slots 1 (stdin), 2 (stdout), 3 (stderr), so
 * an identical call sequence yields identical handle values (the 15a1
 * handle contract). A process whose OS provides no standard handle
 * (GetStdHandle returns NULL or INVALID_HANDLE_VALUE - e.g. a
 * detached/windowed process) or whose handle table is full (resource
 * exhaustion, never a trap) yields 0 from the corresponding function:
 * the "0 on failure" result, consistent with open/read/write. Using a
 * returned handle after the program itself closed it (rt_io_close)
 * makes the index invalid and, with the handler registered, traps
 * AIC-R0814 on the next read/write/close.
 *
 * Trap record conventions: only the AIC-R0814 record is emitted by
 * this package, with a null primary span (the C runtime has no source
 * mapping for the failing call site; the compiler attaches spans when
 * it emits the call - DIAGNOSTIC-CONTRACT sec. 10), phase "trap",
 * severity "error", recovery "authoritative", exit_code 70, and the
 * related facts operation (read/write/close) and handle. This matches
 * the 15a1 AIC-R0807 and 14b2 AIC-R0812/R0813 record conventions.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_IO_RT_IO_STDIO_H
#define AICO_BOOTSTRAP_RUNTIME_RT_IO_RT_IO_STDIO_H

#include <stddef.h>

#include "rt_io_core.h"

/* Register the AIC-R0814 invalid-handle trap wiring with the 15a1 core:
 * from then on, read/write on an invalid handle and close on an
 * invalid or already-closed handle emit the AIC-R0814 trap record to
 * stderr and terminate with exit code 70. Idempotent: calling it again
 * re-registers the same handler. Without this call the 15a1 default is
 * preserved (deterministic no-op, never a trap). Must be called before
 * the first operation that should report. The standard stream
 * functions work with or without this registration. */
void rt_io_stdio_init(void);

/* Standard stream handles (rt.io.stdin/stdout/stderr). Each function
 * registers the stream's OS handle into the runtime handle table on
 * first use and returns the cached runtime handle (never 0 in a
 * process whose OS provides the standard handle; 0 only when the OS
 * provides no handle for the stream or the table is full - "0 on
 * failure", resource exhaustion never traps). */
size_t rt_io_stdin(void);
size_t rt_io_stdout(void);
size_t rt_io_stderr(void);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Formats the AIC-R0814 invalid-handle trap record for `op`/`handle`
 * into `out` as exactly the JSONL line the trap path would write to
 * stderr (no exit, no stderr write). Returns true and appends the
 * record for the known operations (RT_IO_OP_READ -> "read",
 * RT_IO_OP_WRITE -> "write", RT_IO_OP_CLOSE -> "close"); returns false
 * and appends nothing for any other op value or when `out` is NULL.
 * Used by the stdio/failure-path tests to assert the exact record
 * bytes without terminating the test process; the live trap path
 * shares this constructor. */
bool rt_io_format_invalid_handle_trap(RtIoOp op, size_t handle,
                                      DiagBuf *out);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_IO_RT_IO_STDIO_H */
