/* bootstrap/src/types/type_identity.c
 *
 * AI-Co Stage-0 type representation and type identity (WP-M0-11a).
 * See type_identity.h for the model and ownership contract.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "type_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Constructors / destructor
 * ------------------------------------------------------------------------- */

Type *type_prim_new(AstPrimKind prim)
{
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = TYPE_PRIM;
    t->u.prim = prim;
    return t;
}

Type *type_array_new(Type *elem, int64_t extent)
{
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = TYPE_ARRAY;
    t->u.array.elem = elem;
    t->u.array.extent = extent;
    return t;
}

Type *type_slice_new(Type *elem)
{
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = TYPE_SLICE;
    t->u.slice.elem = elem;
    return t;
}

Type *type_ptr_new(Type *elem)
{
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = TYPE_PTR;
    t->u.ptr.elem = elem;
    return t;
}

Type *type_struct_new(const NameSymbol *sym)
{
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = TYPE_STRUCT;
    t->u.sym = sym;
    return t;
}

Type *type_enum_new(const NameSymbol *sym)
{
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = TYPE_ENUM;
    t->u.sym = sym;
    return t;
}

void type_free(Type *t)
{
    if (!t) return;
    switch (t->kind) {
    case TYPE_ARRAY:
        type_free(t->u.array.elem);
        break;
    case TYPE_SLICE:
        type_free(t->u.slice.elem);
        break;
    case TYPE_PTR:
        type_free(t->u.ptr.elem);
        break;
    case TYPE_PRIM:
    case TYPE_STRUCT:
    case TYPE_ENUM:
        break;   /* NameSymbol pointers are borrowed */
    }
    free(t);
}

/* ---------------------------------------------------------------------------
 * Identity (spec sec. 7.3)
 * ------------------------------------------------------------------------- */

bool type_identical(const Type *a, const Type *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case TYPE_PRIM:
        return a->u.prim == b->u.prim;
    case TYPE_ARRAY:
        return a->u.array.extent == b->u.array.extent &&
               type_identical(a->u.array.elem, b->u.array.elem);
    case TYPE_SLICE:
        return type_identical(a->u.slice.elem, b->u.slice.elem);
    case TYPE_PTR:
        return type_identical(a->u.ptr.elem, b->u.ptr.elem);
    case TYPE_STRUCT:
    case TYPE_ENUM:
        /* Same declaration (sec. 7.3): the name package guarantees one
         * NameSymbol per declaration within a build, so pointer equality
         * on the symbol IS same-declaration identity. */
        return a->u.sym == b->u.sym;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

const char *type_kind_text(TypeKind kind)
{
    switch (kind) {
    case TYPE_PRIM:   return "prim";
    case TYPE_ARRAY:  return "array";
    case TYPE_SLICE:  return "slice";
    case TYPE_PTR:    return "ptr";
    case TYPE_STRUCT: return "struct";
    case TYPE_ENUM:   return "enum";
    }
    return "?";
}

static const char *prim_name(AstPrimKind prim)
{
    switch (prim) {
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

/* Append a deterministic rendering of `t` into the growable buffer. */
static bool describe_into(char **buf, size_t *len, size_t *cap,
                          const char *text)
{
    size_t n = strlen(text);
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 32;
        while (ncap < *len + n + 1) ncap *= 2;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) return false;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, text, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static bool describe_type_into(char **buf, size_t *len, size_t *cap,
                               const Type *t)
{
    char tmp[64];
    switch (t->kind) {
    case TYPE_PRIM:
        return describe_into(buf, len, cap, prim_name(t->u.prim));
    case TYPE_ARRAY:
        if (!describe_type_into(buf, len, cap, t->u.array.elem)) return false;
        snprintf(tmp, sizeof(tmp), "[%lld]",
                 (long long)t->u.array.extent);
        return describe_into(buf, len, cap, tmp);
    case TYPE_SLICE:
        if (!describe_type_into(buf, len, cap, t->u.slice.elem)) return false;
        return describe_into(buf, len, cap, "[]");
    case TYPE_PTR:
        if (!describe_type_into(buf, len, cap, t->u.ptr.elem)) return false;
        return describe_into(buf, len, cap, "*");
    case TYPE_STRUCT:
    case TYPE_ENUM:
        if (t->u.sym && t->u.sym->name) {
            return describe_into(buf, len, cap, t->u.sym->name);
        }
        return describe_into(buf, len, cap, "?");
    }
    return false;
}

char *type_describe(const Type *t)
{
    if (!t) return NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (!describe_type_into(&buf, &len, &cap, t)) {
        free(buf);
        return NULL;
    }
    return buf;
}
