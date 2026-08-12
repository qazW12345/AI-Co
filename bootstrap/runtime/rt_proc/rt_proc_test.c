/* bootstrap/runtime/rt_proc/rt_proc_test.c
 *
 * WP-M0-15b rt.proc tests: process arguments and exit behave per spec
 * sec. 15.3 - rt.proc.args() converts the Windows UTF-16 command line
 * to UTF-8 deterministically and losslessly for valid Unicode with
 * deterministic U+FFFD replacement for invalid surrogates, args()[0]
 * is the program path (the executable path as provided); rt.proc.exit
 * terminates with the given exit code and writes no diagnostic record.
 *
 * Three layers of testing:
 *   1. Parser/converter tests drive rt_proc_parse_for_test() (the
 *      test/diagnostic-only hook that shares the production parser and
 *      converter, rt_proc.h) with crafted UTF-16 command lines and
 *      assert the exact argument slices: splitting rules (whitespace,
 *      quoting, backslash-escape parity, doubled quotes, unterminated
 *      quotes), UTF-16 -> UTF-8 encoding of ASCII/BMP/supplementary
 *      code points, and U+FFFD replacement for lone high, lone low,
 *      high+non-low, and high+high surrogate sequences, with position
 *      stability (a replaced unit never shifts subsequent units).
 *      Resource-bound behavior (slices_cap, data_cap, command-line
 *      unit bound) is also exercised with undersized buffers: parsing
 *      stops deterministically, never writes past a bound, and never
 *      emits a truncated trailing argument.
 *   2. Live args()/cache tests call rt_proc_args() in this process:
 *      args()[0] is the program path as provided (compared against the
 *      first token of GetCommandLineW and, when present, the C
 *      runtime's argv[0]), the result is cached (repeated calls return
 *      the same array), and the count is at least 1.
 *   3. Child-process tests spawn this same executable with crafted
 *      command lines via CreateProcessW: --child-args prints the
 *      parsed arguments to stdout (the parent asserts the exact UTF-8
 *      bytes, including a lone-surrogate argument -> U+FFFD), and
 *      --child-exit-<code> calls rt_proc_exit(code) (the parent
 *      asserts the exact exit code and an empty stderr: exit writes no
 *      record, AC2).
 *
 * No temp files are needed: rt.proc does no I/O (spec sec. 15.3; file
 * I/O is 15a). The test writes nothing to the repository and nothing
 * to C:.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-rt-proc' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/runtime/rt_proc/rt_proc_test.c \
 *     bootstrap/runtime/rt_proc/rt_proc.c
 *   ./bootstrap/stage0/msvc-rt-proc/rt_proc_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-rt-proc)
 *
 * rt_proc.c has no diag dependency (it emits no records), so the diag
 * sources are not needed for this test binary.
 */
#include "rt_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

/* Test buffers (declared static so they stay off the stack). */
static RtSlice s_slices[RT_PROC_MAX_ARGS];
static unsigned char s_data[RT_PROC_DATA_CAPACITY];

/* ---------------------------------------------------------------------------
 * Parser/converter helpers
 * ------------------------------------------------------------------------- */

/* Parse `cmdline` into the static test buffers; returns the argument
 * count (mirrors rt_proc_parse_for_test with full-size buffers). */
static size_t parse_cmd(const wchar_t *cmdline)
{
    return rt_proc_parse_for_test(cmdline, s_slices, RT_PROC_MAX_ARGS,
                                  s_data, RT_PROC_DATA_CAPACITY);
}

/* CHECK that slices[i] equals the expected byte string. */
static void check_slice(size_t i, const unsigned char *expect, size_t expect_len)
{
    if (i >= RT_PROC_MAX_ARGS) {
        CHECK(0 && "arg index out of range");
        return;
    }
    CHECK(s_slices[i].len == expect_len);
    if (s_slices[i].len == expect_len && expect_len > 0) {
        CHECK(memcmp(s_slices[i].data, expect, expect_len) == 0);
    }
}

/* CHECK that slices[i] equals an ASCII/UTF-8 literal (len = strlen). */
static void check_slice_str(size_t i, const char *expect)
{
    check_slice(i, (const unsigned char *)expect, strlen(expect));
}

/* ---------------------------------------------------------------------------
 * 1. Parser/converter tests (rt_proc_parse_for_test)
 * ------------------------------------------------------------------------- */

/* Basic splitting: program path token first, then arguments, on
 * whitespace; consecutive whitespace produces no empty argument. */
static void test_parse_basic(void)
{
    CHECK(parse_cmd(L"prog.exe alpha beta gamma") == 4);
    check_slice_str(0, "prog.exe");
    check_slice_str(1, "alpha");
    check_slice_str(2, "beta");
    check_slice_str(3, "gamma");

    /* Tabs delimit too; leading/trailing and repeated whitespace are
     * skipped without empty arguments. */
    CHECK(parse_cmd(L"  prog\talpha   beta  ") == 3);
    check_slice_str(0, "prog");
    check_slice_str(1, "alpha");
    check_slice_str(2, "beta");

    /* Empty command line and whitespace-only command line: no args. */
    CHECK(parse_cmd(L"") == 0);
    CHECK(parse_cmd(L"   \t  ") == 0);
    CHECK(parse_cmd(NULL) == 0);
}

/* Quoting: double quotes group whitespace into one argument; quotes are
 * not part of the argument; a doubled quote inside a quoted region is a
 * literal quote; an unterminated quoted region runs to the end. */
static void test_parse_quoting(void)
{
    CHECK(parse_cmd(L"prog \"hello world\"") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "hello world");

    /* Empty quoted argument. */
    CHECK(parse_cmd(L"prog \"\"") == 2);
    check_slice_str(0, "prog");
    check_slice(1, (const unsigned char *)"", 0);

    /* Doubled quote inside a quoted region -> one literal quote. */
    CHECK(parse_cmd(L"prog \"a\"\"b\"") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "a\"b");

    /* Quotes can open mid-argument; a closing quote can sit mid-arg. */
    CHECK(parse_cmd(L"prog ab\"cd ef\"gh") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "abcd efgh");

    /* Unterminated quote runs to the end of the command line. */
    CHECK(parse_cmd(L"prog \"unterminated") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "unterminated");

    /* A quote immediately after whitespace starts an argument; a
     * doubled quote pair with nothing around it is an empty argument,
     * not a separator. */
    CHECK(parse_cmd(L"a \"\" b") == 3);
    check_slice_str(0, "a");
    check_slice(1, (const unsigned char *)"", 0);
    check_slice_str(2, "b");
}

/* Backslash rules (documented Windows parsing parity): n literal
 * backslashes when not before a quote; 2n backslashes + quote -> n
 * backslashes and the quote toggles the region; 2n+1 backslashes +
 * quote -> n backslashes and a literal quote. */
static void test_parse_backslashes(void)
{
    /* Backslashes not before a quote are literal. */
    CHECK(parse_cmd(L"prog a\\b\\c") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "a\\b\\c");

    /* Even run before a quote: 2n -> n literal backslashes, quote is a
     * region toggle. */
    CHECK(parse_cmd(L"prog a\\\\\"b c\"") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "a\\b c");

    /* Odd run before a quote: 2n+1 -> n literal backslashes plus a
     * literal quote. */
    CHECK(parse_cmd(L"prog a\\\\\\\"b") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "a\\\"b");

    /* A backslash run before a doubled quote inside quotes: the parity
     * rule wins over the doubled-quote rule. `"a\\""b"`: the first
     * quote opens, the 2-backslash run + quote emits one backslash and
     * toggles the region closed, the next quote opens a new region,
     * `b` is emitted, the last quote closes -> `a\b`. */
    CHECK(parse_cmd(L"prog \"a\\\\\"\"b\"") == 2);
    check_slice_str(0, "prog");
    check_slice_str(1, "a\\b");
}

/* UTF-16 -> UTF-8 conversion: ASCII (1 byte), BMP (2 and 3 bytes), and
 * supplementary code points via valid surrogate pairs (4 bytes). */
static void test_parse_utf16_valid(void)
{
    /* ASCII + BMP 2-byte (U+00E9 e-acute) + BMP 3-byte (U+4E2D) +
     * supplementary (U+1F600 via D83D DE00). The space between the
     * two arguments is a delimiter, so arg[1] is only the Unicode
     * bytes. */
    static const wchar_t kCmd[] = L"prog \u00E9\u4E2D\U0001F600";
    static const unsigned char kExpected[] = {
        /* U+00E9 -> C3 A9 */
        0xC3, 0xA9,
        /* U+4E2D -> E4 B8 AD */
        0xE4, 0xB8, 0xAD,
        /* U+1F600 -> F0 9F 98 80 */
        0xF0, 0x9F, 0x98, 0x80
    };

    CHECK(parse_cmd(kCmd) == 2);
    check_slice_str(0, "prog");
    check_slice(1, kExpected, sizeof(kExpected));

    /* Boundary values: U+007F (1 byte max), U+0080 (2 bytes min),
     * U+07FF (2 bytes max), U+0800 (3 bytes min), U+FFFF (3 bytes
     * max), U+10000 (4 bytes min), U+10FFFF (4 bytes max). The two
     * C1/control boundary points cannot be written as universal
     * character names (C17 6.4.3), so the vector is built explicitly. */
    {
        static const wchar_t kB[] = { 0x007F, 0x0080, 0x07FF, 0x0800,
                                      0xFFFF, 0xD800, 0xDC00,
                                      0xDBFF, 0xDFFF, 0 };
        static const unsigned char kE[] = {
            0x7F,
            0xC2, 0x80,
            0xDF, 0xBF,
            0xE0, 0xA0, 0x80,
            0xEF, 0xBF, 0xBF,
            0xF0, 0x90, 0x80, 0x80,
            0xF4, 0x8F, 0xBF, 0xBF
        };
        CHECK(parse_cmd(kB) == 1);
        check_slice(0, kE, sizeof(kE));
    }
}

/* Invalid surrogates -> deterministic U+FFFD (EF BF BD), consuming one
 * code unit each (position-stable: a bad pair cannot shift subsequent
 * units). Output must remain valid UTF-8. The lone-surrogate test
 * vectors are built as explicit wchar_t arrays (a lone surrogate cannot
 * be written as a universal character name in a string literal). */
static void test_parse_invalid_surrogates(void)
{
    static const unsigned char kFffd[3] = { 0xEF, 0xBF, 0xBD };
    static const wchar_t kLoneHigh[] = { L'p', L'r', L'o', L'g', L' ',
                                         0xD800, 0 };
    static const wchar_t kLoneLow[] = { L'p', L'r', L'o', L'g', L' ',
                                        0xDC00, 0 };
    static const wchar_t kHighNonLow[] = { L'p', L'r', L'o', L'g', L' ',
                                           0xD800, L'x', 0 };
    static const wchar_t kHighHigh[] = { L'p', L'r', L'o', L'g', L' ',
                                         0xD800, 0xD800, 0 };
    static const wchar_t kLowHigh[] = { L'p', L'r', L'o', L'g', L' ',
                                        0xDC00, 0xDBFF, 0 };
    static const wchar_t kValidPair[] = { L'p', L'r', L'o', L'g', L' ',
                                          0xD83D, 0xDE00, 0 };

    /* Lone high surrogate. */
    CHECK(parse_cmd(kLoneHigh) == 2);
    check_slice(1, kFffd, 3);

    /* Lone low surrogate. */
    CHECK(parse_cmd(kLoneLow) == 2);
    check_slice(1, kFffd, 3);

    /* High surrogate followed by a non-low unit: high is replaced,
     * following unit is encoded normally. */
    {
        static const unsigned char kExpected[] = { 0xEF, 0xBF, 0xBD, 'x' };
        CHECK(parse_cmd(kHighNonLow) == 2);
        check_slice(1, kExpected, sizeof(kExpected));
    }

    /* High surrogate followed by another high surrogate: both replaced
     * independently (position-stable). */
    {
        static const unsigned char kExpected[] = {
            0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD
        };
        CHECK(parse_cmd(kHighHigh) == 2);
        check_slice(1, kExpected, sizeof(kExpected));
    }

    /* Low surrogate followed by a high surrogate: each replaced. */
    {
        static const unsigned char kExpected[] = {
            0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD
        };
        CHECK(parse_cmd(kLowHigh) == 2);
        check_slice(1, kExpected, sizeof(kExpected));
    }

    /* Valid pair is decoded, not replaced (lossless for valid
     * Unicode): D83D DE00 -> F0 9F 98 80. */
    {
        static const unsigned char kExpected[] = { 0xF0, 0x9F, 0x98, 0x80 };
        CHECK(parse_cmd(kValidPair) == 2);
        check_slice(1, kExpected, sizeof(kExpected));
    }

    /* Replacement is deterministic: the same invalid input parses to
     * the same slice lengths and data bytes every time. */
    {
        static const wchar_t kCmd[] = { L'p', L'r', L'o', L'g', L' ',
                                        0xD800, 0xDC00, L' ',
                                        0xDE00, 0 };
        RtSlice a1[16];
        unsigned char d1[64];
        size_t n1 = parse_cmd(kCmd);
        size_t total1 = 0;
        size_t i;
        size_t n2;
        int same;

        memcpy(a1, s_slices, n1 * sizeof(RtSlice));
        for (i = 0; i < n1; i++) {
            total1 += a1[i].len;
        }
        memcpy(d1, s_data, total1 < sizeof(d1) ? total1 : sizeof(d1));

        n2 = rt_proc_parse_for_test(kCmd, s_slices, RT_PROC_MAX_ARGS,
                                    s_data, RT_PROC_DATA_CAPACITY);
        CHECK(n1 == n2);
        same = 1;
        for (i = 0; i < n1; i++) {
            if (a1[i].len != s_slices[i].len) {
                same = 0;
            }
        }
        CHECK(same);
        CHECK(memcmp(d1, s_data, total1 < sizeof(d1) ? total1 : sizeof(d1))
              == 0);
    }
}

/* Resource bounds: parsing stops deterministically at the first bound
 * crossed (slices_cap, data_cap, or the command-line unit bound), never
 * writes past a bound, and never emits a truncated trailing argument. */
static void test_parse_bounds(void)
{
    RtSlice small_slices[2];
    unsigned char small_data[8];
    size_t n;

    /* slices_cap: only the first two arguments fit. */
    memset(small_data, 0xAA, sizeof(small_data));
    n = rt_proc_parse_for_test(L"a b c d", small_slices, 2, small_data,
                               sizeof(small_data));
    CHECK(n == 2);
    CHECK(small_slices[0].len == 1 && small_slices[0].data[0] == 'a');
    CHECK(small_slices[1].len == 1 && small_slices[1].data[0] == 'b');
    /* Nothing past the used bytes was written. */
    CHECK(small_data[2] == 0xAA && small_data[7] == 0xAA);

    /* data_cap: a single argument larger than the buffer cannot be
     * emitted; parsing stops without overflow and without a truncated
     * trailing argument (guard bytes remain untouched). */
    {
        static const wchar_t kLong[] = L"abcdefghijklmnopqrstuvwxyz";
        memset(small_data, 0xAA, sizeof(small_data));
        n = rt_proc_parse_for_test(kLong, small_slices, 2, small_data, 4);
        CHECK(n == 0);
        CHECK(small_data[0] == 'a');
        CHECK(small_data[4] == 0xAA && small_data[7] == 0xAA);
    }

    /* data_cap with an earlier complete argument: completed arguments
     * are returned, the oversized one is dropped. */
    memset(small_data, 0xAA, sizeof(small_data));
    n = rt_proc_parse_for_test(L"ok abcdefghijklmnop", small_slices, 2,
                               small_data, 4);
    CHECK(n == 1);
    CHECK(small_slices[0].len == 2 && small_slices[0].data[0] == 'o' &&
          small_slices[0].data[1] == 'k');
    CHECK(small_data[4] == 0xAA && small_data[7] == 0xAA);

    /* Command-line unit bound: an over-long command line stops at the
     * declared bound; the trailing argument never completes, so no
     * argument is emitted - but nothing is written past the bound and
     * the call returns without error. */
    {
        static wchar_t overlong[RT_PROC_MAX_COMMAND_UNITS + 64];
        size_t j;
        for (j = 0; j < RT_PROC_MAX_COMMAND_UNITS + 63; j++) {
            overlong[j] = L'a';
        }
        overlong[RT_PROC_MAX_COMMAND_UNITS + 63] = L'\0';
        n = parse_cmd(overlong);
        CHECK(n == 0);
    }
}

/* ---------------------------------------------------------------------------
 * 2. Live args()/cache tests
 * ------------------------------------------------------------------------- */

/* args()[0] is the program path as provided (the first token of
 * GetCommandLineW, the same token the C runtime exposes as argv[0]);
 * the result is cached: repeated calls return the same array. */
static void test_args_program_path_and_cache(void)
{
    RtSliceArray a1 = rt_proc_args();
    RtSliceArray a2 = rt_proc_args();
    const wchar_t *cmd = GetCommandLineW();
    size_t n_first;
    size_t i;
    int same;

    CHECK(a1.len >= 1);
    CHECK(a1.data != NULL);

    /* Cached: identical count, pointer, and slice contents on repeat. */
    CHECK(a2.len == a1.len);
    CHECK(a2.data == a1.data);

    /* args()[0] is the program path: the first token of the command
     * line, parsed with the same rules. */
    n_first = rt_proc_parse_for_test(cmd, s_slices, RT_PROC_MAX_ARGS,
                                     s_data, RT_PROC_DATA_CAPACITY);
    CHECK(n_first >= 1);
    CHECK(s_slices[0].len == a1.data[0].len);
    CHECK(memcmp(s_slices[0].data, a1.data[0].data, a1.data[0].len) == 0);

    /* When the C runtime exposes argv[0] (this executable was launched
     * with a program-path token), args()[0] must match it: both come
     * from the same command-line token. The test executable is always
     * launched with an explicit path, so this is expected to hold. */
    if (__argc >= 1 && __argv != NULL && __argv[0] != NULL &&
        __argv[0][0] != '\0') {
        CHECK(strlen(__argv[0]) == a1.data[0].len);
        CHECK(memcmp(__argv[0], a1.data[0].data, a1.data[0].len) == 0);
    }

    /* The argument slices are contiguous and non-overlapping; the data
     * region is exactly the concatenation of the argument lengths. */
    same = 1;
    for (i = 0; i + 1 < a1.len; i++) {
        if (a1.data[i].data + a1.data[i].len != a1.data[i + 1].data) {
            same = 0;
        }
    }
    CHECK(same);
}

/* ---------------------------------------------------------------------------
 * 3. Child-process tests
 * ------------------------------------------------------------------------- */

/* Spawn this executable with a wide command line (CreateProcessW) and
 * capture stdout/stderr plus the exit code. `payload` is appended to
 * the exe path (quoted) to form the command line. Returns 1 on success
 * (process spawned and reaped), 0 on failure. */
static int run_child(const wchar_t *payload,
                     char *out_buf, size_t out_cap, size_t *out_len,
                     char *err_buf, size_t err_cap, size_t *err_len,
                     int *exit_code)
{
    wchar_t exe[MAX_PATH + 1];
    wchar_t full_cmd[MAX_PATH + 512];
    SECURITY_ATTRIBUTES sa;
    HANDLE in_read = NULL;
    HANDLE in_write = NULL;
    HANDLE out_read = NULL;
    HANDLE out_write = NULL;
    HANDLE err_read = NULL;
    HANDLE err_write = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_dword = 0;
    DWORD got = 0;
    size_t total = 0;
    int ok = 0;
    int n;

    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        return 0;
    }

    /* Build "\"<exe>\" <payload>". */
    n = swprintf(full_cmd, MAX_PATH + 512, L"\"%s\" %s", exe, payload);
    if (n < 0) {
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    /* stdin pipe: the child never reads it (rt.proc does no I/O); a
     * fresh inheritable pipe guarantees CreateProcess succeeds and the
     * child sees EOF, independent of the parent's console state. */
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

    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = err_write;

    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(NULL, full_cmd, NULL, NULL, TRUE, 0, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(in_write);
        CloseHandle(out_write);
        CloseHandle(err_write);
        in_write = NULL;
        out_write = NULL;
        err_write = NULL;

        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_dword);

        total = 0;
        while (total + 1 < out_cap &&
               ReadFile(out_read, out_buf + total,
                        (DWORD)(out_cap - 1 - total), &got, NULL) &&
               got > 0) {
            total += (size_t)got;
        }
        out_buf[total] = '\0';
        *out_len = total;

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

/* Child mode: print the parsed arguments to stdout as one line per
 * argument: "<index>:<len>:<raw bytes>\n". The parent asserts the exact
 * bytes. Output goes through WriteFile on the OS stdout handle (not
 * CRT stdio), so there is no text-mode newline translation and the
 * parent reads exactly the bytes written. */
static int child_print_args(void)
{
    RtSliceArray a = rt_proc_args();
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    size_t i;

    for (i = 0; i < a.len; i++) {
        char hdr[48];
        int hn = snprintf(hdr, sizeof(hdr), "%zu:%zu:", i, a.data[i].len);
        DWORD w = 0;
        if (hn > 0) {
            (void)WriteFile(h, hdr, (DWORD)hn, &w, NULL);
        }
        if (a.data[i].len > 0) {
            (void)WriteFile(h, a.data[i].data, (DWORD)a.data[i].len, &w,
                            NULL);
        }
        (void)WriteFile(h, "\n", 1, &w, NULL);
    }
    return 0;
}

/* Child mode: call rt_proc_exit(code) with the code given in
 * argv[1] after "--child-exit-"; never returns (rt_proc_exit is
 * _Noreturn), so no code follows the call. */
static int child_exit(const char *code_text)
{
    char *end = NULL;
    long code = strtol(code_text, &end, 10);
    if (end == code_text) {
        fprintf(stderr, "FAIL: bad exit code '%s'\n", code_text);
        return 2;
    }
    rt_proc_exit((int32_t)code);
    /* Unreachable: rt_proc_exit never returns (spec sec. 15.3). */
}

/* End-to-end args: spawn with a crafted command line including quoted
 * whitespace, BMP and supplementary Unicode, and a lone surrogate; the
 * child prints its args and the parent asserts the exact UTF-8 bytes
 * (including U+FFFD for the lone surrogate) and that args()[0] is the
 * program path as provided (the exe path the parent used). */
static void test_child_args_pipeline(void)
{
    char out_buf[8192];
    char err_buf[1024];
    size_t out_len = 0;
    size_t err_len = 0;
    int exit_code = 0;
    wchar_t exe[MAX_PATH + 1];
    size_t exe_len;
    /* Payload: --child-args "hello world" <U+00E9><U+4E2D><U+1F600>
     * <lone high surrogate> */
    static const wchar_t kPayload[] = {
        L' ', L'-', L'-', L'c', L'h', L'i', L'l', L'd', L'-', L'a', L'r',
        L'g', L's', L' ', L'"', L'h', L'e', L'l', L'l', L'o', L' ',
        L'w', L'o', L'r', L'l', L'd', L'"', L' ',
        0x00E9, 0x4E2D, 0xD83D, 0xDE00, L' ', 0xD800, 0
    };
    char exe_utf8[MAX_PATH * 3 + 8];
    size_t exe_utf8_len = 0;
    char expect[8192];
    size_t e = 0;

    /* Expected args()[0] == the exe path (as provided, quoted in the
     * command line). Convert the exe path to UTF-8 via the shared
     * conversion rules for the expected bytes. */
    CHECK(GetModuleFileNameW(NULL, exe, MAX_PATH) != 0);
    exe_len = wcslen(exe);
    {
        size_t off = 0;
        size_t j = 0;
        while (j < exe_len) {
            uint32_t u = (uint32_t)exe[j];
            if (u >= 0xD800u && u <= 0xDBFFu && j + 1 < exe_len) {
                uint32_t lo = (uint32_t)exe[j + 1];
                if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                    u = 0x10000u + ((u - 0xD800u) << 10u) + (lo - 0xDC00u);
                    j++;
                }
            }
            if (u < 0x80u) {
                exe_utf8[off++] = (unsigned char)u;
            } else if (u < 0x800u) {
                exe_utf8[off++] = (unsigned char)(0xC0u | (u >> 6u));
                exe_utf8[off++] = (unsigned char)(0x80u | (u & 0x3Fu));
            } else if (u < 0x10000u) {
                exe_utf8[off++] = (unsigned char)(0xE0u | (u >> 12u));
                exe_utf8[off++] = (unsigned char)(0x80u | ((u >> 6u) & 0x3Fu));
                exe_utf8[off++] = (unsigned char)(0x80u | (u & 0x3Fu));
            } else {
                exe_utf8[off++] = (unsigned char)(0xF0u | (u >> 18u));
                exe_utf8[off++] = (unsigned char)(0x80u | ((u >> 12u) & 0x3Fu));
                exe_utf8[off++] = (unsigned char)(0x80u | ((u >> 6u) & 0x3Fu));
                exe_utf8[off++] = (unsigned char)(0x80u | (u & 0x3Fu));
            }
            j++;
        }
        exe_utf8_len = off;
    }

    CHECK(run_child(kPayload, out_buf, sizeof(out_buf), &out_len,
                    err_buf, sizeof(err_buf), &err_len, &exit_code));
    CHECK(exit_code == 0);
    CHECK(err_len == 0);

    /* Build the expected lines:
     *   0:<exe len>:<exe utf8>
     *   1:12:--child-args
     *   2:11:hello world
     *   3:9:<C3 A9 E4 B8 AD F0 9F 98 80>
     *   4:3:<EF BF BD>
     */
    {
        static const unsigned char kU1[] = { 0xC3, 0xA9, 0xE4, 0xB8, 0xAD,
                                             0xF0, 0x9F, 0x98, 0x80 };
        static const unsigned char kFffd[] = { 0xEF, 0xBF, 0xBD };
        int n;

        n = snprintf(expect + e, sizeof(expect) - e, "0:%zu:", exe_utf8_len);
        if (n > 0) { e += (size_t)n; }
        memcpy(expect + e, exe_utf8, exe_utf8_len);
        e += exe_utf8_len;
        n = snprintf(expect + e, sizeof(expect) - e,
                     "\n1:12:--child-args\n2:11:hello world\n3:9:");
        if (n > 0) { e += (size_t)n; }
        memcpy(expect + e, kU1, sizeof(kU1));
        e += sizeof(kU1);
        n = snprintf(expect + e, sizeof(expect) - e, "\n4:3:");
        if (n > 0) { e += (size_t)n; }
        memcpy(expect + e, kFffd, sizeof(kFffd));
        e += sizeof(kFffd);
        n = snprintf(expect + e, sizeof(expect) - e, "\n");
        if (n > 0) { e += (size_t)n; }
    }

    CHECK(out_len == e);
    CHECK(memcmp(out_buf, expect, e) == 0);
}

/* End-to-end exit: rt.proc.exit terminates with the exact code and no
 * record (stderr empty). */
static void test_child_exit(void)
{
    static const struct {
        const char *arg;
        int code;
    } kCases[] = {
        { "--child-exit-0", 0 },
        { "--child-exit-7", 7 },
        { "--child-exit-42", 42 },
        { "--child-exit-255", 255 },
    };
    size_t i;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        wchar_t argw[32];
        char out_buf[1024];
        char err_buf[1024];
        size_t out_len = 0;
        size_t err_len = 0;
        int exit_code = -999;
        size_t k;

        for (k = 0; k < 32 && kCases[i].arg[k] != '\0'; k++) {
            argw[k] = (wchar_t)(unsigned char)kCases[i].arg[k];
        }
        argw[k] = L'\0';

        CHECK(run_child(argw, out_buf, sizeof(out_buf), &out_len,
                        err_buf, sizeof(err_buf), &err_len, &exit_code));
        CHECK(exit_code == kCases[i].code);
        CHECK(out_len == 0);
        CHECK(err_len == 0); /* no record (AC2) */
    }
}

/* ---------------------------------------------------------------------------
 * Child-mode dispatch and main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "--child-args") == 0) {
            return child_print_args();
        }
        if (strncmp(argv[1], "--child-exit-", 13) == 0) {
            return child_exit(argv[1] + 13);
        }
        fprintf(stderr, "FAIL: unknown child mode '%s'\n", argv[1]);
        return 2;
    }

    test_parse_basic();
    fprintf(stderr, "after test_parse_basic\n");
    test_parse_quoting();
    fprintf(stderr, "after test_parse_quoting\n");
    test_parse_backslashes();
    fprintf(stderr, "after test_parse_backslashes\n");
    test_parse_utf16_valid();
    fprintf(stderr, "after test_parse_utf16_valid\n");
    test_parse_invalid_surrogates();
    fprintf(stderr, "after test_parse_invalid_surrogates\n");
    test_parse_bounds();
    fprintf(stderr, "after test_parse_bounds\n");
    test_args_program_path_and_cache();
    fprintf(stderr, "after test_args_program_path_and_cache\n");

    /* Child-process tests (children only: the parent must never call
     * rt_proc_exit). */
    test_child_args_pipeline();
    fprintf(stderr, "after test_child_args_pipeline\n");
    test_child_exit();
    fprintf(stderr, "after test_child_exit\n");

    if (g_failures) {
        fprintf(stderr, "rt_proc_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("rt_proc_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
