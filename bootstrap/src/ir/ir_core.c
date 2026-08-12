/* bootstrap/src/ir/ir_core.c
 *
 * AI-Co Stage-0 canonical IR node model and invariants (WP-M0-16b1).
 *
 * Implements the IR node model of the accepted canonical IR contract
 * (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.0) and the invariant
 * verification of contract sec. 10: violations are reported as AIC-I0501
 * records (phase "ir", severity "error", recovery "authoritative", primary
 * span = the violating node's span), returned sorted with the
 * DIAGNOSTIC-CONTRACT sec. 9 comparator.
 *
 * Invariants enforced here (contract sec. 10):
 *   1 graph well-formedness     2 span/cause presence
 *   3 type well-formedness      4 operand typing
 *   5 terminators               6 no fall-through
 *   7 break/continue placement  8 return typing
 *   9 trap-code presence       10 store/lvalue rules
 *  11 evaluation order
 * Invariant 12 (byte round-trip through the deterministic dump) is owned by
 * WP-M0-16b2; this package enforces the graph-side determinism
 * preconditions (unique gapless ids, interned types/constants).
 *
 * The invariant list is closed for the contract; the per-kind typing rules,
 * the per-kind trap-code sets, and the tail-termination analysis below are
 * implementation details of this package within the contract.
 *
 * Tail-termination analysis (invariant 5, non-void function tails): the IR
 * is built only from accepted programs (spec sec. 13.4/13.5 already reject
 * non-returning paths, AIC-E0416); this analysis is the structural backstop
 * that makes the rule structural in the IR. A statement sequence terminates
 * when its last statement is a returning/noreturn terminator, a fully
 * terminating if/else, a switch with a terminating default and terminating
 * case bodies, or an always-true loop whose body never exits it. `break`
 * and `continue` never count as terminating for the tail (they fall out of
 * the enclosing construct); `continue` counts as non-exiting inside an
 * always-true loop body (it re-enters the always-true condition).
 */
#include "ir_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

static const char *const kKindNames[] = {
    "IR_MODULE", "IR_IMPORT", "IR_STRUCT_DECL", "IR_ENUM_DECL",
    "IR_GLOBAL_CONST", "IR_GLOBAL_VAR", "IR_FUNCTION",
    "IR_BLOCK", "IR_LOCAL_DECL", "IR_IF", "IR_WHILE", "IR_FOR",
    "IR_SWITCH", "IR_CASE", "IR_DEFAULT", "IR_BREAK", "IR_CONTINUE",
    "IR_RETURN", "IR_EXPR_STMT", "IR_EMPTY", "IR_CALL_TERM", "IR_TRAP",
    "IR_INT", "IR_BOOL", "IR_NULL", "IR_STR", "IR_ENUM_VAL",
    "IR_LOCAL", "IR_GLOBAL", "IR_FIELD_ADDR", "IR_INDEX_ADDR", "IR_DEREF",
    "IR_LOAD", "IR_STORE",
    "IR_ADD", "IR_SUB", "IR_MUL", "IR_DIV", "IR_MOD", "IR_NEG",
    "IR_SHL", "IR_SHR",
    "IR_BAND", "IR_BOR", "IR_BXOR", "IR_BNOT",
    "IR_LNOT",
    "IR_LAND", "IR_LOR",
    "IR_EQ", "IR_NE", "IR_LT", "IR_LE", "IR_GT", "IR_GE",
    "IR_SLICE_EQ",
    "IR_SELECT", "IR_CALL", "IR_LEN", "IR_PTR", "IR_SLICE",
    "IR_CAST", "IR_WRAP",
    "IR_PTR_ADD", "IR_PTR_SUB", "IR_PTR_DIFF",
    "IR_ZERO"
};

const char *ir_kind_text(IrNodeKind kind)
{
    if (kind < 0 || (size_t)kind >= sizeof(kKindNames) / sizeof(kKindNames[0])) {
        return "IR_?";
    }
    return kKindNames[kind];
}

static const char *const kTypeKindNames[] = {
    "void", "bool", "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64", "isize", "usize",
    "str", "array", "slice", "ptr", "struct", "enum"
};

const char *ir_type_kind_text(IrTypeKind kind)
{
    if (kind < 0 || (size_t)kind >= sizeof(kTypeKindNames) / sizeof(kTypeKindNames[0])) {
        return "?";
    }
    return kTypeKindNames[kind];
}

static const char *const kConstKindNames[] = {
    "int", "bool", "null", "str", "enum", "struct", "array", "addr"
};

const char *ir_const_kind_text(IrConstKind kind)
{
    if (kind < 0 || (size_t)kind >= sizeof(kConstKindNames) / sizeof(kConstKindNames[0])) {
        return "?";
    }
    return kConstKindNames[kind];
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    size_t n;
    char *p;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

/* Append `item` to a pointer array; returns false on allocation failure
 * (the array is left unchanged). */
static bool ptr_array_append(void ***arr, size_t *count, void *item)
{
    void **p;
    p = (void **)realloc(*arr, (*count + 1) * sizeof(void *));
    if (p == NULL) {
        return false;
    }
    p[*count] = item;
    *arr = p;
    *count += 1;
    return true;
}

static bool str_list_contains(const char *const *list, const char *s)
{
    size_t i;
    for (i = 0; list[i] != NULL; i++) {
        if (strcmp(list[i], s) == 0) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Build lifecycle
 * ------------------------------------------------------------------------- */

IrBuild *ir_build_new(void)
{
    IrBuild *b = (IrBuild *)calloc(1, sizeof(*b));
    static const int64_t kBaseSize[13] = { 0, 1, 1, 2, 4, 8, 1, 2, 4, 8, 8, 8, 16 };
    static const int64_t kBaseAlign[13] = { 1, 1, 1, 2, 4, 8, 1, 2, 4, 8, 8, 8, 8 };
    int64_t k;
    if (b == NULL) {
        return NULL;
    }
    /* Base types interned at build creation in spec sec. 7.1 table order
     * (void, bool, i8, i16, i32, i64, u8, u16, u32, u64, isize, usize,
     * str); id == kind for the base types. */
    for (k = 0; k < 13; k++) {
        IrType *t = (IrType *)calloc(1, sizeof(*t));
        if (t == NULL) {
            b->oom = true;
            break;
        }
        t->kind = (IrTypeKind)k;
        t->id = (int64_t)b->ntypes;
        t->size = kBaseSize[k];
        t->align = kBaseAlign[k];
        if (!ptr_array_append((void ***)&b->types, &b->ntypes, t)) {
            free(t);
            b->oom = true;
            break;
        }
    }
    if (b->oom) {
        ir_build_free(b);
        return NULL;
    }
    return b;
}

static void const_free_payload(IrConst *c)
{
    switch (c->kind) {
    case IRC_STR:
        free(c->u.str.bytes);
        break;
    case IRC_STRUCT:
        free(c->u.strukt.items);
        break;
    case IRC_ARRAY:
        free(c->u.arr.items);
        break;
    default:
        break;
    }
    free(c);
}

static void node_free_payload(IrNode *n)
{
    size_t i;
    for (i = 0; i < n->cause_count; i++) {
        free(n->causes[i].construct_kind);
        diag_span_free(n->causes[i].span);
    }
    free(n->causes);
    diag_span_free(n->span);
    switch (n->kind) {
    case IR_MODULE:
        free(n->u.module.name);
        free(n->u.module.imports);
        free(n->u.module.decls);
        break;
    case IR_IMPORT:
        free(n->u.import.name);
        break;
    case IR_STRUCT_DECL:
        free(n->u.struct_decl.name);
        for (i = 0; i < n->u.struct_decl.nfields; i++) {
            free(n->u.struct_decl.fields[i].name);
            diag_span_free(n->u.struct_decl.fields[i].span);
        }
        free(n->u.struct_decl.fields);
        break;
    case IR_ENUM_DECL:
        free(n->u.enum_decl.name);
        for (i = 0; i < n->u.enum_decl.nmembers; i++) {
            free(n->u.enum_decl.members[i].name);
            diag_span_free(n->u.enum_decl.members[i].span);
        }
        free(n->u.enum_decl.members);
        break;
    case IR_GLOBAL_CONST:
        free(n->u.global_const.name);
        break;
    case IR_GLOBAL_VAR:
        free(n->u.global_var.name);
        break;
    case IR_FUNCTION:
        free(n->u.function.name);
        for (i = 0; i < n->u.function.nparams; i++) {
            free(n->u.function.params[i].name);
            diag_span_free(n->u.function.params[i].span);
        }
        free(n->u.function.params);
        for (i = 0; i < n->u.function.nslots; i++) {
            free(n->u.function.slots[i]->name);
            diag_span_free(n->u.function.slots[i]->span);
            free(n->u.function.slots[i]);
        }
        free(n->u.function.slots);
        break;
    case IR_BLOCK:
        free(n->u.block.stmts);
        break;
    case IR_SWITCH:
        free(n->u.switch_stmt.cases);
        break;
    case IR_CALL:
        free(n->u.call.args);
        break;
    case IR_CALL_TERM:
        free(n->u.call_term.args);
        break;
    default:
        break;
    }
    free(n);
}

void ir_build_free(IrBuild *b)
{
    size_t i;
    if (b == NULL) {
        return;
    }
    for (i = 0; i < b->nnodes; i++) {
        node_free_payload(b->nodes[i]);
    }
    free(b->nodes);
    for (i = 0; i < b->nconsts; i++) {
        const_free_payload(b->consts[i]);
    }
    free(b->consts);
    for (i = 0; i < b->ntypes; i++) {
        free(b->types[i]);
    }
    free(b->types);
    free(b->modules);
    free(b);
}

/* ---------------------------------------------------------------------------
 * Type descriptors (interning; contract sec. 6.3)
 * ------------------------------------------------------------------------- */

IrType *ir_type_void(IrBuild *b)  { return b->types[IRT_VOID]; }
IrType *ir_type_bool(IrBuild *b)  { return b->types[IRT_BOOL]; }
IrType *ir_type_i8(IrBuild *b)    { return b->types[IRT_I8]; }
IrType *ir_type_i16(IrBuild *b)   { return b->types[IRT_I16]; }
IrType *ir_type_i32(IrBuild *b)   { return b->types[IRT_I32]; }
IrType *ir_type_i64(IrBuild *b)   { return b->types[IRT_I64]; }
IrType *ir_type_u8(IrBuild *b)    { return b->types[IRT_U8]; }
IrType *ir_type_u16(IrBuild *b)   { return b->types[IRT_U16]; }
IrType *ir_type_u32(IrBuild *b)   { return b->types[IRT_U32]; }
IrType *ir_type_u64(IrBuild *b)   { return b->types[IRT_U64]; }
IrType *ir_type_isize(IrBuild *b) { return b->types[IRT_ISIZE]; }
IrType *ir_type_usize(IrBuild *b) { return b->types[IRT_USIZE]; }
IrType *ir_type_str(IrBuild *b)   { return b->types[IRT_STR]; }

bool ir_type_identical(const IrType *a, const IrType *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case IRT_ARRAY:
        return ir_type_identical(a->u.array.elem, b->u.array.elem) &&
               a->u.array.extent == b->u.array.extent;
    case IRT_SLICE:
        return ir_type_identical(a->u.slice.elem, b->u.slice.elem);
    case IRT_PTR:
        return ir_type_identical(a->u.ptr.elem, b->u.ptr.elem);
    case IRT_STRUCT:
    case IRT_ENUM:
        return a->u.decl == b->u.decl;
    default:
        return true;   /* base types and str: same kind */
    }
}

static IrType *type_intern(IrBuild *b, IrType *proto)
{
    size_t i;
    for (i = 0; i < b->ntypes; i++) {
        IrType *t = b->types[i];
        bool same = false;
        if (t->kind != proto->kind) {
            continue;
        }
        switch (proto->kind) {
        case IRT_ARRAY:
            same = t->u.array.elem == proto->u.array.elem &&
                   t->u.array.extent == proto->u.array.extent;
            break;
        case IRT_SLICE:
            same = t->u.slice.elem == proto->u.slice.elem;
            break;
        case IRT_PTR:
            same = t->u.ptr.elem == proto->u.ptr.elem;
            break;
        case IRT_STRUCT:
        case IRT_ENUM:
            same = t->u.decl == proto->u.decl;
            break;
        default:
            same = false;   /* base types/str are pre-interned */
            break;
        }
        if (same) {
            return t;
        }
    }
    if (b->oom) {
        return NULL;
    }
    {
        IrType *t = (IrType *)calloc(1, sizeof(*t));
        if (t == NULL) {
            b->oom = true;
            return NULL;
        }
        *t = *proto;
        t->id = (int64_t)b->ntypes;
        if (!ptr_array_append((void ***)&b->types, &b->ntypes, t)) {
            free(t);
            b->oom = true;
            return NULL;
        }
        return t;
    }
}

IrType *ir_type_array(IrBuild *b, IrType *elem, int64_t extent)
{
    IrType proto;
    int64_t esize;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRT_ARRAY;
    proto.u.array.elem = elem;
    proto.u.array.extent = extent;
    esize = (elem != NULL && elem->size >= 0) ? elem->size : 0;
    if (extent >= 0 && esize > 0 && extent <= INT64_MAX / esize) {
        proto.size = extent * esize;
    } else if (extent >= 0) {
        proto.size = INT64_MAX;   /* saturate; invariant 3 checks extent */
    } else {
        proto.size = 0;
    }
    proto.align = (elem != NULL && elem->align >= 1) ? elem->align : 1;
    return type_intern(b, &proto);
}

IrType *ir_type_slice(IrBuild *b, IrType *elem)
{
    IrType proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRT_SLICE;
    proto.u.slice.elem = elem;
    proto.size = 16;
    proto.align = 8;
    return type_intern(b, &proto);
}

IrType *ir_type_ptr(IrBuild *b, IrType *elem)
{
    IrType proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRT_PTR;
    proto.u.ptr.elem = elem;
    proto.size = 8;
    proto.align = 8;
    return type_intern(b, &proto);
}

IrType *ir_type_struct(IrBuild *b, IrNode *decl)
{
    IrType proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRT_STRUCT;
    proto.u.decl = decl;
    if (decl != NULL && decl->kind == IR_STRUCT_DECL) {
        proto.size = decl->u.struct_decl.size;
        proto.align = decl->u.struct_decl.align >= 1 ? decl->u.struct_decl.align : 1;
    } else {
        proto.size = 0;
        proto.align = 1;
    }
    return type_intern(b, &proto);
}

IrType *ir_type_enum(IrBuild *b, IrNode *decl)
{
    IrType proto;
    const IrType *under;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRT_ENUM;
    proto.u.decl = decl;
    under = (decl != NULL && decl->kind == IR_ENUM_DECL)
                ? decl->u.enum_decl.underlying : NULL;
    proto.size = (under != NULL && under->size >= 0) ? under->size : 8;
    proto.align = (under != NULL && under->align >= 1) ? under->align : 8;
    return type_intern(b, &proto);
}

/* ---------------------------------------------------------------------------
 * Constants (interning; contract sec. 6.4)
 * ------------------------------------------------------------------------- */

static bool const_value_identical(const IrConst *a, const IrConst *b)
{
    if (a->kind != b->kind || !ir_type_identical(a->type, b->type)) {
        return false;
    }
    switch (a->kind) {
    case IRC_INT:
        return a->u.int_bits == b->u.int_bits;
    case IRC_BOOL:
        return a->u.b == b->u.b;
    case IRC_NULL:
        return true;
    case IRC_STR:
        return a->u.str.len == b->u.str.len &&
               (a->u.str.len == 0 ||
                memcmp(a->u.str.bytes, b->u.str.bytes, a->u.str.len) == 0);
    case IRC_ENUM:
        return a->u.en.value == b->u.en.value &&
               a->u.en.enum_decl == b->u.en.enum_decl;
    case IRC_STRUCT:
        return a->u.strukt.count == b->u.strukt.count &&
               (a->u.strukt.count == 0 ||
                memcmp(a->u.strukt.items, b->u.strukt.items,
                       a->u.strukt.count * sizeof(void *)) == 0);
    case IRC_ARRAY:
        return a->u.arr.count == b->u.arr.count &&
               (a->u.arr.count == 0 ||
                memcmp(a->u.arr.items, b->u.arr.items,
                       a->u.arr.count * sizeof(void *)) == 0);
    case IRC_ADDR:
        return a->u.addr.target == b->u.addr.target &&
               a->u.addr.offset == b->u.addr.offset;
    }
    return false;
}

static IrConst *const_intern(IrBuild *b, const IrConst *proto)
{
    size_t i;
    for (i = 0; i < b->nconsts; i++) {
        if (const_value_identical(b->consts[i], proto)) {
            return b->consts[i];
        }
    }
    if (b->oom) {
        return NULL;
    }
    {
        IrConst *c = (IrConst *)calloc(1, sizeof(*c));
        if (c == NULL) {
            b->oom = true;
            return NULL;
        }
        *c = *proto;
        if (proto->kind == IRC_STR) {
            size_t n = proto->u.str.len ? proto->u.str.len : 1;
            uint8_t *bytes = (uint8_t *)malloc(n);
            if (bytes == NULL) {
                free(c);
                b->oom = true;
                return NULL;
            }
            if (proto->u.str.len) {
                memcpy(bytes, proto->u.str.bytes, proto->u.str.len);
            }
            c->u.str.bytes = bytes;
        } else if (proto->kind == IRC_STRUCT || proto->kind == IRC_ARRAY) {
            size_t n = proto->u.strukt.count;
            IrConst **items = NULL;
            if (n > 0) {
                items = (IrConst **)malloc(n * sizeof(*items));
                if (items == NULL) {
                    free(c);
                    b->oom = true;
                    return NULL;
                }
                memcpy(items, proto->u.strukt.items, n * sizeof(*items));
            }
            c->u.strukt.items = items;
        }
        c->id = (int64_t)b->nconsts;
        if (!ptr_array_append((void ***)&b->consts, &b->nconsts, c)) {
            const_free_payload(c);
            b->oom = true;
            return NULL;
        }
        return c;
    }
}

IrConst *ir_const_int(IrBuild *b, IrType *type, uint64_t bits)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_INT;
    proto.type = type;
    proto.u.int_bits = bits;
    return const_intern(b, &proto);
}

IrConst *ir_const_bool(IrBuild *b, bool value)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_BOOL;
    proto.type = ir_type_bool(b);
    proto.u.b = value;
    return const_intern(b, &proto);
}

IrConst *ir_const_null(IrBuild *b, IrType *ptr_type)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_NULL;
    proto.type = ptr_type;
    return const_intern(b, &proto);
}

IrConst *ir_const_str(IrBuild *b, const uint8_t *bytes, size_t len)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_STR;
    proto.type = ir_type_str(b);
    proto.u.str.bytes = (uint8_t *)bytes;   /* copied by const_intern */
    proto.u.str.len = len;
    return const_intern(b, &proto);
}

IrConst *ir_const_enum(IrBuild *b, IrType *enum_type, uint64_t value)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_ENUM;
    proto.type = enum_type;
    proto.u.en.value = value;
    proto.u.en.enum_decl = (enum_type != NULL && enum_type->kind == IRT_ENUM)
                               ? enum_type->u.decl : NULL;
    return const_intern(b, &proto);
}

IrConst *ir_const_struct(IrBuild *b, IrType *struct_type,
                         IrConst **items, size_t count)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_STRUCT;
    proto.type = struct_type;
    proto.u.strukt.items = items;   /* copied by const_intern */
    proto.u.strukt.count = count;
    return const_intern(b, &proto);
}

IrConst *ir_const_array(IrBuild *b, IrType *array_type,
                        IrConst **items, size_t count)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_ARRAY;
    proto.type = array_type;
    proto.u.arr.items = items;   /* copied by const_intern */
    proto.u.arr.count = count;
    return const_intern(b, &proto);
}

IrConst *ir_const_addr(IrBuild *b, IrType *ptr_type, IrNode *target,
                       int64_t offset)
{
    IrConst proto;
    memset(&proto, 0, sizeof(proto));
    proto.kind = IRC_ADDR;
    proto.type = ptr_type;
    proto.u.addr.target = target;
    proto.u.addr.offset = offset;
    return const_intern(b, &proto);
}

/* ---------------------------------------------------------------------------
 * Node construction
 * ------------------------------------------------------------------------- */

IrNode *ir_node_new(IrBuild *b, IrNodeKind kind, const DiagSpan *span)
{
    IrNode *n;
    if (b->oom) {
        return NULL;
    }
    n = (IrNode *)calloc(1, sizeof(*n));
    if (n == NULL) {
        b->oom = true;
        return NULL;
    }
    n->kind = kind;
    n->id = b->next_id++;
    n->span = diag_span_clone(span);
    if (n->span == NULL) {
        free(n);
        b->oom = true;
        return NULL;
    }
    if (!ptr_array_append((void ***)&b->nodes, &b->nnodes, n)) {
        free(n->span);
        free(n);
        b->oom = true;
        return NULL;
    }
    return n;
}

void ir_node_add_cause(IrBuild *b, IrNode *node, const char *construct_kind,
                       const DiagSpan *span,
                       int64_t ref_decl, int64_t ref_type, int64_t ref_const)
{
    IrCauseLink *links;
    char *kind;
    DiagSpan *copy;
    if (b->oom || node == NULL) {
        return;
    }
    kind = dup_str(construct_kind);
    copy = diag_span_clone(span);
    if (kind == NULL || copy == NULL) {
        free(kind);
        diag_span_free(copy);
        b->oom = true;
        return;
    }
    links = (IrCauseLink *)realloc(node->causes,
                                   (node->cause_count + 1) * sizeof(*links));
    if (links == NULL) {
        free(kind);
        diag_span_free(copy);
        b->oom = true;
        return;
    }
    node->causes = links;
    node->causes[node->cause_count].construct_kind = kind;
    node->causes[node->cause_count].span = copy;
    node->causes[node->cause_count].ref_decl = ref_decl;
    node->causes[node->cause_count].ref_type = ref_type;
    node->causes[node->cause_count].ref_const = ref_const;
    node->cause_count += 1;
}

void ir_module_add_import(IrBuild *b, IrNode *module, IrNode *import)
{
    if (b->oom || module == NULL) {
        return;
    }
    if (!ptr_array_append((void ***)&module->u.module.imports,
                          &module->u.module.nimports, import)) {
        b->oom = true;
    }
}

void ir_module_add_decl(IrBuild *b, IrNode *module, IrNode *decl)
{
    if (b->oom || module == NULL) {
        return;
    }
    if (!ptr_array_append((void ***)&module->u.module.decls,
                          &module->u.module.ndecls, decl)) {
        b->oom = true;
    }
}

void ir_block_add_stmt(IrBuild *b, IrNode *block, IrNode *stmt)
{
    if (b->oom || block == NULL) {
        return;
    }
    if (!ptr_array_append((void ***)&block->u.block.stmts,
                          &block->u.block.nstmts, stmt)) {
        b->oom = true;
    }
}

void ir_call_add_arg(IrBuild *b, IrNode *call, IrNode *arg)
{
    if (b->oom || call == NULL) {
        return;
    }
    if (!ptr_array_append((void ***)&call->u.call.args,
                          &call->u.call.nargs, arg)) {
        b->oom = true;
    }
}

void ir_call_term_add_arg(IrBuild *b, IrNode *call_term, IrNode *arg)
{
    if (b->oom || call_term == NULL) {
        return;
    }
    if (!ptr_array_append((void ***)&call_term->u.call_term.args,
                          &call_term->u.call_term.nargs, arg)) {
        b->oom = true;
    }
}

void ir_records_free(DiagRecord **records, size_t count)
{
    size_t i;
    if (records == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        diag_record_free(records[i]);
    }
    free(records);
}

void ir_build_add_module(IrBuild *b, IrNode *module)
{
    if (b->oom || module == NULL) {
        return;
    }
    if (!ptr_array_append((void ***)&b->modules, &b->nmodules, module)) {
        b->oom = true;
    }
}

/* ---------------------------------------------------------------------------
 * Invariant verification
 * ------------------------------------------------------------------------- */

#define IR_INV_GRAPH "graph well-formedness"
#define IR_INV_SPAN  "span/cause presence"
#define IR_INV_TYPE  "type well-formedness"
#define IR_INV_TYPING "operand typing"
#define IR_INV_TERM  "terminators"
#define IR_INV_FALL  "no fall-through"
#define IR_INV_PLACE "break/continue placement"
#define IR_INV_RET   "return typing"
#define IR_INV_TRAP  "trap-code presence"
#define IR_INV_STORE "store/lvalue rules"
#define IR_INV_ORDER "evaluation order"

typedef struct IrVerify {
    const IrBuild *build;
    DiagRecord **recs;
    size_t nrecs;
    size_t cap;
    bool oom;
    /* pass 1 collections */
    const IrNode **decls;      /* declaration nodes reachable from modules */
    size_t ndecls;
    const char **module_names;
    size_t nmodule_names;
    unsigned char *seen;       /* node id bitmap */
    unsigned char *reached;    /* node id bitmap */
    size_t nnodes;
    /* pass 2 context */
    const IrNode *fn;          /* current function, or NULL */
} IrVerify;

typedef struct Encl {
    const IrNode **items;      /* enclosing WHILE/FOR/SWITCH nodes */
    size_t count;
    size_t cap;
} Encl;

static void verify_add_violation(IrVerify *v, const IrNode *node,
                                 int invariant, const char *inv_name,
                                 const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    DiagRecord *r;
    int hdr;
    if (v->oom) {
        return;
    }
    hdr = snprintf(msg, sizeof(msg),
                   "IR invariant violation (invariant %d, %s): ",
                   invariant, inv_name);
    if (hdr < 0 || (size_t)hdr >= sizeof(msg)) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(msg + hdr, sizeof(msg) - hdr, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    r = diag_record_new();
    if (r == NULL) {
        v->oom = true;
        return;
    }
    if (!diag_record_set_code(r, "AIC-I0501") ||
        !diag_record_set_message(r, msg) ||
        !diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r);
        v->oom = true;
        return;
    }
    if (node != NULL && node->span != NULL) {
        if (!diag_record_set_primary_span(r, node->span)) {
            diag_record_free(r);
            v->oom = true;
            return;
        }
    }
    if (node != NULL) {
        if (!diag_record_add_related_int(r, "invariant", invariant) ||
            !diag_record_add_related_int(r, "node_id", node->id) ||
            !diag_record_add_related_str(r, "node_kind",
                                         ir_kind_text(node->kind))) {
            diag_record_free(r);
            v->oom = true;
            return;
        }
    } else {
        if (!diag_record_add_related_int(r, "invariant", invariant)) {
            diag_record_free(r);
            v->oom = true;
            return;
        }
    }
    if (!ptr_array_append((void ***)&v->recs, &v->nrecs, r)) {
        diag_record_free(r);
        v->oom = true;
    }
}

/* --- kind predicates (contract sec. 5) --- */

static bool ir_kind_produces_value(IrNodeKind kind)
{
    switch (kind) {
    case IR_INT: case IR_BOOL: case IR_NULL: case IR_STR: case IR_ENUM_VAL:
    case IR_LOCAL: case IR_GLOBAL: case IR_FIELD_ADDR: case IR_INDEX_ADDR:
    case IR_DEREF: case IR_LOAD:
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_NEG: case IR_SHL: case IR_SHR:
    case IR_BAND: case IR_BOR: case IR_BXOR: case IR_BNOT: case IR_LNOT:
    case IR_LAND: case IR_LOR:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
    case IR_SLICE_EQ: case IR_SELECT: case IR_CALL:
    case IR_LEN: case IR_PTR: case IR_SLICE: case IR_CAST: case IR_WRAP:
    case IR_PTR_ADD: case IR_PTR_SUB: case IR_PTR_DIFF:
        return true;
    default:
        return false;
    }
}

static bool ir_kind_is_expr(IrNodeKind kind)
{
    return ir_kind_produces_value(kind) || kind == IR_STORE || kind == IR_ZERO;
}

static bool ir_kind_is_stmt(IrNodeKind kind)
{
    switch (kind) {
    case IR_BLOCK: case IR_LOCAL_DECL: case IR_IF: case IR_WHILE:
    case IR_FOR: case IR_SWITCH: case IR_CASE: case IR_DEFAULT:
    case IR_BREAK: case IR_CONTINUE: case IR_RETURN: case IR_EXPR_STMT:
    case IR_EMPTY: case IR_CALL_TERM: case IR_TRAP:
        return true;
    default:
        return false;
    }
}

static bool ir_kind_is_terminator(IrNodeKind kind)
{
    switch (kind) {
    case IR_RETURN: case IR_BREAK: case IR_CONTINUE:
    case IR_CALL_TERM: case IR_TRAP:
        return true;
    default:
        return false;
    }
}

static bool ir_kind_is_decl(IrNodeKind kind)
{
    switch (kind) {
    case IR_STRUCT_DECL: case IR_ENUM_DECL: case IR_GLOBAL_CONST:
    case IR_GLOBAL_VAR: case IR_FUNCTION:
        return true;
    default:
        return false;
    }
}

/* --- type predicates (contract sec. 5.4) --- */

static bool is_int_type(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    switch (t->kind) {
    case IRT_I8: case IRT_I16: case IRT_I32: case IRT_I64:
    case IRT_U8: case IRT_U16: case IRT_U32: case IRT_U64:
    case IRT_ISIZE: case IRT_USIZE:
        return true;
    default:
        return false;
    }
}

static bool is_signed_int_type(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    switch (t->kind) {
    case IRT_I8: case IRT_I16: case IRT_I32: case IRT_I64: case IRT_ISIZE:
        return true;
    default:
        return false;
    }
}

static bool is_scalar_type(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    return is_int_type(t) || t->kind == IRT_BOOL || t->kind == IRT_PTR ||
           t->kind == IRT_ENUM;
}

/* --- lvalue model (contract sec. 5.4) --- */

static bool is_lvalue(const IrNode *n)
{
    if (n == NULL) {
        return false;
    }
    switch (n->kind) {
    case IR_LOCAL: case IR_GLOBAL: case IR_DEREF: case IR_FIELD_ADDR:
        return true;
    case IR_INDEX_ADDR:
        /* lvalue only when the base is a mutable array/slice lvalue; a
         * `str` index address is a value address, never an lvalue
         * (contract sec. 5.3 IR_INDEX_ADDR). The base itself must be an
         * lvalue: an array-typed value (e.g. an IR_CALL result) has no
         * storage location, so indexing it cannot produce an lvalue. */
        return n->u.index_addr.base != NULL &&
               n->u.index_addr.base->type != NULL &&
               is_lvalue(n->u.index_addr.base) &&
               (n->u.index_addr.base->type->kind == IRT_ARRAY ||
                n->u.index_addr.base->type->kind == IRT_SLICE);
    default:
        return false;
    }
}

/* The type of the value stored at the location an lvalue denotes:
 * IR_LOCAL/IR_GLOBAL/IR_DEREF carry the value type directly;
 * IR_FIELD_ADDR/IR_INDEX_ADDR carry U* and the location holds U. */
static const IrType *lvalue_value_type(const IrNode *n)
{
    if (n == NULL) {
        return NULL;
    }
    switch (n->kind) {
    case IR_LOCAL: case IR_GLOBAL: case IR_DEREF:
        return n->type;
    case IR_FIELD_ADDR: case IR_INDEX_ADDR:
        if (n->type != NULL && n->type->kind == IRT_PTR) {
            return n->type->u.ptr.elem;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* --- per-kind trap-code sets (contract sec. 5.3 table) --- */

static const char *const kTrapIndex[] = { "AIC-R0807", NULL };
static const char *const kTrapDeref[] = { "AIC-R0809", "AIC-R0811", NULL };
static const char *const kTrapArith[] = { "AIC-R0802", NULL };
static const char *const kTrapDivMod[] = { "AIC-R0803", "AIC-R0802", NULL };
static const char *const kTrapShift[] = { "AIC-R0804", NULL };
static const char *const kTrapSlice[] = { "AIC-R0807", "AIC-R0808", NULL };
static const char *const kTrapCast[] = { "AIC-R0801", "AIC-R0806", NULL };
static const char *const kTrapPtrArith[] = { "AIC-R0816", NULL };
static const char *const kTrapPtrDiff[] = { "AIC-R0816", "AIC-R0810", NULL };

static const char *const *ir_kind_trap_codes(IrNodeKind kind)
{
    switch (kind) {
    case IR_INDEX_ADDR: return kTrapIndex;
    case IR_DEREF: return kTrapDeref;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_NEG: return kTrapArith;
    case IR_DIV: case IR_MOD: return kTrapDivMod;
    case IR_SHL: case IR_SHR: return kTrapShift;
    case IR_SLICE: return kTrapSlice;
    case IR_CAST: return kTrapCast;
    case IR_PTR_ADD: case IR_PTR_SUB: return kTrapPtrArith;
    case IR_PTR_DIFF: return kTrapPtrDiff;
    default: return NULL;
    }
}

/* --- enclosing-construct stack (invariant 7) --- */

static void encl_push(Encl *e, const IrNode *n, bool *oom)
{
    if (!ptr_array_append((void ***)&e->items, &e->count, (void *)n)) {
        *oom = true;
    }
}

static void encl_pop(Encl *e)
{
    if (e->count > 0) {
        e->count -= 1;
    }
}

static bool encl_contains(const Encl *e, const IrNode *n)
{
    size_t i;
    for (i = 0; i < e->count; i++) {
        if (e->items[i] == n) {
            return true;
        }
    }
    return false;
}

/* --- tail-termination analysis (invariant 5, contract sec. 5.6) --- */

static bool cond_is_const_true(const IrNode *cond)
{
    if (cond == NULL) {
        return true;   /* absent for-loop condition = true (spec sec. 13.3) */
    }
    return cond->kind == IR_BOOL &&
           cond->u.constant.value != NULL &&
           cond->u.constant.value->kind == IRC_BOOL &&
           cond->u.constant.value->u.b;
}

static bool path_seq_never_exits(IrNode *const *stmts, size_t n);
static bool path_block_never_exits(const IrNode *b)
{
    return b != NULL && b->kind == IR_BLOCK &&
           path_seq_never_exits(b->u.block.stmts, b->u.block.nstmts);
}

/* Every path through the statement list ends in a terminator that does not
 * exit the enclosing always-true loop (return/call_term/trap terminate;
 * continue re-enters the always-true condition; break exits -> false). */
static bool path_seq_never_exits(IrNode *const *stmts, size_t n)
{
    const IrNode *last;
    if (n == 0) {
        return false;
    }
    last = stmts[n - 1];
    switch (last->kind) {
    case IR_RETURN: case IR_CALL_TERM: case IR_TRAP: case IR_CONTINUE:
        return true;
    case IR_BREAK:
        return false;
    case IR_BLOCK:
        return path_seq_never_exits(last->u.block.stmts,
                                    last->u.block.nstmts);
    case IR_IF:
        return last->u.if_stmt.else_block != NULL &&
               path_block_never_exits(last->u.if_stmt.then_block) &&
               path_block_never_exits(last->u.if_stmt.else_block);
    case IR_SWITCH: {
        size_t i;
        if (last->u.switch_stmt.default_clause == NULL) {
            return false;
        }
        for (i = 0; i < last->u.switch_stmt.ncases; i++) {
            const IrNode *c = last->u.switch_stmt.cases[i];
            if (c == NULL || c->kind != IR_CASE ||
                !path_block_never_exits(c->u.case_clause.body)) {
                return false;
            }
        }
        return path_block_never_exits(
            last->u.switch_stmt.default_clause->u.default_clause.body);
    }
    case IR_WHILE: case IR_FOR: {
        const IrNode *cond = (last->kind == IR_WHILE)
                                 ? last->u.while_stmt.cond
                                 : last->u.for_stmt.cond;
        const IrNode *body = (last->kind == IR_WHILE)
                                 ? last->u.while_stmt.body
                                 : last->u.for_stmt.body;
        return cond_is_const_true(cond) && path_block_never_exits(body);
    }
    default:
        return false;
    }
}

static bool stmt_seq_terminates(IrNode *const *stmts, size_t n);
static bool block_terminates(const IrNode *b)
{
    return b != NULL && b->kind == IR_BLOCK &&
           stmt_seq_terminates(b->u.block.stmts, b->u.block.nstmts);
}

/* Every reachable path through the statement list ends in a terminator that
 * does not fall out of the enclosing construct (the non-void function tail
 * rule of contract sec. 5.6). `break`/`continue` fall out and never
 * terminate a tail; an always-true loop whose body never exits it
 * terminates the tail. */
static bool stmt_seq_terminates(IrNode *const *stmts, size_t n)
{
    const IrNode *last;
    if (n == 0) {
        return false;
    }
    last = stmts[n - 1];
    switch (last->kind) {
    case IR_RETURN: case IR_CALL_TERM: case IR_TRAP:
        return true;
    case IR_BREAK: case IR_CONTINUE:
        return false;
    case IR_BLOCK:
        return stmt_seq_terminates(last->u.block.stmts,
                                   last->u.block.nstmts);
    case IR_IF:
        return last->u.if_stmt.else_block != NULL &&
               block_terminates(last->u.if_stmt.then_block) &&
               block_terminates(last->u.if_stmt.else_block);
    case IR_SWITCH: {
        size_t i;
        if (last->u.switch_stmt.default_clause == NULL) {
            return false;
        }
        for (i = 0; i < last->u.switch_stmt.ncases; i++) {
            const IrNode *c = last->u.switch_stmt.cases[i];
            if (c == NULL || c->kind != IR_CASE ||
                !block_terminates(c->u.case_clause.body)) {
                return false;
            }
        }
        return block_terminates(
            last->u.switch_stmt.default_clause->u.default_clause.body);
    }
    case IR_WHILE: case IR_FOR: {
        const IrNode *cond = (last->kind == IR_WHILE)
                                 ? last->u.while_stmt.cond
                                 : last->u.for_stmt.cond;
        const IrNode *body = (last->kind == IR_WHILE)
                                 ? last->u.while_stmt.body
                                 : last->u.for_stmt.body;
        return cond_is_const_true(cond) && path_block_never_exits(body);
    }
    default:
        return false;
    }
}

/* --- child iteration (pass 1 reachability marking) ---
 *
 * Traverses STRUCTURAL children only. Reference edges (break/continue
 * targets, IR_GLOBAL targets, call callees) are not followed: they point
 * to nodes already reachable structurally (enclosing constructs are
 * ancestors; globals and functions are module declarations), and following
 * them would create cycles (e.g. a continue inside a switch targeting the
 * enclosing switch). The reference edges are validated, not traversed, in
 * pass 2. */

typedef void (*child_visitor)(void *ctx, const IrNode *child);

static void visit_all_children(const IrNode *n, child_visitor fn, void *ctx)
{
    size_t i;
    switch (n->kind) {
    case IR_MODULE:
        for (i = 0; i < n->u.module.nimports; i++) {
            fn(ctx, n->u.module.imports[i]);
        }
        for (i = 0; i < n->u.module.ndecls; i++) {
            fn(ctx, n->u.module.decls[i]);
        }
        break;
    case IR_FUNCTION:
        fn(ctx, n->u.function.body);
        break;
    case IR_BLOCK:
        for (i = 0; i < n->u.block.nstmts; i++) {
            fn(ctx, n->u.block.stmts[i]);
        }
        break;
    case IR_LOCAL_DECL:
        fn(ctx, n->u.local_decl.init);
        break;
    case IR_IF:
        fn(ctx, n->u.if_stmt.cond);
        fn(ctx, n->u.if_stmt.then_block);
        fn(ctx, n->u.if_stmt.else_block);
        break;
    case IR_WHILE:
        fn(ctx, n->u.while_stmt.cond);
        fn(ctx, n->u.while_stmt.body);
        break;
    case IR_FOR:
        fn(ctx, n->u.for_stmt.init);
        fn(ctx, n->u.for_stmt.cond);
        fn(ctx, n->u.for_stmt.step);
        fn(ctx, n->u.for_stmt.body);
        break;
    case IR_SWITCH:
        fn(ctx, n->u.switch_stmt.selector);
        for (i = 0; i < n->u.switch_stmt.ncases; i++) {
            fn(ctx, n->u.switch_stmt.cases[i]);
        }
        fn(ctx, n->u.switch_stmt.default_clause);
        break;
    case IR_CASE:
        fn(ctx, n->u.case_clause.body);
        break;
    case IR_DEFAULT:
        fn(ctx, n->u.default_clause.body);
        break;
    case IR_RETURN:
        fn(ctx, n->u.return_stmt.value);
        break;
    case IR_EXPR_STMT:
        fn(ctx, n->u.expr_stmt.expr);
        break;
    case IR_CALL_TERM:
        for (i = 0; i < n->u.call_term.nargs; i++) {
            fn(ctx, n->u.call_term.args[i]);
        }
        break;
    case IR_FIELD_ADDR:
        fn(ctx, n->u.field_addr.base);
        break;
    case IR_INDEX_ADDR:
        fn(ctx, n->u.index_addr.base);
        fn(ctx, n->u.index_addr.index);
        break;
    case IR_DEREF:
        fn(ctx, n->u.deref.ptr);
        break;
    case IR_LOAD:
        fn(ctx, n->u.load.lvalue);
        break;
    case IR_STORE:
        fn(ctx, n->u.store.dest);
        fn(ctx, n->u.store.value);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_SHL: case IR_SHR: case IR_BAND: case IR_BOR: case IR_BXOR:
    case IR_LAND: case IR_LOR:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
    case IR_SLICE_EQ:
    case IR_PTR_DIFF:
        fn(ctx, n->u.binary.left);
        fn(ctx, n->u.binary.right);
        break;
    case IR_NEG: case IR_BNOT: case IR_LNOT:
    case IR_LEN: case IR_PTR: case IR_CAST: case IR_WRAP: case IR_ZERO:
        fn(ctx, n->u.unary.operand);
        break;
    case IR_SELECT:
        fn(ctx, n->u.select.cond);
        fn(ctx, n->u.select.then_value);
        fn(ctx, n->u.select.else_value);
        break;
    case IR_CALL:
        for (i = 0; i < n->u.call.nargs; i++) {
            fn(ctx, n->u.call.args[i]);
        }
        break;
    case IR_SLICE:
        fn(ctx, n->u.slice.base);
        fn(ctx, n->u.slice.start);
        fn(ctx, n->u.slice.end);
        break;
    case IR_PTR_ADD: case IR_PTR_SUB:
        fn(ctx, n->u.ptr_arith.ptr);
        fn(ctx, n->u.ptr_arith.offset);
        break;
    default:
        break;   /* leaf kinds: no node children */
    }
}

/* --- pass 1 --- */

static void pass1_mark(void *ctx, const IrNode *child)
{
    IrVerify *v = (IrVerify *)ctx;
    if (child == NULL) {
        return;   /* NULL children are reported in pass 2 */
    }
    if (child->id >= 0 && (size_t)child->id < v->build->nnodes) {
        v->reached[child->id] = 1;
    }
    if (ir_kind_is_decl(child->kind)) {
        if (v->oom) {
            return;
        }
        if (!ptr_array_append((void ***)&v->decls, &v->ndecls,
                              (void *)child)) {
            v->oom = true;
            return;
        }
    }
    visit_all_children(child, pass1_mark, ctx);
}

/* --- per-kind payload checks and structural recursion (pass 2) --- */

static void verify_node(IrVerify *v, const IrNode *node, Encl *encl);

static void verify_child(IrVerify *v, const IrNode *child, Encl *encl)
{
    if (child != NULL) {
        verify_node(v, child, encl);
    }
}

static bool span_valid(const DiagSpan *s)
{
    return s != NULL && s->file != NULL && s->file[0] != '\0' &&
           s->start.line >= 1 && s->start.col >= 1 && s->start.offset >= 0 &&
           s->end.line >= 1 && s->end.col >= 1 && s->end.offset >= 0 &&
           s->start.offset <= s->end.offset;
}

static void verify_span_cause(IrVerify *v, const IrNode *node)
{
    size_t i;
    if (!span_valid(node->span)) {
        verify_add_violation(v, node, 2, IR_INV_SPAN,
                             "%s id %lld has no valid primary span",
                             ir_kind_text(node->kind), node->id);
    }
    if (node->causes == NULL || node->cause_count < 1) {
        verify_add_violation(v, node, 2, IR_INV_SPAN,
                             "%s id %lld has no cause chain",
                             ir_kind_text(node->kind), node->id);
        return;
    }
    for (i = 0; i < node->cause_count; i++) {
        const IrCauseLink *l = &node->causes[i];
        if (l->construct_kind == NULL || l->construct_kind[0] == '\0' ||
            !span_valid(l->span)) {
            verify_add_violation(v, node, 2, IR_INV_SPAN,
                                 "cause link %zu of %s id %lld lacks a "
                                 "construct kind or valid span",
                                 i, ir_kind_text(node->kind), node->id);
        }
    }
    if (node->span != NULL && node->span->file != NULL &&
        node->causes[0].span != NULL && node->causes[0].span->file != NULL &&
        strcmp(node->causes[0].span->file, node->span->file) != 0) {
        verify_add_violation(v, node, 2, IR_INV_SPAN,
                             "cause chain of %s id %lld does not terminate "
                             "at the module root (root link file '%s' differs "
                             "from node file '%s')",
                             ir_kind_text(node->kind), node->id,
                             node->causes[0].span->file, node->span->file);
    }
}

static void verify_result_type(IrVerify *v, const IrNode *node)
{
    if (ir_kind_produces_value(node->kind)) {
        if (node->type == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "%s id %lld is a value node but has no "
                                 "result type",
                                 ir_kind_text(node->kind), node->id);
        } else if (node->type->kind == IRT_VOID && node->kind != IR_CALL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "%s id %lld has void result type (void never "
                                 "appears as a value type)",
                                 ir_kind_text(node->kind), node->id);
        }
    } else if (node->type != NULL) {
        verify_add_violation(v, node, 3, IR_INV_TYPE,
                             "%s id %lld is not a value node but carries a "
                             "result type",
                             ir_kind_text(node->kind), node->id);
    }
}

static void verify_trap_code(IrVerify *v, const IrNode *node)
{
    const char *const *allowed;
    if (node->kind == IR_LOAD) {
        /* A bool-typed load carries the AIC-R0805 obligation (contract
         * sec. 5.3 IR_LOAD); non-bool loads have no failure mode. */
        if (node->type != NULL && node->type->kind == IRT_BOOL) {
            if (node->trap_code == NULL) {
                verify_add_violation(v, node, 9, IR_INV_TRAP,
                                     "bool-typed IR_LOAD id %lld lacks the "
                                     "AIC-R0805 trap obligation",
                                     node->id);
            } else if (strcmp(node->trap_code, "AIC-R0805") != 0) {
                verify_add_violation(v, node, 9, IR_INV_TRAP,
                                     "bool-typed IR_LOAD id %lld carries trap "
                                     "code '%s', expected AIC-R0805",
                                     node->id, node->trap_code);
            }
        } else if (node->trap_code != NULL) {
            verify_add_violation(v, node, 9, IR_INV_TRAP,
                                 "non-bool IR_LOAD id %lld carries a trap "
                                 "obligation",
                                 node->id);
        }
        return;
    }
    if (node->kind == IR_TRAP) {
        return;   /* IR_TRAP's code is carried in the payload; checked there */
    }
    allowed = ir_kind_trap_codes(node->kind);
    if (allowed != NULL) {
        if (node->trap_code == NULL) {
            verify_add_violation(v, node, 9, IR_INV_TRAP,
                                 "%s id %lld has a runtime failure mode but "
                                 "declares no trap code",
                                 ir_kind_text(node->kind), node->id);
        } else if (diag_code_lookup(node->trap_code) == NULL) {
            verify_add_violation(v, node, 9, IR_INV_TRAP,
                                 "%s id %lld declares trap code '%s' which is "
                                 "not in the diagnostic registry",
                                 ir_kind_text(node->kind), node->id,
                                 node->trap_code);
        } else if (!str_list_contains(allowed, node->trap_code)) {
            verify_add_violation(v, node, 9, IR_INV_TRAP,
                                 "%s id %lld declares trap code '%s' outside "
                                 "the declared set for this node kind",
                                 ir_kind_text(node->kind), node->id,
                                 node->trap_code);
        }
    } else if (node->trap_code != NULL) {
        verify_add_violation(v, node, 9, IR_INV_TRAP,
                             "%s id %lld has no runtime failure mode but "
                             "carries trap code '%s'",
                             ir_kind_text(node->kind), node->id,
                             node->trap_code);
    }
}

static void verify_kind(IrVerify *v, const IrNode *node, Encl *encl)
{
    const IrNode *fn;
    size_t i;
    switch (node->kind) {

    /* --- module structure --- */

    case IR_MODULE:
        for (i = 0; i < node->u.module.nimports; i++) {
            const IrNode *c = node->u.module.imports[i];
            if (c == NULL) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_MODULE id %lld has a NULL import "
                                     "entry", node->id);
            } else if (c->kind != IR_IMPORT) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_MODULE id %lld import entry is %s, "
                                     "expected IR_IMPORT",
                                     node->id, ir_kind_text(c->kind));
            } else {
                verify_child(v, c, encl);
            }
        }
        for (i = 0; i < node->u.module.ndecls; i++) {
            const IrNode *c = node->u.module.decls[i];
            if (c == NULL) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_MODULE id %lld has a NULL "
                                     "declaration entry", node->id);
            } else if (!ir_kind_is_decl(c->kind)) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_MODULE id %lld declaration entry is "
                                     "%s, expected a declaration",
                                     node->id, ir_kind_text(c->kind));
            } else {
                verify_child(v, c, encl);
            }
        }
        break;

    case IR_IMPORT:
        if (node->u.import.name == NULL || node->u.import.name[0] == '\0') {
            verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                 "IR_IMPORT id %lld has no module name",
                                 node->id);
        }
        break;

    case IR_STRUCT_DECL:
        if (node->u.struct_decl.name == NULL ||
            node->u.struct_decl.name[0] == '\0') {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_STRUCT_DECL id %lld has no name",
                                 node->id);
        }
        if (node->u.struct_decl.size < 0) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_STRUCT_DECL id %lld has negative size",
                                 node->id);
        }
        if (node->u.struct_decl.align < 1) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_STRUCT_DECL id %lld has alignment < 1",
                                 node->id);
        }
        for (i = 0; i < node->u.struct_decl.nfields; i++) {
            const IrField *f = &node->u.struct_decl.fields[i];
            int64_t fsize = (f->type != NULL) ? f->type->size : 0;
            if (f->name == NULL || f->name[0] == '\0') {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_STRUCT_DECL id %lld field %zu has no "
                                     "name", node->id, i);
            }
            if (f->type == NULL) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_STRUCT_DECL id %lld field %zu has no "
                                     "type", node->id, i);
            } else if (f->type->kind == IRT_VOID) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_STRUCT_DECL id %lld field %zu has "
                                     "void type", node->id, i);
            }
            if (!span_valid(f->span)) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_STRUCT_DECL id %lld field %zu has no "
                                     "valid span", node->id, i);
            }
            if (f->byte_offset < 0) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_STRUCT_DECL id %lld field %zu has a "
                                     "negative byte offset",
                                     node->id, i);
            } else if (f->byte_offset > node->u.struct_decl.size ||
                       (fsize > 0 &&
                        f->byte_offset > node->u.struct_decl.size - fsize)) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_STRUCT_DECL id %lld field %zu "
                                     "exceeds the struct size",
                                     node->id, i);
            }
            if (i > 0) {
                const IrField *prev = &node->u.struct_decl.fields[i - 1];
                if (prev->type != NULL &&
                    f->byte_offset < prev->byte_offset + prev->type->size) {
                    verify_add_violation(v, node, 3, IR_INV_TYPE,
                                         "IR_STRUCT_DECL id %lld field %zu "
                                         "overlaps the previous field",
                                         node->id, i);
                }
            }
        }
        break;

    case IR_ENUM_DECL:
        if (node->u.enum_decl.name == NULL ||
            node->u.enum_decl.name[0] == '\0') {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_ENUM_DECL id %lld has no name", node->id);
        }
        if (node->u.enum_decl.underlying == NULL ||
            !is_int_type(node->u.enum_decl.underlying)) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_ENUM_DECL id %lld underlying type is not "
                                 "an integer type", node->id);
        }
        for (i = 0; i < node->u.enum_decl.nmembers; i++) {
            const IrEnumMember *m = &node->u.enum_decl.members[i];
            if (m->name == NULL || m->name[0] == '\0') {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_ENUM_DECL id %lld member %zu has no "
                                     "name", node->id, i);
            }
            if (!span_valid(m->span)) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_ENUM_DECL id %lld member %zu has no "
                                     "valid span", node->id, i);
            }
        }
        break;

    case IR_GLOBAL_CONST:
        if (node->u.global_const.name == NULL ||
            node->u.global_const.name[0] == '\0') {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_CONST id %lld has no name",
                                 node->id);
        }
        if (node->u.global_const.type == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_CONST id %lld has no type",
                                 node->id);
        }
        if (node->u.global_const.value == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_CONST id %lld has no constant "
                                 "value", node->id);
        } else if (node->u.global_const.type != NULL &&
                   !ir_type_identical(node->u.global_const.value->type,
                                      node->u.global_const.type)) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_CONST id %lld value type does not "
                                 "match the declared type", node->id);
        }
        break;

    case IR_GLOBAL_VAR:
        if (node->u.global_var.name == NULL ||
            node->u.global_var.name[0] == '\0') {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_VAR id %lld has no name",
                                 node->id);
        }
        if (node->u.global_var.type == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_VAR id %lld has no type",
                                 node->id);
        }
        if (node->u.global_var.init == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_VAR id %lld has no constant "
                                 "initializer (spec sec. 8.2; accepted "
                                 "programs always carry one)", node->id);
        } else if (node->u.global_var.type != NULL &&
                   !ir_type_identical(node->u.global_var.init->type,
                                      node->u.global_var.type)) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_GLOBAL_VAR id %lld initializer type does "
                                 "not match the declared type", node->id);
        }
        break;

    case IR_FUNCTION: {
        const IrNode *body = node->u.function.body;
        if (node->u.function.name == NULL ||
            node->u.function.name[0] == '\0') {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_FUNCTION id %lld has no name", node->id);
        }
        if (node->u.function.ret_type == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_FUNCTION id %lld has no return type",
                                 node->id);
        }
        if (node->u.function.noreturn &&
            node->u.function.name != NULL &&
            strcmp(node->u.function.name, "rt.proc.exit") != 0 &&
            strcmp(node->u.function.name, "rt.trap.report") != 0) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_FUNCTION id %lld carries the noreturn "
                                 "flag but is not rt.proc.exit or "
                                 "rt.trap.report", node->id);
        }
        if (body == NULL) {
            if (!node->u.function.noreturn) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld has no body block",
                                     node->id);
            }
        } else if (body->kind != IR_BLOCK) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_FUNCTION id %lld body is not an IR_BLOCK",
                                 node->id);
        }
        if (node->u.function.nslots < node->u.function.nparams) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_FUNCTION id %lld slot table (%zu) is "
                                 "smaller than the parameter list (%zu)",
                                 node->id, node->u.function.nslots,
                                 node->u.function.nparams);
        }
        for (i = 0; i < node->u.function.nparams; i++) {
            const IrParam *p = &node->u.function.params[i];
            const IrSlot *s = (i < node->u.function.nslots)
                                  ? node->u.function.slots[i] : NULL;
            if (p->name == NULL || p->name[0] == '\0') {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld parameter %zu has "
                                     "no name", node->id, i);
            }
            if (p->type == NULL || p->type->kind == IRT_VOID) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld parameter %zu has "
                                     "no usable type", node->id, i);
            }
            if (!span_valid(p->span)) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld parameter %zu has no "
                                     "valid span", node->id, i);
            }
            if (p->slot_index != (int64_t)i) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld parameter %zu slot "
                                     "index %lld does not match its position",
                                     node->id, i, p->slot_index);
            }
            if (s == NULL || s->kind != IR_SLOT_PARAM || s->index != (int64_t)i) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld slot %zu is not the "
                                     "matching parameter slot", node->id, i);
            }
        }
        for (i = 0; i < node->u.function.nslots; i++) {
            const IrSlot *s = node->u.function.slots[i];
            if (s == NULL) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld has a NULL slot "
                                     "entry at %zu", node->id, i);
                continue;
            }
            if (s->index != (int64_t)i) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld slot %zu has index "
                                     "%lld", node->id, i, s->index);
            }
            if (i < node->u.function.nparams) {
                if (s->kind != IR_SLOT_PARAM) {
                    verify_add_violation(v, node, 3, IR_INV_TYPE,
                                         "IR_FUNCTION id %lld slot %zu is a "
                                         "parameter slot but has kind %d",
                                         node->id, i, (int)s->kind);
                }
            } else if (s->kind != IR_SLOT_LOCAL && s->kind != IR_SLOT_TEMP) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld slot %zu is neither "
                                     "local nor temporary", node->id, i);
            }
            if (s->type == NULL || s->type->kind == IRT_VOID) {
                verify_add_violation(v, node, 3, IR_INV_TYPE,
                                     "IR_FUNCTION id %lld slot %zu has no "
                                     "usable type", node->id, i);
            }
        }
        /* invariant 5: non-void function tails terminate */
        if (body != NULL && body->kind == IR_BLOCK &&
            node->u.function.ret_type != NULL &&
            node->u.function.ret_type->kind != IRT_VOID &&
            !stmt_seq_terminates(body->u.block.stmts,
                                 body->u.block.nstmts)) {
            verify_add_violation(v, node, 5, IR_INV_TERM,
                                 "non-void function '%s' tail does not "
                                 "terminate (every reachable path must end in "
                                 "IR_RETURN / IR_CALL_TERM / IR_TRAP)",
                                 node->u.function.name != NULL
                                     ? node->u.function.name : "");
        }
        fn = v->fn;
        v->fn = node;
        verify_child(v, body, encl);
        v->fn = fn;
        break;
    }

    /* --- statements --- */

    case IR_BLOCK:
        for (i = 0; i < node->u.block.nstmts; i++) {
            const IrNode *s = node->u.block.stmts[i];
            if (s == NULL) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_BLOCK id %lld has a NULL statement at "
                                     "index %zu", node->id, i);
            } else if (i + 1 < node->u.block.nstmts &&
                       ir_kind_is_terminator(s->kind)) {
                verify_add_violation(v, node, 5, IR_INV_TERM,
                                     "%s id %lld is a terminator but is not "
                                     "the last statement of IR_BLOCK id %lld",
                                     ir_kind_text(s->kind), s->id, node->id);
            } else {
                verify_child(v, s, encl);
            }
        }
        break;

    case IR_LOCAL_DECL: {
        const IrSlot *slot = NULL;
        if (v->fn == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL_DECL id %lld appears outside a "
                                 "function", node->id);
        } else if (node->u.local_decl.slot_index < 0 ||
                   (size_t)node->u.local_decl.slot_index >=
                       v->fn->u.function.nslots) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL_DECL id %lld references invalid "
                                 "slot %lld", node->id,
                                 node->u.local_decl.slot_index);
        } else {
            slot = v->fn->u.function.slots[node->u.local_decl.slot_index];
        }
        if (node->u.local_decl.init == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL_DECL id %lld has no initializer "
                                 "(no uninitialized state; spec sec. 8.2)",
                                 node->id);
        } else if (slot != NULL &&
                   !ir_type_identical(node->u.local_decl.init->type,
                                      slot->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL_DECL id %lld initializer type does "
                                 "not match slot %lld type", node->id,
                                 node->u.local_decl.slot_index);
        }
        verify_child(v, node->u.local_decl.init, encl);
        break;
    }

    case IR_IF:
        if (node->u.if_stmt.cond == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_IF id %lld has no condition", node->id);
        } else if (node->u.if_stmt.cond->type == NULL ||
                   node->u.if_stmt.cond->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_IF id %lld condition is not bool-typed",
                                 node->id);
        }
        if (node->u.if_stmt.then_block == NULL ||
            node->u.if_stmt.then_block->kind != IR_BLOCK) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_IF id %lld then-body is not an IR_BLOCK",
                                 node->id);
        }
        if (node->u.if_stmt.else_block != NULL &&
            node->u.if_stmt.else_block->kind != IR_BLOCK) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_IF id %lld else-body is not an IR_BLOCK",
                                 node->id);
        }
        verify_child(v, node->u.if_stmt.cond, encl);
        verify_child(v, node->u.if_stmt.then_block, encl);
        verify_child(v, node->u.if_stmt.else_block, encl);
        break;

    case IR_WHILE:
        if (node->u.while_stmt.cond == NULL ||
            node->u.while_stmt.cond->type == NULL ||
            node->u.while_stmt.cond->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_WHILE id %lld condition is not "
                                 "bool-typed", node->id);
        }
        if (node->u.while_stmt.body == NULL ||
            node->u.while_stmt.body->kind != IR_BLOCK) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_WHILE id %lld body is not an IR_BLOCK",
                                 node->id);
        }
        encl_push(encl, node, &v->oom);
        verify_child(v, node->u.while_stmt.cond, encl);
        verify_child(v, node->u.while_stmt.body, encl);
        encl_pop(encl);
        break;

    case IR_FOR:
        if (node->u.for_stmt.init != NULL &&
            !ir_kind_is_stmt(node->u.for_stmt.init->kind)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FOR id %lld init is not a statement",
                                 node->id);
        }
        if (node->u.for_stmt.step != NULL &&
            !ir_kind_is_expr(node->u.for_stmt.step->kind)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FOR id %lld step is not an expression",
                                 node->id);
        }
        if (node->u.for_stmt.cond != NULL &&
            (node->u.for_stmt.cond->type == NULL ||
             node->u.for_stmt.cond->type->kind != IRT_BOOL)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FOR id %lld condition is not bool-typed",
                                 node->id);
        }
        if (node->u.for_stmt.body == NULL ||
            node->u.for_stmt.body->kind != IR_BLOCK) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FOR id %lld body is not an IR_BLOCK",
                                 node->id);
        }
        encl_push(encl, node, &v->oom);
        verify_child(v, node->u.for_stmt.init, encl);
        verify_child(v, node->u.for_stmt.cond, encl);
        verify_child(v, node->u.for_stmt.step, encl);
        verify_child(v, node->u.for_stmt.body, encl);
        encl_pop(encl);
        break;

    case IR_SWITCH: {
        const IrType *sel = (node->u.switch_stmt.selector != NULL)
                                ? node->u.switch_stmt.selector->type : NULL;
        if (node->u.switch_stmt.selector == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SWITCH id %lld has no selector",
                                 node->id);
        } else if (sel == NULL ||
                   (!is_int_type(sel) && sel->kind != IRT_ENUM)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SWITCH id %lld selector type is not "
                                 "integer or enum", node->id);
        }
        for (i = 0; i < node->u.switch_stmt.ncases; i++) {
            const IrNode *c = node->u.switch_stmt.cases[i];
            if (c == NULL) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_SWITCH id %lld has a NULL case entry",
                                     node->id);
            } else if (c->kind != IR_CASE) {
                verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                     "IR_SWITCH id %lld case entry is %s, "
                                     "expected IR_CASE",
                                     node->id, ir_kind_text(c->kind));
            } else if (c->u.case_clause.value != NULL && sel != NULL &&
                       !ir_type_identical(c->u.case_clause.value->type,
                                          sel)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_SWITCH id %lld case %zu value type "
                                     "does not match the selector type",
                                     node->id, i);
            }
        }
        if (node->u.switch_stmt.default_clause != NULL &&
            node->u.switch_stmt.default_clause->kind != IR_DEFAULT) {
            verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                 "IR_SWITCH id %lld default entry is %s, "
                                 "expected IR_DEFAULT",
                                 node->id,
                                 ir_kind_text(
                                     node->u.switch_stmt.default_clause->kind));
        }
        encl_push(encl, node, &v->oom);
        verify_child(v, node->u.switch_stmt.selector, encl);
        for (i = 0; i < node->u.switch_stmt.ncases; i++) {
            verify_child(v, node->u.switch_stmt.cases[i], encl);
        }
        verify_child(v, node->u.switch_stmt.default_clause, encl);
        encl_pop(encl);
        break;
    }

    case IR_CASE:
        if (node->u.case_clause.value == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_CASE id %lld has no constant value",
                                 node->id);
        }
        if (node->u.case_clause.body == NULL ||
            node->u.case_clause.body->kind != IR_BLOCK) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_CASE id %lld body is not an IR_BLOCK",
                                 node->id);
        } else if (node->u.case_clause.body->u.block.nstmts == 0 ||
                   !ir_kind_is_terminator(
                       node->u.case_clause.body->u.block.stmts[
                           node->u.case_clause.body->u.block.nstmts - 1]
                           ->kind)) {
            verify_add_violation(v, node, 6, IR_INV_FALL,
                                 "IR_CASE id %lld body does not end in a "
                                 "terminator (no fall-through)", node->id);
        }
        verify_child(v, node->u.case_clause.body, encl);
        break;

    case IR_DEFAULT:
        if (node->u.default_clause.body == NULL ||
            node->u.default_clause.body->kind != IR_BLOCK) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_DEFAULT id %lld body is not an IR_BLOCK",
                                 node->id);
        } else if (node->u.default_clause.body->u.block.nstmts == 0 ||
                   !ir_kind_is_terminator(
                       node->u.default_clause.body->u.block.stmts[
                           node->u.default_clause.body->u.block.nstmts - 1]
                           ->kind)) {
            verify_add_violation(v, node, 6, IR_INV_FALL,
                                 "IR_DEFAULT id %lld body does not end in a "
                                 "terminator (no fall-through)", node->id);
        }
        verify_child(v, node->u.default_clause.body, encl);
        break;

    case IR_BREAK:
        if (node->u.break_stmt.target == NULL) {
            verify_add_violation(v, node, 7, IR_INV_PLACE,
                                 "IR_BREAK id %lld has no target", node->id);
        } else if (!encl_contains(encl, node->u.break_stmt.target)) {
            verify_add_violation(v, node, 7, IR_INV_PLACE,
                                 "IR_BREAK id %lld targets a construct that "
                                 "does not enclose it", node->id);
        } else if (node->u.break_stmt.target->kind != IR_SWITCH &&
                   node->u.break_stmt.target->kind != IR_WHILE &&
                   node->u.break_stmt.target->kind != IR_FOR) {
            verify_add_violation(v, node, 7, IR_INV_PLACE,
                                 "IR_BREAK id %lld targets %s, expected an "
                                 "enclosing IR_SWITCH / IR_WHILE / IR_FOR",
                                 node->id,
                                 ir_kind_text(
                                     node->u.break_stmt.target->kind));
        }
        break;

    case IR_CONTINUE:
        if (node->u.continue_stmt.target == NULL) {
            verify_add_violation(v, node, 7, IR_INV_PLACE,
                                 "IR_CONTINUE id %lld has no target",
                                 node->id);
        } else if (!encl_contains(encl, node->u.continue_stmt.target)) {
            verify_add_violation(v, node, 7, IR_INV_PLACE,
                                 "IR_CONTINUE id %lld targets a construct "
                                 "that does not enclose it", node->id);
        } else if (node->u.continue_stmt.target->kind != IR_WHILE &&
                   node->u.continue_stmt.target->kind != IR_FOR) {
            verify_add_violation(v, node, 7, IR_INV_PLACE,
                                 "IR_CONTINUE id %lld targets %s, expected an "
                                 "enclosing IR_WHILE / IR_FOR",
                                 node->id,
                                 ir_kind_text(
                                     node->u.continue_stmt.target->kind));
        }
        break;

    case IR_RETURN: {
        const IrType *ret = (v->fn != NULL)
                                ? v->fn->u.function.ret_type : NULL;
        if (v->fn == NULL) {
            verify_add_violation(v, node, 8, IR_INV_RET,
                                 "IR_RETURN id %lld appears outside a "
                                 "function", node->id);
        } else if (ret != NULL && ret->kind == IRT_VOID) {
            if (node->u.return_stmt.value != NULL) {
                verify_add_violation(v, node, 8, IR_INV_RET,
                                     "IR_RETURN id %lld in a void function "
                                     "carries a value", node->id);
            }
        } else {
            if (node->u.return_stmt.value == NULL) {
                verify_add_violation(v, node, 8, IR_INV_RET,
                                     "IR_RETURN id %lld in a non-void "
                                     "function carries no value", node->id);
            } else if (ret != NULL &&
                       !ir_type_identical(node->u.return_stmt.value->type,
                                          ret)) {
                verify_add_violation(v, node, 8, IR_INV_RET,
                                     "IR_RETURN id %lld value type does not "
                                     "match the function return type",
                                     node->id);
            }
        }
        verify_child(v, node->u.return_stmt.value, encl);
        break;
    }

    case IR_EXPR_STMT:
        if (node->u.expr_stmt.expr == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_EXPR_STMT id %lld has no expression",
                                 node->id);
        } else if (!ir_kind_is_expr(node->u.expr_stmt.expr->kind)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_EXPR_STMT id %lld child is %s, not an "
                                 "expression", node->id,
                                 ir_kind_text(node->u.expr_stmt.expr->kind));
        }
        verify_child(v, node->u.expr_stmt.expr, encl);
        break;

    case IR_EMPTY:
        break;

    case IR_CALL_TERM:
        if (node->u.call_term.callee == NULL ||
            node->u.call_term.callee->kind != IR_FUNCTION) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_CALL_TERM id %lld callee is not an "
                                 "IR_FUNCTION", node->id);
        } else {
            const IrNode *callee = node->u.call_term.callee;
            if (!callee->u.function.noreturn) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_CALL_TERM id %lld calls a "
                                     "non-noreturn function", node->id);
            }
            if (node->u.call_term.nargs != callee->u.function.nparams) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_CALL_TERM id %lld argument count "
                                     "%zu does not match callee parameter "
                                     "count %zu",
                                     node->id, node->u.call_term.nargs,
                                     callee->u.function.nparams);
            }
            for (i = 0; i < node->u.call_term.nargs &&
                            i < callee->u.function.nparams; i++) {
                const IrNode *arg = node->u.call_term.args[i];
                if (arg == NULL) {
                    verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                         "IR_CALL_TERM id %lld has a NULL "
                                         "argument at index %zu",
                                         node->id, i);
                } else if (callee->u.function.params[i].type != NULL &&
                           !ir_type_identical(
                               arg->type,
                               callee->u.function.params[i].type)) {
                    verify_add_violation(v, node, 4, IR_INV_TYPING,
                                         "IR_CALL_TERM id %lld argument %zu "
                                         "type does not match the callee "
                                         "parameter type", node->id, i);
                }
            }
        }
        /* The callee is a reference edge, verified through its own module
         * declaration; recursing into it here would double-verify and
         * infinitely recurse on recursive functions (spec sec. 13.4). */
        for (i = 0; i < node->u.call_term.nargs; i++) {
            verify_child(v, node->u.call_term.args[i], encl);
        }
        break;

    case IR_TRAP: {
        bool has_code = (node->u.trap.code != NULL);
        if (has_code == node->u.trap.has_user_code) {
            verify_add_violation(v, node, 9, IR_INV_TRAP,
                                 "IR_TRAP id %lld must carry exactly one of a "
                                 "registry trap code or a numeric user trap "
                                 "code", node->id);
        } else if (has_code) {
            if (diag_code_lookup(node->u.trap.code) == NULL) {
                verify_add_violation(v, node, 9, IR_INV_TRAP,
                                     "IR_TRAP id %lld registry code '%s' is "
                                     "not in the diagnostic registry",
                                     node->id, node->u.trap.code);
            }
        } else if (node->u.trap.user_code < 0 ||
                   (uint64_t)node->u.trap.user_code > UINT32_MAX) {
            verify_add_violation(v, node, 9, IR_INV_TRAP,
                                 "IR_TRAP id %lld user trap code %lld is out "
                                 "of u32 range",
                                 node->id, node->u.trap.user_code);
        }
        break;
    }

    /* --- value-producing nodes --- */

    case IR_INT:
        if (node->u.constant.value == NULL) {
            verify_add_violation(v, node, 3, IR_INV_TYPE,
                                 "IR_INT id %lld has no constant value",
                                 node->id);
        } else if (node->u.constant.value->kind != IRC_INT) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INT id %lld constant is not an integer "
                                 "constant", node->id);
        }
        if (!is_int_type(node->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INT id %lld result type is not an "
                                 "integer type", node->id);
        }
        if (node->u.constant.value != NULL &&
            node->u.constant.value->kind == IRC_INT &&
            node->type != NULL &&
            !ir_type_identical(node->u.constant.value->type, node->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INT id %lld constant type does not match "
                                 "the result type", node->id);
        }
        break;

    case IR_BOOL:
        if (node->u.constant.value == NULL ||
            node->u.constant.value->kind != IRC_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_BOOL id %lld constant is not a boolean "
                                 "constant", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_BOOL id %lld result type is not bool",
                                 node->id);
        }
        break;

    case IR_NULL:
        if (node->type == NULL || node->type->kind != IRT_PTR) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_NULL id %lld result type is not a "
                                 "pointer type", node->id);
        }
        break;

    case IR_STR:
        if (node->u.constant.value == NULL ||
            node->u.constant.value->kind != IRC_STR) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_STR id %lld constant is not a string "
                                 "constant", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_STR) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_STR id %lld result type is not str",
                                 node->id);
        }
        break;

    case IR_ENUM_VAL:
        if (node->u.constant.value == NULL ||
            node->u.constant.value->kind != IRC_ENUM) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_ENUM_VAL id %lld constant is not an enum "
                                 "constant", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_ENUM) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_ENUM_VAL id %lld result type is not an "
                                 "enum type", node->id);
        }
        break;

    case IR_LOCAL: {
        const IrSlot *slot = NULL;
        if (v->fn == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL id %lld appears outside a "
                                 "function", node->id);
        } else if (node->u.local.slot_index < 0 ||
                   (size_t)node->u.local.slot_index >=
                       v->fn->u.function.nslots) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL id %lld references invalid slot "
                                 "%lld", node->id, node->u.local.slot_index);
        } else {
            slot = v->fn->u.function.slots[node->u.local.slot_index];
        }
        if (slot != NULL &&
            !ir_type_identical(node->type, slot->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOCAL id %lld type does not match slot "
                                 "%lld type",
                                 node->id, node->u.local.slot_index);
        }
        break;
    }

    case IR_GLOBAL:
        if (node->u.global.target == NULL ||
            node->u.global.target->kind != IR_GLOBAL_VAR) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_GLOBAL id %lld target is not an "
                                 "IR_GLOBAL_VAR (consts have no addressable "
                                 "node)", node->id);
        } else if (node->type != NULL &&
                   !ir_type_identical(
                       node->type,
                       node->u.global.target->u.global_var.type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_GLOBAL id %lld type does not match the "
                                 "global's type", node->id);
        }
        break;

    case IR_FIELD_ADDR: {
        const IrType *btype = (node->u.field_addr.base != NULL)
                                  ? node->u.field_addr.base->type : NULL;
        const IrNode *decl = (btype != NULL && btype->kind == IRT_STRUCT)
                                 ? btype->u.decl : NULL;
        if (node->u.field_addr.base == NULL ||
            !is_lvalue(node->u.field_addr.base)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FIELD_ADDR id %lld base is not an "
                                 "lvalue", node->id);
        } else if (btype == NULL || btype->kind != IRT_STRUCT) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FIELD_ADDR id %lld base type is not a "
                                 "struct", node->id);
        } else if (decl == NULL || decl->kind != IR_STRUCT_DECL ||
                   node->u.field_addr.field_index < 0 ||
                   (size_t)node->u.field_addr.field_index >=
                       decl->u.struct_decl.nfields) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FIELD_ADDR id %lld field index %lld out "
                                 "of range",
                                 node->id, node->u.field_addr.field_index);
        } else if (node->type == NULL || node->type->kind != IRT_PTR ||
                   !ir_type_identical(
                       node->type->u.ptr.elem,
                       decl->u.struct_decl
                           .fields[node->u.field_addr.field_index].type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_FIELD_ADDR id %lld result type is not "
                                 "U* of the field type", node->id);
        }
        verify_child(v, node->u.field_addr.base, encl);
        break;
    }

    case IR_INDEX_ADDR: {
        const IrType *btype = (node->u.index_addr.base != NULL)
                                  ? node->u.index_addr.base->type : NULL;
        const IrType *elem = NULL;
        if (node->u.index_addr.base == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INDEX_ADDR id %lld has no base",
                                 node->id);
        } else if (btype == NULL ||
                   (btype->kind != IRT_ARRAY && btype->kind != IRT_SLICE &&
                    btype->kind != IRT_STR)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INDEX_ADDR id %lld base type is not "
                                 "array/slice/str", node->id);
        } else {
            /* contract sec. 5.3: the operand is "array/slice lvalue or
             * `str` value". An array/slice base must itself be a mutable
             * lvalue; a `str` base is a value (its element address is a
             * value address, never an lvalue). */
            if ((btype->kind == IRT_ARRAY || btype->kind == IRT_SLICE) &&
                !is_lvalue(node->u.index_addr.base)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_INDEX_ADDR id %lld base is not an "
                                     "lvalue (array/slice base must be a "
                                     "mutable lvalue)", node->id);
            }
            if (btype->kind == IRT_ARRAY) {
                elem = btype->u.array.elem;
            } else if (btype->kind == IRT_SLICE) {
                elem = btype->u.slice.elem;
            } else {
                elem = ir_type_u8((IrBuild *)v->build);
            }
        }
        if (node->u.index_addr.index == NULL ||
            node->u.index_addr.index->type == NULL ||
            node->u.index_addr.index->type->kind != IRT_USIZE) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INDEX_ADDR id %lld index is not "
                                 "usize-typed", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_PTR ||
            elem == NULL ||
            !ir_type_identical(node->type->u.ptr.elem, elem)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_INDEX_ADDR id %lld result type is not "
                                 "T* of the element type", node->id);
        }
        verify_child(v, node->u.index_addr.base, encl);
        verify_child(v, node->u.index_addr.index, encl);
        break;
    }

    case IR_DEREF:
        if (node->u.deref.ptr == NULL ||
            node->u.deref.ptr->type == NULL ||
            node->u.deref.ptr->type->kind != IRT_PTR) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_DEREF id %lld operand is not a pointer",
                                 node->id);
        } else if (node->type == NULL ||
                   !ir_type_identical(
                       node->type,
                       node->u.deref.ptr->type->u.ptr.elem)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_DEREF id %lld result type does not match "
                                 "the pointed-to type", node->id);
        }
        verify_child(v, node->u.deref.ptr, encl);
        break;

    case IR_LOAD: {
        const IrNode *lv = node->u.load.lvalue;
        const IrType *vt = lvalue_value_type(lv);
        bool lv_ok = (lv != NULL) &&
                     (is_lvalue(lv) ||
                      (lv->kind == IR_INDEX_ADDR &&
                       lv->u.index_addr.base != NULL &&
                       lv->u.index_addr.base->type != NULL &&
                       lv->u.index_addr.base->type->kind == IRT_STR));
        if (!lv_ok) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOAD id %lld operand is not an lvalue",
                                 node->id);
        } else if (!is_scalar_type(node->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOAD id %lld result type is not scalar",
                                 node->id);
        } else if (!ir_type_identical(node->type, vt)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LOAD id %lld result type does not match "
                                 "the lvalue's value type", node->id);
        }
        verify_child(v, lv, encl);
        break;
    }

    case IR_STORE: {
        const IrNode *dest = node->u.store.dest;
        const IrType *vt = lvalue_value_type(dest);
        if (dest == NULL || !is_lvalue(dest)) {
            verify_add_violation(v, node, 10, IR_INV_STORE,
                                 "IR_STORE id %lld destination is not an "
                                 "lvalue (consts and str element addresses "
                                 "are unrepresentable store targets)",
                                 node->id);
        }
        if (node->u.store.value == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_STORE id %lld has no value operand",
                                 node->id);
        } else if (dest != NULL && vt != NULL &&
                   !ir_type_identical(node->u.store.value->type, vt)) {
            verify_add_violation(v, node, 10, IR_INV_STORE,
                                 "IR_STORE id %lld value type does not match "
                                 "the destination's value type (complete "
                                 "object representation)", node->id);
        }
        verify_child(v, dest, encl);
        verify_child(v, node->u.store.value, encl);
        break;
    }

    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else {
            if (!is_int_type(node->u.binary.left->type) ||
                !is_int_type(node->u.binary.right->type)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "%s id %lld operands must be "
                                     "integer-typed",
                                     ir_kind_text(node->kind), node->id);
            }
        }
        if (!is_int_type(node->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type is not an integer "
                                 "type", ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;

    case IR_SHL: case IR_SHR:
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else {
            if (!is_int_type(node->u.binary.left->type) ||
                !is_int_type(node->u.binary.right->type)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "%s id %lld operands must be "
                                     "integer-typed",
                                     ir_kind_text(node->kind), node->id);
            }
        }
        if (!is_int_type(node->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type is not an integer "
                                 "type", ir_kind_text(node->kind), node->id);
        }
        if (node->u.binary.left != NULL &&
            !ir_type_identical(node->type, node->u.binary.left->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type does not match the "
                                 "left operand's type (contract sec. 5.3)",
                                 ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;

    case IR_BAND: case IR_BOR: case IR_BXOR:
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else if (!is_int_type(node->u.binary.left->type) ||
                   !is_int_type(node->u.binary.right->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld operands must be integer-typed",
                                 ir_kind_text(node->kind), node->id);
        }
        if (!is_int_type(node->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type is not an integer "
                                 "type", ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;

    case IR_LAND: case IR_LOR:
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else {
            const IrType *lt = node->u.binary.left->type;
            const IrType *rt = node->u.binary.right->type;
            if (lt == NULL || lt->kind != IRT_BOOL ||
                rt == NULL || rt->kind != IRT_BOOL) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "%s id %lld operands must be bool-typed",
                                     ir_kind_text(node->kind), node->id);
            }
        }
        if (node->type == NULL || node->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type is not bool",
                                 ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;

    case IR_EQ: case IR_NE: {
        const IrType *lt = (node->u.binary.left != NULL)
                               ? node->u.binary.left->type : NULL;
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else if (!ir_type_identical(lt, node->u.binary.right->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld operands are not same-typed",
                                 ir_kind_text(node->kind), node->id);
        } else if (lt == NULL ||
                   !(is_int_type(lt) || lt->kind == IRT_BOOL ||
                     lt->kind == IRT_PTR || lt->kind == IRT_ENUM ||
                     lt->kind == IRT_STR)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld operand type is not equality-"
                                 "eligible", ir_kind_text(node->kind),
                                 node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type is not bool",
                                 ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;
    }

    case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
        const IrType *lt = (node->u.binary.left != NULL)
                               ? node->u.binary.left->type : NULL;
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else if (!ir_type_identical(lt, node->u.binary.right->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld operands are not same-typed",
                                 ir_kind_text(node->kind), node->id);
        } else if (lt == NULL ||
                   !(is_int_type(lt) || lt->kind == IRT_ENUM ||
                     lt->kind == IRT_STR || lt->kind == IRT_PTR)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld operand type is not totally "
                                 "ordered", ir_kind_text(node->kind), node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type is not bool",
                                 ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;
    }

    case IR_SLICE_EQ: {
        const IrType *lt = (node->u.binary.left != NULL)
                               ? node->u.binary.left->type : NULL;
        const IrType *rt = (node->u.binary.right != NULL)
                               ? node->u.binary.right->type : NULL;
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_SLICE_EQ id %lld is missing an operand",
                                 node->id);
        } else if (lt == NULL || rt == NULL || lt->kind != IRT_SLICE ||
                   rt->kind != IRT_SLICE ||
                   !ir_type_identical(lt->u.slice.elem, rt->u.slice.elem)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SLICE_EQ id %lld operands are not "
                                 "same-element slices", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SLICE_EQ id %lld result type is not "
                                 "bool", node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;
    }

    case IR_NEG:
        if (node->u.unary.operand == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_NEG id %lld has no operand", node->id);
        } else if (!is_signed_int_type(node->u.unary.operand->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_NEG id %lld operand is not a signed "
                                 "integer", node->id);
        }
        if (node->u.unary.operand != NULL && node->type != NULL &&
            !ir_type_identical(node->type, node->u.unary.operand->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_NEG id %lld result type does not match "
                                 "the operand type", node->id);
        }
        verify_child(v, node->u.unary.operand, encl);
        break;

    case IR_BNOT:
        if (node->u.unary.operand == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_BNOT id %lld has no operand", node->id);
        } else if (!is_int_type(node->u.unary.operand->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_BNOT id %lld operand is not an integer",
                                 node->id);
        }
        if (node->u.unary.operand != NULL && node->type != NULL &&
            !ir_type_identical(node->type, node->u.unary.operand->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_BNOT id %lld result type does not match "
                                 "the operand type", node->id);
        }
        verify_child(v, node->u.unary.operand, encl);
        break;

    case IR_LNOT:
        if (node->u.unary.operand == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_LNOT id %lld has no operand", node->id);
        } else if (node->u.unary.operand->type == NULL ||
                   node->u.unary.operand->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LNOT id %lld operand is not bool-typed",
                                 node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LNOT id %lld result type is not bool",
                                 node->id);
        }
        verify_child(v, node->u.unary.operand, encl);
        break;

    case IR_SELECT: {
        const IrType *tt = (node->u.select.then_value != NULL)
                               ? node->u.select.then_value->type : NULL;
        if (node->u.select.cond == NULL ||
            node->u.select.cond->type == NULL ||
            node->u.select.cond->type->kind != IRT_BOOL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SELECT id %lld condition is not "
                                 "bool-typed", node->id);
        }
        if (node->u.select.then_value == NULL ||
            node->u.select.else_value == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_SELECT id %lld is missing a branch",
                                 node->id);
        } else if (!ir_type_identical(tt,
                                      node->u.select.else_value->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SELECT id %lld branches have different "
                                 "types", node->id);
        }
        if (tt != NULL && node->type != NULL &&
            !ir_type_identical(node->type, tt)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SELECT id %lld result type does not "
                                 "match the branch type", node->id);
        }
        verify_child(v, node->u.select.cond, encl);
        verify_child(v, node->u.select.then_value, encl);
        verify_child(v, node->u.select.else_value, encl);
        break;
    }

    case IR_CALL: {
        const IrNode *callee = node->u.call.callee;
        if (callee == NULL || callee->kind != IR_FUNCTION) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_CALL id %lld callee is not an "
                                 "IR_FUNCTION", node->id);
        } else {
            if (node->type == NULL ||
                !ir_type_identical(node->type,
                                   callee->u.function.ret_type)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_CALL id %lld result type does not "
                                     "match the callee return type",
                                     node->id);
            }
            if (node->u.call.nargs != callee->u.function.nparams) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_CALL id %lld argument count %zu "
                                     "does not match callee parameter count "
                                     "%zu",
                                     node->id, node->u.call.nargs,
                                     callee->u.function.nparams);
            }
            for (i = 0; i < node->u.call.nargs &&
                            i < callee->u.function.nparams; i++) {
                const IrNode *arg = node->u.call.args[i];
                if (arg == NULL) {
                    verify_add_violation(v, node, 1, IR_INV_GRAPH,
                                         "IR_CALL id %lld has a NULL argument "
                                         "at index %zu", node->id, i);
                } else if (callee->u.function.params[i].type != NULL &&
                           !ir_type_identical(
                               arg->type,
                               callee->u.function.params[i].type)) {
                    verify_add_violation(v, node, 4, IR_INV_TYPING,
                                         "IR_CALL id %lld argument %zu type "
                                         "does not match the callee "
                                         "parameter type", node->id, i);
                }
            }
        }
        /* The callee is a reference edge, verified through its own module
         * declaration; recursing into it here would double-verify and
         * infinitely recurse on recursive functions (spec sec. 13.4). */
        for (i = 0; i < node->u.call.nargs; i++) {
            verify_child(v, node->u.call.args[i], encl);
        }
        break;
    }

    case IR_LEN: {
        const IrType *ot = (node->u.unary.operand != NULL)
                               ? node->u.unary.operand->type : NULL;
        if (node->u.unary.operand == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_LEN id %lld has no operand", node->id);
        } else if (ot == NULL ||
                   (ot->kind != IRT_ARRAY && ot->kind != IRT_SLICE &&
                    ot->kind != IRT_STR)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LEN id %lld operand is not "
                                 "array/slice/str", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_USIZE) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_LEN id %lld result type is not usize",
                                 node->id);
        }
        verify_child(v, node->u.unary.operand, encl);
        break;
    }

    case IR_PTR: {
        const IrType *ot = (node->u.unary.operand != NULL)
                               ? node->u.unary.operand->type : NULL;
        const IrType *elem = NULL;
        if (node->u.unary.operand == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_PTR id %lld has no operand", node->id);
        } else if (ot == NULL ||
                   (ot->kind != IRT_ARRAY && ot->kind != IRT_SLICE &&
                    ot->kind != IRT_STR)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_PTR id %lld operand is not "
                                 "array/slice/str", node->id);
        } else {
            if (ot->kind == IRT_ARRAY) {
                elem = ot->u.array.elem;
            } else if (ot->kind == IRT_SLICE) {
                elem = ot->u.slice.elem;
            } else {
                elem = ir_type_u8((IrBuild *)v->build);
            }
        }
        if (node->type == NULL || node->type->kind != IRT_PTR ||
            elem == NULL ||
            !ir_type_identical(node->type->u.ptr.elem, elem)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_PTR id %lld result type is not the "
                                 "element pointer type", node->id);
        }
        verify_child(v, node->u.unary.operand, encl);
        break;
    }

    case IR_SLICE: {
        const IrType *bt = (node->u.slice.base != NULL)
                               ? node->u.slice.base->type : NULL;
        if (node->u.slice.base == NULL) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SLICE id %lld has no base", node->id);
        } else if (bt == NULL ||
                   (bt->kind != IRT_ARRAY && bt->kind != IRT_SLICE &&
                    bt->kind != IRT_STR)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SLICE id %lld base type is not "
                                 "array/slice/str", node->id);
        }
        if (node->u.slice.start != NULL &&
            (node->u.slice.start->type == NULL ||
             node->u.slice.start->type->kind != IRT_USIZE)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SLICE id %lld start bound is not "
                                 "usize-typed", node->id);
        }
        if (node->u.slice.end != NULL &&
            (node->u.slice.end->type == NULL ||
             node->u.slice.end->type->kind != IRT_USIZE)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_SLICE id %lld end bound is not "
                                 "usize-typed", node->id);
        }
        if (bt != NULL && bt->kind == IRT_STR) {
            if (node->type == NULL || node->type->kind != IRT_STR) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_SLICE id %lld of a str base must "
                                     "have str result type", node->id);
            }
        } else {
            const IrType *elem = (bt != NULL && bt->kind == IRT_ARRAY)
                                     ? bt->u.array.elem
                                     : ((bt != NULL && bt->kind == IRT_SLICE)
                                            ? bt->u.slice.elem : NULL);
            if (node->type == NULL || node->type->kind != IRT_SLICE ||
                elem == NULL ||
                !ir_type_identical(node->type->u.slice.elem, elem)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "IR_SLICE id %lld result type is not the "
                                     "element slice type", node->id);
            }
        }
        verify_child(v, node->u.slice.base, encl);
        verify_child(v, node->u.slice.start, encl);
        verify_child(v, node->u.slice.end, encl);
        break;
    }

    case IR_CAST: case IR_WRAP:
        if (node->u.cast_wrap.value == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld has no operand",
                                 ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.cast_wrap.value, encl);
        break;

    case IR_PTR_ADD: case IR_PTR_SUB:
        if (node->u.ptr_arith.ptr == NULL ||
            node->u.ptr_arith.offset == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "%s id %lld is missing an operand",
                                 ir_kind_text(node->kind), node->id);
        } else {
            if (node->u.ptr_arith.ptr->type == NULL ||
                node->u.ptr_arith.ptr->type->kind != IRT_PTR) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "%s id %lld pointer operand is not a "
                                     "pointer type",
                                     ir_kind_text(node->kind), node->id);
            }
            if (!is_int_type(node->u.ptr_arith.offset->type)) {
                verify_add_violation(v, node, 4, IR_INV_TYPING,
                                     "%s id %lld offset operand is not an "
                                     "integer type",
                                     ir_kind_text(node->kind), node->id);
            }
        }
        if (node->u.ptr_arith.ptr != NULL && node->type != NULL &&
            !ir_type_identical(node->type, node->u.ptr_arith.ptr->type)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "%s id %lld result type does not match the "
                                 "pointer type",
                                 ir_kind_text(node->kind), node->id);
        }
        verify_child(v, node->u.ptr_arith.ptr, encl);
        verify_child(v, node->u.ptr_arith.offset, encl);
        break;

    case IR_PTR_DIFF: {
        const IrType *lt = (node->u.binary.left != NULL)
                               ? node->u.binary.left->type : NULL;
        const IrType *rt = (node->u.binary.right != NULL)
                               ? node->u.binary.right->type : NULL;
        if (node->u.binary.left == NULL || node->u.binary.right == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_PTR_DIFF id %lld is missing an operand",
                                 node->id);
        } else if (lt == NULL || rt == NULL || lt->kind != IRT_PTR ||
                   rt->kind != IRT_PTR ||
                   !ir_type_identical(lt->u.ptr.elem, rt->u.ptr.elem)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_PTR_DIFF id %lld operands are not "
                                 "same-element pointers", node->id);
        }
        if (node->type == NULL || node->type->kind != IRT_ISIZE) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_PTR_DIFF id %lld result type is not "
                                 "isize", node->id);
        }
        verify_child(v, node->u.binary.left, encl);
        verify_child(v, node->u.binary.right, encl);
        break;
    }

    case IR_ZERO: {
        const IrType *ot = (node->u.unary.operand != NULL)
                               ? node->u.unary.operand->type : NULL;
        if (node->u.unary.operand == NULL) {
            verify_add_violation(v, node, 11, IR_INV_ORDER,
                                 "IR_ZERO id %lld has no operand", node->id);
        } else if (!is_lvalue(node->u.unary.operand)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_ZERO id %lld operand is not an "
                                 "lvalue/object image", node->id);
        } else if (ot == NULL ||
                   (ot->kind != IRT_STRUCT && ot->kind != IRT_ARRAY)) {
            verify_add_violation(v, node, 4, IR_INV_TYPING,
                                 "IR_ZERO id %lld operand type is not a "
                                 "struct or array", node->id);
        }
        verify_child(v, node->u.unary.operand, encl);
        break;
    }

    default:
        verify_add_violation(v, node, 1, IR_INV_GRAPH,
                             "unknown node kind %d (id %lld)",
                             (int)node->kind, node->id);
        break;
    }
}

static void verify_node(IrVerify *v, const IrNode *node, Encl *encl)
{
    if (v->oom || node == NULL) {
        return;
    }
    verify_span_cause(v, node);
    verify_result_type(v, node);
    verify_trap_code(v, node);
    verify_kind(v, node, encl);
}

/* --- entry point --- */

IrStatus ir_core_verify(const IrBuild *build,
                        DiagRecord ***out_records, size_t *out_record_count)
{
    IrVerify v;
    size_t i, j;
    memset(&v, 0, sizeof(v));
    v.build = build;
    if (out_records != NULL) {
        *out_records = NULL;
    }
    if (out_record_count != NULL) {
        *out_record_count = 0;
    }

    v.nnodes = build->nnodes;
    if (build->nnodes > 0) {
        v.seen = (unsigned char *)calloc(build->nnodes, 1);
        v.reached = (unsigned char *)calloc(build->nnodes, 1);
        if (v.seen == NULL || v.reached == NULL) {
            free(v.seen);
            free(v.reached);
            return IR_OOM;
        }
    }

    /* pass 1: module structure, decl collection, reachability marking */
    for (i = 0; i < build->nmodules; i++) {
        const IrNode *m = build->modules[i];
        if (m == NULL) {
            verify_add_violation(&v, NULL, 1, IR_INV_GRAPH,
                                 "module list contains a NULL entry");
            continue;
        }
        if (m->kind != IR_MODULE) {
            verify_add_violation(&v, m, 1, IR_INV_GRAPH,
                                 "module list entry is %s, expected IR_MODULE",
                                 ir_kind_text(m->kind));
            continue;
        }
        if (m->u.module.name == NULL || m->u.module.name[0] == '\0') {
            verify_add_violation(&v, m, 1, IR_INV_GRAPH,
                                 "module has no name");
        } else {
            for (j = 0; j < v.nmodule_names; j++) {
                if (strcmp(v.module_names[j], m->u.module.name) == 0) {
                    verify_add_violation(&v, m, 1, IR_INV_GRAPH,
                                         "duplicate module name '%s'",
                                         m->u.module.name);
                    break;
                }
            }
            if (j == v.nmodule_names) {
                if (!ptr_array_append((void ***)&v.module_names,
                                      &v.nmodule_names,
                                      (void *)m->u.module.name)) {
                    v.oom = true;
                }
            }
        }
        pass1_mark(&v, m);
    }

    /* invariant 1: node ids unique, in range, gapless */
    for (i = 0; i < build->nnodes; i++) {
        const IrNode *n = build->nodes[i];
        if (n->id < 0 || (size_t)n->id >= build->nnodes) {
            verify_add_violation(&v, n, 1, IR_INV_GRAPH,
                                 "node id %lld out of range [0, %zu)",
                                 n->id, build->nnodes);
        } else if (v.seen[n->id]) {
            verify_add_violation(&v, n, 1, IR_INV_GRAPH,
                                 "duplicate node id %lld", n->id);
        } else {
            v.seen[n->id] = 1;
        }
    }

    /* invariant 1: every node reachable from its module root */
    for (i = 0; i < build->nnodes; i++) {
        const IrNode *n = build->nodes[i];
        if (n->id >= 0 && (size_t)n->id < build->nnodes &&
            !v.reached[n->id]) {
            verify_add_violation(&v, n, 1, IR_INV_GRAPH,
                                 "node not reachable from any module root");
        }
    }

    /* invariant 3: interned type descriptor well-formedness */
    for (i = 0; i < build->ntypes; i++) {
        const IrType *t = build->types[i];
        switch (t->kind) {
        case IRT_ARRAY:
            if (t->u.array.extent < 0) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "array type id %lld has negative extent "
                                     "%lld", t->id, t->u.array.extent);
            }
            if (t->u.array.elem == NULL) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "array type id %lld has no element type",
                                     t->id);
            }
            break;
        case IRT_SLICE:
            if (t->u.slice.elem == NULL) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "slice type id %lld has no element type",
                                     t->id);
            }
            break;
        case IRT_PTR:
            if (t->u.ptr.elem == NULL) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "pointer type id %lld has no element "
                                     "type", t->id);
            }
            break;
        case IRT_STRUCT:
        case IRT_ENUM: {
            const IrNode *decl = t->u.decl;
            bool in_set = false;
            if (decl == NULL) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "named type id %lld has no declaration "
                                     "reference", t->id);
                break;
            }
            if ((t->kind == IRT_STRUCT && decl->kind != IR_STRUCT_DECL) ||
                (t->kind == IRT_ENUM && decl->kind != IR_ENUM_DECL)) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "named type id %lld references %s, "
                                     "expected a %s declaration",
                                     t->id, ir_kind_text(decl->kind),
                                     t->kind == IRT_STRUCT
                                         ? "IR_STRUCT_DECL"
                                         : "IR_ENUM_DECL");
                break;
            }
            for (j = 0; j < v.ndecls; j++) {
                if (v.decls[j] == decl) {
                    in_set = true;
                    break;
                }
            }
            if (!in_set) {
                verify_add_violation(&v, NULL, 3, IR_INV_TYPE,
                                     "named type id %lld references a "
                                     "declaration not present in the build",
                                     t->id);
            }
            break;
        }
        default:
            break;
        }
    }

    /* invariant 10: IRConst_ADDR targets static storage only (consts have
     * no address; spec sec. 8.1) */
    for (i = 0; i < build->nconsts; i++) {
        const IrConst *c = build->consts[i];
        const IrNode *target;
        if (c->kind != IRC_ADDR) {
            continue;
        }
        if (c->type == NULL || c->type->kind != IRT_PTR) {
            verify_add_violation(&v, NULL, 10, IR_INV_STORE,
                                 "IRConst_ADDR id %lld type is not a pointer "
                                 "type", c->id);
        }
        target = c->u.addr.target;
        if (target == NULL) {
            verify_add_violation(&v, NULL, 10, IR_INV_STORE,
                                 "IRConst_ADDR id %lld has no target",
                                 c->id);
        } else if (target->kind == IR_GLOBAL_VAR) {
            /* static global slot: valid */
        } else if (target->kind == IR_INDEX_ADDR) {
            /* &arr[0] of a static array: the base must resolve to a static
             * global slot (IR_GLOBAL_VAR declaration or an IR_GLOBAL lvalue
             * referencing one); a local/parameter base is automatic
             * storage and is not a valid address constant. */
            const IrNode *b = target->u.index_addr.base;
            bool static_base = (b != NULL) &&
                               (b->kind == IR_GLOBAL_VAR ||
                                (b->kind == IR_GLOBAL &&
                                 b->u.global.target != NULL &&
                                 b->u.global.target->kind == IR_GLOBAL_VAR));
            if (!static_base) {
                verify_add_violation(&v, target, 10, IR_INV_STORE,
                                     "IRConst_ADDR id %lld targets a "
                                     "non-static element address", c->id);
            }
        } else {
            verify_add_violation(&v, target, 10, IR_INV_STORE,
                                 "IRConst_ADDR id %lld target is not a "
                                 "static-storage lvalue (consts have no "
                                 "address)", c->id);
        }
    }

    /* pass 2: per-node invariant checks with context */
    if (!v.oom) {
        Encl encl;
        memset(&encl, 0, sizeof(encl));
        for (i = 0; i < build->nmodules; i++) {
            verify_node(&v, build->modules[i], &encl);
        }
        free(encl.items);
    }

    free(v.seen);
    free(v.reached);
    free(v.decls);
    free(v.module_names);

    if (v.oom) {
        ir_records_free(v.recs, v.nrecs);
        return IR_OOM;
    }
    if (v.nrecs > 0) {
        diag_sort_records(v.recs, v.nrecs);
        if (out_records != NULL) {
            *out_records = v.recs;
        }
        if (out_record_count != NULL) {
            *out_record_count = v.nrecs;
        }
        return IR_VIOLATION;
    }
    return IR_OK;
}
