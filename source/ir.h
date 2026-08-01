#pragma once
#include "ast.h"

typedef int IrVal; /* -1 = no value */
#define IR_NO_VAL (-1)

typedef enum {
    IR_CONST_INT,   /* v = ival */
    IR_CONST_FLOAT, /* v = fval */
    IR_PARAM,       /* v = param[idx] */
    IR_CALL,        /* v = name(args...) */
    IR_RET,         /* return v (or IR_NO_VAL for void) */
    IR_BINOP,       /* v = lhs OP rhs */
    IR_UNOP,        /* v = OP src */
    IR_JMP,         /* unconditional jump */
    IR_JMP_IF,      /* conditional jump if src != 0 */
    IR_LABEL,       /* label for jumps */
    IR_ALLOCA,      /* v = alloca(type)  -- yields *T, stack slot address */
    IR_LOAD,        /* v = *src          -- load from pointer */
    IR_STORE,       /* *dst_ptr = src    -- store through pointer (no dst val) */
    IR_STRING,      /* v = &rodata[str_idx]  -- pointer to string literal */
    IR_GEP,         /* v = base_ptr + idx*elem_size  -- get element pointer */
    IR_ARRAY_INIT,  /* v = alloca([T;N]) -- stack array, elements filled by IR_STOREs */
} IrOpcode;

typedef struct {
    IrOpcode op;
    IrVal    dst;
    Type*    type;
    int      label; /* for IR_JMP, IR_JMP_IF, and IR_LABEL */
    union {
        long long ival;
        double    fval;
        struct {
            char*  name;
            IrVal* args;
            int    arg_count;
        } call;
        IrVal src;
        int   param_idx;
        struct {
            BinOp bop;
            IrVal blhs, brhs;
        };
        struct {
            UnOp  uop;
            IrVal usrc;
        };
        struct {
            IrVal store_ptr;
            IrVal store_val;
        };
        struct {
            IrVal load_ptr;
        };
        struct {
            IrVal gep_base;
            IrVal gep_idx;
            int   gep_scale;
        };
        int str_idx;
        int alloca_slots;
    };
} IrInstr;

typedef struct {
    char*    name;
    Param*   params;
    int      param_count;
    Type*    ret_type;
    IrInstr* instrs;
    int      instr_count;
    int      instr_cap;
    int      next_val;
    bool     is_extern;
    bool     no_mangle;
} IrFunc;

typedef struct {
    IrFunc** funcs;
    int      count;
    int      cap;
    struct IrStringEntry {
        char*          data;
        size_t         len;
        StrPrefixFlags str_flags;
    }* strings;
    int str_count;
    int str_cap;
} IrModule;

IrModule* ir_lower(Module* ast);
void      ir_dump(IrModule* m);
