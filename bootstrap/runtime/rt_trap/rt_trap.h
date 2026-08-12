/* bootstrap/runtime/rt_trap/rt_trap.h
 *
 * AI-Co Stage-0 runtime: rt.trap (WP-M0-15c1). This is the
 * source-visible rt.trap surface of spec sec. 15.4 (trap reporting)
 * and the compiler-emitted trap-report call of spec sec. 15.8, on the
 * Windows 10 22H2 x64 baseline of ADR-004.
 *
 * ---------------------------------------------------------------------------
 * What this package delivers (WP-M0-15c1 "rt.trap module")
 * ---------------------------------------------------------------------------
 *   - rt.trap.report(code: u32, message: str) -> void: raises a named
 *     user trap. The runtime emits a JSONL trap record on stderr per
 *     DIAGNOSTIC-CONTRACT sec. 10 - code "AIC-U0000", trap_code = the
 *     caller-supplied u32 code, phase "trap", severity "error",
 *     recovery "authoritative", exit_code 70 - and then terminates the
 *     process with exit code 70 (the trap exit code). The function
 *     never returns (spec sec. 15.4/15.7: noreturn for reachability).
 *
 * Ownership boundaries (never produced here; the manifest sec. 2
 * file-ownership matrix is binding):
 *   - stable runtime traps with fixed codes (AIC-Rxxxx) are raised by
 *     the owning package directly through the WP-M0-06 diag shape,
 *     exactly as WP-M0-14b2 raised AIC-R0812/R0813 and WP-M0-15a1/15a2
 *     raised AIC-R0807/AIC-R0814 (rt_io_stdio.h). rt.trap does not own
 *     or centralize those records;
 *   - allocator internals -> WP-M0-14 (rt_mem.*). This package performs
 *     no allocation: the message is copied into a fixed process-
 *     lifetime static buffer (declared resource bound below), so
 *     rt_trap.c never calls the allocator and adds no coupling to it;
 *   - file/stdio I/O -> WP-M0-15a (rt_io.*); process args/exit ->
 *     WP-M0-15b (rt_proc.*). This package writes the trap record
 *     directly to the C standard error stream (fwrite/fflush) like the
 *     WP-M0-06 emitter's other trap paths and terminates with the C
 *     library exit(); it calls no rt.io/rt.proc function.
 *
 * ---------------------------------------------------------------------------
 * Symbol mapping to the AI-Co runtime surface (spec sec. 15.4 / 15.8)
 * ---------------------------------------------------------------------------
 * The AI-Co source-visible function maps to this C symbol:
 *
 *   rt.trap.report(code: u32, message: str) -> void  ==
 *       _Noreturn void rt_trap_report(uint32_t code,
 *                                     const unsigned char *message_data,
 *                                     size_t message_len)
 *
 * Type mapping (the natural ABI mapping of rt_mem_api.h/rt_io_core.h,
 * spec sec. 15.7): u32 is uint32_t, and `str` follows the Section 7.2
 * layout (data pointer at offset 0, length at offset 8) spelled out as
 * two argument words in consecutive integer/pointer registers per the
 * internal Microsoft x64 calling convention (spec sec. 15.7) - exactly
 * the rt.io.open convention of rt_io_core.h. A conforming compiler
 * emits: RCX=code, RDX=message_data, R8=message_len.
 *
 * Determinism: given identical arguments, rt_trap_report emits a
 * byte-identical record and exits with the identical code. The record
 * contains no timestamps and no absolute host paths; the message bytes
 * are the caller's bytes (JSON-escaped by the WP-M0-06 emitter), so
 * identical calls produce identical JSONL lines.
 *
 * Resource bound (declared, documented implementation contract):
 *   RT_TRAP_MAX_MESSAGE_BYTES - the maximum number of message bytes
 *     carried into the emitted record, 4096. A caller message longer
 *     than the bound is truncated deterministically at the bound (the
 *     first RT_TRAP_MAX_MESSAGE_BYTES - 1 bytes are emitted); a
 *     message containing an embedded NUL byte is emitted up to that
 *     NUL (the record message is a C string in the WP-M0-06 model, so
 *     bytes past an embedded NUL are not representable). Both behaviors
 *     are deterministic and documented; no message bytes beyond the
 *     bound are ever read.
 *
 * Record conventions: only the AIC-U0000 record is emitted by this
 * package, with a null primary span (the C runtime has no source
 * mapping for the failing call site; the compiler attaches spans when
 * it emits the call - DIAGNOSTIC-CONTRACT sec. 10), phase "trap",
 * severity "error", recovery "authoritative", exit_code 70, trap_code =
 * caller code, and the related facts operation ("trap.report") and code
 * (the caller code). This matches the 14b2/15a1/15a2 trap-record
 * conventions and the user-trap golden fixture shape
 * (bootstrap/src/diag/golden/user-trap-null-span.jsonl).
 *
 * Line endings and CRT text-mode translation: at the emitter/DiagBuf
 * level (and in rt_trap_format's in-process byte assertions) the
 * record is a single LF-terminated JSONL line. On the live trap path
 * the record is written to the C standard error stream via fwrite,
 * and on the ADR-004 Windows baseline the CRT standard streams are
 * text-mode by default: the CRT translates each LF to CRLF at the OS
 * handle, so the bytes observed on a console, pipe, or file capture
 * are CRLF-terminated ("...}\r\n"). This matches every sibling trap
 * path (14b2/15a1/15a2 and the rt_mem AIC-R0812 path) and is not a
 * contract deviation: DIAGNOSTIC-CONTRACT sec. 10 defines the JSONL
 * record fields and shape, not the physical line terminator of the
 * text-mode stderr stream. Both byte forms are deterministic; only
 * the observation level differs.
 */
#ifndef AICO_BOOTSTRAP_RUNTIME_RT_TRAP_RT_TRAP_H
#define AICO_BOOTSTRAP_RUNTIME_RT_TRAP_RT_TRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../src/diag/diag.h"

/* Declared resource bound (documented implementation contract; see the
 * header notes above). The message buffer is RT_TRAP_MAX_MESSAGE_BYTES
 * bytes including the NUL terminator. */
#define RT_TRAP_MAX_MESSAGE_BYTES ((size_t)4096)

/* Raise a named user trap (rt.trap.report). Emits the AIC-U0000 JSONL
 * trap record to stderr (trap_code = code, exit_code = 70) and
 * terminates the process with exit code 70 (DIAG_TRAP_EXIT_CODE).
 * Never returns; the compiler treats calls to it as terminators for
 * reachability (spec sec. 13.5/15.7). `message_data`/`message_len` is
 * the caller's message (a `str` value, sec. 7.2); the emitted message
 * is the caller's bytes up to the first NUL byte or the declared bound
 * (see the header notes). */
_Noreturn void rt_trap_report(uint32_t code,
                              const unsigned char *message_data,
                              size_t message_len);

/* TEST/DIAGNOSTIC ONLY - not part of the runtime contract.
 * Formats the AIC-U0000 user-trap record for `code`/`message_data`/
 * `message_len` into `out` as exactly the JSONL line the trap path
 * would write to stderr (no exit, no stderr write). Returns true and
 * appends the record; returns false and appends nothing when `out` is
 * NULL or on record/emission failure. Message truncation/embedded-NUL
 * handling is identical to rt_trap_report. Used by the trap tests to
 * assert the exact record bytes without terminating the test process;
 * the live trap path shares this constructor. */
bool rt_trap_format(uint32_t code, const unsigned char *message_data,
                    size_t message_len, DiagBuf *out);

#endif /* AICO_BOOTSTRAP_RUNTIME_RT_TRAP_RT_TRAP_H */
