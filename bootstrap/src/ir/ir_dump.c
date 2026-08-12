/* bootstrap/src/ir/ir_dump.c
 *
 * AI-Co Stage-0 canonical IR deterministic dump and verification support
 * (WP-M0-16b2).
 *
 * Implements the deterministic dump obligations of the accepted canonical
 * IR contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1,
 * WP-M0-16a) over the IrBuild node model of WP-M0-16b1 (ir_core.h):
 *
 *   - ir_dump_write: deterministic canonical textual dump (contract
 *     sec. 11.1-11.3). Iterates the build's arrays in canonical order
 *     (nodes by id, types/consts by intern order, modules by module
 *     order), emits every node's id/kind/result type/operand ids (in
 *     evaluation order)/constant values/spans/cause links/trap codes,
 *     the interned type and constant tables, and the module order. No
 *     timestamps, pointers, environment values, or host identity.
 *
 *   - ir_dump_parse: reconstructs a new IrBuild from a dump (round-trip,
 *     contract sec. 11.4). Reconstruction order: build (base types
 *     interned) -> node shells in id order (ir_node_new assigns the same
 *     deterministic ids) -> struct/enum declaration headers (size/align,
 *     underlying type; the composite type constructors read these from
 *     the decl node) -> composite types in intern order -> consts in
 *     intern order -> full node payloads -> modules. Type/const
 *     construction verifies that interning returns exactly the dumped
 *     id, so a non-canonical dump is rejected.
 *
 *   - ir_dump_verify: dump -> parse -> re-dump -> byte compare
 *     (invariant 12) and run the sec. 10 invariant checks over the
 *     reconstructed graph; violations reported as AIC-I0501 records
 *     sorted with the contract sec. 9 comparator.
 *
 * Format and determinism notes: see ir_dump.h. Strings are escaped with
 * backslash sequences ('\\' '\s' '\t' '\n' '\r' '\xHH'); NULL node refs
 * are '-1'; NULL strings are '-'. Slot kinds are emitted as 0/1/2
 * (param/local/temp). Trap-code strings are resolved through the
 * diagnostic registry on parse (they are borrowed registry literals in
 * the node model; invariant 9 requires registry membership, so an
 * unregistered code in a dump is malformed input). Empty text fields
 * are rejected on parse (see ir_dump.h): a zero-width field (leading or
 * consecutive separators) and a token that decodes to a zero-length
 * string both produce a deterministic malformed-dump error. Numeric
 * tokens that overflow strtoll/strtoull (errno ERANGE) are rejected the
 * same way: tok_int/tok_uint check errno after conversion and refuse a
 * token whose value is out of range instead of silently clamping it
 * (MIN-2, reviewer2 t_47cce3e7).
 *
 * Ownership: ir_dump_parse returns an owned IrBuild on IR_DUMP_OK
 * (ir_build_free); on failure nothing is owned. ir_dump_verify returns
 * owned records on IR_VIOLATION (ir_records_free). Internal parse
 * records are always released.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1   /* strdup is a POSIX name (MSVC) */

#include "ir_dump.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Output buffer helpers (DiagBuf is the public growable byte buffer of the
 * diag package; diag_emit.c grows it the same way).
 * ------------------------------------------------------------------------- */

static bool d_reserve(DiagBuf *buf, size_t extra)
{
    size_t need;
    size_t cap;
    if (buf->oom) {
        return false;
    }
    if (extra > (size_t)-1 - buf->len - 1) {
        buf->oom = true;
        return false;
    }
    need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return true;
    }
    cap = buf->cap ? buf->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    {
        char *p = (char *)realloc(buf->data, cap);
        if (p == NULL) {
            buf->oom = true;
            return false;
        }
        buf->data = p;
        buf->cap = cap;
    }
    return true;
}

static bool d_append_n(DiagBuf *buf, const char *s, size_t n)
{
    if (!d_reserve(buf, n)) {
        return false;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return true;
}

static bool d_append_cstr(DiagBuf *buf, const char *s)
{
    return d_append_n(buf, s, strlen(s));
}

static bool d_printf(DiagBuf *buf, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf->oom = true;
        return false;
    }
    if ((size_t)n < sizeof(tmp)) {
        return d_append_n(buf, tmp, (size_t)n);
    }
    {
        char *big = (char *)malloc((size_t)n + 1);
        if (big == NULL) {
            buf->oom = true;
            return false;
        }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        d_append_n(buf, big, (size_t)n);
        free(big);
        return !buf->oom;
    }
}

/* ---------------------------------------------------------------------------
 * String escaping (deterministic textual form)
 * ------------------------------------------------------------------------- */

/* Escape a text field: '\\' '\s' '\t' '\n' '\r', other bytes < 0x20 or
 * >= 0x7f as '\xHH' (lowercase hex). Returns false on OOM. */
static bool d_escape(DiagBuf *buf, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        switch (*p) {
        case '\\':
            if (!d_append_cstr(buf, "\\\\")) { return false; }
            break;
        case ' ':
            if (!d_append_cstr(buf, "\\s")) { return false; }
            break;
        case '\t':
            if (!d_append_cstr(buf, "\\t")) { return false; }
            break;
        case '\n':
            if (!d_append_cstr(buf, "\\n")) { return false; }
            break;
        case '\r':
            if (!d_append_cstr(buf, "\\r")) { return false; }
            break;
        default:
            if (*p < 0x20 || *p >= 0x7f) {
                if (!d_printf(buf, "\\x%02x", (unsigned)*p)) { return false; }
            } else {
                if (!d_append_n(buf, (const char *)p, 1)) { return false; }
            }
            break;
        }
        p++;
    }
    return true;
}

/* Optional string field: '-' encodes NULL; otherwise the escaped string. */
static bool d_opt_string(DiagBuf *buf, const char *s)
{
    if (!d_append_cstr(buf, " ")) {
        return false;
    }
    if (s == NULL) {
        return d_append_cstr(buf, "-");
    }
    return d_escape(buf, s);
}

/* ---------------------------------------------------------------------------
 * Span emission (DIAGNOSTIC-CONTRACT sec. 6 shape: file + start/end
 * line, column, byte offset).
 * ------------------------------------------------------------------------- */

static bool d_span(DiagBuf *buf, const DiagSpan *span)
{
    if (!d_opt_string(buf, span != NULL ? span->file : NULL)) {
        return false;
    }
    if (span == NULL) {
        return d_printf(buf, " 0 0 0 0 0 0");
    }
    return d_printf(buf, " %lld %lld %lld %lld %lld %lld",
                    span->start.line, span->start.col, span->start.offset,
                    span->end.line, span->end.col, span->end.offset);
}

/* Node reference: id or -1. */
static bool d_node_ref(DiagBuf *buf, const IrNode *n)
{
    if (!d_append_cstr(buf, " ")) {
        return false;
    }
    if (n == NULL) {
        return d_append_cstr(buf, "-1");
    }
    return d_printf(buf, "%lld", n->id);
}

/* Type reference: id or -1. */
static bool d_type_ref(DiagBuf *buf, const IrType *t)
{
    if (!d_append_cstr(buf, " ")) {
        return false;
    }
    if (t == NULL) {
        return d_append_cstr(buf, "-1");
    }
    return d_printf(buf, "%lld", t->id);
}

/* Constant reference: id or -1. */
static bool d_const_ref(DiagBuf *buf, const IrConst *c)
{
    if (!d_append_cstr(buf, " ")) {
        return false;
    }
    if (c == NULL) {
        return d_append_cstr(buf, "-1");
    }
    return d_printf(buf, "%lld", c->id);
}

/* ---------------------------------------------------------------------------
 * Record emission
 * ------------------------------------------------------------------------- */

static bool emit_type(DiagBuf *out, const IrType *t)
{
    switch (t->kind) {
    case IRT_ARRAY:
        return d_printf(out, "T %lld array %lld %lld %lld %lld\n",
                        t->id, t->size, t->align,
                        t->u.array.elem != NULL ? t->u.array.elem->id : -1,
                        t->u.array.extent);
    case IRT_SLICE:
        return d_printf(out, "T %lld slice %lld %lld %lld\n",
                        t->id, t->size, t->align,
                        t->u.slice.elem != NULL ? t->u.slice.elem->id : -1);
    case IRT_PTR:
        return d_printf(out, "T %lld ptr %lld %lld %lld\n",
                        t->id, t->size, t->align,
                        t->u.ptr.elem != NULL ? t->u.ptr.elem->id : -1);
    case IRT_STRUCT:
        return d_printf(out, "T %lld struct %lld %lld %lld\n",
                        t->id, t->size, t->align,
                        t->u.decl != NULL ? t->u.decl->id : -1);
    case IRT_ENUM:
        return d_printf(out, "T %lld enum %lld %lld %lld\n",
                        t->id, t->size, t->align,
                        t->u.decl != NULL ? t->u.decl->id : -1);
    default:
        return d_printf(out, "T %lld %s %lld %lld\n",
                        t->id, ir_type_kind_text(t->kind), t->size, t->align);
    }
}

static bool emit_const(DiagBuf *out, const IrConst *c)
{
    size_t i;
    switch (c->kind) {
    case IRC_INT:
        return d_printf(out, "C %lld int %lld %llu\n",
                        c->id, c->type != NULL ? c->type->id : -1,
                        (unsigned long long)c->u.int_bits);
    case IRC_BOOL:
        return d_printf(out, "C %lld bool %lld %d\n",
                        c->id, c->type != NULL ? c->type->id : -1,
                        c->u.b ? 1 : 0);
    case IRC_NULL:
        return d_printf(out, "C %lld null %lld\n",
                        c->id, c->type != NULL ? c->type->id : -1);
    case IRC_STR: {
        static const char hex[] = "0123456789abcdef";
        size_t k;
        if (!d_printf(out, "C %lld str %lld %zu", c->id,
                      c->type != NULL ? c->type->id : -1,
                      c->u.str.len)) {
            return false;
        }
        if (c->u.str.len == 0) {
            return d_append_cstr(out, "\n");   /* no hex token for empty */
        }
        if (!d_append_cstr(out, " ")) {
            return false;
        }
        for (k = 0; k < c->u.str.len; k++) {
            unsigned char b = c->u.str.bytes[k];
            if (!d_append_n(out, &hex[b >> 4], 1) ||
                !d_append_n(out, &hex[b & 0x0f], 1)) {
                return false;
            }
        }
        return d_append_cstr(out, "\n");
    }
    case IRC_ENUM:
        return d_printf(out, "C %lld enum %lld %llu %lld\n",
                        c->id, c->type != NULL ? c->type->id : -1,
                        (unsigned long long)c->u.en.value,
                        c->u.en.enum_decl != NULL ? c->u.en.enum_decl->id : -1);
    case IRC_STRUCT:
        if (!d_printf(out, "C %lld struct %lld %zu",
                      c->id, c->type != NULL ? c->type->id : -1,
                      c->u.strukt.count)) {
            return false;
        }
        for (i = 0; i < c->u.strukt.count; i++) {
            if (!d_printf(out, " %lld",
                          c->u.strukt.items[i] != NULL
                              ? c->u.strukt.items[i]->id : -1)) {
                return false;
            }
        }
        return d_append_cstr(out, "\n");
    case IRC_ARRAY:
        if (!d_printf(out, "C %lld array %lld %zu",
                      c->id, c->type != NULL ? c->type->id : -1,
                      c->u.arr.count)) {
            return false;
        }
        for (i = 0; i < c->u.arr.count; i++) {
            if (!d_printf(out, " %lld",
                          c->u.arr.items[i] != NULL
                              ? c->u.arr.items[i]->id : -1)) {
                return false;
            }
        }
        return d_append_cstr(out, "\n");
    case IRC_ADDR:
        return d_printf(out, "C %lld addr %lld %lld %lld\n",
                        c->id, c->type != NULL ? c->type->id : -1,
                        c->u.addr.target != NULL ? c->u.addr.target->id : -1,
                        c->u.addr.offset);
    }
    return false;
}

/* Node payload emission: one 'P' line per node, fields per kind. The
 * line always starts with "P"; IR_EMPTY carries no fields after it. */
static bool emit_payload(DiagBuf *out, const IrNode *n)
{
    size_t i;
    if (!d_append_cstr(out, "P")) {
        return false;
    }
    switch (n->kind) {
    case IR_MODULE:
        if (!d_opt_string(out, n->u.module.name)) {
            return false;
        }
        if (!d_printf(out, " %zu", n->u.module.nimports)) { return false; }
        for (i = 0; i < n->u.module.nimports; i++) {
            if (!d_printf(out, " %lld", n->u.module.imports[i] != NULL
                            ? n->u.module.imports[i]->id : -1)) {
                return false;
            }
        }
        if (!d_printf(out, " %zu", n->u.module.ndecls)) { return false; }
        for (i = 0; i < n->u.module.ndecls; i++) {
            if (!d_printf(out, " %lld", n->u.module.decls[i] != NULL
                            ? n->u.module.decls[i]->id : -1)) {
                return false;
            }
        }
        break;
    case IR_IMPORT:
        if (!d_opt_string(out, n->u.import.name)) {
            return false;
        }
        break;
    case IR_STRUCT_DECL:
        if (!d_opt_string(out, n->u.struct_decl.name)) {
            return false;
        }
        if (!d_printf(out, " %lld %lld %zu", n->u.struct_decl.size,
                      n->u.struct_decl.align, n->u.struct_decl.nfields)) {
            return false;
        }
        for (i = 0; i < n->u.struct_decl.nfields; i++) {
            const IrField *f = &n->u.struct_decl.fields[i];
            if (!d_opt_string(out, f->name)) {
                return false;
            }
            if (!d_type_ref(out, f->type)) { return false; }
            if (!d_span(out, f->span)) { return false; }
            if (!d_printf(out, " %lld", f->byte_offset)) { return false; }
        }
        break;
    case IR_ENUM_DECL:
        if (!d_opt_string(out, n->u.enum_decl.name)) {
            return false;
        }
        if (!d_type_ref(out, n->u.enum_decl.underlying)) { return false; }
        if (!d_printf(out, " %zu", n->u.enum_decl.nmembers)) { return false; }
        for (i = 0; i < n->u.enum_decl.nmembers; i++) {
            const IrEnumMember *m = &n->u.enum_decl.members[i];
            if (!d_opt_string(out, m->name)) {
                return false;
            }
            if (!d_printf(out, " %lld", m->value)) { return false; }
            if (!d_span(out, m->span)) { return false; }
        }
        break;
    case IR_GLOBAL_CONST:
        if (!d_opt_string(out, n->u.global_const.name)) {
            return false;
        }
        if (!d_type_ref(out, n->u.global_const.type)) { return false; }
        if (!d_const_ref(out, n->u.global_const.value)) { return false; }
        break;
    case IR_GLOBAL_VAR:
        if (!d_opt_string(out, n->u.global_var.name)) {
            return false;
        }
        if (!d_type_ref(out, n->u.global_var.type)) { return false; }
        if (!d_const_ref(out, n->u.global_var.init)) { return false; }
        break;
    case IR_FUNCTION:
        if (!d_opt_string(out, n->u.function.name)) {
            return false;
        }
        if (!d_type_ref(out, n->u.function.ret_type)) { return false; }
        if (!d_printf(out, " %d", n->u.function.noreturn ? 1 : 0)) {
            return false;
        }
        if (!d_printf(out, " %zu", n->u.function.nparams)) { return false; }
        for (i = 0; i < n->u.function.nparams; i++) {
            const IrParam *p = &n->u.function.params[i];
            if (!d_opt_string(out, p->name)) {
                return false;
            }
            if (!d_type_ref(out, p->type)) { return false; }
            if (!d_printf(out, " %lld", p->slot_index)) { return false; }
            if (!d_span(out, p->span)) { return false; }
        }
        if (!d_printf(out, " %zu", n->u.function.nslots)) { return false; }
        for (i = 0; i < n->u.function.nslots; i++) {
            const IrSlot *s = n->u.function.slots[i];
            if (!d_printf(out, " %lld %s", s->index,
                          s->kind == IR_SLOT_PARAM ? "0" :
                          s->kind == IR_SLOT_LOCAL ? "1" : "2")) {
                return false;
            }
            if (!d_opt_string(out, s->name)) { return false; }
            if (!d_type_ref(out, s->type)) { return false; }
            if (!d_span(out, s->span)) { return false; }
        }
        if (!d_node_ref(out, n->u.function.body)) { return false; }
        break;
    case IR_BLOCK:
        if (!d_printf(out, " %zu", n->u.block.nstmts)) { return false; }
        for (i = 0; i < n->u.block.nstmts; i++) {
            if (!d_node_ref(out, n->u.block.stmts[i])) { return false; }
        }
        break;
    case IR_LOCAL_DECL:
        if (!d_printf(out, " %lld", n->u.local_decl.slot_index)) {
            return false;
        }
        if (!d_node_ref(out, n->u.local_decl.init)) { return false; }
        break;
    case IR_IF:
        if (!d_node_ref(out, n->u.if_stmt.cond)) { return false; }
        if (!d_node_ref(out, n->u.if_stmt.then_block)) { return false; }
        if (!d_node_ref(out, n->u.if_stmt.else_block)) { return false; }
        break;
    case IR_WHILE:
        if (!d_node_ref(out, n->u.while_stmt.cond)) { return false; }
        if (!d_node_ref(out, n->u.while_stmt.body)) { return false; }
        break;
    case IR_FOR:
        if (!d_node_ref(out, n->u.for_stmt.init)) { return false; }
        if (!d_node_ref(out, n->u.for_stmt.cond)) { return false; }
        if (!d_node_ref(out, n->u.for_stmt.step)) { return false; }
        if (!d_node_ref(out, n->u.for_stmt.body)) { return false; }
        break;
    case IR_SWITCH:
        if (!d_node_ref(out, n->u.switch_stmt.selector)) { return false; }
        if (!d_printf(out, " %zu", n->u.switch_stmt.ncases)) { return false; }
        for (i = 0; i < n->u.switch_stmt.ncases; i++) {
            if (!d_node_ref(out, n->u.switch_stmt.cases[i])) { return false; }
        }
        if (!d_node_ref(out, n->u.switch_stmt.default_clause)) { return false; }
        break;
    case IR_CASE:
        if (!d_const_ref(out, n->u.case_clause.value)) { return false; }
        if (!d_node_ref(out, n->u.case_clause.body)) { return false; }
        break;
    case IR_DEFAULT:
        if (!d_node_ref(out, n->u.default_clause.body)) { return false; }
        break;
    case IR_BREAK:
        if (!d_node_ref(out, n->u.break_stmt.target)) { return false; }
        break;
    case IR_CONTINUE:
        if (!d_node_ref(out, n->u.continue_stmt.target)) { return false; }
        break;
    case IR_RETURN:
        if (!d_node_ref(out, n->u.return_stmt.value)) { return false; }
        break;
    case IR_EXPR_STMT:
        if (!d_node_ref(out, n->u.expr_stmt.expr)) { return false; }
        break;
    case IR_EMPTY:
        break;
    case IR_CALL_TERM:
        if (!d_node_ref(out, n->u.call_term.callee)) { return false; }
        if (!d_printf(out, " %zu", n->u.call_term.nargs)) { return false; }
        for (i = 0; i < n->u.call_term.nargs; i++) {
            if (!d_node_ref(out, n->u.call_term.args[i])) { return false; }
        }
        break;
    case IR_TRAP:
        if (!d_opt_string(out, n->u.trap.code)) { return false; }
        if (!d_printf(out, " %d %lld", n->u.trap.has_user_code ? 1 : 0,
                      n->u.trap.user_code)) {
            return false;
        }
        break;
    case IR_INT: case IR_BOOL: case IR_STR: case IR_ENUM_VAL:
        if (!d_const_ref(out, n->u.constant.value)) { return false; }
        break;
    case IR_NULL:
        break;   /* no payload; the pointer type is the node's result type */
    case IR_LOCAL:
        if (!d_printf(out, " %lld", n->u.local.slot_index)) { return false; }
        break;
    case IR_GLOBAL:
        if (!d_node_ref(out, n->u.global.target)) { return false; }
        break;
    case IR_FIELD_ADDR:
        if (!d_node_ref(out, n->u.field_addr.base)) { return false; }
        if (!d_printf(out, " %lld", n->u.field_addr.field_index)) {
            return false;
        }
        break;
    case IR_INDEX_ADDR:
        if (!d_node_ref(out, n->u.index_addr.base)) { return false; }
        if (!d_node_ref(out, n->u.index_addr.index)) { return false; }
        break;
    case IR_DEREF:
        if (!d_node_ref(out, n->u.deref.ptr)) { return false; }
        break;
    case IR_LOAD:
        if (!d_node_ref(out, n->u.load.lvalue)) { return false; }
        break;
    case IR_STORE:
        if (!d_node_ref(out, n->u.store.dest)) { return false; }
        if (!d_node_ref(out, n->u.store.value)) { return false; }
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_SHL: case IR_SHR: case IR_BAND: case IR_BOR: case IR_BXOR:
    case IR_LAND: case IR_LOR:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
    case IR_SLICE_EQ: case IR_PTR_DIFF:
        if (!d_node_ref(out, n->u.binary.left)) { return false; }
        if (!d_node_ref(out, n->u.binary.right)) { return false; }
        break;
    case IR_NEG: case IR_BNOT: case IR_LNOT:
    case IR_LEN: case IR_PTR: case IR_CAST: case IR_WRAP: case IR_ZERO:
        if (!d_node_ref(out, n->u.unary.operand)) { return false; }
        break;
    case IR_SELECT:
        if (!d_node_ref(out, n->u.select.cond)) { return false; }
        if (!d_node_ref(out, n->u.select.then_value)) { return false; }
        if (!d_node_ref(out, n->u.select.else_value)) { return false; }
        break;
    case IR_CALL:
        if (!d_node_ref(out, n->u.call.callee)) { return false; }
        if (!d_printf(out, " %zu", n->u.call.nargs)) { return false; }
        for (i = 0; i < n->u.call.nargs; i++) {
            if (!d_node_ref(out, n->u.call.args[i])) { return false; }
        }
        break;
    case IR_SLICE:
        if (!d_node_ref(out, n->u.slice.base)) { return false; }
        if (!d_node_ref(out, n->u.slice.start)) { return false; }
        if (!d_node_ref(out, n->u.slice.end)) { return false; }
        break;
    case IR_PTR_ADD: case IR_PTR_SUB:
        if (!d_node_ref(out, n->u.ptr_arith.ptr)) { return false; }
        if (!d_node_ref(out, n->u.ptr_arith.offset)) { return false; }
        break;
    default:
        return false;   /* unknown kind: internal defect, never silent */
    }
    return d_append_cstr(out, "\n");
}

bool ir_dump_write(const IrBuild *build, DiagBuf *out)
{
    size_t i;
    if (!d_printf(out, "# AI-Co IR deterministic dump v1\n")) {
        return false;
    }
    if (!d_printf(out, "H %zu %zu %zu %zu\n", build->nmodules,
                  build->ntypes, build->nconsts, build->nnodes)) {
        return false;
    }
    for (i = 0; i < build->ntypes; i++) {
        if (!emit_type(out, build->types[i])) {
            return false;
        }
    }
    for (i = 0; i < build->nconsts; i++) {
        if (!emit_const(out, build->consts[i])) {
            return false;
        }
    }
    if (!d_append_cstr(out, "M")) {
        return false;
    }
    for (i = 0; i < build->nmodules; i++) {
        if (!d_printf(out, " %lld", build->modules[i] != NULL
                          ? build->modules[i]->id : -1)) {
            return false;
        }
    }
    if (!d_append_cstr(out, "\n")) {
        return false;
    }
    for (i = 0; i < build->nnodes; i++) {
        const IrNode *n = build->nodes[i];
        size_t k;
        if (!d_printf(out, "N %lld %s", n->id, ir_kind_text(n->kind))) {
            return false;
        }
        if (!d_type_ref(out, n->type)) { return false; }
        if (!d_opt_string(out, n->trap_code)) { return false; }
        if (!d_span(out, n->span)) { return false; }
        if (!d_printf(out, " %zu\n", n->cause_count)) { return false; }
        for (k = 0; k < n->cause_count; k++) {
            const IrCauseLink *c = &n->causes[k];
            if (!d_append_cstr(out, "K")) { return false; }
            if (!d_opt_string(out, c->construct_kind)) {
                return false;
            }
            if (!d_span(out, c->span)) { return false; }
            if (!d_printf(out, " %lld %lld %lld\n", c->ref_decl,
                          c->ref_type, c->ref_const)) {
                return false;
            }
        }
        if (!emit_payload(out, n)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Parse support
 * ------------------------------------------------------------------------- */

/* A parsed dump line: the raw line buffer plus a growable array of
 * whitespace-separated token pointers into it. Escapes are decoded in
 * place by the callers (decode_token), so each token is read once. */
typedef struct DumpLine {
    char **toks;
    size_t ntoks;
    size_t toks_cap;
    char *line;
    size_t line_cap;
    bool empty_field;   /* a zero-width field (leading or consecutive
                         * separators) was found on the current line */
} DumpLine;

typedef struct DumpReader {
    const char *text;
    size_t len;
    size_t pos;
    size_t lineno;
} DumpReader;

/* Decode one escaped token in place; returns the decoded pointer (same
 * buffer), or NULL on malformed escape. */
static char *decode_token(char *tok)
{
    char *r = tok;
    char *w = tok;
    while (*r) {
        if (*r == '\\') {
            r++;
            switch (*r) {
            case '\\': *w++ = '\\'; r++; break;
            case 's':  *w++ = ' ';  r++; break;
            case 't':  *w++ = '\t'; r++; break;
            case 'n':  *w++ = '\n'; r++; break;
            case 'r':  *w++ = '\r'; r++; break;
            case 'x': {
                int hi, lo;
                if (r[1] == '\0' || r[2] == '\0') {
                    return NULL;
                }
                hi = r[1]; lo = r[2];
                if (!isxdigit((unsigned char)hi) ||
                    !isxdigit((unsigned char)lo)) {
                    return NULL;
                }
                {
                    char hex[3] = { (char)hi, (char)lo, '\0' };
                    unsigned long v = strtoul(hex, NULL, 16);
                    *w++ = (char)(v & 0xff);
                }
                r += 3;
                break;
            }
            default:
                return NULL;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return tok;
}

/* Grow the token array to hold `want` tokens. */
static bool grow_toks(DumpLine *dl, size_t want)
{
    char **p;
    size_t cap = dl->toks_cap ? dl->toks_cap : 16;
    while (cap < want) {
        cap *= 2;
    }
    p = (char **)realloc(dl->toks, cap * sizeof(char *));
    if (p == NULL) {
        return false;
    }
    dl->toks = p;
    dl->toks_cap = cap;
    return true;
}

/* Split dl->line into whitespace-separated tokens (spaces and tabs).
 * Returns false on OOM. Tokens are pointers into dl->line; the line is
 * mutated (separator bytes become NUL). A zero-width field (a leading
 * separator or consecutive separators) is recorded on dl->empty_field;
 * the canonical dump never contains one, so the parse rejects the line
 * (the empty text field would otherwise collapse into the adjacent
 * field and be silently misparsed). Empty tokens are skipped. */
static bool tokenize_line(DumpLine *dl)
{
    char *s = dl->line;
    dl->ntoks = 0;
    dl->empty_field = false;
    while (*s) {
        char *start = s;
        while (*s && *s != ' ' && *s != '\t') {
            s++;
        }
        if (s > start) {
            if (dl->ntoks + 1 > dl->toks_cap && !grow_toks(dl, dl->ntoks + 1)) {
                return false;
            }
            dl->toks[dl->ntoks++] = start;
        } else {
            dl->empty_field = true;
        }
        if (*s == ' ' || *s == '\t') {
            *s = '\0';
            s++;
        }
    }
    return true;
}

/* Read the next non-empty, non-comment line into dl (tokens
 * tokenized). Returns 1 on success, 0 at EOF, -1 on OOM. */
static int read_line(DumpReader *rd, DumpLine *dl)
{
    while (rd->pos < rd->len) {
        size_t start = rd->pos;
        size_t end;
        while (rd->pos < rd->len && rd->text[rd->pos] != '\n') {
            rd->pos++;
        }
        end = rd->pos;
        if (rd->pos < rd->len) {
            rd->pos++;   /* consume '\n' */
        }
        rd->lineno++;
        if (end > start && rd->text[end - 1] == '\r') {
            end--;       /* tolerate CRLF in hand-written inputs */
        }
        if (end == start || rd->text[start] == '#') {
            continue;    /* blank / comment line */
        }
        if (end - start + 1 > dl->line_cap) {
            char *p = (char *)realloc(dl->line, end - start + 1);
            if (p == NULL) {
                return -1;
            }
            dl->line = p;
            dl->line_cap = end - start + 1;
        }
        memcpy(dl->line, rd->text + start, end - start);
        dl->line[end - start] = '\0';
        if (!tokenize_line(dl)) {
            return -1;
        }
        return 1;
    }
    return 0;
}

/* True when the line just read contains a zero-width field (a leading
 * separator or consecutive separators). The canonical dump never emits
 * one; a zero-width text field would otherwise collapse into the
 * adjacent field and be silently misparsed, so the parse rejects the
 * line. Fills errbuf with a deterministic reason. */
static bool line_has_empty_field(const DumpLine *dl, char *errbuf,
                                 size_t errbuf_size, size_t lineno)
{
    if (!dl->empty_field) {
        return false;
    }
    if (errbuf != NULL) {
        snprintf(errbuf, errbuf_size,
                 "line %zu: empty field (consecutive separators)", lineno);
    }
    return true;
}

/* Parse a non-negative integer token; returns false if malformed or if
 * the value overflows int64 (errno == ERANGE; the token is rejected
 * rather than silently clamped). */
static bool tok_int(const char *tok, int64_t *out)
{
    char *end = NULL;
    long long v;
    if (tok == NULL) {
        return false;
    }
    errno = 0;
    v = strtoll(tok, &end, 10);
    if (errno == ERANGE || end == tok || *end != '\0') {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

/* Parse an unsigned integer token (constant bit patterns / enum values);
 * returns false if malformed or if the value overflows uint64 (errno ==
 * ERANGE; the token is rejected rather than silently clamped). */
static bool tok_uint(const char *tok, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (tok == NULL) {
        return false;
    }
    errno = 0;
    v = strtoull(tok, &end, 10);
    if (errno == ERANGE || end == tok || *end != '\0') {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool tok_size(const char *tok, size_t *out)
{
    int64_t v;
    if (!tok_int(tok, &v) || v < 0) {
        return false;
    }
    *out = (size_t)v;
    return true;
}

/* Node reference token: '-1' -> NULL, else the node with that id.
 * `nodes` is the build's node array (id == index). */
static bool tok_node_ref(const char *tok, IrNode **out, IrNode **nodes,
                         size_t nnodes)
{
    int64_t v;
    if (tok == NULL) {
        return false;
    }
    if (strcmp(tok, "-1") == 0) {
        *out = NULL;
        return true;
    }
    if (!tok_int(tok, &v) || v < 0 || (size_t)v >= nnodes) {
        return false;
    }
    *out = nodes[v];
    return true;
}

static bool tok_type_ref(const char *tok, IrType **out, IrType **types,
                         size_t ntypes)
{
    int64_t v;
    if (tok == NULL) {
        return false;
    }
    if (strcmp(tok, "-1") == 0) {
        *out = NULL;
        return true;
    }
    if (!tok_int(tok, &v) || v < 0 || (size_t)v >= ntypes) {
        return false;
    }
    *out = types[v];
    return true;
}

static bool tok_const_ref(const char *tok, IrConst **out, IrConst **consts,
                          size_t nconsts)
{
    int64_t v;
    if (tok == NULL) {
        return false;
    }
    if (strcmp(tok, "-1") == 0) {
        *out = NULL;
        return true;
    }
    if (!tok_int(tok, &v) || v < 0 || (size_t)v >= nconsts) {
        return false;
    }
    *out = consts[v];
    return true;
}

/* Span token: file then 6 ints (7 tokens starting at *idx). */
static bool tok_span(DumpLine *dl, size_t *idx, DiagSpan **out,
                     char *errbuf, size_t errbuf_size, size_t lineno)
{
    const char *file;
    DiagSpan *span;
    int64_t sl, sc, so, el, ec, eo;
    if (*idx + 7 > dl->ntoks) {
        if (errbuf != NULL) {
            snprintf(errbuf, errbuf_size,
                     "line %zu: truncated span", lineno);
        }
        return false;
    }
    file = dl->toks[*idx];
    if (strcmp(file, "-") == 0) {
        file = NULL;
    } else {
        char *dec = decode_token(dl->toks[*idx]);
        if (dec == NULL) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: malformed escape in span file", lineno);
            }
            return false;
        }
        if (dec[0] == '\0') {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: empty span file", lineno);
            }
            return false;
        }
        file = dec;
    }
    if (!tok_int(dl->toks[*idx + 1], &sl) ||
        !tok_int(dl->toks[*idx + 2], &sc) ||
        !tok_int(dl->toks[*idx + 3], &so) ||
        !tok_int(dl->toks[*idx + 4], &el) ||
        !tok_int(dl->toks[*idx + 5], &ec) ||
        !tok_int(dl->toks[*idx + 6], &eo)) {
        if (errbuf != NULL) {
            snprintf(errbuf, errbuf_size,
                     "line %zu: malformed span numbers", lineno);
        }
        return false;
    }
    *idx += 7;
    if (file == NULL) {
        span = NULL;
    } else {
        span = diag_span_new_range(file, sl, sc, so, el, ec, eo);
        if (span == NULL) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size, "out of memory");
            }
            return false;
        }
    }
    *out = span;
    return true;
}

/* ---------------------------------------------------------------------------
 * Kind lookup (parser): reverse of ir_kind_text / ir_type_kind_text /
 * ir_const_kind_text by scanning the enum range.
 * ------------------------------------------------------------------------- */

static bool kind_from_text(const char *s, IrNodeKind *out)
{
    IrNodeKind k;
    if (s == NULL) {
        return false;
    }
    for (k = IR_MODULE; k <= IR_ZERO; k = (IrNodeKind)((int)k + 1)) {
        if (strcmp(ir_kind_text(k), s) == 0) {
            *out = k;
            return true;
        }
    }
    return false;
}

static bool type_kind_from_text(const char *s, IrTypeKind *out)
{
    IrTypeKind k;
    if (s == NULL) {
        return false;
    }
    for (k = IRT_VOID; k <= IRT_ENUM; k = (IrTypeKind)((int)k + 1)) {
        if (strcmp(ir_type_kind_text(k), s) == 0) {
            *out = k;
            return true;
        }
    }
    return false;
}

static bool const_kind_from_text(const char *s, IrConstKind *out)
{
    IrConstKind k;
    if (s == NULL) {
        return false;
    }
    for (k = IRC_INT; k <= IRC_ADDR; k = (IrConstKind)((int)k + 1)) {
        if (strcmp(ir_const_kind_text(k), s) == 0) {
            *out = k;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Intermediate parse records
 * ------------------------------------------------------------------------- */

typedef struct TypeRec {
    int64_t id;
    IrTypeKind kind;
    int64_t size;
    int64_t align;
    int64_t a;   /* array: elem id (extent in b); slice/ptr: elem id;
                  * struct/enum: decl node id */
    int64_t b;   /* array extent */
} TypeRec;

typedef struct ConstRec {
    int64_t id;
    IrConstKind kind;
    int64_t type_id;
    uint64_t u;          /* int bits / bool 0|1 / enum value */
    int64_t a;           /* enum: decl node id; addr: target node id */
    int64_t b;           /* addr: byte offset */
    uint8_t *bytes;      /* str bytes */
    size_t blen;
    size_t count;        /* struct/array item count */
    int64_t *items;      /* struct/array item ids */
} ConstRec;

typedef struct NodeRec {
    int64_t id;
    IrNodeKind kind;
    int64_t type_id;
    const char *trap;    /* registry literal or NULL (borrowed) */
    DiagSpan *span;
    size_t ncauses;
    char **cause_kinds;  /* decoded, owned */
    DiagSpan **cause_spans;
    int64_t *cause_decl;
    int64_t *cause_type;
    int64_t *cause_const;
    size_t npayload;
    char **payload;      /* decoded strings; numbers kept as strings */
} NodeRec;

static void free_node_rec(NodeRec *r)
{
    size_t i;
    diag_span_free(r->span);
    /* The cause arrays are allocated together only after the primary
     * span parse succeeds; a malformed-input rejection can reach this
     * cleanup with r->ncauses > 0 but the arrays still NULL. */
    if (r->cause_kinds != NULL) {
        for (i = 0; i < r->ncauses; i++) {
            free(r->cause_kinds[i]);
        }
    }
    if (r->cause_spans != NULL) {
        for (i = 0; i < r->ncauses; i++) {
            diag_span_free(r->cause_spans[i]);
        }
    }
    free(r->cause_kinds);
    free(r->cause_spans);
    free(r->cause_decl);
    free(r->cause_type);
    free(r->cause_const);
    for (i = 0; i < r->npayload; i++) {
        free(r->payload[i]);
    }
    free(r->payload);
}

/* ---------------------------------------------------------------------------
 * Payload reconstruction
 * ------------------------------------------------------------------------- */

typedef struct PayloadCursor {
    NodeRec *rec;
    size_t idx;
    char *errbuf;
    size_t errbuf_size;
} PayloadCursor;

static bool pl_int(PayloadCursor *pc, int64_t *out)
{
    if (pc->idx >= pc->rec->npayload ||
        !tok_int(pc->rec->payload[pc->idx], out)) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: expected integer at field %zu",
                     pc->idx);
        }
        return false;
    }
    pc->idx++;
    return true;
}

static bool pl_size(PayloadCursor *pc, size_t *out)
{
    int64_t v;
    if (!pl_int(pc, &v) || v < 0) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: expected count at field %zu",
                     pc->idx > 0 ? pc->idx - 1 : 0);
        }
        return false;
    }
    *out = (size_t)v;
    return true;
}

/* Required string: '-' is treated as an empty string (callers that need
 * NULL use the node ref path); returns the decoded pointer. A text
 * field that decodes to a zero-length string is an empty required field
 * and is rejected (identifiers, construct kinds, and file paths are
 * non-empty per the language facts; ir_dump_write never emits one). */
static bool pl_string(PayloadCursor *pc, char **out)
{
    if (pc->idx >= pc->rec->npayload) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: missing string at field %zu",
                     pc->idx);
        }
        return false;
    }
    if (strcmp(pc->rec->payload[pc->idx], "-") == 0) {
        *out = NULL;
    } else {
        *out = pc->rec->payload[pc->idx];
        if (**out == '\0') {
            if (pc->errbuf != NULL) {
                snprintf(pc->errbuf, pc->errbuf_size,
                         "malformed payload: empty string at field %zu",
                         pc->idx);
            }
            return false;
        }
    }
    pc->idx++;
    return true;
}

static bool pl_node(PayloadCursor *pc, IrNode **out, IrBuild *b)
{
    if (pc->idx >= pc->rec->npayload) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: missing node ref at field %zu",
                     pc->idx);
        }
        return false;
    }
    if (!tok_node_ref(pc->rec->payload[pc->idx], out, b->nodes, b->nnodes)) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: bad node ref at field %zu",
                     pc->idx);
        }
        return false;
    }
    pc->idx++;
    return true;
}

static bool pl_type(PayloadCursor *pc, IrType **out, IrBuild *b)
{
    if (pc->idx >= pc->rec->npayload) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: missing type ref at field %zu",
                     pc->idx);
        }
        return false;
    }
    if (!tok_type_ref(pc->rec->payload[pc->idx], out, b->types, b->ntypes)) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: bad type ref at field %zu",
                     pc->idx);
        }
        return false;
    }
    pc->idx++;
    return true;
}

static bool pl_const(PayloadCursor *pc, IrConst **out, IrBuild *b)
{
    if (pc->idx >= pc->rec->npayload) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: missing const ref at field %zu",
                     pc->idx);
        }
        return false;
    }
    if (!tok_const_ref(pc->rec->payload[pc->idx], out, b->consts,
                       b->nconsts)) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: bad const ref at field %zu",
                     pc->idx);
        }
        return false;
    }
    pc->idx++;
    return true;
}

static bool pl_span(PayloadCursor *pc, DiagSpan **out)
{
    size_t i = pc->idx;
    const char *file;
    int64_t sl, sc, so, el, ec, eo;
    if (i + 7 > pc->rec->npayload) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: truncated span at field %zu", i);
        }
        return false;
    }
    file = pc->rec->payload[i];
    if (strcmp(file, "-") == 0) {
        file = NULL;
    } else if (file[0] == '\0') {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: empty span file at field %zu", i);
        }
        return false;
    }
    if (!tok_int(pc->rec->payload[i + 1], &sl) ||
        !tok_int(pc->rec->payload[i + 2], &sc) ||
        !tok_int(pc->rec->payload[i + 3], &so) ||
        !tok_int(pc->rec->payload[i + 4], &el) ||
        !tok_int(pc->rec->payload[i + 5], &ec) ||
        !tok_int(pc->rec->payload[i + 6], &eo)) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size,
                     "malformed payload: bad span numbers at field %zu", i);
        }
        return false;
    }
    pc->idx += 7;
    *out = (file != NULL)
               ? diag_span_new_range(file, sl, sc, so, el, ec, eo) : NULL;
    if (file != NULL && *out == NULL) {
        if (pc->errbuf != NULL) {
            snprintf(pc->errbuf, pc->errbuf_size, "out of memory");
        }
        return false;
    }
    return true;
}

/* Trap-code resolution: node trap codes are borrowed registry literals in
 * the model; resolve through the registry so the reconstructed pointer is
 * stable. NULL for a '-' token; false when not in the registry (malformed
 * dump: invariant 9 requires registry membership). */
static bool pl_trap(PayloadCursor *pc, const char **out)
{
    char *code;
    if (!pl_string(pc, &code)) {
        return false;
    }
    if (code == NULL) {
        *out = NULL;
        return true;
    }
    {
        const DiagCodeInfo *info = diag_code_lookup(code);
        if (info == NULL) {
            if (pc->errbuf != NULL) {
                snprintf(pc->errbuf, pc->errbuf_size,
                         "malformed payload: trap code '%s' is not in the "
                         "diagnostic registry", code);
            }
            return false;
        }
        *out = info->code;
    }
    return true;
}

static bool set_payload(IrBuild *b, IrNode *n, NodeRec *rec,
                        char *errbuf, size_t errbuf_size)
{
    PayloadCursor pc;
    size_t i;
    memset(&pc, 0, sizeof(pc));
    pc.rec = rec;
    pc.errbuf = errbuf;
    pc.errbuf_size = errbuf_size;

    if (rec->type_id >= 0) {
        if ((size_t)rec->type_id >= b->ntypes) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "node %lld: type id %lld out of range", n->id,
                         rec->type_id);
            }
            return false;
        }
        n->type = b->types[rec->type_id];
    }
    n->trap_code = rec->trap;

    switch (n->kind) {
    case IR_MODULE: {
        size_t nimp, ndecl;
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.module.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.module.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        if (!pl_size(&pc, &nimp)) { return false; }
        if (nimp > 0) {
            n->u.module.imports = (IrNode **)calloc(nimp, sizeof(IrNode *));
            if (n->u.module.imports == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < nimp; i++) {
            if (!pl_node(&pc, &n->u.module.imports[i], b)) { return false; }
        }
        n->u.module.nimports = nimp;
        if (!pl_size(&pc, &ndecl)) { return false; }
        if (ndecl > 0) {
            n->u.module.decls = (IrNode **)calloc(ndecl, sizeof(IrNode *));
            if (n->u.module.decls == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < ndecl; i++) {
            if (!pl_node(&pc, &n->u.module.decls[i], b)) { return false; }
        }
        n->u.module.ndecls = ndecl;
        break;
    }
    case IR_IMPORT: {
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.import.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.import.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        break;
    }
    case IR_STRUCT_DECL: {
        size_t nf;
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.struct_decl.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.struct_decl.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        if (!pl_int(&pc, &n->u.struct_decl.size)) { return false; }
        if (!pl_int(&pc, &n->u.struct_decl.align)) { return false; }
        if (!pl_size(&pc, &nf)) { return false; }
        if (nf > 0) {
            n->u.struct_decl.fields = (IrField *)calloc(nf, sizeof(IrField));
            if (n->u.struct_decl.fields == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < nf; i++) {
            IrField *f = &n->u.struct_decl.fields[i];
            char *fname;
            if (!pl_string(&pc, &fname)) { return false; }
            f->name = fname != NULL ? strdup(fname) : NULL;
            if (fname != NULL && f->name == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
            if (!pl_type(&pc, &f->type, b)) { return false; }
            if (!pl_span(&pc, &f->span)) { return false; }
            if (!pl_int(&pc, &f->byte_offset)) { return false; }
        }
        n->u.struct_decl.nfields = nf;
        break;
    }
    case IR_ENUM_DECL: {
        size_t nm;
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.enum_decl.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.enum_decl.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        if (!pl_type(&pc, &n->u.enum_decl.underlying, b)) { return false; }
        if (!pl_size(&pc, &nm)) { return false; }
        if (nm > 0) {
            n->u.enum_decl.members = (IrEnumMember *)calloc(nm,
                                                           sizeof(IrEnumMember));
            if (n->u.enum_decl.members == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < nm; i++) {
            IrEnumMember *m = &n->u.enum_decl.members[i];
            char *mname;
            if (!pl_string(&pc, &mname)) { return false; }
            m->name = mname != NULL ? strdup(mname) : NULL;
            if (mname != NULL && m->name == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
            if (!pl_int(&pc, &m->value)) { return false; }
            if (!pl_span(&pc, &m->span)) { return false; }
        }
        n->u.enum_decl.nmembers = nm;
        break;
    }
    case IR_GLOBAL_CONST: {
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.global_const.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.global_const.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        if (!pl_type(&pc, &n->u.global_const.type, b)) { return false; }
        if (!pl_const(&pc, &n->u.global_const.value, b)) { return false; }
        break;
    }
    case IR_GLOBAL_VAR: {
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.global_var.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.global_var.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        if (!pl_type(&pc, &n->u.global_var.type, b)) { return false; }
        if (!pl_const(&pc, &n->u.global_var.init, b)) { return false; }
        break;
    }
    case IR_FUNCTION: {
        size_t np, ns;
        char *name;
        if (!pl_string(&pc, &name)) { return false; }
        n->u.function.name = name != NULL ? strdup(name) : NULL;
        if (name != NULL && n->u.function.name == NULL) {
            if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
            return false;
        }
        if (!pl_type(&pc, &n->u.function.ret_type, b)) { return false; }
        {
            int64_t nr;
            if (!pl_int(&pc, &nr)) { return false; }
            n->u.function.noreturn = (nr != 0);
        }
        if (!pl_size(&pc, &np)) { return false; }
        if (np > 0) {
            n->u.function.params = (IrParam *)calloc(np, sizeof(IrParam));
            if (n->u.function.params == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < np; i++) {
            IrParam *p = &n->u.function.params[i];
            char *pname;
            if (!pl_string(&pc, &pname)) { return false; }
            p->name = pname != NULL ? strdup(pname) : NULL;
            if (pname != NULL && p->name == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
            if (!pl_type(&pc, &p->type, b)) { return false; }
            if (!pl_int(&pc, &p->slot_index)) { return false; }
            if (!pl_span(&pc, &p->span)) { return false; }
        }
        n->u.function.nparams = np;
        if (!pl_size(&pc, &ns)) { return false; }
        if (ns > 0) {
            n->u.function.slots = (IrSlot **)calloc(ns, sizeof(IrSlot *));
            if (n->u.function.slots == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < ns; i++) {
            IrSlot *s = (IrSlot *)calloc(1, sizeof(IrSlot));
            char *sname;
            int64_t kind;
            if (s == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
            n->u.function.slots[i] = s;
            if (!pl_int(&pc, &s->index)) { goto fail_slot; }
            if (!pl_int(&pc, &kind)) { goto fail_slot; }
            if (kind < IR_SLOT_PARAM || kind > IR_SLOT_TEMP) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "malformed payload: bad slot kind %lld", kind);
                }
                goto fail_slot;
            }
            s->kind = (IrSlotKind)kind;
            if (!pl_string(&pc, &sname)) { goto fail_slot; }
            s->name = sname != NULL ? strdup(sname) : NULL;
            if (sname != NULL && s->name == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                goto fail_slot;
            }
            if (!pl_type(&pc, &s->type, b)) { goto fail_slot; }
            if (!pl_span(&pc, &s->span)) { goto fail_slot; }
            continue;
fail_slot:
            return false;
        }
        n->u.function.nslots = ns;
        if (!pl_node(&pc, &n->u.function.body, b)) { return false; }
        break;
    }
    case IR_BLOCK: {
        size_t nst;
        if (!pl_size(&pc, &nst)) { return false; }
        if (nst > 0) {
            n->u.block.stmts = (IrNode **)calloc(nst, sizeof(IrNode *));
            if (n->u.block.stmts == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < nst; i++) {
            if (!pl_node(&pc, &n->u.block.stmts[i], b)) { return false; }
        }
        n->u.block.nstmts = nst;
        break;
    }
    case IR_LOCAL_DECL:
        if (!pl_int(&pc, &n->u.local_decl.slot_index)) { return false; }
        if (!pl_node(&pc, &n->u.local_decl.init, b)) { return false; }
        break;
    case IR_IF:
        if (!pl_node(&pc, &n->u.if_stmt.cond, b)) { return false; }
        if (!pl_node(&pc, &n->u.if_stmt.then_block, b)) { return false; }
        if (!pl_node(&pc, &n->u.if_stmt.else_block, b)) { return false; }
        break;
    case IR_WHILE:
        if (!pl_node(&pc, &n->u.while_stmt.cond, b)) { return false; }
        if (!pl_node(&pc, &n->u.while_stmt.body, b)) { return false; }
        break;
    case IR_FOR:
        if (!pl_node(&pc, &n->u.for_stmt.init, b)) { return false; }
        if (!pl_node(&pc, &n->u.for_stmt.cond, b)) { return false; }
        if (!pl_node(&pc, &n->u.for_stmt.step, b)) { return false; }
        if (!pl_node(&pc, &n->u.for_stmt.body, b)) { return false; }
        break;
    case IR_SWITCH: {
        size_t nc;
        if (!pl_node(&pc, &n->u.switch_stmt.selector, b)) { return false; }
        if (!pl_size(&pc, &nc)) { return false; }
        if (nc > 0) {
            n->u.switch_stmt.cases = (IrNode **)calloc(nc, sizeof(IrNode *));
            if (n->u.switch_stmt.cases == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < nc; i++) {
            if (!pl_node(&pc, &n->u.switch_stmt.cases[i], b)) { return false; }
        }
        n->u.switch_stmt.ncases = nc;
        if (!pl_node(&pc, &n->u.switch_stmt.default_clause, b)) {
            return false;
        }
        break;
    }
    case IR_CASE:
        if (!pl_const(&pc, &n->u.case_clause.value, b)) { return false; }
        if (!pl_node(&pc, &n->u.case_clause.body, b)) { return false; }
        break;
    case IR_DEFAULT:
        if (!pl_node(&pc, &n->u.default_clause.body, b)) { return false; }
        break;
    case IR_BREAK:
        if (!pl_node(&pc, &n->u.break_stmt.target, b)) { return false; }
        break;
    case IR_CONTINUE:
        if (!pl_node(&pc, &n->u.continue_stmt.target, b)) { return false; }
        break;
    case IR_RETURN:
        if (!pl_node(&pc, &n->u.return_stmt.value, b)) { return false; }
        break;
    case IR_EXPR_STMT:
        if (!pl_node(&pc, &n->u.expr_stmt.expr, b)) { return false; }
        break;
    case IR_EMPTY:
        break;
    case IR_CALL_TERM: {
        size_t na;
        if (!pl_node(&pc, &n->u.call_term.callee, b)) { return false; }
        if (!pl_size(&pc, &na)) { return false; }
        if (na > 0) {
            n->u.call_term.args = (IrNode **)calloc(na, sizeof(IrNode *));
            if (n->u.call_term.args == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < na; i++) {
            if (!pl_node(&pc, &n->u.call_term.args[i], b)) { return false; }
        }
        n->u.call_term.nargs = na;
        break;
    }
    case IR_TRAP: {
        const char *code;
        int64_t huc;
        if (!pl_trap(&pc, &code)) { return false; }
        n->u.trap.code = code;
        if (!pl_int(&pc, &huc)) { return false; }
        n->u.trap.has_user_code = (huc != 0);
        if (!pl_int(&pc, &n->u.trap.user_code)) { return false; }
        break;
    }
    case IR_INT: case IR_BOOL: case IR_STR: case IR_ENUM_VAL:
        if (!pl_const(&pc, &n->u.constant.value, b)) { return false; }
        break;
    case IR_NULL:
        break;   /* no payload; the pointer type is the node's result type */
    case IR_LOCAL:
        if (!pl_int(&pc, &n->u.local.slot_index)) { return false; }
        break;
    case IR_GLOBAL:
        if (!pl_node(&pc, &n->u.global.target, b)) { return false; }
        break;
    case IR_FIELD_ADDR:
        if (!pl_node(&pc, &n->u.field_addr.base, b)) { return false; }
        if (!pl_int(&pc, &n->u.field_addr.field_index)) { return false; }
        break;
    case IR_INDEX_ADDR:
        if (!pl_node(&pc, &n->u.index_addr.base, b)) { return false; }
        if (!pl_node(&pc, &n->u.index_addr.index, b)) { return false; }
        break;
    case IR_DEREF:
        if (!pl_node(&pc, &n->u.deref.ptr, b)) { return false; }
        break;
    case IR_LOAD:
        if (!pl_node(&pc, &n->u.load.lvalue, b)) { return false; }
        break;
    case IR_STORE:
        if (!pl_node(&pc, &n->u.store.dest, b)) { return false; }
        if (!pl_node(&pc, &n->u.store.value, b)) { return false; }
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_SHL: case IR_SHR: case IR_BAND: case IR_BOR: case IR_BXOR:
    case IR_LAND: case IR_LOR:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
    case IR_SLICE_EQ: case IR_PTR_DIFF:
        if (!pl_node(&pc, &n->u.binary.left, b)) { return false; }
        if (!pl_node(&pc, &n->u.binary.right, b)) { return false; }
        break;
    case IR_NEG: case IR_BNOT: case IR_LNOT:
    case IR_LEN: case IR_PTR: case IR_CAST: case IR_WRAP: case IR_ZERO:
        if (!pl_node(&pc, &n->u.unary.operand, b)) { return false; }
        break;
    case IR_SELECT:
        if (!pl_node(&pc, &n->u.select.cond, b)) { return false; }
        if (!pl_node(&pc, &n->u.select.then_value, b)) { return false; }
        if (!pl_node(&pc, &n->u.select.else_value, b)) { return false; }
        break;
    case IR_CALL: {
        size_t na;
        if (!pl_node(&pc, &n->u.call.callee, b)) { return false; }
        if (!pl_size(&pc, &na)) { return false; }
        if (na > 0) {
            n->u.call.args = (IrNode **)calloc(na, sizeof(IrNode *));
            if (n->u.call.args == NULL) {
                if (errbuf != NULL) { snprintf(errbuf, errbuf_size, "out of memory"); }
                return false;
            }
        }
        for (i = 0; i < na; i++) {
            if (!pl_node(&pc, &n->u.call.args[i], b)) { return false; }
        }
        n->u.call.nargs = na;
        break;
    }
    case IR_SLICE:
        if (!pl_node(&pc, &n->u.slice.base, b)) { return false; }
        if (!pl_node(&pc, &n->u.slice.start, b)) { return false; }
        if (!pl_node(&pc, &n->u.slice.end, b)) { return false; }
        break;
    case IR_PTR_ADD: case IR_PTR_SUB:
        if (!pl_node(&pc, &n->u.ptr_arith.ptr, b)) { return false; }
        if (!pl_node(&pc, &n->u.ptr_arith.offset, b)) { return false; }
        break;
    default:
        if (errbuf != NULL) {
            snprintf(errbuf, errbuf_size, "node %lld: unsupported kind %d",
                     n->id, (int)n->kind);
        }
        return false;
    }
    return true;
}

/* Set the declaration-header fields that composite type constructors read
 * from decl nodes BEFORE those constructors run: struct size/align and
 * enum underlying type. The full payload is applied later by
 * set_payload (which rewrites the same fields from the same dump data). */
static bool set_decl_header(IrBuild *b, IrNode *n, NodeRec *rec,
                            char *errbuf, size_t errbuf_size)
{
    size_t i;
    if (n->kind == IR_STRUCT_DECL) {
        if (rec->npayload < 4) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "struct decl %lld: truncated payload", n->id);
            }
            return false;
        }
        if (!tok_int(rec->payload[1], &n->u.struct_decl.size) ||
            !tok_int(rec->payload[2], &n->u.struct_decl.align)) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "struct decl %lld: bad size/align", n->id);
            }
            return false;
        }
        return true;
    }
    if (n->kind == IR_ENUM_DECL) {
        int64_t underlying;
        if (rec->npayload < 3) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "enum decl %lld: truncated payload", n->id);
            }
            return false;
        }
        if (!tok_int(rec->payload[1], &underlying) || underlying < 0 ||
            (size_t)underlying >= b->ntypes) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "enum decl %lld: bad underlying type", n->id);
            }
            return false;
        }
        n->u.enum_decl.underlying = b->types[underlying];
        (void)i;
        return true;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * ir_dump_parse
 * ------------------------------------------------------------------------- */

IrDumpStatus ir_dump_parse(const char *text, size_t len, IrBuild **out_build,
                           char *errbuf, size_t errbuf_size)
{
    DumpReader rd;
    DumpLine dl;
    IrBuild *b = NULL;
    IrDumpStatus result = IR_DUMP_OK;
    TypeRec *types = NULL;
    size_t ntypes = 0;
    ConstRec *consts = NULL;
    size_t nconsts = 0;
    int64_t *module_ids = NULL;
    size_t nmodules = 0;
    NodeRec *nodes = NULL;
    size_t nnodes = 0;
    size_t i;
    int64_t h_nmod, h_nt, h_nc, h_nn;

    if (out_build != NULL) {
        *out_build = NULL;
    }
    if (errbuf != NULL && errbuf_size > 0) {
        errbuf[0] = '\0';
    }
    memset(&rd, 0, sizeof(rd));
    memset(&dl, 0, sizeof(dl));
    rd.text = text;
    rd.len = len;
    rd.pos = 0;

    /* Header */
    {
        int rl = read_line(&rd, &dl);
        if (rl == -1) {
            result = IR_DUMP_OOM;
            goto done;
        }
        if (rl == 0) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size, "empty dump");
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
    }
    if (dl.ntoks < 1 || strcmp(dl.toks[0], "H") != 0 ||
        dl.ntoks != 5 ||
        !tok_int(dl.toks[1], &h_nmod) || h_nmod < 0 ||
        !tok_int(dl.toks[2], &h_nt) || h_nt < 13 ||
        !tok_int(dl.toks[3], &h_nc) || h_nc < 0 ||
        !tok_int(dl.toks[4], &h_nn) || h_nn < 0) {
        if (errbuf != NULL) {
            snprintf(errbuf, errbuf_size,
                     "line %zu: expected header 'H <nmodules> <ntypes> "
                     "<nconsts> <nnodes>' with ntypes >= 13 (base types "
                     "are always interned)", rd.lineno);
        }
        result = IR_DUMP_MALFORMED;
        goto done;
    }
    nmodules = (size_t)h_nmod;
    ntypes = (size_t)h_nt;
    nconsts = (size_t)h_nc;
    nnodes = (size_t)h_nn;

    /* Types */
    if (ntypes > 0) {
        types = (TypeRec *)calloc(ntypes, sizeof(TypeRec));
        if (types == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
    }
    for (i = 0; i < ntypes; i++) {
        TypeRec *t = &types[i];
        int rl;
        if ((rl = read_line(&rd, &dl)) != 1) {
            if (rl == -1) {
                result = IR_DUMP_OOM;
            } else if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: truncated type table", rd.lineno);
                result = IR_DUMP_MALFORMED;
            } else {
                result = IR_DUMP_MALFORMED;
            }
            goto done;
        }
        if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (dl.ntoks < 1 || strcmp(dl.toks[0], "T") != 0 ||
            !tok_int(dl.toks[1], &t->id) ||
            !type_kind_from_text(dl.toks[2], &t->kind) ||
            !tok_int(dl.toks[3], &t->size) ||
            !tok_int(dl.toks[4], &t->align)) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: malformed type record", rd.lineno);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        switch (t->kind) {
        case IRT_ARRAY:
            if (dl.ntoks != 7 || !tok_int(dl.toks[5], &t->a) ||
                !tok_int(dl.toks[6], &t->b)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        case IRT_SLICE:
        case IRT_PTR:
        case IRT_STRUCT:
        case IRT_ENUM:
            if (dl.ntoks != 6 || !tok_int(dl.toks[5], &t->a)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        default:
            if (dl.ntoks != 5) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        }
    }

    /* Consts */
    if (nconsts > 0) {
        consts = (ConstRec *)calloc(nconsts, sizeof(ConstRec));
        if (consts == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
    }
    for (i = 0; i < nconsts; i++) {
        ConstRec *c = &consts[i];
        size_t j;
        int rl;
        if ((rl = read_line(&rd, &dl)) != 1) {
            if (rl == -1) {
                result = IR_DUMP_OOM;
            } else if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: truncated const table", rd.lineno);
                result = IR_DUMP_MALFORMED;
            } else {
                result = IR_DUMP_MALFORMED;
            }
            goto done;
        }
        if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (dl.ntoks < 1 || strcmp(dl.toks[0], "C") != 0 ||
            !tok_int(dl.toks[1], &c->id) ||
            !const_kind_from_text(dl.toks[2], &c->kind) ||
            !tok_int(dl.toks[3], &c->type_id)) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: malformed const record", rd.lineno);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        switch (c->kind) {
        case IRC_INT:
            if (dl.ntoks != 5 || !tok_uint(dl.toks[4], &c->u)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        case IRC_BOOL: {
            int64_t bv;
            if (dl.ntoks != 5 || !tok_int(dl.toks[4], &bv) ||
                (bv != 0 && bv != 1)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            c->u = (uint64_t)bv;
            break;
        }
        case IRC_NULL:
            if (dl.ntoks != 4) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        case IRC_STR: {
            int64_t blen;
            if (dl.ntoks < 5 || !tok_int(dl.toks[4], &blen) || blen < 0) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            if (blen == 0) {
                /* empty string: no hex token after the length */
                if (dl.ntoks != 5) {
                    result = IR_DUMP_MALFORMED;
                    goto done;
                }
            } else if (dl.ntoks != 6 ||
                       (size_t)blen * 2 != strlen(dl.toks[5])) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            c->blen = (size_t)blen;
            if (c->blen > 0) {
                const char *hexs = dl.toks[5];
                size_t h;
                for (h = 0; h < c->blen * 2; h++) {
                    if (!isxdigit((unsigned char)hexs[h])) {
                        result = IR_DUMP_MALFORMED;
                        goto done;
                    }
                }
                c->bytes = (uint8_t *)malloc(c->blen);
                if (c->bytes == NULL) {
                    result = IR_DUMP_OOM;
                    goto done;
                }
                for (j = 0; j < c->blen; j++) {
                    char pair[3] = { hexs[j * 2], hexs[j * 2 + 1], '\0' };
                    unsigned long v = strtoul(pair, NULL, 16);
                    c->bytes[j] = (uint8_t)(v & 0xff);
                }
            }
            break;
        }
        case IRC_ENUM:
            if (dl.ntoks != 6 || !tok_uint(dl.toks[4], &c->u) ||
                !tok_int(dl.toks[5], &c->a)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        case IRC_STRUCT:
        case IRC_ARRAY: {
            int64_t count;
            if (!tok_int(dl.toks[4], &count) || count < 0 ||
                (size_t)count + 5 != dl.ntoks) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            c->count = (size_t)count;
            if (c->count > 0) {
                c->items = (int64_t *)malloc(c->count * sizeof(int64_t));
                if (c->items == NULL) {
                    result = IR_DUMP_OOM;
                    goto done;
                }
                for (j = 0; j < c->count; j++) {
                    if (!tok_int(dl.toks[5 + j], &c->items[j])) {
                        result = IR_DUMP_MALFORMED;
                        goto done;
                    }
                }
            }
            break;
        }
        case IRC_ADDR:
            if (dl.ntoks != 6 || !tok_int(dl.toks[4], &c->a) ||
                !tok_int(dl.toks[5], &c->b)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            break;
        default:
            result = IR_DUMP_MALFORMED;
            goto done;
        }
    }

    /* Module order */
    {
        int rl = read_line(&rd, &dl);
        if (rl == -1) {
            result = IR_DUMP_OOM;
            goto done;
        }
        if (rl == 0) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size, "line %zu: missing module "
                         "order", rd.lineno);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
    }
    if (dl.ntoks < 1 || strcmp(dl.toks[0], "M") != 0 ||
        dl.ntoks != nmodules + 1) {
        if (errbuf != NULL) {
            snprintf(errbuf, errbuf_size,
                     "line %zu: malformed module order (expected %zu ids)",
                     rd.lineno, nmodules);
        }
        result = IR_DUMP_MALFORMED;
        goto done;
    }
    if (nmodules > 0) {
        module_ids = (int64_t *)malloc(nmodules * sizeof(int64_t));
        if (module_ids == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
    }
    for (i = 0; i < nmodules; i++) {
        if (!tok_int(dl.toks[i + 1], &module_ids[i])) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
    }

    /* Nodes */
    if (nnodes > 0) {
        nodes = (NodeRec *)calloc(nnodes, sizeof(NodeRec));
        if (nodes == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
    }
    for (i = 0; i < nnodes; i++) {
        NodeRec *r = &nodes[i];
        size_t k;
        int rl;
        if ((rl = read_line(&rd, &dl)) != 1) {
            if (rl == -1) {
                result = IR_DUMP_OOM;
            } else if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: truncated node table", rd.lineno);
                result = IR_DUMP_MALFORMED;
            } else {
                result = IR_DUMP_MALFORMED;
            }
            goto done;
        }
        if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        /* N <id> <kind> <type|-1> <trap|- > <file> <sl> <sc> <so> <el>
         * <ec> <eo> <ncauses>  (13 tokens) */
        if (dl.ntoks != 13 || strcmp(dl.toks[0], "N") != 0 ||
            !tok_int(dl.toks[1], &r->id) ||
            !kind_from_text(dl.toks[2], &r->kind) ||
            !tok_int(dl.toks[3], &r->type_id) ||
            !tok_size(dl.toks[12], &r->ncauses)) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: malformed node header", rd.lineno);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        /* trap code: '-', or a registry code (borrowed literal) */
        if (strcmp(dl.toks[4], "-") == 0) {
            r->trap = NULL;
        } else {
            char *dec = decode_token(dl.toks[4]);
            const DiagCodeInfo *info;
            if (dec == NULL) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            info = diag_code_lookup(dec);
            if (info == NULL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "line %zu: trap code '%s' is not in the "
                             "diagnostic registry", rd.lineno, dec);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            r->trap = info->code;
        }
        /* span: file at index 5, 6 position ints 6..11 */
        {
            size_t span_idx = 5;
            if (!tok_span(&dl, &span_idx, &r->span, errbuf, errbuf_size,
                          rd.lineno)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
        }
        if (r->span == NULL) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: node %lld has no primary span "
                         "(invariant 2 requires one)", rd.lineno, r->id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (r->ncauses > 0) {
            r->cause_kinds = (char **)calloc(r->ncauses, sizeof(char *));
            r->cause_spans = (DiagSpan **)calloc(r->ncauses,
                                                 sizeof(DiagSpan *));
            r->cause_decl = (int64_t *)calloc(r->ncauses, sizeof(int64_t));
            r->cause_type = (int64_t *)calloc(r->ncauses, sizeof(int64_t));
            r->cause_const = (int64_t *)calloc(r->ncauses, sizeof(int64_t));
            if (r->cause_kinds == NULL || r->cause_spans == NULL ||
                r->cause_decl == NULL || r->cause_type == NULL ||
                r->cause_const == NULL) {
                result = IR_DUMP_OOM;
                goto done;
            }
        }
        for (k = 0; k < r->ncauses; k++) {
            size_t idx = 2;
            if ((rl = read_line(&rd, &dl)) != 1) {
                if (rl == -1) {
                    result = IR_DUMP_OOM;
                } else if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "line %zu: truncated cause chain", rd.lineno);
                    result = IR_DUMP_MALFORMED;
                } else {
                    result = IR_DUMP_MALFORMED;
                }
                goto done;
            }
            if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            /* K <construct_kind> <file> <sl> <sc> <so> <el> <ec> <eo>
             * <ref_decl> <ref_type> <ref_const>  (12 tokens) */
            if (dl.ntoks != 12 || strcmp(dl.toks[0], "K") != 0) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "line %zu: malformed cause record (first='%s', "
                             "ntoks=%zu)", rd.lineno, dl.toks[0], dl.ntoks);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            if (strcmp(dl.toks[1], "-") == 0) {
                r->cause_kinds[k] = NULL;
            } else {
                char *dec = decode_token(dl.toks[1]);
                if (dec == NULL) {
                    result = IR_DUMP_MALFORMED;
                    goto done;
                }
                if (dec[0] == '\0') {
                    if (errbuf != NULL) {
                        snprintf(errbuf, errbuf_size,
                                 "line %zu: empty construct kind", rd.lineno);
                    }
                    result = IR_DUMP_MALFORMED;
                    goto done;
                }
                r->cause_kinds[k] = strdup(dec);
                if (r->cause_kinds[k] == NULL) {
                    result = IR_DUMP_OOM;
                    goto done;
                }
            }
            if (!tok_span(&dl, &idx, &r->cause_spans[k], errbuf, errbuf_size,
                          rd.lineno)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            if (!tok_int(dl.toks[9], &r->cause_decl[k]) ||
                !tok_int(dl.toks[10], &r->cause_type[k]) ||
                !tok_int(dl.toks[11], &r->cause_const[k])) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
        }
        /* Payload line */
        if ((rl = read_line(&rd, &dl)) != 1) {
            if (rl == -1) {
                result = IR_DUMP_OOM;
            } else if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: missing payload", rd.lineno);
                result = IR_DUMP_MALFORMED;
            } else {
                result = IR_DUMP_MALFORMED;
            }
            goto done;
        }
        if (line_has_empty_field(&dl, errbuf, errbuf_size, rd.lineno)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (dl.ntoks < 1 || strcmp(dl.toks[0], "P") != 0) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "line %zu: expected payload record (first='%s', "
                         "ntoks=%zu)", rd.lineno, dl.toks[0], dl.ntoks);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (dl.ntoks > 1) {
            r->payload = (char **)calloc(dl.ntoks - 1, sizeof(char *));
            if (r->payload == NULL) {
                result = IR_DUMP_OOM;
                goto done;
            }
            for (k = 1; k < dl.ntoks; k++) {
                char *dec = decode_token(dl.toks[k]);
                if (dec == NULL) {
                    result = IR_DUMP_MALFORMED;
                    goto done;
                }
                r->payload[k - 1] = strdup(dec);
                if (r->payload[k - 1] == NULL) {
                    result = IR_DUMP_OOM;
                    goto done;
                }
            }
            r->npayload = dl.ntoks - 1;
        }
    }

    /* Reconstruct */
    b = ir_build_new();
    if (b == NULL) {
        result = IR_DUMP_OOM;
        goto done;
    }
    /* 1. Node shells in id order (ir_node_new assigns deterministic ids) */
    for (i = 0; i < nnodes; i++) {
        NodeRec *r = &nodes[i];
        IrNode *n;
        size_t k;
        if (r->id != (int64_t)i) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "node ids must be gapless 0..%zu (found %lld)",
                         nnodes, r->id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        n = ir_node_new(b, r->kind, r->span);
        if (n == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
        for (k = 0; k < r->ncauses; k++) {
            if (r->cause_kinds[k] == NULL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "line %zu: cause link without a construct kind "
                             "(invariant 2 requires one)", rd.lineno);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            ir_node_add_cause(b, n, r->cause_kinds[k], r->cause_spans[k],
                              r->cause_decl[k], r->cause_type[k],
                              r->cause_const[k]);
            if (b->oom) {
                result = IR_DUMP_OOM;
                goto done;
            }
        }
    }
    /* 2. Struct/enum declaration headers (composite type constructors
     * read size/align/underlying from the decl node) */
    for (i = 0; i < nnodes; i++) {
        if (nodes[i].kind == IR_STRUCT_DECL ||
            nodes[i].kind == IR_ENUM_DECL) {
            if (!set_decl_header(b, b->nodes[i], &nodes[i], errbuf,
                                 errbuf_size)) {
                result = IR_DUMP_MALFORMED;
                goto done;
            }
        }
    }
    /* 3. Composite types in intern order; verify interning reproduces ids */
    for (i = 0; i < ntypes; i++) {
        TypeRec *t = &types[i];
        IrType *created;
        if ((int64_t)i != t->id) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "type ids must be gapless 0..%zu (found %lld)",
                         ntypes, t->id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (t->kind <= IRT_STR) {
            IrType *base = b->types[i];
            if (base->kind != t->kind || base->size != t->size ||
                base->align != t->align) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "base type %zu does not match the interned "
                             "descriptor", i);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            continue;
        }
        switch (t->kind) {
        case IRT_ARRAY: {
            IrType *elem = (t->a >= 0 && (size_t)t->a < ntypes)
                               ? b->types[t->a] : NULL;
            if (elem == NULL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "array type %zu: element type id %lld out of "
                             "range", i, t->a);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            created = ir_type_array(b, elem, t->b);
            break;
        }
        case IRT_SLICE: {
            IrType *elem = (t->a >= 0 && (size_t)t->a < ntypes)
                               ? b->types[t->a] : NULL;
            if (elem == NULL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "slice type %zu: element type id %lld out of "
                             "range", i, t->a);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            created = ir_type_slice(b, elem);
            break;
        }
        case IRT_PTR: {
            IrType *elem = (t->a >= 0 && (size_t)t->a < ntypes)
                               ? b->types[t->a] : NULL;
            if (elem == NULL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "pointer type %zu: element type id %lld out of "
                             "range", i, t->a);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            created = ir_type_ptr(b, elem);
            break;
        }
        case IRT_STRUCT: {
            IrNode *decl = (t->a >= 0 && (size_t)t->a < nnodes)
                               ? b->nodes[t->a] : NULL;
            if (decl == NULL || decl->kind != IR_STRUCT_DECL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "struct type %zu: decl id %lld is not a "
                             "IR_STRUCT_DECL node", i, t->a);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            created = ir_type_struct(b, decl);
            break;
        }
        case IRT_ENUM: {
            IrNode *decl = (t->a >= 0 && (size_t)t->a < nnodes)
                               ? b->nodes[t->a] : NULL;
            if (decl == NULL || decl->kind != IR_ENUM_DECL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "enum type %zu: decl id %lld is not a "
                             "IR_ENUM_DECL node", i, t->a);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            created = ir_type_enum(b, decl);
            break;
        }
        default:
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "type %zu: unsupported kind %d", i, (int)t->kind);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (created == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
        if (created->id != t->id) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "type %zu did not intern to id %lld (got %lld); "
                         "dump is not canonical", i, t->id, created->id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (created->size != t->size || created->align != t->align) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "type %zu size/align mismatch", i);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
    }
    /* 4. Consts in intern order; verify interning reproduces ids */
    for (i = 0; i < nconsts; i++) {
        ConstRec *c = &consts[i];
        IrConst *created = NULL;
        IrType *type = NULL;
        size_t j;
        if ((int64_t)i != c->id) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "const ids must be gapless 0..%zu (found %lld)",
                         nconsts, c->id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (c->type_id >= 0 && (size_t)c->type_id < ntypes) {
            type = b->types[c->type_id];
        }
        if (type == NULL) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "const %zu: type id %lld out of range",
                         i, c->type_id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        switch (c->kind) {
        case IRC_INT:
            created = ir_const_int(b, type, c->u);
            break;
        case IRC_BOOL:
            created = ir_const_bool(b, c->u != 0);
            break;
        case IRC_NULL:
            created = ir_const_null(b, type);
            break;
        case IRC_STR:
            created = ir_const_str(b, c->bytes, c->blen);
            break;
        case IRC_ENUM:
            created = ir_const_enum(b, type, c->u);
            break;
        case IRC_STRUCT: {
            IrConst **items = NULL;
            if (c->count > 0) {
                items = (IrConst **)calloc(c->count, sizeof(IrConst *));
                if (items == NULL) {
                    result = IR_DUMP_OOM;
                    goto done;
                }
                for (j = 0; j < c->count; j++) {
                    if (c->items[j] < 0 || (size_t)c->items[j] >= nconsts) {
                        if (errbuf != NULL) {
                            snprintf(errbuf, errbuf_size,
                                     "struct const %zu: item id %lld out of "
                                     "range", i, c->items[j]);
                        }
                        free(items);
                        result = IR_DUMP_MALFORMED;
                        goto done;
                    }
                    items[j] = b->consts[c->items[j]];
                }
            }
            created = ir_const_struct(b, type, items, c->count);
            free(items);
            break;
        }
        case IRC_ARRAY: {
            IrConst **items = NULL;
            if (c->count > 0) {
                items = (IrConst **)calloc(c->count, sizeof(IrConst *));
                if (items == NULL) {
                    result = IR_DUMP_OOM;
                    goto done;
                }
                for (j = 0; j < c->count; j++) {
                    if (c->items[j] < 0 || (size_t)c->items[j] >= nconsts) {
                        if (errbuf != NULL) {
                            snprintf(errbuf, errbuf_size,
                                     "array const %zu: item id %lld out of "
                                     "range", i, c->items[j]);
                        }
                        free(items);
                        result = IR_DUMP_MALFORMED;
                        goto done;
                    }
                    items[j] = b->consts[c->items[j]];
                }
            }
            created = ir_const_array(b, type, items, c->count);
            free(items);
            break;
        }
        case IRC_ADDR: {
            IrNode *target = (c->a >= 0 && (size_t)c->a < nnodes)
                                 ? b->nodes[c->a] : NULL;
            if (target == NULL) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "addr const %zu: target id %lld out of range",
                             i, c->a);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
            created = ir_const_addr(b, type, target, c->b);
            break;
        }
        default:
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "const %zu: unsupported kind %d",
                         i, (int)c->kind);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (created == NULL) {
            result = IR_DUMP_OOM;
            goto done;
        }
        if (created->id != c->id) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "const %zu did not intern to id %lld (got %lld); "
                         "dump is not canonical", i, c->id, created->id);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        if (c->kind == IRC_STR) {
            if (created->u.str.len != c->blen ||
                (c->blen > 0 &&
                 memcmp(created->u.str.bytes, c->bytes, c->blen) != 0)) {
                if (errbuf != NULL) {
                    snprintf(errbuf, errbuf_size,
                             "str const %zu: byte content mismatch", i);
                }
                result = IR_DUMP_MALFORMED;
                goto done;
            }
        }
    }
    /* 5. Node payloads */
    for (i = 0; i < nnodes; i++) {
        if (!set_payload(b, b->nodes[i], &nodes[i], errbuf, errbuf_size)) {
            result = IR_DUMP_MALFORMED;
            goto done;
        }
    }
    /* 6. Modules */
    for (i = 0; i < nmodules; i++) {
        IrNode *m;
        if (module_ids[i] < 0 || (size_t)module_ids[i] >= nnodes) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "module %zu: module id %lld out of range",
                         i, module_ids[i]);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        m = b->nodes[module_ids[i]];
        if (m->kind != IR_MODULE) {
            if (errbuf != NULL) {
                snprintf(errbuf, errbuf_size,
                         "module %zu: node %lld is not an IR_MODULE",
                         i, module_ids[i]);
            }
            result = IR_DUMP_MALFORMED;
            goto done;
        }
        ir_build_add_module(b, m);
        if (b->oom) {
            result = IR_DUMP_OOM;
            goto done;
        }
    }
    if (b->oom) {
        result = IR_DUMP_OOM;
        goto done;
    }

    if (out_build != NULL) {
        *out_build = b;
        b = NULL;
    }
    result = IR_DUMP_OK;

done:
    if (b != NULL) {
        ir_build_free(b);
    }
    if (result == IR_DUMP_MALFORMED && errbuf != NULL && errbuf_size > 0 &&
        errbuf[0] == '\0') {
        snprintf(errbuf, errbuf_size, "malformed dump (near line %zu)",
                 rd.lineno);
    }
    if (consts != NULL) {
        for (i = 0; i < nconsts; i++) {
            free(consts[i].bytes);
            free(consts[i].items);
        }
    }
    free(types);
    free(consts);
    free(module_ids);
    if (nodes != NULL) {
        for (i = 0; i < nnodes; i++) {
            free_node_rec(&nodes[i]);
        }
        free(nodes);
    }
    free(dl.toks);
    free(dl.line);
    return result;
}

/* ---------------------------------------------------------------------------
 * Verification: dump -> parse -> re-dump -> byte compare, then invariants
 * ------------------------------------------------------------------------- */

/* Build one AIC-I0501 record for a round-trip failure (invariant 12). */
static DiagRecord *make_inv12_record(const char *detail, const char *fmt, ...)
{
    char msg[640];
    DiagRecord *r;
    va_list ap;
    int hdr;
    hdr = snprintf(msg, sizeof(msg),
                   "IR invariant violation (invariant 12, determinism): %s",
                   detail);
    if (hdr < 0 || (size_t)hdr >= sizeof(msg)) {
        return NULL;
    }
    va_start(ap, fmt);
    vsnprintf(msg + hdr, sizeof(msg) - hdr, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    r = diag_record_new();
    if (r == NULL) {
        return NULL;
    }
    if (!diag_record_set_code(r, "AIC-I0501") ||
        !diag_record_set_message(r, msg) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE) ||
        !diag_record_add_related_int(r, "invariant", 12)) {
        diag_record_free(r);
        return NULL;
    }
    return r;
}

static bool rec_array_append(DiagRecord ***recs, size_t *n, DiagRecord *r)
{
    DiagRecord **p = (DiagRecord **)realloc(*recs, (*n + 1) * sizeof(*p));
    if (p == NULL) {
        return false;
    }
    p[*n] = r;
    *recs = p;
    *n += 1;
    return true;
}

IrStatus ir_dump_verify(const IrBuild *build, DiagRecord ***out_records,
                        size_t *out_record_count)
{
    DiagBuf buf1;
    DiagBuf buf2;
    IrBuild *rebuilt = NULL;
    IrDumpStatus ps;
    DiagRecord **recs = NULL;
    size_t nrecs = 0;
    IrStatus st;
    char errbuf[256];
    size_t i;

    if (out_records != NULL) {
        *out_records = NULL;
    }
    if (out_record_count != NULL) {
        *out_record_count = 0;
    }
    errbuf[0] = '\0';
    diag_buf_init(&buf1);
    diag_buf_init(&buf2);

    /* 1. dump */
    if (!ir_dump_write(build, &buf1)) {
        diag_buf_free(&buf1);
        diag_buf_free(&buf2);
        return IR_OOM;
    }
    /* 2. parse back */
    ps = ir_dump_parse(buf1.data, buf1.len, &rebuilt, errbuf,
                       sizeof(errbuf));
    if (ps == IR_DUMP_OOM) {
        diag_buf_free(&buf1);
        diag_buf_free(&buf2);
        return IR_OOM;
    }
    if (ps == IR_DUMP_MALFORMED) {
        DiagRecord *r = make_inv12_record(
            "dump of this build is not parseable", "%s", errbuf);
        diag_buf_free(&buf1);
        diag_buf_free(&buf2);
        if (r == NULL) {
            return IR_OOM;
        }
        if (!rec_array_append(&recs, &nrecs, r)) {
            diag_record_free(r);
            return IR_OOM;
        }
        goto finish;
    }
    /* 3. re-dump and byte-compare */
    if (!ir_dump_write(rebuilt, &buf2)) {
        ir_build_free(rebuilt);
        diag_buf_free(&buf1);
        diag_buf_free(&buf2);
        for (i = 0; i < nrecs; i++) {
            diag_record_free(recs[i]);
        }
        free(recs);
        return IR_OOM;
    }
    if (buf1.len != buf2.len ||
        memcmp(buf1.data, buf2.data, buf1.len) != 0) {
        size_t off = 0;
        size_t m = buf1.len < buf2.len ? buf1.len : buf2.len;
        while (off < m && buf1.data[off] == buf2.data[off]) {
            off++;
        }
        {
            DiagRecord *r = make_inv12_record(
                "re-dump differs from the original dump",
                " first differing byte offset %zu (original len %zu, "
                "re-dump len %zu)", off, buf1.len, buf2.len);
            if (r == NULL) {
                for (i = 0; i < nrecs; i++) {
                    diag_record_free(recs[i]);
                }
                free(recs);
                ir_build_free(rebuilt);
                diag_buf_free(&buf1);
                diag_buf_free(&buf2);
                return IR_OOM;
            }
            if (!rec_array_append(&recs, &nrecs, r)) {
                diag_record_free(r);
                for (i = 0; i < nrecs; i++) {
                    diag_record_free(recs[i]);
                }
                free(recs);
                ir_build_free(rebuilt);
                diag_buf_free(&buf1);
                diag_buf_free(&buf2);
                return IR_OOM;
            }
        }
    }
    /* 4. invariant checks over the reconstructed graph (contract
     * sec. 11.5). Re-dump byte equality already implies structural
     * agreement (invariant 12); run the core checks on the
     * reconstructed graph. */
    {
        DiagRecord **core = NULL;
        size_t ncore = 0;
        size_t j;
        st = ir_core_verify(rebuilt, &core, &ncore);
        if (st == IR_OOM) {
            for (i = 0; i < nrecs; i++) {
                diag_record_free(recs[i]);
            }
            free(recs);
            ir_build_free(rebuilt);
            diag_buf_free(&buf1);
            diag_buf_free(&buf2);
            return IR_OOM;
        }
        for (i = 0; i < ncore; i++) {
            if (!rec_array_append(&recs, &nrecs, core[i])) {
                for (j = i; j < ncore; j++) {
                    diag_record_free(core[j]);
                }
                free(core);
                for (j = 0; j < nrecs; j++) {
                    diag_record_free(recs[j]);
                }
                free(recs);
                ir_build_free(rebuilt);
                diag_buf_free(&buf1);
                diag_buf_free(&buf2);
                return IR_OOM;
            }
            core[i] = NULL;   /* ownership moved to recs */
        }
        free(core);
    }
    ir_build_free(rebuilt);
    diag_buf_free(&buf1);
    diag_buf_free(&buf2);

finish:
    if (nrecs > 0) {
        diag_sort_records(recs, nrecs);
        if (out_records != NULL) {
            *out_records = recs;
        }
        if (out_record_count != NULL) {
            *out_record_count = nrecs;
        }
        return IR_VIOLATION;
    }
    return IR_OK;
}
