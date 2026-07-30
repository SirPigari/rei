#include "codegen.h"

#include "diagnostics.h"

#include <string.h>

CodegenTarget* codegen_find_target(const char* name) {
    if (strcmp(name, "x86_64-linux") == 0)
        return &target_x86_64_linux;
    return NULL;
}

void codegen_emit(IrModule* m, CodegenTarget* t, FILE* out) {
    t->emit(m, out);
}
