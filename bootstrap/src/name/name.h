/* bootstrap/src/name/name.h
 *
 * AI-Co Stage-0 name resolution, modules, and imports (WP-M0-10).
 *
 * Consumes the parsed AST (WP-M0-09) of the entry module plus the project
 * root and entry module name supplied by the driver (WP-M0-19 reads them
 * from the build manifest per spec §14.4) and produces:
 *   - the module graph (entry + imports) with canonical module-to-file
 *     mapping (spec §6.5: a.b.c -> <root>/a/b/c.ai), cycle detection, and
 *     reserved rt.* handling;
 *   - per-module name tables: module scope (single name space, §6.2),
 *     function/block scopes with shadowing (§6.1), struct field and enum
 *     member namespaces;
 *   - name-phase diagnostics AIC-N0201..N0209 per spec §6 and the
 *     diagnostic contract §11.3 / §7.
 *
 * This header is the resolution API contract the build driver (WP-M0-19)
 * consumes: it supplies the project root (canonical import-resolution root)
 * and the entry module name from the build manifest; the name package
 * resolves imports itself by loading/lexing/parsing imported files at their
 * canonical paths (consuming the WP-M0-07/08/09 read-only APIs).
 *
 * Determinism obligations (spec §14.2):
 *   - resolution depends only on the project root and the entry file
 *     (never the current working directory, environment, registry, or
 *     network);
 *   - module iteration order is deterministic (entry first, then imports in
 *     source order, depth-first);
 *   - records are returned sorted with the contract §9 comparator
 *     (diag_sort_records) and carry phase "name", severity "error",
 *     recovery "authoritative";
 *   - no absolute host paths or timestamps appear in any span or record;
 *     span file names are repository-relative canonical paths.
 *
 * Ownership:
 *   - On NAME_OK / NAME_DIAG_ERROR, *out_result and (when non-empty)
 *     *out_records / *out_record_count are owned by the caller
 *     (name_result_free / name_records_free). The result OWNS the imported
 *     module sources and ASTs; the entry module's source/AST remain owned
 *     by the caller (they are borrowed, never freed here).
 *   - On NAME_OOM nothing is allocated and nothing is owned.
 */
#ifndef AICO_BOOTSTRAP_SRC_NAME_NAME_H
#define AICO_BOOTSTRAP_SRC_NAME_NAME_H

#include "../ast/ast.h"
#include "../diag/diag.h"
#include "../load/load.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum NameStatus {
    NAME_OK = 0,             /* resolution completed, no diagnostics */
    NAME_DIAG_ERROR,         /* resolution completed AND diagnostics exist */
    NAME_OOM                 /* allocation failure; nothing produced */
} NameStatus;

/* Symbol kinds (spec §6.1-6.3). */
typedef enum NameSymbolKind {
    NAME_SYM_FN = 0,
    NAME_SYM_STRUCT,
    NAME_SYM_ENUM,
    NAME_SYM_GLOBAL_VAR,
    NAME_SYM_GLOBAL_CONST,
    NAME_SYM_PARAM,
    NAME_SYM_LOCAL_VAR,
    NAME_SYM_LOCAL_CONST,
    NAME_SYM_FIELD,
    NAME_SYM_ENUM_MEMBER,
    NAME_SYM_MODULE_IMPORT   /* binding for an imported module name */
} NameSymbolKind;

typedef struct NameModule NameModule;
typedef struct NameSymbol NameSymbol;

/* One resolved declaration. `fqn` is the fully qualified name
 * (module.name for module scope; module.fn.name for locals;
 * module.Type.name for fields/members). `decl` is the declaring AST node
 * (NULL for import bindings). `span` is the declaration's identifier span
 * (used for secondary spans). `scope_depth` is 0 for module scope, 1 for
 * the function parameter scope, 2+ for block scopes, -1 for field/enum
 * member namespaces. `owner` is the owning struct/enum symbol for
 * field/enum-member symbols (NULL otherwise); `members` is the
 * field/enum-member symbol list on a struct/enum symbol (NULL otherwise). */
struct NameSymbol {
    NameSymbolKind kind;
    char *name;              /* unqualified name, owned */
    char *fqn;               /* fully qualified name, owned */
    bool is_pub;
    AstNode *decl;           /* declaring AST node; NULL for import bindings */
    DiagSpan *span;          /* identifier span, owned */
    NameModule *module;      /* owning module */
    int scope_depth;
    NameSymbol *owner;       /* struct/enum owner for fields/members */
    NameSymbol **members;    /* member list on struct/enum symbols */
    size_t nmembers;
};

/* One resolved reference: an AST node (identifier expression, named type,
 * or member/arrow access) mapped to the symbol it resolves to. */
typedef struct NameRef {
    AstNode *node;
    NameSymbol *sym;
} NameRef;

/* One module in the build. The entry module is modules[0]; imported
 * modules follow in deterministic depth-first source order. `path` is the
 * canonical repository-relative path (e.g. "a/b.ai"); it is NULL for
 * reserved runtime modules. `is_runtime` marks the compiler-provided
 * rt.mem/rt.io/rt.proc/rt.trap modules. */
struct NameModule {
    char **parts;            /* fqn parts, owned */
    size_t nparts;
    char *fqn;               /* dotted fqn, owned */
    char *path;              /* canonical relative path or NULL, owned */
    AstNode *program;        /* AST; owned by result for imports, borrowed for entry */
    LoadSource *src;         /* source; owned by result for imports, borrowed for entry */
    bool is_entry;
    bool is_runtime;
    NameSymbol **module_scope;   /* top-level declarations (single name space) */
    size_t nmodule_scope;
    NameModule **imports;        /* imported modules (module graph edges) */
    size_t nimports;
    NameRef *refs;               /* resolved references in this module */
    size_t nrefs;
};

/* Result of name resolution. Owns imported module sources/ASTs and all
 * symbol/ref tables. */
typedef struct NameResult {
    NameModule **modules;    /* entry first, then imports in DFS order */
    size_t nmodules;
    char *project_root;      /* owned copy */
    char *entry_file;        /* owned copy */
    /* Internal ownership list: every NameSymbol created during resolution
     * (module scope, fields/members, params, locals). Freed by
     * name_result_free; not part of the resolution contract. */
    NameSymbol **syms;
    size_t nsyms;
    size_t syms_cap;
} NameResult;

/* Resolve the program rooted at `entry_program`.
 *
 *   project_root       canonical import-resolution root (from the build
 *                      manifest; used only to build canonical file paths
 *                      for imports - never embedded in spans/records).
 *   entry_module_name  the entry module name from the build manifest
 *                      (spec §14.4); the entry file's module declaration
 *                      must match it, else AIC-N0205.
 *   entry_file         repository-relative path of the entry source.
 *   entry_src          the loaded entry source (must be LOAD_OK; spans use
 *                      its file name). Borrowed.
 *   entry_program      the parsed entry AST (must be a clean PARSE_OK
 *                      tree). Borrowed.
 *
 * Returns NAME_OK / NAME_DIAG_ERROR with *out_result set (and records when
 * non-empty), or NAME_OOM with nothing owned.
 */
NameStatus name_resolve(const char *project_root,
                        const char *entry_module_name,
                        const char *entry_file,
                        const LoadSource *entry_src,
                        const AstNode *entry_program,
                        NameResult **out_result,
                        DiagRecord ***out_records, size_t *out_record_count);

void name_result_free(NameResult *result);
void name_records_free(DiagRecord **records, size_t count);

/* Lookup helpers (used by later packages, e.g. WP-M0-11 types). */
NameModule *name_module_by_fqn(const NameResult *result, const char *fqn);
NameSymbol *name_module_lookup(const NameModule *module, const char *name);
NameSymbol *name_symbol_for_node(const NameModule *module, const AstNode *node);
const char *name_symbol_kind_text(NameSymbolKind kind);

#endif /* AICO_BOOTSTRAP_SRC_NAME_NAME_H */
