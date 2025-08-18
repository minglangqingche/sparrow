#include "obj_thread.h"
#include "class.h"
#include "common.h"
#include "header_obj.h"
#include "obj_fn.h"
#include "utils.h"
#include "vm.h"

void prepare_frame(ObjThread* thread, ObjClosure* closure, Value* stack_start) {
    ASSERT(thread->frame_capacity > thread->used_frame_num, "frame not enough.");

    Frame* frame = &(thread->frames[thread->used_frame_num++]);
    frame->stack_start = stack_start;
    frame->closure = closure;
    frame->ip = closure->fn->instr_stream.datas;
}

ObjThread* objthread_new(VM* vm, ObjClosure* closure) {
    ASSERT(closure != NULL, "closure is null.");

    Frame* frames = ALLOCATE_ARRAY(vm, Frame, INITIAL_FRAME_NUM);

    u32 stack_capacity = ceil_to_power_of_2(closure->fn->max_stack_slot_used + 1);
    Value* new_stack = ALLOCATE_ARRAY(vm, Value, stack_capacity);

    ObjThread* thread = ALLOCATE(vm, ObjThread);
    objheader_init(vm, &thread->header, OT_THREAD, vm->thread_class);
    thread->frames = frames;
    thread->frame_capacity = INITIAL_FRAME_NUM;
    thread->stack = new_stack;
    thread->stack_capacity = stack_capacity;

    objthread_reset(thread, closure);

    return thread;
}

void objthread_reset(ObjThread* thread, ObjClosure* closure) {
    ASSERT(closure != NULL, "closure is null.");
    
    thread->esp = thread->stack;
    thread->open_upvalue = NULL;
    thread->caller = NULL;
    thread->error_obj = VT_TO_VALUE(VT_NULL);
    thread->used_frame_num = 0;
    
    prepare_frame(thread, closure, thread->stack);
}
