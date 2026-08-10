/* bootstrap/src/load/load.h
 *
 * WP-M0-07 - Source loader / UTF-8 validation.
 *
 * Reads source files as bytes, validates the encoding per spec sec. 3.1, and
 * produces line-terminator-normalized source text plus a span index so that
 * every later stage computes 1-based line/col and 0-based byte offsets from
 * one canonical text (diag.h: "the loader package WP-M0-07 performs
 * terminator normalization").
 *
 * Load-level diagnostics (spec sec. 3.1, contract sec. 11.1):
 *   AIC-L0001  invalid byte / invalid UTF-8 sequence (overlong, surrogate,
 *              out-of-range, stray continuation, truncated at EOF, invalid
 *              lead byte). Primary span: the malformed run as consumed
 *              (see README.md for the exact span policy).
 *   AIC-L0002  UTF-8 BOM at the start of a file. Primary span: the BOM bytes.
 *   AIC-L0003  NUL byte (U+0000) outside a string literal or comment.
 *              Primary span: the NUL byte.
 *
 * Records are emitted through the WP-M0-06 diag package: severity error,
 * phase lex (registry defaults), recovery "authoritative" (these are
 * root-cause load failures). All load-level failures in a file are reported;
 * records are sorted with the contract sec. 9 comparator before being returned.
 *
 * The normalized text is a byte buffer (LF line endings) and MAY contain
 * embedded NUL bytes (a NUL inside a string literal or comment is legal per
 * spec sec. 3.1/sec. 4.4); `len` is authoritative, the buffer is not a C string.
 *
 * API ownership:
 *   - On LOAD_OK, *out_src is owned by the caller (load_source_free).
 *   - On LOAD_VALIDATION_ERROR, *out_records / *out_record_count are owned
 *     by the caller (load_records_free).
 *   - On LOAD_IO_ERROR / LOAD_OOM nothing is allocated.
 */
#ifndef AICO_BOOTSTRAP_SRC_LOAD_LOAD_H
#define AICO_BOOTSTRAP_SRC_LOAD_LOAD_H

#include "../diag/diag.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum LoadStatus {
    LOAD_OK = 0,             /* source produced, no diagnostics */
    LOAD_VALIDATION_ERROR,   /* load-level diagnostics produced */
    LOAD_IO_ERROR,           /* file could not be read (no diag record; no
                              * applicable code in the registry - see README) */
    LOAD_OOM                 /* allocation failure */
} LoadStatus;

typedef struct LoadSource {
    char *file;             /* span file name, as supplied by the caller */
    char *text;             /* normalized UTF-8 text, LF line endings;
                             * may contain embedded NUL bytes; len authoritative */
    size_t len;             /* byte length of text */
    int64_t *line_starts;   /* byte offset of each line's first byte;
                             * line_starts[0] == 0; count == line_count */
    int64_t line_count;     /* number of lines (1-based line number of EOF
                             * is line_count) */
} LoadSource;

/* Validate a byte buffer as a source file. `file` is the repository-relative
 * name used in spans (duplicated into the source / into records). */
LoadStatus load_source_from_bytes(const char *file,
                                  const uint8_t *bytes, size_t len,
                                  LoadSource **out_src,
                                  DiagRecord ***out_records,
                                  size_t *out_record_count);

/* Read a file (binary mode) and validate it. `path` is used verbatim as the
 * span file name. Returns LOAD_IO_ERROR when the file cannot be opened or
 * read; no diag record is produced for I/O failures. */
LoadStatus load_source_from_file(const char *path,
                                 LoadSource **out_src,
                                 DiagRecord ***out_records,
                                 size_t *out_record_count);

void load_source_free(LoadSource *src);
void load_records_free(DiagRecord **records, size_t count);

/* Position lookup on the normalized text: 1-based line, 1-based UTF-8 byte
 * column, for a 0-based byte offset. Returns false for offset < 0 or
 * offset > len (offset == len is the EOF position). */
bool load_position(const LoadSource *src, int64_t offset,
                   int64_t *line, int64_t *col);

/* Span builders over the normalized text (point and half-open range
 * [start, end); end position is the byte after the last included byte). */
DiagSpan *load_span_point(const LoadSource *src, int64_t offset);
DiagSpan *load_span_range(const LoadSource *src, int64_t start_offset,
                          int64_t end_offset);

#endif /* AICO_BOOTSTRAP_SRC_LOAD_LOAD_H */
