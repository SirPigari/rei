#pragma once
#include "ir.h"

#include <stdio.h>

typedef struct {
    bool compile_only; /* -c: stop after object file */
    bool asm_only;     /* -S: stop after assembly */
    bool make_lib;     /* --lib: create shared library */
    bool make_static;  /* --static-lib: create static library */
    bool no_libm;      /* --no-libm: don't link against libm (math library) */
} CompileOptions;

typedef struct CodegenTarget {
    const char* name;
    void (*emit)(IrModule* m, FILE* out);
    bool (*compile)(const char* asm_path, const char* out_path, const char* tmp_dir, const CompileOptions* opts);
} CodegenTarget;

extern CodegenTarget target_x86_64_linux;

CodegenTarget* codegen_find_target(const char* name);
void           codegen_emit(IrModule* m, CodegenTarget* t, FILE* out);
