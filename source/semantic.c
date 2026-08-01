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

static char* type_str(Type* t, char* buf, size_t cap) {
    if (!t) {
        snprintf(buf, cap, "?");
        return buf;
    }
    switch (t->kind) {
        case TYPE_VOID:
            snprintf(buf, cap, "void");
            break;
        case TYPE_INT:
            snprintf(buf, cap, "%s%d",
                     t->int_type.is_unsigned ? "u" : "i",
                     t->bits ? t->bits : 64);
            break;
        case TYPE_FLOAT:
            snprintf(buf, cap, "f%d", t->bits ? t->bits : 64);
            break;
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
    return t && (t->kind == TYPE_INT || t->kind == TYPE_FLOAT);
}

static bool is_integer(Type* t) {
    return t && t->kind == TYPE_INT;
}

static bool is_pointer(Type* t) {
    return t && t->kind == TYPE_PTR;
}

static Type _error_type = { .kind = TYPE_VOID };
#define ERROR_TYPE (&_error_type)

static bool is_error_type(Type* t) { return t == ERROR_TYPE; }

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
            if (!e->type) {
                if (hint && hint->kind == TYPE_INT)
                    e->type = hint;
                else
                    e->type = type_number(TYPE_INT, 64, 0);
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
                Type* u8 = type_number(TYPE_INT, 8, true);
                int   flags = e->str_flags;
                if (flags & STR_PREFIX_C)
                    e->type = type_ptr(u8, false);       /* *u8  */
                else if (flags & STR_PREFIX_B)
                    e->type = type_array(u8, 0);         /* [u8] */
                else
                    e->type = type_ptr(u8, true);        /* []u8 */
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
                    diag_emit(DIAG_ERROR, e->loc,
                              "cannot infer type of empty array literal; "
                              "annotate the variable");
                    elem = type_void();
                }
                e->type = type_array(elem, 0);
                return e->type;
            }

            Type* elem_hint = NULL;
            if (hint) {
                if (hint->kind == TYPE_ARRAY) elem_hint = hint->array_type.elem_type;
                else if (hint->kind == TYPE_PTR) elem_hint = hint->ptr_type.elem_type;
            }

            Type* elem_type = check_expr_hint(e->elements[0], sc, elem_hint);
            for (size_t i = 1; i < e->element_count; i++) {
                Type* et = check_expr_hint(e->elements[i], sc, elem_type);
                if (!types_equal(elem_type, et)) {
                    char es[64], fs[64];
                    diag_emit(DIAG_ERROR, e->elements[i]->loc,
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
            if (e->arg_count != fn->param_count)
                diag_emit(DIAG_ERROR,
                          e->loc,
                          "'%s' expects %d arg(s), got %d",
                          fn->function_name,
                          fn->param_count,
                          e->arg_count);
            int check_n = e->arg_count < fn->param_count ? e->arg_count : fn->param_count;
            for (int i = 0; i < e->arg_count; i++) {
                Type* param_hint = i < check_n ? fn->params[i].type : NULL;
                Type* at = check_expr_hint(e->args[i], sc, param_hint);
                if (i < check_n) {
                    Type* pt = fn->params[i].type;
                    if (!types_equal(at, pt)) {
                        char as[64], ps[64];
                        diag_emit(DIAG_ERROR,
                                  e->args[i]->loc,
                                  "argument %d of '%s': expected %s, got %s",
                                  i + 1,
                                  fn->function_name,
                                  type_str(pt, ps, sizeof(ps)),
                                  type_str(at, as, sizeof(as)));
                    }
                }
            }
            e->type = fn->ret_type;
            return e->type;
        }

        case AST_BINOP: {
            Type* lt = check_expr(e->lhs, sc);
            Type* rt = check_expr(e->rhs, sc);

            if (is_pointer(lt) || is_pointer(rt)) {
                char ls[64], rs[64];
                diag_emit(DIAG_ERROR,
                          e->loc,
                          "binary operator '%s' not supported on pointer types (%s, %s); "
                          "dereference first",
                          e->op == OP_ADD  ? "+"  :
                          e->op == OP_SUB  ? "-"  :
                          e->op == OP_MUL  ? "*"  :
                          e->op == OP_DIV  ? "/"  :
                          e->op == OP_MOD  ? "%"  :
                          e->op == OP_POW  ? "**" : "op",
                          type_str(lt, ls, sizeof(ls)),
                          type_str(rt, rs, sizeof(rs)));
                e->type = lt ? lt : rt;
                return e->type;
            }

            if (lt && rt && !types_equal(lt, rt) &&
                    !is_error_type(lt) && !is_error_type(rt)) {
                char ls[64], rs[64];
                diag_emit(DIAG_ERROR,
                          e->loc,
                          "type mismatch: %s and %s",
                          type_str(lt, ls, sizeof(ls)),
                          type_str(rt, rs, sizeof(rs)));
            }

            if (e->op == OP_POW) {
                if ((lt && lt->kind == TYPE_FLOAT) || (rt && rt->kind == TYPE_FLOAT))
                    diag_emit(DIAG_ERROR, e->loc,
                              "power operator '**' does not support float operands");
                if (rt && rt->kind != TYPE_INT)
                    diag_emit(DIAG_ERROR, e->rhs->loc,
                              "power operator exponent must be an integer");
                else if (e->rhs->kind == AST_INT_LIT && e->rhs->ival < 0)
                    diag_emit(DIAG_ERROR, e->rhs->loc,
                              "power operator exponent must be non-negative");
            }

            bool is_bitwise = (e->op == OP_BITAND || e->op == OP_BITOR ||
                                e->op == OP_BITXOR || e->op == OP_SHL  || e->op == OP_SHR);
            if (is_bitwise) {
                if (!is_integer(lt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR, e->lhs->loc,
                              "bitwise operator requires integer operand, got %s",
                              type_str(lt, s, sizeof(s)));
                }
                if (!is_integer(rt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR, e->rhs->loc,
                              "bitwise operator requires integer operand, got %s",
                              type_str(rt, s, sizeof(s)));
                }
            }

            bool is_logical = (e->op == OP_LAND || e->op == OP_LOR);
            if (is_logical) {
                if (!is_numeric(lt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR, e->lhs->loc,
                              "logical operator requires numeric operand, got %s",
                              type_str(lt, s, sizeof(s)));
                }
                if (!is_numeric(rt)) {
                    char s[64];
                    diag_emit(DIAG_ERROR, e->rhs->loc,
                              "logical operator requires numeric operand, got %s",
                              type_str(rt, s, sizeof(s)));
                }
            }

            bool is_cmp = (e->op == OP_EQ || e->op == OP_NEQ  ||
                            e->op == OP_LESS || e->op == OP_MORE  ||
                            e->op == OP_LESSEQ || e->op == OP_MOREEQ);
            e->type = is_cmp ? type_number(TYPE_INT, 64, 0) : (lt ? lt : rt);
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
                        diag_emit(DIAG_ERROR, e->loc,
                                  "cannot dereference non-pointer type %s",
                                  type_str(t, s, sizeof(s)));
                        e->type = ERROR_TYPE;
                    } else if (t->ptr_type.is_fat) {
                        diag_emit(DIAG_ERROR, e->loc,
                                  "cannot dereference fat pointer (slice) directly; "
                                  "index it instead");
                        e->type = t->ptr_type.elem_type;
                    } else {
                        e->type = t->ptr_type.elem_type;
                    }
                    break;

                case UOP_ADDR:
                    e->type = type_ptr(t, false);
                    break;

                case UOP_NOT:
                    if (!is_numeric(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR, e->loc,
                                  "logical not requires numeric operand, got %s",
                                  type_str(t, s, sizeof(s)));
                    }
                    e->type = type_number(TYPE_INT, 64, 0);
                    break;

                case UOP_BITNOT:
                    if (!is_integer(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR, e->loc,
                                  "bitwise not requires integer operand, got %s",
                                  type_str(t, s, sizeof(s)));
                    }
                    e->type = t;
                    break;

                case UOP_NEG:
                case UOP_POS:
                    if (!is_numeric(t)) {
                        char s[64];
                        diag_emit(DIAG_ERROR, e->loc,
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
                        diag_emit(DIAG_ERROR, e->loc,
                                  "increment/decrement requires integer operand, got %s",
                                  type_str(t, s, sizeof(s)));
                    }
                    e->type = t;
                    break;
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
                } else if (ret && !types_equal(vt, ret)) {
                    char vs[64], rs[64];
                    diag_emit(DIAG_ERROR, s->loc,
                              "return type mismatch: expected %s, got %s",
                              type_str(ret, rs, sizeof(rs)),
                              type_str(vt, vs, sizeof(vs)));
                }
            } else if (ret && ret->kind != TYPE_VOID) {
                diag_emit(DIAG_WARN, s->loc,
                          "missing return value in non-void function");
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
                diag_emit(DIAG_ERROR, s->if_cond->loc,
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
                diag_emit(DIAG_ERROR, s->while_cond->loc,
                          "condition cannot be a pointer type (%s); dereference first",
                          type_str(ct, ts, sizeof(ts)));
            }
            check_stmt(s->while_body, sc, fn);
            break;
        }

        case AST_VAR_DECL: {
            s->type = s->var_type;
            Type* scope_type = s->var_type;
            if (s->init) {
                Type* it = check_expr_hint(s->init, sc, s->var_type);
                if (s->var_type && !types_equal(it, s->var_type) && !is_error_type(it)) {
                    char is[64], vs[64];
                    diag_emit(DIAG_ERROR, s->init->loc,
                              "initializer type mismatch: variable '%s' has type %s, "
                              "initializer has type %s",
                              s->var_name,
                              type_str(s->var_type, vs, sizeof(vs)),
                              type_str(it, is, sizeof(is)));
                    scope_type = it;
                }
            }
            SymbolEntry* existing = scope_lookup(sc, s->var_name);
            if (existing && existing->decl != NULL)
                diag_emit(DIAG_ERROR, s->loc, "redefinition of variable '%s'", s->var_name);
            else
                scope_add(sc, s->var_name, s, scope_type);
            break;
        }

        case AST_CONST_DECL: {
            s->type = s->var_type;
            Type* scope_type = s->var_type;
            if (s->init) {
                Type* it = check_expr_hint(s->init, sc, s->var_type);
                if (s->var_type && !types_equal(it, s->var_type)) {
                    char is[64], vs[64];
                    diag_emit(DIAG_ERROR, s->init->loc,
                              "initializer type mismatch: constant '%s' has type %s, "
                              "initializer has type %s",
                              s->var_name,
                              type_str(s->var_type, vs, sizeof(vs)),
                              type_str(it, is, sizeof(is)));
                    scope_type = it;
                }
            } else {
                diag_emit(DIAG_ERROR, s->loc, "const declaration requires an initializer");
            }
            SymbolEntry* existing = scope_lookup(sc, s->var_name);
            if (existing && existing->decl != NULL)
                diag_emit(DIAG_ERROR, s->loc, "redefinition of constant '%s'", s->var_name);
            else
                scope_add(sc, s->var_name, s, scope_type);
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
            if (!types_equal(sym->type, vt)) {
                char ss[64], vs[64];
                diag_emit(DIAG_ERROR, s->loc,
                          "assignment type mismatch: '%s' has type %s, value has type %s",
                          s->assign_name,
                          type_str(sym->type, ss, sizeof(ss)),
                          type_str(vt, vs, sizeof(vs)));
            }
            if (s->assign_op != ASSIGN_EQ && !is_numeric(sym->type)) {
                char ts[64];
                diag_emit(DIAG_ERROR, s->loc,
                          "compound assignment requires numeric type, got %s",
                          type_str(sym->type, ts, sizeof(ts)));
            }
            break;
        }

        default:
            ICE("unexpected node in statement context");
    }
}

int semantic_check(Module* m) {
    int    prev   = diag_error_count;
    Scope* global = scope_new(NULL);

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if (d->kind == AST_FUNC_DECL || d->kind == AST_EXTERN_DECL) {
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
        if (d->kind != AST_FUNC_DECL)
            continue;
        Scope* fn_sc = scope_new(global);
        for (int p = 0; p < d->param_count; p++)
            scope_add(fn_sc, d->params[p].name, NULL, d->params[p].type);
        if (d->body)
            check_stmt(d->body, fn_sc, d);
    }

    for (int i = 0; i < m->count; i++) {
        AstNode* d = m->decls[i];
        if ((d->kind == AST_VAR_DECL || d->kind == AST_CONST_DECL) && d->init) {
            Type* it = check_expr_hint(d->init, global, d->var_type);
            if (d->var_type && !types_equal(it, d->var_type)) {
                char is[64], vs[64];
                diag_emit(DIAG_ERROR, d->init->loc,
                          "initializer type mismatch: '%s' has type %s, initializer has type %s",
                          d->var_name,
                          type_str(d->var_type, vs, sizeof(vs)),
                          type_str(it, is, sizeof(is)));
            }
        }
    }

    return diag_error_count > prev ? -1 : 0;
}
