#ifndef __OBJECT_OBJ_LIST_H__
#define __OBJECT_OBJ_LIST_H__

#include "header_obj.h"
#include "utils.h"

struct _ObjList {
    ObjHeader header;
    BufferType(Value) elements;
};

ObjList* objlist_new(VM* vm, u32 element_count);
Value objlist_remove_element(VM* vm, ObjList* list, u32 index);
void objlist_insert_element(VM* vm, ObjList* list, u32 index, Value value);

#endif