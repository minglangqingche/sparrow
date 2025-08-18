#ifndef __OBJECT_OBJ_RANGE_H__
#define __OBJECT_OBJ_RANGE_H__

#include "class.h"
#include "header_obj.h"

typedef struct {
    ObjHeader header;
    int from;
    int to;
    int step;
} ObjRange;

ObjRange* objrange_new(VM* vm, int from, int to, int step);

#endif