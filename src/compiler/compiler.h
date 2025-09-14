#ifndef __COMPILER_H__
#define __COMPILER_H__

#include "common.h"
#include "obj_fn.h"
#include "utils.h"
#include "opcode.h"

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

typedef struct _CompileUnitPubStruct {
    ObjFn* fn; // 正在编译的函数
    
    LocalVar local_vars[MAX_LOCAL_VAR_NUM];
    u32 local_vars_count;

    Upvalue upvalues[MAX_LOCAL_VAR_NUM];

    int scope_depth;
    u32 stack_slot_num;
    Loop* cur_loop;
    ClassBookKeep* enclosing_classbk;

    VM* vm;
    ObjModule* cur_module;

    struct _CompileUnitPubStruct* enclosing_unit;
} CompileUnitPubStruct;

typedef enum {
    VAR_SCOPE_INVALID,
    VAR_SCOPE_LOCAL,
    VAR_SCOPE_UPVALUE,
    VAR_SCOPE_MODULE,
    VAR_SCOPE_METHOD_FIELD, // field in method
} VarScopeType;

typedef struct {
    VarScopeType scope_type;
    int index;
} Variable;

u32 get_byte_of_operands(Byte* instr_stream, Value* constants, int ip);
int ensure_symbol_exist(VM* vm, SymbolTable* table, const char* symbol, u32 len);
void compile_unit_pubstruct_init(VM* vm, ObjModule* cur_module, CompileUnitPubStruct* cu, CompileUnitPubStruct* enclosing_unit, bool is_method);

typedef void (*CompileProgram)(CompileUnitPubStruct* pub_cu, Parser* parser);
ObjFn* compile_module(VM* vm, ObjModule* module, const char* module_code, CompileProgram compile_program);

// 编译器工具函数
extern const int opcode_slots_used[];
int write_byte(CompileUnitPubStruct* cu, int byte);
void write_opcode(CompileUnitPubStruct* cu, OpCode op);
int write_byte_operand(CompileUnitPubStruct* cu, int operand);
void write_short_operand(CompileUnitPubStruct* cu, int operand);
int write_opcode_byte_operand(CompileUnitPubStruct* cu, OpCode opcode, int operand);
void write_opcode_short_operand(CompileUnitPubStruct* cu, OpCode opcode, int operand);
u32 add_constant(CompileUnitPubStruct* cu, Value constant);
int find_upvalue(CompileUnitPubStruct* cu, const char* name, u32 len);
int find_local(CompileUnitPubStruct* cu, const char* name, u32 len);
Variable get_var_from_local_or_upvalue(CompileUnitPubStruct* cu, const char* name, u32 len);
int add_upvalue(CompileUnitPubStruct* cu, bool is_enclosing_local_var, u32 index);
void emit_load_constant(CompileUnitPubStruct* cu, Value val);
void emit_load_variable(CompileUnitPubStruct* cu, Variable var);
void emit_store_variable(CompileUnitPubStruct* cu, Variable var);
void emit_load_self(CompileUnitPubStruct* cu);
u32 sign2string(Signature* sign, char* buf);
void emit_call_by_signature(CompileUnitPubStruct* cu, Signature* sign, OpCode op);
void emit_call(CompileUnitPubStruct* cu, int argc, const char* name, u32 len);
u32 add_local_var(CompileUnitPubStruct* cu, const char* name, u32 len);
int declare_local_var(CompileUnitPubStruct* cu, const char* name, u32 len);
int declare_variable(CompileUnitPubStruct* cu, const char* name, u32 len);
int define_module_var(VM* vm, ObjModule* module, const char* name, u32 len, Value val);
int declare_module_var(VM* vm, ObjModule* module, const char* name, u32 len, Value val);
CompileUnitPubStruct* get_enclosing_classbk_unit(CompileUnitPubStruct* cu);
ClassBookKeep* get_enclosing_classbk(CompileUnitPubStruct* cu);
ObjFn* end_compile_unit(CompileUnitPubStruct* cu);
void emit_load_module_var(CompileUnitPubStruct* cu, const char* name);
u32 emit_instr_with_placeholder(CompileUnitPubStruct* cu, OpCode opcode);
void patch_placeholder(CompileUnitPubStruct* cu, u32 abs_index);
void define_variable(CompileUnitPubStruct* cu, u32 index);
Variable find_variable(CompileUnitPubStruct* cu, const char* name, u32 len);
void enter_loop_setting(CompileUnitPubStruct* cu, Loop* loop);
void leave_loop_patch(CompileUnitPubStruct* cu);
u32 discard_local_var(CompileUnitPubStruct* cu, int scope_depth);
void enter_scope(CompileUnitPubStruct* cu);
void leave_scope(CompileUnitPubStruct* cu);
void emit_store_module_var(CompileUnitPubStruct* cu, int index);
int declare_method(CompileUnitPubStruct* cu, char* sign_str, u32 len);
void define_method(CompileUnitPubStruct* cu, Variable class, bool is_static, int method_index);
void emit_create_instance(CompileUnitPubStruct* cu, Signature* sign, u32 constructor_index);

#endif