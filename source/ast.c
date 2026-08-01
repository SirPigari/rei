#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

Type* type_number(TypeKind k, uint16_t bits, bool is_unsigned) {
    Type* t        = calloc(1, sizeof(*t));
    t->kind        = k;
    t->bits        = bits;
    t->int_type.is_unsigned = is_unsigned;
    return t;
}

Type* type_void(void) {
    Type* t = calloc(1, sizeof(*t));
    t->kind = TYPE_VOID;
    return t;
}

Type* type_ptr(Type* elem, bool is_fat) {
    Type* t            = calloc(1, sizeof(*t));
    t->kind            = TYPE_PTR;
    t->ptr_type.is_fat = is_fat;
    t->ptr_type.elem_type = elem;
    return t;
}

Type* type_array(Type* elem, size_t len) {
    Type* t               = calloc(1, sizeof(*t));
    t->kind               = TYPE_ARRAY;
    t->array_type.elem_type = elem;
    t->array_type.len     = len;
    return t;
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
            printf("%s%d", t->int_type.is_unsigned ? "u" : "i", t->bits ? t->bits : 64);
            return;
        case TYPE_FLOAT:
            printf("f%d", t->bits ? t->bits : 64);
            return;
        case TYPE_PTR:
            if (t->ptr_type.is_fat)
                printf("[]");
            else
                printf("*");
            print_type(t->ptr_type.elem_type);
            return;
        case TYPE_ARRAY:
            printf("[");
            print_type(t->array_type.elem_type);
            if (t->array_type.len)
                printf("; %zu", t->array_type.len);
            printf("]");
            return;
        case TYPE_UNSUPPORTED:
            printf("<unsupported>");
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
        case OP_LESSEQ:
            return "<=";
        case OP_MOREEQ:
            return ">=";
        case OP_MOD:
            return "%";
        case OP_DIV:
            return "/";
        case OP_MUL:
            return "*";
        case OP_NEQ:
            return "!=";
        case OP_BITAND:
            return "&";
        case OP_BITOR:
            return "|";
        case OP_BITXOR:
            return "^";
        case OP_SHL:
            return "<<";
        case OP_SHR:
            return ">>";
        case OP_LAND:
            return "&&";
        case OP_LOR:
            return "||";
        case OP_POW:
            return "**";
    }
    return "?";
}

static const char* unop_str(UnOp op) {
    switch (op) {
        case UOP_NEG:
            return "-";
        case UOP_POS:
            return "+";
        case UOP_NOT:
            return "!";
        case UOP_BITNOT:
            return "~";
        case UOP_PREINC:
            return "++";
        case UOP_PREDEC:
            return "--";
        case UOP_POSTINC:
            return "++";
        case UOP_POSTDEC:
            return "--";
        case UOP_DEREF:
            return "*";
        case UOP_ADDR:
            return "&";
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
        case AST_STRING_LIT: {
            if (n->str_flags & STR_PREFIX_C) printf("c");
            else if (n->str_flags & STR_PREFIX_B) printf("b");
            if (n->str_flags & STR_PREFIX_R) printf("r");
            if (n->str_flags & STR_PREFIX_M) printf("m");
            printf("\"");
            for (size_t i = 0; i < n->len; i++) {
                unsigned char ch = (unsigned char)n->str[i];
                if      (ch == '"')  printf("\\\"");
                else if (ch == '\\') printf("\\\\");
                else if (ch == '\n') printf("\\n");
                else if (ch == '\r') printf("\\r");
                else if (ch == '\t') printf("\\t");
                else if (ch == '\0') printf("\\0");
                else if (ch < 0x20 || ch == 0x7F)
                    printf("\\x%02x", ch);
                else
                    putchar(ch);
            }
            printf("\"");
            break;
        }
        case AST_ARRAY_LIT:
            printf("[");
            for (size_t i = 0; i < n->element_count; i++) {
                if (i) printf(", ");
                dump_expr(n->elements[i], ind);
            }
            printf("]");
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
        case AST_VAR_ASSIGN: {
            printf("%s ", s->assign_name);
            switch (s->assign_op) {
                case ASSIGN_EQ:
                    printf("= ");
                    break;
                case ASSIGN_ADDEQ:
                    printf("+= ");
                    break;
                case ASSIGN_SUBEQ:
                    printf("-= ");
                    break;
                case ASSIGN_MULEQ:
                    printf("*= ");
                    break;
                case ASSIGN_DIVEQ:
                    printf("/= ");
                    break;
                case ASSIGN_MODEQ:
                    printf("%%= ");
                    break;
            }
            dump_expr(s->assign_value, ind);
            printf(";\n");
            break;
        }
        case AST_VAR_DECL:
            printf("%s: ", s->var_name);
            print_type(s->var_type);
            if (s->init) {
                printf(" = ");
                dump_expr(s->init, ind);
            }
            printf(";\n");
            break;
        case AST_CONST_DECL:
            printf("%s :: ", s->var_name);
            print_type(s->var_type);
            if (s->init) {
                printf(" = ");
                dump_expr(s->init, ind);
            }
            printf(";\n");
            break;
        default:
            printf("?stmt(%d)\n", s->kind);
            break;
    }
}

void ast_dump(Module* m) {
    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        switch (d->kind) {
            case AST_EXTERN_DECL:
                printf("extern %s(", d->function_name);
                for (int p = 0; p < d->param_count; p++) {
                    if (p)
                        printf(", ");
                    printf("%s: ", d->params[p].name);
                    print_type(d->params[p].type);
                }
                printf(") -> ");
                print_type(d->ret_type);
                printf("\n");
                break;
            case AST_FUNC_DECL:
                printf("%s :: (", d->function_name);
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
                break;
            case AST_VAR_DECL:
                printf("%s: ", d->var_name);
                print_type(d->var_type);
                if (d->init) {
                    printf(" = ");
                    dump_expr(d->init, 0);
                }
                printf(";\n");
                break;
            case AST_CONST_DECL:
                printf("%s :: ", d->var_name);
                print_type(d->var_type);
                if (d->init) {
                    printf(" = ");
                    dump_expr(d->init, 0);
                }
                printf(";\n");
                break;
            default:
                printf("?decl(%d)\n", d->kind);
        }
    }
}
