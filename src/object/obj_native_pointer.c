#include "obj_native_pointer.h"
#include "class.h"
#include "header_obj.h"
#include "vm.h"

bool native_pointer_check_classifier(ObjNativePointer* np, ObjString* expect) {
    if ((np->classifier == NULL || expect == NULL) && expect != np->classifier) {
        return false;
    }
    return value_is_equal(OBJ_TO_VALUE(np->classifier), OBJ_TO_VALUE(expect));
}

ObjNativePointer* native_pointer_new(VM* vm, void* ptr, ObjString* classifier, Destroy destroy) {
    ObjNativePointer* self = ALLOCATE(vm, ObjNativePointer);
    
    objheader_init(vm, &self->header, OT_NATIVE_POINTER, vm->native_pointer_class);
    
    self->classifier = classifier;
    self->destroy = destroy;
    self->ptr = ptr;

    return self;
}

inline bool native_pointer_is_eq(ObjNativePointer* l, ObjNativePointer* r) {
    return native_pointer_check_classifier(l, r->classifier) && l->ptr == r->ptr;
}
