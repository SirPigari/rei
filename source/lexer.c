#include "lexer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Lexer {
    const char* src;
    const char* cur;
    const char* filename;
    int         line, col;
    int         has_peek;
    Token       peek_tok;
};

Lexer* lexer_new(const char* src, const char* filename) {
    Lexer* l = calloc(1, sizeof(*l));
    l->src = l->cur = src;
    l->filename     = filename;
    l->line         = 1;
    l->col          = 1;
    return l;
}

void lexer_free(Lexer* l) {
    free(l);
}

static Location loc_here(Lexer* l) {
    return (Location){l->filename, l->line, l->col};
}

static char advance(Lexer* l) {
    char c = *l->cur++;
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static void skip_block_comment(Lexer* l, Location loc) {
    int depth = 1;
    while (*l->cur && depth > 0) {
        if (l->cur[0] == '/' && l->cur[1] == '*') {
            advance(l); advance(l);
            depth++;
        } else if (l->cur[0] == '*' && l->cur[1] == '/') {
            advance(l); advance(l);
            depth--;
        } else {
            advance(l);
        }
    }
    if (depth > 0)
        diag_emit(DIAG_ERROR, loc, "unterminated block comment");
}

static void skip_ws(Lexer* l) {
    for (;;) {
        while (*l->cur && isspace((unsigned char)*l->cur))
            advance(l);
        if (l->cur[0] == '/' && l->cur[1] == '/') {
            while (*l->cur && *l->cur != '\n')
                advance(l);
            continue;
        }
        if (l->cur[0] == '/' && l->cur[1] == '*') {
            Location loc = loc_here(l);
            advance(l); advance(l);
            skip_block_comment(l, loc);
            continue;
        }
        break;
    }
}

static int is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}
static int is_ident_cont(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static Token make_tok(Lexer* l, TokenKind k, Location loc, const char* start) {
    Token t = {0};
    t.kind  = k;
    t.loc   = loc;
    t.start = start;
    t.len   = (size_t)(l->cur - start);
    return t;
}

static Token lex_char(Lexer* l) {
    Location    loc   = loc_here(l);
    const char* start = l->cur;

    advance(l);

    uint8_t value = 0;

    if (*l->cur == '\\') {
        advance(l);

        switch (*l->cur) {
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case '\\':
                value = '\\';
                break;
            case '\'':
                value = '\'';
                break;
            default:
                value = (uint8_t)*l->cur;
                break;
        }

        advance(l);
    } else {
        value = (uint8_t)advance(l);
    }

    if (*l->cur != '\'') {
        diag_emit(DIAG_ERROR, loc, "unterminated character literal");
    } else {
        advance(l);
    }

    Token t = {0};
    t.kind  = TK_INT_LIT;
    t.loc   = loc;
    t.start = start;
    t.len   = (size_t)(l->cur - start);
    t.ival  = value;

    return t;
}

static Token lex_number(Lexer* l) {
    Location    loc      = loc_here(l);
    const char* start    = l->cur;
    bool        is_float = false;

    while (isdigit((unsigned char)*l->cur))
        advance(l);
    if (*l->cur == '.') {
        is_float = true;
        advance(l);
        while (isdigit((unsigned char)*l->cur))
            advance(l);
    }

    char suffix[16] = {0};
    int  si         = 0;
    while (is_ident_cont(*l->cur) && si < 15)
        suffix[si++] = advance(l);

    Token t = {0};
    t.loc   = loc;
    t.start = start;
    t.len   = (size_t)(l->cur - start);
    memcpy(t.suffix, suffix, sizeof(suffix));

    if (is_float || (suffix[0] == 'f')) {
        t.kind = TK_FLOAT_LIT;
        char   buf[64];
        size_t numlen = (size_t)(t.len - strlen(suffix));
        snprintf(buf, sizeof(buf), "%.*s", (int)numlen, start);
        t.fval = atof(buf);
    } else {
        t.kind = TK_INT_LIT;
        t.ival = strtoll(start, NULL, 10);
    }
    return t;
}

static Token lex_one(Lexer* l) {
    skip_ws(l);
    if (!*l->cur)
        return make_tok(l, TK_EOF, loc_here(l), l->cur);

    Location    loc   = loc_here(l);
    const char* start = l->cur;
    char        c     = advance(l);

    switch (c) {
        case '(':
            return make_tok(l, TK_LPAREN, loc, start);
        case ')':
            return make_tok(l, TK_RPAREN, loc, start);
        case '{':
            return make_tok(l, TK_LBRACE, loc, start);
        case '}':
            return make_tok(l, TK_RBRACE, loc, start);
        case ',':
            return make_tok(l, TK_COMMA, loc, start);
        case ';':
            return make_tok(l, TK_SEMI, loc, start);
        case '[':
            return make_tok(l, TK_LBRACKET, loc, start);
        case ']':
            return make_tok(l, TK_RBRACKET, loc, start);
        case ':':
            if (*l->cur == ':') {
                advance(l);
                return make_tok(l, TK_DCOLON, loc, start);
            }
            return make_tok(l, TK_COLON, loc, start);
        case '+':
            return make_tok(l, TK_PLUS, loc, start);
        case '-':
            if (*l->cur == '>') {
                advance(l);
                return make_tok(l, TK_ARROW, loc, start);
            }
            return make_tok(l, TK_MINUS, loc, start);
        case '=':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_EQEQ, loc, start);
            }
            return make_tok(l, TK_EQ, loc, start);
        case '!':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_BANGEQ, loc, start);
            }
            diag_emit(DIAG_ERROR, loc, "expected '=' after '!'");
            return lex_one(l);
        case '<':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_LESSEQ, loc, start);
            }
            return make_tok(l, TK_LESS, loc, start);
        case '>':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_MOREEQ, loc, start);
            }
            return make_tok(l, TK_MORE, loc, start);
        case '*':
            return make_tok(l, TK_STAR, loc, start);
        case '/':
            return make_tok(l, TK_SLASH, loc, start);
        case '%':
            return make_tok(l, TK_PERCENT, loc, start);
        default:
            break;
    }

    if (c == '\'') {
        l->cur  = start;
        l->col  = loc.col;
        l->line = loc.line;
        return lex_char(l);
    }

    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)*l->cur))) {
        l->cur  = start;
        l->col  = loc.col;
        l->line = loc.line;
        return lex_number(l);
    }

    if (is_ident_start(c)) {
        while (is_ident_cont(*l->cur))
            advance(l);
        Token t = make_tok(l, TK_IDENT, loc, start);
        if (t.len == 6 && memcmp(start, "return", 6) == 0)
            t.kind = TK_RETURN;
        else if (t.len == 6 && memcmp(start, "extern", 6) == 0)
            t.kind = TK_EXTERN;
        else if (t.len == 2 && memcmp(start, "if", 2) == 0)
            t.kind = TK_IF;
        else if (t.len == 4 && memcmp(start, "else", 4) == 0)
            t.kind = TK_ELSE;
        else if (t.len == 5 && memcmp(start, "while", 5) == 0)
            t.kind = TK_WHILE;
        return t;
    }

    diag_emit(DIAG_ERROR, loc, "unexpected character '%c'", c);
    return lex_one(l);
}

Token lexer_next(Lexer* l) {
    if (l->has_peek) {
        l->has_peek = 0;
        return l->peek_tok;
    }
    return lex_one(l);
}

Token lexer_peek(Lexer* l) {
    if (!l->has_peek) {
        l->peek_tok = lex_one(l);
        l->has_peek = 1;
    }
    return l->peek_tok;
}

const char* token_kind_str(TokenKind k) {
    switch (k) {
        case TK_EOF:
            return "EOF";
        case TK_INT_LIT:
            return "int-literal";
        case TK_FLOAT_LIT:
            return "float-literal";
        case TK_IDENT:
            return "identifier";
        case TK_RETURN:
            return "'return'";
        case TK_EXTERN:
            return "'extern'";
        case TK_IF:
            return "'if'";
        case TK_ELSE:
            return "'else'";
        case TK_WHILE:
            return "'while'";
        case TK_LPAREN:
            return "'('";
        case TK_RPAREN:
            return "')'";
        case TK_LBRACE:
            return "'{'";
        case TK_RBRACE:
            return "'}'";
        case TK_COMMA:
            return "','";
        case TK_COLON:
            return "':'";
        case TK_SEMI:
            return "';'";
        case TK_ARROW:
            return "'->'";
        case TK_DCOLON:
            return "'::'";
        case TK_LBRACKET:
            return "'['";
        case TK_RBRACKET:
            return "']'";
        case TK_PLUS:
            return "'+'";
        case TK_MINUS:
            return "'-'";
        case TK_EQ:
            return "'='";
        case TK_EQEQ:
            return "'=='";
        case TK_BANGEQ:
            return "'!='";
        case TK_LESS:
            return "'<'";
        case TK_LESSEQ:
            return "'<='";
        case TK_MORE:
            return "'>'";
        case TK_MOREEQ:
            return "'>='";
        case TK_STAR:
            return "'*'";
        case TK_SLASH:
            return "'/'";
        case TK_PERCENT:
            return "'%'";
    }
    return "?";
}
