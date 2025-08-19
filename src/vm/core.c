#include "core.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "class.h"
#include "common.h"
#include "compiler.h"
#include "gc.h"
#include "header_obj.h"
#include "meta_obj.h"
#include "obj_fn.h"
#include "obj_list.h"
#include "obj_map.h"
#include "obj_native_pointer.h"
#include "obj_range.h"
#include "obj_string.h"
#include "obj_thread.h"
#include "utf8.h"
#include "utils.h"
#include "vm.h"
#include <math.h>
#include <time.h>
#include "disassemble.h"

#include "core.script.inc"

#define CORE_MODULE VT_TO_VALUE(VT_NULL)
char* root_dir = NULL;

#define RVAL(val) \
    do {\
        args[0] = val;\
        return true;\
    } while (0)
#define ROBJ(obj) RVAL(OBJ_TO_VALUE(obj))
#define RBOOL(boolean) RVAL(BOOL_TO_VALUE(boolean))
#define RI32(i) RVAL(I32_TO_VALUE(i))
#define RF64(f) RVAL(F64_TO_VALUE(f))
#define RNULL() RVAL(VT_TO_VALUE(VT_NULL))
#define RTRUE() RVAL(VT_TO_VALUE(VT_TRUE))
#define RFALSE() RVAL(VT_TO_VALUE(VT_FALSE))

#define SET_ERROR_FALSE(vm, msg) \
    do {\
        vm->cur_thread->error_obj = OBJ_TO_VALUE(objstring_new(vm, msg, strlen(msg)));\
        return false;\
    } while (0)
#define BIND_PRIM_METHOD(class, name, func) \
    {\
        u32 len = strlen(name);\
        i32 global_index = get_index_from_symbol_table(&vm->all_method_names, name, len);\
        if (global_index == -1) {\
            global_index = add_symbol(vm, &vm->all_method_names, name, len);\
        }\
        Method method = {\
            .type = MT_PRIMITIVE,\
            .prim = func,\
        };\
        bind_method(vm, class, (u32)global_index, method);\
    }

char* read_file(const char* path) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        IO_ERROR("Could not open file '%s'.", path);
    }

    struct stat file_stat;
    stat(path, &file_stat);
    
    usize file_size = file_stat.st_size;
    char* file_content = (char*)malloc(file_size + 1);
    if (file_content == NULL) {
        MEM_ERROR(
            "Could not allocate memory(%dB) for reading file '%s'.",
            (file_size + 1), path
        );
    }

    usize num_read = fread(file_content, sizeof(char), file_size, file);
    if (num_read < file_size) {
        IO_ERROR(
            "Could not read file '%s'. Total %dB but read-in %dB",
            path, file_size, num_read
        );
    }
    file_content[file_size] = '\0';

    fclose(file);
    return file_content;
}

static ObjModule* get_module(VM* vm, Value module_name) {
    Value val = objmap_get(vm->all_module, module_name);
    return val.type == VT_UNDEFINED ? NULL : VALUE_TO_OBJMODULE(val);
}

static ObjThread* load_module(VM* vm, Value module_name, const char* module_code) {
    ObjModule* module = get_module(vm, module_name);
    
    if (module == NULL) {
        ObjString* name = VALUE_TO_OBJSTR(module_name);
        ASSERT(name->val.start[name->val.len] == '\0', "string is not terminated.");
        
        module = objmodule_new(vm, name->val.start);
        push_tmp_root(vm, (ObjHeader*)module);
        objmap_set(vm, vm->all_module, module_name, OBJ_TO_VALUE(module));
        pop_tmp_root(vm);

        // 继承核心模块的模块变量
        ObjModule* core = get_module(vm, CORE_MODULE);
        for (int i = 0; i < core->module_var_name.count; i++) {
            String name = core->module_var_name.datas[i];
            Value val = core->module_var_value.datas[i];
            define_module_var(vm, module, name.str, name.len, val);
        }
    }

    ObjFn* fn = compile_module(vm, module, module_code);
    push_tmp_root(vm, (ObjHeader*)fn);

#ifdef DIS_ASM_CHUNK
    printf("====== dis_asm '%s' ======\n", VALUE_IS_NULL(module_name) ? "core.sp" : VALUE_TO_STRING(module_name)->val.start);
    dis_asm(vm, module, fn);
    printf("======   '%s' end   ======\n", VALUE_IS_NULL(module_name) ? "core.sp" : VALUE_TO_STRING(module_name)->val.start);
#endif

    ObjClosure* closure = objclosure_new(vm, fn);
    push_tmp_root(vm, (ObjHeader*)closure);
    
    ObjThread* thread = objthread_new(vm, closure);

    pop_tmp_root(vm);
    pop_tmp_root(vm);
    return thread;
}

VMResult execute_module(VM* vm, Value module_name, const char* module_code) {
    ObjThread* obj_thread = load_module(vm, module_name, module_code);
    return execute_instruction(vm, obj_thread);
}

int get_index_from_symbol_table(SymbolTable* table, const char* symbol, u32 len) {
    ASSERT(len != 0, "length of symbole is 0.");

    for (int i = 0; i < table->count; i++) {
        if (len == table->datas[i].len && memcmp(symbol, table->datas[i].str, len) == 0) {
            return i;
        }
    }

    return -1;
}

int add_symbol(VM* vm, SymbolTable* table, const char* symbol, u32 len) {
    ASSERT(len != 0, "length of symbole is 0.");

    String str = {
        .str = ALLOCATE_ARRAY(vm, char, len + 1),
        .len = len,
    };
    memcpy(str.str, symbol, len);
    str.str[len] = '\0';

    BufferAdd(String, table, vm, str);
    
    return table->count - 1;
}

static Class* define_class(VM* vm, ObjModule* module, const char* name) {
    // 创建裸类并作为普通模块变量在模块中定义
    Class* class = class_new_raw(vm, name, 0);
    define_module_var(vm, module, name, strlen(name), OBJ_TO_VALUE(class));
    return class;
}

void bind_method(VM* vm, Class* class, u32 index, Method method) {
    if (index >= class->methods.count) {
        Method empty_pad = {.type = MT_NONE, .prim = NULL};
        BufferFill(Method, &class->methods, vm, empty_pad, (index - class->methods.count + 1));
    }
    class->methods.datas[index] = method;
}

void bind_super_class(VM* vm, Class* sub_class, Class* super_calss) {
    sub_class->super_class = super_calss;
    sub_class->field_number += super_calss->field_number;
    for (int i = 0; i < super_calss->methods.count; i++) {
        bind_method(vm, sub_class, i, super_calss->methods.datas[i]);
    }
}

#define def_prim(name) static bool prim_##name(VM* vm, Value* args)
#define prim_name(name) prim_##name

// Objtct::!(self) -> bool;
def_prim(Object_not) {
    RFALSE();
}

// Object::==(self, other: Object) -> bool;
def_prim(Object_eq) {
    RBOOL(value_is_equal(args[0], args[1]));
}

// Object::!=(self, other: Object) -> bool;
def_prim(Object_ne) {
    RBOOL(!value_is_equal(args[0], args[1]));
}

// Object::is(self, base: Class) -> bool;
def_prim(Object_is) {
    ObjHeader* self = VALUE_TO_OBJ(args[0]);

    if (!VALUE_IS_CLASS(args[1])) {
        RUNTIME_ERROR("Object::is(self, base: Class) -> bool;");
    }

    Class* base = VALUE_TO_CLASS(args[1]);
    Class* self_class = self->class;
    
    while (self_class != NULL) {
        if (self_class == base) {
            RTRUE();
        }

        self_class = self_class->super_class;
    }

    RFALSE();
}

// Object::to_string(self) -> String;
def_prim(Object_to_string) {
    ObjString* class_name = VALUE_TO_OBJ(args[0])->class->name;
    // "<instance of %(self.class.name) at %(self)>"
    int max_len = 13 + class_name->val.len + 4 + 18 + 1;
    char* buf = ALLOCATE_ARRAY(vm, char, max_len);
    sprintf(buf, "<instance of %s at %p>", class_name->val.start, args[0].header);
    ObjString* str = objstring_new(vm, buf, strlen(buf));
    DEALLOCATE_ARRAY(vm, buf, max_len);
    ROBJ(str);
}

def_prim(Object_debug_str) {
    ObjString* class_name = VALUE_TO_OBJ(args[1])->class->name;
    // "<instance of %(self.class.name) at %(self)>"
    int max_len = 13 + class_name->val.len + 4 + 18 + 1;
    char* buf = ALLOCATE_ARRAY(vm, char, max_len);
    sprintf(buf, "<instance of %s at %p>", class_name->val.start, args[1].header);
    ObjString* str = objstring_new(vm, buf, strlen(buf));
    DEALLOCATE_ARRAY(vm, buf, max_len);
    ROBJ(str);
}

// Object::type(self) -> Class;
def_prim(Object_type) {
    ROBJ(VALUE_TO_OBJ(args[0])->class);
}

// Object::super_type(self) -> Class;
def_prim(Object_super_type) {
    if (VALUE_TO_OBJ(args[0])->class->super_class != NULL) {
        ROBJ(VALUE_TO_OBJ(args[0])->class->super_class);
    }
    RNULL();
}

// Class::name(self) -> String;
def_prim(Class_name) {
    ROBJ(VALUE_TO_CLASS(args[0])->name);
}

// Class::super_type(self) -> Class;
def_prim(Class_super_type) {
    Class* self = VALUE_TO_CLASS(args[0]);
    if (self->super_class != NULL) {
        ROBJ(self->super_class);
    }
    RNULL();
}

// Class::to_string(self) -> String;
def_prim(Class_to_string) {
    ObjString* name = VALUE_TO_CLASS(args[0])->name;
    char buf[MAX_ID_LEN] = {'\0'};
    sprintf(buf, "<Class %s>", name->val.start);
    ROBJ(objstring_new(vm, buf, strlen(buf)));
}

// Object::same(o1: Object, o2: Object) -> bool;
def_prim(ObjectMeta_same) {
    RBOOL(value_is_equal(args[1], args[2]));
}

inline static Value get_core_class_value(ObjModule* module, const char* name) {
    int index = get_index_from_symbol_table(&module->module_var_name, name, strlen(name));
    if (index == -1) {
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, strlen(name));
        MEM_ERROR("something wrong occur: missing core class '%s'.", id);
    }
    return module->module_var_value.datas[index];
}

// bool.to_string() -> String;
def_prim(bool_to_string) {
    char* s = VALUE_TO_BOOL(args[0]) ? "true" : "false";
    int s_len = VALUE_TO_BOOL(args[0]) ? 4 : 5;
    ObjString* str = objstring_new(vm, s, s_len);
    ROBJ(str);
}

// bool.! -> bool;
def_prim(bool_not) {
    RBOOL(!VALUE_TO_BOOL(args[0]));
}

static bool validate_fn(VM* vm, Value arg) {
    if (VALUE_TO_OBJCLOSURE(arg)) {
        return true;
    }
    vm->cur_thread->error_obj = OBJ_TO_VALUE(objstring_new(vm, "argument must be a function.", 28));
    return false;
}

// Thread::new(func: Fn) -> Thread;
def_prim(Thread_new) {
    if (!validate_fn(vm, args[1])) {
        return false; // 已经设置好了报错，直接返回false表示出错
    }

    ObjThread* thread = objthread_new(vm, VALUE_TO_OBJCLOSURE(args[1]));
    thread->stack[0] = VT_TO_VALUE(VT_NULL);
    thread->esp++;
    
    ROBJ(thread);
}

// Thread::abort(msg: String);
def_prim(Thread_abort) {
    vm->cur_thread->error_obj = args[1];
    // TODO: 没写完
    return VALUE_IS_NULL(args[1]);
}

// Thread::current();
def_prim(Thread_current) {
    ROBJ(vm->cur_thread);
}

// Thread::suspend()
def_prim(Thread_suspend) {
    // TODO: 挂起线程，但目前只能直接退出vm
    vm->cur_thread = NULL;
    return false; // 切换线程
}

// Thread::yield(arg: Any);
def_prim(Thread_yield_arg1) {
    // 回到caller
    ObjThread* cur_thread = vm->cur_thread;
    vm->cur_thread = cur_thread->caller;

    cur_thread->caller = NULL; // 断开与caller的联系

    if (vm->cur_thread != NULL) {
        vm->cur_thread->esp[-1] = args[1];
        cur_thread->esp--; // 弹栈，只保留args[0]的位置用于保存Thread.call的参数
    }

    return false;
}

// Thread::yield();
def_prim(Thread_yield) {
    // 回到caller
    ObjThread* cur_thread = vm->cur_thread;
    vm->cur_thread = cur_thread->caller;

    cur_thread->caller = NULL; // 断开与caller的联系

    if (vm->cur_thread != NULL) {
        vm->cur_thread->esp[-1] = VT_TO_VALUE(VT_NULL);
    }

    return false;
}

static bool switch_thread(VM* vm, ObjThread* next_thread, Value* args, bool with_arg) {
    if (next_thread->caller != NULL) {
        RUNTIME_ERROR("thread has been called.");
    }

    if (next_thread->used_frame_num == 0) { // 已结束的thread，used_frame_num为0
        SET_ERROR_FALSE(vm, "a finished thread can't be switched to.");
    }

    if (!VALUE_IS_NULL(next_thread->error_obj)) {
        SET_ERROR_FALSE(vm, "a aborted thread can't be switched to.");
    }

    next_thread->caller = vm->cur_thread;

    if (with_arg) {
        vm->cur_thread->esp--; // 弹栈，只保留args[0]的位置用于保存yield返回的结果
    }

    ASSERT(next_thread->esp > next_thread->stack, "esp should be greater than stack.");

    // 将参数作为被调thread中Thread.yield()的返回值，若无则为null
    next_thread->esp[-1] = with_arg ? args[1] : VT_TO_VALUE(VT_NULL);

    vm->cur_thread = next_thread;

    return false; // thread切换
}

// Thread.call();
def_prim(Thread_call) {
    return switch_thread(vm, VALUE_TO_THREAD(args[0]), args, false);
}

// Thread.call(val: Any);
def_prim(Thread_call_arg1) {
    return switch_thread(vm, VALUE_TO_THREAD(args[0]), args, true);
}

// Thread.is_done
def_prim(Thread_is_done) {
    ObjThread* thread = VALUE_TO_THREAD(args[0]);
    RBOOL(thread->used_frame_num == 0 || !VALUE_IS_NULL(thread->error_obj));
}

// Fn::new(func: Fn) -> Fn;
def_prim(Fn_new) {
    if (!validate_fn(vm, args[1])) {
        return false;
    }
    RVAL(args[1]);
}

def_prim(Fn_disasm) {
    ObjClosure* self = VALUE_TO_OBJCLOSURE(args[0]);
    dis_asm(vm, self->fn->module, self->fn);
    RNULL();
}

static void bind_fn_overload_call(VM* vm, const char* sign) {
    u32 index = ensure_symbol_exist(vm, &vm->all_method_names, sign, strlen(sign));
    Method method = {
        .type = MT_FN_CALL,
        .obj = NULL,
    };
    bind_method(vm, vm->fn_class, index, method);
}

def_prim(Null_not) {
    RTRUE();
}

def_prim(Null_to_string) {
    ObjString* str = objstring_new(vm, "null", 4);
    ROBJ(str);
}

inline static ObjString* f64_2str(VM* vm, double num) {
    if (num != num) {
        return objstring_new(vm, "nan", 3);
    }

    if (num == INFINITY) {
        return objstring_new(vm, "inf", 3);
    }

    if (num == -INFINITY) {
        return objstring_new(vm, "-inf", 4);
    }

    char buf[24] = {'\0'};
    int len = sprintf(buf, "%lf", num);
    return objstring_new(vm, buf, len);
}

inline static ObjString* i32_2str(VM* vm, int num) {
    char buf[24] = {'\0'};
    int len = sprintf(buf, "%d", num);
    return objstring_new(vm, buf, len);
}

inline static int validate_num(VM* vm, Value arg) {
    if (VALUE_IS_F64(arg)) {
        return 2;
    }
    if (VALUE_IS_I32(arg)) {
        return 1;
    }
    SET_ERROR_FALSE(vm, "argument must be number.");
}

inline static bool validate_str(VM* vm, Value arg) {
    if (VALUE_IS_STRING(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be string.");
}

def_prim(f64_from_string) {
    if (!validate_str(vm, args[1])) {
        return false; // 报错
    }

    ObjString* str = VALUE_TO_OBJSTR(args[1]);
    if (str->val.len == 0) {
        RNULL();
    }

    ASSERT(str->val.start[str->val.len] == '\0', "str don't teminate.");

    errno = 0;
    char* end_ptr;

    double num = strtod(str->val.start, &end_ptr);

    while (*end_ptr != '\0' && isspace((unsigned char)*end_ptr)) {
        end_ptr++;
    }

    if (errno == ERANGE) {
        RUNTIME_ERROR("string too large.");
    }

    if (end_ptr < str->val.start + str->val.len) {
        RNULL();
    }

    RF64(num);
}

def_prim(i32_from_string) {
    if (!validate_str(vm, args[1])) {
        return false; // 报错
    }

    ObjString* str = VALUE_TO_OBJSTR(args[1]);
    if (str->val.len == 0) {
        RNULL();
    }

    ASSERT(str->val.start[str->val.len] == '\0', "str don't teminate.");

    errno = 0;
    char* end_ptr;

    int num = strtod(str->val.start, &end_ptr);

    while (*end_ptr != '\0' && isspace((unsigned char)*end_ptr)) {
        end_ptr++;
    }

    if (errno == ERANGE) {
        RUNTIME_ERROR("string too large.");
    }

    if (end_ptr < str->val.start + str->val.len) {
        RNULL();
    }

    RI32(num);
}

def_prim(Math_pi) {
    RF64(3.141592653589793);
}

def_prim(i32_add) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival + args[1].ival);
        case 2:
            RF64((double)(args[0].ival) + args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_sub) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival - args[1].ival);
        case 2:
            RF64((double)(args[0].ival) - args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_mul) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival * args[1].ival);
        case 2:
            RF64((double)(args[0].ival) * args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_div) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival / args[1].ival);
        case 2:
            RF64((double)(args[0].ival) / args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_mod) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival % args[1].ival);
        case 2:
            RF64(fmod((double)(args[0].ival), args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(i32_gt) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].ival > args[1].ival);
        case 2:
            RBOOL((double)(args[0].ival) > args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_ge) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].ival >= args[1].ival);
        case 2:
            RBOOL((double)(args[0].ival) >= args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_lt) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].ival < args[1].ival);
        case 2:
            RBOOL((double)(args[0].ival) < args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_le) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].ival <= args[1].ival);
        case 2:
            RBOOL((double)(args[0].ival) <= args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(i32_bit_and) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival & args[1].ival);
        case 2:
            RI32(args[0].ival & (int)(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(i32_bit_or) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival | args[1].ival);
        case 2:
            RI32(args[0].ival | (int)(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(i32_bit_ls) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival << args[1].ival);
        case 2:
            RI32(args[0].ival << (u32)(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(i32_bit_rs) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(args[0].ival >> args[1].ival);
        case 2:
            RI32(args[0].ival >> (u32)(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(f64_add) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(args[0].fval + (double)(args[1].ival));
        case 2:
            RF64(args[0].fval + args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_sub) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(args[0].fval - (double)(args[1].ival));
        case 2:
            RF64(args[0].fval - args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_mul) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(args[0].fval * (double)(args[1].ival));
        case 2:
            RF64(args[0].fval * args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_div) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(args[0].fval / (double)(args[1].ival));
        case 2:
            RF64(args[0].fval / args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_mod) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(fmod(args[0].fval, (double)(args[1].ival)));
        case 2:
            RF64(fmod(args[0].fval, args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(f64_gt) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].fval > (double)(args[1].ival));
        case 2:
            RBOOL(args[0].fval > args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_ge) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].fval >= (double)(args[1].ival));
        case 2:
            RBOOL(args[0].fval >= args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_lt) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].fval < (double)(args[1].ival));
        case 2:
            RBOOL(args[0].fval < args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(f64_le) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].fval <= (double)(args[1].ival));
        case 2:
            RBOOL(args[0].fval <= args[1].fval);
        default:
            return false; // 报错
    }
}

def_prim(Math_abs) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RI32(abs(args[1].ival));
        case 2:
            RF64(fabs(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_acos) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(acos((double)(args[1].ival)));
        case 2:
            RF64(acos(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_asin) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(asin((double)(args[1].ival)));
        case 2:
            RF64(asin(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_atan) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(atan((double)(args[1].ival)));
        case 2:
            RF64(atan(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_ceil) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64((double)(args[1].ival));
        case 2:
            RF64(ceil(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_floor) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64((double)(args[1].ival));
        case 2:
            RF64(floor(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(i32_neg) {
    RI32(-args[0].ival);
}

def_prim(f64_neg) {
    RF64(-args[0].fval);
}

def_prim(Math_cos) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(cos((double)(args[1].ival)));
        case 2:
            RF64(cos(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_sin) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(sin((double)(args[1].ival)));
        case 2:
            RF64(sin(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_tan) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(tan((double)(args[1].ival)));
        case 2:
            RF64(tan(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(Math_sqrt) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(sqrt((double)(args[1].ival)));
        case 2:
            RF64(sqrt(args[1].fval));
        default:
            return false; // 报错
    }
}

def_prim(i32_bit_not) {
    RI32(~(u32)args[0].ival);
}

def_prim(i32_range) {
    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "expect i32 value for i32.range(to: i32) -> Range;");
    }
    ROBJ(objrange_new(vm, args[0].ival, args[1].ival, 1));
}

def_prim(Math_atan2) {
    double p1 = 0.0;
    double p2 = 0.0;
    
    switch (validate_num(vm, args[1])) {
        case 1:
            p1 = args[1].ival;
        case 2:
            p2 = args[1].fval;
        default:
            return false; // 报错
    }

    switch (validate_num(vm, args[2])) {
        case 1:
            p1 = args[2].ival;
        case 2:
            p2 = args[2].fval;
        default:
            return false; // 报错
    }

    RF64(atan2(p1, p2));
}

def_prim(Math_fraction) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RF64(0.0);
        case 2: {
            double dummy;
            RF64(modf(args[1].fval, &dummy));
        }
        default:
            return false; // 报错
    }
}

def_prim(Math_truncate) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RVAL(args[1]);
        case 2: {
            RI32((i32)trunc(args[1].fval));
        }
        default:
            return false; // 报错
    }
}

def_prim(Math_is_infinity) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RFALSE();
        case 2:
            RBOOL(isinf(VALUE_TO_F64(args[0])));
        default:
            RFALSE();
    }
}

def_prim(Math_is_nan) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RFALSE();
        case 2:
            RBOOL(isnan(VALUE_TO_F64(args[0])));
        default:
            RFALSE();
    }
}

def_prim(i32_to_string) {
    ROBJ(i32_2str(vm, args[0].ival));
}

def_prim(f64_to_string) {
    ROBJ(f64_2str(vm, args[0].fval));
}

def_prim(i32_eq) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].ival == args[1].ival);
        case 2:
            RBOOL(args[0].ival == args[1].fval);
        default:
            RFALSE();
    }
}

def_prim(i32_ne) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].ival != args[1].ival);
        case 2:
            RBOOL(args[0].ival != args[1].fval);
        default:
            RTRUE();
    }
}

def_prim(f64_eq) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].fval == args[1].ival);
        case 2:
            RBOOL(args[0].fval == args[1].fval);
        default:
            RFALSE();
    }
}

def_prim(f64_ne) {
    switch (validate_num(vm, args[1])) {
        case 1:
            RBOOL(args[0].fval != args[1].ival);
        case 2:
            RBOOL(args[0].fval != args[1].fval);
        default:
            RTRUE();
    }
}

inline static u32 validate_index_value(VM* vm, int index, u32 len) {
    if (index < 0) {
        index += len;
    }

    if (index >= 0 && index < len) {
        return index;
    }

    vm->cur_thread->error_obj = OBJ_TO_VALUE(objstring_new(vm, "index out of bound.", 19));
    return UINT32_MAX;
}

inline static u32 validate_index(VM* vm, Value index, u32 len) {
    if (!VALUE_IS_I32(index)) {
        SET_ERROR_FALSE(vm, "index must be a i32 value.");
        return UINT32_MAX;
    }
    return validate_index_value(vm, index.ival, len);
}

static Value make_string_from_code_point(VM* vm, int value) {
    u32 byte = get_byte_of_decode_utf8(value);
    ASSERT(byte != 0, "utf8 encode bytes should be between 1 and 4.");

    ObjString* str = ALLOCATE_EXTRA(vm, ObjString, byte + 1);
    if (str == NULL) {
        MEM_ERROR("allocate memory failed in runtime.");
    }

    objheader_init(vm, &str->header, OT_STRING, vm->string_class);
    str->val.len = byte;
    str->val.start[byte] = '\0';
    encode_utf8((u8*)str->val.start, value);
    objstring_hash(str);
    
    return OBJ_TO_VALUE(str);
}

inline static Value string_code_point_at(VM* vm, ObjString* str, u32 index) {
    ASSERT(index < str->val.len, "String[index: i32]: index out of bound.");
    int code_point = decode_utf8((u8*)&str->val.start[index], str->val.len - index);
    if (code_point == -1) {
        return OBJ_TO_VALUE(objstring_new(vm, &str->val.start[index], 1));
    }
    return make_string_from_code_point(vm, code_point);
}

static u32 calculate_range(VM* vm, ObjRange* range, u32* count_ptr, int* step_ptr) {
    u32 from = validate_index_value(vm, range->from, *count_ptr);
    if (from == UINT32_MAX) {
        return UINT32_MAX;
    }

    u32 to = validate_index_value(vm, range->to, *count_ptr);
    if (to == UINT32_MAX) {
        return UINT32_MAX;
    }

    *step_ptr = range->step;
    ASSERT((from < to && range->step < 0) || (from >= to && range->step > 0), "range step is opposite to its direction.");
    *count_ptr = floor((double)(to - 1 - from) / abs(range->step) + 1) + 1; // 索引的元素数量

    return from;
}

static ObjString* objstring_from_sub(VM* vm, ObjString* src_str, int start, u32 count, int step) {
    u8* src = (u8*)src_str->val.start;
    
    u32 total_len = 0;
    for (int i = 0; i < count; i++) {
        total_len += get_byte_of_decode_utf8(src[start + i * step]);
    }

    ObjString* res = ALLOCATE_EXTRA(vm, ObjString, total_len + 1);
    if (res == NULL) {
        MEM_ERROR("allocate memory failed in runtime.");
    }

    objheader_init(vm, &res->header, OT_STRING, vm->string_class);
    res->val.start[total_len] = '\0';
    res->val.len = total_len;

    u8* dest = (u8*)res->val.start;
    for (int i = 0; i < count; i++) {
        // 计算src中的索引
        int src_index = start + i * step;
        // 解码utf8
        int code_point = decode_utf8(&src[src_index], src_str->val.len - src_index);
        // 若正确解码，则加入dest，dest根据字节长度自增
        if (code_point != -1) {
            dest += encode_utf8(dest, code_point);
        }
    }

    objstring_hash(res);
    return res;
}

static int find_string(ObjString* haystack, ObjString* needle) {
    if (needle->val.len == 0) {
        return 0;
    }

    if (needle->val.len > haystack->val.len) {
        return -1;
    }

    u32 shift[UINT8_MAX];
    u32 needle_end = needle->val.len - 1;

    for (int i = 0; i < UINT8_MAX; i++) {
        shift[i] = needle->val.len;
    }

    for (int i = 0; i < needle_end; i++) {
        char c = needle->val.start[i];
        shift[(u8)c] = needle_end - i;
    }

    char last_char = needle->val.start[needle_end];

    u32 range = haystack->val.len - needle->val.len;

    for (int i = 0; i <= range;)  {
        char c = haystack->val.start[i + needle_end];
        if (last_char == c && memcmp(&haystack->val.start[i], needle->val.start, needle_end) == 0) {
            return i;
        }
        i += shift[(u8)c];
    }

    return -1;
}

def_prim(String_from_code_point) {
    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "String.from_code_point(index: i32) -> String; index must be i32 value.");
    }

    int code_point = args[1].ival;
    if (code_point < 0) {
        SET_ERROR_FALSE(vm, "code point can't be negetive.");
    }

    if (code_point > 0x10FFFF) {
        SET_ERROR_FALSE(vm, "code point must be between 0 and 0x10FFFF.");
    }

    RVAL(make_string_from_code_point(vm, code_point));
}

def_prim(String_add) {
    if (!validate_str(vm, args[1])) {
        return false;
    }

    ObjString* l = VALUE_TO_OBJSTR(args[0]);
    ObjString* r = VALUE_TO_OBJSTR(args[1]);

    if (r->val.len == 0) {
        ROBJ(l);
    }

    if (l->val.len == 0) {
        ROBJ(r);
    }

    u32 total_len = l->val.len + r->val.len;
    
    ObjString* res = ALLOCATE_EXTRA(vm, ObjString, total_len + 1);
    if (res == NULL) {
        MEM_ERROR("allocate memory failed in runtime.");
    }

    objheader_init(vm, &res->header, OT_STRING, vm->string_class);
    memcpy(res->val.start, l->val.start, l->val.len);
    memcpy(res->val.start + l->val.len, r->val.start, r->val.len);
    res->val.len = total_len;
    res->val.start[res->val.len] = '\0';
    objstring_hash(res);

    ROBJ(res);
}

def_prim(String_subscript) {
    ObjString* str = VALUE_TO_OBJSTR(args[0]);
    if (VALUE_IS_I32(args[1])) {
        u32 index = validate_index_value(vm, args[1].ival, str->val.len);
        if (index == UINT32_MAX) {
            return false; // 报错
        }
        RVAL(string_code_point_at(vm, str, index));
    }

    if (!VALUE_IS_RANGE(args[1])) {
        SET_ERROR_FALSE(vm, "String.[index: i32 | Range] -> String; subscript should be i32 or Range value.");
    }

    int step;
    u32 count;
    u32 start = calculate_range(vm, VALUE_TO_RANGE(args[1]), &count, &step);
    if (start == UINT32_MAX) {
        return false; // 报错
    }

    ROBJ(objstring_from_sub(vm, str, start, count, step));
}

def_prim(String_byte_at) {
    ObjString* self = VALUE_TO_OBJSTR(args[0]);
    u32 index = validate_index(vm, args[1], self->val.len);
    if (index == UINT32_MAX) {
        return false; // error
    }
    RI32((u8)self->val.start[index]);
}

def_prim(String_byte_count) {
    RI32(VALUE_TO_STRING(args[0])->val.len);
}

def_prim(String_code_point_at) {
    ObjString* self = VALUE_TO_OBJSTR(args[0]);
    u32 index = validate_index(vm, args[1], self->val.len);
    if (index == UINT32_MAX) {
        return false; // error
    }

    const u8* bytes = (u8*)self->val.start;
    if ((bytes[index] & 0xC0) == 0x80) {
        RI32(-1);
    }

    RI32(decode_utf8((u8*)&self->val.start[index], self->val.len - index));
}

def_prim(String_contains) {
    if (!validate_str(vm, args[1])) {
        return false;
    }

    ObjString* self = VALUE_TO_STRING(args[0]);
    ObjString* pattern = VALUE_TO_STRING(args[1]);

    RBOOL(find_string(self, pattern) != -1);
}

def_prim(String_ends_with) {
    if (!validate_str(vm, args[1])) {
        return false; // error
    }

    ObjString* self = VALUE_TO_STRING(args[0]);
    ObjString* pattern = VALUE_TO_STRING(args[1]);

    if (pattern->val.len > self->val.len) {
        RFALSE();
    }

    char* cmp_index = self->val.start + self->val.len - pattern->val.len;
    RBOOL(memcmp(cmp_index, pattern->val.start, pattern->val.len) == 0);
}

def_prim(String_index_of) {
    // 查找args[0]中子串args[1]的起始下标
    if (!validate_str(vm, args[1])) {
        return false; // error
    }

    ObjString* self = VALUE_TO_STRING(args[0]);
    ObjString* pattern = VALUE_TO_STRING(args[1]);

    int index = find_string(self, pattern);
    RI32(index);
}

def_prim(String_iterate) {
    ObjString* self = VALUE_TO_STRING(args[0]);

    if (VALUE_IS_NULL(args[1])) {
        if (self->val.len == 0) {
            RFALSE();
        }
        RI32(0);
    }

    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "iter-var must be a i32 value.");
    }

    int iter_var = args[1].ival; // iter_var保存的是上一个迭代的值
    if (iter_var < 0) {
        RFALSE();
    }

    do {
        iter_var++;
        if (iter_var >= self->val.len) {
            RFALSE();
        }
    } while ((self->val.start[iter_var] & 0xC0) == 0x80); // 跳转到下一个utf8字符的起始字节

    RI32(iter_var);
}

def_prim(String_iterate_byte) {
    ObjString* self = VALUE_TO_STRING(args[0]);

    if (VALUE_IS_NULL(args[1])) {
        if (self->val.len == 0) {
            RFALSE();
        }
        RI32(0);
    }

    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "iter-var must be a i32 value.");
    }

    int iter_var = args[1].ival; // iter_var保存的是上一个迭代的值
    if (iter_var < 0) {
        RFALSE();
    }

    // 迭代
    iter_var++;
    if (iter_var >= self->val.len) {
        RFALSE();
    }

    RI32(iter_var);
}

def_prim(String_iterator_value) {
    ObjString* self = VALUE_TO_OBJSTR(args[0]);
    uint32_t index = validate_index(vm, args[1], self->val.len);
    if (index == UINT32_MAX) {
        return false; 
    }
    RVAL(string_code_point_at(vm, self, index));
}

def_prim(String_starts_with) {
    if (!validate_str(vm, args[1])) {
        return false; // error
    }

    ObjString* self = VALUE_TO_STRING(args[0]);
    ObjString* pattern = VALUE_TO_STRING(args[1]);

    if (pattern->val.len > self->val.len) {
        RFALSE();
    }

    RBOOL(memcmp(self->val.start, pattern->val.start, pattern->val.len) == 0);
}

def_prim(String_to_string) {
    RVAL(args[0]);
}

def_prim(List_new) {
    ROBJ(objlist_new(vm, 0));
}

def_prim(List_subscript) {
    ObjList* self = VALUE_TO_LIST(args[0]);

    if (VALUE_IS_I32(args[1])) {
        u32 index = validate_index_value(vm, args[1].ival, self->elements.count);
        if (index == UINT32_MAX) {
            return false; // error
        }
        RVAL(self->elements.datas[index]);
    }

    if (!VALUE_IS_RANGE(args[1])) {
        SET_ERROR_FALSE(vm, "List<T>.[index: i32 | Range] -> T | List<T>; index must be i32 or Rnage value.");
    }

    int step;
    u32 count;
    u32 start = calculate_range(vm, VALUE_TO_RANGE(args[1]), &count, &step);

    ObjList* res = objlist_new(vm, count);
    for (int i = 0; i < count; i++) {
        res->elements.datas[i] = self->elements.datas[start + i * step];
    }
    ROBJ(res);
}

def_prim(List_subscript_set) {
    if (!VALUE_IS_RANGE(args[1])) {
        SET_ERROR_FALSE(vm, "List<T>.[index: i32]=(val: T) -> T | List<T>; index must be i32 value.");
    }
    
    ObjList* self = VALUE_TO_LIST(args[0]);
    u32 index = validate_index_value(vm, args[1].ival, self->elements.count);

    self->elements.datas[index] = args[2];
    RVAL(args[2]);
}

def_prim(List_append) {
    ObjList* self = VALUE_TO_LIST(args[0]);
    BufferAdd(Value, &self->elements, vm, args[1]);
    RVAL(args[1]);
}

def_prim(List_core_append) {
    ObjList* self = VALUE_TO_LIST(args[0]);
    BufferAdd(Value, &self->elements, vm, args[1]);
    RVAL(args[0]);
}

def_prim(List_clear) {
    ObjList* self = VALUE_TO_LIST(args[0]);
    BufferClear(Value, &self->elements, vm);
    RNULL();
}

def_prim(List_insert) {
    ObjList* self = VALUE_TO_LIST(args[0]);
    u32 index = validate_index(vm, args[1], self->elements.count + 1);
    if (index == UINT32_MAX) {
        return false; // error
    }

    objlist_insert_element(vm, self, index, args[2]);
    RVAL(args[2]);
}

def_prim(List_iterate) {
    ObjList* self = VALUE_TO_LIST(args[0]);

    if (VALUE_IS_NULL(args[1])) {
        if (self->elements.count == 0) {
            RFALSE();
        }
        RI32(0);
    }

    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "iter-var must be a i32 value.");
    }

    int iter = VALUE_TO_I32(args[1]) + 1;
    if (iter < 0 || iter >= self->elements.count) {
        RFALSE();
    }

    RI32(iter);
}

def_prim(List_iterator_value) {
    ObjList* self = VALUE_TO_LIST(args[0]);

    u32 index = validate_index(vm, args[1], self->elements.count);
    if (index == UINT32_MAX) {
        return false; // error
    }

    RVAL(self->elements.datas[index]);
}

def_prim(List_remove_at) {
    ObjList* self = VALUE_TO_LIST(args[0]);
    
    u32 index = validate_index(vm, args[1], self->elements.count);
    if (index == UINT32_MAX) {
        return false; // error
    }

    RVAL(objlist_remove_element(vm, self, index));
}

def_prim(List_len) {
    RI32(VALUE_TO_LIST(args[0])->elements.count);
}

static bool validate_key(VM* vm, Value arg) {
    if (VALUE_IS_TRUE(arg)
        || VALUE_IS_FALSE(arg)
        || VALUE_IS_NULL(arg)
        || VALUE_IS_NUM(arg)
        || VALUE_IS_STRING(arg)
        || VALUE_IS_RANGE(arg)
        || VALUE_IS_CLASS(arg)
    ) {
        return true;
    }
    SET_ERROR_FALSE(vm, "key must be hashable value.");
}

def_prim(Map_new) {
    ROBJ(objmap_new(vm));
}

def_prim(Map_subscript) {
    if (!validate_key(vm, args[1])) {
        return false; // error
    }

    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    Value val = objmap_get(self, args[1]);
    if (VALUE_IS_UNDEFINED(val)) {
        RNULL();
    }
    RVAL(val);
}

def_prim(Map_subscript_set) {
    if (!validate_key(vm, args[1])) {
        return false; // error
    }

    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    objmap_set(vm, self, args[1], args[2]);
    RVAL(args[2]);
}

def_prim(Map_core_insert) {
    if (!validate_key(vm, args[1])) {
        return false; // error
    }

    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    objmap_set(vm, self, args[1], args[2]);
    RVAL(args[0]);
}

def_prim(Map_clear) {
    if (!validate_key(vm, args[1])) {
        return false; // error
    }

    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    objmap_clear(vm, self);
    RNULL();
}

def_prim(Map_contains_key) {
    if (!validate_key(vm, args[1])) {
        return false; // error
    }
    RBOOL(!VALUE_IS_UNDEFINED(objmap_get(VALUE_TO_OBJMAP(args[0]), args[1])));
}

def_prim(Map_len) {
    RI32(VALUE_TO_OBJMAP(args[0])->len);
}

def_prim(Map_remove) {
    if (!validate_key(vm, args[1])) {
        return false;
    }

    RVAL(objmap_remove(vm, VALUE_TO_OBJMAP(args[0]), args[1]));
}

def_prim(Map_iterate) {
    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    if (self->len == 0) {
        RFALSE();
    }

    u32 index = 0;
    if (!VALUE_IS_NULL(args[1])) {
        if (!VALUE_IS_I32(args[1])) {
            SET_ERROR_FALSE(vm, "iter-var must be a i32 value.");
        }

        if (args[1].ival < 0) {
            RFALSE();
        }

        index = args[1].ival;

        if (index >= self->capacity) {
            RFALSE();
        }

        index++;
    }

    // 迭代到下一个有效的entry
    while (index < self->capacity) {
        if (!VALUE_IS_UNDEFINED(self->entries[index].key)) {
            RI32(index);
        }
        index++;
    }

    // 未迭代到下一个有效entry，则迭代已结束
    RFALSE();
}

def_prim(Map_val_iterator_value) {
    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    u32 index = validate_index(vm, args[1], self->capacity);
    if (index == UINT32_MAX) {
        return false; // error
    }

    Entry* entry = &self->entries[index];
    if (VALUE_IS_UNDEFINED(entry->key)) {
        SET_ERROR_FALSE(vm, "invalid iter.");
    }

    RVAL(entry->val);
}

def_prim(Map_key_iterator_value) {
    ObjMap* self = VALUE_TO_OBJMAP(args[0]);
    u32 index = validate_index(vm, args[1], self->capacity);
    if (index == UINT32_MAX) {
        return false; // error
    }

    Entry* entry = &self->entries[index];
    if (VALUE_IS_UNDEFINED(entry->key)) {
        SET_ERROR_FALSE(vm, "invalid iter.");
    }

    RVAL(entry->key);
}

def_prim(Range_from) {
    RI32(VALUE_TO_RANGE(args[0])->from);
}

def_prim(Range_to) {
    RI32(VALUE_TO_RANGE(args[0])->to);
}

def_prim(Range_step) {
    RI32(VALUE_TO_RANGE(args[0])->step);
}

def_prim(Range_iterate) {
    ObjRange* self = VALUE_TO_RANGE(args[0]);
    
    if (self->from == self->to) {
        RFALSE();
    }
    
    if (VALUE_IS_NULL(args[1])) {
        RI32(self->from);
    }

    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "iter-var must be a i32 value.");
    }

    int iter = args[1].ival + self->step;

    if (self->from < self->to && self->to <= iter || self->from > self->to && self->to >= iter) {
        RFALSE();
    }

    RI32(iter);
}

def_prim(Range_iterator_value) {
    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "iter-var must be a i32 value.");
    }
    
    ObjRange* self = VALUE_TO_RANGE(args[0]);
    int iter = args[1].ival;

    if (self->from < self->to && self->to <= iter || self->from > self->to && self->to >= iter) {
        RFALSE();
    }

    RI32(iter);
}

def_prim(Range_new_arg3) {
    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "Range.from must be a i32 value.");
    }

    if (!VALUE_IS_I32(args[2])) {
        SET_ERROR_FALSE(vm, "Range.to must be a i32 value.");
    }

    if (!VALUE_IS_I32(args[3])) {
        SET_ERROR_FALSE(vm, "Range.step must be a i32 value.");
    }

    ROBJ(objrange_new(vm, args[1].ival, args[2].ival, args[3].ival));
}

def_prim(Range_new_arg2) {
    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "Range.from must be a i32 value.");
    }

    if (!VALUE_IS_I32(args[2])) {
        SET_ERROR_FALSE(vm, "Range.to must be a i32 value.");
    }

    int step = args[1].ival <= args[2].ival ? 1 : -1;

    ROBJ(objrange_new(vm, args[1].ival, args[2].ival, step));
}

def_prim(Range_new_arg1) {
    int from = 0;

    if (!VALUE_IS_I32(args[1])) {
        SET_ERROR_FALSE(vm, "Range.from must be a i32 value.");
    }

    int step = args[1].ival <= args[2].ival ? 1 : -1;

    ROBJ(objrange_new(vm, from, args[1].ival, step));
}

static char* get_file_path(const char* module_name) {
    u32 root_dir_len = root_dir == NULL ? 0 : strlen(root_dir);
    u32 name_len = strlen(module_name);
    u32 extension_len = strlen(SCRIPT_EXTENSION);
    u32 path_len = root_dir_len + name_len + extension_len;

    char* path = malloc(path_len + 1);
    if (path == NULL) {
        MEM_ERROR("memory error when make module path.");
    }
    
    if (root_dir != NULL) {
        memmove(path, root_dir, root_dir_len);
    }

    memmove(path + root_dir_len, module_name, name_len);
    memmove(path + root_dir_len + name_len, SCRIPT_EXTENSION, extension_len);
    path[path_len] = '\0';

    return path;
}

inline static char* read_module(const char* module_name) {
    char* module_path = get_file_path(module_name);
    char* module_code = read_file(module_path);
    free(module_path);
    return module_code;
}

inline static void print_str(const char* str) {
    printf("%s", str);
    fflush(stdout);
}

inline static void fprint_str(FILE* fp, const char* str) {
    fprintf(fp, "%s", str);
    fflush(fp);
}

static Value import_module(VM* vm, Value module_name) {
    if (!VALUE_IS_UNDEFINED(objmap_get(vm->all_module, module_name))) {
        return VT_TO_VALUE(VT_NULL);
    }

    ObjString* str = VALUE_TO_STRING(module_name);
    const char* src = read_module(str->val.start);

    ObjThread* module_thread = load_module(vm, module_name, src);
    return OBJ_TO_VALUE(module_thread);
}

static Value get_module_variable(VM* vm, Value module_name, Value var_name) {
    ObjModule* module = get_module(vm, module_name);
    if (module == NULL) {
        ObjString* name = VALUE_TO_STRING(module_name);
        ASSERT(name->val.len < 512 - 24, "id's buffer not big enough.");
        char id[512] = {'\0'};
        int len = sprintf(id, "module '%s' is not loaded.", name->val.start);
        vm->cur_thread->error_obj = OBJ_TO_VALUE(objstring_new(vm, id, len));
        return VT_TO_VALUE(VT_NULL);
    }

    ObjString* var = VALUE_TO_OBJSTR(var_name);
    int index = get_index_from_symbol_table(&module->module_var_name, var->val.start, var->val.len);
    if (index == -1) {
        ObjString* name = VALUE_TO_STRING(module_name);
        ASSERT((name->val.len + var->val.len) < 512 - 27, "id's buffer not big enough.");
        char id[512] = {'\0'};
        int len = sprintf(id, "var '%s' is not in module '%s'.", var->val.start, name->val.start);
        vm->cur_thread->error_obj = OBJ_TO_VALUE(objstring_new(vm, id, len));
        return VT_TO_VALUE(VT_NULL);
    }

    return module->module_var_value.datas[index];
}

bool validate_np(VM* vm, Value val, ObjString* expected) {
    if (!VALUE_IS_NATIVE_POINTER(val)) {
        SET_ERROR_FALSE(vm, "expected NativePointer value.");
    }
    
    if (!native_pointer_check_classifier(VALUE_TO_NATIVE_POINTER(val), expected)) {
        usize len = 42 + expected->val.len;
        char* buf = ALLOCATE_ARRAY(vm, char, len + 1);
        sprintf(buf, "expected NativePointer with classifier '%s'.", expected->val.start);
        vm->cur_thread->error_obj = OBJ_TO_VALUE(objstring_new(vm, buf, len));
        DEALLOCATE_ARRAY(vm, buf, len + 1);
        return false;
    }

    return true;
}

def_prim(System_clock) {
    RF64((double)time(NULL));
}

def_prim(System_import_module) {
    if (!validate_str(vm, args[1])) {
        return false; // error
    }

    Value res = import_module(vm, args[1]); // 编译module
    if (VALUE_IS_NULL(res)) { // module已存在，因此只需要
        RNULL();
    }

    vm->cur_thread--; // 回收args[1]，只保留args[0]用于存放thread返回的结果。

    ObjThread* next_thread = VALUE_TO_THREAD(res);
    next_thread->caller = vm->cur_thread;
    return false; // switch thread
}

def_prim(System_get_module_variable) {
    if (!validate_str(vm, args[1])) {
        return false; // error
    }

    if (!validate_str(vm, args[2])) {
        return false; // error
    }

    Value res = get_module_variable(vm, args[1], args[2]);
    if (VALUE_IS_NULL(res)) {
        return false; //error
    }

    RVAL(res);
}

def_prim(System_write_string) {
    if (!validate_str(vm, args[1])) {
        return false; // error
    }
    ObjString* str = VALUE_TO_OBJSTR(args[1]);
    ASSERT(str->val.start[str->val.len] == '\0', "string isn't terminated.");
    print_str(str->val.start);
    RVAL(args[1]);
}

static ObjString* CFILE_ptr_classifier = NULL;

def_prim(System_fwrites) {
    if (validate_np(vm, args[1], CFILE_ptr_classifier) || !validate_str(vm, args[2])) {
        return false; // error
    }
    ObjString* str = VALUE_TO_OBJSTR(args[2]);
    ASSERT(str->val.start[str->val.len] == '\0', "string isn't terminated.");
    fprint_str(VALUE_TO_NATIVE_POINTER(args[1])->ptr, str->val.start);
    RVAL(args[1]);
}

def_prim(VM_gc) {
    start_gc(vm);
    RNULL();
}

def_prim(VM_allocated_bytes) {
    RI32((int)vm->allocated_bytes);
}

def_prim(NativePointer_check_classifier) {
    if (!validate_str(vm, args[1])) {
        return false; // error
    }
    RBOOL(native_pointer_check_classifier(VALUE_TO_NATIVE_POINTER(args[0]), VALUE_TO_STRING(args[1])));
}

def_prim(NativePointer_is_null) {
    RBOOL(VALUE_TO_NATIVE_POINTER(args[0])->ptr == NULL);
}

def_prim(CFILE_stdin) {
    ROBJ(native_pointer_new(vm, stdin, CFILE_ptr_classifier, NULL));
}

def_prim(CFILE_stdout) {
    ROBJ(native_pointer_new(vm, stdout, CFILE_ptr_classifier, NULL));
}

def_prim(CFILE_stderr) {
    ROBJ(native_pointer_new(vm, stderr, CFILE_ptr_classifier, NULL));
}

static void destroy_file(ObjNativePointer* ptr) {
    if (ptr->ptr != NULL) {
        fclose(ptr->ptr);
    }
}

def_prim(CFILE_fopen) {
    if (!validate_str(vm, args[1]) || !validate_str(vm, args[2])) {
        return false; // error
    }
    
    FILE* fp = fopen(VALUE_TO_STRING(args[1])->val.start, VALUE_TO_STRING(args[2])->val.start);
    if (fp == NULL) {
        RNULL();
    }

    ROBJ(native_pointer_new(vm, fp, CFILE_ptr_classifier, destroy_file));
}

def_prim(CFILE_fclose) {
    if (!validate_np(vm, args[1], CFILE_ptr_classifier)) {
        return false; // error
    }
    
    FILE* fp = VALUE_TO_NATIVE_POINTER(args[1])->ptr;
    fclose(fp);
    VALUE_TO_NATIVE_POINTER(args[1])->ptr = NULL;

    RVAL(args[1]);
}

void build_core(VM* vm) {
    ObjModule* core_module = objmodule_new(vm, NULL);
    push_tmp_root(vm, (ObjHeader*)core_module);
    objmap_set(vm, vm->all_module, CORE_MODULE, OBJ_TO_VALUE(core_module));
    pop_tmp_root(vm);

    vm->object_class = define_class(vm, core_module, "Object");
    BIND_PRIM_METHOD(vm->object_class, "!", prim_name(Object_not));
    BIND_PRIM_METHOD(vm->object_class, "==(_)", prim_name(Object_eq));
    BIND_PRIM_METHOD(vm->object_class, "!=(_)", prim_name(Object_ne));
    BIND_PRIM_METHOD(vm->object_class, "is(_)", prim_name(Object_is));
    BIND_PRIM_METHOD(vm->object_class, "to_string()", prim_name(Object_to_string));
    BIND_PRIM_METHOD(vm->object_class, "type()", prim_name(Object_type));
    BIND_PRIM_METHOD(vm->object_class, "super_type()", prim_name(Object_super_type));

    vm->class_of_class = define_class(vm, core_module, "Class");
    bind_super_class(vm, vm->class_of_class, vm->object_class);
    BIND_PRIM_METHOD(vm->class_of_class, "name", prim_name(Class_name));
    BIND_PRIM_METHOD(vm->class_of_class, "super_type()", prim_name(Class_super_type));
    BIND_PRIM_METHOD(vm->class_of_class, "to_string()", prim_name(Class_to_string));

    Class* object_meta_class = define_class(vm, core_module, "Object@Meta");
    bind_super_class(vm, object_meta_class, vm->class_of_class);
    BIND_PRIM_METHOD(object_meta_class, "same(_,_)", prim_name(ObjectMeta_same));
    BIND_PRIM_METHOD(object_meta_class, "debug_str(_)", prim_name(Object_debug_str));

    vm->object_class->header.class = object_meta_class;
    object_meta_class->header.class = vm->class_of_class;
    vm->class_of_class->header.class = vm->class_of_class;

    execute_module(vm, CORE_MODULE, core_module_code);

    // bool
    vm->bool_class = VALUE_TO_CLASS(get_core_class_value(core_module, "bool"));
    BIND_PRIM_METHOD(vm->bool_class, "!", prim_name(bool_not));
    BIND_PRIM_METHOD(vm->bool_class, "to_string()", prim_name(bool_to_string));

    vm->thread_class = VALUE_TO_CLASS(get_core_class_value(core_module, "Thread"));
    // static
    BIND_PRIM_METHOD(vm->thread_class->header.class, "new(_)", prim_name(Thread_new));
    BIND_PRIM_METHOD(vm->thread_class->header.class, "abort(_)", prim_name(Thread_abort));
    BIND_PRIM_METHOD(vm->thread_class->header.class, "current", prim_name(Thread_current));
    BIND_PRIM_METHOD(vm->thread_class->header.class, "suspend()", prim_name(Thread_suspend));
    BIND_PRIM_METHOD(vm->thread_class->header.class, "yield(_)", prim_name(Thread_yield_arg1));
    BIND_PRIM_METHOD(vm->thread_class->header.class, "yield()", prim_name(Thread_yield));
    // method
    BIND_PRIM_METHOD(vm->thread_class, "call()", prim_name(Thread_call));
    BIND_PRIM_METHOD(vm->thread_class, "call(_)", prim_name(Thread_call_arg1));
    BIND_PRIM_METHOD(vm->thread_class, "is_done", prim_name(Thread_is_done));

    vm->fn_class = VALUE_TO_CLASS(get_core_class_value(core_module, "Fn"));
    // static
    BIND_PRIM_METHOD(vm->fn_class->header.class, "new(_)", prim_name(Fn_new));
    // field
    BIND_PRIM_METHOD(vm->fn_class, "disasm()", prim_name(Fn_disasm));
    bind_fn_overload_call(vm, "call()");
    bind_fn_overload_call(vm, "call(_)");
    bind_fn_overload_call(vm, "call(_,_)");
    bind_fn_overload_call(vm, "call(_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bind_fn_overload_call(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");

    vm->null_class = VALUE_TO_CLASS(get_core_class_value(core_module, "Null"));
    BIND_PRIM_METHOD(vm->null_class, "!", prim_name(Null_not));
    BIND_PRIM_METHOD(vm->null_class, "to_string()", prim_name(Null_to_string));

    vm->i32_class = VALUE_TO_CLASS(get_core_class_value(core_module, "i32"));
    BIND_PRIM_METHOD(vm->i32_class, "+(_)", prim_name(i32_add));
    BIND_PRIM_METHOD(vm->i32_class, "-(_)", prim_name(i32_sub));
    BIND_PRIM_METHOD(vm->i32_class, "*(_)", prim_name(i32_mul));
    BIND_PRIM_METHOD(vm->i32_class, "/(_)", prim_name(i32_div));
    BIND_PRIM_METHOD(vm->i32_class, "\%(_)", prim_name(i32_mod));
    BIND_PRIM_METHOD(vm->i32_class, ">(_)", prim_name(i32_gt));
    BIND_PRIM_METHOD(vm->i32_class, ">=(_)", prim_name(i32_ge));
    BIND_PRIM_METHOD(vm->i32_class, "<(_)", prim_name(i32_lt));
    BIND_PRIM_METHOD(vm->i32_class, "<=(_)", prim_name(i32_le));
    BIND_PRIM_METHOD(vm->i32_class, "==(_)", prim_name(i32_eq));
    BIND_PRIM_METHOD(vm->i32_class, "!=(_)", prim_name(i32_ne));
    BIND_PRIM_METHOD(vm->i32_class, "&(_)", prim_name(i32_bit_and));
    BIND_PRIM_METHOD(vm->i32_class, "|(_)", prim_name(i32_bit_or));
    BIND_PRIM_METHOD(vm->i32_class, "<<(_)", prim_name(i32_bit_ls));
    BIND_PRIM_METHOD(vm->i32_class, ">>(_)", prim_name(i32_bit_rs));
    BIND_PRIM_METHOD(vm->i32_class, "~", prim_name(i32_bit_not));
    BIND_PRIM_METHOD(vm->i32_class, "-", prim_name(i32_neg));
    BIND_PRIM_METHOD(vm->i32_class, "to_string()", prim_name(i32_to_string));
    BIND_PRIM_METHOD(vm->i32_class, "..(_)", prim_name(i32_range));

    vm->f64_class = VALUE_TO_CLASS(get_core_class_value(core_module, "f64"));
    BIND_PRIM_METHOD(vm->f64_class, "+(_)", prim_name(f64_add));
    BIND_PRIM_METHOD(vm->f64_class, "-(_)", prim_name(f64_sub));
    BIND_PRIM_METHOD(vm->f64_class, "*(_)", prim_name(f64_mul));
    BIND_PRIM_METHOD(vm->f64_class, "/(_)", prim_name(f64_div));
    BIND_PRIM_METHOD(vm->f64_class, "\%(_)", prim_name(f64_mod));
    BIND_PRIM_METHOD(vm->f64_class, ">(_)", prim_name(f64_gt));
    BIND_PRIM_METHOD(vm->f64_class, ">=(_)", prim_name(f64_ge));
    BIND_PRIM_METHOD(vm->f64_class, "<(_)", prim_name(f64_lt));
    BIND_PRIM_METHOD(vm->f64_class, "<=(_)", prim_name(f64_le));
    BIND_PRIM_METHOD(vm->f64_class, "==(_)", prim_name(f64_eq));
    BIND_PRIM_METHOD(vm->f64_class, "!=(_)", prim_name(f64_ne));
    BIND_PRIM_METHOD(vm->f64_class, "-", prim_name(f64_neg));
    BIND_PRIM_METHOD(vm->f64_class, "to_string()", prim_name(f64_to_string));

    Class* math = VALUE_TO_CLASS(get_core_class_value(core_module, "Math"));
    BIND_PRIM_METHOD(math, "abs(_,_)", prim_name(Math_abs));
    BIND_PRIM_METHOD(math, "acos(_)", prim_name(Math_acos));
    BIND_PRIM_METHOD(math, "asin(_)", prim_name(Math_asin));
    BIND_PRIM_METHOD(math, "atan(_)", prim_name(Math_atan));
    BIND_PRIM_METHOD(math, "cos(_)", prim_name(Math_cos));
    BIND_PRIM_METHOD(math, "sin(_)", prim_name(Math_sin));
    BIND_PRIM_METHOD(math, "tan(_)", prim_name(Math_tan));
    BIND_PRIM_METHOD(math, "ceil(_)", prim_name(Math_ceil));
    BIND_PRIM_METHOD(math, "floor(_)", prim_name(Math_floor));
    BIND_PRIM_METHOD(math, "sqrt(_)", prim_name(Math_sqrt));
    BIND_PRIM_METHOD(math, "atan2(_,_)", prim_name(Math_atan2));
    BIND_PRIM_METHOD(math, "fraction(_)", prim_name(Math_fraction));
    BIND_PRIM_METHOD(math, "truncate(_)", prim_name(Math_truncate));
    BIND_PRIM_METHOD(math, "isinf(_)", prim_name(Math_is_infinity));
    BIND_PRIM_METHOD(math, "isnan(_)", prim_name(Math_is_nan));
    
    vm->string_class = VALUE_TO_CLASS(get_core_class_value(core_module, "String"));
    BIND_PRIM_METHOD(vm->string_class, "+(_)", prim_name(String_add));
    BIND_PRIM_METHOD(vm->string_class, "[_]", prim_name(String_subscript));
    BIND_PRIM_METHOD(vm->string_class, "byte_at(_)", prim_name(String_byte_at));
    BIND_PRIM_METHOD(vm->string_class, "byte_count", prim_name(String_byte_count));
    BIND_PRIM_METHOD(vm->string_class, "code_point_at(_)", prim_name(String_code_point_at));
    BIND_PRIM_METHOD(vm->string_class, "contains(_)", prim_name(String_contains));
    BIND_PRIM_METHOD(vm->string_class, "ends_with(_)", prim_name(String_ends_with));
    BIND_PRIM_METHOD(vm->string_class, "starts_with(_)", prim_name(String_starts_with));
    BIND_PRIM_METHOD(vm->string_class, "index_of(_)", prim_name(String_index_of));
    BIND_PRIM_METHOD(vm->string_class, "iterate(_)", prim_name(String_iterate));
    BIND_PRIM_METHOD(vm->string_class, "iterator_value(_)", prim_name(String_iterator_value));
    BIND_PRIM_METHOD(vm->string_class, "iterate_byte(_)", prim_name(String_iterate_byte));
    BIND_PRIM_METHOD(vm->string_class, "to_string()", prim_name(String_to_string));
    BIND_PRIM_METHOD(vm->string_class, "len", prim_name(String_byte_count));

    vm->list_class = VALUE_TO_CLASS(get_core_class_value(core_module, "List"));
    // static
    BIND_PRIM_METHOD(vm->list_class->header.class, "new()", prim_name(List_new));
    // field
    BIND_PRIM_METHOD(vm->list_class, "[_]", prim_name(List_subscript));
    BIND_PRIM_METHOD(vm->list_class, "[_]=(_)", prim_name(List_subscript_set));
    BIND_PRIM_METHOD(vm->list_class, "set(_,_)", prim_name(List_subscript_set));
    BIND_PRIM_METHOD(vm->list_class, "append(_)", prim_name(List_append));
    BIND_PRIM_METHOD(vm->list_class, "core_append(_)", prim_name(List_core_append));
    BIND_PRIM_METHOD(vm->list_class, "clear(_)", prim_name(List_clear));
    BIND_PRIM_METHOD(vm->list_class, "len", prim_name(List_len));
    BIND_PRIM_METHOD(vm->list_class, "insert(_,_)", prim_name(List_insert));
    BIND_PRIM_METHOD(vm->list_class, "iterate(_)", prim_name(List_iterate));
    BIND_PRIM_METHOD(vm->list_class, "iterator_value(_)", prim_name(List_iterator_value));
    BIND_PRIM_METHOD(vm->list_class, "remove_at(_)", prim_name(List_remove_at));

    vm->map_class = VALUE_TO_CLASS(get_core_class_value(core_module, "Map"));
    // static
    BIND_PRIM_METHOD(vm->map_class->header.class, "new()", prim_name(Map_new));
    // field
    BIND_PRIM_METHOD(vm->map_class, "[_]", prim_name(Map_subscript));
    BIND_PRIM_METHOD(vm->map_class, "[_]=(_)", prim_name(Map_subscript_set));
    BIND_PRIM_METHOD(vm->map_class, "core_insert(_,_)", prim_name(Map_core_insert));
    BIND_PRIM_METHOD(vm->map_class, "insert(_,_)", prim_name(Map_subscript_set));
    BIND_PRIM_METHOD(vm->map_class, "clear()", prim_name(Map_clear));
    BIND_PRIM_METHOD(vm->map_class, "contains_key(_)", prim_name(Map_contains_key));
    BIND_PRIM_METHOD(vm->map_class, "len", prim_name(Map_len));
    BIND_PRIM_METHOD(vm->map_class, "remove(_)", prim_name(Map_remove));
    BIND_PRIM_METHOD(vm->map_class, "iterate(_)", prim_name(Map_iterate));
    BIND_PRIM_METHOD(vm->map_class, "key_iterator_value(_)", prim_name(Map_key_iterator_value));
    BIND_PRIM_METHOD(vm->map_class, "val_iterator_value(_)", prim_name(Map_val_iterator_value));

    vm->range_class = VALUE_TO_CLASS(get_core_class_value(core_module, "Range"));
    // static
    BIND_PRIM_METHOD(vm->range_class->header.class, "new(_)", prim_name(Range_new_arg1));
    BIND_PRIM_METHOD(vm->range_class->header.class, "new(_,_)", prim_name(Range_new_arg2));
    BIND_PRIM_METHOD(vm->range_class->header.class, "new(_,_,_)", prim_name(Range_new_arg3));
    // field
    BIND_PRIM_METHOD(vm->range_class, "from", prim_name(Range_from));
    BIND_PRIM_METHOD(vm->range_class, "to", prim_name(Range_to));
    BIND_PRIM_METHOD(vm->range_class, "step", prim_name(Range_step));
    BIND_PRIM_METHOD(vm->range_class, "iterate(_)", prim_name(Range_iterate));
    BIND_PRIM_METHOD(vm->range_class, "iterator_value(_)", prim_name(Range_iterator_value));

    Class* system = VALUE_TO_CLASS(get_core_class_value(core_module, "System"));
    BIND_PRIM_METHOD(system->header.class, "clock()", prim_name(System_clock));
    BIND_PRIM_METHOD(system->header.class, "import_module(_)", prim_name(System_import_module));
    BIND_PRIM_METHOD(system->header.class, "get_module_variable(_,_)", prim_name(System_get_module_variable));
    BIND_PRIM_METHOD(system->header.class, "write_string(_)", prim_name(System_write_string));
    
    Class* vm_class = VALUE_TO_CLASS(get_core_class_value(core_module, "VM"));
    BIND_PRIM_METHOD(vm_class->header.class, "gc()", prim_name(VM_gc));
    BIND_PRIM_METHOD(vm_class->header.class, "allocated_bytes", prim_name(VM_allocated_bytes));

    vm->native_pointer_class = VALUE_TO_CLASS(get_core_class_value(core_module, "NativePointer"));
    BIND_PRIM_METHOD(vm->native_pointer_class, "check_classifier(_)", prim_name(NativePointer_check_classifier));
    BIND_PRIM_METHOD(vm->native_pointer_class, "is_null", prim_name(NativePointer_is_null));

    if (CFILE_ptr_classifier == NULL ) {
        CFILE_ptr_classifier = objstring_new(vm, "FILE", 4);
        BufferAdd(Value, &vm->allways_keep_roots, vm, OBJ_TO_VALUE(CFILE_ptr_classifier));
    }
    Class* cfile_class = VALUE_TO_CLASS(get_core_class_value(core_module, "CFILE"));
    BIND_PRIM_METHOD(cfile_class->header.class, "stdin", prim_name(CFILE_stdin));
    BIND_PRIM_METHOD(cfile_class->header.class, "stdout", prim_name(CFILE_stdout));
    BIND_PRIM_METHOD(cfile_class->header.class, "stderr", prim_name(CFILE_stderr));
    BIND_PRIM_METHOD(cfile_class->header.class, "fopen(_,_)", prim_name(CFILE_fopen));
    BIND_PRIM_METHOD(cfile_class->header.class, "fclose(_)", prim_name(CFILE_fclose));

    // 编译core.sp时字符串对象初始化时vm->string->class为NULL，现重新填充
    ObjHeader* header = vm->all_objs;
    while (header != NULL) {
        if (header->type == OT_STRING) {
            header->class = vm->string_class;
        }
        header = header->next;
    }
}
