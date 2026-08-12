/* bootstrap/runtime/rt_proc/rt_proc.c
 *
 * AI-Co Stage-0 runtime: rt.proc (WP-M0-15b). See rt_proc.h for the
 * interface, the symbol mapping to the AI-Co runtime surface, the
 * resource bounds, and the command-line parsing notes.
 *
 * Implementation summary:
 *   - rt_proc_args() reads the process command line with
 *     GetCommandLineW (kernel32; no CRT argv dependence, so the result
 *     is independent of the C runtime's own argv construction), splits
 *     it into arguments with the documented Windows parsing rules
 *     (whitespace delimiters, double-quote quoting, backslash-escape
 *     parity, the doubled-quote rule), converts each argument's UTF-16
 *     code units to UTF-8 deterministically (valid surrogate pairs are
 *     decoded to their code point; a high surrogate not followed by a
 *     low surrogate, or a lone low surrogate, is replaced with U+FFFD
 *     consuming one code unit), and caches the result in process-
 *     lifetime static storage on first use. The produced bytes are
 *     always valid UTF-8 (spec sec. 15.3);
 *   - the parser/converter is a pure function of the command line: the
 *     same code path serves the production entry and the test hook
 *     rt_proc_parse_for_test, which parses arbitrary caller-provided
 *     UTF-16 command lines into caller-provided buffers and stops
 *     deterministically at the first resource bound crossed;
 *   - rt_proc_exit() terminates the process with the given exit code
 *     via the C library exit() (flushing stdio and running atexit
 *     handlers exactly like every other runtime termination path in
 *     the bootstrap, e.g. the trap paths' exit(DIAG_TRAP_EXIT_CODE)),
 *     with no diagnostic record. It is declared _Noreturn: the
 *     compiler treats calls to it as terminators (spec sec. 15.7).
 *
 * Windows API usage (ADR-004 baseline: Windows 10 22H2 x64):
 *   GetCommandLineW only. No CRT heap, no allocator calls, no file
 *   I/O, no shell32/CommandLineToArgvW dependency (the parsing is
 *   implemented here to keep the runtime self-contained and fully
 *   deterministic, and to avoid LocalAlloc-based storage). The static
 *   storage is sized to the documented CreateProcess command-line
 *   limit (32767 units) so rt_proc_args never fails and never traps.
 */
#define WIN32_LEAN_AND_MEAN 1
#include "rt_proc.h"

#include <windows.h>

#include <stdbool.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Static process-lifetime storage (declared resource bounds, see header)
 * ------------------------------------------------------------------------- */
static RtSlice s_arg_slices[RT_PROC_MAX_ARGS];
static unsigned char s_arg_data[RT_PROC_DATA_CAPACITY];
static size_t s_arg_count = 0;
static int s_args_ready = 0;

/* ---------------------------------------------------------------------------
 * UTF-16 -> UTF-8 conversion
 * ------------------------------------------------------------------------- */

/* Append the UTF-8 encoding of one scalar code point `cp` to `data` at
 * `*off`, advancing `*off`. `cp` must be a valid Unicode scalar value
 * (never a surrogate; the caller replaces lone surrogates with U+FFFD
 * before calling). Returns false when fewer than the needed bytes
 * remain in `data[0..data_cap)`; on success returns true and the bytes
 * are written. The encoding is the standard 1-4 byte UTF-8 form. */
static bool rt_proc_append_utf8(uint32_t cp, unsigned char *data,
                                size_t data_cap, size_t *off)
{
    if (cp <= 0x7Fu) {
        if (*off >= data_cap) {
            return false;
        }
        data[(*off)++] = (unsigned char)cp;
    } else if (cp <= 0x7FFu) {
        if (*off + 2 > data_cap) {
            return false;
        }
        data[(*off)++] = (unsigned char)(0xC0u | (cp >> 6u));
        data[(*off)++] = (unsigned char)(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        if (*off + 3 > data_cap) {
            return false;
        }
        data[(*off)++] = (unsigned char)(0xE0u | (cp >> 12u));
        data[(*off)++] = (unsigned char)(0x80u | ((cp >> 6u) & 0x3Fu));
        data[(*off)++] = (unsigned char)(0x80u | (cp & 0x3Fu));
    } else {
        if (*off + 4 > data_cap) {
            return false;
        }
        data[(*off)++] = (unsigned char)(0xF0u | (cp >> 18u));
        data[(*off)++] = (unsigned char)(0x80u | ((cp >> 12u) & 0x3Fu));
        data[(*off)++] = (unsigned char)(0x80u | ((cp >> 6u) & 0x3Fu));
        data[(*off)++] = (unsigned char)(0x80u | (cp & 0x3Fu));
    }
    return true;
}

/* Append the UTF-8 encoding of the UTF-16 code unit(s) at `src[i]` to
 * `data` at `*off`, advancing `i` past the consumed code units. A
 * valid surrogate pair (high at i, low at i+1) decodes to its code
 * point and consumes both units; a high surrogate not followed by a
 * low surrogate, or a lone low surrogate, emits U+FFFD (EF BF BD) and
 * consumes one unit - deterministic and position-stable replacement
 * (spec sec. 15.3). `has_next` tells whether `src[i + 1]` is a real
 * code unit (the caller knows the command line is NUL-terminated and
 * that the unit at i is not the terminator). Returns false when the
 * UTF-8 bytes do not fit in `data[0..data_cap)`. */
static bool rt_proc_append_arg_unit(const wchar_t *src, int has_next,
                                    size_t *i, unsigned char *data,
                                    size_t data_cap, size_t *off)
{
    uint32_t u = (uint32_t)src[*i];

    if (u >= 0xD800u && u <= 0xDBFFu) {
        /* High surrogate: needs a low surrogate at the next position. */
        if (has_next) {
            uint32_t lo = (uint32_t)src[*i + 1];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                uint32_t cp = 0x10000u +
                              ((u - 0xD800u) << 10u) + (lo - 0xDC00u);
                if (!rt_proc_append_utf8(cp, data, data_cap, off)) {
                    return false;
                }
                *i += 2;
                return true;
            }
        }
        /* Lone high surrogate -> U+FFFD. */
        if (!rt_proc_append_utf8(0xFFFDu, data, data_cap, off)) {
            return false;
        }
        *i += 1;
        return true;
    }

    if (u >= 0xDC00u && u <= 0xDFFFu) {
        /* Lone low surrogate -> U+FFFD. */
        if (!rt_proc_append_utf8(0xFFFDu, data, data_cap, off)) {
            return false;
        }
        *i += 1;
        return true;
    }

    /* Ordinary scalar value (never a surrogate). */
    if (!rt_proc_append_utf8(u, data, data_cap, off)) {
        return false;
    }
    *i += 1;
    return true;
}

/* ---------------------------------------------------------------------------
 * Command-line splitting (documented Windows parsing rules, see header)
 * ------------------------------------------------------------------------- */

/* Parse the NUL-terminated UTF-16 `cmdline` into `slices` (up to
 * `slices_cap` RtSlice entries) with the UTF-8 bytes of every argument
 * written contiguously into `data` (up to `data_cap` bytes). Returns
 * the number of arguments produced. Parsing stops deterministically at
 * the first resource bound crossed (slices_cap, data_cap, or the
 * declared command-line unit bound RT_PROC_MAX_COMMAND_UNITS): the
 * arguments completed before that point are returned and nothing past
 * a bound is written. Returns 0 when `cmdline` is NULL or the
 * command-line unit bound is exceeded before the first argument. This
 * is the shared core of rt_proc_args and rt_proc_parse_for_test. */
static size_t rt_proc_parse_cmdline(const wchar_t *cmdline, RtSlice *slices,
                                    size_t slices_cap, unsigned char *data,
                                    size_t data_cap)
{
    size_t unit_count = 0;   /* command-line code units consumed */
    size_t arg_index = 0;    /* next argument slot */
    size_t data_off = 0;     /* next free byte in data */
    size_t cur_off = 0;      /* data offset where the current argument starts */
    size_t cur_len = 0;      /* UTF-8 bytes accumulated for the current argument */
    int in_quotes = 0;       /* inside a double-quoted region */
    int arg_open = 0;        /* an argument is being accumulated */
    int done = 0;            /* reached the end of the command line */
    size_t i = 0;

    if (cmdline == NULL) {
        return 0;
    }

    while (!done) {
        wchar_t c = cmdline[i];

        if (c == L'\0') {
            done = 1;
            break;
        }

        if (unit_count >= RT_PROC_MAX_COMMAND_UNITS) {
            /* Command line longer than the documented CreateProcess
             * limit: environmental input outside the model. Stop
             * deterministically without writing past the bound and
             * without emitting a truncated trailing argument. */
            break;
        }
        unit_count++;

        if (!in_quotes && (c == L' ' || c == L'\t')) {
            /* Whitespace outside a quoted region ends the current
             * argument (if any); consecutive whitespace produces no
             * empty argument. */
            if (arg_open) {
                if (arg_index >= slices_cap) {
                    break;
                }
                slices[arg_index].data = data + cur_off;
                slices[arg_index].len = cur_len;
                arg_index++;
                arg_open = 0;
            }
            i++;
            continue;
        }

        if (!arg_open) {
            /* Start a new argument at the current data position. */
            if (arg_index >= slices_cap) {
                break;
            }
            if (data_off >= data_cap) {
                break;
            }
            arg_open = 1;
            cur_off = data_off;
            cur_len = 0;
        }

        if (c == L'"') {
            if (in_quotes) {
                /* Inside a quoted region: a doubled quote is a literal
                 * quote; a single quote closes the region. */
                if (cmdline[i + 1] == L'"') {
                    if (data_off >= data_cap) {
                        break;
                    }
                    data[data_off++] = (unsigned char)'"';
                    cur_len++;
                    i += 2;
                    continue;
                }
                in_quotes = 0;
                i++;
                continue;
            }
            in_quotes = 1;
            i++;
            continue;
        }

        if (c == L'\\') {
            /* Backslash run: count the run; the parity rule applies
             * only when the run is immediately followed by a quote. */
            size_t run = 0;
            size_t j = i;
            while (cmdline[j] == L'\\') {
                run++;
                j++;
            }
            if (cmdline[j] == L'"') {
                /* 2n backslashes + quote: n literal backslashes, quote
                 * toggles the region. 2n+1 backslashes + quote: n
                 * literal backslashes, quote is a literal character. */
                size_t pairs = run / 2;
                size_t k;
                if (data_off + pairs > data_cap) {
                    break;
                }
                for (k = 0; k < pairs; k++) {
                    data[data_off++] = (unsigned char)'\\';
                }
                cur_len += pairs;
                if ((run & 1u) != 0u) {
                    /* Odd run: the remaining backslash escapes the
                     * quote into a literal double quote. */
                    if (data_off >= data_cap) {
                        break;
                    }
                    data[data_off++] = (unsigned char)'"';
                    cur_len++;
                    i = j + 1; /* consume the quote too */
                } else {
                    /* Even run: the quote toggles quoting. */
                    in_quotes = !in_quotes;
                    i = j + 1;
                }
                continue;
            }
            /* Backslashes not followed by a quote are literal. */
            if (data_off + run > data_cap) {
                break;
            }
            while (run-- > 0) {
                data[data_off++] = (unsigned char)'\\';
            }
            cur_len += (j - i);
            i = j;
            continue;
        }

        /* Ordinary code unit: UTF-16 -> UTF-8 (surrogate-aware).
         * The command line is NUL-terminated, so the unit at i is not
         * the terminator and src[i+1] is readable (a real unit or the
         * NUL); has_next tells whether it is a real unit. */
        if (!rt_proc_append_arg_unit(cmdline, cmdline[i + 1] != L'\0', &i,
                                     data, data_cap, &data_off)) {
            break;
        }
        cur_len = data_off - cur_off;
    }

    /* Close a trailing unterminated argument (including an empty
     * quoted argument ""), but only when the end of the command line
     * was actually reached: a resource-bound stop must not emit a
     * truncated trailing argument. */
    if (arg_open && done) {
        if (arg_index >= slices_cap) {
            return arg_index;
        }
        slices[arg_index].data = data + cur_off;
        slices[arg_index].len = cur_len;
        arg_index++;
    }

    return arg_index;
}

/* ---------------------------------------------------------------------------
 * Process functions (spec sec. 15.3 / 15.8)
 * ------------------------------------------------------------------------- */

RtSliceArray rt_proc_args(void)
{
    RtSliceArray out;

    if (!s_args_ready) {
        const wchar_t *cmd = GetCommandLineW();
        if (cmd != NULL) {
            s_arg_count = rt_proc_parse_cmdline(cmd, s_arg_slices,
                                                RT_PROC_MAX_ARGS, s_arg_data,
                                                RT_PROC_DATA_CAPACITY);
        }
        s_args_ready = 1;
    }

    out.data = s_arg_slices;
    out.len = s_arg_count;
    return out;
}

_Noreturn void rt_proc_exit(int32_t code)
{
    exit((int)code);
}

/* ---------------------------------------------------------------------------
 * TEST/DIAGNOSTIC ONLY (see header)
 * ------------------------------------------------------------------------- */

size_t rt_proc_parse_for_test(const wchar_t *cmdline, RtSlice *slices,
                              size_t slices_cap, unsigned char *data,
                              size_t data_cap)
{
    return rt_proc_parse_cmdline(cmdline, slices, slices_cap, data, data_cap);
}
