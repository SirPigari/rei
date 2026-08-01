#define NOB_EXPERIMENTAL_DELETE_OLD
#define NOB_IMPLEMENTATION
#define NOB_NO_ECHO
#include "./thirdparty/nob.h"

#define SOURCE  "./source/"
#define CODEGEN "./source/codegen/"
#define BUILD   "./build/"
#define OUTPUT  "./rei"

typedef struct {
    const char* file;
    const char* deps[16];
    size_t      dep_count;
    const char* flag;
} Build_File;

#define DEPS(...) {__VA_ARGS__}, (sizeof((const char*[]){__VA_ARGS__}) / sizeof(const char*))

Build_File files[] = {
    {SOURCE "main.c",
     DEPS(SOURCE "ast.h",
          SOURCE "codegen.h",
          SOURCE "diagnostics.h",
          SOURCE "ir.h",
          SOURCE "lexer.h",
          SOURCE "parser.h",
          SOURCE "semantic.h",
          SOURCE "../thirdparty/ht.h")},
    {SOURCE "ast.c", DEPS(SOURCE "ast.h")},
    {SOURCE "lexer.c", DEPS(SOURCE "lexer.h")},
    {SOURCE "parser.c", DEPS(SOURCE "parser.h")},
    {SOURCE "diagnostics.c", DEPS(SOURCE "diagnostics.h")},
    {SOURCE "semantic.c", DEPS(SOURCE "semantic.h", SOURCE "../thirdparty/ht.h")},
    {SOURCE "ir.c", DEPS(SOURCE "ir.h")},
    {SOURCE "codegen.c", DEPS(SOURCE "codegen.h")},
    {CODEGEN "x86_64-linux.c", DEPS(CODEGEN "x86_64-linux.h", CODEGEN "../../thirdparty/ht.h")},
};

static const char* obj_path(const char* src) {
    return nob_temp_sprintf(BUILD "%s.o", nob_path_name(src));
}

static bool needs_rebuild2(const char* obj, Build_File* file) {
    if (nob_needs_rebuild1(obj, file->file) > 0)
        return true;

    for (size_t i = 0; i < file->dep_count; i++) {
        if (nob_needs_rebuild1(obj, file->deps[i]) > 0)
            return true;
    }

    return false;
}

int main(int argc, char** argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    int  run_idx = -1;
    bool rebuild = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-B") == 0) {
            rebuild = true;
        }

        if (strcmp(argv[i], "run") == 0) {
            run_idx = i;
            break;
        }
    }

    if (!nob_mkdir_if_not_exists(BUILD))
        return 1;

    bool      ok    = true;
    Nob_Procs procs = {0};
    Cmd       cmd   = {0};

    for (size_t i = 0; i < NOB_ARRAY_LEN(files); i++) {
        const char* src = files[i].file;
        const char* obj = obj_path(src);

        if (!rebuild && !needs_rebuild2(obj, &files[i]))
            continue;

        cmd_append(&cmd, "gcc", "-rdynamic", "-c", "-o", obj, src);

        if (!nob_cmd_run(&cmd, .async = &procs)) {
            ok = false;
            break;
        }
    }

    if (!nob_procs_flush(&procs) || !ok)
        return 1;

    const char* objs[NOB_ARRAY_LEN(files)];

    for (size_t i = 0; i < NOB_ARRAY_LEN(files); i++)
        objs[i] = obj_path(files[i].file);

    if (nob_needs_rebuild(OUTPUT, objs, NOB_ARRAY_LEN(files)) > 0) {
        cmd_append(&cmd, "gcc", "-rdynamic", "-o", OUTPUT, "-lm");

        for (size_t i = 0; i < NOB_ARRAY_LEN(files); i++)
            cmd_append(&cmd, objs[i]);

        if (!nob_cmd_run(&cmd))
            return 1;
    } else {
        nob_log(NOB_INFO, "%s is up to date", OUTPUT);
    }

    if (run_idx != -1) {
        cmd_append(&cmd, OUTPUT);

        for (int i = run_idx + 1; i < argc; i++)
            cmd_append(&cmd, argv[i]);

        if (!nob_cmd_run(&cmd))
            return 1;
    }

    return 0;
}