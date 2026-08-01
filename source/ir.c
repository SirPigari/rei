#include "ir.h"

#include "../thirdparty/ht.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IrVal val;
    IrVal ptr;
} VarSlot;
typedef Ht(const char*, VarSlot) VarMap;

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

static int intern_string(IrModule* m, const char* data, size_t len, StrPrefixFlags flags) {
    if (m->str_count >= m->str_cap) {
        m->str_cap  = m->str_cap ? m->str_cap * 2 : 8;
        m->strings  = realloc(m->strings, m->str_cap * sizeof(*m->strings));
    }
    char* copy               = malloc(len + 1);
    memcpy(copy, data, len);
    copy[len]                = '\0';
    int idx                  = m->str_count++;
    m->strings[idx].data      = copy;
    m->strings[idx].len       = len;
    m->strings[idx].str_flags = flags;
    return idx;
}

static int find_param(IrFunc* f, const char* name) {
    for (int i = 0; i < f->param_count; i++)
        if (strcmp(f->params[i].name, name) == 0)
            return i;
    return -1;
}

static IrVal lower_expr(IrModule* m, IrFunc* f, AstNode* e, VarMap* vars);
static void  lower_stmt(IrModule* m, IrFunc* f, AstNode* s, VarMap* vars);

static IrVal ensure_alloca(IrFunc* f, VarMap* vars, const char* name, Type* type, int slots) {
    VarSlot* slot = ht_find(vars, name);
    if (slot && slot->ptr != IR_NO_VAL)
        return slot->ptr;

    IrInstr* a    = emit(f);
    a->op         = IR_ALLOCA;
    a->dst        = next_val(f);
    a->type       = type_ptr(type, false);
    a->alloca_slots = slots;

    if (!slot) {
        VarSlot* ns = ht_put(vars, name);
        ns->val     = IR_NO_VAL;
        ns->ptr     = a->dst;
    } else {
        slot->ptr   = a->dst;
    }
    return a->dst;
}

static IrVal lower_expr(IrModule* m, IrFunc* f, AstNode* e, VarMap* vars) {
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
        case AST_STRING_LIT: {
            int            sidx  = intern_string(m, e->str, e->len, e->str_flags);
            StrPrefixFlags flags = e->str_flags;
            bool           is_c  = (flags & STR_PREFIX_C) != 0;
            bool           is_b  = (flags & STR_PREFIX_B) != 0;
            bool           is_fat = !is_c && !is_b;

            Type* u8_type   = type_number(TYPE_INT, 8, true);
            Type* ptr_u8    = type_ptr(u8_type, false);
            Type* fat_u8    = type_ptr(u8_type, true);
            Type* str_type  = e->type ? e->type
                            : is_fat  ? fat_u8
                                      : ptr_u8;

            IrInstr* ptr_i = emit(f);
            ptr_i->op      = IR_STRING;
            ptr_i->dst     = next_val(f);
            ptr_i->type    = ptr_u8;
            ptr_i->str_idx = sidx;

            if (!is_fat) {
                ptr_i->type = str_type;
                return ptr_i->dst;
            }

            IrInstr* fat = emit(f);
            fat->op           = IR_ALLOCA;
            fat->dst          = next_val(f);
            fat->type         = str_type;
            fat->alloca_slots = 2;

            IrInstr* st_ptr   = emit(f);
            st_ptr->op        = IR_STORE;
            st_ptr->dst       = IR_NO_VAL;
            st_ptr->type      = ptr_u8;
            st_ptr->store_ptr = fat->dst;
            st_ptr->store_val = ptr_i->dst;

            IrInstr* one_i = emit(f);
            one_i->op      = IR_CONST_INT;
            one_i->dst     = next_val(f);
            one_i->type    = type_number(TYPE_INT, 64, false);
            one_i->ival    = 1;

            IrInstr* len_ptr  = emit(f);
            len_ptr->op       = IR_GEP;
            len_ptr->dst      = next_val(f);
            len_ptr->type     = type_ptr(type_number(TYPE_INT, 64, false), false);
            len_ptr->gep_base = fat->dst;
            len_ptr->gep_idx  = one_i->dst;
            len_ptr->gep_scale = 8;

            IrInstr* len_val  = emit(f);
            len_val->op       = IR_CONST_INT;
            len_val->dst      = next_val(f);
            len_val->type     = type_number(TYPE_INT, 64, false);
            len_val->ival     = (long long)e->len;

            IrInstr* st_len   = emit(f);
            st_len->op        = IR_STORE;
            st_len->dst       = IR_NO_VAL;
            st_len->type      = type_number(TYPE_INT, 64, false);
            st_len->store_ptr = len_ptr->dst;
            st_len->store_val = len_val->dst;

            return fat->dst;
        }
        case AST_ARRAY_LIT: {
            size_t   n     = e->element_count;
            Type*    etype = (e->type && e->type->kind == TYPE_ARRAY) ? e->type->array_type.elem_type : NULL;
            Type*    ptype = type_ptr(etype ? etype : type_number(TYPE_INT, 64, false), false);

            IrInstr* arr   = emit(f);
            arr->op         = IR_ARRAY_INIT;
            arr->dst        = next_val(f);
            arr->type       = ptype;
            arr->alloca_slots = (int)n ? (int)n : 1;

            IrVal base = arr->dst;
            for (size_t ei = 0; ei < n; ei++) {
                IrVal elem_val = lower_expr(m, f, e->elements[ei], vars);

                if (ei == 0) {
                    IrInstr* st  = emit(f);
                    st->op       = IR_STORE;
                    st->dst      = IR_NO_VAL;
                    st->type     = etype;
                    st->store_ptr = base;
                    st->store_val = elem_val;
                } else {
                    IrInstr* idx_i = emit(f);
                    idx_i->op      = IR_CONST_INT;
                    idx_i->dst     = next_val(f);
                    idx_i->type    = type_number(TYPE_INT, 64, false);
                    idx_i->ival    = (long long)ei;

                    IrInstr* gep   = emit(f);
                    gep->op        = IR_GEP;
                    gep->dst       = next_val(f);
                    gep->type      = ptype;
                    gep->gep_base  = base;
                    gep->gep_idx   = idx_i->dst;
                    gep->gep_scale = 8;

                    IrInstr* st    = emit(f);
                    st->op         = IR_STORE;
                    st->dst        = IR_NO_VAL;
                    st->type       = etype;
                    st->store_ptr  = gep->dst;
                    st->store_val  = elem_val;
                }
            }
            return base;
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
            if (vars) {
                VarSlot* slot = ht_find(vars, e->ident);
                if (slot) {
                    if (slot->ptr != IR_NO_VAL) {
                        IrInstr* ld  = emit(f);
                        ld->op       = IR_LOAD;
                        ld->dst      = next_val(f);
                        ld->type     = e->type;
                        ld->load_ptr = slot->ptr;
                        return ld->dst;
                    }
                    return slot->val;
                }
            }
            return IR_NO_VAL;
        }
        case AST_CALL: {
            IrVal* arg_vals = e->arg_count ? malloc(e->arg_count * sizeof(IrVal)) : NULL;
            for (int a = 0; a < e->arg_count; a++) {
                IrVal v = lower_expr(m, f, e->args[a], vars);
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
            if (e->op == OP_POW) {
                if ((e->lhs->type && e->lhs->type->kind == TYPE_FLOAT) ||
                    (e->rhs->type && e->rhs->type->kind == TYPE_FLOAT))
                    ICE("TODO: support floats");
                if (e->rhs->kind != AST_INT_LIT)
                    ICE("TODO: support non-literals");

                long long exp = e->rhs->ival;
                if (exp < 0)
                    ICE("unexpected negative exponent for power op");
                if (exp == 0) {
                    IrInstr* i = emit(f);
                    i->op      = IR_CONST_INT;
                    i->dst     = next_val(f);
                    i->type    = e->type;
                    i->ival    = 1;
                    return i->dst;
                }
                if (exp == 1)
                    return lower_expr(m, f, e->lhs, vars);

                IrVal base = lower_expr(m, f, e->lhs, vars);

                if (exp < 10) {
                    IrVal result = base;
                    for (long long i = 1; i < exp; i++) {
                        IrVal    next_result = next_val(f);
                        IrInstr* mul         = emit(f);
                        mul->op              = IR_BINOP;
                        mul->bop             = OP_MUL;
                        mul->blhs            = result;
                        mul->brhs            = base;
                        mul->dst             = next_result;
                        mul->type            = e->type;
                        result               = next_result;
                    }
                    return result;
                } else {
                    IrVal result = next_val(f);
                    {
                        IrInstr* i = emit(f);
                        i->op      = IR_CONST_INT;
                        i->dst     = result;
                        i->type    = e->type;
                        i->ival    = 1;
                    }

                    IrVal     power_of_base = base;
                    long long exp_copy      = exp;

                    while (exp_copy > 0) {
                        if (exp_copy & 1) {
                            IrVal    new_result = next_val(f);
                            IrInstr* mul        = emit(f);
                            mul->op             = IR_BINOP;
                            mul->bop            = OP_MUL;
                            mul->blhs           = result;
                            mul->brhs           = power_of_base;
                            mul->dst            = new_result;
                            mul->type           = e->type;
                            result              = new_result;
                        }
                        exp_copy >>= 1;
                        if (exp_copy > 0) {
                            IrVal    new_power = next_val(f);
                            IrInstr* sq        = emit(f);
                            sq->op             = IR_BINOP;
                            sq->bop            = OP_MUL;
                            sq->blhs           = power_of_base;
                            sq->brhs           = power_of_base;
                            sq->dst            = new_power;
                            sq->type           = e->type;
                            power_of_base      = new_power;
                        }
                    }
                    return result;
                }
            }

            if (e->op == OP_LAND || e->op == OP_LOR) {
                IrVal lv     = lower_expr(m, f, e->lhs, vars);
                IrVal result = next_val(f);

                if (e->op == OP_LAND) {
                    int false_label_idx = f->instr_count + 4;
                    int end_label_idx   = f->instr_count + 5;

                    IrInstr* jmp_false = emit(f);
                    jmp_false->op      = IR_JMP_IF;
                    jmp_false->src     = lv;
                    jmp_false->label   = false_label_idx;

                    IrVal rv = lower_expr(m, f, e->rhs, vars);

                    IrInstr* combine = emit(f);
                    combine->op      = IR_BINOP;
                    combine->bop     = OP_LAND;
                    combine->blhs    = lv;
                    combine->brhs    = rv;
                    combine->dst     = result;
                    combine->type    = e->type;

                    IrInstr* jmp_end = emit(f);
                    jmp_end->op      = IR_JMP;
                    jmp_end->label   = end_label_idx;

                    IrInstr* false_lbl = emit(f);
                    false_lbl->op      = IR_LABEL;
                    false_lbl->label   = false_label_idx;

                    IrInstr* zero = emit(f);
                    zero->op      = IR_CONST_INT;
                    zero->dst     = result;
                    zero->ival    = 0;
                    zero->type    = e->type;

                    IrInstr* end_lbl = emit(f);
                    end_lbl->op      = IR_LABEL;
                    end_lbl->label   = end_label_idx;

                    return result;
                } else { /* OP_LOR */
                    int true_label_idx = f->instr_count + 6;
                    int end_label_idx  = f->instr_count + 7;

                    IrVal    zero_val   = next_val(f);
                    IrInstr* zero_const = emit(f);
                    zero_const->op      = IR_CONST_INT;
                    zero_const->dst     = zero_val;
                    zero_const->ival    = 0;
                    zero_const->type    = e->type;

                    IrVal    not_lv    = next_val(f);
                    IrInstr* not_instr = emit(f);
                    not_instr->op      = IR_BINOP;
                    not_instr->bop     = OP_EQ;
                    not_instr->blhs    = lv;
                    not_instr->brhs    = zero_val;
                    not_instr->dst     = not_lv;
                    not_instr->type    = e->type;

                    IrInstr* jmp_true = emit(f);
                    jmp_true->op      = IR_JMP_IF;
                    jmp_true->src     = not_lv;
                    jmp_true->label   = true_label_idx;

                    IrVal rv = lower_expr(m, f, e->rhs, vars);

                    IrInstr* combine = emit(f);
                    combine->op      = IR_BINOP;
                    combine->bop     = OP_LOR;
                    combine->blhs    = lv;
                    combine->brhs    = rv;
                    combine->dst     = result;
                    combine->type    = e->type;

                    IrInstr* jmp_end = emit(f);
                    jmp_end->op      = IR_JMP;
                    jmp_end->label   = end_label_idx;

                    IrInstr* true_lbl = emit(f);
                    true_lbl->op      = IR_LABEL;
                    true_lbl->label   = true_label_idx;

                    IrInstr* one = emit(f);
                    one->op      = IR_CONST_INT;
                    one->dst     = result;
                    one->ival    = 1;
                    one->type    = e->type;

                    IrInstr* end_lbl = emit(f);
                    end_lbl->op      = IR_LABEL;
                    end_lbl->label   = end_label_idx;

                    return result;
                }
            }

            IrVal    lv = lower_expr(m, f, e->lhs, vars);
            IrVal    rv = lower_expr(m, f, e->rhs, vars);
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
            switch (e->uop) {
                case UOP_ADDR: {
                    if (e->operand->kind != AST_IDENT)
                        ICE("& operand must be an identifier (for now)");
                    const char* name = e->operand->ident;
                    if (vars) {
                        VarSlot* slot = ht_find(vars, name);
                        if (slot && slot->ptr == IR_NO_VAL) {
                            IrVal ptr = ensure_alloca(f, vars, name, e->operand->type, 1);
                            if (slot->val != IR_NO_VAL) {
                                IrInstr* st   = emit(f);
                                st->op        = IR_STORE;
                                st->dst       = IR_NO_VAL;
                                st->type      = e->operand->type;
                                st->store_ptr = ptr;
                                st->store_val = slot->val;
                            }
                            return ptr;
                        } else if (slot) {
                            return slot->ptr;
                        }
                    }
                    int pidx = find_param(f, name);
                    if (pidx >= 0) {
                        IrVal ptr = ensure_alloca(f, vars ? vars : NULL, name, f->params[pidx].type, 1);
                        IrInstr* par  = emit(f);
                        par->op       = IR_PARAM;
                        par->dst      = next_val(f);
                        par->type     = f->params[pidx].type;
                        par->param_idx = pidx;
                        IrInstr* st   = emit(f);
                        st->op        = IR_STORE;
                        st->dst       = IR_NO_VAL;
                        st->type      = f->params[pidx].type;
                        st->store_ptr = ptr;
                        st->store_val = par->dst;
                        return ptr;
                    }
                    ICE("& on unknown identifier");
                    return IR_NO_VAL;
                }
                case UOP_DEREF: {
                    IrVal ptr   = lower_expr(m, f, e->operand, vars);
                    IrInstr* ld = emit(f);
                    ld->op      = IR_LOAD;
                    ld->dst     = next_val(f);
                    ld->type    = e->type;
                    ld->load_ptr = ptr;
                    return ld->dst;
                }
                case UOP_PREINC:
                case UOP_PREDEC: {
                    if (e->operand->kind != AST_IDENT)
                        ICE("++ / -- operand must be an identifier");
                    const char* name  = e->operand->ident;
                    IrVal       old_v = lower_expr(m, f, e->operand, vars);

                    IrInstr* one = emit(f);
                    one->op      = IR_CONST_INT;
                    one->dst     = next_val(f);
                    one->type    = e->type;
                    one->ival    = 1;

                    IrInstr* op  = emit(f);
                    op->op       = IR_BINOP;
                    op->bop      = (e->uop == UOP_PREINC) ? OP_ADD : OP_SUB;
                    op->blhs     = old_v;
                    op->brhs     = one->dst;
                    op->dst      = next_val(f);
                    op->type     = e->type;

                    if (vars) {
                        VarSlot* slot = ht_find(vars, name);
                        if (slot && slot->ptr != IR_NO_VAL) {
                            IrInstr* st   = emit(f);
                            st->op        = IR_STORE;
                            st->dst       = IR_NO_VAL;
                            st->type      = e->type;
                            st->store_ptr = slot->ptr;
                            st->store_val = op->dst;
                        } else if (slot) {
                            slot->val = op->dst;
                        }
                    }
                    return op->dst;
                }
                case UOP_POSTINC:
                case UOP_POSTDEC: {
                    if (e->operand->kind != AST_IDENT)
                        ICE("++ / -- operand must be an identifier");
                    const char* name  = e->operand->ident;
                    IrVal       old_v = lower_expr(m, f, e->operand, vars);

                    IrInstr* one = emit(f);
                    one->op      = IR_CONST_INT;
                    one->dst     = next_val(f);
                    one->type    = e->type;
                    one->ival    = 1;

                    IrInstr* op  = emit(f);
                    op->op       = IR_BINOP;
                    op->bop      = (e->uop == UOP_POSTINC) ? OP_ADD : OP_SUB;
                    op->blhs     = old_v;
                    op->brhs     = one->dst;
                    op->dst      = next_val(f);
                    op->type     = e->type;

                    if (vars) {
                        VarSlot* slot = ht_find(vars, name);
                        if (slot && slot->ptr != IR_NO_VAL) {
                            IrInstr* st   = emit(f);
                            st->op        = IR_STORE;
                            st->dst       = IR_NO_VAL;
                            st->type      = e->type;
                            st->store_ptr = slot->ptr;
                            st->store_val = op->dst;
                        } else if (slot) {
                            slot->val = op->dst;
                        }
                    }
                    return old_v;
                }
                default: {
                    IrVal    sv = lower_expr(m, f, e->operand, vars);
                    IrInstr* i  = emit(f);
                    i->op       = IR_UNOP;
                    i->dst      = next_val(f);
                    i->type     = e->type;
                    i->uop      = e->uop;
                    i->usrc     = sv;
                    return i->dst;
                }
            }
        }
        case AST_TYPE_LIT:
            return IR_NO_VAL;
        default:
            ICE("unexpected node in expression context");
            return IR_NO_VAL;
    }
}

static void lower_stmt(IrModule* m, IrFunc* f, AstNode* s, VarMap* vars) {
    switch (s->kind) {
        case AST_BLOCK_STMT:
            for (int i = 0; i < s->stmt_count; i++)
                lower_stmt(m, f, s->stmts[i], vars);
            break;
        case AST_EXPR_STMT:
            lower_expr(m, f, s->expr, vars);
            break;
        case AST_RETURN_STMT: {
            IrVal    src = s->ret_val ? lower_expr(m, f, s->ret_val, vars) : IR_NO_VAL;
            IrInstr* i   = emit(f);
            i->op        = IR_RET;
            i->src       = src;
            break;
        }
        case AST_IF_STMT: {
            IrVal cond_val = lower_expr(m, f, s->if_cond, vars);

            int jmp_if_idx            = f->instr_count;
            emit(f)->op               = IR_JMP_IF;
            f->instrs[jmp_if_idx].src = cond_val;

            lower_stmt(m, f, s->then_branch, vars);

            if (s->else_branch) {
                int jmp_else_idx = f->instr_count;
                emit(f)->op      = IR_JMP;

                int      else_label_idx     = f->instr_count;
                IrInstr* el                 = emit(f);
                el->op                      = IR_LABEL;
                el->label                   = else_label_idx;
                f->instrs[jmp_if_idx].label = else_label_idx;

                lower_stmt(m, f, s->else_branch, vars);

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

            IrVal cond_val = lower_expr(m, f, s->while_cond, vars);

            int jmp_if_idx            = f->instr_count;
            emit(f)->op               = IR_JMP_IF;
            f->instrs[jmp_if_idx].src = cond_val;

            lower_stmt(m, f, s->while_body, vars);

            IrInstr* jmp_back = emit(f);
            jmp_back->op      = IR_JMP;
            jmp_back->label   = loop_label_idx;

            int      end_label_idx      = f->instr_count;
            IrInstr* endl               = emit(f);
            endl->op                    = IR_LABEL;
            endl->label                 = end_label_idx;
            f->instrs[jmp_if_idx].label = end_label_idx;
        } break;
        case AST_VAR_DECL: {
            int slots = 1;
            if (s->var_type && s->var_type->kind == TYPE_ARRAY && s->var_type->array_type.len)
                slots = (int)s->var_type->array_type.len;

            Type* vtype = s->var_type ? s->var_type : (s->init ? s->init->type : NULL);
            IrVal ptr   = ensure_alloca(f, vars, s->var_name, vtype, slots);

            if (s->init) {
                IrVal init_val = lower_expr(m, f, s->init, vars);
                if (init_val != IR_NO_VAL) {
                    bool is_agg = s->init->kind == AST_ARRAY_LIT ||
                                  (s->init->kind == AST_STRING_LIT &&
                                   !(s->init->str_flags & STR_PREFIX_C) &&
                                   !(s->init->str_flags & STR_PREFIX_B));
                    if (!is_agg) {
                        IrInstr* st   = emit(f);
                        st->op        = IR_STORE;
                        st->dst       = IR_NO_VAL;
                        st->type      = vtype;
                        st->store_ptr = ptr;
                        st->store_val = init_val;
                    }
                    VarSlot* slot = ht_find(vars, s->var_name);
                    if (slot) slot->val = init_val;
                }
            }
            break;
        }
        case AST_CONST_DECL:
            if (s->init) {
                IrVal init_val = lower_expr(m, f, s->init, vars);
                if (init_val != IR_NO_VAL && vars) {
                    VarSlot* slot = ht_put(vars, s->var_name);
                    slot->val     = init_val;
                    slot->ptr     = IR_NO_VAL;
                }
            }
            break;
        case AST_VAR_ASSIGN: {
            IrVal assign_val = lower_expr(m, f, s->assign_value, vars);
            if (assign_val == IR_NO_VAL || !vars)
                break;

            VarSlot* slot = ht_find(vars, s->assign_name);

            IrVal new_val = assign_val;
            if (s->assign_op != ASSIGN_EQ && slot) {
                IrVal old_val = IR_NO_VAL;
                if (slot->ptr != IR_NO_VAL) {
                    IrInstr* ld  = emit(f);
                    ld->op       = IR_LOAD;
                    ld->dst      = next_val(f);
                    ld->type     = s->type;
                    ld->load_ptr = slot->ptr;
                    old_val      = ld->dst;
                } else {
                    old_val = slot->val;
                }

                if (old_val != IR_NO_VAL) {
                    IrInstr* i = emit(f);
                    i->op      = IR_BINOP;
                    i->dst     = next_val(f);
                    i->type    = s->type;
                    i->blhs    = old_val;
                    i->brhs    = assign_val;

                    switch (s->assign_op) {
                        case ASSIGN_ADDEQ:   i->bop = OP_ADD;    break;
                        case ASSIGN_SUBEQ:   i->bop = OP_SUB;    break;
                        case ASSIGN_MULEQ:   i->bop = OP_MUL;    break;
                        case ASSIGN_DIVEQ:   i->bop = OP_DIV;    break;
                        case ASSIGN_MODEQ:   i->bop = OP_MOD;    break;
                        case ASSIGN_BITANDEQ: i->bop = OP_BITAND; break;
                        case ASSIGN_BITOREQ: i->bop = OP_BITOR;  break;
                        case ASSIGN_BITXOREQ: i->bop = OP_BITXOR; break;
                        case ASSIGN_SHLEQ:   i->bop = OP_SHL;    break;
                        case ASSIGN_SHREQ:   i->bop = OP_SHR;    break;
                        case ASSIGN_LANDEQ:  i->bop = OP_LAND;   break;
                        case ASSIGN_LOREQ:   i->bop = OP_LOR;    break;
                        case ASSIGN_POWEQ:   i->bop = OP_POW;    break;
                        default: break;
                    }
                    new_val = i->dst;
                }
            }

            if (slot && slot->ptr != IR_NO_VAL) {
                IrInstr* st   = emit(f);
                st->op        = IR_STORE;
                st->dst       = IR_NO_VAL;
                st->type      = s->type;
                st->store_ptr = slot->ptr;
                st->store_val = new_val;
                slot->val     = new_val;
            } else {
                VarSlot* ns = slot ? slot : ht_put(vars, s->assign_name);
                ns->val     = new_val;
                if (!slot) ns->ptr = IR_NO_VAL;
            }
            break;
        }
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
        f->name        = d->function_name;
        f->params      = d->params;
        f->param_count = d->param_count;
        f->ret_type    = d->ret_type;
        f->is_extern   = (d->kind == AST_EXTERN_DECL);

        if (!f->is_extern && d->body) {
            VarMap var_map = {.hasheq = ht_cstr_hasheq};
            lower_stmt(m, f, d->body, &var_map);
            ht_free(&var_map);
        }

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
    for (int si = 0; si < m->str_count; si++)
        free(m->strings[si].data);
    free(m->strings);
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
            print_ty(t->ptr_type.elem_type);
            return;
        case TYPE_ARRAY:
            printf("[");
            print_ty(t->array_type.elem_type);
            if (t->array_type.len)
                printf("; %zu", t->array_type.len);
            printf("]");
            return;
        case TYPE_UNSUPPORTED:
            printf("<unsupported>");
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
        case OP_NEQ:
            return "neq";
        case OP_BITAND:
            return "bitand";
        case OP_BITOR:
            return "bitor";
        case OP_BITXOR:
            return "bitxor";
        case OP_SHL:
            return "shl";
        case OP_SHR:
            return "shr";
        case OP_LAND:
            return "land";
        case OP_LOR:
            return "lor";
        case OP_POW:
            return "pow";
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
        case UOP_NOT:
            return "not";
        case UOP_BITNOT:
            return "bitnot";
        case UOP_PREINC:
            return "preinc";
        case UOP_PREDEC:
            return "predec";
        case UOP_POSTINC:
            return "postinc";
        case UOP_POSTDEC:
            return "postdec";
        case UOP_DEREF:
            return "deref";
        case UOP_ADDR:
            return "addr";
        default:
            ICE("unhandled unop in ir_dump");
            return "?";
    }
}

void ir_dump(IrModule* m) {
    char buf[256];
    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (f->is_extern) {
            printf("extern %s\n", f->name);
            continue;
        }
        printf("func %s:\n", f->name);
        for (int ii = 0; ii < f->instr_count; ii++) {
            IrInstr* i   = &f->instrs[ii];
            int      pos = 0;

            if (i->op == IR_LABEL) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "label_%d:", i->label);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  ");
                if (i->dst != IR_NO_VAL)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "v%d = ", i->dst);
                switch (i->op) {
                    case IR_CONST_INT:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "const_int %lld", i->ival);
                        break;
                    case IR_CONST_FLOAT:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "const_float %g", i->fval);
                        break;
                    case IR_PARAM:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "param[%d]", i->param_idx);
                        break;
                    case IR_CALL: {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "call %s(", i->call.name);
                        for (int a = 0; a < i->call.arg_count; a++) {
                            if (a)
                                pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
                            pos += snprintf(buf + pos, sizeof(buf) - pos, "v%d", i->call.args[a]);
                        }
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ")");
                        break;
                    }
                    case IR_RET:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "ret");
                        if (i->src != IR_NO_VAL)
                            pos += snprintf(buf + pos, sizeof(buf) - pos, " v%d", i->src);
                        break;
                    case IR_BINOP:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s v%d, v%d", bop_s(i->bop), i->blhs, i->brhs);
                        break;
                    case IR_UNOP:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s v%d", uop_s(i->uop), i->usrc);
                        break;
                    case IR_JMP:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "jmp label_%d", i->label);
                        break;
                    case IR_JMP_IF:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "jmp_if v%d, label_%d", i->src, i->label);
                        break;
                    case IR_ALLOCA:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "alloca(%d slots)", i->alloca_slots);
                        break;
                    case IR_ARRAY_INIT:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "array_init(%d slots)", i->alloca_slots);
                        break;
                    case IR_LOAD:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "load *v%d", i->load_ptr);
                        break;
                    case IR_STORE:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "store *v%d <- v%d", i->store_ptr, i->store_val);
                        break;
                    case IR_STRING:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "string[%d]", i->str_idx);
                        break;
                    case IR_GEP:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "gep v%d[v%d * %d]", i->gep_base, i->gep_idx, i->gep_scale);
                        break;
                    default:
                        ICE("unhandled IR opcode in ir_dump");
                        break;
                }
            }

            if (i->type) {
                printf("%-*s ; ", 22, buf);
                print_ty(i->type);
                printf("\n");
            } else {
                printf("%s\n", buf);
            }
        }
    }
}
