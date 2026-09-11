#pragma once
#include <stddef.h>
#include <stdio.h>

#define ANSI_RED    "\x1b[31m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_BLUE   "\x1b[34m"
#define ANSI_CYAN   "\x1b[36m"
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_BOLD   "\x1b[1m"
#define ANSI_RESET  "\x1b[0m"

typedef struct {
    const char* file;
    const char* source;
    int         line;
    int         col;
} Location;

typedef enum {
    DIAG_ERROR,
    DIAG_WARN,
    DIAG_NOTE,
} DiagLevel;

typedef enum {
    DIAG_KIND_SIMPLE,               /* basic error/warning message */
    DIAG_KIND_WITH_CONTEXT,         /* error with code snippet highlighting */
    DIAG_KIND_CONSTRAINT_VIOLATION, /* constraint violation with primary & secondary locations */
} DiagKind;

int aprintf(FILE* stream, const char* fmt, ...);

void diag_emit(DiagLevel level, Location loc, const char* fmt, ...);

void diag_emit_with_context(
    DiagLevel   level,
    Location    loc,
    const char* message,
    int         col_start, /* 0-indexed */
    int         col_end    /* exclusive */
);

void diag_emit_specific(
    DiagKind    kind,
    DiagLevel   level,
    Location    primary_loc,
    const char* message,
    /* Additional args depend on kind:
       DIAG_KIND_SIMPLE: none
       DIAG_KIND_WITH_CONTEXT: int col_start, int col_end
       DIAG_KIND_CONSTRAINT_VIOLATION: const char* constraint_expr, const char* constraint_eval,
                                        Location constraint_def_loc
    */
    ...
);

void diag_ice(const char* file, int line, ...); /* internal compiler error */

void diag_silence_start(void);
int  diag_had_error(void);
void diag_silence_end(void);

#define ICE(...) diag_ice(__FILE__, __LINE__, __VA_ARGS__)

extern int diag_error_count;
