#ifndef __ONE_PASS_COMPILER_H__
#define __ONE_PASS_COMPILER_H__

#include "obj_fn.h"

int one_pass_define_module_var(VM* vm, ObjModule* module, const char* name, u32 len, Value val);
ObjFn* one_pass_compile_module(VM* vm, ObjModule* module, const char* module_code);

#endif