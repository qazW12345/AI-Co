/* bootstrap/src/ir/ir_builder_decl.c
 *
 * AI-Co Stage-0 IR builder Phase A mapping (WP-M0-16c1b).
 *
 * Implements the Phase A mapping of the accepted canonical IR contract
 * (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) sections 4.1-4.5 and
 * 6 over the resolved, validated build (WP-M0-10 NameResult + WP-M0-11b
 * LayoutBuild + WP-M0-12 constant evaluation), as declared in
 * ir_builder_decl.h.
 *
 * Construction order (determinism, contract 6.1): the module mapper runs
 * a creation pre-pass on its first invocation for a build, creating ALL
 * module/import/declaration nodes in canonical order (entry module
 * first, then imports depth-first in import order; within a module,
 * top-level declarations in source order) with their identity facts
 * (names, spans, struct size/align, enum underlying). The 16c1a driver
 * then calls the decl mapper per symbol in canonical order; each call
 * fills one declaration's detail (struct fields, enum members, global
 * const/var values, function params/slots/body). Because every
 * declaration node exists before any type/value mapping runs,
 * references to declarations in any module resolve as direct node
 * pointers (contract 4.1), and no deferral table is needed for
 * pointer-to-struct or address-of-global forms.
 *
 * Ownership and OOM discipline:
 *   - Build-owned payloads (names, spans, arrays, slot structs) are
 *     allocated with the helpers below, which set build->oom on failure;
 *     ir_build_free releases them.
 *   - Mapper-scratch state (the per-build symbol -> node table) is
 *     process-global (single-build compiler) and released at the start
 *     of the next build.
 *   - The NameResult and LayoutBuild are borrowed and never modified.
 */
#include "ir_builder_decl.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Per-build mapper scratch state
 * ------------------------------------------------------------------------- */

typedef struct DeclNodeEntry {
    const NameSymbol *sym;
    IrNode *node;
} DeclNodeEntry;

typedef struct DeclBuildState {
    const IrBuild *build_key;   /* ctx->build of the current construction */
    DeclNodeEntry *entries;     /* symbol -> declaration IR node */
    size_t nentries;
    size_t cap;
} DeclBuildState;

static DeclBuildState s_decl;

static void decl_state_reset(void)
{
    free(s_decl.entries);
    memset(&s_decl, 0, sizeof(s_decl));
}

static bool decl_state_grow(BuilderCtx *ctx)
{
    size_t ncap = s_decl.cap ? s_decl.cap * 2 : 16;
    DeclNodeEntry *p = (DeclNodeEntry *)realloc(
        s_decl.entries, ncap * sizeof(DeclNodeEntry));
    if (p == NULL) {
        ctx->oom = true;
        return false;
    }
    s_decl.entries = p;
    s_decl.cap = ncap;
    return true;
}

static void decl_state_add(BuilderCtx *ctx, const NameSymbol *sym,
                           IrNode *node)
{
    if (s_decl.nentries == s_decl.cap && !decl_state_grow(ctx)) {
        return;
    }
    s_decl.entries[s_decl.nentries].sym = sym;
    s_decl.entries[s_decl.nentries].node = node;
    s_decl.nentries++;
}

static IrNode *decl_state_find(const NameSymbol *sym)
{
    size_t i;
    if (sym == NULL) {
        return NULL;
    }
    for (i = 0; i < s_decl.nentries; i++) {
        if (s_decl.entries[i].sym == sym) {
            return s_decl.entries[i].node;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Small allocation helpers (build-owned memory; set build->oom on failure)
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

/* Attach one root cause link naming the source construct kind and its
 * span, with no resolved-reference facts (16c2 refines). */
static void add_cause(BuilderCtx *ctx, IrNode *node, const char *kind)
{
    ir_node_add_cause(ctx->build, node, kind, node->span, -1, -1, -1);
}

/* Deterministic synthetic point span for compiler-provided nodes that
 * have no source construct (runtime modules/functions). The file is the
 * node's fqn so the span is unique and deterministic per build. */
static DiagSpan *synthetic_span(const char *file)
{
    return diag_span_new_point(file != NULL ? file : "rt", 1, 1, 0);
}

/* ---------------------------------------------------------------------------
 * Declaration node creation (identity + self facts)
 * ------------------------------------------------------------------------- */

static IrBuilderStatus decl_new_decl(BuilderCtx *ctx, const NameModule *module,
                                     const NameSymbol *sym, IrNode **out);

/* Forward declaration for the type mapper (defined below). */
static IrType *decl_type_from_ast(BuilderCtx *ctx, const NameModule *module,
                                  const AstNode *type_node);

/* ---------------------------------------------------------------------------
 * Type mapping (contract 4.4; interning delegated to ir_core)
 * ------------------------------------------------------------------------- */

static IrType *base_type(BuilderCtx *ctx, AstPrimKind prim)
{
    IrBuild *b = ctx->build;
    switch (prim) {
    case AST_PRIM_VOID:   return ir_type_void(b);
    case AST_PRIM_BOOL:   return ir_type_bool(b);
    case AST_PRIM_STR:    return ir_type_str(b);
    case AST_PRIM_I8:     return ir_type_i8(b);
    case AST_PRIM_I16:    return ir_type_i16(b);
    case AST_PRIM_I32:    return ir_type_i32(b);
    case AST_PRIM_I64:    return ir_type_i64(b);
    case AST_PRIM_U8:     return ir_type_u8(b);
    case AST_PRIM_U16:    return ir_type_u16(b);
    case AST_PRIM_U32:    return ir_type_u32(b);
    case AST_PRIM_U64:    return ir_type_u64(b);
    case AST_PRIM_ISIZE:  return ir_type_isize(b);
    case AST_PRIM_USIZE:  return ir_type_usize(b);
    }
    return NULL;
}

IrType *ir_builder_type_from_type(BuilderCtx *ctx, const Type *type)
{
    IrBuild *b = ctx->build;
    IrType *elem;
    if (type == NULL) {
        return NULL;
    }
    switch (type->kind) {
    case TYPE_PRIM:
        return base_type(ctx, type->u.prim);
    case TYPE_ARRAY:
        elem = ir_builder_type_from_type(ctx, type->u.array.elem);
        if (elem == NULL) {
            return NULL;
        }
        return ir_type_array(b, elem, type->u.array.extent);
    case TYPE_SLICE:
        elem = ir_builder_type_from_type(ctx, type->u.slice.elem);
        if (elem == NULL) {
            return NULL;
        }
        return ir_type_slice(b, elem);
    case TYPE_PTR:
        elem = ir_builder_type_from_type(ctx, type->u.ptr.elem);
        if (elem == NULL) {
            return NULL;
        }
        return ir_type_ptr(b, elem);
    case TYPE_STRUCT:
    case TYPE_ENUM: {
        IrNode *decl = decl_state_find(type->u.sym);
        if (decl == NULL) {
            return NULL;
        }
        if (type->kind == TYPE_STRUCT) {
            return ir_type_struct(b, decl);
        }
        return ir_type_enum(b, decl);
    }
    }
    return NULL;
}

/* Evaluate one constant-expression site (array extent) and return the
 * int64 value; NULL on any non-EVAL_OK outcome (defensive; accepted
 * builds always evaluate array extents). */
static EvalValue *eval_int_expr(BuilderCtx *ctx,
                                const NameModule *module,
                                const AstNode *expr, EvalValue *out)
{
    EvalCtx ec;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    eval_ctx_init(&ec, ctx->result, ctx->layout, module);
    st = const_eval_expr(&ec, expr, out, &fail);
    eval_ctx_cleanup(&ec);
    if (st != EVAL_OK) {
        return NULL;
    }
    return out;
}

static IrType *decl_type_from_ast(BuilderCtx *ctx, const NameModule *module,
                                  const AstNode *type_node)
{
    IrBuild *b = ctx->build;
    if (type_node == NULL || module == NULL) {
        return NULL;
    }
    switch (type_node->kind) {
    case AST_TYPE_PRIM:
        return base_type(ctx, type_node->u.type_prim.prim);
    case AST_TYPE_NAMED: {
        const NameSymbol *sym = name_symbol_for_node(module, type_node);
        IrNode *decl;
        if (sym == NULL || (sym->kind != NAME_SYM_STRUCT &&
                            sym->kind != NAME_SYM_ENUM)) {
            return NULL;
        }
        decl = decl_state_find(sym);
        if (decl == NULL) {
            return NULL;
        }
        if (sym->kind == NAME_SYM_STRUCT) {
            return ir_type_struct(b, decl);
        }
        return ir_type_enum(b, decl);
    }
    case AST_TYPE_PTR: {
        IrType *elem = decl_type_from_ast(ctx, module,
                                          type_node->u.type_derived.base);
        if (elem == NULL) {
            return NULL;
        }
        return ir_type_ptr(b, elem);
    }
    case AST_TYPE_SLICE: {
        IrType *elem = decl_type_from_ast(ctx, module,
                                          type_node->u.type_derived.base);
        if (elem == NULL) {
            return NULL;
        }
        return ir_type_slice(b, elem);
    }
    case AST_TYPE_ARRAY: {
        IrType *elem = decl_type_from_ast(ctx, module,
                                          type_node->u.type_derived.base);
        EvalValue ev;
        EvalValue *r;
        int64_t extent;
        if (elem == NULL) {
            return NULL;
        }
        r = eval_int_expr(ctx, module, type_node->u.type_derived.len, &ev);
        if (r == NULL) {
            return NULL;
        }
        if (r->kind != EVAL_VAL_INT || r->u.i.big || r->u.i.v < 0) {
            eval_value_free(r);
            return NULL;
        }
        extent = r->u.i.v;
        eval_value_free(r);
        return ir_type_array(b, elem, extent);
    }
    default:
        return NULL;
    }
}

IrType *ir_builder_type_from_ast(BuilderCtx *ctx, const NameModule *module,
                                 const AstNode *type_node)
{
    return decl_type_from_ast(ctx, module, type_node);
}

/* ---------------------------------------------------------------------------
 * Constant mapping (contract 4.5/6.4; dedup delegated to ir_core)
 * ------------------------------------------------------------------------- */

/* Width in bits of an integer/underlying type (8/16/32/64; 0 otherwise). */
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
    default:
        return 0;
    }
}

/* Normalize a two's-complement bit pattern to the type's width
 * (contract 6.4: integer constants stored as exact bit patterns
 * normalized to the type's width). */
static uint64_t mask_to_width(uint64_t bits, int width)
{
    if (width <= 0) {
        return bits;
    }
    if (width >= 64) {
        return bits;
    }
    return bits & (((uint64_t)1 << width) - 1);
}

/* Follow a module-scope const-reference chain from the AST at a value
 * position (`expr`, an IDENT or module-qualified MEMBER) to the
 * terminal initializer AST. Returns the terminal initializer
 * (AST_EXPR_STRUCT_INIT or AST_EXPR_ARRAY_LITERAL) and its owning
 * module, or NULL when the chain does not reach a literal (defensive;
 * accepted builds evaluate const references, so the chain always ends
 * in the initializer the evaluator used). A small hop bound guards
 * against malformed cycles (accepted builds cannot contain them: the
 * const evaluator rejects cycles as EVAL_NOT_CONST). */
static const AstNode *const_ref_terminal(const NameModule *module,
                                         const AstNode *expr,
                                         const NameModule **out_mod)
{
    const NameModule *m = module;
    const AstNode *e = expr;
    size_t hops = 0;
    while (e != NULL &&
           (e->kind == AST_EXPR_IDENT || e->kind == AST_EXPR_MEMBER)) {
        const NameSymbol *ref = name_symbol_for_node(m, e);
        const AstNode *init;
        if (ref == NULL || ref->kind != NAME_SYM_GLOBAL_CONST ||
            ref->decl == NULL ||
            ref->decl->kind != AST_GLOBAL_CONST_DECL || ++hops > 64) {
            return NULL;
        }
        init = ref->decl->u.global_decl.init;
        if (init == NULL) {
            return NULL;
        }
        if (init->kind == AST_EXPR_STRUCT_INIT ||
            init->kind == AST_EXPR_ARRAY_LITERAL) {
            *out_mod = (ref->module != NULL) ? ref->module : m;
            return init;
        }
        m = (ref->module != NULL) ? ref->module : m;
        e = init;
    }
    return NULL;
}

IrConst *ir_builder_const_from_eval(BuilderCtx *ctx, const NameModule *module,
                                    IrType *expected, const AstNode *expr,
                                    const EvalValue *ev, bool *out_supported)
{
    IrBuild *b = ctx->build;
    if (out_supported != NULL) {
        *out_supported = true;
    }
    if (ev == NULL) {
        return NULL;
    }
    /* MAJOR-1 (reviewer2 t_e1758837): a module-scope const reference
     * (spec 10.5 const names are constant expressions) whose referenced
     * const is already mapped reuses that const's IRConst directly.
     * This is AC3 dedup (identical constants share one IRConst) and is
     * what makes composite (struct/array-of-struct) const references
     * representable: the per-kind branches below need the literal AST
     * at the position for field-name recovery (contract 4.5), which a
     * reference (IDENT/MEMBER) does not provide. For scalar forms the
     * reuse is consistent with interning the per-kind branch would do
     * (the referenced const's IRConst IS the interned representative).
     * Forward/cross-module references (const not yet filled) fall
     * through to the per-kind branches, which recover the names from
     * the referenced const's own initializer AST. */
    if (expr != NULL &&
        (expr->kind == AST_EXPR_IDENT || expr->kind == AST_EXPR_MEMBER)) {
        const NameSymbol *ref = name_symbol_for_node(module, expr);
        if (ref != NULL && ref->kind == NAME_SYM_GLOBAL_CONST) {
            IrNode *ref_node = decl_state_find(ref);
            if (ref_node != NULL && ref_node->kind == IR_GLOBAL_CONST &&
                ref_node->u.global_const.value != NULL) {
                return ref_node->u.global_const.value;
            }
        }
    }
    switch (ev->kind) {

    case EVAL_VAL_INT: {
        IrType *t = (ev->type != NULL) ? ir_builder_type_from_type(ctx, ev->type)
                                       : expected;
        uint64_t bits = (uint64_t)ev->u.i.v;
        if (t == NULL) {
            return NULL;
        }
        if (t->kind == IRT_ENUM) {
            int w = 0;
            if (t->u.decl != NULL && t->u.decl->kind == IR_ENUM_DECL) {
                w = ir_type_width(t->u.decl->u.enum_decl.underlying);
            }
            return ir_const_enum(b, t, mask_to_width(bits, w));
        }
        return ir_const_int(b, t, mask_to_width(bits, ir_type_width(t)));
    }

    case EVAL_VAL_BOOL:
        return ir_const_bool(b, ev->u.b);

    case EVAL_VAL_STR:
        return ir_const_str(b, (const uint8_t *)ev->u.str.bytes, ev->u.str.len);

    case EVAL_VAL_NULL: {
        /* The null literal carries no type in EvalValue; the declared
         * pointer type is the constant's type (contract 4.5 IRConst_NULL). */
        if (expected == NULL || expected->kind != IRT_PTR) {
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        return ir_const_null(b, expected);
    }

    case EVAL_VAL_ADDR: {
        IrNode *target;
        IrType *pt;
        if (ev->u.addr.sym == NULL) {
            /* Raw address (no static object): not representable. */
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        target = decl_state_find(ev->u.addr.sym);
        if (target == NULL || target->kind != IR_GLOBAL_VAR) {
            /* Address-of a const is unrepresentable (contract 4.2: consts
             * have no storage); anything non-static is malformed here. */
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        pt = (ev->type != NULL) ? ir_builder_type_from_type(ctx, ev->type)
                                : expected;
        if (pt == NULL || pt->kind != IRT_PTR) {
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        return ir_const_addr(b, pt, target, ev->u.addr.byte_offset);
    }

    case EVAL_VAL_SLICE:
        /* Disclosed gap: the closed IRConst set has no slice constant
         * kind (ir_builder_core.h / ir_builder_decl.h gap list). */
        if (out_supported != NULL) {
            *out_supported = false;
        }
        return NULL;

    case EVAL_VAL_ARRAY: {
        IrType *at = (ev->type != NULL) ? ir_builder_type_from_type(ctx, ev->type)
                                        : expected;
        const AstNode *alit = (expr != NULL && expr->kind == AST_EXPR_ARRAY_LITERAL)
                                  ? expr : NULL;
        const NameModule *amod = module;
        IrConst **items;
        size_t n, i;
        bool repeat;
        if (at == NULL || at->kind != IRT_ARRAY) {
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        if (alit == NULL) {
            /* MAJOR-1 (reviewer2 t_e1758837): a whole-array const
             * reference (e.g. `const b: Point[2] = arr;` where arr is a
             * Point[2] const) provides no array-literal AST here. The
             * referenced const's own initializer is that literal; its
             * element ASTs (and module) recover per-element names the
             * same way. Forward/cross-module/chained references follow
             * the const-reference chain to the terminal literal. */
            const NameModule *tmod = NULL;
            const AstNode *term = const_ref_terminal(module, expr, &tmod);
            if (term != NULL && term->kind == AST_EXPR_ARRAY_LITERAL) {
                alit = term;
                amod = (tmod != NULL) ? tmod : module;
            }
        }
        n = ev->u.array.nelems;
        if (n > 0) {
            items = (IrConst **)build_alloc(ctx, n, sizeof(*items));
            if (items == NULL) {
                return NULL;
            }
        } else {
            items = NULL;
        }
        repeat = (alit != NULL && alit->u.array_literal.count != NULL &&
                  alit->u.array_literal.nelems > 0);
        for (i = 0; i < n; i++) {
            const AstNode *ee = NULL;
            bool ok = true;
            if (alit != NULL) {
                if (repeat) {
                    ee = alit->u.array_literal.elems[0];
                } else if (i < alit->u.array_literal.nelems) {
                    ee = alit->u.array_literal.elems[i];
                }
            }
            items[i] = ir_builder_const_from_eval(
                ctx, amod, at->u.array.elem, ee, &ev->u.array.elems[i], &ok);
            if (items[i] == NULL) {
                if (!ok && out_supported != NULL) {
                    *out_supported = false;
                }
                free(items);
                return NULL;
            }
        }
        return ir_const_array(b, at, items, n);
    }

    case EVAL_VAL_STRUCT: {
        const AstNode *sinit = (expr != NULL && expr->kind == AST_EXPR_STRUCT_INIT)
                                   ? expr : NULL;
        const NameModule *smod = module;
        const NameSymbol *ssym;
        IrType *st;
        IrConst **items;
        size_t nd, nl, j, k;
        if (sinit == NULL) {
            /* MAJOR-1 (reviewer2 t_e1758837): a const reference at this
             * position evaluates to a struct value but provides no
             * struct-init AST to recover field names (contract 4.5).
             * The referenced const's own initializer is that struct
             * init (spec 10.5 const names are constant expressions);
             * the EvalValue fields are in that literal's order, so the
             * same name-based reordering works. Forward references
             * (const declared later), cross-module references, and
             * chains of references all resolve this way: follow the
             * const-reference chain to the terminal struct-init, which
             * lives in the referenced const's module. */
            const NameModule *tmod = NULL;
            const AstNode *term = const_ref_terminal(module, expr, &tmod);
            if (term != NULL && term->kind == AST_EXPR_STRUCT_INIT) {
                sinit = term;
                smod = (tmod != NULL) ? tmod : module;
            }
            if (sinit == NULL) {
                /* Field names are needed to emit IRConst_STRUCT in
                 * declaration order (contract 4.5; spec 12.7 allows any
                 * literal order). */
                if (out_supported != NULL) {
                    *out_supported = false;
                }
                return NULL;
            }
        }
        /* The struct declaration symbol: the evaluated value carries it
         * module-independently (also covers const-reference initializers
         * from other modules); fall back to the struct-init base name. */
        if (ev->type != NULL && ev->type->kind == TYPE_STRUCT &&
            ev->type->u.sym != NULL) {
            ssym = ev->type->u.sym;
        } else {
            ssym = name_symbol_for_node(smod, sinit->u.struct_init.base);
        }
        if (ssym == NULL || ssym->kind != NAME_SYM_STRUCT) {
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        {
            IrNode *sdecl = decl_state_find(ssym);
            if (sdecl == NULL) {
                if (out_supported != NULL) {
                    *out_supported = false;
                }
                return NULL;
            }
            st = (ev->type != NULL) ? ir_builder_type_from_type(ctx, ev->type)
                                    : ir_type_struct(b, sdecl);
        }
        if (st == NULL || st->kind != IRT_STRUCT) {
            if (out_supported != NULL) {
                *out_supported = false;
            }
            return NULL;
        }
        nd = ssym->nmembers;   /* declaration field count */
        nl = ev->u.st.nfields; /* literal field count */
        if (nd > 0) {
            items = (IrConst **)build_alloc(ctx, nd, sizeof(*items));
            if (items == NULL) {
                return NULL;
            }
        } else {
            items = NULL;
        }
        for (j = 0; j < nd; j++) {
            const NameSymbol *field = ssym->members[j];
            const AstNode *fval_expr = NULL;
            const AstNode *fval_ast = NULL;
            bool found = false;
            bool ok = true;
            for (k = 0; k < nl; k++) {
                const AstNode *fi = sinit->u.struct_init.fields[k];
                if (fi != NULL && fi->u.named.name != NULL &&
                    field != NULL && field->name != NULL &&
                    strcmp(fi->u.named.name, field->name) == 0) {
                    fval_expr = fi->u.named.value;
                    found = true;
                    break;
                }
            }
            if (!found || k >= nl) {
                /* A literal field is missing for a declaration field:
                 * malformed (accepted builds reject with AIC-T0313). */
                free(items);
                if (out_supported != NULL) {
                    *out_supported = false;
                }
                return NULL;
            }
            if (field->decl != NULL && field->decl->u.named.type != NULL) {
                fval_ast = field->decl->u.named.type;
            }
            items[j] = ir_builder_const_from_eval(
                ctx, smod,
                fval_ast != NULL ? decl_type_from_ast(ctx, smod, fval_ast)
                                 : NULL,
                fval_expr, &ev->u.st.fields[k], &ok);
            if (items[j] == NULL) {
                if (!ok && out_supported != NULL) {
                    *out_supported = false;
                }
                free(items);
                return NULL;
            }
        }
        return ir_const_struct(b, st, items, nd);
    }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Declaration node creation (identity + self facts)
 * ------------------------------------------------------------------------- */

/* Create one declaration node with identity facts (name, span, cause)
 * plus the self facts its own type descriptor depends on (struct
 * size/align, enum underlying). Everything else is filled by the decl
 * mapper. Returns IR_BUILDER_OK and sets *out, or returns an error
 * status with *out untouched. */
static IrBuilderStatus decl_new_decl(BuilderCtx *ctx, const NameModule *module,
                                     const NameSymbol *sym, IrNode **out)
{
    IrBuild *b = ctx->build;
    IrNode *n;
    const char *kind_str;
    DiagSpan *span;
    if (out != NULL) {
        *out = NULL;
    }
    if (sym == NULL || sym->fqn == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    if (sym->decl != NULL) {
        span = diag_span_clone(sym->decl->span);
        if (span == NULL) {
            b->oom = true;
            return IR_BUILDER_OOM;
        }
    } else {
        /* Runtime built-in: no source construct; deterministic synthetic
         * span (disclosed in ir_builder_decl.h). */
        span = synthetic_span(sym->fqn);
        if (span == NULL) {
            b->oom = true;
            return IR_BUILDER_OOM;
        }
    }

    switch (sym->kind) {
    case NAME_SYM_STRUCT: {
        const LayoutStruct *ls;
        n = ir_node_new(b, IR_STRUCT_DECL, span);
        if (n == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        n->u.struct_decl.name = build_dup(ctx, sym->fqn);
        if (n->u.struct_decl.name == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        ls = layout_build_struct(ctx->layout, sym);
        if (ls == NULL) {
            diag_span_free(span);
            return IR_BUILDER_UNSUPPORTED;
        }
        n->u.struct_decl.size = ls->size;
        n->u.struct_decl.align = ls->align;
        kind_str = "AST_STRUCT_DECL";
        break;
    }
    case NAME_SYM_ENUM: {
        n = ir_node_new(b, IR_ENUM_DECL, span);
        if (n == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        n->u.enum_decl.name = build_dup(ctx, sym->fqn);
        if (n->u.enum_decl.name == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        if (sym->decl == NULL || sym->decl->u.enum_decl.underlying == NULL) {
            diag_span_free(span);
            return IR_BUILDER_UNSUPPORTED;
        }
        n->u.enum_decl.underlying = decl_type_from_ast(
            ctx, module, sym->decl->u.enum_decl.underlying);
        if (n->u.enum_decl.underlying == NULL) {
            diag_span_free(span);
            return b->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
        }
        kind_str = "AST_ENUM_DECL";
        break;
    }
    case NAME_SYM_GLOBAL_CONST:
        n = ir_node_new(b, IR_GLOBAL_CONST, span);
        if (n == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        n->u.global_const.name = build_dup(ctx, sym->fqn);
        if (n->u.global_const.name == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        kind_str = "AST_GLOBAL_CONST_DECL";
        break;
    case NAME_SYM_GLOBAL_VAR:
        n = ir_node_new(b, IR_GLOBAL_VAR, span);
        if (n == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        n->u.global_var.name = build_dup(ctx, sym->fqn);
        if (n->u.global_var.name == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        kind_str = "AST_GLOBAL_VAR_DECL";
        break;
    case NAME_SYM_FN:
        n = ir_node_new(b, IR_FUNCTION, span);
        if (n == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        n->u.function.name = build_dup(ctx, sym->fqn);
        if (n->u.function.name == NULL) {
            diag_span_free(span);
            return IR_BUILDER_OOM;
        }
        kind_str = (sym->decl != NULL) ? "AST_FN_DECL" : "RUNTIME_FUNCTION";
        break;
    default:
        diag_span_free(span);
        return IR_BUILDER_UNSUPPORTED;
    }
    ir_node_add_cause(b, n, kind_str, n->span, -1, -1, -1);
    diag_span_free(span);
    if (out != NULL) {
        *out = n;
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Module / import creation and the creation pre-pass
 * ------------------------------------------------------------------------- */

/* The primary span of a module unit: the module declaration's span when
 * present, else the program span (runtime modules get a synthetic
 * compiler-provided span). */
static DiagSpan *module_span(const NameModule *module)
{
    if (module->program == NULL) {
        return synthetic_span(module->fqn);
    }
    if (module->program->u.program.module_decl != NULL) {
        return diag_span_clone(module->program->u.program.module_decl->span);
    }
    return diag_span_clone(module->program->span);
}

/* The span of the AST import declaration that names `import_fqn` in
 * `module` (source order; the resolved import edges are deduplicated, so
 * the first matching declaration is used), or NULL when no match exists
 * (defensive fallback handled by the caller). */
static DiagSpan *import_decl_span(const NameModule *module,
                                  const char *import_fqn)
{
    const AstNode *program = module->program;
    size_t i;
    if (program == NULL || program->u.program.imports == NULL) {
        return NULL;
    }
    for (i = 0; i < program->u.program.nimports; i++) {
        const AstNode *imp = program->u.program.imports[i];
        char *name;
        DiagSpan *out;
        if (imp == NULL || imp->u.qname.name == NULL) {
            continue;
        }
        name = ast_name_to_string(imp->u.qname.name);
        if (name == NULL) {
            continue;
        }
        if (strcmp(name, import_fqn) == 0) {
            free(name);
            out = diag_span_clone(imp->span);
            return out;
        }
        free(name);
    }
    return NULL;
}

/* Create the IR_MODULE node and its IR_IMPORT children for one module;
 * attaches the imports. Returns the module node or NULL. */
static IrNode *decl_new_module(BuilderCtx *ctx, const NameModule *module)
{
    IrBuild *b = ctx->build;
    IrNode *mnode;
    DiagSpan *span;
    size_t i;
    span = module_span(module);
    if (span == NULL) {
        b->oom = true;
        return NULL;
    }
    mnode = ir_node_new(b, IR_MODULE, span);
    diag_span_free(span);
    if (mnode == NULL) {
        return NULL;
    }
    mnode->u.module.name = build_dup(ctx, module->fqn);
    if (mnode->u.module.name == NULL) {
        return NULL;
    }
    add_cause(ctx, mnode, "AST_MODULE_DECL");
    for (i = 0; i < module->nimports; i++) {
        const NameModule *imp = module->imports[i];
        DiagSpan *ispan;
        IrNode *inode;
        if (imp == NULL || imp->fqn == NULL) {
            return NULL;   /* malformed import edge (build->oom untouched) */
        }
        ispan = import_decl_span(module, imp->fqn);
        if (ispan == NULL) {
            ispan = diag_span_clone(mnode->span);
            if (ispan == NULL) {
                b->oom = true;
                return NULL;
            }
        }
        inode = ir_node_new(b, IR_IMPORT, ispan);
        diag_span_free(ispan);
        if (inode == NULL) {
            return NULL;
        }
        inode->u.import.name = build_dup(ctx, imp->fqn);
        if (inode->u.import.name == NULL) {
            return NULL;
        }
        add_cause(ctx, inode, "AST_IMPORT_DECL");
        ir_module_add_import(b, mnode, inode);
        if (b->oom) {
            return NULL;
        }
    }
    return mnode;
}

/* Creation pre-pass: create every module unit (in canonical order) and
 * every top-level declaration node (in source order per module), attach
 * declarations to their modules, and record the symbol -> node mapping
 * for the decl mapper's fill pass. */
static IrBuilderStatus decl_prepass(BuilderCtx *ctx)
{
    const NameResult *r = ctx->result;
    size_t mi, di;
    if (r->nmodules > 0 && r->modules == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* malformed module array */
    }
    for (mi = 0; mi < r->nmodules; mi++) {
        const NameModule *module = r->modules[mi];
        IrNode *mnode;
        if (module == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        if (module->nmodule_scope > 0 && module->module_scope == NULL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        mnode = decl_new_module(ctx, module);
        if (mnode == NULL) {
            return ctx->build->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
        }
        ir_build_add_module(ctx->build, mnode);
        for (di = 0; di < module->nmodule_scope; di++) {
            const NameSymbol *sym = module->module_scope[di];
            IrNode *dnode;
            IrBuilderStatus st;
            if (sym == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            st = decl_new_decl(ctx, module, sym, &dnode);
            if (st != IR_BUILDER_OK) {
                return st;
            }
            ir_module_add_decl(ctx->build, mnode, dnode);
            decl_state_add(ctx, sym, dnode);
            if (ctx->oom || ctx->build->oom) {
                return IR_BUILDER_OOM;
            }
        }
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Module mapper (the 16c1b seam entry)
 * ------------------------------------------------------------------------- */

IrBuilderStatus ir_builder_decl_module(BuilderCtx *ctx,
                                       const NameModule *module)
{
    (void)module;
    /* The creation pre-pass runs once per build: on the first module
     * visit, or when the state no longer matches a fresh build (a new
     * IrBuild may reuse a freed build's address, so a fresh build is
     * recognized by its empty module list). */
    if (s_decl.build_key != ctx->build || ctx->build->nmodules == 0) {
        decl_state_reset();
        s_decl.build_key = ctx->build;
        return decl_prepass(ctx);
    }
    return IR_BUILDER_OK;   /* nodes already created by the pre-pass */
}

/* ---------------------------------------------------------------------------
 * Declaration detail fills (run by the decl mapper in canonical order)
 * ------------------------------------------------------------------------- */

static IrBuilderStatus decl_fill_struct(BuilderCtx *ctx,
                                        const NameModule *module,
                                        const NameSymbol *sym, IrNode *node)
{
    IrBuild *b = ctx->build;
    const AstNode *decl = sym->decl;
    const LayoutStruct *ls;
    IrField *fields;
    size_t nf, i;
    if (decl == NULL || decl->kind != AST_STRUCT_DECL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    ls = layout_build_struct(ctx->layout, sym);
    if (ls == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* unevaluable/absent: not accepted */
    }
    nf = decl->u.struct_decl.nfields;
    if (ls->nfields != nf) {
        return IR_BUILDER_UNSUPPORTED;   /* layout/AST field mismatch */
    }
    if (nf > 0) {
        fields = (IrField *)build_alloc(ctx, nf, sizeof(*fields));
        if (fields == NULL) {
            return IR_BUILDER_OOM;
        }
        /* Attach before filling so ir_build_free releases partial work. */
        node->u.struct_decl.fields = fields;
        node->u.struct_decl.nfields = nf;
        for (i = 0; i < nf; i++) {
            const AstNode *fa = decl->u.struct_decl.fields[i];
            IrType *ft;
            if (fa == NULL || fa->kind != AST_FIELD_DECL ||
                fa->u.named.name == NULL || fa->u.named.type == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            ft = decl_type_from_ast(ctx, module, fa->u.named.type);
            if (ft == NULL || ft->kind == IRT_VOID) {
                return b->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
            }
            fields[i].name = build_dup(ctx, fa->u.named.name);
            fields[i].type = ft;
            fields[i].span = diag_span_clone(fa->span);
            fields[i].byte_offset = ls->fields[i].offset;
            if (fields[i].name == NULL || fields[i].span == NULL) {
                return IR_BUILDER_OOM;
            }
        }
    }
    return IR_BUILDER_OK;
}

static IrBuilderStatus decl_fill_enum(BuilderCtx *ctx,
                                      const NameModule *module,
                                      const NameSymbol *sym, IrNode *node)
{
    const AstNode *decl = sym->decl;
    const LayoutEnum *le;
    IrEnumMember *members;
    size_t nm, i;
    (void)module;
    if (decl == NULL || decl->kind != AST_ENUM_DECL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    le = layout_build_enum(ctx->layout, sym);
    if (le == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }
    nm = decl->u.enum_decl.nmembers;
    if (le->nmembers != nm) {
        return IR_BUILDER_UNSUPPORTED;   /* layout/AST member mismatch */
    }
    if (nm > 0) {
        members = (IrEnumMember *)build_alloc(ctx, nm, sizeof(*members));
        if (members == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.enum_decl.members = members;
        node->u.enum_decl.nmembers = nm;
        for (i = 0; i < nm; i++) {
            const AstNode *ma = decl->u.enum_decl.members[i];
            const LayoutEnumMember *lm = &le->members[i];
            if (ma == NULL || ma->kind != AST_ENUM_MEMBER ||
                ma->u.named.name == NULL) {
                return IR_BUILDER_UNSUPPORTED;
            }
            if (lm->domain_overflow) {
                /* Value exceeds u64: rejected pre-IR (AIC-T0301), so this
                 * is unreachable on accepted builds. */
                return IR_BUILDER_UNSUPPORTED;
            }
            members[i].name = build_dup(ctx, ma->u.named.name);
            members[i].value = (int64_t)(uint64_t)lm->value;
            members[i].span = diag_span_clone(ma->span);
            if (members[i].name == NULL || members[i].span == NULL) {
                return IR_BUILDER_OOM;
            }
        }
    }
    return IR_BUILDER_OK;
}

/* Evaluate a global const/var initializer and map it to an interned
 * IRConst; attaches it to the node. */
static IrBuilderStatus decl_fill_global(BuilderCtx *ctx,
                                        const NameModule *module,
                                        const NameSymbol *sym, IrNode *node,
                                        bool is_const)
{
    IrBuild *b = ctx->build;
    const AstNode *decl = sym->decl;
    const AstNode *type_ast, *init_ast;
    IrType *t;
    EvalCtx ec;
    EvalValue ev;
    EvalFailure fail = EVAL_FAIL_NONE;
    EvalStatus st;
    IrConst *c;
    bool supported = true;
    if (decl == NULL || (decl->kind != AST_GLOBAL_CONST_DECL &&
                         decl->kind != AST_GLOBAL_VAR_DECL)) {
        return IR_BUILDER_UNSUPPORTED;
    }
    type_ast = decl->u.global_decl.type;
    init_ast = decl->u.global_decl.init;
    if (type_ast == NULL || init_ast == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* accepted builds always carry both */
    }
    t = decl_type_from_ast(ctx, module, type_ast);
    if (t == NULL) {
        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
    }
    if (is_const) {
        node->u.global_const.type = t;
    } else {
        node->u.global_var.type = t;
    }
    eval_ctx_init(&ec, ctx->result, ctx->layout, module);
    st = const_eval_expr(&ec, init_ast, &ev, &fail);
    eval_ctx_cleanup(&ec);
    if (st == EVAL_OOM || ec.oom) {
        return IR_BUILDER_OOM;
    }
    if (st != EVAL_OK) {
        return IR_BUILDER_UNSUPPORTED;   /* defensive; accepted builds evaluate */
    }
    c = ir_builder_const_from_eval(ctx, module, t, init_ast, &ev, &supported);
    eval_value_free(&ev);
    if (c == NULL) {
        return supported ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
    }
    if (is_const) {
        node->u.global_const.value = c;
    } else {
        node->u.global_var.init = c;
    }
    return IR_BUILDER_OK;
}

/* Function detail fill: return type, parameter list with slots (storage
 * model 4.3: parameter slots first, in parameter order), and the body
 * placeholder block. Runtime built-ins (no decl) get void return, no
 * params/slots, and no body when noreturn (rt.proc.exit /
 * rt.trap.report; ir_core_verify invariant 3 permits body == NULL for
 * noreturn functions), otherwise an empty body placeholder. */
static IrBuilderStatus decl_fill_fn(BuilderCtx *ctx, const NameModule *module,
                                    const NameSymbol *sym, IrNode *node)
{
    IrBuild *b = ctx->build;
    const AstNode *decl = sym->decl;
    IrType *ret;
    size_t i;
    if (decl != NULL) {
        if (decl->kind != AST_FN_DECL) {
            return IR_BUILDER_UNSUPPORTED;
        }
        ret = decl_type_from_ast(ctx, module, decl->u.fn_decl.ret_type);
        if (ret == NULL) {
            return b->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
        }
        node->u.function.ret_type = ret;
        node->u.function.noreturn = false;
        {
            size_t np = decl->u.fn_decl.nparams;
            if (np > 0) {
                IrParam *params = (IrParam *)build_alloc(ctx, np,
                                                         sizeof(*params));
                if (params == NULL) {
                    return IR_BUILDER_OOM;
                }
                node->u.function.params = params;
                node->u.function.nparams = np;
                for (i = 0; i < np; i++) {
                    const AstNode *pa = decl->u.fn_decl.params[i];
                    IrType *pt;
                    if (pa == NULL || pa->kind != AST_PARAM ||
                        pa->u.named.name == NULL || pa->u.named.type == NULL) {
                        return IR_BUILDER_UNSUPPORTED;
                    }
                    pt = decl_type_from_ast(ctx, module, pa->u.named.type);
                    if (pt == NULL || pt->kind == IRT_VOID) {
                        return b->oom ? IR_BUILDER_OOM : IR_BUILDER_UNSUPPORTED;
                    }
                    params[i].name = build_dup(ctx, pa->u.named.name);
                    params[i].type = pt;
                    params[i].slot_index = (int64_t)i;
                    params[i].span = diag_span_clone(pa->span);
                    if (params[i].name == NULL || params[i].span == NULL) {
                        return IR_BUILDER_OOM;
                    }
                }
                /* Parameter slots first, in parameter order. */
                for (i = 0; i < np; i++) {
                    IrSlot *s = ir_builder_add_slot(b, node, IR_SLOT_PARAM,
                                                    params[i].name,
                                                    params[i].type,
                                                    params[i].span);
                    if (s == NULL) {
                        return IR_BUILDER_OOM;
                    }
                }
            }
        }
        {
            /* Body placeholder: the empty IR_BLOCK Phase B fills. */
            const AstNode *body_ast = decl->u.fn_decl.body;
            IrNode *body = ir_node_new(b, IR_BLOCK,
                                       body_ast != NULL ? body_ast->span
                                                        : node->span);
            if (body == NULL) {
                return IR_BUILDER_OOM;
            }
            add_cause(ctx, body, "AST_BLOCK");
            node->u.function.body = body;
        }
    } else {
        /* Runtime built-in (rt.* surface): no source, no signature in the
         * resolved build; identity + noreturn flag only (disclosed in
         * ir_builder_decl.h; call sites carry the spec signatures). */
        ret = ir_type_void(b);
        if (ret == NULL) {
            return IR_BUILDER_OOM;
        }
        node->u.function.ret_type = ret;
        node->u.function.noreturn =
            (strcmp(node->u.function.name, "rt.proc.exit") == 0 ||
             strcmp(node->u.function.name, "rt.trap.report") == 0);
        if (!node->u.function.noreturn) {
            IrNode *body = ir_node_new(b, IR_BLOCK, node->span);
            if (body == NULL) {
                return IR_BUILDER_OOM;
            }
            add_cause(ctx, body, "RUNTIME_FUNCTION");
            node->u.function.body = body;
        }
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Declaration mapper (the 16c1b seam entry)
 * ------------------------------------------------------------------------- */

IrBuilderStatus ir_builder_decl_decl(BuilderCtx *ctx,
                                     const NameModule *module,
                                     const NameSymbol *sym)
{
    IrNode *node;
    if (s_decl.build_key != ctx->build) {
        /* A decl visit without a prior module visit is malformed
         * (unreachable through the 16c1a driver skeleton). */
        return IR_BUILDER_UNSUPPORTED;
    }
    node = decl_state_find(sym);
    if (node == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* defensive */
    }
    switch (node->kind) {
    case IR_STRUCT_DECL:
        return decl_fill_struct(ctx, module, sym, node);
    case IR_ENUM_DECL:
        return decl_fill_enum(ctx, module, sym, node);
    case IR_GLOBAL_CONST:
        return decl_fill_global(ctx, module, sym, node, true);
    case IR_GLOBAL_VAR:
        return decl_fill_global(ctx, module, sym, node, false);
    case IR_FUNCTION:
        return decl_fill_fn(ctx, module, sym, node);
    default:
        return IR_BUILDER_UNSUPPORTED;
    }
}

/* ---------------------------------------------------------------------------
 * Storage model (contract 4.3): slot-table append
 * ------------------------------------------------------------------------- */

IrSlot *ir_builder_add_slot(IrBuild *b, IrNode *fn_node, IrSlotKind kind,
                            const char *name, IrType *type,
                            const DiagSpan *span)
{
    IrSlot **slots;
    IrSlot *s;
    size_t index;
    char *sname = NULL;
    DiagSpan *sspan = NULL;
    if (b == NULL || fn_node == NULL || fn_node->kind != IR_FUNCTION ||
        type == NULL || type->kind == IRT_VOID || b->oom) {
        return NULL;
    }
    index = fn_node->u.function.nslots;
    slots = (IrSlot **)realloc(fn_node->u.function.slots,
                               (index + 1) * sizeof(*slots));
    if (slots == NULL) {
        b->oom = true;
        return NULL;
    }
    fn_node->u.function.slots = slots;
    if (name != NULL) {
        sname = (char *)malloc(strlen(name) + 1);
        if (sname == NULL) {
            b->oom = true;
            return NULL;   /* slots array grown but entry not stored: safe
                            * (nslots unchanged); ir_build_free frees the
                            * grown array */
        }
        memcpy(sname, name, strlen(name) + 1);
    }
    if (span != NULL) {
        sspan = diag_span_clone(span);
        if (sspan == NULL) {
            free(sname);
            b->oom = true;
            return NULL;
        }
    }
    s = (IrSlot *)calloc(1, sizeof(*s));
    if (s == NULL) {
        free(sname);
        diag_span_free(sspan);
        b->oom = true;
        return NULL;
    }
    s->index = (int64_t)index;
    s->kind = kind;
    s->name = sname;
    s->type = type;
    s->span = sspan;
    fn_node->u.function.slots[index] = s;
    fn_node->u.function.nslots = index + 1;
    return s;
}

/* ---------------------------------------------------------------------------
 * Phase B body stub (16c1b representable surface: bodies are placeholders)
 * ------------------------------------------------------------------------- */

static IrBuilderStatus decl_body_placeholder(BuilderCtx *ctx,
                                             const NameModule *module,
                                             const NameSymbol *fn_sym)
{
    (void)ctx;
    (void)module;
    (void)fn_sym;
    /* Function bodies are the Phase A placeholder blocks until
     * WP-M0-16c1c/16c1d install the expression/statement mappers. */
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------- */

void ir_builder_decl_install(void)
{
    decl_state_reset();
    ir_builder_set_module_mapper(ir_builder_decl_module);
    ir_builder_set_decl_mapper(ir_builder_decl_decl);
    ir_builder_set_body_mapper(decl_body_placeholder);
}

