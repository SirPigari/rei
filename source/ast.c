#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Type* type_new(TypeKind k, int bits, int is_unsigned) {
    Type* t        = calloc(1, sizeof(*t));
    t->kind        = k;
    t->bits        = bits;
    t->is_unsigned = is_unsigned;
    return t;
}

Type* type_void(void) {
    return type_new(TYPE_VOID, 0, 0);
}

AstNode* ast_node(AstKind kind, Location loc) {
    AstNode* n = calloc(1, sizeof(*n));
    n->kind    = kind;
    n->loc     = loc;
    return n;
}

static void print_type(Type* t) {
    if (!t) {
        printf("?");
        return;
    }
    switch (t->kind) {
        case TYPE_VOID:
            printf("void");
            return;
        case TYPE_INT:
            printf("%s%d", t->is_unsigned ? "u" : "i", t->bits ? t->bits : 64);
            return;
        case TYPE_FLOAT:
            printf("f%d", t->bits ? t->bits : 64);
            return;
    }
}

static const char* binop_str(BinOp op) {
    switch (op) {
        case OP_ADD:
            return "+";
        case OP_SUB:
            return "-";
        case OP_EQ:
            return "==";
        case OP_LESS:
            return "<";
        case OP_MORE:
            return ">";
    }
    return "?";
}

static const char* unop_str(UnOp op) {
    switch (op) {
        case UOP_NEG:
            return "-";
        case UOP_POS:
            return "+";
    }
    return "?";
}

static void dump_expr(AstNode* n, int ind);

static void dump_expr(AstNode* n, int ind) {
    if (!n) {
        printf("(null)");
        return;
    }
    switch (n->kind) {
        case AST_INT_LIT:
            printf("%lld", n->ival);
            break;
        case AST_FLOAT_LIT:
            printf("%g", n->fval);
            break;
        case AST_IDENT:
            printf("%s", n->ident);
            break;
        case AST_CALL:
            printf("%s(", n->callee);
            for (int i = 0; i < n->arg_count; i++) {
                if (i)
                    printf(", ");
                dump_expr(n->args[i], ind);
            }
            printf(")");
            break;
        case AST_BINOP:
            printf("(");
            dump_expr(n->lhs, ind);
            printf(" %s ", binop_str(n->op));
            dump_expr(n->rhs, ind);
            printf(")");
            break;
        case AST_UNOP:
            printf("(%s", unop_str(n->uop));
            dump_expr(n->operand, ind);
            printf(")");
            break;
        default:
            printf("?expr(%d)", n->kind);
            break;
    }
}

static void dump_stmt(AstNode* s, int ind) {
    printf("%*s", ind, "");
    switch (s->kind) {
        case AST_EXPR_STMT:
            dump_expr(s->expr, ind);
            printf(";\n");
            break;
        case AST_RETURN_STMT:
            printf("return");
            if (s->ret_val) {
                printf(" ");
                dump_expr(s->ret_val, ind);
            }
            printf(";\n");
            break;
        case AST_BLOCK_STMT:
            printf("{\n");
            for (int i = 0; i < s->stmt_count; i++) {
                dump_stmt(s->stmts[i], ind + 4);
            }
            printf("%*s}\n", ind, "");
            break;
        case AST_IF_STMT:
            printf("if (");
            dump_expr(s->if_cond, 0);
            printf(")\n");
            dump_stmt(s->then_branch, ind + 4);
            if (s->else_branch) {
                printf("else\n");
                dump_stmt(s->else_branch, ind + 4);
            }
            break;
        case AST_WHILE_STMT:
            printf("while (");
            dump_expr(s->while_cond, 0);
            printf(")\n");
            dump_stmt(s->while_body, ind + 4);
            break;
        default:
            printf("?stmt(%d)\n", s->kind);
            break;
    }
}

void ast_dump(Module* m) {
    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_EXTERN_DECL) {
            printf("extern %s(", d->name);
            for (int p = 0; p < d->param_count; p++) {
                if (p)
                    printf(", ");
                printf("%s: ", d->params[p].name);
                print_type(d->params[p].type);
            }
            printf(") -> ");
            print_type(d->ret_type);
            printf("\n");
        } else if (d->kind == AST_FUNC_DECL) {
            printf("%s :: (", d->name);
            for (int p = 0; p < d->param_count; p++) {
                if (p)
                    printf(", ");
                printf("%s: ", d->params[p].name);
                print_type(d->params[p].type);
            }
            printf(") -> ");
            print_type(d->ret_type);
            printf(" ");
            dump_stmt(d->body, 0);
            printf("\n");
        }
    }
}
