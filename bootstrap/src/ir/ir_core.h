/* bootstrap/src/ir/ir_core.h
 *
 * AI-Co Stage-0 canonical IR node model and invariants (WP-M0-16b1).
 *
 * Implements the IR node model of the accepted canonical IR contract
 * (docs/contracts/IR-CONTRACT-2026-08-12.md, v0.1.0, WP-M0-16a) over the
 * closed node-kind set of contract sec. 5 (module structure, declarations,
 * statements, value-producing instructions), the type descriptor model of
 * sec. 4.4, the constant model of sec. 4.5, the cause-chain model of
 * sec. 8, and the invariant verification of sec. 10 (violations reported
 * AIC-I0501, phase "ir").
 *
 * Scope (manifest WP-M0-16b1): the node model and invariant enforcement
 * only. The deterministic dump / round-trip verification is WP-M0-16b2
 * (bootstrap/src/ir/ir_dump.*); the typed-AST -> IR builder is WP-M0-16c.
 * This package is deliberately self-contained: it depends only on the
 * WP-M0-06 diagnostic package (spans, records, code registry) and the C
 * standard library. It does not depend on the AST, name, type, or const
 * packages; cause links carry AST construct-kind names as opaque strings
 * supplied by the builder (WP-M0-16c2).
 *
 * Determinism conventions (documented implementation detail within the
 * contract):
 *  - Node ids are assigned at construction from a single per-build counter
 *    (contract sec. 6.1: canonical construction order). The i-th node
 *    allocated in a build therefore has id == i, and ids are unique and
 *    gapless; the dump (16b2) iterates nodes in id order.
 *  - Base types (void, bool, the ten integer types, str; spec sec. 7.1)
 *    are interned at build creation in the spec table order (ids 0..12).
 *    Composite types (array, slice, pointer, struct, enum) intern on first
 *    use by structural key; the intern table order is first-occurrence in
 *    canonical construction order (contract sec. 6.3).
 *  - Constants are interned by value (contract sec. 6.4): identical
 *    constants share one IrConst; the representative is the first
 *    occurrence in canonical order. Struct/array item lists reference
 *    interned item constants, so pointer identity implies value identity.
 *  - Spans are copied at construction (the AST rule: owned spans); cause
 *    links copy their construct-kind string and span.
 *
 * Invariant enforcement (contract sec. 10; violations -> AIC-I0501):
 *   ir_core_verify checks invariants 1-11 at verification time over the
 *   constructed graph. Invariant 12 (byte round-trip through the
 *   deterministic dump) is owned by WP-M0-16b2; this package enforces the
 *   graph-side determinism preconditions (unique gapless ids, interned
 *   types/constants). The check list is closed for the contract; the
 *   per-kind typing and trap tables below are implementation details of
 *   this package within the contract.
 *
 * Verification returns records sorted with the DIAGNOSTIC-CONTRACT sec. 9
 * comparator (diag_sort_records). Each violation is one AIC-I0501 record:
 *   - message: "IR invariant violation (invariant <N>, <name>): <detail>";
 *   - primary span: the violating node's primary span (the derived span of
 *     DIAGNOSTIC-CONTRACT sec. 11.6); violations of interned type/constant
 *     tables have no node and carry a null span (they sort before
 *     file-bearing records per the contract sec. 9 rule);
 *   - recovery "authoritative" (the invariant violation is the root cause
 *     of the compiler-internal failure);
 *   - related facts: invariant number, node id, node kind.
 *
 * Ownership:
 *   - IrBuild owns everything reachable: nodes (in build->nodes), interned
 *     types (build->types), interned constants (build->consts). ir_build_free
 *     releases the whole build. No node/type/const is freed individually.
 *   - ir_core_verify borrows the build and returns an owned record array
 *     (freed with ir_records_free) on IR_VIOLATION; on IR_OOM nothing is
 *     owned and any partially accumulated records are released.
 *   - Node payload arrays (module imports/decls, block statements, call
 *     arguments) are grown by the add helpers below; callers attach the
 *     child node pointers (borrowed by the parent, owned by the build).
 *     Strings and spans passed to constructors/helpers are copied.
 */
#ifndef AICO_BOOTSTRAP_SRC_IR_IR_CORE_H
#define AICO_BOOTSTRAP_SRC_IR_IR_CORE_H

#include "../diag/diag.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations so constructors can be declared before the full
 * struct definitions. */
typedef struct IrBuild IrBuild;
typedef struct IrNode IrNode;
typedef struct IrConst IrConst;

/* ---------------------------------------------------------------------------
 * Node kinds: the closed instruction set (contract sec. 4.1-4.2, 5.2, 5.3).
 * No other kind may be added by an implementation package; adding one is an
 * architecture change (contract sec. 5.5).
 * ------------------------------------------------------------------------- */

typedef enum IrNodeKind {
    /* module structure (sec. 4.1-4.2) */
    IR_MODULE = 0,
    IR_IMPORT,
    IR_STRUCT_DECL,
    IR_ENUM_DECL,
    IR_GLOBAL_CONST,
    IR_GLOBAL_VAR,
    IR_FUNCTION,
    /* statements (sec. 5.2) */
    IR_BLOCK,
    IR_LOCAL_DECL,
    IR_IF,
    IR_WHILE,
    IR_FOR,
    IR_SWITCH,
    IR_CASE,
    IR_DEFAULT,
    IR_BREAK,
    IR_CONTINUE,
    IR_RETURN,
    IR_EXPR_STMT,
    IR_EMPTY,
    IR_CALL_TERM,
    IR_TRAP,
    /* value-producing instructions (sec. 5.3) */
    IR_INT,
    IR_BOOL,
    IR_NULL,
    IR_STR,
    IR_ENUM_VAL,
    IR_LOCAL,
    IR_GLOBAL,
    IR_FIELD_ADDR,
    IR_INDEX_ADDR,
    IR_DEREF,
    IR_LOAD,
    IR_STORE,
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_NEG,
    IR_SHL, IR_SHR,
    IR_BAND, IR_BOR, IR_BXOR, IR_BNOT,
    IR_LNOT,
    IR_LAND, IR_LOR,
    IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE,
    IR_SLICE_EQ,
    IR_SELECT,
    IR_CALL,
    IR_LEN, IR_PTR,
    IR_SLICE,
    IR_CAST, IR_WRAP,
    IR_PTR_ADD, IR_PTR_SUB, IR_PTR_DIFF,
    IR_ZERO
} IrNodeKind;

/* Stable kind name for messages and tests ("IR_MODULE", "IR_ADD", ...). */
const char *ir_kind_text(IrNodeKind kind);

/* ---------------------------------------------------------------------------
 * Type descriptors (contract sec. 4.4). Sizes/alignments are the spec's
 * fixed facts (language facts, never target choices).
 * ------------------------------------------------------------------------- */

typedef enum IrTypeKind {
    IRT_VOID = 0,
    IRT_BOOL,
    IRT_I8, IRT_I16, IRT_I32, IRT_I64,
    IRT_U8, IRT_U16, IRT_U32, IRT_U64,
    IRT_ISIZE, IRT_USIZE,
    IRT_STR,
    IRT_ARRAY,       /* T[N] */
    IRT_SLICE,       /* T[] */
    IRT_PTR,         /* T* */
    IRT_STRUCT,      /* named struct (declaration ref) */
    IRT_ENUM         /* named enum (declaration ref) */
} IrTypeKind;

typedef struct IrType {
    IrTypeKind kind;
    int64_t id;          /* deterministic intern order id (base types 0..12) */
    int64_t size;        /* bytes (spec sec. 7.1-7.5 facts) */
    int64_t align;       /* bytes */
    union {
        struct { struct IrType *elem; int64_t extent; } array;   /* IRT_ARRAY */
        struct { struct IrType *elem; } slice;                   /* IRT_SLICE */
        struct { struct IrType *elem; } ptr;                     /* IRT_PTR */
        struct IrNode *decl;   /* IR_STRUCT_DECL / IR_ENUM_DECL (borrowed) */
    } u;
} IrType;

/* Base-type accessors: return the interned descriptor (never NULL, never
 * fail: base types are pre-interned at build creation). */
IrType *ir_type_void(IrBuild *b);
IrType *ir_type_bool(IrBuild *b);
IrType *ir_type_i8(IrBuild *b);
IrType *ir_type_i16(IrBuild *b);
IrType *ir_type_i32(IrBuild *b);
IrType *ir_type_i64(IrBuild *b);
IrType *ir_type_u8(IrBuild *b);
IrType *ir_type_u16(IrBuild *b);
IrType *ir_type_u32(IrBuild *b);
IrType *ir_type_u64(IrBuild *b);
IrType *ir_type_isize(IrBuild *b);
IrType *ir_type_usize(IrBuild *b);
IrType *ir_type_str(IrBuild *b);

/* Composite type constructors: intern by structural key (contract sec. 6.3);
 * return the canonical descriptor, or NULL on allocation failure. `elem`
 * must be an interned type of this build; `decl` must be an IR_STRUCT_DECL /
 * IR_ENUM_DECL node of this build (borrowed). */
IrType *ir_type_array(IrBuild *b, IrType *elem, int64_t extent);
IrType *ir_type_slice(IrBuild *b, IrType *elem);
IrType *ir_type_ptr(IrBuild *b, IrType *elem);
IrType *ir_type_struct(IrBuild *b, IrNode *decl);
IrType *ir_type_enum(IrBuild *b, IrNode *decl);

/* Structural type identity (spec sec. 7.3): same kind; composites identical
 * element/extent; struct/enum identical iff same declaration node. NULL ==
 * NULL is true. */
bool ir_type_identical(const IrType *a, const IrType *b);

const char *ir_type_kind_text(IrTypeKind kind);

/* ---------------------------------------------------------------------------
 * Constants (contract sec. 4.5): interned by value, owned by the build.
 * ------------------------------------------------------------------------- */

typedef enum IrConstKind {
    IRC_INT = 0,
    IRC_BOOL,
    IRC_NULL,
    IRC_STR,
    IRC_ENUM,
    IRC_STRUCT,
    IRC_ARRAY,
    IRC_ADDR
} IrConstKind;

struct IrConst {
    IrConstKind kind;
    int64_t id;              /* first-occurrence intern order id */
    struct IrType *type;     /* the constant's type (borrowed interned) */
    union {
        uint64_t int_bits;             /* exact bit pattern, two's complement
                                        * (sec. 6.4) */
        bool b;                        /* IRC_BOOL */
        struct { uint8_t *bytes; size_t len; } str;   /* owned copy */
        struct { uint64_t value; struct IrNode *enum_decl; } en;  /* IRC_ENUM */
        struct { struct IrConst **items; size_t count; } strukt;  /* IRC_STRUCT */
        struct { struct IrConst **items; size_t count; } arr;     /* IRC_ARRAY */
        struct { struct IrNode *target; int64_t offset; } addr;   /* IRC_ADDR */
    } u;
};

/* Constant constructors (interned): return the first-occurrence
 * representative, or NULL on allocation failure. Item arrays for
 * IRC_STRUCT/IRC_ARRAY must contain interned constants of this build
 * (borrowed; the constructor copies the pointer array). */
IrConst *ir_const_int(IrBuild *b, IrType *type, uint64_t bits);
IrConst *ir_const_bool(IrBuild *b, bool value);
IrConst *ir_const_null(IrBuild *b, IrType *ptr_type);
IrConst *ir_const_str(IrBuild *b, const uint8_t *bytes, size_t len);
IrConst *ir_const_enum(IrBuild *b, IrType *enum_type, uint64_t value);
IrConst *ir_const_struct(IrBuild *b, IrType *struct_type,
                         IrConst **items, size_t count);
IrConst *ir_const_array(IrBuild *b, IrType *array_type,
                        IrConst **items, size_t count);
IrConst *ir_const_addr(IrBuild *b, IrType *ptr_type, IrNode *target,
                       int64_t offset);

const char *ir_const_kind_text(IrConstKind kind);

/* ---------------------------------------------------------------------------
 * Cause chain (contract sec. 8): ordered root-cause-first links. Each link
 * records the source construct kind (AST node kind as an opaque string),
 * that construct's primary span, and resolved-reference facts (declaration
 * node id / type id / constant id, or -1 when none). The chain terminates at
 * the module root (the first link's span belongs to the node's module file).
 * ------------------------------------------------------------------------- */

typedef struct IrCauseLink {
    char *construct_kind;    /* owned copy, e.g. "AST_EXPR_BINARY" */
    DiagSpan *span;          /* owned; the construct's primary span */
    int64_t ref_decl;        /* declaration node id, or -1 */
    int64_t ref_type;        /* type id, or -1 */
    int64_t ref_const;       /* constant id, or -1 */
} IrCauseLink;

/* ---------------------------------------------------------------------------
 * Fields / members / params / slots (owned by their parent node).
 * ------------------------------------------------------------------------- */

typedef struct IrField {
    char *name;              /* owned */
    struct IrType *type;     /* borrowed interned; never void */
    DiagSpan *span;          /* owned */
    int64_t byte_offset;     /* layout fact (spec sec. 7.4) */
} IrField;

typedef struct IrEnumMember {
    char *name;              /* owned */
    int64_t value;           /* underlying integer value (resolved) */
    DiagSpan *span;          /* owned */
} IrEnumMember;

typedef struct IrParam {
    char *name;              /* owned */
    struct IrType *type;     /* borrowed interned */
    int64_t slot_index;      /* position in the function slot table */
    DiagSpan *span;          /* owned */
} IrParam;

/* Storage model (contract sec. 4.3): per-function slot table; parameter
 * slots first (in parameter order), then local slots in first-declaration
 * order, then compiler temporaries. */
typedef enum IrSlotKind {
    IR_SLOT_PARAM = 0,
    IR_SLOT_LOCAL,
    IR_SLOT_TEMP
} IrSlotKind;

typedef struct IrSlot {
    int64_t index;           /* position in the function slot table */
    IrSlotKind kind;
    char *name;              /* owned, or NULL for temporaries */
    struct IrType *type;     /* borrowed interned; never void */
    DiagSpan *span;          /* owned */
} IrSlot;

/* ---------------------------------------------------------------------------
 * The IR node. Every node carries: kind, deterministic id, owned primary
 * span (never NULL on constructed nodes), owned cause chain, result type
 * (value-producing nodes; NULL otherwise; void only for IR_CALL), and the
 * declared registry trap code (nodes with a runtime failure mode only;
 * borrowed literal, validated against the registry by ir_core_verify).
 * ------------------------------------------------------------------------- */

struct IrNode {
    IrNodeKind kind;
    int64_t id;
    DiagSpan *span;          /* owned; never NULL on constructed nodes */
    IrCauseLink *causes;     /* owned array; root cause first */
    size_t cause_count;
    struct IrType *type;     /* result type (see above) */
    const char *trap_code;   /* declared registry trap code, or NULL */
    union {
        /* module structure (sec. 4.1-4.2) */
        struct {
            char *name;              /* fully qualified name */
            struct IrNode **imports; /* IR_IMPORT */
            size_t nimports;
            struct IrNode **decls;   /* top-level declarations, source order */
            size_t ndecls;
        } module;
        struct { char *name; } import;
        struct {
            char *name;
            IrField *fields;         /* declaration order */
            size_t nfields;
            int64_t size;            /* layout facts */
            int64_t align;
        } struct_decl;
        struct {
            char *name;
            struct IrType *underlying; /* integer type (spec sec. 7.5) */
            IrEnumMember *members;     /* declaration order */
            size_t nmembers;
        } enum_decl;
        struct { char *name; struct IrType *type; struct IrConst *value; } global_const;
        struct { char *name; struct IrType *type; struct IrConst *init; } global_var;
        struct {
            char *name;
            struct IrType *ret_type;   /* may be void */
            IrParam *params;           /* parameter order */
            size_t nparams;
            IrSlot **slots;            /* param slots first, then locals/temps */
            size_t nslots;
            struct IrNode *body;       /* IR_BLOCK */
            bool noreturn;             /* only rt.proc.exit / rt.trap.report */
        } function;
        /* statements (sec. 5.2) */
        struct { struct IrNode **stmts; size_t nstmts; } block;
        struct { int64_t slot_index; struct IrNode *init; } local_decl;
        struct { struct IrNode *cond; struct IrNode *then_block;
                 struct IrNode *else_block; } if_stmt;
        struct { struct IrNode *cond; struct IrNode *body; } while_stmt;
        struct { struct IrNode *init; struct IrNode *cond;
                 struct IrNode *step; struct IrNode *body; } for_stmt;
        struct { struct IrNode *selector; struct IrNode **cases; size_t ncases;
                 struct IrNode *default_clause; } switch_stmt;
        struct { struct IrConst *value; struct IrNode *body; } case_clause;
        struct { struct IrNode *body; } default_clause;
        struct { struct IrNode *target; } break_stmt;
        struct { struct IrNode *target; } continue_stmt;
        struct { struct IrNode *value; } return_stmt;
        struct { struct IrNode *expr; } expr_stmt;
        struct { unsigned char unused; } empty_stmt;   /* no payload; span
                                                        * preserved */
        struct { struct IrNode *callee; struct IrNode **args; size_t nargs; } call_term;
        struct { const char *code; int64_t user_code; bool has_user_code; } trap;
        /* value-producing nodes (sec. 5.3) */
        struct { struct IrConst *value; } constant;   /* IR_INT/IR_BOOL/IR_STR/IR_ENUM_VAL */
        struct { int64_t slot_index; } local;         /* IR_LOCAL */
        struct { struct IrNode *target; } global;     /* IR_GLOBAL: ref to IR_GLOBAL_VAR */
        struct { struct IrNode *base; int64_t field_index; } field_addr;
        struct { struct IrNode *base; struct IrNode *index; } index_addr;
        struct { struct IrNode *ptr; } deref;
        struct { struct IrNode *lvalue; } load;
        struct { struct IrNode *dest; struct IrNode *value; } store;
        struct { struct IrNode *left; struct IrNode *right; } binary;
        struct { struct IrNode *operand; } unary;
        struct { struct IrNode *cond; struct IrNode *then_value;
                 struct IrNode *else_value; } select;
        struct { struct IrNode *callee; struct IrNode **args; size_t nargs; } call;
        struct { struct IrNode *base; struct IrNode *start; struct IrNode *end; } slice;
        struct { struct IrNode *value; } cast_wrap;   /* IR_CAST / IR_WRAP */
        struct { struct IrNode *ptr; struct IrNode *offset; } ptr_arith; /* IR_PTR_ADD/SUB */
    } u;
};

/* ---------------------------------------------------------------------------
 * Build root
 * ------------------------------------------------------------------------- */

typedef enum IrStatus {
    IR_OK = 0,       /* verification passed; no records */
    IR_VIOLATION,    /* one or more AIC-I0501 records produced */
    IR_OOM           /* allocation failure; nothing owned */
} IrStatus;

struct IrBuild {
    IrNode **modules;        /* IR_MODULE in canonical order */
    size_t nmodules;
    IrType **types;          /* interned descriptors; base types first (0..12) */
    size_t ntypes;
    IrConst **consts;        /* interned constants, first-occurrence order */
    size_t nconsts;
    IrNode **nodes;          /* every allocated node, in id order (id == index) */
    size_t nnodes;
    int64_t next_id;
    bool oom;                /* sticky allocation-failure flag */
};

IrBuild *ir_build_new(void);      /* NULL on allocation failure */
void ir_build_free(IrBuild *b);   /* releases the whole build (NULL accepted) */

/* Append one module unit in canonical order (entry module first, then
 * imports depth-first in import order). */
void ir_build_add_module(IrBuild *b, IrNode *module);

/* Allocate a node with the next deterministic id and a copied primary span;
 * appends it to the build (unattached nodes are caught by invariant 1 as
 * unreachable). Returns NULL on allocation failure (build->oom set). */
IrNode *ir_node_new(IrBuild *b, IrNodeKind kind, const DiagSpan *span);

/* Append one root-cause-first cause link (copies kind string and span). */
void ir_node_add_cause(IrBuild *b, IrNode *node, const char *construct_kind,
                       const DiagSpan *span,
                       int64_t ref_decl, int64_t ref_type, int64_t ref_const);

/* Growable child-array helpers (realloc; build->oom on failure). */
void ir_module_add_import(IrBuild *b, IrNode *module, IrNode *import);
void ir_module_add_decl(IrBuild *b, IrNode *module, IrNode *decl);
void ir_block_add_stmt(IrBuild *b, IrNode *block, IrNode *stmt);
void ir_call_add_arg(IrBuild *b, IrNode *call, IrNode *arg);
void ir_call_term_add_arg(IrBuild *b, IrNode *call_term, IrNode *arg);

/* ---------------------------------------------------------------------------
 * Invariant verification (contract sec. 10)
 * ------------------------------------------------------------------------- */

/* Verify the whole build against the invariant list; violations are
 * reported as AIC-I0501 records (phase "ir", severity "error", recovery
 * "authoritative", primary span = the violating node's span, related facts
 * invariant/node_id/node_kind), returned sorted with the contract sec. 9
 * comparator. On IR_VIOLATION *out_records / *out_record_count are owned by
 * the caller (ir_records_free). On IR_OOM nothing is owned. */
IrStatus ir_core_verify(const IrBuild *build,
                        DiagRecord ***out_records, size_t *out_record_count);

void ir_records_free(DiagRecord **records, size_t count);

#endif /* AICO_BOOTSTRAP_SRC_IR_IR_CORE_H */
