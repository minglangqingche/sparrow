#include "class.h"
#include "common.h"
#include <string.h>
#include "compiler.h"
#include "header_obj.h"
#include "obj_native_pointer.h"
#include "obj_range.h"
#include "core.h"
#include "obj_string.h"
#include "sparrow.h"
#include "utils.h"
#include "vm.h"

DEFINE_BUFFER_METHOD(Method)

bool value_is_equal(Value a, Value b) {
    if (a.type != b.type) {
        return false;
    }

    if (a.type == VT_NULL || a.type == VT_FALSE || a.type == VT_TRUE) {
        return true;
    }

    if (a.type == VT_I32) {
        return a.i32val == b.i32val;
    }

    if (a.type == VT_F64) {
        return a.f64val == b.f64val;
    }

    if (a.type == VT_U32) {
        return a.u32val == b.u32val;
    }

    if (a.type == VT_U8) {
        return a.u8val == b.u8val;
    }

    if (a.header == b.header) {
        return true;
    }

    if (a.header->type != b.header->type) {
        return false;
    }

    if (a.header->type == OT_STRING) {
        ObjString* str_a = VALUE_TO_STRING(a);
        ObjString* str_b = VALUE_TO_STRING(b);
        return (str_a->val.len == str_b->val.len)
            && (str_a->hash_code == str_b->hash_code)
            && (memcmp(str_a->val.start, str_b->val.start, str_a->val.len) == 0);
    }

    if (a.header->type == OT_RANGE) {
        ObjRange* ra = VALUE_TO_RANGE(a);
        ObjRange* rb = VALUE_TO_RANGE(b);
        return (ra->from == rb->from) && (ra->to == rb->to) && (ra->step == rb->step);
    }

    if (a.header->type == OT_NATIVE_POINTER) {
        return native_pointer_is_eq((ObjNativePointer*)a.header, (ObjNativePointer*)b.header);
    }

    return false;
}

Class* class_new_raw(VM* vm, const char* name, u32 field_num) {
    Class* class = ALLOCATE(vm, Class);
    objheader_init(vm, &class->header, OT_CLASS, class);

    push_tmp_root(vm, (ObjHeader*)class);

    class->name = objstring_new(vm, name, strlen(name));
    class->field_number = field_num;
    class->super_class = NULL;
    BufferInit(Method, &class->methods);

    pop_tmp_root(vm);
    return class;
}

inline Class* get_class_of_object(VM* vm, Value object) {
    switch (object.type) {
        case VT_U8:
            return vm->u8_class;
        case VT_U32:
            return vm->u32_class;
        case VT_I32:
            return vm->i32_class;
        case VT_F64:
            return vm->f64_class;
        case VT_TRUE:
        case VT_FALSE:
            return vm->bool_class;
        case VT_NULL:
            return vm->null_class;
        case VT_OBJ:
            return VALUE_TO_OBJ(object)->class;
        case VT_UNDEFINED:
            RUNTIME_ERROR("get class of UNDEFINED value.");
            return NULL;
        default:
            UNREACHABLE();
    }
}

Class* class_new(VM* vm, ObjString* class_name, u32 field_num, Class* super_class) {
    #define MAX_METACLASS_LEN (MAX_ID_LEN + 5)
    char new_class_name[MAX_METACLASS_LEN] = {'\0'};
    #undef MAX_METACLASS_LEN

    memcpy(new_class_name, class_name->val.start, class_name->val.len);
    memcpy(new_class_name + class_name->val.len, "@Meta", 5);

    Class* metaclass = class_new_raw(vm, new_class_name, 0);
    push_tmp_root(vm, (ObjHeader*)metaclass);
    // metaclass的class和其super都是class_of_class
    metaclass->header.class = vm->class_of_class;
    bind_super_class(vm, metaclass, vm->class_of_class);

    memcpy(new_class_name, class_name->val.start, class_name->val.len);
    new_class_name[class_name->val.len] = 0;

    Class* class = class_new_raw(vm, new_class_name, field_num);
    push_tmp_root(vm, (ObjHeader*)class);
    // class的class是metaclass
    class->header.class = metaclass;
    bind_super_class(vm, class, super_class);

    pop_tmp_root(vm);
    pop_tmp_root(vm);

    return class;
}
