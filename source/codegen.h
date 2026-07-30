#pragma once
#include "ir.h"

#include <stdio.h>

typedef struct CodegenTarget {
    const char* name;
    void (*emit)(IrModule* m, FILE* out);
    bool (*compile)(const char* asm_path, const char* out_path, const char* tmp_dir);
} CodegenTarget;

extern CodegenTarget target_x86_64_linux;

CodegenTarget* codegen_find_target(const char* name);
void           codegen_emit(IrModule* m, CodegenTarget* t, FILE* out);
