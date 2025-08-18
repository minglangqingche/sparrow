#include "obj_string.h"
#include "common.h"
#include "header_obj.h"
#include "utils.h"
#include <string.h>
#include "vm.h"

// fnv-1a
u32 hash_string(char* str, u32 len) {
    u32 hash = 2166136261;
    for (int i = 0; i < len; i++) {
        hash ^= str[i];
        hash *= 16777619;
    }
    return hash;
}

inline void objstring_hash(ObjString* str) {
    str->hash_code = hash_string(str->val.start, str->val.len);
}

ObjString* objstring_new(VM* vm, const char* str, u32 len) {
    ASSERT(len == 0 || str != NULL, "str len don't match str.");

    ObjString* obj = ALLOCATE_EXTRA(vm, ObjString, len + 1);

    if (obj == NULL) {
        MEM_ERROR("Allocating ObjString failed.");
    }

    objheader_init(vm, &obj->header, OT_STRING, vm->string_class);

    obj->val.len = len;

    if (len > 0) {
        memcpy(obj->val.start, str, len);
    }
    obj->val.start[len] = '\0';
    objstring_hash(obj);

    return obj;
}
