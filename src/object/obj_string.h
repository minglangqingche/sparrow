#ifndef __OBJECT_OBJ_STRING_H__
#define __OBJECT_OBJ_STRING_H__

#include "header_obj.h"

typedef struct {
    ObjHeader header;
    u32 hash_code;
    CharValue val;
} ObjString;

u32 hash_string(char* str, u32 len);
void objstring_hash(ObjString* str);
ObjString* objstring_new(VM* vm, const char* str, u32 len);

#endif