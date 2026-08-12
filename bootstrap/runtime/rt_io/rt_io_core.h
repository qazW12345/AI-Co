/* bootstrap/runtime/rt_io/rt_io_core.h
 *
 * AI-Co Stage-0 runtime: rt.io core (WP-M0-15a1). This is the
 * source-visible rt.io surface of spec sec. 15.2 (file handles and
 * open/read/write/close) and the compiler-emitted call subset of spec
 * sec. 15.8, on the Windows 10 22H2 x64 baseline of ADR-004.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-15a1 "rt.io core: handle model and
 * file operations")
 * ---------------------------------------------------------------------------
 *   - the handle model: file handles are `usize` values (runtime-
 *     managed indices into a fixed table). 0 is invalid and is never
 *     returned by a successful operation;
 *   - file operations per spec sec. 15.2: open (modes 0..3), read,
 *     write, close, with deterministic observable behavior and `0` on
 *     failure (never a trap for open/read/write failures: open
 *     exhaustion and I/O errors return explicit result values);
 *   - the read buffer-length contract: `len(buf) >= count` is enforced
 *     and a violation raises the stable runtime trap AIC-R0807 (spec
 *     sec. 15.2/15.5), reported per DIAGNOSTIC-CONTRACT sec. 10 as a
 *     JSONL trap record on stderr with exit code 70 - the same record
 *     shape the WP-M0-14b2 release traps use;
 *   - the integration contract for WP-M0-15a2 (see below): the
 *     invalid-handle failure paths (AIC-R0814) and the standard stream
 *     functions (stdin/stdout/stderr) are owned by 15a2 and plug in
 *     through the hooks registered here; this package never edits
 *     15a2's files and vice versa (manifest rule 4).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - stdio behavior and invalid-handle failures AIC-R0814 -> WP-M0-15a2
 *     (rt_io_stdio.*); the hook rt_io_core_set_invalid_handle_handler
 *     below is the delivery point 15a2 registers its trap handler on;
 *   - process args/exit -> WP-M0-15b (rt_proc.*);
 *   - the rt.trap module (rt.trap.report, user trap AIC-U0000) ->
 *     WP-M0-15c1 (rt_trap.*). Stable runtime traps with fixed codes
 *     (AIC-Rxxxx) are raised by the owning package directly through the
 *     WP-M0-06 diag shape, exactly as WP-M0-14b2 raised AIC-R0812/R0813;
 *   - allocator internals -> WP-M0-14 (rt_mem.*). This package performs
 *     no allocation: the handle table is fixed-size and the path
 *     conversion buffer is a bounded stack buffer, so rt_io_core.c
 *     never calls the allocator and adds no coupling to it.
 *
 * ---------------------------------------------------------------------------
 * Symbol mapping to the AI-Co runtime surface (spec sec. 15.2 / 15.8)
 * ---------------------------------------------------------------------------
 * The AI-Co source-visible functions map to these C symbols:
 *
 *   rt.io.open(path: str, mode: u32) -> usize  ==
 *       size_t rt_io_open(const unsigned char *path_data, size_t path_len,
 *                         uint32_t mode)
 *   rt.io.read(handle: usize, buf: u8[], count: usize) -> usize  ==
 *       size_t rt_io_read(size_t handle, unsigned char *buf_data,
 *                         size_t buf_len, size_t count)
 *   rt.io.write(handle: usize, buf: u8[], count: usize) -> usize  ==
 *       size_t rt_io_write(size_t handle, const unsigned char *buf_data,
 *                          size_t buf_len, size_t count)
 *   rt.io.close(handle: usize) -> void  ==
 *       void rt_io_close(size_t handle)
 *
 * Type mapping (the natural ABI mapping of rt_mem_api.h, spec sec.
 * 15.7): usize is size_t, u32 is uint32_t, u8* is an unsigned byte
 * pointer. `str` and `u8[]` follow the Section 7.2 layouts (data at
 * offset 0, length at offset 8) and are passed as two argument words
 * in consecutive integer/pointer registers per the internal Microsoft
 * x64 calling convention (spec sec. 15.7); the C declarations above
 * spell out those two words as separate parameters so a conforming
 * compiler emits exactly the register pair (e.g. open: RCX=path_data,
 * RDX=path_len, R8=mode; read: RCX=handle, RDX=buf_data, R8=buf_len,
 * R9=count).
 *
 * Determinism: given identical arguments and identical environmental
 * inputs (file contents, available OS resources; spec sec. 15.6), the
 * observable outputs - handle values, bytes read/written, the
 * `0`-on-failure result - are fully deterministic. Handle values are
 * table indices: the first free slot in increasing index order is
 * reused (slot 1 is the first open, a closed slot becomes free for a
 * later open), so an identical open/close sequence yields identical
 * handle values. I/O operates on bytes; no text translation and no
 * buffering beyond what the OS provides (spec sec. 15.2).
 *
 * Record conventions: only the AIC-R0807 trap record is emitted (read
 * with len(buf) < count), with a null primary span - the C runtime has
 * no source mapping for the failing call site; the compiler attaches
 * spans when it emits the call (DIAGNOSTIC-CONTRACT sec. 10 allows a
 * null span, matching the 14b2 trap-record convention). All other
 * failures are explicit return values and never emit records.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_IO_RT_IO_CORE_H
#define AICO_BOOTSTRAP_RUNTIME_RT_IO_RT_IO_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "../../src/diag/diag.h"

/* Handle table capacity: the maximum number of simultaneously open
 * handles the runtime can track. A declared resource bound of the
 * runtime (like the stack limit of spec sec. 15.5 and the allocator
 * registry capacity): an open request that cannot find a free slot is
 * resource exhaustion and returns 0 (never a trap), consistent with
 * the environmental-input model of spec sec. 15.6. */
#define RT_IO_TABLE_CAPACITY ((size_t)256)

/* ---------------------------------------------------------------------------
 * File operations (spec sec. 15.2)
 * ------------------------------------------------------------------------- */

/* Open a file (rt.io.open). `mode`:
 *   0 = read            (OPEN_EXISTING; missing file is a failure -> 0)
 *   1 = write (truncate)(create or truncate to zero)
 *   2 = write (append)  (create if missing, do not truncate; the file
 *                        pointer is positioned at the end on open)
 *   3 = read/write      (create/truncate; both GENERIC_READ and
 *                        GENERIC_WRITE)
 * Any other `mode` value is a failure -> 0.
 *
 * `path_data`/`path_len` is the UTF-8 path (a `str` value, sec. 7.2).
 * The path is converted to UTF-16 deterministically (MultiByteToWideChar,
 * CP_UTF8, MB_ERR_INVALID_CHARS). Returns a handle (a table index in
 * [1, RT_IO_TABLE_CAPACITY]) on success, or 0 on any failure: invalid
 * mode, invalid UTF-8, path too long (> RT_IO_PATH_MAX_BYTES), an
 * embedded NUL in the path (the path truncates at the NUL, matching
 * CreateFileW semantics - deterministic), the file cannot be opened
 * with the requested access/disposition, or the handle table is full
 * (resource exhaustion, never a trap). */
#define RT_IO_PATH_MAX_BYTES ((size_t)32768)
size_t rt_io_open(const unsigned char *path_data, size_t path_len,
                  uint32_t mode);

/* Read up to `count` bytes from `handle` into buf (rt.io.read).
 * `buf_data`/`buf_len` is the destination slice (u8[]); `count` must
 * satisfy `count <= buf_len` - otherwise the runtime raises the stable
 * trap AIC-R0807 (spec sec. 15.2/15.5): a JSONL trap record is written
 * to stderr and the process terminates with exit code 70.
 *
 * Returns the number of bytes read: `count` when the file had at least
 * that many bytes available at the current position, fewer on a short
 * read, 0 at EOF or on I/O failure (spec sec. 15.2 "0 at EOF"; "0 on
 * failure" per WP-M0-15a1 acceptance). The handle must be valid and
 * open (see the failure-handler contract below); a buffer whose length
 * is 0 with count > 0 raises AIC-R0807 before any OS call. */
size_t rt_io_read(size_t handle, unsigned char *buf_data, size_t buf_len,
                  size_t count);

/* Write up to `count` bytes from buf to `handle` (rt.io.write).
 * `buf_data`/`buf_len` is the source slice (u8[]); the function never
 * reads past `buf_len`: at most `count` bytes are written, and if
 * `count > buf_len` only `buf_len` bytes are written (spec sec. 15.2
 * "writes up to count bytes from buf"; the spec attaches no trap to
 * this case, so the runtime clamps deterministically instead).
 * Returns the number of bytes written; 0 on failure (or when count is
 * 0). The handle must be valid and open (see the failure-handler
 * contract below). */
size_t rt_io_write(size_t handle, const unsigned char *buf_data,
                   size_t buf_len, size_t count);

/* Close `handle` (rt.io.close). Closes the OS handle and frees the
 * table slot for reuse. Closing an invalid or already-closed handle is
 * a deterministic no-op at this layer and is delivered to the
 * invalid-handle handler when one is registered (AIC-R0814 wiring is
 * owned by WP-M0-15a2). */
void rt_io_close(size_t handle);

/* ---------------------------------------------------------------------------
 * Integration contract (for WP-M0-15a2)
 * ---------------------------------------------------------------------------
 * Invalid-handle failures (AIC-R0814, spec sec. 15.2: read/write on an
 * invalid handle and close on an invalid or already-closed handle) are
 * owned by WP-M0-15a2. The core detects the condition and delivers it
 * to the registered handler below; 15a2 registers a handler that raises
 * the AIC-R0814 trap record and terminates with exit 70, exactly as
 * WP-M0-14b2 wired AIC-R0812/R0813 through the 14a2 release-status
 * handler hook. With no handler registered (the 15a1 default) an
 * invalid handle is a deterministic no-op: read returns 0, write
 * returns 0, close does nothing - never a trap, never a state change.
 * The handler must be registered before the first operation that should
 * report; registration is idempotent. */

/* The operation that encountered the invalid handle. */
typedef enum RtIoOp {
    RT_IO_OP_READ = 1,
    RT_IO_OP_WRITE = 2,
    RT_IO_OP_CLOSE = 3
} RtIoOp;

typedef void (*RtIoInvalidHandleHandler)(RtIoOp op, size_t handle);

void rt_io_core_set_invalid_handle_handler(RtIoInvalidHandleHandler handler);

/* Register an existing OS handle (a Windows HANDLE) into the handle
 * table and return its index, or 0 when `os_handle` is NULL /
 * INVALID_HANDLE_VALUE or the table is full. Ownership of the OS
 * handle transfers to the runtime: a later rt_io_close on the returned
 * index closes it. This is the integration point WP-M0-15a2 uses to
 * place the standard stream handles (GetStdHandle results) under
 * runtime management for rt.io.stdin/stdout/stderr; 15a2 never edits
 * this file. Slot selection is the same deterministic first-free
 * policy as rt_io_open. */
size_t rt_io_core_register_handle(void *os_handle);

/* ---------------------------------------------------------------------------
 * Introspection (tests, diagnostics, integration)
 * ------------------------------------------------------------------------- */

/* Capacity of the handle table (== RT_IO_TABLE_CAPACITY). */
size_t rt_io_core_capacity(void);

/* Number of currently open handles (registered or opened). */
size_t rt_io_core_open_count(void);

/* True when `handle` is a valid, currently open table index. */
int rt_io_core_handle_is_valid(size_t handle);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Resets the handle table to its initial state: closes every open OS
 * handle, empties the table, and clears the invalid-handle handler.
 * Provided so determinism tests can replay identical open/read/write/
 * close sequences from a pristine state (mirrors rt_mem_reg_reset). */
void rt_io_core_reset(void);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Formats the AIC-R0807 read-buffer trap record for `handle`,
 * `buf_len`, `count` into `out` as exactly the JSONL line the trap
 * path would write to stderr (no exit, no stderr write). Returns true
 * and appends the record; returns false and appends nothing when `out`
 * is NULL or on record/emission failure. Used by the file-operation
 * tests to assert the exact record bytes without terminating the test
 * process; the live trap path shares this constructor. */
bool rt_io_format_read_buf_trap(size_t handle, size_t buf_len, size_t count,
                                DiagBuf *out);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_IO_RT_IO_CORE_H */
