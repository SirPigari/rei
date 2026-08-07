#pragma once
#include <stddef.h>

typedef struct {
    const char* file;
    int         line;
    int         col;
} Location;

typedef enum {
    DIAG_ERROR,
    DIAG_WARN,
    DIAG_NOTE,
} DiagLevel;

void diag_emit(DiagLevel level, Location loc, const char* fmt, ...);
void diag_ice(const char* file, int line, ...); /* internal compiler error */

#define ICE(...) diag_ice(__FILE__, __LINE__, __VA_ARGS__)

extern int diag_error_count;
