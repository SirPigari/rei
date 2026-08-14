#include "diagnostics.h"

#include <execinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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

void diag_emit(DiagLevel level, Location loc, const char* fmt, ...) {
    if (level == DIAG_ERROR)
        diag_error_count++;

    if (diag_silent_mode && level == DIAG_ERROR)
        return;

    fprintf(stderr, "%s:%d:%d: %s: ", loc.file, loc.line, loc.col, level_str(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
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

void diag_ice(const char* file, int line, ...) {
    va_list args;
    va_start(args, line);

    const char* fmt = va_arg(args, const char*);

    void* frames[64];
    int   n = backtrace(frames, 64);

    char** symbols = backtrace_symbols(frames, n);
    if (symbols) {
        for (int i = 0; i < n; i++)
            fprintf(stderr, "    %s\n", symbols[i]);

        free(symbols);
    }

    fprintf(stderr, "^^^ Backtrace\n");
    fprintf(stderr, "%s:%d: Internal Compiler Error (ICE):\n", file, line);

    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    va_end(args);

    exit(128);
}
