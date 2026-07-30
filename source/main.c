#include "ast.h"
#include "codegen.h"
#include "diagnostics.h"
#include "ir.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HT_IMPLEMENTATION
#include "../thirdparty/ht.h"

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char* buf = malloc(sz + 1);
    if (!buf)
        exit(1);

    fread(buf, 1, sz, f);
    buf[sz] = '\0';

    fclose(f);
    return buf;
}

static void usage(FILE* stream, const char* argv0) {
    fprintf(stream,
            "Usage: %s [options] <source.rei>\n"
            "Options:\n"
            "  -o <file>          output file\n"
            "  -t <target>        codegen target (default: x86_64-linux)\n"
            "  --dump-ast         print AST and stop\n"
            "  --dump-ir          print IR and stop\n"
            "  --run, -r          run compiled output\n"
            "  --help, -h         show this message\n",
            argv0);
    exit(1);
}

static void default_output_path(const char* src_path, char* out, size_t size) {
    const char* dot = strrchr(src_path, '.');
    size_t      len = dot ? (size_t)(dot - src_path) : strlen(src_path);

#ifdef _WIN32
    const char* ext = ".exe";
#else
    const char* ext = "";
#endif

    if (len + strlen(ext) + 1 >= size)
        exit(1);

    memcpy(out, src_path, len);
    out[len] = '\0';
    strcat(out, ext);
}

static bool make_tmp_dir(char* tmp, size_t size) {
    if (size < 32)
        return false;

    snprintf(tmp, size, "/tmp/rei-XXXXXX");

    return mkdtemp(tmp) != NULL;
}

static bool make_abs_path(const char* path, char* out, size_t size) {
    char* abs = realpath(path, NULL);

    if (!abs)
        return false;

    strncpy(out, abs, size - 1);
    out[size - 1] = '\0';

    free(abs);
    return true;
}

static bool make_abs_output_path(const char* path, char* out, size_t size) {
    if (path[0] == '/')
        return snprintf(out, size, "%s", path) < (int)size;

    char cwd[PATH_MAX];

    if (!getcwd(cwd, sizeof(cwd)))
        return false;

    return snprintf(out, size, "%s/%s", cwd, path) < (int)size;
}

int main(int argc, char** argv) {
    const char* src_path    = NULL;
    const char* out_path    = NULL;
    const char* target_name = "x86_64-linux";

    bool dump_ast = false;
    bool dump_ir  = false;
    bool dump_asm = false;
    bool run      = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            target_name = argv[++i];
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            dump_ast = true;
        } else if (strcmp(argv[i], "--dump-ir") == 0) {
            dump_ir = true;
        } else if (strcmp(argv[i], "--dump-asm") == 0) {
            dump_asm = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
        } else if (strcmp(argv[i], "--run") == 0 || strcmp(argv[i], "-r") == 0) {
            run = true;
        } else if (argv[i][0] != '-') {
            src_path = argv[i];
        } else {
            usage(stderr, argv[0]);
        }
    }

    if (!src_path)
        usage(stderr, argv[0]);

    char abs_src[PATH_MAX];
    char abs_out[PATH_MAX];
    char out_buf[PATH_MAX];

    if (!make_abs_path(src_path, abs_src, sizeof(abs_src))) {
        perror(src_path);
        return 1;
    }

    src_path = abs_src;

    if (!out_path) {
        default_output_path(src_path, out_buf, sizeof(out_buf));
        out_path = out_buf;
    }

    if (!make_abs_output_path(out_path, abs_out, sizeof(abs_out))) {
        fprintf(stderr, "invalid output path\n");
        return 1;
    }

    out_path = abs_out;

    char* src = read_file(src_path);

    Lexer*  lexer  = lexer_new(src, src_path);
    Module* module = parse(lexer);

    if (diag_error_count)
        goto done;

    if (dump_ast) {
        ast_dump(module);
        goto done;
    }

    if (semantic_check(module) < 0)
        goto done;

    IrModule* ir = ir_lower(module);

    if (dump_ir) {
        ir_dump(ir);
        goto done;
    }

    CodegenTarget* target = codegen_find_target(target_name);

    if (!target) {
        fprintf(stderr, "unknown target '%s'\n", target_name);
        goto done;
    }

    char tmp_dir[PATH_MAX];

    if (!make_tmp_dir(tmp_dir, sizeof(tmp_dir))) {
        fprintf(stderr, "failed to create temporary directory\n");
        goto done;
    }

    char asm_path[PATH_MAX];

    snprintf(asm_path, sizeof(asm_path), "%s/output.asm", tmp_dir);

    FILE* asm_file = fopen(asm_path, "w");

    if (!asm_file) {
        perror(asm_path);
        goto done;
    }

    target->emit(ir, asm_file);

    fclose(asm_file);

    if (dump_asm) {
        asm_file = fopen(asm_path, "r");
        if (!asm_file) {
            perror(asm_path);
            goto done;
        }

        char buf[4096];
        while (fgets(buf, sizeof(buf), asm_file)) {
            fputs(buf, stdout);
        }
        fclose(asm_file);
    }

    if (!target->compile(asm_path, out_path, tmp_dir)) {
        fprintf(stderr, "compile failed\n");
        goto done;
    }

    fprintf(stderr, "wrote %s\n", out_path);

    if (run) {
        char cmd[PATH_MAX + 4];

        snprintf(cmd, sizeof(cmd), "'%s'", out_path);

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        fprintf(stderr, "================================\n", out_path);
        int ret = system(cmd);
        fprintf(stderr, "\n================================\n", out_path);

        clock_gettime(CLOCK_MONOTONIC, &end);

        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

        fprintf(stderr, "execution time: %.3f seconds\n", elapsed);

        if (ret < 0) {
            perror("run");
            goto done;
        }

        if (WIFEXITED(ret)) {
            fprintf(stderr, "program exited with code %d\n", WEXITSTATUS(ret));
        } else if (WIFSIGNALED(ret)) {
            fprintf(stderr, "program terminated by signal %d\n", WTERMSIG(ret));
        } else {
            fprintf(stderr, "program terminated abnormally\n");
        }
    }

    char cleanup[PATH_MAX + 16];

    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", tmp_dir);

    system(cleanup);

done:
    lexer_free(lexer);
    free(src);

    return diag_error_count ? 1 : 0;
}
