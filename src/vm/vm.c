#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include "class.h"
#include "common.h"
#include "compiler.h"
#include "core.h"
#include "gc.h"
#include "header_obj.h"
#include "meta_obj.h"
#include "obj_fn.h"
#include "obj_map.h"
#include "obj_string.h"
#include "obj_thread.h"
#include "utils.h"

#ifdef DIS_ASM_CHUNK_WHEN_CALL
    #include "disassemble.h"
#endif

inline void push_tmp_root(VM* vm, ObjHeader* obj) {
    ASSERT(obj != NULL, "root obj is null.");
    ASSERT(vm->tmp_roots_num < MAX_TEMP_ROOTS_NUM, "temporary roots too much.");
    vm->tmp_roots[vm->tmp_roots_num++] = obj;
}

inline void pop_tmp_root(VM* vm) {
    ASSERT(vm->tmp_roots_num < MAX_TEMP_ROOTS_NUM, "temporary root too much.");
    vm->tmp_roots_num--;
}

void vm_init(VM* vm) {
    vm->allocated_bytes = 0;
    vm->cur_parser = NULL;
    vm->all_objs = NULL;
    BufferInit(String, &vm->all_method_names);
    vm->all_module = objmap_new(vm);

    vm->config = (Configuration) {
        .heap_growth_factor = 1.5,
        .min_heap_size      = 1024 * 1024,      // 最小堆大小为1mb
        .initial_heap_size  = 1024 * 1024 * 10, // 初始化为10mb
        .next_gc            = 1024 * 1024 * 10, // 下一次回收大小
    };
    vm->grays = (Gray) {
        .gray_objs = (ObjHeader**)(malloc(32 * sizeof(ObjHeader*))),
        .count = 0,
        .capacity = 32,
    };
}

VM* vm_new() {
    VM* vm = (VM*)malloc(sizeof(VM));
    if (vm == NULL) {
        MEM_ERROR("allocate VM failed!\n");
    }
    
    vm_init(vm);
    build_core(vm);
    
    return vm;
}

void vm_free(VM* vm) {
    ASSERT(vm->all_method_names.count > 0, "vm have already been freed.");

    ObjHeader* header = vm->all_objs;
    while (header != NULL) {
        ObjHeader* next = header->next;
        free_obj(vm, header);
        header = next;
    }

    vm->grays.gray_objs = DEALLOCATE(vm, vm->grays.gray_objs);
    BufferClear(String, &vm->all_method_names, vm);
    DEALLOCATE(vm, vm);
}

void ensure_stack(VM* vm, ObjThread* thread, u32 neede_slots) {
    if (thread->stack_capacity >= neede_slots) {
        return;
    }

    u32 new_stack_capacity = ceil_to_power_of_2(neede_slots);
    ASSERT(new_stack_capacity > thread->stack_capacity, "new stack capacity error.");

    Value* old_stack_buttom = thread->stack;

    u32 slot_size = sizeof(Value);
    thread->stack = (Value*)mem_manager(
        vm, thread->stack,
        thread->stack_capacity * slot_size, new_stack_capacity * slot_size
    );

    thread->stack_capacity = new_stack_capacity;

    i64 offset = thread->stack - old_stack_buttom;

    if (offset == 0) {
        return; // 原位扩容，不需要调整地址
    }

    for (int i = 0; i < thread->used_frame_num; i++) {
        thread->frames[i].stack_start += offset;
    }

    ObjUpvalue* upvalue = thread->open_upvalue;
    while (upvalue != NULL) {
        upvalue->local_var_ptr += offset;
        upvalue = upvalue->next;
    }

    thread->esp += offset;
}

inline static void create_frame(VM* vm, ObjThread* thread, ObjClosure* closure, int argc) {
    if (thread->used_frame_num + 1 > thread->frame_capacity) {
        u32 new_capacity = thread->frame_capacity * 2;
        u32 frame_size = sizeof(Frame);
        thread->frames = (Frame*)mem_manager(
            vm, thread->frames,
            thread->frame_capacity * frame_size, new_capacity * frame_size
        );
        thread->frame_capacity = new_capacity;
    }

    u32 stack_slots = (u32)(thread->esp - thread->stack);
    u32 needed_slots = stack_slots + closure->fn->max_stack_slot_used;
    ensure_stack(vm, thread, needed_slots);
    prepare_frame(thread, closure, thread->esp - argc);
}

static void closed_upvalue(ObjThread* thread, Value* last_slot) {
    ObjUpvalue* upvalue = thread->open_upvalue;
    while (upvalue != NULL && upvalue->local_var_ptr >= last_slot) {
        upvalue->closed_upvalue = *(upvalue->local_var_ptr);
        upvalue->local_var_ptr = &(upvalue->closed_upvalue);
        upvalue = upvalue->next;
    }
    thread->open_upvalue = upvalue;
}

static ObjUpvalue* create_open_upvalue(VM* vm, ObjThread* thread, Value* local_var) {
    if (thread->open_upvalue == NULL) {
        thread->open_upvalue = objupvalue_new(vm, local_var);
        return thread->open_upvalue;
    }

    ObjUpvalue* pre_upvalue = NULL;
    ObjUpvalue* upvalue = thread->open_upvalue;

    while (upvalue != NULL && upvalue->local_var_ptr > local_var) {
        pre_upvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->local_var_ptr == local_var) {
        return upvalue; // upvalue 已存在
    }

    ObjUpvalue* new_upvalue = objupvalue_new(vm, local_var);
    if (pre_upvalue == NULL) {
        thread->open_upvalue = new_upvalue;
    } else {
        pre_upvalue->next = new_upvalue;
    }
    new_upvalue->next = upvalue;

    return new_upvalue;
}

static void validate_super_class(VM* vm, Value class_name, u32 field_num, Value super_class) {
    if (!VALUE_IS_CLASS(super_class)) {
        ObjString* class_name_str = VALUE_TO_STRING(class_name);
        RUNTIME_ERROR("class '%s' super class is not a valid class.", class_name_str->val.start);
    }

    Class* super = VALUE_TO_CLASS(super_class);
    if (CLASS_IS_BUILTIN(vm, super)) {
        RUNTIME_ERROR("A class cannot inherit from a built-in class.");
    }

    if (super->field_number + field_num > MAX_FIELD_NUM) {
        RUNTIME_ERROR("number of field including super exceed %d.", MAX_FIELD_NUM);
    }
}

static void patch_operand(Class* class, ObjFn* fn) {
    int ip = 0;
    while (true) {
        OpCode opcode = (OpCode)fn->instr_stream.datas[ip++];
        switch (opcode) {
            case OPCODE_LOAD_FIELD: 
            case OPCODE_STORE_FIELD: 
            case OPCODE_LOAD_SELF_FIELD: 
            case OPCODE_STORE_SELF_FIELD: 
                //修正子类的field数目 <opcode> [1b field_number]
                fn->instr_stream.datas[ip++] += class->super_class->field_number;
                break;

            case OPCODE_SUPER0:
            case OPCODE_SUPER1:
            case OPCODE_SUPER2:
            case OPCODE_SUPER3:
            case OPCODE_SUPER4:
            case OPCODE_SUPER5:
            case OPCODE_SUPER6:
            case OPCODE_SUPER7:
            case OPCODE_SUPER8:
            case OPCODE_SUPER9:
            case OPCODE_SUPER10:
            case OPCODE_SUPER11:
            case OPCODE_SUPER12:
            case OPCODE_SUPER13:
            case OPCODE_SUPER14:
            case OPCODE_SUPER15:
            case OPCODE_SUPER16: {
                // <opcode> [2b method_index] [2b super_class_index]
                ip += 2; // 跳过2字节的method索引
                uint32_t superClassIdx = (fn->instr_stream.datas[ip] << 8) | fn->instr_stream.datas[ip + 1];

                // 回填在函数emitCallBySignature中的占位VT_TO_VALUE(VT_NULL)
                fn->constants.datas[superClassIdx] = OBJ_TO_VALUE(class->super_class);

                ip += 2; // 跳过2字节的基类索引
                break;
            }

            case OPCODE_CREATE_CLOSURE: {
                // 指令流: 2字节待创建闭包的函数在常量表中的索引+函数所用的upvalue数 * 2 

                // 函数是存储到常量表中,获取待创建闭包的函数在常量表中的索引
                uint32_t fnIdx = (fn->instr_stream.datas[ip] << 8) | fn->instr_stream.datas[ip + 1]; 

                // 递归进入该函数的指令流,继续为其中的super和field修正操作数
                patch_operand(class, VALUE_TO_OBJFN(fn->constants.datas[fnIdx]));	    
            
                // ip-1是操作码OPCODE_CREATE_CLOSURE,
                // 闭包中的参数涉及到upvalue,调用getBytesOfOperands获得参数字节数
                ip += get_byte_of_operands(fn->instr_stream.datas, fn->constants.datas, ip - 1);
                break;
            }

            case OPCODE_END:
                // 用于从当前及递归嵌套闭包时返回
                return;

            default:
                // 其它指令不需要回填因此就跳过
                ip += get_byte_of_operands(fn->instr_stream.datas, fn->constants.datas, ip - 1);
                break;
        }
   }
}

static void bind_method_and_patch(VM* vm, OpCode opcode, u32 method_index, Class* class, Value method) {
    if (opcode == OPCODE_STATIC_METHOD) {
        class = class->header.class;
    }

    Method m = {
        .type = MT_SCRIPT,
        .obj = VALUE_TO_OBJCLOSURE(method),
    };

    patch_operand(class, m.obj->fn);

    bind_method(vm, class, method_index, m);
}

VMResult execute_instruction(VM* vm, register ObjThread* cur_thread) {
    vm->cur_thread = cur_thread;
    register Frame* cur_frame = NULL;
    register Value* stack_start = NULL;
    register u8* ip = 0;
    register ObjFn* fn = NULL;
    OpCode opcode;

    #define PUSH(value) (*cur_thread->esp++ = value)
    #define POP()       (*(--cur_thread->esp))
    #define DROP()      (cur_thread->esp--)
    #define PEEK_K(k)   (*(cur_thread->esp - k))
    #define PEEK()      PEEK_K(1)
    #define PEEK2()     PEEK_K(2)

    #define READ_1B()   (*ip++)
    #define READ_2B()   (ip += 2, (u16)(ip[-2] << 8) | ip[-1])
    
    #define STORE_CUR_FRAME()   cur_frame->ip = ip;
    #define LOAD_CUR_FRAME()    \
        cur_frame = &cur_thread->frames[cur_thread->used_frame_num - 1];\
        stack_start = cur_frame->stack_start;\
        ip = cur_frame->ip;\
        fn = cur_frame->closure->fn;

    #define DECODE \
        loop_start:\
            opcode = READ_1B();\
            switch (opcode)
    #define CASE(code)  case OPCODE_##code
    #define LOOP()      goto loop_start

    LOAD_CUR_FRAME();

    DECODE {
        CASE(LOAD_LOCAL_VAR): {
            // LOAD_LOCAL_VAR [1b local_var_index]
            // 局部变量存储在栈中，索引为其在栈中相对栈底的索引
            PUSH(stack_start[READ_1B()]);
            LOOP();
        }

        CASE(POP): {
            // POP
            DROP();
            LOOP();
        }

        CASE(PUSH_NULL): {
            // PUSH_NULL
            PUSH(VT_TO_VALUE(VT_NULL));
            LOOP();
        }

        CASE(PUSH_TRUE): {
            // PUSH_TRUE
            PUSH(VT_TO_VALUE(VT_TRUE));
            LOOP();
        }

        CASE(PUSH_FALSE): {
            // PUSH_FALSE
            PUSH(VT_TO_VALUE(VT_FALSE));
            LOOP();
        }

        CASE(STORE_LOCAL_VAR): {
            // STORE_LOCAL_VAR [1b local_var_index]
            // 该指令不消耗栈顶
            stack_start[READ_1B()] = PEEK();
            LOOP();
        }

        CASE(LOAD_CONSTANT): {
            // LOAD_CONSTANT [2b constant_index]
            // 从当前frame的常量表中读取index位置的常数入栈
            PUSH(fn->constants.datas[READ_2B()]);
            LOOP();
        }

        { // 所有call及super指令的共用作用阈
            int argc = 0;
            int index = 0;
            Value* args = NULL;
            Class* class = NULL;
            Method* method = NULL;

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
                // CALLX [2b method_index]
                argc = opcode - OPCODE_CALL0 + 1; // 计算argc，所有函数都至少有一个args[0]参数为self
                index = READ_2B(); // 方法索引
                args = cur_thread->esp - argc;

                class = get_class_of_object(vm, args[0]); // 方法所在的class
                
                goto invoke_method; // enter method

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
                // SUPERX [2b method_index] [2b super_class_index]
                argc = opcode - OPCODE_CALL0 + 1; // 计算argc，所有函数都至少有一个args[0]参数为self
                index = READ_2B(); // 方法索引
                args = cur_thread->esp - argc;

                class = VALUE_TO_CLASS(fn->constants.datas[READ_2B()]);

        invoke_method:
            if ((u32)index > class->methods.count || (method = &class->methods.datas[index])->type == MT_NONE) {
                RUNTIME_ERROR(
                    "method '%s.%s' not found.",
                    class->name->val.start, vm->all_method_names.datas[index]
                );
            }

            switch (method->type) {
                case MT_PRIMITIVE:
                    if (method->prim(vm, args)) {
                        cur_thread->esp -= argc - 1;
                    } else {
                        // primitive返回false：
                        // 1. 出现错误，此时cur_thread->error_obj != NULL
                        // 2. 切换线程，此时vm->cur_thread变更为新线程
                        STORE_CUR_FRAME(); // 保存当前运行状态

                        if (!VALUE_IS_NULL(cur_thread->error_obj)) {
                            if (VALUE_IS_STRING(cur_thread->error_obj)) {
                                ObjString* err = VALUE_TO_STRING(cur_thread->error_obj);
                                fprintf(stderr, "thread error: %s", err->val.start);
                            }
                            PEEK() = VT_TO_VALUE(VT_NULL); // 防止错误传递，返回null
                        }

                        if (vm->cur_thread == NULL) {
                            return VM_RES_SUCCESS;
                        }

                        cur_thread = vm->cur_thread;
                        LOAD_CUR_FRAME();
                    }
                    break;
                
                case MT_FN_CALL:
                    ASSERT(VALUE_IS_CLOSURE(args[0]), "instance must be a closure");
                    ObjFn* objfn = VALUE_TO_OBJCLOSURE(args[0])->fn;
                    if (argc - 1 != objfn->argc) {
                        RUNTIME_ERROR("argument miss match.");
                    }

                    #ifdef DIS_ASM_CHUNK_WHEN_CALL
                        printf("------ FN_CALL %d ------\n", index);
                        dis_asm(vm, objfn->module, objfn);
                        printf("\n");
                    #endif

                    STORE_CUR_FRAME();
                    create_frame(vm, cur_thread, VALUE_TO_OBJCLOSURE(args[0]), argc);
                    LOAD_CUR_FRAME();
                    break;

                case MT_SCRIPT:
                    #ifdef DIS_ASM_CHUNK_WHEN_CALL
                        printf("------ SCRIPT_CALL %d ------\n", index);
                        dis_asm(vm, method->obj->fn->module, method->obj->fn);
                        printf("\n");
                    #endif

                    STORE_CUR_FRAME();
                    create_frame(vm, cur_thread, method->obj, argc);
                    LOAD_CUR_FRAME();
                    break;

                default:
                    UNREACHABLE();
            }

            LOOP();
        }

        CASE(LOAD_UPVALUE): {
            // LOAD_UPVALUE [1b upvalue_index]
            PUSH(*((cur_frame->closure->upvalue[READ_1B()])->local_var_ptr));
            LOOP();
        }

        CASE(STORE_UPVALUE): {
            // STORE_UPVALUE [1b upvalue_index]
            *(cur_frame->closure->upvalue[READ_1B()])->local_var_ptr = PEEK();
            LOOP();
        }

        CASE(LOAD_MODULE_VAR): {
            // LOAD_MODULE_VAR [2b module_var_index]
            PUSH(fn->module->module_var_value.datas[READ_2B()]);
            LOOP();
        }

        CASE(STORE_MODULE_VAR): {
            // STORE_MODULE_VAR [2b module_var_index]
            fn->module->module_var_value.datas[READ_2B()] = PEEK();
            LOOP();
        }

        CASE(LOAD_SELF_FIELD): {
            // LOAD_SELF_FIELD [1b field_index]
            // 该指令只在方法中使用，因此栈底一定为self对象
            // stack_start[0]是当前方法中的第一个参数，即为self
            ASSERT(VALUE_IS_INSTANCE(stack_start[0]), "method receiver should be instance.");
            ObjInstance* self = VALUE_TO_INSTANCE(stack_start[0]);

            u8 field_index = READ_1B();
            ASSERT(field_index < self->header.class->field_number, "(decoder)[LOAD_SELF_FIELD] field index out of bounds.");
            
            PUSH(self->fields[field_index]);
            LOOP();
        }

        CASE(STORE_SELF_FIELD): {
            // STORE_SELF_FIELD [1b field_index]
            ASSERT(VALUE_IS_INSTANCE(stack_start[0]), "receiver should be instance.");
            ObjInstance* self = VALUE_TO_INSTANCE(stack_start[0]);

            u8 field_index = READ_1B();
            ASSERT(field_index < self->header.class->field_number, "(decoder)[STORE_SELF_FIELD] field index out of bounds.");

            self->fields[field_index] = PEEK();
            LOOP();
        }

        CASE(LOAD_FIELD): {
            // LOAD_FIELD [1b field_index]
            // 栈顶是instance对象
            Value obj = POP();
            ASSERT(VALUE_IS_INSTANCE(obj), "receiver should be instance.");
            ObjInstance* instance = VALUE_TO_INSTANCE(obj);

            u8 field_index = READ_1B();
            ASSERT(field_index < instance->header.class->field_number, "(decoder)[LOAD_FIELD] field index out of bounds.");

            PUSH(instance->fields[field_index]);
            LOOP();
        }

        CASE(STORE_FIELD): {
            // STORE_FIELD [1b field_index]
            Value obj = POP();
            ASSERT(VALUE_IS_INSTANCE(obj), "[STORE_FIELD] receiver should be instance.");
            ObjInstance* instance = VALUE_TO_INSTANCE(obj);

            u8 field_index = READ_1B();
            ASSERT(field_index < instance->header.class->field_number, "[STORE_FIELD] field index out of bounds.");

            instance->fields[field_index] = PEEK();
            LOOP();
        }

        CASE(LOOP): {
            // LOOP [2b offset]
            i16 offset = READ_2B();
            ip -= offset;
            LOOP();
        }

        CASE(JMP): {
            // JMP [2b offset]
            i16 offset = READ_2B();
            ip += offset;
            LOOP();
        }

        CASE(JMP_IF_FALSE): {
            // JMP_IF_FALSE [2b offset]
            i16 offset = READ_2B();
            Value condition = POP();
            if (VALUE_IS_FALSE(condition) || VALUE_IS_NULL(condition)) {
                ip += offset;
            }
            LOOP();
        }

        CASE(AND): {
            // AND [2b offset]
            i16 offset = READ_2B();
            Value cond1 = PEEK(); // 第一个条件已在栈顶
            
            // 若为false则直接跳转offset跳过该表达式，栈顶值则为表达式值
            if (VALUE_IS_FALSE(cond1) || VALUE_IS_NULL(cond1)) {
                ip += offset;
            } else {
                // cond1值为true，则弹栈，cond2为表达式的值
                DROP();
            }
            LOOP();
        }

        CASE(OR): {
            // OR [2b offset]
            i16 offset = READ_2B();
            Value cond1 = PEEK(); // 第一个条件已在栈顶
            
            // 若为false则直接跳转offset跳过该表达式，栈顶值则为表达式值
            if (VALUE_IS_FALSE(cond1) || VALUE_IS_NULL(cond1)) {
                // cond1值为false，则弹栈，cond2为表达式的值
                DROP();
            } else {
                // cond1为true，则表达式结果也为true
                ip += offset;
            }
            LOOP();
        }

        CASE(CLOSE_UPVALUE): {
            // CLOSE_UPVALUE
            closed_upvalue(cur_thread, cur_thread->esp - 1);
            DROP();
            LOOP();
        }

        CASE(RETURN): {
            // RETURN
            Value res = POP(); // 返回值
            
            // 推出当前frame
            cur_thread->used_frame_num--;
            
            // 局部变量被复写前保留upvalue
            closed_upvalue(cur_thread, cur_thread->esp - 1);

            if (cur_thread->used_frame_num == 0) {
                // 当前线程已无等待运行的frame
                if (cur_thread->caller == NULL) {
                    // 没有调用者，保存result并退出
                    cur_thread->stack[0] = res;
                    cur_thread->esp = cur_thread->stack + 1;
                    return VM_RES_SUCCESS;
                }

                // 当前线程被其他线程唤起，切换到caller
                ObjThread* caller = cur_thread->caller;
                cur_thread->caller = NULL;
                cur_thread = caller;
                vm->cur_thread = caller;
                cur_thread->esp[-1] = res; // 把当前线程运行的结果保存到caller的栈顶

                LOAD_CUR_FRAME();
                LOOP();
            }

            // 还有等待运行的frame
            stack_start[0] = res;
            cur_thread->esp = stack_start + 1;

            LOAD_CUR_FRAME();
            LOOP();
        }

        CASE(CONSTRUCT): {
            ASSERT(VALUE_IS_CLASS(stack_start[0]), "(decoder)[CONSTRUCT] stack_start[0] should be a class.");
            ObjInstance* instance = objinstance_new(vm, VALUE_TO_CLASS(stack_start[0]));
            stack_start[0] = OBJ_TO_VALUE(instance);
            LOOP();
        }

        CASE(CREATE_CLOSURE): {
            // CREATE_CLOSURE [2b closure_constant_index] <[1b bool_is_local_var] [1b index_for_value] for each upvalue>
            ObjFn* objfn = VALUE_TO_OBJFN(fn->constants.datas[READ_2B()]);
            ObjClosure* closure = objclosure_new(vm, objfn);
            PUSH(OBJ_TO_VALUE(closure));

            for (int i = 0; i < objfn->upvalue_number; i++) {
                u8 is_enclosing_local_var = READ_1B();
                u8 index = READ_1B();

                if (is_enclosing_local_var) {
                    closure->upvalue[i] = create_open_upvalue(vm, cur_thread, &cur_frame->stack_start[index]);
                } else {
                    closure->upvalue[i] = cur_frame->closure->upvalue[index];
                }
            }

            LOOP();
        }

        CASE(CREATE_CLASS): {
            // CREATE_CLASS [1b field_number]
            u32 field_num = READ_1B();
            // 用到栈顶两个值，弹一个，保留一个位置存放class
            Value super = POP();
            Value class_name = PEEK();

            // 判断是否是合法的super
            validate_super_class(vm, class_name, field_num, super);

            Class* class = class_new(vm, VALUE_TO_STRING(class_name), field_num, VALUE_TO_CLASS(super));
            PEEK() = OBJ_TO_VALUE(class);

            LOOP();
        }

        CASE(STATIC_METHOD):
        CASE(INSTANCE_METHOD): {
            // <OPCODE> [2b method_index]
            i16 method_index = READ_2B();
            Value class = POP();
            Value method = POP();

            bind_method_and_patch(vm, opcode, method_index, VALUE_TO_CLASS(class), method);

            LOOP();
        }

        default:
            printf(">>> %-5ld? %d\n", ip - cur_frame->closure->fn->instr_stream.datas - 1, *(ip - 1));
            UNREACHABLE();
    }

    #undef CASE
    #undef LOOP
    #undef POP
    #undef PUSH
    #undef PEEK_K
    #undef PEEK
    #undef PEEK2
    #undef DECODE
    #undef LOAD_CUR_FRAME
    #undef STORE_CUR_FRAME
    #undef READ_1B
    #undef READ_2B

    UNREACHABLE();
}
