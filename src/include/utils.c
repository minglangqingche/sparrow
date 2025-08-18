#include "utils.h"
#include "common.h"
#include "vm.h"
#include "parser.h"
#include <stdlib.h>
#include <stdarg.h>
#include "gc.h"

void* mem_manager(VM* vm, void* ptr, u32 old_size, u32 new_size) {
    vm->allocated_bytes += new_size - old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }

    if (new_size > 0 && vm->allocated_bytes > vm->config.next_gc) {
        start_gc(vm);
    }

    return realloc(ptr, new_size);
}

u32 ceil_to_power_of_2(u32 v) {
    v += (v == 0) ? 1 : 0;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

DEFINE_BUFFER_METHOD(String)
DEFINE_BUFFER_METHOD(Int)
DEFINE_BUFFER_METHOD(Char)
DEFINE_BUFFER_METHOD(Byte)

void symbol_table_clear(VM* vm, SymbolTable* buffer) {
    u32 idx = 0;
    while (idx < buffer->count) {
        mem_manager(vm, buffer->datas[idx++].str, 0, 0);
    }
    StringBufferClear(vm, buffer);
}

void error_report_proto(char* file, int line, char* func, void* parser, ErrorType error_type, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char buffer[DEFAULT_BUFFER_SIZE] = {'\0'};
    vsnprintf(buffer, DEFAULT_BUFFER_SIZE, fmt, ap);
    va_end(ap);

    switch (error_type) {
        case ERROR_IO:
        case ERROR_MEM:
            fprintf(stderr, "%s:%d in function '%s': %s\n", file, line, func, buffer);
            break;
        case ERROR_LEX:
        case ERROR_COMPILE:
            ASSERT(parser != NULL, "parser is null!");
            fprintf(
                stderr, "%s:%d: %s\n",
                ((Parser*)parser)->file, ((Parser*)parser)->pre_token.line, buffer
            );
            break;
        case ERROR_RUNTIME:
            fprintf(stderr, "%s\n", buffer);
            break;
        default:
            UNREACHABLE();
    }
    exit(1);
}
