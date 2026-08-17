#pragma once
#include "diagnostics.h"
#include "lexer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* void */
    TYPE_VOID = 0,
    /* i<bits>, u<bits>, s<bits> */
    /* i, s => signed; u => unsiged */
    TYPE_INT,
    /* f<bits> */
    TYPE_FLOAT,
    /* *T, []T */
    /* []T => fat */
    TYPE_PTR,
    /* [T], [T; N] */
    TYPE_ARRAY,
    /* unresolved identifier (const reference) */
    TYPE_IDENT,
    /* !, no return */
    TYPE_NEVER,
    /* unsupported | count */
    TYPE_UNSUPPORTED,
} TypeKind;

typedef struct Type Type;
typedef struct Type {
    TypeKind kind;
    uint16_t bits;
    union {
        struct {
            unsigned is_unsigned : 1;
            unsigned is_abstract : 1;
            unsigned is_size     : 1;
        } int_type;
        struct {
            unsigned is_fat   : 1;
            unsigned non_null : 1;
            Type*    elem_type;
        } ptr_type;
        struct {
            Type*  elem_type;
            size_t len;
        } array_type;
        struct {
            char* name;
        } ident_type;
    };
} Type;

Type* type_number(TypeKind k, uint16_t bits, bool is_unsigned);
Type* type_number_with_flag(TypeKind k, uint16_t bits, bool is_unsigned, bool is_abstract, bool is_size);
Type* type_void(void);
Type* type_never(void);
Type* type_ptr(Type* elem, bool is_fat, bool non_null);
Type* type_array(Type* elem, size_t len);
Type* type_abstract_int(void);
Type* type_abstract_float(void);
Type* type_ident(const char* name);

typedef enum {
    AST_FUNC_DECL,   /* name :: (params) -> ret { body }  */
    AST_EXTERN_DECL, /* extern name(params) -> ret        */
    AST_VAR_DECL,    /* name: type = init;                */
    AST_CONST_DECL,  /* name :: type = init;              */

    AST_RETURN_STMT, /* return [expr];                    */
    AST_EXPR_STMT,   /* expr;                             */
    AST_BLOCK_STMT,  /* { stmts }                         */
    AST_IF_STMT,     /* if (cond) then [else]             */
    AST_WHILE_STMT,  /* while (cond) body                 */
    AST_FOR_STMT,    /* for val: iterable body            */
    AST_ASSIGN,      /* target = expr;                    */
    AST_ANNOTATION,  /* #annot(args)                      */

    AST_INT_LIT,     /* 42, 42u8, 42i32                   */
    AST_FLOAT_LIT,   /* 3.14, 3.14f32                     */
    AST_STRING_LIT,  /* "hello"                           */
    AST_ARRAY_LIT,   /* [1, 2, 3]                         */
    AST_NULLPTR,     /* nullptr                           */
    AST_TYPE_LIT,    /* i32, u64, f32                     */
    AST_IDENT,       /* foo                               */
    AST_INDEX,       /* arr[idx]                          */
    AST_MEMBER,      /* value.member                      */
    AST_RANGE,       /* start..end:step                   */
    AST_CALL,        /* foo(args...)                      */
    AST_BINOP,       /* lhs OP rhs                        */
    AST_UNOP,        /* OP operand                        */
    AST_CAST,        /* expr as Type                      */
} AstKind;

typedef enum {
    OP_ADD,
    OP_SUB,
    OP_EQ,
    OP_LESS,
    OP_MORE,
    OP_LESSEQ,
    OP_MOREEQ,
    OP_MOD,
    OP_DIV,
    OP_MUL,
    OP_NEQ,
    OP_BITAND,
    OP_BITOR,
    OP_BITXOR,
    OP_SHL,
    OP_SHR,
    OP_LAND,
    OP_LOR,
    OP_POW,
} BinOp;

typedef enum {
    UOP_NEG,
    UOP_POS,
    UOP_NOT,
    UOP_BITNOT,
    UOP_PREINC,
    UOP_PREDEC,
    UOP_POSTINC,
    UOP_POSTDEC,
    UOP_DEREF,
    UOP_ADDR,
} UnOp;

typedef enum {
    ASSIGN_EQ,
    ASSIGN_ADDEQ,
    ASSIGN_SUBEQ,
    ASSIGN_MULEQ,
    ASSIGN_DIVEQ,
    ASSIGN_MODEQ,
    ASSIGN_BITANDEQ,
    ASSIGN_BITOREQ,
    ASSIGN_BITXOREQ,
    ASSIGN_SHLEQ,
    ASSIGN_SHREQ, /* shrek */
    ASSIGN_LANDEQ,
    ASSIGN_LOREQ,
    ASSIGN_POWEQ,
} AssignOp;

/* builtin => no target, annot => whatever is after it is the target */
typedef enum {
    BUILTIN_SIZEOF,      /* #sizeof(T) || #sizeof(expr) */
    BUILTIN_ALIGNOF,     /* #alignof(T) || #alignof(expr) */
    BUILTIN_OFFSETOF,    /* #offsetof(T, member) */

    ANNOT_NO_MANGLE,     /* #no_mangle <func decl> */
    ANNOT_PRINTF_LIKE,   /* #printf_like(fmt) || #printf_like("fmt") || #printf_like(0) */
    ANNOT_SCANF_LIKE,    /* #scanf_like(fmt) || #scanf_like("fmt") || #scanf_like(0) */
    ANNOT_STRFTIME_LIKE, /* #strftime_like(fmt) || #strftime_like("fmt") || #strftime_like(0) */
    ANNOT_DEPRECATED,    /* #deprecated <func decl> || #deprecated("message") <func decl> */
    ANNOT_INLINE,        /* #inline <func decl> || #inline(never) <func decl> || #inline(always) <func decl> */
    ANNOT_SENTINEL,      /* #sentinel <func decl> || #sentinel(const value) <func decl> */
    ANNOT_LINK_NAME,     /* #link_name(name) <func decl> || #link_name("name") <func decl> */
} AnnotationType;

typedef enum {
    FUNC_FLAG_EXTERN        = 1 << 0,
    FUNC_FLAG_VARIADIC      = 1 << 1,
    FUNC_FLAG_PRINTF_LIKE   = 1 << 2,
    FUNC_FLAG_SCANF_LIKE    = 1 << 3,
    FUNC_FLAG_STRFTIME_LIKE = 1 << 4,
    FUNC_FLAG_NO_MANGLE     = 1 << 5,
    FUNC_FLAG_DEPRECATED    = 1 << 6,
    FUNC_FLAG_SENTINEL      = 1 << 7,
    FUNC_FLAG_LINK_NAME     = 1 << 8,
} FuncFlags;

#define FUNC_IS_FLAG(fn, flag)  ((fn)->flags & (flag))
#define FUNC_SET_FLAG(fn, flag) ((fn)->flags |= (flag))

typedef struct AstNode AstNode;

typedef struct {
    char*    name;          /* NULL = unnamed */
    Type*    type;
    AstNode* default_value; /* NULL = no default value */
    Location loc;
} Param;

struct AstNode {
    AstKind  kind;
    Location loc;
    Type*    type;
    union {
        /* AST_FUNC_DECL, AST_EXTERN_DECL */
        struct {
            char*    function_name;
            Param*   params;
            int      param_count;
            Type*    ret_type;
            AstNode* body;           /* NULL = decl */
            uint16_t flags;          /* FuncFlags */
            uint8_t  inline_hint;    /* 0=none/hint, 1=never, 2=always */
            int      param_idx;      /* for printf_like, scanf_like, strftime_like */
            char*    deprecated_msg; /* NULL = no message */
            char*    link_name_str;  /* NULL = not set */
            AstNode* sentinel_value; /* sentinel value for #sentinel */
        };

        /* AST_VAR_DECL, AST_CONST_DECL */
        struct {
            char*    var_name;
            Type*    var_type; /* NULL = inferred type */
            AstNode* init;     /* NULL = no initializer */
        };

        /* AST_RETURN_STMT */
        struct {
            AstNode* ret_val;
        }; /* NULL = bare return        */

        /* AST_EXPR_STMT */
        struct {
            AstNode* expr;
        };

        /* AST_BLOCK_STMT */
        struct {
            AstNode** stmts;
            int       stmt_count;
        };

        /* AST_IF_STMT */
        struct {
            AstNode* if_cond;
            AstNode* then_branch;
            AstNode* else_branch; /* NULL = no else */
        };

        /* AST_WHILE_STMT */
        struct {
            AstNode* while_cond;
            AstNode* while_body;
        };

        /* AST_FOR_STMT */
        struct {
            AstNode* for_val;
            AstNode* for_iterable;
            AstNode* for_body;
        };

        /* AST_ASSIGN */
        struct {
            AstNode* assign_target; /* AST_IDENT or AST_INDEX */
            AssignOp assign_op;
            AstNode* assign_value;
        };

        /* AST_ANNOTATION */
        struct {
            AnnotationType annot_type;
            AstNode*       annot_target; /* NULL = no target */
            AstNode**      annot_exprs;  /* NULL = no exprs/couldnt parse */
            size_t         annot_expr_count;
            const char*    annot_str;
            unsigned       annot_arg_parse_failed : 1;
        };

        /* AST_INT_LIT */
        struct {
            int64_t ival;
        };

        /* AST_FLOAT_LIT */
        struct {
            double fval;
        };

        /* AST_STRING_LIT */
        struct {
            char*          str;
            size_t         len;
            StrPrefixFlags str_flags;
        };

        /* AST_ARRAY_LIT */
        struct {
            AstNode** elements;
            size_t    element_count;
        };

        /* AST_TYPE_LIT */
        struct {
            Type* typeval;
        };

        /* AST_IDENT */
        struct {
            char* ident;
        };

        /* AST_INDEX */
        struct {
            AstNode* array;
            AstNode* index;
        };

        /* AST_MEMBER */
        struct {
            AstNode* member_value;
            char*    member_name;
        };

        /* AST_RANGE */
        struct {
            AstNode* range_start;
            AstNode* range_end;
            AstNode* range_step; /* NULL = default step (eg 1) */
            enum {
                RANGE_INCLUSIVE,
                RANGE_END_EXCLUSIVE,
            } range_kind;
        };

        /* AST_CALL */
        struct {
            char*     callee;
            AstNode** args;
            int       arg_count;
            AstNode*  func_decl; /* semantic fills in */
        };

        /* AST_BINOP */
        struct {
            BinOp    op;
            AstNode* lhs;
            AstNode* rhs;
        };

        /* AST_UNOP */
        struct {
            UnOp     uop;
            AstNode* operand;
        };

        /* AST_CAST */
        struct {
            AstNode* cast_expr;
            Type*    cast_type; /* NULL = inferred type */
        };
    };
};

typedef struct {
    AstNode**   decls;
    int         count;
    const char* filepath;
} Module;

void     type_to_string(Type* t, char* buf, size_t buf_size);
int      type_bytes(Type* t);
AstNode* ast_node(AstKind kind, Location loc);
void     ast_dump(Module* m);
