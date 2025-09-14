#include "ast.h"
#include "class.h"
#include "common.h"
#include "obj_string.h"
#include "parser.h"
#include "sparrow.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include "vm.h"

// 一条 import 语句中最多能引入的模块变量数
#define IMPORT_VAR_MAX_NUM 64

typedef AST_Expr* (*NudFunc)(Parser* parser, bool can_assign);
typedef AST_Expr* (*LedFunc)(Parser* parser, AST_Expr* l, bool can_assign);
typedef void (*MethodSignatureFunc)(Parser* parser, struct _ClassMethod* method); // 签名函数

typedef enum {
    BP_NONE,
    BP_LOWEST,
    BP_ASSIGN,
    BP_CONDITION,
    BP_LOGICAL_OR,
    BP_LOGICAL_AND,
    BP_IS,
    BP_BIT_OR,
    BP_BIT_XOR,
    BP_BIT_AND,
    BP_EQ,
    BP_CMP,
    BP_BIT_SHIFT,
    BP_RANGE,
    BP_TERM,
    BP_FACTOR,
    BP_UNARY,
    BP_CALL,
    BP_HIGHEST,
} BindPower;

typedef struct {
    const char* id;
    BindPower lbp;
    NudFunc nud;
    LedFunc led;
    MethodSignatureFunc method_sign;
} AST_SymbolBindRule;

AST_Expr* compile_expr(Parser* parser, BindPower rbp);

// 编译实参列表
static void compile_args(Parser* parser, u32* argc, AST_Expr* args[MAX_ARG_NUM]) {
    do {
        if (*argc > MAX_ARG_NUM) {
            COMPILE_ERROR(parser, "argc must less then %d.", MAX_ARG_NUM);
        }
        args[(*argc)++] = compile_expr(parser, BP_LOWEST);
    } while (match_token(parser, TOKEN_COMMA));
}

AST_Expr* literal(Parser* parser, bool can_assign) {
    AST_Expr* expr = malloc(sizeof(AST_Expr));
    expr->type = AST_LITERAL_EXPR;
    expr->expr.literal = parser->pre_token.value;
    if (VALUE_IS_OBJ(expr->expr.literal)) {
        BufferAdd(Value, &parser->vm->ast_obj_root, parser->vm, expr->expr.literal);
    }
    return expr;
}

AST_Expr* literal_true(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_LITERAL_TRUE;
    return res;
}

AST_Expr* literal_false(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_LITERAL_FALSE;
    return res;
}

AST_Expr* literal_null(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_LITERAL_NULL;
    return res;
}

AST_Expr* self(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_SELF_EXPR;
    return res;
}

AST_Expr* super(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_SUPER_EXPR;
    AST_SuperCallExpr* expr = &res->expr.super_call;

    if (match_token(parser, TOKEN_DOT)) {
        consume_cur_token(parser, TOKEN_ID, "expect method name after 'super.'");

        expr->method_name = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
    } else {
        expr->method_name = (ScriptID) {.start = NULL, .len = -1};

        // 没有 method name 可能是 subscript subscript-setter
        if (match_token(parser, TOKEN_LB)) {
            expr->type = SUPER_SUBSCRIPT;
            expr->call_method.subscript.argc = 0;
            
            if (!match_token(parser, TOKEN_RB)) {
                compile_args(parser, &expr->call_method.subscript.argc, expr->call_method.subscript.args);
                consume_cur_token(parser, TOKEN_RB, "expect ']' in the end of index.");
            } else {
                COMPILE_ERROR(parser, "subscript method must have index.");
            }

            if (match_token(parser, TOKEN_ASSIGN)) {
                expr->type = SUPER_SUBSCRIPT_SETTER;
                expr->call_method.subscript.args[expr->call_method.subscript.argc++] = compile_expr(parser, BP_LOWEST);
            }

            return res;
        }
    }

    // 无论是否有 method name，都可能被解析为以下三种类型
    if (match_token(parser, TOKEN_LP)) {
        expr->type = SUPER_METHOD;
        expr->call_method.method.argc = 0;

        // 解析参数
        if (!match_token(parser, TOKEN_RP)) {
            compile_args(parser, &expr->call_method.method.argc, expr->call_method.method.args);
            consume_cur_token(parser, TOKEN_RP, "expect ')' in the end of args list.");
        }
    } else if (match_token(parser, TOKEN_ASSIGN)) {
        expr->type = SUPER_SETTER;
        expr->call_method.setter_value = compile_expr(parser, BP_LOWEST);
    } else {
        expr->type = SUPER_GETTER;
    }

    return res;
}

AST_Expr* string_interpolation(Parser* parser, bool can_assign) {
    // 去糖，编译为[array literal].join();
    
    // 构建array literal
    AST_Expr* obj = malloc(sizeof(AST_Expr));
    obj->type = AST_ARRAY_LITERAL;
    
    AST_ArrayLiteral* arr = &obj->expr.array_literal;
    do {
        struct AST_ArrayItem* item_str = malloc(sizeof(struct AST_ArrayItem));
        
        // 解析interpolation
        if (((ObjString*)parser->pre_token.value.header)->val.len != 0) { // 当其为非空字符串时添加
            item_str->item = literal(parser, false);
            item_str->next = NULL;
        } else {
            item_str->next = item_str; // item_str中没有内容，复用空间避免重复申请内存
        }
        
        item_str->next = item_str->next ?: malloc(sizeof(struct AST_ArrayItem));
        item_str->next->item = compile_expr(parser, BP_LOWEST);
        item_str->next->next = NULL;

        if (arr->head == NULL) {
            arr->head = item_str;
        } else {
            arr->tail->next = item_str;
        }
        arr->tail = item_str->next ?: item_str;
    } while (match_token(parser, TOKEN_INTERPOLATION));

    // 必然以string结尾
    consume_cur_token(
        parser, TOKEN_STRING,
        "expect string at the end of interpolatation."
    );

    // 结尾的TOKEN_STRING
    if (((ObjString*)parser->pre_token.value.header)->val.len != 0) { // 当其为非空字符串时添加
        struct AST_ArrayItem* item_str = malloc(sizeof(struct AST_ArrayItem));
        item_str->item = literal(parser, false);
        item_str->next = NULL;
        arr->tail->next = item_str;
        arr->tail = item_str;
    }

    // .join()
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_CALL_METHOD_EXPR;

    AST_CallMethodExpr* method = &res->expr.call_method;
    method->obj = obj;
    method->method_name = (ScriptID) {.start = "join", .len = 4};
    method->argc = 0;

    return res;
}

AST_Expr* parentheses(Parser* parser, bool can_assign) {
    AST_Expr* res = compile_expr(parser, BP_LOWEST);
    consume_cur_token(parser, TOKEN_RP, "expect ')' after expression.");
    return res;
}

AST_Expr* list_literal(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_ARRAY_LITERAL;

    AST_ArrayLiteral* arr = &res->expr.array_literal;
    arr->head = NULL;
    arr->tail = NULL;

    do {
        if (PEEK_TOKEN(parser) == TOKEN_RB) {
            break;
        }

        struct AST_ArrayItem* new = malloc(sizeof(struct AST_ArrayItem));
        new->next = NULL;
        new->item = compile_expr(parser, BP_LOWEST);

        if (arr->head == NULL) {
            arr->head = new;
        } else {
            arr->tail->next = new;
        }
        arr->tail = new;
    } while (match_token(parser, TOKEN_COMMA));

    consume_cur_token(parser, TOKEN_RB, "expect ']' after list element.");

    return res;
}

AST_Expr* map_literal(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_MAP_LITERAL;

    AST_MapLiteral* map = &res->expr.map_literal;
    map->entrys = NULL;

    do {
        if (PEEK_TOKEN(parser) == TOKEN_RC) {
            break;
        }

        struct AST_MapEntry* new = malloc(sizeof(struct AST_MapEntry));
        new->next = map->entrys;
        map->entrys = new;

        new->key = compile_expr(parser, BP_UNARY);
        consume_cur_token(parser, TOKEN_COLON, "expect ':' after key.");
        new->val = compile_expr(parser, BP_LOWEST);
    } while (match_token(parser, TOKEN_COMMA));

    consume_cur_token(parser, TOKEN_RC, "map literal should end with '}'.");

    return res;
}

AST_Expr* id(Parser* parser, bool can_assign) {
    ScriptID id = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
    
    AST_Expr* res = malloc(sizeof(AST_Expr));

    res->type = AST_ID_EXPR;
    res->expr.id = id;

    // id后接(...)解析为id_call
    if (match_token(parser, TOKEN_LP)) {
        res->type = AST_ID_CALL_EXPR;
        
        AST_IdCallExpr* call = &res->expr.id_call;
        call->id = id;
        if (!match_token(parser, TOKEN_RP)) {
            compile_args(parser, &call->argc, call->args);
            consume_cur_token(parser, TOKEN_RP, "expect ')' in the end of arg list.");
        } else {
            call->argc = 0;
        }
    }

    return res;
}

AST_Expr* condition(Parser* parser, AST_Expr* l, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_CONDITION_EXPR;

    AST_ConditionExpr* expr = &res->expr.condition_expr;
    expr->condition = l;

    expr->true_val = compile_expr(parser, BP_LOWEST);
    consume_cur_token(parser, TOKEN_COLON, "expect ':' after true branch.");
    expr->false_val = compile_expr(parser, BP_LOWEST);

    return res;
}

AST_Expr* logical_or(Parser* parser, AST_Expr* l, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_LOGICAL_OR;
    res->expr.logical_cmp.l = l;
    res->expr.logical_cmp.r = compile_expr(parser, BP_LOGICAL_OR);
    return res;
}

AST_Expr* logical_and(Parser* parser, AST_Expr* l, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_LOGICAL_AND;
    res->expr.logical_cmp.l = l;
    res->expr.logical_cmp.r = compile_expr(parser, BP_LOGICAL_AND);
    return res;
}

AST_Expr* assign(Parser* parser, AST_Expr* l, bool can_assign) {
    if (l->type != AST_ID_EXPR) {
        COMPILE_ERROR(parser, "the left operand of the infix operator '=' must be an id.");
    }

    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_ASSIGN_EXPR;

    AST_AssignExpr* assign = &res->expr.assign;
    assign->id = l->expr.id;
    assign->expr = compile_expr(parser, BP_LOWEST);

    free(l); // 左操作数不再追踪
    return res;
}

#define COMPOUND_ASSIGNMENT_OPERATOR(func_name, op_str) \
    AST_Expr* func_name##_assign(Parser* parser, AST_Expr* l, bool can_assign) { \
        if (l->type != AST_ID_EXPR) { \
            COMPILE_ERROR(parser, "the left operand of the infix operator '" op_str "=' must be an id."); \
        } \
        AST_Expr* res = malloc(sizeof(AST_Expr)); \
        res->type = AST_ASSIGN_EXPR; \
        AST_AssignExpr* assign = &res->expr.assign; \
        assign->id = l->expr.id; \
        assign->expr = ({ \
            AST_Expr* infix_expr = malloc(sizeof(AST_Expr)); \
            infix_expr->type = AST_INFIX_EXPR; \
            infix_expr->expr.infix = (AST_InfixExpr) { \
                .op = op_str, \
                .l = l, \
                .r = compile_expr(parser, BP_LOWEST), \
            }; \
            infix_expr; \
        }); \
        return res; \
    }

COMPOUND_ASSIGNMENT_OPERATOR(add, "+")
COMPOUND_ASSIGNMENT_OPERATOR(sub, "-")
COMPOUND_ASSIGNMENT_OPERATOR(mul, "*")
COMPOUND_ASSIGNMENT_OPERATOR(div, "/")
COMPOUND_ASSIGNMENT_OPERATOR(mod, "%")
COMPOUND_ASSIGNMENT_OPERATOR(bit_or, "|")
COMPOUND_ASSIGNMENT_OPERATOR(bit_and, "&")
COMPOUND_ASSIGNMENT_OPERATOR(bit_xor, "^")

#undef COMPOUND_ASSIGNMENT_OPERATOR

AST_Expr* unary_operator(Parser* parser, bool can_assign);
AST_Expr* closure_expr(Parser* parser, bool can_assign);

AST_Expr* infix_operator(Parser* parser, AST_Expr* l, bool can_assign);
AST_Expr* subscript(Parser* parser, AST_Expr* l, bool can_assign);
AST_Expr* call_entry(Parser* parser, AST_Expr* l, bool can_assign);

void infix_method_signature(Parser* parser, struct _ClassMethod* method);
void unary_method_signature(Parser* parser, struct _ClassMethod* method);
void id_method_signature(Parser* parser, struct _ClassMethod* method);
void subscript_method_signature(Parser* parser, struct _ClassMethod* method);
void mix_method_signature(Parser* parser, struct _ClassMethod* method);

#define PREFIX_SYMBOL(nud)      {NULL,  BP_NONE,    nud,            NULL,           NULL}
#define INFIX_SYMBOL(lbp, led)  {NULL,  lbp,        NULL,           led,            NULL}
#define INFIX_OPERATOR(id, lbp) {id,    lbp,        NULL,           infix_operator, infix_method_signature}
#define MIX_OPERATOR(id)        {id,    BP_TERM,    unary_operator, infix_operator, mix_method_signature}
#define UNUSED_RULE             {NULL,  BP_NONE,    NULL,           NULL,           NULL}
#define PREFIX_OPERATOR(id)     {id,    BP_NONE,    unary_operator, NULL,           unary_method_signature}

AST_SymbolBindRule AST_Rules[] = {
    [TOKEN_UNKNOWN]         = UNUSED_RULE,
    [TOKEN_U8]              = PREFIX_SYMBOL(literal),
    [TOKEN_U32]             = PREFIX_SYMBOL(literal),
    [TOKEN_I32]             = PREFIX_SYMBOL(literal),
    [TOKEN_F64]             = PREFIX_SYMBOL(literal),
    [TOKEN_STRING]          = PREFIX_SYMBOL(literal),
    [TOKEN_ID]              = {NULL, BP_NONE, id, NULL, id_method_signature},
    [TOKEN_INTERPOLATION]   = PREFIX_SYMBOL(string_interpolation),
    [TOKEN_TRUE]            = PREFIX_SYMBOL(literal_true),
    [TOKEN_FALSE]           = PREFIX_SYMBOL(literal_false),
    [TOKEN_NULL]            = PREFIX_SYMBOL(literal_null),
    [TOKEN_SELF]            = PREFIX_SYMBOL(self),
    [TOKEN_IS]              = INFIX_OPERATOR("is", BP_IS),
    [TOKEN_SUPER]           = PREFIX_SYMBOL(super),
    [TOKEN_LP]              = PREFIX_SYMBOL(parentheses),
    [TOKEN_LB]              = {NULL, BP_CALL, list_literal, subscript, subscript_method_signature},
    [TOKEN_LC]              = PREFIX_SYMBOL(map_literal),
    [TOKEN_DOT]             = INFIX_SYMBOL(BP_CALL, call_entry),
    [TOKEN_DOTDOT]          = INFIX_OPERATOR("..", BP_RANGE),
    [TOKEN_ADD]             = INFIX_OPERATOR("+", BP_TERM),
    [TOKEN_SUB]             = MIX_OPERATOR("-"),
    [TOKEN_MUL]             = INFIX_OPERATOR("*", BP_FACTOR),
    [TOKEN_DIV]             = INFIX_OPERATOR("/", BP_FACTOR),
    [TOKEN_MOD]             = INFIX_OPERATOR("\%", BP_FACTOR),
    [TOKEN_BIT_AND]         = INFIX_OPERATOR("&", BP_BIT_AND),
    [TOKEN_BIT_OR]          = INFIX_OPERATOR("|", BP_BIT_OR),
    [TOKEN_BIT_NOT]         = PREFIX_OPERATOR("~"),
    [TOKEN_BIT_SL]          = INFIX_OPERATOR("<<", BP_BIT_SHIFT),
    [TOKEN_BIT_SR]          = INFIX_OPERATOR(">>", BP_BIT_SHIFT),
    [TOKEN_LOGICAL_OR]      = INFIX_SYMBOL(BP_LOGICAL_OR, logical_or),
    [TOKEN_LOGICAL_AND]     = INFIX_SYMBOL(BP_LOGICAL_AND, logical_and),
    [TOKEN_LOGICAL_NOT]     = PREFIX_OPERATOR("!"),
    [TOKEN_EQ]              = INFIX_OPERATOR("==", BP_EQ),
    [TOKEN_NE]              = INFIX_OPERATOR("!=", BP_EQ),
    [TOKEN_GT]              = INFIX_OPERATOR(">", BP_CMP),
    [TOKEN_GE]              = INFIX_OPERATOR(">=", BP_CMP),
    [TOKEN_LT]              = INFIX_OPERATOR("<", BP_CMP),
    [TOKEN_LE]              = INFIX_OPERATOR("<=", BP_CMP),
    [TOKEN_QUESTION]        = INFIX_SYMBOL(BP_CONDITION, condition),
    [TOKEN_FN]              = PREFIX_SYMBOL(closure_expr),
    [TOKEN_ASSIGN]          = INFIX_SYMBOL(BP_ASSIGN, assign),
    [TOKEN_BIT_XOR]         = INFIX_OPERATOR("^", BP_BIT_XOR),
    [TOKEN_ADD_ASSIGN]      = INFIX_SYMBOL(BP_ASSIGN, add_assign),
    [TOKEN_SUB_ASSIGN]      = INFIX_SYMBOL(BP_ASSIGN, sub_assign),
    [TOKEN_MUL_ASSIGN]      = INFIX_SYMBOL(BP_ASSIGN, mul_assign),
    [TOKEN_DIV_ASSIGN]      = INFIX_SYMBOL(BP_ASSIGN, div_assign),
    [TOKEN_MOD_ASSIGN]      = INFIX_SYMBOL(BP_ASSIGN, mod_assign),
    [TOKEN_BIT_AND_ASSIGN]  = INFIX_SYMBOL(BP_ASSIGN, bit_and_assign),
    [TOKEN_BIT_OR_ASSIGN]   = INFIX_SYMBOL(BP_ASSIGN, bit_or_assign),
    [TOKEN_BIT_XOR_ASSIGN]  = INFIX_SYMBOL(BP_ASSIGN, bit_xor_assign),
};

AST_Expr* unary_operator(Parser* parser, bool can_assign) {
    AST_SymbolBindRule* rule = &AST_Rules[parser->pre_token.type];
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_PREFIX_EXPR;
    res->expr.prefix = (AST_PrefixExpr) {
        .op = rule->id,
        .expr = compile_expr(parser, BP_UNARY),
    };
    return res;
}

AST_Expr* infix_operator(Parser* parser, AST_Expr* l, bool can_assign) {
    AST_SymbolBindRule* rule = &AST_Rules[parser->pre_token.type];
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_INFIX_EXPR;
    res->expr.infix = (AST_InfixExpr) {
        .op = rule->id,
        .l = l,
        .r = compile_expr(parser, rule->lbp),
    };
    return res;
}

// 类型注释
static bool has_pending_gt = false; // 为了解决 '>>' 闭合 <> 的问题。
static void type_annotation(Parser* parser) {
    // 类型注释按规则解析后不保存任何信息，只做注释用
    // String | List<String> | Map<String, int> | List<Map<String, int>> | Tuple<int, int, int> | Fn<(T) -> K> | Fn<() -> None>?
    // 类型均以id起始，后可以接可嵌套的‘<>’，‘<>’中至少有一个id，id之间可以使用‘,’但不能以‘,’结尾。允许使用‘|’表示或关系
    // 所有类型后均可添加?表示可空类型
    // callable类型内部以‘(’起始，返回值标注‘->’必须，参数列表中不可使用可变参数标注‘...’（鉴于语言本身不支持可变参数）

    consume_cur_token(parser, TOKEN_ID, "typping must start by id.");

    if (match_token(parser, TOKEN_LT)) {
        if (match_token(parser, TOKEN_LP)) {
            // callable标注
            // 参数列表解析
            if (!match_token(parser, TOKEN_RP)) {
                do {
                    type_annotation(parser);
                } while (match_token(parser, TOKEN_COMMA));

                consume_cur_token(parser, TOKEN_RP, "uncloused callable typping parameter.");
            }

            consume_cur_token(parser, TOKEN_SUB, "expect callable typping result.");
            consume_cur_token(parser, TOKEN_GT, "expect callable typping result.");

            type_annotation(parser); // result type
        } else {
            // 一般标注
            do {
                type_annotation(parser);
            } while (match_token(parser, TOKEN_COMMA));
        }

        if (has_pending_gt) {
            has_pending_gt = false;
        } else if (match_token(parser, TOKEN_BIT_SR)) {
            has_pending_gt = true;
        } else {
            consume_cur_token(parser, TOKEN_GT, "uncloused typping <>.");
        }
    }

    match_token(parser, TOKEN_QUESTION);

    if (match_token(parser, TOKEN_BIT_OR)) {
        type_annotation(parser);
    }
}
#define FUNCTION_RESULT_TYPPING_CHECK() \
    if (match_token(parser, TOKEN_SUB)) { \
        consume_cur_token(parser, TOKEN_GT, "expect '->' for result typping."); \
        type_annotation(parser); \
    }
#define VAR_TYPPING_CHECK() \
    if (match_token(parser, TOKEN_COLON)) { \
        type_annotation(parser); \
    }

inline static void native_annotation(Parser* parser) {
    match_token(parser, TOKEN_STATIC);

    if (match_token(parser, TOKEN_LB)) {
        // native [_]; native [_] = (_);
        do {
            consume_cur_token(parser, TOKEN_ID, "(native annotation): expect parameter list.");
            if (match_token(parser, TOKEN_COLON)) {
                type_annotation(parser);
            }
        } while (match_token(parser, TOKEN_COMMA));
        consume_cur_token(parser, TOKEN_RB, "(native annotation): expect ']';");
    } else if (AST_Rules[parser->cur_token.type].method_sign != NULL) {
        // native id(); native id = (_);
        get_next_token(parser);
        if (match_token(parser, TOKEN_LP) && !match_token(parser, TOKEN_RP)) {
            do {
                consume_cur_token(parser, TOKEN_ID, "(native annotation): expect parameter list.");
                if (match_token(parser, TOKEN_COLON)) {
                    type_annotation(parser);
                }
            } while (match_token(parser, TOKEN_COMMA));
            consume_cur_token(parser, TOKEN_RP, "(native annotation): expect ')';");
        }
    } else {
        COMPILE_ERROR(parser, "(native annotation): expect id or [ or -> for native function.");
    }

    if (match_token(parser, TOKEN_ASSIGN)) {
        consume_cur_token(parser, TOKEN_LP, "(native annotation): expect '= (id)'.");
        consume_cur_token(parser, TOKEN_ID, "(native annotation): expect '= (id)'.");
        if (match_token(parser, TOKEN_COLON)) {
            type_annotation(parser);
        }
        consume_cur_token(parser, TOKEN_RP, "(native annotation): expect '= (id)'.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();

    consume_cur_token(parser, TOKEN_SEMICOLON, "(native annotation): expect ';'.");
}

// 解析型参列表
static void process_para_list(Parser* parser, u32* argc, ScriptID names[MAX_ARG_NUM]) {
    u32 origin_argc = *argc;

    do {
        if (++(*argc) > MAX_ARG_NUM) {
            COMPILE_ERROR(parser, "the max number of argument is %d.", MAX_ARG_NUM);
        }

        consume_cur_token(parser, TOKEN_ID, "expect param name.");
        names[*argc - 1] = (ScriptID) {.start = parser->pre_token.start, .len = parser->pre_token.len};

        VAR_TYPPING_CHECK();
    } while (match_token(parser, TOKEN_COMMA));
}

void infix_method_signature(Parser* parser, struct _ClassMethod* method) {
    method->type = AST_CLASS_METHOD;

    // 保存 id
    method->name = (ScriptID) {.start = parser->pre_token.start, .len = parser->pre_token.len};

    // 解析型参
    method->argc = 1;
    consume_cur_token(parser, TOKEN_LP, "expect '(' after infix operator.");
    
    consume_cur_token(parser, TOKEN_ID, "expect var name.");
    VAR_TYPPING_CHECK();

    method->arg_names[0] = (ScriptID) {.start = parser->pre_token.start, .len = parser->pre_token.len};

    consume_cur_token(parser, TOKEN_RP, "expect ')' after var name.");

    FUNCTION_RESULT_TYPPING_CHECK();
}

void unary_method_signature(Parser* parser, struct _ClassMethod* method) {
    method->type = AST_CLASS_GETTER;
    method->name = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
    method->argc = 0;
    FUNCTION_RESULT_TYPPING_CHECK();
}

void id_method_signature(Parser* parser, struct _ClassMethod* method) {
    method->name = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
    method->type = AST_CLASS_GETTER;

    if (method->name.len == 3 && memcmp(method->name.start, "new", 3) == 0) {
        method->type = AST_CLASS_CONSTRUCTOR;
        consume_cur_token(parser, TOKEN_LP, "constructor must be a method.");
        if (!match_token(parser, TOKEN_RP)) {
            process_para_list(parser, &method->argc, method->arg_names);
            consume_cur_token(parser, TOKEN_RP, "expect ')' for parameter list");
        }
    } else if (match_token(parser, TOKEN_LP)) {
        method->type = AST_CLASS_METHOD;
        method->argc = 0;
        if (!match_token(parser, TOKEN_RP)) {
            process_para_list(parser, &method->argc, method->arg_names);
            consume_cur_token(parser, TOKEN_RP, "expect ')' for parameter list");
        }
    } else if (match_token(parser, TOKEN_ASSIGN)) {
        method->type = AST_CLASS_SETTER;
        method->argc = 1;
        consume_cur_token(parser, TOKEN_LP, "setter must have parameter list.");
        
        consume_cur_token(parser, TOKEN_ID, "expect parameter name.");
        method->arg_names[0] = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
        VAR_TYPPING_CHECK();

        consume_cur_token(parser, TOKEN_RP, "expect ')' for parameter list");
    }
    
    FUNCTION_RESULT_TYPPING_CHECK();
}

AST_Expr* subscript(Parser* parser, AST_Expr* l, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    
    res->type = AST_SUBSCRIPT_EXPR;
    AST_SubscriptExpr* subscript = &res->expr.subscript;
    subscript->obj = l;
    subscript->argc = 0;

    if (match_token(parser, TOKEN_RB)) {
        COMPILE_ERROR(parser, "subscript argc must > 0.");
    }
    compile_args(parser, &subscript->argc, subscript->args);
    consume_cur_token(parser, TOKEN_RB, "expect ']' in the end of args list.");

    if (match_token(parser, TOKEN_ASSIGN)) {
        res->type = AST_SUBSCRIPT_SETTER_EXPR;
        subscript->args[subscript->argc++] = compile_expr(parser, BP_LOWEST);
    }

    return res;
}

AST_Expr* call_entry(Parser* parser, AST_Expr* l, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));

    consume_cur_token(parser, TOKEN_ID, "expect method name.");
    ScriptID name = SCRIPT_ID_FROM_TOKEN(parser->pre_token);

    #define COMPOUND_ASSIGNMENT_OPERATOR(op_str) \
        res->type = AST_SETTER_EXPR; \
        AST_SetterExpr* setter = &res->expr.setter; \
        setter->obj = l; \
        setter->method_name = name; \
        setter->val = malloc(sizeof(AST_Expr)); \
        setter->val->type = AST_INFIX_EXPR; \
        setter->val->expr.infix = (AST_InfixExpr) { \
            .op = op_str, \
            .l = ({ \
                AST_Expr* expr = malloc(sizeof(AST_Expr)); \
                expr->type = AST_GETTER_EXPR; \
                expr->expr.getter.obj = malloc(sizeof(AST_Expr)); \
                *expr->expr.getter.obj = *setter->obj; \
                expr->expr.getter.method_name = setter->method_name; \
                expr; \
            }), \
            .r = compile_expr(parser, BP_LOWEST), \
        };

    if (match_token(parser, TOKEN_LP)) {
        res->type = AST_CALL_METHOD_EXPR;
        AST_CallMethodExpr* method = &res->expr.call_method;
        method->obj = l;
        method->method_name = name;
        method->argc = 0;

        if (!match_token(parser, TOKEN_RP)) {
            compile_args(parser, &method->argc, method->args);
            consume_cur_token(parser, TOKEN_RP, "expect ')' in the end of args.");
        }
    } else if (can_assign && match_token(parser, TOKEN_ASSIGN)) {
        res->type = AST_SETTER_EXPR;
        AST_SetterExpr* setter = &res->expr.setter;
        setter->obj = l;
        setter->method_name = name;
        setter->val = compile_expr(parser, BP_LOWEST);
    } else if (can_assign && match_token(parser, TOKEN_ADD_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("+");
    } else if (can_assign && match_token(parser, TOKEN_SUB_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("-");
    } else if (can_assign && match_token(parser, TOKEN_MUL_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("*");
    } else if (can_assign && match_token(parser, TOKEN_DIV_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("/");
    } else if (can_assign && match_token(parser, TOKEN_MOD_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("%");
    } else if (can_assign && match_token(parser, TOKEN_BIT_AND_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("&");
    } else if (can_assign && match_token(parser, TOKEN_BIT_OR_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("|");
    } else if (can_assign && match_token(parser, TOKEN_BIT_XOR_ASSIGN)) {
        COMPOUND_ASSIGNMENT_OPERATOR("^");
    } else {
        res->type = AST_GETTER_EXPR;
        AST_GetterExpr* getter = &res->expr.getter;
        getter->obj = l;
        getter->method_name = name;
    }

    #undef COMPOUND_ASSIGNMENT_OPERATOR

    return res;
}

void subscript_method_signature(Parser* parser, struct _ClassMethod* method) {
    method->name = SCRIPT_ID_NULL();
    method->type = AST_CLASS_SUBSCRIPT;
    
    method->argc = 0;
    if (match_token(parser, TOKEN_RB)) {
        COMPILE_ERROR(parser, "subscript argc must > 0.");
    }
    process_para_list(parser, &method->argc, method->arg_names);
    consume_cur_token(parser, TOKEN_RB, "expect ']' in the end of args list.");

    if (match_token(parser, TOKEN_ASSIGN)) {
        method->type = AST_CLASS_SUBSCRIPT_SETTER;

        consume_cur_token(parser, TOKEN_LP, "subscript-setter must have parameter list.");
        
        consume_cur_token(parser, TOKEN_ID, "expect parameter name.");
        method->arg_names[method->argc++] = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
        VAR_TYPPING_CHECK();

        consume_cur_token(parser, TOKEN_RP, "subscript-setter only have 1 parameter, expect ')' in the end of parameter list.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();
}

void mix_method_signature(Parser* parser, struct _ClassMethod* method) {
    method->name = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
    
    if (match_token(parser, TOKEN_LP)) {
        method->type = AST_CLASS_METHOD;
        method->argc = 1;
        consume_cur_token(parser, TOKEN_LP, "expect '(' in the start of parameter list.");
        consume_cur_token(parser, TOKEN_ID, "expect parameter name.");
        method->arg_names[0] = SCRIPT_ID_FROM_TOKEN(parser->pre_token);
        VAR_TYPPING_CHECK();
        consume_cur_token(parser, TOKEN_RP, "expect ')' in the end of parameter list.");
    } else {
        method->type = AST_CLASS_GETTER;
        method->argc = 0;
    }

    FUNCTION_RESULT_TYPPING_CHECK();
}

AST_Expr* compile_expr(Parser* parser, BindPower rbp) {    
    bool can_assign = rbp < BP_ASSIGN;

    NudFunc nud = AST_Rules[parser->cur_token.type].nud;
    if (nud == NULL) {
        COMPILE_ERROR(
            parser, "token '%.*s' nud is NULL.",
            parser->cur_token.len, parser->cur_token.start
        );
    }
    get_next_token(parser);
    AST_Expr* res = nud(parser, can_assign);

    while (rbp < AST_Rules[parser->cur_token.type].lbp) {
        LedFunc led = AST_Rules[parser->cur_token.type].led;
        ASSERT(led != NULL, "token's led is NULL.");
        get_next_token(parser);
        res = led(parser, res, can_assign);
    }

    return res;
}

AST_Stmt* compile_stmt(Parser* parser);
void compile_var_def(Parser* parser, AST_VarDef* res);

void compile_block(Parser* parser, AST_Block* res) {
    while (!match_token(parser, TOKEN_RC)) {
        if (PEEK_TOKEN(parser) == TOKEN_EOF) {
            COMPILE_ERROR(parser, "expect '}' at the end of block.");
        }

        struct AST_BlockContext* context = malloc(sizeof(struct AST_BlockContext));
        context->next = NULL;

        context->stmt = compile_stmt(parser);

        if (res->head == NULL) {
            res->head = context;
        } else {
            res->tail->next = context;
        }
        res->tail = context;
    }
}

AST_Expr* closure_expr(Parser* parser, bool can_assign) {
    AST_Expr* res = malloc(sizeof(AST_Expr));
    res->type = AST_CLOSURE_EXPR;
    
    AST_ClosureExpr* closure = &res->expr.closure;
    closure->argc = 0;

    consume_cur_token(parser, TOKEN_LP, "closure must have parameter list (even it's empty).");
    if (!match_token(parser, TOKEN_RP)) {
        process_para_list(parser, &closure->argc, closure->arg_names);
        consume_cur_token(parser, TOKEN_RP, "expect ')' in the end of parameter list.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();

    closure->body = malloc(sizeof(AST_Block));
    consume_cur_token(parser, TOKEN_LC, "expect '{' in the start of closure body.");
    compile_block(parser, closure->body);

    return res;
}

void compile_if_stmt(Parser* parser, AST_IfStmt* res) {
    res->condition = compile_expr(parser, BP_LOWEST);

    consume_cur_token(parser, TOKEN_LC, "expect '{' for if stmt then block.");
    res->then_block = malloc(sizeof(AST_Block));
    compile_block(parser, res->then_block);

    if (match_token(parser, TOKEN_ELSE)) {
        if (match_token(parser, TOKEN_IF)) {
            res->else_type = ELSE_IF;
            res->else_branch.else_if = malloc(sizeof(AST_IfStmt));
            compile_if_stmt(parser, res->else_branch.else_if);
        } else {
            res->else_type = ELSE_BLOCK;
            consume_cur_token(parser, TOKEN_LC, "expect '{' for if stmt else block.");
            res->else_branch.block = malloc(sizeof(AST_Block));
            compile_block(parser, res->else_branch.block);
        }
    } else {
        res->else_type = ELSE_NONE;
        res->else_branch.block = NULL;
    }
}

void compile_while_stmt(Parser* parser, AST_WhileStmt* res) {
    res->condition = compile_expr(parser, BP_LOWEST);
    res->body = malloc(sizeof(AST_Block));
    consume_cur_token(parser, TOKEN_LC, "expect '{' for while body start.");
    compile_block(parser, res->body);
}

void compile_loop_stmt(Parser* parser, AST_WhileStmt* res) {
    res->condition = malloc(sizeof(AST_Expr));
    res->condition->type = AST_LITERAL_TRUE;

    res->body = malloc(sizeof(AST_Block));
    consume_cur_token(parser, TOKEN_LC, "expect '{' for while body start.");
    compile_block(parser, res->body);
}

void compile_for_stmt(Parser* parser, AST_Block* res) {
    consume_cur_token(parser, TOKEN_ID, "expect id for loop variable name.");
    
    Token* loop_var_name_token = &parser->pre_token;
    ScriptID loop_var_name = {.start = loop_var_name_token->start, .len = loop_var_name_token->len};

    consume_cur_token(parser, TOKEN_IN, "expect 'in' after loop variable name.");

    ScriptID for_seq = {.start = "for@seq", .len = 7};
    ScriptID for_iter = {.start = "for@iter", .len = 8};

    // let for@seq = sequence;
    struct AST_BlockContext* seq_def_context = malloc(sizeof(struct AST_BlockContext));
    seq_def_context->stmt = ({
        AST_Stmt* stmt = malloc(sizeof(AST_Stmt));
        stmt->type = AST_VAR_DEF_STMT;
        AST_VarDef* seq_def = &stmt->stmt.var_def;
        seq_def->name = for_seq;
        seq_def->init_val = compile_expr(parser, BP_LOWEST);
        stmt;
    });
    seq_def_context->next = NULL;

    res->head = seq_def_context;
    res->tail = seq_def_context;

    // let for@iter = null;
    struct AST_BlockContext* iter_def_context = malloc(sizeof(struct AST_BlockContext));
    iter_def_context->stmt = ({
        AST_Stmt* stmt = malloc(sizeof(AST_Stmt));
        stmt->type = AST_VAR_DEF_STMT;
        AST_VarDef* iter_def = &stmt->stmt.var_def;
        iter_def->name = for_iter;
        iter_def->init_val = NULL;
        stmt;
    });
    iter_def_context->next = NULL;

    res->tail->next = iter_def_context;
    res->tail = iter_def_context;

    // while for@iter = for@seq.iterate(for@iter)
    // while
    AST_Stmt* while_loop = malloc(sizeof(AST_Stmt));
    while_loop->type = AST_WHILE_STMT;
    AST_WhileStmt* while_stmt = &while_loop->stmt.while_stmt;
    // for@seq.iterate(for@iter)
    AST_Expr* iterate_call = malloc(sizeof(AST_Expr));
    iterate_call->type = AST_CALL_METHOD_EXPR;
    iterate_call->expr.call_method = (AST_CallMethodExpr) {
        .obj = ({
            AST_Expr* id = malloc(sizeof(AST_Expr));
            id->type = AST_ID_EXPR;
            id->expr.id = for_seq;
            id;
        }),
        .method_name = (ScriptID) {.start = "iterate", .len = 7},
        .argc = 1,
        .args = {
            [0] = ({
                AST_Expr* id = malloc(sizeof(AST_Expr));
                id->type = AST_ID_EXPR;
                id->expr.id = for_iter;
                id;
            }),
        },
    };

    // while ..condition..
    while_stmt->condition = ({
        AST_Expr* loop_condition = malloc(sizeof(AST_Expr));
        loop_condition->type = AST_ASSIGN_EXPR;
        loop_condition->expr.assign = (AST_AssignExpr) {
            .id = for_iter,
            .expr = iterate_call,
        };
        loop_condition;
    });

    // while-body
    consume_cur_token(parser, TOKEN_LC, "expect '{' for for-loop body start.");
    while_stmt->body = malloc(sizeof(AST_Block));
    compile_block(parser, while_stmt->body);
    // 如果循环体为空，那么 while-loop 的体也为空，优化冗余的循环变量赋值和弹出。
    // 有可能实际需要的是迭代这个过程（即调用iterate函数以达到某些目的），因此不会因为循环体为空而忽略整条for语句
    if (while_stmt->body->head != NULL) {
        // 不是空循环体，在代码块的头部添加循环变量的赋值语句
        struct AST_BlockContext* new = malloc(sizeof(struct AST_BlockContext));
        new->next = while_stmt->body->head;
        new->stmt = ({
            AST_Stmt* stmt = malloc(sizeof(AST_Stmt));
            stmt->type = AST_VAR_DEF_STMT;
            AST_VarDef* loop_var_def = &stmt->stmt.var_def;
            loop_var_def->name = loop_var_name;
            loop_var_def->init_val = ({
                AST_Expr* iter_val_call = malloc(sizeof(AST_Expr));
                iter_val_call->type = AST_CALL_METHOD_EXPR;
                iter_val_call->expr.call_method = (AST_CallMethodExpr) {
                    .obj = ({
                        AST_Expr* id = malloc(sizeof(AST_Expr));
                        id->type = AST_ID_EXPR;
                        id->expr.id = for_seq;
                        id;
                    }),
                    .method_name = (ScriptID) {.start = "iterator_value", .len = 14},
                    .argc = 1,
                    .args = {
                        [0] = ({
                            AST_Expr* id = malloc(sizeof(AST_Expr));
                            id->type = AST_ID_EXPR;
                            id->expr.id = for_iter;
                            id;
                        }),
                    },
                };
                iter_val_call;
            });
            stmt;
        });
        while_stmt->body->head = new;
    }

    struct AST_BlockContext* loop_context = malloc(sizeof(struct AST_BlockContext));
    loop_context->stmt = while_loop;
    loop_context->next = NULL;

    res->tail->next = loop_context;
    res->tail = loop_context;
}

AST_Stmt* compile_stmt(Parser* parser) {
    AST_Stmt* res = malloc(sizeof(AST_Stmt));
    
    if (match_token(parser, TOKEN_IF)) {
        res->type = AST_IF_STMT;
        compile_if_stmt(parser, &res->stmt.if_stmt);
    } else if (match_token(parser, TOKEN_WHILE)) {
        res->type = AST_WHILE_STMT;
        compile_while_stmt(parser, &res->stmt.while_stmt);
    } else if (match_token(parser, TOKEN_LOOP)) {
        res->type = AST_WHILE_STMT;
        compile_loop_stmt(parser, &res->stmt.while_stmt);
    } else if (match_token(parser, TOKEN_FOR)) {
        // for 在此处直接去糖
        res->type = AST_BLOCK;
        compile_for_stmt(parser, &res->stmt.block);
    } else if (match_token(parser, TOKEN_BREAK)) {
        res->type = AST_BREAK_STMT;
        consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of break stmt.");
    } else if (match_token(parser, TOKEN_RETURN)) {
        res->type = AST_RETURN_STMT;
        if (match_token(parser, TOKEN_SEMICOLON)) {
            res->stmt.ret_stmt_res = NULL;
        } else {
            res->stmt.ret_stmt_res = compile_expr(parser, BP_LOWEST);
            consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of return stmt.");
        }
    } else if (match_token(parser, TOKEN_CONTINUE)) {
        res->type  = AST_CONTINUE_STMT;
        consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of continue stmt.");
    } else if (match_token(parser, TOKEN_LC)) {
        res->type = AST_BLOCK;
        compile_block(parser, &res->stmt.block);
    } else if (match_token(parser, TOKEN_LET)) {
        res->type = AST_VAR_DEF_STMT;
        compile_var_def(parser, &res->stmt.var_def);
    } else {
        res->type = AST_EXPRESSION_STMT;
        res->stmt.expr_stmt = compile_expr(parser, BP_LOWEST);        
        consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of stmt.");
    }
    
    return res;
}

static AST_Block* compile_body(Parser* parser, bool is_constructor) {
    AST_Block* res = malloc(sizeof(AST_Block));
    compile_block(parser, res);

    // 无论什么原因导致末尾非 return，都插入 return 避免造成错误。
    if (res->head == NULL || res->tail->stmt->type != AST_RETURN_STMT) {
        AST_Stmt* ret_stmt = malloc(sizeof(AST_Stmt));
        ret_stmt->type = AST_RETURN_STMT;
        
        if (is_constructor) {
            ret_stmt->stmt.ret_stmt_res = malloc(sizeof(AST_Expr));
            ret_stmt->stmt.ret_stmt_res->type = AST_SELF_EXPR;
        } else {
            ret_stmt->stmt.ret_stmt_res = NULL;
        }

        struct AST_BlockContext* tail = malloc(sizeof(struct AST_BlockContext));
        tail->stmt = ret_stmt;
        tail->next = NULL;

        if (res->head == NULL) {
            res->head = tail;
        } else {
            res->tail->next = tail;
        }
        res->tail = tail;
    }

    return res;
}

AST_ImportStmt* compile_import_stmt(Parser* parser) {
    // 读入一个token，判断是否是特殊根
    consume_cur_token(parser, TOKEN_ID, "expect module path after import.");
    Token* token = &parser->pre_token;

    enum ImportRootType root_type = DEFAULT_ROOT;
    
    char* path_buf = NULL;
    u32 path_buf_len = 0;

    if (token->len == 3 && strncmp(token->start, "std", 3) == 0) {
        root_type = STD_ROOT;
    } else if (token->len == 3 && strncmp(token->start, "lib", 3) == 0) {
        root_type = LIB_ROOT;
    } else if (token->len == 4 && strncmp(token->start, "home", 4) == 0) {
        path_buf_len = 1;
        path_buf = malloc(2);
        path_buf[0] = '.';
        path_buf[path_buf_len] = '\0';
    } else {
        path_buf_len = token->len;
        path_buf = malloc(path_buf_len + 1);
        memcpy(path_buf, token->start, path_buf_len);
        path_buf[path_buf_len] = '\0';
    }

    while (match_token(parser, TOKEN_DOT)) {
        consume_cur_token(parser, TOKEN_ID, "expect id for muti-level-path.");
        token = &parser->pre_token;
        int old_len = path_buf_len;
        path_buf_len += token->len + 1;
        path_buf = realloc(path_buf, path_buf_len + 1);
        path_buf[old_len] = '/';
        memcpy(&path_buf[old_len + 1], token->start, token->len);
        path_buf[path_buf_len] = '\0';
    }

    if ((root_type != DEFAULT_ROOT && path_buf_len == 0) || (root_type == DEFAULT_ROOT && path_buf_len == 1 && path_buf[0] == '.')) {
        COMPILE_ERROR(parser, "expect id for muti-level-path after root path.");
    }

    AST_ImportStmt* res = malloc(sizeof(AST_ImportStmt));
    res->path_root = root_type;
    res->path = objstring_new(parser->vm, path_buf, path_buf_len);
    res->varc = 0;
    res->vars = NULL;
    res->next = NULL;

    BufferAdd(Value, &parser->vm->ast_obj_root, parser->vm, OBJ_TO_VALUE(res->path));
    free(path_buf);

    if (match_token(parser, TOKEN_SEMICOLON)) {
        return res;
    }

    consume_cur_token(parser, TOKEN_FOR, "miss match token 'for' or ';'");

    ObjString* tmp_buf[IMPORT_VAR_MAX_NUM] = {0};

    do {
        if (res->varc >= IMPORT_VAR_MAX_NUM) {
            COMPILE_ERROR(parser, "A maximum of %d variables can be imported in a single import statement.", IMPORT_VAR_MAX_NUM);
        }
        consume_cur_token(parser, TOKEN_ID, "expect variable name after 'for' in import.");
        tmp_buf[res->varc++] = objstring_new(parser->vm, parser->pre_token.start, parser->pre_token.len);
        BufferAdd(Value, &parser->vm->ast_obj_root, parser->vm, OBJ_TO_VALUE(tmp_buf[res->varc - 1]));
    } while (match_token(parser, TOKEN_COMMA));

    consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of statement.");

    res->vars = malloc(sizeof(ObjString*) * res->varc);
    for (int i = 0; i < res->varc; i++) {
        res->vars[i] = tmp_buf[i];
    }

    return res;
}

AST_FuncDef* compile_func_def(Parser* parser) {
    consume_cur_token(parser, TOKEN_ID, "missing function name.");
    Token* func_name = &parser->pre_token;

    AST_FuncDef* res = malloc(sizeof(AST_FuncDef));
    res->name = (ScriptID) {.start = func_name->start, .len = func_name->len};
    res->argc = 0;
    res->body = NULL;
    res->next = NULL;

    // 型参解析
    consume_cur_token(parser, TOKEN_LP, "expect '(' after function name.");
    if (!match_token(parser, TOKEN_RP)) {
        process_para_list(parser, &res->argc, res->arg_names);
        consume_cur_token(parser, TOKEN_RP, "expect ')' after parameter list.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();

    consume_cur_token(parser, TOKEN_LC, "expect '{' at the beginning of function body.");

    res->body = compile_body(parser, false);
    return res;
}

void compile_var_def(Parser* parser, AST_VarDef* res) {
    // 解析 id
    consume_cur_token(parser, TOKEN_ID, "missing variable name.");
    Token* name_token = &parser->pre_token;

    res->name = (ScriptID) {.start = name_token->start, .len = name_token->len};

    VAR_TYPPING_CHECK();

    // 初始化
    if (match_token(parser, TOKEN_ASSIGN)) {
        res->init_val = compile_expr(parser, BP_LOWEST);
    } else {
        res->init_val = malloc(sizeof(AST_Expr));
        res->init_val->type = AST_LITERAL_EXPR;
        res->init_val->expr.literal.type = VT_NULL;
    }

    consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of variable definition.");
}

AST_ClassDef* compile_class_def(Parser* parser) {
    consume_cur_token(parser, TOKEN_ID, "need a name for class.");
    Token* name = &parser->pre_token;
    
    AST_ClassDef* res = malloc(sizeof(AST_ClassDef));
    res->next = NULL;
    res->fields = NULL;
    res->methods = NULL;

    res->name = objstring_new(parser->vm, name->start, name->len);
    BufferAdd(Value, &parser->vm->ast_obj_root, parser->vm, OBJ_TO_VALUE(res->name));

    res->super = match_token(parser, TOKEN_LT) ? compile_expr(parser, BP_CALL) : NULL; // 继承的父类

    consume_cur_token(parser, TOKEN_LC, "expect '{' in the start of class-define-body.");

    while (!match_token(parser, TOKEN_RC)) { // class body
        bool getter = false;
        bool setter = false;
        if (match_token(parser, TOKEN_GETTER)) {
            getter = true;
            setter = match_token(parser, TOKEN_SETTER);
        } else if (match_token(parser, TOKEN_SETTER)) {
            setter = true;
            getter = match_token(parser, TOKEN_GETTER);
        }
        
        bool is_static = match_token(parser, TOKEN_STATIC);

        if (match_token(parser, TOKEN_LET)) {
            consume_cur_token(parser, TOKEN_ID, "missing field name");
            
            Token* field_name = &parser->pre_token;
            struct _ClassFields* field = malloc(sizeof(struct _ClassFields));
            
            field->is_static = is_static;
            
            field->next = res->fields; // 加在链表头，因此 index 和书写顺序相反
            res->fields = field;

            field->name = (ScriptID) {.start = field_name->start, .len = field_name->len};

            VAR_TYPPING_CHECK();

            if (is_static && match_token(parser, TOKEN_ASSIGN)) {
                field->init_val = compile_expr(parser, BP_LOWEST);
            } else {
                field->init_val = NULL;
            }

            consume_cur_token(parser, TOKEN_SEMICOLON, "expect ';' in the end of field definition.");

            if (getter) {
                struct _ClassMethod* getter_method = malloc(sizeof(struct _ClassMethod));
                getter_method->next = res->methods;
                res->methods = getter_method;
                
                getter_method->is_static = is_static;

                getter_method->type = AST_CLASS_GETTER;
                getter_method->name = field->name;
                getter_method->argc = 0;

                AST_Block* body = malloc(sizeof(AST_Block));
                
                struct AST_BlockContext* context = malloc(sizeof(struct AST_BlockContext));
                
                AST_Stmt* return_stmt = malloc(sizeof(AST_Stmt));
                return_stmt->type = AST_RETURN_STMT;
                
                AST_Expr* result = malloc(sizeof(AST_Expr));
                result->type = AST_ID_EXPR;
                result->expr.id = field->name;

                return_stmt->stmt.ret_stmt_res = result;

                context->stmt = return_stmt;
                context->next = NULL;

                body->head = context;
                body->tail = context;
                
                getter_method->body =  body;
            }

            if (setter) {
                struct _ClassMethod* setter_method = malloc(sizeof(struct _ClassMethod));
                setter_method->next = res->methods;
                res->methods = setter_method;
                
                setter_method->is_static = is_static;

                setter_method->type = AST_CLASS_SETTER;
                setter_method->name = field->name;
                setter_method->argc = 1;

                ScriptID getter_val = (ScriptID) {.start = "getter@val", .len = 10};
                setter_method->arg_names[0] = getter_val;

                AST_Block* body = malloc(sizeof(AST_Block));
                
                struct AST_BlockContext* context = malloc(sizeof(struct AST_BlockContext));
                
                AST_Stmt* return_stmt = malloc(sizeof(AST_Stmt));
                return_stmt->type = AST_RETURN_STMT;
                
                AST_Expr* result = malloc(sizeof(AST_Expr));
                result->type = AST_ASSIGN_EXPR;
                result->expr.assign = (AST_AssignExpr) {
                    .id = field->name,
                    .expr = ({
                        AST_Expr* val = malloc(sizeof(AST_Expr));
                        val->type = AST_ID_EXPR;
                        val->expr.id = getter_val;
                        val;
                    }),
                };

                return_stmt->stmt.ret_stmt_res = result;

                context->stmt = return_stmt;
                context->next = NULL;

                body->head = context;
                body->tail = context;
                
                setter_method->body = body;
            }
        } else if (!(getter || setter) && match_token(parser, TOKEN_NATIVE)) {
            native_annotation(parser);
        } else {
            if (getter || setter) {
                COMPILE_ERROR(parser, "getter and setter can only modify class fields.");
            }

            struct _ClassMethod* method = malloc(sizeof(struct _ClassMethod));
            method->next = res->methods;
            res->methods = method;

            method->is_static = is_static;
            method->type = AST_CLASS_METHOD;

            AST_SymbolBindRule* sign_rule = &AST_Rules[parser->cur_token.type];
            if (sign_rule->method_sign == NULL) {
                char buf[MAX_ID_LEN] = {'\0'};
                u32 buf_len = parser->cur_token.len > MAX_ID_LEN ? MAX_ID_LEN : parser->cur_token.len;
                memcpy(buf, parser->cur_token.start, buf_len);
                buf[buf_len] = '\0';
                COMPILE_ERROR(parser, "token '%s' need signature function.", buf);
            }
            get_next_token(parser); // skip sign_token
            
            // 解析函数签名
            sign_rule->method_sign(parser, method); // 设置好 type name argc arg_names

            // 解析函数体
            consume_cur_token(parser, TOKEN_LC, "expect '{' at the beginning of method body.");
            method->body = compile_body(parser, method->type == AST_CLASS_CONSTRUCTOR);
        }
        
        if (PEEK_TOKEN(parser) == TOKEN_EOF) {
            COMPILE_ERROR(parser, "expect '}' in the end of class-define-body.");
        }
    }

    return res;
}

AST_Prog* compile_prog(Parser* parser) {
    AST_Prog* prog = malloc(sizeof(AST_Prog));
    prog->import_stmt_head = NULL;
    prog->import_stmt_tail = NULL;
    prog->func_def_head = NULL;
    prog->func_def_tail = NULL;
    prog->class_def_head = NULL;
    prog->class_def_tail = NULL;
    prog->toplevel_head = NULL;
    prog->toplevel_tail = NULL;
    
    while (!match_token(parser, TOKEN_EOF)) {
        if (match_token(parser, TOKEN_IMPORT)) {
            AST_ImportStmt* res = compile_import_stmt(parser);
            if (prog->import_stmt_head == NULL) {
                prog->import_stmt_head = res;
            } else {
                prog->import_stmt_tail->next = res;
            }
            prog->import_stmt_tail = res;
        } else if (match_token(parser, TOKEN_FN)) {
            AST_FuncDef* res = compile_func_def(parser);
            if (prog->func_def_head == NULL) {
                prog->func_def_head = res;
            } else {
                prog->func_def_tail->next = res;
            }
            prog->func_def_tail = res;
        } else if (match_token(parser, TOKEN_CLASS)) {
            AST_ClassDef* res = compile_class_def(parser);
            if (prog->class_def_head == NULL) {
                prog->class_def_head = res;
            } else {
                prog->class_def_tail->next = res;
            }
            prog->class_def_tail = res;
        } else {
            struct AST_ToplevelStmt* new = malloc(sizeof(struct AST_ToplevelStmt));
            new->stmt = compile_stmt(parser);
            new->next = NULL;
            if (prog->toplevel_head == NULL) {
                prog->toplevel_head = new;
            } else {
                prog->toplevel_tail->next = new;
            }
            prog->toplevel_tail = new;
        }
    }

    return prog;
}

void destroy_ast_block_not_free(AST_Block* block);
void destroy_ast_block(AST_Block* block);

void destroy_ast_expr(AST_Expr* expr) {
    switch (expr->type) {
        case AST_ARRAY_LITERAL:
            while (expr->expr.array_literal.head != NULL) {
                struct AST_ArrayItem* tmp = expr->expr.array_literal.head;
                expr->expr.array_literal.head = expr->expr.array_literal.head->next;
                destroy_ast_expr(tmp->item);
                free(tmp);
            }
            break;
        
        case AST_MAP_LITERAL:
            while (expr->expr.map_literal.entrys != NULL) {
                struct AST_MapEntry* tmp = expr->expr.map_literal.entrys;
                expr->expr.map_literal.entrys = expr->expr.map_literal.entrys->next;
                destroy_ast_expr(tmp->key);
                destroy_ast_expr(tmp->val);
                free(tmp);
            }
            break;
        
        case AST_ASSIGN_EXPR:
            if (expr->expr.assign.expr != NULL) {
                destroy_ast_expr(expr->expr.assign.expr);
            }
            break;
        
        case AST_INFIX_EXPR:
            destroy_ast_expr(expr->expr.infix.l);
            destroy_ast_expr(expr->expr.infix.r);
            break;
        
        case AST_PREFIX_EXPR:
            destroy_ast_expr(expr->expr.prefix.expr);
            break;

        case AST_LOGICAL_AND:
        case AST_LOGICAL_OR:
            destroy_ast_expr(expr->expr.logical_cmp.l);
            destroy_ast_expr(expr->expr.logical_cmp.r);
            break;

        case AST_CALL_METHOD_EXPR:
            destroy_ast_expr(expr->expr.call_method.obj);
            break;
        case AST_GETTER_EXPR:
            destroy_ast_expr(expr->expr.getter.obj);
            break;
        case AST_SETTER_EXPR:
            destroy_ast_expr(expr->expr.setter.obj);
            if (expr->expr.setter.val != NULL) {
                destroy_ast_expr(expr->expr.setter.val);
            }
            break;
        case AST_SUBSCRIPT_SETTER_EXPR:
        case AST_SUBSCRIPT_EXPR:
            destroy_ast_expr(expr->expr.subscript.obj);
            break;

        case AST_SUPER_EXPR:
            switch (expr->expr.super_call.type) {
                case SUPER_SETTER:
                    if (expr->expr.super_call.call_method.setter_value != NULL) {
                        destroy_ast_expr(expr->expr.super_call.call_method.setter_value);
                    }
                    break;
                case SUPER_SUBSCRIPT:
                case SUPER_SUBSCRIPT_SETTER:
                case SUPER_GETTER:
                case SUPER_METHOD:
                    // do noting
                    break;
            }
            break;
        
        case AST_CONDITION_EXPR:
            destroy_ast_expr(expr->expr.condition_expr.condition);
            destroy_ast_expr(expr->expr.condition_expr.true_val);
            destroy_ast_expr(expr->expr.condition_expr.false_val);
            break;

        case AST_CLOSURE_EXPR:
            destroy_ast_block(expr->expr.closure.body);
            break;

        case AST_SELF_EXPR:
        case AST_ID_EXPR:
        case AST_LITERAL_EXPR:
        case AST_ID_CALL_EXPR:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            // do noting
            break;
    }

    free(expr);
}

void destroy_ast_if_stmt(AST_IfStmt* if_stmt) {
    destroy_ast_expr(if_stmt->condition);
    destroy_ast_block(if_stmt->then_block);
    switch (if_stmt->else_type) {
        case ELSE_IF:
            destroy_ast_if_stmt(if_stmt->else_branch.else_if);
            free(if_stmt->else_branch.else_if);
            break;
        case ELSE_BLOCK:
            destroy_ast_block(if_stmt->else_branch.block);
            break;
        case ELSE_NONE:
            // do noting
            break;
    }
}

void destroy_ast_stmt(AST_Stmt* stmt) {
    switch (stmt->type) {
        case AST_IF_STMT:
            destroy_ast_if_stmt(&stmt->stmt.if_stmt);
            break;
        case AST_WHILE_STMT:
            destroy_ast_expr(stmt->stmt.while_stmt.condition);
            destroy_ast_block(stmt->stmt.while_stmt.body);
            break;
        case AST_RETURN_STMT:
            if (stmt->stmt.ret_stmt_res != NULL) {
                destroy_ast_expr(stmt->stmt.ret_stmt_res);
            }
            break;
        case AST_BLOCK:
            destroy_ast_block_not_free(&stmt->stmt.block);
            break;
        case AST_EXPRESSION_STMT:
            destroy_ast_expr(stmt->stmt.expr_stmt);
            break;
        case AST_VAR_DEF_STMT:
            if (stmt->stmt.var_def.init_val != NULL) {
                destroy_ast_expr(stmt->stmt.var_def.init_val);
            }
            break;
        case AST_CONTINUE_STMT:
        case AST_BREAK_STMT:
            // do noting
            break;
    }

    free(stmt);
}

inline void destroy_ast_block_not_free(AST_Block* block) {
    while (block->head != NULL) {
        struct AST_BlockContext* tmp = block->head;
        block->head = block->head->next;
        destroy_ast_stmt(tmp->stmt);
        free(tmp);
    }
}

void destroy_ast_block(AST_Block* block) {
    destroy_ast_block_not_free(block);
    free(block);
}

void destroy_ast_import_stmt(AST_ImportStmt* import_stmt) {
    free(import_stmt->vars);

    if (import_stmt->next != NULL) {
        destroy_ast_import_stmt(import_stmt->next);
    }

    free(import_stmt);
}

void destroy_ast_func_def(AST_FuncDef* func_def) {
    destroy_ast_block(func_def->body);

    if (func_def->next != NULL) {
        destroy_ast_func_def(func_def->next);
    }

    free(func_def);
}

void destroy_class_def(AST_ClassDef* class_def) {
    if (class_def->super != NULL) {
        destroy_ast_expr(class_def->super);
    }

    while (class_def->fields != NULL) {
        struct _ClassFields* tmp = class_def->fields;
        class_def->fields = class_def->fields->next;

        if (tmp->init_val != NULL) {
            destroy_ast_expr(tmp->init_val);
        }
        
        free(tmp);
    }

    while (class_def->methods != NULL) {
        struct _ClassMethod* tmp = class_def->methods;
        class_def->methods = class_def->methods->next;

        destroy_ast_block(tmp->body);

        free(tmp);
    }

    if (class_def->next != NULL) {
        destroy_class_def(class_def->next);
    }
}

void destroy_ast_prog(VM* vm, AST_Prog* prog) {
    if (prog->import_stmt_head != NULL) {
        destroy_ast_import_stmt(prog->import_stmt_head);
    }

    if (prog->func_def_head != NULL) {
        destroy_ast_func_def(prog->func_def_head);
    }

    if (prog->class_def_head != NULL) {
        destroy_class_def(prog->class_def_head);
    }

    while (prog->toplevel_head != NULL) {
        struct AST_ToplevelStmt* tmp = prog->toplevel_head;
        prog->toplevel_head = tmp->next;
        destroy_ast_stmt(tmp->stmt);
        free(tmp);
    }

    free(prog);
    BufferClear(Value, &vm->ast_obj_root, vm);
}
