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
    int         has_putback;
    Token       putback_tok;
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

const char* lexer_filename(Lexer* l) {
    return l ? l->filename : NULL;
}

void lexer_put_back(Lexer* l, Token t) {
    l->has_putback = 1;
    l->putback_tok = t;
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
            advance(l);
            advance(l);
            depth++;
        } else if (l->cur[0] == '*' && l->cur[1] == '/') {
            advance(l);
            advance(l);
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
            advance(l);
            advance(l);
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
    int         base     = 10;

    if (*l->cur == '0' && (*(l->cur + 1) == 'x' || *(l->cur + 1) == 'X')) {
        base = 16;
        advance(l);
        advance(l);
        while (isxdigit((unsigned char)*l->cur))
            advance(l);
    } else if (*l->cur == '0' && (*(l->cur + 1) == 'b' || *(l->cur + 1) == 'B')) {
        base = 2;
        advance(l);
        advance(l);
        while (*l->cur == '0' || *l->cur == '1')
            advance(l);
    } else {
        while (isdigit((unsigned char)*l->cur))
            advance(l);
        if (*l->cur == '.' && isdigit((unsigned char)*(l->cur + 1))) {
            is_float = true;
            advance(l);
            while (isdigit((unsigned char)*l->cur))
                advance(l);
        }
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
        t.ival = strtoll(start, NULL, base);
    }
    return t;
}

static int encode_utf8(uint32_t cp, char* buf) {
    if (cp <= 0x7F) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

#define STR_PUSH(byte)               \
    do {                             \
        if (used + 1 >= cap) {       \
            cap *= 2;                \
            buf = realloc(buf, cap); \
        }                            \
        buf[used++] = (char)(byte);  \
    } while (0)

#define STR_PUSH_UTF8(cp)                                                                   \
    do {                                                                                    \
        char _u[4];                                                                         \
        int  _n = encode_utf8((uint32_t)(cp), _u);                                          \
        if (_n == 0) {                                                                      \
            diag_emit(DIAG_ERROR, loc, "invalid Unicode codepoint U+%04X", (unsigned)(cp)); \
            _n    = 1;                                                                      \
            _u[0] = '?';                                                                    \
        }                                                                                   \
        for (int _i = 0; _i < _n; _i++)                                                     \
            STR_PUSH(_u[_i]);                                                               \
    } while (0)

static Token lex_string(Lexer* l, int flags, Location loc) {
    const char* raw_start = l->cur;
    advance(l);

    bool is_raw       = (flags & STR_PREFIX_R) != 0;
    bool is_multiline = (flags & STR_PREFIX_M) != 0;

    size_t cap = 64, used = 0;
    char*  buf = malloc(cap);

    if (is_multiline && *l->cur == '\n') {
        advance(l);
    } else if (is_multiline && l->cur[0] == '\r' && l->cur[1] == '\n') {
        advance(l);
        advance(l);
    }

    while (*l->cur && *l->cur != '"') {
        if (!is_multiline && (*l->cur == '\n' || *l->cur == '\r')) {
            diag_emit(DIAG_ERROR, loc, "unterminated string literal (use m\"...\" for multiline)");
            break;
        }

        if (is_raw) {
            STR_PUSH(advance(l));
            continue;
        }

        char c = advance(l);
        if (c != '\\') {
            STR_PUSH(c);
            continue;
        }

        if (!*l->cur) {
            diag_emit(DIAG_ERROR, loc, "unterminated escape sequence");
            break;
        }
        char esc = advance(l);
        switch (esc) {
            case 'n':
                STR_PUSH('\n');
                break;
            case 'r':
                STR_PUSH('\r');
                break;
            case 't':
                STR_PUSH('\t');
                break;
            case '\\':
                STR_PUSH('\\');
                break;
            case '"':
                STR_PUSH('"');
                break;
            case '\'':
                STR_PUSH('\'');
                break;
            case '0':
                STR_PUSH('\0');
                break;
            case 'a':
                STR_PUSH('\a');
                break;
            case 'b':
                STR_PUSH('\b');
                break;
            case 'f':
                STR_PUSH('\f');
                break;
            case 'v':
                STR_PUSH('\v');
                break;
            case 'e':
                STR_PUSH(0x1B);
                break; /* ESC */

            case 'x': {
                int hi = hex_digit(*l->cur);
                if (hi < 0) {
                    diag_emit(DIAG_ERROR, loc, "invalid \\x escape: expected hex digit");
                    break;
                }
                advance(l);
                int lo = hex_digit(*l->cur);
                if (lo < 0) {
                    diag_emit(DIAG_ERROR, loc, "invalid \\x escape: expected two hex digits");
                    STR_PUSH((char)(hi << 4));
                    break;
                }
                advance(l);
                STR_PUSH((char)((hi << 4) | lo));
                break;
            }

            case 'u': {
                uint32_t cp = 0;
                if (*l->cur == '{') {
                    advance(l);
                    int digits = 0;
                    while (*l->cur && *l->cur != '}') {
                        int d = hex_digit(*l->cur);
                        if (d < 0) {
                            diag_emit(DIAG_ERROR, loc, "invalid character in \\u{...} escape");
                            break;
                        }
                        cp = (cp << 4) | (uint32_t)d;
                        advance(l);
                        digits++;
                    }
                    if (*l->cur == '}')
                        advance(l);
                    else
                        diag_emit(DIAG_ERROR, loc, "unclosed \\u{...} escape");
                    if (digits == 0)
                        diag_emit(DIAG_ERROR, loc, "empty \\u{} escape");
                } else {
                    for (int i = 0; i < 4; i++) {
                        int d = hex_digit(*l->cur);
                        if (d < 0) {
                            diag_emit(DIAG_ERROR, loc, "\\uHHHH escape requires exactly 4 hex digits");
                            break;
                        }
                        cp = (cp << 4) | (uint32_t)d;
                        advance(l);
                    }
                }
                STR_PUSH_UTF8(cp);
                break;
            }

            /* \U{HHHHHHHH} or \UHHHHHHHH - uhhhhh */
            case 'U': {
                uint32_t cp = 0;
                if (*l->cur == '{') {
                    advance(l);
                    int digits = 0;
                    while (*l->cur && *l->cur != '}') {
                        int d = hex_digit(*l->cur);
                        if (d < 0) {
                            diag_emit(DIAG_ERROR, loc, "invalid character in \\U{...} escape");
                            break;
                        }
                        cp = (cp << 4) | (uint32_t)d;
                        advance(l);
                        digits++;
                    }
                    if (*l->cur == '}')
                        advance(l);
                    else
                        diag_emit(DIAG_ERROR, loc, "unclosed \\U{...} escape");
                } else {
                    for (int i = 0; i < 8; i++) {
                        int d = hex_digit(*l->cur);
                        if (d < 0) {
                            diag_emit(DIAG_ERROR, loc, "\\UHHHHHHHH escape requires exactly 8 hex digits");
                            break;
                        }
                        cp = (cp << 4) | (uint32_t)d;
                        advance(l);
                    }
                }
                STR_PUSH_UTF8(cp);
                break;
            }

            default:
                diag_emit(DIAG_ERROR, loc, "unknown escape sequence '\\%c'", esc);
                STR_PUSH(esc);
                break;
        }
    }

    if (*l->cur == '"')
        advance(l);
    else
        diag_emit(DIAG_ERROR, loc, "unterminated string literal");

    STR_PUSH('\0');

    Token t     = {0};
    t.kind      = TK_STRING_LIT;
    t.loc       = loc;
    t.start     = raw_start;
    t.len       = (size_t)(l->cur - raw_start);
    t.str       = buf;
    t.str_len   = used - 1;
    t.str_flags = flags;
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
        case '.':
            if (*l->cur == '.') {
                advance(l);
                if (*l->cur == '<') {
                    advance(l);
                    return make_tok(l, TK_DOTDOTLESS, loc, start);
                } else if (*l->cur == '.') {
                    advance(l);
                    return make_tok(l, TK_DOTDOTDOT, loc, start);
                }
                return make_tok(l, TK_DOTDOT, loc, start);
            }
            return make_tok(l, TK_DOT, loc, start);
        case '?':
            return make_tok(l, TK_QUESTION, loc, start);
        case ':':
            if (*l->cur == ':') {
                advance(l);
                return make_tok(l, TK_DCOLON, loc, start);
            } else if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_COLONEQ, loc, start);
            }
            return make_tok(l, TK_COLON, loc, start);
        case '+':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_PLUSEQ, loc, start);
            }
            if (*l->cur == '+') {
                advance(l);
                return make_tok(l, TK_PLUSPLUS, loc, start);
            }
            return make_tok(l, TK_PLUS, loc, start);
        case '-':
            if (*l->cur == '>') {
                advance(l);
                return make_tok(l, TK_ARROW, loc, start);
            }
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_MINUSEQ, loc, start);
            }
            if (*l->cur == '-') {
                advance(l);
                return make_tok(l, TK_MINUSMINUS, loc, start);
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
            return make_tok(l, TK_BANG, loc, start);
        case '<':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_LESSEQ, loc, start);
            }
            if (*l->cur == '<') {
                advance(l);
                if (*l->cur == '=') {
                    advance(l);
                    return make_tok(l, TK_SHLEQ, loc, start);
                }
                return make_tok(l, TK_SHL, loc, start);
            }
            return make_tok(l, TK_LESS, loc, start);
        case '>':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_MOREEQ, loc, start);
            }
            if (*l->cur == '>') {
                advance(l);
                if (*l->cur == '=') {
                    advance(l);
                    return make_tok(l, TK_SHREQ, loc, start);
                }
                return make_tok(l, TK_SHR, loc, start);
            }
            return make_tok(l, TK_MORE, loc, start);
        case '*':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_STAREQ, loc, start);
            }
            if (*l->cur == '*') {
                advance(l);
                if (*l->cur == '=') {
                    advance(l);
                    return make_tok(l, TK_STARSTAREQ, loc, start);
                }
                return make_tok(l, TK_STARSTAR, loc, start);
            }
            return make_tok(l, TK_STAR, loc, start);
        case '/':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_SLASHEQ, loc, start);
            }
            return make_tok(l, TK_SLASH, loc, start);
        case '%':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_PERCENTEQ, loc, start);
            }
            return make_tok(l, TK_PERCENT, loc, start);
        case '&':
            if (*l->cur == '&') {
                advance(l);
                if (*l->cur == '=') {
                    advance(l);
                    return make_tok(l, TK_LANDEQ, loc, start);
                }
                return make_tok(l, TK_LAND, loc, start);
            }
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_BITANDEQ, loc, start);
            }
            return make_tok(l, TK_BITAND, loc, start);
        case '|':
            if (*l->cur == '|') {
                advance(l);
                if (*l->cur == '=') {
                    advance(l);
                    return make_tok(l, TK_LOREQ, loc, start);
                }
                return make_tok(l, TK_LOR, loc, start);
            }
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_BITOREQ, loc, start);
            }
            return make_tok(l, TK_BITOR, loc, start);
        case '^':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_BITXOREQ, loc, start);
            }
            return make_tok(l, TK_BITXOR, loc, start);
        case '~':
            if (*l->cur == '=') {
                advance(l);
                return make_tok(l, TK_BITNOTEQ, loc, start);
            }
            return make_tok(l, TK_BITNOT, loc, start);
        case '#':
            return make_tok(l, TK_HASH, loc, start);
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

    if (c == '"') {
        l->cur  = start;
        l->col  = loc.col;
        l->line = loc.line;
        return lex_string(l, 0, loc);
    }

    if (is_ident_start(c)) {
        while (is_ident_cont(*l->cur))
            advance(l);
        size_t word_len = (size_t)(l->cur - start);

        if (*l->cur == '"' && word_len >= 1 && word_len <= 3) {
            int  flags      = 0;
            bool valid      = true;
            int  type_count = 0;
            for (size_t i = 0; i < word_len; i++) {
                switch (start[i]) {
                    case 'c':
                        flags |= STR_PREFIX_C;
                        type_count++;
                        break;
                    case 'b':
                        flags |= STR_PREFIX_B;
                        type_count++;
                        break;
                    case 'r':
                        flags |= STR_PREFIX_R;
                        break;
                    case 'm':
                        flags |= STR_PREFIX_M;
                        break;
                    default:
                        valid = false;
                        break;
                }
            }
            if (valid && type_count <= 1 && flags != 0) {
                if ((flags & STR_PREFIX_C) && (flags & STR_PREFIX_B)) {
                    diag_emit(DIAG_ERROR, loc, "string prefix cannot combine 'c' and 'b'");
                    flags &= ~STR_PREFIX_B;
                }
                return lex_string(l, flags, loc);
            }
        }

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
        else if (t.len == 3 && memcmp(start, "for", 3) == 0)
            t.kind = TK_FOR;
        else if (t.len == 2 && memcmp(start, "as", 2) == 0)
            t.kind = TK_AS;
        else if (t.len == 7 && memcmp(start, "nullptr", 7) == 0)
            t.kind = TK_NULLPTR;
        return t;
    }

    diag_emit(DIAG_ERROR, loc, "unexpected character '%c'", c);
    return lex_one(l);
}

Token lexer_next(Lexer* l) {
    if (l->has_putback) {
        l->has_putback = 0;
        return l->putback_tok;
    }
    if (l->has_peek) {
        l->has_peek = 0;
        return l->peek_tok;
    }
    return lex_one(l);
}

Token lexer_peek(Lexer* l) {
    if (l->has_putback) {
        return l->putback_tok;
    }
    if (!l->has_peek) {
        l->peek_tok = lex_one(l);
        l->has_peek = 1;
    }
    return l->peek_tok;
}

const char* lexer_source(Lexer* l) {
    return l ? l->src : NULL;
}

size_t lexer_position(Lexer* l) {
    return l ? (size_t)(l->cur - l->src) : 0;
}

const char* token_kind_str(TokenKind k) {
    switch (k) {
        case TK_EOF:
            return "EOF";
        case TK_INT_LIT:
            return "int-literal";
        case TK_FLOAT_LIT:
            return "float-literal";
        case TK_STRING_LIT:
            return "string-literal";
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
        case TK_FOR:
            return "'for'";
        case TK_AS:
            return "'as'";
        case TK_NULLPTR:
            return "'nullptr'";
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
        case TK_DOT:
            return "'.'";
        case TK_DOTDOT:
            return "'..'";
        case TK_DOTDOTLESS:
            return "'..<'";
        case TK_DOTDOTDOT:
            return "'...'";
        case TK_QUESTION:
            return "'?'";
        case TK_PLUS:
            return "'+'";
        case TK_MINUS:
            return "'-'";
        case TK_EQ:
            return "'='";
        case TK_PLUSEQ:
            return "'+='";
        case TK_MINUSEQ:
            return "'-='";
        case TK_STAREQ:
            return "'*='";
        case TK_SLASHEQ:
            return "'/='";
        case TK_PERCENTEQ:
            return "'%='";
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
        case TK_STARSTAR:
            return "'**'";
        case TK_SLASH:
            return "'/'";
        case TK_PERCENT:
            return "'%'";
        case TK_BITAND:
            return "'&'";
        case TK_BITOR:
            return "'|'";
        case TK_BITXOR:
            return "'^'";
        case TK_BITNOT:
            return "'~'";
        case TK_SHL:
            return "'<<'";
        case TK_SHR:
            return "'>>'";
        case TK_BITANDEQ:
            return "'&='";
        case TK_BITOREQ:
            return "'|='";
        case TK_BITXOREQ:
            return "'^='";
        case TK_BITNOTEQ:
            return "'~='";
        case TK_SHLEQ:
            return "'<<='";
        case TK_SHREQ:
            return "'>>='";
        case TK_STARSTAREQ:
            return "'**='";
        case TK_COLONEQ:
            return "':='";
        case TK_BANG:
            return "'!'";
        case TK_LAND:
            return "'&&'";
        case TK_LOR:
            return "'||'";
        case TK_PLUSPLUS:
            return "'++'";
        case TK_MINUSMINUS:
            return "'--'";
        case TK_LANDEQ:
            return "'&&='";
        case TK_LOREQ:
            return "'||='";
        case TK_HASH:
            return "'#'";
    }
    return "?";
}
