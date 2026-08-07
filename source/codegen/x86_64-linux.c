#include "x86_64-linux.h"

#include "../../thirdparty/ht.h"
#include "../codegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REI_FUNC_PREFIX "rei__"

#define MAX_ARGREGS 6
static const char* ARGREGS[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

typedef struct {
    int offset;
    int stack_size;
} SlotInfo;

typedef struct {
    Ht(int, int) offsets;
    Ht(int, int) allocas;
    Ht(int, Type*) val_types;
    Ht(int, long long) const_vals;
    int stack_size;
} SlotMap;

static int slot_of(SlotMap* sm, IrVal v) {
    int* offset = ht_find(&sm->offsets, v);
    return offset ? *offset : 0;
}

static void alloc_slot(SlotMap* sm, IrVal v, int size) {
    sm->stack_size += size;
    int* offset_ptr = ht_find_or_put(&sm->offsets, v);
    *offset_ptr     = -(int)sm->stack_size;
}

static void mark_alloca(SlotMap* sm, IrVal v) {
    *ht_put(&sm->allocas, v) = 1;
}

static bool is_alloca(SlotMap* sm, IrVal v) {
    return ht_find(&sm->allocas, v) != NULL;
}

static Type* val_type(SlotMap* sm, IrVal v) {
    Type** t = ht_find(&sm->val_types, v);
    return t ? *t : NULL;
}

static void L(FILE* out, const char* fmt, ...) {
    fprintf(out, "  ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

static const char* mem(int offset, char* buf) {
    if (offset < 0)
        sprintf(buf, "[rbp - %d]", -offset);
    else
        sprintf(buf, "[rbp + %d]", offset);
    return buf;
}

typedef Ht(const char*, const char*) SymMap;

static SymMap symmap_build(IrModule* m) {
    SymMap sm = {.hasheq = ht_cstr_hasheq};
    for (int i = 0; i < m->count; i++) {
        IrFunc*     f = m->funcs[i];
        const char* asm_name;
        if (f->is_extern || f->no_mangle) {
            asm_name = f->name;
        } else {
            char* buf = malloc(strlen(REI_FUNC_PREFIX) + strlen(f->name) + 1);
            sprintf(buf, REI_FUNC_PREFIX "%s", f->name);
            asm_name = buf;
        }
        *ht_put(&sm, f->name) = asm_name;
    }
    return sm;
}

static const char* symmap_lookup(SymMap* sm, const char* fname) {
    const char** asm_name = ht_find(sm, fname);
    return asm_name ? *asm_name : fname;
}

static void symmap_free(SymMap* sm) {
    ht_foreach(asm_name, sm) {
        free((void*)*asm_name);
    }
    ht_free(sm);
}

static const char* size_kw(int bytes) {
    switch (bytes) {
        case 1:
            return "byte";
        case 2:
            return "word";
        case 4:
            return "dword";
        case 8:
            return "qword";
        default:
            ICE("unsupported size for x86_64: %d bytes", bytes);
            return "?";
    }
}
static const char* reg_a(int bytes) {
    switch (bytes) {
        case 1:
            return "al";
        case 2:
            return "ax";
        case 4:
            return "eax";
        case 8:
            return "rax";
        default:
            ICE("unsupported register size: %d bytes", bytes);
            return "?";
    }
}

static const char* reg_for_bytes(const char* base_reg, int bytes) {
    if (strcmp(base_reg, "rax") == 0)
        return reg_a(bytes);
    if (strcmp(base_reg, "rcx") == 0) {
        switch (bytes) {
            case 1:
                return "cl";
            case 2:
                return "cx";
            case 4:
                return "ecx";
            case 8:
                return "rcx";
        }
    }
    if (strcmp(base_reg, "r10") == 0) {
        switch (bytes) {
            case 1:
                return "r10b";
            case 2:
                return "r10w";
            case 4:
                return "r10d";
            case 8:
                return "r10";
        }
    }
    if (strcmp(base_reg, "r11") == 0) {
        switch (bytes) {
            case 1:
                return "r11b";
            case 2:
                return "r11w";
            case 4:
                return "r11d";
            case 8:
                return "r11";
        }
    }
    if (strcmp(base_reg, "rdi") == 0) {
        switch (bytes) {
            case 1:
                return "dil";
            case 2:
                return "di";
            case 4:
                return "edi";
            case 8:
                return "rdi";
        }
    }
    if (strcmp(base_reg, "rsi") == 0) {
        switch (bytes) {
            case 1:
                return "sil";
            case 2:
                return "si";
            case 4:
                return "esi";
            case 8:
                return "rsi";
        }
    }
    if (strcmp(base_reg, "rdx") == 0) {
        switch (bytes) {
            case 1:
                return "dl";
            case 2:
                return "dx";
            case 4:
                return "edx";
            case 8:
                return "rdx";
        }
    }
    if (strcmp(base_reg, "r8") == 0) {
        switch (bytes) {
            case 1:
                return "r8b";
            case 2:
                return "r8w";
            case 4:
                return "r8d";
            case 8:
                return "r8";
        }
    }
    if (strcmp(base_reg, "r9") == 0) {
        switch (bytes) {
            case 1:
                return "r9b";
            case 2:
                return "r9w";
            case 4:
                return "r9d";
            case 8:
                return "r9";
        }
    }
    return base_reg;
}

static void emit_load_slot_to(FILE* out, const char* reg, int bytes, bool is_signed, const char* mem_expr) {
    const char* target = reg_for_bytes(reg, bytes);
    if (bytes == 8) {
        L(out, "mov %s, qword %s", target, mem_expr);
    } else if (bytes == 4) {
        if (is_signed) {
            L(out, "movsxd %s, dword %s", reg, mem_expr);
        } else {
            L(out, "mov %s, dword %s", target, mem_expr);
        }
    } else if (bytes == 2) {
        const char* dest = reg_for_bytes(reg, 2);
        L(out, "%s %s, %s %s", is_signed ? "movsx" : "movzx", dest, size_kw(bytes), mem_expr);
    } else if (bytes == 1) {
        const char* dest = reg_for_bytes(reg, 2);
        L(out, "%s %s, %s %s", is_signed ? "movsx" : "movzx", dest, size_kw(bytes), mem_expr);
    } else {
        ICE("unsupported load size: %d bytes", bytes);
    }
}

static void emit_load_slot(FILE* out, int bytes, bool is_signed, const char* mem_expr) {
    emit_load_slot_to(out, "rax", bytes, is_signed, mem_expr);
}

static bool type_is_signed(Type* t) {
    if (!t || t->kind != TYPE_INT)
        return false;
    return !t->int_type.is_unsigned;
}

static void emit_load_or_const(FILE* out, SlotMap* sm, IrVal v, int bytes, bool is_signed, const char* mem_expr) {
    long long* const_val = ht_find(&sm->const_vals, v);
    if (const_val) {
        if (bytes == 8) {
            L(out, "mov rax, %lld", *const_val);
        } else if (bytes == 4) {
            L(out, "mov eax, %lld", *const_val);
        } else if (bytes == 2) {
            L(out, "mov eax, %lld", *const_val);
        } else if (bytes == 1) {
            L(out, "mov eax, %lld", *const_val);
        }
    } else {
        emit_load_slot(out, bytes, is_signed, mem_expr);
    }
}

static void emit_load_or_const_to(
    FILE* out, SlotMap* sm, IrVal v, const char* reg, int bytes, bool is_signed, const char* mem_expr) {
    long long* const_val = ht_find(&sm->const_vals, v);
    if (const_val) {
        const char* target = reg_for_bytes(reg, bytes);
        if (bytes == 8) {
            if (*const_val >= (-1LL << 31) && *const_val <= (1LL << 31) - 1) {
                const char* target_32 = reg_for_bytes(reg, 4);
                L(out, "mov %s, %lld", target_32, *const_val);
            } else {
                L(out, "mov rax, %lld", *const_val);
                if (strcmp(reg, "rax") != 0)
                    L(out, "mov %s, rax", reg);
            }
        } else if (bytes == 4) {
            L(out, "mov %s, %lld", reg_for_bytes(reg, 4), *const_val);
        } else {
            L(out, "mov %s, %lld", target, *const_val);
        }
    } else {
        Type* val_type_info = val_type(sm, v);
        if (val_type_info) {
            int  val_bytes  = type_bytes(val_type_info);
            bool val_signed = type_is_signed(val_type_info);
            emit_load_slot_to(out, reg, val_bytes, val_signed, mem_expr);
        } else {
            emit_load_slot_to(out, reg, bytes, is_signed, mem_expr);
        }
    }
}

static void emit_store_slot_from(FILE* out, int bytes, const char* mem_expr, const char* reg) {
    const char* src = reg_for_bytes(reg, bytes);
    L(out, "mov %s %s, %s", size_kw(bytes), mem_expr, src);
}

static void emit_store_slot(FILE* out, int bytes, const char* mem_expr) {
    emit_store_slot_from(out, bytes, mem_expr, "rax");
}

static void emit_func(IrFunc* f, SymMap* sm, FILE* out) {
    fprintf(out, "%s:\n", symmap_lookup(sm, f->name));
    L(out, "push rbp");
    L(out, "mov rbp, rsp");

    SlotMap slotmap = {0};
    for (int ii = 0; ii < f->instr_count; ii++) {
        IrInstr* ins = &f->instrs[ii];
        if (ins->dst != IR_NO_VAL && ins->type)
            *ht_put(&slotmap.val_types, ins->dst) = ins->type;

        if (ins->dst != IR_NO_VAL) {
            if (ins->op == IR_ALLOCA || ins->op == IR_ARRAY_INIT) {
                slotmap.stack_size += ins->alloca_slots * 8;
                int* offset_ptr = ht_find_or_put(&slotmap.offsets, ins->dst);
                *offset_ptr     = -(int)slotmap.stack_size + (ins->alloca_slots - 1) * 8;
                mark_alloca(&slotmap, ins->dst);
            } else if (ins->op == IR_CONST_INT) {
                *ht_put(&slotmap.const_vals, ins->dst) = ins->ival;
            } else if (ins->op == IR_CONST_FLOAT) {
                union {
                    double    d;
                    long long ll;
                } u;
                u.d                                    = ins->fval;
                *ht_put(&slotmap.const_vals, ins->dst) = u.ll;
            } else {
                if (ht_find(&slotmap.offsets, ins->dst))
                    continue;
                int size = type_bytes(ins->type);
                if (ins->op == IR_PARAM && ins->type && ins->type->kind == TYPE_PTR && ins->type->ptr_type.is_fat) {
                    size = 16;
                }
                alloc_slot(&slotmap, ins->dst, size);
            }
        }
    }

    if (slotmap.stack_size) {
        int total_needed = (slotmap.stack_size + 8 + 15) & ~15;
        int sub_amount   = total_needed - 8;
        L(out, "sub rsp, %d", sub_amount);
    }

    char buf[32], buf2[32];

    for (int ii = 0; ii < f->instr_count; ii++) {
        IrInstr* i        = &f->instrs[ii];
        int      is_float = i->type && i->type->kind == TYPE_FLOAT;

        switch (i->op) {
            case IR_CONST_INT: {
                break;
            }

            case IR_CONST_FLOAT: {
                break;
            }

            case IR_PARAM: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);

                if (i->type && i->type->kind == TYPE_PTR && i->type->ptr_type.is_fat) {
                    int slot_offset = slot_of(&slotmap, i->dst);
                    L(out, "mov rax, %s", ARGREGS[i->param_idx]);
                    emit_store_slot(out, 8, mem(slot_offset, buf));
                    L(out, "mov rax, %s", ARGREGS[i->param_idx + 1]);
                    emit_store_slot(out, 8, mem(slot_offset - 8, buf));
                } else if (i->param_idx < MAX_ARGREGS) {
                    L(out, "mov rax, %s", ARGREGS[i->param_idx]);
                    emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                } else {
                    int stack_off = 16 + (i->param_idx - MAX_ARGREGS) * 8;
                    L(out, "mov rax, qword [rbp + %d]", stack_off);
                    emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                }
                break;
            }

            case IR_CALL: {
                int bytes   = type_bytes(i->type);
                int reg_idx = 0;
                for (int a = 0; a < i->call.arg_count && reg_idx < MAX_ARGREGS; a++) {
                    Type* arg_type = i->call.arg_types ? i->call.arg_types[a] : NULL;

                    if (arg_type && arg_type->kind == TYPE_PTR && arg_type->ptr_type.is_fat) {
                        if (reg_idx + 1 < MAX_ARGREGS) {
                            int slot_offset = slot_of(&slotmap, i->call.args[a]);
                            L(out, "mov %s, qword %s", ARGREGS[reg_idx], mem(slot_offset, buf));
                            L(out, "mov %s, qword %s", ARGREGS[reg_idx + 1], mem(slot_offset - 8, buf2));
                            reg_idx += 2;
                        }
                    } else {
                        int  arg_bytes  = arg_type ? type_bytes(arg_type) : 8;
                        bool arg_signed = arg_type ? type_is_signed(arg_type) : false;
                        int  arg_slot   = slot_of(&slotmap, i->call.args[a]);

                        long long* const_val = ht_find(&slotmap.const_vals, i->call.args[a]);
                        if (const_val) {
                            if (*const_val >= (-1LL << 31) && *const_val <= (1LL << 31) - 1) {
                                const char* arg_reg_32 = reg_for_bytes(ARGREGS[reg_idx], 4);
                                L(out, "mov %s, %lld", arg_reg_32, *const_val);
                            } else {
                                L(out, "mov rax, %lld", *const_val);
                                L(out, "mov %s, rax", ARGREGS[reg_idx]);
                            }
                        } else {
                            emit_load_or_const(
                                out, &slotmap, i->call.args[a], arg_bytes, arg_signed, mem(arg_slot, buf));
                            L(out, "mov %s, rax", ARGREGS[reg_idx]);
                        }
                        reg_idx++;
                    }
                }
                L(out, "xor eax, eax");
                L(out, "call %s", symmap_lookup(sm, i->call.name));
                if (i->dst != IR_NO_VAL) {
                    if (is_float)
                        L(out, "movq rax, xmm0");
                    emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                }
                break;
            }

            case IR_BINOP: {
                int  lhs_bytes  = i->lhs_type ? type_bytes(i->lhs_type) : 8;
                bool lhs_signed = i->lhs_type ? type_is_signed(i->lhs_type) : false;
                int  rhs_bytes  = i->rhs_type ? type_bytes(i->rhs_type) : 8;
                bool rhs_signed = i->rhs_type ? type_is_signed(i->rhs_type) : false;
                bool cmp_signed = lhs_signed || rhs_signed;

                emit_load_or_const_to(
                    out, &slotmap, i->blhs, "rax", lhs_bytes, lhs_signed, mem(slot_of(&slotmap, i->blhs), buf));
                emit_load_or_const_to(
                    out, &slotmap, i->brhs, "rcx", rhs_bytes, rhs_signed, mem(slot_of(&slotmap, i->brhs), buf2));

                switch (i->bop) {
                    case OP_ADD:
                        L(out, "add rax, rcx");
                        break;
                    case OP_SUB:
                        L(out, "sub rax, rcx");
                        break;
                    case OP_EQ:
                        L(out, "cmp rax, rcx");
                        L(out, "sete al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_LESS:
                        L(out, "cmp rax, rcx");
                        L(out, cmp_signed ? "setl al" : "setb al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_MORE:
                        L(out, "cmp rax, rcx");
                        L(out, cmp_signed ? "setg al" : "seta al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_LESSEQ:
                        L(out, "cmp rax, rcx");
                        L(out, cmp_signed ? "setle al" : "setbe al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_MOREEQ:
                        L(out, "cmp rax, rcx");
                        L(out, cmp_signed ? "setge al" : "setae al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_MOD:
                        if (cmp_signed) {
                            L(out, "cqo");
                            L(out, "idiv rcx");
                        } else {
                            L(out, "xor edx, edx");
                            L(out, "div rcx");
                        }
                        L(out, "mov rax, rdx");
                        break;
                    case OP_DIV:
                        if (cmp_signed) {
                            L(out, "cqo");
                            L(out, "idiv rcx");
                        } else {
                            L(out, "xor edx, edx");
                            L(out, "div rcx");
                        }
                        break;
                    case OP_MUL:
                        L(out, "imul rax, rcx");
                        break;
                    case OP_NEQ:
                        L(out, "cmp rax, rcx");
                        L(out, "setne al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_BITAND:
                        L(out, "and rax, rcx");
                        break;
                    case OP_BITOR:
                        L(out, "or rax, rcx");
                        break;
                    case OP_BITXOR:
                        L(out, "xor rax, rcx");
                        break;
                    case OP_SHL:
                        L(out, lhs_signed ? "sal rax, cl" : "shl rax, cl");
                        break;
                    case OP_SHR:
                        L(out, lhs_signed ? "sar rax, cl" : "shr rax, cl");
                        break;
                    case OP_LAND:
                        L(out, "test rax, rax");
                        L(out, "setne al");
                        L(out, "movzx rax, al");
                        L(out, "test rcx, rcx");
                        L(out, "setne cl");
                        L(out, "and rax, rcx");
                        break;
                    case OP_LOR:
                        L(out, "test rax, rax");
                        L(out, "setne al");
                        L(out, "movzx rax, al");
                        L(out, "test rcx, rcx");
                        L(out, "setne cl");
                        L(out, "or rax, rcx");
                        break;
                    case OP_POW:
                        L(out, "cvtsi2sd xmm0, rax");
                        L(out, "cvtsi2sd xmm1, rcx");
                        L(out, "call pow");
                        L(out, "cvttsd2si rax, xmm0");
                        break;
                    default:
                        ICE("unhandled binop in codegen");
                        break;
                }
                int bytes = type_bytes(i->type);
                emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_UNOP: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                emit_load_or_const(out, &slotmap, i->usrc, bytes, is_signed, mem(slot_of(&slotmap, i->usrc), buf));
                switch (i->uop) {
                    case UOP_NEG:
                        L(out, "neg rax");
                        break;
                    case UOP_POS:
                        break; /* no-op */
                    case UOP_NOT:
                        L(out, "cmp rax, 0");
                        L(out, "sete al");
                        L(out, "movzx rax, al");
                        break;
                    case UOP_BITNOT:
                        L(out, "not rax");
                        break;
                    case UOP_PREINC:
                    case UOP_PREDEC:
                    case UOP_POSTINC:
                    case UOP_POSTDEC:
                        ICE("inc/dec unop reached codegen (should have been lowered in ir.c)");
                        break;
                    case UOP_DEREF:
                        ICE("deref unop reached codegen (should have been lowered to IR_LOAD)");
                        break;
                    case UOP_ADDR:
                        ICE("addr-of unop reached codegen (should have been lowered to IR_ALLOCA)");
                        break;
                    default:
                        ICE("unhandled unop in codegen");
                        break;
                }
                emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_RET:
                if (i->src != IR_NO_VAL) {
                    if (is_float) {
                        L(out, "movq xmm0, qword %s", mem(slot_of(&slotmap, i->src), buf));
                    } else {
                        int  ret_bytes  = type_bytes(i->type);
                        bool ret_signed = type_is_signed(i->type);
                        emit_load_or_const(
                            out, &slotmap, i->src, ret_bytes, ret_signed, mem(slot_of(&slotmap, i->src), buf));
                    }
                }
                L(out, "leave");
                L(out, "ret");
                break;

            case IR_LABEL:
                fprintf(out, "label_%d:\n", i->label);
                break;

            case IR_JMP:
                L(out, "jmp label_%d", i->label);
                break;

            case IR_JMP_IF: {
                Type* src_type  = val_type(&slotmap, i->src);
                int   bytes     = src_type ? type_bytes(src_type) : 8;
                bool  is_signed = type_is_signed(src_type);
                emit_load_or_const(out, &slotmap, i->src, bytes, is_signed, mem(slot_of(&slotmap, i->src), buf));
                L(out, "test rax, rax");
                L(out, "je label_%d", i->label);
                break;
            }

            case IR_ALLOCA:
            case IR_ARRAY_INIT:
                break;

            case IR_LOAD: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                int  dst_slot  = slot_of(&slotmap, i->dst);
                bool fat       = i->type && i->type->kind == TYPE_PTR && i->type->ptr_type.is_fat;

                if (is_alloca(&slotmap, i->load_ptr)) {
                    int src_slot = slot_of(&slotmap, i->load_ptr);
                    if (fat) {
                        L(out, "mov rax, qword %s", mem(src_slot, buf));
                        L(out, "mov rcx, qword %s", mem(src_slot - 8, buf2));
                        L(out, "mov qword %s, rax", mem(dst_slot, buf));
                        L(out, "mov qword %s, rcx", mem(dst_slot - 8, buf2));
                    } else {
                        L(out, "lea rax, %s", mem(src_slot, buf));
                        switch (bytes) {
                            case 1:
                                L(out, is_signed ? "movsx rax, byte [rax]" : "movzx rax, byte [rax]");
                                break;
                            case 2:
                                L(out, is_signed ? "movsx rax, word [rax]" : "movzx rax, word [rax]");
                                break;
                            case 4:
                                L(out, is_signed ? "movsxd rax, dword [rax]" : "mov eax, dword [rax]");
                                break;
                            default:
                                L(out, "mov rax, qword [rax]");
                                break;
                        }
                        emit_store_slot(out, bytes, mem(dst_slot, buf2));
                    }
                } else {
                    int ptr_off = slot_of(&slotmap, i->load_ptr);
                    L(out, "mov rax, qword %s", mem(ptr_off, buf));
                    if (fat) {
                        L(out, "mov rcx, qword [rax]");
                        L(out, "mov rdx, qword [rax + 8]");
                        L(out, "mov qword %s, rcx", mem(dst_slot, buf));
                        L(out, "mov qword %s, rdx", mem(dst_slot - 8, buf2));
                    } else {
                        switch (bytes) {
                            case 1:
                                L(out, is_signed ? "movsx rax, byte [rax]" : "movzx rax, byte [rax]");
                                break;
                            case 2:
                                L(out, is_signed ? "movsx rax, word [rax]" : "movzx rax, word [rax]");
                                break;
                            case 4:
                                L(out, is_signed ? "movsxd rax, dword [rax]" : "mov eax, dword [rax]");
                                break;
                            default:
                                L(out, "mov rax, qword [rax]");
                                break;
                        }
                        emit_store_slot(out, bytes, mem(dst_slot, buf2));
                    }
                }
                break;
            }

            case IR_STORE: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                bool fat       = i->type && i->type->kind == TYPE_PTR && i->type->ptr_type.is_fat;

                if (fat) {
                    int val_off = slot_of(&slotmap, i->store_val);
                    int ptr_off = slot_of(&slotmap, i->store_ptr);
                    L(out, "mov rax, qword %s", mem(val_off, buf));
                    L(out, "mov rcx, qword %s", mem(val_off - 8, buf2));
                    L(out, "mov qword %s, rax", mem(ptr_off, buf));
                    L(out, "mov qword %s, rcx", mem(ptr_off - 8, buf2));
                } else if (is_alloca(&slotmap, i->store_ptr)) {
                    int alloca_off = slot_of(&slotmap, i->store_ptr);
                    L(out, "lea r11, %s", mem(alloca_off, buf));

                    if (is_alloca(&slotmap, i->store_val)) {
                        L(out, "lea rax, %s", mem(slot_of(&slotmap, i->store_val), buf2));
                    } else {
                        emit_load_or_const(
                            out, &slotmap, i->store_val, bytes, is_signed, mem(slot_of(&slotmap, i->store_val), buf2));
                    }

                    switch (bytes) {
                        case 1:
                            L(out, "mov byte [r11], al");
                            break;
                        case 2:
                            L(out, "mov word [r11], ax");
                            break;
                        case 4:
                            L(out, "mov dword [r11], eax");
                            break;
                        default:
                            L(out, "mov qword [r11], rax");
                            break;
                    }
                } else {
                    int ptr_off = slot_of(&slotmap, i->store_ptr);
                    L(out, "mov r11, qword %s", mem(ptr_off, buf));

                    if (is_alloca(&slotmap, i->store_val)) {
                        L(out, "lea rax, %s", mem(slot_of(&slotmap, i->store_val), buf2));
                    } else {
                        emit_load_or_const(
                            out, &slotmap, i->store_val, bytes, is_signed, mem(slot_of(&slotmap, i->store_val), buf2));
                    }

                    switch (bytes) {
                        case 1:
                            L(out, "mov byte [r11], al");
                            break;
                        case 2:
                            L(out, "mov word [r11], ax");
                            break;
                        case 4:
                            L(out, "mov dword [r11], eax");
                            break;
                        default:
                            L(out, "mov qword [r11], rax");
                            break;
                    }
                }
                break;
            }

            case IR_STRING: {
                L(out, "lea rax, [__str%d]", i->str_idx);
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_GEP: {
                int  base_off    = slot_of(&slotmap, i->gep_base);
                int  idx_off     = slot_of(&slotmap, i->gep_idx);
                bool base_is_ptr = i->gep_base_type && i->gep_base_type->kind == TYPE_PTR;

                Type* idx_type   = val_type(&slotmap, i->gep_idx);
                int   idx_bytes  = idx_type ? type_bytes(idx_type) : 8;
                bool  idx_signed = type_is_signed(idx_type);
                emit_load_or_const_to(out, &slotmap, i->gep_idx, "rsi", idx_bytes, idx_signed, mem(idx_off, buf2));

                if (base_is_ptr && !is_alloca(&slotmap, i->gep_base)) {
                    L(out, "mov rax, qword %s", mem(base_off, buf));
                } else if (base_is_ptr && i->gep_base_type->ptr_type.is_fat) {
                    L(out, "mov rax, qword %s", mem(base_off, buf));
                } else {
                    L(out, "lea rax, %s", mem(base_off, buf));
                }

                switch (i->gep_scale) {
                    case 1:
                        L(out, "lea rax, [rax + rsi]");
                        break;
                    case 2:
                        L(out, "lea rax, [rax + rsi*2]");
                        break;
                    case 4:
                        L(out, "lea rax, [rax + rsi*4]");
                        break;
                    case 8:
                        L(out, "lea rax, [rax + rsi*8]");
                        break;
                    default:
                        L(out, "imul rsi, %d", i->gep_scale);
                        L(out, "add rax, rsi");
                        break;
                }
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_FAT_PTR: {
                int src_slot = slot_of(&slotmap, i->src);
                int dst_slot = slot_of(&slotmap, i->dst);
                L(out, "mov rax, qword %s", mem(src_slot, buf));
                L(out, "mov qword %s, rax", mem(dst_slot, buf2));
                break;
            }

            case IR_FAT_LEN: {
                int src_slot = slot_of(&slotmap, i->src);
                int dst_slot = slot_of(&slotmap, i->dst);
                L(out, "mov rax, qword %s", mem(src_slot - 8, buf));
                L(out, "mov qword %s, rax", mem(dst_slot, buf2));
                break;
            }

            case IR_FAT_SET_LEN: {
                int fat_slot = slot_of(&slotmap, i->store_ptr);
                int val_slot = slot_of(&slotmap, i->store_val);
                L(out, "mov rax, qword %s", mem(val_slot, buf));
                L(out, "mov qword %s, rax", mem(fat_slot - 8, buf2));
                break;
            }

            case IR_CAST: {
                int  src_bytes  = i->cast_from_type ? type_bytes(i->cast_from_type) : 8;
                bool src_signed = type_is_signed(i->cast_from_type);
                int  dst_bytes  = type_bytes(i->type);

                bool src_is_float = i->cast_from_type && i->cast_from_type->kind == TYPE_FLOAT;
                bool dst_is_float = i->type && i->type->kind == TYPE_FLOAT;

                int src_slot = slot_of(&slotmap, i->cast_src);
                int dst_slot = slot_of(&slotmap, i->dst);

                if (src_is_float && dst_is_float) {
                    emit_load_or_const(out, &slotmap, i->cast_src, src_bytes, false, mem(src_slot, buf));
                    emit_store_slot(out, dst_bytes, mem(dst_slot, buf2));
                } else if (src_is_float && !dst_is_float) {
                    L(out, "movq xmm0, qword %s", mem(src_slot, buf));
                    L(out, "cvttsd2si rax, xmm0");
                    emit_store_slot(out, dst_bytes, mem(dst_slot, buf2));
                } else if (!src_is_float && dst_is_float) {
                    emit_load_or_const(out, &slotmap, i->cast_src, src_bytes, src_signed, mem(src_slot, buf));
                    L(out, "cvtsi2sd xmm0, rax");
                    L(out, "movq rax, xmm0");
                    emit_store_slot(out, dst_bytes, mem(dst_slot, buf2));
                } else {
                    emit_load_or_const(out, &slotmap, i->cast_src, src_bytes, src_signed, mem(src_slot, buf));
                    emit_store_slot(out, dst_bytes, mem(dst_slot, buf2));
                }
                break;
            }

            default:
                ICE("unhandled IR opcode in codegen");
                break;
        }
    }

    if (f->instr_count == 0 || f->instrs[f->instr_count - 1].op != IR_RET) {
        L(out, "leave");
        L(out, "ret");
    }

    fprintf(out, "\n");
    ht_free(&slotmap.offsets);
    ht_free(&slotmap.allocas);
    ht_free(&slotmap.val_types);
}

void codegen_x86_64_linux(IrModule* m, FILE* out) {
    fprintf(out, "; Generated by rei - target x86_64-linux (fasm)\n");
    fprintf(out, "format ELF64\n\n");
    fprintf(out, "section '.text' executable\n\n");

    if (m->rei_main.params == MAIN_FUNC_FAT_FAT) {
        fprintf(out, "__rei_strlen:\n");
        L(out, "xor rax, rax");
        fprintf(out, "%%strlen_loop:\n");
        L(out, "cmp byte [rdi + rax], 0");
        L(out, "je %%strlen_end");
        L(out, "inc rax");
        L(out, "jmp %%strlen_loop");
        fprintf(out, "%%strlen_end:\n");
        L(out, "ret");
        fprintf(out, "\n");
    }

    SymMap sm = symmap_build(m);

    int     has_main = 0;
    IrFunc* rei_main = NULL;
    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (!f->is_extern && !f->no_mangle && strcmp(f->name, "main") == 0) {
            has_main = 1;
            rei_main = f;
            break;
        }
    }

    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (f->is_extern)
            fprintf(out, "extrn %s\n", f->name);
        else
            fprintf(out, "public %s\n", symmap_lookup(&sm, f->name));
    }
    if (has_main)
        fprintf(out, "public main\n");
    fprintf(out, "\n");

    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (!f->is_extern)
            emit_func(f, &sm, out);
    }

    if (has_main) {
        fprintf(out, "main:\n");
        L(out, "push rbp");
        L(out, "mov rbp, rsp");

        if (m->rei_main.params == MAIN_FUNC_NONE) {
            L(out, "call " REI_FUNC_PREFIX "main");
        } else if (m->rei_main.params == MAIN_FUNC_CRT0) {
            L(out, "call " REI_FUNC_PREFIX "main");
        } else if (m->rei_main.params == MAIN_FUNC_CRT0_SWAPPED) {
            L(out, "mov r10, rdi");
            L(out, "mov rdi, rsi");
            L(out, "mov rsi, r10");
            L(out, "call " REI_FUNC_PREFIX "main");
        } else if (m->rei_main.params == MAIN_FUNC_FAT_FAT || m->rei_main.params == MAIN_FUNC_FAT_THIN) {
            bool is_fat_fat = (m->rei_main.params == MAIN_FUNC_FAT_FAT);

            if (is_fat_fat) {
                L(out, "mov r8, rdi");
                L(out, "mov r11, rdi");
                L(out, "mov r9, rsi");
                L(out, "mov rax, r8");
                L(out, "imul rax, 16");
                L(out, "add rax, 8");
                L(out, "sub rsp, rax");
                L(out, "mov r10, rsp");

                fprintf(out, "  ; Build FAT_FAT array\n");
                fprintf(out, "%%fat_fat_loop:\n");
                L(out, "test r8, r8");
                L(out, "jz %%fat_fat_done");

                L(out, "mov rdi, qword [r9]");
                L(out, "call __rei_strlen");
                L(out, "mov rcx, rax");

                L(out, "mov rax, qword [r9]");
                L(out, "mov qword [r10], rax");
                L(out, "mov qword [r10 + 8], rcx");

                L(out, "add r10, 16");
                L(out, "add r9, 8");
                L(out, "dec r8");
                L(out, "jmp %%fat_fat_loop");

                fprintf(out, "%%fat_fat_done:\n");

                L(out, "mov rdi, rsp");
                L(out, "mov rsi, r11");
                L(out, "call " REI_FUNC_PREFIX "main");
            } else {
                L(out, "mov r10, rdi");
                L(out, "mov rdi, rsi");
                L(out, "mov rsi, r10");
                L(out, "call " REI_FUNC_PREFIX "main");
            }
        }

        L(out, "leave");
        L(out, "ret");
        fprintf(out, "\n");
    }

    symmap_free(&sm);

    if (m->str_count > 0) {
        fprintf(out, "\nsection '.rodata'\n\n");
        for (int si = 0; si < m->str_count; si++) {
            fprintf(out, "__str%d:\n  db ", si);
            const char*    d        = m->strings[si].data;
            size_t         n        = m->strings[si].len;
            StrPrefixFlags flags    = m->strings[si].str_flags;
            bool           add_null = !(flags & STR_PREFIX_B);
            bool           first    = true;

            size_t bi = 0;
            while (bi < n) {
                unsigned char ch = (unsigned char)d[bi];
                if (ch >= 0x20 && ch < 0x7f && ch != '\'' && ch != '\\') {
                    if (!first)
                        fprintf(out, ", ");
                    first = false;
                    fputc('\'', out);
                    while (bi < n) {
                        unsigned char c2 = (unsigned char)d[bi];
                        if (c2 < 0x20 || c2 >= 0x7f || c2 == '\'' || c2 == '\\')
                            break;
                        fputc(c2, out);
                        bi++;
                    }
                    fputc('\'', out);
                } else {
                    if (!first)
                        fprintf(out, ", ");
                    first = false;
                    fprintf(out, "0x%02x", ch);
                    bi++;
                }
            }

            if (n == 0 && !add_null) {
                fprintf(out, "0x00");
            }
            if (add_null) {
                if (!first)
                    fprintf(out, ", ");
                fprintf(out, "0x00");
            }
            fprintf(out, "\n");
        }
        fprintf(out, "\n");
    }
}

extern bool copy_file(const char* src, const char* dst);

bool codegen_x86_64_linux_compile(const char*           asm_path,
                                  const char*           out_path,
                                  const char*           tmp_dir,
                                  const CompileOptions* opts) {
    CompileOptions default_opts = {0};
    if (!opts)
        opts = &default_opts;

    if (opts->asm_only) {
        return false; /* no-op, assembly already generated */
    }

    char obj_path[4096];
    char cmd[8192];

    snprintf(obj_path, sizeof(obj_path), "%s/codegen_tmp.o", tmp_dir);

    snprintf(cmd, sizeof(cmd), "fasm \"%s\" \"%s\" > /dev/null", asm_path, obj_path);

    if (system(cmd) != 0)
        return false;

    if (opts->compile_only) {
        bool result = copy_file(obj_path, out_path);
        snprintf(cmd, sizeof(cmd), "rm \"%s\"", obj_path);
        system(cmd);
        return result;
    }

    char* libs = opts->no_libm ? "" : "-lm";

    if (opts->make_lib) {
        if (opts->make_static) {
            snprintf(cmd, sizeof(cmd), "ar rcs \"%s\" \"%s\" && rm \"%s\"", out_path, obj_path, obj_path);
        } else {
            snprintf(cmd,
                     sizeof(cmd),
                     "gcc %s -shared -fPIC \"%s\" -o \"%s\" && rm \"%s\"",
                     libs,
                     obj_path,
                     out_path,
                     obj_path);
        }
    } else {
        snprintf(cmd, sizeof(cmd), "gcc %s -no-pie \"%s\" -o \"%s\" && rm \"%s\"", libs, obj_path, out_path, obj_path);
    }

    return system(cmd) == 0;
}

CodegenTarget target_x86_64_linux = {
    .name    = "x86_64-linux",
    .emit    = codegen_x86_64_linux,
    .compile = codegen_x86_64_linux_compile,
};
