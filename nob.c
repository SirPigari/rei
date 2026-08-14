#define NOB_EXPERIMENTAL_DELETE_OLD
#define NOB_IMPLEMENTATION
#define NOB_NO_ECHO
#include "./thirdparty/nob.h"

#define SOURCE     "./source/"
#define CODEGEN    "./source/codegen/"
#define THIRDPARTY "./thirdparty/"
#define BUILD      "./build/"
#define DEPS_DIR   "./build/deps/"
#define OUTPUT     "./rei"

const char* source_files[] = {
    SOURCE "main.c",
    SOURCE "ast.c",
    SOURCE "lexer.c",
    SOURCE "parser.c",
    SOURCE "diagnostics.c",
    SOURCE "semantic.c",
    SOURCE "ir.c",
    SOURCE "codegen.c",
    CODEGEN "x86_64-linux.c",
};

static const char* obj_path(const char* src) {
    return nob_temp_sprintf(BUILD "%s.o", nob_path_name(src));
}

static const char* dep_path(const char* src) {
    return nob_temp_sprintf(DEPS_DIR "%s.d", nob_path_name(src));
}

static bool needs_rebuild_from_deps(const char* obj, const char* dep_file) {
    Nob_String_Builder content = {0};
    if (!nob_read_entire_file(dep_file, &content)) {
        return true;
    }

    nob_sb_append_null(&content);
    const char* line = content.items;

    while (*line && *line != ':')
        line++;
    if (!*line) {
        nob_sb_free(content);
        return true;
    }
    line++;

    while (*line) {
        while (*line && (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\\' || *line == '\r')) {
            line++;
        }
        if (!*line)
            break;

        const char* dep_start = line;
        while (*line && *line != ' ' && *line != '\t' && *line != '\n' && *line != '\\' && *line != '\r') {
            line++;
        }
        size_t dep_len = line - dep_start;

        char* dep = nob_temp_strndup(dep_start, dep_len);

        if (nob_needs_rebuild1(obj, dep) > 0) {
            nob_sb_free(content);
            return true;
        }
    }

    nob_sb_free(content);
    return false;
}

static void print_help(void) {
    printf("Usage: nob [OPTIONS] [TARGET] [ARGS]\n");
    printf("\n");
    printf("Targets:\n");
    printf("  (default)    Build the project\n");
    printf("  run          Build and run the project\n");
    printf("  clean        Remove all build artifacts\n");
    printf("\n");
    printf("Options:\n");
    printf("  -B           Force rebuild all files\n");
    printf("  -j JOBS      Maximum concurrent compilations (default: 1)\n");
    printf("  -h, --help   Show this help message\n");
}

static bool delete_file_recursive(Nob_Walk_Entry entry) {
    if (entry.type == NOB_FILE_REGULAR || entry.type == NOB_FILE_DIRECTORY) {
        nob_delete_file(entry.path);
    }
    return true;
}

static void clean_build(void) {
    nob_log(NOB_INFO, "Cleaning build directory...");
    nob_walk_dir(BUILD, delete_file_recursive, .post_order = true);
    nob_log(NOB_INFO, "Clean complete");
}

int main(int argc, char** argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    int  run_idx   = -1;
    bool rebuild   = false;
    int  max_procs = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-B") == 0) {
            rebuild = true;
        } else if (strcmp(argv[i], "-j") == 0) {
            if (i + 1 < argc) {
                max_procs = atoi(argv[i + 1]);
                if (max_procs < 1)
                    max_procs = 1;
                i++;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        } else if (strcmp(argv[i], "clean") == 0) {
            clean_build();
            return 0;
        } else if (strcmp(argv[i], "run") == 0) {
            run_idx = i;
            break;
        } else {
            print_help();
            return 1;
        }
    }

    if (!nob_mkdir_if_not_exists(BUILD))
        return 1;
    if (!nob_mkdir_if_not_exists(DEPS_DIR))
        return 1;

    bool      ok    = true;
    Nob_Procs procs = {0};
    Cmd       cmd   = {0};

    for (size_t i = 0; i < NOB_ARRAY_LEN(source_files); i++) {
        const char* src = source_files[i];
        const char* obj = obj_path(src);
        const char* dep = dep_path(src);

        bool should_rebuild = rebuild || nob_needs_rebuild1(obj, src) > 0;

        if (!should_rebuild && nob_file_exists(obj)) {
            should_rebuild = needs_rebuild_from_deps(obj, dep);
        } else if (!should_rebuild && !nob_file_exists(obj)) {
            should_rebuild = true;
        }

        if (!should_rebuild)
            continue;

        cmd_append(&cmd, "gcc", "-rdynamic", "-c", "-o", obj, "-MMD", "-MF", dep, "-MT", obj, src);

        if (!nob_cmd_run(&cmd, .async = &procs)) {
            ok = false;
            break;
        }

        if ((int)procs.count >= max_procs) {
            if (!nob_procs_flush(&procs)) {
                ok = false;
                break;
            }
        }
    }

    if (!nob_procs_flush(&procs) || !ok)
        return 1;

    bool need_relink = !nob_file_exists(OUTPUT);

    if (!need_relink) {
        for (size_t i = 0; i < NOB_ARRAY_LEN(source_files); i++) {
            const char* obj = obj_path(source_files[i]);
            if (nob_needs_rebuild1(OUTPUT, obj) > 0) {
                need_relink = true;
                break;
            }
        }
    }

    if (need_relink) {
        cmd_append(&cmd, "gcc", "-rdynamic", "-o", OUTPUT, "-lm");

        for (size_t i = 0; i < NOB_ARRAY_LEN(source_files); i++)
            cmd_append(&cmd, obj_path(source_files[i]));

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
