/* bootstrap/src/types/type_tables.c
 *
 * AI-Co Stage-0 type tables and completeness rules (WP-M0-11a).
 * See type_tables.h for the model; design decisions in README.md.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "type_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Primitive type table (spec sec. 7.1)
 * ------------------------------------------------------------------------- */

static const TypePrimInfo kPrims[] = {
    /* kind, name, is_integer, is_signed, is_pointer_sized,
     * width_bits, size_bytes, align_bytes */
    { AST_PRIM_VOID,  "void",  false, false, false, 0,  0,  0 },
    { AST_PRIM_BOOL,  "bool",  false, false, false, 0,  1,  1 },
    { AST_PRIM_STR,   "str",   false, false, false, 0,  16, 8 },
    { AST_PRIM_I8,    "i8",    true,  true,  false, 8,  1,  1 },
    { AST_PRIM_I16,   "i16",   true,  true,  false, 16, 2,  2 },
    { AST_PRIM_I32,   "i32",   true,  true,  false, 32, 4,  4 },
    { AST_PRIM_I64,   "i64",   true,  true,  false, 64, 8,  8 },
    { AST_PRIM_U8,    "u8",    true,  false, false, 8,  1,  1 },
    { AST_PRIM_U16,   "u16",   true,  false, false, 16, 2,  2 },
    { AST_PRIM_U32,   "u32",   true,  false, false, 32, 4,  4 },
    { AST_PRIM_U64,   "u64",   true,  false, false, 64, 8,  8 },
    { AST_PRIM_ISIZE, "isize", true,  true,  true,  64, 8,  8 },
    { AST_PRIM_USIZE, "usize", true,  false, true,  64, 8,  8 },
};

static size_t kPrimsLen(void)
{
    return sizeof(kPrims) / sizeof(kPrims[0]);
}

const TypePrimInfo *types_prim_info(AstPrimKind kind)
{
    size_t i;
    for (i = 0; i < kPrimsLen(); i++) {
        if (kPrims[i].kind == kind) return &kPrims[i];
    }
    return NULL;
}

const TypePrimInfo *types_prim_by_name(const char *name)
{
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < kPrimsLen(); i++) {
        if (strcmp(kPrims[i].name, name) == 0) return &kPrims[i];
    }
    return NULL;
}

size_t types_prim_count(void)
{
    return kPrimsLen();
}

/* ---------------------------------------------------------------------------
 * Composite type table (spec sec. 7.2)
 * ------------------------------------------------------------------------- */

static const TypeCompositeInfo kComposites[] = {
    { TY_COMPOSITE_ARRAY,  "array",  "T[N]",
      "N * sizeof(T) bytes, alignment alignof(T)" },
    { TY_COMPOSITE_SLICE,  "slice",  "T[]",
      "16 bytes: pointer (offset 0), usize length (offset 8); alignment 8" },
    { TY_COMPOSITE_PTR,    "pointer", "T*",
      "8 bytes; alignment 8" },
    { TY_COMPOSITE_STRUCT, "struct", "struct S { ... }",
      "Section 7.4 (declaration order, alignment, zero padding)" },
    { TY_COMPOSITE_ENUM,   "enum",   "enum E: T { ... }",
      "same size/alignment as underlying type T" },
};

static size_t kCompositesLen(void)
{
    return sizeof(kComposites) / sizeof(kComposites[0]);
}

const TypeCompositeInfo *types_composite_info(TypeCompositeKind kind)
{
    size_t i;
    for (i = 0; i < kCompositesLen(); i++) {
        if (kComposites[i].kind == kind) return &kComposites[i];
    }
    return NULL;
}

size_t types_composite_count(void)
{
    return kCompositesLen();
}

/* ---------------------------------------------------------------------------
 * Completeness pass (spec sec. 7.6)
 * ------------------------------------------------------------------------- */

typedef struct TypeCtx {
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;

    /* NameSymbol pointers that are complete (closing brace processed). */
    const NameSymbol **closed;
    size_t nclosed, closed_cap;

    /* NameSymbol pointers used as a value anywhere in the build. */
    const NameSymbol **used;
    size_t nused, used_cap;

    /* Symbols that already received an incomplete/recursion record
     * (dedupe: one record per root cause, contract sec. 13). */
    const NameSymbol **reported;
    size_t nreported, reported_cap;
} TypeCtx;

static bool ptr_push(TypeCtx *c, const NameSymbol ***arr, size_t *n,
                     size_t *cap, const NameSymbol *sym)
{
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        const NameSymbol **na = (const NameSymbol **)realloc(
            (void *)*arr, ncap * sizeof(const NameSymbol *));
        if (!na) { c->oom = true; return false; }
        *arr = na;
        *cap = ncap;
    }
    (*arr)[(*n)++] = sym;
    return true;
}

static bool set_add(TypeCtx *c, const NameSymbol ***arr, size_t *n,
                    size_t *cap, const NameSymbol *sym)
{
    size_t i;
    for (i = 0; i < *n; i++) {
        if ((*arr)[i] == sym) return true;   /* already present */
    }
    return ptr_push(c, arr, n, cap, sym);
}

static bool contains(const NameSymbol *const *arr, size_t n,
                     const NameSymbol *sym)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (arr[i] == sym) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Record creation (all completeness records: phase "type", severity
 * "error", recovery "authoritative")
 * ------------------------------------------------------------------------- */

static DiagRecord *new_type_record(TypeCtx *c, const char *code,
                                   const char *message,
                                   const DiagSpan *primary)
{
    DiagRecord *r = diag_record_new();
    if (!r) { c->oom = true; return NULL; }
    if (!diag_record_set_code(r, code)) {
        diag_record_free(r); c->oom = true; return NULL;
    }
    if (!diag_record_set_message(r, message)) {
        diag_record_free(r); c->oom = true; return NULL;
    }
    if (!diag_record_set_primary_span(r, primary)) {
        diag_record_free(r); c->oom = true; return NULL;
    }
    if (!diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r); c->oom = true; return NULL;
    }
    return r;
}

static bool rec_push(TypeCtx *c, DiagRecord *r)
{
    if (c->nrecords == c->records_cap) {
        size_t ncap = c->records_cap ? c->records_cap * 2 : 16;
        DiagRecord **nr = (DiagRecord **)realloc(
            c->records, ncap * sizeof(DiagRecord *));
        if (!nr) { c->oom = true; return false; }
        c->records = nr;
        c->records_cap = ncap;
    }
    c->records[c->nrecords++] = r;
    return true;
}

/* ---------------------------------------------------------------------------
 * Declaration-name span
 * ------------------------------------------------------------------------- */

/* Skip whitespace and comments starting at offset `pos` in `src`. */
static int64_t skip_trivia(const LoadSource *src, int64_t pos)
{
    const char *t = src->text;
    int64_t len = (int64_t)src->len;
    while (pos < len) {
        unsigned char ch = (unsigned char)t[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            pos++;
        } else if (ch == '/' && pos + 1 < len && t[pos + 1] == '/') {
            while (pos < len && t[pos] != '\n') pos++;
        } else if (ch == '/' && pos + 1 < len && t[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) {
                pos++;
            }
            if (pos + 1 < len) pos += 2;
            else pos = len;
        } else {
            break;
        }
    }
    return pos;
}

/* End offset (exclusive) of the identifier starting at `pos`; -1 when the
 * byte at `pos` does not start an identifier. */
static int64_t ident_end_at(const LoadSource *src, int64_t pos)
{
    const char *t = src->text;
    int64_t len = (int64_t)src->len;
    if (pos >= len) return -1;
    unsigned char ch = (unsigned char)t[pos];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')) {
        return -1;
    }
    while (pos < len) {
        ch = (unsigned char)t[pos];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            break;
        }
        pos++;
    }
    return pos;
}

/* The declaration-name span of a struct declaration (the identifier after
 * the `struct` keyword). The AST stores the whole declaration span but not
 * the name span; we locate the identifier in the normalized source.
 * Returns NULL on failure (malformed source / OOM); the caller skips the
 * record when the span cannot be computed (defensive: the source was
 * already validated and parsed, so this should not happen). */
static DiagSpan *struct_name_span(TypeCtx *c, const LoadSource *src,
                                  const AstNode *decl)
{
    static const char kStruct[] = "struct";
    int64_t pos = decl->span ? decl->span->start.offset : 0;
    int64_t len = (int64_t)src->len;
    (void)c;
    if (pos + (int64_t)(sizeof(kStruct) - 1) > len ||
        memcmp(src->text + pos, kStruct, sizeof(kStruct) - 1) != 0) {
        return NULL;
    }
    pos += (int64_t)(sizeof(kStruct) - 1);
    pos = skip_trivia(src, pos);
    int64_t end = ident_end_at(src, pos);
    if (end < 0) return NULL;

    DiagSpan *sp = NULL, *ep = NULL;
    if (!diag_span_from_offset(src->file, src->text, src->len, pos, &sp) ||
        !diag_span_from_offset(src->file, src->text, src->len, end, &ep)) {
        diag_span_free(sp);
        diag_span_free(ep);
        return NULL;
    }
    DiagSpan *range = diag_span_new_range(
        src->file,
        sp->start.line, sp->start.col, sp->start.offset,
        ep->start.line, ep->start.col, ep->start.offset);
    diag_span_free(sp);
    diag_span_free(ep);
    return range;
}

/* ---------------------------------------------------------------------------
 * Struct symbol lookup
 * ------------------------------------------------------------------------- */

/* The module-scope struct symbol whose declaration node is `decl`, or NULL. */
static const NameSymbol *struct_symbol_for_decl(const NameModule *module,
                                                const AstNode *decl)
{
    size_t i;
    for (i = 0; i < module->nmodule_scope; i++) {
        const NameSymbol *s = module->module_scope[i];
        if (s->kind == NAME_SYM_STRUCT && s->decl == decl) return s;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Value-use scan (spec sec. 7.6 selection; corpus-pinned)
 * ------------------------------------------------------------------------- */

/* Mark `sym` as used as a value if it is a struct symbol. */
static void mark_struct_use(TypeCtx *c, const NameSymbol *sym)
{
    if (sym && sym->kind == NAME_SYM_STRUCT) {
        set_add(c, &c->used, &c->nused, &c->used_cap, sym);
    }
}

static void scan_type_value_use(TypeCtx *c, const NameModule *module,
                                const AstNode *type);

static void scan_expr_value_use(TypeCtx *c, const NameModule *module,
                                const AstNode *expr);

static void scan_stmt_value_use(TypeCtx *c, const NameModule *module,
                                const AstNode *stmt);

/* A type in a value position: named structs reachable through value
 * positions (the type itself, array/slice element) count as value uses;
 * pointers stop the walk (pointer to incomplete is permitted). */
static void scan_type_value_use(TypeCtx *c, const NameModule *module,
                                const AstNode *type)
{
    if (!type || c->oom) return;
    switch (type->kind) {
    case AST_TYPE_NAMED:
        mark_struct_use(c, name_symbol_for_node(module, type));
        return;
    case AST_TYPE_ARRAY:
    case AST_TYPE_SLICE:
        scan_type_value_use(c, module, type->u.type_derived.base);
        return;
    case AST_TYPE_PTR:
    case AST_TYPE_PRIM:
    default:
        return;
    }
}

static void scan_expr_value_use(TypeCtx *c, const NameModule *module,
                                const AstNode *expr)
{
    if (!expr || c->oom) return;
    switch (expr->kind) {
    case AST_EXPR_STRUCT_INIT:
        mark_struct_use(c, name_symbol_for_node(module,
                                                expr->u.struct_init.base));
        scan_expr_value_use(c, module, expr->u.struct_init.base);
        for (size_t i = 0; i < expr->u.struct_init.nfields; i++) {
            const AstNode *fi = expr->u.struct_init.fields[i];
            scan_expr_value_use(c, module,
                                fi ? fi->u.named.value : NULL);
        }
        return;
    case AST_EXPR_ARRAY_LITERAL:
        for (size_t i = 0; i < expr->u.array_literal.nelems; i++) {
            scan_expr_value_use(c, module, expr->u.array_literal.elems[i]);
        }
        scan_expr_value_use(c, module, expr->u.array_literal.count);
        return;
    case AST_EXPR_PAREN:
        scan_expr_value_use(c, module, expr->u.paren.expr);
        return;
    case AST_EXPR_UNARY:
        scan_expr_value_use(c, module, expr->u.unary.operand);
        return;
    case AST_EXPR_BINARY:
        scan_expr_value_use(c, module, expr->u.binary.lhs);
        scan_expr_value_use(c, module, expr->u.binary.rhs);
        return;
    case AST_EXPR_ASSIGN:
        scan_expr_value_use(c, module, expr->u.assign.target);
        scan_expr_value_use(c, module, expr->u.assign.value);
        return;
    case AST_EXPR_TERNARY:
        scan_expr_value_use(c, module, expr->u.branch.cond);
        scan_expr_value_use(c, module, expr->u.branch.then);
        scan_expr_value_use(c, module, expr->u.branch.els);
        return;
    case AST_EXPR_INDEX:
        scan_expr_value_use(c, module, expr->u.index_slice.base);
        scan_expr_value_use(c, module, expr->u.index_slice.index);
        return;
    case AST_EXPR_SLICE:
        scan_expr_value_use(c, module, expr->u.index_slice.base);
        scan_expr_value_use(c, module, expr->u.index_slice.lo);
        scan_expr_value_use(c, module, expr->u.index_slice.hi);
        return;
    case AST_EXPR_CALL:
        scan_expr_value_use(c, module, expr->u.call.callee);
        for (size_t i = 0; i < expr->u.call.nargs; i++) {
            scan_expr_value_use(c, module, expr->u.call.args[i]);
        }
        return;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        scan_expr_value_use(c, module, expr->u.member.base);
        return;
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        /* casting to a struct type forms a value of that type */
        scan_type_value_use(c, module, expr->u.cast_wrap.type);
        scan_expr_value_use(c, module, expr->u.cast_wrap.expr);
        return;
    case AST_EXPR_SIZEOF_EXPR:
        scan_expr_value_use(c, module, expr->u.size_op.operand);
        return;
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF:
        /* type queries do not form values */
        return;
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        scan_expr_value_use(c, module, expr->u.size_op.operand);
        return;
    case AST_EXPR_IDENT:
    case AST_EXPR_INT_LITERAL:
    case AST_EXPR_STR_LITERAL:
    case AST_EXPR_BOOL_LITERAL:
    case AST_EXPR_NULL_LITERAL:
    default:
        return;
    }
}

static void scan_stmt_value_use(TypeCtx *c, const NameModule *module,
                                const AstNode *stmt)
{
    if (!stmt || c->oom) return;
    switch (stmt->kind) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->u.list.count; i++) {
            scan_stmt_value_use(c, module, stmt->u.list.items[i]);
        }
        return;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        scan_type_value_use(c, module, stmt->u.local_decl.type);
        scan_expr_value_use(c, module, stmt->u.local_decl.init);
        return;
    case AST_IF:
        scan_expr_value_use(c, module, stmt->u.branch.cond);
        scan_stmt_value_use(c, module, stmt->u.branch.then);
        scan_stmt_value_use(c, module, stmt->u.branch.els);
        return;
    case AST_WHILE:
        scan_expr_value_use(c, module, stmt->u.while_loop.cond);
        scan_stmt_value_use(c, module, stmt->u.while_loop.body);
        return;
    case AST_FOR:
        scan_stmt_value_use(c, module, stmt->u.for_loop.init);
        scan_expr_value_use(c, module, stmt->u.for_loop.cond);
        scan_expr_value_use(c, module, stmt->u.for_loop.step);
        scan_stmt_value_use(c, module, stmt->u.for_loop.body);
        return;
    case AST_SWITCH:
        scan_expr_value_use(c, module, stmt->u.switch_stmt.selector);
        for (size_t i = 0; i < stmt->u.switch_stmt.ncases; i++) {
            scan_stmt_value_use(c, module, stmt->u.switch_stmt.cases[i]);
        }
        return;
    case AST_CASE_CLAUSE:
        scan_expr_value_use(c, module, stmt->u.clause.value);
        scan_stmt_value_use(c, module, stmt->u.clause.body);
        return;
    case AST_DEFAULT_CLAUSE:
        scan_stmt_value_use(c, module, stmt->u.clause.body);
        return;
    case AST_RETURN:
        scan_expr_value_use(c, module, stmt->u.ret.value);
        return;
    case AST_EXPR_STMT:
        scan_expr_value_use(c, module, stmt->u.expr_stmt.expr);
        return;
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_EMPTY_STMT:
    default:
        return;
    }
}

/* Scan one module for struct types used as values (var/const/global decl
 * types, fn params/returns, struct literals and casts in bodies). */
static void scan_module_value_uses(TypeCtx *c, const NameModule *module)
{
    const AstNode *program = module->program;
    if (!program) return;
    for (size_t i = 0; i < program->u.program.ndecls; i++) {
        const AstNode *decl = program->u.program.decls[i];
        switch (decl->kind) {
        case AST_GLOBAL_VAR_DECL:
        case AST_GLOBAL_CONST_DECL:
            scan_type_value_use(c, module, decl->u.global_decl.type);
            scan_expr_value_use(c, module, decl->u.global_decl.init);
            break;
        case AST_FN_DECL:
            for (size_t p = 0; p < decl->u.fn_decl.nparams; p++) {
                scan_type_value_use(c, module,
                                    decl->u.fn_decl.params[p]->u.named.type);
            }
            scan_type_value_use(c, module, decl->u.fn_decl.ret_type);
            scan_stmt_value_use(c, module, decl->u.fn_decl.body);
            break;
        case AST_STRUCT_DECL:
        case AST_ENUM_DECL:
        case AST_MODULE_DECL:
        case AST_IMPORT_DECL:
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Field-type walk (recursion + incomplete field detection)
 * ------------------------------------------------------------------------- */

/* Walk a field type at value depth. Returns true when the walk reaches
 * `self` (by-value recursion). Emits AIC-T0302 when the walk reaches a
 * DIFFERENT struct that is still incomplete (forming a field of an
 * incomplete struct type, spec sec. 7.6). Pointers stop the walk. */
static bool walk_field_type(TypeCtx *c, const NameModule *module,
                            const AstNode *type, const NameSymbol *self)
{
    if (!type || c->oom) return false;
    switch (type->kind) {
    case AST_TYPE_NAMED: {
        const NameSymbol *sym = name_symbol_for_node(module, type);
        if (!sym || sym->kind != NAME_SYM_STRUCT) return false;
        if (sym == self) return true;   /* recursion by value */
        if (!contains(c->closed, c->nclosed, sym)) {
            /* forming a field of an incomplete struct type (sec. 7.6) */
            if (!contains(c->reported, c->nreported, sym)) {
                char msg[256];
                const char *name = sym->name ? sym->name : "?";
                snprintf(msg, sizeof(msg),
                         "use of incomplete struct type '%s' as a value",
                         name);
                DiagSpan *span = NULL;
                if (sym->decl && sym->module && sym->module->src) {
                    span = struct_name_span(c, sym->module->src, sym->decl);
                }
                DiagRecord *r = new_type_record(c, "AIC-T0302", msg, span);
                diag_span_free(span);
                if (r) {
                    if (!rec_push(c, r)) diag_record_free(r);
                    else set_add(c, &c->reported, &c->nreported,
                                 &c->reported_cap, sym);
                }
            }
        }
        return false;
    }
    case AST_TYPE_ARRAY:
        return walk_field_type(c, module, type->u.type_derived.base, self);
    case AST_TYPE_SLICE:
        return walk_field_type(c, module, type->u.type_derived.base, self);
    case AST_TYPE_PTR:
    case AST_TYPE_PRIM:
    default:
        return false;
    }
}

/* Emit the completeness record for one struct declaration (corpus-pinned
 * selection): a struct that recurses by value and is also used as a value
 * anywhere gets AIC-T0302 at its declaration-name span; a recursive
 * struct that is never used as a value gets AIC-T0303. */
static void emit_struct_completeness(TypeCtx *c, const NameModule *module,
                                     const AstNode *decl,
                                     const NameSymbol *sym,
                                     bool has_recursion,
                                     const char *recursive_field)
{
    if (!has_recursion || !sym || c->oom) return;
    if (contains(c->reported, c->nreported, sym)) return;
    bool used = contains(c->used, c->nused, sym);
    const char *name = sym->name ? sym->name : "?";
    char msg[256];
    const char *code = used ? "AIC-T0302" : "AIC-T0303";
    if (used) {
        snprintf(msg, sizeof(msg),
                 "use of incomplete struct type '%s' as a value", name);
    } else {
        snprintf(msg, sizeof(msg),
                 "struct '%s' has infinite size due to recursive by-value "
                 "field '%s'", name,
                 recursive_field ? recursive_field : "?");
    }
    DiagSpan *span = NULL;
    if (module->src) {
        span = struct_name_span(c, module->src, decl);
    }
    DiagRecord *r = new_type_record(c, code, msg, span);
    diag_span_free(span);
    if (!r) return;
    if (!rec_push(c, r)) {
        diag_record_free(r);
    } else {
        set_add(c, &c->reported, &c->nreported, &c->reported_cap, sym);
    }
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

TypeCheckStatus types_check_completeness(const NameResult *result,
                                         DiagRecord ***out_records,
                                         size_t *out_record_count)
{
    TypeCtx c;
    memset(&c, 0, sizeof(c));
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    if (!result) return TYPE_CHECK_OK;

    /* Pass 1: collect value uses across the whole build (order-free). */
    for (size_t m = 0; m < result->nmodules; m++) {
        scan_module_value_uses(&c, result->modules[m]);
        if (c.oom) goto oom;
    }

    /* Pass 2: process struct declarations in deterministic order (entry
     * module first, then imports in depth-first order; within a module,
     * top-level declarations in source order). A struct is complete once
     * its own declaration has been processed. */
    for (size_t m = 0; m < result->nmodules; m++) {
        const NameModule *module = result->modules[m];
        const AstNode *program = module->program;
        if (!program) continue;
        for (size_t i = 0; i < program->u.program.ndecls; i++) {
            const AstNode *decl = program->u.program.decls[i];
            if (decl->kind != AST_STRUCT_DECL) continue;
            const NameSymbol *sym = struct_symbol_for_decl(module, decl);
            if (!sym) continue;   /* defensive: name phase guarantees one */

            bool has_recursion = false;
            const char *recursive_field = NULL;
            for (size_t f = 0; f < decl->u.struct_decl.nfields; f++) {
                const AstNode *field = decl->u.struct_decl.fields[f];
                if (!field) continue;
                if (walk_field_type(&c, module, field->u.named.type, sym)) {
                    if (!has_recursion) {
                        has_recursion = true;
                        recursive_field = field->u.named.name;
                    }
                }
                if (c.oom) goto oom;
            }
            emit_struct_completeness(&c, module, decl, sym,
                                     has_recursion, recursive_field);
            if (c.oom) goto oom;

            /* closing brace processed: struct is now complete */
            if (!set_add(&c, &c.closed, &c.nclosed, &c.closed_cap, sym)) {
                goto oom;
            }
        }
    }

    if (c.oom) goto oom;

    if (c.nrecords > 0) {
        diag_sort_records(c.records, c.nrecords);
    }
    if (out_records) *out_records = c.records;
    if (out_record_count) *out_record_count = c.nrecords;
    free((void *)c.closed);
    free((void *)c.used);
    free((void *)c.reported);
    return c.nrecords > 0 ? TYPE_CHECK_DIAG_ERROR : TYPE_CHECK_OK;

oom:
    for (size_t i = 0; i < c.nrecords; i++) diag_record_free(c.records[i]);
    free(c.records);
    free((void *)c.closed);
    free((void *)c.used);
    free((void *)c.reported);
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;
    return TYPE_CHECK_OOM;
}

void types_records_free(DiagRecord **records, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) diag_record_free(records[i]);
    free(records);
}
