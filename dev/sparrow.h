#ifndef __DEV_SPARROW_H__
#define __DEV_SPARROW_H__

#include "common.h"

typedef struct objHeader ObjHeader;

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

// sparrow 中的值
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

// native 函数的定义
typedef bool (*Primitive)(VM* vm, Value* args);

typedef struct _SprApi SprApi;
struct _SprApi {
    // 虚拟机信息。
    VM* vm; // 获取dylib的vm
    Class* class; // dylib将绑定到该类上
    
    // 工具函数
    void (*set_error)(SprApi* api, const char* msg);

    // 注册器
    void (*register_method)(SprApi* api, const char* sign_str, Primitive func, bool is_static);
};

// 动态库初始化函数，需导出给虚拟机。
typedef void (*SprDyLibInit)(SprApi api); // 约定名称为 pub_spr_dylib_init

#endif