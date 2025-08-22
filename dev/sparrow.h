#ifndef __DEV_SPARROW_H__
#define __DEV_SPARROW_H__

#include "common.h"

// sparrow 中的值。内部细节不公开，修改需要使用 api 提供的函数
typedef struct _Value Value;

// native 函数的定义
typedef bool (*Primitive)(VM* vm, Value* args);

typedef struct _SprApi SprApi;
struct _SprApi {
    // 虚拟机信息。
    VM* vm; // 获取dylib的vm
    Class* class; // dylib将绑定到该类上
    
    // 工具函数
    bool (*validate_i32)(Value val, i32* res);
    Value (*i32_to_value)(i32 val);

    bool (*validate_f64)(Value val, f64* res);
    Value (*f64_to_value)(f64 val);

    void (*set_error)(SprApi* api, const char* msg);

    // 注册器
    void (*register_method)(SprApi* api, const char* sign_str, Primitive func, bool is_static);
};

// 动态库初始化函数，需导出给虚拟机。
typedef void (*SprDyLibInit)(SprApi api); // 约定名称为 pub_spr_dylib_init

#endif