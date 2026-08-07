#pragma once
#include "ast.h"

typedef struct {
    bool no_main;     /* skip main function checking */
    bool no_rei_main; /* treat main as C main instead of rei__main */
    bool is_library;  /* building a library, main not required */
} CompileConfig;

int semantic_check(Module* m, const CompileConfig* config);
