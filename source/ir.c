#include "ir.h"

#include "../thirdparty/ht.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IrVal    val;
    IrVal    ptr;
    AstNode* ast;
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
        m->str_cap = m->str_cap ? m->str_cap * 2 : 8;
        m->strings = realloc(m->strings, m->str_cap * sizeof(*m->strings));
    }
    char* copy = malloc(len + 1);
    memcpy(copy, data, len);
    copy[len]                 = '\0';
    int idx                   = m->str_count++;
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

static bool val_is_const_int(IrFunc* f, IrVal v, long long* out) {
    for (int i = f->instr_count - 1; i >= 0; i--) {
        IrInstr* ins = &f->instrs[i];
        if (ins->dst == v) {
            if (ins->op == IR_CONST_INT) {
                *out = ins->ival;
                return true;
            }
            if (ins->op == IR_CAST) {
                return val_is_const_int(f, ins->cast_src, out);
            }
            return false;
        }
    }
    return false;
}

static IrVal try_fold_binop(IrFunc* f, BinOp bop, IrVal lv, IrVal rv, Type* result_type) {
    long long lc, rc;
    if (!val_is_const_int(f, lv, &lc) || !val_is_const_int(f, rv, &rc))
        return IR_NO_VAL;
    long long res;
    switch (bop) {
        case OP_ADD:
            res = lc + rc;
            break;
        case OP_SUB:
            res = lc - rc;
            break;
        case OP_MUL:
            res = lc * rc;
            break;
        case OP_DIV:
            if (!rc)
                return IR_NO_VAL;
            res = lc / rc;
            break;
        case OP_MOD:
            if (!rc)
                return IR_NO_VAL;
            res = lc % rc;
            break;
        case OP_EQ:
            res = lc == rc;
            break;
        case OP_NEQ:
            res = lc != rc;
            break;
        case OP_LESS:
            res = lc < rc;
            break;
        case OP_MORE:
            res = lc > rc;
            break;
        case OP_LESSEQ:
            res = lc <= rc;
            break;
        case OP_MOREEQ:
            res = lc >= rc;
            break;
        case OP_BITAND:
            res = lc & rc;
            break;
        case OP_BITOR:
            res = lc | rc;
            break;
        case OP_BITXOR:
            res = lc ^ rc;
            break;
        case OP_SHL:
            res = (rc >= 0 && rc < 64) ? (lc << rc) : 0;
            break;
        case OP_SHR:
            res = (rc >= 0 && rc < 64) ? (lc >> rc) : 0;
            break;
        case OP_LAND:
            res = (lc && rc) ? 1 : 0;
            break;
        case OP_LOR:
            res = (lc || rc) ? 1 : 0;
            break;
        default:
            return IR_NO_VAL;
    }
    IrInstr* c = emit(f);
    c->op      = IR_CONST_INT;
    c->dst     = next_val(f);
    c->type    = result_type;
    c->ival    = res;
    return c->dst;
}

static int next_label(IrModule* m) {
    return m->next_label++;
}

static IrVal emit_cast(IrFunc* f, IrVal val, Type* from_type, Type* to_type) {
    if (!from_type || !to_type)
        return val;
    if (from_type->kind != to_type->kind)
        goto do_cast;
    if (from_type->bits != to_type->bits)
        goto do_cast;
    if (from_type->kind == TYPE_INT && from_type->int_type.is_unsigned != to_type->int_type.is_unsigned)
        goto do_cast;
    return val;

do_cast:;
    IrInstr* c        = emit(f);
    c->op             = IR_CAST;
    c->dst            = next_val(f);
    c->type           = to_type;
    c->cast_src       = val;
    c->cast_from_type = from_type;
    return c->dst;
}

static IrVal ensure_alloca(IrFunc* f, VarMap* vars, const char* name, Type* type, int slots) {
    VarSlot* slot = ht_find(vars, name);
    if (slot && slot->ptr != IR_NO_VAL)
        return slot->ptr;

    IrInstr* a = emit(f);
    a->op      = IR_ALLOCA;
    a->dst     = next_val(f);

    if (type && type->kind == TYPE_ARRAY) {
        a->type = type_ptr(type->array_type.elem_type, false);
    } else {
        a->type = type_ptr(type, false);
    }

    a->alloca_slots = slots;

    if (!slot) {
        VarSlot* ns = ht_put(vars, name);
        ns->val     = IR_NO_VAL;
        ns->ptr     = a->dst;
    } else {
        slot->ptr = a->dst;
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
            int            sidx   = intern_string(m, e->str, e->len, e->str_flags);
            StrPrefixFlags flags  = e->str_flags;
            bool           is_c   = (flags & STR_PREFIX_C) != 0;
            bool           is_b   = (flags & STR_PREFIX_B) != 0;
            bool           is_fat = !is_c && !is_b;

            Type* u8_type  = type_number(TYPE_INT, 8, true);
            Type* ptr_u8   = type_ptr(u8_type, false);
            Type* fat_u8   = type_ptr(u8_type, true);
            Type* str_type = e->type ? e->type : is_fat ? fat_u8 : ptr_u8;

            IrInstr* ptr_i = emit(f);
            ptr_i->op      = IR_STRING;
            ptr_i->dst     = next_val(f);
            ptr_i->type    = ptr_u8;
            ptr_i->str_idx = sidx;

            if (!is_fat) {
                ptr_i->type = str_type;
                return ptr_i->dst;
            }

            IrInstr* fat      = emit(f);
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

            IrInstr* len_val = emit(f);
            len_val->op      = IR_CONST_INT;
            len_val->dst     = next_val(f);
            len_val->type    = type_number(TYPE_INT, 64, false);
            len_val->ival    = (long long)e->len;

            IrInstr* st_len   = emit(f);
            st_len->op        = IR_FAT_SET_LEN;
            st_len->dst       = IR_NO_VAL;
            st_len->type      = type_number(TYPE_INT, 64, false);
            st_len->store_ptr = fat->dst;
            st_len->store_val = len_val->dst;

            return fat->dst;
        }
        case AST_ARRAY_LIT: {
            size_t n      = e->element_count;
            Type*  etype  = (e->type && e->type->kind == TYPE_ARRAY) ? e->type->array_type.elem_type : NULL;
            Type*  ptype  = type_ptr(etype ? etype : type_number(TYPE_INT, 64, false), false);
            int    escale = etype ? type_bytes(etype) : 8;

            IrInstr* arr      = emit(f);
            arr->op           = IR_ARRAY_INIT;
            arr->dst          = next_val(f);
            arr->type         = ptype;
            arr->alloca_slots = (int)n ? (int)n : 1;

            IrVal base = arr->dst;
            for (size_t ei = 0; ei < n; ei++) {
                IrVal elem_val = lower_expr(m, f, e->elements[ei], vars);

                if (ei == 0) {
                    IrInstr* st   = emit(f);
                    st->op        = IR_STORE;
                    st->dst       = IR_NO_VAL;
                    st->type      = etype;
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
                    gep->gep_scale = escale;

                    IrInstr* st   = emit(f);
                    st->op        = IR_STORE;
                    st->dst       = IR_NO_VAL;
                    st->type      = etype;
                    st->store_ptr = gep->dst;
                    st->store_val = elem_val;
                }
            }
            return base;
        }
        case AST_IDENT: {
            int idx = find_param(f, e->ident);
            if (idx >= 0) {
                if (vars) {
                    VarSlot* slot = ht_find(vars, e->ident);
                    if (slot && slot->ptr != IR_NO_VAL) {
                        IrInstr* ld  = emit(f);
                        ld->op       = IR_LOAD;
                        ld->dst      = next_val(f);
                        ld->type     = e->type;
                        ld->load_ptr = slot->ptr;
                        return ld->dst;
                    }
                    if (slot)
                        return slot->val;
                }
                int reg_idx = 0;
                for (int pi = 0; pi < idx; pi++) {
                    Type* pt = f->params[pi].type;
                    reg_idx += (pt && pt->kind == TYPE_PTR && pt->ptr_type.is_fat) ? 2 : 1;
                }
                IrInstr* i   = emit(f);
                i->op        = IR_PARAM;
                i->dst       = next_val(f);
                i->type      = f->params[idx].type;
                i->param_idx = reg_idx;
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
                    if (slot->ast) {
                        return lower_expr(m, f, slot->ast, vars);
                    }
                    return slot->val;
                }
            }
            return IR_NO_VAL;
        }
        case AST_CALL: {
            IrVal* arg_vals  = e->arg_count ? malloc(e->arg_count * sizeof(IrVal)) : NULL;
            Type** arg_types = e->arg_count ? malloc(e->arg_count * sizeof(Type*)) : NULL;
            for (int a = 0; a < e->arg_count; a++) {
                IrVal v = lower_expr(m, f, e->args[a], vars);
                if (v == IR_NO_VAL)
                    ICE("call argument lowered to IR_NO_VAL");
                arg_vals[a]  = v;
                arg_types[a] = e->args[a]->type;
            }
            IrInstr* i        = emit(f);
            i->op             = IR_CALL;
            i->type           = e->type;
            i->call.name      = e->callee;
            i->call.arg_count = e->arg_count;
            i->call.args      = arg_vals;
            i->call.arg_types = arg_types;
            if (e->type && e->type->kind != TYPE_VOID)
                i->dst = next_val(f);
            return i->dst;
        }
        case AST_INDEX: {
            IrVal array = lower_expr(m, f, e->array, vars);
            IrVal index = lower_expr(m, f, e->index, vars);

            if (array == IR_NO_VAL || index == IR_NO_VAL)
                return IR_NO_VAL;

            if (array == IR_NO_VAL)
                return IR_NO_VAL;

            bool was_fat = e->array->type && e->array->type->kind == TYPE_PTR && e->array->type->ptr_type.is_fat;
            if (was_fat) {
                IrInstr* fp = emit(f);
                fp->op      = IR_FAT_PTR;
                fp->dst     = next_val(f);
                fp->type    = type_ptr(e->array->type->ptr_type.elem_type, false);
                fp->src     = array;
                array       = fp->dst;
            }

            int scale = e->type ? type_bytes(e->type) : 8;

            IrInstr* gep       = emit(f);
            gep->op            = IR_GEP;
            gep->dst           = next_val(f);
            gep->type          = type_ptr(e->type, false);
            gep->gep_base      = array;
            gep->gep_idx       = index;
            gep->gep_scale     = scale;
            gep->gep_base_type = was_fat ? type_ptr(e->array->type->ptr_type.elem_type, false) : e->array->type;

            IrInstr* ld  = emit(f);
            ld->op       = IR_LOAD;
            ld->dst      = next_val(f);
            ld->type     = e->type;
            ld->load_ptr = gep->dst;

            return ld->dst;
        }
        case AST_MEMBER: {
            if (!e->member_value->type || e->member_value->type->kind != TYPE_PTR ||
                !e->member_value->type->ptr_type.is_fat) {
                ICE("member access only valid on fat pointers for now");
                return IR_NO_VAL;
            }

            if (strcmp(e->member_name, "len") != 0 && strcmp(e->member_name, "length") != 0 &&
                strcmp(e->member_name, "count") != 0) {
                ICE("unsupported member access '%s'", e->member_name);
                return IR_NO_VAL;
            }

            IrVal fat_alloca = IR_NO_VAL;
            if (e->member_value->kind == AST_IDENT && vars) {
                VarSlot* slot = ht_find(vars, e->member_value->ident);
                if (slot && slot->ptr != IR_NO_VAL)
                    fat_alloca = slot->ptr;
            }
            if (fat_alloca == IR_NO_VAL)
                fat_alloca = lower_expr(m, f, e->member_value, vars);

            IrInstr* i = emit(f);
            i->op      = IR_FAT_LEN;
            i->dst     = next_val(f);
            i->type    = e->type ? e->type : type_number(TYPE_INT, 64, false);
            i->src     = fat_alloca;
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
                        mul->lhs_type        = e->type;
                        mul->rhs_type        = e->type;
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
                            mul->lhs_type       = e->type;
                            mul->rhs_type       = e->type;
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
                            sq->lhs_type       = e->type;
                            sq->rhs_type       = e->type;
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
                    int false_label_idx = next_label(m);
                    int end_label_idx   = next_label(m);

                    IrInstr* jmp_false = emit(f);
                    jmp_false->op      = IR_JMP_IF;
                    jmp_false->src     = lv;
                    jmp_false->label   = false_label_idx;

                    IrVal rv = lower_expr(m, f, e->rhs, vars);

                    IrInstr* combine  = emit(f);
                    combine->op       = IR_BINOP;
                    combine->bop      = OP_LAND;
                    combine->blhs     = lv;
                    combine->brhs     = rv;
                    combine->dst      = result;
                    combine->type     = e->type;
                    combine->lhs_type = e->lhs->type;
                    combine->rhs_type = e->rhs->type;

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
                    int true_label_idx = next_label(m);
                    int end_label_idx  = next_label(m);

                    IrVal    zero_val   = next_val(f);
                    IrInstr* zero_const = emit(f);
                    zero_const->op      = IR_CONST_INT;
                    zero_const->dst     = zero_val;
                    zero_const->ival    = 0;
                    zero_const->type    = e->type;

                    IrVal    not_lv     = next_val(f);
                    IrInstr* not_instr  = emit(f);
                    not_instr->op       = IR_BINOP;
                    not_instr->bop      = OP_EQ;
                    not_instr->blhs     = lv;
                    not_instr->brhs     = zero_val;
                    not_instr->dst      = not_lv;
                    not_instr->type     = e->type;
                    not_instr->lhs_type = e->lhs->type;
                    not_instr->rhs_type = e->lhs->type;

                    IrInstr* jmp_true = emit(f);
                    jmp_true->op      = IR_JMP_IF;
                    jmp_true->src     = not_lv;
                    jmp_true->label   = true_label_idx;

                    IrVal rv = lower_expr(m, f, e->rhs, vars);

                    IrInstr* combine  = emit(f);
                    combine->op       = IR_BINOP;
                    combine->bop      = OP_LOR;
                    combine->blhs     = lv;
                    combine->brhs     = rv;
                    combine->dst      = result;
                    combine->type     = e->type;
                    combine->lhs_type = e->lhs->type;
                    combine->rhs_type = e->rhs->type;

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

            IrVal lv = lower_expr(m, f, e->lhs, vars);
            IrVal rv = lower_expr(m, f, e->rhs, vars);

            if ((e->op == OP_ADD || e->op == OP_SUB) && ((e->lhs->type && e->lhs->type->kind == TYPE_PTR) ||
                                                         (e->rhs->type && e->rhs->type->kind == TYPE_PTR))) {
                Type* ptr_type  = (e->lhs->type && e->lhs->type->kind == TYPE_PTR) ? e->lhs->type : e->rhs->type;
                Type* elem_type = ptr_type->ptr_type.elem_type;
                int   elem_size = 8;

                IrVal lhs_for_binop = lv;
                IrVal rhs_for_binop = rv;

                if (e->lhs->type->kind == TYPE_PTR && e->rhs->type->kind == TYPE_INT) {
                    Type* i64     = type_number(TYPE_INT, 64, false);
                    rhs_for_binop = emit_cast(f, rv, e->rhs->type, i64);
                    if (elem_size != 1) {
                        IrInstr* size_const = emit(f);
                        size_const->op      = IR_CONST_INT;
                        size_const->dst     = next_val(f);
                        size_const->type    = i64;
                        size_const->ival    = elem_size;

                        IrInstr* scale = emit(f);
                        scale->op      = IR_BINOP;
                        scale->bop     = OP_MUL;
                        scale->blhs    = rhs_for_binop;
                        scale->brhs    = size_const->dst;
                        scale->dst     = next_val(f);
                        scale->type    = i64;

                        rhs_for_binop = scale->dst;
                    }
                } else if (e->rhs->type->kind == TYPE_PTR && e->lhs->type->kind == TYPE_INT && e->op == OP_ADD) {
                    Type* i64     = type_number(TYPE_INT, 64, false);
                    lhs_for_binop = emit_cast(f, lv, e->lhs->type, i64);
                    if (elem_size != 1) {
                        IrInstr* size_const = emit(f);
                        size_const->op      = IR_CONST_INT;
                        size_const->dst     = next_val(f);
                        size_const->type    = i64;
                        size_const->ival    = elem_size;

                        IrInstr* scale = emit(f);
                        scale->op      = IR_BINOP;
                        scale->bop     = OP_MUL;
                        scale->blhs    = lhs_for_binop;
                        scale->brhs    = size_const->dst;
                        scale->dst     = next_val(f);
                        scale->type    = i64;

                        lhs_for_binop = scale->dst;
                    }
                    IrVal tmp     = lhs_for_binop;
                    lhs_for_binop = rhs_for_binop;
                    rhs_for_binop = tmp;
                } else if (e->lhs->type->kind == TYPE_PTR && e->rhs->type->kind == TYPE_PTR) {
                    /* ptr - ptr: no scaling needed */
                }

                IrInstr* i = emit(f);
                i->op      = IR_BINOP;
                i->dst     = next_val(f);
                i->type    = e->type;
                i->bop     = e->op;
                i->blhs    = lhs_for_binop;
                i->brhs    = rhs_for_binop;
                return i->dst;
            }

            Type* common_type = e->type;
            if (e->lhs->type && e->rhs->type) {
                int lhs_bits = e->lhs->type->bits ? e->lhs->type->bits : 64;
                int rhs_bits = e->rhs->type->bits ? e->rhs->type->bits : 64;
                if (lhs_bits != rhs_bits) {
                    common_type = (lhs_bits > rhs_bits) ? e->lhs->type : e->rhs->type;
                } else if (e->lhs->type->kind != e->rhs->type->kind) {
                    if (e->rhs->type->kind == TYPE_FLOAT)
                        common_type = e->rhs->type;
                    else
                        common_type = e->lhs->type;
                } else {
                    common_type = e->lhs->type;
                }
            }

            lv = emit_cast(f, lv, e->lhs->type, common_type);
            rv = emit_cast(f, rv, e->rhs->type, common_type);

            IrVal folded = try_fold_binop(f, e->op, lv, rv, e->type);
            if (folded != IR_NO_VAL)
                return folded;

            IrInstr* i  = emit(f);
            i->op       = IR_BINOP;
            i->dst      = next_val(f);
            i->type     = e->type;
            i->bop      = e->op;
            i->blhs     = lv;
            i->brhs     = rv;
            i->lhs_type = common_type;
            i->rhs_type = common_type;
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
                        IrVal ptr     = ensure_alloca(f, vars ? vars : NULL, name, f->params[pidx].type, 1);
                        int   reg_idx = 0;
                        for (int pi = 0; pi < pidx; pi++) {
                            Type* pt = f->params[pi].type;
                            reg_idx += (pt && pt->kind == TYPE_PTR && pt->ptr_type.is_fat) ? 2 : 1;
                        }
                        IrInstr* par   = emit(f);
                        par->op        = IR_PARAM;
                        par->dst       = next_val(f);
                        par->type      = f->params[pidx].type;
                        par->param_idx = reg_idx;
                        IrInstr* st    = emit(f);
                        st->op         = IR_STORE;
                        st->dst        = IR_NO_VAL;
                        st->type       = f->params[pidx].type;
                        st->store_ptr  = ptr;
                        st->store_val  = par->dst;
                        return ptr;
                    }
                    ICE("& on unknown identifier");
                    return IR_NO_VAL;
                }
                case UOP_DEREF: {
                    IrVal    ptr = lower_expr(m, f, e->operand, vars);
                    IrInstr* ld  = emit(f);
                    ld->op       = IR_LOAD;
                    ld->dst      = next_val(f);
                    ld->type     = e->type;
                    ld->load_ptr = ptr;
                    return ld->dst;
                }
                case UOP_PREINC:
                case UOP_PREDEC: {
                    if (e->operand->kind != AST_IDENT)
                        ICE("++ / -- operand must be an identifier");
                    const char* name  = e->operand->ident;
                    IrVal       old_v = lower_expr(m, f, e->operand, vars);
                    old_v             = emit_cast(f, old_v, e->operand->type, e->type);

                    IrInstr* one = emit(f);
                    one->op      = IR_CONST_INT;
                    one->dst     = next_val(f);
                    one->type    = e->type;
                    one->ival    = 1;

                    IrInstr* op = emit(f);
                    op->op      = IR_BINOP;
                    op->bop     = (e->uop == UOP_PREINC) ? OP_ADD : OP_SUB;
                    op->blhs    = old_v;
                    op->brhs    = one->dst;
                    op->dst     = next_val(f);
                    op->type    = e->type;

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
                    op->lhs_type = e->type;
                    op->rhs_type = e->type;

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
        case AST_CAST: {
            IrVal src = lower_expr(m, f, e->cast_expr, vars);

            if (e->cast_type && e->cast_type != e->cast_expr->type) {
                return emit_cast(f, src, e->cast_expr->type, e->cast_type);
            }
            return src;
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

                int      else_label_idx     = next_label(m);
                IrInstr* el                 = emit(f);
                el->op                      = IR_LABEL;
                el->label                   = else_label_idx;
                f->instrs[jmp_if_idx].label = else_label_idx;

                lower_stmt(m, f, s->else_branch, vars);

                int      end_label_idx        = next_label(m);
                IrInstr* endl                 = emit(f);
                endl->op                      = IR_LABEL;
                endl->label                   = end_label_idx;
                f->instrs[jmp_else_idx].label = end_label_idx;
            } else {
                int      end_label_idx      = next_label(m);
                IrInstr* endl               = emit(f);
                endl->op                    = IR_LABEL;
                endl->label                 = end_label_idx;
                f->instrs[jmp_if_idx].label = end_label_idx;
            }
            break;
        }
        case AST_WHILE_STMT: {
            int      loop_label_idx = next_label(m);
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

            int      end_label_idx      = next_label(m);
            IrInstr* endl               = emit(f);
            endl->op                    = IR_LABEL;
            endl->label                 = end_label_idx;
            f->instrs[jmp_if_idx].label = end_label_idx;
        } break;
        case AST_VAR_DECL: {
            Type* vtype = s->var_type ? s->var_type : (s->init ? s->init->type : NULL);

            int slots = 1;
            if (s->var_type && s->var_type->kind == TYPE_ARRAY && s->var_type->array_type.len) {
                slots = (int)s->var_type->array_type.len;
            } else if (vtype) {
                int bytes = type_bytes(vtype);
                slots     = (bytes + 7) / 8;
            }

            IrVal ptr = ensure_alloca(f, vars, s->var_name, vtype, slots);

            if (s->init) {
                if (s->init->kind == AST_ARRAY_LIT) {
                    Type* elem_type  = (vtype && vtype->kind == TYPE_ARRAY) ? vtype->array_type.elem_type : NULL;
                    int   elem_scale = elem_type ? type_bytes(elem_type) : 8;
                    for (size_t ei = 0; ei < s->init->element_count; ei++) {
                        IrVal elem_val = lower_expr(m, f, s->init->elements[ei], vars);
                        if (elem_val == IR_NO_VAL)
                            break;

                        if (ei == 0) {
                            IrInstr* st   = emit(f);
                            st->op        = IR_STORE;
                            st->dst       = IR_NO_VAL;
                            st->type      = elem_type;
                            st->store_ptr = ptr;
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
                            gep->type      = type_ptr(elem_type, false);
                            gep->gep_base  = ptr;
                            gep->gep_idx   = idx_i->dst;
                            gep->gep_scale = elem_scale;

                            IrInstr* st   = emit(f);
                            st->op        = IR_STORE;
                            st->dst       = IR_NO_VAL;
                            st->type      = elem_type;
                            st->store_ptr = gep->dst;
                            st->store_val = elem_val;
                        }
                    }
                } else {
                    IrVal init_val = lower_expr(m, f, s->init, vars);
                    if (init_val != IR_NO_VAL) {
                        bool is_agg = (s->init->kind == AST_STRING_LIT && !(s->init->str_flags & STR_PREFIX_C) &&
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
                        if (slot)
                            slot->val = init_val;
                    }
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
                        case ASSIGN_ADDEQ:
                            i->bop = OP_ADD;
                            break;
                        case ASSIGN_SUBEQ:
                            i->bop = OP_SUB;
                            break;
                        case ASSIGN_MULEQ:
                            i->bop = OP_MUL;
                            break;
                        case ASSIGN_DIVEQ:
                            i->bop = OP_DIV;
                            break;
                        case ASSIGN_MODEQ:
                            i->bop = OP_MOD;
                            break;
                        case ASSIGN_BITANDEQ:
                            i->bop = OP_BITAND;
                            break;
                        case ASSIGN_BITOREQ:
                            i->bop = OP_BITOR;
                            break;
                        case ASSIGN_BITXOREQ:
                            i->bop = OP_BITXOR;
                            break;
                        case ASSIGN_SHLEQ:
                            i->bop = OP_SHL;
                            break;
                        case ASSIGN_SHREQ:
                            i->bop = OP_SHR;
                            break;
                        case ASSIGN_LANDEQ:
                            i->bop = OP_LAND;
                            break;
                        case ASSIGN_LOREQ:
                            i->bop = OP_LOR;
                            break;
                        case ASSIGN_POWEQ:
                            i->bop = OP_POW;
                            break;
                        default:
                            break;
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
                if (!slot)
                    ns->ptr = IR_NO_VAL;
            }
            break;
        }
        case AST_INDEX_ASSIGN: {
            IrVal array      = IR_NO_VAL;
            IrVal index      = lower_expr(m, f, s->idx_index, vars);
            IrVal assign_val = lower_expr(m, f, s->idx_assign_value, vars);

            if (index == IR_NO_VAL || assign_val == IR_NO_VAL)
                break;

            if (s->idx_array->kind == AST_IDENT && vars) {
                VarSlot* slot = ht_find(vars, s->idx_array->ident);
                if (slot && slot->ptr != IR_NO_VAL) {
                    if (s->idx_array->type && s->idx_array->type->kind == TYPE_PTR) {
                        IrInstr* ld  = emit(f);
                        ld->op       = IR_LOAD;
                        ld->dst      = next_val(f);
                        ld->type     = s->idx_array->type;
                        ld->load_ptr = slot->ptr;
                        array        = ld->dst;
                    } else {
                        array = slot->ptr;
                    }
                } else {
                    array = lower_expr(m, f, s->idx_array, vars);
                }
            } else {
                array = lower_expr(m, f, s->idx_array, vars);
            }

            if (array == IR_NO_VAL)
                break;

            bool was_fat_assign =
                s->idx_array->type && s->idx_array->type->kind == TYPE_PTR && s->idx_array->type->ptr_type.is_fat;
            if (was_fat_assign) {
                IrInstr* fp = emit(f);
                fp->op      = IR_FAT_PTR;
                fp->dst     = next_val(f);
                fp->type    = type_ptr(s->idx_array->type->ptr_type.elem_type, false);
                fp->src     = array;
                array       = fp->dst;
            }

            Type* elem_type = NULL;
            if (s->idx_array->type && s->idx_array->type->kind == TYPE_ARRAY) {
                elem_type = s->idx_array->type->array_type.elem_type;
            } else if (s->idx_array->type && s->idx_array->type->kind == TYPE_PTR) {
                elem_type = s->idx_array->type->ptr_type.elem_type;
            }

            int scale = elem_type ? type_bytes(elem_type)
                                  : (s->idx_assign_value->type ? type_bytes(s->idx_assign_value->type) : 8);

            IrInstr* gep   = emit(f);
            gep->op        = IR_GEP;
            gep->dst       = next_val(f);
            gep->type      = type_ptr(elem_type ? elem_type : s->idx_assign_value->type, false);
            gep->gep_base  = array;
            gep->gep_idx   = index;
            gep->gep_scale = scale;
            gep->gep_base_type =
                was_fat_assign ? type_ptr(s->idx_array->type->ptr_type.elem_type, false) : s->idx_array->type;

            IrVal new_val = assign_val;
            if (s->idx_assign_op != ASSIGN_EQ) {
                IrInstr* ld   = emit(f);
                ld->op        = IR_LOAD;
                ld->dst       = next_val(f);
                ld->type      = elem_type ? elem_type : s->idx_assign_value->type;
                ld->load_ptr  = gep->dst;
                IrVal old_val = ld->dst;

                IrInstr* binop = emit(f);
                binop->op      = IR_BINOP;
                binop->dst     = next_val(f);
                binop->type    = elem_type ? elem_type : s->idx_assign_value->type;
                binop->blhs    = old_val;
                binop->brhs    = assign_val;

                switch (s->idx_assign_op) {
                    case ASSIGN_ADDEQ:
                        binop->bop = OP_ADD;
                        break;
                    case ASSIGN_SUBEQ:
                        binop->bop = OP_SUB;
                        break;
                    case ASSIGN_MULEQ:
                        binop->bop = OP_MUL;
                        break;
                    case ASSIGN_DIVEQ:
                        binop->bop = OP_DIV;
                        break;
                    case ASSIGN_MODEQ:
                        binop->bop = OP_MOD;
                        break;
                    case ASSIGN_BITANDEQ:
                        binop->bop = OP_BITAND;
                        break;
                    case ASSIGN_BITOREQ:
                        binop->bop = OP_BITOR;
                        break;
                    case ASSIGN_BITXOREQ:
                        binop->bop = OP_BITXOR;
                        break;
                    case ASSIGN_SHLEQ:
                        binop->bop = OP_SHL;
                        break;
                    case ASSIGN_SHREQ:
                        binop->bop = OP_SHR;
                        break;
                    case ASSIGN_LANDEQ:
                        binop->bop = OP_LAND;
                        break;
                    case ASSIGN_LOREQ:
                        binop->bop = OP_LOR;
                        break;
                    case ASSIGN_POWEQ:
                        binop->bop = OP_POW;
                        break;
                    default:
                        break;
                }
                new_val = binop->dst;
            }

            IrInstr* st   = emit(f);
            st->op        = IR_STORE;
            st->dst       = IR_NO_VAL;
            st->type      = s->idx_assign_value->type;
            st->store_ptr = gep->dst;
            st->store_val = new_val;
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

    AstNode** const_decls = NULL;
    int       const_count = 0, const_cap = 0;
    for (int di = 0; di < ast->count; di++) {
        AstNode* d = ast->decls[di];
        if (d->kind == AST_CONST_DECL) {
            if (const_count >= const_cap) {
                const_cap   = const_cap ? const_cap * 2 : 8;
                const_decls = realloc(const_decls, const_cap * sizeof(AstNode*));
            }
            const_decls[const_count++] = d;
        }
    }

    VarMap global_vars = {.hasheq = ht_cstr_hasheq};
    for (int ci = 0; ci < const_count; ci++) {
        AstNode* const_decl = const_decls[ci];
        if (const_decl->init) {
            VarSlot* slot = ht_put(&global_vars, const_decl->var_name);
            slot->val     = IR_NO_VAL;
            slot->ptr     = IR_NO_VAL;
            slot->ast     = const_decl->init;
        }
    }

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

            for (int ci = 0; ci < const_count; ci++) {
                const char* const_name  = const_decls[ci]->var_name;
                VarSlot*    global_slot = ht_find(&global_vars, const_name);
                if (global_slot) {
                    VarSlot* local_slot = ht_put(&var_map, const_name);
                    *local_slot         = *global_slot;
                }
            }

            int reg_idx = 0;
            for (int pi = 0; pi < f->param_count; pi++) {
                Type* ptype = f->params[pi].type;
                if (ptype && ptype->kind == TYPE_PTR && ptype->ptr_type.is_fat) {
                    IrInstr* al      = emit(f);
                    al->op           = IR_ALLOCA;
                    al->dst          = next_val(f);
                    al->type         = ptype;
                    al->alloca_slots = 2;

                    IrInstr* pp   = emit(f);
                    pp->op        = IR_PARAM;
                    pp->dst       = al->dst;
                    pp->type      = ptype;
                    pp->param_idx = reg_idx;

                    VarSlot* vs = ht_put(&var_map, f->params[pi].name);
                    vs->ptr     = al->dst;
                    vs->val     = IR_NO_VAL;
                    vs->ast     = NULL;

                    reg_idx += 2;
                } else {
                    IrInstr* pp   = emit(f);
                    pp->op        = IR_PARAM;
                    pp->dst       = next_val(f);
                    pp->type      = ptype;
                    pp->param_idx = reg_idx;

                    VarSlot* vs = ht_put(&var_map, f->params[pi].name);
                    vs->ptr     = IR_NO_VAL;
                    vs->val     = pp->dst;
                    vs->ast     = NULL;

                    reg_idx += 1;
                }
            }

            lower_stmt(m, f, d->body, &var_map);
            ht_free(&var_map);
        }

        if (m->count >= m->cap) {
            m->cap *= 2;
            m->funcs = realloc(m->funcs, m->cap * sizeof(IrFunc*));
        }
        m->funcs[m->count++] = f;
    }

    ht_free(&global_vars);
    free(const_decls);

    m->rei_main.params = MAIN_FUNC_NO_FUNC;
    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (strcmp(f->name, "main") == 0 && !f->is_extern) {
            m->rei_main.name     = f->name;
            m->rei_main.func     = f;
            m->rei_main.ret_type = f->ret_type;

            if (f->param_count == 0) {
                m->rei_main.params = MAIN_FUNC_NONE;
            } else if (f->param_count == 1) {
                m->rei_main.params = MAIN_FUNC_FAT_FAT;
                if (f->params && f->params[0].type) {
                    if (f->params[0].type->kind == TYPE_PTR && f->params[0].type->ptr_type.is_fat) {
                        Type* elem = f->params[0].type->ptr_type.elem_type;
                        if (elem && elem->kind == TYPE_PTR) {
                            if (elem->ptr_type.is_fat) {
                                m->rei_main.params = MAIN_FUNC_FAT_FAT;  /* [][]u8 */
                            } else {
                                m->rei_main.params = MAIN_FUNC_FAT_THIN; /* []*u8 */
                            }
                        }
                    }
                }
                m->rei_main.param_types[0] = f->params[0].type;
            } else if (f->param_count == 2) {
                Type* p0                   = f->params[0].type;
                Type* p1                   = f->params[1].type;
                m->rei_main.param_types[0] = p0;
                m->rei_main.param_types[1] = p1;

                if (p0 && p0->kind == TYPE_INT) {
                    m->rei_main.params = MAIN_FUNC_CRT0;
                } else {
                    m->rei_main.params = MAIN_FUNC_CRT0_SWAPPED;
                }
            }
            break;
        }
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
                        pos += snprintf(
                            buf + pos, sizeof(buf) - pos, "gep v%d[v%d * %d]", i->gep_base, i->gep_idx, i->gep_scale);
                        break;
                    case IR_FAT_PTR:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "fat_ptr v%d", i->src);
                        break;
                    case IR_FAT_LEN:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "fat_len v%d", i->src);
                        break;
                    case IR_FAT_SET_LEN:
                        pos += snprintf(
                            buf + pos, sizeof(buf) - pos, "fat_set_len v%d <- v%d", i->store_ptr, i->store_val);
                        break;
                    case IR_CAST:
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "cast v%d: ", i->cast_src);
                        if (i->cast_from_type) {
                            char type_buf[64];
                            type_to_string(i->cast_from_type, type_buf, sizeof(type_buf));
                            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", type_buf);
                        }
                        if (i->type) {
                            char type_buf[64];
                            type_to_string(i->type, type_buf, sizeof(type_buf));
                            pos += snprintf(buf + pos, sizeof(buf) - pos, " -> %s", type_buf);
                        }
                        break;
                    default:
                        ICE("unhandled IR opcode in ir_dump: %d", i->op);
                        break;
                }
            }

            if (i->type) {
                printf("%-*s ; ", 32, buf);
                print_ty(i->type);
                printf("\n");
            } else {
                printf("%s\n", buf);
            }
        }
    }
}
