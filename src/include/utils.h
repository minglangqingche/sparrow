#ifndef __INCLUDE_UTILS_H__
#define __INCLUDE_UTILS_H__

#include "common.h"

#define DEFAULT_BUFFER_SIZE (512)

void* mem_manager(VM* vm, void* ptr, u32 old_size, u32 new_size);

#define ALLOCATE(vm_ptr, type) \
    (type*)mem_manager(vm_ptr, NULL, 0, sizeof(type))

#define ALLOCATE_EXTRA(vm_ptr, main_type, extra_size) \
    (main_type*)mem_manager(vm_ptr, NULL, 0, sizeof(main_type) + extra_size)

#define ALLOCATE_ARRAY(vm_ptr, type, count) \
    (type*)mem_manager(vm_ptr, NULL, 0, sizeof(type) * count)

#define DEALLOCATE_ARRAY(vm_ptr, array_ptr, count) \
    mem_manager(vm_ptr, array_ptr, sizeof(array_ptr[0]) * count, 0)

#define DEALLOCATE(vm_ptr, mem_ptr) \
    mem_manager(vm_ptr, mem_ptr, 0, 0)

u32 ceil_to_power_of_2(u32 v);

typedef struct {
    char* str;
    u32 len;
} String;

typedef struct {
    u32 len;
    char start[];
} CharValue;

#define DECLARE_BUFFER_TYPE(type) \
    typedef struct {\
        type* datas;\
        u32 capacity;\
        u32 count;\
    } type##Buffer;\
    void type##BufferInit(type##Buffer* buf);\
    void type##BufferFillWrite(VM* vm, type##Buffer* buf, type data, u32 fill_count);\
    void type##BufferAdd(VM* vm, type##Buffer* buf, type data);\
    void type##BufferClear(VM* vm, type##Buffer* buf);\
    void type##_gc_BufferClear(VM* vm, type##Buffer* buf);

#define BufferType(type) type##Buffer
#define BufferInit(type, buffer) type##BufferInit(buffer)
#define BufferFill(type, buffer, vm, data, count) type##BufferFillWrite(vm, buffer, data, count)
#define BufferAdd(type, buffer, vm, data) type##BufferAdd(vm, buffer, data)
#define BufferClear(type, buffer, vm) type##BufferClear(vm, buffer)
#define gc_BufferClear(type, buffer, vm) type##_gc_BufferClear(vm, buffer)

#define DEFINE_BUFFER_METHOD(type) \
    void type##BufferInit(type##Buffer* buf) {\
        buf->datas = NULL;\
        buf->count = 0;\
        buf->capacity = 0;\
    }\
    void type##BufferFillWrite(VM* vm, type##Buffer* buf, type data, u32 fill_count) {\
        u32 new_count = buf->count + fill_count;\
        if (new_count > buf->capacity) {\
            usize old_size = buf->capacity * sizeof(type);\
            buf->capacity = ceil_to_power_of_2(new_count);\
            usize new_size = buf->capacity * sizeof(type);\
            ASSERT(new_size > old_size, "faint...memory allocate!");\
            buf->datas = (type*)mem_manager(vm, buf->datas, old_size, new_size);\
        }\
        for (u32 i = 0; i < fill_count; i++) {\
            buf->datas[buf->count++] = data;\
        }\
    }\
    void type##BufferAdd(VM* vm, type##Buffer* buf, type data) {\
        type##BufferFillWrite(vm, buf, data, 1);\
    }\
    void type##BufferClear(VM* vm, type##Buffer* buf) {\
        usize old_size = buf->capacity * sizeof(buf->datas[0]);\
        mem_manager(vm, buf->datas, old_size, 0);\
        type##BufferInit(buf);\
    }\
    void type##_gc_BufferClear(VM* vm, type##Buffer* buf) {\
        mem_manager(vm, buf->datas, 0, 0);\
        type##BufferInit(buf);\
    }

typedef u8 Byte;
typedef char Char;
typedef int Int;

DECLARE_BUFFER_TYPE(String)
typedef BufferType(String) SymbolTable;
DECLARE_BUFFER_TYPE(Int)
DECLARE_BUFFER_TYPE(Char)
DECLARE_BUFFER_TYPE(Byte)

typedef enum {
    ERROR_IO, ERROR_MEM,
    ERROR_LEX, ERROR_COMPILE,
    ERROR_RUNTIME,
} ErrorType;

void symbol_table_clear(VM* vm, SymbolTable* buffer);
void error_report_proto(char* file, int line, char* func, void* parser, ErrorType error_type, const char* fmt, ...);
#define error_report(parser, error_type, ...) error_report_proto(__FILE__, __LINE__, (char*)__func__, parser, error_type, __VA_ARGS__)

#define IO_ERROR(...) \
    error_report(NULL, ERROR_IO, __VA_ARGS__)
#define MEM_ERROR(...) \
    error_report(NULL, ERROR_MEM, __VA_ARGS__)
#define LEX_ERROR(parser, ...) \
    error_report(parser, ERROR_LEX, __VA_ARGS__)
#define COMPILE_ERROR(parser, ...) \
    error_report(parser, ERROR_COMPILE, __VA_ARGS__)
#define RUNTIME_ERROR(...) \
    error_report(NULL, ERROR_RUNTIME, __VA_ARGS__)

#endif