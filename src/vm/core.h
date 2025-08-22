#ifndef __VM_CORE_H__
#define __VM_CORE_H__

#include "vm.h"
#include "class.h"

#define STD_LIB_PATH     "/Users/minglangqingche/sparrow/lib/"
#define SCRIPT_EXTENSION ".sp"

extern char* root_dir;

char* read_file(const char* path);
VMResult execute_module(VM* vm, Value module_name, const char* module_code);
void build_core(VM* vm);
int add_symbol(VM* vm, SymbolTable* table, const char* symbol, u32 len);
int get_index_from_symbol_table(SymbolTable* table, const char* symbol, u32 len);
void bind_super_class(VM* vm, Class* sub_class, Class* super_calss);
void bind_method(VM* vm, Class* class, u32 index, Method method);

#endif