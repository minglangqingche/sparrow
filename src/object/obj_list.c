#include "obj_list.h"
#include "class.h"
#include "header_obj.h"
#include "utils.h"
#include "vm.h"

ObjList* objlist_new(VM* vm, u32 element_count) {
    Value* element_array = element_count <= 0 ? NULL : ALLOCATE_ARRAY(vm, Value, element_count);
    ObjList* obj = ALLOCATE(vm, ObjList);
    obj->elements = (BufferType(Value)){
        .datas = element_array,
        .capacity = element_count,
        .count = element_count,
    };
    objheader_init(vm, &obj->header, OT_LIST, vm->list_class);
    return obj;
}

void objlist_insert_element(VM* vm, ObjList* list, u32 index, Value value) {
    if (index > list->elements.count - 1) {
        RUNTIME_ERROR("index out of bounded.");
    }

    if (VALUE_IS_OBJ(value)) {
        push_tmp_root(vm, value.header);
    }

    BufferAdd(Value, &list->elements, vm, value);

    if (VALUE_IS_OBJ(value)) {
        pop_tmp_root(vm);
    }

    for (int i = list->elements.count - 1; i > index; i--) {
        list->elements.datas[i] = list->elements.datas[i - 1];
    }

    list->elements.datas[index] = value;
}

static void shrink_list(VM* vm, ObjList* list, u32 new_capacity) {
    u32 old_size = list->elements.capacity * sizeof(Value);
    u32 new_size = new_capacity * sizeof(Value);
    mem_manager(vm, list->elements.datas, old_size, new_size);
    list->elements.capacity = new_capacity;
}

Value objlist_remove_element(VM* vm, ObjList* list, u32 index) {
    Value value_removed = list->elements.datas[index];
    if (VALUE_IS_OBJ(value_removed)) {
        push_tmp_root(vm, value_removed.header);
    }

    for (int i = index; i < list->elements.count - 1; i++) {
        list->elements.datas[i] = list->elements.datas[i + 1];
    }
    list->elements.count--;

    u32 new_capacity = list->elements.capacity / CAPACITY_GROW_FACTOR;
    if (new_capacity > list->elements.count) {
        shrink_list(vm, list, new_capacity);
    }
    
    if (VALUE_IS_OBJ(value_removed)) {
        pop_tmp_root(vm);
    }
    return value_removed;
}
