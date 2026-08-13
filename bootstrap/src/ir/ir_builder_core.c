/* bootstrap/src/ir/ir_builder_core.c
 *
 * AI-Co Stage-0 IR builder foundation (WP-M0-16c1a).
 *
 * Implements the builder entry surface, status/ownership contract,
 * builder context/allocation discipline, and the canonical-order
 * two-phase construction driver skeleton over the accepted IR contract
 * (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.1) and the IR node
 * model of WP-M0-16b1 (ir_core.h).
 *
 * Phase A (builder_phase_a): walks the resolved build's module units in
 * canonical order (entry first, then imports depth-first in import
 * order - the NameResult.modules array order, per name.h), and within
 * each module its top-level declarations in source order
 * (NameModule.module_scope order). Each module/declaration is delegated
 * to the registered module/declaration mappers (WP-M0-16c1b).
 *
 * Phase B (builder_phase_b): walks function bodies in canonical order
 * (same module order, then function symbols in source order), delegated
 * to the registered body mapper (WP-M0-16c1c/16c1d).
 *
 * At 16c1a the default mappers are defensive stubs returning
 * IR_BUILDER_UNSUPPORTED: the representable surface of this package is
 * an empty build. Successor packages install their implementations
 * through ir_builder_set_*_mapper; tests install recording mappers to
 * verify the driver skeleton's canonical order.
 *
 * Ownership: on IR_BUILDER_OK *out_build is owned by the caller. On
 * IR_BUILDER_UNSUPPORTED / IR_BUILDER_OOM the whole (possibly partial)
 * IrBuild is released and nothing is owned. The NameResult/LayoutBuild
 * are borrowed and never modified.
 *
 * Determinism: the driver iterates the resolved build's own arrays
 * (modules in canonical order, module_scope in source order) - no
 * sorting, no hash iteration, no pointer-address ordering - so identical
 * inputs produce identical walk order and therefore identical node ids.
 */
#include "ir_builder_core.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Default mapping hooks (defensive stubs)
 * ------------------------------------------------------------------------- */

static IrBuilderStatus builder_map_module_default(BuilderCtx *ctx,
                                                  const NameModule *module)
{
    (void)ctx;
    (void)module;
    /* Module/import/declaration mapping is WP-M0-16c1b; at 16c1a no
     * module content is representable. */
    return IR_BUILDER_UNSUPPORTED;
}

static IrBuilderStatus builder_map_decl_default(BuilderCtx *ctx,
                                                const NameModule *module,
                                                const NameSymbol *sym)
{
    (void)ctx;
    (void)module;
    (void)sym;
    /* Module/import/declaration mapping is WP-M0-16c1b; at 16c1a no
     * declaration is representable. */
    return IR_BUILDER_UNSUPPORTED;
}

static IrBuilderStatus builder_map_body_default(BuilderCtx *ctx,
                                                const NameModule *module,
                                                const NameSymbol *fn_sym)
{
    (void)ctx;
    (void)module;
    (void)fn_sym;
    /* Expression/statement mapping is WP-M0-16c1c/16c1d; at 16c1a no
     * function body is representable. */
    return IR_BUILDER_UNSUPPORTED;
}

/* Process-global hook table (single-build compiler). ir_builder_build
 * snapshots these into the context at entry. */
static IrBuilderStatus (*s_module_mapper)(BuilderCtx *ctx,
                                          const NameModule *module) =
    builder_map_module_default;
static IrBuilderStatus (*s_decl_mapper)(BuilderCtx *ctx,
                                        const NameModule *module,
                                        const NameSymbol *sym) =
    builder_map_decl_default;
static IrBuilderStatus (*s_body_mapper)(BuilderCtx *ctx,
                                        const NameModule *module,
                                        const NameSymbol *fn_sym) =
    builder_map_body_default;

void ir_builder_set_module_mapper(
    IrBuilderStatus (*fn)(BuilderCtx *ctx, const NameModule *module))
{
    s_module_mapper = fn ? fn : builder_map_module_default;
}

void ir_builder_set_decl_mapper(
    IrBuilderStatus (*fn)(BuilderCtx *ctx, const NameModule *module,
                          const NameSymbol *sym))
{
    s_decl_mapper = fn ? fn : builder_map_decl_default;
}

void ir_builder_set_body_mapper(
    IrBuilderStatus (*fn)(BuilderCtx *ctx, const NameModule *module,
                          const NameSymbol *fn_sym))
{
    s_body_mapper = fn ? fn : builder_map_body_default;
}

/* ---------------------------------------------------------------------------
 * Context allocation
 * ------------------------------------------------------------------------- */

void *ir_builder_ctx_alloc(BuilderCtx *ctx, size_t size)
{
    void *p;
    if (ctx->oom || size == 0) {
        return NULL;
    }
    p = calloc(1, size);
    if (p == NULL) {
        ctx->oom = true;
    }
    return p;
}

/* ---------------------------------------------------------------------------
 * Two-phase driver skeleton
 * ------------------------------------------------------------------------- */

/* Phase A: modules in canonical order; declarations in source order. */
static IrBuilderStatus builder_phase_a(BuilderCtx *ctx)
{
    const NameResult *r = ctx->result;
    size_t mi, di;

    /* Defensive (MIN-1): a NULL module array with a nonzero count is
     * malformed input; refuse before any dereference. Unreachable from
     * the accepted pipeline (IR contract sec. 1.3). */
    if (r->nmodules > 0 && r->modules == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* malformed module array */
    }
    for (mi = 0; mi < r->nmodules; mi++) {
        const NameModule *module = r->modules[mi];
        IrBuilderStatus st;

        if (module == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* malformed module list */
        }
        if (module->nmodule_scope > 0 && module->module_scope == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* malformed scope array */
        }
        ctx->module_index = mi;
        st = ctx->map_module(ctx, module);
        if (st != IR_BUILDER_OK) {
            return st;
        }
        for (di = 0; di < module->nmodule_scope; di++) {
            const NameSymbol *sym = module->module_scope[di];
            if (sym == NULL) {
                return IR_BUILDER_UNSUPPORTED;   /* malformed scope entry */
            }
            ctx->decl_index = di;
            st = ctx->map_decl(ctx, module, sym);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        }
    }
    return IR_BUILDER_OK;
}

/* Phase B: function bodies in canonical order (function symbols only). */
static IrBuilderStatus builder_phase_b(BuilderCtx *ctx)
{
    const NameResult *r = ctx->result;
    size_t mi, di;

    /* Defensive (MIN-1): same NULL-array guards as Phase A. */
    if (r->nmodules > 0 && r->modules == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* malformed module array */
    }
    for (mi = 0; mi < r->nmodules; mi++) {
        const NameModule *module = r->modules[mi];
        if (module == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* malformed module list */
        }
        if (module->nmodule_scope > 0 && module->module_scope == NULL) {
            return IR_BUILDER_UNSUPPORTED;   /* malformed scope array */
        }
        ctx->module_index = mi;
        for (di = 0; di < module->nmodule_scope; di++) {
            const NameSymbol *sym = module->module_scope[di];
            IrBuilderStatus st;
            if (sym == NULL) {
                return IR_BUILDER_UNSUPPORTED;   /* malformed scope entry */
            }
            if (sym->kind != NAME_SYM_FN) {
                continue;   /* Phase B lowers function bodies only */
            }
            ctx->decl_index = di;
            st = ctx->map_body(ctx, module, sym);
            if (st != IR_BUILDER_OK) {
                return st;
            }
        }
    }
    return IR_BUILDER_OK;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

IrBuilderStatus ir_builder_build(const NameResult *result,
                                 const LayoutBuild *layout,
                                 IrBuild **out_build)
{
    BuilderCtx ctx;
    IrBuilderStatus st;

    if (out_build == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* cannot report ownership */
    }
    *out_build = NULL;                   /* nothing owned until OK */
    if (result == NULL || layout == NULL) {
        return IR_BUILDER_UNSUPPORTED;   /* malformed entry input */
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.result = result;
    ctx.layout = layout;
    ctx.build = ir_build_new();
    if (ctx.build == NULL) {
        return IR_BUILDER_OOM;           /* nothing owned */
    }
    ctx.map_module = s_module_mapper;
    ctx.map_decl = s_decl_mapper;
    ctx.map_body = s_body_mapper;

    ctx.phase = 1;
    st = builder_phase_a(&ctx);
    if (st == IR_BUILDER_OK) {
        ctx.phase = 2;
        st = builder_phase_b(&ctx);
    }
    if (st == IR_BUILDER_OK && (ctx.oom || ctx.build->oom)) {
        st = IR_BUILDER_OOM;
    }
    if (st != IR_BUILDER_OK) {
        ir_build_free(ctx.build);        /* release partial build */
        return st;                       /* nothing owned */
    }

    *out_build = ctx.build;              /* owned by the caller */
    return IR_BUILDER_OK;
}
