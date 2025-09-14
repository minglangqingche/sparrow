#include "gc.h"
#include "class.h"
#include "common.h"
#include "compiler.h"
#include "header_obj.h"
#include "meta_obj.h"
#include "obj_fn.h"
#include "obj_list.h"
#include "obj_map.h"
#include "obj_native_pointer.h"
#include "obj_range.h"
#include "obj_thread.h"
#include "utils.h"
#include "vm.h"
#include "parser.h"
#include <stdlib.h>

#ifdef OUTPUT_GC_INFO
    #include <time.h>
    #include "disassemble.h"
#endif

void gray_obj(VM* vm, ObjHeader* obj) {
    if (obj == NULL || obj->is_dark) {
        return;
    }

    obj->is_dark = true;

    if (vm->grays.count >= vm->grays.capacity) {
        vm->grays.capacity = vm->grays.count * 2;
        vm->grays.gray_objs = (ObjHeader**)(realloc(vm->grays.gray_objs, vm->grays.capacity * sizeof(ObjHeader*)));
    }

    vm->grays.gray_objs[vm->grays.count++] = obj;
}

void gray_value(VM* vm, Value val) {
    if (!VALUE_IS_OBJ(val)) {
        return;
    }
    gray_obj(vm, val.header);
}

inline static void gray_buffer(VM* vm, BufferType(Value)* buffer) {
    for (int i = 0; i < buffer->count; i++) {
        gray_value(vm, buffer->datas[i]);
    }
}

static void black_class(VM* vm, Class* class) {
    gray_obj(vm, (ObjHeader*)class->header.class);
    gray_obj(vm, (ObjHeader*)class->super_class);
    for (int i = 0; i < class->methods.count; i++) {
        if (class->methods.datas[i].type == MT_SCRIPT) {
            gray_obj(vm, (ObjHeader*)class->methods.datas[i].obj);
        }
    }
    gray_obj(vm, (ObjHeader*)class->name);
    vm->allocated_bytes += sizeof(Class);
    vm->allocated_bytes += sizeof(Method) * class->methods.capacity;
}

static void black_closure(VM* vm, ObjClosure* closure) {
    gray_obj(vm, (ObjHeader*)closure->fn);
    for (int i = 0; i < closure->fn->upvalue_number; i++) {
        gray_obj(vm, (ObjHeader*)closure->upvalue[i]);
    }
    vm->allocated_bytes += sizeof(ObjClosure);
    vm->allocated_bytes += sizeof(ObjUpvalue) * closure->fn->upvalue_number;
}

static void black_thread(VM* vm, ObjThread* thread) {
    for (int i = 0; i < thread->used_frame_num; i++) {
        gray_obj(vm, (ObjHeader*)thread->frames[i].closure);
    }

    Value* slot = thread->stack;
    while (slot < thread->esp) {
        gray_value(vm, *slot++);
    }

    ObjUpvalue* upvalue = thread->open_upvalue;
    while (upvalue != NULL) {
        gray_obj(vm, (ObjHeader*)upvalue);
        upvalue = upvalue->next;
    }

    gray_obj(vm, (ObjHeader*)thread->caller);
    gray_value(vm, thread->error_obj);

    vm->allocated_bytes += sizeof(ObjThread);
    vm->allocated_bytes += sizeof(Value) * thread->stack_capacity;
    vm->allocated_bytes += sizeof(Frame) * thread->frame_capacity;
}

static void black_fn(VM* vm, ObjFn* fn) {
    gray_buffer(vm, &fn->constants);
    vm->allocated_bytes += sizeof(ObjFn);
    vm->allocated_bytes += sizeof(u8) * fn->instr_stream.capacity;
    vm->allocated_bytes += sizeof(Value) * fn->constants.capacity;
#if DEBUG
    vm->allocated_bytes += sizeof(Int) * fn->instr_stream.capacity;
#endif
}

static void black_instance(VM* vm, ObjInstance* instance) {
    gray_obj(vm, (ObjHeader*)instance->header.class);
    for (int i = 0; i < instance->header.class->field_number; i++) {
        gray_value(vm, instance->fields[i]);
    }
    vm->allocated_bytes += sizeof(ObjInstance);
    vm->allocated_bytes += sizeof(Value) * instance->header.class->field_number;
}

static void black_list(VM* vm, ObjList* list) {
    gray_buffer(vm, &list->elements);
    vm->allocated_bytes += sizeof(ObjList);
    vm->allocated_bytes += sizeof(Value) * list->elements.capacity;
}

static void black_map(VM* vm, ObjMap* map) {
    for (int i = 0; i < map->capacity; i++) {
        if (VALUE_IS_UNDEFINED(map->entries[i].key)) {
            continue;
        }

        gray_value(vm, map->entries[i].key);
        gray_value(vm, map->entries[i].val);
    }
    vm->allocated_bytes += sizeof(ObjMap);
    vm->allocated_bytes += sizeof(Entry) * map->capacity;
}

static void black_range(VM* vm) {
    vm->allocated_bytes += sizeof(ObjRange);
}

static void black_string(VM* vm, ObjString* string) {
    vm->allocated_bytes += sizeof(ObjString);
    vm->allocated_bytes += sizeof(char) * (string->val.len + 1);
}

static void black_upvalue(VM* vm, ObjUpvalue* upvalue) {
    gray_value(vm, upvalue->closed_upvalue);
    vm->allocated_bytes += sizeof(ObjUpvalue);
}

inline static void black_module(VM* vm, ObjModule* module) {
    for (int i = 0; i < module->module_var_value.count; i++) {
        gray_value(vm, module->module_var_value.datas[i]);
    }

    gray_obj(vm, (ObjHeader*)module->name);

    vm->allocated_bytes += sizeof(ObjModule);
    vm->allocated_bytes += sizeof(String) * module->module_var_name.capacity;
    vm->allocated_bytes += sizeof(Value) * module->module_var_value.capacity;
}

inline static void black_native_pointer(VM* vm, ObjNativePointer* np) {
    gray_obj(vm, (ObjHeader*)np->classifier);
}

static void black_obj(VM* vm, ObjHeader* obj) {
#if OUTPUT_GC_INFO
    printf("~ mark [%u] ", vm->allocated_bytes);
    u32 _before = vm->allocated_bytes;
    print_value(&OBJ_TO_VALUE(obj));
#endif
    switch (obj->type) {
        case OT_CLASS:
            black_class(vm, (Class*)obj);
            break;
        case OT_CLOSURE:
            black_closure(vm, (ObjClosure*)obj);
            break;
        case OT_THREAD:
            black_thread(vm, (ObjThread*)obj);
            break;
        case OT_FUNCTION:
            black_fn(vm, (ObjFn*)obj);
            break;
        case OT_INSTANCE:
            black_instance(vm, (ObjInstance*)obj);
            break;
        case OT_LIST:
            black_list(vm, (ObjList*)obj);
            break;
        case OT_MAP:
            black_map(vm, (ObjMap*)obj);
            break;
        case OT_MODULE:
            black_module(vm, (ObjModule*)obj);
            break;
        case OT_RANGE:
            black_range(vm);
            break;
        case OT_UPVALUE:
            black_upvalue(vm, (ObjUpvalue*)obj);
            break;
        case OT_STRING:
            black_string(vm, (ObjString*)obj);
            break;
        case OT_NATIVE_POINTER:
            black_native_pointer(vm, (ObjNativePointer*)obj);
            break;
        default:
            UNREACHABLE();
    }

#ifdef OUTPUT_GC_INFO
    printf(" [%u, %ld]\n", vm->allocated_bytes, (long)vm->allocated_bytes - (long)_before);
#endif
}

static void black_obj_in_gray(VM* vm) {
    while (vm->grays.count > 0) {
        ObjHeader* header = vm->grays.gray_objs[--vm->grays.count];
        black_obj(vm, header);
    }
}

void free_obj(VM* vm, ObjHeader* header) {
#ifdef OUTPUT_GC_INFO
    u32 _before = vm->allocated_bytes;
    printf("# free [%u] ", _before);
    print_value(&OBJ_TO_VALUE(header));
#endif

    switch (header->type) {
        case OT_CLASS: {
            gc_BufferClear(Method, &((Class*)header)->methods, vm);
            break;
        }
        case OT_THREAD: {
            ObjThread* thread = (ObjThread*)header;
            DEALLOCATE(vm, thread->frames);
            DEALLOCATE(vm, thread->stack);
            break;
        }
        case OT_FUNCTION: {
            ObjFn* fn = (ObjFn*)header;
            gc_BufferClear(Value, &fn->constants, vm);
            gc_BufferClear(Byte, &fn->instr_stream, vm);
        #ifdef DEBUG
            gc_BufferClear(Int, &fn->debug->line);
            DEALLOCATE(vm, fn->debug->fn_name);
            DEALLOCATE(vm, fn->debug);
        #endif
            break;
        }
        case OT_LIST: {
            gc_BufferClear(Value, &((ObjList*)header)->elements, vm);
            break;
        }
        case OT_MAP: {
            DEALLOCATE(vm, ((ObjMap*)header)->entries);
            break;
        }
        case OT_MODULE:{
            gc_BufferClear(String, &((ObjModule*)header)->module_var_name, vm);
            gc_BufferClear(Value, &((ObjModule*)header)->module_var_value, vm);
            break;
        }

        case OT_NATIVE_POINTER: {
            ObjNativePointer* np = (ObjNativePointer*)header;
            if (np->destroy != NULL) {
                np->destroy(np);
            }
        }

        case OT_STRING:
        case OT_RANGE:
        case OT_UPVALUE:
        case OT_CLOSURE:
        case OT_INSTANCE:
            break;

        default:
            UNREACHABLE();
    }

    DEALLOCATE(vm, header);

#ifdef OUTPUT_GC_INFO
    printf(" [%u, %ld]\n", vm->allocated_bytes, (long)vm->allocated_bytes - (long)_before);
#endif
}

void gray_compile_unit(VM* vm, CompileUnitPubStruct* cu) {
    //向上遍历父编译器外层链 使其fn可到达
    //编译结束后,vm->curParser会在endCompileUnit中置为NULL,
    //本函数是在编译过程中调用的,即vm->curParser肯定不为NULL,
    ASSERT(vm->cur_parser != NULL, "only called while compiling!");
    do {
        gray_obj(vm, (ObjHeader*)cu->fn);
        cu = cu->enclosing_unit;
    } while (cu != NULL);
}

void start_gc(VM* vm) {
#ifdef OUTPUT_GC_INFO
    double start_time = (double)clock();
    u32 before = vm->allocated_bytes;
    printf("-- gc before: %d vm: %p --\n", before, vm);
#endif

    vm->allocated_bytes = 0;
    gray_obj(vm, (ObjHeader*)vm->all_module);

    for (int i = 0; i < vm->tmp_roots_num; i++) {
        gray_obj(vm, vm->tmp_roots[i]);
    }

    gray_obj(vm, (ObjHeader*)vm->cur_thread);

    if (vm->cur_parser != NULL) {
        gray_value(vm, vm->cur_parser->cur_token.value);
        gray_value(vm, vm->cur_parser->pre_token.value);
    }

    if (vm->cur_cu != NULL) {
        gray_compile_unit(vm, vm->cur_cu);
    }

    gray_buffer(vm, &vm->allways_keep_roots);
    gray_buffer(vm, &vm->ast_obj_root);

    black_obj_in_gray(vm);

    ObjHeader** obj = &vm->all_objs;
    while (*obj != NULL) {
        if (!(*obj)->is_dark) {
            ObjHeader* unreached = *obj;
            *obj = unreached->next;
            free_obj(vm, unreached);
        } else {
            (*obj)->is_dark = false;
            obj = &(*obj)->next;
        }
    }

    vm->config.next_gc = vm->allocated_bytes * vm->config.heap_growth_factor;
    if (vm->config.next_gc < vm->config.min_heap_size) {
        vm->config.next_gc = vm->config.min_heap_size;
    }

#ifdef OUTPUT_GC_INFO
    double elapsed = (double)clock() - start_time;
    printf(
        ">> gc after: %u, collected: %ld, next_gc: %u, take %.3fms.\n",
        vm->allocated_bytes, (long)before - (long)vm->allocated_bytes, vm->config.next_gc, elapsed
    );
#endif
}
