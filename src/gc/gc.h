#ifndef __GC_GC_H__
#define __GC_GC_H__

#include "vm.h"

void start_gc(VM* vm);
void free_obj(VM* vm, ObjHeader* header);

#endif