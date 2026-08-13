/* bootstrap/src/ir/ir_builder_expr.c
 *
 * AI-Co Stage-0 IR builder Phase B expression mapping and lowering
 * (WP-M0-16c1c).
 *
 * Implements the expression lowering of the accepted canonical IR
 * contract (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) sections
 * 5.3-5.4, 6, 9.6-9.8 and 12.1 over the resolved, validated build
 * (WP-M0-10 NameResult + WP-M0-11b LayoutBuild + WP-M0-12 constant
 * evaluation), as declared in ir_builder_expr.h. The statement mapper
 * (WP-M0-16c1d) consumes this API; this package installs no mapper
 * through the 16c1a seam (16c1b's Phase B body placeholder remains
 * until 16c1d).
 *
 * Lowering model:
 *   - Every expression is lowered to an IrExprResult carrying the value
 *     category (contract 5.4) and the expression's source-level result
 *     type. Lvalue results are loaded (IR_LOAD, scalar only) when a
 *     value is required; composite results are object-image addresses
 *     (or composite value nodes such as IR_STR/IR_SLICE/IR_CALL).
 *   - Intermediate nodes that must execute before the produced value is
 *     consumed (IR_ZERO + field/element IR_STORE for struct/array
 *     literal images, materialization copies, the single evaluated
 *     element of a repetition-form literal when N == 0) are appended to
 *     the current block via ir_block_add_stmt, so the block's statement
 *     order is the evaluation order (contract 6.1 = spec 10.4).
 *   - Trap obligations are attached as declared trap codes per the
 *     contract 5.3 table (validated by ir_core_verify against the
 *     per-kind allowed sets).
 *   - Defensive IR_BUILDER_UNSUPPORTED (nothing owned) covers the
 *     disclosed representable-surface gaps (ir_builder_expr.h).
 *
 * Runtime-call signatures: runtime functions (rt.mem/rt.io/rt.proc/
 * rt.trap) are mapped by 16c1b without parameters and with void return;
 * ir_core_verify demands IR_CALL/IR_CALL_TERM argument counts and types
 * match the callee parameters. This package attaches the spec 15.1-15.4
 * signatures (params + return type + parameter slots) to runtime
 * IR_FUNCTION nodes on first use as a call callee (idempotent; the
 * 16c1b header delegates this to 16c1c/16c1d).
 *
 * Determinism: node construction order is the AST walk order (spec
 * 10.4); the local-symbol slot registration is keyed by symbol identity
 * (stable per build); runtime signatures are attached in first-use
 * order, which is deterministic for a given build.
 */
#include "ir_builder_expr.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Per-build mapper scratch state (local symbol -> slot registration)
 * ------------------------------------------------------------------------- */

typedef struct ExprLocalEntry {
    const NameSymbol *fn;
    const NameSymbol *sym;
    int64_t slot_index;
} ExprLocalEntry;

static ExprLocalEntry *s_locals;
static size_t s_nlocals;
static size_t s_locals_cap;
static const IrBuild *s_build_key;   /* reset guard (build identity) */

static void expr_state_reset(void)
{
    free(s_locals);
    s_locals = NULL;
    s_nlocals = 0;
    s_locals_cap = 0;
    s_build_key = NULL;
}

void ir_builder_expr_install(void)
{
    expr_state_reset();
}

void ir_builder_expr_register_local(IrBuild *b, const NameSymbol *fn_sym,
                                    const NameSymbol *local_sym,
                                    int64_t slot_index)
{
    ExprLocalEntry *p;
    if (local_sym == NULL) {
        return;
    }
    if (s_build_key != b) {
        expr_state_reset();
        s_build_key = b;
    }
    if (s_nlocals == s_locals_cap) {
        size_t ncap = s_locals_cap ? s_locals_cap * 2 : 16;
        p = (ExprLocalEntry *)realloc(s_locals, ncap * sizeof(*p));
        if (p == NULL) {
            return;   /* registration failure: identifier lowering will
                       * fail defensively (UNSUPPORTED) on lookup miss */
        }
        s_locals = p;
        s_locals_cap = ncap;
    }
    s_locals[s_nlocals].fn = fn_sym;
    s_locals[s_nlocals].sym = local_sym;
    s_locals[s_nlocals].slot_index = slot_index;
    s_nlocals++;
}

/* ---------------------------------------------------------------------------
 * Small allocation helpers (build-owned; set build->oom on failure)
 * ------------------------------------------------------------------------- */

static void *build_alloc(BuilderCtx *ctx, size_t n, size_t size)
{
    IrBuild *b = ctx->build;
    void *p;
    if (b->oom || size == 0) {
        return NULL;
    }
    p = calloc(n, size);
    if (p == NULL) {
        b->oom = true;
    }
    return p;
}

static char *build_dup(BuilderCtx *ctx, const char *s)
{
    IrBuild *b = ctx->build;
    size_t n;
    char *p;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p == NULL) {
        b->oom = true;
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

/* ---------------------------------------------------------------------------
 * Cause links (minimal structural; full preservation is WP-M0-16c2)
 * ------------------------------------------------------------------------- */

static void add_cause(BuilderCtx *ctx, IrNode *node, const char *kind)
{
    ir_node_add_cause(ctx->build, node, kind, node->span, -1, -1, -1);
}

/* Deterministic AST construct-kind label for cause links (16c2 refines
 * the full chain; this mirrors the 16c1b convention of an opaque
 * construct-kind string per node). */
static const char *expr_kind_text(AstNodeKind kind)
{
    switch (kind) {
    case AST_EXPR_INT_LITERAL:   return "AST_EXPR_INT_LITERAL";
    case AST_EXPR_STR_LITERAL:   return "AST_EXPR_STR_LITERAL";
    case AST_EXPR_BOOL_LITERAL:  return "AST_EXPR_BOOL_LITERAL";
    case AST_EXPR_NULL_LITERAL:  return "AST_EXPR_NULL_LITERAL";
    case AST_EXPR_IDENT:         return "AST_EXPR_IDENT";
    case AST_EXPR_ARRAY_LITERAL: return "AST_EXPR_ARRAY_LITERAL";
    case AST_EXPR_PAREN:         return "AST_EXPR_PAREN";
    case AST_EXPR_UNARY:         return "AST_EXPR_UNARY";
    case AST_EXPR_BINARY:        return "AST_EXPR_BINARY";
    case AST_EXPR_ASSIGN:        return "AST_EXPR_ASSIGN";
    case AST_EXPR_TERNARY:       return "AST_EXPR_TERNARY";
    case AST_EXPR_INDEX:         return "AST_EXPR_INDEX";
    case AST_EXPR_SLICE:         return "AST_EXPR_SLICE";
    case AST_EXPR_CALL:          return "AST_EXPR_CALL";
    case AST_EXPR_MEMBER:        return "AST_EXPR_MEMBER";
    case AST_EXPR_ARROW:         return "AST_EXPR_ARROW";
    case AST_EXPR_STRUCT_INIT:   return "AST_EXPR_STRUCT_INIT";
    case AST_EXPR_SIZEOF_TYPE:   return "AST_EXPR_SIZEOF_TYPE";
    case AST_EXPR_SIZEOF_EXPR:   return "AST_EXPR_SIZEOF_EXPR";
    case AST_EXPR_ALIGNOF:       return "AST_EXPR_ALIGNOF";
    case AST_EXPR_CAST:          return "AST_EXPR_CAST";
    case AST_EXPR_WRAP:          return "AST_EXPR_WRAP";
    case AST_EXPR_LEN:           return "AST_EXPR_LEN";
    case AST_EXPR_PTR:           return "AST_EXPR_PTR";
    default:                     return "AST_EXPR";
    }
}

/* ---------------------------------------------------------------------------
 * Node creation
 * ------------------------------------------------------------------------- */

static IrNode *mk_node(BuilderCtx *ctx, IrNodeKind kind,
                       const DiagSpan *span, const char *construct_kind)
{
    IrNode *n = ir_node_new(ctx->build, kind, span);
    if (n == NULL) {
        return NULL;
    }
    add_cause(ctx, n, construct_kind);
    return n;
}

static IrNode *mk_value_node(BuilderCtx *ctx, IrNodeKind kind,
                             const DiagSpan *span, const char *construct_kind,
                             IrType *type)
{
    IrNode *n = mk_node(ctx, kind, span, construct_kind);
    if (n == NULL) {
        return NULL;
    }
    n->type = type;
    return n;
}

static IrNode *mk_trap_node(BuilderCtx *ctx, IrNodeKind kind,
                            const DiagSpan *span, const char *construct_kind,
                            IrType *type, const char *trap_code)
{
    IrNode *n = mk_value_node(ctx, kind, span, construct_kind, type);
    if (n == NULL) {
        return NULL;
    }
    n->trap_code = trap_code;
    return n;
}

/* ---------------------------------------------------------------------------
 * Type helpers
 * ------------------------------------------------------------------------- */

static IrType *prim_from_lex(BuilderCtx *ctx, LexIntType t)
{
    switch (t) {
    case LEX_INT_I8:    return ir_type_i8(ctx->build);
    case LEX_INT_I16:   return ir_type_i16(ctx->build);
    case LEX_INT_I32:   return ir_type_i32(ctx->build);
    case LEX_INT_I64:   return ir_type_i64(ctx->build);
    case LEX_INT_U8:    return ir_type_u8(ctx->build);
    case LEX_INT_U16:   return ir_type_u16(ctx->build);
    case LEX_INT_U32:   return ir_type_u32(ctx->build);
    case LEX_INT_U64:   return ir_type_u64(ctx->build);
    case LEX_INT_ISIZE: return ir_type_isize(ctx->build);
    case LEX_INT_USIZE: return ir_type_usize(ctx->build);
    }
    return ir_type_i32(ctx->build);   /* defensive; lexer always valid */
}

static int ir_type_width(const IrType *t)
{
    if (t == NULL) {
        return 0;
    }
    switch (t->kind) {
    case IRT_I8: case IRT_U8:   return 8;
    case IRT_I16: case IRT_U16: return 16;
    case IRT_I32: case IRT_U32: return 32;
    case IRT_I64: case IRT_U64:
    case IRT_ISIZE: case IRT_USIZE: return 64;
    default: return 0;
    }
}

static bool ir_type_is_signed(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    switch (t->kind) {
    case IRT_I8: case IRT_I16: case IRT_I32: case IRT_I64:
    case IRT_ISIZE: return true;
    default: return false;
    }
}

static bool ir_type_is_int(const IrType *t)
{
    return ir_type_width(t) > 0;
}

static bool ir_type_is_composite(const IrType *t)
{
    if (t == NULL) {
        return false;
    }
    return t->kind == IRT_ARRAY || t->kind == IRT_SLICE ||
           t->kind == IRT_STRUCT || t->kind == IRT_STR;
}

/* True when `from` may be implicitly converted to `to` per Table 11.1
 * (value-preserving widening; identity). Integer-only helper. */
static bool ir_int_implicit(const IrType *from, const IrType *to)
{
    int wf, wt;
    bool sf, st;
    if (from == NULL || to == NULL) {
        return false;
    }
    if (ir_type_identical(from, to)) {
        return true;
    }
    wf = ir_type_width(from);
    wt = ir_type_width(to);
    if (wf <= 0 || wt <= 0) {
        return false;
    }
    sf = ir_type_is_signed(from);
    st = ir_type_is_signed(to);
    if (sf && st) {
        return wt >= wf;
    }
    if (!sf && !st) {
        return wt >= wf;
    }
    if (!sf && st) {
        /* unsigned -> signed only when strictly wider */
        return wt > wf;
    }
    /* signed -> unsigned is never implicit */
    return false;
}

/* Common type of two integer types (spec 11.1 bullet 2): the wider of
 * the two with both conversions present in Table 11.1; equal-width
 * identical-sign pairs (i64/isize, u64/usize) tie-break to the
 * fixed-width type (commutative). NULL when no common type exists. */
static IrType *common_int_type(BuilderCtx *ctx, const IrType *a,
                               const IrType *b)
{
    (void)ctx;
    if (a == NULL || b == NULL || !ir_type_is_int(a) || !ir_type_is_int(b)) {
        return NULL;
    }
    if (ir_type_identical(a, b)) {
        return (IrType *)a;
    }
    if (ir_type_width(a) > ir_type_width(b)) {
        return ir_int_implicit(b, a) ? (IrType *)a : NULL;
    }
    if (ir_type_width(b) > ir_type_width(a)) {
        return ir_int_implicit(a, b) ? (IrType *)b : NULL;
    }
    if (ir_type_is_signed(a) != ir_type_is_signed(b)) {
        return NULL;
    }
    if (a->kind == IRT_ISIZE || a->kind == IRT_USIZE) {
        return (IrType *)b;
    }
    return (IrType *)a;
}

/* ---------------------------------------------------------------------------
 * Build lookups
 * ------------------------------------------------------------------------- */

static IrNode *find_decl_node(IrBuild *b, const char *fqn)
{
    size_t mi, di;
    if (b == NULL || fqn == NULL) {
        return NULL;
    }
    for (mi = 0; mi < b->nmodules; mi++) {
        IrNode *m = b->modules[mi];
        if (m == NULL) {
            continue;
        }
        for (di = 0; di < m->u.module.ndecls; di++) {
            IrNode *d = m->u.module.decls[di];
            const char *n = NULL;
            if (d == NULL) {
                continue;
            }
            switch (d->kind) {
            case IR_STRUCT_DECL:  n = d->u.struct_decl.name; break;
            case IR_ENUM_DECL:    n = d->u.enum_decl.name; break;
            case IR_GLOBAL_CONST: n = d->u.global_const.name; break;
            case IR_GLOBAL_VAR:   n = d->u.global_var.name; break;
            case IR_FUNCTION:     n = d->u.function.name; break;
            default: break;
            }
            if (n != NULL && strcmp(n, fqn) == 0) {
                return d;
            }
        }
    }
    return NULL;
}

static IrNode *find_fn_node(BuilderCtx *ctx, const NameSymbol *fn_sym)
{
    IrNode *n;
    if (fn_sym == NULL || fn_sym->fqn == NULL) {
        return NULL;
    }
    n = find_decl_node(ctx->build, fn_sym->fqn);
    if (n != NULL && n->kind != IR_FUNCTION) {
        return NULL;
    }
    return n;
}

/* The parameter slot index of a parameter symbol: the position of its
 * declaration AST node in the function's parameter list (16c1b created
 * parameter slots in parameter order with slot_index == position). */
static int64_t param_slot_index(BuilderCtx *ctx, const NameSymbol *fn_sym,
                                const NameSymbol *param_sym)
{
    const AstNode *decl;
    size_t i;
    (void)ctx;
    if (fn_sym == NULL || fn_sym->decl == NULL ||
        fn_sym->decl->kind != AST_FN_DECL ||
        param_sym == NULL || param_sym->decl == NULL) {
        return -1;
    }
    decl = fn_sym->decl;
    for (i = 0; i < decl->u.fn_decl.nparams; i++) {
        if (decl->u.fn_decl.params[i] == param_sym->decl) {
            return (int64_t)i;
        }
    }
    return -1;
}

/* The registered slot index of a local variable symbol, or -1. */
static int64_t local_slot_index(BuilderCtx *ctx, const NameSymbol *fn_sym,
                                const NameSymbol *sym)
{
    size_t i;
    if (s_build_key != ctx->build) {
        return -1;
    }
    for (i = 0; i < s_nlocals; i++) {
        if (s_locals[i].fn == fn_sym && s_locals[i].sym == sym) {
            return s_locals[i].slot_index;
        }
    }
    return -1;
}

/* The type of the value stored at the location an lvalue denotes
 * (mirrors ir_core.c lvalue_value_type). */
static const IrType *value_at_type(const IrNode *n)
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

static bool is_lvalue_node(const IrNode *n)
{
    if (n == NULL) {
        return false;
    }
    switch (n->kind) {
    case IR_LOCAL: case IR_GLOBAL: case IR_DEREF: case IR_FIELD_ADDR:
        return true;
    case IR_INDEX_ADDR:
        return n->u.index_addr.base != NULL &&
               n->u.index_addr.base->type != NULL &&
               is_lvalue_node(n->u.index_addr.base) &&
               (n->u.index_addr.base->type->kind == IRT_ARRAY ||
                n->u.index_addr.base->type->kind == IRT_SLICE);
    default:
        return false;
    }
}

/* The field index of a named field in a struct declaration IR node, or
 * the member index of a named enum member in an enum declaration IR
 * node, or -1 when not found. */
static int64_t field_index_of(IrNode *sdecl, const char *name)
{
    size_t i;
    if (sdecl == NULL || name == NULL) {
        return -1;
    }
    if (sdecl->kind == IR_STRUCT_DECL) {
        for (i = 0; i < sdecl->u.struct_decl.nfields; i++) {
            if (sdecl->u.struct_decl.fields[i].name != NULL &&
                strcmp(sdecl->u.struct_decl.fields[i].name, name) == 0) {
                return (int64_t)i;
            }
        }
    } else if (sdecl->kind == IR_ENUM_DECL) {
        for (i = 0; i < sdecl->u.enum_decl.nmembers; i++) {
            if (sdecl->u.enum_decl.members[i].name != NULL &&
                strcmp(sdecl->u.enum_decl.members[i].name, name) == 0) {
                return (int64_t)i;
            }
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Runtime-call signatures (spec 15.1-15.4; see header gap note 7)
 * ------------------------------------------------------------------------- */

typedef enum RtKind {
    RTK_USIZE = 0, RTK_U32, RTK_I32, RTK_U8,
    RTK_STR, RTK_PTR_U8, RTK_SLICE_U8, RTK_SLICE_SLICE_U8
} RtKind;

typedef struct RtParamSpec {
    RtKind kind;
    const char *name;
} RtParamSpec;

typedef struct RtSig {
    const char *fqn;
    size_t nparams;
    RtParamSpec params[4];
    RtKind ret;
} RtSig;

static const RtSig *rt_sig_for(const char *fqn);
static bool rt_is_void_ret(const char *fqn);
static IrType *rt_kind_type(BuilderCtx *ctx, RtKind k);

/* ---------------------------------------------------------------------------
 * Expression type derivation
 * ------------------------------------------------------------------------- */

/* Derive the source-level type of an expression (accepted-build
 * surface; mirrors the convert package's bounded expression typing but
 * produces interned IrType descriptors). `expected` is the declared
 * type at the position (used for null literals and array literals). On
 * success *out is an interned IrType (borrowed); *out_is_null is set
 * for a null literal. Returns false when the expression cannot be
 * typed (defensive; the caller returns IR_BUILDER_UNSUPPORTED). */
static bool expr_type(BuilderCtx *ctx, const NameModule *module,
                      const NameSymbol *fn_sym, const AstNode *e,
                      IrType *expected, IrType **out, bool *out_is_null)
{
    IrBuild *b = ctx->build;
    if (out_is_null != NULL) {
        *out_is_null = false;
    }
    if (e == NULL) {
        return false;
    }
    switch (e->kind) {
    case AST_EXPR_INT_LITERAL:
        if (expected != NULL && ir_type_is_int(expected)) {
            *out = expected;   /* typed-node model: widened literal */
        } else {
            *out = prim_from_lex(ctx, e->u.int_literal.type);
        }
        return *out != NULL;
    case AST_EXPR_BOOL_LITERAL:
        *out = ir_type_bool(b);
        return true;
    case AST_EXPR_STR_LITERAL:
        *out = ir_type_str(b);
        return true;
    case AST_EXPR_NULL_LITERAL:
        if (out_is_null != NULL) {
            *out_is_null = true;
        }
        *out = (expected != NULL && expected->kind == IRT_PTR) ? expected
                                                               : NULL;
        return *out != NULL;
    case AST_EXPR_IDENT: {
        const NameSymbol *sym = name_symbol_for_node(module, e);
        IrNode *fn_node;
        int64_t si;
        if (sym == NULL) {
            return false;
        }
        switch (sym->kind) {
        case NAME_SYM_PARAM:
            si = param_slot_index(ctx, fn_sym, sym);
            fn_node = find_fn_node(ctx, fn_sym);
            if (fn_node != NULL && si >= 0 &&
                (size_t)si < fn_node->u.function.nslots) {
                *out = fn_node->u.function.slots[si]->type;
                return *out != NULL;
            }
            return false;
        case NAME_SYM_LOCAL_VAR:
            si = local_slot_index(ctx, fn_sym, sym);
            fn_node = find_fn_node(ctx, fn_sym);
            if (fn_node != NULL && si >= 0 &&
                (size_t)si < fn_node->u.function.nslots) {
                *out = fn_node->u.function.slots[si]->type;
                return *out != NULL;
            }
            return false;
        case NAME_SYM_LOCAL_CONST:
            /* a local const reference: its value is the initializer's
             * constant; type = the declared const type */
            if (sym->decl != NULL && sym->decl->kind == AST_CONST_DECL &&
                sym->decl->u.local_decl.type != NULL) {
                *out = ir_builder_type_from_ast(
                    ctx, module, sym->decl->u.local_decl.type);
                return *out != NULL;
            }
            return false;
        case NAME_SYM_GLOBAL_VAR: {
            IrNode *gn = find_decl_node(b, sym->fqn);
            if (gn == NULL) {
                return false;
            }
            *out = gn->u.global_var.type;
            return *out != NULL;
        }
        case NAME_SYM_GLOBAL_CONST: {
            IrNode *cn = find_decl_node(b, sym->fqn);
            if (cn == NULL || cn->u.global_const.value == NULL) {
                return false;
            }
            *out = cn->u.global_const.value->type;
            return *out != NULL;
        }
        case NAME_SYM_ENUM_MEMBER: {
            IrNode *ed = sym->owner != NULL
                             ? find_decl_node(b, sym->owner->fqn) : NULL;
            if (ed == NULL) {
                return false;
            }
            *out = ir_type_enum(b, ed);
            return *out != NULL;
        }
        default:
            return false;
        }
    }
    case AST_EXPR_PAREN:
        return expr_type(ctx, module, fn_sym, e->u.paren.expr, expected, out,
                         out_is_null);
    case AST_EXPR_UNARY: {
        IrType *ot = NULL;
        bool onull = false;
        if (e->u.unary.op == AST_UN_NOT) {
            *out = ir_type_bool(b);
            return true;
        }
        if (!expr_type(ctx, module, fn_sym, e->u.unary.operand, NULL, &ot,
                       &onull)) {
            return false;
        }
        switch (e->u.unary.op) {
        case AST_UN_NEG:
        case AST_UN_PLUS:
        case AST_UN_BNOT:
            *out = ot;
            return true;
        case AST_UN_DEREF:
            if (ot->kind == IRT_PTR) {
                *out = ot->u.ptr.elem;
                return true;
            }
            return false;
        case AST_UN_ADDR:
            *out = ir_type_ptr(b, ot);
            return *out != NULL;
        default:
            return false;
        }
    }
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_ALIGNOF:
        *out = ir_type_usize(b);
        return true;
    case AST_EXPR_LEN:
        *out = ir_type_usize(b);
        return true;
    case AST_EXPR_PTR: {
        IrType *ot = NULL;
        bool onull = false;
        if (!expr_type(ctx, module, fn_sym, e->u.size_op.operand, NULL, &ot,
                       &onull)) {
            return false;
        }
        if (ot->kind == IRT_ARRAY) {
            *out = ir_type_ptr(b, ot->u.array.elem);
        } else if (ot->kind == IRT_SLICE) {
            *out = ir_type_ptr(b, ot->u.slice.elem);
        } else if (ot->kind == IRT_STR) {
            *out = ir_type_ptr(b, ir_type_u8(b));
        } else {
            return false;
        }
        return *out != NULL;
    }
    case AST_EXPR_INDEX: {
        IrType *bt = NULL;
        bool bnull = false;
        if (!expr_type(ctx, module, fn_sym, e->u.index_slice.base, NULL, &bt,
                       &bnull)) {
            return false;
        }
        if (bt->kind == IRT_ARRAY) {
            *out = bt->u.array.elem;
        } else if (bt->kind == IRT_SLICE) {
            *out = bt->u.slice.elem;
        } else if (bt->kind == IRT_STR) {
            *out = ir_type_u8(b);
        } else {
            return false;
        }
        return true;
    }
    case AST_EXPR_SLICE: {
        IrType *bt = NULL;
        bool bnull = false;
        if (!expr_type(ctx, module, fn_sym, e->u.index_slice.base, NULL, &bt,
                       &bnull)) {
            return false;
        }
        if (bt->kind == IRT_STR) {
            *out = ir_type_str(b);
        } else if (bt->kind == IRT_ARRAY) {
            *out = ir_type_slice(b, bt->u.array.elem);
        } else if (bt->kind == IRT_SLICE) {
            *out = ir_type_slice(b, bt->u.slice.elem);
        } else {
            return false;
        }
        return *out != NULL;
    }
    case AST_EXPR_BINARY: {
        AstBinaryOp op = e->u.binary.op;
        if (op == AST_BIN_LAND || op == AST_BIN_LOR) {
            *out = ir_type_bool(b);
            return true;
        }
        if (op == AST_BIN_SHL || op == AST_BIN_SHR) {
            IrType *lt = NULL;
            bool lnull = false;
            if (!expr_type(ctx, module, fn_sym, e->u.binary.lhs, NULL, &lt,
                           &lnull)) {
                return false;
            }
            *out = lt;
            return true;
        }
        if (op == AST_BIN_LT || op == AST_BIN_LE || op == AST_BIN_GT ||
            op == AST_BIN_GE || op == AST_BIN_EQ || op == AST_BIN_NE) {
            *out = ir_type_bool(b);
            return true;
        }
        {
            IrType *lt = NULL, *rt = NULL;
            bool lnull = false, rnull = false;
            if (!expr_type(ctx, module, fn_sym, e->u.binary.lhs, NULL, &lt,
                           &lnull) ||
                !expr_type(ctx, module, fn_sym, e->u.binary.rhs, NULL, &rt,
                           &rnull)) {
                return false;
            }
            if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
                if (lt->kind == IRT_PTR && ir_type_is_int(rt)) {
                    *out = lt;
                    return true;
                }
                if (ir_type_is_int(lt) && rt->kind == IRT_PTR) {
                    *out = rt;
                    return true;
                }
                if (op == AST_BIN_SUB && lt->kind == IRT_PTR &&
                    rt->kind == IRT_PTR &&
                    ir_type_identical(lt->u.ptr.elem, rt->u.ptr.elem)) {
                    *out = ir_type_isize(b);
                    return true;
                }
            }
            {
                IrType *ct = common_int_type(ctx, lt, rt);
                if (ct == NULL) {
                    return false;
                }
                *out = ct;
                return true;
            }
        }
    }
    case AST_EXPR_TERNARY: {
        IrType *tt = NULL, *et = NULL;
        bool tnull = false, enull = false;
        IrType *ct;
        if (!expr_type(ctx, module, fn_sym, e->u.branch.then, NULL, &tt, &tnull) ||
            !expr_type(ctx, module, fn_sym, e->u.branch.els, NULL, &et, &enull)) {
            return false;
        }
        if (tnull && enull) {
            return false;
        }
        if (tnull) {
            if (et == NULL || et->kind != IRT_PTR) {
                return false;
            }
            *out = et;
            return true;
        }
        if (enull) {
            if (tt == NULL || tt->kind != IRT_PTR) {
                return false;
            }
            *out = tt;
            return true;
        }
        ct = common_int_type(ctx, tt, et);
        if (ct != NULL) {
            *out = ct;
            return true;
        }
        if (ir_type_identical(tt, et)) {
            *out = tt;
            return true;
        }
        return false;
    }
    case AST_EXPR_CALL: {
        const NameSymbol *fsym =
            name_symbol_for_node(module, e->u.call.callee);
        if (fsym == NULL || fsym->kind != NAME_SYM_FN) {
            return false;
        }
        if (fsym->decl != NULL) {
            const AstNode *fdecl = fsym->decl;
            IrType *rt;
            if (fdecl->kind != AST_FN_DECL ||
                fdecl->u.fn_decl.ret_type == NULL) {
                return false;
            }
            rt = ir_builder_type_from_ast(ctx, fsym->module,
                                          fdecl->u.fn_decl.ret_type);
            if (rt == NULL) {
                return false;
            }
            *out = rt;
            return true;
        }
        {
            const RtSig *sig = rt_sig_for(fsym->fqn);
            bool void_ret = rt_is_void_ret(fsym->fqn);
            if (sig == NULL) {
                return false;
            }
            *out = void_ret ? ir_type_void(b) : rt_kind_type(ctx, sig->ret);
            return *out != NULL;
        }
    }
    case AST_EXPR_ASSIGN: {
        IrType *tt = NULL;
        bool tnull = false;
        if (!expr_type(ctx, module, fn_sym, e->u.assign.target, NULL, &tt,
                       &tnull)) {
            return false;
        }
        *out = tt;
        return true;
    }
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        const NameSymbol *msym = name_symbol_for_node(module, e);
        if (msym != NULL && msym->kind == NAME_SYM_ENUM_MEMBER) {
            IrNode *ed = msym->owner != NULL
                             ? find_decl_node(b, msym->owner->fqn) : NULL;
            if (ed == NULL) {
                return false;
            }
            *out = ir_type_enum(b, ed);
            return *out != NULL;
        }
        if (msym != NULL && msym->kind == NAME_SYM_GLOBAL_VAR) {
            IrNode *gn = find_decl_node(b, msym->fqn);
            if (gn == NULL) {
                return false;
            }
            *out = gn->u.global_var.type;
            return *out != NULL;
        }
        if (msym != NULL && msym->kind == NAME_SYM_GLOBAL_CONST) {
            IrNode *cn = find_decl_node(b, msym->fqn);
            if (cn == NULL || cn->u.global_const.value == NULL) {
                return false;
            }
            *out = cn->u.global_const.value->type;
            return *out != NULL;
        }
        {
            IrType *bt = NULL;
            bool bnull = false;
            const IrType *struct_t = NULL;
            IrNode *sdecl;
            size_t i;
            if (!expr_type(ctx, module, fn_sym, e->u.member.base, NULL, &bt,
                           &bnull)) {
                return false;
            }
            if (e->kind == AST_EXPR_ARROW) {
                if (bt->kind == IRT_PTR) {
                    struct_t = bt->u.ptr.elem;
                }
            } else {
                struct_t = bt;
            }
            if (struct_t == NULL || struct_t->kind != IRT_STRUCT ||
                struct_t->u.decl == NULL) {
                return false;
            }
            sdecl = struct_t->u.decl;
            for (i = 0; i < sdecl->u.struct_decl.nfields; i++) {
                if (sdecl->u.struct_decl.fields[i].name != NULL &&
                    strcmp(sdecl->u.struct_decl.fields[i].name,
                           e->u.member.name) == 0) {
                    *out = sdecl->u.struct_decl.fields[i].type;
                    return true;
                }
            }
            return false;
        }
    }
    case AST_EXPR_ARRAY_LITERAL:
        if (expected != NULL && expected->kind == IRT_ARRAY) {
            *out = expected;
            return true;
        }
        return false;
    case AST_EXPR_STRUCT_INIT: {
        const NameSymbol *bsym =
            name_symbol_for_node(module, e->u.struct_init.base);
        IrNode *sd;
        if (bsym == NULL || bsym->kind != NAME_SYM_STRUCT) {
            return false;
        }
        sd = find_decl_node(b, bsym->fqn);
        if (sd == NULL) {
            return false;
        }
        *out = ir_type_struct(b, sd);
        return *out != NULL;
    }
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP: {
        IrType *t = ir_builder_type_from_ast(ctx, module,
                                             e->u.cast_wrap.type);
        if (t == NULL) {
            return false;
        }
        *out = t;
        return true;
    }
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Runtime-call signatures (spec 15.1-15.4; see header gap note 7)
 * ------------------------------------------------------------------------- */

static const RtSig kRtSigs[] = {
    { "rt.mem.alloc_bytes", 1, { { RTK_USIZE, "count" } }, RTK_PTR_U8 },
    { "rt.mem.dealloc_bytes", 1, { { RTK_PTR_U8, "p" } }, RTK_USIZE },
    { "rt.mem.copy", 3,
      { { RTK_PTR_U8, "dst" }, { RTK_PTR_U8, "src" },
        { RTK_USIZE, "count" } }, RTK_USIZE },
    { "rt.mem.fill", 3,
      { { RTK_PTR_U8, "dst" }, { RTK_U8, "value" },
        { RTK_USIZE, "count" } }, RTK_USIZE },
    { "rt.io.open", 2, { { RTK_STR, "path" }, { RTK_U32, "mode" } },
      RTK_USIZE },
    { "rt.io.read", 3,
      { { RTK_USIZE, "handle" }, { RTK_SLICE_U8, "buf" },
        { RTK_USIZE, "count" } }, RTK_USIZE },
    { "rt.io.write", 3,
      { { RTK_USIZE, "handle" }, { RTK_SLICE_U8, "buf" },
        { RTK_USIZE, "count" } }, RTK_USIZE },
    { "rt.io.close", 1, { { RTK_USIZE, "handle" } }, RTK_USIZE },
    { "rt.io.stdin", 0, { { RTK_USIZE, "" } }, RTK_USIZE },
    { "rt.io.stdout", 0, { { RTK_USIZE, "" } }, RTK_USIZE },
    { "rt.io.stderr", 0, { { RTK_USIZE, "" } }, RTK_USIZE },
    { "rt.proc.args", 0, { { RTK_USIZE, "" } }, RTK_SLICE_SLICE_U8 },
    { "rt.proc.exit", 1, { { RTK_I32, "code" } }, RTK_USIZE },
    { "rt.trap.report", 2,
      { { RTK_U32, "code" }, { RTK_STR, "message" } }, RTK_USIZE }
};

static const RtSig *rt_sig_for(const char *fqn)
{
    size_t i;
    if (fqn == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(kRtSigs) / sizeof(kRtSigs[0]); i++) {
        if (strcmp(kRtSigs[i].fqn, fqn) == 0) {
            return &kRtSigs[i];
        }
    }
    return NULL;
}

static bool rt_is_void_ret(const char *fqn)
{
    if (fqn == NULL) {
        return false;
    }
    return (strcmp(fqn, "rt.mem.dealloc_bytes") == 0 ||
            strcmp(fqn, "rt.mem.copy") == 0 ||
            strcmp(fqn, "rt.mem.fill") == 0 ||
            strcmp(fqn, "rt.io.close") == 0 ||
            strcmp(fqn, "rt.proc.exit") == 0 ||
            strcmp(fqn, "rt.trap.report") == 0);
}

static IrType *rt_kind_type(BuilderCtx *ctx, RtKind k)
{
    IrBuild *b = ctx->build;
    switch (k) {
    case RTK_USIZE: return ir_type_usize(b);
    case RTK_U32:   return ir_type_u32(b);
    case RTK_I32:   return ir_type_i32(b);
    case RTK_U8:    return ir_type_u8(b);
    case RTK_STR:   return ir_type_str(b);
    case RTK_PTR_U8: return ir_type_ptr(b, ir_type_u8(b));
    case RTK_SLICE_U8: return ir_type_slice(b, ir_type_u8(b));
    case RTK_SLICE_SLICE_U8:
        return ir_type_slice(b, ir_type_slice(b, ir_type_u8(b)));
    }
    return NULL;
}

/* Attach the spec signature to a runtime IR_FUNCTION node on first use
 * as a call callee (idempotent). The 16c1b node has no params and a
 * void return; ir_core_verify demands call argument counts/types match
 * the callee parameters, so the signature must exist before the first
 * IR_CALL/IR_CALL_TERM is created. */
static IrBuilderStatus ensure_runtime_signature(BuilderCtx *ctx,
                                                IrNode *fn_node)
{
    const RtSig *sig;
    IrBuild *b = ctx->build;
    IrParam *params;
    size_t i;

    if (fn_node == NULL || fn_node->kind != IR_FUNCTION ||
        fn_node->u.function.name == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    sig = rt_sig_for(fn_node->u.function.name);
    if (sig == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* unknown runtime function */
    }
    if (fn_node->u.function.nparams == sig->nparams) {
        return IR_BUILDER_OK;            /* already patched (or no-op) */
    }
    if (fn_node->u.function.nparams != 0) {
        return IR_BUILDER_UNSUPPORTED;   /* defensive: conflicting state */
    }
    if (sig->nparams > 0) {
        params = (IrParam *)build_alloc(ctx, sig->nparams, sizeof(*params));
        if (params == NULL) {
            return IR_BUILDER_OOM;
        }
        fn_node->u.function.params = params;
        fn_node->u.function.nparams = sig->nparams;
        for (i = 0; i < sig->nparams; i++) {
            IrType *pt = rt_kind_type(ctx, sig->params[i].kind);
            if (pt == NULL) {
                return IR_BUILDER_OOM;
            }
            params[i].name = build_dup(ctx, sig->params[i].name);
            params[i].type = pt;
            params[i].slot_index = (int64_t)i;
            params[i].span = diag_span_clone(fn_node->span);
            if (params[i].name == NULL || params[i].span == NULL) {
                return IR_BUILDER_OOM;
            }
            {
                IrSlot *s = ir_builder_add_slot(b, fn_node, IR_SLOT_PARAM,
                                                params[i].name, pt,
                                                params[i].span);
                if (s == NULL) {
                    return IR_BUILDER_OOM;
                }
            }
        }
    }
    if (rt_is_void_ret(fn_node->u.function.name)) {
        fn_node->u.function.ret_type = ir_type_void(b);
    } else {
        IrType *rt = rt_kind_type(ctx, sig->ret);
        if (rt == NULL) {
            return IR_BUILDER_OOM;
        }
        fn_node->u.function.ret_type = rt;
        /* A patched non-void runtime function has no source body (the
         * 16c1b placeholder IR_BLOCK is empty), so invariant 5 (non-void
         * function tails terminate) would reject the build. Append an
         * unreachable IR_TRAP terminator: the runtime implementation is
         * external and the IR body is never executed, so the tail is a
         * trap placeholder. Idempotent: runs only on the first patch. */
        if (fn_node->u.function.body != NULL &&
            fn_node->u.function.body->kind == IR_BLOCK) {
            IrNode *trap = mk_node(ctx, IR_TRAP, fn_node->span,
                                   "RUNTIME_FUNCTION");
            if (trap == NULL) {
                return IR_BUILDER_OOM;
            }
            trap->u.trap.code = NULL;
            trap->u.trap.has_user_code = true;
            trap->u.trap.user_code = 0;
            ir_block_add_stmt(b, fn_node->u.function.body, trap);
        }
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Constant nodes and composite materialization
 * ------------------------------------------------------------------------- */

static IrBuilderStatus const_value_node(BuilderCtx *ctx,
                                        const NameSymbol *fn_sym,
                                        IrNode *block,
                                        const IrConst *c,
                                        const DiagSpan *span,
                                        IrNode **out);
static IrBuilderStatus const_composite_image(BuilderCtx *ctx,
                                             const NameSymbol *fn_sym,
                                             IrNode *block,
                                             const IrConst *c,
                                             const DiagSpan *span,
                                             IrNode **out_image);

static IrBuilderStatus const_value_node(BuilderCtx *ctx,
                                        const NameSymbol *fn_sym,
                                        IrNode *block,
                                        const IrConst *c,
                                        const DiagSpan *span,
                                        IrNode **out)
{
    IrBuild *b = ctx->build;
    IrNode *n;
    switch (c->kind) {
    case IRC_INT:
        n = mk_value_node(ctx, IR_INT, span, "AST_EXPR", c->type);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = (IrConst *)c;
        break;
    case IRC_BOOL:
        n = mk_value_node(ctx, IR_BOOL, span, "AST_EXPR", ir_type_bool(b));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = (IrConst *)c;
        break;
    case IRC_NULL:
        n = mk_value_node(ctx, IR_NULL, span, "AST_EXPR", c->type);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        break;
    case IRC_STR:
        n = mk_value_node(ctx, IR_STR, span, "AST_EXPR", ir_type_str(b));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = (IrConst *)c;
        break;
    case IRC_ENUM:
        n = mk_value_node(ctx, IR_ENUM_VAL, span, "AST_EXPR", c->type);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = (IrConst *)c;
        break;
    case IRC_STRUCT:
    case IRC_ARRAY:
        return const_composite_image(ctx, fn_sym, block, c, span, out);
    default:
        /* IRC_ADDR has no value-node form in runtime expressions. */
        return IR_BUILDER_UNSUPPORTED;
    }
    *out = n;
    return IR_BUILDER_OK;
}

static IrBuilderStatus const_composite_image(BuilderCtx *ctx,
                                             const NameSymbol *fn_sym,
                                             IrNode *block,
                                             const IrConst *c,
                                             const DiagSpan *span,
                                             IrNode **out_image)
{
    IrBuild *b = ctx->build;
    IrNode *fn_node;
    IrSlot *slot;
    IrNode *loc;
    IrNode *zero;
    size_t i;

    if (c == NULL || c->type == NULL ||
        (c->kind != IRC_STRUCT && c->kind != IRC_ARRAY)) {
        return IR_BUILDER_UNSUPPORTED;
    }
    fn_node = find_fn_node(ctx, fn_sym);
    if (fn_node == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    slot = ir_builder_add_slot(b, fn_node, IR_SLOT_TEMP, NULL, c->type,
                               span);
    if (slot == NULL) {
        return IR_BUILDER_OOM;
    }
    loc = mk_value_node(ctx, IR_LOCAL, span, "AST_EXPR", (IrType *)c->type);
    if (loc == NULL) {
        return IR_BUILDER_OOM;
    }
    loc->u.local.slot_index = slot->index;
    zero = mk_trap_node(ctx, IR_ZERO, span, "AST_EXPR", NULL, NULL);
    if (zero == NULL) {
        return IR_BUILDER_OOM;
    }
    zero->u.unary.operand = loc;
    ir_block_add_stmt(b, block, zero);
    if (b->oom) {
        return IR_BUILDER_OOM;
    }

    if (c->kind == IRC_STRUCT) {
        IrNode *sdecl = c->type->u.decl;
        for (i = 0; i < c->u.strukt.count; i++) {
            IrNode *field_addr;
            IrNode *val;
            IrNode *store;
            const IrType *ft = (sdecl != NULL &&
                                sdecl->kind == IR_STRUCT_DECL &&
                                i < sdecl->u.struct_decl.nfields)
                                   ? sdecl->u.struct_decl.fields[i].type
                                   : NULL;
            if (c->u.strukt.items[i] == NULL || ft == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            field_addr = mk_trap_node(ctx, IR_FIELD_ADDR, span, "AST_EXPR",
                                      ir_type_ptr(b, (IrType *)ft), NULL);
            if (field_addr == NULL) {
                return IR_BUILDER_OOM;
            }
            field_addr->u.field_addr.base = loc;
            field_addr->u.field_addr.field_index = (int64_t)i;
            {
                IrBuilderStatus st = const_value_node(
                    ctx, fn_sym, block, c->u.strukt.items[i], span, &val);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
            }
            store = mk_trap_node(ctx, IR_STORE, span, "AST_EXPR", NULL,
                                 NULL);
            if (store == NULL) {
                return IR_BUILDER_OOM;
            }
            store->u.store.dest = field_addr;
            store->u.store.value = val;
            ir_block_add_stmt(b, block, store);
            if (b->oom) {
                return IR_BUILDER_OOM;
            }
        }
    } else {   /* IRC_ARRAY */
        IrType *elem = c->type->u.array.elem;
        for (i = 0; i < c->u.arr.count; i++) {
            IrNode *index;
            IrNode *index_addr;
            IrNode *val;
            IrNode *store;
            IrConst *zero_int;
            if (c->u.arr.items[i] == NULL || elem == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            zero_int = ir_const_int(b, ir_type_usize(b), (uint64_t)i);
            if (zero_int == NULL) {
                return IR_BUILDER_OOM;
            }
            index = mk_value_node(ctx, IR_INT, span, "AST_EXPR",
                                  ir_type_usize(b));
            if (index == NULL) {
                return IR_BUILDER_OOM;
            }
            index->u.constant.value = zero_int;
            index_addr = mk_trap_node(ctx, IR_INDEX_ADDR, span, "AST_EXPR",
                                      ir_type_ptr(b, elem), "AIC-R0807");
            if (index_addr == NULL) {
                return IR_BUILDER_OOM;
            }
            index_addr->u.index_addr.base = loc;
            index_addr->u.index_addr.index = index;
            {
                IrBuilderStatus st = const_value_node(
                    ctx, fn_sym, block, c->u.arr.items[i], span, &val);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
            }
            store = mk_trap_node(ctx, IR_STORE, span, "AST_EXPR", NULL,
                                 NULL);
            if (store == NULL) {
                return IR_BUILDER_OOM;
            }
            store->u.store.dest = index_addr;
            store->u.store.value = val;
            ir_block_add_stmt(b, block, store);
            if (b->oom) {
                return IR_BUILDER_OOM;
            }
        }
    }
    *out_image = loc;
    return IR_BUILDER_OK;
}

/* Ensure a composite expression result is an lvalue-shaped object image:
 * when the result node is not an lvalue (e.g. an IR_CALL / IR_STR /
 * IR_SLICE / IR_SELECT of composite type), allocate a temporary slot of
 * the composite type, append IR_STORE(image, node) to the block, and
 * return the IR_LOCAL image. When the result is already an lvalue, it
 * is returned unchanged. */
static IrBuilderStatus materialize(BuilderCtx *ctx,
                                   const NameSymbol *fn_sym,
                                   IrNode *block,
                                   const DiagSpan *span,
                                   const IrExprResult *r,
                                   IrNode **out_image)
{
    IrBuild *b = ctx->build;
    IrNode *fn_node;
    IrSlot *slot;
    IrNode *loc;
    IrNode *store;

    if (r == NULL || r->node == NULL ||
        r->cat != IR_EXPR_COMPOSITE || r->type == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (is_lvalue_node(r->node)) {
        *out_image = r->node;
        return IR_BUILDER_OK;
    }
    fn_node = find_fn_node(ctx, fn_sym);
    if (fn_node == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    slot = ir_builder_add_slot(b, fn_node, IR_SLOT_TEMP, NULL, r->type,
                               span);
    if (slot == NULL) {
        return IR_BUILDER_OOM;
    }
    loc = mk_value_node(ctx, IR_LOCAL, span, "AST_EXPR", r->type);
    if (loc == NULL) {
        return IR_BUILDER_OOM;
    }
    loc->u.local.slot_index = slot->index;
    store = mk_trap_node(ctx, IR_STORE, span, "AST_EXPR", NULL, NULL);
    if (store == NULL) {
        return IR_BUILDER_OOM;
    }
    store->u.store.dest = loc;
    store->u.store.value = r->node;
    ir_block_add_stmt(b, block, store);
    if (b->oom) {
        return IR_BUILDER_OOM;
    }
    *out_image = loc;
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Lowering core
 * ------------------------------------------------------------------------- */

static IrBuilderStatus lower_expr(BuilderCtx *ctx,
                                  const NameModule *module,
                                  const NameSymbol *fn_sym,
                                  IrNode *block,
                                  const AstNode *expr,
                                  IrExprWant want,
                                  IrType *expected,
                                  IrExprResult *out);

static IrBuilderStatus lower_symbol_ref(BuilderCtx *ctx,
                                        const NameModule *module,
                                        const NameSymbol *fn_sym,
                                        IrNode *block,
                                        const AstNode *expr,
                                        IrExprResult *out);
static IrBuilderStatus lower_binary(BuilderCtx *ctx,
                                    const NameModule *module,
                                    const NameSymbol *fn_sym,
                                    IrNode *block,
                                    const AstNode *expr,
                                    IrExprWant want,
                                    IrType *expected,
                                    IrExprResult *out);
static IrBuilderStatus lower_ternary(BuilderCtx *ctx,
                                     const NameModule *module,
                                     const NameSymbol *fn_sym,
                                     IrNode *block,
                                     const AstNode *expr,
                                     IrExprWant want,
                                     IrType *expected,
                                     IrExprResult *out);
static IrBuilderStatus lower_call(BuilderCtx *ctx,
                                  const NameModule *module,
                                  const NameSymbol *fn_sym,
                                  IrNode *block,
                                  const AstNode *expr,
                                  IrExprWant want,
                                  IrType *expected,
                                  IrExprResult *out);
static IrBuilderStatus lower_member(BuilderCtx *ctx,
                                    const NameModule *module,
                                    const NameSymbol *fn_sym,
                                    IrNode *block,
                                    const AstNode *expr,
                                    IrExprWant want,
                                    IrType *expected,
                                    IrExprResult *out);
static IrBuilderStatus lower_assign(BuilderCtx *ctx,
                                    const NameModule *module,
                                    const NameSymbol *fn_sym,
                                    IrNode *block,
                                    const AstNode *expr,
                                    IrExprWant want,
                                    IrType *expected,
                                    IrExprResult *out);
static IrBuilderStatus lower_struct_literal(BuilderCtx *ctx,
                                            const NameModule *module,
                                            const NameSymbol *fn_sym,
                                            IrNode *block,
                                            const AstNode *expr,
                                            IrExprWant want,
                                            IrType *expected,
                                            IrExprResult *out);
static IrBuilderStatus lower_array_literal(BuilderCtx *ctx,
                                           const NameModule *module,
                                           const NameSymbol *fn_sym,
                                           IrNode *block,
                                           const AstNode *expr,
                                           IrExprWant want,
                                           IrType *expected,
                                           IrExprResult *out);

static IrBuilderStatus lower_expr(BuilderCtx *ctx,
                                  const NameModule *module,
                                  const NameSymbol *fn_sym,
                                  IrNode *block,
                                  const AstNode *expr,
                                  IrExprWant want,
                                  IrType *expected,
                                  IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const char *ck = expr_kind_text(expr->kind);

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (expr == NULL || block == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }

    switch (expr->kind) {

    case AST_EXPR_INT_LITERAL: {
        IrType *t = (expected != NULL && ir_type_is_int(expected))
                        ? expected
                        : prim_from_lex(ctx, expr->u.int_literal.type);
        uint64_t bits;
        IrConst *c;
        IrNode *n;
        if (t == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (expr->u.int_literal.is_min) {
            int w = ir_type_width(t);
            int64_t v = (w >= 64) ? INT64_MIN
                                  : -(int64_t)((uint64_t)1 << (w - 1));
            bits = (uint64_t)v;
        } else {
            bits = expr->u.int_literal.value;
        }
        if (ir_type_width(t) < 64) {
            bits &= (((uint64_t)1 << ir_type_width(t)) - 1);
        }
        c = ir_const_int(b, t, bits);
        if (c == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_value_node(ctx, IR_INT, expr->span, ck, t);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = c;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = t;
        return IR_BUILDER_OK;
    }

    case AST_EXPR_BOOL_LITERAL: {
        IrConst *c = ir_const_bool(b, expr->u.bool_literal.value);
        IrNode *n;
        if (c == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_value_node(ctx, IR_BOOL, expr->span, ck, ir_type_bool(b));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = c;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = ir_type_bool(b);
        return IR_BUILDER_OK;
    }

    case AST_EXPR_STR_LITERAL: {
        IrConst *c = ir_const_str(b,
                                  (const uint8_t *)expr->u.str_literal.bytes,
                                  expr->u.str_literal.len);
        IrNode *n;
        if (c == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_value_node(ctx, IR_STR, expr->span, ck, ir_type_str(b));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = c;
        out->cat = IR_EXPR_COMPOSITE;
        out->node = n;
        out->type = ir_type_str(b);
        return IR_BUILDER_OK;
    }

    case AST_EXPR_NULL_LITERAL: {
        IrType *t = (expected != NULL && expected->kind == IRT_PTR)
                        ? expected : NULL;
        IrNode *n;
        if (t == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* no pointer context */
        }
        n = mk_value_node(ctx, IR_NULL, expr->span, ck, t);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = t;
        return IR_BUILDER_OK;
    }

    case AST_EXPR_IDENT:
        return lower_symbol_ref(ctx, module, fn_sym, block, expr, out);

    case AST_EXPR_PAREN:
        return lower_expr(ctx, module, fn_sym, block, expr->u.paren.expr,
                          want, expected, out);

    case AST_EXPR_UNARY: {
        AstUnaryOp op = expr->u.unary.op;
        if (op == AST_UN_NOT) {
            IrNode *v = NULL;
            IrNode *n;
            IrBuilderStatus st = ir_builder_expr_to_value(
                ctx, module, fn_sym, block, expr->u.unary.operand, NULL,
                &v);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            n = mk_value_node(ctx, IR_LNOT, expr->span, ck,
                              ir_type_bool(b));
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.unary.operand = v;
            out->cat = IR_EXPR_SCALAR;
            out->node = n;
            out->type = ir_type_bool(b);
            return IR_BUILDER_OK;
        }
        if (op == AST_UN_BNOT) {
            IrNode *v = NULL;
            IrNode *n;
            IrBuilderStatus st = ir_builder_expr_to_value(
                ctx, module, fn_sym, block, expr->u.unary.operand, NULL,
                &v);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (v == NULL || v->type == NULL || !ir_type_is_int(v->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            n = mk_value_node(ctx, IR_BNOT, expr->span, ck, v->type);
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.unary.operand = v;
            out->cat = IR_EXPR_SCALAR;
            out->node = n;
            out->type = v->type;
            return IR_BUILDER_OK;
        }
        if (op == AST_UN_NEG) {
            IrNode *v = NULL;
            IrNode *n;
            IrBuilderStatus st = ir_builder_expr_to_value(
                ctx, module, fn_sym, block, expr->u.unary.operand, NULL,
                &v);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (v == NULL || v->type == NULL ||
                !ir_type_is_signed(v->type)) {
                return IR_BUILDER_UNSUPPORTED;   /* unsigned - rejected
                                                  * pre-IR (AIC-T0306) */
            }
            n = mk_trap_node(ctx, IR_NEG, expr->span, ck, v->type,
                             "AIC-R0802");
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.unary.operand = v;
            out->cat = IR_EXPR_SCALAR;
            out->node = n;
            out->type = v->type;
            return IR_BUILDER_OK;
        }
        if (op == AST_UN_PLUS) {
            IrNode *v = NULL;
            IrBuilderStatus st = ir_builder_expr_to_value(
                ctx, module, fn_sym, block, expr->u.unary.operand, NULL,
                &v);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (v == NULL || v->type == NULL || !ir_type_is_int(v->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            out->cat = IR_EXPR_SCALAR;
            out->node = v;
            out->type = v->type;
            return IR_BUILDER_OK;
        }
        if (op == AST_UN_DEREF) {
            IrNode *p = NULL;
            IrNode *n;
            IrBuilderStatus st = ir_builder_expr_to_value(
                ctx, module, fn_sym, block, expr->u.unary.operand, NULL,
                &p);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (p == NULL || p->type == NULL || p->type->kind != IRT_PTR) {
                return IR_BUILDER_UNSUPPORTED;
            }
            n = mk_trap_node(ctx, IR_DEREF, expr->span, ck,
                             p->type->u.ptr.elem, "AIC-R0809");
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.deref.ptr = p;
            out->cat = IR_EXPR_LVALUE;
            out->node = n;
            out->type = p->type->u.ptr.elem;
            return IR_BUILDER_OK;
        }
        if (op == AST_UN_ADDR) {
            /* Gap 1: representable only over index (IR_INDEX_ADDR, IR
             * type T*) and member field access (IR_FIELD_ADDR, IR type
             * U*); address-of a plain scalar local/param/global or a
             * dereference is unmappable (no address-of node). */
            const AstNode *operand = expr->u.unary.operand;
            if (operand != NULL &&
                (operand->kind == AST_EXPR_INDEX ||
                 operand->kind == AST_EXPR_MEMBER)) {
                IrExprResult r;
                IrBuilderStatus st = lower_expr(
                    ctx, module, fn_sym, block, operand,
                    IR_EXPR_WANT_ANY, NULL, &r);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
                if (r.node != NULL &&
                    (r.node->kind == IR_INDEX_ADDR ||
                     r.node->kind == IR_FIELD_ADDR)) {
                    /* the node already carries the T* / U* address type */
                    out->cat = IR_EXPR_SCALAR;
                    out->node = r.node;
                    out->type = r.node->type;
                    return IR_BUILDER_OK;
                }
                return IR_BUILDER_UNSUPPORTED;
            }
            return IR_BUILDER_UNSUPPORTED;
        }
        return IR_BUILDER_UNSUPPORTED;
    }

    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF: {
        IrType *t = ir_builder_type_from_ast(ctx, module,
                                             expr->u.size_op.operand);
        IrNode *n;
        IrConst *c;
        uint64_t v;
        if (t == NULL || t->kind == IRT_VOID) {
            return ctx->build->oom ? IR_BUILDER_OOM
                                   : IR_BUILDER_UNSUPPORTED;
        }
        v = (expr->kind == AST_EXPR_ALIGNOF) ? (uint64_t)t->align
                                             : (uint64_t)t->size;
        c = ir_const_int(b, ir_type_usize(b), v);
        if (c == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_value_node(ctx, IR_INT, expr->span, ck, ir_type_usize(b));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = c;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = ir_type_usize(b);
        return IR_BUILDER_OK;
    }

    case AST_EXPR_SIZEOF_EXPR: {
        /* operand not evaluated (spec 10.4): only its type is needed */
        IrType *t = NULL;
        bool isnull = false;
        IrNode *n;
        IrConst *c;
        if (!expr_type(ctx, module, fn_sym, expr->u.size_op.operand, NULL, &t,
                       &isnull)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        c = ir_const_int(b, ir_type_usize(b), (uint64_t)t->size);
        if (c == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_value_node(ctx, IR_INT, expr->span, ck, ir_type_usize(b));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = c;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = ir_type_usize(b);
        return IR_BUILDER_OK;
    }

    case AST_EXPR_LEN:
    case AST_EXPR_PTR: {
        IrExprResult r;
        IrBuilderStatus st = lower_expr(
            ctx, module, fn_sym, block, expr->u.size_op.operand,
            IR_EXPR_WANT_ANY, NULL, &r);
        IrType *ot;
        IrNode *n;
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (r.node == NULL || r.type == NULL ||
            (r.type->kind != IRT_ARRAY && r.type->kind != IRT_SLICE &&
             r.type->kind != IRT_STR)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        ot = r.type;
        if (expr->kind == AST_EXPR_LEN) {
            n = mk_value_node(ctx, IR_LEN, expr->span, ck,
                              ir_type_usize(b));
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.unary.operand = r.node;
            out->cat = IR_EXPR_SCALAR;
            out->node = n;
            out->type = ir_type_usize(b);
            return IR_BUILDER_OK;
        }
        {
            IrType *elem = (ot->kind == IRT_ARRAY)
                               ? ot->u.array.elem
                               : ((ot->kind == IRT_SLICE)
                                      ? ot->u.slice.elem
                                      : ir_type_u8(b));
            IrType *pt = ir_type_ptr(b, elem);
            if (pt == NULL) {
                return IR_BUILDER_OOM;
            }
            n = mk_value_node(ctx, IR_PTR, expr->span, ck, pt);
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.unary.operand = r.node;
            out->cat = IR_EXPR_SCALAR;
            out->node = n;
            out->type = pt;
            return IR_BUILDER_OK;
        }
    }

    case AST_EXPR_INDEX: {
        const AstNode *base_ast = expr->u.index_slice.base;
        const AstNode *index_ast = expr->u.index_slice.index;
        IrExprResult br;
        IrBuilderStatus st;
        IrNode *base_node;
        IrType *bt;
        IrType *elem;
        IrNode *index_node;
        IrNode *n;

        if (base_ast == NULL || index_ast == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        /* Pre-check the base type without creating nodes, so the
         * unsupported paths below leave nothing owned (invariant 1:
         * every created node must be reachable). */
        {
            IrType *bt0 = NULL;
            bool bnull = false;
            if (!expr_type(ctx, module, fn_sym, base_ast, NULL, &bt0,
                           &bnull)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (bt0 == NULL ||
                (bt0->kind != IRT_ARRAY && bt0->kind != IRT_SLICE &&
                 bt0->kind != IRT_STR)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            /* str indexing is a value address, never an lvalue (AC2) */
            if (bt0->kind == IRT_STR && want == IR_EXPR_WANT_LVALUE) {
                return IR_BUILDER_UNSUPPORTED;
            }
        }
        /* Pre-check the index type (gap 4): a runtime index expression
         * must be usize-typed before any node is created. */
        if (index_ast->kind != AST_EXPR_INT_LITERAL &&
            index_ast->kind != AST_EXPR_PAREN) {
            IrType *it0 = NULL;
            bool inull = false;
            if (!expr_type(ctx, module, fn_sym, index_ast, NULL, &it0,
                           &inull)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (it0 == NULL || it0->kind != IRT_USIZE) {
                return IR_BUILDER_UNSUPPORTED;   /* gap 4 */
            }
        }
        st = lower_expr(ctx, module, fn_sym, block, base_ast,
                        IR_EXPR_WANT_ANY, NULL, &br);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        bt = br.type;
        if (bt == NULL ||
            (bt->kind != IRT_ARRAY && bt->kind != IRT_SLICE &&
             bt->kind != IRT_STR)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (bt->kind == IRT_ARRAY || bt->kind == IRT_SLICE) {
            /* array/slice base must be a mutable lvalue (contract 5.3);
             * a composite value is materialized into an image first */
            if (!is_lvalue_node(br.node)) {
                IrNode *img = NULL;
                st = materialize(ctx, fn_sym, block, expr->span, &br,
                                 &img);
                if (st != IR_BUILDER_OK) {
                    return st;
                }
                br.node = img;
            }
            base_node = br.node;
            elem = (bt->kind == IRT_ARRAY) ? bt->u.array.elem
                                           : bt->u.slice.elem;
        } else {
            base_node = br.node;
            elem = ir_type_u8(b);
        }
        /* index: constant integers re-typed to usize (typed-node
         * model); non-usize runtime values are a disclosed gap (header
         * note 4) */
        if (index_ast->kind == AST_EXPR_INT_LITERAL ||
            index_ast->kind == AST_EXPR_PAREN) {
            st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                          index_ast, ir_type_usize(b),
                                          &index_node);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        } else {
            st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                          index_ast, NULL, &index_node);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            if (index_node == NULL || index_node->type == NULL ||
                index_node->type->kind != IRT_USIZE) {
                return IR_BUILDER_UNSUPPORTED;   /* gap 4 */
            }
        }
        n = mk_trap_node(ctx, IR_INDEX_ADDR, expr->span, ck,
                         ir_type_ptr(b, elem), "AIC-R0807");
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.index_addr.base = base_node;
        n->u.index_addr.index = index_node;
        if (bt->kind == IRT_STR) {
            /* value address, never an lvalue (AC2; contract 5.3) */
            out->cat = IR_EXPR_COMPOSITE;
            out->node = n;
            out->type = elem;
            return IR_BUILDER_OK;
        }
        out->cat = IR_EXPR_LVALUE;
        out->node = n;
        out->type = elem;
        return IR_BUILDER_OK;
    }

    case AST_EXPR_SLICE: {
        const AstNode *base_ast = expr->u.index_slice.base;
        IrExprResult br;
        IrBuilderStatus st;
        IrType *bt;
        IrNode *base_node;
        IrNode *lo = NULL, *hi = NULL;
        IrType *stype;
        const char *trap;
        IrNode *n;

        if (base_ast == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        /* Pre-check base and bound types without creating nodes, so the
         * unsupported paths below leave nothing owned (invariant 1). */
        {
            IrType *bt0 = NULL;
            bool bnull = false;
            if (!expr_type(ctx, module, fn_sym, base_ast, NULL, &bt0,
                           &bnull)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (bt0 == NULL ||
                (bt0->kind != IRT_ARRAY && bt0->kind != IRT_SLICE &&
                 bt0->kind != IRT_STR)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (want == IR_EXPR_WANT_LVALUE) {
                /* a slice expression is a value, never an lvalue */
                return IR_BUILDER_UNSUPPORTED;
            }
        }
        if (expr->u.index_slice.lo != NULL &&
            expr->u.index_slice.lo->kind != AST_EXPR_INT_LITERAL &&
            expr->u.index_slice.lo->kind != AST_EXPR_PAREN) {
            IrType *lt0 = NULL;
            bool lnull = false;
            if (!expr_type(ctx, module, fn_sym, expr->u.index_slice.lo,
                           NULL, &lt0, &lnull)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (lt0 == NULL || lt0->kind != IRT_USIZE) {
                return IR_BUILDER_UNSUPPORTED;   /* gap 4 */
            }
        }
        if (expr->u.index_slice.hi != NULL &&
            expr->u.index_slice.hi->kind != AST_EXPR_INT_LITERAL &&
            expr->u.index_slice.hi->kind != AST_EXPR_PAREN) {
            IrType *ht0 = NULL;
            bool hnull = false;
            if (!expr_type(ctx, module, fn_sym, expr->u.index_slice.hi,
                           NULL, &ht0, &hnull)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (ht0 == NULL || ht0->kind != IRT_USIZE) {
                return IR_BUILDER_UNSUPPORTED;   /* gap 4 */
            }
        }
        st = lower_expr(ctx, module, fn_sym, block, base_ast,
                        IR_EXPR_WANT_ANY, NULL, &br);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        bt = br.type;
        if (bt == NULL ||
            (bt->kind != IRT_ARRAY && bt->kind != IRT_SLICE &&
             bt->kind != IRT_STR)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        base_node = br.node;
        if (expr->u.index_slice.lo != NULL) {
            if (expr->u.index_slice.lo->kind == AST_EXPR_INT_LITERAL ||
                expr->u.index_slice.lo->kind == AST_EXPR_PAREN) {
                st = ir_builder_expr_to_value(
                    ctx, module, fn_sym, block, expr->u.index_slice.lo,
                    ir_type_usize(b), &lo);
            } else {
                st = ir_builder_expr_to_value(
                    ctx, module, fn_sym, block, expr->u.index_slice.lo,
                    NULL, &lo);
                if (st == IR_BUILDER_OK &&
                    (lo == NULL || lo->type == NULL ||
                     lo->type->kind != IRT_USIZE)) {
                    return IR_BUILDER_UNSUPPORTED;   /* gap 4 */
                }
            }
            if (st != IR_BUILDER_OK) {
                return st;
            }
        }
        if (expr->u.index_slice.hi != NULL) {
            if (expr->u.index_slice.hi->kind == AST_EXPR_INT_LITERAL ||
                expr->u.index_slice.hi->kind == AST_EXPR_PAREN) {
                st = ir_builder_expr_to_value(
                    ctx, module, fn_sym, block, expr->u.index_slice.hi,
                    ir_type_usize(b), &hi);
            } else {
                st = ir_builder_expr_to_value(
                    ctx, module, fn_sym, block, expr->u.index_slice.hi,
                    NULL, &hi);
                if (st == IR_BUILDER_OK &&
                    (hi == NULL || hi->type == NULL ||
                     hi->type->kind != IRT_USIZE)) {
                    return IR_BUILDER_UNSUPPORTED;   /* gap 4 */
                }
            }
            if (st != IR_BUILDER_OK) {
                return st;
            }
        }
        if (bt->kind == IRT_STR) {
            stype = ir_type_str(b);
            trap = "AIC-R0808";   /* code-point boundary obligation */
        } else if (bt->kind == IRT_ARRAY) {
            stype = ir_type_slice(b, bt->u.array.elem);
            trap = "AIC-R0807";
        } else {
            stype = ir_type_slice(b, bt->u.slice.elem);
            trap = "AIC-R0807";
        }
        if (stype == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_trap_node(ctx, IR_SLICE, expr->span, ck, stype, trap);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.slice.base = base_node;
        n->u.slice.start = lo;
        n->u.slice.end = hi;
        out->cat = IR_EXPR_COMPOSITE;
        out->node = n;
        out->type = stype;
        return IR_BUILDER_OK;
    }

    case AST_EXPR_BINARY:
        return lower_binary(ctx, module, fn_sym, block, expr, want,
                            expected, out);

    case AST_EXPR_TERNARY:
        return lower_ternary(ctx, module, fn_sym, block, expr, want,
                             expected, out);

    case AST_EXPR_CALL:
        return lower_call(ctx, module, fn_sym, block, expr, want,
                          expected, out);

    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        return lower_member(ctx, module, fn_sym, block, expr, want,
                            expected, out);

    case AST_EXPR_ASSIGN:
        return lower_assign(ctx, module, fn_sym, block, expr, want,
                            expected, out);

    case AST_EXPR_STRUCT_INIT:
        return lower_struct_literal(ctx, module, fn_sym, block, expr,
                                    want, expected, out);

    case AST_EXPR_ARRAY_LITERAL:
        return lower_array_literal(ctx, module, fn_sym, block, expr,
                                   want, expected, out);

    case AST_EXPR_CAST:
    case AST_EXPR_WRAP: {
        IrType *t = ir_builder_type_from_ast(ctx, module,
                                             expr->u.cast_wrap.type);
        IrNode *v = NULL;
        IrNode *n;
        IrBuilderStatus st;
        if (t == NULL) {
            return ctx->build->oom ? IR_BUILDER_OOM
                                   : IR_BUILDER_UNSUPPORTED;
        }
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.cast_wrap.expr, NULL, &v);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (v == NULL || v->type == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (expr->kind == AST_EXPR_WRAP) {
            n = mk_value_node(ctx, IR_WRAP, expr->span, ck, t);
        } else {
            /* IR_CAST: u8[] -> str carries the UTF-8 obligation
             * (AIC-R0806); all other checked conversions carry
             * AIC-R0801 (contract 5.3). */
            const char *trap = "AIC-R0801";
            if (t->kind == IRT_STR) {
                trap = "AIC-R0806";
            }
            n = mk_trap_node(ctx, IR_CAST, expr->span, ck, t, trap);
        }
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.cast_wrap.value = v;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = t;
        return IR_BUILDER_OK;
    }

    default:
        return IR_BUILDER_UNSUPPORTED;
    }
}

/* Lower an identifier reference (also used by lower_member for
 * module-qualified / enum-member references). */
static IrBuilderStatus lower_symbol_ref(BuilderCtx *ctx,
                                        const NameModule *module,
                                        const NameSymbol *fn_sym,
                                        IrNode *block,
                                        const AstNode *expr,
                                        IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const NameSymbol *sym = name_symbol_for_node(module, expr);
    const char *ck = expr_kind_text(expr->kind);
    IrNode *fn_node;
    int64_t si;
    IrType *st;
    IrNode *n;

    if (sym == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    switch (sym->kind) {

    case NAME_SYM_PARAM:
    case NAME_SYM_LOCAL_VAR: {
        if (sym->kind == NAME_SYM_PARAM) {
            si = param_slot_index(ctx, fn_sym, sym);
        } else {
            si = local_slot_index(ctx, fn_sym, sym);
        }
        fn_node = find_fn_node(ctx, fn_sym);
        if (fn_node == NULL || si < 0 ||
            (size_t)si >= fn_node->u.function.nslots) {
            return IR_BUILDER_UNSUPPORTED;
        }
        st = fn_node->u.function.slots[si]->type;
        if (st == NULL || st->kind == IRT_VOID) {
            return IR_BUILDER_UNSUPPORTED;
        }
        n = mk_value_node(ctx, IR_LOCAL, expr->span, ck, st);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.local.slot_index = si;
        out->cat = ir_type_is_composite(st) ? IR_EXPR_COMPOSITE
                                            : IR_EXPR_LVALUE;
        out->node = n;
        out->type = st;
        return IR_BUILDER_OK;
    }

    case NAME_SYM_LOCAL_CONST: {
        /* a local const reference: evaluate the const initializer and
         * map it (the same path 16c1b uses for global consts) */
        EvalCtx ec;
        EvalValue ev;
        EvalFailure fail = EVAL_FAIL_NONE;
        EvalStatus est;
        IrType *lt = NULL;
        bool lnull = false;
        bool supported = true;
        IrConst *c;
        if (sym->decl == NULL || sym->decl->kind != AST_CONST_DECL ||
            sym->decl->u.local_decl.init == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (!expr_type(ctx, module, fn_sym, sym->decl->u.local_decl.init, NULL,
                       &lt, &lnull)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        eval_ctx_init(&ec, ctx->result, ctx->layout, module);
        est = const_eval_expr(&ec, sym->decl->u.local_decl.init, &ev,
                              &fail);
        if (est == EVAL_OOM || ec.oom) {
            eval_ctx_cleanup(&ec);
            return IR_BUILDER_OOM;
        }
        if (est != EVAL_OK) {
            eval_ctx_cleanup(&ec);
            return IR_BUILDER_UNSUPPORTED;
        }
        c = ir_builder_const_from_eval(ctx, module, lt,
                                       sym->decl->u.local_decl.init, &ev,
                                       &supported);
        eval_value_free(&ev);
        eval_ctx_cleanup(&ec);
        if (c == NULL) {
            return supported ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
        }
        {
            IrBuilderStatus stt = const_value_node(ctx, fn_sym, block, c,
                                                   expr->span, &n);
            if (stt != IR_BUILDER_OK) {
                return stt;
            }
        }
        out->cat = IR_EXPR_COMPOSITE;
        out->node = n;
        out->type = c->type;
        return IR_BUILDER_OK;
    }

    case NAME_SYM_GLOBAL_VAR: {
        IrNode *gn = find_decl_node(b, sym->fqn);
        if (gn == NULL || gn->kind != IR_GLOBAL_VAR) {
            return IR_BUILDER_UNSUPPORTED;
        }
        st = gn->u.global_var.type;
        n = mk_value_node(ctx, IR_GLOBAL, expr->span, ck, st);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.global.target = gn;
        out->cat = ir_type_is_composite(st) ? IR_EXPR_COMPOSITE
                                            : IR_EXPR_LVALUE;
        out->node = n;
        out->type = st;
        return IR_BUILDER_OK;
    }

    case NAME_SYM_GLOBAL_CONST: {
        IrNode *cn = find_decl_node(b, sym->fqn);
        if (cn == NULL || cn->kind != IR_GLOBAL_CONST ||
            cn->u.global_const.value == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        {
            IrBuilderStatus stt = const_value_node(
                ctx, fn_sym, block, cn->u.global_const.value, expr->span,
                &n);
            if (stt != IR_BUILDER_OK) {
                return stt;
            }
        }
        out->cat = IR_EXPR_COMPOSITE;
        out->node = n;
        out->type = cn->u.global_const.value->type;
        return IR_BUILDER_OK;
    }

    case NAME_SYM_ENUM_MEMBER: {
        IrNode *ed = sym->owner != NULL
                         ? find_decl_node(b, sym->owner->fqn) : NULL;
        IrType *et;
        int64_t idx;
        IrConst *c;
        if (ed == NULL || ed->kind != IR_ENUM_DECL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        idx = field_index_of(ed, sym->name);
        if (idx < 0) {
            return IR_BUILDER_UNSUPPORTED;
        }
        et = ir_type_enum(b, ed);
        if (et == NULL) {
            return IR_BUILDER_OOM;
        }
        c = ir_const_enum(b, et,
                          (uint64_t)ed->u.enum_decl.members[idx].value);
        if (c == NULL) {
            return IR_BUILDER_OOM;
        }
        n = mk_value_node(ctx, IR_ENUM_VAL, expr->span, ck, et);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.constant.value = c;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = et;
        return IR_BUILDER_OK;
    }

    default:
        return IR_BUILDER_UNSUPPORTED;
    }
}

/* Binary operator node kind and trap code per contract 5.3. */
typedef struct BinOpSpec {
    IrNodeKind kind;
    const char *trap;
} BinOpSpec;

static bool binary_op_spec(AstBinaryOp op, BinOpSpec *out)
{
    switch (op) {
    case AST_BIN_MUL: out->kind = IR_MUL; out->trap = "AIC-R0802"; break;
    case AST_BIN_DIV: out->kind = IR_DIV; out->trap = "AIC-R0803"; break;
    case AST_BIN_MOD: out->kind = IR_MOD; out->trap = "AIC-R0803"; break;
    case AST_BIN_ADD: out->kind = IR_ADD; out->trap = "AIC-R0802"; break;
    case AST_BIN_SUB: out->kind = IR_SUB; out->trap = "AIC-R0802"; break;
    case AST_BIN_SHL: out->kind = IR_SHL; out->trap = "AIC-R0804"; break;
    case AST_BIN_SHR: out->kind = IR_SHR; out->trap = "AIC-R0804"; break;
    case AST_BIN_LT:  out->kind = IR_LT;  out->trap = NULL; break;
    case AST_BIN_LE:  out->kind = IR_LE;  out->trap = NULL; break;
    case AST_BIN_GT:  out->kind = IR_GT;  out->trap = NULL; break;
    case AST_BIN_GE:  out->kind = IR_GE;  out->trap = NULL; break;
    case AST_BIN_EQ:  out->kind = IR_EQ;  out->trap = NULL; break;
    case AST_BIN_NE:  out->kind = IR_NE;  out->trap = NULL; break;
    case AST_BIN_BAND: out->kind = IR_BAND; out->trap = NULL; break;
    case AST_BIN_BXOR: out->kind = IR_BXOR; out->trap = NULL; break;
    case AST_BIN_BOR:  out->kind = IR_BOR;  out->trap = NULL; break;
    case AST_BIN_LAND: out->kind = IR_LAND; out->trap = NULL; break;
    case AST_BIN_LOR:  out->kind = IR_LOR;  out->trap = NULL; break;
    default: return false;
    }
    return true;
}

static bool is_slice_pair(const IrType *lt, const IrType *rt)
{
    return lt != NULL && rt != NULL && lt->kind == IRT_SLICE &&
           rt->kind == IRT_SLICE;
}

static IrBuilderStatus lower_binary(BuilderCtx *ctx,
                                    const NameModule *module,
                                    const NameSymbol *fn_sym,
                                    IrNode *block,
                                    const AstNode *expr,
                                    IrExprWant want,
                                    IrType *expected,
                                    IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const char *ck = expr_kind_text(expr->kind);
    AstBinaryOp op = expr->u.binary.op;
    BinOpSpec spec;
    IrNode *lhs = NULL, *rhs = NULL;
    IrBuilderStatus st;
    IrNode *n;
    bool lhs_is_null, rhs_is_null;

    (void)want;
    (void)expected;
    if (!binary_op_spec(op, &spec)) {
        return IR_BUILDER_UNSUPPORTED;
    }

    /* && / || : bool operands, short-circuit (contract 5.3) */
    if (op == AST_BIN_LAND || op == AST_BIN_LOR) {
        IrType *bt = ir_type_bool(b);
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.binary.lhs, NULL, &lhs);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (lhs == NULL || lhs->type == NULL ||
            lhs->type->kind != IRT_BOOL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.binary.rhs, NULL, &rhs);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (rhs == NULL || rhs->type == NULL ||
            rhs->type->kind != IRT_BOOL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        n = mk_value_node(ctx, spec.kind, expr->span, ck, bt);
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.binary.left = lhs;
        n->u.binary.right = rhs;
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = bt;
        return IR_BUILDER_OK;
    }

    lhs_is_null = (expr->u.binary.lhs != NULL &&
                   expr->u.binary.lhs->kind == AST_EXPR_NULL_LITERAL);
    rhs_is_null = (expr->u.binary.rhs != NULL &&
                   expr->u.binary.rhs->kind == AST_EXPR_NULL_LITERAL);

    {
        IrType *lt = NULL, *rt = NULL;
        bool lnull = false, rnull = false;
        IrType *result_type = NULL;
        bool result_bool = false;
        bool is_shift = (op == AST_BIN_SHL || op == AST_BIN_SHR);
        bool is_cmp = (op == AST_BIN_LT || op == AST_BIN_LE ||
                       op == AST_BIN_GT || op == AST_BIN_GE ||
                       op == AST_BIN_EQ || op == AST_BIN_NE);

        if (lhs_is_null) {
            lnull = true;
        } else if (!expr_type(ctx, module, fn_sym, expr->u.binary.lhs, NULL, &lt,
                              &lnull)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (rhs_is_null) {
            rnull = true;
        } else if (!expr_type(ctx, module, fn_sym, expr->u.binary.rhs, NULL, &rt,
                              &rnull)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (is_cmp) {
            result_type = ir_type_bool(b);
            result_bool = true;
        } else if (is_shift) {
            result_type = lt;
        } else if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
            if (lt->kind == IRT_PTR && ir_type_is_int(rt)) {
                result_type = lt;
            } else if (ir_type_is_int(lt) && rt->kind == IRT_PTR) {
                result_type = rt;
            } else if (op == AST_BIN_SUB && lt->kind == IRT_PTR &&
                       rt->kind == IRT_PTR &&
                       ir_type_identical(lt->u.ptr.elem,
                                         rt->u.ptr.elem)) {
                result_type = ir_type_isize(b);
            } else {
                result_type = common_int_type(ctx, lt, rt);
            }
        } else {
            result_type = common_int_type(ctx, lt, rt);
        }
        if (result_type == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (!is_cmp && (lnull || rnull)) {
            /* null only participates in comparisons */
            return IR_BUILDER_UNSUPPORTED;
        }
        if (is_cmp && lnull && rnull) {
            return IR_BUILDER_UNSUPPORTED;   /* null == null: pre-IR */
        }
        if (is_cmp && (lnull || rnull)) {
            /* p == null / null == p: the null side takes the pointer
             * type of the other side */
            if (lnull) {
                if (rt == NULL || rt->kind != IRT_PTR) {
                    return IR_BUILDER_UNSUPPORTED;
                }
            } else {
                if (lt == NULL || lt->kind != IRT_PTR) {
                    return IR_BUILDER_UNSUPPORTED;
                }
            }
        } else if (is_cmp) {
            /* same-type operands (invariant 4): constants re-typed to
             * the common type; mixed-width runtime operands are a
             * disclosed gap (header note 5) */
            if (ir_type_is_int(lt) && ir_type_is_int(rt)) {
                IrType *ct = common_int_type(ctx, lt, rt);
                if (ct == NULL) {
                    return IR_BUILDER_UNSUPPORTED;
                }
            } else if (is_slice_pair(lt, rt)) {
                if (!ir_type_identical(lt, rt)) {
                    return IR_BUILDER_UNSUPPORTED;
                }
            } else if (!ir_type_identical(lt, rt)) {
                return IR_BUILDER_UNSUPPORTED;
            }
        }

        /* Lower the operands in spec order (left fully, then right). */
        {
            IrType *lhs_expected = NULL;
            IrType *rhs_expected = NULL;
            if (is_shift) {
                /* right operand assignment-converted to the left type */
                rhs_expected = lt;
            } else if (is_cmp) {
                if (lnull) {
                    lhs_expected = rt;
                } else if (rnull) {
                    rhs_expected = lt;
                } else if (ir_type_is_int(lt) && ir_type_is_int(rt)) {
                    IrType *ct = common_int_type(ctx, lt, rt);
                    lhs_expected = ct;
                    rhs_expected = ct;
                }
            }
            st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                          expr->u.binary.lhs,
                                          lhs_expected, &lhs);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                          expr->u.binary.rhs,
                                          rhs_expected, &rhs);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        }

        /* Operand type validation (defensive; accepted builds pass) */
        if (lhs == NULL || rhs == NULL || lhs->type == NULL ||
            rhs->type == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (is_cmp) {
            if (!ir_type_identical(lhs->type, rhs->type)) {
                return IR_BUILDER_UNSUPPORTED;   /* gap 5 */
            }
        } else if (is_shift) {
            if (!ir_type_is_int(lhs->type) || !ir_type_is_int(rhs->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
        } else if (op == AST_BIN_ADD || op == AST_BIN_SUB) {
            if (result_type->kind == IRT_PTR) {
                if (!(lhs->type->kind == IRT_PTR ||
                      rhs->type->kind == IRT_PTR)) {
                    return IR_BUILDER_UNSUPPORTED;
                }
            } else if (op == AST_BIN_SUB &&
                       result_type->kind == IRT_ISIZE &&
                       lhs->type->kind == IRT_PTR &&
                       rhs->type->kind == IRT_PTR) {
                /* p - q -> IR_PTR_DIFF (isize result): pointer operands
                 * are valid here (unlike the integer-only fallback) */
            } else if (!ir_type_is_int(lhs->type) ||
                       !ir_type_is_int(rhs->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
        } else {
            if (!ir_type_is_int(lhs->type) || !ir_type_is_int(rhs->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
        }

        /* Build the node. */
        if (result_type->kind == IRT_PTR &&
            (op == AST_BIN_ADD || op == AST_BIN_SUB) &&
            result_type != ir_type_isize(b) &&
            lhs->type->kind == IRT_PTR) {
            /* pointer +/- integer -> IR_PTR_ADD / IR_PTR_SUB */
            n = mk_trap_node(ctx, op == AST_BIN_ADD ? IR_PTR_ADD
                                                    : IR_PTR_SUB,
                             expr->span, ck, result_type, "AIC-R0816");
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.ptr_arith.ptr = lhs;
            n->u.ptr_arith.offset = rhs;
        } else if (result_type->kind == IRT_PTR &&
                   (op == AST_BIN_ADD || op == AST_BIN_SUB) &&
                   rhs->type->kind == IRT_PTR &&
                   lhs->type->kind != IRT_PTR) {
            /* integer + pointer -> IR_PTR_ADD with the pointer rhs */
            n = mk_trap_node(ctx, op == AST_BIN_ADD ? IR_PTR_ADD
                                                    : IR_PTR_SUB,
                             expr->span, ck, result_type, "AIC-R0816");
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.ptr_arith.ptr = rhs;
            n->u.ptr_arith.offset = lhs;
        } else if (op == AST_BIN_SUB && result_type->kind == IRT_ISIZE &&
                   lhs->type->kind == IRT_PTR &&
                   rhs->type->kind == IRT_PTR) {
            /* p - q -> IR_PTR_DIFF */
            n = mk_trap_node(ctx, IR_PTR_DIFF, expr->span, ck,
                             result_type, "AIC-R0816");
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.binary.left = lhs;
            n->u.binary.right = rhs;
        } else if (op == AST_BIN_EQ || op == AST_BIN_NE) {
            /* slice equality -> IR_SLICE_EQ; otherwise IR_EQ/IR_NE */
            if (lhs->type->kind == IRT_SLICE) {
                n = mk_value_node(ctx, IR_SLICE_EQ, expr->span, ck,
                                  result_type);
            } else {
                n = mk_value_node(ctx, spec.kind, expr->span, ck,
                                  result_type);
            }
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.binary.left = lhs;
            n->u.binary.right = rhs;
        } else {
            n = mk_trap_node(ctx, spec.kind, expr->span, ck, result_type,
                             spec.trap);
            if (n == NULL) {
                return IR_BUILDER_OOM;
            }
            n->u.binary.left = lhs;
            n->u.binary.right = rhs;
        }
        out->cat = IR_EXPR_SCALAR;
        out->node = n;
        out->type = result_type;
        (void)result_bool;
        return IR_BUILDER_OK;
    }
}

static IrBuilderStatus lower_ternary(BuilderCtx *ctx,
                                     const NameModule *module,
                                     const NameSymbol *fn_sym,
                                     IrNode *block,
                                     const AstNode *expr,
                                     IrExprWant want,
                                     IrType *expected,
                                     IrExprResult *out)
{
    const char *ck = expr_kind_text(expr->kind);
    IrNode *cond = NULL, *tv = NULL, *ev = NULL;
    IrBuilderStatus st;
    IrType *tt = NULL, *et = NULL;
    bool tnull = false, enull = false;
    IrType *result_type;
    IrNode *n;
    IrExprResult tr, er;
    bool then_is_null, else_is_null;

    (void)want;
    (void)expected;
    if (expr->u.branch.cond == NULL || expr->u.branch.then == NULL ||
        expr->u.branch.els == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                  expr->u.branch.cond, NULL, &cond);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (cond == NULL || cond->type == NULL ||
        cond->type->kind != IRT_BOOL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    then_is_null = (expr->u.branch.then->kind == AST_EXPR_NULL_LITERAL);
    else_is_null = (expr->u.branch.els->kind == AST_EXPR_NULL_LITERAL);
    if (then_is_null && else_is_null) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (!then_is_null &&
        !expr_type(ctx, module, fn_sym, expr->u.branch.then, NULL, &tt, &tnull)) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (!else_is_null &&
        !expr_type(ctx, module, fn_sym, expr->u.branch.els, NULL, &et, &enull)) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (then_is_null) {
        if (et == NULL || et->kind != IRT_PTR) {
            return IR_BUILDER_UNSUPPORTED;
        }
        result_type = et;
    } else if (else_is_null) {
        if (tt == NULL || tt->kind != IRT_PTR) {
            return IR_BUILDER_UNSUPPORTED;
        }
        result_type = tt;
    } else {
        result_type = common_int_type(ctx, tt, et);
        if (result_type == NULL) {
            if (!ir_type_identical(tt, et)) {
                return IR_BUILDER_UNSUPPORTED;   /* gap 5 */
            }
            result_type = tt;
        }
    }
    if (ir_type_is_composite(result_type)) {
        /* composite branches: lower naturally; literal branches
         * materialize eagerly (header gap note 6) */
        st = lower_expr(ctx, module, fn_sym, block, expr->u.branch.then,
                        IR_EXPR_WANT_ANY, result_type, &tr);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        st = lower_expr(ctx, module, fn_sym, block, expr->u.branch.els,
                        IR_EXPR_WANT_ANY, result_type, &er);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (tr.cat == IR_EXPR_COMPOSITE && !is_lvalue_node(tr.node)) {
            st = materialize(ctx, fn_sym, block, expr->span, &tr, &tv);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        } else {
            tv = tr.node;
        }
        if (er.cat == IR_EXPR_COMPOSITE && !is_lvalue_node(er.node)) {
            st = materialize(ctx, fn_sym, block, expr->span, &er, &ev);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        } else {
            ev = er.node;
        }
    } else {
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.branch.then,
                                      then_is_null ? result_type : NULL,
                                      &tv);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.branch.els,
                                      else_is_null ? result_type : NULL,
                                      &ev);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (tv == NULL || ev == NULL || tv->type == NULL ||
            ev->type == NULL || !ir_type_identical(tv->type, ev->type)) {
            return IR_BUILDER_UNSUPPORTED;   /* gap 5 */
        }
    }
    n = mk_value_node(ctx, IR_SELECT, expr->span, ck, result_type);
    if (n == NULL) {
        return IR_BUILDER_OOM;
    }
    n->u.select.cond = cond;
    n->u.select.then_value = tv;
    n->u.select.else_value = ev;
    out->cat = ir_type_is_composite(result_type) ? IR_EXPR_COMPOSITE
                                                 : IR_EXPR_SCALAR;
    out->node = n;
    out->type = result_type;
    return IR_BUILDER_OK;
}

static IrBuilderStatus lower_call(BuilderCtx *ctx,
                                  const NameModule *module,
                                  const NameSymbol *fn_sym,
                                  IrNode *block,
                                  const AstNode *expr,
                                  IrExprWant want,
                                  IrType *expected,
                                  IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const char *ck = expr_kind_text(expr->kind);
    const NameSymbol *fsym =
        name_symbol_for_node(module, expr->u.call.callee);
    IrNode *callee_node;
    IrNode *call;
    IrType *ret_type;
    size_t i;
    IrBuilderStatus st;

    (void)want;
    (void)expected;
    if (fsym == NULL || fsym->kind != NAME_SYM_FN) {
        return IR_BUILDER_UNSUPPORTED;   /* non-function callee: pre-IR */
    }
    callee_node = find_decl_node(b, fsym->fqn);
    if (callee_node == NULL || callee_node->kind != IR_FUNCTION) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (fsym->decl == NULL) {
        /* runtime built-in: attach the spec signature on first use */
        st = ensure_runtime_signature(ctx, callee_node);
        if (st != IR_BUILDER_OK) {
            return st;
        }
    }
    if (callee_node->u.function.nparams != expr->u.call.nargs) {
        return IR_BUILDER_UNSUPPORTED;   /* count mismatch: pre-IR
                                          * (AIC-T0312); defensive */
    }
    ret_type = callee_node->u.function.ret_type;
    call = mk_value_node(ctx, IR_CALL, expr->span, ck, ret_type);
    if (call == NULL) {
        return IR_BUILDER_OOM;
    }
    call->u.call.callee = callee_node;
    for (i = 0; i < expr->u.call.nargs; i++) {
        const AstNode *arg_ast = expr->u.call.args[i];
        const IrType *pt = callee_node->u.function.params[i].type;
        IrNode *arg = NULL;
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      arg_ast, (IrType *)pt, &arg);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (arg == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        ir_call_add_arg(b, call, arg);
        if (b->oom) {
            return IR_BUILDER_OOM;
        }
    }
    out->cat = ir_type_is_composite(ret_type) ? IR_EXPR_COMPOSITE
                                              : IR_EXPR_SCALAR;
    out->node = call;
    out->type = ret_type;
    return IR_BUILDER_OK;
}

static IrBuilderStatus lower_member(BuilderCtx *ctx,
                                    const NameModule *module,
                                    const NameSymbol *fn_sym,
                                    IrNode *block,
                                    const AstNode *expr,
                                    IrExprWant want,
                                    IrType *expected,
                                    IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const char *ck = expr_kind_text(expr->kind);
    const NameSymbol *msym = name_symbol_for_node(module, expr);
    IrBuilderStatus st;
    IrExprResult br;
    IrType *bt = NULL;
    const IrType *struct_t = NULL;
    IrNode *sdecl = NULL;
    int64_t fidx;
    IrType *ft;
    IrNode *base_node = NULL;
    IrNode *n;

    (void)expected;
    (void)want;
    if (msym != NULL) {
        if (msym->kind == NAME_SYM_ENUM_MEMBER ||
            msym->kind == NAME_SYM_GLOBAL_VAR ||
            msym->kind == NAME_SYM_GLOBAL_CONST) {
            /* module-qualified / enum-member reference: same path as
             * an identifier */
            return lower_symbol_ref(ctx, module, fn_sym, block, expr, out);
        }
    }
    if (expr->u.member.base == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (expr->kind == AST_EXPR_ARROW) {
        /* p->f == (*p).f (spec 12.6): lower the pointer, deref, field */
        IrNode *p = NULL;
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.member.base, NULL, &p);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (p == NULL || p->type == NULL || p->type->kind != IRT_PTR ||
            p->type->u.ptr.elem == NULL ||
            p->type->u.ptr.elem->kind != IRT_STRUCT) {
            return IR_BUILDER_UNSUPPORTED;
        }
        struct_t = p->type->u.ptr.elem;
        sdecl = struct_t->u.decl;
        fidx = field_index_of(sdecl, expr->u.member.name);
        if (fidx < 0) {
            return IR_BUILDER_UNSUPPORTED;
        }
        ft = sdecl->u.struct_decl.fields[fidx].type;
        base_node = mk_trap_node(ctx, IR_DEREF, expr->span, ck,
                                 (IrType *)struct_t, "AIC-R0809");
        if (base_node == NULL) {
            return IR_BUILDER_OOM;
        }
        base_node->u.deref.ptr = p;
        n = mk_value_node(ctx, IR_FIELD_ADDR, expr->span, ck,
                          ir_type_ptr(b, ft));
        if (n == NULL) {
            return IR_BUILDER_OOM;
        }
        n->u.field_addr.base = base_node;
        n->u.field_addr.field_index = fidx;
        out->cat = IR_EXPR_LVALUE;
        out->node = n;
        out->type = ft;
        return IR_BUILDER_OK;
    }
    /* value member access: s.f */
    st = lower_expr(ctx, module, fn_sym, block, expr->u.member.base,
                    IR_EXPR_WANT_ANY, NULL, &br);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    bt = br.type;
    if (bt == NULL || bt->kind != IRT_STRUCT) {
        return IR_BUILDER_UNSUPPORTED;
    }
    struct_t = bt;
    sdecl = struct_t->u.decl;
    fidx = field_index_of(sdecl, expr->u.member.name);
    if (fidx < 0) {
        return IR_BUILDER_UNSUPPORTED;
    }
    ft = sdecl->u.struct_decl.fields[fidx].type;
    if (br.cat == IR_EXPR_COMPOSITE && !is_lvalue_node(br.node)) {
        st = materialize(ctx, fn_sym, block, expr->span, &br, &base_node);
        if (st != IR_BUILDER_OK) {
            return st;
        }
    } else {
        base_node = br.node;
    }
    n = mk_value_node(ctx, IR_FIELD_ADDR, expr->span, ck,
                      ir_type_ptr(b, ft));
    if (n == NULL) {
        return IR_BUILDER_OOM;
    }
    n->u.field_addr.base = base_node;
    n->u.field_addr.field_index = fidx;
    out->cat = ir_type_is_composite(ft) ? IR_EXPR_COMPOSITE
                                        : IR_EXPR_LVALUE;
    out->node = n;
    out->type = ft;
    return IR_BUILDER_OK;
}

static IrBuilderStatus lower_assign(BuilderCtx *ctx,
                                    const NameModule *module,
                                    const NameSymbol *fn_sym,
                                    IrNode *block,
                                    const AstNode *expr,
                                    IrExprWant want,
                                    IrType *expected,
                                    IrExprResult *out)
{
    const char *ck = expr_kind_text(expr->kind);
    IrNode *dest = NULL;
    IrNode *store;
    IrBuilderStatus st;
    IrType *dt = NULL;

    (void)expected;
    if (want == IR_EXPR_WANT_VALUE) {
        /* gap 3: assignment has no value form in the closed IR */
        return IR_BUILDER_UNSUPPORTED;
    }
    /* destination location first (spec 10.4; contract 5.3 IR_STORE) */
    st = ir_builder_expr_to_lvalue(ctx, module, fn_sym, block,
                                   expr->u.assign.target, &dest);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (dest == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    dt = (IrType *)value_at_type(dest);
    if (dt == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (expr->u.assign.op == AST_ASGN_ASSIGN) {
        IrNode *val = NULL;
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.assign.value, dt, &val);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (val == NULL || val->type == NULL ||
            !ir_type_identical(val->type, dt)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        store = mk_trap_node(ctx, IR_STORE, expr->span, ck, NULL, NULL);
        if (store == NULL) {
            return IR_BUILDER_OOM;
        }
        store->u.store.dest = dest;
        store->u.store.value = val;
    } else {
        /* compound: dest-location + source + op + store (contract 9.7).
         * The IR's fixed per-node child order (IR_STORE: destination
         * then value; the binary op: left then right) evaluates the
         * destination read before the source expression; the source is
         * lowered first in construction order. See the ordering note in
         * the completion report / header gap note. */
        BinOpSpec spec;
        AstBinaryOp bop;
        IrNode *src = NULL;
        IrNode *loaded = NULL;
        IrNode *op_node;
        switch (expr->u.assign.op) {
        case AST_ASGN_ADD: bop = AST_BIN_ADD; break;
        case AST_ASGN_SUB: bop = AST_BIN_SUB; break;
        case AST_ASGN_MUL: bop = AST_BIN_MUL; break;
        case AST_ASGN_DIV: bop = AST_BIN_DIV; break;
        case AST_ASGN_MOD: bop = AST_BIN_MOD; break;
        case AST_ASGN_SHL: bop = AST_BIN_SHL; break;
        case AST_ASGN_SHR: bop = AST_BIN_SHR; break;
        case AST_ASGN_BAND: bop = AST_BIN_BAND; break;
        case AST_ASGN_BOR: bop = AST_BIN_BOR; break;
        case AST_ASGN_BXOR: bop = AST_BIN_BXOR; break;
        default: return IR_BUILDER_UNSUPPORTED;
        }
        if (!binary_op_spec(bop, &spec)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        st = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                      expr->u.assign.value, NULL, &src);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        if (src == NULL || src->type == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (dt->kind == IRT_PTR) {
            /* pointer += / -= (spec 12.5): the offset is any integer */
            if (!ir_type_is_int(src->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            op_node = mk_trap_node(ctx,
                                   bop == AST_BIN_ADD ? IR_PTR_ADD
                                                      : IR_PTR_SUB,
                                   expr->span, ck, dt, "AIC-R0816");
            if (op_node == NULL) {
                return IR_BUILDER_OOM;
            }
            op_node->u.ptr_arith.ptr = dest;
            op_node->u.ptr_arith.offset = src;
        } else {
            IrType *lt = (IrType *)value_at_type(dest);
            IrType *ct;
            if (!ir_type_is_int(lt) || !ir_type_is_int(src->type)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            ct = common_int_type(ctx, lt, src->type);
            if (ct == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            loaded = mk_trap_node(ctx, IR_LOAD, expr->span, ck, lt, NULL);
            if (loaded == NULL) {
                return IR_BUILDER_OOM;
            }
            loaded->u.load.lvalue = dest;
            if (bop == AST_BIN_SHL || bop == AST_BIN_SHR) {
                /* shift result = destination (left operand) type */
                op_node = mk_trap_node(ctx, spec.kind, expr->span, ck, lt,
                                       spec.trap);
            } else {
                op_node = mk_trap_node(ctx, spec.kind, expr->span, ck, ct,
                                       spec.trap);
            }
            if (op_node == NULL) {
                return IR_BUILDER_OOM;
            }
            op_node->u.binary.left = loaded;
            op_node->u.binary.right = src;
        }
        store = mk_trap_node(ctx, IR_STORE, expr->span, ck, NULL, NULL);
        if (store == NULL) {
            return IR_BUILDER_OOM;
        }
        store->u.store.dest = dest;
        store->u.store.value = op_node;
        /* the op result type must match the destination (complete
         * object representation; the 11.6 assignability rule guarantees
         * this on accepted builds; otherwise the program is outside the
         * accepted surface and we refuse defensively) */
        if (!ir_type_identical(store->u.store.value->type, dt)) {
            return IR_BUILDER_UNSUPPORTED;
        }
    }
    out->cat = IR_EXPR_EFFECT;
    out->node = store;
    out->type = dt;
    return IR_BUILDER_OK;
}

static IrBuilderStatus lower_struct_literal(BuilderCtx *ctx,
                                            const NameModule *module,
                                            const NameSymbol *fn_sym,
                                            IrNode *block,
                                            const AstNode *expr,
                                            IrExprWant want,
                                            IrType *expected,
                                            IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const char *ck = expr_kind_text(expr->kind);
    const NameSymbol *bsym =
        name_symbol_for_node(module, expr->u.struct_init.base);
    IrNode *fn_node;
    IrNode *sdecl;
    IrType *st;
    IrSlot *slot;
    IrNode *loc;
    IrNode *zero;
    size_t i;
    IrBuilderStatus stt;

    (void)want;
    (void)expected;
    if (bsym == NULL || bsym->kind != NAME_SYM_STRUCT) {
        return IR_BUILDER_UNSUPPORTED;
    }
    sdecl = find_decl_node(b, bsym->fqn);
    if (sdecl == NULL || sdecl->kind != IR_STRUCT_DECL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    st = ir_type_struct(b, sdecl);
    if (st == NULL) {
        return IR_BUILDER_OOM;
    }
    fn_node = find_fn_node(ctx, fn_sym);
    if (fn_node == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    slot = ir_builder_add_slot(b, fn_node, IR_SLOT_TEMP, NULL, st,
                               expr->span);
    if (slot == NULL) {
        return IR_BUILDER_OOM;
    }
    loc = mk_value_node(ctx, IR_LOCAL, expr->span, ck, st);
    if (loc == NULL) {
        return IR_BUILDER_OOM;
    }
    loc->u.local.slot_index = slot->index;
    zero = mk_trap_node(ctx, IR_ZERO, expr->span, ck, NULL, NULL);
    if (zero == NULL) {
        return IR_BUILDER_OOM;
    }
    zero->u.unary.operand = loc;
    ir_block_add_stmt(b, block, zero);
    if (b->oom) {
        return IR_BUILDER_OOM;
    }
    /* field stores in literal order (spec 12.7: initializer evaluation
     * left-to-right in literal order; padding zeroed by IR_ZERO) */
    for (i = 0; i < expr->u.struct_init.nfields; i++) {
        const AstNode *fi = expr->u.struct_init.fields[i];
        int64_t fidx;
        IrType *ft;
        IrNode *field_addr;
        IrNode *val;
        IrNode *store;
        if (fi == NULL || fi->u.named.name == NULL ||
            fi->u.named.value == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        fidx = field_index_of(sdecl, fi->u.named.name);
        if (fidx < 0) {
            return IR_BUILDER_UNSUPPORTED;   /* unknown field: pre-IR
                                              * AIC-T0313 */
        }
        ft = sdecl->u.struct_decl.fields[fidx].type;
        field_addr = mk_value_node(ctx, IR_FIELD_ADDR, fi->span, ck,
                                   ir_type_ptr(b, ft));
        if (field_addr == NULL) {
            return IR_BUILDER_OOM;
        }
        field_addr->u.field_addr.base = loc;
        field_addr->u.field_addr.field_index = fidx;
        if (ir_type_is_composite(ft)) {
            IrExprResult fr;
            stt = lower_expr(ctx, module, fn_sym, block, fi->u.named.value,
                             IR_EXPR_WANT_ANY, ft, &fr);
            if (stt != IR_BUILDER_OK) {
                return stt;
            }
            if (fr.cat == IR_EXPR_COMPOSITE && !is_lvalue_node(fr.node)) {
                stt = materialize(ctx, fn_sym, block, fi->span, &fr, &val);
                if (stt != IR_BUILDER_OK) {
                    return stt;
                }
            } else {
                val = fr.node;
            }
        } else {
            stt = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                           fi->u.named.value, ft, &val);
            if (stt != IR_BUILDER_OK) {
                return stt;
            }
        }
        if (val == NULL || val->type == NULL ||
            !ir_type_identical(val->type, ft)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        store = mk_trap_node(ctx, IR_STORE, fi->span, ck, NULL, NULL);
        if (store == NULL) {
            return IR_BUILDER_OOM;
        }
        store->u.store.dest = field_addr;
        store->u.store.value = val;
        ir_block_add_stmt(b, block, store);
        if (b->oom) {
            return IR_BUILDER_OOM;
        }
    }
    out->cat = IR_EXPR_COMPOSITE;
    out->node = loc;
    out->type = st;
    return IR_BUILDER_OK;
}

static IrBuilderStatus lower_array_literal(BuilderCtx *ctx,
                                           const NameModule *module,
                                           const NameSymbol *fn_sym,
                                           IrNode *block,
                                           const AstNode *expr,
                                           IrExprWant want,
                                           IrType *expected,
                                           IrExprResult *out)
{
    IrBuild *b = ctx->build;
    const char *ck = expr_kind_text(expr->kind);
    IrNode *fn_node;
    IrType *at;
    IrType *elem;
    int64_t extent;
    IrSlot *slot;
    IrNode *loc;
    IrNode *zero;
    IrBuilderStatus stt;

    (void)want;
    if (expected == NULL || expected->kind != IRT_ARRAY) {
        /* no type inference for standalone array literals (spec 12.7) */
        return IR_BUILDER_UNSUPPORTED;
    }
    at = expected;
    elem = at->u.array.elem;
    extent = at->u.array.extent;
    if (extent < 0) {
        return IR_BUILDER_UNSUPPORTED;
    }
    fn_node = find_fn_node(ctx, fn_sym);
    if (fn_node == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    slot = ir_builder_add_slot(b, fn_node, IR_SLOT_TEMP, NULL, at,
                               expr->span);
    if (slot == NULL) {
        return IR_BUILDER_OOM;
    }
    loc = mk_value_node(ctx, IR_LOCAL, expr->span, ck, at);
    if (loc == NULL) {
        return IR_BUILDER_OOM;
    }
    loc->u.local.slot_index = slot->index;
    zero = mk_trap_node(ctx, IR_ZERO, expr->span, ck, NULL, NULL);
    if (zero == NULL) {
        return IR_BUILDER_OOM;
    }
    zero->u.unary.operand = loc;
    ir_block_add_stmt(b, block, zero);
    if (b->oom) {
        return IR_BUILDER_OOM;
    }

    if (expr->u.array_literal.count != NULL) {
        /* repetition form [e; N] (IRC-N1): evaluate e exactly once and
         * store the single value into each of the N elements; when
         * N == 0 the value is still evaluated once (appended to the
         * block) */
        const AstNode *e_ast = (expr->u.array_literal.nelems > 0)
                                   ? expr->u.array_literal.elems[0] : NULL;
        IrNode *val = NULL;
        int64_t i;
        if (e_ast == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (ir_type_is_composite(elem)) {
            IrExprResult er;
            stt = lower_expr(ctx, module, fn_sym, block, e_ast,
                             IR_EXPR_WANT_ANY, elem, &er);
            if (stt != IR_BUILDER_OK) {
                return stt;
            }
            if (er.cat == IR_EXPR_COMPOSITE && !is_lvalue_node(er.node)) {
                stt = materialize(ctx, fn_sym, block, e_ast->span, &er,
                                  &val);
                if (stt != IR_BUILDER_OK) {
                    return stt;
                }
            } else {
                val = er.node;
            }
        } else {
            stt = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                           e_ast, elem, &val);
            if (stt != IR_BUILDER_OK) {
                return stt;
            }
        }
        if (val == NULL || val->type == NULL ||
            !ir_type_identical(val->type, elem)) {
            return IR_BUILDER_UNSUPPORTED;
        }
        for (i = 0; i < extent; i++) {
            IrNode *index;
            IrNode *index_addr;
            IrNode *store;
            IrConst *ic = ir_const_int(b, ir_type_usize(b), (uint64_t)i);
            if (ic == NULL) {
                return IR_BUILDER_OOM;
            }
            index = mk_value_node(ctx, IR_INT, expr->span, ck,
                                  ir_type_usize(b));
            if (index == NULL) {
                return IR_BUILDER_OOM;
            }
            index->u.constant.value = ic;
            index_addr = mk_trap_node(ctx, IR_INDEX_ADDR, expr->span, ck,
                                      ir_type_ptr(b, elem), "AIC-R0807");
            if (index_addr == NULL) {
                return IR_BUILDER_OOM;
            }
            index_addr->u.index_addr.base = loc;
            index_addr->u.index_addr.index = index;
            store = mk_trap_node(ctx, IR_STORE, expr->span, ck, NULL,
                                 NULL);
            if (store == NULL) {
                return IR_BUILDER_OOM;
            }
            store->u.store.dest = index_addr;
            store->u.store.value = val;
            ir_block_add_stmt(b, block, store);
            if (b->oom) {
                return IR_BUILDER_OOM;
            }
        }
        if (extent == 0) {
            /* IRC-N1: e evaluated exactly once even when N == 0; the
             * single evaluation is the val node - append it so it is
             * reachable and executed (block-appending convention) */
            ir_block_add_stmt(b, block, val);
            if (b->oom) {
                return IR_BUILDER_OOM;
            }
        }
    } else {
        /* element-list form [e0, ..., eN-1]: count must equal N
         * (AIC-T0309 pre-IR; defensive check here) */
        int64_t n = (int64_t)expr->u.array_literal.nelems;
        int64_t i;
        if (n != extent) {
            return IR_BUILDER_UNSUPPORTED;
        }
        for (i = 0; i < n; i++) {
            const AstNode *e_ast = expr->u.array_literal.elems[i];
            IrNode *val = NULL;
            IrNode *index;
            IrNode *index_addr;
            IrNode *store;
            IrConst *ic;
            if (e_ast == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (ir_type_is_composite(elem)) {
                IrExprResult er;
                stt = lower_expr(ctx, module, fn_sym, block, e_ast,
                                 IR_EXPR_WANT_ANY, elem, &er);
                if (stt != IR_BUILDER_OK) {
                    return stt;
                }
                if (er.cat == IR_EXPR_COMPOSITE &&
                    !is_lvalue_node(er.node)) {
                    stt = materialize(ctx, fn_sym, block, e_ast->span,
                                      &er, &val);
                    if (stt != IR_BUILDER_OK) {
                        return stt;
                    }
                } else {
                    val = er.node;
                }
            } else {
                stt = ir_builder_expr_to_value(ctx, module, fn_sym, block,
                                               e_ast, elem, &val);
                if (stt != IR_BUILDER_OK) {
                    return stt;
                }
            }
            if (val == NULL || val->type == NULL ||
                !ir_type_identical(val->type, elem)) {
                return IR_BUILDER_UNSUPPORTED;
            }
            ic = ir_const_int(b, ir_type_usize(b), (uint64_t)i);
            if (ic == NULL) {
                return IR_BUILDER_OOM;
            }
            index = mk_value_node(ctx, IR_INT, expr->span, ck,
                                  ir_type_usize(b));
            if (index == NULL) {
                return IR_BUILDER_OOM;
            }
            index->u.constant.value = ic;
            index_addr = mk_trap_node(ctx, IR_INDEX_ADDR, expr->span, ck,
                                      ir_type_ptr(b, elem), "AIC-R0807");
            if (index_addr == NULL) {
                return IR_BUILDER_OOM;
            }
            index_addr->u.index_addr.base = loc;
            index_addr->u.index_addr.index = index;
            store = mk_trap_node(ctx, IR_STORE, expr->span, ck, NULL,
                                 NULL);
            if (store == NULL) {
                return IR_BUILDER_OOM;
            }
            store->u.store.dest = index_addr;
            store->u.store.value = val;
            ir_block_add_stmt(b, block, store);
            if (b->oom) {
                return IR_BUILDER_OOM;
            }
        }
    }
    out->cat = IR_EXPR_COMPOSITE;
    out->node = loc;
    out->type = at;
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */

/* Whether the AST expression can possibly lower to an lvalue (a storage
 * location). Used as a pre-lowering guard for IR_EXPR_WANT_LVALUE so the
 * wrapper rejects a non-lvalue request BEFORE creating any node (the
 * "nothing owned" contract; a post-hoc rejection would leave an orphaned
 * node and violate invariant 1). Mirrors the lvalue-producing lowering
 * cases: identifier refs to params/locals/globals, member/arrow access on
 * a struct base, dereference, array/slice indexing, and parens. */
static bool expr_lvalue_candidate(BuilderCtx *ctx, const NameModule *module,
                                  const NameSymbol *fn_sym,
                                  const AstNode *expr)
{
    const NameSymbol *sym;
    if (expr == NULL) {
        return false;
    }
    switch (expr->kind) {
    case AST_EXPR_IDENT:
        sym = name_symbol_for_node(module, expr);
        return sym != NULL &&
               (sym->kind == NAME_SYM_PARAM ||
                sym->kind == NAME_SYM_LOCAL_VAR ||
                sym->kind == NAME_SYM_GLOBAL_VAR);
    case AST_EXPR_PAREN:
        return expr_lvalue_candidate(ctx, module, fn_sym,
                                     expr->u.paren.expr);
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        IrType *bt = NULL;
        bool bnull = false;
        sym = name_symbol_for_node(module, expr);
        if (sym != NULL &&
            (sym->kind == NAME_SYM_ENUM_MEMBER ||
             sym->kind == NAME_SYM_GLOBAL_CONST ||
             sym->kind == NAME_SYM_LOCAL_CONST)) {
            return false;   /* constant / enum value, not a location */
        }
        if (sym != NULL && sym->kind == NAME_SYM_GLOBAL_VAR) {
            return true;    /* module-qualified global var ref */
        }
        if (!expr_type(ctx, module, fn_sym, expr->u.member.base, NULL, &bt,
                       &bnull)) {
            return false;
        }
        if (expr->kind == AST_EXPR_ARROW) {
            /* p->f == (*p).f: the base is a pointer to struct */
            return bt != NULL && bt->kind == IRT_PTR &&
                   bt->u.ptr.elem != NULL &&
                   bt->u.ptr.elem->kind == IRT_STRUCT;
        }
        return bt != NULL && bt->kind == IRT_STRUCT;
    }
    case AST_EXPR_INDEX: {
        IrType *bt = NULL;
        bool bnull = false;
        if (!expr_type(ctx, module, fn_sym, expr->u.index_slice.base, NULL,
                       &bt, &bnull)) {
            return false;
        }
        /* array/slice indexing is an lvalue; str indexing yields a value
         * address, never an lvalue (AC2) */
        return bt != NULL &&
               (bt->kind == IRT_ARRAY || bt->kind == IRT_SLICE);
    }
    case AST_EXPR_UNARY:
        return expr->u.unary.op == AST_UN_DEREF;
    default:
        return false;
    }
}

IrBuilderStatus ir_builder_expr_lower(BuilderCtx *ctx,
                                      const NameModule *module,
                                      const NameSymbol *fn_sym,
                                      IrNode *block,
                                      const AstNode *expr,
                                      IrExprWant want,
                                      IrType *expected,
                                      IrExprResult *out)
{
    IrBuilderStatus st;
    if (out == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    memset(out, 0, sizeof(*out));
    if (want == IR_EXPR_WANT_LVALUE &&
        !expr_lvalue_candidate(ctx, module, fn_sym, expr)) {
        return IR_BUILDER_UNSUPPORTED;   /* nothing owned */
    }
    st = lower_expr(ctx, module, fn_sym, block, expr, want, expected,
                    out);
    if (st != IR_BUILDER_OK) {
        memset(out, 0, sizeof(*out));   /* nothing owned */
        return st;
    }
    if (out->node == NULL || out->type == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    /* enforce the requested value category */
    if (want == IR_EXPR_WANT_LVALUE && out->cat != IR_EXPR_LVALUE) {
        memset(out, 0, sizeof(*out));
        return IR_BUILDER_UNSUPPORTED;
    }
    if (want == IR_EXPR_WANT_VALUE) {
        if (out->cat == IR_EXPR_EFFECT) {
            memset(out, 0, sizeof(*out));
            return IR_BUILDER_UNSUPPORTED;   /* gap 3 */
        }
        if (out->cat == IR_EXPR_LVALUE) {
            /* scalar lvalues are loaded into values; composite lvalues
             * are already the object-image address */
            if (ir_type_is_composite(out->type)) {
                out->cat = IR_EXPR_COMPOSITE;
            } else {
                IrNode *load = mk_trap_node(
                    ctx, IR_LOAD, expr->span, "AST_EXPR", out->type,
                    out->type->kind == IRT_BOOL ? "AIC-R0805" : NULL);
                if (load == NULL) {
                    return IR_BUILDER_OOM;
                }
                load->u.load.lvalue = out->node;
                out->cat = IR_EXPR_SCALAR;
                out->node = load;
            }
        } else if (out->cat == IR_EXPR_COMPOSITE &&
                   !ir_type_is_composite(out->type)) {
            /* a value address with a scalar value-at-address (str
             * indexing): load the byte value (AC2: value address, never
             * an lvalue) */
            IrNode *load = mk_trap_node(
                ctx, IR_LOAD, expr->span, "AST_EXPR", out->type,
                out->type->kind == IRT_BOOL ? "AIC-R0805" : NULL);
            if (load == NULL) {
                return IR_BUILDER_OOM;
            }
            load->u.load.lvalue = out->node;
            out->cat = IR_EXPR_SCALAR;
            out->node = load;
        }
    }
    return IR_BUILDER_OK;
}

IrBuilderStatus ir_builder_expr_to_value(BuilderCtx *ctx,
                                         const NameModule *module,
                                         const NameSymbol *fn_sym,
                                         IrNode *block,
                                         const AstNode *expr,
                                         IrType *expected,
                                         IrNode **out_value)
{
    IrExprResult r;
    IrBuilderStatus st;
    if (out_value != NULL) {
        *out_value = NULL;
    }
    st = ir_builder_expr_lower(ctx, module, fn_sym, block, expr,
                               IR_EXPR_WANT_VALUE, expected, &r);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (out_value != NULL) {
        *out_value = r.node;
    }
    return IR_BUILDER_OK;
}

IrBuilderStatus ir_builder_expr_to_lvalue(BuilderCtx *ctx,
                                          const NameModule *module,
                                          const NameSymbol *fn_sym,
                                          IrNode *block,
                                          const AstNode *expr,
                                          IrNode **out_lvalue)
{
    IrExprResult r;
    IrBuilderStatus st;
    if (out_lvalue != NULL) {
        *out_lvalue = NULL;
    }
    st = ir_builder_expr_lower(ctx, module, fn_sym, block, expr,
                               IR_EXPR_WANT_LVALUE, NULL, &r);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (out_lvalue != NULL) {
        *out_lvalue = r.node;
    }
    return IR_BUILDER_OK;
}

IrBuilderStatus ir_builder_expr_to_any(BuilderCtx *ctx,
                                       const NameModule *module,
                                       const NameSymbol *fn_sym,
                                       IrNode *block,
                                       const AstNode *expr,
                                       IrType *expected,
                                       IrNode **out_node)
{
    IrExprResult r;
    IrBuilderStatus st;
    if (out_node != NULL) {
        *out_node = NULL;
    }
    st = ir_builder_expr_lower(ctx, module, fn_sym, block, expr,
                               IR_EXPR_WANT_ANY, expected, &r);
    if (st != IR_BUILDER_OK) {
        return st;
    }
    if (out_node != NULL) {
        *out_node = r.node;
    }
    return IR_BUILDER_OK;
}
