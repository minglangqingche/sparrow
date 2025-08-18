#include "meta_obj.h"
#include <string.h>
#include "class.h"
#include "header_obj.h"
#include "utils.h"
#include "vm.h"

ObjModule* objmodule_new(VM* vm, const char* mod_name) {
    ObjModule* obj = ALLOCATE(vm, ObjModule);
    if (obj == NULL) {
        MEM_ERROR("allocate ObjModule failed.");
    }

    objheader_init(vm, &obj->header, OT_MODULE, NULL);
    push_tmp_root(vm, (ObjHeader*)obj);
    
    BufferInit(String, &obj->module_var_name);
    BufferInit(Value, &obj->module_var_value);

    obj->name = mod_name == NULL ? NULL : objstring_new(vm, mod_name, strlen(mod_name));

    pop_tmp_root(vm);
    return obj;
}

ObjInstance* objinstance_new(VM* vm, Class* class) {
    ObjInstance* obj = ALLOCATE_EXTRA(vm, ObjInstance, sizeof(Value) * class->field_number);
    if (obj == NULL) {
        MEM_ERROR("allocate ObjInstance failed.");
    }

    objheader_init(vm, &obj->header, OT_INSTANCE, class);

    for (int i = 0; i < class->field_number; i++) {
        obj->fields[i] = VT_TO_VALUE(VT_NULL);
    }

    return obj;
}
