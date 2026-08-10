/* bootstrap/src/ast/ast.c
 *
 * AI-Co Stage-0 AST ownership and deterministic dump (WP-M0-09).
 *
 * The dump is the package's deterministic textual oracle: one line per node
 * with a field label, the node kind, key attributes, and the source span as
 * [start_offset,end_offset). The rendering is pure function of the tree:
 * no addresses, host paths, or timestamps. The parser's golden tests
 * compare dumps byte-for-byte.
 */
#include "ast.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Small allocation helpers
 * ------------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    size_t len;
    char *p;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    p = (char *)malloc(len + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

/* ---------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

AstName *ast_name_new(const char *const *parts, size_t count)
{
    AstName *name;
    char **copy;
    size_t i;

    name = (AstName *)malloc(sizeof(AstName));
    if (name == NULL) {
        return NULL;
    }
    name->count = count;
    name->parts = NULL;
    if (count > 0) {
        copy = (char **)calloc(count, sizeof(char *));
        if (copy == NULL) {
            free(name);
            return NULL;
        }
        name->parts = copy;
        for (i = 0; i < count; ++i) {
            copy[i] = dup_str(parts[i]);
            if (copy[i] == NULL) {
                size_t j;
                for (j = 0; j < i; ++j) {
                    free(copy[j]);
                }
                free(copy);
                free(name);
                return NULL;
            }
        }
    }
    return name;
}

void ast_name_free(AstName *name)
{
    size_t i;

    if (name == NULL) {
        return;
    }
    for (i = 0; i < name->count; ++i) {
        free(name->parts[i]);
    }
    free(name->parts);
    free(name);
}

char *ast_name_to_string(const AstName *name)
{
    size_t total = 0;
    size_t i;
    char *out;
    char *p;

    if (name == NULL) {
        return dup_str("");
    }
    for (i = 0; i < name->count; ++i) {
        total += strlen(name->parts[i]) + (i + 1 < name->count ? 1 : 0);
    }
    out = (char *)malloc(total + 1);
    if (out == NULL) {
        return NULL;
    }
    p = out;
    for (i = 0; i < name->count; ++i) {
        size_t n = strlen(name->parts[i]);
        memcpy(p, name->parts[i], n);
        p += n;
        if (i + 1 < name->count) {
            *p++ = '.';
        }
    }
    *p = '\0';
    return out;
}

int ast_prim_from_keyword(LexKeyword kw)
{
    switch (kw) {
    case KW_VOID:  return AST_PRIM_VOID;
    case KW_BOOL:  return AST_PRIM_BOOL;
    case KW_STR:   return AST_PRIM_STR;
    case KW_I8:    return AST_PRIM_I8;
    case KW_I16:   return AST_PRIM_I16;
    case KW_I32:   return AST_PRIM_I32;
    case KW_I64:   return AST_PRIM_I64;
    case KW_U8:    return AST_PRIM_U8;
    case KW_U16:   return AST_PRIM_U16;
    case KW_U32:   return AST_PRIM_U32;
    case KW_U64:   return AST_PRIM_U64;
    case KW_ISIZE: return AST_PRIM_ISIZE;
    case KW_USIZE: return AST_PRIM_USIZE;
    default:       return -1;
    }
}

/* ---------------------------------------------------------------------------
 * Dump buffer
 * ------------------------------------------------------------------------- */

void ast_dump_init(AstDumpBuf *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->oom = false;
}

void ast_dump_free(AstDumpBuf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->oom = false;
}

bool ast_dump_ok(const AstDumpBuf *buf)
{
    return !buf->oom;
}

static bool dump_reserve(AstDumpBuf *b, size_t extra)
{
    size_t need;
    size_t ncap;
    char *nd;

    if (b->oom) {
        return false;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) {
        return true;
    }
    ncap = b->cap == 0 ? 256 : b->cap;
    while (ncap < need) {
        ncap *= 2;
    }
    nd = (char *)realloc(b->data, ncap);
    if (nd == NULL) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void dump_putc(AstDumpBuf *b, char c)
{
    if (!dump_reserve(b, 1)) {
        return;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void dump_puts(AstDumpBuf *b, const char *s)
{
    size_t n = strlen(s);

    if (!dump_reserve(b, n)) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void dump_printf(AstDumpBuf *b, const char *fmt, ...)
{
    va_list ap;
    char small[256];
    char *buf = small;
    va_list ap2;
    int n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = true;
        va_end(ap2);
        return;
    }
    if ((size_t)n >= sizeof(small)) {
        buf = (char *)malloc((size_t)n + 1);
        if (buf == NULL) {
            b->oom = true;
            va_end(ap2);
            return;
        }
        vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    dump_puts(b, buf);
    if (buf != small) {
        free(buf);
    }
}

/* ---------------------------------------------------------------------------
 * Text tables
 * ------------------------------------------------------------------------- */

static const char *kind_text(AstNodeKind k)
{
    switch (k) {
    case AST_PROGRAM:              return "program";
    case AST_MODULE_DECL:          return "module_decl";
    case AST_IMPORT_DECL:          return "import_decl";
    case AST_STRUCT_DECL:          return "struct_decl";
    case AST_ENUM_DECL:            return "enum_decl";
    case AST_FN_DECL:              return "fn_decl";
    case AST_GLOBAL_VAR_DECL:      return "global_var_decl";
    case AST_GLOBAL_CONST_DECL:    return "global_const_decl";
    case AST_FIELD_DECL:           return "field_decl";
    case AST_ENUM_MEMBER:          return "enum_member";
    case AST_PARAM:                return "param";
    case AST_BLOCK:                return "block";
    case AST_VAR_DECL:             return "var_decl";
    case AST_CONST_DECL:           return "const_decl";
    case AST_IF:                   return "if";
    case AST_WHILE:                return "while";
    case AST_FOR:                  return "for";
    case AST_SWITCH:               return "switch";
    case AST_CASE_CLAUSE:          return "case_clause";
    case AST_DEFAULT_CLAUSE:       return "default_clause";
    case AST_BREAK:                return "break";
    case AST_CONTINUE:             return "continue";
    case AST_RETURN:               return "return";
    case AST_EXPR_STMT:            return "expr_stmt";
    case AST_EMPTY_STMT:           return "empty_stmt";
    case AST_TYPE_PRIM:            return "type_prim";
    case AST_TYPE_NAMED:           return "type_named";
    case AST_TYPE_PTR:             return "type_ptr";
    case AST_TYPE_ARRAY:           return "type_array";
    case AST_TYPE_SLICE:           return "type_slice";
    case AST_EXPR_INT_LITERAL:     return "expr_int_literal";
    case AST_EXPR_STR_LITERAL:     return "expr_str_literal";
    case AST_EXPR_BOOL_LITERAL:    return "expr_bool_literal";
    case AST_EXPR_NULL_LITERAL:    return "expr_null_literal";
    case AST_EXPR_IDENT:           return "expr_ident";
    case AST_EXPR_ARRAY_LITERAL:   return "expr_array_literal";
    case AST_EXPR_PAREN:           return "expr_paren";
    case AST_EXPR_UNARY:           return "expr_unary";
    case AST_EXPR_BINARY:          return "expr_binary";
    case AST_EXPR_ASSIGN:          return "expr_assign";
    case AST_EXPR_TERNARY:         return "expr_ternary";
    case AST_EXPR_INDEX:           return "expr_index";
    case AST_EXPR_SLICE:           return "expr_slice";
    case AST_EXPR_CALL:            return "expr_call";
    case AST_EXPR_MEMBER:          return "expr_member";
    case AST_EXPR_ARROW:           return "expr_arrow";
    case AST_EXPR_STRUCT_INIT:     return "expr_struct_init";
    case AST_FIELD_INIT:           return "field_init";
    case AST_EXPR_SIZEOF_TYPE:     return "expr_sizeof_type";
    case AST_EXPR_SIZEOF_EXPR:     return "expr_sizeof_expr";
    case AST_EXPR_ALIGNOF:         return "expr_alignof";
    case AST_EXPR_CAST:            return "expr_cast";
    case AST_EXPR_WRAP:            return "expr_wrap";
    case AST_EXPR_LEN:             return "expr_len";
    case AST_EXPR_PTR:             return "expr_ptr";
    }
    return "?";
}

static const char *prim_text(AstPrimKind p)
{
    switch (p) {
    case AST_PRIM_VOID:  return "void";
    case AST_PRIM_BOOL:  return "bool";
    case AST_PRIM_STR:   return "str";
    case AST_PRIM_I8:    return "i8";
    case AST_PRIM_I16:   return "i16";
    case AST_PRIM_I32:   return "i32";
    case AST_PRIM_I64:   return "i64";
    case AST_PRIM_U8:    return "u8";
    case AST_PRIM_U16:   return "u16";
    case AST_PRIM_U32:   return "u32";
    case AST_PRIM_U64:   return "u64";
    case AST_PRIM_ISIZE: return "isize";
    case AST_PRIM_USIZE: return "usize";
    }
    return "?";
}

static const char *unary_text(AstUnaryOp op)
{
    switch (op) {
    case AST_UN_NEG:   return "-";
    case AST_UN_PLUS:  return "+";
    case AST_UN_NOT:   return "!";
    case AST_UN_BNOT:  return "~";
    case AST_UN_DEREF: return "*";
    case AST_UN_ADDR:  return "&";
    }
    return "?";
}

static const char *binary_text(AstBinaryOp op)
{
    switch (op) {
    case AST_BIN_MUL:   return "*";
    case AST_BIN_DIV:   return "/";
    case AST_BIN_MOD:   return "%";
    case AST_BIN_ADD:   return "+";
    case AST_BIN_SUB:   return "-";
    case AST_BIN_SHL:   return "<<";
    case AST_BIN_SHR:   return ">>";
    case AST_BIN_LT:    return "<";
    case AST_BIN_LE:    return "<=";
    case AST_BIN_GT:    return ">";
    case AST_BIN_GE:    return ">=";
    case AST_BIN_EQ:    return "==";
    case AST_BIN_NE:    return "!=";
    case AST_BIN_BAND:  return "&";
    case AST_BIN_BXOR:  return "^";
    case AST_BIN_BOR:   return "|";
    case AST_BIN_LAND:  return "&&";
    case AST_BIN_LOR:   return "||";
    }
    return "?";
}

static const char *assign_text(AstAssignOp op)
{
    switch (op) {
    case AST_ASGN_ASSIGN: return "=";
    case AST_ASGN_ADD:    return "+=";
    case AST_ASGN_SUB:    return "-=";
    case AST_ASGN_MUL:    return "*=";
    case AST_ASGN_DIV:    return "/=";
    case AST_ASGN_MOD:    return "%=";
    case AST_ASGN_SHL:    return "<<=";
    case AST_ASGN_SHR:    return ">>=";
    case AST_ASGN_BAND:   return "&=";
    case AST_ASGN_BOR:    return "|=";
    case AST_ASGN_BXOR:   return "^=";
    }
    return "?";
}

static void dump_str_bytes(AstDumpBuf *b, const char *bytes, size_t len)
{
    static const char hexd[] = "0123456789abcdef";
    size_t i;
    unsigned char c;

    dump_putc(b, '"');
    for (i = 0; i < len; ++i) {
        c = (unsigned char)bytes[i];
        switch (c) {
        case '"':  dump_puts(b, "\\\""); break;
        case '\\': dump_puts(b, "\\\\"); break;
        case '\n': dump_puts(b, "\\n"); break;
        case '\r': dump_puts(b, "\\r"); break;
        case '\t': dump_puts(b, "\\t"); break;
        case '\0': dump_puts(b, "\\0"); break;
        default:
            if (c < 0x20) {
                char esc[4];
                esc[0] = '\\';
                esc[1] = 'x';
                esc[2] = hexd[(c >> 4) & 0xF];
                esc[3] = hexd[c & 0xF];
                dump_puts(b, esc);
            } else {
                dump_putc(b, (char)c);
            }
            break;
        }
    }
    dump_putc(b, '"');
}

/* ---------------------------------------------------------------------------
 * Dump
 * ------------------------------------------------------------------------- */

static void dump_span(AstDumpBuf *b, const AstNode *n)
{
    if (n->span != NULL) {
        dump_printf(b, " [%lld,%lld)",
                    (long long)n->span->start.offset,
                    (long long)n->span->end.offset);
    } else {
        dump_puts(b, " [?,?)");
    }
}

static void dump_indent(AstDumpBuf *b, int depth)
{
    int i;

    for (i = 0; i < depth; ++i) {
        dump_puts(b, "  ");
    }
}

static void dump_name_attr(AstDumpBuf *b, const char *label,
                           const AstName *name)
{
    char *s = ast_name_to_string(name);

    dump_printf(b, " %s=", label);
    if (s != NULL) {
        dump_puts(b, s);
        free(s);
    } else {
        b->oom = true;
    }
}

static void dump_children(AstDumpBuf *b, const AstNode *n, int depth);

static void dump_node(AstDumpBuf *b, const AstNode *n, int depth,
                      const char *field)
{
    dump_indent(b, depth);
    if (field != NULL) {
        dump_puts(b, field);
        dump_putc(b, ' ');
    }
    dump_puts(b, kind_text(n->kind));

    switch (n->kind) {
    case AST_MODULE_DECL:
    case AST_IMPORT_DECL:
        dump_name_attr(b, "name", n->u.qname.name);
        break;
    case AST_STRUCT_DECL:
        dump_printf(b, " name=%s pub=%s", n->u.struct_decl.name,
                    n->u.struct_decl.is_pub ? "true" : "false");
        break;
    case AST_ENUM_DECL:
        dump_printf(b, " name=%s pub=%s", n->u.enum_decl.name,
                    n->u.enum_decl.is_pub ? "true" : "false");
        break;
    case AST_FN_DECL:
        dump_printf(b, " name=%s pub=%s", n->u.fn_decl.name,
                    n->u.fn_decl.is_pub ? "true" : "false");
        break;
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        dump_printf(b, " name=%s pub=%s", n->u.global_decl.name,
                    n->u.global_decl.is_pub ? "true" : "false");
        break;
    case AST_FIELD_DECL:
    case AST_ENUM_MEMBER:
    case AST_PARAM:
    case AST_FIELD_INIT:
        dump_printf(b, " name=%s", n->u.named.name);
        break;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        dump_printf(b, " name=%s", n->u.local_decl.name);
        break;
    case AST_TYPE_PRIM:
        dump_printf(b, " name=%s", prim_text(n->u.type_prim.prim));
        break;
    case AST_TYPE_NAMED:
        dump_name_attr(b, "name", n->u.type_named.name);
        break;
    case AST_EXPR_INT_LITERAL:
        dump_printf(b, " type=%s value=%llu",
                    lex_int_type_text(n->u.int_literal.type),
                    (unsigned long long)n->u.int_literal.value);
        if (n->u.int_literal.is_min) {
            dump_puts(b, " min=true");
        }
        break;
    case AST_EXPR_STR_LITERAL:
        dump_printf(b, " len=%llu bytes=",
                    (unsigned long long)n->u.str_literal.len);
        dump_str_bytes(b, n->u.str_literal.bytes, n->u.str_literal.len);
        break;
    case AST_EXPR_BOOL_LITERAL:
        dump_puts(b, n->u.bool_literal.value ? " value=true" : " value=false");
        break;
    case AST_EXPR_IDENT:
        dump_printf(b, " name=%s", n->u.ident.name);
        break;
    case AST_EXPR_ARRAY_LITERAL:
        dump_puts(b, n->u.array_literal.count != NULL ? " repeat=true"
                                                      : " repeat=false");
        break;
    case AST_EXPR_UNARY:
        dump_printf(b, " op=%s", unary_text(n->u.unary.op));
        break;
    case AST_EXPR_BINARY:
        dump_printf(b, " op=%s", binary_text(n->u.binary.op));
        break;
    case AST_EXPR_ASSIGN:
        dump_printf(b, " op=%s", assign_text(n->u.assign.op));
        break;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        dump_printf(b, " name=%s", n->u.member.name);
        break;
    case AST_EXPR_STRUCT_INIT:
        dump_printf(b, " nfields=%llu",
                    (unsigned long long)n->u.struct_init.nfields);
        break;
    case AST_EXPR_CALL:
        dump_printf(b, " nargs=%llu",
                    (unsigned long long)n->u.call.nargs);
        break;
    default:
        break;
    }

    dump_span(b, n);
    dump_putc(b, '\n');
    dump_children(b, n, depth + 1);
}

static void dump_children(AstDumpBuf *b, const AstNode *n, int depth)
{
    size_t i;

    switch (n->kind) {
    case AST_PROGRAM:
        if (n->u.program.module_decl != NULL) {
            dump_node(b, n->u.program.module_decl, depth, "module");
        }
        for (i = 0; i < n->u.program.nimports; ++i) {
            dump_node(b, n->u.program.imports[i], depth, "import");
        }
        for (i = 0; i < n->u.program.ndecls; ++i) {
            dump_node(b, n->u.program.decls[i], depth, "decl");
        }
        break;
    case AST_STRUCT_DECL:
        for (i = 0; i < n->u.struct_decl.nfields; ++i) {
            dump_node(b, n->u.struct_decl.fields[i], depth, "field");
        }
        break;
    case AST_ENUM_DECL:
        dump_node(b, n->u.enum_decl.underlying, depth, "underlying");
        for (i = 0; i < n->u.enum_decl.nmembers; ++i) {
            dump_node(b, n->u.enum_decl.members[i], depth, "member");
        }
        break;
    case AST_FN_DECL:
        for (i = 0; i < n->u.fn_decl.nparams; ++i) {
            dump_node(b, n->u.fn_decl.params[i], depth, "param");
        }
        dump_node(b, n->u.fn_decl.ret_type, depth, "ret_type");
        dump_node(b, n->u.fn_decl.body, depth, "body");
        break;
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        dump_node(b, n->u.global_decl.type, depth, "type");
        dump_node(b, n->u.global_decl.init, depth, "init");
        break;
    case AST_FIELD_DECL:
    case AST_PARAM:
        dump_node(b, n->u.named.type, depth, "type");
        break;
    case AST_ENUM_MEMBER:
        if (n->u.named.value != NULL) {
            dump_node(b, n->u.named.value, depth, "value");
        }
        break;
    case AST_FIELD_INIT:
        dump_node(b, n->u.named.value, depth, "value");
        break;
    case AST_BLOCK:
        for (i = 0; i < n->u.list.count; ++i) {
            dump_node(b, n->u.list.items[i], depth, "stmt");
        }
        break;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        dump_node(b, n->u.local_decl.type, depth, "type");
        dump_node(b, n->u.local_decl.init, depth, "init");
        break;
    case AST_IF:
        dump_node(b, n->u.branch.cond, depth, "cond");
        dump_node(b, n->u.branch.then, depth, "then");
        if (n->u.branch.els != NULL) {
            dump_node(b, n->u.branch.els, depth, "else");
        }
        break;
    case AST_WHILE:
        dump_node(b, n->u.while_loop.cond, depth, "cond");
        dump_node(b, n->u.while_loop.body, depth, "body");
        break;
    case AST_FOR:
        if (n->u.for_loop.init != NULL) {
            dump_node(b, n->u.for_loop.init, depth, "init");
        }
        if (n->u.for_loop.cond != NULL) {
            dump_node(b, n->u.for_loop.cond, depth, "cond");
        }
        if (n->u.for_loop.step != NULL) {
            dump_node(b, n->u.for_loop.step, depth, "step");
        }
        dump_node(b, n->u.for_loop.body, depth, "body");
        break;
    case AST_SWITCH:
        dump_node(b, n->u.switch_stmt.selector, depth, "selector");
        for (i = 0; i < n->u.switch_stmt.ncases; ++i) {
            dump_node(b, n->u.switch_stmt.cases[i], depth, "case");
        }
        break;
    case AST_CASE_CLAUSE:
        dump_node(b, n->u.clause.value, depth, "value");
        dump_node(b, n->u.clause.body, depth, "body");
        break;
    case AST_DEFAULT_CLAUSE:
        dump_node(b, n->u.clause.body, depth, "body");
        break;
    case AST_RETURN:
        if (n->u.ret.value != NULL) {
            dump_node(b, n->u.ret.value, depth, "value");
        }
        break;
    case AST_EXPR_STMT:
        dump_node(b, n->u.expr_stmt.expr, depth, "expr");
        break;
    case AST_TYPE_PTR:
        dump_node(b, n->u.type_derived.base, depth, "base");
        break;
    case AST_TYPE_ARRAY:
        dump_node(b, n->u.type_derived.base, depth, "elem");
        dump_node(b, n->u.type_derived.len, depth, "len");
        break;
    case AST_TYPE_SLICE:
        dump_node(b, n->u.type_derived.base, depth, "elem");
        break;
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < n->u.array_literal.nelems; ++i) {
            dump_node(b, n->u.array_literal.elems[i], depth, "elem");
        }
        if (n->u.array_literal.count != NULL) {
            dump_node(b, n->u.array_literal.count, depth, "count");
        }
        break;
    case AST_EXPR_PAREN:
        dump_node(b, n->u.paren.expr, depth, "expr");
        break;
    case AST_EXPR_UNARY:
        dump_node(b, n->u.unary.operand, depth, "operand");
        break;
    case AST_EXPR_BINARY:
        dump_node(b, n->u.binary.lhs, depth, "lhs");
        dump_node(b, n->u.binary.rhs, depth, "rhs");
        break;
    case AST_EXPR_ASSIGN:
        dump_node(b, n->u.assign.target, depth, "target");
        dump_node(b, n->u.assign.value, depth, "value");
        break;
    case AST_EXPR_TERNARY:
        dump_node(b, n->u.branch.cond, depth, "cond");
        dump_node(b, n->u.branch.then, depth, "then");
        dump_node(b, n->u.branch.els, depth, "else");
        break;
    case AST_EXPR_INDEX:
        dump_node(b, n->u.index_slice.base, depth, "base");
        dump_node(b, n->u.index_slice.index, depth, "index");
        break;
    case AST_EXPR_SLICE:
        dump_node(b, n->u.index_slice.base, depth, "base");
        if (n->u.index_slice.lo != NULL) {
            dump_node(b, n->u.index_slice.lo, depth, "lo");
        }
        if (n->u.index_slice.hi != NULL) {
            dump_node(b, n->u.index_slice.hi, depth, "hi");
        }
        break;
    case AST_EXPR_CALL:
        dump_node(b, n->u.call.callee, depth, "callee");
        for (i = 0; i < n->u.call.nargs; ++i) {
            dump_node(b, n->u.call.args[i], depth, "arg");
        }
        break;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        dump_node(b, n->u.member.base, depth, "base");
        break;
    case AST_EXPR_STRUCT_INIT:
        dump_node(b, n->u.struct_init.base, depth, "base");
        for (i = 0; i < n->u.struct_init.nfields; ++i) {
            dump_node(b, n->u.struct_init.fields[i], depth, "field");
        }
        break;
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF:
        dump_node(b, n->u.size_op.operand, depth, "type");
        break;
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        dump_node(b, n->u.size_op.operand, depth, "expr");
        break;
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        dump_node(b, n->u.cast_wrap.type, depth, "type");
        dump_node(b, n->u.cast_wrap.expr, depth, "expr");
        break;
    default:
        break;
    }
}

bool ast_dump(const AstNode *node, AstDumpBuf *out)
{
    if (node == NULL) {
        return true;
    }
    dump_node(out, node, 0, NULL);
    return ast_dump_ok(out);
}

/* ---------------------------------------------------------------------------
 * Ownership / free
 * ------------------------------------------------------------------------- */

void ast_node_free(AstNode *n)
{
    size_t i;

    if (n == NULL) {
        return;
    }
    switch (n->kind) {
    case AST_MODULE_DECL:
    case AST_IMPORT_DECL:
        ast_name_free(n->u.qname.name);
        break;
    case AST_STRUCT_DECL:
        free(n->u.struct_decl.name);
        for (i = 0; i < n->u.struct_decl.nfields; ++i) {
            ast_node_free(n->u.struct_decl.fields[i]);
        }
        free(n->u.struct_decl.fields);
        break;
    case AST_ENUM_DECL:
        free(n->u.enum_decl.name);
        ast_node_free(n->u.enum_decl.underlying);
        for (i = 0; i < n->u.enum_decl.nmembers; ++i) {
            ast_node_free(n->u.enum_decl.members[i]);
        }
        free(n->u.enum_decl.members);
        break;
    case AST_FN_DECL:
        free(n->u.fn_decl.name);
        for (i = 0; i < n->u.fn_decl.nparams; ++i) {
            ast_node_free(n->u.fn_decl.params[i]);
        }
        free(n->u.fn_decl.params);
        ast_node_free(n->u.fn_decl.ret_type);
        ast_node_free(n->u.fn_decl.body);
        break;
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        free(n->u.global_decl.name);
        ast_node_free(n->u.global_decl.type);
        ast_node_free(n->u.global_decl.init);
        break;
    case AST_FIELD_DECL:
    case AST_PARAM:
    case AST_ENUM_MEMBER:
    case AST_FIELD_INIT:
        free(n->u.named.name);
        ast_node_free(n->u.named.type);
        ast_node_free(n->u.named.value);
        break;
    case AST_BLOCK:
        for (i = 0; i < n->u.list.count; ++i) {
            ast_node_free(n->u.list.items[i]);
        }
        free(n->u.list.items);
        break;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        free(n->u.local_decl.name);
        ast_node_free(n->u.local_decl.type);
        ast_node_free(n->u.local_decl.init);
        break;
    case AST_IF:
    case AST_EXPR_TERNARY:
        ast_node_free(n->u.branch.cond);
        ast_node_free(n->u.branch.then);
        ast_node_free(n->u.branch.els);
        break;
    case AST_WHILE:
        ast_node_free(n->u.while_loop.cond);
        ast_node_free(n->u.while_loop.body);
        break;
    case AST_FOR:
        ast_node_free(n->u.for_loop.init);
        ast_node_free(n->u.for_loop.cond);
        ast_node_free(n->u.for_loop.step);
        ast_node_free(n->u.for_loop.body);
        break;
    case AST_SWITCH:
        ast_node_free(n->u.switch_stmt.selector);
        for (i = 0; i < n->u.switch_stmt.ncases; ++i) {
            ast_node_free(n->u.switch_stmt.cases[i]);
        }
        free(n->u.switch_stmt.cases);
        break;
    case AST_CASE_CLAUSE:
    case AST_DEFAULT_CLAUSE:
        ast_node_free(n->u.clause.value);
        ast_node_free(n->u.clause.body);
        break;
    case AST_RETURN:
        ast_node_free(n->u.ret.value);
        break;
    case AST_EXPR_PTR:
    case AST_EXPR_LEN:
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
        ast_node_free(n->u.size_op.operand);
        break;
    case AST_EXPR_STMT:
        ast_node_free(n->u.expr_stmt.expr);
        break;
    case AST_TYPE_NAMED:
        ast_name_free(n->u.type_named.name);
        break;
    case AST_TYPE_PTR:
    case AST_TYPE_ARRAY:
    case AST_TYPE_SLICE:
        ast_node_free(n->u.type_derived.base);
        ast_node_free(n->u.type_derived.len);
        break;
    case AST_EXPR_STR_LITERAL:
        free(n->u.str_literal.bytes);
        break;
    case AST_EXPR_IDENT:
        free(n->u.ident.name);
        break;
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < n->u.array_literal.nelems; ++i) {
            ast_node_free(n->u.array_literal.elems[i]);
        }
        free(n->u.array_literal.elems);
        ast_node_free(n->u.array_literal.count);
        break;
    case AST_EXPR_PAREN:
        ast_node_free(n->u.paren.expr);
        break;
    case AST_EXPR_UNARY:
        ast_node_free(n->u.unary.operand);
        break;
    case AST_EXPR_BINARY:
        ast_node_free(n->u.binary.lhs);
        ast_node_free(n->u.binary.rhs);
        break;
    case AST_EXPR_ASSIGN:
        ast_node_free(n->u.assign.target);
        ast_node_free(n->u.assign.value);
        break;
    case AST_EXPR_INDEX:
    case AST_EXPR_SLICE:
        ast_node_free(n->u.index_slice.base);
        ast_node_free(n->u.index_slice.index);
        ast_node_free(n->u.index_slice.lo);
        ast_node_free(n->u.index_slice.hi);
        break;
    case AST_EXPR_CALL:
        ast_node_free(n->u.call.callee);
        for (i = 0; i < n->u.call.nargs; ++i) {
            ast_node_free(n->u.call.args[i]);
        }
        free(n->u.call.args);
        break;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        ast_node_free(n->u.member.base);
        free(n->u.member.name);
        break;
    case AST_EXPR_STRUCT_INIT:
        ast_node_free(n->u.struct_init.base);
        for (i = 0; i < n->u.struct_init.nfields; ++i) {
            ast_node_free(n->u.struct_init.fields[i]);
        }
        free(n->u.struct_init.fields);
        break;
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        ast_node_free(n->u.cast_wrap.type);
        ast_node_free(n->u.cast_wrap.expr);
        break;
    default:
        break;
    }
    diag_span_free(n->span);
    free(n);
}
