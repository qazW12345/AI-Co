/* bootstrap/runtime/rt_io/rt_io_core.c
 *
 * AI-Co Stage-0 runtime: rt.io core (WP-M0-15a1). See rt_io_core.h
 * for the interface, the symbol mapping to the AI-Co runtime surface,
 * and the integration contract for WP-M0-15a2.
 *
 * Implementation summary:
 *   - a fixed handle table (RT_IO_TABLE_CAPACITY entries; index 0 is
 *     reserved and never returned) maps runtime-managed indices to OS
 *     file handles. Slot selection is deterministic: the first free
 *     slot in increasing index order. Closing frees the slot for
 *     reuse; an identical open/close sequence therefore yields
 *     identical handle values (the observable handle contract,
 *     mirroring the allocator's deterministic offset contract);
 *   - open converts the UTF-8 path to UTF-16 (MultiByteToWideChar,
 *     CP_UTF8, MB_ERR_INVALID_CHARS) and calls CreateFileW with the
 *     access/disposition of the requested mode (0 read, 1 write
 *     truncate, 2 write append, 3 read/write create/truncate). Append
 *     positions the file pointer at the end on open
 *     (SetFilePointerEx). Every failure - invalid mode, invalid UTF-8,
 *     over-long path, open failure, full table - returns 0 and never
 *     traps; a created OS handle is closed before returning 0 so no
 *     OS handle leaks on exhaustion;
 *   - read/write call ReadFile/WriteFile on the OS handle with the
 *     byte count; EOF and I/O failures both yield 0 from read (spec
 *     sec. 15.2 "0 at EOF"; "0 on failure" per acceptance), write
 *     returns the bytes actually written and 0 on failure. I/O is
 *     byte-oriented with no text translation and no buffering beyond
 *     the OS (spec sec. 15.2);
 *   - the read buffer-length contract (count <= buf_len) is enforced
 *     and a violation raises the stable trap AIC-R0807 through the
 *     WP-M0-06 diag shape (phase trap, severity error, recovery
 *     authoritative, exit code 70, null primary span), exactly like
 *     the WP-M0-14b2 release traps;
 *   - invalid handles (0, out of range, or closed) never touch the
 *     OS: read returns 0, write returns 0, close does nothing, and
 *     the condition is delivered to the registered invalid-handle
 *     handler when one is present. The AIC-R0814 trap wiring is owned
 *     by WP-M0-15a2 via rt_io_core_set_invalid_handle_handler.
 *
 * Windows API usage (ADR-004 baseline: Windows 10 22H2 x64):
 *   CreateFileW, ReadFile, WriteFile, CloseHandle, SetFilePointerEx,
 *   MultiByteToWideChar. Sharing is FILE_SHARE_READ | FILE_SHARE_WRITE
 *   | FILE_SHARE_DELETE for every open: the least restrictive
 *   deterministic constant, so a file already open by this or another
 *   process does not fail our open with a sharing violation (in
 *   particular, re-opening the same path with create/truncate while a
 *   previous handle is still open is permitted). No CRT heap, no
 *   allocator calls, no file-buffering layer.
 */
#define WIN32_LEAN_AND_MEAN 1
#include "rt_io_core.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Handle table. s_handles[i] is the OS handle for runtime handle i;
 * index 0 is reserved (invalid, never returned). NULL = free slot. */
static void *s_handles[RT_IO_TABLE_CAPACITY + 1];
static size_t s_open_count = 0;

/* Invalid-handle handler for WP-M0-15a2; NULL by default (15a1). */
static RtIoInvalidHandleHandler s_invalid_handle_handler = NULL;

/* ---------------------------------------------------------------------------
 * AIC-R0807 trap record (read buffer too small)
 * ------------------------------------------------------------------------- */

bool rt_io_format_read_buf_trap(size_t handle, size_t buf_len, size_t count,
                                DiagBuf *out)
{
    char message[96];
    DiagRecord *rec;
    bool ok;

    if (out == NULL) {
        return false;
    }

    /* Message states the failing operation and values (DIAGNOSTIC-
     * CONTRACT sec. 10); formatting is explicit (never %p) so the
     * record is byte-identical across the pinned toolchains. */
    snprintf(message, sizeof(message),
             "read buffer too small: count %llu > buf length %llu",
             (unsigned long long)count, (unsigned long long)buf_len);

    rec = diag_trap_record("AIC-R0807", message, NULL);
    if (rec == NULL) {
        return false;
    }

    ok = diag_record_add_related_str(rec, "operation", "read") &&
         diag_record_add_related_int(rec, "handle", (int64_t)handle) &&
         diag_record_add_related_int(rec, "buf_len", (int64_t)buf_len) &&
         diag_record_add_related_int(rec, "count", (int64_t)count) &&
         diag_emit_record(out, rec);

    diag_record_free(rec);
    return ok;
}

/* Live trap path for the read buffer-length violation: emit the
 * AIC-R0807 record to stderr and terminate with the trap exit code.
 * Never returns. */
static void rt_io_trap_read_buf(size_t handle, size_t buf_len, size_t count)
{
    DiagBuf buf;

    diag_buf_init(&buf);
    (void)rt_io_format_read_buf_trap(handle, buf_len, count, &buf);
    if (diag_buf_ok(&buf)) {
        (void)diag_buf_write_file(&buf, stderr);
        fflush(stderr);
    }
    diag_buf_free(&buf);

    /* Always terminate with the trap exit code, even if record
     * emission could not complete (e.g. out of memory): a read
     * buffer-length violation is a trap per spec sec. 15.2/15.5,
     * never a silent return. */
    exit(DIAG_TRAP_EXIT_CODE);
}

/* ---------------------------------------------------------------------------
 * Handle table helpers
 * ------------------------------------------------------------------------- */

/* First free slot in [1, RT_IO_TABLE_CAPACITY], or 0 when the table is
 * full. Deterministic: increasing index order. */
static size_t rt_io_table_first_free(void)
{
    size_t i;

    for (i = 1; i <= RT_IO_TABLE_CAPACITY; i++) {
        if (s_handles[i] == NULL) {
            return i;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * File operations (spec sec. 15.2)
 * ------------------------------------------------------------------------- */

size_t rt_io_open(const unsigned char *path_data, size_t path_len,
                  uint32_t mode)
{
    DWORD desired_access;
    DWORD creation_disposition;
    wchar_t wide[RT_IO_PATH_MAX_BYTES + 1];
    int wlen;
    HANDLE h;
    size_t slot;

    /* Mode validation (spec sec. 15.2): any value other than 0..3 is a
     * failure -> 0. */
    switch (mode) {
    case 0: /* read */
        desired_access = GENERIC_READ;
        creation_disposition = OPEN_EXISTING;
        break;
    case 1: /* write (truncate) */
        desired_access = GENERIC_WRITE;
        creation_disposition = CREATE_ALWAYS;
        break;
    case 2: /* write (append) */
        desired_access = GENERIC_WRITE;
        creation_disposition = OPEN_ALWAYS;
        break;
    case 3: /* read/write (create/truncate) */
        desired_access = GENERIC_READ | GENERIC_WRITE;
        creation_disposition = CREATE_ALWAYS;
        break;
    default:
        return 0;
    }

    /* Path bound (documented in rt_io_core.h). */
    if (path_len > RT_IO_PATH_MAX_BYTES) {
        return 0;
    }

    /* UTF-8 -> UTF-16. MultiByteToWideChar with MB_ERR_INVALID_CHARS
     * is deterministic: invalid UTF-8 fails the conversion and open
     * returns 0 (a str value is guaranteed valid UTF-8 by the
     * language, so this path is defensive only). An embedded NUL
     * converts to L'\0' and CreateFileW truncates the path there -
     * deterministic and documented. */
    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               (const char *)path_data, (int)path_len,
                               wide, (int)(path_len + 1));
    if (wlen == 0) {
        return 0;
    }
    wide[wlen] = L'\0';

    h = CreateFileW(wide, desired_access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, creation_disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    /* Append mode: position the file pointer at the end on open. */
    if (mode == 2) {
        LARGE_INTEGER off;
        off.QuadPart = 0;
        if (!SetFilePointerEx(h, off, NULL, FILE_END)) {
            CloseHandle(h);
            return 0;
        }
    }

    slot = rt_io_table_first_free();
    if (slot == 0) {
        /* Table full: resource exhaustion, never a trap. Close the OS
         * handle so no handle leaks. */
        CloseHandle(h);
        return 0;
    }

    s_handles[slot] = (void *)h;
    s_open_count++;
    return slot;
}

size_t rt_io_read(size_t handle, unsigned char *buf_data, size_t buf_len,
                  size_t count)
{
    HANDLE h;
    DWORD want;
    DWORD got = 0;

    if (!rt_io_core_handle_is_valid(handle)) {
        if (s_invalid_handle_handler != NULL) {
            s_invalid_handle_handler(RT_IO_OP_READ, handle);
        }
        return 0;
    }

    /* Buffer-length contract (spec sec. 15.2): must have
     * len(buf) >= count, else the stable trap AIC-R0807. */
    if (count > buf_len) {
        rt_io_trap_read_buf(handle, buf_len, count);
    }
    if (count == 0) {
        return 0;
    }

    h = (HANDLE)s_handles[handle];

    /* ReadFile takes a DWORD count. count <= buf_len here, and a slice
     * longer than 4 GiB is beyond the OS call's range; clamp the OS
     * request so a short read (returned byte count) is the observable
     * result, deterministically. */
    want = (count > (size_t)0xFFFFFFFFu) ? (DWORD)0xFFFFFFFFu
                                         : (DWORD)count;
    if (!ReadFile(h, buf_data, want, &got, NULL)) {
        return 0;
    }
    return (size_t)got;
}

size_t rt_io_write(size_t handle, const unsigned char *buf_data,
                   size_t buf_len, size_t count)
{
    HANDLE h;
    size_t effective;
    DWORD want;
    DWORD written = 0;

    if (!rt_io_core_handle_is_valid(handle)) {
        if (s_invalid_handle_handler != NULL) {
            s_invalid_handle_handler(RT_IO_OP_WRITE, handle);
        }
        return 0;
    }

    /* Never read past the source slice: at most min(count, buf_len)
     * bytes are written (spec sec. 15.2 "writes up to count bytes
     * from buf"; no trap is attached to this case, so the runtime
     * clamps deterministically). */
    effective = (count < buf_len) ? count : buf_len;
    if (effective == 0) {
        return 0;
    }

    h = (HANDLE)s_handles[handle];
    want = (effective > (size_t)0xFFFFFFFFu) ? (DWORD)0xFFFFFFFFu
                                             : (DWORD)effective;
    if (!WriteFile(h, buf_data, want, &written, NULL)) {
        return 0;
    }
    return (size_t)written;
}

void rt_io_close(size_t handle)
{
    if (!rt_io_core_handle_is_valid(handle)) {
        if (s_invalid_handle_handler != NULL) {
            s_invalid_handle_handler(RT_IO_OP_CLOSE, handle);
        }
        return;
    }

    /* CloseHandle failure is not observable to the caller (close is
     * void per spec sec. 15.2); the slot is freed regardless so the
     * runtime state stays deterministic and the index becomes invalid
     * for later operations. */
    (void)CloseHandle((HANDLE)s_handles[handle]);
    s_handles[handle] = NULL;
    s_open_count--;
}

/* ---------------------------------------------------------------------------
 * Integration contract (for WP-M0-15a2)
 * ------------------------------------------------------------------------- */

void rt_io_core_set_invalid_handle_handler(RtIoInvalidHandleHandler handler)
{
    s_invalid_handle_handler = handler;
}

size_t rt_io_core_register_handle(void *os_handle)
{
    size_t slot;

    if (os_handle == NULL || os_handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    slot = rt_io_table_first_free();
    if (slot == 0) {
        return 0;
    }
    s_handles[slot] = os_handle;
    s_open_count++;
    return slot;
}

/* ---------------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------------- */

size_t rt_io_core_capacity(void)
{
    return RT_IO_TABLE_CAPACITY;
}

size_t rt_io_core_open_count(void)
{
    return s_open_count;
}

int rt_io_core_handle_is_valid(size_t handle)
{
    return handle >= 1 && handle <= RT_IO_TABLE_CAPACITY &&
           s_handles[handle] != NULL;
}

void rt_io_core_reset(void)
{
    size_t i;

    for (i = 1; i <= RT_IO_TABLE_CAPACITY; i++) {
        if (s_handles[i] != NULL) {
            (void)CloseHandle((HANDLE)s_handles[i]);
            s_handles[i] = NULL;
        }
    }
    s_open_count = 0;
    s_invalid_handle_handler = NULL;
}
