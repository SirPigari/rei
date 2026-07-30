#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Token expect(Lexer* l, TokenKind k) {
    Token t = lexer_next(l);
    if (t.kind != k)
        diag_emit(DIAG_ERROR, t.loc, "expected %s, got %s", token_kind_str(k), token_kind_str(t.kind));
    return t;
}

static char* tok_str(Token t) {
    char* s = malloc(t.len + 1);
    memcpy(s, t.start, t.len);
    s[t.len] = '\0';
    return s;
}

static Type* parse_type(Lexer* l) {
    Token  t        = expect(l, TK_IDENT);
    char   name[64] = {0};
    size_t len      = t.len < 63 ? t.len : 63;
    memcpy(name, t.start, len);
    if (strcmp(name, "void") == 0)
        return type_void();
    char p    = name[0];
    int  bits = len > 1 ? atoi(name + 1) : 0;
    if (p == 'i' || p == 's')
        return type_new(TYPE_INT, bits, 0);
    if (p == 'u')
        return type_new(TYPE_INT, bits, 1);
    if (p == 'f')
        return type_new(TYPE_FLOAT, bits, 0);
    diag_emit(DIAG_ERROR, t.loc, "unknown type '%s'", name);
    return type_void();
}

static AstNode* parse_expr(Lexer* l);

static AstNode* parse_primary(Lexer* l) {
    Token    t   = lexer_next(l);
    Location loc = t.loc;

    if (t.kind == TK_INT_LIT) {
        AstNode* n = ast_node(AST_INT_LIT, loc);
        n->ival    = t.ival;
        if (t.suffix[0]) {
            char p    = t.suffix[0];
            int  bits = atoi(t.suffix + 1);
            n->type   = (p == 'u') ? type_new(TYPE_INT, bits, 1) : type_new(TYPE_INT, bits, 0);
        }
        return n;
    }
    if (t.kind == TK_FLOAT_LIT) {
        AstNode* n = ast_node(AST_FLOAT_LIT, loc);
        n->fval    = t.fval;
        if (t.suffix[0] == 'f')
            n->type = type_new(TYPE_FLOAT, atoi(t.suffix + 1), 0);
        return n;
    }
    if (t.kind == TK_IDENT) {
        if (lexer_peek(l).kind == TK_LPAREN) {
            lexer_next(l);
            AstNode* n   = ast_node(AST_CALL, loc);
            n->callee    = tok_str(t);
            int cap      = 8;
            n->arg_count = 0;
            n->args      = malloc(cap * sizeof(AstNode*));
            while (lexer_peek(l).kind != TK_RPAREN && lexer_peek(l).kind != TK_EOF) {
                if (n->arg_count >= cap) {
                    cap *= 2;
                    n->args = realloc(n->args, cap * sizeof(AstNode*));
                }
                n->args[n->arg_count++] = parse_expr(l);
                if (lexer_peek(l).kind == TK_COMMA)
                    lexer_next(l);
            }
            expect(l, TK_RPAREN);
            return n;
        }
        AstNode* n = ast_node(AST_IDENT, loc);
        n->ident   = tok_str(t);
        return n;
    }
    if (t.kind == TK_LPAREN) {
        AstNode* n = parse_expr(l);
        expect(l, TK_RPAREN);
        return n;
    }

    diag_emit(DIAG_ERROR, loc, "expected expression, got %s", token_kind_str(t.kind));
    AstNode* n = ast_node(AST_INT_LIT, loc);
    n->ival    = 0;
    return n;
}

static AstNode* parse_unary(Lexer* l) {
    Token pk = lexer_peek(l);
    if (pk.kind == TK_MINUS || pk.kind == TK_PLUS) {
        Token    op = lexer_next(l);
        AstNode* n  = ast_node(AST_UNOP, op.loc);
        n->uop      = (op.kind == TK_MINUS) ? UOP_NEG : UOP_POS;
        n->operand  = parse_unary(l);
        return n;
    }
    return parse_primary(l);
}

#define PARSE_BINOP(name, next, ...)                   \
    static AstNode* name(Lexer* l) {                   \
        AstNode* lhs = next(l);                        \
        for (;;) {                                     \
            Token pk = lexer_peek(l);                  \
            BinOp n_op;                                \
            switch (pk.kind) {                         \
                __VA_ARGS__                            \
                default:                               \
                    goto done;                         \
            }                                          \
            Token    op = lexer_next(l);               \
            AstNode* n  = ast_node(AST_BINOP, op.loc); \
            n->op       = n_op;                        \
            n->lhs      = lhs;                         \
            n->rhs      = next(l);                     \
            lhs         = n;                           \
        }                                              \
done:                                                  \
        return lhs;                                    \
    }

/* Precedence (high -> low), same as C:
 *   multiplicative : * / %
 *   additive       : + -
 *   relational     : < <= > >=
 *   equality       : == !=
 */
PARSE_BINOP(parse_multiplicative, parse_unary, case TK_STAR : n_op = OP_MUL; break; case TK_SLASH : n_op = OP_DIV;
            break;
            case TK_PERCENT : n_op = OP_MOD;
            break;)

PARSE_BINOP(parse_additive, parse_multiplicative, case TK_PLUS : n_op = OP_ADD; break; case TK_MINUS : n_op = OP_SUB;
            break;)

PARSE_BINOP(parse_relational, parse_additive, case TK_LESS : n_op = OP_LESS; break; case TK_LESSEQ : n_op = OP_LESSEQ;
            break;
            case TK_MORE : n_op = OP_MORE;
            break;
            case TK_MOREEQ : n_op = OP_MOREEQ;
            break;)

PARSE_BINOP(parse_equality, parse_relational, case TK_EQEQ : n_op = OP_EQ; break; case TK_BANGEQ : n_op = OP_NEQ;
            break;)

static AstNode* parse_expr(Lexer* l) {
    return parse_equality(l);
}

static AstNode* parse_stmt(Lexer* l) {
    Token pk = lexer_peek(l);
    switch (pk.kind) {
        case TK_LBRACE: {
            Token    lb   = lexer_next(l);
            AstNode* n    = ast_node(AST_BLOCK_STMT, lb.loc);
            int      cap  = 16;
            n->stmts      = malloc(cap * sizeof(AstNode*));
            n->stmt_count = 0;
            while (lexer_peek(l).kind != TK_RBRACE && lexer_peek(l).kind != TK_EOF) {
                if (n->stmt_count >= cap) {
                    cap *= 2;
                    n->stmts = realloc(n->stmts, cap * sizeof(AstNode*));
                }
                n->stmts[n->stmt_count++] = parse_stmt(l);
            }
            expect(l, TK_RBRACE);
            return n;
        } break;
        case TK_IF: {
            Token    if_tok = lexer_next(l);
            AstNode* n      = ast_node(AST_IF_STMT, if_tok.loc);
            n->if_cond         = parse_expr(l);
            n->then_branch  = parse_stmt(l);
            if (lexer_peek(l).kind == TK_ELSE) {
                lexer_next(l);
                n->else_branch = parse_stmt(l);
            }
            return n;
        } break;
        case TK_WHILE: {
            Token    while_tok = lexer_next(l);
            AstNode* n         = ast_node(AST_WHILE_STMT, while_tok.loc);
            n->while_cond            = parse_expr(l);
            n->while_body            = parse_stmt(l);
            return n;
        } break;
        case TK_RETURN: {
            Token    rt = lexer_next(l);
            AstNode* n  = ast_node(AST_RETURN_STMT, rt.loc);
            n->ret_val  = (lexer_peek(l).kind != TK_SEMI) ? parse_expr(l) : NULL;
            expect(l, TK_SEMI);
            return n;
        } break;
        default:
            break;
    }
    AstNode* n = ast_node(AST_EXPR_STMT, pk.loc);
    n->expr    = parse_expr(l);
    expect(l, TK_SEMI);
    return n;
}

static void parse_params(Lexer* l, Param** out, int* count) {
    int cap       = 8;
    *count        = 0;
    Param* params = malloc(cap * sizeof(Param));
    while (lexer_peek(l).kind != TK_RPAREN && lexer_peek(l).kind != TK_EOF) {
        if (*count >= cap) {
            cap *= 2;
            params = realloc(params, cap * sizeof(Param));
        }
        Token name_tok = expect(l, TK_IDENT);
        expect(l, TK_COLON);
        params[*count].name = tok_str(name_tok);
        params[*count].type = parse_type(l);
        (*count)++;
        if (lexer_peek(l).kind == TK_COMMA)
            lexer_next(l);
    }
    *out = params;
}

static AstNode* parse_func_decl(Lexer* l, Token name_tok) {
    expect(l, TK_DCOLON);
    expect(l, TK_LPAREN);

    AstNode* n = ast_node(AST_FUNC_DECL, name_tok.loc);
    n->name    = tok_str(name_tok);

    parse_params(l, &n->params, &n->param_count);

    expect(l, TK_RPAREN);
    expect(l, TK_ARROW);

    n->ret_type = parse_type(l);

    AstNode* body = parse_stmt(l);

    n->body = body;

    return n;
}

static AstNode* parse_one_extern(Lexer* l) {
    Location loc      = lexer_peek(l).loc;
    Token    name_tok = expect(l, TK_IDENT);
    expect(l, TK_LPAREN);
    AstNode* n = ast_node(AST_EXTERN_DECL, loc);
    n->name    = tok_str(name_tok);
    parse_params(l, &n->params, &n->param_count);
    expect(l, TK_RPAREN);
    expect(l, TK_ARROW);
    n->ret_type = parse_type(l);
    return n;
}

static void parse_extern(Lexer* l, AstNode*** decls, int* count, int* cap) {
#define PUSH(d)                                                \
    do {                                                       \
        if (*count >= *cap) {                                  \
            *cap *= 2;                                         \
            *decls = realloc(*decls, *cap * sizeof(AstNode*)); \
        }                                                      \
        (*decls)[(*count)++] = (d);                            \
    } while (0)

    if (lexer_peek(l).kind == TK_LBRACE) {
        lexer_next(l);
        while (lexer_peek(l).kind != TK_RBRACE && lexer_peek(l).kind != TK_EOF) {
            PUSH(parse_one_extern(l));
            if (lexer_peek(l).kind == TK_COMMA)
                lexer_next(l);
        }
        expect(l, TK_RBRACE);
    } else {
        PUSH(parse_one_extern(l));
        expect(l, TK_SEMI);
    }
#undef PUSH
}

Module* parse(Lexer* l) {
    int       cap = 32, count = 0;
    AstNode** decls = malloc(cap * sizeof(AstNode*));

    for (;;) {
        Token t = lexer_peek(l);
        if (t.kind == TK_EOF)
            break;
        if (t.kind == TK_EXTERN) {
            lexer_next(l);
            parse_extern(l, &decls, &count, &cap);
            continue;
        }
        if (t.kind == TK_IDENT) {
            Token name = lexer_next(l);
            if (lexer_peek(l).kind == TK_DCOLON) {
                if (count >= cap) {
                    cap *= 2;
                    decls = realloc(decls, cap * sizeof(AstNode*));
                }
                decls[count++] = parse_func_decl(l, name);
                continue;
            }
            diag_emit(DIAG_ERROR, name.loc, "unexpected token at top level");
            continue;
        }
        diag_emit(DIAG_ERROR, t.loc, "unexpected token '%.*s' at top level", (int)t.len, t.start);
        lexer_next(l);
    }

    Module* m = calloc(1, sizeof(*m));
    m->decls  = decls;
    m->count  = count;
    return m;
}