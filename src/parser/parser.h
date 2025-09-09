#ifndef __PARSER_PARSER_H__
#define __PARSER_PARSER_H__

#include "common.h"
#include "compiler.h"
#include "meta_obj.h"

typedef enum {
    TOKEN_UNKNOWN,

    // LITERAL
    TOKEN_U8, TOKEN_U32,
    TOKEN_I32, TOKEN_F64,
    TOKEN_TRUE, TOKEN_FALSE,
    TOKEN_STRING, TOKEN_INTERPOLATION,
    TOKEN_ID,

    // KEYWORD
    TOKEN_LET,
    TOKEN_FN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKEN_NULL,
    TOKEN_IN,
    TOKEN_NATIVE,

    // MODULE
    TOKEN_CLASS,
    TOKEN_SELF,
    TOKEN_STATIC,
    TOKEN_IS,
    TOKEN_SUPER,
    TOKEN_IMPORT,

    // SEPARATOR
    TOKEN_COMMA,
    TOKEN_COLON, TOKEN_SEMICOLON,
    TOKEN_LP, TOKEN_RP, // ()
    TOKEN_LB, TOKEN_RB, // []
    TOKEN_LC, TOKEN_RC, // {}
    TOKEN_DOT, TOKEN_DOTDOT,
    
    // BINARY OPERATOR
    TOKEN_ADD, TOKEN_SUB,
    TOKEN_MUL, TOKEN_DIV, TOKEN_MOD,

    // ASSIGN
    TOKEN_ASSIGN,

    // BIT
    TOKEN_BIT_AND, TOKEN_BIT_OR, TOKEN_BIT_NOT,
    TOKEN_BIT_SR, TOKEN_BIT_SL, // >> <<

    // LOGICAL OPERATOR
    TOKEN_LOGICAL_AND, TOKEN_LOGICAL_OR, TOKEN_LOGICAL_NOT,

    // RELATIONSHIP OPERAND
    TOKEN_EQ, TOKEN_NE,
    TOKEN_LT, TOKEN_LE,
    TOKEN_GT, TOKEN_GE,

    // OTHER MARK
    TOKEN_QUESTION,

    // UN-PRINTABLE MARK
    TOKEN_EOF,
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    u32 len;
    u32 line;
    Value value;
} Token;

struct _Parser {
    const char* file;
    const char* src;
    
    const char* next_char;
    char cur_char;
    Token cur_token;
    Token pre_token;
    ObjModule* cur_module;
    CompileUnit* cur_compile_unit;

    int interpolation_rp_trace;
    
    Parser* parent;
    VM* vm;
};

#define PEEK_TOKEN(parser) parser->cur_token.type

char look_ahead_char(Parser* parser);
void get_next_token(Parser* parser);
bool match_token(Parser* parser, TokenType expected);
void consume_cur_token(Parser* parser, TokenType expected, const char* msg);
void consume_next_token(Parser* parser, TokenType expected, const char* msg);
void init_parser(VM* vm, Parser* parser, const char* file, const char* src, ObjModule* module);

#endif