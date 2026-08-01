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
    int stack_size;
} SlotMap;

static int slot_of(SlotMap* sm, IrVal v) {
    int* offset = ht_find(&sm->offsets, v);
    return offset ? *offset : 0;
}

static void alloc_slot(SlotMap* sm, IrVal v) {
    sm->stack_size += 8;
    int* offset_ptr = ht_find_or_put(&sm->offsets, v);
    *offset_ptr     = -(int)sm->stack_size;
}

static void mark_alloca(SlotMap* sm, IrVal v) {
    *ht_put(&sm->allocas, v) = 1;
}

static bool is_alloca(SlotMap* sm, IrVal v) {
    return ht_find(&sm->allocas, v) != NULL;
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

static int type_bytes(Type* t) {
    if (!t) return 8;
    switch (t->kind) {
        case TYPE_INT:
        case TYPE_FLOAT:
            return t->bits ? (t->bits / 8) : 8;
        case TYPE_PTR:
        case TYPE_ARRAY:
            return 8;
        default:
            return 8;
    }
}

static const char* size_kw(int bytes) {
    switch (bytes) {
        case 1:  return "byte";
        case 2:  return "word";
        case 4:  return "dword";
        default: return "qword";
    }
}
static const char* reg_a(int bytes) {
    switch (bytes) {
        case 1:  return "al";
        case 2:  return "ax";
        case 4:  return "eax";
        default: return "rax";
    }
}

static void emit_load_slot(FILE* out, int bytes, bool is_signed, const char* mem_expr) {
    if (bytes == 8) {
        L(out, "mov rax, qword %s", mem_expr);
    } else if (bytes == 4 && !is_signed) {
        L(out, "mov eax, dword %s", mem_expr);
    } else if (bytes == 4 && is_signed) {
        L(out, "movsxd rax, dword %s", mem_expr);
    } else {
        L(out, "%s rax, %s %s", is_signed ? "movsx" : "movzx", size_kw(bytes), mem_expr);
    }
}

static void emit_store_slot(FILE* out, int bytes, const char* mem_expr) {
    L(out, "mov %s %s, %s", size_kw(bytes), mem_expr, reg_a(bytes));
}

static bool type_is_signed(Type* t) {
    if (!t || t->kind != TYPE_INT) return false;
    return !t->int_type.is_unsigned;
}

static void emit_func(IrFunc* f, SymMap* sm, FILE* out) {
    fprintf(out, "%s:\n", symmap_lookup(sm, f->name));
    L(out, "push rbp");
    L(out, "mov rbp, rsp");

    SlotMap slotmap = {0};
    for (int ii = 0; ii < f->instr_count; ii++) {
        IrInstr* ins = &f->instrs[ii];
        if (ins->dst != IR_NO_VAL) {
            alloc_slot(&slotmap, ins->dst);
            if (ins->op == IR_ALLOCA || ins->op == IR_ARRAY_INIT) {
                slotmap.stack_size += ins->alloca_slots * 8;
                mark_alloca(&slotmap, ins->dst);
            }
        }
    }

    if (slotmap.stack_size) {
        int aligned = (slotmap.stack_size + 15) & ~15;
        L(out, "sub rsp, %d", aligned);
    }

    char buf[32], buf2[32];

    for (int ii = 0; ii < f->instr_count; ii++) {
        IrInstr* i        = &f->instrs[ii];
        int      is_float = i->type && i->type->kind == TYPE_FLOAT;

        switch (i->op) {
            case IR_CONST_INT: {
                int  bytes = type_bytes(i->type);
                L(out, "mov rax, %lld", i->ival);
                emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_CONST_FLOAT: {
                union {
                    double    d;
                    long long ll;
                } u;
                u.d = i->fval;
                L(out, "; float literal %g", i->fval);
                L(out, "mov rax, %lld", u.ll);
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_PARAM: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                if (i->param_idx < MAX_ARGREGS) {
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
                int bytes = type_bytes(i->type);
                for (int a = 0; a < i->call.arg_count && a < MAX_ARGREGS; a++)
                    L(out, "mov %s, qword %s", ARGREGS[a], mem(slot_of(&slotmap, i->call.args[a]), buf));
                L(out, "call %s", symmap_lookup(sm, i->call.name));
                if (i->dst != IR_NO_VAL) {
                    if (is_float)
                        L(out, "movq rax, xmm0");
                    emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                }
                break;
            }

            case IR_BINOP: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                emit_load_slot(out, bytes, is_signed, mem(slot_of(&slotmap, i->blhs), buf));
                L(out, "push rax");
                emit_load_slot(out, bytes, is_signed, mem(slot_of(&slotmap, i->brhs), buf2));
                L(out, "mov rcx, rax");
                L(out, "pop rax");
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
                        L(out, "setl al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_MORE:
                        L(out, "cmp rax, rcx");
                        L(out, "setg al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_LESSEQ:
                        L(out, "cmp rax, rcx");
                        L(out, "setle al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_MOREEQ:
                        L(out, "cmp rax, rcx");
                        L(out, "setge al");
                        L(out, "movzx rax, al");
                        break;
                    case OP_MOD:
                        L(out, "xor rdx, rdx");
                        L(out, "idiv rcx");
                        L(out, "mov rax, rdx");
                        break;
                    case OP_DIV:
                        L(out, "xor rdx, rdx");
                        L(out, "idiv rcx");
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
                        L(out, "mov cl, cl");
                        L(out, "shl rax, cl");
                        break;
                    case OP_SHR:
                        L(out, "mov cl, cl");
                        L(out, "sar rax, cl");
                        break;
                    case OP_LAND:
                        L(out, "cmp rax, 0");
                        L(out, "setne al");
                        L(out, "movzx rax, al");
                        L(out, "cmp rcx, 0");
                        L(out, "setne cl");
                        L(out, "and rax, rcx");
                        break;
                    case OP_LOR:
                        L(out, "cmp rax, 0");
                        L(out, "setne al");
                        L(out, "movzx rax, al");
                        L(out, "cmp rcx, 0");
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
                emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf));
                break;
            }

            case IR_UNOP: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                emit_load_slot(out, bytes, is_signed, mem(slot_of(&slotmap, i->usrc), buf));
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
                        emit_load_slot(out, ret_bytes, ret_signed,
                                       mem(slot_of(&slotmap, i->src), buf));
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

            case IR_JMP_IF:
                L(out, "mov rax, qword %s", mem(slot_of(&slotmap, i->src), buf));
                L(out, "cmp rax, 0");
                L(out, "je label_%d", i->label);
                break;

            case IR_ALLOCA:
            case IR_ARRAY_INIT:
                break;

            case IR_LOAD: {
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                if (is_alloca(&slotmap, i->load_ptr)) {
                    int val_off = slot_of(&slotmap, i->load_ptr) - 8;
                    emit_load_slot(out, bytes, is_signed, mem(val_off, buf));
                } else {
                    int ptr_off = slot_of(&slotmap, i->load_ptr);
                    L(out, "mov rax, qword %s", mem(ptr_off, buf));
                    switch (bytes) {
                        case 1: L(out, is_signed ? "movsx rax, byte [rax]"
                                                 : "movzx rax, byte [rax]"); break;
                        case 2: L(out, is_signed ? "movsx rax, word [rax]"
                                                 : "movzx rax, word [rax]"); break;
                        case 4: L(out, is_signed ? "movsxd rax, dword [rax]"
                                                 : "mov eax, dword [rax]");  break;
                        default: L(out, "mov rax, qword [rax]");              break;
                    }
                }
                emit_store_slot(out, bytes, mem(slot_of(&slotmap, i->dst), buf2));
                break;
            }

            case IR_STORE: {
                int  src_off   = slot_of(&slotmap, i->store_val);
                int  bytes     = type_bytes(i->type);
                bool is_signed = type_is_signed(i->type);
                if (is_alloca(&slotmap, i->store_ptr)) {
                    int val_off = slot_of(&slotmap, i->store_ptr) - 8;
                    emit_load_slot(out, bytes, is_signed, mem(src_off, buf));
                    emit_store_slot(out, bytes, mem(val_off, buf2));
                } else {
                    int ptr_off = slot_of(&slotmap, i->store_ptr);
                    L(out, "mov r11, qword %s", mem(ptr_off, buf));
                    emit_load_slot(out, bytes, is_signed, mem(src_off, buf2));
                    switch (bytes) {
                        case 1:  L(out, "mov byte [r11], al");    break;
                        case 2:  L(out, "mov word [r11], ax");    break;
                        case 4:  L(out, "mov dword [r11], eax");  break;
                        default: L(out, "mov qword [r11], rax");  break;
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
                int base_off = slot_of(&slotmap, i->gep_base);
                int idx_off  = slot_of(&slotmap, i->gep_idx);
                L(out, "mov rax, qword %s", mem(base_off, buf));
                L(out, "mov rcx, qword %s", mem(idx_off, buf2));
                if (i->gep_scale == 1) {
                    L(out, "add rax, rcx");
                } else {
                    L(out, "imul rcx, %d", i->gep_scale);
                    L(out, "add rax, rcx");
                }
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
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
}

void codegen_x86_64_linux(IrModule* m, FILE* out) {
    fprintf(out, "; Generated by rei - target x86_64-linux (fasm)\n");
    fprintf(out, "format ELF64\n\n");
    fprintf(out, "section '.text' executable\n\n");

    SymMap sm = symmap_build(m);

    int has_main = 0;
    for (int fi = 0; fi < m->count; fi++) {
        IrFunc* f = m->funcs[fi];
        if (!f->is_extern && !f->no_mangle && strcmp(f->name, "main") == 0) {
            has_main = 1;
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
        L(out, "call " REI_FUNC_PREFIX "main");
        L(out, "leave");
        L(out, "ret");
        fprintf(out, "\n");
    }

    symmap_free(&sm);

    if (m->str_count > 0) {
        fprintf(out, "\nsection '.rodata'\n\n");
        for (int si = 0; si < m->str_count; si++) {
            fprintf(out, "__str%d:\n", si);
            fprintf(out, "  db ");
            const char*    d     = m->strings[si].data;
            size_t         n     = m->strings[si].len;
            StrPrefixFlags flags = m->strings[si].str_flags;
            for (size_t bi = 0; bi < n; bi++) {
                if (bi) fprintf(out, ", ");
                fprintf(out, "0x%02x", (unsigned char)d[bi]);
            }
            if (n == 0)
                fprintf(out, "0x00");
            bool add_null = !(flags & STR_PREFIX_B);
            if (add_null)
                fprintf(out, "%s0x00", n ? ", " : "");
            fprintf(out, "\n");
        }
        fprintf(out, "\n");
    }
}

bool codegen_x86_64_linux_compile(const char* asm_path, const char* out_path, const char* tmp_dir) {
    char obj_path[4096];
    char cmd[8192];

    snprintf(obj_path, sizeof(obj_path), "%s/codegen_tmp.o", tmp_dir);

    snprintf(cmd,
             sizeof(cmd),
             "fasm \"%s\" \"%s\" && gcc -no-pie \"%s\" -o \"%s\" && rm \"%s\"",
             asm_path,
             obj_path,
             obj_path,
             out_path,
             obj_path);

    return system(cmd) == 0;
}

CodegenTarget target_x86_64_linux = {
    .name    = "x86_64-linux",
    .emit    = codegen_x86_64_linux,
    .compile = codegen_x86_64_linux_compile,
};
