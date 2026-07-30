#include "semantic.h"

#include "../thirdparty/ht.h"
#include "diagnostics.h"

#include <stdlib.h>
#include <string.h>

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

static Type* check_expr(AstNode* e, Scope* sc);
static void  check_stmt(AstNode* s, Scope* sc, AstNode* fn);

static Type* check_expr(AstNode* e, Scope* sc) {
    switch (e->kind) {
        case AST_INT_LIT:
            if (!e->type)
                e->type = type_new(TYPE_INT, 64, 0);
            return e->type;
        case AST_FLOAT_LIT:
            if (!e->type)
                e->type = type_new(TYPE_FLOAT, 64, 0);
            return e->type;
        case AST_IDENT: {
            SymbolEntry* sym = scope_lookup(sc, e->ident);
            if (!sym) {
                diag_emit(DIAG_ERROR, e->loc, "undefined symbol '%s'", e->ident);
                e->type = type_void();
            } else
                e->type = sym->type;
            return e->type;
        }
        case AST_CALL: {
            SymbolEntry* sym = scope_lookup(sc, e->callee);
            if (!sym || !sym->decl) {
                diag_emit(DIAG_ERROR, e->loc, "undefined function '%s'", e->callee);
                e->type = type_void();
            } else {
                AstNode* fn = sym->decl;
                if (e->arg_count != fn->param_count)
                    diag_emit(
                        DIAG_ERROR, e->loc, "'%s' expects %d arg(s), got %d", fn->name, fn->param_count, e->arg_count);
                for (int i = 0; i < e->arg_count; i++)
                    check_expr(e->args[i], sc);
                e->type = fn->ret_type;
            }
            return e->type;
        }
        case AST_BINOP: {
            Type* lt = check_expr(e->lhs, sc);
            Type* rt = check_expr(e->rhs, sc);
            e->type  = lt ? lt : rt;
            (void)rt;
            return e->type;
        }
        case AST_UNOP: {
            Type* t = check_expr(e->operand, sc);
            e->type = t;
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
        case AST_RETURN_STMT:
            if (s->ret_val) {
                check_expr(s->ret_val, sc);
                if (fn && fn->ret_type->kind == TYPE_VOID)
                    diag_emit(DIAG_WARN, s->loc, "returning value from void function");
            }
            break;
        case AST_BLOCK_STMT: {
            Scope* block_sc = scope_new(sc);
            for (int i = 0; i < s->stmt_count; i++)
                check_stmt(s->stmts[i], block_sc, fn);
            break;
        }
        case AST_IF_STMT:
            check_expr(s->if_cond, sc);
            check_stmt(s->then_branch, sc, fn);
            if (s->else_branch)
                check_stmt(s->else_branch, sc, fn);
            break;
        case AST_WHILE_STMT:
            check_expr(s->while_cond, sc);
            check_stmt(s->while_body, sc, fn);
            break;
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
            if (scope_lookup(global, d->name))
                diag_emit(DIAG_ERROR, d->loc, "redefinition of '%s'", d->name);
            else
                scope_add(global, d->name, d, d->ret_type);
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
    return diag_error_count > prev ? -1 : 0;
}
