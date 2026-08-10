/* bootstrap/src/types/type_identity.h
 *
 * AI-Co Stage-0 type representation and type identity (WP-M0-11a).
 *
 * Consumes the resolved name tables (WP-M0-10): a named struct/enum type
 * is represented by the NameSymbol for its declaration, so identity for
 * named types is "same declaration" (spec sec. 7.3), never name-based.
 *
 * The Type model covers spec sec. 7.1-7.3:
 *   - primitives (the 13 types of sec. 7.1; the kind enum is the AST's
 *     AstPrimKind so the parser's AST_TYPE_PRIM maps directly);
 *   - composite types (sec. 7.2): array T[N], slice T[], pointer T*,
 *     named struct, named enum.
 *
 * Identity (sec. 7.3), implemented by type_identical():
 *   - same primitive type;
 *   - same named struct/enum type (same declaration: the same NameSymbol
 *     instance, which the name package guarantees is unique per
 *     declaration within a build);
 *   - composite types with identical element type and identical extent
 *     (T[N] vs T[N] for the same constant N; T[] vs T[]; T* vs T*).
 *
 * There are no anonymous struct/enum types and no type aliases in the
 * minimal language (sec. 7.3), so no identity machinery is needed for
 * either.
 *
 * Layout computation (sizes/alignments/padding) is explicitly out of
 * scope: it belongs to WP-M0-11b (bootstrap/src/types/layout.*). This
 * package records the spec's primitive size/alignment facts as table data
 * (type_tables.h) but never derives a composite size.
 *
 * Ownership:
 *   - Type objects are heap-allocated. Composite constructors take
 *     ownership of their element Type pointer (transfer); type_free
 *     releases the whole graph. Callers must not free an element
 *     separately after passing it to a constructor.
 *   - The NameSymbol pointers stored for struct/enum types are borrowed
 *     (owned by the NameResult); type_free does not touch them.
 */
#ifndef AICO_BOOTSTRAP_SRC_TYPES_TYPE_IDENTITY_H
#define AICO_BOOTSTRAP_SRC_TYPES_TYPE_IDENTITY_H

#include "../ast/ast.h"
#include "../name/name.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Composite type forms (spec sec. 7.2). */
typedef enum TypeKind {
    TYPE_PRIM = 0,   /* one of the sec. 7.1 primitives (AstPrimKind) */
    TYPE_ARRAY,      /* T[N], fixed-size array of N elements of T */
    TYPE_SLICE,      /* T[], bounded view: data pointer + element count */
    TYPE_PTR,        /* T*, nullable raw pointer to T */
    TYPE_STRUCT,     /* named struct (same declaration) */
    TYPE_ENUM        /* named enum (same declaration) */
} TypeKind;

/* A resolved type descriptor. `sym` is non-NULL for TYPE_STRUCT/TYPE_ENUM
 * and is the declaration symbol from name resolution (borrowed). */
typedef struct Type {
    TypeKind kind;
    union {
        AstPrimKind prim;              /* TYPE_PRIM */
        struct {
            struct Type *elem;         /* owned */
            int64_t extent;            /* constant N (array length) */
        } array;                       /* TYPE_ARRAY */
        struct {
            struct Type *elem;         /* owned */
        } slice;                       /* TYPE_SLICE */
        struct {
            struct Type *elem;         /* owned */
        } ptr;                         /* TYPE_PTR */
        const NameSymbol *sym;         /* borrowed; TYPE_STRUCT/TYPE_ENUM */
    } u;
} Type;

/* Constructors. The composite constructors take ownership of `elem`
 * (transfer); `sym` is borrowed. Return NULL on allocation failure (the
 * caller retains ownership of `elem` in that case). */
Type *type_prim_new(AstPrimKind prim);
Type *type_array_new(Type *elem, int64_t extent);
Type *type_slice_new(Type *elem);
Type *type_ptr_new(Type *elem);
Type *type_struct_new(const NameSymbol *sym);
Type *type_enum_new(const NameSymbol *sym);

/* Free a type graph (NULL accepted). Element types are freed recursively;
 * NameSymbol pointers are borrowed and never freed. */
void type_free(Type *t);

/* Type identity (spec sec. 7.3): structural, never name-based for the
 * composite forms; named struct/enum types are identical iff they are the
 * same declaration (same NameSymbol). Returns true for NULL == NULL and
 * false when exactly one operand is NULL. */
bool type_identical(const Type *a, const Type *b);

/* Human-readable deterministic rendering for diagnostic messages, e.g.
 * "i32", "bool[4]", "u8[]", "Node*", "Node", "Color". Returns a heap
 * string (caller frees) or NULL on allocation failure. */
char *type_describe(const Type *t);

/* Kind name ("prim", "array", "slice", "ptr", "struct", "enum"). */
const char *type_kind_text(TypeKind kind);

#endif /* AICO_BOOTSTRAP_SRC_TYPES_TYPE_IDENTITY_H */
