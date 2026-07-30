#pragma once
#include "diagnostics.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TYPE_VOID,
    TYPE_INT,
    TYPE_FLOAT,
} TypeKind;

typedef struct Type {
    TypeKind kind;
    uint16_t bits;
    int      is_unsigned;
} Type;

Type* type_new(TypeKind k, int bits, int is_unsigned);
Type* type_void(void);

typedef struct {
    char* name;
    Type* type;
} Param;

typedef enum {
    AST_FUNC_DECL,   /* name :: (params) -> ret { body }  */
    AST_EXTERN_DECL, /* extern name(params) -> ret        */

    AST_RETURN_STMT, /* return [expr];                    */
    AST_EXPR_STMT,   /* expr;                             */
    AST_BLOCK_STMT,  /* { stmts }                         */
    AST_IF_STMT,     /* if (cond) then [else]             */
    AST_WHILE_STMT,  /* while (cond) body                 */

    AST_INT_LIT,     /* 42, 42u8, 42i32                   */
    AST_FLOAT_LIT,   /* 3.14, 3.14f32                     */
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
} BinOp;

typedef enum {
    UOP_NEG,
    UOP_POS,
} UnOp;

typedef struct AstNode AstNode;

struct AstNode {
    AstKind  kind;
    Location loc;
    Type*    type;
    union {
        /* AST_FUNC_DECL, AST_EXTERN_DECL */
        struct {
            char*    name;
            Param*   params;
            int      param_count;
            Type*    ret_type;
            bool     is_extern;
            AstNode* body; /* NULL = decl */
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

        /* AST_INT_LIT */
        struct {
            int64_t ival;
        };

        /* AST_FLOAT_LIT */
        struct {
            double fval;
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
