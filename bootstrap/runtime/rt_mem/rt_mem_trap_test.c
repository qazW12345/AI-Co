/* bootstrap/runtime/rt_mem/rt_mem_trap_test.c
 *
 * WP-M0-14b2 release-trap tests: duplicate release -> AIC-R0812, invalid
 * release -> AIC-R0813, and every release trap reports the trap record
 * on stderr and terminates the process with exit code 70 (spec sec.
 * 15.5 / DIAGNOSTIC-CONTRACT sec. 10 / 11.8).
 *
 * Two layers of testing:
 *   1. In-process record-shape tests call rt_mem_trap_format() (the
 *      test/diagnostic-only constructor, rt_mem_trap.h) with fixed fake
 *      addresses and assert the exact JSONL record bytes for both trap
 *      codes, plus the defensive rejections (OK status, NULL out).
 *   2. Trap-path tests spawn this same executable as a child process
 *      with a mode argument (--child-double / --child-invalid /
 *      --child-ok / --child-nohandler-double / --child-double-reuse).
 *      The child performs the real release sequence; a live trap
 *      terminates the child with exit 70 and writes the JSONL record to
 *      stderr. The parent captures the child's stderr through an
 *      anonymous pipe and asserts the exit code and record fields. This
 *      is the only way to observe the authoritative trap behavior: the
 *      trap path never returns to the caller.
 *
 * Child modes:
 *   --child-double          register traps (twice, proving idempotence),
 *                           alloc 16, dealloc, dealloc again -> trap
 *                           AIC-R0812, exit 70.
 *   --child-invalid         register traps, dealloc a stack variable
 *                           (not from the allocator) -> AIC-R0813, 70.
 *   --child-ok              register traps, alloc + dealloc once -> exit
 *                           0, no trap record (normal release never
 *                           traps).
 *   --child-nohandler-double  do NOT register traps, alloc + double
 *                           dealloc -> deterministic no-op (14a2
 *                           default), exit 0, no record. Proves the
 *                           traps are opt-in and 14b2 does not change
 *                           the default behavior.
 *   --child-double-reuse    register 14b1 reuse hooks + 14b2 traps,
 *                           alloc 16, dealloc, dealloc again -> trap
 *                           AIC-R0812, exit 70. Proves the 14b1 release
 *                           hook (0xDD poisoning) and the 14b2 trap
 *                           path coexist without interference.
 *
 * The parent process never registers the trap handler and performs no
 * allocator operations: any trap in the parent would terminate the test
 * run, so trap behavior is exercised only in child processes.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-mem-b2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_mem/rt_mem_trap_test.c \
 *     bootstrap/runtime/rt_mem/rt_mem_trap.c \
 *     bootstrap/runtime/rt_mem/rt_mem_api.c \
 *     bootstrap/runtime/rt_mem/rt_mem_alloc.c \
 *     bootstrap/runtime/rt_mem/rt_mem_reuse.c \
 *     bootstrap/src/diag/diag_codes.c \
 *     bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-rt-mem-b2/rt_mem_trap_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-mem-b2)
 */
#include "rt_mem_api.h"

#include "rt_mem_alloc.h"
#include "rt_mem_reuse.h"
#include "rt_mem_trap.h"

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

/* Fixed fake address for the record-shape tests (deterministic bytes).
 * 0x12345678 = 305419896, so the related "address" fact is assertable. */
#define FAKE_ADDR ((void *)(uintptr_t)0x12345678ULL)

/* ---------------------------------------------------------------------------
 * In-process record-shape tests
 * ------------------------------------------------------------------------- */

/* Expected record for RT_MEM_REL_DOUBLE with FAKE_ADDR. Field order and
 * JSON shape are the diag emitter's canonical order (diag_emit.c); the
 * record is built by diag_trap_record (phase=trap, severity=error,
 * recovery=authoritative, exit_code=70) plus the two related facts. */
static void test_format_double_record(void)
{
    DiagBuf buf;
    static const char kExpected[] =
        "{\"schema_version\":\"1\",\"code\":\"AIC-R0812\","
        "\"severity\":\"error\",\"phase\":\"trap\","
        "\"message\":\"double release of allocation at 0x0000000012345678\","
        "\"primary_span\":null,\"recovery\":\"authoritative\","
        "\"related\":{\"operation\":\"dealloc_bytes\",\"address\":305419896,\"status\":1},"
        "\"exit_code\":70}\n";

    diag_buf_init(&buf);
    CHECK(rt_mem_trap_format(RT_MEM_REL_DOUBLE, FAKE_ADDR, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len == strlen(kExpected));
    CHECK(buf.len > 0 && memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    diag_buf_free(&buf);
}

/* Expected record for RT_MEM_REL_INVALID with FAKE_ADDR. */
static void test_format_invalid_record(void)
{
    DiagBuf buf;
    static const char kExpected[] =
        "{\"schema_version\":\"1\",\"code\":\"AIC-R0813\","
        "\"severity\":\"error\",\"phase\":\"trap\","
        "\"message\":\"invalid release: pointer not from allocator at 0x0000000012345678\","
        "\"primary_span\":null,\"recovery\":\"authoritative\","
        "\"related\":{\"operation\":\"dealloc_bytes\",\"address\":305419896,\"status\":2},"
        "\"exit_code\":70}\n";

    diag_buf_init(&buf);
    CHECK(rt_mem_trap_format(RT_MEM_REL_INVALID, FAKE_ADDR, &buf));
    CHECK(diag_buf_ok(&buf));
    CHECK(buf.len == strlen(kExpected));
    CHECK(buf.len > 0 && memcmp(buf.data, kExpected, strlen(kExpected)) == 0);
    diag_buf_free(&buf);
}

/* Defensive rejections: OK status is not a release trap and produces no
 * record; a NULL out buffer is rejected. */
static void test_format_rejects_non_trap_status(void)
{
    DiagBuf buf;

    diag_buf_init(&buf);
    CHECK(!rt_mem_trap_format(RT_MEM_REL_OK, FAKE_ADDR, &buf));
    CHECK(buf.len == 0);
    CHECK(!rt_mem_trap_format(12345, FAKE_ADDR, &buf)); /* unknown status */
    CHECK(buf.len == 0);
    CHECK(!rt_mem_trap_format(RT_MEM_REL_DOUBLE, FAKE_ADDR, NULL));
    diag_buf_free(&buf);
}

/* ---------------------------------------------------------------------------
 * Child-process trap-path tests
 * ------------------------------------------------------------------------- */

/* Spawn this executable with `mode` as argv[1], capture the child's
 * stderr into stderr_buf (NUL-terminated) and the child's exit code
 * into *exit_code. Returns 1 on success (process spawned and waited),
 * 0 on failure (e.g. CreateProcess error). */
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
        /* Close the parent's copy of the write end so the read below
         * terminates at EOF once the child exits. */
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

/* Assert a trap child: exit code 70, stderr carries a trap-phase record
 * with the given code, authoritative recovery, and exit_code 70. */
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
    snprintf(needle, sizeof(needle), "\"operation\":\"dealloc_bytes\"");
    CHECK(strstr(stderr_buf, needle) != NULL);
    CHECK(strstr(stderr_buf, message_fragment) != NULL);
}

/* Assert a non-trap child: exit 0 and no release-trap record anywhere
 * on stderr. */
static void check_no_trap_child(const char *mode)
{
    char stderr_buf[8192];
    size_t stderr_len = 0;
    int exit_code = 0;

    CHECK(run_child(mode, stderr_buf, sizeof(stderr_buf), &stderr_len,
                    &exit_code));
    CHECK(exit_code == 0);
    CHECK(strstr(stderr_buf, "AIC-R0812") == NULL);
    CHECK(strstr(stderr_buf, "AIC-R0813") == NULL);
    CHECK(strstr(stderr_buf, "\"phase\":\"trap\"") == NULL);
}

static void test_child_double_release_trap(void)
{
    check_trap_child("--child-double", "AIC-R0812", "double release");
}

static void test_child_invalid_release_trap(void)
{
    check_trap_child("--child-invalid", "AIC-R0813", "invalid release");
}

static void test_child_ok_release_no_trap(void)
{
    check_no_trap_child("--child-ok");
}

static void test_child_without_registration_no_trap(void)
{
    check_no_trap_child("--child-nohandler-double");
}

static void test_child_double_release_with_reuse_trap(void)
{
    check_trap_child("--child-double-reuse", "AIC-R0812", "double release");
}

/* ---------------------------------------------------------------------------
 * Child entry points (argv[1] mode dispatch)
 * ------------------------------------------------------------------------- */

static int child_double(void)
{
    unsigned char *p;

    rt_mem_trap_init();
    rt_mem_trap_init(); /* idempotent: second registration is a no-op */

    p = rt_mem_alloc_bytes(16);
    if (p == NULL) {
        fprintf(stderr, "FAIL: alloc returned NULL\n");
        return 1;
    }
    rt_mem_dealloc_bytes(p);
    rt_mem_dealloc_bytes(p); /* double release -> AIC-R0812, exit 70 */

    /* Unreachable when the trap fires; a live trap never returns. */
    fprintf(stderr, "FAIL: double release did not trap\n");
    return 1;
}

static int child_invalid(void)
{
    unsigned char stack_var = 0;

    rt_mem_trap_init();

    /* A stack address is not the start of a tracked allocation ->
     * invalid release -> AIC-R0813, exit 70. */
    rt_mem_dealloc_bytes(&stack_var);

    fprintf(stderr, "FAIL: invalid release did not trap\n");
    return 1;
}

static int child_ok(void)
{
    unsigned char *p;

    rt_mem_trap_init();

    p = rt_mem_alloc_bytes(16);
    if (p == NULL) {
        fprintf(stderr, "FAIL: alloc returned NULL\n");
        return 1;
    }
    rt_mem_dealloc_bytes(p); /* live release -> OK, handler not called */
    return 0;
}

static int child_nohandler_double(void)
{
    unsigned char *p;

    /* No rt_mem_trap_init: the 14a2 default applies. A double release
     * is a deterministic no-op (rt_mem_api.h) and never a trap. */
    p = rt_mem_alloc_bytes(16);
    if (p == NULL) {
        fprintf(stderr, "FAIL: alloc returned NULL\n");
        return 1;
    }
    rt_mem_dealloc_bytes(p);
    rt_mem_dealloc_bytes(p);
    return 0;
}

static int child_double_reuse(void)
{
    unsigned char *p;

    /* 14b1 reuse/0xDD hooks + 14b2 traps: the release hook poisons on
     * the first release; the second release of the same pointer is
     * still a double release -> AIC-R0812, exit 70. */
    rt_mem_reuse_init();
    rt_mem_trap_init();

    p = rt_mem_alloc_bytes(16);
    if (p == NULL) {
        fprintf(stderr, "FAIL: alloc returned NULL\n");
        return 1;
    }
    rt_mem_dealloc_bytes(p);
    rt_mem_dealloc_bytes(p);

    fprintf(stderr, "FAIL: double release did not trap\n");
    return 1;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "--child-double") == 0) {
            return child_double();
        }
        if (strcmp(argv[1], "--child-invalid") == 0) {
            return child_invalid();
        }
        if (strcmp(argv[1], "--child-ok") == 0) {
            return child_ok();
        }
        if (strcmp(argv[1], "--child-nohandler-double") == 0) {
            return child_nohandler_double();
        }
        if (strcmp(argv[1], "--child-double-reuse") == 0) {
            return child_double_reuse();
        }
        fprintf(stderr, "FAIL: unknown child mode '%s'\n", argv[1]);
        return 2;
    }

    /* Parent: record-shape tests (no handler registered, no allocator
     * use in the parent), then the child-process trap-path tests. */
    test_format_double_record();
    fprintf(stderr, "after test_format_double_record\n");
    test_format_invalid_record();
    fprintf(stderr, "after test_format_invalid_record\n");
    test_format_rejects_non_trap_status();
    fprintf(stderr, "after test_format_rejects_non_trap_status\n");
    test_child_double_release_trap();
    fprintf(stderr, "after test_child_double_release_trap\n");
    test_child_invalid_release_trap();
    fprintf(stderr, "after test_child_invalid_release_trap\n");
    test_child_ok_release_no_trap();
    fprintf(stderr, "after test_child_ok_release_no_trap\n");
    test_child_without_registration_no_trap();
    fprintf(stderr, "after test_child_without_registration_no_trap\n");
    test_child_double_release_with_reuse_trap();
    fprintf(stderr, "after test_child_double_release_with_reuse_trap\n");

    if (g_failures) {
        fprintf(stderr, "rt_mem_trap_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_mem_trap_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
