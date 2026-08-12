/* bootstrap/runtime/rt_mem/rt_mem_api.c
 *
 * AI-Co Stage-0 runtime: public allocator API (WP-M0-14a2). See
 * rt_mem_api.h for the interface, the symbol mapping to the AI-Co
 * runtime surface, and the integration contract.
 *
 * Implementation summary:
 *   - alloc_bytes delegates to the 14a1 core (rt_mem_core_alloc): the
 *     allocation registry, zero-initialization, alignment, the
 *     controlled region, and null-on-exhaustion semantics all live in
 *     the core; this package adds no allocation path of its own, so no
 *     host allocator identity can leak into behavior;
 *   - dealloc_bytes delegates to the 14a1 core (rt_mem_core_release),
 *     which marks the entry FREE and runs the 14b1 release hook when
 *     registered; a DOUBLE/INVALID status is delivered to the 14b2
 *     release-status handler (NULL by default at 14a2: deterministic
 *     no-op, no trap);
 *   - copy uses memmove (overlap-safe "as if a temporary buffer were
 *     used", spec sec. 15.1); fill uses memset. Both have fully
 *     specified observable output in C17, so they are deterministic and
 *     add no host-allocator or host-library identity.
 *
 * Windows API usage: none in this file. All Windows calls belong to
 * the 14a1 core (one MEM_RESERVE, fixed-range MEM_COMMIT, GetSystemInfo).
 */
#include "rt_mem_api.h"

#include <string.h>

/* Release-status handler for WP-M0-14b2; NULL by default (14a2). */
static RtMemReleaseStatusHandler s_release_status_handler = NULL;

void *rt_mem_alloc_bytes(size_t count)
{
    /* All registry/region semantics are owned by WP-M0-14a1. */
    return rt_mem_core_alloc(count);
}

void rt_mem_dealloc_bytes(void *p)
{
    int status = rt_mem_core_release(p);

    if (status != RT_MEM_REL_OK && s_release_status_handler != NULL) {
        s_release_status_handler(status, p);
    }
}

void rt_mem_copy(void *dst, const void *src, size_t count)
{
    if (count == 0) {
        return;
    }
    memmove(dst, src, count);
}

void rt_mem_fill(void *dst, unsigned char value, size_t count)
{
    if (count == 0) {
        return;
    }
    memset(dst, (int)value, count);
}

void rt_mem_api_set_release_status_handler(RtMemReleaseStatusHandler handler)
{
    s_release_status_handler = handler;
}
