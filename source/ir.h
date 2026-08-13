#pragma once
#include "ast.h"
#include "diagnostics.h"

typedef int IrVal; /* -1 = no value */
#define IR_NO_VAL (-1)

typedef enum {
    IR_CONST_INT,   /* v = ival */
    IR_CONST_FLOAT, /* v = fval */
    IR_NULLPTR,     /* v = nullptr (target-defined) */
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
    IR_FAT_PTR,     /* v = fat.ptr       -- extract data pointer from fat pointer alloca */
    IR_FAT_LEN,     /* v = fat.len       -- extract length from fat pointer alloca */
    IR_FAT_SET_LEN, /* fat.len = val     -- store length into fat pointer alloca */
    IR_CAST,        /* v = (type)src     -- numeric type conversion */
    IR_SELECT,      /* v = cond ? true_val : false_val  -- ternary select */
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
            Type** arg_types;
            int    arg_count;
        } call;
        IrVal src;
        int   param_idx;
        struct {
            BinOp bop;
            IrVal blhs, brhs;
            Type* lhs_type;
            Type* rhs_type;
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
            Type* gep_base_type; /* Type of the base value (may be fat pointer) */
        };
        int str_idx;
        int alloca_slots;
        struct {
            IrVal cast_src;
            Type* cast_from_type; /* source type; i->type is the destination type */
        };
        struct {
            IrVal sel_cond;
            IrVal sel_true_val;
            IrVal sel_false_val;
        };
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
    bool     is_public;
    bool     no_mangle;
} IrFunc;

typedef enum {
    MAIN_FUNC_NO_FUNC,      /* no main func */
    MAIN_FUNC_NONE,         /* main func with no params */
    MAIN_FUNC_FAT_FAT,      /* main func with [][]u8 */
    MAIN_FUNC_FAT_THIN,     /* main func with []*u8 */
    MAIN_FUNC_CRT0,         /* main func with argc argv */
    MAIN_FUNC_CRT0_SWAPPED, /* main func with argv argc*/
    /* for crt0 params get the actual type from param_types[] */
} MainFuncParams;

typedef struct {
    char*          name;
    MainFuncParams params;
    IrFunc*        func;
    Type*          param_types[2]; /* param types, if NULL then none for that field */
    Type*          ret_type;       /* return type */
} MainFuncInfo;

typedef struct {
    IrFunc** funcs;
    int      count;
    int      cap;
    struct IrStringEntry {
        char*          data;
        size_t         len;
        StrPrefixFlags str_flags;
    }*           strings;
    int          str_count;
    int          str_cap;
    MainFuncInfo rei_main;
    int          next_label;
} IrModule;

IrModule* ir_lower(Module* ast);
void      ir_dump(IrModule* m);
