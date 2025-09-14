#include "compiler.h"
#include "meta_obj.h"
#include "vm.h"
#include "class.h"
#include "core.h"
#include "parser.h"
#include <stdlib.h>
#include <string.h>

#define OPCODE_SLOTS(opcode, effect) effect,
const int opcode_slots_used[] = {
    #include "opcode.inc"
};
#undef OPCODE_SLOTS

// 编译器工具函数
inline int write_byte(CompileUnitPubStruct* cu, int byte) {
    BufferAdd(Byte, &cu->fn->instr_stream, cu->vm, (u8)byte);
    return cu->fn->instr_stream.count - 1;
}

inline void write_opcode(CompileUnitPubStruct* cu, OpCode op) {
    write_byte(cu, op);
    cu->stack_slot_num += opcode_slots_used[op];
    if (cu->stack_slot_num > cu->fn->max_stack_slot_used) {
        cu->fn->max_stack_slot_used = cu->stack_slot_num;
    }
}

inline int write_byte_operand(CompileUnitPubStruct* cu, int operand) {
    return write_byte(cu, operand);
}

inline void write_short_operand(CompileUnitPubStruct* cu, int operand) {
    write_byte(cu, (operand >> 8) & 0xFF);
    write_byte(cu, operand & 0xFF);
}

inline int write_opcode_byte_operand(CompileUnitPubStruct* cu, OpCode opcode, int operand) {
    write_opcode(cu, opcode);
    return write_byte_operand(cu, operand);
}

inline void write_opcode_short_operand(CompileUnitPubStruct* cu, OpCode opcode, int operand) {
    write_opcode(cu, opcode);
    write_short_operand(cu, operand);
}

u32 add_constant(CompileUnitPubStruct* cu, Value constant) {
    if (VALUE_IS_OBJ(constant)) {
        push_tmp_root(cu->vm, constant.header);
    }

    // 避免重复常量重复入表
    for (u32 i = 0; i < cu->fn->constants.count; i++) {
        Value val = cu->fn->constants.datas[i];
        if (value_is_equal(val, constant)) {
            if (VALUE_IS_OBJ(constant)) {
                pop_tmp_root(cu->vm);
            }
            return i;
        }
    }

    // 表中没有相同的常量，入表
    BufferAdd(Value, &cu->fn->constants, cu->vm, constant);

    if (VALUE_IS_OBJ(constant)) {
        pop_tmp_root(cu->vm);
    }
    return cu->fn->constants.count - 1;
}

int find_local(CompileUnitPubStruct* cu, const char* name, u32 len) {
    for (int index = cu->local_vars_count - 1; index >= 0; index--) {
        LocalVar* t = &cu->local_vars[index];
        if (t->len == len && memcmp(t->name, name, len) == 0) {
            return index;
        }
    }
    return -1;
}

int find_upvalue(CompileUnitPubStruct* cu, const char* name, u32 len) {
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

int add_upvalue(CompileUnitPubStruct* cu, bool is_enclosing_local_var, u32 index) {
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

Variable get_var_from_local_or_upvalue(CompileUnitPubStruct* cu, const char* name, u32 len) {
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

inline void emit_load_constant(CompileUnitPubStruct* cu, Value val) {
    int index = add_constant(cu, val);
    write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, index);
}

void emit_load_variable(CompileUnitPubStruct* cu, Variable var) {
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
        case VAR_SCOPE_METHOD_FIELD:
            write_opcode_byte_operand(cu, OPCODE_LOAD_SELF_FIELD, var.index);
            break;
        default:
            UNREACHABLE();
    }
}

void emit_store_variable(CompileUnitPubStruct* cu, Variable var) {
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
        case VAR_SCOPE_METHOD_FIELD:
            write_opcode_byte_operand(cu, OPCODE_STORE_SELF_FIELD, var.index);
            break;
        default:
            UNREACHABLE();
    }
}

inline void emit_load_self(CompileUnitPubStruct* cu) {
    Variable var = get_var_from_local_or_upvalue(cu, "self", 4);
    ASSERT(var.scope_type != VAR_SCOPE_INVALID, "get self failed!");
    emit_load_variable(cu, var);
}

void emit_call_by_signature(CompileUnitPubStruct* cu, Signature* sign, OpCode op) {
    char sign_buffer[MAX_SIGN_LEN];
    u32 len = sign2string(sign, sign_buffer);

    int symbol_index = ensure_symbol_exist(
        cu->vm, &cu->vm->all_method_names,
        sign_buffer, len
    );
    write_opcode_short_operand(cu, op + sign->argc, symbol_index);

    if (op == OPCODE_SUPER0) {
        // 为将来需要填入的基类占位
        write_short_operand(cu, add_constant(cu, VT_TO_VALUE(VT_NULL)));
    }
}

u32 add_local_var(CompileUnitPubStruct* cu, const char* name, u32 len) {
    LocalVar* var = &(cu->local_vars[cu->local_vars_count]);

    var->name = name;
    var->len = len;
    var->scope_depth = cu->scope_depth;
    var->is_upvalue = false;

    return cu->local_vars_count++;
}

int declare_local_var(CompileUnitPubStruct* cu, const char* name, u32 len) {
    if (cu->local_vars_count >= MAX_LOCAL_VAR_NUM) {
        COMPILE_ERROR(
            cu->vm->cur_parser,
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
                cu->vm->cur_parser,
                "local variable '%s' redefinition.", id
            );
        }
    }

    return add_local_var(cu, name, len);
}

int declare_variable(CompileUnitPubStruct* cu, const char* name, u32 len) {
    if (cu->scope_depth == -1) {
        int index = define_module_var(
            cu->vm, cu->cur_module,
            name, len, VT_TO_VALUE(VT_NULL)
        );

        if (index == -1) {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, len);
            COMPILE_ERROR(
                cu->vm->cur_parser,
                "module variable '%s' redefinition.", id
            );
        }

        return index;
    }

    return declare_local_var(cu, name, len);
}

void emit_call(CompileUnitPubStruct* cu, int argc, const char* name, u32 len) {
    int symbol_index = ensure_symbol_exist(
        cu->vm, &cu->vm->all_method_names,
        name, len
    );

    write_opcode_short_operand(cu, OPCODE_CALL0 + argc, symbol_index);
}

u32 sign2string(Signature* sign, char* buf) {
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

// 编译期用于声明模块变量，val正常声明时为null，在定义前使用为第一次出现的行号
int define_module_var(VM* vm, ObjModule* module, const char* name, u32 len, Value val) {
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

void compile_unit_pubstruct_init(VM* vm, ObjModule* cur_module, CompileUnitPubStruct* cu, CompileUnitPubStruct* enclosing_unit, bool is_method) {
    cu->vm = vm;
    vm->cur_cu = cu;
    cu->cur_module = cur_module;
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
    cu->fn = objfn_new(cu->vm, cu->cur_module, cu->local_vars_count);
}

u32 get_byte_of_operands(Byte* instr_stream, Value* constants, int ip) {
    // 获得操作码占用字节数

    #define CASE(code) case OPCODE_##code
    switch ((OpCode)instr_stream[ip]) {
        CASE(CONSTRUCT):
        CASE(RETURN):
        CASE(END):
        CASE(CLOSE_UPVALUE):
        CASE(PUSH_NULL):
        CASE(PUSH_FALSE):
        CASE(PUSH_TRUE):
        CASE(POP):
            return 0;
        
        CASE(CREATE_CLASS):
        CASE(LOAD_SELF_FIELD):
        CASE(STORE_SELF_FIELD):
        CASE(LOAD_FIELD):
        CASE(STORE_FIELD):
        CASE(LOAD_LOCAL_VAR):
        CASE(STORE_LOCAL_VAR):
        CASE(LOAD_UPVALUE):
        CASE(STORE_UPVALUE):
            return 1;

        CASE(CALL0):
        CASE(CALL1):
        CASE(CALL2):
        CASE(CALL3):
        CASE(CALL4):
        CASE(CALL5):
        CASE(CALL6):
        CASE(CALL7):
        CASE(CALL8):
        CASE(CALL9):
        CASE(CALL10):
        CASE(CALL11):
        CASE(CALL12):
        CASE(CALL13):
        CASE(CALL14):
        CASE(CALL15):
        CASE(CALL16):
        CASE(LOAD_CONSTANT):
        CASE(LOAD_MODULE_VAR):
        CASE(STORE_MODULE_VAR):
        CASE(LOOP):
        CASE(JMP):
        CASE(JMP_IF_FALSE):
        CASE(AND):
        CASE(OR):
        CASE(INSTANCE_METHOD):
        CASE(STATIC_METHOD):
            return 2;

        CASE(SUPER0):
        CASE(SUPER1):
        CASE(SUPER2):
        CASE(SUPER3):
        CASE(SUPER4):
        CASE(SUPER5):
        CASE(SUPER6):
        CASE(SUPER7):
        CASE(SUPER8):
        CASE(SUPER9):
        CASE(SUPER10):
        CASE(SUPER11):
        CASE(SUPER12):
        CASE(SUPER13):
        CASE(SUPER14):
        CASE(SUPER15):
        CASE(SUPER16):
            return 4;

        CASE(CREATE_CLOSURE): {
            u32 fn_idx = (instr_stream[ip + 1] << 8) | instr_stream[ip + 2];
            return 2 + (VALUE_TO_OBJFN(constants[fn_idx])->upvalue_number * 2);
        }

        default:
            fprintf(stderr, "[%d] %d\n", ip, (OpCode)instr_stream[ip]);
            UNREACHABLE();
    }
    #undef CASE
}

int ensure_symbol_exist(VM* vm, SymbolTable* table, const char* symbol, u32 len) {
    int symbol_index = get_index_from_symbol_table(table, symbol, len);
    return symbol_index == -1 ? add_symbol(vm, table, symbol, len) : symbol_index;
}

int declare_module_var(VM* vm, ObjModule* module, const char* name, u32 len, Value val) {
    BufferAdd(Value, &module->module_var_value, vm, val);
    return add_symbol(vm, &module->module_var_name, name, len);
}

CompileUnitPubStruct* get_enclosing_classbk_unit(CompileUnitPubStruct* cu) {
    while (cu != NULL) {
        if (cu->enclosing_classbk != NULL) {
            return cu;
        }
        cu = cu->enclosing_unit;
    }
    return NULL;
}

ClassBookKeep* get_enclosing_classbk(CompileUnitPubStruct* cu) {
    CompileUnitPubStruct* ncu = get_enclosing_classbk_unit(cu);
    return ncu == NULL ? NULL : ncu->enclosing_classbk;
}

ObjFn* end_compile_unit(CompileUnitPubStruct* cu) {
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

    return cu->fn;
}

void emit_load_module_var(CompileUnitPubStruct* cu, const char* name) {
    int index = get_index_from_symbol_table(
        &cu->cur_module->module_var_name,
        name, strlen(name)
    );
    
    ASSERT(index != -1, "symbol should have been defined.");

    write_opcode_short_operand(cu, OPCODE_LOAD_MODULE_VAR, index);
}

u32 emit_instr_with_placeholder(CompileUnitPubStruct* cu, OpCode opcode) {
    write_opcode(cu, opcode);
    u32 hp = write_byte(cu, 0xFF); // 填充高位
    write_byte(cu, 0xFF); // 填充低位
    return hp; // 返回高位地址
}

void patch_placeholder(CompileUnitPubStruct* cu, u32 abs_index) {
    u32 offset = cu->fn->instr_stream.count - abs_index - 2; // 计算跳转量
    cu->fn->instr_stream.datas[abs_index] = (offset >> 8) & 0xFF; // 填入跳转量的高八位
    cu->fn->instr_stream.datas[abs_index + 1] = offset & 0xFF; // 填入跳转量的低八位
}

inline void define_variable(CompileUnitPubStruct* cu, u32 index) {
    // 局部变量在栈中存储，不需要处理
    // 模块变量写回相应位置
    if (cu->scope_depth == -1) {
        write_opcode_short_operand(cu, OPCODE_STORE_MODULE_VAR, index);
        write_opcode(cu, OPCODE_POP);
    }
}

Variable find_variable(CompileUnitPubStruct* cu, const char* name, u32 len) {
    Variable var = get_var_from_local_or_upvalue(cu, name, len);
    if (var.index != -1) {
        return var;
    }

    var.index = get_index_from_symbol_table(&cu->cur_module->module_var_name, name, len);
    if (var.index != -1) {
        var.scope_type = VAR_SCOPE_MODULE;
    }
    
    return var;
}

void enter_loop_setting(CompileUnitPubStruct* cu, Loop* loop) {
    loop->cond_start_index = cu->fn->instr_stream.count - 1;
    loop->scope_depth = cu->scope_depth;
    loop->enclosing_loop = cu->cur_loop;
    cu->cur_loop = loop;
}

void leave_loop_patch(CompileUnitPubStruct* cu) {
    int loop_back_offset = cu->fn->instr_stream.count - cu->cur_loop->cond_start_index + 2;
    write_opcode_short_operand(cu, OPCODE_LOOP, loop_back_offset);

    if (cu->cur_loop->exit_index != -1) {
        // 若 exit_index == -1，则说明条件被优化，无需回填退出位置。
        patch_placeholder(cu, cu->cur_loop->exit_index);
    }

    u32 loop_end_index = cu->fn->instr_stream.count;

    // 将原本占位的END替换为正确的break语句
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

u32 discard_local_var(CompileUnitPubStruct* cu, int scope_depth) {
    ASSERT(cu->scope_depth > -1, "toplevel scope can't exit.");
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

inline void enter_scope(CompileUnitPubStruct* cu) {
    cu->scope_depth++;
}

void leave_scope(CompileUnitPubStruct* cu) {
    u32 discard_num = discard_local_var(cu, cu->scope_depth);
    cu->local_vars_count -= discard_num;
    cu->stack_slot_num -= discard_num;
    cu->scope_depth--;
}

void emit_store_module_var(CompileUnitPubStruct* cu, int index) {
    write_opcode_short_operand(cu, OPCODE_STORE_MODULE_VAR, index);
    write_opcode(cu, OPCODE_POP);
}

int declare_method(CompileUnitPubStruct* cu, char* sign_str, u32 len) {
    ClassBookKeep* cls = cu->enclosing_classbk;
    // printf(">>> declare %s.%s\n", cls->name->val.start, sign_str);
    VM* vm = cu->vm;
    int index = ensure_symbol_exist(vm, &vm->all_method_names, sign_str, len);
    
    IntBuffer* methods = cls->in_static ? &cls->static_methods : &cls->instant_methods;
    for (int i = 0; i < methods->count; i++) {
        if (methods->datas[i] == index) {
            COMPILE_ERROR(
                vm->cur_parser,
                "redefine method '%s' in class %s.", sign_str, cls->name->val.start
            );
        }
    }

    BufferAdd(Int, methods, vm, index);
    return index;
}

void define_method(CompileUnitPubStruct* cu, Variable class, bool is_static, int method_index) {
    // 将栈顶的方法存入class中
    // 1. 将class压入栈顶
    emit_load_variable(cu, class);
    // 2. 使用STATIC_METHOD or INSTANCE_METHOD关键字
    OpCode opcode = is_static ? OPCODE_STATIC_METHOD : OPCODE_INSTANCE_METHOD;
    write_opcode_short_operand(cu, opcode, method_index);
}

void emit_create_instance(CompileUnitPubStruct* cu, Signature* sign, u32 constructor_index) {
    CompileUnitPubStruct method_cu;
    compile_unit_pubstruct_init(cu->vm, cu->cur_module, &method_cu, cu, true);
    
    // 1. push instance to stack[0]
    write_opcode(&method_cu, OPCODE_CONSTRUCT);
    // 2. call Class.new(...)
    write_opcode_short_operand(&method_cu, (OpCode)(OPCODE_CALL0 + sign->argc), constructor_index);
    // 3. return instance
    write_opcode(&method_cu, OPCODE_RETURN);

    end_compile_unit(&method_cu);
}

ObjFn* compile_module(VM* vm, ObjModule* module, const char* module_code, CompileProgram compile_program) {
    Parser parser;
    parser.parent = vm->cur_parser;
    vm->cur_parser = &parser;
    
    init_parser(
        vm, &parser, 
        module->name == NULL ? "core.script.inc" : (const char*)module->name->val.start,
        module_code
    );

    CompileUnitPubStruct module_cu;
    compile_unit_pubstruct_init(vm, module, &module_cu, NULL, false);

    u32 module_var_number_befor = module->module_var_value.count;

    get_next_token(&parser);

    compile_program(&module_cu, &parser);

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

    vm->cur_parser = vm->cur_parser->parent;
    vm->cur_cu = NULL;

    return end_compile_unit(&module_cu);
}
