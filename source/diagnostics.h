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
void diag_ice(const char* file, int line, const char* msg); /* internal compiler error */

#define ICE(msg) diag_ice(__FILE__, __LINE__, msg)

extern int diag_error_count;
