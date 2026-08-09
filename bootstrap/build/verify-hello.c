/* bootstrap/build/verify-hello.c
 *
 * WP-M0-01 entry verification program: a trivial C17 hello-world compiled and
 * linked through each accepted host-compiler build entry point
 * (build-stage0-msvc.cmd / build-stage0-clang.cmd) to verify the bootstrap
 * toolchain wiring.
 *
 * Deterministic by construction: prints exactly one line with no timestamps,
 * no host identity, and no environment dependence. Exit code 0.
 *
 * Owned by WP-M0-01 (bootstrap/build/ area). Not part of any per-area source
 * fragment; it exists only for build-entry-point verification.
 */
#include <stdio.h>

int main(void)
{
    printf("hello from AI-Co stage0\n");
    return 0;
}
