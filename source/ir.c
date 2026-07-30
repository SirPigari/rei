#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IrVal next_val(IrFunc* f) {
    return f->next_val++;
}

static IrInstr* emit(IrFunc* f) {
    if (f->instr_count >= f->instr_cap) {
        f->instr_cap = f->instr_cap ? f->instr_cap * 2 : 16;
        f->instrs    = realloc(f->instrs, f->instr_cap * sizeof(IrInstr));
    }
    IrInstr* i = &f->instrs[f->instr_count++];
    memset(i, 0, sizeof(*i));
    i->dst = IR_NO_VAL;
    return i;
}

static int find_param(IrFunc* f, const char* name) {
    for (int i = 0; i < f->param_count; i++)
        if (strcmp(f->params[i].name, name) == 0)
            return i;
    return -1;
}

static IrVal lower_expr(IrFunc* f, AstNode* e);
static void  lower_stmt(IrFunc* f, AstNode* s);

static IrVal lower_expr(IrFunc* f, AstNode* e) {
    switch (e->kind) {
        case AST_INT_LIT: {
            IrInstr* i = emit(f);
            i->op      = IR_CONST_INT;
            i->dst     = next_val(f);
            i->type    = e->type;
            i->ival    = e->ival;
            return i->dst;
        }
        case AST_FLOAT_LIT: {
            IrInstr* i = emit(f);
            i->op      = IR_CONST_FLOAT;
            i->dst     = next_val(f);
            i->type    = e->type;
            i->fval    = e->fval;
            return i->dst;
        }
        case AST_IDENT: {
            int idx = find_param(f, e->ident);
            if (idx >= 0) {
                IrInstr* i   = emit(f);
                i->op        = IR_PARAM;
                i->dst       = next_val(f);
                i->type      = f->params[idx].type;
                i->param_idx = idx;
                return i->dst;
            }
            return IR_NO_VAL;
        }
        case AST_CALL: {
            IrVal* arg_vals = e->arg_count ? malloc(e->arg_count * sizeof(IrVal)) : NULL;
            for (int a = 0; a < e->arg_count; a++) {
                IrVal v = lower_expr(f, e->args[a]);
                if (v == IR_NO_VAL)
                    ICE("call argument lowered to IR_NO_VAL");
                arg_vals[a] = v;
            }
            IrInstr* i        = emit(f);
            i->op             = IR_CALL;
            i->type           = e->type;
            i->call.name      = e->callee;
            i->call.arg_count = e->arg_count;
            i->call.args      = arg_vals;
            if (e->type && e->type->kind != TYPE_VOID)
                i->dst = next_val(f);
            return i->dst;
        }
        case AST_BINOP: {
            IrVal    lv = lower_expr(f, e->lhs);
            IrVal    rv = lower_expr(f, e->rhs);
            IrInstr* i  = emit(f);
            i->op       = IR_BINOP;
            i->dst      = next_val(f);
            i->type     = e->type;
            i->bop      = e->op;
            i->blhs     = lv;
            i->brhs     = rv;
            return i->dst;
        }
        case AST_UNOP: {
            IrVal    sv = lower_expr(f, e->operand);
            IrInstr* i  = emit(f);
            i->op       = IR_UNOP;
            i->dst      = next_val(f);
            i->type     = e->type;
            i->uop      = e->uop;
            i->usrc     = sv;
            return i->dst;
        }
        default:
            ICE("unexpected node in expression context");
            return IR_NO_VAL;
    }
}

static void lower_stmt(IrFunc* f, AstNode* s) {
    switch (s->kind) {
        case AST_BLOCK_STMT:
            for (int i = 0; i < s->stmt_count; i++)
                lower_stmt(f, s->stmts[i]);
            break;
        case AST_EXPR_STMT:
            lower_expr(f, s->expr);
            break;
        case AST_RETURN_STMT: {
            IrVal    src = s->ret_val ? lower_expr(f, s->ret_val) : IR_NO_VAL;
            IrInstr* i   = emit(f);
            i->op        = IR_RET;
            i->src       = src;
            break;
        }
        case AST_IF_STMT: {
            IrVal cond_val = lower_expr(f, s->if_cond);

            int jmp_if_idx            = f->instr_count;
            emit(f)->op               = IR_JMP_IF;
            f->instrs[jmp_if_idx].src = cond_val;

            lower_stmt(f, s->then_branch);

            if (s->else_branch) {
                int jmp_else_idx = f->instr_count;
                emit(f)->op      = IR_JMP;

                int      else_label_idx     = f->instr_count;
                IrInstr* el                 = emit(f);
                el->op                      = IR_LABEL;
                el->label                   = else_label_idx;
                f->instrs[jmp_if_idx].label = else_label_idx;

                lower_stmt(f, s->else_branch);

                int      end_label_idx        = f->instr_count;
                IrInstr* endl                 = emit(f);
                endl->op                      = IR_LABEL;
                endl->label                   = end_label_idx;
                f->instrs[jmp_else_idx].label = end_label_idx;
            } else {
                int      end_label_idx      = f->instr_count;
                IrInstr* endl               = emit(f);
                endl->op                    = IR_LABEL;
                endl->label                 = end_label_idx;
                f->instrs[jmp_if_idx].label = end_label_idx;
            }
            break;
        }
        case AST_WHILE_STMT: {
            int      loop_label_idx = f->instr_count;
            IrInstr* loop_label     = emit(f);
            loop_label->op          = IR_LABEL;
            loop_label->label       = loop_label_idx;

            IrVal cond_val = lower_expr(f, s->while_cond);

            int jmp_if_idx            = f->instr_count;
            emit(f)->op               = IR_JMP_IF;
            f->instrs[jmp_if_idx].src = cond_val;

            lower_stmt(f, s->while_body);

            int jmp_back_idx      = f->instr_count;
            IrInstr* jmp_back     = emit(f);
            jmp_back->op          = IR_JMP;
            jmp_back->label       = loop_label_idx;

            int      end_label_idx        = f->instr_count;
            IrInstr* endl                 = emit(f);
            endl->op                      = IR_LABEL;
            endl->label                   = end_label_idx;
            f->instrs[jmp_if_idx].label   = end_label_idx;
        } break;
        default:
            ICE("unexpected node in statement context");
            break;
    }
}

IrModule* ir_lower(Module* ast) {
    IrModule* m = calloc(1, sizeof(*m));
    m->cap      = ast->count ? ast->count : 1;
    m->funcs    = malloc(m->cap * sizeof(IrFunc*));

    for (int di = 0; di < ast->count; di++) {
        AstNode* d = ast->decls[di];
        if (d->kind != AST_FUNC_DECL && d->kind != AST_EXTERN_DECL)
            continue;

        IrFunc* f      = calloc(1, sizeof(*f));
        f->name        = d->name;
        f->params      = d->params;
        f->param_count = d->param_count;
        f->ret_type    = d->ret_type;
        f->is_extern   = (d->kind == AST_EXTERN_DECL);

        if (!f->is_extern && d->body)
            lower_stmt(f, d->body);

        if (m->count >= m->cap) {
            m->cap *= 2;
            m->funcs = realloc(m->funcs, m->cap * sizeof(IrFunc*));
        }
        m->funcs[m->count++] = f;
    }
    return m;
}

void ir_free(IrModule* m) {
    if (!m)
        return;
    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        for (int ii = 0; ii < f->instr_count; ii++) {
            IrInstr* i = &f->instrs[ii];
            if (i->op == IR_CALL)
                free(i->call.args);
        }
        free(f->instrs);
        free(f);
    }
    free(m->funcs);
    free(m);
}

static void print_ty(Type* t) {
    if (!t) {
        printf("?<null type>");
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
        default:
            printf("?<type>");
            return;
    }
}

static const char* bop_s(BinOp b) {
    switch (b) {
        case OP_ADD:
            return "add";
        case OP_SUB:
            return "sub";
        case OP_EQ:
            return "eq";
        case OP_LESS:
            return "less";
        case OP_MORE:
            return "more";
        case OP_LESSEQ:
            return "lesseq";
        case OP_MOREEQ:
            return "moreeq";
        case OP_MOD:
            return "mod";
        case OP_DIV:
            return "div";
        case OP_MUL:
            return "mul";
        default:
            ICE("unhandled binop in ir_dump");
            return "?";
    }
}

static const char* uop_s(UnOp u) {
    switch (u) {
        case UOP_NEG:
            return "neg";
        case UOP_POS:
            return "pos";
        default:
            ICE("unhandled unop in ir_dump");
            return "?";
    }
}

void ir_dump(IrModule* m) {
    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (f->is_extern) {
            printf("extern %s\n", f->name);
            continue;
        }
        printf("func %s:\n", f->name);
        for (int ii = 0; ii < f->instr_count; ii++) {
            IrInstr* i = &f->instrs[ii];
            printf("  ");
            if (i->dst != IR_NO_VAL)
                printf("v%d = ", i->dst);
            switch (i->op) {
                case IR_CONST_INT:
                    printf("const_int %lld", i->ival);
                    break;
                case IR_CONST_FLOAT:
                    printf("const_float %g", i->fval);
                    break;
                case IR_PARAM:
                    printf("param[%d]", i->param_idx);
                    break;
                case IR_CALL:
                    printf("call %s(", i->call.name);
                    for (int a = 0; a < i->call.arg_count; a++) {
                        if (a)
                            printf(", ");
                        printf("v%d", i->call.args[a]);
                    }
                    printf(")");
                    break;
                case IR_RET:
                    printf("ret");
                    if (i->src != IR_NO_VAL)
                        printf(" v%d", i->src);
                    break;
                case IR_BINOP:
                    printf("%s v%d, v%d", bop_s(i->bop), i->blhs, i->brhs);
                    break;
                case IR_UNOP:
                    printf("%s v%d", uop_s(i->uop), i->usrc);
                    break;
                case IR_JMP:
                    printf("jmp label_%d", i->label);
                    break;
                case IR_JMP_IF:
                    printf("jmp_if v%d, label_%d", i->src, i->label);
                    break;
                case IR_LABEL:
                    printf("label_%d:", i->label);
                    break;
                default:
                    ICE("unhandled IR opcode in ir_dump");
                    break;
            }
            if (i->type) {
                printf("  ; ");
                print_ty(i->type);
            }
            printf("\n");
        }
    }
};
