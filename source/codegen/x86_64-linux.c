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

static void emit_func(IrFunc* f, SymMap* sm, FILE* out) {
    fprintf(out, "%s:\n", symmap_lookup(sm, f->name));
    L(out, "push rbp");
    L(out, "mov rbp, rsp");

    SlotMap slotmap = {0};
    for (int ii = 0; ii < f->instr_count; ii++)
        if (f->instrs[ii].dst != IR_NO_VAL)
            alloc_slot(&slotmap, f->instrs[ii].dst);

    if (slotmap.stack_size) {
        int aligned = (slotmap.stack_size + 15) & ~15;
        L(out, "sub rsp, %d", aligned);
    }

    char buf[32], buf2[32];

    for (int ii = 0; ii < f->instr_count; ii++) {
        IrInstr* i        = &f->instrs[ii];
        int      is_float = i->type && i->type->kind == TYPE_FLOAT;

        switch (i->op) {
            case IR_CONST_INT:
                L(out, "mov rax, %lld", i->ival);
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                break;

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

            case IR_PARAM:
                if (i->param_idx < MAX_ARGREGS)
                    L(out, "mov qword %s, %s", mem(slot_of(&slotmap, i->dst), buf), ARGREGS[i->param_idx]);
                else {
                    int stack_off = 16 + (i->param_idx - MAX_ARGREGS) * 8;
                    L(out, "mov rax, qword [rbp + %d]", stack_off);
                    L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                }
                break;

            case IR_CALL:
                for (int a = 0; a < i->call.arg_count && a < MAX_ARGREGS; a++)
                    L(out, "mov %s, qword %s", ARGREGS[a], mem(slot_of(&slotmap, i->call.args[a]), buf));
                /* TODO: push stack args for >6 */
                L(out, "call %s", symmap_lookup(sm, i->call.name));
                if (i->dst != IR_NO_VAL) {
                    if (is_float)
                        L(out, "movq rax, xmm0");
                    L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                }
                break;

            case IR_BINOP:
                L(out, "mov rax, qword %s", mem(slot_of(&slotmap, i->blhs), buf));
                L(out, "mov rcx, qword %s", mem(slot_of(&slotmap, i->brhs), buf2));
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
                    default:
                        ICE("unhandled binop in codegen");
                        break;
                }
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                break;

            case IR_UNOP:
                L(out, "mov rax, qword %s", mem(slot_of(&slotmap, i->usrc), buf));
                switch (i->uop) {
                    case UOP_NEG:
                        L(out, "neg rax");
                        break;
                    case UOP_POS:
                        break; /* no-op */
                }
                L(out, "mov qword %s, rax", mem(slot_of(&slotmap, i->dst), buf));
                break;

            case IR_RET:
                if (i->src != IR_NO_VAL) {
                    if (is_float) {
                        L(out, "movq xmm0, qword %s", mem(slot_of(&slotmap, i->src), buf));
                    } else {
                        L(out, "mov rax, qword %s", mem(slot_of(&slotmap, i->src), buf));
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