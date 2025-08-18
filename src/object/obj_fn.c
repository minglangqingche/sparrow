#include "obj_fn.h"
#include "class.h"
#include "header_obj.h"
#include "utils.h"
#include "vm.h"

ObjUpvalue* objupvalue_new(VM* vm, Value* local_var_ptr) {
    ObjUpvalue* obj = ALLOCATE(vm, ObjUpvalue);
    objheader_init(vm, &obj->header, OT_UPVALUE, NULL);
    obj->local_var_ptr = local_var_ptr;
    obj->closed_upvalue = VT_TO_VALUE(VT_NULL);
    obj->next = NULL;
    return obj;
}

ObjClosure* objclosure_new(VM* vm, ObjFn* objfn) {
    ObjClosure* obj = ALLOCATE_EXTRA(vm, ObjClosure, sizeof(ObjUpvalue*) * objfn->upvalue_number);

    objheader_init(vm, &obj->header, OT_CLOSURE, vm->fn_class);
    
    obj->fn = objfn;

    for (int i = 0; i < objfn->upvalue_number; i++) {
        obj->upvalue[i] = NULL;
    }

    return obj;
}

ObjFn* objfn_new(VM* vm, ObjModule* module, u32 max_stack_slot_used) {
    ObjFn* obj = ALLOCATE(vm, ObjFn);
    if (obj == NULL) {
        MEM_ERROR("allocate ObjFn failed.");
    }

    objheader_init(vm, &obj->header, OT_FUNCTION, vm->fn_class);

    BufferInit(Byte, &obj->instr_stream);
    BufferInit(Value, &obj->constants);

    obj->module = module;
    obj->max_stack_slot_used = max_stack_slot_used;
    
    obj->upvalue_number = 0;
    obj->argc = 0;

#ifdef DEBUG
    obj->debug = ALLOCATE(vm, FnDebug);
    obj->debug->fn_name = NULL;
    BufferInit(Int, &obj->debug->line);
#endif

    return obj;
}
