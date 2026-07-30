#pragma once
#include "diagnostics.h"

#include <stddef.h>

typedef enum {
    TK_EOF = 0,

    /* literals */
    TK_INT_LIT,   /* 42, 42u8, 42i64, 42s32 */
    TK_FLOAT_LIT, /* 3.14, 3.14f32 */

    /* identifiers / keywords */
    TK_IDENT,
    TK_RETURN,
    TK_EXTERN,
    TK_IF,
    TK_ELSE,
    TK_WHILE,

    /* punctuation */
    TK_LPAREN,   /* (  */
    TK_RPAREN,   /* )  */
    TK_LBRACE,   /* {  */
    TK_RBRACE,   /* }  */
    TK_COMMA,    /* ,  */
    TK_COLON,    /* :  */
    TK_SEMI,     /* ;  */
    TK_ARROW,    /* -> */
    TK_DCOLON,   /* :: */
    TK_LBRACKET, /* [  */
    TK_RBRACKET, /* ]  */
    TK_PLUS,     /* +  */
    TK_MINUS,    /* -  */

    TK_EQ,       /* =  */
    TK_EQEQ,     /* == */
    TK_BANGEQ,   /* != */
    TK_LESS,     /* <  */
    TK_LESSEQ,   /* <= */
    TK_MORE,     /* >  */
    TK_MOREEQ,   /* >= */
    TK_STAR,     /* *  */
    TK_SLASH,    /* /  */
    TK_PERCENT,  /* %  */
} TokenKind;

typedef struct {
    TokenKind   kind;
    Location    loc;
    const char* start;
    size_t      len;
    union {
        long long ival;
        double    fval;
    };
    char suffix[16];
} Token;

typedef struct Lexer Lexer;

Lexer* lexer_new(const char* src, const char* filename);
void   lexer_free(Lexer* l);
Token  lexer_next(Lexer* l);
Token  lexer_peek(Lexer* l);

const char* token_kind_str(TokenKind k);
