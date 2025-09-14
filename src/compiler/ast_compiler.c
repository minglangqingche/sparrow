#include "ast_compiler.h"
#include "ast.h"
#include "class.h"
#include "common.h"
#include "compiler.h"
#include "core.h"
#include "opcode.h"
#include "utils.h"
#include "vm.h"
#include <string.h>

#include "ast_printer.h"

// 保存需要释放的local-var的name
// 使用的位置：class 中静态变量名创建后需添加进入该列表追踪
// 释放的时机：由于 class 不新建 cu，因此在 prog 解析结束后释放列表中的元素
static char* local_var_names[MAX_LOCAL_VAR_NUM] = {0};
static u32 local_var_names_count = 0;

static inline void generate_para_list(CompileUnitPubStruct* cu, u32 argc, ScriptID arg_names[MAX_FIELD_NUM]) {
    for (int i = 0; i < argc; i++) {
        declare_variable(cu, arg_names[i].start, arg_names[i].len);
    }
}

void generate_ast_expr(CompileUnitPubStruct* cu, AST_Expr* expr);
void generate_ast_block(CompileUnitPubStruct* cu, AST_Block* block);

void generate_ast_array_literal(CompileUnitPubStruct* cu, AST_ArrayLiteral* arr) {
    // 新建 List 对象
    emit_load_module_var(cu, "List");
    emit_call(cu, 0, "new()", 5);
    
    // 填入 item
    struct AST_ArrayItem* item = arr->head;
    while (item != NULL) {
        generate_ast_expr(cu, item->item);
        emit_call(cu, 1, "core_append(_)", 14);
        item = item->next;
    }
}

void generate_ast_map_literal(CompileUnitPubStruct* cu, AST_MapLiteral* map) {
    // 新建 Map 对象
    emit_load_module_var(cu, "Map");
    emit_call(cu, 0, "new()", 5);

    // 填入 entres
    struct AST_MapEntry* entry = map->entrys;
    while (entry != NULL) {
        generate_ast_expr(cu, entry->key);
        generate_ast_expr(cu, entry->val);
        emit_call(cu, 2, "core_insert(_,_)", 16);
        entry = entry->next;
    }
}

void generate_logical_cmp(CompileUnitPubStruct* cu, AST_LogicalCmpExpr* cmp, OpCode op) {
    generate_ast_expr(cu, cmp->l);
    u32 placeholder = emit_instr_with_placeholder(cu, op);
    generate_ast_expr(cu, cmp->r);
    patch_placeholder(cu, placeholder);
}

void generate_ast_subscript_expr(CompileUnitPubStruct* cu, AST_SubscriptExpr* subscript, SignatureType type) {
    Signature sign = {
        .type = type,
        .name = "",
        .len = 0,
        .argc = subscript->argc,
    };
    
    generate_ast_expr(cu, subscript->obj);
    for (int i = 0; i < subscript->argc; i++) {
        generate_ast_expr(cu, subscript->args[i]);
    }

    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

Variable process_id_in_class(CompileUnitPubStruct* cu, ClassBookKeep* clsbk, const char* id, u32 len) {
    // 尝试作为实例域解析
    int field_index = get_index_from_symbol_table(&clsbk->fields, id, len);
    if (field_index != -1) {
        // FIXME: 我怀疑这种情况不存在，因此报错
        if (cu->enclosing_unit == NULL) {
            printf("fot id '%.*s', try to get field but enclosing_unit == NULL.", len, id);
            UNREACHABLE();
        }

        return (Variable) {.scope_type = VAR_SCOPE_METHOD_FIELD, .index = field_index};
    }

    // 尝试解析静态域
    char* cls_name = clsbk->name->val.start;
    u32 cls_name_len = clsbk->name->val.len;
    
    char static_field_id[MAX_ID_LEN] = {0};
    memcpy(static_field_id, "Cls", 3);
    memcpy(static_field_id + 3, cls_name, cls_name_len);
    static_field_id[3 + cls_name_len] = '@';
    memcpy(static_field_id + 4 + cls_name_len, id, len);

    u32 static_field_id_len = 4 + cls_name_len + len;

    Variable var = get_var_from_local_or_upvalue(cu, static_field_id, static_field_id_len);

    if (var.index != -1) {
        return var;
    }

    // 并非以上处理的情况
    return (Variable) {.scope_type = VAR_SCOPE_INVALID, .index = -1};
}

Variable process_id(CompileUnitPubStruct* cu, const char* start, u32 len, bool is_id_call) {
    // 尝试作为局部变量或upvalue解析
    Variable var = get_var_from_local_or_upvalue(cu, start, len);
    if (var.index != -1) {
        return var;
    }

    // 类中的id解析
    ClassBookKeep* clsbk = get_enclosing_classbk(cu);
    if (clsbk != NULL) {
        var = process_id_in_class(cu, clsbk, start, len);
        if (var.index != -1) {
            return var;
        }
    }

    // 作为模块变量解析
    var.scope_type = VAR_SCOPE_MODULE;
    var.index = get_index_from_symbol_table(&cu->cur_module->module_var_name, start, len);

    if (var.index == -1) {
        char fn_name[MAX_ID_LEN] = {0};
        memcpy(fn_name, "Fn@", 3);
        memcpy(fn_name + 3, start, len);

        u32 fn_name_len = len + 3;

        var.index = get_index_from_symbol_table(
            &cu->cur_module->module_var_name,
            fn_name, fn_name_len
        );

        if (var.index != -1) {
            return var;
        }
        
        // 不是已定义的函数
        if (clsbk != NULL && ('a' <= *start && *start <= 'z')) {
            // 在类中，前文未定义的符号若以小写字符开头则会被解析为类方法调用，返回 -1 提示调用者处理。
            // 若要调用类中前文为定义的大写字符开头的类方法，则须self.Method调用
            // getter 的调用由 id 处理，
            // setter 的调用由 assign 处理，
            // method 的调用由 id_call 处理，
            // subscript 和 subscript-setter 不能通过该种方式调用。
            return (Variable) {.index = -1};
        }

        // 不是已定义的模块变量，可能在后文中有定义，预先填入行号作为标记
        if (var.index == -1) {
            if (is_id_call) {
                char fn_name[MAX_ID_LEN] = {0};
                memcpy(fn_name, "Fn@", 3);
                memcpy(fn_name + 3, start, len);

                u32 fn_name_len = len + 3;

                var.index = declare_module_var(
                    cu->vm, cu->cur_module,
                    fn_name, fn_name_len, I32_TO_VALUE(-99) // TODO: 此处没有正确处理行号，以 -99 做占位
                );
            } else {
                var.index = declare_module_var(
                    cu->vm, cu->cur_module,
                    start, len, I32_TO_VALUE(-99) // TODO: 此处没有正确处理行号，以 -99 做占位
                );
            }
        }
    }

    return var;
}

void generate_ast_id_call_expr(CompileUnitPubStruct* cu, AST_IdCallExpr* call) {
    Variable var = process_id(cu, call->id.start, call->id.len, true);

    if (var.index != -1) {
        // 调用栈准备
        emit_load_variable(cu, var);
        for (int i = 0; i < call->argc; i++) {
            generate_ast_expr(cu, call->args[i]);
        }

        // fn.call
        Signature call_sign = {
            .type = SIGN_METHOD,
            .name = "call",
            .len = 4,
            .argc = call->argc,
        };
        emit_call_by_signature(cu, &call_sign, OPCODE_CALL0);
        return;
    }

    // 不是全局调用（包括callable变量的调用和函数的调用），且在类内，解析为类函数调用
    ClassBookKeep* clsbk = get_enclosing_classbk(cu);
    ASSERT(clsbk != NULL, "clsbk == NULL");
    // 调用栈准备
    emit_load_self(cu);
    for (int i = 0; i < call->argc; i++) {
        generate_ast_expr(cu, call->args[i]);
    }
    Signature sign = {
        .type = SIGN_METHOD,
        .name = call->id.start,
        .len = call->id.len,
        .argc = call->argc,
    };
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

void generate_ast_assign_expr(CompileUnitPubStruct* cu, AST_AssignExpr* assign) {
    Variable var = process_id(cu, assign->id.start, assign->id.len, false);
    if (var.index == -1) {
        // 处理类中前文未定义符号的 setter 方法调用。
        emit_load_self(cu);
        generate_ast_expr(cu, assign->expr); // 赋值右值

        Signature sign = {
            .type = SIGN_SETTER,
            .name = assign->id.start,
            .len = assign->id.len,
            .argc = 1,
        };
        emit_call_by_signature(cu, &sign, OPCODE_CALL0);
    } else {
        generate_ast_expr(cu, assign->expr); // 赋值右值
        emit_store_variable(cu, var);
    }
}

void generate_ast_id_expr(CompileUnitPubStruct* cu, ScriptID id) {
    Variable var = process_id(cu, id.start, id.len, false);
    if (var.index == -1) {
        // 处理类中前文未定义符号的 getter 方法调用。
        emit_load_self(cu);
        Signature sign = {
            .type = SIGN_GETTER,
            .name = id.start,
            .len = id.len,
            .argc = 0,
        };
        emit_call_by_signature(cu, &sign, OPCODE_CALL0);
    } else {
        emit_load_variable(cu, var);
    }
}

void generate_ast_call_method_expr(CompileUnitPubStruct* cu, AST_CallMethodExpr* call) {
    // 调用栈准备
    generate_ast_expr(cu, call->obj);
    for (int i = 0; i < call->argc; i++) {
        generate_ast_expr(cu, call->args[i]);
    }
    
    Signature sign = {
        .type = SIGN_METHOD,
        .name = call->method_name.start,
        .len = call->method_name.len,
        .argc = call->argc,
    };
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

void generate_ast_getter_expr(CompileUnitPubStruct* cu, AST_GetterExpr* call) {
    Signature sign = {
        .type = SIGN_GETTER,
        .name = call->method_name.start,
        .len = call->method_name.len,
        .argc = 0,
    };
    generate_ast_expr(cu, call->obj);
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

void generate_ast_setter_expr(CompileUnitPubStruct* cu, AST_SetterExpr* call) {
    generate_ast_expr(cu, call->obj);
    generate_ast_expr(cu, call->val);
    Signature sign = {
        .type = SIGN_SETTER,
        .name = call->method_name.start,
        .len = call->method_name.len,
        .argc = 1,
    };
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

void generate_ast_self_expr(CompileUnitPubStruct* cu) {
    if (get_enclosing_classbk(cu) == NULL) {
        COMPILE_ERROR(cu->vm->cur_parser, "'self' must be inside a class method.");
    }
    emit_load_self(cu);
}

void generate_ast_infix_expr(CompileUnitPubStruct* cu, AST_InfixExpr* infix) {
    // 调用栈准备
    generate_ast_expr(cu, infix->l);
    generate_ast_expr(cu, infix->r);
    Signature sign = {
        .type = SIGN_METHOD,
        .name = infix->op,
        .len = strlen(infix->op),
        .argc = 1,
    };
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

void generate_ast_prefix_expr(CompileUnitPubStruct* cu, AST_PrefixExpr* prefix) {
    // 调用栈准备
    generate_ast_expr(cu, prefix->expr);
    Signature sign = {
        .type = SIGN_GETTER,
        .name = prefix->op,
        .len = strlen(prefix->op),
        .argc = 0,
    };
    emit_call_by_signature(cu, &sign, OPCODE_CALL0);
}

void generate_ast_condition_expr(CompileUnitPubStruct* cu, AST_ConditionExpr* expr) {
    generate_ast_expr(cu, expr->condition);
    int false_branch = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE);
    
    generate_ast_expr(cu, expr->true_val);
    int expr_end = emit_instr_with_placeholder(cu, OPCODE_JMP);

    patch_placeholder(cu, false_branch);
    generate_ast_expr(cu, expr->false_val);
    patch_placeholder(cu, expr_end);
}

void generate_ast_super_expr(CompileUnitPubStruct* cu, AST_SuperCallExpr* super) {
    ClassBookKeep* clsbk = get_enclosing_classbk(cu);
    if (clsbk == NULL) {
        COMPILE_ERROR(cu->vm->cur_parser, "can't invoke super outside a class method.");
    }
    
    Signature sign;
    if (super->method_name.start != NULL) {
        sign.name = super->method_name.start;
        sign.len = super->method_name.len;
    } else {
        sign.name = clsbk->signature->name;
        sign.len = clsbk->signature->len;
    }

    emit_load_self(cu); // args[0] = self;

    switch (super->type) {
        case SUPER_METHOD:
            sign.type = SIGN_METHOD;
            sign.argc = super->call_method.method.argc;
            for (int i = 0; i < super->call_method.method.argc; i++) {
                generate_ast_expr(cu, super->call_method.method.args[i]);
            }
            break;
            
        case SUPER_GETTER:
            sign.type = SIGN_GETTER;
            sign.argc = 0;
            break;

        case SUPER_SETTER:
            sign.type = SIGN_SETTER;
            sign.argc = 1;
            generate_ast_expr(cu, super->call_method.setter_value);
            break;

        case SUPER_SUBSCRIPT:
            sign.type = SIGN_SUBSCRIPT;
            sign.argc = super->call_method.subscript.argc;
            for (int i = 0; i < super->call_method.subscript.argc; i++) {
                generate_ast_expr(cu, super->call_method.subscript.args[i]);
            }
            break;

        case SUPER_SUBSCRIPT_SETTER:
            sign.type = SIGN_SUBSCRIPT_SETTER;
            sign.argc = super->call_method.subscript.argc;
            for (int i = 0; i < super->call_method.subscript.argc; i++) {
                generate_ast_expr(cu, super->call_method.subscript.args[i]);
            }
            break;
    }
    
    emit_call_by_signature(cu, &sign, OPCODE_SUPER0);
}

void generate_ast_closure_expr(CompileUnitPubStruct* cu, AST_ClosureExpr* closure) {
    CompileUnitPubStruct fncu;
    compile_unit_pubstruct_init(cu->vm, cu->cur_module, &fncu, cu, false);

    fncu.fn->argc = closure->argc;
    generate_para_list(&fncu, closure->argc, closure->arg_names);

    generate_ast_block(&fncu, closure->body);

    end_compile_unit(&fncu);
}

void generate_ast_expr(CompileUnitPubStruct* cu, AST_Expr* expr) {
    switch (expr->type) {
        case AST_LITERAL_TRUE:
            write_opcode(cu, OPCODE_PUSH_TRUE);
            break;
        case AST_LITERAL_FALSE:
            write_opcode(cu, OPCODE_PUSH_FALSE);
            break;
        case AST_LITERAL_NULL:
            write_opcode(cu, OPCODE_PUSH_NULL);
            break;

        case AST_LITERAL_EXPR:
            emit_load_constant(cu, expr->expr.literal);
            break;
        case AST_ARRAY_LITERAL:
            generate_ast_array_literal(cu, &expr->expr.array_literal);
            break;
        case AST_MAP_LITERAL:
            generate_ast_map_literal(cu, &expr->expr.map_literal);
            break;

        case AST_ID_EXPR:
            generate_ast_id_expr(cu, expr->expr.id);
            break;
        case AST_ASSIGN_EXPR:
            generate_ast_assign_expr(cu, &expr->expr.assign);
            break;
        case AST_ID_CALL_EXPR:
            generate_ast_id_call_expr(cu, &expr->expr.id_call);
            break;

        case AST_INFIX_EXPR:
            generate_ast_infix_expr(cu, &expr->expr.infix);
            break;
        case AST_PREFIX_EXPR:
            generate_ast_prefix_expr(cu, &expr->expr.prefix);
            break;

        case AST_LOGICAL_OR:
            generate_logical_cmp(cu, &expr->expr.logical_cmp, OPCODE_OR);
            break;
        case AST_LOGICAL_AND:
            generate_logical_cmp(cu, &expr->expr.logical_cmp, OPCODE_AND);
            break;

        case AST_CALL_METHOD_EXPR:
            generate_ast_call_method_expr(cu, &expr->expr.call_method);
            break;
        case AST_GETTER_EXPR:
            generate_ast_getter_expr(cu, &expr->expr.getter);
            break;
        case AST_SETTER_EXPR:
            generate_ast_setter_expr(cu, &expr->expr.setter);
            break;
        case AST_SUBSCRIPT_EXPR:
            generate_ast_subscript_expr(cu, &expr->expr.subscript, SIGN_SUBSCRIPT);
            break;
        case AST_SUBSCRIPT_SETTER_EXPR:
            generate_ast_subscript_expr(cu, &expr->expr.subscript, SIGN_SUBSCRIPT_SETTER);
            break;

        case AST_SELF_EXPR:
            generate_ast_self_expr(cu);
            break;
        case AST_SUPER_EXPR:
            generate_ast_super_expr(cu, &expr->expr.super_call);
            break;
        case AST_CONDITION_EXPR:
            generate_ast_condition_expr(cu, &expr->expr.condition_expr);
            break;
        case AST_CLOSURE_EXPR:
            generate_ast_closure_expr(cu, &expr->expr.closure);
            break;
    }
}

void generate_ast_stmt(CompileUnitPubStruct* cu, AST_Stmt* stmt);

void generate_ast_block(CompileUnitPubStruct* cu, AST_Block* block) {
    enter_scope(cu);
    struct AST_BlockContext* context = block->head;
    while (context != NULL) {
        generate_ast_stmt(cu, context->stmt);
        context = context->next;
    }
    leave_scope(cu);
}

void generate_ast_if_stmt(CompileUnitPubStruct* cu, AST_IfStmt* if_stmt) {
    generate_ast_expr(cu, if_stmt->condition); // 条件
    u32 else_block_start = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE); // 跳转指令

    generate_ast_block(cu, if_stmt->then_block); // then block

    if (if_stmt->else_type == ELSE_NONE) {
        patch_placeholder(cu, else_block_start);
        return;
    }

    u32 stmt_end = emit_instr_with_placeholder(cu, OPCODE_JMP); // then block 跳转
    patch_placeholder(cu, else_block_start);
    if (if_stmt->else_type == ELSE_IF) {
        generate_ast_if_stmt(cu, if_stmt->else_branch.else_if);
    } else {
        generate_ast_block(cu, if_stmt->else_branch.block);
    }
    patch_placeholder(cu, stmt_end);
}

void generate_ast_while_stmt(CompileUnitPubStruct* cu, AST_WhileStmt* stmt) {
    if (stmt->condition->type == AST_LITERAL_FALSE || stmt->condition->type == AST_LITERAL_NULL) {
        // 条件始终为false，不再编译，直接返回
        return;
    }
    
    Loop loop;
    enter_loop_setting(cu, &loop);

    if (stmt->condition->type != AST_LITERAL_TRUE && stmt->condition->type != AST_LITERAL_EXPR) {
        // 条件为 true，则不再继续生成条件及跳转指令
        generate_ast_expr(cu, stmt->condition); // 条件
        // 跳转
        loop.exit_index = emit_instr_with_placeholder(cu, OPCODE_JMP_IF_FALSE);
    } else {
        loop.exit_index = -1; // 不再回填jmp_if_false
    }

    cu->cur_loop->body_start_index = cu->fn->instr_stream.count;
    generate_ast_block(cu, stmt->body); // 循环体

    leave_loop_patch(cu);
}

void generate_ast_break_stmt(CompileUnitPubStruct* cu) {
    if (cu->cur_loop == NULL) {
        COMPILE_ERROR(cu->vm->cur_parser, "break statment should be used inside a loop");
    }
    
    // 清除循环体内的局部变量
    discard_local_var(cu, cu->cur_loop->scope_depth + 1);

    // 填充end表示break占位
    emit_instr_with_placeholder(cu, OPCODE_END);
}

void generate_ast_continue_stmt(CompileUnitPubStruct* cu) {
    if (cu->cur_loop == NULL) {
        COMPILE_ERROR(cu->vm->cur_parser, "continue should be used inside a loop.");
    }

    discard_local_var(cu, cu->cur_loop->scope_depth + 1);
    int loop_back_offset = cu->fn->instr_stream.count - cu->cur_loop->cond_start_index + 2;
    write_opcode_short_operand(cu, OPCODE_LOOP, loop_back_offset);
}

void generate_ast_return_stmt(CompileUnitPubStruct* cu, AST_Expr* result) {
    if (result != NULL) {
        generate_ast_expr(cu, result);
    } else {
        write_opcode(cu, OPCODE_PUSH_NULL);
    }

    write_opcode(cu, OPCODE_RETURN);
}

void generate_ast_var_def_stmt(CompileUnitPubStruct* cu, AST_VarDef* def) {
    if (def->init_val == NULL) {
        write_opcode(cu, OPCODE_PUSH_NULL);
    } else {
        generate_ast_expr(cu, def->init_val);
    }
    
    u32 index = declare_variable(cu, def->name.start, def->name.len);
    define_variable(cu, index);
}

void generate_ast_stmt(CompileUnitPubStruct* cu, AST_Stmt* stmt) {
    switch (stmt->type) {
        case AST_IF_STMT:
            generate_ast_if_stmt(cu, &stmt->stmt.if_stmt);
            break;
        case AST_WHILE_STMT:
            generate_ast_while_stmt(cu, &stmt->stmt.while_stmt);
            break;
        case AST_BREAK_STMT:
            generate_ast_break_stmt(cu);
            break;
        case AST_RETURN_STMT:
            generate_ast_return_stmt(cu, stmt->stmt.ret_stmt_res);
            break;
        case AST_CONTINUE_STMT:
            generate_ast_continue_stmt(cu);
            break;
        case AST_BLOCK:
            generate_ast_block(cu, &stmt->stmt.block);
            break;
        case AST_EXPRESSION_STMT:
            generate_ast_expr(cu, stmt->stmt.expr_stmt);
            write_opcode(cu, OPCODE_POP);
            break;
        case AST_VAR_DEF_STMT:
            generate_ast_var_def_stmt(cu, &stmt->stmt.var_def);
            break;
    }
}

void generate_ast_import_stmt(CompileUnitPubStruct* cu, AST_ImportStmt* import) {
    u32 const_name_index = add_constant(cu, OBJ_TO_VALUE(import->path));

    emit_load_module_var(cu, "System");
    write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_name_index);
    switch (import->path_root) {
        case DEFAULT_ROOT:
            emit_call(cu, 1, "import_module(_)", 16);
            break;
        
        case STD_ROOT:
            emit_call(cu, 1, "import_std_module(_)", 20);
            break;
        
        case LIB_ROOT:
            emit_call(cu, 1, "import_lib_module(_)", 20);
            break;
    }
    write_opcode(cu, OPCODE_POP);

    for (int i = 0; i < import->varc; i++) {
        u32 var_index = declare_variable(cu, import->vars[i]->val.start, import->vars[i]->val.len);
        u32 const_var_name = add_constant(cu, OBJ_TO_VALUE(import->vars[i]));

        // $top = System.get_module_variable("foo", "bar");
        emit_load_module_var(cu, "System");
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_name_index);
        write_opcode_short_operand(cu, OPCODE_LOAD_CONSTANT, const_var_name);
        emit_call(cu, 2, "get_module_variable(_,_)", 24);

        // bar = $top
        define_variable(cu, var_index);
    }
}

void generate_ast_func_def(CompileUnitPubStruct* cu, AST_FuncDef* func) {
    if (cu->enclosing_unit != NULL) {
        COMPILE_ERROR(cu->vm->cur_parser, "'fn' should be in module scope.");
    }

    char fn_name[MAX_SIGN_LEN + 4] = {0};
    memmove(fn_name, "Fn@", 3);
    memmove(fn_name + 3, func->name.start, func->name.len);
    u32 fn_name_len = strlen(fn_name);

    u32 fn_name_index = declare_variable(cu, fn_name, fn_name_len); // 声明函数，便于函数递归

    CompileUnitPubStruct fncu;
    compile_unit_pubstruct_init(cu->vm, cu->cur_module, &fncu, cu, false);

    generate_para_list(&fncu, func->argc, func->arg_names);
    fncu.fn->argc = func->argc;

    generate_ast_block(&fncu, func->body);

    end_compile_unit(&fncu); // 生成了一个closure在栈顶
    define_variable(cu, fn_name_index); // 定义函数为栈顶closure
}

void generate_ast_class_def(CompileUnitPubStruct* cu, AST_ClassDef* class_def) {
    if (cu->scope_depth != -1) {
        COMPILE_ERROR(cu->vm->cur_parser, "class definition must be in the module scope.");
    }

    Variable class = {
        .scope_type = VAR_SCOPE_MODULE,
        .index = declare_variable(cu, class_def->name->val.start, class_def->name->val.len),
    };

    emit_load_constant(cu, OBJ_TO_VALUE(class_def->name)); // 加载函数名
    // 加载父类
    if (class_def->super != NULL) {
        generate_ast_expr(cu, class_def->super);
    } else {
        emit_load_module_var(cu, "Object");
    }

    int field_num_index = write_opcode_byte_operand(cu, OPCODE_CREATE_CLASS, 0xFF); // 未知字段数，填入0xFF占位
    emit_store_module_var(cu, class.index); // 将栈顶创建好的class填入var

    ClassBookKeep clsbk = {
        .name = class_def->name,
        .in_static = false,
    };
    BufferInit(String, &clsbk.fields);
    BufferInit(Int, &clsbk.instant_methods);
    BufferInit(Int, &clsbk.static_methods);
    cu->enclosing_classbk = &clsbk;

    enter_scope(cu); // 步入class定义体

    // 解析fields
    struct _ClassFields* field = class_def->fields;
    while (field != NULL) {
        if (field->is_static) { // 静态字段
            // 构造静态字段名
            char* static_field_name = malloc(MAX_ID_LEN);
            local_var_names[local_var_names_count++] = static_field_name;
            
            u32 static_field_len = 3;
            memcpy(static_field_name, "Cls", 3);
            memcpy(static_field_name + 3, class_def->name->val.start, class_def->name->val.len);
            static_field_len += class_def->name->val.len;
            static_field_name[static_field_len++] = '@';
            memcpy(&static_field_name[static_field_len], field->name.start, field->name.len);
            static_field_len += field->name.len;
            static_field_name[static_field_len] = '\0';

            // 重定义检查
            if (find_local(cu, static_field_name, static_field_len) != -1) {
                COMPILE_ERROR(cu->vm->cur_parser, "static field '%s.%s' redefinition.", clsbk.name->val.start, static_field_name);
            }

            // 变量定义，静态字段作为局部变量保存
            // declare_local_var(cu, static_field_name, static_field_len);
            if (cu->local_vars_count >= MAX_LOCAL_VAR_NUM) {
                COMPILE_ERROR(
                    cu->vm->cur_parser,
                    "the max len of local variable of one scope is %d.", MAX_LOCAL_VAR_NUM
                );
            }
            add_local_var(cu, static_field_name, static_field_len);

            // 静态字段初始化
            if (field->init_val != NULL) {
                generate_ast_expr(cu, field->init_val);
            } else {
                write_opcode(cu, OPCODE_PUSH_NULL);
            }
        } else { // 实例字段
            ClassBookKeep* class_bk = get_enclosing_classbk(cu);
            int field_index = get_index_from_symbol_table(&class_bk->fields, field->name.start, field->name.len);
            if (field_index != -1) {
                if (field_index > MAX_FIELD_NUM) {
                    COMPILE_ERROR(cu->vm->cur_parser, "the max number of instance field is %d.", MAX_FIELD_NUM);
                }

                char id[MAX_ID_LEN] = {0};
                memcpy(id, field->name.start, field->name.len);
                COMPILE_ERROR(cu->vm->cur_parser, "instance field '%s.%s' redefinition.", clsbk.name->val.start, id);
            }

            field_index = add_symbol(cu->vm, &class_bk->fields, field->name.start, field->name.len);
        }
        field = field->next;
    }

    // 解析method
    struct _ClassMethod* method = class_def->methods;
    while (method != NULL) {        
        Signature sign = {
            .type = method->type == AST_CLASS_CONSTRUCTOR ? SIGN_CONSTRUCT
                    : method->type == AST_CLASS_METHOD ? SIGN_METHOD
                    : method->type == AST_CLASS_GETTER ? SIGN_GETTER
                    : method->type == AST_CLASS_SETTER ? SIGN_SETTER
                    : method->type == AST_CLASS_SUBSCRIPT ? SIGN_SUBSCRIPT
                    : SIGN_SUBSCRIPT_SETTER,
            .name = method->name.start,
            .len = method->name.len,
            .argc = method->argc,
        };

        // 方法的编译单元
        CompileUnitPubStruct method_cu;
        compile_unit_pubstruct_init(cu->vm, cu->cur_module, &method_cu, cu, true);

        // 声明型参
        generate_para_list(&method_cu, method->argc, method->arg_names);

        // 声明方法
        char sign_str[MAX_SIGN_LEN] = {0};
        u32 sign_len = sign2string(&sign, sign_str);
        u32 method_index = declare_method(cu, sign_str, sign_len);

        // 解析方法体
        generate_ast_block(&method_cu, method->body);
        end_compile_unit(&method_cu);

        // 定义方法
        define_method(cu, class, method->is_static, method_index);

        // 为构造函数生成静态构造函数
        if (sign.type == SIGN_CONSTRUCT) {
            sign.type = SIGN_METHOD;
            u32 constructor_index = ensure_symbol_exist(cu->vm, &cu->vm->all_method_names, sign_str, sign_len);
            emit_create_instance(cu, &sign, constructor_index);
            define_method(cu, class, true, constructor_index);
        }

        method = method->next;
    }

    cu->fn->instr_stream.datas[field_num_index] = (u8)clsbk.fields.count; // 回填字段数

    symbol_table_clear(cu->vm, &clsbk.fields);
    BufferClear(Int, &clsbk.instant_methods, cu->vm);
    BufferClear(Int, &clsbk.static_methods, cu->vm);

    cu->enclosing_classbk = NULL;
    leave_scope(cu);
}

void ast_compile_program(CompileUnitPubStruct* cu, Parser* parser) {
    AST_Prog* prog = compile_prog(parser);
    
#ifdef DUMP_AST_WHEN_COMPILE_PROG
    char buf[512] = {0};
    const char* root_path = getenv("SPR_HOME");
    u32 root_dir_len = 0;
    if (root_path != NULL) {
        root_dir_len = strlen(root_path);
    } else {
        root_dir_len = root_dir == NULL ? 0 : strlen(root_dir);
        root_path = root_dir;
    }

    if (root_dir_len == 0) {
        sprintf(buf, "ast.%s.txt", cu->cur_module->name != NULL ? cu->cur_module->name->val.start : "core.sp");
    } else {
        sprintf(buf, "%s/ast.%s.txt", root_path, cu->cur_module->name != NULL ? cu->cur_module->name->val.start : "core.sp");
    }

    FILE* ast_output = fopen(buf, "w");
    if (ast_output != NULL) {
        printf("ast dump to: %s\n", buf);
        print_ast_prog(ast_output, prog);
    } else {
        printf("ast dump error: %s\n", buf);
    }
    fclose(ast_output);
#endif

    AST_ImportStmt* import = prog->import_stmt_head;
    while (import != NULL) {
        generate_ast_import_stmt(cu, import);
        import = import->next;
    }

    AST_FuncDef* func = prog->func_def_head;
    while (func != NULL) {
        generate_ast_func_def(cu, func);
        func = func->next;
    }

    AST_ClassDef* class_def = prog->class_def_head;
    while (class_def != NULL) {
        generate_ast_class_def(cu, class_def);
        class_def = class_def->next;
    }

    struct AST_ToplevelStmt* stmt = prog->toplevel_head;
    while (stmt != NULL) {
        generate_ast_stmt(cu, stmt->stmt);
        stmt = stmt->next;
    }

    destroy_ast_prog(cu->vm, prog);

    for (int i = 0; i < local_var_names_count; i++) {
        free(local_var_names[i]);
    }
    local_var_names_count = 0;
}
