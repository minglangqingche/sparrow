#include "header_obj.h"
#include "class.h"
#include "vm.h"

DEFINE_BUFFER_METHOD(Value)

void objheader_init(VM* vm, ObjHeader* header, ObjType type, Class* class) {
    header->type = type;
    header->is_dark = false;
    header->class = class;
    
    header->next = vm->all_objs;
    vm->all_objs = header;
}
