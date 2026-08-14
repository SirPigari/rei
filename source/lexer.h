#pragma once
#include "diagnostics.h"

#include <stddef.h>
#include <stdint.h>

typedef uint8_t StrPrefixFlags;
enum StrPrefixFlags {
    STR_PREFIX_C = 0b0001, /* c"..." => *u8  (C-string, null-terminated) */
    STR_PREFIX_B = 0b0010, /* b"..." => [u8 (raw bytes, no null)         */
    STR_PREFIX_R = 0b0100, /* r"..." => raw  (no escape processing)       */
    STR_PREFIX_M = 0b1000, /* m"..." => multiline (strip leading newline)  */
    /* absence of C and B => default "..." => []u8 (fat pointer / slice)         */
};

#define TK_DOUBLEDICK TK_COLONEQ

typedef enum {
    TK_EOF = 0,

    /* literals */
    TK_INT_LIT,    /* 42, 42u8, 42i64, 42s32 */
    TK_FLOAT_LIT,  /* 3.14, 3.14f32           */
    TK_STRING_LIT, /* "hi", c"hi", b"hi", r"hi", m"hi", rm"hi", ... */

    /* identifiers / keywords */
    TK_IDENT,
    TK_RETURN,
    TK_EXTERN,
    TK_IF,
    TK_ELSE,
    TK_WHILE,
    TK_FOR,
    TK_AS,
    TK_NULLPTR,

    /* punctuation */
    TK_LPAREN,     /* (   */
    TK_RPAREN,     /* )   */
    TK_LBRACE,     /* {   */
    TK_RBRACE,     /* }   */
    TK_COMMA,      /* ,   */
    TK_COLON,      /* :   */
    TK_SEMI,       /* ;   */
    TK_ARROW,      /* ->  */
    TK_DCOLON,     /* ::  */
    TK_LBRACKET,   /* [   */
    TK_RBRACKET,   /* ]   */
    TK_DOT,        /* .   */
    TK_DOTDOT,     /* ..  */
    TK_DOTDOTLESS, /* ..< */
    TK_DOTDOTDOT,  /* ... */
    TK_QUESTION,   /* ?   */
    TK_PLUS,       /* +   */
    TK_MINUS,      /* -   */

    TK_EQ,         /* =   */
    TK_PLUSEQ,     /* +=  */
    TK_MINUSEQ,    /* -=  */
    TK_STAREQ,     /* *=  */
    TK_SLASHEQ,    /* /=  */
    TK_PERCENTEQ,  /* %=  */
    TK_EQEQ,       /* ==  */
    TK_BANGEQ,     /* !=  */
    TK_LESS,       /* <   */
    TK_LESSEQ,     /* <=  */
    TK_MORE,       /* >   */
    TK_MOREEQ,     /* >=  */
    TK_STAR,       /* *   */
    TK_SLASH,      /* /   */
    TK_PERCENT,    /* %   */
    TK_STARSTAR,   /* **  */

    TK_HASH,       /* #   */
    TK_BITAND,     /* &   */
    TK_BITOR,      /* |   */
    TK_BITXOR,     /* ^   */
    TK_BITNOT,     /* ~   */
    TK_SHL,        /* <<  */
    TK_SHR,        /* >>  */
    TK_BITANDEQ,   /* &=  */
    TK_BITOREQ,    /* |=  */
    TK_BITXOREQ,   /* ^=  */
    TK_BITNOTEQ,   /* ~=  */
    TK_SHLEQ,      /* <<= */
    TK_SHREQ,      /* >>= */
    TK_STARSTAREQ, /* **= */
    TK_COLONEQ,    /* :=  */

    TK_BANG,       /* !   */
    TK_LAND,       /* &&  */
    TK_LOR,        /* ||  */
    TK_PLUSPLUS,   /* ++  */
    TK_MINUSMINUS, /* --  */
    TK_LANDEQ,     /* &&= */
    TK_LOREQ,      /* ||= */
} TokenKind;

typedef struct {
    TokenKind   kind;
    Location    loc;
    const char* start;
    size_t      len;
    union {
        long long ival;
        double    fval;
        struct {
            char*          str;     /* heap-allocated, decoded content */
            size_t         str_len; /* byte length, NOT including null terminator */
            StrPrefixFlags str_flags;
        };
    };
    char suffix[16];
} Token;

typedef struct Lexer Lexer;

Lexer*      lexer_new(const char* src, const char* filename);
void        lexer_free(Lexer* l);
Token       lexer_next(Lexer* l);
Token       lexer_peek(Lexer* l);
void        lexer_put_back(Lexer* l, Token t);
const char* lexer_filename(Lexer* l);
const char* lexer_source(Lexer* l);
size_t      lexer_position(Lexer* l);

const char* token_kind_str(TokenKind k);
