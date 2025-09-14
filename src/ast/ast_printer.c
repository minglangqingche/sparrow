#include "ast.h"
#include <stdio.h>
#include "ast_printer.h"
#include "obj_range.h"

static u8 indent = 0;

void print_block_inline(FILE* file, AST_Block* block);
void print_ast_block(FILE* file, AST_Block* block);

void fprint_value(FILE* file, Value* val) {
    switch (val->type) {
        case VT_I32:
            fprintf(file, "<i32 %d>", val->i32val);
            break;
        case VT_U32:
            fprintf(file, "<u32 %u>", val->u32val);
            break;
        case VT_U8:
            fprintf(file, "<u8 %u>", val->u8val);
            break;
        case VT_F64:
            fprintf(file, "<f64 %lf>", val->f64val);
            break;
        case VT_TRUE:
            fprintf(file, "<bool true>");
            break;
        case VT_FALSE:
            fprintf(file, "<bool false>");
            break;
        case VT_NULL:
            fprintf(file, "<null>");
            break;
        case VT_UNDEFINED:
            fprintf(file, "<?undefined?>");
            break;
        case VT_OBJ: {
            ObjHeader* obj = val->header;
            switch (obj->type) {
                case OT_STRING: {
                    fprintf(file, "<String '%s' at %p>", ((ObjString*)obj)->val.start, obj);
                    break;
                }

                case OT_CLASS: {
                    fprintf(file, "<Class %s at %p>", ((Class*)obj)->name->val.start, obj);
                    break;
                }

                case OT_INSTANCE: {
                    fprintf(file, "<Instance of %s at %p>", ((ObjInstance*)obj)->header.class->name->val.start, obj);
                    break;
                }

                case OT_RANGE: {
                    fprintf(file, "<Range %d %d %d>", ((ObjRange*)obj)->from, ((ObjRange*)obj)->to, ((ObjRange*)obj)->step);
                    break;
                }

                case OT_LIST: {
                    fprintf(file, "<List at %p>", obj);
                    break;
                }

                case OT_FUNCTION: {
                    fprintf(file, "<Function at %p>", obj);
                    break;
                }

                case OT_MAP: {
                    fprintf(file, "<Map at %p>", obj);
                    break;
                }

                case OT_MODULE: {
                    fprintf(file, "<Module at %p>", obj);
                    break;
                }

                case OT_THREAD: {
                    fprintf(file, "<Thread at %p>", obj);
                    break;
                }

                case OT_UPVALUE: {
                    fprintf(file, "<Upvalue at %p>", obj);
                    break;
                }

                case OT_CLOSURE: {
                    fprintf(file, "<Closure at %p>", obj);
                    break;
                }

                default: {
                    fprintf(file, "<Obj? at %p>", obj);
                    break;
                }
            }
        }
    }
}

void print_ast_expr(FILE* file, AST_Expr* expr) {
    switch (expr->type) {
        case AST_LITERAL_TRUE:
            fprintf(file, "(true)");
            break;
        case AST_LITERAL_FALSE:
            fprintf(file, "(false)");
            break;
        case AST_LITERAL_NULL:
            fprintf(file, "(null)");
            break;

        case AST_LITERAL_EXPR:
            fprintf(file, "(literal ");
            fprint_value(file, &expr->expr.literal);
            fprintf(file, ")");
            break;

        case AST_ARRAY_LITERAL:
            fprintf(file, "(list-literal [");
            AST_ArrayLiteral* arr = &expr->expr.array_literal;
            
            struct AST_ArrayItem* item = arr->head;
            while (item != NULL) {
                print_ast_expr(file, item->item);
                if (item->next != NULL) {
                    fprintf(file, ", ");
                }
                item = item->next;
            }
            
            fprintf(file, "])");
            break;

        case AST_MAP_LITERAL:
            fprintf(file, "(map-literal {");
            AST_MapLiteral* map = &expr->expr.map_literal;

            struct AST_MapEntry* entry = map->entrys;
            while (entry != NULL) {
                print_ast_expr(file, entry->key);
                fprintf(file, ": ");
                print_ast_expr(file, entry->val);
                entry = entry->next;
            }
            
            fprintf(file, "})");
            break;

        case AST_ID_EXPR:
            fprintf(file, "(id %.*s)", expr->expr.id.len, expr->expr.id.start);
            break;

        case AST_ASSIGN_EXPR:
            fprintf(file, "(= %.*s, ", expr->expr.assign.id.len, expr->expr.assign.id.start);
            print_ast_expr(file, expr->expr.assign.expr);
            fprintf(file, ")");
            break;

        case AST_INFIX_EXPR:
            fprintf(file, "(%s ", expr->expr.infix.op);
            print_ast_expr(file, expr->expr.infix.l);
            fprintf(file, ", ");
            print_ast_expr(file, expr->expr.infix.r);
            fprintf(file, ")");
            break;

        case AST_PREFIX_EXPR:
            fprintf(file, "(%s ", expr->expr.prefix.op);
            print_ast_expr(file, expr->expr.prefix.expr);
            fprintf(file, ")");
            break;

        case AST_LOGICAL_OR:
            fprintf(file, "(|| ");
            print_ast_expr(file, expr->expr.logical_cmp.l);
            fprintf(file, ", ");
            print_ast_expr(file, expr->expr.logical_cmp.r);
            fprintf(file, ")");
            break;

        case AST_LOGICAL_AND:
            fprintf(file, "(&& ");
            print_ast_expr(file, expr->expr.logical_cmp.l);
            fprintf(file, ", ");
            print_ast_expr(file, expr->expr.logical_cmp.r);
            fprintf(file, ")");
            break;

        case AST_CALL_METHOD_EXPR:
            fprintf(file, "(");
            print_ast_expr(file, expr->expr.call_method.obj);
            fprintf(file, ".%.*s ", expr->expr.call_method.method_name.len, expr->expr.call_method.method_name.start);
            if (expr->expr.call_method.argc == 0) {
                fprintf(file, "())");
                break;
            }
            for (int i = 0; i < expr->expr.call_method.argc; i++) {
                if (i > 0) {
                    fprintf(file, ", ");
                }
                print_ast_expr(file, expr->expr.call_method.args[i]);
            }
            fprintf(file, ")");
            break;

        case AST_GETTER_EXPR:
            fprintf(file, "(");
            print_ast_expr(file, expr->expr.getter.obj);
            fprintf(file, ".%.*s)", expr->expr.getter.method_name.len, expr->expr.getter.method_name.start);
            break;

        case AST_SETTER_EXPR:
            fprintf(file, "(");
            print_ast_expr(file, expr->expr.setter.obj);
            fprintf(file, ".%.*s = ", expr->expr.setter.method_name.len, expr->expr.setter.method_name.start);
            print_ast_expr(file, expr->expr.setter.val);
            fprintf(file, ")");
            break;

        case AST_SUBSCRIPT_EXPR:
            fprintf(file, "(");
            print_ast_expr(file, expr->expr.subscript.obj);
            fprintf(file, " [] ");
            for (int i = 0; i < expr->expr.subscript.argc; i++) {
                if (i > 0) {
                    fprintf(file, ", ");
                }
                print_ast_expr(file, expr->expr.subscript.args[i]);
            }
            fprintf(file, ")");
            break;

        case AST_SUBSCRIPT_SETTER_EXPR:
            fprintf(file, "(");
            print_ast_expr(file, expr->expr.subscript.obj);
            fprintf(file, " []= ");
            for (int i = 0; i < expr->expr.subscript.argc; i++) {
                if (i > 0) {
                    fprintf(file, ", ");
                }
                print_ast_expr(file, expr->expr.subscript.args[i]);
            }
            fprintf(file, ")");
            break;

        case AST_SELF_EXPR:
            fprintf(file, "(self)");
            break;

        case AST_SUPER_EXPR: {
            AST_SuperCallExpr* super = &expr->expr.super_call;
            fprintf(file, "(super");
            
            if (super->method_name.start != NULL) {
                fprintf(file, ".%.*s ", super->method_name.len, super->method_name.start);
            }

            switch (super->type) {
                case SUPER_METHOD:
                    for (int i = 0; i < super->call_method.method.argc; i++) {
                        if (i > 0) {
                            fprintf(file, ", ");
                        }
                        print_ast_expr(file, super->call_method.method.args[i]);
                    }
                    break;
                case SUPER_GETTER:
                    /* do noting */
                    break;
                case SUPER_SETTER:
                    fprintf(file, "= ");
                    print_ast_expr(file, super->call_method.setter_value);
                    break;
                case SUPER_SUBSCRIPT:
                    fprintf(file, "[] ");
                    for (int i = 0; i < super->call_method.subscript.argc; i++) {
                        if (i > 0) {
                            fprintf(file, ", ");
                        }
                        print_ast_expr(file, super->call_method.subscript.args[i]);
                    }
                    break;
                case SUPER_SUBSCRIPT_SETTER:
                    fprintf(file, "[]= ");
                    for (int i = 0; i < super->call_method.subscript.argc; i++) {
                        if (i > 0) {
                            fprintf(file, ", ");
                        }
                        print_ast_expr(file, super->call_method.subscript.args[i]);
                    }
                    break;
            }
            fprintf(file, ")");
            break;
        }
            

        case AST_CONDITION_EXPR:
            fprintf(file, "(condition ");
            print_ast_expr(file, expr->expr.condition_expr.condition);
            fprintf(file, " ? ");
            print_ast_expr(file, expr->expr.condition_expr.true_val);
            fprintf(file, " : ");
            print_ast_expr(file, expr->expr.condition_expr.false_val);
            fprintf(file, ")");
            break;

        case AST_CLOSURE_EXPR:
            fprintf(file, "(closure (");
            for (int i = 0; i < expr->expr.closure.argc; i++) {
                if (i > 0) {
                    fprintf(file, ", ");
                }
                fprintf(file, "%.*s", expr->expr.closure.arg_names[i].len, expr->expr.closure.arg_names[i].start);
            }
            fprintf(file, ") ");
            print_block_inline(file, expr->expr.closure.body);
            fprintf(file, "%*s)", indent, "");
            break;

        case AST_ID_CALL_EXPR:
            fprintf(file, "(%.*s ", expr->expr.id_call.id.len, expr->expr.id_call.id.start);
            if (expr->expr.id_call.argc == 0) {
                fprintf(file, "())");
                break;
            }
            for (int i = 0; i < expr->expr.id_call.argc; i++) {
                if (i > 0) {
                    fprintf(file, ", ");
                }
                print_ast_expr(file, expr->expr.id_call.args[i]);
            }
            fprintf(file, ")");
            break;

    }
}

void print_ast_if_stmt(FILE* file, AST_IfStmt* stmt) {
    fprintf(file, "if ");
    print_ast_expr(file, stmt->condition);
    fprintf(file, " ");
    print_block_inline(file, stmt->then_block);

    switch (stmt->else_type) {
        case ELSE_NONE:
            /* do noting */
            break;
        case ELSE_IF:
            fseek(file, -1, SEEK_CUR);    
            fprintf(file, " else ");
            print_ast_if_stmt(file, stmt->else_branch.else_if);
            break;
        case ELSE_BLOCK:
            fseek(file, -1, SEEK_CUR);
            fprintf(file, " else ");
            print_block_inline(file, stmt->else_branch.block);
            break;
    }
}

void print_ast_stmt(FILE* file, AST_Stmt* stmt) {
    fprintf(file, "%*s", indent, "");
    
    switch (stmt->type) {
        case AST_BREAK_STMT:
            fprintf(file, "{break}");
            break;
        case AST_CONTINUE_STMT:
            fprintf(file, "{continue}");
            break;
        case AST_RETURN_STMT:
            fprintf(file, "{return ");
            if (stmt->stmt.ret_stmt_res != NULL) {
                print_ast_expr(file, stmt->stmt.ret_stmt_res);
            } else {
                fprintf(file, "null");
            }
            fprintf(file, "}");
            break;
        case AST_EXPRESSION_STMT:
            fprintf(file, "{expr ");
            print_ast_expr(file, stmt->stmt.expr_stmt);
            fprintf(file, "}");
            break;
        case AST_VAR_DEF_STMT:
            fprintf(file, "{let %.*s", stmt->stmt.var_def.name.len, stmt->stmt.var_def.name.start);
            if (stmt->stmt.var_def.init_val != NULL) {
                fprintf(file, " = ");
                print_ast_expr(file, stmt->stmt.var_def.init_val);
            }
            fprintf(file, "}");
            break;
        case AST_IF_STMT:
            indent += 2;
            fprintf(file, "{ (if)\n%*s", indent, "");
            print_ast_if_stmt(file, &stmt->stmt.if_stmt);
            indent -= 2;
            fprintf(file, "%*s}", indent, "");
            break;
        case AST_BLOCK:
            print_block_inline(file, &stmt->stmt.block);
            fseek(file, -1, SEEK_CUR);
            break;
        case AST_WHILE_STMT:
            indent += 2;
            fprintf(file, "{ (while)\n%*swhile ", indent, "");
            
            AST_WhileStmt* w_stmt = &stmt->stmt.while_stmt;
            print_ast_expr(file, w_stmt->condition);
            fprintf(file, " ");

            print_block_inline(file, w_stmt->body);

            indent -= 2;
            fprintf(file, "%*s}", indent, "");
            break;
    }

    fprintf(file, "\n");
}

// 语句中的块用这个函数输出可以使 { 接在同一行
void print_block_inline(FILE* file, AST_Block* block) {
    fprintf(file, "{\n");
    indent += 2;

    struct AST_BlockContext* context = block->head;
    while (context != NULL) {
        print_ast_stmt(file, context->stmt);
        context = context->next;
    }

    indent -= 2;
    fprintf(file, "%*s}\n", indent, "");
}

void print_ast_block(FILE* file, AST_Block* block) {
    fprintf(file, "%*s", indent, "");
    print_block_inline(file, block);
}

void symple_print_ast_func_def(FILE* file, AST_FuncDef* def) {
    AST_FuncDef* tmp = def;
    while (tmp != NULL) {
        fprintf(file, "%*s{fn %.*s}\n", indent, "", tmp->name.len, tmp->name.start);
        tmp = tmp->next;
    }
}

void print_ast_func_def(FILE* file, AST_FuncDef* def) {
    AST_FuncDef* tmp = def;
    while (tmp != NULL) {
        fprintf(file, "%*s{\n", indent, "");
        indent += 2;

        fprintf(file, "%*sfn %.*s(", indent, "", def->name.len, def->name.start);
        for (int i = 0; i < def->argc; i++) {
            if (i > 0) {
                fprintf(file, ", ");
            }
            fprintf(file, "%.*s", def->arg_names[i].len, def->arg_names[i].start);
        }
        fprintf(file, ") ");

        print_block_inline(file, def->body);

        indent -= 2;
        fprintf(file, "%*s}\n", indent, "");
        tmp = tmp->next;
    }
}

void print_ast_import_stmt(FILE* file, AST_ImportStmt* import) {
    AST_ImportStmt* tmp = import;
    while (tmp != NULL) {
        char* root_name = tmp->path_root == DEFAULT_ROOT ? "home" : tmp->path_root == STD_ROOT ? "std" : "lib";
        fprintf(file, "%*s{import %s.%s", indent, "", root_name, tmp->path->val.start);

        if (tmp->varc != 0) {
            fprintf(file, " for ");
            for (int i = 0; i < tmp->varc; i++) {
                fprintf(file, "%s", tmp->vars[i]->val.start);
                
                if (i + 1 < tmp->varc) {
                    fprintf(file, ", ");
                }
            }
        }

        fprintf(file, "}\n");

        tmp = tmp->next;
    }
}

void symple_print_ast_class_def(FILE* file, AST_ClassDef* def) {
    AST_ClassDef* tmp = def;
    while (tmp != NULL) {
        fprintf(file, "%*s{class %s}\n", indent, "", tmp->name->val.start);
        tmp = tmp->next;
    }
}

void print_ast_class_def(FILE* file, AST_ClassDef* def) {
    AST_ClassDef* tmp = def;
    while (tmp != NULL) {
        if (tmp->methods == NULL && tmp->fields == NULL) {
            // 空体定义，简单输出即可
            fprintf(file, "%*s{class %s < ", indent, "", tmp->name->val.start);
            if (tmp->super != NULL) {
                print_ast_expr(file, tmp->super);
            } else {
                fprintf(file, "Object");
            }
            fprintf(file, "}\n");

            tmp = tmp->next;
            continue;
        }

        fprintf(file, "%*s{", indent, "");
        indent += 2;

        // 定义头
        fprintf(file, "class %s < ", tmp->name->val.start);
        if (tmp->super != NULL) {
            print_ast_expr(file, tmp->super);
        } else {
            fprintf(file, "Object");
        }
        fprintf(file, "\n");

        // 字段定义
        struct _ClassFields* tmp_field = tmp->fields;
        while (tmp_field != NULL) {
            fprintf(file, "%*s{%slet %.*s", indent, "", tmp_field->is_static ? "static " : "", tmp_field->name.len, tmp_field->name.start);
            if (tmp_field->init_val != NULL) {
                fprintf(file, " = ");
                print_ast_expr(file, tmp_field->init_val);
            }
            fprintf(file, "}\n");

            tmp_field = tmp_field->next;
        }

        // 方法定义
        struct _ClassMethod* tmp_method = tmp->methods;
        while (tmp_method != NULL) {
            fprintf(
                file, "%*s{ (%s)\n", indent, "",
                tmp_method->type == AST_CLASS_CONSTRUCTOR ? "constructor"
                    : tmp_method->type == AST_CLASS_METHOD ? "method"
                    : tmp_method->type == AST_CLASS_GETTER ? "getter"
                    : tmp_method->type == AST_CLASS_SETTER ? "setter"
                    : tmp_method->type == AST_CLASS_SUBSCRIPT ? "subscript"
                    : "subscript-setter"
            );
            indent += 2;

            fprintf(file, "%*s%s", indent, "", tmp_method->is_static ? "static " : "");

            switch (tmp_method->type) {
                case AST_CLASS_CONSTRUCTOR:
                    fprintf(file, "new(");
                    for (int i = 0; i < tmp_method->argc; i++) {
                        if (i > 0) {
                            fprintf(file, ", %.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        } else {
                            fprintf(file, "%.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        }
                    }
                    fprintf(file, ") ");
                    break;

                case AST_CLASS_METHOD:
                    fprintf(file, "%.*s(", tmp_method->name.len, tmp_method->name.start);
                    for (int i = 0; i < tmp_method->argc; i++) {
                        if (i > 0) {
                            fprintf(file, ", %.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        } else {
                            fprintf(file, "%.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        }
                    }
                    fprintf(file, ") ");
                    break;
                
                case AST_CLASS_GETTER:
                    fprintf(file, "%.*s ", tmp_method->name.len, tmp_method->name.start);
                    break;

                case AST_CLASS_SETTER:
                    fprintf(
                        file, "%.*s = (%.*s) ",
                        tmp_method->name.len, tmp_method->name.start,
                        tmp_method->arg_names[0].len, tmp_method->arg_names[0].start
                    );
                    break;

                case AST_CLASS_SUBSCRIPT:
                    fprintf(file, "[");
                    for (int i = 0; i < tmp_method->argc; i++) {
                        if (i > 0) {
                            fprintf(file, ", %.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        } else {
                            fprintf(file, "%.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        }
                    }
                    fprintf(file, "] ");
                    break;

                case AST_CLASS_SUBSCRIPT_SETTER:
                    fprintf(file, "[");
                    for (int i = 0; i < tmp_method->argc - 1; i++) {
                        if (i > 0) {
                            fprintf(file, ", %.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        } else {
                            fprintf(file, "%.*s", tmp_method->arg_names[i].len, tmp_method->arg_names[i].start);
                        }
                    }
                    fprintf(
                        file, "] = (%.*s)",
                        tmp_method->arg_names[tmp_method->argc - 1].len,
                        tmp_method->arg_names[tmp_method->argc - 1].start
                    );
                    break;
            }

            print_block_inline(file, tmp_method->body);

            indent -= 2;
            fprintf(file, "%*s}\n", indent, "");
            
            tmp_method = tmp_method->next;
        }
        
        indent -= 2;
        fprintf(file, "%*s}\n", indent, "");
        
        tmp = tmp->next;
    }
}

void print_ast_prog(FILE* file, AST_Prog* prog) {
    indent = 2;
    
    fprintf(file, "{\n");

    print_ast_import_stmt(file, prog->import_stmt_head);
    print_ast_func_def(file, prog->func_def_head);
    print_ast_class_def(file, prog->class_def_head);
    
    struct AST_ToplevelStmt* toplevel = prog->toplevel_head;
    while (toplevel != NULL) {
        print_ast_stmt(file, toplevel->stmt);
        toplevel = toplevel->next;
    }

    fprintf(file, "}\n");
}
