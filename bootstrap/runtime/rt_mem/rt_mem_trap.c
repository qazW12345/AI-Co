/* bootstrap/runtime/rt_mem/rt_mem_trap.c
 *
 * AI-Co Stage-0 runtime: release traps (WP-M0-14b2). See rt_mem_trap.h
 * for the interface, the ownership boundaries, and the integration
 * contract.
 *
 * Implementation summary:
 *   - the package integrates with the 14a2 public API through the
 *     release-status handler hook owned there
 *     (rt_mem_api_set_release_status_handler); it never edits
 *     rt_mem_api.c (manifest rule 4);
 *   - the release-status handler maps a release status from the 14a1
 *     core to the normative trap: RT_MEM_REL_DOUBLE -> AIC-R0812
 *     (double release), RT_MEM_REL_INVALID -> AIC-R0813 (invalid
 *     release), per spec sec. 15.5 and DIAGNOSTIC-CONTRACT sec. 11.8;
 *   - the trap record uses the WP-M0-06 diag record shape
 *     (bootstrap/src/diag/diag.h): phase = "trap", severity = "error",
 *     recovery = "authoritative", exit_code = 70. The primary span is
 *     null because the C runtime has no source mapping for the failing
 *     dealloc_bytes call site (the compiler attaches spans when it
 *     emits the call; DIAGNOSTIC-CONTRACT sec. 10 allows a null span).
 *     The record is emitted to stderr as one JSONL line, then the
 *     process terminates with the trap exit code 70.
 *   - record construction is exposed (test/diagnostic only) as
 *     rt_mem_trap_format so the release-trap tests can assert the exact
 *     record bytes without terminating the test process; the real trap
 *     path shares the same constructor.
 *
 * Windows API usage: none in this file. Trap-record emission goes to
 * stderr through the WP-M0-06 emitter (fwrite on the standard error
 * stream) and termination uses the C library exit() with the
 * DIAGNOSTIC-CONTRACT trap exit code. All Windows calls belong to the
 * 14a1 core and to later rt_io/rt_proc/rt_trap packages.
 */
#include "rt_mem_trap.h"

#include "rt_mem_api.h"
#include "rt_mem_alloc.h"

#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Record construction (shared by the trap path and the tests)
 * ------------------------------------------------------------------------- */

bool rt_mem_trap_format(int status, void *addr, DiagBuf *out)
{
    const char *code;
    const char *kind;
    char message[96];
    DiagRecord *rec;
    bool ok;

    if (out == NULL) {
        return false;
    }

    switch (status) {
    case RT_MEM_REL_DOUBLE:
        code = "AIC-R0812";
        kind = "double release of allocation";
        break;
    case RT_MEM_REL_INVALID:
        code = "AIC-R0813";
        kind = "invalid release: pointer not from allocator";
        break;
    default:
        /* Not a release-trap status (e.g. RT_MEM_REL_OK): no record.
         * The release-status handler is only invoked for non-OK
         * statuses, so this is defensive. */
        return false;
    }

    /* Message states the failing operation and the offending value
     * (DIAGNOSTIC-CONTRACT sec. 10); the value is also carried as a
     * related fact. Formatting is explicit (never %p) so the record is
     * byte-identical across the pinned toolchains. */
    snprintf(message, sizeof(message), "%s at 0x%016llx",
             kind, (unsigned long long)(uintptr_t)addr);

    rec = diag_trap_record(code, message, NULL);
    if (rec == NULL) {
        return false;
    }

    ok = diag_record_add_related_str(rec, "operation", "dealloc_bytes") &&
         diag_record_add_related_int(rec, "address",
                                     (int64_t)(uintptr_t)addr) &&
         diag_record_add_related_int(rec, "status", (int64_t)status) &&
         diag_emit_record(out, rec);

    diag_record_free(rec);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Release-status handler (WP-M0-14b2, registered via rt_mem_trap_init)
 * -------------------------------------------------------------------------
 * Called by rt_mem_dealloc_bytes exactly when the 14a1 core reports a
 * status other than RT_MEM_REL_OK (RT_MEM_REL_DOUBLE or
 * RT_MEM_REL_INVALID; rt_mem_api.h). Emits the trap record to stderr
 * and terminates the process with the trap exit code 70. The handler
 * never returns to the allocator: a release trap is authoritative
 * (DIAGNOSTIC-CONTRACT sec. 10) and the program cannot continue.
 */
static void rt_mem_trap_release_status(int status, void *addr)
{
    DiagBuf buf;

    diag_buf_init(&buf);
    (void)rt_mem_trap_format(status, addr, &buf);
    if (diag_buf_ok(&buf)) {
        (void)diag_buf_write_file(&buf, stderr);
        fflush(stderr);
    }
    diag_buf_free(&buf);

    /* Always terminate with the trap exit code, even if record
     * emission could not complete (e.g. out of memory): a failed
     * release is a trap per spec sec. 15.5, never a silent return. */
    exit(DIAG_TRAP_EXIT_CODE);
}

/* ---------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

void rt_mem_trap_init(void)
{
    rt_mem_api_set_release_status_handler(rt_mem_trap_release_status);
}
