#include "diagnostics.h"

#include <elfutils/libdwfl.h>
#include <execinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int diag_error_count = 0;

static int diag_silent_mode            = 0;
static int diag_error_count_at_silence = 0;

static const char* level_str(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR: return "error";
        case DIAG_WARN:  return "warning";
        case DIAG_NOTE:  return "note";
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
        fprintf(stderr, "    %-40s %s:%d\n",
                func ? func : "??",
                src  ? src  : "??",
                lineno);
    } else if (mod) {
        Dwarf_Addr  bias = 0;
        const char* name = dwfl_module_info(mod, NULL, NULL, NULL, &bias, NULL, NULL, NULL);
        fprintf(stderr, "    %-40s (%s+0x%lx)\n",
                func ? func : "??",
                name ? name : "??",
                (unsigned long)(ua - bias));
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
    if (elen != -1) exe[elen] = '\0';

    Dwfl* dwfl = dwfl_init();

    for (int i = 0; i < n; i++)
        print_frame(dwfl, frames[i], elen != -1 ? exe : "");

    fprintf(stderr, "\n^^^ Backtrace:\n");
    fprintf(stderr, "%s:%d: ICE: ", file, line);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(128);
}
