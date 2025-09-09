#include "compiler.h"
#include "common.h"
#include "header_obj.h"
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

#define OPCODE_SLOTS(opcode, effect) effect,
static const int opcode_slots_used[] = {
    #include "opcode.inc"
};
#undef OPCODE_SLOTS

typedef enum {
    VAR_SCOPE_INVALID,
    VAR_SCOPE_LOCAL,
    VAR_SCOPE_UPVALUE,
    VAR_SCOPE_MODULE,
} VarScopeType;

typedef struct {
    VarScopeType scope_type;
    int index;
} Variable;

static void compileunit_init(Parser* parser, CompileUnit* cu, CompileUnit* enclosing_unit, bool is_method) {
    parser->cur_compile_unit = cu;
    cu->parser = parser;
    cu->enclosing_unit = enclosing_unit;
    cu->cur_loop = NULL;
    cu->enclosing_classbk = NULL;

    if (enclosing_unit == NULL) {
        cu->scope_depth = -1;
        cu->local_vars_count = 0;
    } else {
        if (is_method) {
            cu->local_vars[0].name = "self";
            cu->local_vars[0].len = 4;
        } else {
            cu->local_vars[0].name = NULL;
            cu->local_vars[0].len = 0;
        }

        cu->local_vars[0].scope_depth = -1;
        cu->local_vars[0].is_upvalue = false;
        cu->local_vars_count = 1;

        cu->scope_depth = 0;
    }

    cu->stack_slot_num = cu->local_vars_count;
    cu->fn = objfn_new(cu->parser->vm, cu->parser->cur_module, cu->local_vars_count);
}

static int write_byte(CompileUnit* cu, int byte) {
#if DEBUG
    BufferAdd(Int, &cu->fn->debug->line, cu->parser->vm, cu->parser->pre_token.line);
#endif

    BufferAdd(Byte, &cu->fn->instr_stream, cu->parser->vm, (u8)byte);
    return cu->fn->instr_stream.count - 1;
}

static void write_opcode(CompileUnit* cu, OpCode op) {
    write_byte(cu, op);
    cu->stack_slot_num += opcode_slots_used[op];
    if (cu->stack_slot_num > cu->fn->max_stack_slot_used) {
        cu->fn->max_stack_slot_used = cu->stack_slot_num;
    }
}

inline static int write_byte_operand(CompileUnit* cu, int operand) {
    return write_byte(cu, operand);
}

inline static void write_short_operand(CompileUnit* cu, int operand) {
    write_byte(cu, (operand >> 8) & 0xFF);
    write_byte(cu, operand & 0xFF);
}

inline static int write_opcode_byte_operand(CompileUnit* cu, OpCode opcode, int operand) {
    write_opcode(cu, opcode);
    return write_byte_operand(cu, operand);
}

inline static void write_opcode_short_operand(CompileUnit* cu, OpCode opcode, int operand) {
    write_opcode(cu, opcode);
    write_short_operand(cu, operand);
}

// 编译期用于声明模块变量，val正常声明时为null，在定义前使用为第一次出现的行号
int one_pass_define_module_var(VM* vm, ObjModule* module, const char* name, u32 len, Value val) {
    if (len > MAX_ID_LEN) {
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, len);

        if (vm->cur_parser != NULL) {
            COMPILE_ERROR(
                vm->cur_parser,
                "len of id '%s...' should less then %d.",
                id, MAX_ID_LEN
            );
        } else {
            MEM_ERROR("len of id '%s...' should less then %d.", id, MAX_ID_LEN);
        }
    }

    if (VALUE_IS_OBJ(val)) {
        push_tmp_root(vm, val.header);
    }

    int symbol_index = get_index_from_symbol_table(&module->module_var_name, name, len);
    if (symbol_index == -1) {
        symbol_index = add_symbol(vm, &module->module_var_name, name, len);
        BufferAdd(Value, &module->module_var_value, vm, val);
    } else if (VALUE_IS_I32(module->module_var_value.datas[symbol_index])) {
        module->module_var_value.datas[symbol_index] = val;
    } else {
        symbol_index = -1;
    }

    if (VALUE_IS_OBJ(val)) {
        pop_tmp_root(vm);
    }

    return symbol_index;
}

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

static u32 add_constant(CompileUnit* cu, Value constant) {
    if (VALUE_IS_OBJ(constant)) {
        push_tmp_root(cu->parser->vm, constant.header);
    }

    // 避免重复常量重复入表
    for (u32 i = 0; i < cu->fn->constants.count; i++) {
        Value val = cu->fn->constants.datas[i];
        if (value_is_equal(val, constant)) {
            if (VALUE_IS_OBJ(constant)) {
                pop_tmp_root(cu->parser->vm);
            }
            return i;
        }
    }

    // 表中没有相同的常量，入表
    BufferAdd(Value, &cu->fn->constants, cu->parser->vm, constant);

    if (VALUE_IS_OBJ(constant)) {
        pop_tmp_root(cu->parser->vm);
    }
    return cu->fn->constants.count - 1;
}

static void expression(CompileUnit* cu, BindPower rbp);
static void compile_program(CompileUnit* cu);

typedef void (*DenotationFunc)(CompileUnit* cu, bool can_assign);
typedef void (*MethodSignatureFunc)(CompileUnit* cu, Signature* sign); // 签名函数

typedef struct {
    const char* id;
    BindPower lbp;
    
    DenotationFunc nud;
    DenotationFunc led;
    
    MethodSignatureFunc method_sign;
} SymbolBindRule;

static Variable get_var_from_local_or_upvalue(CompileUnit* cu, const char* name, u32 len);

inline static void emit_load_constant(CompileUnit* cu, Value val) {
    int index = add_constant(cu, val);
    write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, index);
}

static void emit_load_variable(CompileUnit* cu, Variable var) {
    switch (var.scope_type) {
        case VAR_SCOPE_LOCAL:
            write_opcode_byte_operand(cu, OPCODE_LOAD_LOCAL_VAR, var.index);
            break;
        case VAR_SCOPE_UPVALUE:
            write_opcode_byte_operand(cu, OPCODE_LOAD_UPVALUE, var.index);
            break;
        case VAR_SCOPE_MODULE:
            write_opcode_short_operand(cu, OPCODE_LOAD_MODULE_VAR, var.index);
            break;
        default:
            UNREACHABLE();
    }
}

static void emit_store_variable(CompileUnit* cu, Variable var) {
    switch (var.scope_type) {
        case VAR_SCOPE_LOCAL:
            write_opcode_byte_operand(cu, OPCODE_STORE_LOCAL_VAR, var.index);
            break;
        case VAR_SCOPE_UPVALUE:
            write_opcode_byte_operand(cu, OPCODE_STORE_UPVALUE, var.index);
            break;
        case VAR_SCOPE_MODULE:
            write_opcode_short_operand(cu, OPCODE_STORE_MODULE_VAR, var.index);
            break;
        default:
            UNREACHABLE();
    }
}

static void emit_load_or_store_variable(CompileUnit* cu, bool can_assign, Variable var) {
    if (can_assign && match_token(cu->parser, TOKEN_ASSIGN)) {
        expression(cu, BP_LOWEST); // = 右值
        emit_store_variable(cu, var);
        return;
    }
    emit_load_variable(cu, var);
}

inline static void emit_load_self(CompileUnit* cu) {
    Variable var = get_var_from_local_or_upvalue(cu, "self", 4);
    ASSERT(var.scope_type != VAR_SCOPE_INVALID, "get self failed!");
    emit_load_variable(cu, var);
}

static u32 sign2string(Signature* sign, char* buf) {
    u32 pos = 0;

    memcpy(buf, sign->name, sign->len);
    pos += sign->len;

    switch (sign->type) {
        case SIGN_GETTER:
            // GETTER id
            break;
        
        case SIGN_SETTER:
            // SETTER id=(_)
            buf[pos++] = '=';
            buf[pos++] = '(';
            buf[pos++] = '_';
            buf[pos++] = ')';
            break;
        
        case SIGN_CONSTRUCT:
        case SIGN_METHOD:
            // id(_,...)

            buf[pos++] = '(';
            
            for (int i = 0; i < sign->argc; i++) {
                buf[pos++] = '_';
                buf[pos++] = ',';
            }

            if (sign->argc == 0) {
                buf[pos++] = ')';
            } else {
                buf[pos - 1] = ')';
            }

            break;

        case SIGN_SUBSCRIPT:
            // id[_,...]
            buf[pos++] = '[';
            
            for (int i = 0; i < sign->argc; i++) {
                buf[pos++] = '_';
                buf[pos++] = ',';
            }

            if (sign->argc == 0) {
                buf[pos++] = ']';
            } else {
                buf[pos - 1] = ']';
            }

            break;

        case SIGN_SUBSCRIPT_SETTER:
            // id[_,...]=(_)
            buf[pos++] = '[';

            for (int i = 0; i < sign->argc - 1; i++) {
                buf[pos++] = '_';
                buf[pos++] = ',';
            }

            if (sign->argc == 0) {
                buf[pos++] = ']';
            } else {
                buf[pos - 1] = ']';
            }

            buf[pos++] = '=';
            buf[pos++] = '(';
            buf[pos++] = '_';
            buf[pos++] = ')';

            break;
    }

    buf[pos] = '\0';
    return pos;
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
    ASSERT(nud != NULL, "token nud is NULL.");
    get_next_token(cu->parser);
    nud(cu, can_assign);

    while (rbp < Rules[cu->parser->cur_token.type].lbp) {
        DenotationFunc led = Rules[cu->parser->cur_token.type].led;
        get_next_token(cu->parser);
        led(cu, can_assign);
    }
}

static void emit_call_by_signature(CompileUnit* cu, Signature* sign, OpCode op) {
    char sign_buffer[MAX_SIGN_LEN];
    u32 len = sign2string(sign, sign_buffer);

    int symbol_index = ensure_symbol_exist(
        cu->parser->vm, &cu->parser->vm->all_method_names,
        sign_buffer, len
    );
    write_opcode_short_operand(cu, op + sign->argc, symbol_index);

    if (op == OPCODE_SUPER0) {
        // 为将来需要填入的基类占位
        write_short_operand(cu, add_constant(cu, VT_TO_VALUE(VT_NULL)));
    }
}

static void emit_call(CompileUnit* cu, int argc, const char* name, u32 len) {
    int symbol_index = ensure_symbol_exist(
        cu->parser->vm, &cu->parser->vm->all_method_names,
        name, len
    );

    write_opcode_short_operand(cu, OPCODE_CALL0 + argc, symbol_index);
}

static void infix_operator(CompileUnit* cu, bool can_assign) {
    SymbolBindRule* rule = &Rules[cu->parser->pre_token.type];
    BindPower rbp = rule->lbp;
    
    expression(cu, rbp);

    Signature sign = {SIGN_METHOD, rule->id, strlen(rule->id), 1};
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

static void unary_operator(CompileUnit* cu, bool can_assign) {
    SymbolBindRule* rule = &Rules[cu->parser->pre_token.type];

    expression(cu, BP_UNARY);

    emit_call(cu, 0, rule->id, 1);
}

static void literal(CompileUnit* cu, bool can_assign) {
    emit_load_constant(cu, cu->parser->pre_token.value);
}

// 添加局部变量到cu
static u32 add_local_var(CompileUnit* cu, const char* name, u32 len) {
    LocalVar* var = &(cu->local_vars[cu->local_vars_count]);

    var->name = name;
    var->len = len;
    var->scope_depth = cu->scope_depth;
    var->is_upvalue = false;

    return cu->local_vars_count++;
}

// 声明局部变量
static int declare_local_var(CompileUnit* cu, const char* name, u32 len) {
    if (cu->local_vars_count >= MAX_LOCAL_VAR_NUM) {
        COMPILE_ERROR(
            cu->parser,
            "the max len of local variable of one scope is %d.", MAX_LOCAL_VAR_NUM
        );
    }

    for (int i = (int)cu->local_vars_count - 1; i >= 0; i--) {
        LocalVar* var = &cu->local_vars[i];
        
        // 只搜索当前作用域是否重定义
        if (var->scope_depth < cu->scope_depth) {
            break;
        }

        // 重定义检查
        if (var->len == len && memcmp(var->name, name, len) == 0) {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, len);
            COMPILE_ERROR(
                cu->parser,
                "local variable '%s' redefinition.", id
            );
        }
    }

    return add_local_var(cu, name, len);
}

// 根据当前作用域定义变量
static int declare_variable(CompileUnit* cu, const char* name, u32 len) {
    if (cu->scope_depth == -1) {
        int index = one_pass_define_module_var(
            cu->parser->vm, cu->parser->cur_module,
            name, len, VT_TO_VALUE(VT_NULL)
        );

        if (index == -1) {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, len);
            COMPILE_ERROR(
                cu->parser,
                "module variable '%s' redefinition.", id
            );
        }

        return index;
    }

    return declare_local_var(cu, name, len);
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
    declare_variable(cu, cu->parser->cur_token.start, cu->parser->cur_token.len);

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
        declare_variable(cu, cu->parser->cur_token.start, cu->parser->cur_token.len);

        // typping
        if (match_token(cu->parser, TOKEN_COLON)) {
            type_annotation(cu);
        }

        consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after var name.");
    }

    FUNCTION_RESULT_TYPPING_CHECK();
}

static int declare_module_var(
    VM* vm, ObjModule* module, const char* name, u32 len, Value val
) {
    BufferAdd(Value, &module->module_var_value, vm, val);
    return add_symbol(vm, &module->module_var_name, name, len);
}

static CompileUnit* get_enclosing_classbk_unit(CompileUnit* cu) {
    while (cu != NULL) {
        if (cu->enclosing_classbk != NULL) {
            return cu;
        }
        cu = cu->enclosing_unit;
    }
    return NULL;
}

static ClassBookKeep* get_enclosing_classbk(CompileUnit* cu) {
    CompileUnit* ncu = get_enclosing_classbk_unit(cu);
    return ncu == NULL ? NULL : ncu->enclosing_classbk;
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
        declare_variable(cu, cu->parser->pre_token.start, cu->parser->pre_token.len);

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

    // typping
    if (match_token(cu->parser, TOKEN_COLON)) {
        type_annotation(cu);
    }

    declare_variable(cu, cu->parser->cur_token.start, cu->parser->cur_token.len);

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

static int find_local(CompileUnit* cu, const char* name, u32 len) {
    for (int index = cu->local_vars_count - 1; index >= 0; index--) {
        LocalVar* t = &cu->local_vars[index];
        if (t->len == len && memcmp(t->name, name, len) == 0) {
            return index;
        }
    }
    return -1;
}

static int add_upvalue(CompileUnit* cu, bool is_enclosing_local_var, u32 index) {
    for (int i = 0; i < cu->fn->upvalue_number; i++) {
        Upvalue* t = &cu->upvalues[i];
        if (t->index == index && t->is_enclosing_local_var == is_enclosing_local_var) {
            return i; // 找到，返回upvalue索引
        }
    }

    // 没找到，添加新upvalue
    cu->upvalues[cu->fn->upvalue_number] = (Upvalue) {
        .index = index,
        .is_enclosing_local_var = is_enclosing_local_var,
    };

    return cu->fn->upvalue_number++;
}

static int find_upvalue(CompileUnit* cu, const char* name, u32 len) {
    // toplevel没有upvalue
    if (cu->enclosing_unit == NULL) {
        return -1;
    }

    // 查找的是否不是静态域 && 是否是方法的cu
    // 不是静态域没有upvalue 方法没有upvalue
    if (!strchr(name, '@') && cu->enclosing_unit->enclosing_classbk != NULL) {
        return -1;
    }

    // 若name为外层局部变量，设置该局部变量为upvalue
    int direct_outer_local_index = find_local(cu->enclosing_unit, name, len);
    if (direct_outer_local_index != -1) {
        cu->enclosing_unit->local_vars[direct_outer_local_index].is_upvalue = true;
        return add_upvalue(cu, true, direct_outer_local_index);
    }

    // 函数递归向外层寻找
    int direct_outer_upvalue_index = find_upvalue(cu->enclosing_unit, name, len);
    if (direct_outer_upvalue_index != -1) {
        return add_upvalue(cu, false, direct_outer_upvalue_index);
    }

    return -1; // 未找到
}

static Variable get_var_from_local_or_upvalue(CompileUnit* cu, const char* name, u32 len) {
    Variable var = {.scope_type = VAR_SCOPE_INVALID};

    // 查找local域
    var.index = find_local(cu, name, len);
    if (var.index != -1) {
        var.scope_type = VAR_SCOPE_LOCAL;
        return var;
    }

    // 查找upvalue
    var.index = find_upvalue(cu, name, len);
    if (var.index != -1) {
        var.scope_type = VAR_SCOPE_UPVALUE;
    }

    return var;
}

static void compile_block(CompileUnit* cu) {
    while (!match_token(cu->parser, TOKEN_RC)) {
        if (PEEK_TOKEN(cu->parser) == TOKEN_EOF) {
            COMPILE_ERROR(cu->parser, "expect '}' at the end of block.");
        }
        compile_program(cu);
    }
}

static void compile_body(CompileUnit* cu, bool is_construct) {
    compile_block(cu);

    // 填充默认返回值
    if (is_construct) {
        // 是构造函数，返回self。
        write_opcode_byte_operand(cu, OPCODE_LOAD_LOCAL_VAR, 0);
    } else {
        // 不是构造函数，默认返回null。
        write_opcode(cu, OPCODE_PUSH_NULL);
    }
    
    // 填充默认的return语句
    write_opcode(cu, OPCODE_RETURN);
}

#if DEBUG
static ObjFn* end_compile_unit(CompileUnit* cu, const char* debug_name, u32 debug_name_len) {
    bind_debug_fn_name(cu->parser->vm, cu->fn->debug, debug_name, debug_name_len);
#else
static ObjFn* end_compile_unit(CompileUnit* cu) {
#endif

    write_opcode(cu, OPCODE_END);
    
    if (cu->enclosing_unit != NULL) {
        // 把当前objfn添加到上层cu常量表中
        u32 index = add_constant(cu->enclosing_unit, OBJ_TO_VALUE(cu->fn));

        // 在外层cu中添加指令，将objfn创建为外层cu的闭包
        write_opcode_short_operand(cu->enclosing_unit, OPCODE_CREATE_CLOSURE, index);

        // 为每个upvalue生成2个参数 作为create_closure的操作数
        // [若为局部变量则填入1否则为0] [upvalue的索引]
        for (int i = 0; i < cu->fn->upvalue_number; i++) {
            write_byte(cu->enclosing_unit, cu->upvalues[i].is_enclosing_local_var ? 1 : 0);
            write_byte(cu->enclosing_unit, cu->upvalues[i].index);
        }
    }

    cu->parser->cur_compile_unit = cu->enclosing_unit;
    
    return cu->fn;
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

    emit_call_by_signature(cu, &new_sign, opcode);
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
        
        emit_call_by_signature(cu, &sign, opcode);
        return;
    }

    emit_getter_method_call(cu, &sign, opcode);
}

static bool is_local_name(const char* name) {
    return (*name >= 'a' && *name <= 'z'); // 小写字母开头，为局部变量名
}

static void id(CompileUnit* cu, bool can_assign) {
    Token name = cu->parser->pre_token;
    ClassBookKeep* class_bk = get_enclosing_classbk(cu);

    // 标识符处理顺序：
    // func call -> local var & upvalue -> instance field -> static field -> getter -> module var

    // function call
    // class外的作用域(此处实际只检查toplevel) && id后有括号
    if (cu->enclosing_unit == NULL && match_token(cu->parser, TOKEN_LP)) {
        char id[MAX_ID_LEN + 4] = {'\0'};
        memmove(id, "Fn@", 3);
        memmove(id + 3, name.start, name.len);

        Variable var = {
            .scope_type = VAR_SCOPE_MODULE,
            .index = get_index_from_symbol_table(
                &cu->parser->cur_module->module_var_name,
                id, strlen(id)
            ),
        };

        if (var.index == -1) {
            memmove(id, name.start, name.len);
            id[name.len] = '\0';

            COMPILE_ERROR(cu->parser, "Undefined function '%s'.", id);
        }

        // 加载函数闭包到栈
        emit_load_variable(cu, var);

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
        emit_call_by_signature(cu, &call_sign, OPCODE_CALL0);

        return;
    }

    // 非函数调用，尝试解析为局部变量与upvalue。
    Variable var = get_var_from_local_or_upvalue(cu, name.start, name.len);
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

            if (cu->enclosing_unit != NULL) {
                write_opcode_byte_operand(cu, is_read ? OPCODE_LOAD_SELF_FIELD : OPCODE_STORE_SELF_FIELD, field_index);
            } else {
                emit_load_self(cu);
                write_opcode_byte_operand(cu, is_read ? OPCODE_LOAD_FIELD : OPCODE_STORE_FIELD, field_index);
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

        var = get_var_from_local_or_upvalue(cu, static_field_id, static_field_id_len);
        
        DEALLOCATE_ARRAY(cu->parser->vm, static_field_id, MAX_ID_LEN);

        if (var.index != -1) {
            emit_load_or_store_variable(cu, can_assign, var);
            return;
        }

        // 类中方法调用
        // 若id开头为小写字母，那么则将其作为方法调用进行编译。
        // 如果存在开头大写字母的方法，则需要使用self.MethodName()的形式调用
        if (is_local_name(name.start)) {
            emit_load_self(cu);
            emit_method_call(cu, name.start, name.len, OPCODE_CALL0, can_assign);
            return;
        }
    }

    // 作为模块变量处理
    var.scope_type = VAR_SCOPE_MODULE;
    var.index = get_index_from_symbol_table(&cu->parser->cur_module->module_var_name, name.start, name.len);
    if (var.index == -1) {
        char fn_name[MAX_ID_LEN] = {'\0'};
        memmove(fn_name, "Fn@", 3);
        memmove(fn_name + 3, name.start, name.len);

        u32 fn_name_len = strlen(fn_name);

        var.index = get_index_from_symbol_table(
            &cu->parser->cur_module->module_var_name,
            fn_name, fn_name_len
        );

        // 在函数内部()调用函数的解决方案
        if (match_token(cu->parser, TOKEN_LP)) {
            if (var.index == -1) {
                // 调用当前未定义函数
                var.index = declare_module_var(
                    cu->parser->vm, cu->parser->cur_module,
                    fn_name, fn_name_len, I32_TO_VALUE(name.line)
                );
            }

            // 加载函数闭包到栈
            emit_load_variable(cu, var);

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
            emit_call_by_signature(cu, &call_sign, OPCODE_CALL0);

            return;
        }

        // 不是已定义的模块变量，可能在后文中有定义，预先填入行号作为标记
        if (var.index == -1) {
            var.index = declare_module_var(
                cu->parser->vm, cu->parser->cur_module,
                name.start, name.len, I32_TO_VALUE(name.line)
            );
        }
    }

    emit_load_or_store_variable(cu, can_assign, var);
}

static void emit_load_module_var(CompileUnit* cu, const char* name) {
    int index = get_index_from_symbol_table(
        &cu->parser->cur_module->module_var_name,
        name, strlen(name)
    );
    
    ASSERT(index != -1, "symbol should have been defined.");

    write_opcode_short_operand(cu, OPCODE_LOAD_MODULE_VAR, index);
}

static void string_interpolation(CompileUnit* cu, bool can_assign) {
    // "a %(b + c) d %(e) f" => ["a ", b + c, " d ", e, " f"].join()

    emit_load_module_var(cu, "List");
    emit_call(cu, 0, "new()", 5);

    do {
        if (((ObjString*)cu->parser->pre_token.value.header)->val.len != 0) { // 当其为非空字符串时添加
            literal(cu, false); // 解析字符串
            // List.core_append(_: any) -> List 用于编译器内部构造列表。
            emit_call(cu, 1, "core_append(_)", 14);
        }

        expression(cu, BP_LOWEST); // 解析内嵌表达式
        emit_call(cu, 1, "core_append(_)", 14); // 将内嵌表达式的值加入列表
    } while (match_token(cu->parser, TOKEN_INTERPOLATION));

    consume_cur_token(
        cu->parser, TOKEN_STRING,
        "expect string at the end of interpolatation."
    );

    // 结尾的TOKEN_STRING
    if (((ObjString*)cu->parser->pre_token.value.header)->val.len != 0) { // 当其为非空字符串时添加
        literal(cu, false);
        emit_call(cu, 1, "core_append(_)", 14);
    }

    emit_call(cu, 0, "join()", 6);
}

static void boolean(CompileUnit* cu, bool can_assign) {
    OpCode opcode = cu->parser->pre_token.type == TOKEN_TRUE ? OPCODE_PUSH_TRUE : OPCODE_PUSH_FALSE;
    write_opcode(cu, opcode);
}

static void null(CompileUnit* cu, bool can_assign) {
    write_opcode(cu, OPCODE_PUSH_NULL);
}

static void self(CompileUnit* cu, bool can_assign) {
    if (get_enclosing_classbk(cu) == NULL) {
        COMPILE_ERROR(cu->parser, "'self' must be inside a class method.");
    }
    emit_load_self(cu);
}

static void super(CompileUnit* cu, bool can_assign) {
    ClassBookKeep* enclosing_classbk = get_enclosing_classbk(cu);
    if (enclosing_classbk == NULL) {
        COMPILE_ERROR(cu->parser, "can't invoke super outside a class method.");
    }

    emit_load_self(cu); // 函数调用的argv[0]永远指向self

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

    emit_load_module_var(cu, "List");
    emit_call(cu, 0, "new()", 5);

    do {
        if (PEEK_TOKEN(cu->parser) == TOKEN_RB) {
            break;
        }
        
        expression(cu, BP_LOWEST);
        emit_call(cu, 1, "core_append(_)", 14);
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

    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
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
    emit_load_module_var(cu, "Map");
    emit_call(cu, 0, "new()", 5);

    do {
        if (PEEK_TOKEN(cu->parser) == TOKEN_RC) {
            break;
        }

        expression(cu, BP_UNARY); // key

        consume_cur_token(cu->parser, TOKEN_COLON, "expect ':' after key.");

        expression(cu, BP_LOWEST); // value

        emit_call(cu, 2, "core_insert(_,_)", 16);
    } while (match_token(cu->parser, TOKEN_COMMA));

    consume_cur_token(cu->parser, TOKEN_RC, "map literal should end with '}'.");
}

static u32 emit_instr_with_placeholder(CompileUnit* cu, OpCode opcode) {
    write_opcode(cu, opcode);
    u32 hp = write_byte(cu, 0xFF); // 填充高位
    write_byte(cu, 0xFF); // 填充低位
    return hp; // 返回高位地址
}

static void patch_placeholder(CompileUnit* cu, u32 abs_index) {
    u32 offset = cu->fn->instr_stream.count - abs_index - 2; // 计算跳转量
    cu->fn->instr_stream.datas[abs_index] = (offset >> 8) & 0xFF; // 填入跳转量的高八位
    cu->fn->instr_stream.datas[abs_index + 1] = offset & 0xFF; // 填入跳转量的低八位
}

static void logical_or(CompileUnit* cu, bool can_assign) {
    // OPCODE_OR <next_stmt_addr>
    u32 placeholder = emit_instr_with_placeholder(cu, OPCODE_OR);
    expression(cu, BP_LOGICAL_OR); // 右操作数
    patch_placeholder(cu, placeholder);
}

static void logical_and(CompileUnit* cu, bool can_assign) {
    // OPCODE_AND <next_stmt_addr>
    u32 placeholder = emit_instr_with_placeholder(cu, OPCODE_AND);
    expression(cu, BP_LOGICAL_AND); // 右操作数
    patch_placeholder(cu, placeholder);
}

static void condition(CompileUnit* cu, bool can_assign) {
    u32 false_branch_start = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE);
    expression(cu, BP_LOWEST); // true branch
    consume_cur_token(cu->parser, TOKEN_COLON, "expect ':' after true branch!");
    u32 expr_end = emit_instr_with_placeholder(cu, OPCODE_JMP);
    
    patch_placeholder(cu, false_branch_start);
    expression(cu, BP_LOWEST); // false branch

    patch_placeholder(cu, expr_end);
}

inline static void define_variable(CompileUnit* cu, u32 index) {
    // 局部变量在栈中存储，不需要处理
    // 模块变量写回相应位置
    if (cu->scope_depth == -1) {
        write_opcode_short_operand(cu, OPCODE_STORE_MODULE_VAR, index);
        write_opcode(cu, OPCODE_POP);
    }
}

static Variable find_variable(CompileUnit* cu, const char* name, u32 len) {
    Variable var = get_var_from_local_or_upvalue(cu, name, len);
    if (var.index != -1) {
        return var;
    }

    var.index = get_index_from_symbol_table(&cu->parser->cur_module->module_var_name, name, len);
    if (var.index != -1) {
        var.scope_type = VAR_SCOPE_MODULE;
    }
    
    return var;
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

    if (cu->enclosing_unit == NULL && cu->enclosing_classbk != NULL) {
        // class field definition

        if (is_static) { // static field
            char* static_field_id = ALLOCATE_ARRAY(cu->parser->vm, char, MAX_ID_LEN);
            memset(static_field_id, 0, MAX_ID_LEN);
            u32 static_field_id_len = 0;
            
            char* cls_name = cu->enclosing_classbk->name->val.start;
            u32 cls_name_len = cu->enclosing_classbk->name->val.len;

            // ClsClass@static_var_name
            memmove(static_field_id, "Cls", 3);
            memmove(static_field_id + 3, cls_name, cls_name_len);
            memmove(static_field_id + 3 + cls_name_len, "@", 1);
            memmove(static_field_id + 4 + cls_name_len, name.start, name.len);

            static_field_id_len = strlen(static_field_id);

            if (find_local(cu, static_field_id, static_field_id_len) != -1) {
                COMPILE_ERROR(
                    cu->parser,
                    "static field '%s', redefinition.", static_field_id
                );
            }

            declare_local_var(cu, static_field_id, static_field_id_len);

            ASSERT(cu->scope_depth == 0, "should in class scope.");

            // 若有初始值，则解析；若无，则初始化为null
            if (match_token(cu->parser, TOKEN_ASSIGN)) {
                expression(cu, BP_LOWEST);
            } else {
                write_opcode(cu, OPCODE_PUSH_NULL);
            }

            consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of variable definition.");

            return;
        }
        
        // 实例域
        ClassBookKeep* class_bk = get_enclosing_classbk(cu);
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
        write_opcode(cu, OPCODE_PUSH_NULL);
    }

    u32 index = declare_variable(cu, name.start, name.len);
    define_variable(cu, index);

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of variable definition.");
}

static void compile_stmt(CompileUnit* cu);

static void compile_if_stmt(CompileUnit* cu) {
    expression(cu, BP_LOWEST); // condition
    u32 else_block_start = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE);

    compile_stmt(cu); // then block

    if (match_token(cu->parser, TOKEN_ELSE)) {
        u32 stmt_end = emit_instr_with_placeholder(cu, OPCODE_JMP);
        patch_placeholder(cu, else_block_start);

        compile_stmt(cu); // else block

        patch_placeholder(cu, stmt_end);
    } else {
        patch_placeholder(cu, else_block_start);
    }
}

static void enter_loop_setting(CompileUnit* cu, Loop* loop) {
    loop->cond_start_index = cu->fn->instr_stream.count - 1;
    loop->scope_depth = cu->scope_depth;
    loop->enclosing_loop = cu->cur_loop;
    cu->cur_loop = loop;
}

static void compile_loop_body(CompileUnit* cu) {
    cu->cur_loop->body_start_index = cu->fn->instr_stream.count;
    compile_stmt(cu);
}

static void leave_loop_patch(CompileUnit* cu) {
    int loop_back_offset = cu->fn->instr_stream.count - cu->cur_loop->cond_start_index + 2;
    write_opcode_short_operand(cu, OPCODE_LOOP, loop_back_offset);
    
    patch_placeholder(cu, cu->cur_loop->exit_index);

    u32 loop_end_index = cu->fn->instr_stream.count;

    for (u32 i = cu->cur_loop->body_start_index; i < loop_end_index; ) {
        if (cu->fn->instr_stream.datas[i] == OPCODE_END) {
            cu->fn->instr_stream.datas[i] = OPCODE_JMP;
            patch_placeholder(cu, i + 1);
            i += 3;
            continue;
        }
        i += 1 + get_byte_of_operands(cu->fn->instr_stream.datas, cu->fn->constants.datas, i);
    }

    cu->cur_loop = cu->cur_loop->enclosing_loop;
}

static void compile_while_stmt(CompileUnit* cu) {
    Loop loop;
    
    enter_loop_setting(cu, &loop);

    expression(cu, BP_LOWEST); // condition

    loop.exit_index = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE);

    compile_loop_body(cu);

    leave_loop_patch(cu);
}

static u32 discard_local_var(CompileUnit* cu, int scope_depth) {
    ASSERT(cu->scope_depth > -1, "upmost scope can't exit.");
    int local_index = cu->local_vars_count - 1;
    while (local_index >= 0 && cu->local_vars[local_index].scope_depth >= scope_depth) {
        if (cu->local_vars[local_index].is_upvalue) {
            write_byte(cu, OPCODE_CLOSE_UPVALUE);
        } else {
            write_byte(cu, OPCODE_POP);
        }
        local_index--;
    }
    return cu->local_vars_count - 1 - local_index;
}

inline static void compile_return_stmt(CompileUnit* cu) {
    // 编译返回值
    if (match_token(cu->parser, TOKEN_SEMICOLON)) {
        write_opcode(cu, OPCODE_PUSH_NULL);
    } else {
        expression(cu, BP_LOWEST);
        consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of return statment.");
    }

    write_opcode(cu, OPCODE_RETURN);
}

inline static void compile_break_stmt(CompileUnit* cu) {
    if (cu->cur_loop == NULL) {
        COMPILE_ERROR(cu->parser, "break statment should be used inside a loop");
    }

    // 清除循环体内的局部变量
    discard_local_var(cu, cu->cur_loop->scope_depth + 1);

    // 填充end表示break占位
    emit_instr_with_placeholder(cu, OPCODE_END);

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of break statment.");
}

inline static void compile_continue_stmt(CompileUnit* cu) {
    if (cu->cur_loop == NULL) {
        COMPILE_ERROR(cu->parser, "continue should be used inside a loop.");
    }

    discard_local_var(cu, cu->cur_loop->scope_depth + 1);

    int loop_back_offset = cu->fn->instr_stream.count - cu->cur_loop->cond_start_index + 2;
    write_opcode_short_operand(cu, OPCODE_LOOP, loop_back_offset);

    consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of continue statment.");
}

static void enter_scope(CompileUnit* cu) {
    cu->scope_depth++;
}

static void leave_scope(CompileUnit* cu) {
    u32 discard_num = discard_local_var(cu, cu->scope_depth);
    cu->local_vars_count -= discard_num;
    cu->stack_slot_num -= discard_num;

    cu->scope_depth--;
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

    enter_scope(cu); // for-loop scope

    consume_cur_token(cu->parser, TOKEN_ID, "expect variable name after for.");
    Token loop_var_name = cu->parser->pre_token;

    consume_cur_token(cu->parser, TOKEN_IN, "expect 'in' after loop var name.");

    // let for@seq = sequnce;
    expression(cu, BP_LOWEST);
    u32 seq = add_local_var(cu, "for@seq", 7);

    // let for@iter = null;
    write_opcode(cu, OPCODE_PUSH_NULL);
    u32 iter = add_local_var(cu, "for@iter", 8);

    // while-loop
    Loop loop;
    enter_loop_setting(cu, &loop);

    // while-loop condition: for@iter = for@seq.iterate(for@iter)
    write_opcode_byte_operand(cu, OPCODE_LOAD_LOCAL_VAR, seq);
    write_opcode_byte_operand(cu, OPCODE_LOAD_LOCAL_VAR, iter);
    emit_call(cu, 1, "iterate(_)", 10);
    write_opcode_byte_operand(cu, OPCODE_STORE_LOCAL_VAR, iter);

    loop.exit_index = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE);

    // let i = for@seq.iterator_value(for@iter);
    write_opcode_byte_operand(cu, OPCODE_LOAD_LOCAL_VAR, seq);
    write_opcode_byte_operand(cu, OPCODE_LOAD_LOCAL_VAR, iter);
    emit_call(cu, 1, "iterator_value(_)", 17);

    enter_scope(cu); // i
    add_local_var(cu, loop_var_name.start, loop_var_name.len);

    compile_loop_body(cu);

    leave_scope(cu); // i
    leave_loop_patch(cu);
    leave_scope(cu); // for@
}

static void emit_store_module_var(CompileUnit* cu, int index) {
    write_opcode_short_operand(cu, OPCODE_STORE_MODULE_VAR, index);
    write_opcode(cu, OPCODE_POP);
}

static int declare_method(CompileUnit* cu, char* sign_str, u32 len) {
    VM* vm = cu->parser->vm;
    int index = ensure_symbol_exist(vm, &vm->all_method_names, sign_str, len);

    ClassBookKeep* cls = cu->enclosing_classbk;
    
    IntBuffer* methods = cls->in_static ? &cls->static_methods : &cls->instant_methods;
    for (int i = 0; i < methods->count; i++) {
        if (methods->datas[i] == index) {
            COMPILE_ERROR(
                cu->parser,
                "repeat define method '%s' in class %s.", sign_str, cls->name->val.len
            );
        }
    }

    BufferAdd(Int, methods, vm, index);
    return index;
}

static void define_method(CompileUnit* cu, Variable class, bool is_static, int method_index) {
    // 将栈顶的方法存入class中
    // 1. 将class压入栈顶
    emit_load_variable(cu, class);
    // 2. 使用STATIC_METHOD or INSTANCE_METHOD关键字
    OpCode opcode = is_static ? OPCODE_STATIC_METHOD : OPCODE_INSTANCE_METHOD;
    write_opcode_short_operand(cu, opcode, method_index);
}

static void emit_create_instance(CompileUnit* cu, Signature* sign, u32 constructor_index) {
    CompileUnit method_cu;
    compileunit_init(cu->parser, &method_cu, cu, true);
    
    // 1. push instance to stack[0]
    write_opcode(&method_cu, OPCODE_CONSTRUCT);
    // 2. call Class.new(...)
    write_opcode_short_operand(&method_cu, (OpCode)(OPCODE_CALL0 + sign->argc), constructor_index);
    // 3. return instance
    write_opcode(&method_cu, OPCODE_RETURN);

#if DEBUG
    end_compile_unit(&method_cu, "", 0);
#else
    end_compile_unit(&method_cu);
#endif
}

static void compile_method(CompileUnit* cu, Variable class, bool is_static) {
    ClassBookKeep* cls = cu->enclosing_classbk;
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
    compileunit_init(cu->parser, &method_cu, cu, true);

    method_sign(&method_cu, &sign); // 解析函数签名
    if (cls->in_static && sign.type == SIGN_CONSTRUCT) {
        COMPILE_ERROR(cu->parser, "constructor is not allowed to be static.");
    }
    
    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' at the beginning of method body.");

    // method declare
    char sign_str[MAX_SIGN_LEN] = {'\0'};
    u32 sign_len = sign2string(&sign, sign_str);
    u32 method_index = declare_method(cu, sign_str, sign_len);

    compile_body(&method_cu, sign.type == SIGN_CONSTRUCT); // method body

#if DEBUG
    end_compile_unit(&method_cu, sign_str, sign_len);
#else
    end_compile_unit(&method_cu);
#endif

    define_method(cu, class, cls->in_static, method_index); // bind method to class

    if (sign.type == SIGN_CONSTRUCT) {
        // 若为构造函数，则为其添加一个对应的static函数定义

        sign.type = SIGN_METHOD;
        
        VM* vm = cu->parser->vm;
        u32 constructor_index = ensure_symbol_exist(vm, &vm->all_method_names, sign_str, sign_len);

        emit_create_instance(cu, &sign, method_index);
        
        define_method(cu, class, true, constructor_index);
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
    if (cu->scope_depth != -1) {
        COMPILE_ERROR(cu->parser, "class definition must be in the module scope.");
    }

    consume_cur_token(cu->parser, TOKEN_ID, "need a name for class.");
    Token name = cu->parser->pre_token;

    Variable class;
    class.scope_type = VAR_SCOPE_MODULE;
    class.index = declare_variable(cu, name.start, name.len);

    ObjString* class_name = objstring_new(cu->parser->vm, name.start, name.len);
    emit_load_constant(cu, OBJ_TO_VALUE(class_name));

    if (match_token(cu->parser, TOKEN_LT)) {
        expression(cu, BP_CALL); // 继承的父类
    } else {
        emit_load_module_var(cu, "Object"); // 默认继承Object
    }

    int field_num_index = write_opcode_byte_operand(cu, OPCODE_CREATE_CLASS, 0xFF); // 未知字段数，填入0xFF占位
    emit_store_module_var(cu, class.index);

    ClassBookKeep class_bk = {
        .name = class_name,
        .in_static = false,
    };
    BufferInit(String, &class_bk.fields);
    BufferInit(Int, &class_bk.instant_methods);
    BufferInit(Int, &class_bk.static_methods);

    cu->enclosing_classbk = &class_bk;

    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' in the start of class-define-body.");

    enter_scope(cu);

    while (!match_token(cu->parser, TOKEN_RC)) {
        compile_class_body(cu, class);
        if (PEEK_TOKEN(cu->parser) == TOKEN_EOF) {
            COMPILE_ERROR(cu->parser, "expect '}' in the end of class-define-body.");
        }
    }

    cu->fn->instr_stream.datas[field_num_index] = class_bk.fields.count; // 回填字段数

    symbol_table_clear(cu->parser->vm, &class_bk.fields);
    BufferClear(Int, &class_bk.instant_methods, cu->parser->vm);
    BufferClear(Int, &class_bk.static_methods, cu->parser->vm);

    cu->enclosing_classbk = NULL;

    leave_scope(cu);
}

static void compile_function_definition(CompileUnit* cu) {
    if (cu->enclosing_unit != NULL) {
        COMPILE_ERROR(cu->parser, "'fn' should be in module scope.");
    }

    consume_cur_token(cu->parser, TOKEN_ID, "missing function name.");

    char fn_name[MAX_SIGN_LEN + 4] = {'\0'};
    memmove(fn_name, "Fn@", 3);
    memmove(fn_name + 3, cu->parser->pre_token.start, cu->parser->pre_token.len);
    u32 fn_name_len = strlen(fn_name);

    u32 fn_name_index = declare_variable(cu, fn_name, fn_name_len);

    CompileUnit fncu;
    compileunit_init(cu->parser, &fncu, cu, false);

    Signature tmp_fn_sign = {SIGN_METHOD, "", 0, 0};
    
    consume_cur_token(cu->parser, TOKEN_LP, "expect '(' after function name.");
    if (!match_token(cu->parser, TOKEN_RP)) {
        process_para_list(&fncu, &tmp_fn_sign);
        consume_cur_token(cu->parser, TOKEN_RP, "expect ')' after parameter list.");
    }
    
    FUNCTION_RESULT_TYPPING_CHECK();

    fncu.fn->argc = tmp_fn_sign.argc;

    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' at the beginning of method body.");

    compile_body(&fncu, false);

#if DEBUG
    end_compile_unit(&fncu, fn_name, fn_name_len);
#else
    end_compile_unit(&fncu);
#endif

    define_variable(cu, fn_name_index);
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
    u32 const_name_inedx = add_constant(cu, OBJ_TO_VALUE(module_name));

    if (buf != NULL) {
        free(buf);
    }

    buf = NULL;

    if (mode == 0) {
        emit_load_module_var(cu, "System");
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_name_inedx);
        emit_call(cu, 1, "import_module(_)", 16);
    } else if (mode == 1) {
        emit_load_module_var(cu, "System");
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_name_inedx);
        emit_call(cu, 1, "import_std_module(_)", 20);
    } else if (mode == 2) {
        emit_load_module_var(cu, "System");
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_name_inedx);
        emit_call(cu, 1, "import_lib_module(_)", 20);
    }
    write_opcode(cu, OPCODE_POP);

    if (match_token(cu->parser, TOKEN_SEMICOLON)) {
        return;
    }

    consume_cur_token(cu->parser, TOKEN_FOR, "miss match token 'for' or ';'.");

    do {
        consume_cur_token(cu->parser, TOKEN_ID, "expect variable name after 'for' in import.");
        u32 var_index = declare_variable(cu, cu->parser->pre_token.start, cu->parser->pre_token.len);
        ObjString* var_name = objstring_new(cu->parser->vm, cu->parser->pre_token.start, cu->parser->pre_token.len);
        u32 const_var_name = add_constant(cu, OBJ_TO_VALUE(var_name));
        
        // $top = System.get_module_variable("foo", "bar");
        emit_load_module_var(cu, "System");
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_name_inedx);
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_var_name);
        emit_call(cu, 2, "get_module_variable(_,_)", 24);

        // bar = $top
        define_variable(cu, var_index);
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
        enter_scope(cu);
        compile_block(cu);
        leave_scope(cu);
    } else {
        expression(cu, BP_LOWEST);
        write_opcode(cu, OPCODE_POP);
        consume_cur_token(cu->parser, TOKEN_SEMICOLON, "expect ';' in the end of expression statment.");
    }
}

static void compile_program(CompileUnit* cu) {
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

ObjFn* one_pass_compile_module(VM* vm, ObjModule* module, const char* module_code) {
    Parser parser;
    parser.parent = vm->cur_parser;
    vm->cur_parser = &parser;
    
    init_parser(
        vm, &parser, 
        module->name == NULL ? "core.script.inc" : (const char*)module->name->val.start,
        module_code, module
    );

    CompileUnit module_cu;
    compileunit_init(&parser, &module_cu, NULL, false);

    u32 module_var_number_befor = module->module_var_value.count;

    get_next_token(&parser);

    while (!match_token(&parser, TOKEN_EOF)) {
        compile_program(&module_cu);
    }

    write_opcode(&module_cu, OPCODE_PUSH_NULL);
    write_opcode(&module_cu, OPCODE_RETURN);

    for (int i = module_var_number_befor; i < module->module_var_value.count; i++) {
        if (VALUE_IS_I32(module->module_var_value.datas[i])) {
            char* str = module->module_var_name.datas[i].str;
            ASSERT(str[module->module_var_name.datas[i].len] == '\0', "module var name not a std-cstring(end with '0').");

            u32 line = VALUE_TO_I32(module->module_var_value.datas[i]);
            COMPILE_ERROR(&parser, "in line %d: module variable '%s' not defined.", line, str);
        }
    }

    vm->cur_parser->cur_compile_unit = NULL;
    vm->cur_parser = vm->cur_parser->parent;

#if DEBUG
    return end_compile_unit(&module_cu, "(script)", 8);
#else
    return end_compile_unit(&module_cu);
#endif
}

// fn.nud
static void closure_expr(CompileUnit* cu, bool can_assign) {
    // fn(n: i32) -> i32 { return n * n; }

    CompileUnit fncu;
    compileunit_init(cu->parser, &fncu, cu, false);

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

    fncu.fn->argc = temp_func_sign.argc;

    consume_cur_token(cu->parser, TOKEN_LC, "expect '{' for closure body.");

    compile_body(&fncu, false);

    #if DEBUG
        char* fn_name = "@unnamed-closure";
        end_compile_unit(&fncu, fn_name, 16);
    #else
        end_compile_unit(&fncu);
    #endif

    // 函数闭包已在栈顶
}
