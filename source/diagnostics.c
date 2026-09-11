#include "diagnostics.h"

#include <ctype.h>
#include <elfutils/libdwfl.h>
#include <execinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int diag_error_count = 0;

static int diag_silent_mode            = 0;
static int diag_error_count_at_silence = 0;

static const char* level_str(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR:
            return "error";
        case DIAG_WARN:
            return "warning";
        case DIAG_NOTE:
            return "note";
    }
    return "?";
}

static const char* level_color(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR:
            return ANSI_RED;
        case DIAG_WARN:
            return ANSI_YELLOW;
        case DIAG_NOTE:
            return ANSI_CYAN;
    }
    return "";
}

int aprintf(FILE* stream, const char* fmt, ...) {
    int is_tty = isatty(fileno(stream));

    char        new_fmt[4096];
    char*       out        = new_fmt;
    const char* in         = fmt;
    int         ansi_count = 0;

    while (*in) {
        if (*in == '%') {
            if (*(in + 1) == 'a') {
                if (is_tty) {
                    *out++ = '%';
                    *out++ = 's';
                }
                in += 2;
                ansi_count++;
            } else if (*(in + 1) == '%') {
                *out++ = '%';
                *out++ = '%';
                in += 2;
            } else {
                *out++ = *in;
                in++;
            }
        } else {
            *out++ = *in;
            in++;
        }
    }
    *out = '\0';

    va_list args;
    va_start(args, fmt);

    if (!is_tty && ansi_count > 0) {
        char    buffer[16384];
        va_list args_copy;
        va_copy(args_copy, args);

        for (int i = 0; i < ansi_count; i++) {
            va_arg(args_copy, const char*);
        }

        int result = vfprintf(stream, new_fmt, args_copy);
        va_end(args_copy);
        va_end(args);
        return result;
    } else {
        int result = vfprintf(stream, new_fmt, args);
        va_end(args);
        return result;
    }
}

static int get_line_from_source(const char* src, int line_num, const char** line_start, const char** line_end) {
    if (!src || line_num < 1)
        return 0;

    const char* cur          = src;
    int         current_line = 1;

    while (current_line < line_num && *cur) {
        if (*cur == '\n')
            current_line++;
        cur++;
    }

    if (current_line != line_num)
        return 0;

    *line_start = cur;

    while (*cur && *cur != '\n')
        cur++;

    *line_end = cur;
    return 1;
}

static int find_token_end(const char* line_start, const char* line_end, int col_start) {
    if (col_start < 0)
        return col_start;

    const char* cur = line_start + col_start;

    while (cur < line_end && *cur != ')' && *cur != ']' && *cur != ';' && *cur != '\n' && *cur != '/') {
        cur++;
    }

    return (int)(cur - line_start);
}

static void print_source_line(const char* line_start, const char* line_end, int col_start, int col_end) {
    aprintf(stderr, "   | ");
    for (const char* p = line_start; p < line_end; p++) {
        fputc(*p, stderr);
    }
    if (line_end == line_start || *(line_end - 1) != '\n')
        fputc('\n', stderr);

    if (col_start >= 0 && col_end > col_start) {
        aprintf(stderr, "   | ");
        for (int i = 0; i < col_start; i++)
            fputc(' ', stderr);
        aprintf(stderr, "%a", ANSI_RED);
        for (int i = col_start; i < col_end; i++)
            fputc('^', stderr);
        aprintf(stderr, "%a\n", ANSI_RESET);
    }
}

void diag_emit(DiagLevel level, Location loc, const char* fmt, ...) {
    if (level == DIAG_ERROR)
        diag_error_count++;

    if (diag_silent_mode && level == DIAG_ERROR)
        return;

    aprintf(
        stderr, "%s:%d:%d: %a%s%a: ", loc.file, loc.line, loc.col, level_color(level), level_str(level), ANSI_RESET
    );
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void diag_emit_with_context(DiagLevel level, Location loc, const char* message, int col_start, int col_end) {
    if (level == DIAG_ERROR)
        diag_error_count++;

    if (diag_silent_mode && level == DIAG_ERROR)
        return;

    aprintf(stderr, "%a%s%a: %s\n", level_color(level), level_str(level), ANSI_RESET, message);
    aprintf(stderr, "  %a--> %s:%d:%d%a\n", ANSI_BOLD, loc.file, loc.line, loc.col, ANSI_RESET);

    if (loc.source) {
        const char* line_start = NULL;
        const char* line_end   = NULL;
        if (get_line_from_source(loc.source, loc.line, &line_start, &line_end)) {
            print_source_line(line_start, line_end, col_start, col_end);
        }
    }
}

static void emit_constraint_violation(
    DiagLevel   level,
    Location    call_loc,
    const char* constraint_expr_str,
    const char* constraint_evaluated,
    Location    constraint_def_loc
) {
    if (level == DIAG_ERROR)
        diag_error_count++;

    if (diag_silent_mode && level == DIAG_ERROR)
        return;

    aprintf(stderr, "%aerror%a: argument violates parameter constraint\n", ANSI_RED, ANSI_RESET);

    aprintf(stderr, "  %a--> %s:%d:%d%a\n", ANSI_BOLD, call_loc.file, call_loc.line, call_loc.col, ANSI_RESET);

    if (call_loc.source) {
        const char* line_start = NULL;
        const char* line_end   = NULL;
        if (get_line_from_source(call_loc.source, call_loc.line, &line_start, &line_end)) {
            int col_start = call_loc.col - 1;
            int col_end   = find_token_end(line_start, line_end, col_start);
            print_source_line(line_start, line_end, col_start, col_end);
        }
    }

    aprintf(stderr, "   |\n");
    aprintf(stderr, "   %a= constraint%a: %s\n", ANSI_GREEN, ANSI_RESET, constraint_expr_str);
    aprintf(stderr, "   %a= evaluated%a: %s\n", ANSI_GREEN, ANSI_RESET, constraint_evaluated);

    aprintf(stderr, "   |\n");
    aprintf(stderr, "%anote%a: constraint defined here\n", ANSI_CYAN, ANSI_RESET);
    aprintf(
        stderr,
        "  %a--> %s:%d:%d%a\n",
        ANSI_BOLD,
        constraint_def_loc.file,
        constraint_def_loc.line,
        constraint_def_loc.col,
        ANSI_RESET
    );

    if (constraint_def_loc.source) {
        const char* line_start = NULL;
        const char* line_end   = NULL;
        if (get_line_from_source(constraint_def_loc.source, constraint_def_loc.line, &line_start, &line_end)) {
            int col_start = -1;
            int col_end   = -1;

            const char* found = strstr(line_start, constraint_expr_str);
            if (found && found < line_end) {
                col_start = (int)(found - line_start);
                col_end   = col_start + (int)strlen(constraint_expr_str);
            } else {
                col_start = constraint_def_loc.col - 1;
                col_end   = find_token_end(line_start, line_end, col_start);
            }

            print_source_line(line_start, line_end, col_start, col_end);
        }
    }

    aprintf(stderr, "\n");
}

void diag_emit_specific(DiagKind kind, DiagLevel level, Location primary_loc, const char* message, ...) {
    va_list args;
    va_start(args, message);

    switch (kind) {
        case DIAG_KIND_SIMPLE:
            diag_emit(level, primary_loc, "%s", message);
            break;

        case DIAG_KIND_WITH_CONTEXT: {
            int col_start = va_arg(args, int);
            int col_end   = va_arg(args, int);
            diag_emit_with_context(level, primary_loc, message, col_start, col_end);
            break;
        }

        case DIAG_KIND_CONSTRAINT_VIOLATION: {
            const char* constraint_expr    = va_arg(args, const char*);
            const char* constraint_eval    = va_arg(args, const char*);
            Location    constraint_def_loc = va_arg(args, Location);
            emit_constraint_violation(level, primary_loc, constraint_expr, constraint_eval, constraint_def_loc);
            break;
        }
    }

    va_end(args);
}

void diag_emit_constraint_violation(
    Location    call_loc,
    const char* function_name,
    const char* constraint_expr_str,
    const char* constraint_evaluated,
    Location    constraint_def_loc
) {
    if (DIAG_ERROR == DIAG_ERROR)
        diag_error_count++;

    if (diag_silent_mode)
        return;

    aprintf(stderr, "%aerror%a: argument violates parameter constraint\n", ANSI_RED, ANSI_RESET);

    aprintf(stderr, "  %a--> %s:%d:%d%a\n", ANSI_BOLD, call_loc.file, call_loc.line, call_loc.col, ANSI_RESET);

    if (call_loc.source) {
        const char* line_start = NULL;
        const char* line_end   = NULL;
        if (get_line_from_source(call_loc.source, call_loc.line, &line_start, &line_end)) {
            print_source_line(line_start, line_end, call_loc.col - 1, call_loc.col + strlen(function_name));
        }
    }

    aprintf(stderr, "   |\n");
    aprintf(stderr, "   %a= constraint%a: %s%a\n", ANSI_GREEN, ANSI_RESET, constraint_expr_str, ANSI_RESET);
    aprintf(stderr, "   %a= evaluated%a: %s%a\n", ANSI_GREEN, ANSI_RESET, constraint_evaluated, ANSI_RESET);

    aprintf(stderr, "   |\n");
    aprintf(stderr, "%anote%a: constraint defined here in #param_constraints annotation\n", ANSI_CYAN, ANSI_RESET);
    aprintf(
        stderr,
        "  %a--> %s:%d:%d%a\n",
        ANSI_BOLD,
        constraint_def_loc.file,
        constraint_def_loc.line,
        constraint_def_loc.col,
        ANSI_RESET
    );

    if (constraint_def_loc.source) {
        const char* line_start = NULL;
        const char* line_end   = NULL;
        if (get_line_from_source(constraint_def_loc.source, constraint_def_loc.line, &line_start, &line_end)) {
            print_source_line(line_start, line_end, constraint_def_loc.col - 1, constraint_def_loc.col + 20);
        }
    }

    aprintf(stderr, "\n");
}

void diag_silence_start(void) {
    diag_silent_mode            = 1;
    diag_error_count_at_silence = diag_error_count;
}

int diag_had_error(void) {
    return diag_error_count > diag_error_count_at_silence;
}

void diag_silence_end(void) {
    diag_silent_mode = 0;
}

static Dwfl* dwfl_init(void) {
    static Dwfl_Callbacks callbacks = {
        .find_elf       = dwfl_linux_proc_find_elf,
        .find_debuginfo = dwfl_standard_find_debuginfo,
    };

    Dwfl* dwfl = dwfl_begin(&callbacks);
    if (!dwfl)
        return NULL;

    if (dwfl_linux_proc_report(dwfl, getpid()) != 0) {
        dwfl_end(dwfl);
        return NULL;
    }

    if (dwfl_report_end(dwfl, NULL, NULL) != 0) {
        dwfl_end(dwfl);
        return NULL;
    }

    return dwfl;
}

static void print_frame(Dwfl* dwfl, void* addr, const char* exe) {
    uintptr_t ua = (uintptr_t)addr;

    Dwfl_Module* mod = dwfl_addrmodule(dwfl, ua);

    if (mod) {
        const char* mod_name = dwfl_module_info(mod, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        if (mod_name && strcmp(mod_name, exe) != 0)
            return;
    }

    const char* func = mod ? dwfl_module_addrname(mod, ua) : NULL;
    if (!func)
        func = "??";
    if (strcmp(func, "_start") == 0 || strcmp(func, "__libc_start_main") == 0)
        return;

    Dwfl_Line* ln = dwfl_getsrc(dwfl, ua);
    if (ln) {
        int         lineno = 0;
        const char* src    = dwfl_lineinfo(ln, NULL, &lineno, NULL, NULL, NULL);
        fprintf(stderr, "    %-40s %s:%d\n", func ? func : "??", src ? src : "??", lineno);
    } else if (mod) {
        Dwarf_Addr  bias = 0;
        const char* name = dwfl_module_info(mod, NULL, NULL, NULL, &bias, NULL, NULL, NULL);
        fprintf(stderr, "    %-40s (%s+0x%lx)\n", func ? func : "??", name ? name : "??", (unsigned long)(ua - bias));
    }
}

void diag_ice(const char* file, int line, ...) {
    va_list args;
    va_start(args, line);
    const char* fmt = va_arg(args, const char*);

    void* frames[64];
    int   n = backtrace(frames, 64);

    char    exe[256];
    ssize_t elen = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (elen != -1)
        exe[elen] = '\0';

    Dwfl* dwfl = dwfl_init();

    for (int i = 0; i < n; i++)
        print_frame(dwfl, frames[i], elen != -1 ? exe : "");

    fprintf(stderr, "\n^^^ Backtrace\n");
    fprintf(stderr, "%s:%d: ICE: ", file, line);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(128);
}
