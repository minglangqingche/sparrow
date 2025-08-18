#ifndef __VM_VM_H__
#define __VM_VM_H__

#include "common.h"
#include "header_obj.h"
#include "obj_map.h"
#include "utils.h"
#include "obj_thread.h"

#define MAX_TEMP_ROOTS_NUM 8

#define OPCODE_SLOTS(opcode, effect) OPCODE_##opcode,
typedef enum {
    #include "opcode.inc"
} OpCode;
#undef OPCODE_SLOTS

typedef enum {
    VM_RES_SUCCESS,
    VM_RES_ERROR,
} VMResult;

typedef struct {
    ObjHeader** gray_objs;
    u32 capacity;
    u32 count;
} Gray;

typedef struct {
    double heap_growth_factor;
    u32 initial_heap_size;
    u32 min_heap_size;
    u32 next_gc;
} Configuration;

struct _VM {
    u32 allocated_bytes;
    ObjHeader* all_objs;
    SymbolTable all_method_names;
    ObjMap* all_module;
    ObjThread* cur_thread;
    Parser* cur_parser;

    ObjHeader* tmp_roots[MAX_TEMP_ROOTS_NUM];
    u32 tmp_roots_num;
    Gray grays;
    Configuration config;

    Class* string_class;
    Class* fn_class;
    Class* list_class;
    Class* range_class;
    Class* map_class;
    Class* class_of_class;
    Class* object_class;
    Class* null_class;
    Class* bool_class;
    Class* i32_class;
    Class* f64_class;
    Class* thread_class;
};

void vm_init(VM* vm);
VM* vm_new();
void vm_free(VM* vm);
VMResult execute_instruction(VM* vm, register ObjThread* cur_thread);
void push_tmp_root(VM* vm, ObjHeader* obj);
void pop_tmp_root(VM* vm);

#endif