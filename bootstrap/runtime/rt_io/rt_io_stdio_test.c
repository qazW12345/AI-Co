/* bootstrap/runtime/rt_io/rt_io_stdio_test.c
 *
 * WP-M0-15a2 rt.io stdio and failure-path tests: the standard stream
 * functions rt.io.stdin/stdout/stderr behave per spec sec. 15.2
 * (valid, deterministic, nonzero runtime handles; "0 on failure" only
 * when the OS provides no standard handle or the table is full), and
 * read/write/close on an invalid or already-closed handle raise the
 * stable trap AIC-R0814: a JSONL trap record on stderr with exit code
 * 70 (DIAGNOSTIC-CONTRACT sec. 10, same shape as the WP-M0-15a1
 * AIC-R0807 and WP-M0-14b2 AIC-R0812/R0813 traps).
 *
 * Two layers of testing:
 *   1. In-process tests assert the exact AIC-R0814 record bytes via
 *      rt_io_format_invalid_handle_trap() (the test/diagnostic-only
 *      constructor, rt_io_stdio.h), the deterministic standard-stream
 *      handle values and caching, and the "0 on failure" result when
 *      the handle table is full.
 *   2. Trap-path tests spawn this same executable as a child process
 *      with a mode argument (--child-invalid-read / -write / -close /
 *      --child-closed-close / --child-nohandler-invalid). The child
 *      performs the real operation; a live trap terminates the child
 *      with exit 70 and writes the JSONL record to stderr. The parent
 *      captures the child's stderr through an anonymous pipe and
 *      asserts the exit code and record fields. Stdio behavior is
 *      tested end-to-end in child processes too (--child-stdio-echo /
 *      --child-stdio-handles): the parent controls all three standard
 *      streams through pipes, so the child's GetStdHandle results are
 *      real, captured pipe handles and the parent asserts the exact
 *      bytes read/written through rt.io.
 *
 * The parent process never registers the trap handler and never uses
 * an invalid handle with the handler registered: any trap in the
 * parent would terminate the test run, so live trap behavior is
 * exercised only in child processes (the 14b2/15a1 discipline). The
 * parent DOES register its own standard stream handles in
 * test_std_stream_handles_and_failure; that test is deliberately the
 * last in-process test that touches the table, and no later test calls
 * rt_io_core_reset() (which would close the parent's own OS standard
 * handles).
 *
 * Temp files are created under tests/artifacts/rt_io_stdio_test/ (the
 * gitignored harness temp area, CONVENTIONS.md sec. 1; run from the
 * repository root so the relative paths resolve) and are deleted at
 * the end - no repository source is modified and no C: drive writes
 * occur.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-io-a2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_io/rt_io_stdio_test.c \
 *     bootstrap/runtime/rt_io/rt_io_stdio.c \
 *     bootstrap/runtime/rt_io/rt_io_core.c \
 *     bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-rt-io-a2/rt_io_stdio_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-io-a2)
 */
#include "rt_io_stdio.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Temp workspace (gitignored harness temp area; repository-relative,
 * resolved against the cwd - run the test from the repository root). */
#define TMP_DIR "tests\\artifacts\\rt_io_stdio_test"
#define TMP_PREFIX "tests\\artifacts\\rt_io_stdio_test\\"

/* ---------------------------------------------------------------------------
 * Temp-file helpers
 * ------------------------------------------------------------------------- */

/* Ensure the temp directory exists (idempotent). */
static void ensure_tmp_dir(void)
{
    CreateDirectoryA("tests\\artifacts", NULL);
    CreateDirectoryA(TMP_DIR, NULL);
}

/* Best-effort delete of a temp file and the temp directory. */
static void cleanup_tmp_dir(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;

    h = FindFirstFileA(TMP_PREFIX "*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] != '.') {
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s", TMP_PREFIX,
                         fd.cFileName);
                DeleteFileA(path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(TMP_DIR);
}

/* ---------------------------------------------------------------------------
 * In-process: AIC-R0814 record shape
 * ------------------------------------------------------------------------- */

/* The exact JSONL bytes for each operation the core can deliver. Field
 * order is the diag emitter's canonical order (diag_emit.c); the
 * record is built by diag_trap_record (phase=trap, severity=error,
 * recovery=authoritative, exit_code=70) plus the two related facts. */
static void test_format_invalid_handle_trap(void)
{
    DiagBuf buf;

    /* read on handle 0 */
    diag_buf_init(&buf);
    {
        static const char kExpected[] =
            "{\"schema_version\":\"1\",\"code\":\"AIC-R0814\","
            "\"severity\":\"error\",\"phase\":\"trap\","
            "\"message\":\"invalid/closed file handle for read: handle 0\","
            "\"primary_span\":null,\"recovery\":\"authoritative\","
            "\"related\":{\"operation\":\"read\",\"handle\":0},"
            "\"exit_code\":70}\n";
        CHECK(rt_io_format_invalid_handle_trap(RT_IO_OP_READ, 0, &buf));
        CHECK(diag_buf_ok(&buf));
        CHECK(buf.len == strlen(kExpected));
        CHECK(buf.len > 0 &&
              memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    }
    diag_buf_free(&buf);

    /* write on an out-of-range handle */
    diag_buf_init(&buf);
    {
        static const char kExpected[] =
            "{\"schema_version\":\"1\",\"code\":\"AIC-R0814\","
            "\"severity\":\"error\",\"phase\":\"trap\","
            "\"message\":\"invalid/closed file handle for write: handle 999\","
            "\"primary_span\":null,\"recovery\":\"authoritative\","
            "\"related\":{\"operation\":\"write\",\"handle\":999},"
            "\"exit_code\":70}\n";
        CHECK(rt_io_format_invalid_handle_trap(RT_IO_OP_WRITE, 999, &buf));
        CHECK(diag_buf_ok(&buf));
        CHECK(buf.len == strlen(kExpected));
        CHECK(buf.len > 0 &&
              memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    }
    diag_buf_free(&buf);

    /* close on a closed handle */
    diag_buf_init(&buf);
    {
        static const char kExpected[] =
            "{\"schema_version\":\"1\",\"code\":\"AIC-R0814\","
            "\"severity\":\"error\",\"phase\":\"trap\","
            "\"message\":\"invalid/closed file handle for close: handle 5\","
            "\"primary_span\":null,\"recovery\":\"authoritative\","
            "\"related\":{\"operation\":\"close\",\"handle\":5},"
            "\"exit_code\":70}\n";
        CHECK(rt_io_format_invalid_handle_trap(RT_IO_OP_CLOSE, 5, &buf));
        CHECK(diag_buf_ok(&buf));
        CHECK(buf.len == strlen(kExpected));
        CHECK(buf.len > 0 &&
              memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    }
    diag_buf_free(&buf);

    /* Defensive: unknown op produces no record; NULL out is rejected. */
    diag_buf_init(&buf);
    CHECK(!rt_io_format_invalid_handle_trap((RtIoOp)99, 0, &buf));
    CHECK(buf.len == 0);
    CHECK(!rt_io_format_invalid_handle_trap(RT_IO_OP_READ, 0, NULL));
    diag_buf_free(&buf);
}

/* ---------------------------------------------------------------------------
 * In-process: standard stream handles and the "0 on failure" result
 * -------------------------------------------------------------------------
 * On a pristine table the standard streams register to the
 * deterministic first-free slots 1/2/3 and are cached (repeated calls
 * consume no further slots). When the table is full, a first-time
 * stream registration returns 0 (resource exhaustion, never a trap);
 * once a stream is registered the cached handle is returned
 * regardless. This test fills the table with real file handles first,
 * proves the 0-on-failure path, closes them, then registers the
 * parent's real standard streams and asserts the deterministic values
 * and caching.
 *
 * ORDERING BOUND: this must be the last in-process test that touches
 * the handle table. After it registers the parent's own OS standard
 * handles, no later test may call rt_io_core_reset() (which closes
 * every open OS handle, including the parent's real stdout/stderr).
 */
static void test_std_stream_handles_and_failure(void)
{
    size_t i;
    size_t in_h, out_h, err_h;

    rt_io_core_reset();
    CHECK(rt_io_core_open_count() == 0);

    /* Fill the table (slots 1..256). */
    for (i = 1; i <= RT_IO_TABLE_CAPACITY; i++) {
        char path[MAX_PATH];
        size_t h;
        snprintf(path, sizeof(path), TMP_PREFIX "std_fill_%04zu.bin", i);
        h = rt_io_open((const unsigned char *)path, strlen(path), 1);
        CHECK(h == i);
    }
    CHECK(rt_io_core_open_count() == RT_IO_TABLE_CAPACITY);

    /* First-time stream registration with a full table: 0 on failure
     * (resource exhaustion), never a trap, nothing cached. */
    CHECK(rt_io_stdout() == 0);
    CHECK(rt_io_core_open_count() == RT_IO_TABLE_CAPACITY);

    /* Close everything; the next stream registration succeeds. */
    for (i = 1; i <= RT_IO_TABLE_CAPACITY; i++) {
        rt_io_close(i);
    }
    CHECK(rt_io_core_open_count() == 0);

    /* Standard streams register to the deterministic first-free slots
     * 1/2/3. Requires the OS to provide all three standard handles
     * (the harness runs in a console session; GetStdHandle returns
     * valid console/pipe handles - the same assumption the 15a1
     * child-spawn tests make). */
    out_h = rt_io_stdout();
    in_h = rt_io_stdin();
    err_h = rt_io_stderr();
    CHECK(out_h == 1);
    CHECK(in_h == 2);
    CHECK(err_h == 3);
    CHECK(rt_io_core_open_count() == 3);
    CHECK(rt_io_core_handle_is_valid(out_h));
    CHECK(rt_io_core_handle_is_valid(in_h));
    CHECK(rt_io_core_handle_is_valid(err_h));

    /* Cached: repeated calls return the same handles, no new slots. */
    CHECK(rt_io_stdout() == out_h);
    CHECK(rt_io_stdin() == in_h);
    CHECK(rt_io_stderr() == err_h);
    CHECK(rt_io_core_open_count() == 3);
}

/* ---------------------------------------------------------------------------
 * Child-process plumbing
 * ------------------------------------------------------------------------- */

/* Spawn this executable with `mode` as argv[1] and full control of the
 * child's standard streams:
 *   - stdin_data[0..stdin_len) is written to the child's stdin pipe,
 *     then the write end is closed (the child sees EOF after reading);
 *   - the child's stdout and stderr are captured into out_buf/err_buf
 *     (NUL-terminated) with their lengths in *out_len and *err_len;
 *   - the child's exit code is returned in *exit_code.
 * Returns 1 on success, 0 on failure (e.g. CreateProcess error). */
static int run_child_stdio(const char *mode,
                           const unsigned char *stdin_data, size_t stdin_len,
                           char *out_buf, size_t out_cap, size_t *out_len,
                           char *err_buf, size_t err_cap, size_t *err_len,
                           int *exit_code)
{
    char exe[MAX_PATH + 1];
    char cmdline[MAX_PATH + 64];
    SECURITY_ATTRIBUTES sa;
    HANDLE in_read = NULL;
    HANDLE in_write = NULL;
    HANDLE out_read = NULL;
    HANDLE out_write = NULL;
    HANDLE err_read = NULL;
    HANDLE err_write = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD exit_dword = 0;
    DWORD got = 0;
    size_t total = 0;
    int ok = 0;

    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) {
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&in_read, &in_write, &sa, 0) ||
        !CreatePipe(&out_read, &out_write, &sa, 0) ||
        !CreatePipe(&err_read, &err_write, &sa, 0)) {
        if (in_read != NULL) { CloseHandle(in_read); }
        if (in_write != NULL) { CloseHandle(in_write); }
        if (out_read != NULL) { CloseHandle(out_read); }
        if (out_write != NULL) { CloseHandle(out_write); }
        if (err_read != NULL) { CloseHandle(err_read); }
        if (err_write != NULL) { CloseHandle(err_write); }
        return 0;
    }

    /* The child inherits only its three standard ends; the parent's
     * ends are non-inheritable so the child cannot hold them open. */
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = err_write;

    snprintf(cmdline, sizeof(cmdline), "\"%s\" %s", exe, mode);

    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL,
                       &si, &pi)) {
        /* Close the parent's copies of the child-visible ends so the
         * reads below terminate at EOF once the child exits. */
        CloseHandle(in_read);
        CloseHandle(out_write);
        CloseHandle(err_write);
        in_read = NULL;
        out_write = NULL;
        err_write = NULL;

        /* Feed stdin, then close the write end (child sees EOF). */
        {
            DWORD written = 0;
            size_t off = 0;
            while (off < stdin_len) {
                DWORD chunk = (DWORD)(stdin_len - off);
                if (!WriteFile(in_write, stdin_data + off, chunk, &written,
                               NULL) || written == 0) {
                    break;
                }
                off += (size_t)written;
            }
        }
        CloseHandle(in_write);
        in_write = NULL;

        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_dword);

        /* Read stdout to EOF. */
        total = 0;
        while (total + 1 < out_cap &&
               ReadFile(out_read, out_buf + total,
                        (DWORD)(out_cap - 1 - total), &got, NULL) &&
               got > 0) {
            total += (size_t)got;
        }
        out_buf[total] = '\0';
        *out_len = total;

        /* Read stderr to EOF. */
        total = 0;
        while (total + 1 < err_cap &&
               ReadFile(err_read, err_buf + total,
                        (DWORD)(err_cap - 1 - total), &got, NULL) &&
               got > 0) {
            total += (size_t)got;
        }
        err_buf[total] = '\0';
        *err_len = total;

        *exit_code = (int)exit_dword;

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = 1;
    }

    if (in_read != NULL) { CloseHandle(in_read); }
    if (in_write != NULL) { CloseHandle(in_write); }
    if (out_read != NULL) { CloseHandle(out_read); }
    if (out_write != NULL) { CloseHandle(out_write); }
    if (err_read != NULL) { CloseHandle(err_read); }
    if (err_write != NULL) { CloseHandle(err_write); }
    return ok;
}

/* Assert a trap child: exit code 70, stderr carries a trap-phase
 * record with the given code, authoritative recovery, exit_code 70,
 * the given operation fact, and the given message fragment. */
static void check_trap_child(const char *mode, const char *code,
                             const char *operation,
                             const char *message_fragment)
{
    char out_buf[8192];
    char err_buf[8192];
    size_t out_len = 0;
    size_t err_len = 0;
    int exit_code = 0;
    char code_field[64];
    char op_field[64];

    CHECK(run_child_stdio(mode, NULL, 0, out_buf, sizeof(out_buf),
                          &out_len, err_buf, sizeof(err_buf), &err_len,
                          &exit_code));
    CHECK(exit_code == 70);
    CHECK(err_len > 0);

    snprintf(code_field, sizeof(code_field), "\"code\":\"%s\"", code);
    CHECK(strstr(err_buf, code_field) != NULL);
    CHECK(strstr(err_buf, "\"phase\":\"trap\"") != NULL);
    CHECK(strstr(err_buf, "\"recovery\":\"authoritative\"") != NULL);
    CHECK(strstr(err_buf, "\"exit_code\":70") != NULL);
    snprintf(op_field, sizeof(op_field), "\"operation\":\"%s\"", operation);
    CHECK(strstr(err_buf, op_field) != NULL);
    CHECK(strstr(err_buf, message_fragment) != NULL);
}

/* ---------------------------------------------------------------------------
 * Child modes: AIC-R0814 trap paths
 * ------------------------------------------------------------------------- */

/* Child: register traps (twice, proving idempotence), then read from
 * invalid handle 0 -> AIC-R0814, exit 70. Never returns. */
static int child_invalid_read(void)
{
    unsigned char buf[8];

    rt_io_stdio_init();
    rt_io_stdio_init(); /* idempotent */

    (void)rt_io_read(0, buf, sizeof(buf), sizeof(buf));

    fprintf(stderr, "FAIL: invalid-handle read did not trap\n");
    return 1;
}

/* Child: write to an out-of-range handle -> AIC-R0814, exit 70. */
static int child_invalid_write(void)
{
    static const unsigned char kBuf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    rt_io_stdio_init();

    (void)rt_io_write(RT_IO_TABLE_CAPACITY + 1, kBuf, sizeof(kBuf),
                      sizeof(kBuf));

    fprintf(stderr, "FAIL: invalid-handle write did not trap\n");
    return 1;
}

/* Child: close invalid handle 0 -> AIC-R0814, exit 70. */
static int child_invalid_close(void)
{
    rt_io_stdio_init();

    rt_io_close(0);

    fprintf(stderr, "FAIL: invalid-handle close did not trap\n");
    return 1;
}

/* Child: open a file, close it, then close it again (already closed)
 * -> AIC-R0814, exit 70. */
static int child_closed_close(void)
{
    size_t h;

    ensure_tmp_dir();
    rt_io_stdio_init();

    h = rt_io_open((const unsigned char *)TMP_PREFIX "closed.bin",
                   strlen(TMP_PREFIX "closed.bin"), 1);
    if (h == 0) {
        fprintf(stderr, "FAIL: open returned 0\n");
        return 1;
    }
    rt_io_close(h);

    rt_io_close(h);

    fprintf(stderr, "FAIL: double close did not trap\n");
    return 1;
}

/* Child: WITHOUT registering traps, invalid handles are deterministic
 * no-ops (the 15a1 default, preserved by 15a2): read 0, write 0,
 * close does nothing, exit 0, no record. Proves the traps are opt-in
 * and 15a2 does not change the default behavior. */
static int child_nohandler_invalid(void)
{
    unsigned char buf[8];

    /* No rt_io_stdio_init(). */

    if (rt_io_read(0, buf, sizeof(buf), sizeof(buf)) != 0) {
        fprintf(stderr, "FAIL: read returned nonzero\n");
        return 1;
    }
    if (rt_io_write(0, buf, sizeof(buf), sizeof(buf)) != 0) {
        fprintf(stderr, "FAIL: write returned nonzero\n");
        return 1;
    }
    rt_io_close(0); /* must not crash, must not trap */
    rt_io_close(RT_IO_TABLE_CAPACITY + 1);
    if (rt_io_core_open_count() != 0) {
        fprintf(stderr, "FAIL: open count changed\n");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Child modes: stdio behavior
 * ------------------------------------------------------------------------- */

/* Child: register the standard streams (no trap registration needed;
 * the stream functions work standalone), report the handle values to
 * stderr, then round-trip stdin -> stdout and write a marker to
 * stderr. The parent controls all three streams through pipes and
 * asserts the exact bytes. */
static int child_stdio_echo(void)
{
    size_t in_h, out_h, err_h;
    unsigned char in_buf[8];
    static const char kOutMarker[] = "OUT:";
    static const char kErrMarker[] = "ERR:";
    static const char kGot[] = "GOT:";
    static const unsigned char kPayload[] = "hello";
    char handle_line[64];
    size_t n;

    in_h = rt_io_stdin();
    out_h = rt_io_stdout();
    err_h = rt_io_stderr();

    if (in_h == 0 || out_h == 0 || err_h == 0) {
        fprintf(stderr, "FAIL: a standard stream handle is 0\n");
        return 1;
    }
    if (in_h == out_h || in_h == err_h || out_h == err_h) {
        fprintf(stderr, "FAIL: standard stream handles not distinct\n");
        return 1;
    }

    /* Report the deterministic handle values to stderr. */
    n = (size_t)snprintf(handle_line, sizeof(handle_line),
                         "IN:%llu OUT:%llu ERR:%llu\n",
                         (unsigned long long)in_h,
                         (unsigned long long)out_h,
                         (unsigned long long)err_h);
    if (rt_io_write(err_h, (const unsigned char *)handle_line, n, n) != n) {
        fprintf(stderr, "FAIL: stderr handle-line write failed\n");
        return 1;
    }

    /* Write a marker to stdout. */
    if (rt_io_write(out_h, (const unsigned char *)kOutMarker,
                    sizeof(kOutMarker) - 1, sizeof(kOutMarker) - 1) !=
        sizeof(kOutMarker) - 1) {
        fprintf(stderr, "FAIL: stdout marker write failed\n");
        return 1;
    }

    /* Read the payload from stdin (5 bytes the parent feeds) and echo
     * it back to stdout. */
    if (rt_io_read(in_h, in_buf, sizeof(in_buf), sizeof(kPayload) - 1) !=
        sizeof(kPayload) - 1) {
        fprintf(stderr, "FAIL: stdin read did not return 5\n");
        return 1;
    }
    if (memcmp(in_buf, kPayload, sizeof(kPayload) - 1) != 0) {
        fprintf(stderr, "FAIL: stdin bytes mismatch\n");
        return 1;
    }
    if (rt_io_write(out_h, (const unsigned char *)kGot, sizeof(kGot) - 1,
                    sizeof(kGot) - 1) != sizeof(kGot) - 1) {
        fprintf(stderr, "FAIL: stdout GOT write failed\n");
        return 1;
    }
    if (rt_io_write(out_h, in_buf, sizeof(kPayload) - 1,
                    sizeof(kPayload) - 1) != sizeof(kPayload) - 1) {
        fprintf(stderr, "FAIL: stdout echo write failed\n");
        return 1;
    }

    /* Write a marker to stderr after the handle line. */
    if (rt_io_write(err_h, (const unsigned char *)kErrMarker,
                    sizeof(kErrMarker) - 1, sizeof(kErrMarker) - 1) !=
        sizeof(kErrMarker) - 1) {
        fprintf(stderr, "FAIL: stderr marker write failed\n");
        return 1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Child-mode dispatch and main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "--child-invalid-read") == 0) {
            return child_invalid_read();
        }
        if (strcmp(argv[1], "--child-invalid-write") == 0) {
            return child_invalid_write();
        }
        if (strcmp(argv[1], "--child-invalid-close") == 0) {
            return child_invalid_close();
        }
        if (strcmp(argv[1], "--child-closed-close") == 0) {
            return child_closed_close();
        }
        if (strcmp(argv[1], "--child-nohandler-invalid") == 0) {
            return child_nohandler_invalid();
        }
        if (strcmp(argv[1], "--child-stdio-echo") == 0) {
            return child_stdio_echo();
        }
        fprintf(stderr, "FAIL: unknown child mode '%s'\n", argv[1]);
        return 2;
    }

    ensure_tmp_dir();

    test_format_invalid_handle_trap();
    fprintf(stderr, "after test_format_invalid_handle_trap\n");
    test_std_stream_handles_and_failure();
    fprintf(stderr, "after test_std_stream_handles_and_failure\n");

    /* Trap paths (children only). */
    check_trap_child("--child-invalid-read", "AIC-R0814", "read",
                     "invalid/closed file handle for read");
    fprintf(stderr, "after check child-invalid-read\n");
    check_trap_child("--child-invalid-write", "AIC-R0814", "write",
                     "invalid/closed file handle for write");
    fprintf(stderr, "after check child-invalid-write\n");
    check_trap_child("--child-invalid-close", "AIC-R0814", "close",
                     "invalid/closed file handle for close");
    fprintf(stderr, "after check child-invalid-close\n");
    check_trap_child("--child-closed-close", "AIC-R0814", "close",
                     "invalid/closed file handle for close");
    fprintf(stderr, "after check child-closed-close\n");

    /* Opt-in default preserved (children only). */
    {
        char out_buf[8192];
        char err_buf[8192];
        size_t out_len = 0;
        size_t err_len = 0;
        int exit_code = 0;

        CHECK(run_child_stdio("--child-nohandler-invalid", NULL, 0,
                              out_buf, sizeof(out_buf), &out_len,
                              err_buf, sizeof(err_buf), &err_len,
                              &exit_code));
        CHECK(exit_code == 0);
        CHECK(err_len == 0); /* no record without registration */
    }
    fprintf(stderr, "after check child-nohandler-invalid\n");

    /* Stdio behavior end-to-end (children with pipe-controlled
     * streams). */
    {
        char out_buf[8192];
        char err_buf[8192];
        size_t out_len = 0;
        size_t err_len = 0;
        int exit_code = 0;
        static const unsigned char kFeed[] = "hello";
        static const char kExpectedOut[] = "OUT:GOT:hello";
        static const char kExpectedErrLine[] = "IN:1 OUT:2 ERR:3\n";

        CHECK(run_child_stdio("--child-stdio-echo", kFeed,
                              sizeof(kFeed) - 1,
                              out_buf, sizeof(out_buf), &out_len,
                              err_buf, sizeof(err_buf), &err_len,
                              &exit_code));
        CHECK(exit_code == 0);
        CHECK(out_len == strlen(kExpectedOut));
        CHECK(memcmp(out_buf, kExpectedOut, strlen(kExpectedOut)) == 0);
        CHECK(strstr(err_buf, kExpectedErrLine) != NULL);
        CHECK(strstr(err_buf, "ERR:") != NULL);
    }
    fprintf(stderr, "after check child-stdio-echo\n");

    cleanup_tmp_dir();

    if (g_failures) {
        fprintf(stderr, "rt_io_stdio_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_io_stdio_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
