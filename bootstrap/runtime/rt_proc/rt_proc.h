/* bootstrap/runtime/rt_proc/rt_proc.h
 *
 * AI-Co Stage-0 runtime: rt.proc (WP-M0-15b). This is the
 * source-visible rt.proc surface of spec sec. 15.3 (process arguments
 * and exit) and the compiler-emitted call subset of spec sec. 15.8,
 * on the Windows 10 22H2 x64 baseline of ADR-004.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-15b "rt.proc: process args and exit")
 * ---------------------------------------------------------------------------
 *   - rt.proc.args(): the process command line as UTF-8 byte slices.
 *     args()[0] is the program path (Windows: the executable path as
 *     provided in the command line); the remainder are the command-line
 *     arguments. The argument bytes are converted from the Windows
 *     UTF-16 command line (GetCommandLineW) to UTF-8; conversion is
 *     deterministic and lossless for valid Unicode, and invalid
 *     surrogates (lone high or lone low surrogate code units) are
 *     replaced deterministically with U+FFFD so the produced bytes are
 *     always valid UTF-8 (spec sec. 15.3). The argument splitting
 *     follows the documented Windows command-line parsing rules used by
 *     the C runtime and CommandLineToArgvW (whitespace delimiters,
 *     double-quote quoting, backslash-escaped quotes; see the parsing
 *     notes below);
 *   - rt.proc.exit(): terminates the process with the given exit code.
 *     The function never returns (spec sec. 15.3/15.7: noreturn for
 *     reachability) and writes no diagnostic record: exit is a plain
 *     process termination, not a trap (spec sec. 15.5 trap table has no
 *     entry for it; the exit code is passed through exactly).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - file/stdio I/O -> WP-M0-15a (rt_io.*); this package calls no
 *     rt.io function and performs no I/O;
 *   - the rt.trap module (rt.trap.report, user trap AIC-U0000) ->
 *     WP-M0-15c1 (rt_trap.*). Interface note (manifest sec. 5
 *     checklist): WP-M0-14/WP-M0-15 coordinate the rt.trap interface as
 *     a documented interface note, not a shared file. rt.proc does NOT
 *     call rt.trap.report: rt.proc.exit is a plain exit with no trap
 *     record and no AIC code; the 15c1 package owns all trap-reporting
 *     behavior. This header is rt.proc's side of that documented note.
 *   - allocator internals -> WP-M0-14 (rt_mem.*). This package performs
 *     no allocation: the argument storage is a fixed static,
 *     process-lifetime buffer sized to the documented Windows
 *     command-line resource bounds (see below), so rt_proc.c never
 *     calls the allocator and adds no coupling to it.
 *
 * ---------------------------------------------------------------------------
 * Symbol mapping to the AI-Co runtime surface (spec sec. 15.3 / 15.8)
 * ---------------------------------------------------------------------------
 * The AI-Co source-visible functions map to these C symbols:
 *
 *   rt.proc.args() -> u8[][]  ==
 *       RtSliceArray rt_proc_args(void)
 *   rt.proc.exit(code: i32) -> void  ==
 *       _Noreturn void rt_proc_exit(int32_t code)
 *
 * Type mapping (the natural ABI mapping of rt_mem_api.h, spec sec.
 * 15.7): i32 is int32_t, usize is size_t, u8* is an unsigned byte
 * pointer. `u8[][]` is an array of byte slices: per the Section 7.2
 * layouts a slice is two machine words (data pointer at offset 0,
 * length at offset 8), so an element is RtSlice below and the outer
 * value is RtSliceArray (an array of RtSlice plus its length). The C
 * declarations above model that as a 16-byte struct return: per the
 * internal Microsoft x64 calling convention (spec sec. 15.7) a
 * 16-byte struct return occupies RAX (data) and RDX (len), so a
 * conforming compiler emits `call rt_proc_args` and reads RAX as the
 * array base and RDX as the argument count - exactly the two words a
 * u8[][] value carries. rt_proc_exit takes the i32 code in ECX (the
 * first integer argument register).
 *
 * Determinism: given identical environmental inputs (the process
 * command line; spec sec. 15.6), rt_proc_args returns a byte-identical
 * result: same argument count, same slice boundaries, same UTF-8
 * bytes, same argument order. The result is computed once on first use
 * and cached in process-lifetime storage; repeated calls return the
 * same array (mutations made by the program through the returned
 * mutable u8[][] slices persist, matching argv semantics). The UTF-16
 * -> UTF-8 conversion is a pure function of the input code units: a
 * valid surrogate pair (high + low) decodes to the single code point
 * it encodes (lossless for valid Unicode); a high surrogate not
 * followed by a low surrogate, or a lone low surrogate, decodes to
 * U+FFFD (EF BF BD in UTF-8) and consumes only that one code unit, so
 * an invalid pair cannot shift subsequent code units - the replacement
 * is deterministic and position-stable.
 *
 * Resource bounds (declared, documented implementation contract):
 *   RT_PROC_MAX_COMMAND_UNITS - the maximum number of UTF-16 code
 *     units the runtime accepts in the command line, 32767, the
 *     documented Windows CreateProcess command-line limit (a process
 *     launched on the ADR-004 baseline receives a command line of at
 *     most 32767 characters). Input beyond the bound is an
 *     environmental input outside the model; parsing stops
 *     deterministically at the bound (see the parse contract below)
 *     and never overflows the static storage.
 *   RT_PROC_MAX_ARGS - the maximum number of arguments produced,
 *     16384 = floor((32767 + 1) / 2), the largest number of
 *     single-unit arguments the maximum-length command line can carry
 *     (each argument needs at least one code unit, separated from the
 *     next by at least one whitespace unit).
 *   RT_PROC_DATA_CAPACITY - the maximum number of UTF-8 bytes in the
 *     concatenated argument data, 98301 = 32767 * 3: each UTF-16 code
 *     unit contributes at most 3 UTF-8 bytes when it is a non-surrogate
 *     scalar value (BMP 3-byte encodings), and a surrogate pair (2
 *     units -> 4 bytes) contributes 2 bytes per unit, so 3 bytes per
 *     unit is a strict upper bound for every possible command line
 *     within the unit bound.
 *
 * Record conventions: none. This package emits no diagnostic records:
 * args() has no failure mode (spec sec. 15.3 attaches no trap to
 * argument conversion; invalid surrogates are replaced, not trapped)
 * and exit() is a plain termination that writes nothing. This differs
 * from the 15a1/15a2/14b2 packages, which raise AIC-Rxxxx trap records;
 * rt.proc intentionally has no diag dependency.
 *
 * ---------------------------------------------------------------------------
 * Command-line parsing notes (documented implementation contract)
 * ---------------------------------------------------------------------------
 * The command line is split into arguments following the documented
 * Windows parsing rules shared by the C runtime and CommandLineToArgvW
 * (the behavior rt.proc.args() must match on Windows, ADR-004):
 *   - arguments are delimited by whitespace (space or tab) outside
 *     quoted regions; runs of whitespace between arguments are skipped
 *     (no empty argument is produced by consecutive whitespace);
 *   - a double quote toggles a quoted region; whitespace inside a
 *     quoted region is literal argument content; the quote characters
 *     themselves are not part of the argument;
 *   - a run of backslashes immediately before a double quote follows
 *     the parity rule: 2n backslashes before a quote produce n literal
 *     backslashes and the quote acts as a region toggle; 2n+1
 *     backslashes before a quote produce n literal backslashes and the
 *     quote becomes a literal double quote character;
 *   - backslashes not immediately before a double quote are literal;
 *   - two consecutive double quotes inside a quoted region produce one
 *     literal double quote character (the doubled-quote rule), and a
 *     quote directly after a backslash is consumed by the parity rule
 *     above rather than the doubled-quote rule;
 *   - an unterminated quoted region runs to the end of the command
 *     line (the closing quote is implied); an argument may therefore
 *     be empty ("" produces an empty argument; "a""b" produces a"b).
 * This splitting is deterministic and matches the platform runtime on
 * the pinned baseline.
 *
 * Parse failure / bound behavior: if the command line exceeds the
 * unit bound, or a caller-provided buffer is too small, parsing stops
 * at the first point the bound would be crossed; the arguments
 * completed before that point are returned and no data past the bound
 * is written (no overflow). With the resource-bound declarations above
 * and the static process-lifetime storage, rt_proc_args always returns
 * the complete argument list for any command line the OS can provide
 * on the baseline; the stop-early behavior is exercised only by tests
 * that deliberately pass undersized buffers.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_PROC_RT_PROC_H
#define AICO_BOOTSTRAP_RUNTIME_RT_PROC_RT_PROC_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Declared resource bounds (documented implementation contract; see the
 * header notes above for the derivation of each bound)
 * ------------------------------------------------------------------------- */
#define RT_PROC_MAX_COMMAND_UNITS ((size_t)32767)
#define RT_PROC_MAX_ARGS ((size_t)16384)
#define RT_PROC_DATA_CAPACITY ((size_t)98301)

/* One byte slice: the Section 7.2 layout of a `u8[]` / `str` value
 * (data pointer at offset 0, length at offset 8). */
typedef struct RtSlice {
    unsigned char *data;
    size_t len;
} RtSlice;

/* The u8[][] value returned by rt.proc.args(): `data` points to an
 * array of `len` RtSlice elements; per the internal Microsoft x64
 * convention (spec sec. 15.7) this 16-byte value is returned in
 * RAX:RDX (data, len). */
typedef struct RtSliceArray {
    RtSlice *data;
    size_t len;
} RtSliceArray;

/* ---------------------------------------------------------------------------
 * Process functions (spec sec. 15.3 / 15.8)
 * ------------------------------------------------------------------------- */

/* Process arguments as UTF-8 byte slices (rt.proc.args). args()[0] is
 * the program path (Windows: the executable path as provided in the
 * command line); the remainder are the command-line arguments. Bytes
 * are converted from the Windows UTF-16 command line to UTF-8,
 * deterministically and losslessly for valid Unicode; invalid
 * surrogates are replaced deterministically with U+FFFD so the bytes
 * are always valid UTF-8. Computed once on first use and cached in
 * process-lifetime storage; repeated calls return the same array.
 * Never fails and never traps. */
RtSliceArray rt_proc_args(void);

/* Terminate the process with the given exit code (rt.proc.exit). The
 * process exits with exactly `code` as its exit code (observable via
 * GetExitCodeProcess / the shell $? / ERRORLEVEL). Never returns; the
 * compiler treats calls to it as terminators for reachability (spec
 * sec. 13.5/15.7). Writes no diagnostic record: this is a plain exit,
 * not a trap. */
_Noreturn void rt_proc_exit(int32_t code);

/* ---------------------------------------------------------------------------
 * TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * -------------------------------------------------------------------------
 * Parse an arbitrary NUL-terminated UTF-16 command line with exactly
 * the production parser and converter (the same code rt_proc_args
 * uses), writing into caller-provided buffers:
 *   `slices` receives up to `slices_cap` RtSlice entries (the split
 *   arguments in order; slices[0] is the program path token);
 *   `data` receives the concatenated UTF-8 bytes of the arguments
 *   (each slice points into it).
 * Returns the number of arguments produced; parsing stops
 * deterministically at the first bound crossed (command-line unit
 * bound, slices_cap, or data_cap) and never writes past a bound.
 * Returns 0 when `cmdline` is NULL. Provided so tests can exercise
 * the splitting and the UTF-16 -> UTF-8 conversion (including U+FFFD
 * replacement for invalid surrogates) deterministically without
 * spawning processes; it shares the exact production implementation. */
size_t rt_proc_parse_for_test(const wchar_t *cmdline, RtSlice *slices,
                              size_t slices_cap, unsigned char *data,
                              size_t data_cap);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_PROC_RT_PROC_H */
