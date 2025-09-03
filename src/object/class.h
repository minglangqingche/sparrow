#ifndef __OBJECT_CLASS_H__
#define __OBJECT_CLASS_H__

#include "common.h"
#include "utils.h"
#include "header_obj.h"
#include "obj_string.h"
#include "obj_fn.h"

#define VT_TO_VALUE(vt)         ((Value){vt, {0}})
#define BOOL_TO_VALUE(b)        (b ? VT_TO_VALUE(VT_TRUE) : VT_TO_VALUE(VT_FALSE))
#define I32_TO_VALUE(i)         ((Value){.type = VT_I32, .i32val = (i)})
#define U32_TO_VALUE(i)         ((Value){.type = VT_U32, .u32val = (i)})
#define U8_TO_VALUE(i)         ((Value){.type = VT_U8,  .u8val = (i)})
#define F64_TO_VALUE(f)         ((Value){.type = VT_F64, .f64val = (f)})
#define OBJ_TO_VALUE(obj)       ((Value){.type = VT_OBJ, .header = (ObjHeader*)(obj)})

#define VALUE_TO_BOOL(v)        ((v).type == VT_TRUE ? true : false)
#define VALUE_TO_I32(v)         ((v).i32val)
#define VALUE_TO_U32(v)         ((v).u32val)
#define VALUE_TO_F64(v)         ((v).f64val)
#define VALUE_TO_OBJ(v)         (v.header)
#define VALUE_TO_OBJSTR(v)      ((ObjString*)VALUE_TO_OBJ(v))
#define VALUE_TO_OBJFN(v)       ((ObjFn*)VALUE_TO_OBJ(v))
#define VALUE_TO_OBJCLOSURE(v)  ((ObjClosure*)VALUE_TO_OBJ(v))
#define VALUE_TO_CLASS(v)       ((Class*)VALUE_TO_OBJ(v))
#define VALUE_TO_STRING(v)      ((ObjString*)VALUE_TO_OBJ(v))
#define VALUE_TO_RANGE(v)       ((ObjRange*)VALUE_TO_OBJ(v))
#define VALUE_TO_OBJMODULE(v)   ((ObjModule*)VALUE_TO_OBJ(v))
#define VALUE_TO_INSTANCE(v)    ((ObjInstance*)VALUE_TO_OBJ(v))
#define VALUE_TO_THREAD(v)      ((ObjThread*)VALUE_TO_OBJ(v))
#define VALUE_TO_LIST(v)        ((ObjList*)VALUE_TO_OBJ(v))
#define VALUE_TO_OBJMAP(v)      ((ObjMap*)VALUE_TO_OBJ(v))
#define VALUE_TO_NATIVE_POINTER(v) ((ObjNativePointer*)VALUE_TO_OBJ(v))

#define VALUE_IS_UNDEFINED(v)   (v.type == VT_UNDEFINED)
#define VALUE_IS_FALSE(v)       (v.type == VT_FALSE)
#define VALUE_IS_TRUE(v)        (v.type == VT_TRUE)
#define VALUE_IS_BOOL(v)        (VALUE_IS_TRUE(v) || VALUE_IS_FALSE(v))
#define VALUE_IS_I32(v)         (v.type == VT_I32)
#define VALUE_IS_U32(v)         (v.type == VT_U32)
#define VALUE_IS_U8(v)          (v.type == VT_U8)
#define VALUE_IS_F64(v)         (v.type == VT_F64)
#define VALUE_IS_NULL(v)        (v.type == VT_NULL)
#define VALUE_IS_NUM(v)         (VALUE_IS_I32(v) || VALUE_IS_F64(v) || VALUE_IS_U32(v))
#define VALUE_IS_OBJ(v)         (v.type == VT_OBJ)
#define VALUE_IS_CLASS(v)       (v.type == VT_OBJ && VALUE_TO_OBJ(v)->type == OT_CLASS)
#define VALUE_IS_INSTANCE(v)    (v.type == VT_OBJ && VALUE_TO_OBJ(v)->type == OT_INSTANCE)
#define VALUE_IS_STRING(v)      (v.type == VT_OBJ && VALUE_TO_OBJ(v)->type == OT_STRING)
#define VALUE_IS_CLOSURE(v)     (v.type == VT_OBJ && VALUE_TO_OBJ(v)->type == OT_CLOSURE)
#define VALUE_IS_RANGE(v)       (v.type == VT_OBJ && VALUE_TO_OBJ(v)->type == OT_RANGE)
#define VALUE_IS_NATIVE_POINTER(v) (v.type == VT_OBJ && VALUE_TO_OBJ(v)->type == OT_NATIVE_POINTER)

#define CLASS_IS_BUILTIN(vm, c) (c == vm->string_class || c == vm->fn_class || c == vm->list_class || c == vm->range_class || c == vm->map_class || c == vm->null_class || c == vm->bool_class || c == vm->i32_class || c == vm->f64_class || c == vm->thread_class || c == vm->native_pointer_class)

typedef enum {
    MT_NONE,
    MT_PRIMITIVE,
    MT_SCRIPT,
    MT_FN_CALL,
} MethodType;

typedef struct {
    MethodType type;
    union {
        Primitive prim;
        ObjClosure* obj;
    };
} Method;

DECLARE_BUFFER_TYPE(Method)

struct _Class {
    ObjHeader header;
    Class* super_class; // 对象的父类
    u32 field_number;
    BufferType(Method) methods;
    ObjString* name;
};

typedef union {
    u64 bits64;
    u32 bits32[2];
    f64 bits_double;
} Bits64;

#define CAPACITY_GROW_FACTOR 4
#define MIN_CAPACITY 64

bool value_is_equal(Value a, Value b);
Class* class_new_raw(VM* vm, const char* name, u32 field_num);
Class* get_class_of_object(VM* vm, Value object);
Class* class_new(VM* vm, ObjString* class_name, u32 field_num, Class* super_class);

#endif