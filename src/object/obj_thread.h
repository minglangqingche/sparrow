#ifndef __OBJECT_OBJ_THREAD_H__
#define __OBJECT_OBJ_THREAD_H__

#include "header_obj.h"
#include "obj_fn.h"

typedef struct _ObjThread {
    ObjHeader header;
    
    Value* stack;
    Value* esp;
    u32 stack_capacity;

    Frame* frames;
    u32 used_frame_num;
    u32 frame_capacity;

    ObjUpvalue* open_upvalue;
    
    struct _ObjThread* caller;

    Value error_obj;
} ObjThread;

void prepare_frame(ObjThread* thread, ObjClosure* closure, Value* stack_start);
ObjThread* objthread_new(VM* vm, ObjClosure* closure);
void objthread_reset(ObjThread* thread, ObjClosure* closure);

#endif