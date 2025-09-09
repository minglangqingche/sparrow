#include "compiler.h"
#include "vm.h"
#include "class.h"
#include "core.h"

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
            UNREACHABLE();
    }
    #undef CASE
}

int ensure_symbol_exist(VM* vm, SymbolTable* table, const char* symbol, u32 len) {
    int symbol_index = get_index_from_symbol_table(table, symbol, len);
    return symbol_index == -1 ? add_symbol(vm, table, symbol, len) : symbol_index;
}
