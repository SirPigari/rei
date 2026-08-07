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

static bool is_type_start(TokenKind kind) {
    return kind == TK_IDENT || kind == TK_STAR || kind == TK_STARSTAR || kind == TK_LBRACKET;
}

static Type* parse_type(Lexer* l) {
    Token pk = lexer_peek(l);

    if (pk.kind == TK_STAR) {
        lexer_next(l);
        return type_ptr(parse_type(l), false);
    }

    if (pk.kind == TK_STARSTAR) {
        lexer_next(l);
        return type_ptr(type_ptr(parse_type(l), false), false);
    }

    if (pk.kind == TK_LBRACKET) {
        Location loc = pk.loc;
        lexer_next(l);

        if (lexer_peek(l).kind == TK_RBRACKET) {
            lexer_next(l);
            return type_ptr(parse_type(l), true);
        }

        Type* elem = parse_type(l);
        if (lexer_peek(l).kind == TK_SEMI) {
            lexer_next(l);
            Token len_tok = expect(l, TK_INT_LIT);
            if (len_tok.ival <= 0)
                diag_emit(DIAG_ERROR, len_tok.loc, "array length must be a positive integer");
            expect(l, TK_RBRACKET);
            return type_array(elem, (size_t)len_tok.ival);
        }
        expect(l, TK_RBRACKET);
        return type_array(elem, 0);
    }

    Token  t         = expect(l, TK_IDENT);
    char   name[64]  = {0};
    size_t len       = t.len < 63 ? t.len : 63;
    size_t word_size = sizeof(void*) * 8;
    memcpy(name, t.start, len);
    if (strcmp(name, "void") == 0)
        return type_void();
    if (strcmp(name, "usize") == 0)
        return type_number_with_flag(TYPE_INT, word_size, 1, false, true);
    if (strcmp(name, "isize") == 0)
        return type_number_with_flag(TYPE_INT, word_size, 0, false, true);
    if (strcmp(name, "ssize") == 0)
        return type_number_with_flag(TYPE_INT, word_size, 0, false, true);
    if (strcmp(name, "fsize") == 0)
        return type_number_with_flag(TYPE_FLOAT, word_size, 0, false, true);
    char p    = name[0];
    int  bits = len > 1 ? atoi(name + 1) : 0;
    if (p == 'i' || p == 's')
        return type_number(TYPE_INT, bits, 0);
    if (p == 'u')
        return type_number(TYPE_INT, bits, 1);
    if (p == 'f')
        return type_number(TYPE_FLOAT, bits, 0);
    diag_emit(DIAG_ERROR, t.loc, "unknown type '%s'", name);
    return type_void();
}

static AstNode* parse_expr(Lexer* l);

static AstNode* parse_primary(Lexer* l) {
    Token    t   = lexer_peek(l);
    Location loc = t.loc;

    if (t.kind != TK_INT_LIT && t.kind != TK_FLOAT_LIT && t.kind != TK_IDENT && t.kind != TK_STRING_LIT &&
        t.kind != TK_LBRACKET && t.kind != TK_LPAREN && t.kind != TK_MINUS && t.kind != TK_PLUS && t.kind != TK_BANG &&
        t.kind != TK_BITNOT && t.kind != TK_BITAND && t.kind != TK_STAR && t.kind != TK_PLUSPLUS &&
        t.kind != TK_MINUSMINUS) {
        diag_emit(DIAG_ERROR, loc, "expected expression, got %s", token_kind_str(t.kind));
        AstNode* n = ast_node(AST_INT_LIT, loc);
        n->ival    = 0;
        return n;
    }

    t = lexer_next(l);

    if (t.kind == TK_INT_LIT) {
        AstNode* n = ast_node(AST_INT_LIT, loc);
        n->ival    = t.ival;
        if (t.suffix[0]) {
            char p    = t.suffix[0];
            int  bits = atoi(t.suffix + 1);
            n->type   = (p == 'u') ? type_number(TYPE_INT, bits, 1) : type_number(TYPE_INT, bits, 0);
        }
        return n;
    }
    if (t.kind == TK_FLOAT_LIT) {
        AstNode* n = ast_node(AST_FLOAT_LIT, loc);
        n->fval    = t.fval;
        if (t.suffix[0] == 'f')
            n->type = type_number(TYPE_FLOAT, atoi(t.suffix + 1), 0);
        return n;
    }
    if (t.kind == TK_IDENT) {
        AstNode* n = ast_node(AST_IDENT, loc);
        n->ident   = tok_str(t);
        return n;
    }
    if (t.kind == TK_STRING_LIT) {
        AstNode* n   = ast_node(AST_STRING_LIT, loc);
        n->str       = t.str;
        n->len       = t.str_len;
        n->str_flags = t.str_flags;
        return n;
    }
    if (t.kind == TK_LBRACKET) {
        AstNode* n       = ast_node(AST_ARRAY_LIT, loc);
        size_t   cap     = 8;
        n->element_count = 0;
        n->elements      = malloc(cap * sizeof(AstNode*));
        while (lexer_peek(l).kind != TK_RBRACKET && lexer_peek(l).kind != TK_EOF) {
            if (n->element_count >= cap) {
                cap *= 2;
                n->elements = realloc(n->elements, cap * sizeof(AstNode*));
            }
            n->elements[n->element_count++] = parse_expr(l);
            if (lexer_peek(l).kind == TK_COMMA)
                lexer_next(l);
        }
        expect(l, TK_RBRACKET);
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

static AstNode* parse_postfix(Lexer* l) {
    AstNode* expr = parse_primary(l);

    for (;;) {
        Token pk = lexer_peek(l);

        if (pk.kind == TK_LBRACKET) {
            lexer_next(l);
            AstNode* n = ast_node(AST_INDEX, pk.loc);
            n->array   = expr;
            n->index   = parse_expr(l);
            expect(l, TK_RBRACKET);
            expr = n;
        } else if (pk.kind == TK_LPAREN) {
            lexer_next(l);
            AstNode* n = ast_node(AST_CALL, pk.loc);
            if (expr->kind == AST_IDENT) {
                n->callee = expr->ident;
            } else {
                diag_emit(DIAG_ERROR, pk.loc, "only identifiers can be called directly");
                n->callee = "";
            }
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
            expr = n;
        } else if (pk.kind == TK_DOT) {
            lexer_next(l);
            Token    member_tok = expect(l, TK_IDENT);
            AstNode* n          = ast_node(AST_MEMBER, pk.loc);
            n->member_value     = expr;
            n->member_name      = tok_str(member_tok);
            expr                = n;
        } else if (pk.kind == TK_PLUSPLUS) {
            lexer_next(l);
            AstNode* n = ast_node(AST_UNOP, pk.loc);
            n->uop     = UOP_POSTINC;
            n->operand = expr;
            expr       = n;
        } else if (pk.kind == TK_MINUSMINUS) {
            lexer_next(l);
            AstNode* n = ast_node(AST_UNOP, pk.loc);
            n->uop     = UOP_POSTDEC;
            n->operand = expr;
            expr       = n;
        } else if (pk.kind == TK_AS) {
            lexer_next(l);
            AstNode* n   = ast_node(AST_CAST, pk.loc);
            n->cast_expr = expr;

            if (lexer_peek(l).kind == TK_QUESTION) {
                lexer_next(l);
                n->cast_type = NULL;
            } else if (is_type_start(lexer_peek(l).kind)) {
                n->cast_type = parse_type(l);
            } else {
                diag_emit(DIAG_ERROR, pk.loc, "expected type or '?' after 'as'");
                n->cast_type = type_void();
            }
            expr = n;
        } else {
            break;
        }
    }

    return expr;
}

static AstNode* parse_unary(Lexer* l) {
    Token pk = lexer_peek(l);
    if (pk.kind == TK_MINUS || pk.kind == TK_PLUS || pk.kind == TK_BANG || pk.kind == TK_BITNOT ||
        pk.kind == TK_BITAND || pk.kind == TK_STAR || pk.kind == TK_PLUSPLUS || pk.kind == TK_MINUSMINUS) {
        Token    op = lexer_next(l);
        AstNode* n  = ast_node(AST_UNOP, op.loc);

        switch (op.kind) {
            case TK_MINUS:
                n->uop = UOP_NEG;
                break;
            case TK_PLUS:
                n->uop = UOP_POS;
                break;
            case TK_BANG:
                n->uop = UOP_NOT;
                break;
            case TK_BITNOT:
                n->uop = UOP_BITNOT;
                break;
            case TK_BITAND:
                n->uop = UOP_ADDR;
                break;
            case TK_STAR:
                n->uop = UOP_DEREF;
                break;
            case TK_PLUSPLUS:
                n->uop = UOP_PREINC;
                break;
            case TK_MINUSMINUS:
                n->uop = UOP_PREDEC;
                break;
            default:
                break;
        }

        n->operand = parse_unary(l);
        return n;
    }
    return parse_postfix(l);
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
 *   power          : **
 *   multiplicative : * / %
 *   additive       : + -
 *   shift          : << >>
 *   relational     : < <= > >=
 *   equality       : == !=
 *   bitwise AND    : &
 *   bitwise XOR    : ^
 *   bitwise OR     : |
 *   logical AND    : &&
 *   logical OR     : ||
 */
PARSE_BINOP(parse_power, parse_unary, case TK_STARSTAR : n_op = OP_POW; break;)

PARSE_BINOP(parse_multiplicative, parse_power, case TK_STAR : n_op = OP_MUL; break; case TK_SLASH : n_op = OP_DIV;
            break;
            case TK_PERCENT : n_op = OP_MOD;
            break;)

PARSE_BINOP(parse_additive, parse_multiplicative, case TK_PLUS : n_op = OP_ADD; break; case TK_MINUS : n_op = OP_SUB;
            break;)

PARSE_BINOP(parse_shift, parse_additive, case TK_SHL : n_op = OP_SHL; break; case TK_SHR : n_op = OP_SHR; break;)

PARSE_BINOP(parse_relational, parse_shift, case TK_LESS : n_op = OP_LESS; break; case TK_LESSEQ : n_op = OP_LESSEQ;
            break;
            case TK_MORE : n_op = OP_MORE;
            break;
            case TK_MOREEQ : n_op = OP_MOREEQ;
            break;)

PARSE_BINOP(parse_equality, parse_relational, case TK_EQEQ : n_op = OP_EQ; break; case TK_BANGEQ : n_op = OP_NEQ;
            break;)

PARSE_BINOP(parse_bitwise_and, parse_equality, case TK_BITAND : n_op = OP_BITAND; break;)
PARSE_BINOP(parse_bitwise_xor, parse_bitwise_and, case TK_BITXOR : n_op = OP_BITXOR; break;)
PARSE_BINOP(parse_bitwise_or, parse_bitwise_xor, case TK_BITOR : n_op = OP_BITOR; break;)
PARSE_BINOP(parse_logical_and, parse_bitwise_or, case TK_LAND : n_op = OP_LAND; break;)
PARSE_BINOP(parse_logical_or, parse_logical_and, case TK_LOR : n_op = OP_LOR; break;)

static AstNode* parse_expr(Lexer* l) {
    return parse_logical_or(l);
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
            n->if_cond      = parse_expr(l);
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
            n->while_cond      = parse_expr(l);
            n->while_body      = parse_stmt(l);
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

    if (pk.kind == TK_IDENT) {
        Token name_tok = lexer_next(l);
        Token next     = lexer_peek(l);

        if (next.kind == TK_COLON) {
            lexer_next(l);
            AstNode* n  = ast_node(AST_VAR_DECL, name_tok.loc);
            n->var_name = tok_str(name_tok);
            n->var_type = parse_type(l);
            if (lexer_peek(l).kind == TK_EQ) {
                lexer_next(l);
                n->init = parse_expr(l);
            }
            expect(l, TK_SEMI);
            return n;
        } else if (next.kind == TK_DCOLON) {
            lexer_next(l);
            AstNode* n  = ast_node(AST_CONST_DECL, name_tok.loc);
            n->var_name = tok_str(name_tok);
            n->var_type = NULL;
            n->init     = parse_expr(l);
            expect(l, TK_SEMI);
            return n;
        } else if (next.kind == TK_EQ || next.kind == TK_PLUSEQ || next.kind == TK_MINUSEQ || next.kind == TK_STAREQ ||
                   next.kind == TK_SLASHEQ || next.kind == TK_PERCENTEQ || next.kind == TK_BITANDEQ ||
                   next.kind == TK_BITOREQ || next.kind == TK_BITXOREQ || next.kind == TK_SHLEQ ||
                   next.kind == TK_SHREQ || next.kind == TK_STARSTAREQ || next.kind == TK_LANDEQ ||
                   next.kind == TK_LOREQ) {
            Token    op_tok = lexer_next(l);
            AstNode* n      = ast_node(AST_VAR_ASSIGN, pk.loc);
            n->assign_name  = tok_str(name_tok);

            switch (op_tok.kind) {
                case TK_EQ:
                    n->assign_op = ASSIGN_EQ;
                    break;
                case TK_PLUSEQ:
                    n->assign_op = ASSIGN_ADDEQ;
                    break;
                case TK_MINUSEQ:
                    n->assign_op = ASSIGN_SUBEQ;
                    break;
                case TK_STAREQ:
                    n->assign_op = ASSIGN_MULEQ;
                    break;
                case TK_SLASHEQ:
                    n->assign_op = ASSIGN_DIVEQ;
                    break;
                case TK_PERCENTEQ:
                    n->assign_op = ASSIGN_MODEQ;
                    break;
                case TK_BITANDEQ:
                    n->assign_op = ASSIGN_BITANDEQ;
                    break;
                case TK_BITOREQ:
                    n->assign_op = ASSIGN_BITOREQ;
                    break;
                case TK_BITXOREQ:
                    n->assign_op = ASSIGN_BITXOREQ;
                    break;
                case TK_SHLEQ:
                    n->assign_op = ASSIGN_SHLEQ;
                    break;
                case TK_SHREQ:
                    n->assign_op = ASSIGN_SHREQ;
                    break;
                case TK_STARSTAREQ:
                    n->assign_op = ASSIGN_POWEQ;
                    break;
                case TK_LANDEQ:
                    n->assign_op = ASSIGN_LANDEQ;
                    break;
                case TK_LOREQ:
                    n->assign_op = ASSIGN_LOREQ;
                    break;
                default:
                    break;
            }

            n->assign_value = parse_expr(l);
            expect(l, TK_SEMI);
            return n;
        }

        AstNode* expr  = ast_node(AST_EXPR_STMT, pk.loc);
        AstNode* ident = ast_node(AST_IDENT, pk.loc);
        ident->ident   = tok_str(name_tok);
        expr->expr     = ident;

        Token next_post = lexer_peek(l);
        if (next_post.kind == TK_LBRACKET || next_post.kind == TK_LPAREN) {
            if (next_post.kind == TK_LBRACKET) {
                lexer_next(l);
                AstNode* idx = ast_node(AST_INDEX, next_post.loc);
                idx->array   = ident;
                idx->index   = parse_expr(l);
                expect(l, TK_RBRACKET);

                Token assign_op_tok = lexer_peek(l);
                if (assign_op_tok.kind == TK_EQ || assign_op_tok.kind == TK_PLUSEQ ||
                    assign_op_tok.kind == TK_MINUSEQ || assign_op_tok.kind == TK_STAREQ ||
                    assign_op_tok.kind == TK_SLASHEQ || assign_op_tok.kind == TK_PERCENTEQ ||
                    assign_op_tok.kind == TK_BITANDEQ || assign_op_tok.kind == TK_BITOREQ ||
                    assign_op_tok.kind == TK_BITXOREQ || assign_op_tok.kind == TK_SHLEQ ||
                    assign_op_tok.kind == TK_SHREQ || assign_op_tok.kind == TK_STARSTAREQ ||
                    assign_op_tok.kind == TK_LANDEQ || assign_op_tok.kind == TK_LOREQ) {
                    lexer_next(l);
                    AstNode* assign_node   = ast_node(AST_INDEX_ASSIGN, pk.loc);
                    assign_node->idx_array = idx->array;
                    assign_node->idx_index = idx->index;

                    switch (assign_op_tok.kind) {
                        case TK_EQ:
                            assign_node->idx_assign_op = ASSIGN_EQ;
                            break;
                        case TK_PLUSEQ:
                            assign_node->idx_assign_op = ASSIGN_ADDEQ;
                            break;
                        case TK_MINUSEQ:
                            assign_node->idx_assign_op = ASSIGN_SUBEQ;
                            break;
                        case TK_STAREQ:
                            assign_node->idx_assign_op = ASSIGN_MULEQ;
                            break;
                        case TK_SLASHEQ:
                            assign_node->idx_assign_op = ASSIGN_DIVEQ;
                            break;
                        case TK_PERCENTEQ:
                            assign_node->idx_assign_op = ASSIGN_MODEQ;
                            break;
                        case TK_BITANDEQ:
                            assign_node->idx_assign_op = ASSIGN_BITANDEQ;
                            break;
                        case TK_BITOREQ:
                            assign_node->idx_assign_op = ASSIGN_BITOREQ;
                            break;
                        case TK_BITXOREQ:
                            assign_node->idx_assign_op = ASSIGN_BITXOREQ;
                            break;
                        case TK_SHLEQ:
                            assign_node->idx_assign_op = ASSIGN_SHLEQ;
                            break;
                        case TK_SHREQ:
                            assign_node->idx_assign_op = ASSIGN_SHREQ;
                            break;
                        case TK_STARSTAREQ:
                            assign_node->idx_assign_op = ASSIGN_POWEQ;
                            break;
                        case TK_LANDEQ:
                            assign_node->idx_assign_op = ASSIGN_LANDEQ;
                            break;
                        case TK_LOREQ:
                            assign_node->idx_assign_op = ASSIGN_LOREQ;
                            break;
                        default:
                            break;
                    }

                    assign_node->idx_assign_value = parse_expr(l);
                    expect(l, TK_SEMI);
                    return assign_node;
                } else {
                    expr->expr = idx;
                }
            } else if (next_post.kind == TK_LPAREN) {
                lexer_next(l);
                AstNode* call   = ast_node(AST_CALL, next_post.loc);
                call->callee    = tok_str(name_tok);
                int cap         = 8;
                call->arg_count = 0;
                call->args      = malloc(cap * sizeof(AstNode*));
                while (lexer_peek(l).kind != TK_RPAREN && lexer_peek(l).kind != TK_EOF) {
                    if (call->arg_count >= cap) {
                        cap *= 2;
                        call->args = realloc(call->args, cap * sizeof(AstNode*));
                    }
                    call->args[call->arg_count++] = parse_expr(l);
                    if (lexer_peek(l).kind == TK_COMMA)
                        lexer_next(l);
                }
                expect(l, TK_RPAREN);
                expr->expr = call;
            }
        }

        next_post = lexer_peek(l);
        while (next_post.kind == TK_DOT) {
            lexer_next(l);
            Token    member_tok  = expect(l, TK_IDENT);
            AstNode* member      = ast_node(AST_MEMBER, next_post.loc);
            member->member_value = expr->expr;
            member->member_name  = tok_str(member_tok);
            expr->expr           = member;
            next_post            = lexer_peek(l);
        }

        next_post = lexer_peek(l);
        if (next_post.kind == TK_PLUSPLUS) {
            lexer_next(l);
            AstNode* postinc = ast_node(AST_UNOP, next_post.loc);
            postinc->uop     = UOP_POSTINC;
            postinc->operand = expr->expr;
            expr->expr       = postinc;
        } else if (next_post.kind == TK_MINUSMINUS) {
            lexer_next(l);
            AstNode* postdec = ast_node(AST_UNOP, next_post.loc);
            postdec->uop     = UOP_POSTDEC;
            postdec->operand = expr->expr;
            expr->expr       = postdec;
        }

        expect(l, TK_SEMI);
        return expr;
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

        Token first = lexer_peek(l);

        if (first.kind == TK_DOTDOT) {
            Location loc = first.loc;
            lexer_next(l);

            if (lexer_peek(l).kind == TK_IDENT) {
                Token name_tok = lexer_next(l);
                expect(l, TK_COLON);
                params[*count].name        = tok_str(name_tok);
                params[*count].type        = parse_type(l);
                params[*count].is_variadic = true;
                params[*count].loc         = name_tok.loc;
            } else {
                params[*count].name        = NULL;
                params[*count].type        = NULL;
                params[*count].is_variadic = true;
                params[*count].loc         = loc;
            }
        } else if (first.kind == TK_IDENT) {
            lexer_next(l);
            if (lexer_peek(l).kind == TK_COLON) {
                lexer_next(l);
                params[*count].name        = tok_str(first);
                params[*count].type        = parse_type(l);
                params[*count].is_variadic = false;
            } else {
                lexer_put_back(l, first);
                params[*count].name        = NULL;
                params[*count].type        = parse_type(l);
                params[*count].is_variadic = false;
            }
            params[*count].loc = first.loc;
        } else {
            params[*count].name        = NULL;
            params[*count].type        = parse_type(l);
            params[*count].is_variadic = false;
            params[*count].loc         = first.loc;
        }

        (*count)++;
        if (lexer_peek(l).kind == TK_COMMA)
            lexer_next(l);
    }
    *out = params;
}

static AstNode* parse_func_decl(Lexer* l, Token name_tok) {
    expect(l, TK_DCOLON);
    expect(l, TK_LPAREN);

    AstNode* n       = ast_node(AST_FUNC_DECL, name_tok.loc);
    n->function_name = tok_str(name_tok);

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
    AstNode* n       = ast_node(AST_EXTERN_DECL, loc);
    n->function_name = tok_str(name_tok);
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

#define PUSH_DECL(d)                                        \
    do {                                                    \
        if (count >= cap) {                                 \
            cap *= 2;                                       \
            decls = realloc(decls, cap * sizeof(AstNode*)); \
        }                                                   \
        decls[count++] = (d);                               \
    } while (0)

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
            Token next = lexer_peek(l);

            if (next.kind == TK_DCOLON) {
                lexer_next(l);
                Token after_dcolon_tok = lexer_peek(l);

                if (after_dcolon_tok.kind == TK_LPAREN) {
                    AstNode* decl       = ast_node(AST_FUNC_DECL, name.loc);
                    decl->function_name = tok_str(name);

                    expect(l, TK_LPAREN);
                    parse_params(l, &decl->params, &decl->param_count);
                    expect(l, TK_RPAREN);
                    expect(l, TK_ARROW);
                    decl->ret_type = parse_type(l);
                    decl->body     = parse_stmt(l);

                    PUSH_DECL(decl);
                    continue;
                } else {
                    AstNode* n  = ast_node(AST_CONST_DECL, name.loc);
                    n->var_name = tok_str(name);
                    n->var_type = NULL;
                    n->init     = parse_expr(l);
                    expect(l, TK_SEMI);
                    PUSH_DECL(n);
                    continue;
                }
            } else if (next.kind == TK_COLON) {
                lexer_next(l);
                AstNode* n  = ast_node(AST_VAR_DECL, name.loc);
                n->var_name = tok_str(name);
                n->var_type = parse_type(l);
                if (lexer_peek(l).kind == TK_EQ) {
                    lexer_next(l);
                    n->init = parse_expr(l);
                }
                expect(l, TK_SEMI);
                PUSH_DECL(n);
                continue;
            }
            diag_emit(DIAG_ERROR, name.loc, "unexpected token at top level");
            continue;
        }
        diag_emit(DIAG_ERROR, t.loc, "unexpected token '%.*s' at top level", (int)t.len, t.start);
        lexer_next(l);
    }

    Module* m   = calloc(1, sizeof(*m));
    m->decls    = decls;
    m->count    = count;
    m->filepath = lexer_filename(l);
    return m;
}
