#include "semantic.h"

#include "../thirdparty/ht.h"
#include "diagnostics.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool types_equal(Type* a, Type* b) {
    if (a == b)
        return true;
    if (!a || !b)
        return a == b;
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
        case TYPE_VOID:
            return true;
        case TYPE_INT:
            if (a->int_type.is_abstract != b->int_type.is_abstract)
                return false;
            if (a->int_type.is_size != b->int_type.is_size)
                return false;
            bool result = a->bits == b->bits && a->int_type.is_unsigned == b->int_type.is_unsigned;
            return result;
        case TYPE_FLOAT:
            if (a->int_type.is_size != b->int_type.is_size)
                return false;
            return a->bits == b->bits;
        case TYPE_PTR:
            return a->ptr_type.is_fat == b->ptr_type.is_fat && a->ptr_type.non_null == b->ptr_type.non_null &&
                   types_equal(a->ptr_type.elem_type, b->ptr_type.elem_type);
        case TYPE_ARRAY:
            return a->array_type.len == b->array_type.len &&
                   types_equal(a->array_type.elem_type, b->array_type.elem_type);
        case TYPE_IDENT:
            return strcmp(a->ident_type.name, b->ident_type.name) == 0;
        case TYPE_UNSUPPORTED:
            return false;
        default:
            ICE("unknown type kind %d", a->kind);
            return false;
    }
    return false;
}

static bool is_abstract_int(Type* t) {
    return t && t->kind == TYPE_INT && t->int_type.is_abstract;
}

static char* type_str(Type* t, char* buf, size_t cap) {
    if (!t) {
        snprintf(buf, cap, "?");
        return buf;
    }
    switch (t->kind) {
        case TYPE_VOID:
            snprintf(buf, cap, "void");
            break;
        case TYPE_INT: {
            if (t->int_type.is_abstract) {
                snprintf(buf, cap, "i<abstract>");
            } else if (t->int_type.is_size) {
                if (t->int_type.is_unsigned)
                    snprintf(buf, cap, "usize");
                else
                    snprintf(buf, cap, "isize");
            } else {
                snprintf(buf, cap, "%s%d", t->int_type.is_unsigned ? "u" : "i", t->bits ? t->bits : 64);
            }
            break;
        }
        case TYPE_FLOAT: {
            if (t->int_type.is_size) {
                snprintf(buf, cap, "fsize");
            } else {
                snprintf(buf, cap, "f%d", t->bits ? t->bits : 64);
            }
            break;
        }
        case TYPE_PTR: {
            char inner[64];
            type_str(t->ptr_type.elem_type, inner, sizeof(inner));
            if (t->ptr_type.is_fat) {
                snprintf(buf, cap, "[]%s%s", t->ptr_type.non_null ? "!" : "", inner);
            } else {
                snprintf(buf, cap, "*%s%s", t->ptr_type.non_null ? "!" : "", inner);
            }
            break;
        }
        case TYPE_ARRAY: {
            char inner[64];
            type_str(t->array_type.elem_type, inner, sizeof(inner));
            if (t->array_type.len)
                snprintf(buf, cap, "[%s; %zu]", inner, t->array_type.len);
            else
                snprintf(buf, cap, "[%s]", inner);
            break;
        }
        case TYPE_IDENT:
            snprintf(buf, cap, "%s", t->ident_type.name);
            break;
        case TYPE_NEVER:
            snprintf(buf, cap, "never");
            break;
        case TYPE_UNSUPPORTED:
            snprintf(buf, cap, "<unsupported>");
            break;
        default:
            ICE("unknown type kind %d", t->kind);
            break;
    }
    return buf;
}

static bool is_numeric(Type* t) {
    if (!t)
        return false;
    return t->kind == TYPE_INT || t->kind == TYPE_FLOAT;
}

static bool is_integer(Type* t) {
    if (!t)
        return false;
    return t->kind == TYPE_INT;
}

static bool is_pointer(Type* t) {
    return t && t->kind == TYPE_PTR;
}

static bool types_compatible_numeric(Type* a, Type* b) {
    if (!a || !b)
        return false;
    if (a->kind != b->kind)
        return false;
    return (a->kind == TYPE_INT || a->kind == TYPE_FLOAT);
}

static bool types_compatible_with_decay(Type* actual, Type* expected) {
    if (types_equal(actual, expected))
        return true;

    if (actual && actual->kind == TYPE_ARRAY && expected && expected->kind == TYPE_PTR) {
        return types_equal(actual->array_type.elem_type, expected->ptr_type.elem_type) && !expected->ptr_type.is_fat;
    }

    return false;
}

static bool types_ptr_compatible(Type* actual, Type* expected) {
    if (!actual || !expected)
        return false;

    if (actual->kind != TYPE_PTR || expected->kind != TYPE_PTR)
        return false;

    if (actual->ptr_type.is_fat != expected->ptr_type.is_fat)
        return false;

    if (expected->ptr_type.non_null && !actual->ptr_type.non_null)
        return false;

    Type* void_type        = type_void();
    bool  actual_is_void   = types_equal(actual->ptr_type.elem_type, void_type);
    bool  expected_is_void = types_equal(expected->ptr_type.elem_type, void_type);

    if (actual_is_void || expected_is_void)
        return true;

    return types_equal(actual->ptr_type.elem_type, expected->ptr_type.elem_type);
}

static bool is_thin_ptr_u8(Type* t) {
    if (!t || t->kind != TYPE_PTR || t->ptr_type.is_fat)
        return false;
    Type* e = t->ptr_type.elem_type;
    return e && e->kind == TYPE_INT && e->bits == 8 && e->int_type.is_unsigned;
}

static bool is_fat_ptr_u8(Type* t) {
    if (!t || t->kind != TYPE_PTR || !t->ptr_type.is_fat)
        return false;
    Type* e = t->ptr_type.elem_type;
    return e && e->kind == TYPE_INT && e->bits == 8 && e->int_type.is_unsigned;
}

static void diag_type_hint(Type* actual, Type* expected, Location arg_loc) {
    if (!actual || !expected)
        return;

    if (is_fat_ptr_u8(actual) && is_thin_ptr_u8(expected)) {
        diag_emit(DIAG_NOTE, arg_loc, "plain string literals have type []u8 (a fat pointer / slice);");
        diag_emit(DIAG_NOTE, arg_loc, "use the c\"...\" prefix to get a *u8 (null-terminated C string)");
        return;
    }

    if (is_thin_ptr_u8(actual) && is_fat_ptr_u8(expected)) {
        diag_emit(
            DIAG_NOTE,
            arg_loc,
            "c\"...\" string literals have type *u8 (null-terminated C string); "
            "use a plain \"...\" literal to get []u8 (a fat pointer / slice)"
        );
        return;
    }

    if (actual->kind == TYPE_PTR && expected->kind == TYPE_PTR) {
        Type* actual_inner   = actual->ptr_type.elem_type;
        Type* expected_inner = expected->ptr_type.elem_type;

        if (expected_inner && expected_inner->kind == TYPE_PTR && types_equal(actual, expected_inner)) {
            char ts[64];
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "you have %s but %s is required; "
                "consider taking the address with &x to get an extra level of indirection",
                type_str(actual, ts, sizeof(ts)),
                type_str(expected, ts, sizeof(ts))
            );
            return;
        }

        if (actual_inner && actual_inner->kind == TYPE_PTR && types_equal(actual_inner, expected)) {
            char as[64], es[64];
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "you have %s but %s is required; "
                "consider dereferencing with *x to remove one level of indirection",
                type_str(actual, as, sizeof(as)),
                type_str(expected, es, sizeof(es))
            );
            return;
        }

        if (actual->ptr_type.is_fat && !expected->ptr_type.is_fat && actual_inner &&
            types_equal(actual_inner, expected_inner)) {
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "fat pointer (slice) cannot coerce to thin pointer automatically; "
                "use &x[0] to get a pointer to the first element"
            );
            return;
        }

        if (!actual->ptr_type.is_fat && expected->ptr_type.is_fat && actual_inner &&
            types_equal(actual_inner, expected_inner)) {
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "thin pointer cannot coerce to a fat pointer (slice) automatically; "
                "wrap it in a slice expression or change the declaration"
            );
            return;
        }

        if (!actual->ptr_type.non_null && expected->ptr_type.non_null && actual_inner &&
            types_equal(actual_inner, expected_inner)) {
            char ts[64];
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "to override this check, use a cast: value as %s - but only if you are certain it's not null",
                type_str(expected, ts, sizeof(ts))
            );
            return;
        }
    }

    if (expected->kind == TYPE_PTR && !expected->ptr_type.is_fat && actual->kind != TYPE_PTR) {
        char es[64];
        if (expected->ptr_type.elem_type && types_equal(actual, expected->ptr_type.elem_type)) {
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "expected a pointer %s; if you have a value, take its address with &x",
                type_str(expected, es, sizeof(es))
            );
            return;
        }
    }

    if (actual->kind == TYPE_PTR && !actual->ptr_type.is_fat && expected->kind != TYPE_PTR) {
        char as[64];
        if (actual->ptr_type.elem_type && types_equal(actual->ptr_type.elem_type, expected)) {
            diag_emit(
                DIAG_NOTE,
                arg_loc,
                "you have a pointer %s but a value is expected; "
                "dereference it with *x",
                type_str(actual, as, sizeof(as))
            );
            return;
        }
    }
}

static bool types_assignable(Type* actual, Type* expected) {
    if (types_equal(actual, expected))
        return true;

    if (is_abstract_int(actual) && expected && expected->kind == TYPE_INT)
        return true;

    if (actual && expected && actual->kind == TYPE_PTR && expected->kind == TYPE_PTR) {
        return types_ptr_compatible(actual, expected);
    }

    return false;
}

static bool type_matches_printf_spec(Type* t, char spec, char length_mod) {
    if (!t)
        return false;

    switch (spec) {
        case 'd':
        case 'i':
            if (t->kind != TYPE_INT)
                return false;
            if (t->int_type.is_unsigned)
                return false;
            if (t->int_type.is_abstract)
                return true;
            switch (length_mod) {
                case 'h':
                    return t->bits == 16;       /* short */
                case 'l':
                    return t->bits == 64;       /* long */
                case 'z':
                    return t->int_type.is_size; /* ssize_t / isize */
                default:
                    return t->bits == 32;       /* int */
            }
            break;

        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (t->kind != TYPE_INT)
                return false;
            if (t->int_type.is_abstract)
                return true;
            if (!t->int_type.is_unsigned)
                return false;
            switch (length_mod) {
                case 'h':
                    return t->bits == 16;       /* unsigned short */
                case 'l':
                    return t->bits == 64;       /* unsigned long */
                case 'z':
                    return t->int_type.is_size; /* size_t / usize */
                default:
                    return t->bits == 32;       /* unsigned int */
            }
            break;

        case 'c':
            return t->kind == TYPE_INT && !t->int_type.is_unsigned && (t->bits == 32 || t->bits == 8);

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            if (t->kind != TYPE_FLOAT)
                return false;
            switch (length_mod) {
                case 'l':
                    return t->bits == 64; /* double */
                case 'L':
                    return t->bits == 80; /* long double */
                default:
                    return t->bits == 32; /* float */
            }
            break;

        case 's':
            return is_thin_ptr_u8(t);

        case 'p':
            return t->kind == TYPE_PTR;

        default:
            return false;
    }
    return false;
}

static bool check_printf_format(
    const char* fmt,
    size_t      fmt_len,
    AstNode**   variadic_args,
    int         variadic_count,
    Location    call_loc
) {
    int arg_idx = 0;
    for (size_t i = 0; i < fmt_len; i++) {
        if (fmt[i] == '%' && i + 1 < fmt_len) {
            i++;
            if (fmt[i] == '%') {
                continue;
            }

            while (i < fmt_len && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0')) {
                i++;
            }

            if (i < fmt_len && fmt[i] == '*') {
                if (arg_idx >= variadic_count) {
                    diag_emit(
                        DIAG_ERROR, call_loc, "printf format string expects more arguments than provided (width)"
                    );
                    return false;
                }
                Type* width_type = variadic_args[arg_idx]->type;
                if (width_type->kind != TYPE_INT || width_type->int_type.is_unsigned || width_type->bits != 32) {
                    char ts[64];
                    diag_emit(
                        DIAG_ERROR,
                        variadic_args[arg_idx]->loc,
                        "printf width (*) must be int (i32), got %s",
                        type_str(width_type, ts, sizeof(ts))
                    );
                    return false;
                }
                arg_idx++;
                i++;
            } else {
                while (i < fmt_len && (fmt[i] >= '0' && fmt[i] <= '9')) {
                    i++;
                }
            }

            if (i < fmt_len && fmt[i] == '.') {
                i++;
                if (i < fmt_len && fmt[i] == '*') {
                    if (arg_idx >= variadic_count) {
                        diag_emit(
                            DIAG_ERROR,
                            call_loc,
                            "printf format string expects more arguments than provided (precision)"
                        );
                        return false;
                    }
                    Type* prec_type = variadic_args[arg_idx]->type;
                    if (prec_type->kind != TYPE_INT || prec_type->int_type.is_unsigned || prec_type->bits != 32) {
                        char ts[64];
                        diag_emit(
                            DIAG_ERROR,
                            variadic_args[arg_idx]->loc,
                            "printf precision (.*) must be int (i32), got %s",
                            type_str(prec_type, ts, sizeof(ts))
                        );
                        return false;
                    }
                    arg_idx++;
                    i++;
                } else {
                    while (i < fmt_len && (fmt[i] >= '0' && fmt[i] <= '9')) {
                        i++;
                    }
                }
            }

            if (i >= fmt_len)
                break;

            char length_mod = '\0';
            if (fmt[i] == 'h' || fmt[i] == 'l' || fmt[i] == 'L' || fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't') {
                length_mod = fmt[i];
                i++;
            }

            if (i >= fmt_len)
                break;

            char spec = fmt[i];

            if (spec == 'n') {
                continue;
            }

            if (arg_idx >= variadic_count) {
                diag_emit(DIAG_ERROR, call_loc, "printf format string expects more arguments than provided");
                return false;
            }

            Type* arg_type = variadic_args[arg_idx]->type;
            if (!type_matches_printf_spec(arg_type, spec, length_mod)) {
                char        ts[64];
                const char* expected = "unknown";
                switch (spec) {
                    case 'd':
                    case 'i':
                        expected = length_mod == 'h'   ? "i16"
                                   : length_mod == 'l' ? "i64"
                                   : length_mod == 'z' ? "isize"
                                                       : "i32";
                        break;
                    case 'u':
                    case 'o':
                    case 'x':
                    case 'X':
                        expected = length_mod == 'h'   ? "u16"
                                   : length_mod == 'l' ? "u64"
                                   : length_mod == 'z' ? "usize"
                                                       : "u32";
                        break;
                    case 'c':
                        expected = "i32 (character)";
                        break;
                    case 'f':
                    case 'e':
                    case 'g':
                        expected = length_mod == 'l' ? "f64" : length_mod == 'L' ? "f80" : "f32";
                        break;
                    case 's':
                        expected = "*u8 (C string)";
                        break;
                    case 'p':
                        expected = "pointer";
                        break;
                }
                if (length_mod) {
                    diag_emit(
                        DIAG_WARN,
                        variadic_args[arg_idx]->loc,
                        "printf argument mismatch: format specifier '%%%c%c' expects %s, got %s",
                        length_mod,
                        spec,
                        expected,
                        type_str(arg_type, ts, sizeof(ts))
                    );
                } else {
                    diag_emit(
                        DIAG_WARN,
                        variadic_args[arg_idx]->loc,
                        "printf argument mismatch: format specifier '%%%c' expects %s, got %s",
                        spec,
                        expected,
                        type_str(arg_type, ts, sizeof(ts))
                    );
                }
                return false;
            }
            arg_idx++;
        }
    }

    if (arg_idx < variadic_count) {
        diag_emit(DIAG_WARN, call_loc, "printf format string uses fewer arguments than provided");
    }

    return true;
}

static Type _error_type = {.kind = TYPE_VOID};
#define ERROR_TYPE (&_error_type)

static bool is_error_type(Type* t) {
    return t == ERROR_TYPE;
}

typedef struct {
    AstNode* decl;
    Type*    type;
} SymbolEntry;

typedef struct Scope {
    Ht(const char*, SymbolEntry) syms;
    Ht(const char*, int) non_null_vars;
    struct Scope* parent;
} Scope;

static Scope* scope_new(Scope* parent) {
    Scope* s                = calloc(1, sizeof(*s));
    s->syms.hasheq          = ht_cstr_hasheq;
    s->non_null_vars.hasheq = ht_cstr_hasheq;
    s->parent               = parent;
    return s;
}

static bool var_is_known_nonnull(Scope* sc, const char* name) {
    for (; sc; sc = sc->parent) {
        int* marked = ht_find(&sc->non_null_vars, name);
        if (marked)
            return true;
    }
    return false;
}

static void mark_nonnull(Scope* sc, const char* name) {
    *ht_put(&sc->non_null_vars, name) = 1;
}

static Type* try_narrow_ptr_to_nonnull(Type* t) {
    if (!t || t->kind != TYPE_PTR || t->ptr_type.non_null)
        return t;
    Type* narrowed = type_ptr(t->ptr_type.elem_type, t->ptr_type.is_fat, true);
    return narrowed;
}

static void scope_add(Scope* sc, const char* name, AstNode* decl, Type* type) {
    SymbolEntry* entry = ht_find_or_put(&sc->syms, name);
    entry->decl        = decl;
    entry->type        = type;
}

static SymbolEntry* scope_lookup(Scope* sc, const char* name) {
    for (; sc; sc = sc->parent) {
        SymbolEntry* entry = ht_find(&sc->syms, name);
        if (entry)
            return entry;
    }
    return NULL;
}

static Type* expr_ident_to_type(const char* name) {
    if (strcmp(name, "void") == 0)
        return type_void();

    if (name[0] == 'i' || name[0] == 's' || name[0] == 'u' || name[0] == 'f') {
        char        prefix = name[0];
        const char* rest   = name + 1;

        if (strcmp(rest, "size") == 0) {
            if (prefix == 'i' || prefix == 's')
                return type_number(TYPE_INT, 0, 0);
            else if (prefix == 'u')
                return type_number(TYPE_INT, 0, 1);
            else if (prefix == 'f') {
                Type* t             = type_number(TYPE_FLOAT, 0, 0);
                t->int_type.is_size = true;
                return t;
            }
            return NULL;
        }

        char* endptr;
        long  bits = strtol(rest, &endptr, 10);

        if (*endptr != '\0' || bits < 0 || bits > 65535)
            return NULL;

        if (prefix == 'i' || prefix == 's')
            return type_number(TYPE_INT, (uint16_t)bits, 0);
        else if (prefix == 'u')
            return type_number(TYPE_INT, (uint16_t)bits, 1);
        else if (prefix == 'f')
            return type_number(TYPE_FLOAT, (uint16_t)bits, 0);
    }

    return NULL;
}

static Type* resolve_type(Type* t, Scope* sc) {
    if (!t || t->kind != TYPE_IDENT)
        return t;

    const char* name = t->ident_type.name;

    Type* builtin = expr_ident_to_type(name);
    if (builtin)
        return builtin;

    SymbolEntry* sym = scope_lookup(sc, name);
    if (sym && sym->decl && sym->decl->kind == AST_CONST_DECL && sym->decl->init) {
        AstNode* init = sym->decl->init;
        if (init->kind == AST_IDENT) {
            Type* resolved = expr_ident_to_type(init->ident);
            if (resolved)
                return resolved;
        }
    }

    if (sym && sym->type) {
        return resolve_type(sym->type, sc);
    }

    return t;
}

static Type* check_expr_hint(AstNode* e, Scope* sc, Type* hint);
static void  check_stmt(AstNode* s, Scope* sc, AstNode* fn);

static Type* check_expr(AstNode* e, Scope* sc) {
    return check_expr_hint(e, sc, NULL);
}

static const char* extract_null_check_var(AstNode* cond, bool* is_negated) {
    if (!cond || cond->kind != AST_BINOP)
        return NULL;

    if ((cond->op == OP_EQ && (cond->rhs->kind == AST_NULLPTR || cond->lhs->kind == AST_NULLPTR)) ||
        (cond->op == OP_NEQ && (cond->rhs->kind == AST_NULLPTR || cond->lhs->kind == AST_NULLPTR))) {
        AstNode* ptr_expr = cond->lhs->kind == AST_NULLPTR ? cond->rhs : cond->lhs;
        if (ptr_expr->kind == AST_IDENT) {
            *is_negated = (cond->op == OP_NEQ);
            return ptr_expr->ident;
        }
    }
    return NULL;
}

static bool stmt_always_exits(AstNode* stmt) {
    if (!stmt)
        return false;

    switch (stmt->kind) {
        case AST_RETURN_STMT:
            return true;

        case AST_BLOCK_STMT: {
            for (int i = 0; i < stmt->stmt_count; i++) {
                if (stmt_always_exits(stmt->stmts[i]))
                    return true;
            }
            return false;
        }

        default:
            return false;
    }
}

static Type* check_expr_hint(AstNode* e, Scope* sc, Type* hint) {
    hint = resolve_type(hint, sc);

    switch (e->kind) {
        case AST_INT_LIT:
            if (hint && hint->kind == TYPE_INT) {
                e->type = hint;
            } else if (!e->type) {
                e->type = type_abstract_int();
            }
            return e->type;

        case AST_FLOAT_LIT:
            if (!e->type) {
                if (hint && hint->kind == TYPE_FLOAT)
                    e->type = hint;
                else
                    e->type = type_number(TYPE_FLOAT, 64, 0);
            }
            return e->type;

        case AST_STRING_LIT: {
            if (!e->type) {
                Type* u8    = type_number(TYPE_INT, 8, true);
                int   flags = e->str_flags;
                if (flags & STR_PREFIX_C)
                    e->type = type_ptr(u8, false, false); /* *u8  */
                else if (flags & STR_PREFIX_B)
                    e->type = type_array(u8, 0);          /* [u8] */
                else
                    e->type = type_ptr(u8, true, false);  /* []u8 */
            }
            return e->type;
        }

        case AST_NULLPTR: {
            if (!e->type) {
                e->type = type_ptr(type_void(), false, false);
            }
            return e->type;
        }

        case AST_ARRAY_LIT: {
            if (e->element_count == 0) {
                Type* elem = NULL;
                if (hint && hint->kind == TYPE_ARRAY)
                    elem = hint->array_type.elem_type;
                else if (hint && hint->kind == TYPE_PTR)
                    elem = hint->ptr_type.elem_type;
                if (!elem) {
                    diag_emit(
                        DIAG_ERROR,
                        e->loc,
                        "cannot infer type of empty array literal; "
                        "annotate the variable"
                    );
                    elem = type_void();
                }
                e->type = type_array(elem, 0);
                return e->type;
            }

            Type* elem_hint = NULL;
            if (hint) {
                if (hint->kind == TYPE_ARRAY)
                    elem_hint = hint->array_type.elem_type;
                else if (hint->kind == TYPE_PTR)
                    elem_hint = hint->ptr_type.elem_type;
            }

            Type* elem_type = check_expr_hint(e->elements[0], sc, elem_hint);
            for (size_t i = 1; i < e->element_count; i++) {
                Type* et = check_expr_hint(e->elements[i], sc, elem_type);
                if (!types_equal(elem_type, et)) {
                    char es[64], fs[64];
                    diag_emit(
                        DIAG_ERROR,
                        e->elements[i]->loc,
                        "array literal element %zu has type %s, expected %s",
                        i,
                        type_str(et, fs, sizeof(fs)),
                        type_str(elem_type, es, sizeof(es))
                    );
                }
            }
            e->type = type_array(elem_type, e->element_count);
            return e->type;
        }

        case AST_IDENT: {
            SymbolEntry* sym = scope_lookup(sc, e->ident);
            if (!sym) {
                diag_emit(DIAG_ERROR, e->loc, "undefined symbol '%s'", e->ident);
                e->type = ERROR_TYPE;
            } else if (sym->decl && sym->decl->kind == AST_CONST_DECL) {
                if (sym->decl->init) {
                    e->type = check_expr_hint(sym->decl->init, sc, hint);
                } else {
                    diag_emit(DIAG_ERROR, e->loc, "constant '%s' has no initializer", e->ident);
                    e->type = ERROR_TYPE;
                }
            } else {
                e->type = resolve_type(sym->type, sc);

                if (e->type && e->type->kind == TYPE_PTR && var_is_known_nonnull(sc, e->ident)) {
                    e->type = try_narrow_ptr_to_nonnull(e->type);
                }
            }
            return e->type;
        }

        case AST_CALL: {
            SymbolEntry* sym = scope_lookup(sc, e->callee);
            if (!sym || !sym->decl) {
                diag_emit(DIAG_ERROR, e->loc, "undefined function '%s'", e->callee);
                for (int i = 0; i < e->arg_count; i++)
                    check_expr(e->args[i], sc);
                e->type = type_void();
                return e->type;
            }
            AstNode* fn = sym->decl;

            int variadic_index = -1;
            if (fn->param_count > 0 && fn->is_variadic) {
                variadic_index = fn->param_count - 1;
            }

            int required_param_count = 0;
            for (int i = 0; i < fn->param_count; i++) {
                if (variadic_index != -1 && i >= variadic_index) {
                    break;
                }
                if (fn->params[i].default_value == NULL) {
                    required_param_count++;
                } else {
                    break;
                }
            }

            if (variadic_index == -1) {
                if (e->arg_count < required_param_count || e->arg_count > fn->param_count) {
                    if (required_param_count == fn->param_count) {
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "'%s' expects %d arg(s), got %d",
                            fn->function_name,
                            required_param_count,
                            e->arg_count
                        );
                    } else {
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "'%s' expects %d to %d arg(s), got %d",
                            fn->function_name,
                            required_param_count,
                            fn->param_count,
                            e->arg_count
                        );
                    }
                }
            } else if (e->arg_count < required_param_count) {
                diag_emit(
                    DIAG_ERROR,
                    e->loc,
                    "'%s' expects at least %d arg(s), got %d",
                    fn->function_name,
                    required_param_count,
                    e->arg_count
                );
            }

            for (int i = 0; i < e->arg_count; i++) {
                if (i < variadic_index || (variadic_index == -1 && i < fn->param_count)) {
                    Type* param_hint = fn->params[i].type;
                    Type* at         = check_expr_hint(e->args[i], sc, param_hint);
                    Type* pt         = fn->params[i].type;

                    if (pt && pt->kind == TYPE_PTR && pt->ptr_type.non_null && e->args[i]->kind == AST_NULLPTR) {
                        char ps[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->args[i]->loc,
                            "argument %d of '%s': cannot pass nullptr to non-null pointer parameter %s",
                            i + 1,
                            fn->function_name,
                            type_str(pt, ps, sizeof(ps))
                        );
                    }

                    if (!types_assignable(at, pt)) {
                        char as[64], ps[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->args[i]->loc,
                            "argument %d of '%s': expected %s, got %s",
                            i + 1,
                            fn->function_name,
                            type_str(pt, ps, sizeof(ps)),
                            type_str(at, as, sizeof(as))
                        );
                        diag_type_hint(at, pt, e->args[i]->loc);
                    }
                } else if (variadic_index != -1 && i >= variadic_index) {
                    Type* var_param_type = fn->params[variadic_index].type;
                    if (var_param_type && var_param_type->kind == TYPE_ARRAY) {
                        Type* elem_type = var_param_type->array_type.elem_type;
                        Type* at        = check_expr_hint(e->args[i], sc, elem_type);
                        if (!types_assignable(at, elem_type)) {
                            char as[64], es[64];
                            diag_emit(
                                DIAG_ERROR,
                                e->args[i]->loc,
                                "argument %d of '%s': expected %s, got %s",
                                i + 1,
                                fn->function_name,
                                type_str(elem_type, es, sizeof(es)),
                                type_str(at, as, sizeof(as))
                            );
                            diag_type_hint(at, elem_type, e->args[i]->loc);
                        }
                    } else {
                        check_expr(e->args[i], sc);
                    }
                }
            }

            if (fn->is_printf_like && fn->param_idx >= 0 && fn->param_idx < e->arg_count) {
                AstNode* fmt_arg = e->args[fn->param_idx];
                if (fmt_arg->kind == AST_STRING_LIT) {
                    int variadic_start = variadic_index;
                    if (variadic_start == -1) {
                        variadic_start = fn->param_count;
                    }
                    int variadic_arg_count = e->arg_count - variadic_start;
                    check_printf_format(
                        fmt_arg->str, fmt_arg->len, &e->args[variadic_start], variadic_arg_count, e->loc
                    );
                }
            }

            e->func_decl = fn;
            e->type      = resolve_type(fn->ret_type, sc);
            return e->type;
        }

        case AST_BINOP: {
            Type* lt = NULL;
            Type* rt = NULL;

            if (e->op == OP_EQ || e->op == OP_NEQ || e->op == OP_LESS || e->op == OP_LESSEQ || e->op == OP_MORE ||
                e->op == OP_MOREEQ) {
                lt = check_expr_hint(e->lhs, sc, hint);
                rt = check_expr_hint(e->rhs, sc, lt);
            } else {
                lt = check_expr_hint(e->lhs, sc, hint);
                rt = check_expr_hint(e->rhs, sc, hint);
            }

            bool is_cmp =
                (e->op == OP_EQ || e->op == OP_NEQ || e->op == OP_LESS || e->op == OP_LESSEQ || e->op == OP_MORE ||
                 e->op == OP_MOREEQ);

            if ((is_pointer(lt) || is_pointer(rt)) && !is_cmp) {
                if (e->op == OP_ADD) {
                    if (is_pointer(lt) && is_pointer(rt)) {
                        diag_emit(DIAG_ERROR, e->loc, "cannot add two pointers");
                        e->type = ERROR_TYPE;
                        return e->type;
                    }
                    if (is_pointer(lt) && !is_integer(rt)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->rhs->loc,
                            "pointer arithmetic requires integer offset, got %s",
                            type_str(rt, s, sizeof(s))
                        );
                        e->type = lt;
                        return e->type;
                    }
                    if (is_pointer(rt) && !is_integer(lt)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->lhs->loc,
                            "pointer arithmetic requires integer offset, got %s",
                            type_str(lt, s, sizeof(s))
                        );
                        e->type = rt;
                        return e->type;
                    }
                    e->type = is_pointer(lt) ? lt : rt;
                    return e->type;
                } else if (e->op == OP_SUB) {
                    if (is_pointer(lt) && is_pointer(rt)) {
                        e->type = type_number(TYPE_INT, 64, 1);
                        return e->type;
                    }
                    if (is_pointer(lt) && !is_integer(rt)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->rhs->loc,
                            "pointer arithmetic requires integer offset, got %s",
                            type_str(rt, s, sizeof(s))
                        );
                        e->type = lt;
                        return e->type;
                    }
                    if (is_pointer(rt) && is_integer(lt)) {
                        diag_emit(DIAG_ERROR, e->loc, "cannot subtract pointer from integer");
                        e->type = ERROR_TYPE;
                        return e->type;
                    }
                    e->type = lt;
                    return e->type;
                } else {
                    char ls[64], rs[64];
                    diag_emit(
                        DIAG_ERROR,
                        e->loc,
                        "binary operator '%s' not supported on pointer types (%s, %s)",
                        e->op == OP_MUL   ? "*"
                        : e->op == OP_DIV ? "/"
                        : e->op == OP_MOD ? "%"
                        : e->op == OP_POW ? "**"
                                          : "op",
                        type_str(lt, ls, sizeof(ls)),
                        type_str(rt, rs, sizeof(rs))
                    );
                    e->type = lt ? lt : rt;
                    return e->type;
                }
            }

            if (lt && rt && !types_equal(lt, rt) && !is_error_type(lt) && !is_error_type(rt) && !is_abstract_int(lt) &&
                !is_abstract_int(rt)) {
                bool is_comparison =
                    (e->op == OP_EQ || e->op == OP_NEQ || e->op == OP_LESS || e->op == OP_LESSEQ || e->op == OP_MORE ||
                     e->op == OP_MOREEQ);

                bool lhs_is_nullptr = e->lhs->kind == AST_NULLPTR;
                bool rhs_is_nullptr = e->rhs->kind == AST_NULLPTR;
                bool ptr_cmp_null =
                    is_comparison && ((lhs_is_nullptr && is_pointer(rt)) || (rhs_is_nullptr && is_pointer(lt)) ||
                                      (is_pointer(lt) && is_pointer(rt)));

                if (ptr_cmp_null) {
                    e->type = type_number(TYPE_INT, 32, 0);
                    return e->type;
                }

                if (is_comparison && types_compatible_numeric(lt, rt)) {
                    e->type = lt;
                    return e->type;
                }

                bool is_arithmetic =
                    (e->op == OP_ADD || e->op == OP_SUB || e->op == OP_MUL || e->op == OP_DIV || e->op == OP_MOD);
                if (is_arithmetic && types_compatible_numeric(lt, rt)) {
                    e->type = lt;
                    return e->type;
                }

                bool is_shift = (e->op == OP_SHL || e->op == OP_SHR);
                if (is_shift) {
                    e->type = lt;
                    return e->type;
                }

                char ls[64], rs[64];
                diag_emit(
                    DIAG_ERROR,
                    e->loc,
                    "type mismatch: %s and %s",
                    type_str(lt, ls, sizeof(ls)),
                    type_str(rt, rs, sizeof(rs))
                );
            }

            if (e->op == OP_POW) {
                if ((lt && lt->kind == TYPE_FLOAT) || (rt && rt->kind == TYPE_FLOAT))
                    diag_emit(DIAG_ERROR, e->loc, "power operator '**' does not support float operands");
                if (rt && rt->kind != TYPE_INT)
                    diag_emit(DIAG_ERROR, e->rhs->loc, "power operator exponent must be an integer");
                else if (e->rhs->kind == AST_INT_LIT && e->rhs->ival < 0)
                    diag_emit(DIAG_ERROR, e->rhs->loc, "power operator exponent must be non-negative");
            }

            bool is_bitwise =
                (e->op == OP_BITAND || e->op == OP_BITOR || e->op == OP_BITXOR || e->op == OP_SHL || e->op == OP_SHR);
            if (is_bitwise) {
                if (!is_integer(lt)) {
                    char s[64];
                    diag_emit(
                        DIAG_ERROR,
                        e->lhs->loc,
                        "bitwise operator requires integer operand, got %s",
                        type_str(lt, s, sizeof(s))
                    );
                }
                if (!is_integer(rt)) {
                    char s[64];
                    diag_emit(
                        DIAG_ERROR,
                        e->rhs->loc,
                        "bitwise operator requires integer operand, got %s",
                        type_str(rt, s, sizeof(s))
                    );
                }
            }

            bool is_logical = (e->op == OP_LAND || e->op == OP_LOR);
            if (is_logical) {
                if (!is_numeric(lt)) {
                    char s[64];
                    diag_emit(
                        DIAG_ERROR,
                        e->lhs->loc,
                        "logical operator requires numeric operand, got %s",
                        type_str(lt, s, sizeof(s))
                    );
                }
                if (!is_numeric(rt)) {
                    char s[64];
                    diag_emit(
                        DIAG_ERROR,
                        e->rhs->loc,
                        "logical operator requires numeric operand, got %s",
                        type_str(rt, s, sizeof(s))
                    );
                }
            }

            if (is_cmp) {
                if ((e->op == OP_EQ || e->op == OP_NEQ) && is_pointer(lt) && is_pointer(rt)) {
                    bool lhs_is_nullptr =
                        (e->lhs->kind == AST_NULLPTR ||
                         (e->lhs->kind == AST_IDENT && lt->kind == TYPE_PTR && !lt->ptr_type.non_null &&
                          !var_is_known_nonnull(sc, e->lhs->ident)));
                    bool rhs_is_nullptr =
                        (e->rhs->kind == AST_NULLPTR ||
                         (e->rhs->kind == AST_IDENT && rt->kind == TYPE_PTR && !rt->ptr_type.non_null &&
                          !var_is_known_nonnull(sc, e->rhs->ident)));

                    if (lhs_is_nullptr && rhs_is_nullptr) {
                        diag_emit(
                            DIAG_WARN,
                            e->loc,
                            "comparing nullptr to nullptr is always %s",
                            e->op == OP_EQ ? "true" : "false"
                        );
                    } else if (lhs_is_nullptr && rt && rt->kind == TYPE_PTR && rt->ptr_type.non_null) {
                        diag_emit(
                            DIAG_WARN,
                            e->loc,
                            "pointer on right is non-null, comparison with nullptr is always %s",
                            e->op == OP_EQ ? "false" : "true"
                        );
                    } else if (rhs_is_nullptr && lt && lt->kind == TYPE_PTR && lt->ptr_type.non_null) {
                        diag_emit(
                            DIAG_WARN,
                            e->loc,
                            "pointer on left is non-null, comparison with nullptr is always %s",
                            e->op == OP_EQ ? "false" : "true"
                        );
                    } else if (rhs_is_nullptr && e->lhs->kind == AST_IDENT && var_is_known_nonnull(sc, e->lhs->ident)) {
                        diag_emit(
                            DIAG_WARN,
                            e->loc,
                            "pointer has been null-checked, comparison with nullptr is always %s",
                            e->op == OP_EQ ? "false" : "true"
                        );
                    } else if (lhs_is_nullptr && e->rhs->kind == AST_IDENT && var_is_known_nonnull(sc, e->rhs->ident)) {
                        diag_emit(
                            DIAG_WARN,
                            e->loc,
                            "pointer has been null-checked, comparison with nullptr is always %s",
                            e->op == OP_EQ ? "false" : "true"
                        );
                    }
                }
                e->type = type_number(TYPE_INT, 32, 0);
            } else {
                if (is_abstract_int(lt) && is_abstract_int(rt)) {
                    e->type = type_abstract_int();
                } else if (is_abstract_int(lt)) {
                    e->type = rt;
                } else {
                    e->type = lt;
                }
            }
            return e->type;
        }

        case AST_UNOP: {
            Type* t = check_expr(e->operand, sc);

            switch (e->uop) {
                case UOP_DEREF:
                    if (is_error_type(t)) {
                        e->type = ERROR_TYPE;
                    } else if (!t || t->kind != TYPE_PTR) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR, e->loc, "cannot dereference non-pointer type %s", type_str(t, s, sizeof(s))
                        );
                        e->type = ERROR_TYPE;
                    } else if (t->ptr_type.is_fat) {
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "cannot dereference fat pointer (slice) directly; "
                            "index it instead"
                        );
                        e->type = resolve_type(t->ptr_type.elem_type, sc);
                    } else {
                        e->type = resolve_type(t->ptr_type.elem_type, sc);
                    }
                    break;

                case UOP_ADDR: {
                    if (t && t->kind == TYPE_ARRAY) {
                        e->type = type_ptr(t->array_type.elem_type, false, false);
                    } else {
                        e->type = type_ptr(t, false, false);
                    }
                    break;
                    ;
                }

                case UOP_NOT:
                    if (!is_numeric(t)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "logical not requires numeric operand, got %s",
                            type_str(t, s, sizeof(s))
                        );
                    }
                    e->type = type_number(TYPE_INT, 64, 0);
                    break;

                case UOP_BITNOT:
                    if (!is_integer(t)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "bitwise not requires integer operand, got %s",
                            type_str(t, s, sizeof(s))
                        );
                    }
                    e->type = t;
                    break;

                case UOP_NEG:
                case UOP_POS:
                    if (!is_numeric(t)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "unary %s requires numeric operand, got %s",
                            e->uop == UOP_NEG ? "-" : "+",
                            type_str(t, s, sizeof(s))
                        );
                    }
                    e->type = t;
                    break;

                case UOP_PREINC:
                case UOP_PREDEC:
                case UOP_POSTINC:
                case UOP_POSTDEC:
                    if (!is_integer(t)) {
                        char s[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "increment/decrement requires integer operand, got %s",
                            type_str(t, s, sizeof(s))
                        );
                    }
                    e->type = t;
                    break;
            }
            return e->type;
        }

        case AST_INDEX: {
            Type* array_type = check_expr(e->array, sc);
            Type* index_type = check_expr(e->index, sc);

            if (!is_integer(index_type)) {
                char s[64];
                diag_emit(
                    DIAG_ERROR, e->index->loc, "array index must be integer, got %s", type_str(index_type, s, sizeof(s))
                );
            }

            if (is_error_type(array_type)) {
                e->type = ERROR_TYPE;
            } else if (array_type && array_type->kind == TYPE_ARRAY) {
                e->type = resolve_type(array_type->array_type.elem_type, sc);
            } else if (array_type && array_type->kind == TYPE_PTR) {
                e->type = resolve_type(array_type->ptr_type.elem_type, sc);
            } else {
                char s[64];
                diag_emit(
                    DIAG_ERROR, e->loc, "cannot index non-array/pointer type %s", type_str(array_type, s, sizeof(s))
                );
                e->type = ERROR_TYPE;
            }
            return e->type;
        }

        case AST_MEMBER: {
            Type* value_type = check_expr(e->member_value, sc);

            if (is_error_type(value_type)) {
                e->type = ERROR_TYPE;
                return e->type;
            }

            if (value_type &&
                ((value_type->kind == TYPE_PTR && value_type->ptr_type.is_fat) || value_type->kind == TYPE_ARRAY)) {
                if (strcmp(e->member_name, "len") == 0 || strcmp(e->member_name, "length") == 0 ||
                    strcmp(e->member_name, "count") == 0) {
                    e->type = type_abstract_int();
                    return e->type;
                } else {
                    diag_emit(
                        DIAG_ERROR,
                        e->loc,
                        "%s has no member '%s'",
                        value_type->kind == TYPE_PTR ? "fat pointer" : "array",
                        e->member_name
                    );
                    e->type = ERROR_TYPE;
                    return e->type;
                }
            }

            char s[64];
            diag_emit(
                DIAG_ERROR, e->loc, "member access not supported for type %s", type_str(value_type, s, sizeof(s))
            );
            e->type = ERROR_TYPE;
            return e->type;
        }

        case AST_CAST: {
            Type* expr_type = check_expr(e->cast_expr, sc);

            if (e->cast_type == NULL) {
                e->type = hint;
            } else {
                e->type = resolve_type(e->cast_type, sc);

                if (e->type && e->type->kind == TYPE_PTR && e->type->ptr_type.elem_type) {
                    if (e->type->ptr_type.elem_type->kind == TYPE_IDENT) {
                        e->type->ptr_type.elem_type = resolve_type(e->type->ptr_type.elem_type, sc);
                    }
                }
            }

            if (expr_type && expr_type->kind == TYPE_PTR && e->type && e->type->kind == TYPE_PTR) {
                bool is_nullptr = e->cast_expr->kind == AST_NULLPTR;

                if (is_nullptr && e->type->ptr_type.non_null) {
                    diag_emit(DIAG_ERROR, e->loc, "cannot cast nullptr to non-null pointer type");
                }
            }

            return e->type;
        }

        case AST_RANGE: {
            Type* start_type = check_expr(e->range_start, sc);
            Type* end_type   = check_expr(e->range_end, sc);

            if (start_type && end_type) {
                if (!types_equal(start_type, end_type)) {
                    if (!is_abstract_int(start_type) && !is_abstract_int(end_type)) {
                        char st[64], et[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->loc,
                            "range bounds must have same type, got %s and %s",
                            type_str(start_type, st, sizeof(st)),
                            type_str(end_type, et, sizeof(et))
                        );
                    }
                }
            }

            bool is_float_range = start_type && start_type->kind == TYPE_FLOAT;

            if (is_float_range && !e->range_step) {
                diag_emit(DIAG_ERROR, e->loc, "float ranges require an explicit step (e.g., 0.2..1.0:0.1)");
            }

            if (!e->range_step) {
                if (e->range_start->kind == AST_INT_LIT && e->range_end->kind == AST_INT_LIT) {
                    if (e->range_start->ival > e->range_end->ival) {
                        diag_emit(
                            DIAG_WARN,
                            e->loc,
                            "range %lld..%lld:1 does not iterate (start > end). Use negative step like -1 "
                            "(start..end:step) to iterate backwards.",
                            e->range_start->ival,
                            e->range_end->ival
                        );
                    }
                }
            }

            if (e->range_step) {
                Type* step_type = check_expr(e->range_step, sc);
                if (step_type) {
                    if (is_float_range && step_type->kind != TYPE_FLOAT) {
                        char st[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->range_step->loc,
                            "float range step must be float, got %s",
                            type_str(step_type, st, sizeof(st))
                        );
                    } else if (!is_float_range && step_type->kind != TYPE_INT) {
                        char st[64];
                        diag_emit(
                            DIAG_ERROR,
                            e->range_step->loc,
                            "integer range step must be integer, got %s",
                            type_str(step_type, st, sizeof(st))
                        );
                    } else if (
                        e->range_start->kind == AST_INT_LIT && e->range_end->kind == AST_INT_LIT &&
                        e->range_step->kind == AST_INT_LIT
                    ) {
                        if (e->range_start->ival > e->range_end->ival && e->range_step->ival > 0) {
                            diag_emit(
                                DIAG_WARN,
                                e->loc,
                                "range %lld..%lld:%lld does not iterate (start > end with positive step). Use "
                                "negative step like -%lld.",
                                e->range_start->ival,
                                e->range_end->ival,
                                e->range_step->ival,
                                e->range_step->ival
                            );
                        }
                    }
                }
            }

            e->type = start_type ? start_type : end_type;
            return e->type;
        }

        default:
            ICE("unexpected node in expression context");
            return type_void();
    }
}

static void check_stmt(AstNode* s, Scope* sc, AstNode* fn) {
    switch (s->kind) {
        case AST_EXPR_STMT:
            check_expr(s->expr, sc);
            break;

        case AST_RETURN_STMT: {
            Type* ret = fn ? fn->ret_type : NULL;
            if (s->ret_val) {
                Type* vt = check_expr_hint(s->ret_val, sc, ret);
                if (is_error_type(vt)) {
                } else if (ret && ret->kind == TYPE_VOID) {
                    diag_emit(DIAG_WARN, s->loc, "returning value from void function");
                } else if (ret && !types_assignable(vt, ret)) {
                    char vs[64], rs[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "return type mismatch: expected %s, got %s",
                        type_str(ret, rs, sizeof(rs)),
                        type_str(vt, vs, sizeof(vs))
                    );
                    diag_type_hint(vt, ret, s->ret_val->loc);
                }
            } else if (ret && ret->kind != TYPE_VOID) {
                diag_emit(DIAG_WARN, s->loc, "missing return value in non-void function");
            }
            break;
        }

        case AST_BLOCK_STMT: {
            Scope* block_sc = scope_new(sc);
            for (int i = 0; i < s->stmt_count; i++)
                check_stmt(s->stmts[i], block_sc, fn);
            break;
        }

        case AST_IF_STMT: {
            Type* ct = check_expr(s->if_cond, sc);
            if (ct && ct->kind == TYPE_PTR) {
                char ts[64];
                diag_emit(
                    DIAG_ERROR,
                    s->if_cond->loc,
                    "condition cannot be a pointer type (%s); dereference first",
                    type_str(ct, ts, sizeof(ts))
                );
            }

            bool        is_negated       = false;
            const char* null_checked_var = extract_null_check_var(s->if_cond, &is_negated);

            if (null_checked_var) {
                Scope* then_sc = scope_new(sc);

                if (is_negated) {
                    mark_nonnull(then_sc, null_checked_var);
                }

                check_stmt(s->then_branch, then_sc, fn);
                ht_free(&then_sc->non_null_vars);
                free(then_sc);

                if (s->else_branch) {
                    Scope* else_sc = scope_new(sc);
                    if (!is_negated) {
                        mark_nonnull(else_sc, null_checked_var);
                    }
                    check_stmt(s->else_branch, else_sc, fn);
                    ht_free(&else_sc->non_null_vars);
                    free(else_sc);
                }

                if (is_negated && stmt_always_exits(s->then_branch)) {
                    mark_nonnull(sc, null_checked_var);
                } else if (!is_negated && stmt_always_exits(s->then_branch) && !s->else_branch) {
                    mark_nonnull(sc, null_checked_var);
                }
            } else {
                check_stmt(s->then_branch, sc, fn);
                if (s->else_branch)
                    check_stmt(s->else_branch, sc, fn);
            }
            break;
        }

        case AST_WHILE_STMT: {
            Type* ct = check_expr(s->while_cond, sc);
            if (ct && ct->kind == TYPE_PTR) {
                char ts[64];
                diag_emit(
                    DIAG_ERROR,
                    s->while_cond->loc,
                    "condition cannot be a pointer type (%s); dereference first",
                    type_str(ct, ts, sizeof(ts))
                );
            }
            check_stmt(s->while_body, sc, fn);
            break;
        }

        case AST_FOR_STMT: {
            Type* iter_type = check_expr(s->for_iterable, sc);

            if (!iter_type) {
                diag_emit(DIAG_ERROR, s->for_iterable->loc, "iterable type cannot be inferred");
            } else if (iter_type->kind == TYPE_ARRAY) {
            } else if (iter_type->kind == TYPE_PTR && iter_type->ptr_type.is_fat) {
            } else if (iter_type->kind == TYPE_INT || iter_type->kind == TYPE_FLOAT) {
            } else {
                char ts[64];
                diag_emit(
                    DIAG_ERROR,
                    s->for_iterable->loc,
                    "can only iterate over arrays, fat pointers, or ranges, got %s",
                    type_str(iter_type, ts, sizeof(ts))
                );
            }

            if (s->for_val && s->for_val->kind == AST_IDENT) {
                Scope* loop_scope = scope_new(sc);

                Type* elem_type = NULL;
                if (iter_type && iter_type->kind == TYPE_ARRAY) {
                    elem_type = resolve_type(iter_type->array_type.elem_type, sc);
                } else if (iter_type && iter_type->kind == TYPE_PTR && iter_type->ptr_type.is_fat) {
                    elem_type = resolve_type(iter_type->ptr_type.elem_type, sc);
                } else if (
                    s->for_iterable->kind == AST_RANGE && iter_type &&
                    (iter_type->kind == TYPE_INT || iter_type->kind == TYPE_FLOAT)
                ) {
                    elem_type = iter_type;
                } else {
                    elem_type = type_number(TYPE_INT, 64, false);
                }

                AstNode* var_decl  = ast_node(AST_VAR_DECL, s->for_val->loc);
                var_decl->var_name = s->for_val->ident;
                var_decl->var_type = elem_type;
                var_decl->init     = NULL;
                scope_add(loop_scope, var_decl->var_name, var_decl, elem_type);

                check_stmt(s->for_body, loop_scope, fn);
            } else {
                check_stmt(s->for_body, sc, fn);
            }
            break;
        }

        case AST_VAR_DECL: {
            Type* scope_type = s->var_type;
            if (s->init) {
                Type* it = check_expr_hint(s->init, sc, s->var_type);

                if (!s->var_type) {
                    s->var_type = it;
                    scope_type  = it;
                } else {
                    if (s->var_type->kind == TYPE_ARRAY && s->var_type->array_type.len == 0 && it &&
                        it->kind == TYPE_ARRAY && it->array_type.len > 0 &&
                        types_equal(s->var_type->array_type.elem_type, it->array_type.elem_type)) {
                        s->var_type = type_array(s->var_type->array_type.elem_type, it->array_type.len);
                        scope_type  = s->var_type;
                        it          = s->var_type;
                    }

                    if (!types_compatible_with_decay(it, s->var_type) && !types_assignable(it, s->var_type) &&
                        !is_error_type(it)) {
                        char is[64], vs[64];

                        if (it && s->var_type && it->kind == TYPE_PTR && s->var_type->kind == TYPE_PTR &&
                            !it->ptr_type.non_null && s->var_type->ptr_type.non_null &&
                            it->ptr_type.is_fat == s->var_type->ptr_type.is_fat &&
                            types_equal(it->ptr_type.elem_type, s->var_type->ptr_type.elem_type)) {
                            diag_emit(
                                DIAG_ERROR,
                                s->init->loc,
                                "variable '%s': cannot assign nullable pointer to non-null pointer; "
                                "the *! prefix is a guarantee that the value is never null",
                                s->var_name
                            );
                            diag_type_hint(it, s->var_type, s->init->loc);
                        } else {
                            diag_emit(
                                DIAG_ERROR,
                                s->init->loc,
                                "variable '%s': expected %s, initializer has type %s",
                                s->var_name,
                                type_str(s->var_type, vs, sizeof(vs)),
                                type_str(it, is, sizeof(is))
                            );
                            diag_type_hint(it, s->var_type, s->init->loc);
                        }
                        scope_type = it;
                    }
                }
            } else if (!s->var_type) {
                ICE("variable declaration '%s' requires either a type annotation or an initializer", s->var_name);
                scope_type = type_void();
            }
            s->type               = s->var_type;
            SymbolEntry* existing = scope_lookup(sc, s->var_name);
            if (existing && existing->decl != NULL) {
                diag_emit(DIAG_ERROR, s->loc, "redefinition of variable '%s'", s->var_name);
            } else {
                scope_add(sc, s->var_name, s, scope_type);
            }
            break;
        }

        case AST_CONST_DECL: {
            Type* scope_type = s->var_type;

            if (!s->init) {
                diag_emit(DIAG_ERROR, s->loc, "const declaration requires an initializer");
                s->type = s->var_type ? s->var_type : type_void();
                break;
            }

            Type* it = check_expr_hint(s->init, sc, s->var_type);

            if (!s->var_type) {
                s->var_type = it;
                scope_type  = it;
            } else {
                if (s->var_type->kind == TYPE_ARRAY && s->var_type->array_type.len == 0 && it &&
                    it->kind == TYPE_ARRAY && it->array_type.len > 0 &&
                    types_equal(s->var_type->array_type.elem_type, it->array_type.elem_type)) {
                    s->var_type = type_array(s->var_type->array_type.elem_type, it->array_type.len);
                    scope_type  = s->var_type;
                    it          = s->var_type;
                }

                if (it && !types_equal(it, s->var_type) && !types_assignable(it, s->var_type)) {
                    char is[64], vs[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->init->loc,
                        "constant '%s': expected %s, initializer has type %s",
                        s->var_name,
                        type_str(s->var_type, vs, sizeof(vs)),
                        type_str(it, is, sizeof(is))
                    );
                    diag_type_hint(it, s->var_type, s->init->loc);
                    scope_type = it;
                }
            }
            s->type = s->var_type;

            SymbolEntry* existing = scope_lookup(sc, s->var_name);
            if (existing && existing->decl != NULL) {
                diag_emit(DIAG_ERROR, s->loc, "redefinition of constant '%s'", s->var_name);
            } else {
                scope_add(sc, s->var_name, s, scope_type);
            }
            break;
        }

        case AST_ASSIGN: {
            if (s->assign_target->kind == AST_IDENT) {
                SymbolEntry* sym = scope_lookup(sc, s->assign_target->ident);
                if (!sym) {
                    diag_emit(DIAG_ERROR, s->loc, "undefined variable '%s'", s->assign_target->ident);
                    check_expr(s->assign_value, sc);
                    break;
                }
                Type* vt = check_expr_hint(s->assign_value, sc, sym->type);
                if (!types_equal(sym->type, vt) && !types_assignable(vt, sym->type)) {
                    char ss[64], vs[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "assignment type mismatch: '%s' has type %s, value has type %s",
                        s->assign_target->ident,
                        type_str(sym->type, ss, sizeof(ss)),
                        type_str(vt, vs, sizeof(vs))
                    );
                    diag_type_hint(vt, sym->type, s->assign_value->loc);
                }
                if (s->assign_op != ASSIGN_EQ && !is_numeric(sym->type)) {
                    char ts[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "compound assignment requires numeric type, got %s",
                        type_str(sym->type, ts, sizeof(ts))
                    );
                }
                s->type = sym->type;
            } else if (s->assign_target->kind == AST_INDEX) {
                Type* array_type = check_expr(s->assign_target->array, sc);
                Type* int_hint   = type_number(TYPE_INT, 64, false);
                Type* index_type = check_expr_hint(s->assign_target->index, sc, int_hint);

                if (!is_integer(index_type)) {
                    char s_str[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->assign_target->index->loc,
                        "array index must be integer, got %s",
                        type_str(index_type, s_str, sizeof(s_str))
                    );
                }

                Type* elem_type = NULL;
                if (is_error_type(array_type)) {
                    elem_type = ERROR_TYPE;
                } else if (array_type && array_type->kind == TYPE_ARRAY) {
                    elem_type = resolve_type(array_type->array_type.elem_type, sc);
                } else if (array_type && array_type->kind == TYPE_PTR) {
                    elem_type = resolve_type(array_type->ptr_type.elem_type, sc);
                } else {
                    char s_str[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "cannot index non-array/pointer type %s",
                        type_str(array_type, s_str, sizeof(s_str))
                    );
                    elem_type = ERROR_TYPE;
                }

                Type* vt = check_expr_hint(s->assign_value, sc, elem_type);
                if (elem_type && !types_equal(elem_type, vt) && !types_assignable(vt, elem_type)) {
                    char et[64], vt_str[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "assignment type mismatch: element has type %s, value has type %s",
                        type_str(elem_type, et, sizeof(et)),
                        type_str(vt, vt_str, sizeof(vt_str))
                    );
                    diag_type_hint(vt, elem_type, s->assign_value->loc);
                }

                if (s->assign_op != ASSIGN_EQ && elem_type && !is_numeric(elem_type)) {
                    char ts[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "compound assignment requires numeric type, got %s",
                        type_str(elem_type, ts, sizeof(ts))
                    );
                }
                s->type = elem_type;
            } else if (s->assign_target->kind == AST_MEMBER) {
                Type* value_type = check_expr(s->assign_target->member_value, sc);

                if (is_error_type(value_type)) {
                    check_expr(s->assign_value, sc);
                    break;
                }

                bool is_fat_ptr = value_type && value_type->kind == TYPE_PTR && value_type->ptr_type.is_fat;
                if (!is_fat_ptr) {
                    diag_emit(DIAG_ERROR, s->loc, ".len assignment only allowed on fat pointers");
                    check_expr(s->assign_value, sc);
                    break;
                }

                if (strcmp(s->assign_target->member_name, "len") != 0 &&
                    strcmp(s->assign_target->member_name, "length") != 0 &&
                    strcmp(s->assign_target->member_name, "count") != 0) {
                    diag_emit(DIAG_ERROR, s->loc, "fat pointer only has writable member 'len'");
                    check_expr(s->assign_value, sc);
                    break;
                }

                Type* target_type = type_abstract_int();
                Type* vt          = check_expr_hint(s->assign_value, sc, target_type);

                if (!types_assignable(vt, target_type)) {
                    char vt_str[64];
                    diag_emit(
                        DIAG_ERROR,
                        s->loc,
                        "cannot assign %s to fat pointer length (expected integer)",
                        type_str(vt, vt_str, sizeof(vt_str))
                    );
                }

                if (s->assign_op != ASSIGN_EQ) {
                    diag_emit(DIAG_ERROR, s->loc, "compound assignment not allowed on fat pointer length");
                }

                s->type = target_type;
            } else {
                diag_emit(
                    DIAG_ERROR, s->loc, "assignment target must be a variable, array index, or fat pointer member"
                );
                check_expr(s->assign_value, sc);
            }
            break;
        }

        default:
            ICE("unexpected node in statement context");
    }
}

static bool is_valid_main_return_type(Type* t) {
    if (!t)
        return false;
    if (t->kind == TYPE_VOID)
        return true;
    if (t->kind == TYPE_INT)
        return true;
    return false;
}

static bool is_fat_ptr_fat_ptr_u8(Type* t) {
    if (!t || t->kind != TYPE_PTR)
        return false;
    if (!t->ptr_type.is_fat)
        return false;

    Type* elem = t->ptr_type.elem_type;
    if (!elem || elem->kind != TYPE_PTR)
        return false;
    if (!elem->ptr_type.is_fat)
        return false;

    Type* elem2 = elem->ptr_type.elem_type;
    if (!elem2 || elem2->kind != TYPE_INT)
        return false;
    if (elem2->bits != 8)
        return false;

    return true;
}

static bool is_fat_ptr_thin_ptr_u8(Type* t) {
    if (!t || t->kind != TYPE_PTR)
        return false;
    if (!t->ptr_type.is_fat)
        return false;

    Type* elem = t->ptr_type.elem_type;
    if (!elem || elem->kind != TYPE_PTR)
        return false;
    if (elem->ptr_type.is_fat)
        return false;

    Type* elem2 = elem->ptr_type.elem_type;
    if (!elem2 || elem2->kind != TYPE_INT)
        return false;
    if (elem2->bits != 8)
        return false;

    return true;
}

static bool is_thin_ptr_thin_ptr_u8(Type* t) {
    if (!t || t->kind != TYPE_PTR)
        return false;
    if (t->ptr_type.is_fat)
        return false;

    Type* elem = t->ptr_type.elem_type;
    if (!elem || elem->kind != TYPE_PTR)
        return false;
    if (elem->ptr_type.is_fat)
        return false;

    Type* elem2 = elem->ptr_type.elem_type;
    if (!elem2 || elem2->kind != TYPE_INT)
        return false;
    if (elem2->bits != 8)
        return false;

    return true;
}

static bool validate_variadic_params(AstNode* func_decl) {
    if (!func_decl || (func_decl->kind != AST_FUNC_DECL && func_decl->kind != AST_EXTERN_DECL))
        return true;

    if (func_decl->is_variadic && func_decl->param_count > 0) {
        Param* var_param = &func_decl->params[func_decl->param_count - 1];
        if (var_param->name != NULL) {
            Type* var_type = var_param->type;
            if (!var_type || var_type->kind != TYPE_ARRAY) {
                char ts[64];
                diag_emit(
                    DIAG_ERROR,
                    var_param->loc,
                    "variadic parameter must have array type, got %s",
                    type_str(var_type, ts, sizeof(ts))
                );
                return false;
            }
        }
    }

    return true;
}

static bool is_compile_time_expr(AstNode* expr) {
    if (!expr)
        return false;

    switch (expr->kind) {
        case AST_INT_LIT:
        case AST_FLOAT_LIT:
        case AST_STRING_LIT:
        case AST_NULLPTR:
        case AST_TYPE_LIT:
            return true;
        case AST_ARRAY_LIT:
            for (int i = 0; i < expr->element_count; i++) {
                if (!is_compile_time_expr(expr->elements[i]))
                    return false;
            }
            return true;
        case AST_CAST:
            return is_compile_time_expr(expr->cast_expr);
        case AST_UNOP:
            return is_compile_time_expr(expr->operand);
        case AST_BINOP:
            return is_compile_time_expr(expr->lhs) && is_compile_time_expr(expr->rhs);
        case AST_RANGE:
            return is_compile_time_expr(expr->range_start) && is_compile_time_expr(expr->range_end) &&
                   (!expr->range_step || is_compile_time_expr(expr->range_step));
        default:
            return false;
    }
}

static bool validate_default_args(AstNode* func_decl) {
    if (!func_decl || (func_decl->kind != AST_FUNC_DECL && func_decl->kind != AST_EXTERN_DECL))
        return true;

    if (func_decl->is_variadic) {
        for (int i = 0; i < func_decl->param_count; i++) {
            if (func_decl->params[i].default_value) {
                diag_emit(DIAG_ERROR, func_decl->params[i].loc, "variadic functions cannot have default arguments");
                return false;
            }
        }
    }

    bool found_default = false;
    for (int i = 0; i < func_decl->param_count; i++) {
        if (func_decl->params[i].default_value) {
            found_default = true;
        } else if (found_default) {
            diag_emit(DIAG_ERROR, func_decl->params[i].loc, "non-default parameter follows default parameter");
            return false;
        }
    }

    for (int i = 0; i < func_decl->param_count; i++) {
        if (func_decl->params[i].default_value) {
            if (!is_compile_time_expr(func_decl->params[i].default_value)) {
                diag_emit(DIAG_ERROR, func_decl->params[i].loc, "default argument must be a compile-time constant");
                return false;
            }
        }
    }

    return true;
}

static bool validate_rei_main_signature(AstNode* func_decl) {
    if (!func_decl || func_decl->kind != AST_FUNC_DECL)
        return false;

    if (!is_valid_main_return_type(func_decl->ret_type)) {
        diag_emit(DIAG_ERROR, func_decl->loc, "main function must return void or int type");
        return false;
    }

    int param_count = func_decl->param_count;

    if (param_count == 0) {
        return true;
    } else if (param_count == 1) {
        Type* param_type = func_decl->params[0].type;
        if (is_fat_ptr_fat_ptr_u8(param_type) || is_fat_ptr_thin_ptr_u8(param_type)) {
            return true;
        }
        if (is_thin_ptr_thin_ptr_u8(param_type)) {
            diag_emit(DIAG_ERROR, func_decl->loc, "**u8 parameter requires argc: main must have both argc and argv");
            return false;
        }
        diag_emit(DIAG_ERROR, func_decl->loc, "main function with 1 argument must be [][]u8 or []*u8");
        return false;
    } else if (param_count == 2) {
        Type* p0 = func_decl->params[0].type;
        Type* p1 = func_decl->params[1].type;

        bool p0_is_thin_ptr_u8 = is_thin_ptr_thin_ptr_u8(p0);
        bool p1_is_thin_ptr_u8 = is_thin_ptr_thin_ptr_u8(p1);
        bool p0_is_int         = p0 && p0->kind == TYPE_INT;
        bool p1_is_int         = p1 && p1->kind == TYPE_INT;

        if ((p0_is_int && p1_is_thin_ptr_u8) || (p0_is_thin_ptr_u8 && p1_is_int)) {
            return true;
        }

        diag_emit(DIAG_ERROR, func_decl->loc, "main function with 2 arguments must have one **u8 and one integer type");
        return false;
    } else {
        diag_emit(DIAG_ERROR, func_decl->loc, "main function can have 0, 1, or 2 arguments, got %d", param_count);
        return false;
    }
}

static bool check_annotation(AstNode* annot, AstNode* target) {
    if (!annot || annot->kind != AST_ANNOTATION)
        return false;

    switch (annot->annot_type) {
        case ANNOT_PRINTF_LIKE: {
            if (!target || (target->kind != AST_FUNC_DECL && target->kind != AST_EXTERN_DECL)) {
                diag_emit(
                    DIAG_ERROR,
                    annot->loc,
                    "printf_like annotation can only be applied to function or extern declarations"
                );
                return false;
            }

            if (annot->annot_arg_parse_failed) {
                diag_emit(DIAG_ERROR, annot->loc, "printf_like annotation has invalid arguments");
                return false;
            }

            if (annot->annot_expr_count != 1) {
                diag_emit(DIAG_ERROR, annot->loc, "printf_like requires exactly one argument");
                diag_emit(
                    DIAG_NOTE,
                    annot->loc,
                    "use an identifier, string literal, or index: #printf_like(fmt), #printf_like(\"fmt\"), or "
                    "#printf_like(0)"
                );
                return false;
            }

            AstNode* arg           = annot->annot_exprs[0];
            int      fmt_param_idx = -1;

            if (arg->kind == AST_IDENT) {
                for (int i = 0; i < target->param_count; i++) {
                    if (target->params[i].name && strcmp(target->params[i].name, arg->ident) == 0) {
                        fmt_param_idx = i;
                        break;
                    }
                }
                if (fmt_param_idx == -1) {
                    diag_emit(
                        DIAG_ERROR,
                        arg->loc,
                        "printf_like: parameter '%s' not found in function '%s'",
                        arg->ident,
                        target->function_name
                    );
                    return false;
                }
            } else if (arg->kind == AST_STRING_LIT) {
                for (int i = 0; i < target->param_count; i++) {
                    if (target->params[i].name && arg->len > 0 &&
                        strncmp(target->params[i].name, arg->str, arg->len) == 0 &&
                        target->params[i].name[arg->len] == '\0') {
                        fmt_param_idx = i;
                        break;
                    }
                }
                if (fmt_param_idx == -1) {
                    diag_emit(
                        DIAG_ERROR,
                        arg->loc,
                        "printf_like: parameter '%.*s' not found in function '%s'",
                        (int)arg->len,
                        arg->str,
                        target->function_name
                    );
                    return false;
                }
            } else if (arg->kind == AST_INT_LIT) {
                fmt_param_idx = (int)arg->ival;
                if (fmt_param_idx < 0 || fmt_param_idx >= target->param_count) {
                    diag_emit(
                        DIAG_ERROR,
                        arg->loc,
                        "printf_like: parameter index %d out of range (function has %d parameters)",
                        fmt_param_idx,
                        target->param_count
                    );
                    return false;
                }
            } else {
                diag_emit(
                    DIAG_ERROR, arg->loc, "printf_like argument must be an identifier, string literal, or integer index"
                );
                return false;
            }

            Type* fmt_param_type = target->params[fmt_param_idx].type;
            if (!is_thin_ptr_u8(fmt_param_type) && !is_fat_ptr_u8(fmt_param_type)) {
                char ts[64];
                diag_emit(
                    DIAG_ERROR,
                    annot->loc,
                    "printf_like format parameter must be *u8 or []u8, got %s",
                    type_str(fmt_param_type, ts, sizeof(ts))
                );
                return false;
            }

            if (!target->is_variadic) {
                diag_emit(
                    DIAG_ERROR, annot->loc, "printf_like cannot be used on functions without variadic parameters"
                );
                return false;
            }

            target->is_printf_like = 1;
            target->param_idx      = fmt_param_idx;

            return true;
        }

        default:
            ICE("TODO: handle other annotation types");
            return false;
    }
}

static Type* convert_type_idents(Type* t, Scope* scope) {
    if (!t)
        return t;

    if (t->kind == TYPE_IDENT) {
        return resolve_type(t, scope);
    } else if (t->kind == TYPE_PTR) {
        t->ptr_type.elem_type = convert_type_idents(t->ptr_type.elem_type, scope);
        return t;
    } else if (t->kind == TYPE_ARRAY) {
        t->array_type.elem_type = convert_type_idents(t->array_type.elem_type, scope);
        return t;
    }

    return t;
}

static void convert_stmt_type_idents(AstNode* stmt, Scope* scope);

static void convert_stmt_type_idents(AstNode* stmt, Scope* scope) {
    if (!stmt)
        return;

    switch (stmt->kind) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            stmt->var_type = convert_type_idents(stmt->var_type, scope);
            break;
        case AST_BLOCK_STMT:
            for (int i = 0; i < stmt->stmt_count; i++)
                convert_stmt_type_idents(stmt->stmts[i], scope);
            break;
        case AST_IF_STMT:
            convert_stmt_type_idents(stmt->then_branch, scope);
            if (stmt->else_branch)
                convert_stmt_type_idents(stmt->else_branch, scope);
            break;
        case AST_WHILE_STMT:
            convert_stmt_type_idents(stmt->while_body, scope);
            break;
        case AST_FOR_STMT:
            convert_stmt_type_idents(stmt->for_body, scope);
            break;
        case AST_FUNC_DECL: {
            for (int i = 0; i < stmt->param_count; i++)
                stmt->params[i].type = convert_type_idents(stmt->params[i].type, scope);
            stmt->ret_type = convert_type_idents(stmt->ret_type, scope);
            if (stmt->body)
                convert_stmt_type_idents(stmt->body, scope);
            break;
        }
        case AST_EXTERN_DECL: {
            for (int i = 0; i < stmt->param_count; i++)
                stmt->params[i].type = convert_type_idents(stmt->params[i].type, scope);
            stmt->ret_type = convert_type_idents(stmt->ret_type, scope);
            break;
        }
        default:
            break;
    }
}

static AstNode* preprocess_expr(AstNode* expr, Scope* scope);
static AstNode* preprocess_stmt(AstNode* stmt, Scope* scope);

static AstNode* preprocess_expr(AstNode* expr, Scope* scope) {
    if (!expr)
        return NULL;

    switch (expr->kind) {
        case AST_IDENT: {
            SymbolEntry* sym = scope_lookup(scope, expr->ident);
            if (sym && sym->decl && sym->decl->kind == AST_CONST_DECL && sym->decl->init) {
                return sym->decl->init;
            }
            return expr;
        }

        case AST_INT_LIT:
        case AST_FLOAT_LIT:
        case AST_STRING_LIT:
        case AST_TYPE_LIT:
            return expr;

        case AST_ARRAY_LIT:
            for (size_t i = 0; i < expr->element_count; i++)
                expr->elements[i] = preprocess_expr(expr->elements[i], scope);
            return expr;

        case AST_CALL:
            for (int i = 0; i < expr->arg_count; i++)
                expr->args[i] = preprocess_expr(expr->args[i], scope);
            return expr;

        case AST_INDEX:
            expr->array = preprocess_expr(expr->array, scope);
            expr->index = preprocess_expr(expr->index, scope);
            return expr;

        case AST_MEMBER:
            expr->member_value = preprocess_expr(expr->member_value, scope);
            return expr;

        case AST_RANGE:
            expr->range_start = preprocess_expr(expr->range_start, scope);
            expr->range_end   = preprocess_expr(expr->range_end, scope);
            expr->range_step  = preprocess_expr(expr->range_step, scope);
            return expr;

        case AST_BINOP:
            expr->lhs = preprocess_expr(expr->lhs, scope);
            expr->rhs = preprocess_expr(expr->rhs, scope);
            return expr;

        case AST_UNOP:
            expr->operand = preprocess_expr(expr->operand, scope);
            return expr;

        case AST_CAST:
            expr->cast_expr = preprocess_expr(expr->cast_expr, scope);
            return expr;

        default:
            return expr;
    }
}

static AstNode* preprocess_stmt(AstNode* stmt, Scope* scope) {
    if (!stmt)
        return NULL;

    switch (stmt->kind) {
        case AST_VAR_DECL:
            stmt->init = preprocess_expr(stmt->init, scope);
            return stmt;

        case AST_CONST_DECL:
            stmt->init = preprocess_expr(stmt->init, scope);
            return stmt;

        case AST_RETURN_STMT:
            stmt->ret_val = preprocess_expr(stmt->ret_val, scope);
            return stmt;

        case AST_EXPR_STMT:
            stmt->expr = preprocess_expr(stmt->expr, scope);
            return stmt;

        case AST_BLOCK_STMT:
            for (int i = 0; i < stmt->stmt_count; i++)
                stmt->stmts[i] = preprocess_stmt(stmt->stmts[i], scope);
            return stmt;

        case AST_IF_STMT:
            stmt->if_cond     = preprocess_expr(stmt->if_cond, scope);
            stmt->then_branch = preprocess_stmt(stmt->then_branch, scope);
            stmt->else_branch = preprocess_stmt(stmt->else_branch, scope);
            return stmt;

        case AST_WHILE_STMT:
            stmt->while_cond = preprocess_expr(stmt->while_cond, scope);
            stmt->while_body = preprocess_stmt(stmt->while_body, scope);
            return stmt;

        case AST_FOR_STMT:
            stmt->for_val      = preprocess_expr(stmt->for_val, scope);
            stmt->for_iterable = preprocess_expr(stmt->for_iterable, scope);
            stmt->for_body     = preprocess_stmt(stmt->for_body, scope);
            return stmt;

        case AST_ASSIGN:
            stmt->assign_target = preprocess_expr(stmt->assign_target, scope);
            stmt->assign_value  = preprocess_expr(stmt->assign_value, scope);
            return stmt;

        case AST_FUNC_DECL:
        case AST_EXTERN_DECL:
            if (stmt->init)
                stmt->init = preprocess_expr(stmt->init, scope);
            if (stmt->body)
                stmt->body = preprocess_stmt(stmt->body, scope);
            return stmt;

        default:
            return stmt;
    }
}

typedef struct {
    AstNode* func_decl;
    int      fmt_param_idx;
} PrintfLikeInfo;

static PrintfLikeInfo printf_like_funcs[256];
static int            printf_like_count = 0;

static PrintfLikeInfo* find_printf_like_info(const char* func_name) {
    for (int i = 0; i < printf_like_count; i++) {
        if (strcmp(printf_like_funcs[i].func_decl->function_name, func_name) == 0) {
            return &printf_like_funcs[i];
        }
    }
    return NULL;
}

int semantic_check(Module* m, const CompileConfig* config) {
    int    prev   = diag_error_count;
    Scope* global = scope_new(NULL);

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_FUNC_DECL || d->kind == AST_EXTERN_DECL) {
            validate_variadic_params(d);
            validate_default_args(d);
            if (scope_lookup(global, d->function_name))
                diag_emit(DIAG_ERROR, d->loc, "redefinition of '%s'", d->function_name);
            else
                scope_add(global, d->function_name, d, d->ret_type);
        }
        if (d->kind == AST_VAR_DECL || d->kind == AST_CONST_DECL) {
            if (scope_lookup(global, d->var_name))
                diag_emit(DIAG_ERROR, d->loc, "redefinition of '%s'", d->var_name);
            else
                scope_add(global, d->var_name, d, d->var_type);
        }
    }

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_FUNC_DECL || d->kind == AST_EXTERN_DECL) {
            if (d->body)
                d->body = preprocess_stmt(d->body, global);
        } else if (d->kind == AST_VAR_DECL || d->kind == AST_CONST_DECL) {
            if (d->init)
                d->init = preprocess_expr(d->init, global);
        }
    }

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        convert_stmt_type_idents(d, global);
    }

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_ANNOTATION) {
            AstNode* target = NULL;

            if (d->annot_type >= ANNOT_NO_MANGLE) {
                for (int j = i + 1; j < m->count; j++) {
                    if (m->decls[j]->kind != AST_ANNOTATION) {
                        target = m->decls[j];
                        break;
                    }
                }
            }

            d->annot_target = target;

            if (d->annot_type >= ANNOT_NO_MANGLE) {
                check_annotation(d, target);
            }
        }
    }

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_VAR_DECL && d->init) {
            Type* it = check_expr_hint(d->init, global, d->var_type);
            if (d->var_type && it && !types_equal(it, d->var_type) && !types_assignable(it, d->var_type)) {
                char is[64], vs[64];
                diag_emit(
                    DIAG_ERROR,
                    d->init->loc,
                    "global '%s': expected %s, initializer has type %s",
                    d->var_name,
                    type_str(d->var_type, vs, sizeof(vs)),
                    type_str(it, is, sizeof(is))
                );
                diag_type_hint(it, d->var_type, d->init->loc);
            }
        }
        if (d->kind == AST_CONST_DECL && d->init) {
            if (d->var_type) {
                Type* it = check_expr_hint(d->init, global, d->var_type);
                if (it && !types_equal(it, d->var_type) && !types_assignable(it, d->var_type)) {
                    char is[64], vs[64];
                    diag_emit(
                        DIAG_ERROR,
                        d->init->loc,
                        "global '%s': expected %s, initializer has type %s",
                        d->var_name,
                        type_str(d->var_type, vs, sizeof(vs)),
                        type_str(it, is, sizeof(is))
                    );
                    diag_type_hint(it, d->var_type, d->init->loc);
                }
            }
        }
    }

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind != AST_FUNC_DECL)
            continue;
        Scope* fn_sc = scope_new(global);
        for (int p = 0; p < d->param_count; p++)
            if (d->params[p].name)
                if (scope_lookup(fn_sc, d->params[p].name))
                    diag_emit(DIAG_ERROR, d->params[p].loc, "redefinition of parameter '%s'", d->params[p].name);
                else
                    scope_add(fn_sc, d->params[p].name, NULL, d->params[p].type);
        if (d->body)
            check_stmt(d->body, fn_sc, d);
    }

    if (config && !config->no_main && !config->is_library) {
        AstNode*    main_func = NULL;
        const char* main_name = config->no_rei_main ? "main" : "main";

        for (int i = 0; i < m->count; i++) {
            if (m->decls[i]->kind == AST_FUNC_DECL && strcmp(m->decls[i]->function_name, main_name) == 0) {
                main_func = m->decls[i];
                break;
            }
        }

        if (!main_func) {
            Location loc = {0};
            loc.file     = m->filepath;
            if (m->count > 0 && m->decls[0]->kind == AST_FUNC_DECL) {
                loc = m->decls[0]->loc;
            }
            diag_emit(DIAG_ERROR, loc, "main function not found; a program must have a main function");
            diag_emit(
                DIAG_NOTE,
                loc,
                "Add a main function like:\n"
                "\n"
                "    main :: () -> i32 {\n"
                "        return 0;\n"
                "    }\n"
                "\n"
                "main can have 0 to 2 arguments (for argc/argv), "
                "and return void or any integer type\n"
                "If you dont want a main function use flag '--no-main' to compile as a library"
            );
            diag_error_count++;
            return -1;
        }

        if (!config->no_rei_main && !validate_rei_main_signature(main_func)) {
            return -1;
        }
    }

    return diag_error_count > prev ? -1 : 0;
}
