/* bootstrap/src/ir/ir_dump.h
 *
 * AI-Co Stage-0 canonical IR deterministic dump and verification support
 * (WP-M0-16b2).
 *
 * Implements the deterministic dump obligations of the accepted canonical
 * IR contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1,
 * WP-M0-16a) over the IrBuild node model of WP-M0-16b1 (ir_core.h):
 *
 *   - ir_dump_write renders a build to a deterministic canonical textual
 *     form (contract sec. 11.1-11.3): every node (id, kind, result type,
 *     operand ids in evaluation order, constant values in canonical
 *     form, spans in the DIAGNOSTIC-CONTRACT sec. 6 shape, cause links,
 *     trap codes), the interned type and constant tables, and the module
 *     order. The dump is complete for reconstruction: nothing that
 *     affects output order is omitted.
 *
 *   - ir_dump_parse reconstructs an IrBuild from a dump (round-trip,
 *     contract sec. 11.4). The reconstructed graph is a new build with
 *     the same node set, order, spans, causes, types, and constants.
 *
 *   - ir_dump_verify is the verification entry point (contract
 *     sec. 11.4-11.5): dump -> parse -> re-dump -> byte compare
 *     (invariant 12), then run the sec. 10 invariant checks over the
 *     reconstructed graph; violations are reported as AIC-I0501 records
 *     sorted with the contract sec. 9 comparator.
 *
 * Determinism (contract sec. 6, 11.2, 11.3): the dump iterates the
 * build's arrays in canonical order (nodes in id order, types in intern
 * order, consts in intern order, modules in module order), emits stable
 * formatting (decimal integers, hex-encoded string-constant bytes,
 * backslash-escaped text fields), and embeds no timestamps, pointer
 * addresses, environment values, or host identity. File paths are
 * emitted exactly as stored in spans (repository-relative with '/'
 * separators per the span contract; no normalization).
 *
 * Format (one record per line, LF-terminated; '#' starts a comment):
 *
 *   H <nmodules> <ntypes> <nconsts> <nnodes>
 *   T <id> <kind> <size> <align> [<a> [<b>]]        type records, id order
 *   C <id> <kind> <type_id> <payload...>            const records, id order
 *   M <module_id> ...                               module order
 *   N <id> <kind> <type_id|-1> <trap|- > <file> <sl> <sc> <so> <el> <ec> <eo> <ncauses>
 *   K <construct_kind> <file> <sl> <sc> <so> <el> <ec> <eo> <rd> <rt> <rc>
 *   P <payload...>                                  node payload, per kind
 *
 * Type payloads: base kinds carry nothing; array carries <elem_id>
 * <extent>; slice/ptr carry <elem_id>; struct/enum carry <decl_id>.
 * Const payloads: int carries the exact bit pattern (unsigned decimal);
 * bool 0/1; null nothing; str <len> <hex bytes> (an empty string carries
 * <len> 0 and no hex token); enum <value> <enum_decl_id>; struct/array
 * <count> <item_id>...; addr <target_id> <offset>. Strings (names,
 * construct kinds, file paths, trap codes) are backslash-escaped: '\\'
 * 's' 't' 'n' 'r' and '\xHH' for other bytes. NULL node refs are '-1';
 * NULL strings are '-'.
 *
 * Empty text fields are malformed: ir_dump_parse rejects (a) a
 * zero-width field (a leading separator or consecutive separators,
 * which would otherwise collapse into the adjacent field and be
 * silently misparsed) and (b) a text token that decodes to a
 * zero-length string (e.g. the escape '\x00'). Identifiers, construct
 * kinds, and file paths are non-empty per the language facts, and
 * ir_dump_write never emits either form.
 *
 * Scope (manifest WP-M0-16b2): the dump/verification support only.
 * Node model and invariants are WP-M0-16b1 (ir_core.*, read-only here);
 * the typed-AST -> IR builder is WP-M0-16c. This package depends only
 * on ir_core.h (hence the WP-M0-06 diag model) and the C standard
 * library.
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_DUMP_H
#define AICO_BOOTSTRAP_SRC_IR_IR_DUMP_H

#include "ir_core.h"

/* ---------------------------------------------------------------------------
 * Deterministic dump
 * ------------------------------------------------------------------------- */

/* Render `build` to the canonical deterministic textual form and append
 * it to `out` (DiagBuf, e.g. initialized with diag_buf_init). Returns
 * false on allocation failure (out->oom is set); nothing is emitted on
 * failure. The output is byte-deterministic: identical IR graphs produce
 * identical bytes (contract sec. 11.3). */
bool ir_dump_write(const IrBuild *build, DiagBuf *out);

/* ---------------------------------------------------------------------------
 * Round-trip parse
 * ------------------------------------------------------------------------- */

typedef enum IrDumpStatus {
    IR_DUMP_OK = 0,       /* reconstructed build produced; *out owned by
                           * the caller (ir_build_free) */
    IR_DUMP_MALFORMED,    /* input is not a well-formed dump; errbuf (when
                           * non-NULL) receives a short deterministic
                           * reason; *out is NULL */
    IR_DUMP_OOM           /* allocation failure; *out is NULL, nothing
                           * owned */
} IrDumpStatus;

/* Parse a dump (exactly as produced by ir_dump_write) and reconstruct a
 * new IrBuild (contract sec. 11.4). `text` need not be NUL-terminated;
 * `len` is the byte length. On IR_DUMP_OK the reconstructed graph is
 * owned by the caller and satisfies the graph-side determinism
 * preconditions (unique gapless ids, interned types/constants); whether
 * it also satisfies the sec. 10 invariants is verified by the caller
 * (ir_core_verify / ir_dump_verify). */
IrDumpStatus ir_dump_parse(const char *text, size_t len, IrBuild **out_build,
                           char *errbuf, size_t errbuf_size);

/* ---------------------------------------------------------------------------
 * Verification
 * ------------------------------------------------------------------------- */

/* Round-trip + invariant verification (contract sec. 11.4-11.5,
 * invariant 12): dump `build`, parse it back, re-dump, and byte-compare
 * the re-dump to the original dump; then run the sec. 10 invariant
 * checks over the reconstructed graph. Returns:
 *   IR_OK         no violations (round-trip byte-identical and the
 *                 reconstructed graph passes all invariants);
 *   IR_VIOLATION  at least one AIC-I0501 record; *out_records /
 *                 *out_record_count are owned by the caller
 *                 (ir_records_free);
 *   IR_OOM        allocation failure; nothing owned.
 * Records are sorted with the contract sec. 9 comparator. Round-trip
 * failures carry related fact invariant=12 (message states the first
 * differing byte offset, or the parse failure); reconstructed-graph
 * violations carry the core invariant number of ir_core_verify. */
IrStatus ir_dump_verify(const IrBuild *build, DiagRecord ***out_records,
                        size_t *out_record_count);

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_DUMP_H */
