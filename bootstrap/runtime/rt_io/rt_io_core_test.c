/* bootstrap/runtime/rt_io/rt_io_core_test.c
 *
 * WP-M0-15a1 rt.io core tests: file handles/open/read/write/close
 * behave per spec sec. 15.2 with deterministic observable behavior,
 * `0` on failure, and the read buffer-length trap AIC-R0807 reported
 * as a JSONL trap record with exit code 70 (DIAGNOSTIC-CONTRACT sec.
 * 10, same shape as the WP-M0-14b2 release traps).
 *
 * The rt_io core is a singleton (fixed handle table). Tests run in one
 * process and are ordered in main() so that state expectations hold;
 * tests that need a pristine table call rt_io_core_reset() first
 * (test-only helper, see rt_io_core.h). Temp files are created under
 * tests/artifacts/rt_io_core_test/ (the gitignored harness temp area,
 * CONVENTIONS.md sec. 1; run from the repository root so the relative
 * paths resolve) and are deleted at the end - no repository source is
 * modified and no C: drive writes occur.
 *
 * "Never a trap" for open/read/write failures: open/read/write return
 * 0 (explicit result values); the harness proves the requirement by
 * the test process exiting 0 with all checks passing. The AIC-R0814
 * invalid-handle trap wiring is owned by WP-M0-15a2 and is NOT raised
 * here: invalid handles are delivered to the registered handler
 * (NULL by default, so at 15a1 they are deterministic no-ops). The
 * AIC-R0807 read-buffer trap IS raised here (spec sec. 15.2 attaches
 * it to read itself) and is exercised only in child processes, like
 * the 14b2 release traps.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-io-a1' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_io/rt_io_core_test.c \
 *     bootstrap/runtime/rt_io/rt_io_core.c \
 *     bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-rt-io-a1/rt_io_core_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-io-a1)
 */
#include "rt_io_core.h"

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
#define TMP_DIR "tests\\artifacts\\rt_io_core_test"
#define TMP_PREFIX "tests\\artifacts\\rt_io_core_test\\"

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

/* Invalid-handle handler record (15a2 integration contract probe). */
static int g_handler_calls = 0;
static int g_handler_op[16];
static size_t g_handler_handle[16];

static void test_invalid_handle_handler(RtIoOp op, size_t handle)
{
    if (g_handler_calls < 16) {
        g_handler_op[g_handler_calls] = (int)op;
        g_handler_handle[g_handler_calls] = handle;
    }
    g_handler_calls++;
}

/* ---------------------------------------------------------------------------
 * test_open_read_write_close
 * ---------------------------------------------------------------------------
 * Round trip: open (write/truncate) -> write -> close -> open (read) ->
 * read back -> compare -> EOF read returns 0 -> close. Handles are
 * nonzero table indices; 0 is never returned by a successful operation.
 */
static void test_open_read_write_close(void)
{
    size_t hw, hr;
    static const unsigned char kPayload[] = "hello rt.io\x00\xff";
    unsigned char buf[64];

    ensure_tmp_dir();

    hw = rt_io_open((const unsigned char *)TMP_PREFIX "roundtrip.bin",
                    strlen(TMP_PREFIX "roundtrip.bin"), 1);
    CHECK(hw != 0);
    CHECK(hw <= rt_io_core_capacity());
    CHECK(rt_io_core_open_count() == 1);
    CHECK(rt_io_core_handle_is_valid(hw));

    CHECK(rt_io_write(hw, kPayload, sizeof(kPayload), sizeof(kPayload)) ==
          sizeof(kPayload));
    CHECK(rt_io_core_handle_is_valid(hw));

    rt_io_close(hw);
    CHECK(!rt_io_core_handle_is_valid(hw));
    CHECK(rt_io_core_open_count() == 0);

    /* Missing file in read mode is a failure -> 0. */
    hr = rt_io_open((const unsigned char *)TMP_PREFIX "missing.bin",
                    strlen(TMP_PREFIX "missing.bin"), 0);
    CHECK(hr == 0);

    hr = rt_io_open((const unsigned char *)TMP_PREFIX "roundtrip.bin",
                    strlen(TMP_PREFIX "roundtrip.bin"), 0);
    CHECK(hr != 0);

    memset(buf, 0, sizeof(buf));
    CHECK(rt_io_read(hr, buf, sizeof(buf), sizeof(kPayload)) ==
          sizeof(kPayload));
    CHECK(memcmp(buf, kPayload, sizeof(kPayload)) == 0);

    /* EOF: the next read returns 0. */
    CHECK(rt_io_read(hr, buf, sizeof(buf), sizeof(kPayload)) == 0);

    rt_io_close(hr);
}

/* ---------------------------------------------------------------------------
 * test_open_modes
 * ---------------------------------------------------------------------------
 * Modes 0..3 per spec sec. 15.2: read (missing -> 0), write/truncate,
 * write/append, read/write (create/truncate). Invalid modes fail -> 0.
 * Invalid UTF-8 paths fail -> 0. write never reads past buf_len.
 */
static void test_open_modes(void)
{
    size_t h;
    unsigned char buf[64];

    /* Mode 1 truncates: write "ab", reopen truncate, write "z". */
    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 1);
    CHECK(h != 0);
    CHECK(rt_io_write(h, (const unsigned char *)"ab", 2, 2) == 2);
    rt_io_close(h);

    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 1);
    CHECK(h != 0);
    CHECK(rt_io_write(h, (const unsigned char *)"z", 1, 1) == 1);
    rt_io_close(h);

    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 0);
    CHECK(h != 0);
    memset(buf, 0, sizeof(buf));
    CHECK(rt_io_read(h, buf, sizeof(buf), 8) == 1);
    CHECK(buf[0] == 'z');
    rt_io_close(h);

    /* Mode 2 appends: "z" then "cd" -> "zcd". */
    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 2);
    CHECK(h != 0);
    CHECK(rt_io_write(h, (const unsigned char *)"cd", 2, 2) == 2);
    rt_io_close(h);

    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 0);
    CHECK(h != 0);
    memset(buf, 0, sizeof(buf));
    CHECK(rt_io_read(h, buf, sizeof(buf), 8) == 3);
    CHECK(memcmp(buf, "zcd", 3) == 0);
    rt_io_close(h);

    /* Mode 3 read/write create/truncate: pre-existing content is
     * truncated, and the same handle can write then read (the read
     * position follows the write). */
    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 3);
    CHECK(h != 0);
    CHECK(rt_io_write(h, (const unsigned char *)"xy", 2, 2) == 2);
    memset(buf, 0, sizeof(buf));
    /* After writing 2 bytes the position is at EOF; a read from the
     * same handle returns 0 (no seek API in the minimal runtime). */
    CHECK(rt_io_read(h, buf, sizeof(buf), 8) == 0);
    rt_io_close(h);

    h = rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                   strlen(TMP_PREFIX "modes.bin"), 0);
    CHECK(h != 0);
    memset(buf, 0, sizeof(buf));
    CHECK(rt_io_read(h, buf, sizeof(buf), 8) == 2);
    CHECK(memcmp(buf, "xy", 2) == 0);
    rt_io_close(h);

    /* Invalid modes fail -> 0. */
    CHECK(rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                     strlen(TMP_PREFIX "modes.bin"), 4) == 0);
    CHECK(rt_io_open((const unsigned char *)TMP_PREFIX "modes.bin",
                     strlen(TMP_PREFIX "modes.bin"), 0xFFFFFFFFu) == 0);

    /* Invalid UTF-8 path (lone continuation byte) fails -> 0. */
    {
        static const unsigned char kBadUtf8[] = { 0x80 };
        CHECK(rt_io_open(kBadUtf8, sizeof(kBadUtf8), 1) == 0);
    }

    /* write clamps to buf_len: request 10 but the slice holds 3. */
    h = rt_io_open((const unsigned char *)TMP_PREFIX "clamp.bin",
                   strlen(TMP_PREFIX "clamp.bin"), 1);
    CHECK(h != 0);
    CHECK(rt_io_write(h, (const unsigned char *)"abc", 3, 10) == 3);
    rt_io_close(h);

    h = rt_io_open((const unsigned char *)TMP_PREFIX "clamp.bin",
                   strlen(TMP_PREFIX "clamp.bin"), 0);
    CHECK(h != 0);
    memset(buf, 0, sizeof(buf));
    CHECK(rt_io_read(h, buf, sizeof(buf), 8) == 3);
    CHECK(memcmp(buf, "abc", 3) == 0);
    rt_io_close(h);
}

/* ---------------------------------------------------------------------------
 * test_handle_model
 * ---------------------------------------------------------------------------
 * Handles are deterministic table indices: first open -> 1, second ->
 * 2, close frees the slot, a later open reuses the first free slot.
 * 0 is never returned by a successful operation. Full-table exhaustion
 * returns 0 (resource bound, never a trap) and does not leak OS
 * handles (open count stays full; closing frees a slot for reuse).
 */
static void test_handle_model(void)
{
    size_t h1, h2, h3;
    size_t i;

    rt_io_core_reset();
    CHECK(rt_io_core_open_count() == 0);
    CHECK(rt_io_core_capacity() == RT_IO_TABLE_CAPACITY);

    h1 = rt_io_open((const unsigned char *)TMP_PREFIX "hm1.bin",
                    strlen(TMP_PREFIX "hm1.bin"), 1);
    h2 = rt_io_open((const unsigned char *)TMP_PREFIX "hm2.bin",
                    strlen(TMP_PREFIX "hm2.bin"), 1);
    h3 = rt_io_open((const unsigned char *)TMP_PREFIX "hm3.bin",
                    strlen(TMP_PREFIX "hm3.bin"), 1);
    CHECK(h1 == 1);
    CHECK(h2 == 2);
    CHECK(h3 == 3);
    CHECK(rt_io_core_open_count() == 3);

    rt_io_close(h2);
    CHECK(!rt_io_core_handle_is_valid(h2));
    CHECK(rt_io_core_open_count() == 2);

    /* The freed slot 2 is reused (first free slot). */
    h2 = rt_io_open((const unsigned char *)TMP_PREFIX "hm2.bin",
                    strlen(TMP_PREFIX "hm2.bin"), 1);
    CHECK(h2 == 2);
    CHECK(rt_io_core_open_count() == 3);

    rt_io_close(h1);
    rt_io_close(h2);
    rt_io_close(h3);
    CHECK(rt_io_core_open_count() == 0);

    /* Exhaustion: fill the table; the next open returns 0 and open
     * count stays at capacity (no OS-handle leak observable through
     * the table). */
    for (i = 1; i <= RT_IO_TABLE_CAPACITY; i++) {
        char path[MAX_PATH];
        size_t h;
        snprintf(path, sizeof(path), TMP_PREFIX "cap_%04zu.bin", i);
        h = rt_io_open((const unsigned char *)path, strlen(path), 1);
        CHECK(h == i);
    }
    CHECK(rt_io_core_open_count() == RT_IO_TABLE_CAPACITY);
    CHECK(rt_io_open((const unsigned char *)TMP_PREFIX "cap_overflow.bin",
                     strlen(TMP_PREFIX "cap_overflow.bin"), 1) == 0);
    CHECK(rt_io_core_open_count() == RT_IO_TABLE_CAPACITY);

    /* Closing one slot makes the first free slot available again. */
    rt_io_close(1);
    CHECK(rt_io_core_open_count() == RT_IO_TABLE_CAPACITY - 1);
    h1 = rt_io_open((const unsigned char *)TMP_PREFIX "cap_overflow.bin",
                    strlen(TMP_PREFIX "cap_overflow.bin"), 1);
    CHECK(h1 == 1);

    /* Close all and verify the table is empty again. */
    for (i = 1; i <= RT_IO_TABLE_CAPACITY; i++) {
        rt_io_close(i);
    }
    CHECK(rt_io_core_open_count() == 0);

    /* register_handle: the 15a2 stdio integration point. Register a
     * real OS handle, read/write through it, close it. */
    {
        HANDLE os;
        size_t h;
        unsigned char buf[16];
        SECURITY_ATTRIBUTES sa;

        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        os = CreateFileA(TMP_PREFIX "registered.bin", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(os != INVALID_HANDLE_VALUE);
        h = rt_io_core_register_handle((void *)os);
        CHECK(h == 1);
        CHECK(rt_io_core_open_count() == 1);
        CHECK(rt_io_write(h, (const unsigned char *)"r", 1, 1) == 1);
        /* Position is after the write; close and re-read through rt_io. */
        rt_io_close(h);
        CHECK(rt_io_core_open_count() == 0);

        h = rt_io_open((const unsigned char *)TMP_PREFIX "registered.bin",
                       strlen(TMP_PREFIX "registered.bin"), 0);
        CHECK(h == 1);
        memset(buf, 0, sizeof(buf));
        CHECK(rt_io_read(h, buf, sizeof(buf), 4) == 1);
        CHECK(buf[0] == 'r');
        rt_io_close(h);
    }

    /* register_handle rejects NULL/INVALID_HANDLE_VALUE. */
    CHECK(rt_io_core_register_handle(NULL) == 0);
    CHECK(rt_io_core_register_handle(INVALID_HANDLE_VALUE) == 0);
}

/* ---------------------------------------------------------------------------
 * test_invalid_handle_failure_paths
 * ---------------------------------------------------------------------------
 * At 15a1 (no handler registered) an invalid handle is a deterministic
 * no-op: read returns 0, write returns 0, close does nothing - never a
 * trap, never a state change. With a handler registered, every invalid
 * handle delivers (op, handle) to the handler; unregistering restores
 * the default. This is the AIC-R0814 delivery contract for WP-M0-15a2.
 */
static void test_invalid_handle_failure_paths(void)
{
    unsigned char buf[8];

    rt_io_core_reset();
    CHECK(rt_io_core_handle_is_valid(0) == 0);
    CHECK(rt_io_core_handle_is_valid(RT_IO_TABLE_CAPACITY + 1) == 0);

    /* Default (no handler): no-op results, no state change. */
    CHECK(rt_io_read(0, buf, sizeof(buf), 4) == 0);
    CHECK(rt_io_write(0, buf, sizeof(buf), 4) == 0);
    rt_io_close(0); /* must not crash */
    rt_io_close(RT_IO_TABLE_CAPACITY + 1);
    CHECK(rt_io_core_open_count() == 0);

    /* A closed handle is invalid for later operations. */
    {
        size_t h = rt_io_open((const unsigned char *)TMP_PREFIX "closed.bin",
                              strlen(TMP_PREFIX "closed.bin"), 1);
        CHECK(h == 1);
        rt_io_close(h);
        CHECK(rt_io_read(h, buf, sizeof(buf), 4) == 0);
        CHECK(rt_io_write(h, buf, sizeof(buf), 4) == 0);
        rt_io_close(h); /* double close: no-op */
        CHECK(rt_io_core_open_count() == 0);
    }

    /* Handler registered: read/write/close deliver (op, handle). */
    g_handler_calls = 0;
    rt_io_core_set_invalid_handle_handler(test_invalid_handle_handler);
    CHECK(rt_io_read(0, buf, sizeof(buf), 4) == 0);
    CHECK(rt_io_write(0, buf, sizeof(buf), 4) == 0);
    rt_io_close(0);
    rt_io_close(999);
    CHECK(g_handler_calls == 4);
    CHECK(g_handler_op[0] == RT_IO_OP_READ && g_handler_handle[0] == 0);
    CHECK(g_handler_op[1] == RT_IO_OP_WRITE && g_handler_handle[1] == 0);
    CHECK(g_handler_op[2] == RT_IO_OP_CLOSE && g_handler_handle[2] == 0);
    CHECK(g_handler_op[3] == RT_IO_OP_CLOSE && g_handler_handle[3] == 999);
    CHECK(rt_io_core_open_count() == 0);

    /* Unregister restores the deterministic default. */
    rt_io_core_set_invalid_handle_handler(NULL);
    g_handler_calls = 0;
    CHECK(rt_io_read(0, buf, sizeof(buf), 4) == 0);
    rt_io_close(0);
    CHECK(g_handler_calls == 0);
}

/* ---------------------------------------------------------------------------
 * test_determinism
 * ---------------------------------------------------------------------------
 * Identical open/write/read/close sequences, replayed from a pristine
 * table, yield identical handle values and identical bytes. Handle
 * values are the deterministic table contract (offsets, like the
 * allocator's deterministic offsets); file contents are environmental
 * inputs (spec sec. 15.6) but are recreated identically by the
 * sequence, so the observable outputs match byte-for-byte.
 */
static void run_determinism_sequence(size_t *out_handles, size_t *out_count)
{
    static const unsigned char kPayload[] = "determinism-42";
    unsigned char buf[32];
    size_t hw, hr;
    size_t n = 0;

    hw = rt_io_open((const unsigned char *)TMP_PREFIX "det.bin",
                    strlen(TMP_PREFIX "det.bin"), 1);
    CHECK(hw != 0);
    CHECK(rt_io_write(hw, kPayload, sizeof(kPayload), sizeof(kPayload)) ==
          sizeof(kPayload));
    rt_io_close(hw);

    hr = rt_io_open((const unsigned char *)TMP_PREFIX "det.bin",
                    strlen(TMP_PREFIX "det.bin"), 0);
    CHECK(hr != 0);
    memset(buf, 0, sizeof(buf));
    CHECK(rt_io_read(hr, buf, sizeof(buf), sizeof(kPayload)) ==
          sizeof(kPayload));
    CHECK(memcmp(buf, kPayload, sizeof(kPayload)) == 0);
    rt_io_close(hr);

    out_handles[n++] = hw;
    out_handles[n++] = hr;
    *out_count = n;
}

static void test_determinism(void)
{
    size_t h1[4], h2[4];
    size_t n1 = 0, n2 = 0, i;

    rt_io_core_reset();
    run_determinism_sequence(h1, &n1);

    rt_io_core_reset();
    run_determinism_sequence(h2, &n2);

    CHECK(n1 == n2);
    for (i = 0; i < n1 && i < n2; i++) {
        CHECK(h1[i] == h2[i]);
    }
}

/* ---------------------------------------------------------------------------
 * AIC-R0807 read-buffer trap
 * ------------------------------------------------------------------------- */

/* In-process record-shape test: the exact JSONL bytes for a fixed
 * read-buffer violation, via the test/diagnostic-only formatter. */
static void test_format_read_buf_trap(void)
{
    DiagBuf buf;
    static const char kExpected[] =
        "{\"schema_version\":\"1\",\"code\":\"AIC-R0807\","
        "\"severity\":\"error\",\"phase\":\"trap\","
        "\"message\":\"read buffer too small: count 9 > buf length 3\","
        "\"primary_span\":null,\"recovery\":\"authoritative\","
        "\"related\":{\"operation\":\"read\",\"handle\":7,\"buf_len\":3,"
        "\"count\":9},"
        "\"exit_code\":70}\n";

    diag_buf_init(&buf);
    CHECK(rt_io_format_read_buf_trap(7, 3, 9, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len == strlen(kExpected));
    CHECK(buf.len > 0 && memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    diag_buf_free(&buf);

    /* Defensive: NULL out is rejected. */
    CHECK(!rt_io_format_read_buf_trap(7, 3, 9, NULL));
}

/* Spawn this executable with `mode` as argv[1], capture the child's
 * stderr into stderr_buf (NUL-terminated) and the child's exit code
 * into *exit_code. Returns 1 on success, 0 on failure. */
static int run_child(const char *mode, char *stderr_buf, size_t stderr_cap,
                     size_t *stderr_len, int *exit_code)
{
    char exe[MAX_PATH + 1];
    char cmdline[MAX_PATH + 64];
    SECURITY_ATTRIBUTES sa;
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
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
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return 0;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(write_pipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = write_pipe;

    snprintf(cmdline, sizeof(cmdline), "\"%s\" %s", exe, mode);

    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(write_pipe);
        write_pipe = NULL;

        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_dword);

        while (total + 1 < stderr_cap &&
               ReadFile(read_pipe, stderr_buf + total,
                        (DWORD)(stderr_cap - 1 - total), &got, NULL) &&
               got > 0) {
            total += (size_t)got;
        }
        stderr_buf[total] = '\0';
        *stderr_len = total;
        *exit_code = (int)exit_dword;

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = 1;
    }

    if (write_pipe != NULL) {
        CloseHandle(write_pipe);
    }
    CloseHandle(read_pipe);
    return ok;
}

/* Assert a trap child: exit code 70, stderr carries a trap-phase
 * record with the given code, authoritative recovery, and exit_code 70. */
static void check_trap_child(const char *mode, const char *code,
                             const char *message_fragment)
{
    char stderr_buf[8192];
    size_t stderr_len = 0;
    int exit_code = 0;
    char code_field[64];
    char needle[128];

    CHECK(run_child(mode, stderr_buf, sizeof(stderr_buf), &stderr_len,
                    &exit_code));
    CHECK(exit_code == 70);
    CHECK(stderr_len > 0);

    snprintf(code_field, sizeof(code_field), "\"code\":\"%s\"", code);
    CHECK(strstr(stderr_buf, code_field) != NULL);
    CHECK(strstr(stderr_buf, "\"phase\":\"trap\"") != NULL);
    CHECK(strstr(stderr_buf, "\"recovery\":\"authoritative\"") != NULL);
    CHECK(strstr(stderr_buf, "\"exit_code\":70") != NULL);
    snprintf(needle, sizeof(needle), "\"operation\":\"read\"");
    CHECK(strstr(stderr_buf, needle) != NULL);
    CHECK(strstr(stderr_buf, message_fragment) != NULL);
}

/* Child: open a file, then read with count > buf_len -> AIC-R0807,
 * exit 70. The parent asserts the record and exit code. */
static int child_buf_trap(void)
{
    size_t h;
    unsigned char small_buf[2];

    ensure_tmp_dir();

    h = rt_io_open((const unsigned char *)TMP_PREFIX "trap.bin",
                   strlen(TMP_PREFIX "trap.bin"), 1);
    if (h == 0) {
        fprintf(stderr, "FAIL: open returned 0\n");
        return 1;
    }
    if (rt_io_write(h, (const unsigned char *)"hello", 5, 5) != 5) {
        fprintf(stderr, "FAIL: write failed\n");
        return 1;
    }
    rt_io_close(h);

    h = rt_io_open((const unsigned char *)TMP_PREFIX "trap.bin",
                   strlen(TMP_PREFIX "trap.bin"), 0);
    if (h == 0) {
        fprintf(stderr, "FAIL: reopen returned 0\n");
        return 1;
    }

    /* buf_len=2 < count=8 -> AIC-R0807, exit 70. Never returns. */
    (void)rt_io_read(h, small_buf, sizeof(small_buf), 8);

    fprintf(stderr, "FAIL: read buffer trap did not fire\n");
    return 1;
}

static void test_child_buf_trap(void)
{
    check_trap_child("--child-buf-trap", "AIC-R0807",
                     "read buffer too small");
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "--child-buf-trap") == 0) {
            return child_buf_trap();
        }
        fprintf(stderr, "FAIL: unknown child mode '%s'\n", argv[1]);
        return 2;
    }

    test_open_read_write_close();
    fprintf(stderr, "after test_open_read_write_close\n");
    test_open_modes();
    fprintf(stderr, "after test_open_modes\n");
    test_handle_model();
    fprintf(stderr, "after test_handle_model\n");
    test_invalid_handle_failure_paths();
    fprintf(stderr, "after test_invalid_handle_failure_paths\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");
    test_format_read_buf_trap();
    fprintf(stderr, "after test_format_read_buf_trap\n");
    test_child_buf_trap();
    fprintf(stderr, "after test_child_buf_trap\n");

    cleanup_tmp_dir();

    if (g_failures) {
        fprintf(stderr, "rt_io_core_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_io_core_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
