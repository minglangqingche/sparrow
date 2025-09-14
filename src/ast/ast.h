#ifndef __AST_H__
#define __AST_H__

#include "common.h"
#include "compiler.h"

typedef struct {
    const char* start;
    u32 len;
} ScriptID;
#define SCRIPT_ID_FROM_TOKEN(token) ((ScriptID) {.start = token.start, .len = token.len})
#define SCRIPT_ID_NULL()            ((ScriptID) {.start = "", .len = 0})

typedef struct _AST_Expr AST_Expr;
typedef struct _AST_Stmt AST_Stmt;

typedef struct _AST_Block {
    struct AST_BlockContext {
        AST_Stmt* stmt;
        struct AST_BlockContext* next;
    }* head;
    struct AST_BlockContext* tail;
} AST_Block;

typedef struct {
    struct AST_ArrayItem {
        AST_Expr* item;
        struct AST_ArrayItem* next;
    }* head;
    struct AST_ArrayItem* tail;
} AST_ArrayLiteral;

typedef struct {
    struct AST_MapEntry {
        AST_Expr* key;
        AST_Expr* val;
        struct AST_MapEntry* next;
    }* entrys;
} AST_MapLiteral;

typedef struct {
    ScriptID id;
    AST_Expr* expr;
} AST_AssignExpr;

typedef struct {
    const char* op;
    AST_Expr* l;
    AST_Expr* r;
} AST_InfixExpr;

typedef struct {
    const char* op;
    AST_Expr* expr;
} AST_PrefixExpr;

typedef struct {
    AST_Expr* obj;
    ScriptID method_name;
    u32 argc;
    AST_Expr* args[MAX_ARG_NUM];
} AST_CallMethodExpr;

typedef struct {
    AST_Expr* obj;
    ScriptID method_name;
} AST_GetterExpr;

typedef struct {
    AST_Expr* obj;
    ScriptID method_name;
    AST_Expr* val;
} AST_SetterExpr;

typedef struct {
    AST_Expr* obj;
    u32 argc;
    AST_Expr* args[MAX_ARG_NUM];
} AST_SubscriptExpr;

typedef struct {
    AST_Expr* condition;
    AST_Expr* true_val;
    AST_Expr* false_val;
} AST_ConditionExpr;

typedef struct {
    u32 argc;
    ScriptID arg_names[MAX_ARG_NUM];
    AST_Block* body;
} AST_ClosureExpr;

typedef struct {
    ScriptID method_name; // 为 NULL 时调用同名方法
    
    enum SuperType {
        SUPER_METHOD,
        SUPER_GETTER,
        SUPER_SETTER,
        SUPER_SUBSCRIPT,
        SUPER_SUBSCRIPT_SETTER,
    } type;

    union {
        struct {
            u32 argc;
            AST_Expr* args[MAX_ARG_NUM];
        } method;
        
        AST_Expr* setter_value;
        
        struct {
            u32 argc;
            AST_Expr* args[MAX_ARG_NUM];
        } subscript;
    } call_method;
} AST_SuperCallExpr;

typedef struct {
    AST_Expr* l;
    AST_Expr* r;
} AST_LogicalCmpExpr;

typedef struct {
    ScriptID id;
    u32 argc;
    AST_Expr* args[MAX_ARG_NUM];
} AST_IdCallExpr;

struct _AST_Expr {
    enum AST_ExprType {
        AST_LITERAL_TRUE,
        AST_LITERAL_FALSE,
        AST_LITERAL_NULL,
        AST_LITERAL_EXPR,
        AST_ARRAY_LITERAL,
        AST_MAP_LITERAL,
        AST_ID_EXPR,
        AST_ASSIGN_EXPR,
        AST_INFIX_EXPR,
        AST_PREFIX_EXPR,

        AST_LOGICAL_OR, AST_LOGICAL_AND,

        AST_CALL_METHOD_EXPR,
        AST_GETTER_EXPR,
        AST_SETTER_EXPR,
        AST_SUBSCRIPT_EXPR,
        AST_SUBSCRIPT_SETTER_EXPR,

        AST_SELF_EXPR,
        AST_SUPER_EXPR,
        AST_CONDITION_EXPR,
        AST_CLOSURE_EXPR,

        AST_ID_CALL_EXPR, // id(...)这种形式的调用
    } type;

    union {
        Value literal;
        AST_ArrayLiteral array_literal;
        AST_MapLiteral map_literal;
        ScriptID id;
        AST_AssignExpr assign;
        AST_InfixExpr infix;
        AST_PrefixExpr prefix;
        AST_CallMethodExpr call_method;
        AST_GetterExpr getter;
        AST_SetterExpr setter;
        AST_SubscriptExpr subscript;
        AST_ConditionExpr condition_expr;
        AST_SuperCallExpr super_call;
        AST_ClosureExpr closure;
        AST_LogicalCmpExpr logical_cmp;
        AST_IdCallExpr id_call;
    } expr;
};

typedef struct _AST_IfStmt {
    AST_Expr* condition;
    AST_Block* then_block;
    enum {
        ELSE_NONE,  // 没有else块
        ELSE_IF,    // else-if
        ELSE_BLOCK, // else
    } else_type;
    union {
        struct _AST_IfStmt* else_if;
        AST_Block* block;
    } else_branch;
} AST_IfStmt;

typedef struct {
    AST_Expr* condition;
    AST_Block* body;
} AST_WhileStmt;

typedef struct _AST_VarDef {
    ScriptID name; // var name
    AST_Expr* init_val; // 初始化值，无则为null
} AST_VarDef;

struct _AST_Stmt {
    enum AST_StmtType {
        AST_IF_STMT,
        AST_WHILE_STMT,
        AST_BREAK_STMT,
        AST_RETURN_STMT,
        AST_CONTINUE_STMT,
        AST_BLOCK,
        AST_EXPRESSION_STMT,
        AST_VAR_DEF_STMT,
    } type;

    union {
        AST_IfStmt if_stmt;
        AST_WhileStmt while_stmt;
        AST_Expr* ret_stmt_res;
        AST_Block block;
        AST_Expr* expr_stmt;
        AST_VarDef var_def;
    } stmt;
};

typedef struct _AST_ImportStmt {
    enum ImportRootType {
        DEFAULT_ROOT,
        STD_ROOT,
        LIB_ROOT,
    } path_root;

    ObjString* path; // 除了根以外的多级路径，. 转化为 /
    u32 varc; // 需要导入的变量名数量
    ObjString** vars; // 需要导入的变量名，无则为null。

    struct _AST_ImportStmt* next;
} AST_ImportStmt;

typedef struct _AST_FuncDef {
    ScriptID name; // 函数名称

    u32 argc; // 参数个数
    ScriptID arg_names[MAX_ARG_NUM]; // 参数名

    AST_Block* body; // 函数体

    struct _AST_FuncDef* next;
} AST_FuncDef;

typedef struct _AST_ClassDef {
    ObjString* name; // class name
    AST_Expr* super; // super_class; 为 null 时默认 Object

    struct _ClassFields {
        bool is_static;

        ScriptID name;
        AST_Expr* init_val;

        struct _ClassFields* next;
    }* fields;

    struct _ClassMethod {
        bool is_static;

        enum AST_ClassMethodType {
            AST_CLASS_CONSTRUCTOR,
            AST_CLASS_METHOD,
            AST_CLASS_GETTER,
            AST_CLASS_SETTER,
            AST_CLASS_SUBSCRIPT,
            AST_CLASS_SUBSCRIPT_SETTER,
        } type;

        ScriptID name; // 有些类型不需要名字则为null

        u32 argc;
        ScriptID arg_names[MAX_ARG_NUM];

        AST_Block* body;

        struct _ClassMethod* next;
    }* methods;

    struct _AST_ClassDef* next;
} AST_ClassDef;

typedef struct {
    AST_ImportStmt* import_stmt_head;
    AST_ImportStmt* import_stmt_tail;

    AST_FuncDef* func_def_head;
    AST_FuncDef* func_def_tail;

    AST_ClassDef* class_def_head;
    AST_ClassDef* class_def_tail;

    struct AST_ToplevelStmt {
        AST_Stmt* stmt;
        struct AST_ToplevelStmt* next;
    }* toplevel_head;
    struct AST_ToplevelStmt* toplevel_tail;
} AST_Prog;

AST_Prog* compile_prog(Parser* parser);
void destroy_ast_prog(VM* vm, AST_Prog* prog);

#endif