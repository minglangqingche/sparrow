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

// NativePointer 相关
typedef struct _ObjNativePointer ObjNativePointer;
typedef void (*Destroy)(ObjNativePointer*);

// ObjString
typedef struct _ObjString ObjString;

// ObjList
typedef struct _ObjList ObjList;

typedef struct _SprApi SprApi;
struct _SprApi {
    // 虚拟机信息。
    VM* vm; // 获取dylib的vm
    Class* class; // dylib将绑定到该类上

    // 工具函数
    void (*set_error)(SprApi* api, const char* msg);
    
    ObjNativePointer* (*create_native_pointer)(VM* vm, void* ptr, ObjString* classifier, Destroy destroy);
    int (*validate_native_pointer)(SprApi* api, Value val, ObjString* expect);
    void* (*unpack_native_pointer)(ObjNativePointer* ptr);
    void (*set_native_pointer)(ObjNativePointer* ptr, void* p);

    bool (*validate_string)(Value val, const char** res, u32* len);
    ObjString* (*create_string)(VM* vm, const char* str, u32 len);

    ObjList* (*create_list)(VM* vm, u32 element_count);
    Value* (*list_elements)(ObjList* list, int* len);

    // 如果需要在一个函数中生成多个对象，就需要使用这两个函数将对象加入临时对象根中。
    u32 tmp_obj_count; // 计数到底有多少个对象加入了临时对象根
    void (*push_tmp_obj)(SprApi* api, Value* val);
    void (*release_tmp_obj)(SprApi* api); // 退出函数之前调用可以一次性释放
    void (*push_keep_root)(SprApi* api, Value val);

    // 注册器
    void (*register_method)(SprApi* api, const char* sign_str, Primitive func, bool is_static);
};

// 动态库初始化函数，需导出给虚拟机。
typedef void (*SprDyLibInit)(SprApi api); // 约定名称为 pub_spr_dylib_init

#endif