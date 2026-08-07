#include "semantic.h"

#include "../thirdparty/ht.h"
#include "diagnostics.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool types_equal(Type* a, Type* b) {
    if (!a || !b)
        return a == b;
    if (a->kind != b->kind)
        return false;
    if (a->int_type.is_abstract != b->int_type.is_abstract)
        return false;
    if (a->int_type.is_size != b->int_type.is_size)
        return false;
    switch (a->kind) {
        case TYPE_VOID:
            return true;
        case TYPE_INT:
            return a->bits == b->bits && a->int_type.is_unsigned == b->int_type.is_unsigned;
        case TYPE_FLOAT:
            return a->bits == b->bits;
        case TYPE_PTR:
            return a->ptr_type.is_fat == b->ptr_type.is_fat &&
                   types_equal(a->ptr_type.elem_type, b->ptr_type.elem_type);
        case TYPE_ARRAY:
            return a->array_type.len == b->array_type.len &&
                   types_equal(a->array_type.elem_type, b->array_type.elem_type);
        case TYPE_UNSUPPORTED:
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
            snprintf(buf, cap, t->ptr_type.is_fat ? "[]%s" : "*%s", inner);
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
        case TYPE_UNSUPPORTED:
            snprintf(buf, cap, "<unsupported>");
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
        diag_emit(DIAG_NOTE,
                  arg_loc,
                  "c\"...\" string literals have type *u8 (null-terminated C string); "
                  "use a plain \"...\" literal to get []u8 (a fat pointer / slice)");
        return;
    }

    if (actual->kind == TYPE_PTR && expected->kind == TYPE_PTR) {
        Type* actual_inner   = actual->ptr_type.elem_type;
        Type* expected_inner = expected->ptr_type.elem_type;

        if (expected_inner && expected_inner->kind == TYPE_PTR && types_equal(actual, expected_inner)) {
            char ts[64];
            diag_emit(DIAG_NOTE,
                      arg_loc,
                      "you have %s but %s is required; "
                      "consider taking the address with &x to get an extra level of indirection",
                      type_str(actual, ts, sizeof(ts)),
                      type_str(expected, ts, sizeof(ts)));
            return;
        }

        if (actual_inner && actual_inner->kind == TYPE_PTR && types_equal(actual_inner, expected)) {
            char as[64], es[64];
            diag_emit(DIAG_NOTE,
                      arg_loc,
                      "you have %s but %s is required; "
                      "consider dereferencing with *x to remove one level of indirection",
                      type_str(actual, as, sizeof(as)),
                      type_str(expected, es, sizeof(es)));
            return;
        }

        if (actual->ptr_type.is_fat && !expected->ptr_type.is_fat && actual_inner &&
            types_equal(actual_inner, expected_inner)) {
            diag_emit(DIAG_NOTE,
                      arg_loc,
                      "fat pointer (slice) cannot coerce to thin pointer automatically; "
                      "use &x[0] to get a pointer to the first element");
            return;
        }

        if (!actual->ptr_type.is_fat && expected->ptr_type.is_fat && actual_inner &&
            types_equal(actual_inner, expected_inner)) {
            diag_emit(DIAG_NOTE,
                      arg_loc,
                      "thin pointer cannot coerce to a fat pointer (slice) automatically; "
                      "wrap it in a slice expression or change the declaration");
            return;
        }
    }

    if (expected->kind == TYPE_PTR && !expected->ptr_type.is_fat && actual->kind != TYPE_PTR) {
        char es[64];
        if (expected->ptr_type.elem_type && types_equal(actual, expected->ptr_type.elem_type)) {
            diag_emit(DIAG_NOTE,
                      arg_loc,
                      "expected a pointer %s; if you have a value, take its address with &x",
                      type_str(expected, es, sizeof(es)));
            return;
        }
    }

    if (actual->kind == TYPE_PTR && !actual->ptr_type.is_fat && expected->kind != TYPE_PTR) {
        char as[64];
        if (actual->ptr_type.elem_type && types_equal(actual->ptr_type.elem_type, expected)) {
            diag_emit(DIAG_NOTE,
                      arg_loc,
                      "you have a pointer %s but a value is expected; "
                      "dereference it with *x",
                      type_str(actual, as, sizeof(as)));
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
    struct Scope* parent;
} Scope;

static Scope* scope_new(Scope* parent) {
    Scope* s       = calloc(1, sizeof(*s));
    s->syms.hasheq = ht_cstr_hasheq;
    s->parent      = parent;
    return s;
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

static Type* check_expr_hint(AstNode* e, Scope* sc, Type* hint);
static void  check_stmt(AstNode* s, Scope* sc, AstNode* fn);

static Type* check_expr(AstNode* e, Scope* sc) {
    return check_expr_hint(e, sc, NULL);
}

static Type* check_expr_hint(AstNode* e, Scope* sc, Type* hint) {
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
                    e->type = type_ptr(u8, false); /* *u8  */
                else if (flags & STR_PREFIX_B)
                    e->type = type_array(u8, 0);   /* [u8] */
                else
                    e->type = type_ptr(u8, true);  /* []u8 */
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
                    diag_emit(DIAG_ERROR,
                              e->loc,
                              "cannot infer type of empty array literal; "
                              "annotate the variable");
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
                    diag_emit(DIAG_ERROR,
                              e->elements[i]->loc,
                              "array literal element %zu has type %s, expected %s",
                              i,
                              type_str(et, fs, sizeof(fs)),
                              type_str(elem_type, es, sizeof(es)));
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
                e->type = sym->type;
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
            for (int i = 0; i < fn->param_count; i++) {
                if (fn->params[i].is_variadic) {
                    variadic_index = i;
                    break;
                }
            }

            if (variadic_index == -1) {
                if (e->arg_count != fn->param_count)
                    diag_emit(DIAG_ERROR,
                              e->loc,
                              "'%s' expects %d arg(s), got %d",
                              fn->function_name,
                              fn->param_count,
                              e->arg_count);
            } else {
                if (e->arg_count < variadic_index)
                    diag_emit(DIAG_ERROR,
                              e->loc,
                              "'%s' expects at least %d arg(s), got %d",
                              fn->function_name,
                              variadic_index,
                              e->arg_count);
            }

            for (int i = 0; i < e->arg_count; i++) {
                if (i < variadic_index || (variadic_index == -1 && i < fn->param_count)) {
                    Type* param_hint = fn->params[i].type;
                    Type* at         = check_expr_hint(e->args[i], sc, param_hint);
                    Type* pt         = fn->params[i].type;
                    if (!types_assignable(at, pt)) {
                        char as[64], ps[64];
                        diag_emit(DIAG_ERROR,
                                  e->args[i]->loc,
                                  "argument %d of '%s': expected %s, got %s",
                                  i + 1,
                                  fn->function_name,
                                  type_str(pt, ps, sizeof(ps)),
                                  type_str(at, as, sizeof(as)));
                        diag_type_hint(at, pt, e->args[i]->loc);
                    }
                } else if (variadic_index != -1 && i >= variadic_index) {
                    Type* var_param_type = fn->params[variadic_index].type;
                    if (var_param_type && var_param_type->kind == TYPE_ARRAY) {
                        Type* elem_type = var_param_type->array_type.elem_type;
                        Type* at        = check_expr_hint(e->args[i], sc, elem_type);
                        if (!types_assignable(at, elem_type)) {
                            char as[64], es[64];
                            diag_emit(DIAG_ERROR,
                                      e->args[i]->loc,
                                      "argument %d of '%s': expected %s, got %s",
                                      i + 1,
                                      fn->function_name,
                                      type_str(elem_type, es, sizeof(es)),
                                      type_str(at, as, sizeof(as)));
                            diag_type_hint(at, elem_type, e->args[i]->loc);
                        }
                    } else {
                        check_expr(e->args[i], sc);
                    }
                }
            }
            e->type = fn->ret_type;
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

            if (is_pointer(lt) || is_pointer(rt)) {
                if (e->op == OP_ADD) {
                    if (is_pointer(lt) && is_pointer(rt)) {
                        diag_emit(DIAG_ERROR, e->loc, "cannot add two pointers");
                        e->type = ERROR_TYPE;
                        return e->type;
                    }
                    if (is_pointer(lt) && !is_integer(rt)) {
                        char s[64];
                        diag_emit(DIAG_ERROR,
                                  e->rhs->loc,
                                  "pointer arithmetic requires integer offset, got %s",
                                  type_str(rt, s, sizeof(s)));
                        e->type = lt;
                        return e->type;
                    }
                    if (is_pointer(rt) && !is_integer(lt)) {
                        char s[64];
                        diag_emit(DIAG_ERROR,
                                  e->lhs->loc,
                                  "pointer arithmetic requires integer offset, got %s",
                                  type_str(lt, s, sizeof(s)));
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
                        diag_emit(DIAG_ERROR,
                                  e->rhs->loc,
                                  "pointer arithmetic requires integer offset, got %s",
                                  type_str(rt, s, sizeof(s)));
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
                    diag_emit(DIAG_ERROR,
                              e->loc,
                              "binary operator '%s' not supported on pointer types (%s, %s)",
                              e->op == OP_MUL   ? "*"
                              : e->op == OP_DIV ? "/"
                              : e->op == OP_MOD ? "%"
                              : e->op == OP_POW ? "**"
                                                : "op",
                              type_str(lt, ls, sizeof(ls)),
                              type_str(rt, rs, sizeof(rs)));
                    e->type = lt ? lt : rt;
                    return e->type;
                }
            }

            if (lt && rt && !types_equal(lt, rt) && !is_error_type(lt) && !is_error_type(rt) && !is_abstract_int(lt) &&
                !is_abstract_int(rt)) {
                bool is_comparison = (e->op == OP_EQ || e->op == OP_NEQ || e->op == OP_LESS || e->op == OP_LESSEQ ||
                                      e->op == OP_MORE || e->op == OP_MOREEQ);

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
                diag_emit(DIAG_ERROR,
                          e->loc,
                          "type mismatch: %s and %s",
                          type_str(lt, ls, sizeof(ls)),
                          type_str(rt, rs, sizeof(rs)));
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
                    diag_emit(DIAG_ERROR,
                              e->lhs->loc,
                              "bitwise operator requires integer operand, got %s",
                              type_str(lt, s, sizeof(s)));
                }
                if (!is_integer(rt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR,
                              e->rhs->loc,
                              "bitwise operator requires integer operand, got %s",
                              type_str(rt, s, sizeof(s)));
                }
            }

            bool is_logical = (e->op == OP_LAND || e->op == OP_LOR);
            if (is_logical) {
                if (!is_numeric(lt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR,
                              e->lhs->loc,
                              "logical operator requires numeric operand, got %s",
                              type_str(lt, s, sizeof(s)));
                }
                if (!is_numeric(rt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR,
                              e->rhs->loc,
                              "logical operator requires numeric operand, got %s",
                              type_str(rt, s, sizeof(s)));
                }
            }

            bool is_cmp = (e->op == OP_EQ || e->op == OP_NEQ || e->op == OP_LESS || e->op == OP_MORE ||
                           e->op == OP_LESSEQ || e->op == OP_MOREEQ);

            if (is_cmp) {
                e->type = type_number(TYPE_INT, 32, 0);
            } else {
                if (is_abstract_int(lt) && is_abstract_int(rt)) {
                    e->type = NULL;
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
                            DIAG_ERROR, e->loc, "cannot dereference non-pointer type %s", type_str(t, s, sizeof(s)));
                        e->type = ERROR_TYPE;
                    } else if (t->ptr_type.is_fat) {
                        diag_emit(DIAG_ERROR,
                                  e->loc,
                                  "cannot dereference fat pointer (slice) directly; "
                                  "index it instead");
                        e->type = t->ptr_type.elem_type;
                    } else {
                        e->type = t->ptr_type.elem_type;
                    }
                    break;

                case UOP_ADDR: {
                    if (t && t->kind == TYPE_ARRAY) {
                        e->type = type_ptr(t->array_type.elem_type, false);
                    } else {
                        e->type = type_ptr(t, false);
                    }
                    break;
                    ;
                }

                case UOP_NOT:
                    if (!is_numeric(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR,
                                  e->loc,
                                  "logical not requires numeric operand, got %s",
                                  type_str(t, s, sizeof(s)));
                    }
                    e->type = type_number(TYPE_INT, 64, 0);
                    break;

                case UOP_BITNOT:
                    if (!is_integer(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR,
                                  e->loc,
                                  "bitwise not requires integer operand, got %s",
                                  type_str(t, s, sizeof(s)));
                    }
                    e->type = t;
                    break;

                case UOP_NEG:
                case UOP_POS:
                    if (!is_numeric(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR,
                                  e->loc,
                                  "unary %s requires numeric operand, got %s",
                                  e->uop == UOP_NEG ? "-" : "+",
                                  type_str(t, s, sizeof(s)));
                    }
                    e->type = t;
                    break;

                case UOP_PREINC:
                case UOP_PREDEC:
                case UOP_POSTINC:
                case UOP_POSTDEC:
                    if (!is_integer(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR,
                                  e->loc,
                                  "increment/decrement requires integer operand, got %s",
                                  type_str(t, s, sizeof(s)));
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
                diag_emit(DIAG_ERROR,
                          e->index->loc,
                          "array index must be integer, got %s",
                          type_str(index_type, s, sizeof(s)));
            }

            if (is_error_type(array_type)) {
                e->type = ERROR_TYPE;
            } else if (array_type && array_type->kind == TYPE_ARRAY) {
                e->type = array_type->array_type.elem_type;
            } else if (array_type && array_type->kind == TYPE_PTR) {
                e->type = array_type->ptr_type.elem_type;
            } else {
                char s[64];
                diag_emit(
                    DIAG_ERROR, e->loc, "cannot index non-array/pointer type %s", type_str(array_type, s, sizeof(s)));
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
                    e->type = NULL;
                    return e->type;
                } else {
                    diag_emit(DIAG_ERROR,
                              e->loc,
                              "%s has no member '%s'",
                              value_type->kind == TYPE_PTR ? "fat pointer" : "array",
                              e->member_name);
                    e->type = ERROR_TYPE;
                    return e->type;
                }
            }

            char s[64];
            diag_emit(
                DIAG_ERROR, e->loc, "member access not supported for type %s", type_str(value_type, s, sizeof(s)));
            e->type = ERROR_TYPE;
            return e->type;
        }

        case AST_CAST: {
            Type* expr_type = check_expr(e->cast_expr, sc);

            if (e->cast_type == NULL) {
                e->type = hint;
            } else {
                e->type = e->cast_type;
            }
            return e->type;
        }

        default:
            ICE("unexpected node in expression context");
            return type_void();
    }
}

static void check_stmt(AstNode* s, Scope* sc, AstNode* fn) {
    switch (s->kind) {
        case AST_VAR_DECL:
            break;
        case AST_CONST_DECL:
            break;
        default:
            break;
    }

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
                    diag_emit(DIAG_ERROR,
                              s->loc,
                              "return type mismatch: expected %s, got %s",
                              type_str(ret, rs, sizeof(rs)),
                              type_str(vt, vs, sizeof(vs)));
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
                diag_emit(DIAG_ERROR,
                          s->if_cond->loc,
                          "condition cannot be a pointer type (%s); dereference first",
                          type_str(ct, ts, sizeof(ts)));
            }
            check_stmt(s->then_branch, sc, fn);
            if (s->else_branch)
                check_stmt(s->else_branch, sc, fn);
            break;
        }

        case AST_WHILE_STMT: {
            Type* ct = check_expr(s->while_cond, sc);
            if (ct && ct->kind == TYPE_PTR) {
                char ts[64];
                diag_emit(DIAG_ERROR,
                          s->while_cond->loc,
                          "condition cannot be a pointer type (%s); dereference first",
                          type_str(ct, ts, sizeof(ts)));
            }
            check_stmt(s->while_body, sc, fn);
            break;
        }

        case AST_VAR_DECL: {
            s->type          = s->var_type;
            Type* scope_type = s->var_type;
            if (s->init) {
                Type* it = check_expr_hint(s->init, sc, s->var_type);
                if (s->var_type && !types_compatible_with_decay(it, s->var_type) &&
                    !types_assignable(it, s->var_type) && !is_error_type(it)) {
                    char is[64], vs[64];
                    diag_emit(DIAG_ERROR,
                              s->init->loc,
                              "initializer type mismatch: variable '%s' has type %s, "
                              "initializer has type %s",
                              s->var_name,
                              type_str(s->var_type, vs, sizeof(vs)),
                              type_str(it, is, sizeof(is)));
                    diag_type_hint(it, s->var_type, s->init->loc);
                    scope_type = it;
                }
            }
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

            if (s->var_type) {
                if (it && !types_equal(it, s->var_type)) {
                    char is[64], vs[64];
                    diag_emit(DIAG_ERROR,
                              s->init->loc,
                              "initializer type mismatch: constant '%s' has type %s, "
                              "initializer has type %s",
                              s->var_name,
                              type_str(s->var_type, vs, sizeof(vs)),
                              type_str(it, is, sizeof(is)));
                    diag_type_hint(it, s->var_type, s->init->loc);
                    scope_type = it;
                }
                s->type = s->var_type;
            } else {
                scope_type = NULL;
                s->type    = NULL;
            }

            SymbolEntry* existing = scope_lookup(sc, s->var_name);
            if (existing && existing->decl != NULL) {
                diag_emit(DIAG_ERROR, s->loc, "redefinition of constant '%s'", s->var_name);
            } else {
                scope_add(sc, s->var_name, s, scope_type);
            }
            break;
        }

        case AST_VAR_ASSIGN: {
            SymbolEntry* sym = scope_lookup(sc, s->assign_name);
            if (!sym) {
                diag_emit(DIAG_ERROR, s->loc, "undefined variable '%s'", s->assign_name);
                check_expr(s->assign_value, sc);
                break;
            }
            Type* vt = check_expr_hint(s->assign_value, sc, sym->type);
            if (!types_equal(sym->type, vt) && !types_assignable(vt, sym->type)) {
                char ss[64], vs[64];
                diag_emit(DIAG_ERROR,
                          s->loc,
                          "assignment type mismatch: '%s' has type %s, value has type %s",
                          s->assign_name,
                          type_str(sym->type, ss, sizeof(ss)),
                          type_str(vt, vs, sizeof(vs)));
                diag_type_hint(vt, sym->type, s->assign_value->loc);
            }
            if (s->assign_op != ASSIGN_EQ && !is_numeric(sym->type)) {
                char ts[64];
                diag_emit(DIAG_ERROR,
                          s->loc,
                          "compound assignment requires numeric type, got %s",
                          type_str(sym->type, ts, sizeof(ts)));
            }
            s->type = sym->type;
            break;
        }

        case AST_INDEX_ASSIGN: {
            Type* array_type = check_expr(s->idx_array, sc);
            Type* int_hint   = type_number(TYPE_INT, 64, false);
            Type* index_type = check_expr_hint(s->idx_index, sc, int_hint);

            if (!is_integer(index_type)) {
                char s_str[64];
                diag_emit(DIAG_ERROR,
                          s->idx_index->loc,
                          "array index must be integer, got %s",
                          type_str(index_type, s_str, sizeof(s_str)));
            }

            Type* elem_type = NULL;
            if (is_error_type(array_type)) {
                elem_type = ERROR_TYPE;
            } else if (array_type && array_type->kind == TYPE_ARRAY) {
                elem_type = array_type->array_type.elem_type;
            } else if (array_type && array_type->kind == TYPE_PTR) {
                elem_type = array_type->ptr_type.elem_type;
            } else {
                char s_str[64];
                diag_emit(DIAG_ERROR,
                          s->loc,
                          "cannot index non-array/pointer type %s",
                          type_str(array_type, s_str, sizeof(s_str)));
                elem_type = ERROR_TYPE;
            }

            Type* vt = check_expr_hint(s->idx_assign_value, sc, elem_type);
            if (elem_type && !types_equal(elem_type, vt) && !types_assignable(vt, elem_type)) {
                char et[64], vt_str[64];
                diag_emit(DIAG_ERROR,
                          s->loc,
                          "assignment type mismatch: element has type %s, value has type %s",
                          type_str(elem_type, et, sizeof(et)),
                          type_str(vt, vt_str, sizeof(vt_str)));
                diag_type_hint(vt, elem_type, s->idx_assign_value->loc);
            }

            if (s->idx_assign_op != ASSIGN_EQ && elem_type && !is_numeric(elem_type)) {
                char ts[64];
                diag_emit(DIAG_ERROR,
                          s->loc,
                          "compound assignment requires numeric type, got %s",
                          type_str(elem_type, ts, sizeof(ts)));
            }
            s->type = elem_type;
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

    int variadic_index = -1;
    for (int i = 0; i < func_decl->param_count; i++) {
        if (func_decl->params[i].is_variadic) {
            if (variadic_index != -1) {
                diag_emit(DIAG_ERROR, func_decl->loc, "only the last parameter can be variadic");
                return false;
            }
            variadic_index = i;
        }
    }

    if (variadic_index != -1) {
        if (variadic_index != func_decl->param_count - 1) {
            diag_emit(
                DIAG_ERROR, func_decl->params[variadic_index].loc, "variadic parameter must be the last parameter");
            return false;
        }

        if (func_decl->params[variadic_index].name != NULL) {
            Type* var_type = func_decl->params[variadic_index].type;
            if (!var_type || var_type->kind != TYPE_ARRAY) {
                char ts[64];
                diag_emit(DIAG_ERROR,
                          func_decl->params[variadic_index].loc,
                          "variadic parameter must have array type, got %s",
                          type_str(var_type, ts, sizeof(ts)));
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

int semantic_check(Module* m, const CompileConfig* config) {
    int    prev   = diag_error_count;
    Scope* global = scope_new(NULL);

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_FUNC_DECL || d->kind == AST_EXTERN_DECL) {
            validate_variadic_params(d);
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
        if (d->kind == AST_VAR_DECL && d->init) {
            Type* it = check_expr_hint(d->init, global, d->var_type);
            if (d->var_type && it && !types_equal(it, d->var_type) && !types_assignable(it, d->var_type)) {
                char is[64], vs[64];
                diag_emit(DIAG_ERROR,
                          d->init->loc,
                          "initializer type mismatch: '%s' has type %s, initializer has type %s",
                          d->var_name,
                          type_str(d->var_type, vs, sizeof(vs)),
                          type_str(it, is, sizeof(is)));
                diag_type_hint(it, d->var_type, d->init->loc);
            }
        }
        if (d->kind == AST_CONST_DECL && d->init) {
            if (d->var_type) {
                Type* it = check_expr_hint(d->init, global, d->var_type);
                if (it && !types_equal(it, d->var_type) && !types_assignable(it, d->var_type)) {
                    char is[64], vs[64];
                    diag_emit(DIAG_ERROR,
                              d->init->loc,
                              "initializer type mismatch: '%s' has type %s, initializer has type %s",
                              d->var_name,
                              type_str(d->var_type, vs, sizeof(vs)),
                              type_str(it, is, sizeof(is)));
                    diag_type_hint(it, d->var_type, d->init->loc);
                }
            } else {
                check_expr(d->init, global);
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
            diag_emit(DIAG_NOTE,
                      loc,
                      "Add a main function like:\n"
                      "\n"
                      "    main :: () -> i32 {\n"
                      "        return 0;\n"
                      "    }\n"
                      "\n"
                      "main can have 0 to 2 arguments (for argc/argv), "
                      "and return void or any integer type\n"
                      "If you dont want a main function use flag '--no-main' to compile as a library");
            diag_error_count++;
            return -1;
        }

        if (!config->no_rei_main && !validate_rei_main_signature(main_func)) {
            return -1;
        }
    }

    return diag_error_count > prev ? -1 : 0;
}
