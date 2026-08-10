/* bootstrap/src/ast/ast.h
 *
 * AI-Co Stage-0 abstract syntax tree (WP-M0-09).
 *
 * Single-meaning syntax representation for the spec sec. 5.2 EBNF. The
 * parser (bootstrap/src/parse/parse.c) builds the tree; this header owns
 * the node definitions plus the deterministic dump and ownership helpers.
 *
 * Design rules:
 *  - Every node carries an owned DiagSpan (span preservation on every
 *    node; spans are exact source ranges of the construct).
 *  - The tree contains only fully-parsed constructs. When the parser
 *    reports a syntax error inside a construct, that construct is dropped
 *    from the tree (error recovery), so a tree produced by a parse with
 *    diagnostics is a prefix/best-effort tree and must not be processed by
 *    downstream stages (the driver stops when diagnostics exist).
 *  - Child arrays are owned (heap arrays of AstNode*); ast_node_free
 *    releases the whole subtree.
 *  - All owned strings and names are heap copies; callers that construct
 *    nodes manually (tests) must copy strings before attaching them.
 *
 * Ownership: this header is owned by WP-M0-09. It consumes (includes) the
 * lexer's LexIntType for integer literal typing (the lexer resolves literal
 * types per spec sec. 4.3); it must not be modified outside this package
 * without a Planner re-planning decision.
 */
#ifndef AICO_BOOTSTRAP_SRC_AST_AST_H
#define AICO_BOOTSTRAP_SRC_AST_AST_H

#include "../lex/lex.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Node kinds (one per grammar production; structs in the union below)
 * ------------------------------------------------------------------------- */

typedef enum AstNodeKind {
    /* top level */
    AST_PROGRAM = 0,
    AST_MODULE_DECL,
    AST_IMPORT_DECL,
    AST_STRUCT_DECL,
    AST_ENUM_DECL,
    AST_FN_DECL,
    AST_GLOBAL_VAR_DECL,
    AST_GLOBAL_CONST_DECL,
    /* struct fields / enum members / params */
    AST_FIELD_DECL,
    AST_ENUM_MEMBER,
    AST_PARAM,
    /* statements */
    AST_BLOCK,
    AST_VAR_DECL,          /* local declaration */
    AST_CONST_DECL,        /* local declaration */
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_SWITCH,
    AST_CASE_CLAUSE,
    AST_DEFAULT_CLAUSE,
    AST_BREAK,
    AST_CONTINUE,
    AST_RETURN,
    AST_EXPR_STMT,
    AST_EMPTY_STMT,
    /* types */
    AST_TYPE_PRIM,
    AST_TYPE_NAMED,
    AST_TYPE_PTR,
    AST_TYPE_ARRAY,
    AST_TYPE_SLICE,
    /* expressions */
    AST_EXPR_INT_LITERAL,
    AST_EXPR_STR_LITERAL,
    AST_EXPR_BOOL_LITERAL,
    AST_EXPR_NULL_LITERAL,
    AST_EXPR_IDENT,
    AST_EXPR_ARRAY_LITERAL,
    AST_EXPR_PAREN,
    AST_EXPR_UNARY,
    AST_EXPR_BINARY,
    AST_EXPR_ASSIGN,
    AST_EXPR_TERNARY,
    AST_EXPR_INDEX,
    AST_EXPR_SLICE,
    AST_EXPR_CALL,
    AST_EXPR_MEMBER,
    AST_EXPR_ARROW,
    AST_EXPR_STRUCT_INIT,
    AST_FIELD_INIT,
    AST_EXPR_SIZEOF_TYPE,
    AST_EXPR_SIZEOF_EXPR,
    AST_EXPR_ALIGNOF,
    AST_EXPR_CAST,
    AST_EXPR_WRAP,
    AST_EXPR_LEN,
    AST_EXPR_PTR
} AstNodeKind;

/* Primitive type names (spec sec. 7.1). */
typedef enum AstPrimKind {
    AST_PRIM_VOID = 0,
    AST_PRIM_BOOL,
    AST_PRIM_STR,
    AST_PRIM_I8, AST_PRIM_I16, AST_PRIM_I32, AST_PRIM_I64,
    AST_PRIM_U8, AST_PRIM_U16, AST_PRIM_U32, AST_PRIM_U64,
    AST_PRIM_ISIZE, AST_PRIM_USIZE
} AstPrimKind;

/* Unary operators (spec sec. 10.1). */
typedef enum AstUnaryOp {
    AST_UN_NEG = 0,   /* - */
    AST_UN_PLUS,      /* + */
    AST_UN_NOT,       /* ! */
    AST_UN_BNOT,      /* ~ */
    AST_UN_DEREF,     /* * */
    AST_UN_ADDR       /* & */
} AstUnaryOp;

/* Binary operators (spec sec. 10.1), left-associative. */
typedef enum AstBinaryOp {
    AST_BIN_MUL = 0, AST_BIN_DIV, AST_BIN_MOD,
    AST_BIN_ADD, AST_BIN_SUB,
    AST_BIN_SHL, AST_BIN_SHR,
    AST_BIN_LT, AST_BIN_LE, AST_BIN_GT, AST_BIN_GE,
    AST_BIN_EQ, AST_BIN_NE,
    AST_BIN_BAND, AST_BIN_BXOR, AST_BIN_BOR,
    AST_BIN_LAND, AST_BIN_LOR
} AstBinaryOp;

/* Assignment operators (spec sec. 10.1), right-associative. */
typedef enum AstAssignOp {
    AST_ASGN_ASSIGN = 0, /* = */
    AST_ASGN_ADD, AST_ASGN_SUB, AST_ASGN_MUL, AST_ASGN_DIV, AST_ASGN_MOD,
    AST_ASGN_SHL, AST_ASGN_SHR,
    AST_ASGN_BAND, AST_ASGN_BOR, AST_ASGN_BXOR
} AstAssignOp;

/* Qualified name: a.b.c (module decl, import, named type). */
typedef struct AstName {
    char **parts;        /* owned copies, parts[0] is the leading identifier */
    size_t count;
} AstName;

typedef struct AstNode AstNode;

typedef struct AstNode {
    AstNodeKind kind;
    DiagSpan *span;      /* owned; never NULL on constructed nodes */
    union {
        /* program: module decl (NULL when missing) + imports + decls */
        struct {
            AstNode *module_decl;
            AstNode **imports;   /* AST_IMPORT_DECL */
            size_t nimports;
            AstNode **decls;     /* top-level decls */
            size_t ndecls;
        } program;

        /* module_decl / import_decl: name=a.b.c */
        struct { AstName *name; } qname;

        /* struct_decl: name + fields */
        struct {
            char *name;
            AstNode **fields;   /* AST_FIELD_DECL */
            size_t nfields;
            bool is_pub;
        } struct_decl;

        /* enum_decl: name + underlying int type + members */
        struct {
            char *name;
            AstNode *underlying; /* AST_TYPE_PRIM (int type) */
            AstNode **members;   /* AST_ENUM_MEMBER */
            size_t nmembers;
            bool is_pub;
        } enum_decl;

        /* fn_decl: name + params + return type + body */
        struct {
            char *name;
            AstNode **params;    /* AST_PARAM */
            size_t nparams;
            AstNode *ret_type;   /* AST_TYPE_* */
            AstNode *body;       /* AST_BLOCK */
            bool is_pub;
        } fn_decl;

        /* global_var_decl / global_const_decl */
        struct {
            char *name;
            AstNode *type;
            AstNode *init;       /* AST_EXPR_* */
            bool is_pub;
        } global_decl;

        /* field_decl / param / enum_member / field_init: name + value */
        struct {
            char *name;
            AstNode *type;       /* field/param: AST_TYPE_*; NULL for enum member/field_init */
            AstNode *value;      /* enum member: optional AST_EXPR_* (NULL implicit);
                                  * field_init: AST_EXPR_* value; NULL otherwise */
        } named;

        /* block (statements) */
        struct {
            AstNode **items;
            size_t count;
        } list;

        /* switch */
        struct {
            AstNode *selector;
            AstNode **cases;     /* AST_CASE_CLAUSE / AST_DEFAULT_CLAUSE */
            size_t ncases;
        } switch_stmt;

        /* local var/const decl */
        struct {
            char *name;
            AstNode *type;
            AstNode *init;
        } local_decl;

        /* if / ternary */
        struct {
            AstNode *cond;
            AstNode *then;
            AstNode *els;        /* NULL when absent */
        } branch;

        /* while */
        struct { AstNode *cond; AstNode *body; } while_loop;

        /* for */
        struct {
            AstNode *init;       /* NULL when absent; AST_VAR_DECL/AST_CONST_DECL or expr */
            AstNode *cond;       /* NULL when absent */
            AstNode *step;       /* NULL when absent */
            AstNode *body;
        } for_loop;

        /* case_clause: value + body; default_clause: body only */
        struct {
            AstNode *value;      /* case value; NULL for default */
            AstNode *body;
        } clause;

        /* return: optional value */
        struct { AstNode *value; } ret;

        /* expr_stmt */
        struct { AstNode *expr; } expr_stmt;

        /* type_prim */
        struct { AstPrimKind prim; } type_prim;

        /* type_named */
        struct { AstName *name; } type_named;

        /* type_ptr / type_slice: base; type_array: base + len */
        struct {
            AstNode *base;
            AstNode *len;        /* AST_TYPE_ARRAY only; AST_EXPR_* const-expr */
        } type_derived;

        /* int literal */
        struct {
            LexIntType type;
            uint64_t value;
            bool is_min;
        } int_literal;

        /* str literal */
        struct {
            char *bytes;         /* decoded bytes, owned; may contain NULs */
            size_t len;
        } str_literal;

        /* bool literal */
        struct { bool value; } bool_literal;

        /* ident */
        struct { char *name; } ident;

        /* array literal: repeat form has exactly one element + count */
        struct {
            AstNode **elems;
            size_t nelems;
            AstNode *count;      /* non-NULL only for repeat form */
        } array_literal;

        /* paren */
        struct { AstNode *expr; } paren;

        /* unary */
        struct {
            AstUnaryOp op;
            AstNode *operand;
        } unary;

        /* binary */
        struct {
            AstBinaryOp op;
            AstNode *lhs;
            AstNode *rhs;
        } binary;

        /* assign */
        struct {
            AstAssignOp op;
            AstNode *target;
            AstNode *value;
        } assign;

        /* index / slice */
        struct {
            AstNode *base;
            AstNode *index;      /* AST_EXPR_INDEX only */
            AstNode *lo;         /* AST_EXPR_SLICE only; NULL when absent */
            AstNode *hi;         /* AST_EXPR_SLICE only; NULL when absent */
        } index_slice;

        /* call */
        struct {
            AstNode *callee;
            AstNode **args;
            size_t nargs;
        } call;

        /* member / arrow */
        struct {
            AstNode *base;
            char *name;
        } member;

        /* struct_init postfix: base + field inits */
        struct {
            AstNode *base;
            AstNode **fields;    /* AST_FIELD_INIT */
            size_t nfields;
        } struct_init;

        /* sizeof type/expr, alignof: one child */
        struct {
            AstNode *operand;    /* type or expr depending on kind */
        } size_op;

        /* cast/wrap: type + expr */
        struct {
            AstNode *type;
            AstNode *expr;
        } cast_wrap;
    } u;
} AstNode;

/* ---------------------------------------------------------------------------
 * Ownership
 * ------------------------------------------------------------------------- */

/* Free a node and its entire subtree (children arrays, names, strings,
 * spans). NULL is accepted. */
void ast_node_free(AstNode *node);

/* Create a name with the given parts; each part is copied. Returns NULL on
 * allocation failure. `parts` may be NULL when count == 0. */
AstName *ast_name_new(const char *const *parts, size_t count);

/* Free a name (NULL accepted). */
void ast_name_free(AstName *name);

/* Render a name as a dotted string ("a.b.c"); returns a heap string (caller
 * frees) or NULL on allocation failure. An empty name renders as "". */
char *ast_name_to_string(const AstName *name);

/* Primitive type kind from a lex keyword, or -1 when the keyword is not a
 * primitive type keyword (void/bool/str/int types). */
int ast_prim_from_keyword(LexKeyword kw);

/* ---------------------------------------------------------------------------
 * Deterministic dump
 * ------------------------------------------------------------------------- */

/* Growable byte buffer used by the dump (mirrors DiagBuf conventions). */
typedef struct AstDumpBuf {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} AstDumpBuf;

void ast_dump_init(AstDumpBuf *buf);
void ast_dump_free(AstDumpBuf *buf);
bool ast_dump_ok(const AstDumpBuf *buf);

/* Append the deterministic text dump of `node` (whole tree from program
 * down). One line per node: indentation, field label, node kind, key
 * attributes, and the source span [start_offset,end_offset). Deterministic
 * for a given tree: no addresses, no host paths, no timestamps. Returns
 * false only on OOM. */
bool ast_dump(const AstNode *node, AstDumpBuf *out);

#endif /* AICO_BOOTSTRAP_SRC_AST_AST_H */
