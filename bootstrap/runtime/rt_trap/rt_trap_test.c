/* bootstrap/runtime/rt_trap/rt_trap_test.c
 *
 * WP-M0-15c1 rt.trap tests: rt.trap.report behaves per spec sec. 15.4
 * and DIAGNOSTIC-CONTRACT sec. 10 - the process writes a JSONL trap
 * record to stderr (code "AIC-U0000", trap_code = the caller-supplied
 * u32 code, phase "trap", severity "error", recovery "authoritative",
 * exit_code 70) and terminates with exit code 70.
 *
 * Two layers of testing:
 *   1. In-process record-shape tests call rt_trap_format() (the
 *      test/diagnostic-only constructor, rt_trap.h) with fixed codes
 *      and messages and assert the exact JSONL record bytes: the basic
 *      record shape, the edge codes 0 and UINT32_MAX (4294967295),
 *      JSON escaping of quotes/backslashes/control characters in the
 *      message, deterministic message truncation at the declared bound
 *      (RT_TRAP_MAX_MESSAGE_BYTES) and at an embedded NUL byte, and
 *      the defensive rejections (NULL out).
 *   2. Trap-path tests spawn this same executable as a child process
 *      with a mode argument (--child-report-7 / --child-report-0 /
 *      --child-report-max / --child-report-escaped /
 *      --child-report-truncated / --child-report-nul / --child-noop).
 *      The child calls rt_trap_report(); a live trap terminates the
 *      child with exit 70 and writes the JSONL record to stderr. The
 *      parent captures the child's stderr through an anonymous pipe
 *      and asserts the exit code and record fields. This is the only
 *      way to observe the authoritative trap behavior: rt_trap_report
 *      never returns to the caller.
 *
 * Child modes:
 *   --child-report-7          rt_trap_report(7, "user trap") -> exit
 *                             70, record with trap_code 7.
 *   --child-report-0          rt_trap_report(0, "zero") -> trap_code 0
 *                             (the minimum u32 code).
 *   --child-report-max        rt_trap_report(4294967295, "max") ->
 *                             trap_code 4294967295 (UINT32_MAX).
 *   --child-report-escaped    message containing a quote, backslash,
 *                             and newline -> JSON-escaped in the
 *                             emitted record, exit 70.
 *   --child-report-truncated  message longer than the declared bound ->
 *                             emitted message truncated at the bound,
 *                             exit 70.
 *   --child-report-nul        message with an embedded NUL byte ->
 *                             emitted message truncated at the NUL,
 *                             exit 70.
 *   --child-noop              no rt_trap_report call -> exit 0, empty
 *                             stderr (no record without a report call).
 *
 * The parent process never calls rt_trap_report: a live trap would
 * terminate the test run, so trap behavior is exercised only in child
 * processes.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-trap' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_trap/rt_trap_test.c \
 *     bootstrap/runtime/rt_trap/rt_trap.c \
 *     bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-rt-trap/rt_trap_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-trap)
 */
#include "rt_trap.h"

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

/* ---------------------------------------------------------------------------
 * In-process record-shape tests
 * ------------------------------------------------------------------------- */

/* Expected record for code=7, message "user trap". Field order and JSON
 * shape are the diag emitter's canonical order (diag_emit.c); the
 * record is built by diag_user_trap_record (code AIC-U0000,
 * phase=trap, severity=error, recovery=authoritative, trap_code=7,
 * exit_code=70) plus the two related facts operation/code. */
static void test_format_basic_record(void)
{
    DiagBuf buf;
    static const unsigned char kMessage[] = "user trap";
    static const char kExpected[] =
        "{\"schema_version\":\"1\",\"code\":\"AIC-U0000\","
        "\"severity\":\"error\",\"phase\":\"trap\","
        "\"message\":\"user trap\","
        "\"primary_span\":null,\"recovery\":\"authoritative\","
        "\"related\":{\"operation\":\"trap.report\",\"code\":7},"
        "\"trap_code\":7,\"exit_code\":70}\n";

    diag_buf_init(&buf);
    CHECK(rt_trap_format(7, kMessage, sizeof(kMessage) - 1, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len == strlen(kExpected));
    CHECK(buf.len > 0 && memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    diag_buf_free(&buf);
}

/* Edge codes: 0 (minimum u32) and UINT32_MAX (maximum) must be emitted
 * exactly in trap_code and in the related code fact. */
static void test_format_edge_codes(void)
{
    DiagBuf buf;
    static const unsigned char kMessage[] = "edge";
    static const char kExpectedZero[] =
        "{\"schema_version\":\"1\",\"code\":\"AIC-U0000\","
        "\"severity\":\"error\",\"phase\":\"trap\","
        "\"message\":\"edge\","
        "\"primary_span\":null,\"recovery\":\"authoritative\","
        "\"related\":{\"operation\":\"trap.report\",\"code\":0},"
        "\"trap_code\":0,\"exit_code\":70}\n";
    static const char kExpectedMax[] =
        "{\"schema_version\":\"1\",\"code\":\"AIC-U0000\","
        "\"severity\":\"error\",\"phase\":\"trap\","
        "\"message\":\"edge\","
        "\"primary_span\":null,\"recovery\":\"authoritative\","
        "\"related\":{\"operation\":\"trap.report\",\"code\":4294967295},"
        "\"trap_code\":4294967295,\"exit_code\":70}\n";

    diag_buf_init(&buf);
    CHECK(rt_trap_format(0, kMessage, sizeof(kMessage) - 1, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len == strlen(kExpectedZero));
    CHECK(buf.len > 0 && memcmp(buf.data, kExpectedZero, strlen(kExpectedZero)) == 0);
    diag_buf_free(&buf);

    diag_buf_init(&buf);
    CHECK(rt_trap_format(4294967295u, kMessage, sizeof(kMessage) - 1, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len == strlen(kExpectedMax));
    CHECK(buf.len > 0 && memcmp(buf.data, kExpectedMax, strlen(kExpectedMax)) == 0);
    diag_buf_free(&buf);
}

/* JSON escaping: quotes, backslashes, and control characters in the
 * caller's message are escaped by the WP-M0-06 emitter (same escaping
 * rules as bootstrap/src/diag/diag_test.c test_escaping); the emitted
 * record remains a single JSONL line with no embedded newlines. */
static void test_format_json_escaping(void)
{
    DiagBuf buf;
    static const unsigned char kMessage[] = "q\"w\\e\x01r\x08t\x0cy\x0du\x0ai\x09o";
    static const char kEscapedFragments[] =
        "q\\\"w\\\\e";   /* quote and backslash */
    static const char kControlFragments[] =
        "\\u0001r\\bt\\fy\\ru\\ni\\to";

    diag_buf_init(&buf);
    CHECK(rt_trap_format(7, kMessage, sizeof(kMessage) - 1, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len > 0 && buf.data[buf.len - 1] == '\n');
    CHECK(strstr(buf.data, kEscapedFragments) != NULL);
    CHECK(strstr(buf.data, kControlFragments) != NULL);
    CHECK(strstr(buf.data, "\"trap_code\":7") != NULL);
    /* exactly one LF: the record terminator, no embedded newlines */
    {
        size_t i, lf = 0;
        for (i = 0; i < buf.len; ++i) {
            if (buf.data[i] == '\n') {
                lf++;
            }
        }
        CHECK(lf == 1);
    }
    diag_buf_free(&buf);
}

/* Message truncation: a message longer than the declared bound is
 * emitted as the first RT_TRAP_MAX_MESSAGE_BYTES - 1 bytes (deterministic
 * resource bound, documented in the header); an embedded NUL byte ends
 * the emitted message (the record message is a C string in the WP-M0-06
 * model). */
static void test_format_message_truncation(void)
{
    static unsigned char big[RT_TRAP_MAX_MESSAGE_BYTES + 64];
    static const unsigned char kWithNul[] = { 'a', 'b', 'c', '\0', 'd', 'e', 'f' };
    DiagBuf buf;
    char expected_field[64 + RT_TRAP_MAX_MESSAGE_BYTES];
    size_t p;

    memset(big, 'x', sizeof(big));

    diag_buf_init(&buf);
    CHECK(rt_trap_format(1, big, sizeof(big), &buf));
    CHECK(diag_buf_ok(&buf));
    /* emitted message field is the bound-1 prefix of 'x' bytes */
    p = (size_t)snprintf(expected_field, sizeof(expected_field),
                         "\"message\":\"");
    memset(expected_field + p, 'x', RT_TRAP_MAX_MESSAGE_BYTES - 1);
    p += RT_TRAP_MAX_MESSAGE_BYTES - 1;
    expected_field[p++] = '\"';
    expected_field[p] = '\0';
    CHECK(strstr(buf.data, expected_field) != NULL);
    /* the full oversized message must not appear */
    CHECK(strstr(buf.data, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") == NULL ||
          strstr(buf.data, expected_field) != NULL);
    CHECK(strstr(buf.data, "\"trap_code\":1") != NULL);
    diag_buf_free(&buf);

    /* embedded NUL: emitted message ends at the first NUL byte */
    diag_buf_init(&buf);
    CHECK(rt_trap_format(2, kWithNul, sizeof(kWithNul), &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(strstr(buf.data, "\"message\":\"abc\",\"primary_span\"") != NULL);
    CHECK(strstr(buf.data, "def") == NULL);
    CHECK(strstr(buf.data, "\"trap_code\":2") != NULL);
    diag_buf_free(&buf);
}

/* Defensive rejections: a NULL out buffer produces no record. */
static void test_format_rejects_null_out(void)
{
    static const unsigned char kMessage[] = "x";

    CHECK(!rt_trap_format(7, kMessage, sizeof(kMessage) - 1, NULL));
}

/* ---------------------------------------------------------------------------
 * Child-process trap-path tests
 * ------------------------------------------------------------------------- */

/* Spawn this executable with `mode` as argv[1], capture the child's
 * stderr into stderr_buf (NUL-terminated) and the child's exit code
 * into *exit_code. Returns 1 on success (process spawned and waited),
 * 0 on failure (e.g. CreateProcess error).
 *
 * The pipe is drained concurrently with waiting: the anonymous-pipe
 * buffer is small (4096 bytes by default), so a child emitting more
 * than the buffer (e.g. the truncated-message trap record, which is
 * RT_TRAP_MAX_MESSAGE_BYTES + JSON overhead) would deadlock a
 * wait-then-read harness - the child blocks writing to the full pipe
 * while the parent blocks in WaitForSingleObject. Reading available
 * bytes on each wait timeout keeps the child's writes flowing. */
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
    /* Only the write end is inherited by the child (as hStdError); the
     * read end stays in the parent so ReadFile below sees EOF when the
     * child exits and all write handles close. */
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
        /* Close the parent's copy of the write end so the pipe reaches
         * EOF once the child exits. */
        CloseHandle(write_pipe);
        write_pipe = NULL;

        /* Drain available bytes on each wait tick until the child
         * exits (see the concurrency note above). */
        for (;;) {
            DWORD avail = 0;
            DWORD wait_result = WaitForSingleObject(pi.hProcess, 50);
            while (total + 1 < stderr_cap &&
                   PeekNamedPipe(read_pipe, NULL, 0, NULL, &avail, NULL) &&
                   avail > 0) {
                if (!ReadFile(read_pipe, stderr_buf + total,
                              (DWORD)(stderr_cap - 1 - total), &got, NULL) ||
                    got == 0) {
                    break;
                }
                total += (size_t)got;
            }
            if (wait_result == WAIT_OBJECT_0) {
                break;
            }
        }
        GetExitCodeProcess(pi.hProcess, &exit_dword);

        /* Drain any bytes still buffered after the child exited. */
        for (;;) {
            DWORD avail = 0;
            if (total + 1 >= stderr_cap ||
                !PeekNamedPipe(read_pipe, NULL, 0, NULL, &avail, NULL) ||
                avail == 0) {
                break;
            }
            if (!ReadFile(read_pipe, stderr_buf + total,
                          (DWORD)(stderr_cap - 1 - total), &got, NULL) ||
                got == 0) {
                break;
            }
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

/* Assert a trap child: exit code 70, stderr carries a trap-phase record
 * with code AIC-U0000, authoritative recovery, exit_code 70, the
 * operation fact, and the given trap_code value. */
static void check_trap_child(const char *mode, const char *trap_code_str)
{
    char stderr_buf[16384];
    size_t stderr_len = 0;
    int exit_code = 0;
    char code_field[64];
    char needle[128];

    CHECK(run_child(mode, stderr_buf, sizeof(stderr_buf), &stderr_len,
                    &exit_code));
    CHECK(exit_code == 70);
    CHECK(stderr_len > 0);

    CHECK(strstr(stderr_buf, "\"code\":\"AIC-U0000\"") != NULL);
    CHECK(strstr(stderr_buf, "\"phase\":\"trap\"") != NULL);
    CHECK(strstr(stderr_buf, "\"recovery\":\"authoritative\"") != NULL);
    CHECK(strstr(stderr_buf, "\"exit_code\":70") != NULL);
    CHECK(strstr(stderr_buf, "\"operation\":\"trap.report\"") != NULL);
    snprintf(code_field, sizeof(code_field), "\"trap_code\":%s", trap_code_str);
    CHECK(strstr(stderr_buf, code_field) != NULL);
    snprintf(needle, sizeof(needle), "\"code\":%s}", trap_code_str);
    CHECK(strstr(stderr_buf, needle) != NULL);
}

static void test_child_report_basic(void)
{
    check_trap_child("--child-report-7", "7");
}

static void test_child_report_zero(void)
{
    check_trap_child("--child-report-0", "0");
}

static void test_child_report_max(void)
{
    check_trap_child("--child-report-max", "4294967295");
}

/* Escaped message: the emitted record must carry the JSON-escaped
 * message (quote, backslash, newline) and still exit 70. */
static void test_child_report_escaped(void)
{
    char stderr_buf[16384];
    size_t stderr_len = 0;
    int exit_code = 0;

    CHECK(run_child("--child-report-escaped", stderr_buf, sizeof(stderr_buf),
                    &stderr_len, &exit_code));
    CHECK(exit_code == 70);
    CHECK(strstr(stderr_buf, "\"code\":\"AIC-U0000\"") != NULL);
    /* message a"b\c\nd -> a\"b\\c\nd */
    CHECK(strstr(stderr_buf, "\"message\":\"a\\\"b\\\\c\\nd\"") != NULL);
    CHECK(strstr(stderr_buf, "\"trap_code\":42") != NULL);
}

/* Truncated message: the child passes a message longer than the bound;
 * the emitted record carries the bound-1 prefix and the process still
 * exits 70. */
static void test_child_report_truncated(void)
{
    char stderr_buf[16384];
    size_t stderr_len = 0;
    int exit_code = 0;
    char expected_field[64 + RT_TRAP_MAX_MESSAGE_BYTES];
    size_t p;

    CHECK(run_child("--child-report-truncated", stderr_buf, sizeof(stderr_buf),
                    &stderr_len, &exit_code));
    CHECK(exit_code == 70);
    CHECK(strstr(stderr_buf, "\"code\":\"AIC-U0000\"") != NULL);
    p = (size_t)snprintf(expected_field, sizeof(expected_field),
                         "\"message\":\"");
    memset(expected_field + p, 'y', RT_TRAP_MAX_MESSAGE_BYTES - 1);
    p += RT_TRAP_MAX_MESSAGE_BYTES - 1;
    expected_field[p++] = '\"';
    expected_field[p] = '\0';
    CHECK(strstr(stderr_buf, expected_field) != NULL);
    CHECK(strstr(stderr_buf, "\"trap_code\":1") != NULL);
}

/* Embedded NUL message: the emitted message ends at the NUL byte. */
static void test_child_report_nul(void)
{
    char stderr_buf[16384];
    size_t stderr_len = 0;
    int exit_code = 0;

    CHECK(run_child("--child-report-nul", stderr_buf, sizeof(stderr_buf),
                    &stderr_len, &exit_code));
    CHECK(exit_code == 70);
    CHECK(strstr(stderr_buf, "\"message\":\"abc\",\"primary_span\"") != NULL);
    CHECK(strstr(stderr_buf, "def") == NULL);
    CHECK(strstr(stderr_buf, "\"trap_code\":2") != NULL);
}

/* No-op child: no rt_trap_report call, exit 0, empty stderr (no record
 * is ever emitted without a report call). */
static void test_child_noop(void)
{
    char stderr_buf[16384];
    size_t stderr_len = 0;
    int exit_code = 0;

    CHECK(run_child("--child-noop", stderr_buf, sizeof(stderr_buf),
                    &stderr_len, &exit_code));
    CHECK(exit_code == 0);
    CHECK(stderr_len == 0);
}

/* ---------------------------------------------------------------------------
 * Child entry points (argv[1] mode dispatch)
 * ------------------------------------------------------------------------- */

static int child_report_7(void)
{
    static const unsigned char kMessage[] = "user trap";
    rt_trap_report(7, kMessage, sizeof(kMessage) - 1);
    /* Unreachable: rt_trap_report never returns (spec sec. 15.4). */
}

static int child_report_0(void)
{
    static const unsigned char kMessage[] = "zero";
    rt_trap_report(0, kMessage, sizeof(kMessage) - 1);
    /* Unreachable: rt_trap_report never returns (spec sec. 15.4). */
}

static int child_report_max(void)
{
    static const unsigned char kMessage[] = "max";
    rt_trap_report(4294967295u, kMessage, sizeof(kMessage) - 1);
    /* Unreachable: rt_trap_report never returns (spec sec. 15.4). */
}

static int child_report_escaped(void)
{
    static const unsigned char kMessage[] = "a\"b\\c\nd";
    rt_trap_report(42, kMessage, sizeof(kMessage) - 1);
    /* Unreachable: rt_trap_report never returns (spec sec. 15.4). */
}

static int child_report_truncated(void)
{
    static unsigned char big[RT_TRAP_MAX_MESSAGE_BYTES + 64];
    memset(big, 'y', sizeof(big));
    rt_trap_report(1, big, sizeof(big));
    /* Unreachable: rt_trap_report never returns (spec sec. 15.4). */
}

static int child_report_nul(void)
{
    static const unsigned char kMessage[] = { 'a', 'b', 'c', '\0', 'd', 'e', 'f' };
    rt_trap_report(2, kMessage, sizeof(kMessage));
    /* Unreachable: rt_trap_report never returns (spec sec. 15.4). */
}

static int child_noop(void)
{
    /* No rt_trap_report call: plain process exit, no record. */
    return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "--child-report-7") == 0) {
            return child_report_7();
        }
        if (strcmp(argv[1], "--child-report-0") == 0) {
            return child_report_0();
        }
        if (strcmp(argv[1], "--child-report-max") == 0) {
            return child_report_max();
        }
        if (strcmp(argv[1], "--child-report-escaped") == 0) {
            return child_report_escaped();
        }
        if (strcmp(argv[1], "--child-report-truncated") == 0) {
            return child_report_truncated();
        }
        if (strcmp(argv[1], "--child-report-nul") == 0) {
            return child_report_nul();
        }
        if (strcmp(argv[1], "--child-noop") == 0) {
            return child_noop();
        }
        fprintf(stderr, "FAIL: unknown child mode '%s'\n", argv[1]);
        return 2;
    }

    /* Parent: record-shape tests (no live trap in the parent), then the
     * child-process trap-path tests. */
    test_format_basic_record();
    fprintf(stderr, "after test_format_basic_record\n");
    test_format_edge_codes();
    fprintf(stderr, "after test_format_edge_codes\n");
    test_format_json_escaping();
    fprintf(stderr, "after test_format_json_escaping\n");
    test_format_message_truncation();
    fprintf(stderr, "after test_format_message_truncation\n");
    test_format_rejects_null_out();
    fprintf(stderr, "after test_format_rejects_null_out\n");
    test_child_report_basic();
    fprintf(stderr, "after test_child_report_basic\n");
    test_child_report_zero();
    fprintf(stderr, "after test_child_report_zero\n");
    test_child_report_max();
    fprintf(stderr, "after test_child_report_max\n");
    test_child_report_escaped();
    fprintf(stderr, "after test_child_report_escaped\n");
    test_child_report_truncated();
    fprintf(stderr, "after test_child_report_truncated\n");
    test_child_report_nul();
    fprintf(stderr, "after test_child_report_nul\n");
    test_child_noop();
    fprintf(stderr, "after test_child_noop\n");

    if (g_failures) {
        fprintf(stderr, "rt_trap_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_trap_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
