#ifndef __OBJECT_META_OBJ_H__
#define __OBJECT_META_OBJ_H__

#include "header_obj.h"
#include "obj_string.h"
#include "utils.h"

typedef struct {
    ObjHeader header;
    SymbolTable module_var_name;  // 存储模块中全局变量名
    ValueBuffer module_var_value; // 存储模块中全局变量值
    ObjString* name;
} ObjModule;

typedef struct {
    ObjHeader header;
    Value fields[];
} ObjInstance;

ObjModule* objmodule_new(VM* vm, const char* mod_name);
ObjInstance* objinstance_new(VM* vm, Class* class);

#endif