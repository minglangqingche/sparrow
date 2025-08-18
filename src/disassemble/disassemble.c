#include "disassemble.h"
#include "class.h"
#include "compiler.h"
#include "header_obj.h"
#include "meta_obj.h"
#include "obj_fn.h"
#include "obj_range.h"
#include "vm.h"
#include <stdio.h>

#define OPCODE_SLOTS(op, slot) [OPCODE_##op] = #op,
static const char* op_name_map[] = {
    #include "opcode.inc"
};
#undef OPCODE_SLOTS

void print_value(Value* val) {
    switch (val->type) {
        case VT_I32:
            printf("<i32 %d>", val->ival);
            break;
        case VT_F64:
            printf("<f64 %lf>", val->fval);
            break;
        case VT_TRUE:
            printf("<bool true>");
            break;
        case VT_FALSE:
            printf("<bool false>");
            break;
        case VT_NULL:
            printf("<null>");
            break;
        case VT_UNDEFINED:
            printf("<?undefined?>");
            break;
        case VT_OBJ: {
            ObjHeader* obj = val->header;
            switch (obj->type) {
                case OT_STRING: {
                    printf("<String '%s'>", ((ObjString*)obj)->val.start);
                    break;
                }

                case OT_CLASS: {
                    printf("<Class %s>", ((Class*)obj)->name->val.start);
                    break;
                }

                case OT_INSTANCE: {
                    printf("<Instance of %s>", ((ObjInstance*)obj)->header.class->name->val.start);
                    break;
                }

                case OT_RANGE: {
                    printf("<Range %d %d %d>", ((ObjRange*)obj)->from, ((ObjRange*)obj)->to, ((ObjRange*)obj)->step);
                    break;
                }

                case OT_LIST: {
                    printf("<List at %p>", obj);
                    break;
                }

                case OT_FUNCTION: {
                    printf("<Function at %p>", obj);
                    break;
                }

                case OT_MAP: {
                    printf("<Map at %p>", obj);
                    break;
                }

                case OT_MODULE: {
                    printf("<Module at %p>", obj);
                    break;
                }

                case OT_THREAD: {
                    printf("<Thread at %p>", obj);
                    break;
                }

                case OT_UPVALUE: {
                    printf("<Upvalue at %p>", obj);
                    break;
                }

                case OT_CLOSURE: {
                    printf("<Closure at %p>", obj);
                    break;
                }

                default: {
                    printf("<Obj? at %p>", obj);
                    break;
                }
            }
        }
    }
}

void dis_asm(VM* vm, ObjModule* module, ObjFn* chunk) {
    int ip = 0;
    while (true) {
        OpCode op = chunk->instr_stream.datas[ip++];
        const char* name = op_name_map[op];
        int operand_byte = get_byte_of_operands(chunk->instr_stream.datas, chunk->constants.datas, ip - 1);
        printf("%5d %-25s", (ip - 1), name);

        if (operand_byte == 4 && op != OPCODE_CREATE_CLOSURE) {
            // SUPERX [2b] [2b]
            ip += 4;
            int operand1 = (u16)(chunk->instr_stream.datas[ip - 4] << 8) | chunk->instr_stream.datas[ip - 3];
            int operand2 = (u16)(chunk->instr_stream.datas[ip - 2] << 8) | chunk->instr_stream.datas[ip - 1];
            printf("%-10d %-10d", operand1, operand2);
        } else if (op == OPCODE_CREATE_CLOSURE) {
            // CREATE_CLOSURE [2b closure_constant_index] <[1b bool_is_local_var] [1b index_for_value] for each upvalue>
            ip += 2;
            int fn_index = (u16)(chunk->instr_stream.datas[ip - 2] << 8) | chunk->instr_stream.datas[ip - 1];
            printf("%-10d", fn_index);

            for (int i = 0; i < operand_byte - 2; i += 2) {
                int operand1 = chunk->instr_stream.datas[ip++];
                int operand2 = chunk->instr_stream.datas[ip++];
                printf("<%-10d %-10d>", operand1, operand2);
            }
        } else if (operand_byte == 1) {
            ip += operand_byte;
            int operand = chunk->instr_stream.datas[ip - 1];
            printf("%-10d", operand);
        } else if (operand_byte == 2) {
            ip += operand_byte;
            int operand = (u16)(chunk->instr_stream.datas[ip - 2] << 8) | chunk->instr_stream.datas[ip - 1];
            printf("%-10d", operand);

            if (op == OPCODE_LOAD_CONSTANT) {
                print_value(&chunk->constants.datas[operand]);
            } else if (op == OPCODE_LOAD_MODULE_VAR || op == OPCODE_STORE_MODULE_VAR) {
                printf("%s", module->module_var_name.datas[operand].str);
            } else if ((OPCODE_CALL0 <= op && op <= OPCODE_CALL16) || (op == OPCODE_STATIC_METHOD || op == OPCODE_INSTANCE_METHOD)) {
                printf("%s", vm->all_method_names.datas[operand].str);
            } else if (op == OPCODE_LOOP) {
                printf("-> %-5d", ip - operand + 1);
            } else if (op == OPCODE_JMP || op == OPCODE_JMP_IF_FALSE || op == OPCODE_AND || op == OPCODE_OR) {
                printf("-> %-5d", ip + operand + 1);
            }
        }

        printf("\n");

        if (op == OPCODE_END) {
            break;
        }
    }
}
