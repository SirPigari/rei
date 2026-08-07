#pragma once
#include "../codegen.h"
#include "../ir.h"

#include <stdio.h>

void codegen_x86_64_linux(IrModule* m, FILE* out);
bool codegen_x86_64_linux_compile(const char*           asm_path,
                                  const char*           out_path,
                                  const char*           tmp_dir,
                                  const CompileOptions* opts);
