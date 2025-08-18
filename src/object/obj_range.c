#include "obj_range.h"
#include "header_obj.h"
#include "utils.h"
#include "vm.h"

ObjRange* objrange_new(VM* vm, int from, int to, int step) {
    ObjRange* obj = ALLOCATE(vm, ObjRange);
    objheader_init(vm, &obj->header, OT_RANGE, vm->range_class);
    obj->from = from;
    obj->step = step;
    obj->to = to;
    return obj;
}
