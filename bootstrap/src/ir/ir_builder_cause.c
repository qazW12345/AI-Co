/* bootstrap/src/ir/ir_builder_cause.c
 *
 * AI-Co Stage-0 IR span/cause preservation (WP-M0-16c2).
 *
 * Implements the full span/cause preservation pass of the accepted
 * canonical IR contract (docs/contracts/IR-CONTRACT-2026-08-12.md,
 * v0.1.1) section 8 over the completed IR builder output.
 *
 * The 16c1a..16c1d builders attach, for every node, a single minimal root
 * cause link (construct kind + the node's primary span, refs -1). This
 * package replaces that single link with the full preservation chain:
 *
 *   - the node's source AST construct is located by matching the node's
 *     primary span (and, when the builder's construct-kind text is an
 *     exact AST kind, that kind) against the owning module's AST;
 *   - the ordered parent-linked chain from the source construct to the
 *     module root (AST_PROGRAM) is built from a deterministic per-module
 *     AST parent index;
 *   - each link carries the AST construct kind text (e.g. "AST_EXPR_BINARY"),
 *     a byte-identical clone of that construct's AST span, and
 *     resolved-reference facts (declaration/type/constant IR ids) filled
 *     where determinable from the resolved build and the built graph;
 *   - nodes whose span has no AST source (runtime synthetic spans) keep
 *     their existing single-link chain (documented in the header).
 *
 * Determinism (contract sec. 8.5): the pass walks modules in NameResult
 * order, each module's AST in source order (children arrays in order, the
 * same walk ast_dump uses), and build nodes in id order. Lookups are
 * linear scans in that fixed order; there is no pointer-address ordering
 * and no hash iteration, so identical ASTs produce byte-identical chains
 * and therefore byte-identical IR.
 *
 * Ownership: on IR_BUILDER_OOM build->oom is set and nothing is owned by
 * the caller beyond the build itself (the caller frees it). A node's new
 * chain is fully built before the old chain is freed, so the build is
 * always freeable. The NameResult is borrowed and never modified.
 */
#include "ir_builder_cause.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * AST kind text: the stable "AST_<ENUM>" strings the builders use in cause
 * links (e.g. "AST_EXPR_BINARY", "AST_BLOCK", "AST_VAR_DECL"). Every kind is
 * covered; the table order matches the AstNodeKind enum order.
 * ------------------------------------------------------------------------- */

static const char *ast_kind_text(AstNodeKind kind)
{
    switch (kind) {
    case AST_PROGRAM:              return "AST_PROGRAM";
    case AST_MODULE_DECL:          return "AST_MODULE_DECL";
    case AST_IMPORT_DECL:          return "AST_IMPORT_DECL";
    case AST_STRUCT_DECL:          return "AST_STRUCT_DECL";
    case AST_ENUM_DECL:            return "AST_ENUM_DECL";
    case AST_FN_DECL:              return "AST_FN_DECL";
    case AST_GLOBAL_VAR_DECL:      return "AST_GLOBAL_VAR_DECL";
    case AST_GLOBAL_CONST_DECL:    return "AST_GLOBAL_CONST_DECL";
    case AST_FIELD_DECL:           return "AST_FIELD_DECL";
    case AST_ENUM_MEMBER:          return "AST_ENUM_MEMBER";
    case AST_PARAM:                return "AST_PARAM";
    case AST_BLOCK:                return "AST_BLOCK";
    case AST_VAR_DECL:             return "AST_VAR_DECL";
    case AST_CONST_DECL:           return "AST_CONST_DECL";
    case AST_IF:                   return "AST_IF";
    case AST_WHILE:                return "AST_WHILE";
    case AST_FOR:                  return "AST_FOR";
    case AST_SWITCH:               return "AST_SWITCH";
    case AST_CASE_CLAUSE:          return "AST_CASE_CLAUSE";
    case AST_DEFAULT_CLAUSE:       return "AST_DEFAULT_CLAUSE";
    case AST_BREAK:                return "AST_BREAK";
    case AST_CONTINUE:             return "AST_CONTINUE";
    case AST_RETURN:               return "AST_RETURN";
    case AST_EXPR_STMT:            return "AST_EXPR_STMT";
    case AST_EMPTY_STMT:           return "AST_EMPTY_STMT";
    case AST_TYPE_PRIM:            return "AST_TYPE_PRIM";
    case AST_TYPE_NAMED:           return "AST_TYPE_NAMED";
    case AST_TYPE_PTR:             return "AST_TYPE_PTR";
    case AST_TYPE_ARRAY:           return "AST_TYPE_ARRAY";
    case AST_TYPE_SLICE:           return "AST_TYPE_SLICE";
    case AST_EXPR_INT_LITERAL:     return "AST_EXPR_INT_LITERAL";
    case AST_EXPR_STR_LITERAL:     return "AST_EXPR_STR_LITERAL";
    case AST_EXPR_BOOL_LITERAL:    return "AST_EXPR_BOOL_LITERAL";
    case AST_EXPR_NULL_LITERAL:    return "AST_EXPR_NULL_LITERAL";
    case AST_EXPR_IDENT:           return "AST_EXPR_IDENT";
    case AST_EXPR_ARRAY_LITERAL:   return "AST_EXPR_ARRAY_LITERAL";
    case AST_EXPR_PAREN:           return "AST_EXPR_PAREN";
    case AST_EXPR_UNARY:           return "AST_EXPR_UNARY";
    case AST_EXPR_BINARY:          return "AST_EXPR_BINARY";
    case AST_EXPR_ASSIGN:          return "AST_EXPR_ASSIGN";
    case AST_EXPR_TERNARY:         return "AST_EXPR_TERNARY";
    case AST_EXPR_INDEX:           return "AST_EXPR_INDEX";
    case AST_EXPR_SLICE:           return "AST_EXPR_SLICE";
    case AST_EXPR_CALL:            return "AST_EXPR_CALL";
    case AST_EXPR_MEMBER:          return "AST_EXPR_MEMBER";
    case AST_EXPR_ARROW:           return "AST_EXPR_ARROW";
    case AST_EXPR_STRUCT_INIT:     return "AST_EXPR_STRUCT_INIT";
    case AST_FIELD_INIT:           return "AST_FIELD_INIT";
    case AST_EXPR_SIZEOF_TYPE:     return "AST_EXPR_SIZEOF_TYPE";
    case AST_EXPR_SIZEOF_EXPR:     return "AST_EXPR_SIZEOF_EXPR";
    case AST_EXPR_ALIGNOF:         return "AST_EXPR_ALIGNOF";
    case AST_EXPR_CAST:            return "AST_EXPR_CAST";
    case AST_EXPR_WRAP:            return "AST_EXPR_WRAP";
    case AST_EXPR_LEN:             return "AST_EXPR_LEN";
    case AST_EXPR_PTR:             return "AST_EXPR_PTR";
    }
    return "AST_?";
}

/* ---------------------------------------------------------------------------
 * Span helpers
 * ------------------------------------------------------------------------- */

static bool span_equal(const DiagSpan *a, const DiagSpan *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->file == NULL || b->file == NULL) {
        return a->file == b->file;
    }
    return strcmp(a->file, b->file) == 0 &&
           a->start.line == b->start.line &&
           a->start.col == b->start.col &&
           a->start.offset == b->start.offset &&
           a->end.line == b->end.line &&
           a->end.col == b->end.col &&
           a->end.offset == b->end.offset;
}

static char *dup_str_cause(const char *s)
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

/* ---------------------------------------------------------------------------
 * Per-module AST parent index (deterministic walk order)
 * ------------------------------------------------------------------------- */

typedef struct AstEntry {
    const AstNode *node;
    const AstNode *parent;
    size_t depth;
} AstEntry;

typedef struct AstIndex {
    AstEntry *entries;
    size_t count;
    size_t cap;
    bool oom;
    const NameModule *module;
} AstIndex;

static bool index_append(AstIndex *idx, const AstNode *node,
                         const AstNode *parent, size_t depth)
{
    AstEntry *p;
    if (idx->count == idx->cap) {
        size_t ncap = idx->cap ? idx->cap * 2 : 64;
        p = (AstEntry *)realloc(idx->entries, ncap * sizeof(*p));
        if (p == NULL) {
            idx->oom = true;
            return false;
        }
        idx->entries = p;
        idx->cap = ncap;
    }
    idx->entries[idx->count].node = node;
    idx->entries[idx->count].parent = parent;
    idx->entries[idx->count].depth = depth;
    idx->count++;
    return true;
}

/* Child walk mirroring ast_dump's deterministic child order (children
 * arrays in source order). Appends (node,parent) for every node and
 * recurses. */
static void index_walk(AstIndex *idx, const AstNode *n,
                       const AstNode *parent, size_t depth)
{
    size_t i;

    if (n == NULL) {
        return;
    }
    if (!index_append(idx, n, parent, depth)) {
        return;
    }
    switch (n->kind) {
    case AST_PROGRAM:
        index_walk(idx, n->u.program.module_decl, n, depth + 1);
        for (i = 0; i < n->u.program.nimports; i++) {
            index_walk(idx, n->u.program.imports[i], n, depth + 1);
        }
        for (i = 0; i < n->u.program.ndecls; i++) {
            index_walk(idx, n->u.program.decls[i], n, depth + 1);
        }
        break;
    case AST_STRUCT_DECL:
        for (i = 0; i < n->u.struct_decl.nfields; i++) {
            index_walk(idx, n->u.struct_decl.fields[i], n, depth + 1);
        }
        break;
    case AST_ENUM_DECL:
        index_walk(idx, n->u.enum_decl.underlying, n, depth + 1);
        for (i = 0; i < n->u.enum_decl.nmembers; i++) {
            index_walk(idx, n->u.enum_decl.members[i], n, depth + 1);
        }
        break;
    case AST_FN_DECL:
        for (i = 0; i < n->u.fn_decl.nparams; i++) {
            index_walk(idx, n->u.fn_decl.params[i], n, depth + 1);
        }
        index_walk(idx, n->u.fn_decl.ret_type, n, depth + 1);
        index_walk(idx, n->u.fn_decl.body, n, depth + 1);
        break;
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:
        index_walk(idx, n->u.global_decl.type, n, depth + 1);
        index_walk(idx, n->u.global_decl.init, n, depth + 1);
        break;
    case AST_FIELD_DECL:
    case AST_PARAM:
        index_walk(idx, n->u.named.type, n, depth + 1);
        break;
    case AST_ENUM_MEMBER:
        index_walk(idx, n->u.named.value, n, depth + 1);
        break;
    case AST_FIELD_INIT:
        index_walk(idx, n->u.named.value, n, depth + 1);
        break;
    case AST_BLOCK:
        for (i = 0; i < n->u.list.count; i++) {
            index_walk(idx, n->u.list.items[i], n, depth + 1);
        }
        break;
    case AST_VAR_DECL:
    case AST_CONST_DECL:
        index_walk(idx, n->u.local_decl.type, n, depth + 1);
        index_walk(idx, n->u.local_decl.init, n, depth + 1);
        break;
    case AST_IF:
        index_walk(idx, n->u.branch.cond, n, depth + 1);
        index_walk(idx, n->u.branch.then, n, depth + 1);
        index_walk(idx, n->u.branch.els, n, depth + 1);
        break;
    case AST_WHILE:
        index_walk(idx, n->u.while_loop.cond, n, depth + 1);
        index_walk(idx, n->u.while_loop.body, n, depth + 1);
        break;
    case AST_FOR:
        index_walk(idx, n->u.for_loop.init, n, depth + 1);
        index_walk(idx, n->u.for_loop.cond, n, depth + 1);
        index_walk(idx, n->u.for_loop.step, n, depth + 1);
        index_walk(idx, n->u.for_loop.body, n, depth + 1);
        break;
    case AST_SWITCH:
        index_walk(idx, n->u.switch_stmt.selector, n, depth + 1);
        for (i = 0; i < n->u.switch_stmt.ncases; i++) {
            index_walk(idx, n->u.switch_stmt.cases[i], n, depth + 1);
        }
        break;
    case AST_CASE_CLAUSE:
        index_walk(idx, n->u.clause.value, n, depth + 1);
        index_walk(idx, n->u.clause.body, n, depth + 1);
        break;
    case AST_DEFAULT_CLAUSE:
        index_walk(idx, n->u.clause.body, n, depth + 1);
        break;
    case AST_RETURN:
        index_walk(idx, n->u.ret.value, n, depth + 1);
        break;
    case AST_EXPR_STMT:
        index_walk(idx, n->u.expr_stmt.expr, n, depth + 1);
        break;
    case AST_TYPE_PTR:
    case AST_TYPE_SLICE:
        index_walk(idx, n->u.type_derived.base, n, depth + 1);
        break;
    case AST_TYPE_ARRAY:
        index_walk(idx, n->u.type_derived.base, n, depth + 1);
        index_walk(idx, n->u.type_derived.len, n, depth + 1);
        break;
    case AST_EXPR_ARRAY_LITERAL:
        for (i = 0; i < n->u.array_literal.nelems; i++) {
            index_walk(idx, n->u.array_literal.elems[i], n, depth + 1);
        }
        index_walk(idx, n->u.array_literal.count, n, depth + 1);
        break;
    case AST_EXPR_PAREN:
        index_walk(idx, n->u.paren.expr, n, depth + 1);
        break;
    case AST_EXPR_UNARY:
        index_walk(idx, n->u.unary.operand, n, depth + 1);
        break;
    case AST_EXPR_BINARY:
        index_walk(idx, n->u.binary.lhs, n, depth + 1);
        index_walk(idx, n->u.binary.rhs, n, depth + 1);
        break;
    case AST_EXPR_ASSIGN:
        index_walk(idx, n->u.assign.target, n, depth + 1);
        index_walk(idx, n->u.assign.value, n, depth + 1);
        break;
    case AST_EXPR_TERNARY:
        index_walk(idx, n->u.branch.cond, n, depth + 1);
        index_walk(idx, n->u.branch.then, n, depth + 1);
        index_walk(idx, n->u.branch.els, n, depth + 1);
        break;
    case AST_EXPR_INDEX:
        index_walk(idx, n->u.index_slice.base, n, depth + 1);
        index_walk(idx, n->u.index_slice.index, n, depth + 1);
        break;
    case AST_EXPR_SLICE:
        index_walk(idx, n->u.index_slice.base, n, depth + 1);
        index_walk(idx, n->u.index_slice.lo, n, depth + 1);
        index_walk(idx, n->u.index_slice.hi, n, depth + 1);
        break;
    case AST_EXPR_CALL:
        index_walk(idx, n->u.call.callee, n, depth + 1);
        for (i = 0; i < n->u.call.nargs; i++) {
            index_walk(idx, n->u.call.args[i], n, depth + 1);
        }
        break;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        index_walk(idx, n->u.member.base, n, depth + 1);
        break;
    case AST_EXPR_STRUCT_INIT:
        index_walk(idx, n->u.struct_init.base, n, depth + 1);
        for (i = 0; i < n->u.struct_init.nfields; i++) {
            index_walk(idx, n->u.struct_init.fields[i], n, depth + 1);
        }
        break;
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF:
    case AST_EXPR_SIZEOF_EXPR:
    case AST_EXPR_LEN:
    case AST_EXPR_PTR:
        index_walk(idx, n->u.size_op.operand, n, depth + 1);
        break;
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        index_walk(idx, n->u.cast_wrap.type, n, depth + 1);
        index_walk(idx, n->u.cast_wrap.expr, n, depth + 1);
        break;
    default:
        /* leaves: module/import decl (qname), prim/named types, literals,
         * identifiers, break/continue/empty stmt */;
        break;
    }
}

static void index_free(AstIndex *idx)
{
    free(idx->entries);
    memset(idx, 0, sizeof(*idx));
}

/* Find the source construct: prefer an entry whose kind text equals the
 * builder's construct kind and whose span equals `span`; otherwise the
 * deepest entry with the exact span (the smallest construct covering that
 * source range - used for lowering-introduced nodes whose kind text is
 * coarse, e.g. "AST_EXPR"). Returns NULL when no entry matches the span. */
static const AstEntry *index_find_source(const AstIndex *idx,
                                         const DiagSpan *span,
                                         const char *kind_text)
{
    const AstEntry *best = NULL;
    size_t i;

    for (i = 0; i < idx->count; i++) {
        const AstEntry *e = &idx->entries[i];
        if (!span_equal(e->node->span, span)) {
            continue;
        }
        if (kind_text != NULL &&
            strcmp(ast_kind_text(e->node->kind), kind_text) == 0) {
            return e;   /* exact kind + span match: the source construct */
        }
        if (best == NULL || e->depth > best->depth) {
            best = e;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * Resolved-reference facts
 * ------------------------------------------------------------------------- */

/* The IR declaration node whose fully qualified name is `fqn` (mirrors the
 * local find_decl_node helpers in the 16c1b/c/d packages; a small copy
 * because those are static). Returns NULL when not found. */
static IrNode *cause_find_decl(IrBuild *build, const char *fqn)
{
    size_t mi, di;
    if (build == NULL || fqn == NULL) {
        return NULL;
    }
    for (mi = 0; mi < build->nmodules; mi++) {
        IrNode *m = build->modules[mi];
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

/* The interned type descriptor for a struct/enum declaration node (the
 * descriptor whose u.decl is that declaration), or -1. */
static int64_t cause_type_id_for_decl(IrBuild *build, const IrNode *decl)
{
    size_t i;
    for (i = 0; i < build->ntypes; i++) {
        IrType *t = build->types[i];
        if ((t->kind == IRT_STRUCT || t->kind == IRT_ENUM) &&
            t->u.decl == decl) {
            return t->id;
        }
    }
    return -1;
}

/* The IRC_ENUM constant id for enum member `member_name` of enum decl
 * `enum_decl` (the interned representative of the member's value), or -1. */
static int64_t cause_enum_member_const(IrBuild *build,
                                       const IrNode *enum_decl,
                                       const char *member_name)
{
    size_t i, mi;
    int64_t value = -1;
    if (enum_decl == NULL || enum_decl->kind != IR_ENUM_DECL) {
        return -1;
    }
    for (mi = 0; mi < enum_decl->u.enum_decl.nmembers; mi++) {
        if (enum_decl->u.enum_decl.members[mi].name != NULL &&
            strcmp(enum_decl->u.enum_decl.members[mi].name,
                   member_name) == 0) {
            value = enum_decl->u.enum_decl.members[mi].value;
            break;
        }
    }
    if (value < 0) {
        return -1;
    }
    for (i = 0; i < build->nconsts; i++) {
        IrConst *c = build->consts[i];
        if (c->kind == IRC_ENUM && c->u.en.enum_decl == enum_decl &&
            (int64_t)c->u.en.value == value) {
            return c->id;
        }
    }
    return -1;
}

/* The IR_LOCAL_DECL node whose primary span equals `decl_span` (the local
 * declaration's AST span; the stmt mapper creates IR_LOCAL_DECL with the
 * AST decl's span), or -1. */
static int64_t cause_local_decl_id(IrBuild *build, const DiagSpan *decl_span)
{
    size_t i;
    for (i = 0; i < build->nnodes; i++) {
        IrNode *n = build->nodes[i];
        if (n->kind == IR_LOCAL_DECL && span_equal(n->span, decl_span)) {
            return n->id;
        }
    }
    return -1;
}

/* Fill resolved-reference facts for one cause link whose construct is
 * `ast` in `module`. Every fact defaults to -1; only determinable facts
 * are filled (see the header for the exact policy). */
static void cause_link_refs(IrBuild *build, const NameModule *module,
                            const AstNode *ast,
                            int64_t *ref_decl, int64_t *ref_type,
                            int64_t *ref_const)
{
    const NameSymbol *sym;

    *ref_decl = -1;
    *ref_type = -1;
    *ref_const = -1;

    if (ast == NULL) {
        return;
    }

    switch (ast->kind) {
    case AST_EXPR_IDENT:
        sym = name_symbol_for_node(module, ast);
        if (sym == NULL) {
            return;
        }
        switch (sym->kind) {
        case NAME_SYM_FN:
        case NAME_SYM_STRUCT:
        case NAME_SYM_ENUM:
        case NAME_SYM_GLOBAL_VAR:
        case NAME_SYM_GLOBAL_CONST: {
            IrNode *decl = cause_find_decl(build, sym->fqn);
            if (decl == NULL) {
                return;
            }
            *ref_decl = decl->id;
            if (sym->kind == NAME_SYM_STRUCT ||
                sym->kind == NAME_SYM_ENUM) {
                *ref_type = cause_type_id_for_decl(build, decl);
            }
            if (sym->kind == NAME_SYM_GLOBAL_CONST &&
                decl->kind == IR_GLOBAL_CONST &&
                decl->u.global_const.value != NULL) {
                *ref_const = decl->u.global_const.value->id;
            }
            return;
        }
        case NAME_SYM_LOCAL_VAR:
            /* the declaring AST node's span identifies the IR_LOCAL_DECL */
            if (sym->decl != NULL) {
                *ref_decl = cause_local_decl_id(build, sym->decl->span);
            }
            return;
        case NAME_SYM_FIELD: {
            IrNode *owner = sym->owner != NULL
                                ? cause_find_decl(build, sym->owner->fqn)
                                : NULL;
            if (owner != NULL) {
                *ref_decl = owner->id;
            }
            return;
        }
        case NAME_SYM_ENUM_MEMBER: {
            IrNode *owner = sym->owner != NULL
                                ? cause_find_decl(build, sym->owner->fqn)
                                : NULL;
            if (owner != NULL) {
                *ref_decl = owner->id;
                *ref_type = cause_type_id_for_decl(build, owner);
                *ref_const = cause_enum_member_const(build, owner,
                                                     sym->name);
            }
            return;
        }
        default:
            /* NAME_SYM_PARAM / NAME_SYM_LOCAL_CONST /
             * NAME_SYM_MODULE_IMPORT: no IR declaration node id (params
             * are slots; local consts fold at use sites) */
            return;
        }
        /* unreachable */

    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW: {
        /* member access: the member node itself is the resolved reference */
        const NameSymbol *msym = name_symbol_for_node(module, ast);
        if (msym == NULL) {
            return;
        }
        if (msym->kind == NAME_SYM_FIELD) {
            IrNode *owner = msym->owner != NULL
                                ? cause_find_decl(build, msym->owner->fqn)
                                : NULL;
            if (owner != NULL) {
                *ref_decl = owner->id;
            }
            return;
        }
        if (msym->kind == NAME_SYM_ENUM_MEMBER) {
            IrNode *owner = msym->owner != NULL
                                ? cause_find_decl(build, msym->owner->fqn)
                                : NULL;
            if (owner != NULL) {
                *ref_decl = owner->id;
                *ref_type = cause_type_id_for_decl(build, owner);
                *ref_const = cause_enum_member_const(build, owner,
                                                     msym->name);
            }
            return;
        }
        return;
    }

    case AST_TYPE_NAMED: {
        const NameSymbol *tsym = name_symbol_for_node(module, ast);
        if (tsym == NULL) {
            return;
        }
        if (tsym->kind == NAME_SYM_STRUCT ||
            tsym->kind == NAME_SYM_ENUM) {
            IrNode *decl = cause_find_decl(build, tsym->fqn);
            if (decl != NULL) {
                *ref_decl = decl->id;
                *ref_type = cause_type_id_for_decl(build, decl);
            }
        }
        return;
    }

    case AST_EXPR_CALL: {
        /* a call's resolved reference is its callee (the callee ident is a
         * child node, so the refs table keys the callee, not the call) */
        const NameSymbol *csym = ast->u.call.callee != NULL
                                     ? name_symbol_for_node(module,
                                                            ast->u.call.callee)
                                     : NULL;
        if (csym != NULL &&
            (csym->kind == NAME_SYM_FN ||
             csym->kind == NAME_SYM_GLOBAL_CONST ||
             csym->kind == NAME_SYM_STRUCT ||
             csym->kind == NAME_SYM_ENUM)) {
            IrNode *decl = cause_find_decl(build, csym->fqn);
            if (decl != NULL) {
                *ref_decl = decl->id;
                if (csym->kind == NAME_SYM_STRUCT ||
                    csym->kind == NAME_SYM_ENUM) {
                    *ref_type = cause_type_id_for_decl(build, decl);
                }
                if (csym->kind == NAME_SYM_GLOBAL_CONST &&
                    decl->kind == IR_GLOBAL_CONST &&
                    decl->u.global_const.value != NULL) {
                    *ref_const = decl->u.global_const.value->id;
                }
            }
        }
        return;
    }

    default:
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Chain building
 * ------------------------------------------------------------------------- */

typedef struct ChainLink {
    const AstNode *construct;
} ChainLink;

typedef struct Chain {
    ChainLink *links;
    size_t count;
    size_t cap;
    bool oom;
} Chain;

static bool chain_append(Chain *ch, const AstNode *construct)
{
    ChainLink *p;
    if (ch->count == ch->cap) {
        size_t ncap = ch->cap ? ch->cap * 2 : 16;
        p = (ChainLink *)realloc(ch->links, ncap * sizeof(*p));
        if (p == NULL) {
            ch->oom = true;
            return false;
        }
        ch->links = p;
        ch->cap = ncap;
    }
    ch->links[ch->count].construct = construct;
    ch->count++;
    return true;
}

/* Walk the parent index from `start` to the module root (the AST_PROGRAM
 * entry, whose parent is NULL), appending source-first links. */
static void chain_from_entry(Chain *ch, const AstIndex *idx,
                             const AstEntry *start)
{
    const AstEntry *e = start;
    while (e != NULL) {
        if (!chain_append(ch, e->node)) {
            return;
        }
        if (e->parent == NULL) {
            return;   /* module root reached */
        }
        /* find the parent entry (linear scan; deterministic) */
        {
            size_t i;
            const AstEntry *pe = NULL;
            for (i = 0; i < idx->count; i++) {
                if (idx->entries[i].node == e->parent) {
                    pe = &idx->entries[i];
                    break;
                }
            }
            e = pe;
        }
    }
}

/* Install `chain` as node->causes, freeing the old chain. `build` is only
 * used for its oom flag on failure; the caller returns IR_BUILDER_OOM. */
static bool install_chain(IrBuild *build, IrNode *node, const Chain *ch,
                          const NameModule *module)
{
    IrCauseLink *links;
    size_t i;

    links = (IrCauseLink *)malloc(ch->count * sizeof(*links));
    if (links == NULL) {
        build->oom = true;
        return false;
    }
    for (i = 0; i < ch->count; i++) {
        int64_t rd, rt, rc;
        const AstNode *c = ch->links[i].construct;
        links[i].construct_kind = dup_str_cause(ast_kind_text(c->kind));
        links[i].span = diag_span_clone(c->span);
        cause_link_refs(build, module, c, &rd, &rt, &rc);
        links[i].ref_decl = rd;
        links[i].ref_type = rt;
        links[i].ref_const = rc;
        if (links[i].construct_kind == NULL || links[i].span == NULL) {
            size_t k;
            for (k = 0; k <= i; k++) {
                free(links[k].construct_kind);
                diag_span_free(links[k].span);
            }
            free(links);
            build->oom = true;
            return false;
        }
    }

    /* free the old chain exactly as ir_build_free frees it */
    for (i = 0; i < node->cause_count; i++) {
        free(node->causes[i].construct_kind);
        diag_span_free(node->causes[i].span);
    }
    free(node->causes);
    node->causes = links;
    node->cause_count = ch->count;
    return true;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

IrBuilderStatus ir_builder_cause_finalize(IrBuild *build,
                                          const NameResult *result)
{
    AstIndex *indexes;
    size_t nindexes;
    size_t mi, ni;

    if (build == NULL || result == NULL) {
        return IR_BUILDER_UNSUPPORTED;
    }

    /* Build one parent index per module with a source AST (deterministic
     * module order = NameResult order). Runtime modules have
     * program == NULL and are skipped: their nodes carry synthetic spans
     * and keep their single-link chains (disclosed in the header). */
    nindexes = 0;
    indexes = (AstIndex *)calloc(result->nmodules, sizeof(*indexes));
    if (indexes == NULL) {
        build->oom = true;
        return IR_BUILDER_OOM;
    }
    for (mi = 0; mi < result->nmodules; mi++) {
        const NameModule *m = result->modules[mi];
        if (m == NULL || m->program == NULL) {
            continue;
        }
        indexes[nindexes].module = m;
        index_walk(&indexes[nindexes], m->program, NULL, 0);
        if (indexes[nindexes].oom) {
            build->oom = true;
            for (ni = 0; ni <= nindexes; ni++) {
                index_free(&indexes[ni]);
            }
            free(indexes);
            return IR_BUILDER_OOM;
        }
        nindexes++;
    }

    for (ni = 0; ni < build->nnodes; ni++) {
        IrNode *node = build->nodes[ni];
        const AstIndex *idx = NULL;
        const AstEntry *src = NULL;
        const NameModule *module = NULL;
        Chain ch;
        size_t i;

        if (node == NULL || node->causes == NULL ||
            node->cause_count < 1 || node->span == NULL) {
            continue;   /* defensive; the builders always attach >= 1 link */
        }

        /* locate the module by span file (the node's primary span is a
         * clone of its source construct's AST span) */
        for (i = 0; i < nindexes; i++) {
            if (indexes[i].module->program != NULL &&
                indexes[i].module->program->span != NULL &&
                indexes[i].module->program->span->file != NULL &&
                node->span->file != NULL &&
                strcmp(indexes[i].module->program->span->file,
                       node->span->file) == 0) {
                idx = &indexes[i];
                break;
            }
        }
        if (idx == NULL) {
            continue;   /* runtime/synthetic span: keep the existing chain */
        }

        src = index_find_source(idx, node->span,
                                node->causes[0].construct_kind);
        if (src == NULL) {
            continue;   /* no AST source construct: keep the existing chain */
        }

        memset(&ch, 0, sizeof(ch));
        chain_from_entry(&ch, idx, src);
        if (ch.oom) {
            free(ch.links);
            build->oom = true;
            goto oom;
        }

        module = idx->module;
        if (!install_chain(build, node, &ch, module)) {
            free(ch.links);
            goto oom;
        }
        free(ch.links);
    }

    for (ni = 0; ni < nindexes; ni++) {
        index_free(&indexes[ni]);
    }
    free(indexes);
    return build->oom ? IR_BUILDER_OOM : IR_BUILDER_OK;

oom:
    for (ni = 0; ni < nindexes; ni++) {
        index_free(&indexes[ni]);
    }
    free(indexes);
    return IR_BUILDER_OOM;
}
