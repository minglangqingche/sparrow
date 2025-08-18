#ifndef __OBJECT_OBJ_MAP_H__
#define __OBJECT_OBJ_MAP_H__

#include "header_obj.h"

#define MAP_LOAD_PRECENT 0.8

typedef struct {
    Value key;
    Value val;
} Entry;

typedef struct {
    ObjHeader header;
    u32 capacity;
    u32 len;
    Entry* entries;
} ObjMap;

ObjMap* objmap_new(VM* vm);
void objmap_set(VM* vm, ObjMap* map, Value key, Value val);
Value objmap_get(ObjMap* map, Value key);
void objmap_clear(VM* vm, ObjMap* map);
Value objmap_remove(VM* vm, ObjMap* map, Value key);

#endif