#ifndef __OBJECT_OBJ_FN_H__
#define __OBJECT_OBJ_FN_H__

#include "header_obj.h"
#include "meta_obj.h"
#include "utils.h"

typedef struct {
    char* fn_name;
    BufferType(Int) line;
} FnDebug;

typedef struct {
    ObjHeader header;
    BufferType(Byte) instr_stream; // 指令流
    ValueBuffer constants; // 常量表
    ObjModule* module;
    u32 max_stack_slot_used;
    u32 upvalue_number;
    u8 argc;
#ifdef DEBUG
    FnDebug* debug;
#endif
} ObjFn;

typedef struct _Upvalue {
    ObjHeader header;
    Value* local_var_ptr;
    Value closed_upvalue;
    struct _Upvalue* next;
} ObjUpvalue;

typedef struct {
    ObjHeader header;
    ObjFn* fn;
    ObjUpvalue* upvalue[];
} ObjClosure;

typedef struct {
    u8* ip;
    ObjClosure* closure;
    Value* stack_start;
} Frame;

#define INITIAL_FRAME_NUM 4

ObjUpvalue* objupvalue_new(VM* vm, Value* local_var_ptr);
ObjClosure* objclosure_new(VM* vm, ObjFn* objfn);
ObjFn* objfn_new(VM* vm, ObjModule* module, u32 max_stack_slot_used);

#endif