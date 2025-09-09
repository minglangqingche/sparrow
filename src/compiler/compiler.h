#ifndef __COMPILER_H__
#define __COMPILER_H__

#include "obj_fn.h"
#include "utils.h"

#define MAX_LOCAL_VAR_NUM   128
#define MAX_UPVALUE_NUM     128
#define MAX_ID_LEN          128
#define MAX_FIELD_NUM       128
#define MAX_METHOD_NAME_LEN 128
#define MAX_ARG_NUM         16
#define MAX_SIGN_LEN        MAX_METHOD_NAME_LEN + (MAX_ARG_NUM * 2 - 1) + 2

typedef struct {
    bool is_enclosing_local_var;
    u32 index;
} Upvalue;

typedef struct {
    const char* name;
    u32 len;

    int scope_depth;

    bool is_upvalue;
} LocalVar;

typedef enum {
    SIGN_CONSTRUCT,
    SIGN_METHOD,
    SIGN_GETTER,
    SIGN_SETTER,
    SIGN_SUBSCRIPT,
    SIGN_SUBSCRIPT_SETTER,
} SignatureType;

typedef struct {
    SignatureType type;
    const char* name;
    u32 len;
    u32 argc;
} Signature;

typedef struct _Loop {
    int cond_start_index;
    int body_start_index;
    int exit_index;
    int scope_depth;
    struct _Loop* enclosing_loop;
} Loop;

typedef struct {
    ObjString* name;
    SymbolTable fields;
    bool in_static;
    BufferType(Int) instant_methods;
    BufferType(Int) static_methods;
    Signature* signature; // 当前正在编译的函数签名
} ClassBookKeep;

typedef struct _CompileUnit {
    ObjFn* fn; // 正在编译的函数
    
    LocalVar local_vars[MAX_LOCAL_VAR_NUM];
    u32 local_vars_count;

    Upvalue upvalues[MAX_LOCAL_VAR_NUM];

    int scope_depth;
    u32 stack_slot_num;
    Loop* cur_loop;
    ClassBookKeep* enclosing_classbk;
    struct _CompileUnit* enclosing_unit;
    Parser* parser;
} CompileUnit;

u32 get_byte_of_operands(Byte* instr_stream, Value* constants, int ip);
int ensure_symbol_exist(VM* vm, SymbolTable* table, const char* symbol, u32 len);

#endif