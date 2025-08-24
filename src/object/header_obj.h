#ifndef __OBJECT_HEADER_OBJ_H__
#define __OBJECT_HEADER_OBJ_H__

#include "utils.h"
#include "sparrow.h"

typedef enum {
    OT_CLASS,
    OT_UPVALUE,
    OT_MODULE,
    OT_LIST,
    OT_MAP,
    OT_RANGE,
    OT_STRING,
    OT_FUNCTION,
    OT_CLOSURE,
    OT_INSTANCE,
    OT_THREAD,
    OT_NATIVE_POINTER,
} ObjType;

typedef struct objHeader {
    ObjType type;
    bool is_dark;
    Class* class; // 对象的元信息类(meta-class)，提示对象类型。
    struct objHeader* next;
} ObjHeader;

DECLARE_BUFFER_TYPE(Value)

void objheader_init(VM* vm, ObjHeader* header, ObjType type, Class* class);

#endif