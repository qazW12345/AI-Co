/* bootstrap/src/name/name.c
 *
 * AI-Co Stage-0 name resolution, modules, and imports (WP-M0-10).
 *
 * Consumes the entry module's parsed AST (WP-M0-09) plus the project root
 * and entry module name supplied by the driver (WP-M0-19 reads them from
 * the build manifest per spec §14.4). Resolves the full module graph
 * (loading, lexing, and parsing imported files at their canonical paths
 * via the WP-M0-07/08/09 read-only APIs), builds per-module name tables,
 * and emits the name-phase diagnostics AIC-N0201..N0209.
 *
 * Design decisions (detailed in README.md):
 *   1. Module-to-file mapping is canonical and cwd-independent: import
 *      a.b.c resolves to <project_root>/a/b/c.ai; spans carry the canonical
 *      repository-relative name (a/b/c.ai), never an absolute path.
 *   2. Module graph resolution is a depth-first walk in import source
 *      order (deterministic). The entry module is resolved first; each
 *      imported module is loaded once and reused by FQN (spec §6.5: same
 *      fully qualified name always denotes the same declaration).
 *   3. Cycle detection: an import of a module already in the current
 *      depth-first path closes a cycle. Primary span: the import statement
 *      in the entry-most cycle member that leads into the cycle (the
 *      corpus pins "import a;" in the entry for main->a->main); secondary
 *      spans: the module declarations of the remaining cycle members.
 *   4. Module scope is the entire module (order-independent): all
 *      top-level declarations are registered before any body/type is
 *      resolved, so mutual recursion works without forward declarations
 *      (spec §6.1).
 *   5. Single name space per scope (spec §6.2): struct/enum names share
 *      the scope with values. Duplicate declarations in the same scope
 *      (module, function-parameter, block, struct-field namespace, enum
 *      member namespace) are AIC-N0201.
 *   6. Span choices follow the accepted negative corpus exactly:
 *      N0201 primary = later declaration's span minus a trailing ';',
 *      secondary = earlier declaration's identifier; N0203 primary = the
 *      reference (member chain), secondary = the private declaration's
 *      head (keyword through identifier); N0204/N0208 primary = the
 *      import's qualified name; N0209 primary = the whole import
 *      statement; N0205/N0207 primary = the whole module declaration;
 *      N0206 primary = the closing import statement, secondary = module
 *      declarations of the remaining cycle members.
 *   7. rt.* rules (spec §6.5): module declarations with the reserved "rt"
 *      prefix are N0207; imports of rt submodules outside the runtime
 *      surface are N0208; bare `import rt;` is N0209; runtime members are
 *      not auto-available (a reference to a reserved runtime name without
 *      the matching import is an ordinary undeclared name, N0202).
 *   8. Records are collected and sorted with the contract §9 comparator
 *      (diag_sort_records) before being returned; every name record is
 *      phase "name", severity "error", recovery "authoritative".
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "name.h"

#include "../lex/lex.h"
#include "../parse/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context
 * ------------------------------------------------------------------------- */

typedef struct NameScope {
    NameSymbol **syms;
    size_t n, cap;
    int depth;               /* 0 = module, 1 = fn params, 2+ = blocks */
} NameScope;

typedef struct NameCtx {
    const char *project_root;   /* borrowed from caller */
    NameResult *result;
    NameModule *cur_module;     /* module whose scope is currently visible */
    DiagRecord **records;
    size_t nrecords, records_cap;
    bool oom;

    /* scope stack during body resolution */
    NameScope *scopes;
    size_t nscopes, scopes_cap;

    /* depth-first module path (cycle detection) */
    NameModule **stack;
    size_t nstack, stack_cap;
    const AstNode **stack_imports;   /* the import that led to each frame
                                      * (NULL for the entry frame) */
    size_t nstack_imports, stack_imports_cap;
} NameCtx;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static void *xmalloc(NameCtx *c, size_t n)
{
    void *p = malloc(n);
    if (!p) c->oom = true;
    return p;
}

static char *dup_str(NameCtx *c, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(c, n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static char *dup_strn(NameCtx *c, const char *s, size_t len)
{
    char *p = (char *)xmalloc(c, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ---------------------------------------------------------------------------
 * Record creation (all name records: phase name, severity error,
 * recovery authoritative)
 * ------------------------------------------------------------------------- */

static DiagRecord *new_name_record(NameCtx *c, const char *code,
                                   const char *message,
                                   const DiagSpan *primary)
{
    DiagRecord *r = diag_record_new();
    if (!r) { c->oom = true; return NULL; }
    if (!diag_record_set_code(r, code)) { diag_record_free(r); c->oom = true; return NULL; }
    if (!diag_record_set_message(r, message)) { diag_record_free(r); c->oom = true; return NULL; }
    if (!diag_record_set_primary_span(r, primary)) { diag_record_free(r); c->oom = true; return NULL; }
    if (!diag_record_set_recovery(r, DIAG_RECOVERY_AUTHORITATIVE)) {
        diag_record_free(r); c->oom = true; return NULL;
    }
    return r;
}

static bool rec_push(NameCtx *c, DiagRecord *r)
{
    if (c->nrecords == c->records_cap) {
        size_t ncap = c->records_cap ? c->records_cap * 2 : 16;
        DiagRecord **nr = (DiagRecord **)realloc(c->records, ncap * sizeof(DiagRecord *));
        if (!nr) { c->oom = true; return false; }
        c->records = nr;
        c->records_cap = ncap;
    }
    c->records[c->nrecords++] = r;
    return true;
}

/* ---------------------------------------------------------------------------
 * Span helpers over the normalized source text
 * ------------------------------------------------------------------------- */

/* Skip whitespace and comments starting at offset `pos` in `src`; return the
 * offset of the next significant byte (or src->len at EOF). Comments are
 * whitespace per spec §4.1. */
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
            while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) pos++;
            if (pos + 1 < len) pos += 2;
            else pos = len;
        } else {
            break;
        }
    }
    return pos;
}

/* Identifier at position `pos` (must start with [A-Za-z_]); return the
 * end offset (exclusive) or -1 if not an identifier. */
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

/* Find the identifier span of a declaration's name. `decl_start` is the
 * declaration's start offset; `keyword` is the leading keyword text (may be
 * NULL when the declaration starts directly with the identifier, e.g. field
 * and param declarations). Returns true and fills *out_start/*out_end on
 * success (identifier token span). */
static bool decl_ident_span(const LoadSource *src, int64_t decl_start,
                            const char *keyword,
                            int64_t *out_start, int64_t *out_end)
{
    int64_t pos = decl_start;
    if (keyword) {
        size_t klen = strlen(keyword);
        if (pos + (int64_t)klen > (int64_t)src->len ||
            memcmp(src->text + pos, keyword, klen) != 0) {
            return false;
        }
        pos += (int64_t)klen;
    }
    pos = skip_trivia(src, pos);
    int64_t end = ident_end_at(src, pos);
    if (end < 0) return false;
    *out_start = pos;
    *out_end = end;
    return true;
}

/* Qualified-name span inside an import/module statement. `stmt_start` is
 * the statement start (at the "import"/"module" keyword), `stmt_end` is the
 * statement end (after the ';'). Scans: keyword, trivia, then the dotted
 * name; fills *out_start/*out_end with the qname token span. */
static bool qname_span_in_stmt(const LoadSource *src,
                               int64_t stmt_start,
                               int64_t *out_start, int64_t *out_end)
{
    int64_t pos = skip_trivia(src, stmt_start);
    int64_t kend = ident_end_at(src, pos);
    if (kend < 0) return false;
    pos = skip_trivia(src, kend);
    int64_t qstart = pos;
    int64_t qend = -1;
    for (;;) {
        int64_t e = ident_end_at(src, pos);
        if (e < 0) break;
        qend = e;
        pos = skip_trivia(src, e);
        if (pos < (int64_t)src->len && src->text[pos] == '.') {
            pos = skip_trivia(src, pos + 1);
            continue;
        }
        break;
    }
    if (qend < 0) return false;
    *out_start = qstart;
    *out_end = qend;
    return true;
}

/* Build a range span from offsets in `src`. */
static DiagSpan *span_range(const LoadSource *src, int64_t start, int64_t end)
{
    return load_span_range(src, start, end);
}

/* ---------------------------------------------------------------------------
 * Name tables
 * ------------------------------------------------------------------------- */

const char *name_symbol_kind_text(NameSymbolKind kind)
{
    switch (kind) {
    case NAME_SYM_FN:            return "fn";
    case NAME_SYM_STRUCT:        return "struct";
    case NAME_SYM_ENUM:          return "enum";
    case NAME_SYM_GLOBAL_VAR:    return "global_var";
    case NAME_SYM_GLOBAL_CONST:  return "global_const";
    case NAME_SYM_PARAM:         return "param";
    case NAME_SYM_LOCAL_VAR:     return "local_var";
    case NAME_SYM_LOCAL_CONST:   return "local_const";
    case NAME_SYM_FIELD:         return "field";
    case NAME_SYM_ENUM_MEMBER:   return "enum_member";
    case NAME_SYM_MODULE_IMPORT: return "module_import";
    }
    return "?";
}

static const char *decl_keyword(AstNodeKind kind)
{
    switch (kind) {
    case AST_FN_DECL:            return "fn";
    case AST_STRUCT_DECL:        return "struct";
    case AST_ENUM_DECL:          return "enum";
    case AST_GLOBAL_VAR_DECL:    return "var";
    case AST_GLOBAL_CONST_DECL:  return "const";
    case AST_VAR_DECL:           return "var";
    case AST_CONST_DECL:         return "const";
    default:                     return NULL;
    }
}

static const char *decl_name(const AstNode *node)
{
    switch (node->kind) {
    case AST_FN_DECL:            return node->u.fn_decl.name;
    case AST_STRUCT_DECL:        return node->u.struct_decl.name;
    case AST_ENUM_DECL:          return node->u.enum_decl.name;
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:  return node->u.global_decl.name;
    case AST_VAR_DECL:
    case AST_CONST_DECL:         return node->u.local_decl.name;
    case AST_FIELD_DECL:
    case AST_PARAM:
    case AST_ENUM_MEMBER:        return node->u.named.name;
    default:                     return NULL;
    }
}

static bool decl_is_pub(const AstNode *node)
{
    switch (node->kind) {
    case AST_FN_DECL:            return node->u.fn_decl.is_pub;
    case AST_STRUCT_DECL:        return node->u.struct_decl.is_pub;
    case AST_ENUM_DECL:          return node->u.enum_decl.is_pub;
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL:  return node->u.global_decl.is_pub;
    default:                     return false;
    }
}

static NameSymbolKind decl_kind(const AstNode *node)
{
    switch (node->kind) {
    case AST_FN_DECL:            return NAME_SYM_FN;
    case AST_STRUCT_DECL:        return NAME_SYM_STRUCT;
    case AST_ENUM_DECL:          return NAME_SYM_ENUM;
    case AST_GLOBAL_VAR_DECL:    return NAME_SYM_GLOBAL_VAR;
    case AST_GLOBAL_CONST_DECL:  return NAME_SYM_GLOBAL_CONST;
    case AST_PARAM:              return NAME_SYM_PARAM;
    case AST_VAR_DECL:           return NAME_SYM_LOCAL_VAR;
    case AST_CONST_DECL:         return NAME_SYM_LOCAL_CONST;
    case AST_FIELD_DECL:         return NAME_SYM_FIELD;
    case AST_ENUM_MEMBER:        return NAME_SYM_ENUM_MEMBER;
    default:                     return NAME_SYM_LOCAL_VAR;
    }
}

/* ---------------------------------------------------------------------------
 * Module / symbol construction
 * ------------------------------------------------------------------------- */

static bool module_push(NameCtx *c, NameModule *m)
{
    if (c->result->nmodules == 0) {
        NameModule **nm = (NameModule **)xmalloc(c, sizeof(NameModule *));
        if (!nm) return false;
        c->result->modules = nm;
    } else {
        NameModule **nm = (NameModule **)realloc(
            c->result->modules,
            (c->result->nmodules + 1) * sizeof(NameModule *));
        if (!nm) { c->oom = true; return false; }
        c->result->modules = nm;
    }
    c->result->modules[c->result->nmodules++] = m;
    return true;
}

static NameModule *module_new(NameCtx *c, const char *const *parts,
                              size_t nparts, const char *path,
                              const char *fqn)
{
    NameModule *m = (NameModule *)xmalloc(c, sizeof(NameModule));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->parts = (char **)xmalloc(c, nparts * sizeof(char *));
    if (!m->parts) { free(m); return NULL; }
    for (size_t i = 0; i < nparts; i++) {
        m->parts[i] = dup_str(c, parts[i]);
        if (!m->parts[i]) { free(m->parts); free(m); return NULL; }
    }
    m->nparts = nparts;
    m->fqn = dup_str(c, fqn);
    if (!m->fqn) { free(m->parts); free(m); return NULL; }
    m->path = path ? dup_str(c, path) : NULL;
    if (path && !m->path) { free(m->fqn); free(m->parts); free(m); return NULL; }
    return m;
}

static void symbol_free(NameSymbol *s)
{
    if (!s) return;
    free(s->name);
    free(s->fqn);
    diag_span_free(s->span);
    free(s);
}

/* Append a symbol to the result-wide ownership list. */
static bool result_sym_append(NameCtx *c, NameSymbol *s)
{
    if (c->result->nsyms == c->result->syms_cap) {
        size_t ncap = c->result->syms_cap ? c->result->syms_cap * 2 : 32;
        NameSymbol **ns = (NameSymbol **)realloc(
            c->result->syms, ncap * sizeof(NameSymbol *));
        if (!ns) { c->oom = true; return false; }
        c->result->syms = ns;
        c->result->syms_cap = ncap;
    }
    c->result->syms[c->result->nsyms++] = s;
    return true;
}

static NameSymbol *symbol_new(NameCtx *c, NameModule *module,
                              NameSymbolKind kind, const char *name,
                              const char *fqn, bool is_pub,
                              AstNode *decl, const DiagSpan *span,
                              int scope_depth)
{
    NameSymbol *s = (NameSymbol *)xmalloc(c, sizeof(NameSymbol));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->name = dup_str(c, name);
    if (!s->name) { free(s); return NULL; }
    s->fqn = fqn ? dup_str(c, fqn) : NULL;
    if (fqn && !s->fqn) { free(s->name); free(s); return NULL; }
    s->is_pub = is_pub;
    s->decl = decl;
    s->span = span ? diag_span_clone(span) : NULL;
    if (span && !s->span) { free(s->fqn); free(s->name); free(s); return NULL; }
    s->module = module;
    s->scope_depth = scope_depth;
    /* register ownership: the result frees every symbol created */
    if (!result_sym_append(c, s)) {
        symbol_free(s);
        return NULL;
    }
    return s;
}

static bool scope_push_symbol(NameScope *scope, NameCtx *c, NameSymbol *s)
{
    if (scope->n == scope->cap) {
        size_t ncap = scope->cap ? scope->cap * 2 : 8;
        NameSymbol **ns = (NameSymbol **)realloc(scope->syms, ncap * sizeof(NameSymbol *));
        if (!ns) { c->oom = true; return false; }
        scope->syms = ns;
        scope->cap = ncap;
    }
    scope->syms[scope->n++] = s;
    return true;
}

static NameSymbol *scope_find(const NameScope *scope, const char *name)
{
    for (size_t i = scope->n; i-- > 0;) {
        if (strcmp(scope->syms[i]->name, name) == 0) return scope->syms[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Scope stack
 * ------------------------------------------------------------------------- */

static bool scope_push(NameCtx *c, int depth)
{
    if (c->nscopes == c->scopes_cap) {
        size_t ncap = c->scopes_cap ? c->scopes_cap * 2 : 8;
        NameScope *ns = (NameScope *)realloc(c->scopes, ncap * sizeof(NameScope));
        if (!ns) { c->oom = true; return false; }
        c->scopes = ns;
        c->scopes_cap = ncap;
    }
    NameScope *sc = &c->scopes[c->nscopes];
    memset(sc, 0, sizeof(*sc));
    sc->depth = depth;
    c->nscopes++;
    return true;
}

static void scope_pop(NameCtx *c)
{
    if (c->nscopes == 0) return;
    c->nscopes--;
    free(c->scopes[c->nscopes].syms);
    c->scopes[c->nscopes].syms = NULL;
}

static NameScope *scope_top(NameCtx *c)
{
    return c->nscopes ? &c->scopes[c->nscopes - 1] : NULL;
}

static NameSymbol *scope_stack_find(NameCtx *c, const char *name)
{
    for (size_t i = c->nscopes; i-- > 0;) {
        NameSymbol *s = scope_find(&c->scopes[i], name);
        if (s) return s;
    }
    /* module scope: the whole module is visible (spec §6.1) */
    if (c->cur_module) {
        NameSymbol *s = name_module_lookup(c->cur_module, name);
        if (s) return s;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Module registry (by FQN)
 * ------------------------------------------------------------------------- */

static NameModule *module_by_fqn_internal(NameCtx *c, const char *fqn)
{
    for (size_t i = 0; i < c->result->nmodules; i++) {
        if (strcmp(c->result->modules[i]->fqn, fqn) == 0) {
            return c->result->modules[i];
        }
    }
    return NULL;
}

NameModule *name_module_by_fqn(const NameResult *result, const char *fqn)
{
    for (size_t i = 0; i < result->nmodules; i++) {
        if (strcmp(result->modules[i]->fqn, fqn) == 0) {
            return result->modules[i];
        }
    }
    return NULL;
}

NameSymbol *name_module_lookup(const NameModule *module, const char *name)
{
    for (size_t i = module->nmodule_scope; i-- > 0;) {
        if (strcmp(module->module_scope[i]->name, name) == 0) {
            return module->module_scope[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Reference map
 * ------------------------------------------------------------------------- */

static bool ref_add(NameCtx *c, NameModule *module, AstNode *node, NameSymbol *sym)
{
    if (module->nrefs == 0) {
        NameRef *nr = (NameRef *)xmalloc(c, sizeof(NameRef));
        if (!nr) return false;
        module->refs = nr;
    } else {
        NameRef *nr = (NameRef *)realloc(module->refs,
                                         (module->nrefs + 1) * sizeof(NameRef));
        if (!nr) { c->oom = true; return false; }
        module->refs = nr;
    }
    module->refs[module->nrefs].node = node;
    module->refs[module->nrefs].sym = sym;
    module->nrefs++;
    return true;
}

NameSymbol *name_symbol_for_node(const NameModule *module, const AstNode *node)
{
    for (size_t i = 0; i < module->nrefs; i++) {
        if (module->refs[i].node == node) return module->refs[i].sym;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

static bool resolve_node(NameCtx *c, NameModule *module, AstNode *node);
static bool resolve_type(NameCtx *c, NameModule *module, AstNode *type);
static bool resolve_expr(NameCtx *c, NameModule *module, AstNode *expr);
static bool resolve_stmt(NameCtx *c, NameModule *module, AstNode *stmt);
static bool resolve_module(NameCtx *c, NameModule *module);
static bool load_and_resolve_import(NameCtx *c, NameModule *from,
                                    AstNode *import_decl);
static NameModule *match_module_prefix(NameCtx *c, NameModule *module,
                                       const char *const *segments,
                                       size_t nsegments, size_t k);

/* ---------------------------------------------------------------------------
 * Diagnostic emitters
 * ------------------------------------------------------------------------- */

/* Emit AIC-N0201 for a duplicate declaration. `later` is the new
 * declaration node, `earlier` the previously-registered symbol. Primary
 * span: later declaration minus a trailing ';' (corpus convention);
 * secondary: earlier declaration's identifier. */
static void emit_duplicate(NameCtx *c, NameModule *module,
                           AstNode *later, NameSymbol *earlier)
{
    const LoadSource *src = module->src;
    int64_t start = later->span->start.offset;
    int64_t end = later->span->end.offset;
    if (src && end > start && (size_t)(end - 1) < src->len &&
        src->text[end - 1] == ';') {
        end--;
    }
    DiagSpan *primary = span_range(src, start, end);
    char msg[256];
    const char *nm = decl_name(later);
    snprintf(msg, sizeof(msg), "duplicate declaration of '%s' in same scope",
             nm ? nm : "?");
    DiagRecord *r = new_name_record(c, "AIC-N0201", msg, primary);
    diag_span_free(primary);
    if (!r) return;
    if (earlier->span) {
        diag_record_add_secondary_span(r, earlier->span);
    }
    rec_push(c, r);
}

/* Emit AIC-N0202 for an undeclared name reference. */
static void emit_undeclared(NameCtx *c, NameModule *module, AstNode *node,
                            const char *name, const DiagSpan *span)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "undeclared name '%s'", name ? name : "?");
    DiagRecord *r = new_name_record(c, "AIC-N0202", msg, span);
    (void)module;
    (void)node;
    if (r) rec_push(c, r);
}

/* Emit AIC-N0203 for access to a private item from another module.
 * `ref` is the reference expression node; `decl` the private declaration
 * node in `module`. Secondary span: the declaration head (keyword through
 * identifier). */
static void emit_private_access(NameCtx *c, NameModule *module,
                                AstNode *ref, AstNode *decl)
{
    const LoadSource *src = module->src;
    char msg[256];
    const char *nm = decl_name(decl);
    snprintf(msg, sizeof(msg), "access to private item '%s' in module '%s'",
             nm ? nm : "?", module->fqn ? module->fqn : "?");
    DiagRecord *r = new_name_record(c, "AIC-N0203", msg, ref->span);
    if (!r) return;
    int64_t dstart = decl->span->start.offset;
    int64_t is, ie;
    const char *kw = decl_keyword(decl->kind);
    if (src && decl_ident_span(src, dstart, kw, &is, &ie)) {
        DiagSpan *head = span_range(src, dstart, ie);
        if (head) {
            diag_record_add_secondary_span(r, head);
            diag_span_free(head);
        }
    }
    rec_push(c, r);
}

/* ---------------------------------------------------------------------------
 * Module scope registration
 * ------------------------------------------------------------------------- */

/* Register one top-level declaration into the module scope (single name
 * space). Returns the symbol (or NULL on OOM). */
static NameSymbol *register_module_decl(NameCtx *c, NameModule *module,
                                        AstNode *decl)
{
    const char *name = decl_name(decl);
    if (!name) return NULL;

    NameSymbol *earlier = scope_find(&c->scopes[0], name);
    if (earlier) {
        emit_duplicate(c, module, decl, earlier);
        /* the later declaration is rejected; keep the first */
        return earlier;
    }

    char *fqn = NULL;
    {
        size_t fl = strlen(module->fqn) + 1 + strlen(name) + 1;
        fqn = (char *)xmalloc(c, fl);
        if (!fqn) return NULL;
        snprintf(fqn, fl, "%s.%s", module->fqn, name);
    }

    int64_t is, ie;
    const char *kw = decl_keyword(decl->kind);
    DiagSpan *ispan = NULL;
    if (decl_ident_span(module->src, decl->span->start.offset, kw, &is, &ie)) {
        ispan = span_range(module->src, is, ie);
    }
    NameSymbol *s = symbol_new(c, module, decl_kind(decl), name, fqn,
                               decl_is_pub(decl), decl,
                               ispan ? ispan : decl->span, 0);
    if (ispan) diag_span_free(ispan);
    free(fqn);
    if (!s) return NULL;
    if (!scope_push_symbol(&c->scopes[0], c, s)) {
        return NULL;   /* OOM; symbol stays owned by result->syms */
    }
    if (module->nmodule_scope == 0) {
        NameSymbol **nm = (NameSymbol **)xmalloc(c, sizeof(NameSymbol *));
        if (!nm) return NULL;
        module->module_scope = nm;
    } else {
        NameSymbol **nm = (NameSymbol **)realloc(
            module->module_scope,
            (module->nmodule_scope + 1) * sizeof(NameSymbol *));
        if (!nm) { c->oom = true; return NULL; }
        module->module_scope = nm;
    }
    module->module_scope[module->nmodule_scope++] = s;
    return s;
}

/* Append a member symbol to a struct/enum symbol's member list. */
static bool member_append(NameCtx *c, NameSymbol *owner, NameSymbol *member)
{
    if (owner->nmembers == 0) {
        NameSymbol **nm = (NameSymbol **)xmalloc(c, sizeof(NameSymbol *));
        if (!nm) return false;
        owner->members = nm;
    } else {
        NameSymbol **nm = (NameSymbol **)realloc(
            owner->members, (owner->nmembers + 1) * sizeof(NameSymbol *));
        if (!nm) { c->oom = true; return false; }
        owner->members = nm;
    }
    owner->members[owner->nmembers++] = member;
    return true;
}

static NameSymbol *member_find(const NameSymbol *owner, const char *name)
{
    for (size_t i = owner->nmembers; i-- > 0;) {
        if (strcmp(owner->members[i]->name, name) == 0) return owner->members[i];
    }
    return NULL;
}

/* Register struct fields / enum members into the type's namespace
 * (spec §6.1: struct fields and enum members are members of the type and
 * do not leak into enclosing scopes). Duplicate names within the type are
 * AIC-N0201. Returns false on OOM. */
static bool register_type_members(NameCtx *c, NameModule *module,
                                  NameSymbol *owner, AstNode **members,
                                  size_t nmembers, NameSymbolKind kind)
{
    for (size_t i = 0; i < nmembers; i++) {
        AstNode *m = members[i];
        const char *name = decl_name(m);
        if (!name) continue;

        NameSymbol *earlier = member_find(owner, name);
        if (earlier) {
            emit_duplicate(c, module, m, earlier);
            continue;   /* keep the first member */
        }

        char *fqn = NULL;
        {
            size_t fl = strlen(owner->fqn) + 1 + strlen(name) + 1;
            fqn = (char *)xmalloc(c, fl);
            if (!fqn) return false;
            snprintf(fqn, fl, "%s.%s", owner->fqn, name);
        }
        int64_t is, ie;
        DiagSpan *ispan = NULL;
        if (decl_ident_span(module->src, m->span->start.offset, NULL, &is, &ie)) {
            ispan = span_range(module->src, is, ie);
        }
        NameSymbol *s = symbol_new(c, module, kind, name, fqn, false, m,
                                   ispan ? ispan : m->span, -1);
        if (ispan) diag_span_free(ispan);
        free(fqn);
        if (!s) return false;
        s->owner = owner;
        if (!member_append(c, owner, s)) {
            return false;   /* OOM; symbol stays owned by result->syms */
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Reference resolution: types, expressions, member chains
 * ------------------------------------------------------------------------- */

static bool resolve_type(NameCtx *c, NameModule *module, AstNode *type)
{
    if (!type) return true;
    switch (type->kind) {
    case AST_TYPE_PRIM:
        return true;
    case AST_TYPE_NAMED: {
        AstName *nm = type->u.type_named.name;
        if (nm->count == 1) {
            const char *name = nm->parts[0];
            NameSymbol *s = scope_stack_find(c, name);
            if (!s) {
                emit_undeclared(c, module, type, name, type->span);
                return true;
            }
            return ref_add(c, module, type, s);
        }
        /* module-qualified named type (spec §6.6): longest prefix that is
         * the current module or an import selects the module; the remaining
         * segments resolve in that module's scope. */
        for (size_t k = nm->count; k >= 1; k--) {
            NameModule *target = match_module_prefix(c, module,
                                                     (const char *const *)nm->parts,
                                                     nm->count, k);
            if (!target) continue;
            NameSymbol *s = NULL;
            bool failed = false;
            size_t remaining = nm->count - k;
            for (size_t i = 0; i < remaining && !failed; i++) {
                const char *nm2 = nm->parts[k + i];
                s = name_module_lookup(target, nm2);
                if (!s) {
                    emit_undeclared(c, module, type, nm2, type->span);
                    failed = true;
                    break;
                }
                if (i + 1 < remaining) {
                    if (s->kind == NAME_SYM_ENUM) {
                        NameSymbol *m = member_find(s, nm->parts[k + i + 1]);
                        if (!m) {
                            emit_undeclared(c, module, type,
                                            nm->parts[k + i + 1], type->span);
                            failed = true;
                            break;
                        }
                        i++;
                    } else {
                        i = remaining;   /* field path: types package owns */
                    }
                }
            }
            if (failed) return true;
            if (!s) continue;
            if (!s->is_pub && target != module && s->decl) {
                emit_private_access(c, target, type, s->decl);
            }
            return ref_add(c, module, type, s);
        }
        /* no module prefix matched: treat the first segment as a scope name */
        {
            const char *name = nm->parts[0];
            NameSymbol *s = scope_stack_find(c, name);
            if (!s) {
                emit_undeclared(c, module, type, name, type->span);
                return true;
            }
            return ref_add(c, module, type, s);
        }
    }
    case AST_TYPE_PTR:
    case AST_TYPE_SLICE:
        return resolve_type(c, module, type->u.type_derived.base);
    case AST_TYPE_ARRAY:
        if (!resolve_type(c, module, type->u.type_derived.base)) return false;
        if (type->u.type_derived.len) {
            return resolve_expr(c, module, type->u.type_derived.len);
        }
        return true;
    default:
        return true;
    }
}

static bool resolve_expr(NameCtx *c, NameModule *module, AstNode *expr);
static bool resolve_member_chain(NameCtx *c, NameModule *module, AstNode *node);

static bool resolve_expr(NameCtx *c, NameModule *module, AstNode *expr)
{
    if (!expr) return true;
    switch (expr->kind) {
    case AST_EXPR_INT_LITERAL:
    case AST_EXPR_STR_LITERAL:
    case AST_EXPR_BOOL_LITERAL:
    case AST_EXPR_NULL_LITERAL:
        return true;
    case AST_EXPR_IDENT: {
        const char *name = expr->u.ident.name;
        NameSymbol *s = scope_stack_find(c, name);
        if (!s) {
            emit_undeclared(c, module, expr, name, expr->span);
            return true;
        }
        return ref_add(c, module, expr, s);
    }
    case AST_EXPR_ARRAY_LITERAL:
        for (size_t i = 0; i < expr->u.array_literal.nelems; i++) {
            if (!resolve_expr(c, module, expr->u.array_literal.elems[i])) return false;
        }
        if (expr->u.array_literal.count) {
            return resolve_expr(c, module, expr->u.array_literal.count);
        }
        return true;
    case AST_EXPR_PAREN:
        return resolve_expr(c, module, expr->u.paren.expr);
    case AST_EXPR_UNARY:
        return resolve_expr(c, module, expr->u.unary.operand);
    case AST_EXPR_BINARY:
        if (!resolve_expr(c, module, expr->u.binary.lhs)) return false;
        return resolve_expr(c, module, expr->u.binary.rhs);
    case AST_EXPR_ASSIGN:
        if (!resolve_expr(c, module, expr->u.assign.target)) return false;
        return resolve_expr(c, module, expr->u.assign.value);
    case AST_EXPR_TERNARY:
        if (!resolve_expr(c, module, expr->u.branch.cond)) return false;
        if (!resolve_expr(c, module, expr->u.branch.then)) return false;
        return resolve_expr(c, module, expr->u.branch.els);
    case AST_EXPR_INDEX:
        if (!resolve_expr(c, module, expr->u.index_slice.base)) return false;
        return resolve_expr(c, module, expr->u.index_slice.index);
    case AST_EXPR_SLICE:
        if (!resolve_expr(c, module, expr->u.index_slice.base)) return false;
        if (expr->u.index_slice.lo &&
            !resolve_expr(c, module, expr->u.index_slice.lo)) return false;
        if (expr->u.index_slice.hi &&
            !resolve_expr(c, module, expr->u.index_slice.hi)) return false;
        return true;
    case AST_EXPR_CALL:
        if (!resolve_expr(c, module, expr->u.call.callee)) return false;
        for (size_t i = 0; i < expr->u.call.nargs; i++) {
            if (!resolve_expr(c, module, expr->u.call.args[i])) return false;
        }
        return true;
    case AST_EXPR_MEMBER:
    case AST_EXPR_ARROW:
        return resolve_member_chain(c, module, expr);
    case AST_EXPR_STRUCT_INIT: {
        /* base is a type-name expression; field names are checked by the
         * type checker (AIC-T0313), the value expressions resolve here. */
        if (!resolve_expr(c, module, expr->u.struct_init.base)) return false;
        for (size_t i = 0; i < expr->u.struct_init.nfields; i++) {
            AstNode *fi = expr->u.struct_init.fields[i];
            if (fi->u.named.value &&
                !resolve_expr(c, module, fi->u.named.value)) return false;
        }
        return true;
    }
    case AST_EXPR_SIZEOF_TYPE:
    case AST_EXPR_ALIGNOF:
        return resolve_type(c, module, expr->u.size_op.operand);
    case AST_EXPR_SIZEOF_EXPR:
        return resolve_expr(c, module, expr->u.size_op.operand);
    case AST_EXPR_CAST:
    case AST_EXPR_WRAP:
        if (!resolve_type(c, module, expr->u.cast_wrap.type)) return false;
        return resolve_expr(c, module, expr->u.cast_wrap.expr);
    case AST_EXPR_LEN:
        return resolve_expr(c, module, expr->u.size_op.operand);
    case AST_EXPR_PTR:
        return resolve_expr(c, module, expr->u.size_op.operand);
    default:
        return true;
    }
}

/* Match a dotted prefix against the module registry: prefix is the joined
 * dotted string of segments[0..k); returns the module when it is the
 * current module or directly imported by `module`. */
static NameModule *match_module_prefix(NameCtx *c, NameModule *module,
                                       const char *const *segments,
                                       size_t nsegments, size_t k)
{
    /* build the prefix string */
    (void)nsegments;
    size_t total = 0;
    for (size_t i = 0; i < k; i++) total += strlen(segments[i]) + 1;
    char *pref = (char *)xmalloc(c, total);
    if (!pref) return NULL;
    pref[0] = '\0';
    for (size_t i = 0; i < k; i++) {
        if (i) strcat(pref, ".");
        strcat(pref, segments[i]);
    }
    NameModule *m = module_by_fqn_internal(c, pref);
    free(pref);
    if (!m) return NULL;
    if (m == module) return m;
    for (size_t i = 0; i < module->nimports; i++) {
        if (module->imports[i] == m) return m;
    }
    return NULL;
}

/* Resolve a member/arrow chain (e.g. a.b.g, Color.Red, p.x, rt.mem.fn).
 * Rules (spec §6.1, §6.4-6.6):
 *  - module-qualified reference: the longest prefix of the chain that is
 *    the current module or an imported module selects the module; the rest
 *    of the chain resolves in that module's scope (visibility checked,
 *    AIC-N0203 for private items reached from another module);
 *  - enum member access: EnumType.Member resolves through the enum type
 *    name only;
 *  - value member access (p.x): the base resolves here; the field name is
 *    a type-checking concern (deferred);
 *  - reserved runtime names without the matching import resolve as
 *    ordinary undeclared names (AIC-N0202, primary span = the reference).
 * Returns false only on allocation failure. */
static bool resolve_member_chain(NameCtx *c, NameModule *module, AstNode *node)
{
    /* collect member names from the left spine, innermost last; the spine
     * ends at the root identifier */
    const char **segs = NULL;
    size_t nsegs = 0, cap = 0;
    AstNode *cur = node;
    AstNode *root_ident = NULL;
    bool oom = false;
    bool failed = false;

    while (cur->kind == AST_EXPR_MEMBER || cur->kind == AST_EXPR_ARROW) {
        if (nsegs == cap) {
            cap = cap ? cap * 2 : 8;
            const char **ns = (const char **)realloc(
                (void *)segs, cap * sizeof(const char *));
            if (!ns) { oom = true; break; }
            segs = ns;
        }
        segs[nsegs++] = cur->u.member.name;
        cur = cur->u.member.base;
    }
    if (oom) { c->oom = true; free((void *)segs); return false; }
    if (cur->kind != AST_EXPR_IDENT) {
        /* base is a complex expression: resolve it, defer the field */
        free((void *)segs);
        return resolve_expr(c, module, cur);
    }
    root_ident = cur;

    /* The spine collects member names outermost-first (for a.b.f it
     * collects "f" then "b"); reverse so segs is innermost-first, then
     * prepend the root identifier so segs[0..nsegs] is the full path in
     * textual order (a, b, f). */
    for (size_t i = 0, j = nsegs; i < j; i++, j--) {
        const char *t = segs[i];
        segs[i] = segs[j - 1];
        segs[j - 1] = t;
    }
    if (nsegs == cap) {
        cap = cap ? cap * 2 : 8;
        const char **ns = (const char **)realloc(
            (void *)segs, cap * sizeof(const char *));
        if (!ns) { c->oom = true; free((void *)segs); return false; }
        segs = ns;
    }
    for (size_t i = nsegs; i > 0; i--) segs[i] = segs[i - 1];
    segs[0] = root_ident->u.ident.name;
    nsegs++;

    if (nsegs >= 2) {
        /* module-qualified: longest prefix that is the current module or an
         * imported module selects the module; resolve the rest within it */
        bool matched = false;
        for (size_t k = nsegs; k >= 1 && !matched && !failed; k--) {
            NameModule *target = match_module_prefix(c, module, segs, nsegs, k);
            if (!target) continue;
            matched = true;

            NameSymbol *s = NULL;
            size_t remaining = nsegs - k;
            for (size_t i = 0; i < remaining && !failed; i++) {
                const char *nm = segs[k + i];
                s = name_module_lookup(target, nm);
                if (!s) {
                    emit_undeclared(c, module, node, nm, node->span);
                    failed = true;
                    break;
                }
                if (i + 1 < remaining) {
                    if (s->kind == NAME_SYM_ENUM) {
                        NameSymbol *m = member_find(s, segs[k + i + 1]);
                        if (!m) {
                            emit_undeclared(c, module, node, segs[k + i + 1],
                                            node->span);
                            failed = true;
                            break;
                        }
                        i++;   /* consume the member segment */
                    } else {
                        /* struct field / value member: type-checking owns
                         * the field name; stop resolving here */
                        i = remaining;
                    }
                }
            }
            if (!failed && s) {
                if (!s->is_pub && target != module && s->decl) {
                    emit_private_access(c, target, node, s->decl);
                }
                if (!ref_add(c, module, node, s)) failed = true;
            }
        }
        if (oom) { free((void *)segs); return false; }
        if (failed) { free((void *)segs); return true; }
        if (matched) { free((void *)segs); return true; }
    }

    /* not module-qualified: root identifier resolves in scope */
    NameSymbol *base = scope_stack_find(c, segs[0]);
    if (!base) {
        emit_undeclared(c, module, root_ident, segs[0], node->span);
        free((void *)segs);
        return true;
    }
    if (base->kind == NAME_SYM_ENUM && nsegs == 2) {
        NameSymbol *m = member_find(base, segs[1]);
        if (!m) {
            emit_undeclared(c, module, node, segs[1], node->span);
            free((void *)segs);
            return true;
        }
        if (!ref_add(c, module, node, m)) { free((void *)segs); return false; }
        free((void *)segs);
        return true;
    }
    /* value member access: record the base, defer the field to types */
    if (!ref_add(c, module, root_ident, base)) { free((void *)segs); return false; }
    free((void *)segs);
    return true;
}

/* ---------------------------------------------------------------------------
 * Local declaration registration (function body scopes)
 * ------------------------------------------------------------------------- */

/* Register a local var/const into the current block scope. Returns the
 * symbol, or NULL on OOM. On duplicate in the same block scope, emits
 * AIC-N0201 and returns the earlier symbol. */
static NameSymbol *register_local_decl(NameCtx *c, NameModule *module,
                                       AstNode *decl)
{
    const char *name = decl_name(decl);
    if (!name) return NULL;
    NameScope *top = scope_top(c);
    if (!top) { c->oom = true; return NULL; }

    NameSymbol *earlier = scope_find(top, name);
    if (earlier) {
        emit_duplicate(c, module, decl, earlier);
        return earlier;
    }

    char *fqn = NULL;
    {
        size_t fl = strlen(module->fqn) + 1 + strlen(name) + 1;
        fqn = (char *)xmalloc(c, fl);
        if (!fqn) return NULL;
        snprintf(fqn, fl, "%s.%s", module->fqn, name);
    }
    int64_t is, ie;
    const char *kw = decl_keyword(decl->kind);
    DiagSpan *ispan = NULL;
    if (decl_ident_span(module->src, decl->span->start.offset, kw, &is, &ie)) {
        ispan = span_range(module->src, is, ie);
    }
    NameSymbol *s = symbol_new(c, module, decl_kind(decl), name, fqn,
                               false, decl, ispan ? ispan : decl->span,
                               top->depth);
    if (ispan) diag_span_free(ispan);
    free(fqn);
    if (!s) return NULL;
    if (!scope_push_symbol(top, c, s)) {
        return NULL;   /* OOM; symbol stays owned by result->syms */
    }
    return s;
}

/* ---------------------------------------------------------------------------
 * Statement resolution
 * ------------------------------------------------------------------------- */

static bool resolve_stmt(NameCtx *c, NameModule *module, AstNode *stmt)
{
    if (!stmt) return true;
    switch (stmt->kind) {
    case AST_BLOCK: {
        if (!scope_push(c, scope_top(c) ? scope_top(c)->depth + 1 : 0)) {
            return false;
        }
        for (size_t i = 0; i < stmt->u.list.count; i++) {
            if (!resolve_stmt(c, module, stmt->u.list.items[i])) {
                scope_pop(c);
                return false;
            }
        }
        scope_pop(c);
        return true;
    }
    case AST_VAR_DECL:
    case AST_CONST_DECL: {
        NameSymbol *s = register_local_decl(c, module, stmt);
        if (!s) return false;
        if (stmt->u.local_decl.type &&
            !resolve_type(c, module, stmt->u.local_decl.type)) return false;
        if (stmt->u.local_decl.init &&
            !resolve_expr(c, module, stmt->u.local_decl.init)) return false;
        return true;
    }
    case AST_IF:
        if (!resolve_expr(c, module, stmt->u.branch.cond)) return false;
        if (!resolve_stmt(c, module, stmt->u.branch.then)) return false;
        return resolve_stmt(c, module, stmt->u.branch.els);
    case AST_WHILE:
        if (!resolve_expr(c, module, stmt->u.while_loop.cond)) return false;
        return resolve_stmt(c, module, stmt->u.while_loop.body);
    case AST_FOR: {
        /* the for-init declares in the loop's own scope (spec §13.3) */
        if (!scope_push(c, scope_top(c) ? scope_top(c)->depth + 1 : 0)) {
            return false;
        }
        if (stmt->u.for_loop.init) {
            if (stmt->u.for_loop.init->kind == AST_VAR_DECL ||
                stmt->u.for_loop.init->kind == AST_CONST_DECL) {
                if (!resolve_stmt(c, module, stmt->u.for_loop.init)) {
                    scope_pop(c);
                    return false;
                }
            } else if (!resolve_expr(c, module, stmt->u.for_loop.init)) {
                scope_pop(c);
                return false;
            }
        }
        if (stmt->u.for_loop.cond &&
            !resolve_expr(c, module, stmt->u.for_loop.cond)) {
            scope_pop(c);
            return false;
        }
        if (stmt->u.for_loop.step &&
            !resolve_expr(c, module, stmt->u.for_loop.step)) {
            scope_pop(c);
            return false;
        }
        if (!resolve_stmt(c, module, stmt->u.for_loop.body)) {
            scope_pop(c);
            return false;
        }
        scope_pop(c);
        return true;
    }
    case AST_SWITCH:
        if (!resolve_expr(c, module, stmt->u.switch_stmt.selector)) return false;
        for (size_t i = 0; i < stmt->u.switch_stmt.ncases; i++) {
            if (!resolve_stmt(c, module, stmt->u.switch_stmt.cases[i])) {
                return false;
            }
        }
        return true;
    case AST_CASE_CLAUSE:
        if (stmt->u.clause.value &&
            !resolve_expr(c, module, stmt->u.clause.value)) return false;
        return resolve_stmt(c, module, stmt->u.clause.body);
    case AST_DEFAULT_CLAUSE:
        return resolve_stmt(c, module, stmt->u.clause.body);
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_EMPTY_STMT:
        return true;
    case AST_RETURN:
        return resolve_expr(c, module, stmt->u.ret.value);
    case AST_EXPR_STMT:
        return resolve_expr(c, module, stmt->u.expr_stmt.expr);
    default:
        return true;
    }
}

/* ---------------------------------------------------------------------------
 * Top-level declaration resolution
 * ------------------------------------------------------------------------- */

static bool resolve_node(NameCtx *c, NameModule *module, AstNode *node)
{
    if (!node) return true;
    switch (node->kind) {
    case AST_STRUCT_DECL: {
        NameSymbol *sym = name_module_lookup(module, node->u.struct_decl.name);
        if (!sym || sym->decl != node) return true;  /* duplicate: first won */
        if (!register_type_members(c, module, sym, node->u.struct_decl.fields,
                                   node->u.struct_decl.nfields, NAME_SYM_FIELD)) {
            return false;
        }
        for (size_t i = 0; i < node->u.struct_decl.nfields; i++) {
            if (!resolve_type(c, module,
                              node->u.struct_decl.fields[i]->u.named.type)) {
                return false;
            }
        }
        return true;
    }
    case AST_ENUM_DECL: {
        NameSymbol *sym = name_module_lookup(module, node->u.enum_decl.name);
        if (!sym || sym->decl != node) return true;  /* duplicate: first won */
        if (node->u.enum_decl.underlying) {
            if (!resolve_type(c, module, node->u.enum_decl.underlying)) {
                return false;
            }
        }
        if (!register_type_members(c, module, sym, node->u.enum_decl.members,
                                   node->u.enum_decl.nmembers,
                                   NAME_SYM_ENUM_MEMBER)) {
            return false;
        }
        for (size_t i = 0; i < node->u.enum_decl.nmembers; i++) {
            AstNode *m = node->u.enum_decl.members[i];
            if (m->u.named.value &&
                !resolve_expr(c, module, m->u.named.value)) return false;
        }
        return true;
    }
    case AST_FN_DECL: {
        NameSymbol *sym = name_module_lookup(module, node->u.fn_decl.name);
        if (!sym || sym->decl != node) return true;  /* duplicate: first won */
        if (!scope_push(c, 1)) return false;
        for (size_t i = 0; i < node->u.fn_decl.nparams; i++) {
            AstNode *p = node->u.fn_decl.params[i];
            NameSymbol *s = register_local_decl(c, module, p);
            if (!s) { scope_pop(c); return false; }
            if (!resolve_type(c, module, p->u.named.type)) {
                scope_pop(c);
                return false;
            }
        }
        if (node->u.fn_decl.ret_type &&
            !resolve_type(c, module, node->u.fn_decl.ret_type)) {
            scope_pop(c);
            return false;
        }
        if (node->u.fn_decl.body &&
            !resolve_stmt(c, module, node->u.fn_decl.body)) {
            scope_pop(c);
            return false;
        }
        scope_pop(c);
        return true;
    }
    case AST_GLOBAL_VAR_DECL:
    case AST_GLOBAL_CONST_DECL: {
        NameSymbol *sym = name_module_lookup(module, node->u.global_decl.name);
        if (!sym || sym->decl != node) return true;  /* duplicate: first won */
        if (node->u.global_decl.type &&
            !resolve_type(c, module, node->u.global_decl.type)) return false;
        if (node->u.global_decl.init &&
            !resolve_expr(c, module, node->u.global_decl.init)) return false;
        return true;
    }
    default:
        return true;
    }
}

/* ---------------------------------------------------------------------------
 * Module graph: imports, cycle detection, canonical file mapping
 * ------------------------------------------------------------------------- */

/* Append an imported module edge to `from->imports` (no duplicates). */
static bool add_import_edge(NameCtx *c, NameModule *from, NameModule *m)
{
    for (size_t i = 0; i < from->nimports; i++) {
        if (from->imports[i] == m) return true;
    }
    if (from->nimports == 0) {
        NameModule **nm = (NameModule **)xmalloc(c, sizeof(NameModule *));
        if (!nm) return false;
        from->imports = nm;
    } else {
        NameModule **nm = (NameModule **)realloc(
            from->imports, (from->nimports + 1) * sizeof(NameModule *));
        if (!nm) { c->oom = true; return false; }
        from->imports = nm;
    }
    from->imports[from->nimports++] = m;
    return true;
}

/* Split a dotted name into heap-owned parts; returns an allocated array of
 * nparts strings on success. */
static char **split_dotted(NameCtx *c, const char *name, size_t *out_n)
{
    size_t n = 1;
    for (const char *p = name; *p; p++) {
        if (*p == '.') n++;
    }
    char **parts = (char **)xmalloc(c, n * sizeof(char *));
    if (!parts) return NULL;
    size_t k = 0;
    const char *start = name;
    for (const char *p = name; ; p++) {
        if (*p == '.' || *p == '\0') {
            parts[k++] = dup_strn(c, start, (size_t)(p - start));
            if (!parts[k - 1]) { free(parts); return NULL; }
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    *out_n = k;
    return parts;
}

/* Join parts with '.'; returns a heap string. */
static char *join_dotted(NameCtx *c, char *const *parts, size_t n)
{
    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += strlen(parts[i]) + 1;
    char *s = (char *)xmalloc(c, total);
    if (!s) return NULL;
    s[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (i) strcat(s, ".");
        strcat(s, parts[i]);
    }
    return s;
}

/* Canonical module-to-file mapping (spec §6.5): a.b.c -> <root>/a/b/c.ai.
 * Returns a heap-owned repository-relative path ("a/b/c.ai"). */
static char *canonical_module_path(NameCtx *c, char *const *parts, size_t n)
{
    size_t total = 1;
    for (size_t i = 0; i < n; i++) total += strlen(parts[i]) + 1;
    total += 3;   /* ".ai" */
    char *p = (char *)xmalloc(c, total);
    if (!p) return NULL;
    p[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (i) strcat(p, "/");
        strcat(p, parts[i]);
    }
    strcat(p, ".ai");
    return p;
}

/* Read a whole file as bytes; caller frees *out. Returns false on I/O error
 * (no diagnostics produced). */
static bool read_file_bytes(const char *full_path, uint8_t **out,
                            size_t *out_len)
{
    FILE *f = fopen(full_path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return false; }
    *out = buf;
    *out_len = (size_t)sz;
    return true;
}

/* Emit AIC-N0206 for an import cycle. `stack` is the current DFS path; the
 * closing import (the one whose target is already on the path) is
 * `closing_import` in `closing_module`. Primary span: the import statement
 * in the entry-most cycle member that leads into the cycle (corpus-pinned);
 * secondary spans: the module declarations of the remaining cycle members. */
static void emit_cycle(NameCtx *c, NameModule *const *stack, size_t nstack,
                       const AstNode *const *stack_imports, size_t found_index,
                       NameModule *closing_module,
                       const AstNode *closing_import)
{
    const AstNode *primary_import = NULL;
    if (found_index + 1 < nstack) {
        primary_import = stack_imports[found_index + 1];
    } else {
        /* direct self-import / cycle closes at the top frame */
        primary_import = closing_import;
    }
    DiagRecord *r = new_name_record(c, "AIC-N0206",
                                    "import cycle detected",
                                    primary_import ? primary_import->span : NULL);
    (void)closing_module;
    if (!r) return;
    /* secondary: module declarations of the remaining cycle members */
    for (size_t i = found_index + 1; i < nstack; i++) {
        const AstNode *prog = stack[i]->program;
        if (prog && prog->u.program.module_decl) {
            diag_record_add_secondary_span(r, prog->u.program.module_decl->span);
        }
    }
    rec_push(c, r);
}

/* Load and resolve one import statement in `from`. Handles rt.* rules,
 * canonical path resolution, cycle detection, dedup, file load/lex/parse,
 * module-declaration checks, and recursive module resolution. Returns false
 * only on allocation failure. */
static bool load_and_resolve_import(NameCtx *c, NameModule *from,
                                    AstNode *import_decl)
{
    AstName *qn = import_decl->u.qname.name;
    size_t n = qn->count;
    if (n == 0) return true;

    /* Build the dotted fqn once. */
    char *fqn = join_dotted(c, qn->parts, n);
    if (!fqn) return false;

    /* --- rt.* reserved rules (spec §6.5) --- */
    if (strcmp(qn->parts[0], "rt") == 0) {
        if (n == 1) {
            /* bare `import rt;` -> AIC-N0209, primary = whole import stmt */
            DiagRecord *r = new_name_record(
                c, "AIC-N0209",
                "bare 'import rt;' is not allowed; "
                "import a specific runtime submodule instead",
                import_decl->span);
            if (r) rec_push(c, r);
            free(fqn);
            return true;
        }
        if (!(strcmp(qn->parts[1], "mem") == 0 ||
              strcmp(qn->parts[1], "io") == 0 ||
              strcmp(qn->parts[1], "proc") == 0 ||
              strcmp(qn->parts[1], "trap") == 0)) {
            /* reserved rt submodule not in the runtime surface -> N0208 */
            int64_t qs, qe;
            DiagSpan *qspan = NULL;
            if (qname_span_in_stmt(from->src,
                                   import_decl->span->start.offset,
                                   &qs, &qe)) {
                qspan = span_range(from->src, qs, qe);
            }
            DiagRecord *r = new_name_record(
                c, "AIC-N0208",
                "import of reserved runtime submodule not in the "
                "runtime surface",
                qspan ? qspan : import_decl->span);
            if (qspan) diag_span_free(qspan);
            if (r) rec_push(c, r);
            free(fqn);
            return true;
        }
        /* valid runtime submodule: bind (or reuse) the compiler-provided
         * module; no user file is consulted. */
        NameModule *rm = module_by_fqn_internal(c, fqn);
        if (!rm) {
            rm = module_new(c, (const char *const *)qn->parts, n, NULL, fqn);
            if (!rm) { free(fqn); return false; }
            rm->is_runtime = true;
            if (!module_push(c, rm)) { free(fqn); return false; }
        }
        if (!add_import_edge(c, from, rm)) { free(fqn); return false; }
        free(fqn);
        return true;
    }

    /* --- non-reserved module --- */
    char *path = canonical_module_path(c, qn->parts, n);
    if (!path) { free(fqn); return false; }

    /* cycle detection: is the target already on the current DFS path? */
    for (size_t i = 0; i < c->nstack; i++) {
        if (strcmp(c->stack[i]->fqn, fqn) == 0) {
            emit_cycle(c, c->stack, c->nstack, c->stack_imports, i,
                       from, import_decl);
            free(fqn);
            free(path);
            return true;
        }
    }

    /* already loaded (diamond / repeated import): reuse, no re-resolution */
    NameModule *existing = module_by_fqn_internal(c, fqn);
    if (existing) {
        if (!add_import_edge(c, from, existing)) {
            free(fqn); free(path); return false;
        }
        free(fqn);
        free(path);
        return true;
    }

    /* canonical file: <project_root>/<path> */
    size_t rl = strlen(c->project_root);
    bool need_sep = rl > 0 && c->project_root[rl - 1] != '/' &&
                    c->project_root[rl - 1] != '\\';
    size_t fl = rl + (need_sep ? 1 : 0) + strlen(path) + 1;
    char *full = (char *)xmalloc(c, fl);
    if (!full) { free(fqn); free(path); return false; }
    snprintf(full, fl, "%s%s%s", c->project_root, need_sep ? "/" : "", path);

    uint8_t *bytes = NULL;
    size_t blen = 0;
    if (!read_file_bytes(full, &bytes, &blen)) {
        /* AIC-N0204: module not found at its canonical path */
        int64_t qs, qe;
        DiagSpan *qspan = NULL;
        if (qname_span_in_stmt(from->src, import_decl->span->start.offset,
                               &qs, &qe)) {
            qspan = span_range(from->src, qs, qe);
        }
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "imported module '%s' not found at canonical path",
                 fqn);
        DiagRecord *r = new_name_record(c, "AIC-N0204", msg,
                                        qspan ? qspan : import_decl->span);
        if (qspan) diag_span_free(qspan);
        if (r) rec_push(c, r);
        free(fqn); free(path); free(full);
        return true;
    }
    free(full);

    /* load (spans carry the canonical relative path) */
    LoadSource *src = NULL;
    DiagRecord **recs = NULL;
    size_t rn = 0;
    LoadStatus lst = load_source_from_bytes(path, bytes, blen,
                                            &src, &recs, &rn);
    free(bytes);
    if (lst == LOAD_VALIDATION_ERROR) {
        for (size_t i = 0; i < rn; i++) rec_push(c, recs[i]);
        free(recs);
        free(fqn); free(path);
        return true;
    }
    if (lst == LOAD_OOM) { free(fqn); free(path); return false; }

    /* lex */
    LexToken *toks = NULL;
    size_t tn = 0;
    LexStatus lxs = lex_tokenize(src, &toks, &tn, &recs, &rn);
    if (lxs == LEX_DIAG_ERROR) {
        for (size_t i = 0; i < rn; i++) rec_push(c, recs[i]);
        free(recs);
        lex_tokens_free(toks, tn);
        load_source_free(src);
        free(fqn); free(path);
        return true;
    }
    if (lxs == LEX_OOM) {
        load_source_free(src);
        free(fqn); free(path);
        return false;
    }

    /* parse */
    AstNode *prog = NULL;
    ParseStatus pst = parse_program(toks, tn, &prog, &recs, &rn);
    lex_tokens_free(toks, tn);
    if (pst == PARSE_DIAG_ERROR) {
        for (size_t i = 0; i < rn; i++) rec_push(c, recs[i]);
        free(recs);
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return true;
    }
    if (pst == PARSE_OOM) {
        load_source_free(src);
        free(fqn); free(path);
        return false;
    }

    /* module declaration checks on the imported file */
    const AstNode *mdecl = prog->u.program.module_decl;
    if (mdecl && mdecl->u.qname.name && mdecl->u.qname.name->count > 0 &&
        strcmp(mdecl->u.qname.name->parts[0], "rt") == 0) {
        /* AIC-N0207: reserved rt prefix on a module declaration */
        DiagRecord *r = new_name_record(
            c, "AIC-N0207",
            "module declaration uses the reserved 'rt' prefix",
            mdecl->span);
        if (r) rec_push(c, r);
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return true;
    }
    if (!mdecl || !mdecl->u.qname.name) {
        /* no module declaration -> cannot match the canonical path */
        DiagRecord *r = new_name_record(
            c, "AIC-N0205",
            "module declaration name does not match canonical path",
            prog->span);
        if (r) rec_push(c, r);
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return true;
    }
    char *decl_name = ast_name_to_string(mdecl->u.qname.name);
    if (!decl_name) {
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return false;
    }
    if (strcmp(decl_name, fqn) != 0) {
        /* AIC-N0205: module declaration does not match canonical path name */
        DiagRecord *r = new_name_record(
            c, "AIC-N0205",
            "module declaration name does not match canonical path",
            mdecl->span);
        if (r) rec_push(c, r);
        free(decl_name);
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return true;
    }
    free(decl_name);

    /* create and register the module */
    NameModule *m = module_new(c, (const char *const *)qn->parts, n,
                               path, fqn);
    if (!m) {
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return false;
    }
    m->src = src;          /* owned by the result */
    m->program = prog;     /* owned by the result */
    if (!module_push(c, m)) {
        ast_node_free(prog);
        load_source_free(src);
        free(fqn); free(path);
        return false;
    }
    if (!add_import_edge(c, from, m)) {
        free(fqn); free(path);
        return false;
    }

    /* DFS push + recursive resolution + pop */
    bool ok = true;
    if (c->nstack == c->stack_cap) {
        size_t ncap = c->stack_cap ? c->stack_cap * 2 : 8;
        NameModule **ns = (NameModule **)realloc(
            c->stack, ncap * sizeof(NameModule *));
        if (!ns) { c->oom = true; ok = false; }
        else {
            c->stack = ns;
            const AstNode **ni = (const AstNode **)realloc(
                c->stack_imports, ncap * sizeof(const AstNode *));
            if (!ni) { c->oom = true; ok = false; }
            else {
                c->stack_imports = ni;
                c->stack_cap = ncap;
            }
        }
    }
    if (ok) {
        c->stack[c->nstack] = m;
        c->stack_imports[c->nstack] = import_decl;
        c->nstack++;
        ok = resolve_module(c, m);
        c->nstack--;
    }

    free(fqn);
    free(path);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Module resolution driver
 * ------------------------------------------------------------------------- */

/* Resolve one module: build its module scope (order-independent), resolve
 * imports (module graph edges, DFS), then resolve declaration bodies. The
 * scope stack is per-module: the module scope sits at index 0 and body
 * resolution pushes/pops on top; the caller's scope stack is restored on
 * return so recursive import resolution is isolated. */
static bool resolve_module(NameCtx *c, NameModule *module)
{
    if (module->is_runtime) return true;   /* no user source to resolve */

    /* Save the caller's scope stack and current module. */
    size_t saved_nscopes = c->nscopes;
    NameScope *saved_scopes = c->scopes;
    size_t saved_cap = c->scopes_cap;
    NameModule *saved_cur = c->cur_module;
    c->nscopes = 0;
    c->scopes = NULL;
    c->scopes_cap = 0;
    c->cur_module = module;

    bool ok = true;

    /* module scope at index 0 */
    if (!scope_push(c, 0)) {
        c->nscopes = saved_nscopes;
        c->scopes = saved_scopes;
        c->scopes_cap = saved_cap;
        c->cur_module = saved_cur;
        return false;
    }

    const AstNode *prog = module->program;

    /* 1. register all top-level declarations (order-independent) */
    for (size_t i = 0; i < prog->u.program.ndecls && ok; i++) {
        if (!register_module_decl(c, module, prog->u.program.decls[i])) {
            ok = false;
        }
    }

    /* 2. resolve imports (module graph edges + recursive resolution) */
    for (size_t i = 0; i < prog->u.program.nimports && ok; i++) {
        if (!load_and_resolve_import(c, module,
                                     prog->u.program.imports[i])) {
            ok = false;
        }
    }

    /* 3. resolve declaration bodies */
    for (size_t i = 0; i < prog->u.program.ndecls && ok; i++) {
        if (!resolve_node(c, module, prog->u.program.decls[i])) {
            ok = false;
        }
    }

    while (c->nscopes > 0) scope_pop(c);
    free(c->scopes);
    c->nscopes = saved_nscopes;
    c->scopes = saved_scopes;
    c->scopes_cap = saved_cap;
    c->cur_module = saved_cur;
    return ok;
}

/* ---------------------------------------------------------------------------
 * Result / records ownership
 * ------------------------------------------------------------------------- */

void name_records_free(DiagRecord **records, size_t count)
{
    for (size_t i = 0; i < count; i++) diag_record_free(records[i]);
    free(records);
}

void name_result_free(NameResult *result)
{
    if (!result) return;
    /* every symbol created during resolution */
    for (size_t i = 0; i < result->nsyms; i++) {
        NameSymbol *s = result->syms[i];
        if (!s) continue;
        free(s->name);
        free(s->fqn);
        diag_span_free(s->span);
        free(s->members);   /* member symbols themselves are in result->syms */
        free(s);
    }
    free(result->syms);
    for (size_t i = 0; i < result->nmodules; i++) {
        NameModule *m = result->modules[i];
        if (!m) continue;
        for (size_t j = 0; j < m->nparts; j++) free(m->parts[j]);
        free(m->parts);
        free(m->fqn);
        free(m->path);
        free(m->module_scope);
        free(m->imports);
        free(m->refs);
        if (!m->is_entry) {
            /* imported modules own their source and AST; the entry is
             * borrowed from the caller */
            ast_node_free(m->program);
            load_source_free(m->src);
        }
        free(m);
    }
    free(result->modules);
    free(result->project_root);
    free(result->entry_file);
    free(result);
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

NameStatus name_resolve(const char *project_root,
                        const char *entry_module_name,
                        const char *entry_file,
                        const LoadSource *entry_src,
                        const AstNode *entry_program,
                        NameResult **out_result,
                        DiagRecord ***out_records, size_t *out_record_count)
{
    if (out_result) *out_result = NULL;
    if (out_records) *out_records = NULL;
    if (out_record_count) *out_record_count = 0;

    NameCtx c;
    memset(&c, 0, sizeof(c));
    c.project_root = project_root;

    c.result = (NameResult *)calloc(1, sizeof(NameResult));
    if (!c.result) return NAME_OOM;
    c.result->project_root = dup_str(&c, project_root);
    c.result->entry_file = dup_str(&c, entry_file);
    if (!c.result->project_root || !c.result->entry_file) {
        name_result_free(c.result);
        return NAME_OOM;
    }

    /* split the entry module name (e.g. "main" or "a.b.c") */
    size_t nparts = 0;
    char **parts = split_dotted(&c, entry_module_name, &nparts);
    if (!parts || nparts == 0) {
        free(parts);
        name_result_free(c.result);
        return NAME_OOM;
    }

    /* entry module declaration checks (spec §6.5 / §6.4) */
    const AstNode *mdecl = entry_program->u.program.module_decl;
    if (mdecl && mdecl->u.qname.name && mdecl->u.qname.name->count > 0 &&
        strcmp(mdecl->u.qname.name->parts[0], "rt") == 0) {
        DiagRecord *r = new_name_record(
            &c, "AIC-N0207",
            "module declaration uses the reserved 'rt' prefix",
            mdecl->span);
        if (r) rec_push(&c, r);
    } else if (!mdecl || !mdecl->u.qname.name) {
        DiagRecord *r = new_name_record(
            &c, "AIC-N0205",
            "module declaration name does not match canonical path",
            entry_program->span);
        if (r) rec_push(&c, r);
    } else {
        char *decl_name = ast_name_to_string(mdecl->u.qname.name);
        if (!decl_name) {
            free(parts);
            name_result_free(c.result);
            return NAME_OOM;
        }
        if (strcmp(decl_name, entry_module_name) != 0) {
            DiagRecord *r = new_name_record(
                &c, "AIC-N0205",
                "module declaration name does not match canonical path",
                mdecl->span);
            if (r) rec_push(&c, r);
        }
        free(decl_name);
    }

    /* create the entry module (source and AST borrowed from the caller) */
    NameModule *entry = module_new(&c, (const char *const *)parts, nparts,
                                   entry_file, entry_module_name);
    free(parts);
    if (!entry) {
        name_result_free(c.result);
        return NAME_OOM;
    }
    entry->is_entry = true;
    entry->src = (LoadSource *)entry_src;
    entry->program = (AstNode *)entry_program;
    if (!module_push(&c, entry)) {
        name_result_free(c.result);
        return NAME_OOM;
    }

    /* DFS stack: entry frame (import = NULL) */
    c.stack = (NameModule **)xmalloc(&c, sizeof(NameModule *));
    c.stack_imports = (const AstNode **)xmalloc(&c, sizeof(const AstNode *));
    if (!c.stack || !c.stack_imports) {
        name_result_free(c.result);
        return NAME_OOM;
    }
    c.stack_cap = 1;
    c.stack[0] = entry;
    c.stack_imports[0] = NULL;
    c.nstack = 1;

    bool ok = resolve_module(&c, entry);
    c.nstack--;

    free(c.stack);
    free(c.stack_imports);

    /* sort records with the contract §9 comparator before returning */
    diag_sort_records(c.records, c.nrecords);

    if (!ok || c.oom) {
        name_records_free(c.records, c.nrecords);
        name_result_free(c.result);
        return NAME_OOM;
    }

    *out_result = c.result;
    if (c.nrecords > 0) {
        *out_records = c.records;
        *out_record_count = c.nrecords;
        return NAME_DIAG_ERROR;
    }
    free(c.records);
    return NAME_OK;
}





