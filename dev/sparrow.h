#ifndef __DEV_SPARROW_H__
#define __DEV_SPARROW_H__

#include "common.h"

// sparrow 中的值。内部细节不公开，修改需要使用 api 提供的函数
typedef struct _Value Value;

// native 函数的定义
typedef bool (*Primitive)(VM* vm, Value* args);

// native function 注册函数
typedef void (*Register)(VM* vm, Class* class, Primitive func);

typedef struct {

} SprApi;

// 动态库初始化函数，需导出给虚拟机。
typedef void (*DyLibInit)(SprApi api);

#endif