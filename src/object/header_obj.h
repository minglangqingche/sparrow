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

typedef enum {
    VT_UNDEFINED,
    VT_NULL,
    VT_FALSE,
    VT_TRUE,
    VT_I32,
    VT_U32,
    VT_U8,
    VT_F64,
    VT_OBJ,
} ValueType;

typedef struct _Value {
    ValueType type;
    union {
        u8 u8val;
        u32 u32val;
        i32 i32val;
        double f64val;
        ObjHeader* header;
    };
} Value;

DECLARE_BUFFER_TYPE(Value)

void objheader_init(VM* vm, ObjHeader* header, ObjType type, Class* class);

#endif