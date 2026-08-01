#pragma once
#include "diagnostics.h"
#include "lexer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
    TYPE_UNSUPPORTED, /* unsupported | count */
} TypeKind;

typedef struct Type Type;
typedef struct Type {
    TypeKind kind;
    uint16_t bits;
    union {
        struct {
            bool is_unsigned;
        } int_type;
        struct {
            bool is_fat;
            Type* elem_type;
        } ptr_type;
        struct {
            Type* elem_type;
            size_t len;
        } array_type;
    };
} Type;

Type* type_number(TypeKind k, uint16_t bits, bool is_unsigned);
Type* type_void(void);
Type* type_ptr(Type* elem, bool is_fat);
Type* type_array(Type* elem, size_t len); /* len=0 => unsized [T] */

typedef struct {
    char* name;
    Type* type;
} Param;

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
    AST_VAR_ASSIGN,  /* name = expr;                      */

    AST_INT_LIT,     /* 42, 42u8, 42i32                   */
    AST_FLOAT_LIT,   /* 3.14, 3.14f32                     */
    AST_STRING_LIT,  /* "hello"                           */
    AST_ARRAY_LIT,   /* [1, 2, 3]                         */
    AST_TYPE_LIT,    /* i32, u64, f32                     */
    AST_IDENT,       /* foo                               */
    AST_CALL,        /* foo(args...)                      */
    AST_BINOP,       /* lhs OP rhs                        */
    AST_UNOP,        /* OP operand                        */
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

typedef struct AstNode AstNode;

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
            bool     is_extern;
            AstNode* body; /* NULL = decl */
        };

        /* AST_VAR_DECL, AST_CONST_DECL */
        struct {
            char*    var_name;
            Type*    var_type;
            AstNode* init; /* NULL = no initializer */
            bool     is_thread_local;
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

        /* AST_VAR_ASSIGN */
        struct {
            char*    assign_name;
            AssignOp assign_op;
            AstNode* assign_value;
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
            char*  str;
            size_t len;
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

        /* AST_CALL */
        struct {
            char*     callee;
            AstNode** args;
            int       arg_count;
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
    };
};

typedef struct {
    AstNode** decls;
    int       count;
} Module;

AstNode* ast_node(AstKind kind, Location loc);
void     ast_dump(Module* m);
