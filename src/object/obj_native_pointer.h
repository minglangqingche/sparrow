#ifndef __OBJ_NATIVE_POINTER_H__
#define __OBJ_NATIVE_POINTER_H__

#include "header_obj.h"
#include "obj_string.h"

typedef struct _ObjNativePointer ObjNativePointer;

typedef void (*Destroy)(ObjNativePointer*);

struct _ObjNativePointer {
    ObjHeader header;
    void* ptr;
    ObjString* classifier;
    Destroy destroy;
};

ObjNativePointer* native_pointer_new(VM* vm, void* ptr, ObjString* classifier, Destroy destroy);
bool native_pointer_check_classifier(ObjNativePointer* np, ObjString* expect);
bool native_pointer_is_eq(ObjNativePointer* l, ObjNativePointer* r);

#endif