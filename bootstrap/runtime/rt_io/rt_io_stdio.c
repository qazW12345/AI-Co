/* bootstrap/runtime/rt_io/rt_io_stdio.c
 *
 * AI-Co Stage-0 runtime: rt.io stdio and failure paths (WP-M0-15a2).
 * See rt_io_stdio.h for the interface, the symbol mapping to the
 * AI-Co runtime surface, and the ownership boundaries.
 *
 * Implementation summary:
 *   - the standard stream functions register the process's OS standard
 *     handles (GetStdHandle results) into the 15a1 handle table on
 *     first use through rt_io_core_register_handle (the integration
 *     point owned by 15a1), cache the runtime handle per stream, and
 *     return it; repeated calls are idempotent and consume no further
 *     table slots. On an empty table the handles are the deterministic
 *     first-free slots 1 (stdin), 2 (stdout), 3 (stderr). A missing OS
 *     standard handle (GetStdHandle returns NULL or
 *     INVALID_HANDLE_VALUE) or a full table yields 0 (the "0 on
 *     failure" result; resource exhaustion never traps);
 *   - the AIC-R0814 invalid-handle trap wiring is opt-in (mirroring
 *     the WP-M0-14b2 release traps): rt_io_stdio_init() registers the
 *     invalid-handle handler with the 15a1 core. From then on, every
 *     read/write on an invalid handle and every close on an invalid or
 *     already-closed handle emits the AIC-R0814 JSONL trap record on
 *     stderr (DIAGNOSTIC-CONTRACT sec. 10) and terminates the process
 *     with exit code 70 (DIAG_TRAP_EXIT_CODE). Without registration
 *     the 15a1 default is preserved: invalid handles are deterministic
 *     no-ops (read 0, write 0, close does nothing).
 *
 * Windows API usage (ADR-004 baseline: Windows 10 22H2 x64):
 *   GetStdHandle only. Registration and all I/O go through the 15a1
 *   core (rt_io_core_register_handle, rt_io_read/write/close); no
 *   allocator calls, no CRT heap, no file-buffering layer. Trap-record
 *   emission goes to stderr through the WP-M0-06 emitter and
 *   termination uses the C library exit() with the trap exit code,
 *   exactly like the 15a1/14b2 trap paths.
 */
#define WIN32_LEAN_AND_MEAN 1
#include "rt_io_stdio.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached runtime handles for the standard streams; 0 = not yet
 * registered (a 0 result is never cached, so a later call retries). */
static size_t s_stdin_handle = 0;
static size_t s_stdout_handle = 0;
static size_t s_stderr_handle = 0;

/* ---------------------------------------------------------------------------
 * AIC-R0814 trap record (invalid/closed file handle)
 * ------------------------------------------------------------------------- */

bool rt_io_format_invalid_handle_trap(RtIoOp op, size_t handle, DiagBuf *out)
{
    const char *opname;
    char message[96];
    DiagRecord *rec;
    bool ok;

    if (out == NULL) {
        return false;
    }

    /* Operation names (deterministic; also the related "operation"
     * fact). Only the operations the core can deliver are accepted. */
    switch (op) {
    case RT_IO_OP_READ:
        opname = "read";
        break;
    case RT_IO_OP_WRITE:
        opname = "write";
        break;
    case RT_IO_OP_CLOSE:
        opname = "close";
        break;
    default:
        /* Not a deliverable invalid-handle operation: no record. */
        return false;
    }

    /* Message states the failing operation and the offending handle
     * (DIAGNOSTIC-CONTRACT sec. 10); formatting is explicit (never %p)
     * so the record is byte-identical across the pinned toolchains. */
    snprintf(message, sizeof(message),
             "invalid/closed file handle for %s: handle %llu",
             opname, (unsigned long long)handle);

    rec = diag_trap_record("AIC-R0814", message, NULL);
    if (rec == NULL) {
        return false;
    }

    ok = diag_record_add_related_str(rec, "operation", opname) &&
         diag_record_add_related_int(rec, "handle", (int64_t)handle) &&
         diag_emit_record(out, rec);

    diag_record_free(rec);
    return ok;
}

/* Live trap path for an invalid/closed handle: emit the AIC-R0814
 * record to stderr and terminate with the trap exit code. Never
 * returns (a failed release-style trap is authoritative). */
static void rt_io_stdio_invalid_handle(RtIoOp op, size_t handle)
{
    DiagBuf buf;

    diag_buf_init(&buf);
    (void)rt_io_format_invalid_handle_trap(op, handle, &buf);
    if (diag_buf_ok(&buf)) {
        (void)diag_buf_write_file(&buf, stderr);
        fflush(stderr);
    }
    diag_buf_free(&buf);

    /* Always terminate with the trap exit code, even if record
     * emission could not complete (e.g. out of memory): an
     * invalid-handle operation is a trap per spec sec. 15.2/15.5,
     * never a silent return. */
    exit(DIAG_TRAP_EXIT_CODE);
}

/* ---------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

void rt_io_stdio_init(void)
{
    rt_io_core_set_invalid_handle_handler(rt_io_stdio_invalid_handle);
}

/* ---------------------------------------------------------------------------
 * Standard streams
 * ------------------------------------------------------------------------- */

/* Register one standard stream OS handle into the core table, cache
 * the runtime handle, and return it. `std_handle` is the GetStdHandle
 * selector (STD_INPUT_HANDLE / STD_OUTPUT_HANDLE / STD_ERROR_HANDLE);
 * `cache` is this stream's cached runtime handle (0 = not yet
 * registered). A nonzero cached value is returned directly (lazy,
 * idempotent, no extra table slots). Returns 0 when the OS provides no
 * handle for the stream or the table is full (never a trap). */
static size_t rt_io_stdio_register_stream(DWORD std_handle, size_t *cache)
{
    HANDLE os;
    size_t slot;

    if (*cache != 0) {
        return *cache;
    }

    os = GetStdHandle(std_handle);
    slot = rt_io_core_register_handle((void *)os);
    if (slot != 0) {
        *cache = slot;
    }
    return slot;
}

size_t rt_io_stdin(void)
{
    return rt_io_stdio_register_stream(STD_INPUT_HANDLE, &s_stdin_handle);
}

size_t rt_io_stdout(void)
{
    return rt_io_stdio_register_stream(STD_OUTPUT_HANDLE, &s_stdout_handle);
}

size_t rt_io_stderr(void)
{
    return rt_io_stdio_register_stream(STD_ERROR_HANDLE, &s_stderr_handle);
}
