#include "compiler.h"
#include "one_pass_compiler.h"
#include "common.h"
#include "meta_obj.h"
#include "obj_fn.h"
#include "obj_string.h"
#include "parser.h"
#include "core.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include "class.h"
#include "vm.h"

typedef struct {
    CompileUnitPubStruct pub;
    Parser* parser;
} CompileUnit;

typedef enum {
    BP_NONE,
    BP_LOWEST,
    BP_ASSIGN,
    BP_CONDITION,
    BP_LOGICAL_OR,
    BP_LOGICAL_AND,
    BP_EQ,
    BP_IS,
    BP_CMP,
    BP_BIT_OR,
    BP_BIT_AND,
    BP_BIT_SHIFT,
    BP_RANGE,
    BP_TERM,
    BP_FACTOR,
    BP_UNARY,
    BP_CALL,
    BP_HIGHEST,
} BindPower;

static void expression(CompileUnit* cu, BindPower rbp);

typedef void (*DenotationFunc)(CompileUnit* cu, bool can_assign);
typedef void (*MethodSignatureFunc)(CompileUnit* cu, Signature* sign); // 签名函数

typedef struct {
    const char* id;
    BindPower lbp;
    
    DenotationFunc nud;
    DenotationFunc led;
    
    MethodSignatureFunc method_sign;
} SymbolBindRule;

static void emit_load_or_store_variable(CompileUnit* cu, bool can_assign, Variable var) {
    if (can_assign && match_token(cu->parser, TOKEN_ASSIGN)) {
        expression(cu, BP_LOWEST); // = 右值
        emit_store_variable(&cu->pub, var);
        return;
    }
    emit_load_variable(&cu->pub, var);
}

static void literal(CompileUnit* cu, bool can_assign);
static void mix_method_signature(CompileUnit* cu, Signature* sign);
static void id(CompileUnit* cu, bool can_assign);
static void id_method_signature(CompileUnit* cu, Signature* sign);
static void string_interpolation(CompileUnit* cu, bool can_assign);
static void null(CompileUnit* cu, bool can_assign);
static void boolean(CompileUnit* cu, bool can_assign);
static void super(CompileUnit* cu, bool can_assign);
static void self(CompileUnit* cu, bool can_assign);
static void infix_method_signature(CompileUnit* cu, Signature* sign);
static void infix_operator(CompileUnit* cu, bool can_assign);
static void parentheses(CompileUnit* cu, bool can_assign);
static void list_literal(CompileUnit* cu, bool can_assign);
static void subscript(CompileUnit* cu, bool can_assign);
static void subscript_method_signature(CompileUnit* cu, Signature* sign);
static void call_entry(CompileUnit* cu, bool can_assign);
static void map_literal(CompileUnit* cu, bool can_assign);
static void logical_and(CompileUnit* cu, bool can_assign);
static void logical_or(CompileUnit* cu, bool can_assign);
static void condition(CompileUnit* cu, bool can_assign);
static void unary_method_signature(CompileUnit* cu, Signature* sign);
static void unary_operator(CompileUnit* cu, bool can_assign);
static void closure_expr(CompileUnit* cu, bool can_assign);

#define PREFIX_SYMBOL(nud)      {NULL,  BP_NONE,    nud,            NULL,           NULL}
#define INFIX_SYMBOL(lbp, led)  {NULL,  lbp,        NULL,           led,            NULL}
#define INFIX_OPERATOR(id, lbp) {id,    lbp,        NULL,           infix_operator, infix_method_signature}
#define MIX_OPERATOR(id)        {id,    BP_TERM,    unary_operator,  infix_operator, mix_method_signature}
#define UNUSED_RULE             {NULL,  BP_NONE,    NULL,           NULL,           NULL}
#define PREFIX_OPERATOR(id)     {id,    BP_NONE,    unary_operator, NULL,           unary_method_signature}

SymbolBindRule Rules[]      = {
    [TOKEN_UNKNOWN]         = UNUSED_RULE,
    [TOKEN_U8]              = PREFIX_SYMBOL(literal),
    [TOKEN_U32]             = PREFIX_SYMBOL(literal),
    [TOKEN_I32]             = PREFIX_SYMBOL(literal),
    [TOKEN_F64]             = PREFIX_SYMBOL(literal),
    [TOKEN_STRING]          = PREFIX_SYMBOL(literal),
    [TOKEN_ID]              = {NULL, BP_NONE, id, NULL, id_method_signature},
    [TOKEN_INTERPOLATION]   = PREFIX_SYMBOL(string_interpolation),
    [TOKEN_TRUE]            = PREFIX_SYMBOL(boolean),
    [TOKEN_FALSE]           = PREFIX_SYMBOL(boolean),
    [TOKEN_NULL]            = PREFIX_SYMBOL(null),
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
    [TOKEN_QUESTION]        = INFIX_SYMBOL(BP_ASSIGN, condition),
    [TOKEN_FN]              = PREFIX_SYMBOL(closure_expr),
};

static void expression(CompileUnit* cu, BindPower rbp) {
    bool can_assign = rbp < BP_ASSIGN;

    // 解析左操作数
    DenotationFunc nud = Rules[cu->parser->cur_token.type].nud;
    if (nud == NULL) {
        printf("nud [%d] != NULL.\n", cu->parser->cur_token.type);
    }
    ASSERT(nud != NULL, "token nud is NULL.");
    get_next_token(cu->parser);
    nud(cu, can_assign);

    while (rbp < Rules[cu->parser->cur_token.type].lbp) {
        DenotationFunc led = Rules[cu->parser->cur_token.type].led;
        get_next_token(cu->parser);
        led(cu, can_assign);
    }
}

static void infix_operator(CompileUnit* cu, bool can_assign) {
    SymbolBindRule* rule = &Rules[cu->parser->pre_token.type];
    BindPower rbp = rule->lbp;
    
    expression(cu, rbp);

    Signature sign = {SIGN_METHOD, rule->id, strlen(rule->id), 1};
    emit_call_by_signature(&cu->pub, &sign, OPCODE_CALL0);
}

static void unary_operator(CompileUnit* cu, bool can_assign) {
    SymbolBindRule* rule = &Rules[cu->parser->pre_token.type];

    expression(cu, BP_UNARY);

    emit_call(&cu->pub, 0, rule->id, 1);
}

static void literal(CompileUnit* cu, bool can_assign) {
    emit_load_constant(&cu->pub, cu->parser->pre_token.value);
}

static bool has_pending_gt = false; // 为了解决 '>>' 闭合 <> 的问题。
static void type_annotation(CompileUnit* cu) {
    // 类型注释按规则解析后不保存任何信息，只做注释用
    // String | List<String> | Map<String, int> | List<Map<String, int>> | Tuple<int, int, int> | Fn<(T) -> K> | Fn<() -> None>?
    // 类型均以id起始，后可以接可嵌套的‘<>’，‘<>’中至少有一个id，id之间可以使用‘,’但不能以‘,’结尾。允许使用‘|’表示或关系
    // 所有类型后均可添加?表示可空类型
    // callable类型内部以‘(’起始，返回值标注‘->’必须，参数列表中不可使用可变参数标注‘...’（鉴于语言本身不支持可变参数）

    consume_cur_token(cu->parser, TOKEN_ID, "typping must start by id.");

    if (match_token(cu->parser, TOKEN_LT)) {
        if (match_token(cu->parser, TOKEN_LP)) {
            // callable标注
            // 参数列表解析
            if (!match_token(cu->parser, TOKEN_RP)) {
                do {
                    type_annotation(cu);
                } while (match_token(cu->parser, TOKEN_COMMA));

                consume_cur_token(cu->parser, TOKEN_RP, "uncloused callable typping parameter.");
            }

            consume_cur_token(cu->parser, TOKEN_SUB, "expect callable typping result.");
            consume_cur_token(cu->parser, TOKEN_GT, "expect callable typping result.");

            type_annotation(cu); // result type
        } else {
            // 一般标注
            do {
                type_annotation(cu);
            } while (match_token(cu->parser, TOKEN_COMMA));
        }

        if (has_pending_gt) {
            has_pending_gt = false;
        } else if (match_token(cu->parser, TOKEN_BIT_SR)) {
            has_pending_gt = true;
        } else {
            consume_cur_token(cu->parser, TOKEN_GT, "uncloused typping <>.");
        }
    }

    match_token(cu->parser, TOKEN_QUESTION);

    if (match_token(cu->parser, TOKEN_BIT_OR)) {
        type_annotation(cu);
    }
}
#define FUNCTION_RESULT_TYPPING_CHECK() \
    if (match_token(cu->parser, TOKEN_SUB)) { \
        consume_cur_token(cu->parser, TOKEN_GT, "expect '->' for result typping."); \
        type_annotation(cu); \
    }

static void unary_method_signature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_GETTER;
    FUNCTION_RESULT_TYPPING_CHECK();
}

static void infix_method_signature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_METHOD;
    sign->argc = 1;
    
    consume_cur_token(cu->parser, TOKEN_LP, "expect '(' after infix operator.");
    
    consume_cur_token(cu->parser, TOKEN_ID, "expect var name.");
    declare_variable(&cu->pub, cu->parser->cur_token.start, cu->parser->cur_token.len);

    // typping
    if (match_token(cu->parser, TOKEN_COLON)) {
        type_annotation(cu);
    }

    consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after var name.");
    
    FUNCTION_RESULT_TYPPING_CHECK();
}

static void mix_method_signature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_GETTER;

    if (match_token(cu->parser, TOKEN_LP)) {
        sign->type = SIGN_METHOD;
        sign->argc = 1;
        
        consume_cur_token(cu->parser, TOKEN_ID, "expect var name.");
        declare_variable(&cu->pub, cu->parser->cur_token.start, cu->parser->cur_token.len);

        // typping
        if (match_token(cu->parser, TOKEN_COLON)) {
            type_annotation(cu);
        }

        consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after var name.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();
}

static void process_arg_list(CompileUnit* cu, Signature* sign) {
    ASSERT(
        cu->parser->cur_token.type != TOKEN_RP && cu->parser->cur_token.type != TOKEN_RB,
        "empty argument list."
    );

    do {
        if (++(sign->argc) > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->parser, "the max number of argument is %d.", MAX_ARG_NUM);
        }

        expression(cu, BP_LOWEST);
    } while (match_token(cu->parser, TOKEN_COMMA));
}

static void process_para_list(CompileUnit* cu, Signature* sign) {
    ASSERT(
        cu->parser->cur_token.type != TOKEN_RP && cu->parser->cur_token.type != TOKEN_RB,
        "empty param list."
    );

    do {
        if (++(sign->argc) > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->parser, "the max number of argument is %d.", MAX_ARG_NUM);
        }

        consume_cur_token(cu->parser, TOKEN_ID, "expect param name.");
        declare_variable(&cu->pub, cu->parser->pre_token.start, cu->parser->pre_token.len);

        if (match_token(cu->parser, TOKEN_COLON)) {
            type_annotation(cu); // 类型注释
        }
    } while (match_token(cu->parser, TOKEN_COMMA));
}

static bool try_setter(CompileUnit* cu, Signature* sign) {
    if (!match_token(cu->parser, TOKEN_ASSIGN)) {
        return false;
    }

    sign->type = sign->type == SIGN_SUBSCRIPT ? SIGN_SUBSCRIPT_SETTER : SIGN_SETTER;

    consume_cur_token(cu->parser, TOKEN_LP, "expect '(' after '='.");

    consume_cur_token(cu->parser, TOKEN_ID, "expect ID for parameter");
    sign->argc++;

    declare_variable(&cu->pub, cu->parser->pre_token.start, cu->parser->pre_token.len);

    // typping
    if (match_token(cu->parser, TOKEN_COLON)) {
        type_annotation(cu);
    }

    consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after argument list");

    return true;
}

static void id_method_signature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_GETTER;

    if (sign->len == 3 && memcmp(sign->name, "new", 3) == 0) {
        if (match_token(cu->parser, TOKEN_ASSIGN)) {
            COMPILE_ERROR(cu->parser, "constructor shouldn't be setter.");
        }

        if (!match_token(cu->parser, TOKEN_LP)) {
            COMPILE_ERROR(cu->parser, "constructor must be method.");
        }

        sign->type = SIGN_CONSTRUCT;

        if (match_token(cu->parser, TOKEN_RP)) {
            return; // 无参数，直接返回
        }
    } else {
        if (try_setter(cu, sign)) {
            FUNCTION_RESULT_TYPPING_CHECK();
            return; // 是setter，已正确设置，直接返回
        }

        if (!match_token(cu->parser, TOKEN_LP)) {
            // typping
            FUNCTION_RESULT_TYPPING_CHECK();
            return; // 是getter，已正确设置，直接返回
        }

        sign->type = SIGN_METHOD;

        if (match_token(cu->parser, TOKEN_RP)) {
            FUNCTION_RESULT_TYPPING_CHECK();
            return; // 无参数，直接返回
        }
    }

    process_para_list(cu, sign);
    consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after parameter list.");

    FUNCTION_RESULT_TYPPING_CHECK();
}

static void compile_prog(CompileUnit* cu);

static void compile_block(CompileUnit* cu) {
    while (!match_token(cu->parser, TOKEN_RC)) {
        if (PEEK_TOKEN(cu->parser) == TOKEN_EOF) {
            COMPILE_ERROR(cu->parser, "expect '}' at the end of block.");
        }
        compile_prog(cu);
    }
}

static void compile_body(CompileUnit* cu, bool is_construct) {
    compile_block(cu);

    // 填充默认返回值
    if (is_construct) {
        // 是构造函数，返回self。
        write_opcode_byte_operand(&cu->pub, OPCODE_LOAD_LOCAL_VAR, 0);
    } else {
        // 不是构造函数，默认返回null。
        write_opcode(&cu->pub, OPCODE_PUSH_NULL);
    }
    
    // 填充默认的return语句
    write_opcode(&cu->pub, OPCODE_RETURN);
}

static void emit_getter_method_call(CompileUnit* cu, Signature* sign, OpCode opcode) {
    Signature new_sign = {
        .type = SIGN_GETTER,
        .name = sign->name,
        .len  = sign->len,
        .argc = 0,
    };

    if (match_token(cu->parser, TOKEN_LP)) {
        new_sign.type = SIGN_METHOD;

        if (!match_token(cu->parser, TOKEN_RP)) {
            process_arg_list(cu, &new_sign);
            consume_cur_token(
                cu->parser, TOKEN_RP,
                "expect ')' after argument list."
            );
        }
    }

    #ifdef BLOCK_ARG_ON
    // 解析块参数
    if (match_token(cu->parser, TOKEN_LC)) {
        new_sign.argc++;
        new_sign.type = SIGN_METHOD;
        
        // 解析块参数
        CompileUnit fncu;
        compileunit_init(cu->parser, &fncu, cu, false);

        Signature temp_func_sign = {
            .type = SIGN_METHOD,
            .name = "",
            .len = 0,
            .argc = 0,
        };

        if (match_token(cu->parser, TOKEN_BIT_OR)) {
            process_para_list(cu, &temp_func_sign);
            consume_cur_token(
                cu->parser, TOKEN_BIT_OR,
                "expect '|' after argument list."
            );
        }

        fncu.fn->argc = temp_func_sign.argc;

        compile_body(&fncu, false);

        #if DEBUG
            char fn_name[MAX_SIGN_LEN + 10] = {'\0'};
            u32 len = sign2string(&new_sign, fn_name);
            memmove(fn_name + len, "@block_arg", 10);
            end_compile_unit(&fncu, fn_name, len + 10);
        #else
            end_compile_unit(cu);
        #endif
    }
    #endif

    if (sign->type == SIGN_CONSTRUCT) {
        if (new_sign.type != SIGN_METHOD) {
            COMPILE_ERROR(cu->parser, "the form of supercall is super() or super(...)");
        }
        new_sign.type = SIGN_CONSTRUCT;
    }

    emit_call_by_signature(&cu->pub, &new_sign, opcode);
}

static void emit_method_call(CompileUnit* cu, const char* name, u32 len, OpCode opcode, bool can_assign) {
    Signature sign = {
        .type = SIGN_GETTER,
        .name = name,
        .len  = len,
    };

    if (match_token(cu->parser, TOKEN_ASSIGN) && can_assign) {
        sign.type = SIGN_SETTER;
        sign.argc = 1;
        
        expression(cu, BP_LOWEST);
        
        emit_call_by_signature(&cu->pub, &sign, opcode);
        return;
    }

    emit_getter_method_call(cu, &sign, opcode);
}

static bool is_local_name(const char* name) {
    return (*name >= 'a' && *name <= 'z'); // 小写字母开头，为局部变量名
}

static void id(CompileUnit* cu, bool can_assign) {
    Token name = cu->parser->pre_token;
    ClassBookKeep* class_bk = get_enclosing_classbk(&cu->pub);

    // 标识符处理顺序：
    // func call -> local var & upvalue -> instance field -> static field -> getter -> module var

    // function call
    // class外的作用域(此处实际只检查toplevel) && id后有括号
    // if (cu->pub.enclosing_unit == NULL && match_token(cu->parser, TOKEN_LP)) {
    //     char id[MAX_ID_LEN + 4] = {'\0'};
    //     memmove(id, "Fn@", 3);
    //     memmove(id + 3, name.start, name.len);

    //     Variable var = {
    //         .scope_type = VAR_SCOPE_MODULE,
    //         .index = get_index_from_symbol_table(
    //             &cu->pub.cur_module->module_var_name,
    //             id, strlen(id)
    //         ),
    //     };

    //     if (var.index == -1) {
    //         memmove(id, name.start, name.len);
    //         id[name.len] = '\0';

    //         COMPILE_ERROR(cu->parser, "Undefined function '%s'.", id);
    //     }

    //     // 加载函数闭包到栈
    //     emit_load_variable(&cu->pub, var);

    //     // fn.call
    //     Signature call_sign = {
    //         .type = SIGN_METHOD,
    //         .name = "call",
    //         .len = 4,
    //         .argc = 0,
    //     };

    //     // 解析参数
    //     if (!match_token(cu->parser, TOKEN_RP)) {
    //         process_arg_list(cu, &call_sign);
    //         consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after argument list.");
    //     }

    //     // call函数调用指令
    //     emit_call_by_signature(&cu->pub, &call_sign, OPCODE_CALL0);

    //     return;
    // }

    // 非函数调用，尝试解析为局部变量与upvalue。
    Variable var = get_var_from_local_or_upvalue(&cu->pub, name.start, name.len);
    if (var.index != -1) {
        emit_load_or_store_variable(cu, can_assign, var);
        return;
    }

    // 非局部变量或upvalue，解析实例域
    if (class_bk != NULL) {
        int field_index = get_index_from_symbol_table(&class_bk->fields, name.start, name.len);
        if (field_index != -1) {
            bool is_read = true;
            
            if (can_assign && match_token(cu->parser, TOKEN_ASSIGN)) {
                // 不是变量的读取，解析=右值
                is_read = false;
                expression(cu, BP_LOWEST);
            }

            if (cu->pub.enclosing_unit != NULL) {
                // 方法中直接获取
                write_opcode_byte_operand(&cu->pub, is_read ? OPCODE_LOAD_SELF_FIELD : OPCODE_STORE_SELF_FIELD, field_index);
            } else {
                // 不在方法中，加载self后再获取
                emit_load_self(&cu->pub);
                write_opcode_byte_operand(&cu->pub, is_read ? OPCODE_LOAD_FIELD : OPCODE_STORE_FIELD, field_index);
            }

            return;
        }

        // 非实例域，尝试按静态域解析
        char* static_field_id = ALLOCATE_ARRAY(cu->parser->vm, char, MAX_ID_LEN);
        memset(static_field_id, 0, MAX_ID_LEN);
        u32 static_field_id_len = 0;

        char* cls_name = class_bk->name->val.start;
        u32 cls_name_len = class_bk->name->val.len;

        const char* tk_name = name.start;
        u32 tk_name_len = name.len;

        // ClsClass@static_var_name
        memmove(static_field_id, "Cls", 3);
        memmove(static_field_id + 3, cls_name, cls_name_len);
        memmove(static_field_id + 3 + cls_name_len, "@", 1);
        memmove(static_field_id + 4 + cls_name_len, tk_name, tk_name_len);
        
        static_field_id_len = strlen(static_field_id);

        var = get_var_from_local_or_upvalue(&cu->pub, static_field_id, static_field_id_len);
        
        DEALLOCATE_ARRAY(cu->parser->vm, static_field_id, MAX_ID_LEN);

        if (var.index != -1) {
            emit_load_or_store_variable(cu, can_assign, var);
            return;
        }

        // 类中方法调用
        // 若id开头为小写字母，那么则将其作为方法调用进行编译。
        // 如果存在开头大写字母的方法，则需要使用self.MethodName()的形式调用
        if (is_local_name(name.start)) {
            emit_load_self(&cu->pub);
            emit_method_call(cu, name.start, name.len, OPCODE_CALL0, can_assign);
            return;
        }
    }

    // 作为模块变量处理
    var.scope_type = VAR_SCOPE_MODULE;
    var.index = get_index_from_symbol_table(&cu->pub.cur_module->module_var_name, name.start, name.len);
    if (var.index == -1) {
        char fn_name[MAX_ID_LEN] = {0};
        memmove(fn_name, "Fn@", 3);
        memmove(fn_name + 3, name.start, name.len);

        u32 fn_name_len = strlen(fn_name);

        var.index = get_index_from_symbol_table(
            &cu->pub.cur_module->module_var_name,
            fn_name, fn_name_len
        );

        // 在函数内部()调用函数的解决方案
        if (match_token(cu->parser, TOKEN_LP)) {
            if (var.index == -1) {
                // 调用当前未定义函数
                var.index = declare_module_var(
                    cu->parser->vm, cu->pub.cur_module,
                    fn_name, fn_name_len, I32_TO_VALUE(name.line)
                );
            }

            // 加载函数闭包到栈
            emit_load_variable(&cu->pub, var);

            // fn.call
            Signature call_sign = {
                .type = SIGN_METHOD,
                .name = "call",
                .len = 4,
                .argc = 0,
            };

            // 解析参数
            if (!match_token(cu->parser, TOKEN_RP)) {
                process_arg_list(cu, &call_sign);
                consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after argument list.");
            }

            // call函数调用指令
            emit_call_by_signature(&cu->pub, &call_sign, OPCODE_CALL0);

            return;
        }

        // 不是已定义的模块变量，可能在后文中有定义，预先填入行号作为标记
        if (var.index == -1) {
            var.index = declare_module_var(
                cu->parser->vm, cu->pub.cur_module,
                name.start, name.len, I32_TO_VALUE(name.line)
            );
        }
    }

    emit_load_or_store_variable(cu, can_assign, var);
}

static void string_interpolation(CompileUnit* cu, bool can_assign) {
    // "a %(b + c) d %(e) f" => ["a ", b + c, " d ", e, " f"].join()

    emit_load_module_var(&cu->pub, "List");
    emit_call(&cu->pub, 0, "new()", 5);

    do {
        if (((ObjString*)cu->parser->pre_token.value.header)->val.len != 0) { // 当其为非空字符串时添加
            literal(cu, false); // 解析字符串
            // List.core_append(_: any) -> List 用于编译器内部构造列表。
            emit_call(&cu->pub, 1, "core_append(_)", 14);
        }

        expression(cu, BP_LOWEST); // 解析内嵌表达式
        emit_call(&cu->pub, 1, "core_append(_)", 14); // 将内嵌表达式的值加入列表
    } while (match_token(cu->parser, TOKEN_INTERPOLATION));

    consume_cur_token(
        cu->parser, TOKEN_STRING,
        "expect string at the end of interpolatation."
    );

    // 结尾的TOKEN_STRING
    if (((ObjString*)cu->parser->pre_token.value.header)->val.len != 0) { // 当其为非空字符串时添加
        literal(cu, false);
        emit_call(&cu->pub, 1, "core_append(_)", 14);
    }

    emit_call(&cu->pub, 0, "join()", 6);
}

static void boolean(CompileUnit* cu, bool can_assign) {
    OpCode opcode = cu->parser->pre_token.type == TOKEN_TRUE ? OPCODE_PUSH_TRUE : OPCODE_PUSH_FALSE;
    write_opcode(&cu->pub, opcode);
}

static void null(CompileUnit* cu, bool can_assign) {
    write_opcode(&cu->pub, OPCODE_PUSH_NULL);
}

static void self(CompileUnit* cu, bool can_assign) {
    if (get_enclosing_classbk(&cu->pub) == NULL) {
        COMPILE_ERROR(cu->parser, "'self' must be inside a class method.");
    }
    emit_load_self(&cu->pub);
}

static void super(CompileUnit* cu, bool can_assign) {
    ClassBookKeep* enclosing_classbk = get_enclosing_classbk(&cu->pub);
    if (enclosing_classbk == NULL) {
        COMPILE_ERROR(cu->parser, "can't invoke super outside a class method.");
    }

    emit_load_self(&cu->pub); // 函数调用的argv[0]永远指向self

    if (match_token(cu->parser, TOKEN_DOT)) {
        // super.method()
        consume_cur_token(cu->parser, TOKEN_ID, "expect name after '.'.");
        Token* id = &cu->parser->pre_token;
        emit_method_call(cu, id->start, id->len, OPCODE_SUPER0, can_assign);
    } else {
        // super() 调用super的同名方法
        emit_getter_method_call(cu, enclosing_classbk->signature, OPCODE_SUPER0);
    }
}

// (.nud
static void parentheses(CompileUnit* cu, bool can_assign) {
    expression(cu, BP_LOWEST);
    consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after expression.");
}

// [.nud
static void list_literal(CompileUnit* cu, bool can_assign) {
    // ["String", 20] => List.new().core_append("String").core_append(20)

    emit_load_module_var(&cu->pub, "List");
    emit_call(&cu->pub, 0, "new()", 5);

    do {
        if (PEEK_TOKEN(cu->parser) == TOKEN_RB) {
            break;
        }
        
        expression(cu, BP_LOWEST);
        emit_call(&cu->pub, 1, "core_append(_)", 14);
    } while (match_token(cu->parser, TOKEN_COMMA));

    consume_cur_token(cu->parser, TOKEN_RB, "expect ']' after list element.");
}

// [.led
static void subscript(CompileUnit* cu, bool can_assign) {
    if (match_token(cu->parser, TOKEN_RB)) {
        COMPILE_ERROR(cu->parser, "need argument in the '[]'.");
    }

    Signature sign = {SIGN_SUBSCRIPT, "", 0, 0};
    process_arg_list(cu, &sign);
    consume_cur_token(cu->parser, TOKEN_RB, "expect ']' after argument list.");

    if (can_assign && match_token(cu->parser, TOKEN_ASSIGN)) {
        sign.type = SIGN_SUBSCRIPT_SETTER;

        if (++sign.argc > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->parser, "the max number of argument is %d.", MAX_ARG_NUM);
        }

        expression(cu, BP_LOWEST);
    }

    emit_call_by_signature(&cu->pub, &sign, OPCODE_CALL0);
}

static void subscript_method_signature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_SUBSCRIPT;
    sign->len = 0;
    process_para_list(cu, sign);
    consume_cur_token(cu->parser, TOKEN_RB, "expect ']' after index list.");
    try_setter(cu, sign);
    FUNCTION_RESULT_TYPPING_CHECK();
}

// '.'.led
static void call_entry(CompileUnit* cu, bool can_assign) {
    consume_cur_token(cu->parser, TOKEN_ID, "expect method name.");
    Token* id = &cu->parser->pre_token;
    emit_method_call(cu, id->start, id->len, OPCODE_CALL0, can_assign);
}

static void map_literal(CompileUnit* cu, bool can_assign) {
    emit_load_module_var(&cu->pub, "Map");
    emit_call(&cu->pub, 0, "new()", 5);

    do {
        if (PEEK_TOKEN(cu->parser) == TOKEN_RC) {
            break;
        }

        expression(cu, BP_UNARY); // key

        consume_cur_token(cu->parser, TOKEN_COLON, "expect ':' after key.");

        expression(cu, BP_LOWEST); // value

        emit_call(&cu->pub, 2, "core_insert(_,_)", 16);
    } while (match_token(cu->parser, TOKEN_COMMA));

    consume_cur_token(cu->parser, TOKEN_RC, "map literal should end with '}'.");
}

static void logical_or(CompileUnit* cu, bool can_assign) {
    // OPCODE_OR <next_stmt_addr>
    u32 placeholder = emit_instr_with_placeholder(&cu->pub, OPCODE_OR);
    expression(cu, BP_LOGICAL_OR); // 右操作数
    patch_placeholder(&cu->pub, placeholder);
}

static void logical_and(CompileUnit* cu, bool can_assign) {
    // OPCODE_AND <next_stmt_addr>
    u32 placeholder = emit_instr_with_placeholder(&cu->pub, OPCODE_AND);
    expression(cu, BP_LOGICAL_AND); // 右操作数
    patch_placeholder(&cu->pub, placeholder);
}

static void condition(CompileUnit* cu, bool can_assign) {
    u32 false_branch_start = emit_instr_with_placeholder(&cu->pub, OPCODE_JMP_IF_FALSE);
    expression(cu, BP_LOWEST); // true branch
    consume_cur_token(cu->parser, TOKEN_COLON, "expect ':' after true branch!");
    u32 expr_end = emit_instr_with_placeholder(&cu->pub, OPCODE_JMP);
    
    patch_placeholder(&cu->pub, false_branch_start);
    expression(cu, BP_LOWEST); // false branch

    patch_placeholder(&cu->pub, expr_end);
}

static void compile_var_definition(CompileUnit* cu, bool is_static) {
    consume_cur_token(cu->parser, TOKEN_ID, "missing variable name.");
    Token name = cu->parser->pre_token;

    if (cu->parser->cur_token.type == TOKEN_COMMA) {
        COMPILE_ERROR(cu->parser, "'let' only support declaring a variable.");
    }

    if (match_token(cu->parser, TOKEN_COLON)) {
        type_annotation(cu);
    }

    if (cu->pub.enclosing_unit == NULL && cu->pub.enclosing_classbk != NULL) {
        // class field definition

        if (is_static) { // static field
            char* static_field_id = ALLOCATE_ARRAY(cu->parser->vm, char, MAX_ID_LEN);
            memset(static_field_id, 0, MAX_ID_LEN);
            u32 static_field_id_len = 0;
            
            char* cls_name = cu->pub.enclosing_classbk->name->val.start;
            u32 cls_name_len = cu->pub.enclosing_classbk->name->val.len;

            // ClsClass@static_var_name
            memmove(static_field_id, "Cls", 3);
            memmove(static_field_id + 3, cls_name, cls_name_len);
            memmove(static_field_id + 3 + cls_name_len, "@", 1);
            memmove(static_field_id + 4 + cls_name_len, name.start, name.len);

            static_field_id_len = strlen(static_field_id);

            if (find_local(&cu->pub, static_field_id, static_field_id_len) != -1) {
                COMPILE_ERROR(
                    cu->parser,
                    "static field '%s', redefinition.", static_field_id
                );
            }

            declare_local_var(&cu->pub, static_field_id, static_field_id_len);

            ASSERT(cu->pub.scope_depth == 0, "should in class scope.");

            // 若有初始值，则解析；若无，则初始化为null
            if (match_token(cu->parser, TOKEN_ASSIGN)) {
                expression(cu, BP_LOWEST);
            } else {
                write_opcode(&cu->pub, OPCODE_PUSH_NULL);
            }

            consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of variable definition.");

            return;
        }
        
        // 实例域
        ClassBookKeep* class_bk = get_enclosing_classbk(&cu->pub);
        int field_index = get_index_from_symbol_table(&class_bk->fields, name.start, name.len);
        if (field_index != -1) {
            if (field_index > MAX_FIELD_NUM) {
                COMPILE_ERROR(cu->parser, "the max number of instance field is %d.", MAX_FIELD_NUM);
            }

            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name.start, name.len);
            COMPILE_ERROR(cu->parser, "instance field '%s' redefinition.", id);
        }

        if (match_token(cu->parser, TOKEN_ASSIGN)) {
            COMPILE_ERROR(cu->parser, "instance field isn't allowed initialization.");
        }
        
        field_index = add_symbol(cu->parser->vm, &class_bk->fields, name.start, name.len);

        consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of variable definition.");

        return;
    }

    // 一般变量
    if (match_token(cu->parser, TOKEN_ASSIGN)) {
        expression(cu, BP_LOWEST);
    } else {
        write_opcode(&cu->pub, OPCODE_PUSH_NULL);
    }

    u32 index = declare_variable(&cu->pub, name.start, name.len);
    define_variable(&cu->pub, index);

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of variable definition.");
}

static void compile_stmt(CompileUnit* cu);

static void compile_if_stmt(CompileUnit* cu) {
    expression(cu, BP_LOWEST); // condition
    u32 else_block_start = emit_instr_with_placeholder(&cu->pub, OPCODE_JMP_IF_FALSE);

    compile_stmt(cu); // then block

    if (match_token(cu->parser, TOKEN_ELSE)) {
        u32 stmt_end = emit_instr_with_placeholder(&cu->pub, OPCODE_JMP);
        patch_placeholder(&cu->pub, else_block_start);

        compile_stmt(cu); // else block

        patch_placeholder(&cu->pub, stmt_end);
    } else {
        patch_placeholder(&cu->pub, else_block_start);
    }
}

static void compile_loop_body(CompileUnit* cu) {
    cu->pub.cur_loop->body_start_index = cu->pub.fn->instr_stream.count;
    compile_stmt(cu);
}

static void compile_while_stmt(CompileUnit* cu) {
    Loop loop;
    
    enter_loop_setting(&cu->pub, &loop);

    expression(cu, BP_LOWEST); // condition

    loop.exit_index = emit_instr_with_placeholder(&cu->pub, OPCODE_JMP_IF_FALSE);

    compile_loop_body(cu);

    leave_loop_patch(&cu->pub);
}

inline static void compile_return_stmt(CompileUnit* cu) {
    // 编译返回值
    if (match_token(cu->parser, TOKEN_SEMICOLON)) {
        write_opcode(&cu->pub, OPCODE_PUSH_NULL);
    } else {
        expression(cu, BP_LOWEST);
        consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of return statment.");
    }

    write_opcode(&cu->pub, OPCODE_RETURN);
}

inline static void compile_break_stmt(CompileUnit* cu) {
    if (cu->pub.cur_loop == NULL) {
        COMPILE_ERROR(cu->parser, "break statment should be used inside a loop");
    }

    // 清除循环体内的局部变量
    discard_local_var(&cu->pub, cu->pub.cur_loop->scope_depth + 1);

    // 填充end表示break占位
    emit_instr_with_placeholder(&cu->pub, OPCODE_END);

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of break statment.");
}

inline static void compile_continue_stmt(CompileUnit* cu) {
    if (cu->pub.cur_loop == NULL) {
        COMPILE_ERROR(cu->parser, "continue should be used inside a loop.");
    }

    discard_local_var(&cu->pub, cu->pub.cur_loop->scope_depth + 1);

    int loop_back_offset = cu->pub.fn->instr_stream.count - cu->pub.cur_loop->cond_start_index + 2;
    write_opcode_short_operand(&cu->pub, OPCODE_LOOP, loop_back_offset);

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of continue statment.");
}

static void compile_for_stmt(CompileUnit* cu) {
    // for i in sequnce {
    //     System.print(i); // do sth.
    // }
    // 会编译为：
    // /* for-loop scope */ {
    //     let for@seq = sequnce;
    //     let for@iter = null;
    //     while for@iter = for@seq.iterate(for@iter)
    //         let i = for@seq.iterator_value(for@iter);
    //     {
    //         System.print(i); // do sth.
    //     }
    // }

    enter_scope(&cu->pub); // for-loop scope

    consume_cur_token(cu->parser, TOKEN_ID, "expect variable name after for.");
    Token loop_var_name = cu->parser->pre_token;

    consume_cur_token(cu->parser, TOKEN_IN, "expect 'in' after loop var name.");

    // let for@seq = sequnce;
    expression(cu, BP_LOWEST);
    u32 seq = add_local_var(&cu->pub, "for@seq", 7);

    // let for@iter = null;
    write_opcode(&cu->pub, OPCODE_PUSH_NULL);
    u32 iter = add_local_var(&cu->pub, "for@iter", 8);

    // while-loop
    Loop loop;
    enter_loop_setting(&cu->pub, &loop);

    // while-loop condition: for@iter = for@seq.iterate(for@iter)
    write_opcode_byte_operand(&cu->pub, OPCODE_LOAD_LOCAL_VAR, seq);
    write_opcode_byte_operand(&cu->pub, OPCODE_LOAD_LOCAL_VAR, iter);
    emit_call(&cu->pub, 1, "iterate(_)", 10);
    write_opcode_byte_operand(&cu->pub, OPCODE_STORE_LOCAL_VAR, iter);

    loop.exit_index = emit_instr_with_placeholder(&cu->pub, OPCODE_JMP_IF_FALSE);

    // let i = for@seq.iterator_value(for@iter);
    write_opcode_byte_operand(&cu->pub, OPCODE_LOAD_LOCAL_VAR, seq);
    write_opcode_byte_operand(&cu->pub, OPCODE_LOAD_LOCAL_VAR, iter);
    emit_call(&cu->pub, 1, "iterator_value(_)", 17);

    enter_scope(&cu->pub); // i
    add_local_var(&cu->pub, loop_var_name.start, loop_var_name.len);

    compile_loop_body(cu);

    leave_scope(&cu->pub); // i
    leave_loop_patch(&cu->pub);
    leave_scope(&cu->pub); // for@
}

static void compile_method(CompileUnit* cu, Variable class, bool is_static) {
    ClassBookKeep* cls = cu->pub.enclosing_classbk;
    cls->in_static = is_static;

    MethodSignatureFunc method_sign = Rules[cu->parser->cur_token.type].method_sign;
    if (method_sign == NULL) {
        COMPILE_ERROR(cu->parser, "method need signature function.");
    }

    Signature sign = {
        .name = cu->parser->cur_token.start,
        .len = cu->parser->cur_token.len,
        .argc = 0,
    };
    cls->signature = &sign;

    get_next_token(cu->parser); // skip id

    CompileUnit method_cu;
    compile_unit_pubstruct_init(cu->pub.vm, cu->pub.cur_module, &method_cu.pub, &cu->pub, true);
    method_cu.parser = cu->parser;

    method_sign(&method_cu, &sign); // 解析函数签名
    if (cls->in_static && sign.type == SIGN_CONSTRUCT) {
        COMPILE_ERROR(cu->parser, "constructor is not allowed to be static.");
    }
    
    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' at the beginning of method body.");

    // method declare
    char sign_str[MAX_SIGN_LEN] = {'\0'};
    u32 sign_len = sign2string(&sign, sign_str);
    u32 method_index = declare_method(&cu->pub, sign_str, sign_len);

    compile_body(&method_cu, sign.type == SIGN_CONSTRUCT); // method body

    end_compile_unit(&method_cu.pub);

    define_method(&cu->pub, class, cls->in_static, method_index); // bind method to class

    if (sign.type == SIGN_CONSTRUCT) {
        // 若为构造函数，则为其添加一个对应的static函数定义

        sign.type = SIGN_METHOD;
        
        VM* vm = cu->parser->vm;
        u32 constructor_index = ensure_symbol_exist(vm, &vm->all_method_names, sign_str, sign_len);

        emit_create_instance(&cu->pub, &sign, method_index);
        
        define_method(&cu->pub, class, true, constructor_index);
    }
}

inline static void native_annotation(CompileUnit* cu) {
    match_token(cu->parser, TOKEN_STATIC);

    if (match_token(cu->parser, TOKEN_LB)) {
        // native [_]; native [_] = (_);
        do {
            consume_cur_token(cu->parser, TOKEN_ID, "(native annotation): expect parameter list.");
            if (match_token(cu->parser, TOKEN_COLON)) {
                type_annotation(cu);
            }
        } while (match_token(cu->parser, TOKEN_COMMA));
        consume_cur_token(cu->parser, TOKEN_RB, "(native annotation): expect ']';");
    } else if (Rules[cu->parser->cur_token.type].method_sign != NULL) {
        // native id(); native id = (_);
        get_next_token(cu->parser);
        if (match_token(cu->parser, TOKEN_LP) && !match_token(cu->parser, TOKEN_RP)) {
            do {
                consume_cur_token(cu->parser, TOKEN_ID, "(native annotation): expect parameter list.");
                if (match_token(cu->parser, TOKEN_COLON)) {
                    type_annotation(cu);
                }
            } while (match_token(cu->parser, TOKEN_COMMA));
            consume_cur_token(cu->parser, TOKEN_RP, "(native annotation): expect ')';");
        }
    } else {
        COMPILE_ERROR(cu->parser, "(native annotation): expect id or [ or -> for native function.");
    }

    if (match_token(cu->parser, TOKEN_ASSIGN)) {
        consume_cur_token(cu->parser, TOKEN_LP, "(native annotation): expect '= (id)'.");
        consume_cur_token(cu->parser, TOKEN_ID, "(native annotation): expect '= (id)'.");
        if (match_token(cu->parser, TOKEN_COLON)) {
            type_annotation(cu);
        }
        consume_cur_token(cu->parser, TOKEN_RP, "(native annotation): expect '= (id)'.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "(native annotation): expect ';'.");
}

inline static void compile_class_body(CompileUnit* cu, Variable class) {
    bool is_static = match_token(cu->parser, TOKEN_STATIC);

    if (match_token(cu->parser, TOKEN_LET)) {
        compile_var_definition(cu, is_static);
    } else if (match_token(cu->parser, TOKEN_NATIVE)) {
        native_annotation(cu);
    } else {
        compile_method(cu, class, is_static);
    }
}

static void compile_class_definition(CompileUnit* cu) {
    if (cu->pub.scope_depth != -1) {
        COMPILE_ERROR(cu->parser, "class definition must be in the module scope.");
    }

    consume_cur_token(cu->parser, TOKEN_ID, "need a name for class.");
    Token name = cu->parser->pre_token;

    Variable class;
    class.scope_type = VAR_SCOPE_MODULE;
    class.index = declare_variable(&cu->pub, name.start, name.len);

    ObjString* class_name = objstring_new(cu->parser->vm, name.start, name.len);
    emit_load_constant(&cu->pub, OBJ_TO_VALUE(class_name));

    if (match_token(cu->parser, TOKEN_LT)) {
        expression(cu, BP_CALL); // 继承的父类
    } else {
        emit_load_module_var(&cu->pub, "Object"); // 默认继承Object
    }

    int field_num_index = write_opcode_byte_operand(&cu->pub, OPCODE_CREATE_CLASS, 0xFF); // 未知字段数，填入0xFF占位
    emit_store_module_var(&cu->pub, class.index);

    ClassBookKeep class_bk = {
        .name = class_name,
        .in_static = false,
    };
    BufferInit(String, &class_bk.fields);
    BufferInit(Int, &class_bk.instant_methods);
    BufferInit(Int, &class_bk.static_methods);

    cu->pub.enclosing_classbk = &class_bk;

    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' in the start of class-define-body.");

    enter_scope(&cu->pub);

    while (!match_token(cu->parser, TOKEN_RC)) {
        compile_class_body(cu, class);
        if (PEEK_TOKEN(cu->parser) == TOKEN_EOF) {
            COMPILE_ERROR(cu->parser, "expect '}' in the end of class-define-body.");
        }
    }

    cu->pub.fn->instr_stream.datas[field_num_index] = class_bk.fields.count; // 回填字段数

    symbol_table_clear(cu->parser->vm, &class_bk.fields);
    BufferClear(Int, &class_bk.instant_methods, cu->parser->vm);
    BufferClear(Int, &class_bk.static_methods, cu->parser->vm);

    cu->pub.enclosing_classbk = NULL;

    leave_scope(&cu->pub);
}

static void compile_function_definition(CompileUnit* cu) {
    if (cu->pub.enclosing_unit != NULL) {
        COMPILE_ERROR(cu->parser, "'fn' should be in module scope.");
    }

    consume_cur_token(cu->parser, TOKEN_ID, "missing function name.");

    char fn_name[MAX_SIGN_LEN + 4] = {0};
    memmove(fn_name, "Fn@", 3);
    memmove(fn_name + 3, cu->parser->pre_token.start, cu->parser->pre_token.len);
    u32 fn_name_len = strlen(fn_name);

    u32 fn_name_index = declare_variable(&cu->pub, fn_name, fn_name_len);

    CompileUnit fncu;
    compile_unit_pubstruct_init(cu->pub.vm, cu->pub.cur_module, &fncu.pub, &cu->pub, false);
    fncu.parser = cu->parser;

    Signature tmp_fn_sign = {SIGN_METHOD, "", 0, 0};
    
    consume_cur_token(cu->parser, TOKEN_LP, "expect '(' after function name.");
    if (!match_token(cu->parser, TOKEN_RP)) {
        process_para_list(&fncu, &tmp_fn_sign);
        consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after parameter list.");
    }
    
    FUNCTION_RESULT_TYPPING_CHECK();

    fncu.pub.fn->argc = tmp_fn_sign.argc;

    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' at the beginning of method body.");

    compile_body(&fncu, false);

    end_compile_unit(&fncu.pub);

    define_variable(&cu->pub, fn_name_index);
}

static void compile_import_stmt(CompileUnit* cu) {
    // import foo;         => System.import_module("foo");
    // import foo for bar; => let bar = System.get_module_variable("foo", "bar");
    
    consume_cur_token(cu->parser, TOKEN_ID, "expect module name after import.");

    Token module_name_token = cu->parser->pre_token;

    int mode = 0;
    char* buf = NULL;
    int buf_len = -1;

    // 读入第一级路径
    if (module_name_token.len == 3 && strncmp(module_name_token.start, "std", 3) == 0) {
        buf_len = 0;
        mode = 1;
    } else if (module_name_token.len == 3 && strncmp(module_name_token.start, "lib", 3) == 0) {
        buf_len = 0;
        mode = 2;
    } else if (module_name_token.len == 4 && strncmp(module_name_token.start, "home", 4) == 0) {
        buf_len = 1;
        buf = malloc(buf_len + 1);
        buf[0] = '.';
        buf[1] = '\0';
    } else {
        buf_len = module_name_token.len;
        buf = malloc(buf_len + 1);
        memcpy(buf, module_name_token.start, module_name_token.len);
        buf[buf_len] = '\0';
    }

    // 多级路径解析
    while (match_token(cu->parser, TOKEN_DOT)) {
        consume_cur_token(cu->parser, TOKEN_ID, "expect id for multi-level-path.");
        module_name_token = cu->parser->pre_token;
        int old_len = buf_len;
        buf_len += module_name_token.len + 1;
        buf = realloc(buf, buf_len + 1);
        buf[old_len] = '/';
        memcpy(&buf[old_len + 1], module_name_token.start, module_name_token.len);
        buf[buf_len] = '\0';
    }

    ObjString* module_name = objstring_new(cu->parser->vm, buf, buf_len);
    u32 const_name_inedx = add_constant(&cu->pub, OBJ_TO_VALUE(module_name));

    if (buf != NULL) {
        free(buf);
    }

    buf = NULL;

    if (mode == 0) {
        emit_load_module_var(&cu->pub, "System");
        write_opcode_short_operand(&cu->pub, OPCODE_LOAD_CONSTANT, const_name_inedx);
        emit_call(&cu->pub, 1, "import_module(_)", 16);
    } else if (mode == 1) {
        emit_load_module_var(&cu->pub, "System");
        write_opcode_short_operand(&cu->pub, OPCODE_LOAD_CONSTANT, const_name_inedx);
        emit_call(&cu->pub, 1, "import_std_module(_)", 20);
    } else if (mode == 2) {
        emit_load_module_var(&cu->pub, "System");
        write_opcode_short_operand(&cu->pub, OPCODE_LOAD_CONSTANT, const_name_inedx);
        emit_call(&cu->pub, 1, "import_lib_module(_)", 20);
    }
    write_opcode(&cu->pub, OPCODE_POP);

    if (match_token(cu->parser, TOKEN_SEMICOLON)) {
        return;
    }

    consume_cur_token(cu->parser, TOKEN_FOR, "miss match token 'for' or ';'.");

    do {
        consume_cur_token(cu->parser, TOKEN_ID, "expect variable name after 'for' in import.");
        u32 var_index = declare_variable(&cu->pub, cu->parser->pre_token.start, cu->parser->pre_token.len);
        ObjString* var_name = objstring_new(cu->parser->vm, cu->parser->pre_token.start, cu->parser->pre_token.len);
        u32 const_var_name = add_constant(&cu->pub, OBJ_TO_VALUE(var_name));
        
        // $top = System.get_module_variable("foo", "bar");
        emit_load_module_var(&cu->pub, "System");
        write_opcode_short_operand(&cu->pub, OPCODE_LOAD_CONSTANT, const_name_inedx);
        write_opcode_short_operand(&cu->pub, OPCODE_LOAD_CONSTANT, const_var_name);
        emit_call(&cu->pub, 2, "get_module_variable(_,_)", 24);

        // bar = $top
        define_variable(&cu->pub, var_index);
    } while (match_token(cu->parser, TOKEN_COMMA));

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of statement.");
}

static void compile_stmt(CompileUnit* cu) {
    if (match_token(cu->parser, TOKEN_IF)) {
        compile_if_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_WHILE)) {
        compile_while_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_FOR)) {
        compile_for_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_BREAK)) {
        compile_break_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_RETURN)) {
        compile_return_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_CONTINUE)) {
        compile_continue_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_LC)) {
        enter_scope(&cu->pub);
        compile_block(cu);
        leave_scope(&cu->pub);
    } else {
        expression(cu, BP_LOWEST);
        write_opcode(&cu->pub, OPCODE_POP);
        consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of expression statment.");
    }
}

static void compile_prog(CompileUnit* cu) {
    if (match_token(cu->parser, TOKEN_IMPORT)) {
        compile_import_stmt(cu);
    } else if (match_token(cu->parser, TOKEN_FN)) {
        compile_function_definition(cu);
    } else if (match_token(cu->parser, TOKEN_CLASS)) {
        compile_class_definition(cu);
    } else if (match_token(cu->parser, TOKEN_LET)) {
        compile_var_definition(cu, cu->parser->pre_token.type == TOKEN_STATIC);
    } else {
        compile_stmt(cu);
    }
}

void one_pass_compile_program(CompileUnitPubStruct* pub_cu, Parser* parser) {
    CompileUnit cu = (CompileUnit) {
        .pub = *pub_cu,
        .parser = pub_cu->vm->cur_parser
    };

    while (!match_token(parser, TOKEN_EOF)) {
        compile_prog(&cu);
    }

    *pub_cu = cu.pub;
}

// fn.nud
static void closure_expr(CompileUnit* cu, bool can_assign) {
    // fn(n: i32) -> i32 { return n * n; }

    CompileUnit fncu;
    compile_unit_pubstruct_init(cu->pub.vm, cu->pub.cur_module, &fncu.pub, &cu->pub, false);
    fncu.parser = cu->parser;

    Signature temp_func_sign = {
        .type = SIGN_METHOD,
        .name = "",
        .len = 0,
        .argc = 0,
    };

    consume_cur_token(cu->parser, TOKEN_LP, "expect '(' after fn.");
    if (!match_token(cu->parser, TOKEN_RP)) {
        process_para_list(&fncu, &temp_func_sign);
        consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after parameter list.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();

    fncu.pub.fn->argc = temp_func_sign.argc;

    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' for closure body.");

    compile_body(&fncu, false);

    end_compile_unit(&fncu.pub);

    // 函数闭包已在栈顶
}
