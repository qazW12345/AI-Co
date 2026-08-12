/* bootstrap/runtime/rt_trap/rt_trap.c
 *
 * AI-Co Stage-0 runtime: rt.trap (WP-M0-15c1). See rt_trap.h for the
 * interface, the symbol mapping to the AI-Co runtime surface, the
 * resource bound, and the record conventions.
 *
 * Implementation summary:
 *   - rt_trap_report() copies the caller's message into a fixed
 *     process-lifetime static buffer (deterministic truncation at the
 *     first NUL byte or the declared bound RT_TRAP_MAX_MESSAGE_BYTES;
 *     no allocation, no allocator coupling), builds the AIC-U0000 user
 *     trap record with the WP-M0-06 diag shape via
 *     diag_user_trap_record (phase "trap", severity "error", recovery
 *     "authoritative", trap_code = caller code, exit_code 70, null
 *     primary span), writes the record to stderr as one JSONL line
 *     (DIAGNOSTIC-CONTRACT sec. 10), flushes, and terminates the
 *     process with the trap exit code 70. The function never returns;
 *     a user trap is authoritative and the program cannot continue.
 *   - record construction is exposed (test/diagnostic only) as
 *     rt_trap_format so the trap tests can assert the exact record
 *     bytes without terminating the test process; the real trap path
 *     shares the same constructor.
 *
 * Windows API usage: none in this file. Trap-record emission goes to
 * stderr through the WP-M0-06 emitter (fwrite on the standard error
 * stream) and termination uses the C library exit() with the
 * DIAGNOSTIC-CONTRACT trap exit code, exactly like the 14b2/15a1/15a2
 * trap paths. All Windows calls belong to the 14a1 core and to the
 * rt_io/rt_proc packages.
 */
#include "rt_trap.h"

#include <stdio.h>
#include <stdlib.h>

/* Fixed process-lifetime message buffer (declared resource bound in
 * the header): the emitted record message is a C string in the WP-M0-06
 * model, so the caller's str bytes are copied here and NUL-terminated.
 * The copy stops deterministically at the first NUL byte (bytes past an
 * embedded NUL are not representable in the record) or at the bound,
 * whichever comes first. */
static char s_message_buf[RT_TRAP_MAX_MESSAGE_BYTES];

/* Copy the caller's message bytes into s_message_buf and NUL-terminate.
 * `data` NULL or `len` 0 yields an empty message. Never reads past
 * `len` and never writes past the bound. */
static void rt_trap_copy_message(const unsigned char *data, size_t len)
{
    size_t n = len;
    size_t i;

    if (n >= RT_TRAP_MAX_MESSAGE_BYTES) {
        n = RT_TRAP_MAX_MESSAGE_BYTES - 1;
    }
    for (i = 0; i < n && data != NULL && data[i] != '\0'; ++i) {
        s_message_buf[i] = (char)data[i];
    }
    s_message_buf[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * Record construction (shared by the trap path and the tests)
 * ------------------------------------------------------------------------- */

bool rt_trap_format(uint32_t code, const unsigned char *message_data,
                    size_t message_len, DiagBuf *out)
{
    DiagRecord *rec;
    bool ok;

    if (out == NULL) {
        return false;
    }

    rt_trap_copy_message(message_data, message_len);

    /* diag_user_trap_record validates the caller code (0..UINT32_MAX;
     * a uint32_t argument is always in range), sets code "AIC-U0000",
     * trap_code = caller code, phase "trap", severity "error",
     * recovery "authoritative", exit_code 70 (DIAGNOSTIC-CONTRACT
     * sec. 10), and a null primary span. */
    rec = diag_user_trap_record((int64_t)code, s_message_buf, NULL);
    if (rec == NULL) {
        return false;
    }

    /* Related facts carry the operation and the caller code, matching
     * the sibling runtime trap records (14b2: operation/address/status;
     * 15a1/15a2: operation/handle) and contract sec. 10's guidance that
     * related carries operation/type/value facts. */
    ok = diag_record_add_related_str(rec, "operation", "trap.report") &&
         diag_record_add_related_int(rec, "code", (int64_t)code) &&
         diag_emit_record(out, rec);

    diag_record_free(rec);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Live trap path (rt.trap.report)
 * ------------------------------------------------------------------------- */

_Noreturn void rt_trap_report(uint32_t code,
                              const unsigned char *message_data,
                              size_t message_len)
{
    DiagBuf buf;

    diag_buf_init(&buf);
    (void)rt_trap_format(code, message_data, message_len, &buf);
    if (diag_buf_ok(&buf)) {
        (void)diag_buf_write_file(&buf, stderr);
        fflush(stderr);
    }
    diag_buf_free(&buf);

    /* Always terminate with the trap exit code, even if record
     * emission could not complete (e.g. out of memory): a trap is
     * authoritative (DIAGNOSTIC-CONTRACT sec. 10), never a silent
     * return. */
    exit(DIAG_TRAP_EXIT_CODE);
}
