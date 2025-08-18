#include "parser.h"
#include "class.h"
#include "common.h"
#include "obj_string.h"
#include "utf8.h"
#include "utils.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define ESC_a '\a'
#define ESC_0 '\0'
#define ESC_b '\b'
#define ESC_f '\f'
#define ESC_n '\n'
#define ESC_r '\r'
#define ESC_t '\t'

struct KeywordToken {
    char* keyword;
    u8 len;
    TokenType token;
};

#define SET(keyword, type) {#keyword, (u8)(sizeof(#keyword) - 1), TOKEN_##type}

struct KeywordToken keywords[] = {
    SET(let, LET),
    SET(fn, FN),
    SET(if, IF),
    SET(else, ELSE),
    SET(true, TRUE),
    SET(false, FALSE),
    SET(while, WHILE),
    SET(for, FOR),
    SET(break, BREAK),
    SET(continue, CONTINUE),
    SET(return, RETURN),
    SET(null, NULL),
    SET(class, CLASS),
    SET(is, IS),
    SET(static, STATIC),
    SET(self, SELF),
    SET(super, SUPER),
    SET(import, IMPORT),
    SET(in, IN),
    {NULL, 0, TOKEN_UNKNOWN},
};
#undef SET

static TokenType id_or_keyword(const char* start, u32 len) {
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        if (keywords[i].len != len) {
            continue;
        }

        if (memcmp(keywords[i].keyword, start, len) != 0) {
            continue;
        }

        return keywords[i].token;
    }

    return TOKEN_ID;
}

inline char look_ahead_char(Parser* parser) {
    return *parser->next_char;
}

inline static void get_next_char(Parser* parser) {
    parser->cur_char = *parser->next_char++;
}

static bool match_next_char(Parser* parser, char expected) {
    if (look_ahead_char(parser) == expected) {
        get_next_char(parser);
        return true;
    }
    return false;
}

static void skip_blanks(Parser* parser) {
    while (isspace(parser->cur_char)) {
        if (parser->cur_char == '\n') {
            parser->cur_token.line++;
        }
        get_next_char(parser);
    }
}

/**
 * 解析可能为id的token
 * id => [a-zA-Z_]+[0-9a-zA-Z_]*
 */
static void parse_id(Parser* parser, TokenType type) {
    while (isalnum(parser->cur_char) || parser->cur_char == '_') {
        get_next_char(parser);
    }

    u32 len = (u32)(parser->next_char - parser->cur_token.start - 1);
    parser->cur_token.type = (type != TOKEN_UNKNOWN) ? type : id_or_keyword(parser->cur_token.start, len);
    parser->cur_token.len = len;
}

/**
 * 解析unicode点位，形如"\uFFFF"，可在字符串中嵌入utf8字符
 */
static void parse_unicode_code_point(Parser* parser, BufferType(Byte)* buf) {    
    // 解析码点的十六进制值
    u32 idx = 0;
    Char_utf8 val = 0;
    u8 digit = 0;
    while (idx++ < 4) {
        get_next_char(parser);
        if (parser->cur_char == '\0') {
            LEX_ERROR(parser, "unterminated unicode!");
        }

        if (parser->cur_char >= '0' && parser->cur_char <= '9') {
            digit = parser->cur_char - '0';
        } else if (parser->cur_char >= 'a' && parser->cur_char <= 'f') {
            digit = parser->cur_char - 'a' + 10;
        } else if (parser->cur_char >= 'A' && parser->cur_char <= 'F') {
            digit = parser->cur_char - 'A' + 10;
        } else {
            LEX_ERROR(parser, "invalid unicode!");
        }

        val = val * 16 | digit;
    }

    u32 byte_num = get_byte_of_decode_utf8(val);
    ASSERT(byte_num != 0, "utf8 encode bytes should be 1 <= byte <= 4.");

    BufferFill(Byte, buf, parser->vm, 0, byte_num); // 预先填0，确保buf空间充足
    encode_utf8(buf->datas + buf->count - byte_num, val);
}

static void parse_string(Parser* parser) {
    BufferType(Byte) str;
    BufferInit(Byte, &str);

    while (true) {
        get_next_char(parser);
        if (parser->cur_char == '\0') {
            LEX_ERROR(parser, "unterminated string.");
        }

        if (parser->cur_char == '"') {
            parser->cur_token.type = TOKEN_STRING;
            break;
        }

        // 内嵌表达式
        if (parser->cur_char == '%') {
            if (!match_next_char(parser, '(')) {
                LEX_ERROR(parser, "'\%' should followed by '('.");
            }

            if (parser->interpolation_rp_trace > 0) {
                COMPILE_ERROR(parser, "don't support nest interpolate expression.");
            }

            parser->interpolation_rp_trace = 1;
            parser->cur_token.type = TOKEN_INTERPOLATION;
            break;
        }

        // 转义字符
        if (parser->cur_char == '\\') {
            get_next_char(parser); // skip '\\'

            #define match(val) \
                case #val[0]:\
                    BufferAdd(Byte, &str, parser->vm, ESC_##val);\
                    get_next_char(parser);\
                    break;
            switch (parser->cur_char) {
                match(0)
                match(a)
                match(b)
                match(f)
                match(n)
                match(r)
                match(t)

                case '"':
                    BufferAdd(Byte, &str, parser->vm, '\"');
                    break;
                case '\\':
                    BufferAdd(Byte, &str, parser->vm, '\\');
                    break;
                case 'u':
                    parse_unicode_code_point(parser, &str);
                    break;
                
                default:
                    LEX_ERROR(parser, "unsupport escape \\%c", parser->cur_char);
                    break;
            }
            #undef match
        }

        BufferAdd(Byte, &str, parser->vm, parser->cur_char);
    }

    ObjString* objstr = objstring_new(parser->vm, (const char*)str.datas, str.count);
    parser->cur_token.value = OBJ_TO_VALUE(objstr);

    BufferClear(Byte, &str, parser->vm);
}

// 跳过一行
static void skip_aline(Parser* parser) {
    get_next_char(parser);
    while (parser->cur_char != '\0') {
        if (parser->cur_char == '\n') {
            parser->cur_token.line++;
            get_next_char(parser);
            break;
        }
        get_next_char(parser);
    }
}

static void skip_comment(Parser* parser) {
    // 单行注释
    if (parser->cur_char == '/') {
        skip_aline(parser);
        skip_blanks(parser);
        return;
    }

    char next_char = look_ahead_char(parser);
    while (next_char != '*' && next_char != '\0') {
        get_next_char(parser);
        
        if (parser->cur_char == '\n') {
            parser->cur_token.line++;
        }

        next_char = look_ahead_char(parser);
    }

    if (next_char == '\0') {
        LEX_ERROR(parser, "expect '*/' before EOF.");
    }

    if (!match_next_char(parser, '*') || !match_next_char(parser, '/')) {
        LEX_ERROR(parser, "expect '/' after '*'.");
    }

    get_next_char(parser);
    skip_blanks(parser);
}

inline static void parse_hex_number(Parser* parser) {
    while (isxdigit(parser->cur_char)) {
        get_next_char(parser);
    }
}

inline static bool parse_dec_number(Parser* parser) {
    while (isdigit(parser->cur_char)) {
        get_next_char(parser);
    }

    if (parser->cur_char == '.' && isdigit(look_ahead_char(parser))) {
        get_next_char(parser);
        while (isdigit(parser->cur_char)) {
            get_next_char(parser);
        }
        return true;
    }

    return false;
}

inline static void parse_oct_number(Parser* parser) {
    while (parser->cur_char >= '0' && parser->cur_char <= '8') {
        get_next_char(parser);
    }
}

static void parse_number(Parser* parser) {
    if (parser->cur_char == '0' && match_next_char(parser, 'x')) {
        get_next_char(parser); // x
        parse_hex_number(parser);
        int val = strtol(parser->cur_token.start, NULL, 16);
        parser->cur_token.value = I32_TO_VALUE(val);
    } else if (parser->cur_char == '0' && match_next_char(parser, 'o')) {
        get_next_char(parser); // o
        parse_oct_number(parser);
        int val = strtol(parser->cur_token.start, NULL, 8);
        parser->cur_token.value = I32_TO_VALUE(val);
    } else {
        if (parse_dec_number(parser)) {
            f64 val = strtod(parser->cur_token.start, NULL);
            parser->cur_token.value = F64_TO_VALUE(val);

            parser->cur_token.len = (u32)(parser->next_char - parser->cur_token.start - 1);
            parser->cur_token.type = TOKEN_F64;
            return;
        } else {
            int val = strtol(parser->cur_token.start, NULL, 10);
            parser->cur_token.value = I32_TO_VALUE(val);
        }
    }

    parser->cur_token.len = (u32)(parser->next_char - parser->cur_token.start - 1);
    parser->cur_token.type = TOKEN_I32;
}

void get_next_token(Parser* parser) {
    parser->pre_token = parser->cur_token;
    skip_blanks(parser);
    
    parser->cur_token.type = TOKEN_EOF;
    parser->cur_token.len = 0;
    parser->cur_token.start = parser->next_char - 1;

    #define CASE(ch, ty) \
        case ch:\
            parser->cur_token.type = TOKEN_##ty;\
            break;
    #define CASE_NEXT(ch, next, cur_ty, next_ty) \
        case ch:\
            parser->cur_token.type = match_next_char(parser, next) ? TOKEN_##next_ty : TOKEN_##cur_ty;\
            break;
    #define CASE_2NEXT(ch, n1, n2, ty, ty1, ty2) \
        case ch:\
            parser->cur_token.type = match_next_char(parser, n1) ? TOKEN_##ty1 : (match_next_char(parser, n2) ? TOKEN_##ty2 : TOKEN_##ty);\
            break;

    while (parser->cur_char != '\0') {
        switch (parser->cur_char) {
            CASE(',', COMMA)
            CASE(':', COLON)
            CASE(';', SEMICOLON)
            CASE('[', LB) CASE(']', RB)
            CASE('{', LC) CASE('}', RC)
            CASE('(', LP)
            case ')':
                if (parser->interpolation_rp_trace > 0) {
                    parser->interpolation_rp_trace--;
                    if (parser->interpolation_rp_trace == 0) {
                        parse_string(parser);
                        break;
                    }
                }
                parser->cur_token.type = TOKEN_RP;
                break;
            CASE_NEXT('.', '.', DOT, DOTDOT)
            CASE('+', ADD) CASE('-', SUB) CASE('*', MUL) CASE('%', MOD)
            case '/':
                if (match_next_char(parser, '/') || match_next_char(parser, '*')) {
                    skip_comment(parser);
                    parser->cur_token.start = parser->next_char - 1;
                    continue;
                }
                parser->cur_token.type = TOKEN_DIV;
                break;
            CASE_NEXT('&', '&', BIT_AND, LOGICAL_AND)
            CASE_NEXT('|', '|', BIT_OR, LOGICAL_OR)
            CASE('~', BIT_NOT)
            CASE('?', QUESTION)
            CASE_NEXT('=', '=', ASSIGN, EQ)
            CASE_NEXT('!', '=', LOGICAL_NOT, NE)
            CASE_2NEXT('>', '>', '=', GT, BIT_SR, GE)
            CASE_2NEXT('<', '>', '=', LT, BIT_SL, LE)
            case '"':
                parse_string(parser);
                break;
            
            default:
                if (isalpha(parser->cur_char) || parser->cur_char == '_') {
                    parse_id(parser, TOKEN_UNKNOWN);
                    return;
                }

                if (isdigit(parser->cur_char)) {
                    parse_number(parser);
                    return;
                }

                if (parser->cur_char == '#' && match_next_char(parser, '!')) {
                    skip_aline(parser);
                    skip_blanks(parser);
                    parser->cur_token.start = parser->next_char - 1;
                    continue;
                }

                LEX_ERROR(parser, "unsupport char '%c'.", parser->cur_char);
                return;
        }
        #undef CASE
        #undef CASE_NEXT
        #undef CASE_2NEXT

        parser->cur_token.len = (u32)(parser->next_char - parser->cur_token.start);
        get_next_char(parser);
        return;
    }
}

inline bool match_token(Parser* parser, TokenType expected) {
    if (parser->cur_token.type == expected) {
        get_next_token(parser);
        return true;
    }
    return false;
}

inline void consume_cur_token(Parser* parser, TokenType expected, const char* msg) {
    if (!match_token(parser, expected)) {
        COMPILE_ERROR(parser, msg);
    }
}

inline void consume_next_token(Parser* parser, TokenType expected, const char* msg) {
    get_next_token(parser);
    consume_cur_token(parser, expected, msg);
}

void init_parser(VM* vm, Parser* parser, const char* file, const char* src, ObjModule* module) {
    parser->file = file;
    parser->src = src;
    parser->cur_char = *parser->src;
    parser->next_char = parser->src + 1;
    parser->cur_token = (Token) {
        .line = 1,
        .len = 0,
        .start = NULL,
        .type = TOKEN_UNKNOWN,
    };
    parser->pre_token = parser->cur_token;
    parser->interpolation_rp_trace = 0;
    parser->vm = vm;
    parser->cur_module = module;
}
